/* ============================================================
 * 3.5" 单色屏驱动（VTM88723V01 / NT7534，4 线 SPI，软件位打）
 *
 * 硬件事实（详见 docs/3.5寸屏开发文档.md §1）:
 *   - 128x64 dots FSTN, NT7534, 132 列 x 65 行 RAM
 *   - 逻辑电平 3.3V, 与 MCU 同电平, 无需电平转换
 *   - 只写不读: 没有 MISO, 因此 MCU 侧无法回读屏的状态
 *
 * 接线（规划文档 §2.3, 已实测可用）:
 *   RS  = P403    L=命令 / H=数据
 *   RES = P409    低有效复位
 *   CLK = P302    SPI 时钟（空闲低, 上升沿采样 -> Mode 0）
 *   SID = P207    SPI 数据（MOSI, MSB first）
 *   CS  = P301    片选, 低有效
 *
 * 为什么用软件 SPI: 不需要改 RASC、不新增 SPI 实例, 少一个变量。
 * 出问题时只可能是接线或初始化序列, 不会是 SPI 配置。1 Hz 刷新够用。
 * 引脚已按硬件 SPI0 排好, 将来要提速只换传输层, 接线一根不动。
 * ============================================================ */
#ifndef BSP_NT7534_H_
#define BSP_NT7534_H_

#include <stdbool.h>
#include <stdint.h>
#include "bsp_api.h"

#define NT7534_WIDTH    (128u)
#define NT7534_HEIGHT   (64u)
#define NT7534_PAGES    (NT7534_HEIGHT / 8u)

/** 可见区在 NT7534 的 132 列 RAM 中的起始列（默认值）。
 *  厂家 demo 用 col+1。列偏移错了的症状是: 一边丢 1~2 列、另一边多 1~2 列空。
 *  ⚠ ADC 反向(0xA1) 之后列映射也跟着反, 偏移量要重新对一次 —— 可用
 *  `NT7534_SetColOffset()` 在运行中扫, 找边框四边都贴边的那一个。 */
#define NT7534_COL_OFFSET   (3u)

/** Display Start Line（命令 0x40 | 本值；本值 = 屏顶对应 RAM 第几行）。
 *  ✅ **实测本屏必须用 0**（命令 0x40）。
 *  厂家 demo 写的 0x60（= 32 行）在这块屏上是错的：用了以后画面整体上移 32 行，
 *  表现为"顺序错位一轮" —— 本应第一行的 `CPU` 跑到第三行，`RAM` 跑到第一行。
 *  （厂家那套 demo 画的是对称网格，错 32 行也看不出来，所以那个值一直没人发现。） */
#define NT7534_START_LINE   (0x00u)

/** 对比度（电子音量）0x00..0x3F。厂家值 0x2B。 */
#define NT7534_CONTRAST     (0x2Bu)

/** 显存页 → 物理页的旋转量（单位: 页, 每页 8 行）**4 页 = 32 行**。
 *
 *  事实（2026-10-22 两次独立目视确认）:
 *    帧缓冲第 0 行画的东西，实际出现在屏的第 32 行；把起始行命令从 0x60 改成 0x40 之后
 *    **现象完全不变** —— 说明本模块的 RAM→玻璃行映射被固定成了"整体上移 32 行、绕回"，
 *    起始行命令对它不起作用（厂家 demo 用 0x60 也看不出，因为它画的是上下对称的网格）。
 *
 *  修法: 不跟控制器较劲，在**发送侧**把页顺序转 4 页。写显存时第 p 页取自缓冲第 (p+4)&7 页。
 *  等价性: 4+4=8，所以这个旋转是**自逆**的 —— 转一次补偿，转两次还原，
 *  方向不可能搞反（原来的疑问"到底该 +4 还是 -4"在这里不成立）。
 *
 *  ⚠ 如果将来换一块同型号但映射正常的屏，把这里改 0 即可，其它代码一行不动。 */
#define NT7534_PAGE_ROT     (4u)

