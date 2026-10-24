/* ============================================================
 * 1.3 寸单色 OLED (128x64) 驱动实现 —— I2C, SH1106
 *
 * 初始化参数与刷新时序逐条对齐立创官方移植示例
 *   1-3-single-oled-screen/src/BSP_Code/OLED/oled.c
 * (官方示例是 SPI 版, 本工程走 I2C, 只是字节传输通道不同,
 *  命令序列 / 页寻址方式 / 显示参数完全相同。)
 *
 * 显存组织: 帧缓冲按「页优先」线性排布 —— 第 0 页 128 字节, 第 1 页 128 字节 ...,
 * 每个字节 = 一列上的竖直 8 像素(bit0 在上)。SH1106 是页寻址,
 * 每页单独设页号 + 列地址后再连续写 128 字节, 与这个排布一一对应。
 * ============================================================ */

#include "oled/bsp_oled.h"
#include "oled/oled_font.h"
#include "i2c/bsp_i2c.h"

#include <string.h>                    /* memcmp / memcpy —— 脏页比对 */

/* ---- 运行时状态 ---- */
volatile uint8_t  g_oled_addr     = 0U;
volatile uint8_t  g_oled_init_err = 0U;
volatile uint8_t  g_oled_xfer_err = 0U;
volatile uint32_t g_oled_frames   = 0U;
volatile uint32_t g_oled_pages_flushed = 0U;

/* ---- 内部缓冲 ---- */
static uint8_t s_fb[OLED_PAGES * OLED_WIDTH];      /* 1024 字节帧缓冲 */
static uint8_t s_shadow[OLED_PAGES * OLED_WIDTH];  /* 1024 字节"已上屏内容"副本, 供脏页比对 */
static uint8_t s_tx[OLED_WIDTH + 1U];              /* 控制字节 + 一页 128 字节 */

/** 影子副本是否可信。false = 上电后第一帧, 强制全刷一遍建立基线。
 *  (.bss 初值就是 false, 不依赖显式初始化) */
static bool s_shadow_valid;

/** I2C 控制字节: 0x00 = 后续为命令; 0x40 = 后续为显存数据 */
#define OLED_CTRL_CMD   (0x00U)
#define OLED_CTRL_DATA  (0x40U)

/* 列地址命令: 0x0X 给低 4 位, 0x1X 给高 4 位(官方用 0x02 + 0x10 -> 列 2) */
#define OLED_CMD_COL_LOW(offset)   ((uint8_t) (0x00U | ((offset) & 0x0FU)))
#define OLED_CMD_COL_HIGH(offset)  ((uint8_t) (0x10U | (((offset) >> 4) & 0x0FU)))

/* ------------------------------------------------------------
 * SH1106 初始化序列 —— 顺序与参数逐条对应官方 oled.c 的 OLED_Init()
 *
 * 与官方的差异只有一处: 官方把列地址命令(0x02/0x10)放在序列里,
 * 这里挪到 OLED_Refresh() 每页都设, 序列里不重复发(效果相同, 且偏移可由
 * OLED_COL_OFFSET 宏统一控制)。
 *
 * 0xAF(开显示)不在序列里 —— 清屏上屏之后再单独发, 避免上电闪一下乱数据。
 * ============================================================ */
static const uint8_t s_init_seq[] =
{
    0xAE,             /* 关显示                       官方: display off */
    0x40,             /* 显示起始行 = 0               官方: set display start line */
    0xB0,             /* 页地址 = 0                   官方: set page address */
    0x81, 0xCF,       /* 对比度                       官方: 0xcf */
    0xA1,             /* 段重映射(左右)               官方: set segment remap */
    0xA6,             /* 正常显示(不反色)             官方: normal / reverse */
    0xA8, 0x3F,       /* 多路复用比 1/64              官方: duty = 1/64 */
    0xAD, 0x8B,       /* 电荷泵使能 + 开(内供 VCC)    官方: 0xad 0x8b —— SH1106 专用 */
    0x33,             /* VPP = 9V                     官方: 0X30---0X33 set VPP 9V */
    0xC8,             /* COM 扫描方向(上下)           官方: Com scan direction */
    0xD3, 0x00,       /* 显示偏移 = 0                 官方: set display offset */
    0xD5, 0x80,       /* 时钟分频 / 振荡频率          官方: set osc division */
    0xD9, 0x1F,       /* 预充电周期                   官方: 0x1f */
    0xDA, 0x12,       /* COM 引脚配置                 官方: set COM pins */
    0xDB, 0x40,       /* VCOMH 去选择电平             官方: set vcomh 0x40 */
    0xA4,             /* 显示内容跟随显存(不全亮) */
};

/* ------------------------------------------------------------
 * I2C 底层收发
 * ------------------------------------------------------------ */
