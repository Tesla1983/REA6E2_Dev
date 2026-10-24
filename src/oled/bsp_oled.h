/* ============================================================
 * 1.3 寸单色 OLED (128x64) 驱动 —— I2C 接口
 *
 * 【器件: SH1106】(固定型号; 规格见 docs/1.3寸OLED开发文档.md §3)
 *
 *   本驱动只适用于 SH1106:
 *     - 电荷泵 0xAD 0x8B, 配 0x33 设 VPP      (写 0x8D 不开泵, VPP 不升压)
 *     - GDDRAM 132 列 x 8 页, 可见 128 列居中 -> 列偏移 2
 *     - 只有页寻址 0xB0+i / 0x00 / 0x10       (0x20/0x21/0x22 这类寻址命令本芯片不接收)
 *     - 列指针只在页内递增, 到本页末回卷本页列 0, 不跨页 -> 必须分 8 页写
 *
 *   ⚠ 不要改成 0x8D 电荷泵 + 水平寻址的写法: 电荷泵不开(VPP 不升压), 列指针不跨页,
 *     一次连写 1024 字节会全堆在页 0 反复覆盖 -> 屏上乱闪。
 *
 * SH1106 要点:
 *   - GDDRAM 132 列 x 8 页; 可见的 128 列对应 RAM 列 2..129 -> 列偏移 = 2
 *   - 只支持页寻址: 发 0xB0+页号, 设列地址(0x0X 低 4 位 / 0x1X 高 4 位), 再连续写数据
 *   - 每页写完必须重新设页号与列地址, 共 8 次
 * ============================================================ */
#ifndef BSP_OLED_H_
#define BSP_OLED_H_

#include <stdbool.h>
#include <stdint.h>
#include "bsp_api.h"

#define OLED_WIDTH    (128u)          /* 可见宽度 */
#define OLED_HEIGHT   (64u)           /* 可见高度 */
#define OLED_PAGES    (OLED_HEIGHT / 8u)
#define OLED_ADDR     (0x3Cu)         /* 实测 I2C 地址(SA0 = 0) */

/** 可见区在 SH1106 的 132 列 RAM 中的起始列。
 *  官方示例用 0x02(低 4 位) + 0x10(高 4 位) 得到列地址 = 2。
 *  判据: 画满屏边框时四条边都紧贴屏幕边缘才对;
 *        若左边框不在最左边、右边框不见 -> 说明这块屏偏移不是 2, 改这个宏。 */
#define OLED_COL_OFFSET   (2u)

/** 字体尺寸(取自 oled_font.h 的 8x16 表 asc2_1608) */
#define OLED_CHAR_W   (8u)
#define OLED_CHAR_H   (16u)

/* ---- 运行时状态, volatile, 便于调试器/探针直接读 ---- */

/** 器件是否在 OLED_ADDR 应答: OLED_ADDR = 在线, 0 = 没找到 */
extern volatile uint8_t  g_oled_addr;
/** 初始化错误码(fsp_err_t); 0 = 初始化全程成功 */
extern volatile uint8_t  g_oled_init_err;
/** 最后一次 I2C 传输的错误码; 0 = 至今无传输错误(用于区分"不亮"和"传输出错") */
extern volatile uint8_t  g_oled_xfer_err;
/** 已刷新帧数, 用于确认程序真的在跑 */
extern volatile uint32_t g_oled_frames;
/** 累计已上屏的页数。除以 frames 得"平均每帧刷几页" —— 脏页优化是否生效的判据:
 *  全刷 = 8.0; 跑马灯只刷文字带 = 2.0 */
extern volatile uint32_t g_oled_pages_flushed;

/* ---- 接口 ---- */

/** 初始化: 开 I2C -> 确认器件在线 -> 发 SH1106 初始化序列 -> 清屏 -> 开显示 */
fsp_err_t OLED_Init (void);

/** 清帧缓冲(不立即上屏, 需再调 OLED_Refresh) */
void OLED_Clear (void);

/**
 * 把帧缓冲刷到 OLED —— **只刷内容有变化的页**。
 *
 * 内部保留一份"已上屏内容"的影子副本, 逐页与帧缓冲比对:
 *   - 内容相同的页直接跳过(不发命令也不发数据)
 *   - 不同的页才设页号+列地址并写 128 字节, 写完同步影子副本
 *
 * 为什么这样而不是"绘图时标脏": 应用每帧都先 OLED_Clear() 再重画,
 * Clear 会把 8 页全标脏, 标脏法就退化成全刷了。按页 diff 与调用顺序无关。
 *
 * 代价: 多 1 KB RAM 存影子副本; 每帧 8 次 128 字节比对(< 5 µs, 可忽略)。
 * 收益: 跑马灯文字只占 y=24..39 即页 3/4, 每帧数据量 1064 -> 266 字节(÷4)。
 *
 * 上电第一帧强制全刷(影子副本初值不可信), 之后才进入 diff 模式。
 */
void OLED_Refresh (void);

/** 画点。x/y 越界自动丢弃。 */
void OLED_DrawPixel (int16_t x, int16_t y, uint8_t on);

/** 画 8x16 字符, (x, y) 为左上角像素坐标 */
void OLED_DrawChar (int16_t x, int16_t y, char c);

/** 画 8x16 字符串, 逐字符裁剪, 可整体处于屏外(用于跑马灯) */
void OLED_DrawString (int16_t x, int16_t y, const char * p_str);

/** 画矩形框(1 像素边) */
void OLED_DrawRect (int16_t x0, int16_t y0, int16_t x1, int16_t y1);

/**
 * 画单色位图(行优先格式)。
 *
 * @param x0,y0    左上角像素坐标(可为负, 自动裁剪)
 * @param p_bmp    位图数据。行优先: 每行 stride 字节, bit7 = 该行最左像素
 * @param w,h      位图宽高(像素)
 * @param stride   每行字节数 = ceil(w/8)
 * @param invert   0 = 原样(bit 1 -> 点亮);  1 = 整体取反(bit 0 -> 点亮)
 *
 * 语义是"不透明贴图": 图覆盖的矩形区域会被整块重画 ——
 * 位图里为 0 的像素会把帧缓冲对应点**擦掉**, 而不是跳过。
 * 想做"透明背景"需要另加一个 key color 参数, 目前没这个需求。
 *
 * 不走 OLED_DrawPixel 逐点调用: 50x52 = 2600 像素, 逐点要走 2600 次
 * 边界判断 + 除法(y/8、y%8)。这里把页基址与位掩码提到行循环外,
 * 每行只算一次, 实测可省掉约 90% 的开销。
 */
void OLED_DrawBitmap (int16_t x0, int16_t y0, const uint8_t * p_bmp,
                      uint16_t w, uint16_t h, uint16_t stride, uint8_t invert);

#endif /* BSP_OLED_H_ */
