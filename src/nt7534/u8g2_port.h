#ifndef U8G2_PORT_H_
#define U8G2_PORT_H_

#include <stdint.h>
#include <stdbool.h>

#include "u8g2.h"

/** 1 = 用 u8g2 画界面（字库自带真 ° 字形、有线段/矩形/圆/位图）；
 *  0 = 用 bsp_nt7534.c 里那套手写绘图 API（8x16 定长字库 + 合成 ℃）。 */
#define U8G2_PORT_ENABLE    (1)

/** 硬件初始化 + 绑定 u8g2，返回句柄（全局唯一）。内部会调 NT7534_Init()。 */
u8g2_t * U8G2_Port_Init(void);

/** 取已初始化的句柄（不重新初始化）。 */
u8g2_t * U8G2_Port(void);

/** 把缓冲送到屏（= 重申几何 + u8g2_SendBuffer）。 */
void U8G2_Port_Flush(void);

/** 脏区刷新: 只把 (tx,ty) 起、tw×th 个 tile(每 tile 8px) 的区域送到屏。
 *  u8g2 tile 坐标: tx/ty 是 tile 索引, tw/th 是 tile 数。用于风扇动画只刷风扇格。 */
void U8G2_Port_FlushArea(uint8_t tx, uint8_t ty, uint8_t tw, uint8_t th);

/** 已刷新帧数（探针可读, 在涨 = 界面在实时刷新）。 */
extern volatile uint32_t g_u8g2_frames;

/** u8g2 帧缓冲（1024 B, 页优先, bit0 在上）。探针可直接 dump 它还原屏幕。 */
extern uint8_t g_u8g2_fb[1024];

#endif /* U8G2_PORT_H_ */
