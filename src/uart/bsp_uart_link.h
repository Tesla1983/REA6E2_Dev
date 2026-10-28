/* ============================================================
 * 上位机 → MCU 串口链路（SCI0 / P100 RXD0 / P101 TXD0, 115200 8N1）
 *
 * 数据来源: 电脑上 `host/link_serial.py` 每秒发一行, 形如
 *     $CT:48,CU:12,CF:0,GT:54,GU:3,GF:1064,MU:69*2F\r\n
 *   `*` 后两位是 `$` 与 `*` 之间全部字符的 XOR（两位大写十六进制）。
 *
 * 为什么按 key 解析, 不按位置:
 *   上位机缺项时会省略（例如拿不到风扇就整段不发 `CF:`）。按位置解析会整体错位,
 *   把 GPU 温度当 RAM 用; 按 key 解析缺项就是缺项, 屏幕上显示 `--`。
 *
 * 为什么中断 + 环形缓冲:
 *   主循环里有 250 ms 级的 `R_BSP_SoftwareDelay` 和整屏刷新的阻塞（约 250 ms）,
 *   115200 bps 下 1 字节约 87 µs, 不缓冲必丢字节。
 * ============================================================ */
#ifndef BSP_UART_LINK_H_
#define BSP_UART_LINK_H_

#include <stdbool.h>
#include <stdint.h>

#include "hal_data.h"

/** 一帧监控数据。字段为 -1 表示这一帧没有该项（屏幕上显示 `--`）。 */
typedef struct
{
    int16_t ct;   /* CPU 温度 ℃      */
    int16_t cu;   /* CPU 占用 %      */
    int16_t cf;   /* CPU 风扇 RPM    */
    int16_t gt;   /* GPU 核心温度 ℃   */
    int16_t gu;   /* GPU 占用 %      */
    int16_t gf;   /* GPU 风扇 RPM    */
    int16_t mu;   /* 内存占用 %      */
} uart_link_data_t;

/* ---- 运行统计（volatile, 探针可直读, 用来判"链路到底通没通"）---- */
extern volatile uint32_t g_link_frames;    /* 校验通过并解析成功的帧数 */
extern volatile uint32_t g_link_rx_bytes;  /* 收到的原始字节数 */
extern volatile uint32_t g_link_err_xor;   /* 校验和不符的帧数 */
extern volatile uint32_t g_link_err_fmt;   /* 格式不合法被丢弃的次数 */
extern volatile uint8_t  g_link_last[40];  /* 最后一次被丢弃的原始行（诊断用） */
extern volatile int32_t  g_link_tx_err;    /* beacon write 的返回码（0=成功） */
extern volatile uint32_t g_link_tx_cnt;    /* beacon 已发起次数 */

/** 屏显示语言（0=英文 linked/unlink，1=中文 已连接/未连接）。
 *  由上位机数据行的 `LG` 字段下发；默认 1（中文）。 */
extern volatile uint8_t  g_screen_lang;

/** 打开 SCI0 并挂上单字节接收。返回 FSP_SUCCESS 表示成功。 */
fsp_err_t UART_Link_Init(void);

/** 串口中断里调用（由 `uart0_callback` 转发）。把字节塞进环形缓冲并重新挂接收。 */
void UART_Link_OnIsr(uart_callback_args_t * p_args);

/** 从别的接收源（现在是 USB CDC）往同一条链路里塞字节。
 *  语义与串口中断一致: 只写生产者指针, 不做解析; 解析仍由 UART_Link_Poll() 做。
 *  这样"数据从哪来"和"帧怎么解析"解耦, 两个源共用一份解析与超时判据。 */
void UART_Link_PushBytes(uint8_t const * p_src, uint32_t n);

/** 主循环里调用: 把环形缓冲里已到的字节喂给解析器。**不阻塞**。 */
void UART_Link_Poll(void);

/** 主循环里周期性调用（建议每 250 ms 一次, 传 250）。
 *  实现接收超时: 连续 2 s 没有有效帧就判为断链，屏幕上显示 `NO LINK`。 */
void UART_Link_Tick(uint16_t elapsed_ms);

/** 链路是否在线（最近 2 s 内收到过合法帧） */
bool UART_Link_Online(void);

/** 取当前数据（只读指针）。即使离线也保留最后一次成功收到的值。 */
const uart_link_data_t * UART_Link_Data(void);

/* ---- 物理连线自检（排查"收不到任何字节"时用）----
 *
 *  RX 收不到东西时, 光看 `g_link_rx_bytes == 0` 无法区分两件事:
 *    ① 上位机写错了 COM 口;  ② TX 线根本没接到 MCU 的 RX 脚。
 *  把本宏置 1, 固件每秒从 TX 发一行 `BEACON <n>\r\n`; 电脑上逐个 COM 口去听,
 *  哪个口能听到 ⇒ 那个口与本板是连着的, 顺带证明波特率和引脚复用都对。
 *
 *  调用点在 `src/nt7534/nt7534.c` 的主循环里, 收本宏控制。 */
#define UART_LINK_BEACON    (1)

/** 从 TX 发一行 `BEACON <n>\r\n`（阻塞发送, 约 1 ms）。仅在排查连线时调用。 */
void UART_Link_Beacon(void);

#endif /* BSP_UART_LINK_H_ */