/** 发 1..7 条连续命令(共用同一个 0x00 控制字节) */
static fsp_err_t oled_cmd_args (const uint8_t * p_args, uint32_t len)
{
    uint32_t i;
    uint8_t  buf[8];
    fsp_err_t err;

    if ((len == 0U) || (len > (sizeof(buf) - 1U)))
    {
        return FSP_ERR_INVALID_SIZE;
    }

    buf[0] = OLED_CTRL_CMD;
    for (i = 0U; i < len; i++)
    {
        buf[i + 1U] = p_args[i];
    }

    err = I2C_Write((uint8_t) g_oled_addr, buf, len + 1U);
    if (FSP_SUCCESS != err)
    {
        g_oled_xfer_err = (uint8_t) err;
    }

    return err;
}

/** 发整条初始化序列(共用同一个 0x00 控制字节) */
static fsp_err_t oled_cmd_seq (const uint8_t * p_seq, uint32_t len)
{
    uint32_t i;
    fsp_err_t err;

    if (g_oled_addr == 0U)
    {
        return FSP_ERR_NOT_OPEN;
    }

    if (len > (sizeof(s_tx) - 1U))
    {
        return FSP_ERR_INVALID_SIZE;
    }

    s_tx[0] = OLED_CTRL_CMD;
    for (i = 0U; i < len; i++)
    {
        s_tx[i + 1U] = p_seq[i];
    }

    err = I2C_Write((uint8_t) g_oled_addr, s_tx, len + 1U);
    if (FSP_SUCCESS != err)
    {
        g_oled_xfer_err = (uint8_t) err;
    }

    return err;
}

/* ------------------------------------------------------------
 * 初始化
 * ------------------------------------------------------------ */
fsp_err_t OLED_Init (void)
{
    fsp_err_t    err;
    const uint8_t display_on[] = {0xAFU};

    err = I2C_Bus_Init();
    if (FSP_SUCCESS != err)
    {
        g_oled_init_err = (uint8_t) err;
        return err;
    }

    /* 确认器件在线。探测只发「控制字节 0x00(命令模式)+ 停止位」,
     * 后面不跟任何命令, 对器件是空操作, 不会改写寄存器。 */
    if (!I2C_Probe(OLED_ADDR))
    {
        g_oled_addr     = 0U;
        g_oled_init_err = (uint8_t) FSP_ERR_NOT_FOUND;
        return FSP_ERR_NOT_FOUND;
    }

    g_oled_addr = OLED_ADDR;

    err = oled_cmd_seq(s_init_seq, sizeof(s_init_seq));
    if (FSP_SUCCESS != err)
    {
        g_oled_init_err = (uint8_t) err;
        return err;
    }

    /* 先清屏上屏, 再开显示 —— 与官方示例顺序一致(官方 OLED_Clear 内部带 Refresh,
     * 最后才发 0xAF), 这样上电不会先闪一下显存里的随机内容。 */
    OLED_Clear();
    OLED_Refresh();

    err = oled_cmd_args(display_on, sizeof(display_on));
    if (FSP_SUCCESS != err)
    {
        g_oled_init_err = (uint8_t) err;
        return err;
    }

    R_BSP_SoftwareDelay(100, BSP_DELAY_UNITS_MILLISECONDS);

    return FSP_SUCCESS;
}

/* ------------------------------------------------------------
 * 帧缓冲操作
 * ------------------------------------------------------------ */
void OLED_Clear (void)
{
    uint16_t i;

    for (i = 0U; i < (uint16_t) (OLED_PAGES * OLED_WIDTH); i++)
    {
        s_fb[i] = 0x00U;
    }
}

void OLED_Refresh (void)
{
    uint8_t   page;
    uint16_t  i;
    uint16_t  base;
    uint8_t   addr_cmd[3];
    fsp_err_t err;

    /* 列地址只需在命令里设一次值, 每页都要重发(SH1106 每换页都要重新设列) */
    addr_cmd[1] = OLED_CMD_COL_LOW (OLED_COL_OFFSET);
    addr_cmd[2] = OLED_CMD_COL_HIGH(OLED_COL_OFFSET);

    for (page = 0U; page < (uint8_t) OLED_PAGES; page++)
    {
        base = (uint16_t) ((uint16_t) page * OLED_WIDTH);

        /* 本页内容与已上屏的一致 -> 整页跳过, 连命令都不发 */
        if (s_shadow_valid && (0 == memcmp(&s_fb[base], &s_shadow[base], OLED_WIDTH)))
        {
            continue;
        }

        addr_cmd[0] = (uint8_t) (0xB0U + page);       /* 页地址 */
        (void) oled_cmd_args(addr_cmd, 3U);

        s_tx[0] = OLED_CTRL_DATA;
        for (i = 0U; i < (uint16_t) OLED_WIDTH; i++)
        {
            s_tx[i + 1U] = s_fb[base + i];
        }

        err = I2C_Write((uint8_t) g_oled_addr, s_tx, (uint32_t) (OLED_WIDTH + 1U));
        if (FSP_SUCCESS != err)
        {
            /* 失败时刻意不更新影子副本 —— 下一帧比对仍不相等, 会自动重刷这一页 */
            g_oled_xfer_err = (uint8_t) err;
        }
        else
        {
            memcpy(&s_shadow[base], &s_fb[base], OLED_WIDTH);
        }

        g_oled_pages_flushed++;
    }

    s_shadow_valid = true;

    g_oled_frames++;
}

