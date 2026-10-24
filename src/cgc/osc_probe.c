#include "cgc/osc_probe.h"

#include "hal_data.h"

/* 轮询上限: 每次循环约 4 个周期(读寄存器 + 判断)。200MHz 下 1ms 约 50,000 次,
 * 200,000 次约 4ms —— 20MHz 晶振的起振时间典型 1~5ms, 给 50 倍余量即 10ms 足够。
 * 这里取 500,000 (约 10ms), 超时即判定"没有晶振/晶振损坏"。 */
#define OSC_PROBE_LOOP_MAX      (500000UL)

/* PRCR 解锁键: KEY(bit15..8)=0xA5, PRC0(bit0)=1 (时钟生成电路可写) */
#define OSC_PROBE_PRCR_UNLOCK   (0xA501U)
#define OSC_PROBE_PRCR_LOCK     (0xA500U)

/* MOSCWTCR.MSTS = 0x0F: 最长的起振稳定等待时间(约 8ms @20MHz), 保证慢起振的晶振也能被认可 */
#define OSC_PROBE_MOSCWTCR      (0x0FU)

static osc_probe_result_t s_result = { 0u, 0u, 0u, 0u };

void OSC_Probe_Run(void)
{
    uint32_t i;

    if (0u != s_result.tried)
    {
        return;
    }
    s_result.tried = 1u;

    /* 1) 解锁时钟寄存器, 设最长稳定等待 */
    R_SYSTEM->PRCR       = OSC_PROBE_PRCR_UNLOCK;

    /* 1a) 振荡器驱动能力: 20MHz 需要较大的驱动才有机会起振。
     *     AUTODRVEN=1 让硬件按频率自动选驱动能力(MODRV 被忽略), 比固定档位稳。
     *     本工程没在 RASC 里启用 XTAL, FSP 从不写 MOMCR, 所以这里必须自己配。 */
    R_SYSTEM->MOMCR_b.AUTODRVEN = 1u;

    R_SYSTEM->MOSCWTCR   = OSC_PROBE_MOSCWTCR;

    /* 2) 启动 MAIN_OSC (MOSTP = 0) */
    R_SYSTEM->MOSCCR_b.MOSTP = 0u;

    /* 3) 轮询 OSCSF.MOSCSF, 带硬上限 —— 绝不允许在这里等死
     *    (曾经就是无条件等 MOSCSF 导致整天假死在 SystemInit 里) */
    for (i = 0u; i < OSC_PROBE_LOOP_MAX; i++)
    {
        if (0u != R_SYSTEM->OSCSF_b.MOSCSF)
        {
            break;
        }
    }

    s_result.wait_loops = i;
    s_result.moscsf     = (uint8_t) R_SYSTEM->OSCSF_b.MOSCSF;
    s_result.pll2sf     = (uint8_t) R_SYSTEM->OSCSF_b.PLL2SF;

    /* 4) 停回 MAIN_OSC: 探测不改动系统时钟, 系统仍跑 HOCO->PLL->200MHz */
    R_SYSTEM->MOSCCR_b.MOSTP = 1u;

    /* 5) 上锁, 恢复默认保护 */
    R_SYSTEM->PRCR = OSC_PROBE_PRCR_LOCK;
}

const osc_probe_result_t * OSC_Probe_Result(void)
{
    return &s_result;
}