/** 伪字符: 画 "℃"（字库里没有这个字形，由驱动用"小圆圈 + 大写 C"合成）。
 *  用法: 与普通字符一样塞进字符串，如 `snprintf(buf, n, "CPU 48%c", NT7534_CH_DEGC);`
 *  宽度与普通字符相同（8 px），所以"每行 16 字符"的排版预算不变。 */
#define NT7534_CH_DEGC      ((char) 0x01)

/* ---- 显示方向（镜像/倒置）----
 * NT7534 有两个独立的扫描方向开关，组合起来得到 4 种朝向:
 *   ADC  (0xA0/0xA1): 段(列)扫描方向 —— 置 1 = 左右镜像
 *   COM  (0xC0/0xC8): 共(行)扫描方向 —— 置 1 = 上下镜像
 * 两个都置 1 就是 180° 倒过来。
 * 本屏实测: 按 0xA0 + 0xC0 显示出来是左右镜像的, 故默认把 ADC 反过来。
 * 若实物不同, 改这两个宏即可, 不必动其它代码。 */
#define NT7534_ADC_REVERSE  (1u)
#define NT7534_COM_REVERSE  (0u)

/** 软件 SPI 半周期(µs)。1 µs ≈ 500 kHz, 整屏 1024 字节约 25~40 ms。
 *  NT7534 串行时钟上限不高, 慢一点最稳; 1 Hz 刷新完全够。 */
#define NT7534_HALF_BIT_US  (1u)

/* ---- 引脚 ----
 *
 * ⚠ RS/RES 原本接 P212/P213, 已于 2026-10-22 改到 P403/P409。
 *   原因: P212/P213 是 EXTAL/XTAL(20MHz 晶振) —— 屏的杜邦线挂上去后形成容性负载,
 *   晶振直接起振不了(实测 MAIN_OSC 探测跑满 10ms 超时)。而 USB CDC 需要 48MHz 精确时钟,
 *   RA6E2 只能由 MAIN_OSC 20MHz -> PLL x12 = 240MHz -> /5 = 48MHz 得到, 故必须让出这两个脚。
 *   P403(RTCIC1) / P409(GTOWUP) 两个功能都没用到, 且与 LED(P402)、OLED(P408/P206) 同在 P4 组。
 */
#define NT7534_PIN_RS    ((bsp_io_port_pin_t) BSP_IO_PORT_04_PIN_03)   /* P403 */
#define NT7534_PIN_RES   ((bsp_io_port_pin_t) BSP_IO_PORT_04_PIN_09)   /* P409 */
#define NT7534_PIN_CLK   ((bsp_io_port_pin_t) BSP_IO_PORT_03_PIN_02)   /* P302 */
#define NT7534_PIN_SID   ((bsp_io_port_pin_t) BSP_IO_PORT_02_PIN_07)   /* P207 */
#define NT7534_PIN_CS    ((bsp_io_port_pin_t) BSP_IO_PORT_03_PIN_01)   /* P301 */

/* ---- 上电自检结果（volatile, 探针可直读）---- */
/** 上电时把 5 根信号线设成「输入 + 内部上拉」读回的电平:
 *  [0]=RS [1]=RES [2]=CLK [3]=SID [4]=CS。全 1 才正常。
 *  读到 0 = 该线被外部拉低(接错到 GND、或与其它信号短路)。
 *  注意: 全 1 只说明「没被拉低」, 不能证明线一定接到了屏上。 */
extern volatile uint8_t g_nt_probe_levels[5];
/** 上电自检中被拉低的线数。>0 时不驱动任何引脚, 直接进错误状态。 */
extern volatile uint8_t g_nt_probe_bad;
/** 初始化过程中累计的传输次数与最后一次错误码（软件 SPI 无错误码, 仅计数） */
extern volatile uint32_t g_nt_bytes_sent;

/* ---- 接口 ---- */

/** 上电引脚自检: 5 根线设输入+上拉, 读回电平, 结果写 g_nt_probe_levels / g_nt_probe_bad。
 *  纯输入操作, 不驱动任何引脚。 */
void NT7534_PinProbe(void);

