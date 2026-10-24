/**
 * @file  bsp_usb_cdc.h
 * @brief USB CDC 虚拟串口（Type-C 直接当串口用）
 *
 * 硬件: 这块板的 Type-C 直连 MCU 原生 USB —— P814 = USB_DP(D+)、P815 = USB_DM(D-)，
 * 板上**没有** CH340/CP2102 之类的 UART 桥。所以"插上 Type-C 就出现一个 COM 口"
 * 只能靠固件实现 USB CDC（设备类 0x02），由 Windows 自带的 usbser.sys 认领。
 *
 * 前提（都已在 configuration.xml 里配好，勿手改生成物）:
 *   - USBCLK = PLL 240MHz / 5 = 48MHz（`BSP_CFG_UCLK_SOURCE/ DIV`）
 *   - 中断: USBI / USBR / D0FIFO / D1FIFO，优先级 12
 *   - 实例: `g_basic0` / `g_basic0_ctrl` / `g_basic0_cfg`（ra_gen/hal_data.c）
 *   - 描述符: `g_usb_descriptor`（src/usb/usb_cdc_descriptor.c）
 */

#ifndef BSP_USB_CDC_H_
#define BSP_USB_CDC_H_

#include <stdbool.h>
#include <stdint.h>

#include "hal_data.h"

/** 打开 USB（配引脚 + R_USB_Open）。必须在其它 USB 调用之前跑一次。
 *  返回 FSP_SUCCESS 只代表驱动已启动，**不代表 PC 已枚举**（那要等 USB_STATUS_CONFIGURED）。 */
fsp_err_t USB_CDC_Init(void);

/** PC 是否已完成枚举（收到 SET_CONFIGURATION）。false 时 Write 会返回错误。 */
bool USB_CDC_Configured(void);

/** 发一段数据到 PC（异步, 数据会被驱动拷走后再返回）。
 *  ⚠ buf 指向的内容必须在完成回调前保持有效 —— 内部已做一份拷贝，可直接传栈上缓冲。 */
fsp_err_t USB_CDC_Write(uint8_t const * p_src, uint32_t len);

extern volatile uint32_t g_usb_open_err;   /**< R_USB_Open 的返回码（0 = FSP_SUCCESS） */
extern volatile uint32_t g_usb_events;     /**< 回调被调用的次数（不涨 = 中断没进来） */
extern volatile uint32_t g_usb_rx_bytes;   /**< 从 PC 收到的字节数 */
extern volatile uint32_t g_usb_tx_bytes;   /**< 发往 PC 的字节数 */
extern volatile uint32_t g_usb_cfg_cnt;    /**< 进入 CONFIGURED 的次数（插拔一次 +1） */
extern volatile uint32_t g_usb_pin_err_dp;  /**< P814 复用成 USB_DP 的返回码（0=成功） */
extern volatile uint32_t g_usb_pin_err_dm;  /**< P815 复用成 USB_DM 的返回码（0=成功） */
/** P407 复用成 USB_VBUS 的返回码（0=成功）。
 *  P407 是 RA6E2 唯一的 VBUS 检测脚；本工程原先把它配成 IIC0-SDA0（OLED），
 *  导致 VBSTS 恒 0、枚举必失败。详见 bsp_usb_cdc.c 的 USB_CDC_CLAIM_VBUS_PIN。 */
extern volatile uint32_t g_usb_pin_err_vbus;
extern volatile uint32_t g_usb_vbsts;    /**< INTSTS0.VBSTS: 1=控制器认为 VBUS 已接入 */
extern volatile uint32_t g_usb_syscfg;   /**< USBFS SYSCFG: 正常应 = 0x0411           */
/** SYSCFG.DPRPU 的最终值（1 = D+ 上拉已打开 = 主机能看见本设备）。
 *  ⚠ FSP 只在"检测到 VBUS"时才置它，本板 VBSTS 恒 0 ⇒ 必须由 USB_CDC_Init() 补开。
 *  读到 0 就说明设备在电气上没挂上总线, 插线不会有任何反应。 */
