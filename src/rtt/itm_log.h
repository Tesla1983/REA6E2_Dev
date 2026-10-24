/********************************************************************
 * ITM (Instrumentation Trace Macrocell) 输出 —— 刺激端口 0
 *
 * 与 RTT 的区别：
 *   RTT  = 主机轮询 RAM 里的环形缓冲，芯片侧几乎零成本，但要额外 RAM；
 *   ITM  = 芯片侧写 ITM 刺激寄存器，由 CoreSight 通路送出，几乎不占 RAM。
 *
 * 本芯片(RA6E2 / Cortex-M33)的 ROM 表实测：
 *   0xe0000000 ITM        ✔ 存在
 *   0xe0047000 TraceFunnel ✔
 *   0xe0048000 TMC (ETF)   ✔ 可当片上缓冲
 *   0xe0041000 ETM-M33    ✔
 *   —— 但**没有 TPIU**，所以 ITM 数据无法从 TRACESWO 引脚输出，
 *      只能走 TMC/ETF 由调试器读走（probe-rs itm memory）。
 *
 * 主机侧：probe-rs itm --chip R7FA6E2BB --probe 0d28:0204 memory <ICLK_Hz>
 *********************************************************************/
#ifndef ITM_LOG_H
#define ITM_LOG_H

#include <stdint.h>

/** 因 ITM FIFO 满而丢弃的字符数（主机读得慢时会增长） */
extern volatile uint32_t g_itm_dropped;

/** 芯片侧使能 ITM + TraceFunnel。必须在任何 ITM 输出前调用一次 */
void ITM_Init (void);

/** 主机是否已使能 ITM 与端口 0（0 = 主机没在抓，写了也白写） */
int  ITM_IsEnabled (void);

/** 写一个字符到刺激端口 0。主机没连或 FIFO 满时直接丢弃，绝不阻塞 */
void ITM_PutChar (char c);

/** 写字符串到刺激端口 0 */
void ITM_PutString (const char * p_str);

/** 格式化输出，语义同 printf（nano.specs 不支持 %f，单次最长 128 字节） */
int  ITM_Printf (const char * p_fmt, ...);

#endif /* ITM_LOG_H */
