/* ============================================================
 * 3.5" NT7534 驱动实现（软件 SPI）
 *
 * 时序照抄厂家 demo（STC8H8K64U-VTM88723V01-DEMO.c 第 60-110 行）:
 *   CLK 空闲低 -> 置 SID -> 拉高 CLK（上升沿采样）-> 保持 1 µs
 *   MSB first, 每字节后给 10 µs 间隔（这里取 2 µs + 每半位 1 µs, 合计更宽）
 *
 * 与 1.3" OLED(SH1106) 的差别只有三处:
 *   1) 传输层从 I2C 换成 SPI（命令/数据靠 RS 引脚区分, 不需要 0x00/0x40 控制字节）
 *   2) 电荷泵命令是 0x2F（SH1106 是 0xAD 0x8B + 0x33）
 *   3) 列地址要 +1 偏移, Display Start Line 用 0x40|line
 * 绘图层（页缓冲 / 画点 / 画字）逻辑与 OLED 完全一致, 字库直接共用。
 * ============================================================ */

#include "nt7534/bsp_nt7534.h"
#include "nt7534/nt_bus.h"

#include <string.h>                   /* memcpy: 双缓冲提交用 */

#include "hal_data.h"                  /* g_ioport_ctrl */
#include "oled/oled_font.h"            /* 8x16 ASCII 字库, 与 OLED 共用 */
#include "rtt/rtt_log.h"

volatile uint8_t  g_nt_probe_levels[5] = {1u, 1u, 1u, 1u, 1u};
volatile uint8_t  g_nt_probe_bad       = 0u;
volatile uint32_t g_nt_bytes_sent      = 0u;

/** 帧缓冲: 8 页 x 128 列, 页优先。bit0 = 该页最上一行（与字库布局一致） */
static uint8_t s_fb[NT7534_PAGES * NT7534_WIDTH];

/** 当前列偏移（运行期可改, 见 NT7534_SetColOffset） */
static uint8_t s_col_offset = NT7534_COL_OFFSET;

/* ---------------- 引脚操作 ---------------- */

#define NT_PIN_WRITE(pin, lv)  (void) R_IOPORT_PinWrite(&g_ioport_ctrl, (pin), (lv))

#define NT_CS_LOW()    NT_PIN_WRITE(NT7534_PIN_CS,  BSP_IO_LEVEL_LOW)
#define NT_CS_HI()     NT_PIN_WRITE(NT7534_PIN_CS,  BSP_IO_LEVEL_HIGH)
#define NT_RS_CMD()    NT_PIN_WRITE(NT7534_PIN_RS,  BSP_IO_LEVEL_LOW)
#define NT_RS_DATA()   NT_PIN_WRITE(NT7534_PIN_RS,  BSP_IO_LEVEL_HIGH)
#define NT_CLK_LOW()   NT_PIN_WRITE(NT7534_PIN_CLK, BSP_IO_LEVEL_LOW)
#define NT_CLK_HI()    NT_PIN_WRITE(NT7534_PIN_CLK, BSP_IO_LEVEL_HIGH)
#define NT_SID_LOW()   NT_PIN_WRITE(NT7534_PIN_SID, BSP_IO_LEVEL_LOW)
#define NT_SID_HI()    NT_PIN_WRITE(NT7534_PIN_SID, BSP_IO_LEVEL_HIGH)
#define NT_RES_LOW()   NT_PIN_WRITE(NT7534_PIN_RES, BSP_IO_LEVEL_LOW)
#define NT_RES_HI()    NT_PIN_WRITE(NT7534_PIN_RES, BSP_IO_LEVEL_HIGH)

#define NT_DELAY_US(n) R_BSP_SoftwareDelay((n), BSP_DELAY_UNITS_MICROSECONDS)

/** 5 根信号线的顺序: RS, RES, CLK, SID, CS（与 g_nt_probe_levels 下标一致） */
static const bsp_io_port_pin_t s_nt_pins[5] =
{
    NT7534_PIN_RS, NT7534_PIN_RES, NT7534_PIN_CLK, NT7534_PIN_SID, NT7534_PIN_CS
};

