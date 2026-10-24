/* 主振荡器(EXTAL/XTAL, 20MHz)起振探测
 *
 * 为什么需要它: Type-C 走的是 MCU 原生 USB(P814=D+ / P815=D-), 要做 USB CDC 就必须有
 * 精确的 48MHz 时钟。RA6E2 的 HOCO 只有 16/18/20MHz 三档, 拿不到 48MHz; PLL 现为
 * 200MHz 也除不出 48MHz。唯一路径是 PLL2 = 20MHz x 12 = 240MHz, 再 USBCKDIV /5 = 48MHz
 * —— 这要求 20MHz 时钟源本身准确, 只有外部晶振满足(USB FS 要求 +/-0.25%)。
 *
 * 所以先回答一个事实问题: 这块板上到底有没有那颗 20MHz 晶振、它能不能起振。
 * 探测只做一次, 全程约 100ms 封顶, 结束后立即把 MAIN_OSC 停回去, 不改动系统时钟。
 */
#ifndef OSC_PROBE_H_
#define OSC_PROBE_H_

#include <stdint.h>

/** 探测结果(供探针/RTT 读取) */
typedef struct
{
    uint8_t  tried;      /* 1 = 已执行过探测 */
    uint8_t  moscsf;     /* 1 = MAIN_OSC 起振稳定(有晶振且工作) */
    uint8_t  pll2sf;     /* 1 = PLL2 稳定(探测前后一致即可) */
    uint32_t wait_loops; /* 实际轮询次数(越小起振越快; 等于上限=超时) */
} osc_probe_result_t;

/** 执行探测(幂等: 第二次调用直接返回上次结果)。结果约 100ms 内可用。 */
void OSC_Probe_Run(void);

/** 取结果(只读指针)。 */
const osc_probe_result_t * OSC_Probe_Result(void);

#endif /* OSC_PROBE_H_ */
