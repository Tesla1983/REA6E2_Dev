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
        (void) R_IOPORT_PinCfg(&g_ioport_ctrl, s_nt_pins[i],
                               (uint32_t) IOPORT_CFG_PORT_DIRECTION_INPUT
                               | (uint32_t) IOPORT_CFG_PULLUP_ENABLE);
    }
}

static void nt_pins_as_output(void)
{
    /* 空闲电平: CS/RS/RES/SID 高, CLK 低。
     * 驱动强度取 MID: 就算某根线被接错到 GND, 电流也只有几 mA, 不会伤 MCU 或屏。 */
    (void) R_IOPORT_PinCfg(&g_ioport_ctrl, NT7534_PIN_CLK,
                           (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT
                           | (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW
                           | (uint32_t) IOPORT_CFG_DRIVE_MID);
    (void) R_IOPORT_PinCfg(&g_ioport_ctrl, NT7534_PIN_SID,
                           (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT
                           | (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH
                           | (uint32_t) IOPORT_CFG_DRIVE_MID);
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

/** 半位延时: 用 NOP 循环, 不用 R_BSP_SoftwareDelay。
 *  实测 R_BSP_SoftwareDelay(1, MICROSECONDS) 单次开销远大于 1 µs（整屏一帧要 ~3 s,
 *  折合 300~400 µs/字节）, 完全是它的函数调用与校准开销, 这里改成 ~16 次空转,
 *  半位约 300~400 ns → 约 1.2~1.6 Mbit/s, 对 NT7534 这类控制器仍很保守。 */
static void nt_half_delay(void)
{
    volatile uint32_t i;

    for (i = 0u; i < 16u; i++)
    {
        __asm__ volatile ("nop");
    }
}

/** 发一个字节（MSB first, CLK 空闲低, 上升沿采样）。 */
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

    g_nt_bytes_sent++;
}

static void nt_cmd(uint8_t c)
{
    NT_CS_LOW();
    NT_RS_CMD();
    nt_write_byte(c);
    NT_CS_HI();
}

static void nt_data(uint8_t d)
{
    NT_CS_LOW();
    NT_RS_DATA();
    nt_write_byte(d);
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
    nt_write_byte(b);
}

fsp_err_t NT7534_Init(void)
{
    uint8_t page;
    uint8_t col;

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
        for (col = 0u; col < NT7534_WIDTH; col++)
        {
            nt_data(0x00u);
        }
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
    uint8_t  page;
    uint8_t  col;

    /* 每帧重申几何/极性: 屏只写不读, 无法回读校验, 只能不断重申。
     * 这一步也是"控制器状态被弄乱后一帧自愈"的全部实现, 详见头文件说明。 */
    NT7534_SyncGeometry();

    for (page = 0u; page < NT7534_PAGES; page++)
    {
        /* 页旋转: 写到第 page 页的数据取自缓冲第 ((page + NT7534_PAGE_ROT) & 7) 页。
         * 本模块的 RAM→玻璃映射固定上移 32 行（起始行命令无效），在此补偿。见头文件。 */
        const uint8_t * p_src = &s_fb[(((uint32_t) page + NT7534_PAGE_ROT) & 7u) * NT7534_WIDTH];

        nt_set_page_col(page, 0u);
        NT_RS_DATA();
        for (col = 0u; col < NT7534_WIDTH; col++)
        {
            nt_data(p_src[col]);
        }
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