/** 初始化: 配置引脚为输出 -> 硬复位 -> 厂家初始化序列 -> 清屏 -> 开显示。
 *  返回 FSP_SUCCESS 表示流程走完（屏是否真的亮, 软件无法回读, 只能目视）。 */
fsp_err_t NT7534_Init(void);

/** 向屏发一个字节（RS/CS 由调用方自己控制）。
 *  只给 u8g2 移植层用: u8g2 自己管 RS/CS 与命令数据流, 只需要"把这一个字节轰到 SPI 上"。 */
void NT7534_BusWrite(uint8_t b);

/** 全点亮 / 恢复正常显示: on=1 -> 0xA5（所有像素点亮）, on=0 -> 0xA4 */
void NT7534_AllPointsOn(uint8_t on);

/** 设置 Display Start Line（0x40|line, line = 0..63） */
void NT7534_SetStartLine(uint8_t line);

/** 重新下发「几何/极性」三条命令: ADC 反向、COM 反向、Display Start Line。
 *
 *  为什么需要它（2026-10-22 踩过的坑）:
 *    本屏只写不读, MCU 无法回读控制器状态; 这三条参数原本只在 `NT7534_Init()` 里下发一次。
 *    如果那一帧没真正生效（烧录后内核没复位、控制器被上电抖动/静电打乱、屏断电重启而 MCU 没重启）,
 *    画面就会一直错着 —— 而 LCD 会**无限期保留旧图像**, 看起来完全像"改了代码没生效"。
 *    实测就是这么误判过一次: 起始行已从 0x20 改成 0x00 并烧进 flash, 但芯片没复位,
 *    控制器里仍是旧的 0x20, 画面继续上移 32 行, 与改之前一模一样。
 *
 *  `NT7534_Refresh()` 每帧自动调用它: 代价 3 字节/帧（≈0.3% 带宽）, 换来画面自愈 ——
 *  控制器状态一旦被弄乱, 最多错一帧就自己恢复, 不需要人去按复位键。 */
void NT7534_SyncGeometry(void);

/** 运行中改列偏移（0..4 有意义）。改完必须重新 `NT7534_Refresh()` 才看得到效果,
 *  因为列地址是在每页开头下发的。 */
void NT7534_SetColOffset(uint8_t offset);

/** 运行中改变显示方向（不需要重画帧缓冲, 显存内容不变, 只是扫描方向变）。
 *  @param adc_rev  1 = 左右镜像（0xA1）, 0 = 正常（0xA0）
 *  @param com_rev  1 = 上下镜像（0xC8）, 0 = 正常（0xC0） */
void NT7534_SetOrientation(uint8_t adc_rev, uint8_t com_rev);

/** 改对比度（0x00..0x3F）, 会立即生效 */
void NT7534_SetContrast(uint8_t v);

/** 清帧缓冲（不上屏, 需再调 NT7534_Refresh） */
void NT7534_Clear(void);

/** 把帧缓冲整屏刷到屏上（8 页 x 128 字节, 不做脏页优化 —— 1 Hz 刷新不需要） */
void NT7534_Refresh(void);

/** 画点, 越界自动丢弃 */
void NT7534_DrawPixel(int16_t x, int16_t y, uint8_t on);

/** 画 8x16 字符（字库与 1.3" OLED 共用 src/oled/oled_font.h） */
void NT7534_DrawChar(int16_t x, int16_t y, char c);

/** 画 "℃"：C 的左上角一个 3x3 空心小圈（行 0..2 × 列 0..2），下面是字库的大写 C。
 *  字库字形只占第 3..13 行、列 2..6，所以左上角那块空白正好塞得下 3x3，与 C 不重叠。 */
void NT7534_DrawDegC(int16_t x, int16_t y);

/** 画字符串, 逐字符裁剪 */
void NT7534_DrawString(int16_t x, int16_t y, const char * p_str);

/** 画 1 像素矩形框 */
void NT7534_DrawRect(int16_t x0, int16_t y0, int16_t x1, int16_t y1);

/** 填充实心矩形 */
void NT7534_FillRect(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t on);

#endif /* BSP_NT7534_H_ */
