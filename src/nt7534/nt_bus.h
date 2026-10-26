/* ============================================================
 * NT7534 传输层抽象（软件位打 / 硬件 SPI0 / SPI0+DMAC）
 *
 * 为什么单独抽一层：
 *   屏的"画什么"由 u8g2 + nt7534.c 决定，"怎么把字节送出去"才是可换的。
 *   把传输层单独成文件，切换实现时上层一行不用改，也方便随时用
 *   NT7534_BUS_MODE 退回软件 SPI。
 *
 * 三档（见 bsp_nt7534.h 的 NT7534_BUS_MODE）:
 *   0 = 软件位打（默认，v24 及以前的行为，完全不变）
 *   1 = 硬件 SPI0，阻塞写
 *   2 = 硬件 SPI0 + DMAC0，异步整屏
 *
 * 引脚分工（重要）:
 *   P302 = SPI0_RSPCK、P207 = SPI0_MOSI 交给硬件；
 *   P301 = /CS、P403 = A0/RS、P409 = /RES 仍然是 GPIO，由本层/驱动控制。
 *   不能把 /CS 交给 SPI0 的 SSLA：NT7534 的"3 字节页地址命令 + 128 字节数据"
 *   必须在同一个 CS 低电平窗口内完成，SSLA 每次启动传输都会翻一次 CS。
 * ============================================================ */
#ifndef NT_BUS_H_
#define NT_BUS_H_

#include <stdbool.h>
#include <stdint.h>
#include "bsp_api.h"

/** 观测用计数器（volatile，可用 probe-rs 直读） */
extern volatile uint32_t g_nt_spi_events;   /* spi0 回调次数（含所有事件类型） */
extern volatile uint32_t g_nt_spi_done;     /* SPI_EVENT_TRANSFER_COMPLETE 次数 */

/** S3 异步分页刷新观测计数器（volatile，probe-rs 可直读） */
extern volatile uint32_t g_nt_dma_kicks;    /* 成功发起的异步 DMA 页数 */
extern volatile uint32_t g_nt_dma_skip;     /* 因总线忙被跳过的刷新次数 */

/** 初始化传输层。MODE 0 时什么也不做（引脚由 bsp_nt7534.c 自己配）。 */
fsp_err_t NT_Bus_Init(void);

/** 阻塞写：返回时字节已全部移出（SPI 模式下等 idle）。MODE 0 不走这里。 */
void NT_Bus_Write(const uint8_t * p, uint32_t n);

/** 异步写（仅 MODE 2）。忙则返回 false，调用方下轮再试。 */
bool NT_Bus_WriteAsync(const uint8_t * p, uint32_t n);

/** 传输层是否空闲（MODE 2 的页状态机用）。 */
bool NT_Bus_IsIdle(void);

#endif /* NT_BUS_H_ */