extern volatile uint32_t g_usb_dprpu;

/* ============================================================
 * 调试探针快照: 调试器(probe-rs)只能读 RAM, 而外设寄存器(0x400xxxxx)与向量表(0x0)
 * 读不到, 故由固件在每帧把这些关键硬件状态"代读"进下面的 RAM 全局, 再用
 * `probe-rs read` 取走(见 USB_CDC_ProbeUpdate)。
 *
 * 判定逻辑(配合 g_usb_events, 它在 usb_callback 最顶上无条件 ++):
 *   g_usb_events == 0  ⇒ usb_callback 一次都没被调用 ⇒ ISR 没真正到达应用层。
 *   若 g_usb_ispr0 的 bit0 = 1 且 events = 0
 *       ⇒ NVIC 已挂起(中断请求到了 ICU/NVIC)但 ISR 永远没跑
 *       ⇒ 断在: 向量表入口不是 usbfs_interrupt_handler / ISR 进不去(FAULT) / ISER 没开。
 *   若 g_usb_ispr0 的 bit0 = 0 且 events = 0
 *       ⇒ ISR 跑过(清了挂起)却没进 usb_callback ⇒ 断在 g_usb_apl_callback[0] 或 FSP 状态机。
 *       (但本工程 events 在回调顶 ++, 此情形基本等于 ISR 没跑, 重点看下面两项)
 *   g_usb_vt_usbfs 应 = usbfs_interrupt_handler 的 flash 地址(0x0000355C); 为 0/异常 ⇒ 向量没接。
 *   g_usb_elcr 的 bit0(ELCON) 必须为 1, 否则 IELSR0 的事件 0x6D 不路由到 ICU(所有外部中断全废)。
 *   g_usb_ielsr0 / g_usb_iser0 / g_usb_intsts0 / g_usb_intenb0 是 INTSTS0&INTENB0 等动态复读,
 *   避免只信屏幕一帧(屏幕只刷新 ~1Hz, 且中断可能转瞬即逝)。
 */
extern volatile uint32_t g_usb_ispr0;    /**< NVIC ISPR0 @0xE000E200: bit0 = USBFS 挂起        */
extern volatile uint32_t g_usb_elcr;     /**< ELC ELCR    @0x40082000: bit0(ELCON)=1 才路由事件 */
extern volatile uint32_t g_usb_vt_usbfs; /**< 向量表 IRQ0 入口(VTOR+0x40): 应=0x0000355C        */
extern volatile uint32_t g_usb_ielsr0;  /**< ICU IELSR0  @0x40006300                          */
extern volatile uint32_t g_usb_iser0;   /**< NVIC ISER0  @0xE000E100                          */
extern volatile uint32_t g_usb_intsts0; /**< USBFS INTSTS0 @0x40090040                        */
extern volatile uint32_t g_usb_intenb0; /**< USBFS INTENB0 @0x40090030                        */

/** 每帧把上述硬件快照刷新进 RAM(供调试器读取)。在主循环里调用一次即可。 */
void USB_CDC_ProbeUpdate(void);

/** ⚠⚠ 裸机 FSP 的 USB 事件派发入口 —— **必须由主循环反复调用, 建议 2 ms 一次**。
 *
 *  ISR 只把事件解码进深度仅 8 的环形缓冲, 派发靠 `usb_pstd_pcd_task()`。
 *  不调 ⇒ 枚举卡在 Address; 调得太稀 ⇒ 缓冲溢出丢事件;
 *  放到 ISR 里调 ⇒ FSP 的类请求 API 会被在中断上下文调用, 主机 SetCommState 超时。
 *  调用点见 nt7534_test.c 主循环里的 2 ms 细片。 */
void USB_CDC_Poll(void);

#endif /* BSP_USB_CDC_H_ */