void OLED_DrawPixel (int16_t x, int16_t y, uint8_t on)
{
    uint16_t idx;
    uint8_t  bit;

    if ((x < 0) || (y < 0) || (x >= (int16_t) OLED_WIDTH) || (y >= (int16_t) OLED_HEIGHT))
    {
        return;
    }

    idx = (uint16_t) (((uint16_t) (y / 8)) * OLED_WIDTH + (uint16_t) x);
    bit = (uint8_t)  (y % 8);

    if (on)
    {
        s_fb[idx] |= (uint8_t) (1U << bit);
    }
    else
    {
        s_fb[idx] &= (uint8_t) ~(1U << bit);
    }
}

void OLED_DrawChar (int16_t x, int16_t y, char c)
{
    uint8_t        cx;
    uint8_t        half;
    uint8_t        bit;
    uint8_t        bits;
    const uint8_t *p_glyph;

    if (((uint8_t) c < OLED_FONT_FIRST) || ((uint8_t) c > OLED_FONT_LAST))
    {
        c = '?';
    }

    p_glyph = &g_oled_font[(uint8_t) c - OLED_FONT_FIRST][0];

    /* 与官方 OLED_ShowChar(size1=16) 的解析一致:
     * 前 8 字节 = 上半(行 0..7), 后 8 字节 = 下半(行 8..15); 每字节 bit0 在最上 */
    for (cx = 0U; cx < OLED_FONT_W; cx++)
    {
        for (half = 0U; half < 2U; half++)
        {
            bits = p_glyph[cx + (half * 8U)];

            for (bit = 0U; bit < 8U; bit++)
            {
                if (0U != (bits & (uint8_t) (1U << bit)))
                {
                    OLED_DrawPixel((int16_t) (x + cx), (int16_t) (y + (half * 8) + bit), 1U);
                }
            }
        }
    }
}

void OLED_DrawString (int16_t x, int16_t y, const char * p_str)
{
    int16_t  cursor = x;
    uint32_t i;

    if (NULL == p_str)
    {
        return;
    }

    for (i = 0U; p_str[i] != '\0'; i++)
    {
        /* 字符完全在屏外就跳过, 省掉逐点裁剪 */
        if ((cursor > -(int16_t) OLED_FONT_W) && (cursor < (int16_t) OLED_WIDTH))
        {
            OLED_DrawChar(cursor, y, p_str[i]);
        }

        cursor = (int16_t) (cursor + (int16_t) OLED_FONT_W);

        if (cursor >= (int16_t) OLED_WIDTH)
        {
            break;                      /* 后面都已出屏 */
        }
    }
}

void OLED_DrawRect (int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    int16_t t;
    int16_t i;

    if (x0 > x1) { t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { t = y0; y0 = y1; y1 = t; }

    for (i = x0; i <= x1; i++)
    {
        OLED_DrawPixel(i, y0, 1U);
        OLED_DrawPixel(i, y1, 1U);
    }

    for (i = y0; i <= y1; i++)
    {
        OLED_DrawPixel(x0, i, 1U);
        OLED_DrawPixel(x1, i, 1U);
    }
}

void OLED_DrawBitmap (int16_t x0, int16_t y0, const uint8_t * p_bmp,
                      uint16_t w, uint16_t h, uint16_t stride, uint8_t invert)
{
    uint16_t row;
    uint16_t col;

    if ((NULL == p_bmp) || (0U == w) || (0U == h) || (0U == stride))
    {
        return;
    }

    for (row = 0U; row < h; row++)
    {
        int16_t         py   = (int16_t) ((int32_t) y0 + (int32_t) row);
        const uint8_t * p_row;
        uint16_t        base;
        uint8_t         mask;

        /* 行整行在屏外就跳过(比逐像素判断便宜得多) */
        if ((py < 0) || (py >= (int16_t) OLED_HEIGHT))
        {
            continue;
        }

        /* 同一行的页基址与位掩码是常量, 提到列循环外 */
        base = (uint16_t) (((uint16_t) (py / 8)) * OLED_WIDTH);
        mask = (uint8_t)   (1U << (py % 8));
        p_row = &p_bmp[((uint32_t) row) * stride];

        for (col = 0U; col < w; col++)
        {
            int16_t px = (int16_t) ((int32_t) x0 + (int32_t) col);
            uint8_t on;

            if ((px < 0) || (px >= (int16_t) OLED_WIDTH))
            {
                continue;
            }

            /* bit7 = 该行最左像素 */
            on = (uint8_t) ((p_row[col >> 3U] >> (7U - (col & 7U))) & 1U);
            if (0U != invert)
            {
                on = (uint8_t) (on ^ 1U);
            }

            if (0U != on)
            {
                s_fb[base + (uint16_t) px] |=  mask;
            }
            else
            {
                s_fb[base + (uint16_t) px] &= (uint8_t) ~mask;
            }
        }
    }
}
