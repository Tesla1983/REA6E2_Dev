/**
 * @file  usb_isr_probe.c
 * @brief USBFS 中断入口探针（诊断用）—— 只做两件事: 计数 + 记录进 ISR 那一刻的 INTSTS0
 *
 * 为什么需要它:
 *   本轮排查已确认 INTENB0=0x9D00、IELSR0=0x6D、ISER0.bit0=1、ELC 开、向量表入口正确、
 *   g_usb_apl_callback 指向 usb_callback，但 g_usb_events 恒 0。
 *   问题只剩两种可能, 且无法从外部寄存器区分:
 *     ① USBFS 外设根本没把中断请求拉起来(INTSTS0 的使能位从未置位);
 *     ② 请求起来了、ISR 也进了, 只是 FSP 的处理路径没走到应用回调。
 *   在向量表 [0] 挂这个薄包装, 就能用 g_usb_isr_entries 一刀切开:
 *     entries == 0  ⇒ ①(中断从未到达 CPU)
 *     entries >  0  ⇒ ②(ISR 在跑, 问题在 FSP/描述符层)
 *
 * ⚠ 这是**诊断脚手架**, 不是产品代码:
 *   - 需要把 ra_gen/vector_data.c 的 [0] 从 usbfs_interrupt_handler 改成 usbfs_isr_probe
 *     (该文件是 RASC 生成物, 重新 --generate 会被覆盖; 备份在 .workbuddy/backup/)。
 *   - 包装体只做一次 16 位读 + 一次自增, 然后原样调用真实 handler, **不改变任何中断语义**。
 *   - 不碰引脚、不碰供电、不驱动任何外部器件。
 */

#include "hal_data.h"

/* FSP 真正的 USBFS 中断服务程序（r_usb_pinthandler_usbip0.c，强符号）。 */
extern void usbfs_interrupt_handler(void);
/* 裸机下的事件派发入口（r_usb_extern.h: `void usb_pstd_pcd_task(void);`）。 */
extern void usb_pstd_pcd_task(void);

/** 进入 USBFS ISR 的累计次数。0 = 中断从未到达 CPU。 */
volatile uint32_t g_usb_isr_entries = 0u;
/** 最近一次进 ISR 时 INTSTS0 的值（判断当时是哪一位引起的）。 */
volatile uint32_t g_usb_isr_last_sts = 0u;
/** 进 ISR 时 INTSTS0 的"或"累积（把出现过的状态位都留下）。 */
volatile uint32_t g_usb_isr_sts_or = 0u;

/** 前 16 次进 ISR 时的 (INTSTS0, BRDYSTS) 快照，用来看事件先后与当时的 CTSQ/管道状态。 */
volatile uint32_t g_usb_isr_seq_sts[16];
volatile uint32_t g_usb_isr_seq_brdy[16];
/** 延迟之后、交给 FSP 之前再读一次 INTSTS0 —— 这才是 FSP 真正会看到的 CTSQ。 */
volatile uint32_t g_usb_isr_seq_post[16];
/** 已记录的条数（封顶 16）。 */
volatile uint32_t g_usb_isr_seq_cnt = 0u;

/* 本文件导出的 ISR 包装入口 —— 供 ra_gen/vector_data.c 的向量表 [0] 引用。
 * 显式给出原型, 免掉 -Wmissing-declarations 告警(它是全局函数但没有头文件)。 */
void usbfs_isr_probe(void);

void usbfs_isr_probe(void)
{
    uint16_t sts  = *(volatile uint16_t *) 0x40090040UL;   /* INTSTS0  */
    uint16_t brdy = *(volatile uint16_t *) 0x40090046UL;   /* BRDYSTS  */

    g_usb_isr_entries++;
    g_usb_isr_last_sts = (uint32_t) sts;
    g_usb_isr_sts_or  |= (uint32_t) sts;

    if (g_usb_isr_seq_cnt < 16u)
    {
        g_usb_isr_seq_sts[g_usb_isr_seq_cnt]  = (uint32_t) sts;
        g_usb_isr_seq_brdy[g_usb_isr_seq_cnt] = (uint32_t) brdy;
        g_usb_isr_seq_cnt++;
    }

    /* ⚠ 关键补丁: FSP 自己的注释写着 "CTSQ bit changes later than CTRT bit for ASSP",
     * 它的做法是在 CTRT 分支里**重新读一次 INTSTS0** 拿最新 CTSQ。
     * 但本板实测(200MHz 内核 + 48MHz USBCLK)进 ISR 太快, 重读时 CTSQ 仍停在 IDST(0),
     * 于是走了 `usb_pstd_stand_req0()` 的空分支(req_type 还是 0), CTRT 随即被清掉,
     * 之后硬件**不会**再补一次 CTRT ⇒ 永远卡在 CTSQ=RDDS、VALID=1、USBREQ=0x0680。
     *
     * 实测证据: USBREQ=0x0680(GET_DESCRIPTOR) / USBLENG=0x0040 一直挂着,
     *           而 g_usb_pstd_req_reg 与 g_usb_pstd_req_type 恒为 0。
     *
     * 这里在 CTRT 待处理时先空转约 1~2 µs, 让硬件的 CTSQ 追上 CTRT, 再交给 FSP。
     * 只延迟时间、不改变任何寄存器语义。 */
    if (0u != (sts & 0x0800u))                 /* INTSTS0.CTRT */
    {
        for (volatile uint32_t i = 0u; i < 120u; i++)
        {
            __NOP();
        }
    }

    if (g_usb_isr_seq_cnt <= 16u)
    {
        /* 延迟后重读: FSP 内部会再读一次 INTSTS0 并拿它的 CTSQ 决定派发分支,
         * 所以这个值才是"FSP 看到的 CTSQ"。与 pre 值对比即可判定是否滞后。 */
        g_usb_isr_seq_post[(g_usb_isr_seq_cnt > 0u) ? (g_usb_isr_seq_cnt - 1u) : 0u] =
            (uint32_t) (*(volatile uint16_t *) 0x40090040UL);
    }

    usbfs_interrupt_handler();

    /* ⚠ 这里**不再**调用 usb_pstd_pcd_task()。
     * 派发必须放在主循环(线程上下文), 因为 FSP 的类请求数据阶段 API
     * (R_USB_PeriControlDataGet/Set) 明确写了"禁止在中断函数/回调里调用"
     * (r_usb_basic.c:3670-3672)。在 ISR 里派发会让 SetCommState 超时(错误121)。
     * 见 USB_CDC_Poll() —— 由主循环以 2 ms 粒度反复调用。 */
}