static void nt_pins_as_input(void)
{
    uint32_t i;

    for (i = 0u; i < 5u; i++)
    {
#if (NT7534_BUS_MODE >= 1)
        /* 硬件 SPI 模式: CLK(P302)/SID(P207) 已是 SPI0 功能脚,
         * 改成 GPIO 输入会把 PSEL 清掉, SPI 直接不出波形 -> 必须跳过。 */
        if ((2u == i) || (3u == i))
        {
            continue;
        }
#endif
        (void) R_IOPORT_PinCfg(&g_ioport_ctrl, s_nt_pins[i],
                               (uint32_t) IOPORT_CFG_PORT_DIRECTION_INPUT
                               | (uint32_t) IOPORT_CFG_PULLUP_ENABLE);
    }
}

static void nt_pins_as_output(void)
{
    /* 空闲电平: CS/RS/RES/SID 高, CLK 低。
     * 驱动强度取 MID: 就算某根线被接错到 GND, 电流也只有几 mA, 不会伤 MCU 或屏。 */
#if (NT7534_BUS_MODE == 0)
    /* 软件 SPI: CLK/SID 必须自己配成 GPIO 输出 */
    (void) R_IOPORT_PinCfg(&g_ioport_ctrl, NT7534_PIN_CLK,
                           (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT
                           | (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW
                           | (uint32_t) IOPORT_CFG_DRIVE_MID);
    (void) R_IOPORT_PinCfg(&g_ioport_ctrl, NT7534_PIN_SID,
                           (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT
                           | (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH
                           | (uint32_t) IOPORT_CFG_DRIVE_MID);
#else
    /* 硬件 SPI: P302/P207 已由 ra_gen/pin_data.c 配成 SPI0 功能引脚,
     * 这里绝对不能再碰, 否则 PSEL 被清掉、SPI 不出波形。 */
#endif
    (void) R_IOPORT_PinCfg(&g_ioport_ctrl, NT7534_PIN_CS,
                           (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT
                           | (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH
                           | (uint32_t) IOPORT_CFG_DRIVE_MID);
    (void) R_IOPORT_PinCfg(&g_ioport_ctrl, NT7534_PIN_RS,
                           (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT
                           | (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH
                           | (uint32_t) IOPORT_CFG_DRIVE_MID);
    (void) R_IOPORT_PinCfg(&g_ioport_ctrl, NT7534_PIN_RES,
                           (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT
                           | (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH
                           | (uint32_t) IOPORT_CFG_DRIVE_MID);
}

/* ---------------- 底层传输 ---------------- */

/** 半位延时: 用少量空转控制 NOP 数(不用 R_BSP_SoftwareDelay, 其单次开销太大)。
 *  ⚠ 实测 volatile 16 次空转整屏要 ~30ms(≈273 kbit/s, 远低于 NT7534 可接受的 1.2–1.6 Mbit/s),
 *    说明延时被高估了。降到 4 次 → 约 1.1 Mbit/s(仍在保守范围内), 整屏 ~7–8ms, 可支撑 ~30fps。
 *  仅 MODE 0 使用。 */
#if (NT7534_BUS_MODE == 0)
static void nt_half_delay(void)
{
    volatile uint32_t i;

    for (i = 0u; i < 4u; i++)
    {
        __asm__ volatile ("nop");
    }
}
#endif

/** 发一个字节（MSB first, CLK 空闲低, 上升沿采样）。
 *  仅 MODE 0（软件 SPI）使用；硬件 SPI 模式下这块代码不参与编译。 */
#if (NT7534_BUS_MODE == 0)
static void nt_write_byte(uint8_t b)
{
    uint8_t i;

    for (i = 0u; i < 8u; i++)
    {
        NT_CLK_LOW();
        if (0u != (b & 0x80u))
        {
            NT_SID_HI();
        }
        else
        {
            NT_SID_LOW();
        }
        nt_half_delay();
        NT_CLK_HI();
        nt_half_delay();
        b = (uint8_t) (b << 1);
    }
}
#endif

/** 批量发送：MODE 0 逐字节位打，MODE>=1 整块交给硬件 SPI。
 *  字节计数放在这里，两种模式都成立（探针可用 g_nt_bytes_sent 判断是否在发数据）。 */
static void nt_write_bytes(const uint8_t * p, uint32_t n)
{
#if (NT7534_BUS_MODE == 0)
    uint32_t i;

    for (i = 0u; i < n; i++)
    {
        nt_write_byte(p[i]);
    }
#else
    NT_Bus_Write(p, n);
#endif

    g_nt_bytes_sent += n;
}

static void nt_cmd(uint8_t c)
{
    NT_CS_LOW();
    NT_RS_CMD();
    nt_write_bytes(&c, 1u);
    NT_CS_HI();
}

/** 设页号 + 列地址（列地址带 s_col_offset 偏移, 厂家 demo 用 +1） */
static void nt_set_page_col(uint8_t page, uint8_t col)
{
    uint8_t c = (uint8_t) (col + s_col_offset);

    nt_cmd((uint8_t) (0xB0u | (page & 0x0Fu)));
    nt_cmd((uint8_t) (0x10u | ((c >> 4) & 0x0Fu)));
    nt_cmd((uint8_t) (c & 0x0Fu));
}

/* ---------------- 对外接口 ---------------- */

void NT7534_PinProbe(void)
{
    uint8_t i;
    uint8_t bad = 0u;

    /* 只做输入 + 上拉读取: 引脚内部上拉约几十 kΩ, 即使某根线接错到 GND,
     * 电流也只有几十 µA, 不会损伤任何东西。 */
    nt_pins_as_input();
    NT_DELAY_US(500u);

    for (i = 0u; i < 5u; i++)
    {
        bsp_io_level_t lv = BSP_IO_LEVEL_HIGH;

#if (NT7534_BUS_MODE >= 1)
        if ((2u == i) || (3u == i))
        {
            /* CLK/SID 属 SPI0 功能脚, 未参与自检 -> 记 2 = 未检测（不能记 1, 那会假装"测过且正常"） */
            g_nt_probe_levels[i] = 2u;
            continue;
        }
#endif

        (void) R_IOPORT_PinRead(&g_ioport_ctrl, s_nt_pins[i], &lv);
        g_nt_probe_levels[i] = (uint8_t) ((BSP_IO_LEVEL_HIGH == lv) ? 1u : 0u);
        if (BSP_IO_LEVEL_HIGH != lv)
        {
            bad++;
        }
    }

    g_nt_probe_bad = bad;
}

void NT7534_BusWrite(uint8_t b)
{
    /* 统一走 nt_write_bytes: 计数与模式选择都在那一处 */
    nt_write_bytes(&b, 1u);
}

void NT7534_BusWriteBuf(const uint8_t * p, uint32_t n)
{
    nt_write_bytes(p, n);
}

fsp_err_t NT7534_Init(void)
{
    static const uint8_t s_zeros[NT7534_WIDTH] = { 0u };
    uint8_t   page;
    fsp_err_t err;

    /* 0) 传输层：MODE 0 空操作；MODE>=1 打开 SPI0。
     *    必须在配引脚之前完成 —— SPI 模式下 P302/P207 的 PSEL 由 BSP 初始化时写好，
     *    R_SPI_Open 只配寄存器，不再动引脚。 */
    err = NT_Bus_Init();
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    nt_pins_as_output();

    /* 1) 硬复位: RES 低 50 µs -> 高, 等 5 ms 让内部电路稳定 */
    NT_RES_LOW();
    NT_DELAY_US(50u);
    NT_RES_HI();
    NT_DELAY_US(5000u);

    /* 2) 厂家初始化序列（VTM88723V01 demo 第 183-208 行） */
    nt_cmd(0xE2u);                     /* 软复位 */
    NT_DELAY_US(200u);

    nt_cmd(0xA0u);                     /* ADC select: 正常（段方向） —— 下面按宏再设一次 */
    nt_cmd(0xA2u);                     /* LCD bias: 1/9 */
    /* 显示方向 + 起始行: 显式下发, 不依赖复位默认值（与每帧重申的是同一段代码） */
    NT7534_SyncGeometry();
    nt_cmd(0x2Fu);                     /* 内部升压 + 稳压 + 电压跟随 全开 */
    NT_DELAY_US(50000u);               /* 等 V0 升起来, 否则前几千字节能看到"淡入" */

    nt_cmd(0x81u);                     /* 电子音量（对比度） */
    nt_cmd(NT7534_CONTRAST);

    nt_cmd(0xA4u);                     /* 全点显示 OFF（正常显示 RAM） */
    nt_cmd(0xA6u);                     /* 正显 */
    nt_cmd(0xE0u);                     /* Read-Modify-Write（厂家值, 只影响读, 无害） */
    nt_cmd((uint8_t) (0x40u | (NT7534_START_LINE & 0x3Fu)));

    /* 3) 先清 RAM 再开显示: 否则上电瞬间会闪一下 RAM 里的随机点 */
    for (page = 0u; page < NT7534_PAGES; page++)
    {
        nt_set_page_col(page, 0u);
        NT_CS_LOW();
        NT_RS_DATA();
        nt_write_bytes(s_zeros, NT7534_WIDTH);
        NT_CS_HI();
    }

    nt_cmd(0xAFu);                     /* 显示 ON */

    return FSP_SUCCESS;
}

void NT7534_AllPointsOn(uint8_t on)
{
    nt_cmd((0u != on) ? 0xA5u : 0xA4u);
}

void NT7534_SetStartLine(uint8_t line)
{
    nt_cmd((uint8_t) (0x40u | (line & 0x3Fu)));
}

void NT7534_SetColOffset(uint8_t offset)
{
    s_col_offset = offset;
}

void NT7534_SetOrientation(uint8_t adc_rev, uint8_t com_rev)
{
    nt_cmd((0u != adc_rev) ? 0xA1u : 0xA0u);
    nt_cmd((0u != com_rev) ? 0xC8u : 0xC0u);
}

void NT7534_SyncGeometry(void)
{
    /* 三条都是"锁存式"命令: 下发后一直有效, 除非控制器被复位或被打乱。
     * 每帧重发一遍, 把"屏只写不读"这个缺陷兜住。 */
    nt_cmd((0u != NT7534_ADC_REVERSE) ? 0xA1u : 0xA0u);
    nt_cmd((0u != NT7534_COM_REVERSE) ? 0xC8u : 0xC0u);
    nt_cmd((uint8_t) (0x40u | (NT7534_START_LINE & 0x3Fu)));
}

void NT7534_SetContrast(uint8_t v)
{
    nt_cmd(0x81u);
    nt_cmd(v);
}

void NT7534_Clear(void)
{
    uint32_t i;

    for (i = 0u; i < (uint32_t) sizeof(s_fb); i++)
    {
        s_fb[i] = 0x00u;
    }
}

void NT7534_Refresh(void)
{
    uint8_t page;

    /* 每帧重申几何/极性: 屏只写不读, 无法回读校验, 只能不断重申。
     * 这一步也是"控制器状态被弄乱后一帧自愈"的全部实现, 详见头文件说明。 */
    NT7534_SyncGeometry();

    for (page = 0u; page < NT7534_PAGES; page++)
    {
        /* 页旋转: 写到第 page 页的数据取自缓冲第 ((page + NT7534_PAGE_ROT) & 7) 页。
         * 本模块的 RAM→玻璃映射固定上移 32 行（起始行命令无效），在此补偿。见头文件。 */
        const uint8_t * p_src = &s_fb[(((uint32_t) page + NT7534_PAGE_ROT) & 7u) * NT7534_WIDTH];

        nt_set_page_col(page, 0u);
        /* 一整页 128 字节压缩在同一个 CS 低电平窗口里发完:
         * 软件 SPI 下少 127 次 CS 翻转, 硬件 SPI 下正好是一次 DMA/一次传输。 */
        NT_CS_LOW();
        NT_RS_DATA();
        nt_write_bytes(p_src, NT7534_WIDTH);
        NT_CS_HI();
    }
}

void NT7534_DrawPixel(int16_t x, int16_t y, uint8_t on)
{
    uint16_t idx;
    uint8_t  bit;

    if ((x < 0) || (y < 0) || (x >= (int16_t) NT7534_WIDTH) || (y >= (int16_t) NT7534_HEIGHT))
    {
        return;
    }

    idx = (uint16_t) (((uint16_t) (y / 8)) * NT7534_WIDTH + (uint16_t) x);
    bit = (uint8_t) (y % 8);

    if (0u != on)
    {
        s_fb[idx] |= (uint8_t) (1U << bit);
    }
    else
    {
        s_fb[idx] &= (uint8_t) ~(1U << bit);
    }
}

void NT7534_DrawDegC(int16_t x, int16_t y)
{
    /* 3x3 空心小圈, 画在 C 的左上角（列 0..2、行 0..2）。
     * 位置不是随手选的: 字库字形只占第 3..13 行, 列 2..6（'C' 顶行是 `..#####.`），
     * 所以字符单元左上角的空白区正好是"行 0..2 × 列 0..2", 圈放这里才是 C 的左上角。
     * （v4 曾放在列 4..6 = 右边, 目视偏右, 已按实拍改到左边。） */
    NT7534_DrawPixel(x,                    y,                    1u);
    NT7534_DrawPixel((int16_t) (x + 1),    y,                    1u);
    NT7534_DrawPixel((int16_t) (x + 2),    y,                    1u);
    NT7534_DrawPixel(x,                    (int16_t) (y + 1),    1u);
    NT7534_DrawPixel((int16_t) (x + 2),    (int16_t) (y + 1),    1u);
    NT7534_DrawPixel(x,                    (int16_t) (y + 2),    1u);
    NT7534_DrawPixel((int16_t) (x + 1),    (int16_t) (y + 2),    1u);
    NT7534_DrawPixel((int16_t) (x + 2),    (int16_t) (y + 2),    1u);

    NT7534_DrawChar(x, y, 'C');
}

void NT7534_DrawChar(int16_t x, int16_t y, char c)
{
    uint8_t        cx;
    uint8_t        half;
    uint8_t        bit;
    uint8_t        bits;
    const uint8_t *p_glyph;

    /* 伪字符 ℃: 字库里没有, 交给合成函数, 宽度仍是 1 个字符单元 */
    if (NT7534_CH_DEGC == c)
    {
        NT7534_DrawDegC(x, y);
        return;
    }

    if (((uint8_t) c < OLED_FONT_FIRST) || ((uint8_t) c > OLED_FONT_LAST))
    {
        c = '?';
    }

    p_glyph = &g_oled_font[(uint8_t) c - OLED_FONT_FIRST][0];

    /* 字库布局: 前 8 字节 = 上半(行 0..7), 后 8 字节 = 下半(行 8..15), 每字节 bit0 在最上 */
    for (cx = 0u; cx < OLED_FONT_W; cx++)
    {
        for (half = 0u; half < 2u; half++)
        {
            bits = p_glyph[cx + (half * 8u)];
            for (bit = 0u; bit < 8u; bit++)
            {
                if (0u != (bits & (uint8_t) (1U << bit)))
                {
                    NT7534_DrawPixel((int16_t) (x + cx), (int16_t) (y + (half * 8) + bit), 1u);
                }
            }
        }
    }
}

void NT7534_DrawString(int16_t x, int16_t y, const char * p_str)
{
    int16_t  cursor = x;
    uint32_t i;

    if (NULL == p_str)
    {
        return;
    }

    for (i = 0u; p_str[i] != '\0'; i++)
    {
        if ((cursor > -(int16_t) OLED_FONT_W) && (cursor < (int16_t) NT7534_WIDTH))
        {
            NT7534_DrawChar(cursor, y, p_str[i]);
        }

        cursor = (int16_t) (cursor + (int16_t) OLED_FONT_W);

        if (cursor >= (int16_t) NT7534_WIDTH)
        {
            break;
        }
    }
}

void NT7534_DrawRect(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    int16_t t;
    int16_t i;

    if (x0 > x1) { t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { t = y0; y0 = y1; y1 = t; }

    for (i = x0; i <= x1; i++)
    {
        NT7534_DrawPixel(i, y0, 1u);
        NT7534_DrawPixel(i, y1, 1u);
    }
    for (i = y0; i <= y1; i++)
    {
        NT7534_DrawPixel(x0, i, 1u);
        NT7534_DrawPixel(x1, i, 1u);
    }
}

void NT7534_FillRect(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t on)
{
    int16_t t;
    int16_t x;
    int16_t y;

    if (x0 > x1) { t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { t = y0; y0 = y1; y1 = t; }

    for (y = y0; y <= y1; y++)
    {
        for (x = x0; x <= x1; x++)
        {
            NT7534_DrawPixel(x, y, on);
        }
    }
}

#if (NT7534_BUS_MODE == 2)
/* ============================================================
 * S3 异步分页刷新状态机（双缓冲版 —— 消除数值更新时的撕裂/闪烁）
 *
 * 旧版缺陷（ tearing / 闪烁根因）:
 *   泵异步分页推进（主循环每 2ms 推一页，整屏约 16ms 才画完），
 *   而渲染任务在 DMA 还在飞的时候就在改写 live 缓冲 g_u8g2_fb：
 *     - 风扇动画每 16ms 重写第 4~7 页
 *     - 整屏每 1s 走 u8g2_ClearBuffer 后全量重画
 *   旧版 nt_pump_start_page() 直接 DMA 读 g_u8g2_fb，于是某一页发到一半时
 *   缓冲已被下一帧改写 -> 同一屏里部分旧、部分新 -> 看到"数值一变就跳动/闪烁"。
 *
 * 修法：加一层影子缓冲 g_nt_shadow。
 *   - 只有"泵空闲"或"一页刚发完的安全边界"才把 live 缓冲按页拷进 shadow
 *     （nt_pump_commit），DMA 永远只读 shadow，永远看不到改写中的 live 缓冲。
 *   - 渲染（draw_layout_a / draw_fan_cells）只写 live 缓冲，与 DMA 彻底解耦。
 *   - 泵忙时新请求按页位图 s_pending 排队，当前 pass 发完再补发，不丢帧、不跳过。
 *
 * 为什么 CS 必须自己管(不能交给 u8g2 的 cad):
 *   NT7534 每页 = 3 字节页地址命令(CS 低/RS 命令) + 128 字节数据(CS 低/RS 数据)，
 *   两者必须在同一个 CS 低电平窗口内；u8g2 的 cad 发完一页会拉高 CS，
 *   若数据改成异步，CS 会在 DMA 没发完就被拉高 => 花屏。
 *   所以 S3 完全绕过 u8g2_SendBuffer，直接读 shadow 自己分页发。
 *
 * 页号映射：写到 controller 第 page 页的数据取自缓冲第 (page + NT7534_PAGE_ROT) & 7 页
 *   （本模块 RAM→玻璃固定上移 32 行，起始行命令无效，在提交侧补偿；见 bsp_nt7534.h）。
 * ============================================================ */

extern uint8_t g_u8g2_fb[1024];     /* u8g2 帧缓冲, u8g2_port.c 里定义 */

/** 影子缓冲：DMA 唯一读取来源。只在泵空闲/安全边界被写入，发送期间恒定稳定。 */
static uint8_t        g_nt_shadow[NT7534_PAGES * NT7534_WIDTH];
static volatile uint32_t s_committed = 0u;   /* 已拷进 shadow、待发的页位图（bit p = 页 p） */
static volatile uint32_t s_pending   = 0u;   /* 已请求、还没拷进 shadow 的页位图 */
static volatile int      s_cur       = -1;   /* 当前正在发的页；-1 = 空闲 */

/** 把 live 缓冲的指定页（按旋转映射）拷进 shadow。仅在泵空闲/安全边界调用。 */
static void nt_pump_commit(uint32_t mask)
{
    uint8_t p;

    for (p = 0u; p < NT7534_PAGES; p++)
    {
        if (0u != (mask & (1u << p)))
        {
            const uint8_t * src = &g_u8g2_fb[(((uint32_t) p + NT7534_PAGE_ROT) & 7u) * NT7534_WIDTH];
            uint8_t       * dst = &g_nt_shadow[(uint32_t) p * NT7534_WIDTH];

            (void) memcpy(dst, src, NT7534_WIDTH);
        }
    }
}

/** 发一页的 3 字节页地址命令(阻塞, ~0.4µs @7.5MHz); CS 拉低、RS 切命令。 */
static void nt_pump_send_page_cmd(uint8_t page)
{
    uint8_t c   = (uint8_t) (0u + s_col_offset);   /* 列偏移(厂家 demo 用 +1) */
    uint8_t cmd[3];

    cmd[0] = (uint8_t) (0xB0u | (page & 0x0Fu));
    cmd[1] = (uint8_t) (0x10u | ((c >> 4) & 0x0Fu));
    cmd[2] = (uint8_t) (c & 0x0Fu);

    NT_CS_LOW();
    NT_RS_CMD();
    nt_write_bytes(cmd, 3u);     /* 阻塞, 走 NT_Bus_Write */
    NT_RS_DATA();                /* 切数据, CS 保持低, 等 DMA 把 128 字节发完 */
}

/** 从 shadow 发起一页的异步 DMA 发送; 成功返回 true。 */
static bool nt_pump_send_page(uint8_t page)
{
    const uint8_t * src = &g_nt_shadow[(uint32_t) page * NT7534_WIDTH];

    nt_pump_send_page_cmd(page);

    if (NT_Bus_WriteAsync(src, NT7534_WIDTH))
    {
        g_nt_dma_kicks++;
        return true;
    }

    /* 异步起点失败(总线仍忙, 理论上不会发生): 放弃本次, 拉高 CS 收尾 */
    NT_CS_HI();
    return false;
}

/** 从 s_committed 里挑最低页发出; 没有可发的页则置空闲。 */
static void nt_pump_advance(void)
{
    uint8_t p;
    int     next = -1;

    for (p = 0u; p < NT7534_PAGES; p++)
    {
        if (0u != (s_committed & (1u << p)))
        {
            next = (int) p;
            break;
        }
    }

    if (next < 0)
    {
        s_cur = -1;
        return;
    }

    s_cur = next;
    if (!nt_pump_send_page((uint8_t) next))
    {
        /* 异步起点失败(总线仍忙): 清空本轮, 下一轮 Poll 会按 s_pending 补发 */
        s_cur       = -1;
        s_committed = 0u;
    }
}

void NT7534_Pump_Start(uint8_t first_page, uint8_t last_page)
{
    uint8_t  p;
    uint32_t mask = 0u;

    for (p = first_page; p <= last_page; p++)
    {
        mask |= (1u << p);
    }

    NT7534_Pump_StartBitmap(mask);
}

void NT7534_Pump_StartBitmap(uint32_t mask)
{
    if (0u == mask)
    {
        return;
    }

    if (s_cur >= 0)
    {
        /* 泵忙: 排队。本轮发完会在 Poll 里补发, 不丢帧、不跳过 */
        s_pending |= mask;
        return;
    }

    /* 空闲: 立即提交 live→shadow 并启动 */
    s_committed = mask;
    s_pending   = 0u;
    nt_pump_commit(s_committed);
    nt_pump_advance();
}

void NT7534_Pump_Poll(void)
{
    if (s_cur < 0)
    {
        /* 空闲: 有排队请求就补发 */
        if (s_pending)
        {
            s_committed = s_pending;
            s_pending   = 0u;
            nt_pump_commit(s_committed);
            nt_pump_advance();
        }
        return;
    }

    if (!NT_Bus_IsIdle())
    {
        return;     /* 当前页 DMA 还在飞, 让出 CPU, 下一轮再来看 */
    }

    /* 当前页发完: 拉高 CS, 从已提交位图摘除, 推进下一页 */
    NT_CS_HI();
    s_committed &= ~(1u << (uint8_t) s_cur);

    if (s_committed)
    {
        nt_pump_advance();
    }
    else if (s_pending)
    {
        s_committed = s_pending;
        s_pending   = 0u;
        nt_pump_commit(s_committed);
        nt_pump_advance();
    }
    /* 否则彻底空闲, 等下一次 Pump_Start */
}

bool NT7534_Pump_Active(void)
{
    return (s_cur >= 0);
}
#endif /* NT7534_BUS_MODE == 2 */
