/* ============================================================
 * NT7534 传输层：软件位打 / 硬件 SPI0 / SPI0+DMAC
 *
 * MODE 0（默认）: 本文件的函数都是空实现，实际位打在 bsp_nt7534.c 里，
 *                 行为与 v24 完全一致，零风险。
 * MODE 1        : R_SPI_Write + 等 SPI_EVENT_TRANSFER_COMPLETE。
 * MODE 2        : 目前等同 MODE 1（DMAC 分支在 S3 阶段接上）。
 *
 * 为什么阻塞写要等回调而不是等寄存器:
 *   FSP 6.6.0 的 r_spi 没有 StatusGet(idle)，完成通知只走 p_callback 的
 *   SPI_EVENT_TRANSFER_COMPLETE。所以改成"置标志-启动-等标志"，
 *   并带超时保护，避免屏掉线时卡死在主循环里。
 * ============================================================ */

#include "nt7534/nt_bus.h"
#include "nt7534/bsp_nt7534.h"

#include "hal_data.h"

volatile uint32_t g_nt_spi_events = 0u;
volatile uint32_t g_nt_spi_done   = 0u;
/** S3 观测计数器 */
volatile uint32_t g_nt_dma_kicks  = 0u;
volatile uint32_t g_nt_dma_skip   = 0u;
/** 实测生效的位率与分频寄存器值（probe-rs 可直读，用于确认提速真的生效） */
volatile uint32_t g_nt_spi_bitrate_actual = 0u;
volatile uint32_t g_nt_spi_spbr = 0xFFFFFFFFu;
volatile uint32_t g_nt_spi_brdv = 0xFFFFFFFFu;

#if (NT7534_BUS_MODE >= 1)
/** 1 = 传输完成（含初始空闲态） */
static volatile uint8_t s_spi_done = 1u;

/** MODE>=2: 1 = 有一笔异步传输在飞 */
static volatile uint8_t s_async_busy = 0u;

/** 运行期可改位率用的配置副本。
 *  g_spi0_cfg / g_spi0_ext_cfg 是 RASC 生成的 const（在 flash 里），改不了；
 *  这里把它们整份拷到 RAM，只换 spck_div 再 Open。
 *  好处：调位率不必回头找 RASC 重新生成（那条路一次要跑很久）。 */
static spi_extended_cfg_t s_spi_ext;
static spi_cfg_t          s_spi_cfg;
static const spi_cfg_t  * s_spi_cfg_use = &g_spi0_cfg;
#endif

/** FSP 回调（hal_data.c 里 .p_callback 指向它）。跑在中断上下文，必须极短。 */
void spi0_callback(spi_callback_args_t * p_args)
{
    g_nt_spi_events++;

    if (SPI_EVENT_TRANSFER_COMPLETE == p_args->event)
    {
        g_nt_spi_done++;
#if (NT7534_BUS_MODE >= 1)
        s_spi_done = 1u;
#endif
#if (NT7534_BUS_MODE >= 2)
        s_async_busy = 0u;
#endif
    }
}

fsp_err_t NT_Bus_Init(void)
{
#if (NT7534_BUS_MODE == 0)
    /* 软件 SPI：引脚由 bsp_nt7534.c 自己配，这里没有可做的 */
    return FSP_SUCCESS;
#else
    rspck_div_setting_t div;

    s_spi_done = 1u;

    /* 按 NT7534_SPI_BITRATE 现算分频；算不出来（超出范围）就退回 RASC 生成的 937.5k 配置，
     * 不拿一个"猜的"分频去冒花屏的险。 */
    if (FSP_SUCCESS == R_SPI_CalculateBitrate((uint32_t) NT7534_SPI_BITRATE, &div))
    {
        s_spi_cfg      = g_spi0_cfg;
        s_spi_ext      = *(const spi_extended_cfg_t *) g_spi0_cfg.p_extend;
        s_spi_ext.spck_div = div;
        s_spi_cfg.p_extend = &s_spi_ext;

        s_spi_cfg_use  = &s_spi_cfg;

        g_nt_spi_spbr  = div.spbr;
        g_nt_spi_brdv  = div.brdv;
        /* 位率 = PCLKA / (2 * (spbr+1) * 2^brdv) */
        g_nt_spi_bitrate_actual =
            (uint32_t) (R_FSP_SystemClockHzGet(BSP_FEATURE_SPI_CLOCK) /
                        (2u * ((uint32_t) div.spbr + 1u) * (1u << div.brdv)));
    }

    return R_SPI_Open(&g_spi0_ctrl, s_spi_cfg_use);
#endif
}

void NT_Bus_Write(const uint8_t * p, uint32_t n)
{
#if (NT7534_BUS_MODE == 0)
    (void) p;
    (void) n;
#else
    fsp_err_t err;
    uint32_t  guard;

    if (0u == n)
    {
        return;
    }

    s_spi_done = 0u;

    err = R_SPI_Write(&g_spi0_ctrl, p, n, SPI_BIT_WIDTH_8_BITS);
    if (FSP_SUCCESS != err)
    {
        /* 上一笔还没完或参数非法：放弃本次，但把标志恢复成空闲，
         * 否则调用方会一直等。 */
        s_spi_done = 1u;
        return;
    }

    /* 超时保护: 3.75MHz 下 128B 约 273µs; 给 ~2ms 的余量(按主频估算的粗计数),
     * 真卡住就放弃, 不把主循环锁死。 */
    guard = 400000u;
    while ((0u == s_spi_done) && (0u != guard))
    {
        guard--;
    }
#endif
}

bool NT_Bus_WriteAsync(const uint8_t * p, uint32_t n)
{
#if (NT7534_BUS_MODE >= 2)
    if (0u != s_async_busy)
    {
        return false;                 /* 上一笔还在飞，本次不发，调用方下轮再试 */
    }

    if (0u == n)
    {
        return true;
    }

    s_spi_done = 0u;

    if (FSP_SUCCESS != R_SPI_Write(&g_spi0_ctrl, p, n, SPI_BIT_WIDTH_8_BITS))
    {
        s_spi_done = 1u;
        return false;
    }

    s_async_busy = 1u;
    return true;
#else
    (void) p;
    (void) n;
    return false;
#endif
}

bool NT_Bus_IsIdle(void)
{
#if (NT7534_BUS_MODE >= 1)
    return (0u != s_spi_done) ? true : false;
#else
    return true;
#endif
}
