/* ============================================================
 * 3.5" 屏接线自检 + 方案A 演示实现
 * 说明见 nt7534.h
 * ============================================================ */

#include "nt7534/nt7534.h"
#include "tsn/bsp_tsn.h"             /* RA6E2 片上温度传感器（1.3" OLED 显示结温） */

#include <stdio.h>

#include "hal_data.h"                  /* g_ioport_ctrl, BSP_IO_PORT_* */
#include "cgc/osc_probe.h"             /* 20MHz 晶振起振探测(USB 时钟可行性) */
#include "nt7534/bsp_nt7534.h"
#include "nt7534/u8g2_port.h"     /* u8g2 移植层: 真 ° 字形 + 线段/矩形/圆 */
#include "nt7534/fan_frames.h"    /* 3 叶 20x20 风扇位图帧(24 帧, scripts/gen_fan_frames.py 生成) */
#include "oled/bsp_oled.h"             /* 1.3" OLED 兼作"第二指示灯" */
#include "rtt/rtt_log.h"
#include "uart/bsp_uart_link.h"        /* 上位机数据（SCI0 / USB CDC 共用解析） */
#include "usb/bsp_usb_cdc.h"          /* Type-C 虚拟串口（P814/P815 原生 USB） */

/* 板载 LED: P402, 低电平点亮 */
#define LED_PIN   ((bsp_io_port_pin_t) BSP_IO_PORT_04_PIN_02)

/* 1 = STAGE6 走"列偏移 0..4 全扫"自检；0 = 直接用 NT7534_COL_OFFSET 常驻方案 A。
 * 实测偏移 3 是对的（用户目视确认 4/5 那张四边齐），所以默认 0。 */
#define NT7534_OFFSET_SWEEP  (0)

/** OLED 是否可用。可用时它显示当前处于第几幕 ——
 *  3.5" 屏不亮时, 这一行字就是"MCU 在跑、代码走到哪"的唯一证据。 */
static bool s_oled_ok = false;

/** 方案 A 累计帧号。画在状态行上: 数字在跳 = 屏在实时刷新, 不是残留图像。 */
static uint16_t s_frame_no = 0u;

/** 固件版本标记, 见 nt7534.h */
volatile uint32_t g_nt_fw_tag = 0u;

/** OLED 行距。字体是 8x16（见 oled_font.h 的 OLED_FONT_W/H）⇒
 *  **每行最多 16 个字符**，屏高 64px 正好 4 行（y = 0/16/32/48）。
 *
 *  ⚠ 历史坑（2026-10-23 修）：第三行原本写死
 *      "P403/09 302/07 301"        ← 19 字符, 超出 16
 *    实际只显示到 "P403/09 302/07 3"，末尾 `301` 被截掉一半；
 *    而且 `09`/`07` 这种省略写法看不出是 P409/P207，容易被当成乱码。
 *    现在改成每行 ≤16 字符、且用 "功能名+完整引脚号" 的写法。 */
#define OLED_LINE_H   (16)

#if (0 != NT7534_SELFTEST)
/** LED 只在自检里用（阶段节拍/失败快闪）。自检关闭时不留未使用函数。 */
static void led_write(bsp_io_level_t lv)
{
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, LED_PIN, lv);
}
#endif

static void led_init(void)
{
    (void) R_IOPORT_PinCfg(&g_ioport_ctrl, LED_PIN,
                           (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT
                           | (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH       /* 高 = 灭 */
                           | (uint32_t) IOPORT_CFG_DRIVE_MID);
}

/** 等 ms 毫秒, 期间 LED 每 250 ms 翻一次 —— 屏全黑时也能确认 MCU 在跑 */
/* ---------------- 方案 A: 4 行 8x16 大字 ---------------- */

#if (0 != NT7534_OFFSET_SWEEP)
/** 边框 + 满宽 16 字符: 列偏移错时, 一边掉字、另一边空一截, 一眼能看出来 */
static void draw_offset_test(void)
{
    NT7534_Clear();
    NT7534_DrawRect(0, 0, (int16_t) (NT7534_WIDTH - 1u), (int16_t) (NT7534_HEIGHT - 1u));
    NT7534_DrawString(0, 8,  "0123456789ABCDEF");
    NT7534_DrawString(0, 32, "0123456789ABCDEF");
    NT7534_Refresh();
}
#endif

#if (0 == U8G2_PORT_ENABLE)

/** 2 位十进制（0..99）写进 p[0..1]。v < 0（本帧没有该项）时写 "--"。
 *  不用 snprintf: 这里要的是"定长 2 字符"的排版, 值超范围时宁可钳位也不让行宽变化。 */
static void fmt2(char * p, int16_t v)
{
    if (v < 0)
    {
        p[0] = '-';
        p[1] = '-';
        return;
    }
    if (v > 99)
    {
        v = 99;
    }
    p[0] = (char) ('0' + (char) (v / 10));
    p[1] = (char) ('0' + (char) (v % 10));
}

/** 4 位十进制（0..9999）写进 p[0..3]。无数据时写 "----"。 */
static void fmt4(char * p, int16_t v)
{
    if (v < 0)
    {
        p[0] = '-'; p[1] = '-'; p[2] = '-'; p[3] = '-';
        return;
    }
    if (v > 9999)
    {
        v = 9999;
    }
    p[0] = (char) ('0' + (char) (v / 1000));
    p[1] = (char) ('0' + (char) ((v / 100) % 10));
    p[2] = (char) ('0' + (char) ((v / 10) % 10));
    p[3] = (char) ('0' + (char) (v % 10));
}

/* ===== 回退路径: 手写绘图 API（bsp_nt7534.c 自带的 8x16 字库 + 合成 ℃）===== */

/** 拼"标签 + 温度℃ + 占用% + 风扇"一行（正好 16 字符 = 128 px）。
 *  例: `CPU 48℃ 12% 1050` —— ℃ 由驱动合成, 宽度仍算 1 个字符。 */
static void build_metric_line(char * line, const char * p_label, int16_t temp,
                              int16_t usage, int16_t fan)
{
    line[0] = p_label[0];
    line[1] = p_label[1];
    line[2] = p_label[2];
    line[3] = ' ';
    fmt2(&line[4], temp);
    line[6] = NT7534_CH_DEGC;
    line[7] = ' ';
    fmt2(&line[8], usage);
    line[10] = '%';
    line[11] = ' ';
    fmt4(&line[12], fan);
    line[16] = '\0';
}

/** 画方案 A 的一帧（真实数据, 来自串口链路）。
 *  行 0 CPU / 行 1 GPU / 行 2 RAM / 行 3 链路状态。每行 16 字符铺满 128 px。 */
static void draw_layout_a(void)
{
    const uart_link_data_t * d = UART_Link_Data();
    char line[17];

    NT7534_Clear();

    build_metric_line(line, "CPU", d->ct, d->cu, d->cf);
    NT7534_DrawString(0, 0, line);

    build_metric_line(line, "GPU", d->gt, d->gu, d->gf);
    NT7534_DrawString(0, 16, line);

    line[0] = 'R'; line[1] = 'A'; line[2] = 'M'; line[3] = ' ';
    fmt2(&line[4], d->mu);
    line[6] = '%';
    line[7] = '\0';
    NT7534_DrawString(0, 32, line);

    /* 状态行: 只显示链路连通状态（linked / unlink）。
     * ⚠ 固件版本号已按需求从屏上移除。 */
    (void) snprintf(line, sizeof(line), "%s", UART_Link_Online() ? "linked" : "unlink");
    NT7534_DrawString(0, 48, line);

    NT7534_Refresh();
}

#else  /* U8G2_PORT_ENABLE */

/* ===== 正路: u8g2 ===== */

/** "°C" 的 UTF-8 字节。必须拆成两个字面量: 写成 "\xB0C" 时那个 C 是十六进制数字,
 *  编译器会把它并进转义序列得到 0x0C, 不是 0xB0。 */
#define U8G2_DEG_C   "\xC2\xB0" "C"

/** 定长 2 位十进制，无数据(-1)写 "--"，超过 99 钳到 99。 */
static void num2(char * p, size_t n, int16_t v)
{
    if (v < 0)
    {
        (void) snprintf(p, n, "--");
        return;
    }
    if (v > 99)
    {
        v = 99;
    }
    (void) snprintf(p, n, "%2d", (int) v);
}

/** 定长 4 位十进制，无数据(-1)写 "----"。 */
static void num4(char * p, size_t n, int16_t v)
{
    if (v < 0)
    {
        (void) snprintf(p, n, "----");
        return;
    }
    if (v > 9999)
    {
        v = 9999;
    }
    (void) snprintf(p, n, "%4d", (int) v);
}

/** USB 中断链"三段自证"。
 *
 *  目的: 当调试器连不上时, 这块屏是唯一还能读的观测窗。USBFS 中断不进 CPU
 *  只可能是下面三处之一, 全部直接读硬件, 不做任何推断:
 *
 *    行 0  `IE <IELSR0>`   0x40006300。这个寄存器把"外设事件"接到"NVIC 槽位"。
 *                          若里面不是 USBFS 的事件号, 中断永远到不了 CPU。
 *    行 1  `NV <ISER0>`    0xE000E100。bit16 = USBFS(向量号 16)。为 0 即中断没使能。
 *    行 2  `S<INTSTS0> E<INTENB0> P<PRIMASK>`  0x40090040 / 0x40090030 + 全局中断开关。
 *                          ST 里挂着的标志位如果一直不清, 说明中断服务没跑过;
 *                          E 应为 0x9D00（BEMPE|BRDYE|VBSE|DVSE|CTRE）, 为 0 则外设侧没使能;
 *                          P=1 表示 PRIMASK 关着 —— 那无论前面几项多正确, 中断都进不来。
 *    行 3  `v10 E<事件> C<配置>`    应用回调计数; 两者都为 0 就是一次都没进。
 *
 *  只在 USB 尚未枚举时占用画面 —— 一旦链路通了, 自动换回 CPU/GPU/RAM 三行。
 */
static void draw_usb_diag(u8g2_t * g)
{
    volatile uint32_t * p_ielsr  = (volatile uint32_t *) 0x40006300UL;  /* IELSR0  */
    volatile uint32_t * p_iser0  = (volatile uint32_t *) 0xE000E100UL;  /* NVIC ISER0 */
    volatile uint16_t * p_sts    = (volatile uint16_t *) 0x40090040UL;  /* INTSTS0 */
    volatile uint16_t * p_enb    = (volatile uint16_t *) 0x40090030UL;  /* INTENB0 */
    char                line[24];

    (void) snprintf(line, sizeof(line), "IE %08lX", (unsigned long) *p_ielsr);
    u8g2_DrawUTF8(g, 0, 0, line);

    (void) snprintf(line, sizeof(line), "NV %08lX", (unsigned long) *p_iser0);
    u8g2_DrawUTF8(g, 0, 16, line);

    (void) snprintf(line, sizeof(line), "S%04X E%04X P%u V%u",
                    (unsigned) *p_sts, (unsigned) *p_enb,
                    (unsigned) (__get_PRIMASK() & 1u),
                    (unsigned) (((*p_sts) >> 7) & 1u));
    u8g2_DrawUTF8(g, 0, 32, line);

    /* ⚠ 固件版本号已按需求从屏上移除（要看版本读 g_nt_fw_tag / RTT）。 */
    (void) snprintf(line, sizeof(line), "E%lu C%lu I%u",
                    (unsigned long) g_usb_events, (unsigned long) g_usb_cfg_cnt,
                    (unsigned) (*(volatile uint32_t *) 0xE000E200UL & 1u));
    u8g2_DrawUTF8(g, 0, 48, line);
}

/* ===== 表格版布局 + 风扇（2026-10-25 定稿）=====
 *
 * 表格(128x64):  竖线 x=0/24/54/86/119, 横线 y=0/16/32/47(仅左半)/63;
 *   第 3、4 行的右侧为两个风扇格(纵向合并 y=32..62), 格顶标 CPU / GPU。
 *   所有单元格文字水平+垂直居中。
 *   linked 行(x=0..54) 不画 x=24 竖线 ⇒ 不被竖线穿过。
 *
 * 风扇: 3 叶 20x20 位图帧(24 帧 = 120° 视觉周期)。相位按 RPM 实时推进 ——
 *   角速度 = RPM/24 度/秒, 与上位机送数频率(1 Hz 也好、别的也好)**完全解耦**;
 *   数据只更新 RPM 值; 动画由主循环 2ms 切片计数自行推进(DWT 在本 M33 上不计, 不用)。
 */

/** 两台风扇的相位, 单位 1/100 度。3 叶对称周期 120° = 12000 cd = 24 帧。 */
static int32_t  s_fan_phase_cd[2] = { 0, 0 };

/** 1 ms 系统心跳（Cortex-M 内核 SysTick 中断驱动）。
 *  为什么要自己开:
 *   - DWT 的 CYCCNT 在本 M33 上**不走**(实测两次读同一个值), 原来那种算法拿不到真实时间;
 *   - FSP 裸机没有系统时基, USB/串口都不依赖 SysTick, 所以这里自建一个不会打架;
 *   - 中断优先级保持默认(最低), 不会挤掉 USB 中断。
 *  用途: 风扇转速按**真实时间**推进(与帧率无关)、主循环按 1 s 定拍、串口轮询按 50 ms。 */
volatile uint32_t g_ms_tick = 0u;

void SysTick_Handler(void)
{
    g_ms_tick++;
}

/** 按 RPM 推进两台风扇相位: 角速度 = RPM/24 (°/s) ⇒ 每 ms 增加 RPM/240 (1/100°)。 */
static void fan_advance(uint32_t dt_ms)
{
    const uart_link_data_t * d = UART_Link_Data();
    int32_t v[2];
    int     i;

    v[0] = (d->cf < 0) ? 0 : ((d->cf > 3000) ? 3000 : d->cf);
    v[1] = (d->gf < 0) ? 0 : ((d->gf > 3000) ? 3000 : d->gf);

    for (i = 0; i < 2; i++)
    {
        int32_t inc = (v[i] * (int32_t) dt_ms) / 240;
        int32_t ph  = s_fan_phase_cd[i] + inc;
        if (ph >= 12000)
        {
            ph %= 12000;
        }
        s_fan_phase_cd[i] = ph;
    }
}

/** 在 (ox,oy) 画 20x20 风扇, 取 phase 对应的位图帧。 */
static void draw_fan(u8g2_t * g, int16_t ox, int16_t oy, int32_t phase_cd)
{
    int idx = (int) ((phase_cd / 500) % FAN_FRAME_COUNT);
    u8g2_DrawXBMP(g, (u8g2_uint_t) ox, (u8g2_uint_t) oy,
                  FAN_FRAME_W, FAN_FRAME_H, g_fan_frames[idx]);
}

/** 在 [x0..x1]×[y0..y1] 单元格内水平+垂直居中绘制文本(FontPosTop)。 */
static void draw_cell(u8g2_t * g, const char * s,
                      int16_t x0, int16_t x1, int16_t y0, int16_t y1)
{
    int16_t      cw = (int16_t) (x1 - x0 + 1);
    int16_t      ch = (int16_t) (y1 - y0 + 1);
    int16_t      fh = (int16_t) (u8g2_GetAscent(g) - u8g2_GetDescent(g));
    u8g2_uint_t  w  = u8g2_GetUTF8Width(g, s);
    int16_t      x  = (int16_t) (x0 + (cw - (int16_t) w) / 2);
    int16_t      y  = (int16_t) (y0 + (ch - fh) / 2);

    if (x < x0) { x = x0; }
    if (y < y0) { y = y0; }
    u8g2_DrawUTF8(g, (u8g2_uint_t) x, (u8g2_uint_t) y, s);
}

/** 自绘 5x7 小字(仅 C/P/U/G), 用于风扇格标签——比 7x13 小, 放得下且不压线。
 *  每个字形 7 行, 每行低 5 位有效(bit4 在左)。纯置位绘制, 不清背景。 */
static const uint8_t g_glyph_C[7] = { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E };
static const uint8_t g_glyph_P[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 };
static const uint8_t g_glyph_U[7] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E };
static const uint8_t g_glyph_G[7] = { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E };

static const uint8_t * small_glyph(char ch)
{
    switch (ch)
    {
        case 'C': return g_glyph_C;
        case 'P': return g_glyph_P;
        case 'U': return g_glyph_U;
        case 'G': return g_glyph_G;
        default:  return NULL;
    }
}

/** 用 5x7 小字在 [x0..x1] 内水平居中, 从 ytop 起画一行标签(CPU / GPU)。 */
static void draw_small_label(u8g2_t * g, const char * s,
                             int16_t x0, int16_t x1, int16_t ytop)
{
    int16_t n = 0;
    int16_t total;
    int16_t x;
    int16_t i;
    int     row;
    int     col;

    while (s[n] != '\0') { n++; }
    total = (int16_t) (n * 6 - 1);
    x     = (int16_t) (x0 + ((x1 - x0 + 1) - total) / 2);

    for (i = 0; i < n; i++)
    {
        const uint8_t * gl = small_glyph(s[i]);
        if (gl != NULL)
        {
            for (row = 0; row < 7; row++)
            {
                for (col = 0; col < 5; col++)
                {
                    if (0u != (gl[row] & (uint8_t) (0x10u >> col)))
                    {
                        u8g2_DrawPixel(g, (u8g2_uint_t) (x + col),
                                       (u8g2_uint_t) (ytop + row));
                    }
                }
            }
        }
        x = (int16_t) (x + 6);
    }
}

/** 只重画右下角两个风扇格(不动表格线与文字), 供动画帧调用。
 *  为什么需要它: 之前动画帧直接调 draw_layout_a() —— 那是**整屏重画 + 清缓冲**,
 *  单次约 16 ms; 1 s 的等待里要跑 125 帧 ⇒ 主循环被拖到 ~2.5 s,
 *  连带把"数字 1 Hz 更新"压成 2.5 s 一次。现在只清两个格子内部再画,
 *  单次约 1.5 ms, 主循环回到 ~1 s, 数字真正 1 Hz 刷新。
 *  ⚠ 清的范围特意避开 y=32 / y=63 横线和 x=54 / x=86 / x=127 竖线。 */
static void draw_fan_cells(void)
{
    u8g2_t * g = U8G2_Port();

    u8g2_SetDrawColor(g, 0);                /* 画刷=0 ⇒ 清像素 */
    u8g2_DrawBox(g, 55, 33, 31, 30);        /* CPU 格内部 x55..85, y33..62 */
    u8g2_DrawBox(g, 87, 33, 40, 30);        /* GPU 格内部 x87..126, y33..62 */
    u8g2_SetDrawColor(g, 1);                /* 恢复画刷=1 ⇒ 置像素 */

    draw_small_label(g, "CPU", 55, 85, 34);
    draw_fan        (g, 60, 43, s_fan_phase_cd[0]);
    draw_small_label(g, "GPU", 87, 126, 34);
    draw_fan        (g, 97, 43, s_fan_phase_cd[1]);
}

/** 画一帧表格版界面(真实数据, 来自串口/USB CDC 链路)。 */
static void draw_layout_a(void)
{
    static u8g2_t         * g = NULL;
    const uart_link_data_t * d = UART_Link_Data();
    char                    s[16];
    char                    t[8];
    char                    u[8];
    char                    r[8];

    if (NULL == g)
    {
        g = U8G2_Port_Init();
        u8g2_SetFont(g, u8g2_font_7x13_tf);
        u8g2_SetFontPosTop(g);      /* y 变成"顶部"而不是基线 */
        u8g2_SetFontMode(g, 1);     /* 透明: 只置位、不清背景(否则会擦断表格线/风扇) */
    }

    u8g2_ClearBuffer(g);

    /* 一条数据通路都没通的时候, 把画面让给 USB 中断三段自证(见 draw_usb_diag)。 */
    if (!UART_Link_Online() && !USB_CDC_Configured())
    {
        draw_usb_diag(g);
        U8G2_Port_Flush();
        return;
    }

    /* --- 表格线 ---
     * 右边界 127(原 119): 屏右侧 x120..127 的 8px 空白全部用掉, 表格贴到最右一列。 */
    u8g2_DrawHLine(g, 0, 0, 128);
    u8g2_DrawHLine(g, 0, 16, 128);
    u8g2_DrawHLine(g, 0, 32, 128);
    u8g2_DrawHLine(g, 0, 47, 55);       /* 仅左半: 右侧风扇格纵向合并 */
    u8g2_DrawHLine(g, 0, 63, 128);
    u8g2_DrawVLine(g, 0, 0, 64);
    u8g2_DrawVLine(g, 24, 0, 47);       /* linked 行特意不画, 免得穿过文字 */
    u8g2_DrawVLine(g, 54, 0, 64);
    u8g2_DrawVLine(g, 86, 0, 64);
    u8g2_DrawVLine(g, 127, 0, 64);

    /* --- CPU 行 --- */
    draw_cell(g, "CPU", 1, 23, 1, 15);
    num2(t, sizeof(t), d->ct);
    if (d->ct < 0) { (void) snprintf(s, sizeof(s), "--"); }
    else           { (void) snprintf(s, sizeof(s), "%s" U8G2_DEG_C, t); }
    draw_cell(g, s, 25, 53, 1, 15);
    num2(u, sizeof(u), d->cu);
    (void) snprintf(s, sizeof(s), "%s%%", u);
    draw_cell(g, s, 55, 85, 1, 15);
    num4(r, sizeof(r), d->cf);
    draw_cell(g, r, 87, 126, 1, 15);

    /* --- GPU 行 --- */
    draw_cell(g, "GPU", 1, 23, 17, 31);
    num2(t, sizeof(t), d->gt);
    if (d->gt < 0) { (void) snprintf(s, sizeof(s), "--"); }
    else           { (void) snprintf(s, sizeof(s), "%s" U8G2_DEG_C, t); }
    draw_cell(g, s, 25, 53, 17, 31);
    num2(u, sizeof(u), d->gu);
    (void) snprintf(s, sizeof(s), "%s%%", u);
    draw_cell(g, s, 55, 85, 17, 31);
    num4(r, sizeof(r), d->gf);
    draw_cell(g, r, 87, 126, 17, 31);

    /* --- RAM 行 --- */
    draw_cell(g, "RAM", 1, 23, 33, 46);
    num2(u, sizeof(u), d->mu);
    (void) snprintf(s, sizeof(s), "%s%%", u);
    draw_cell(g, s, 25, 53, 33, 46);

    /* --- 状态行(独占 x0..54, 不被竖线穿过) --- */
    draw_cell(g, UART_Link_Online() ? "linked" : "unlink", 1, 53, 48, 62);

    /* --- 右下角风扇格: 顶部 5x7 小字标签(y34, 离横线2px/离风扇2px) + 20x20 风扇 --- */
    draw_small_label(g, "CPU", 55, 85, 34);
    draw_fan        (g, 60, 43, s_fan_phase_cd[0]);
    draw_small_label(g, "GPU", 87, 126, 34);
    draw_fan        (g, 97, 43, s_fan_phase_cd[1]);

    /* 数据/状态帧: 全屏刷新(1 Hz 一次, 1024 B ≈ 8 ms, 无感)。
     * 风扇动画帧不走这里, 见 draw_fan_cells() + U8G2_Port_FlushArea()。 */
    U8G2_Port_Flush();
}

#endif /* U8G2_PORT_ENABLE */

/* ---------------- 主流程 ---------------- */

/* ============================================================
 * 屏幕自检（v18 起改为**宏开关**：NT7534_SELFTEST，默认 0 = 关闭）
 *
 * 为什么默认关：自检光等待就 13.7 s（2500+1200+3000+3000+4000），
 * 开机时它跑在前面 ⇒ 拔插/复位后主机要等十几秒才看得到 USB 设备。
 * 3.5" 屏的显示部分已验收完毕（版面 A + linked/unlink 稳定），
 * 所以自检改成"排查时才打开"的开关。
 *
 * 打开方式：`nt7534.h` 里把 `NT7534_SELFTEST` 置 1。
 * 打开后开机流程 = 探线 → Init → 5 段画面（每段停 2~4 s）→ 进主循环。
 * ============================================================ */

#if (0 != NT7534_SELFTEST)

/** OLED 两行状态（第 1 行 = 阶段编号, 第 2 行 = 说明）。OLED 不可用就静默跳过。 */
static void oled_status(const char * l1, const char * l2)
{
    if (false == s_oled_ok)
    {
        return;
    }
    OLED_Clear();
    OLED_DrawString(0, 0, l1);
    OLED_DrawString(0, (int16_t) OLED_LINE_H, l2);
    OLED_Refresh();
}

/** 接线参考页：5 根信号线全列，每行 ≤16 字符（OLED 是 8x16 字库，16 字符/行）。
 *  停 2 s 后会被 STG1 覆盖，所以必须停留。 */
static void oled_wiring_page(void)
{
    if (false == s_oled_ok)
    {
        return;
    }
    OLED_Clear();
    OLED_DrawString(0, 0,                              "NT7534 WIRING");
    OLED_DrawString(0, (int16_t) OLED_LINE_H,          "RS403 RES409");
    OLED_DrawString(0, (int16_t) (OLED_LINE_H * 2u),   "CLK302 SID207");
    OLED_DrawString(0, (int16_t) (OLED_LINE_H * 3u),   "CS301");
    OLED_Refresh();
}

/** 接线不对时的红牌：LED 150 ms 快闪死循环（此时不驱动任何信号线）。 */
static void fail_blink_forever(void)
{
    while (1)
    {
        led_write(BSP_IO_LEVEL_LOW);
        R_BSP_SoftwareDelay(150u, BSP_DELAY_UNITS_MILLISECONDS);
        led_write(BSP_IO_LEVEL_HIGH);
        R_BSP_SoftwareDelay(150u, BSP_DELAY_UNITS_MILLISECONDS);
    }
}

/** 自检等待：250 ms 一片，每 2 ms 排空一次 USB 事件。
 *
 *  自检期间 USB 已经枚举完了（USB 初始化在自检之前），若不排空事件，
 *  FSP 的裸机事件缓冲只有 8 深 ⇒ 溢出丢事件 ⇒ 主机可能卡在 Address 态。 */
static void stage_wait(uint32_t ms)
{
    uint32_t elapsed = 0u;

    while (elapsed < ms)
    {
        uint32_t k;

        led_write((0u == ((elapsed / 250u) & 1u)) ? BSP_IO_LEVEL_LOW : BSP_IO_LEVEL_HIGH);

        for (k = 0u; k < 125u; k++)          /* 125 × 2 ms = 250 ms */
        {
            R_BSP_SoftwareDelay(2u, BSP_DELAY_UNITS_MILLISECONDS);
            USB_CDC_Poll();
        }
        elapsed += 250u;
    }
}

/** 4px 竖条纹：点画法，任何列偏移都能看出来（厂家 demo 用 grid，同构）。 */
static void draw_v_stripes(void)
{
    int16_t x;
    int16_t y;

    NT7534_Clear();
    for (x = 0; x < (int16_t) NT7534_WIDTH; x++)
    {
        if (0 == ((x / 4) & 1))
        {
            for (y = 0; y < (int16_t) NT7534_HEIGHT; y++)
            {
                NT7534_DrawPixel(x, y, 1u);
            }
        }
    }
    NT7534_Refresh();
}

/** 1px 棋盘格：错行错列，糊成一片就说明页/列映射不对。 */
static void draw_checker(void)
{
    int16_t x;
    int16_t y;

    NT7534_Clear();
    for (y = 0; y < (int16_t) NT7534_HEIGHT; y++)
    {
        for (x = 0; x < (int16_t) NT7534_WIDTH; x++)
        {
            if (0 != ((x + y) & 1))
            {
                NT7534_DrawPixel(x, y, 1u);
            }
        }
    }
    NT7534_Refresh();
}

/** 边框 + 文字：边框贴不贴四边 = 列偏移 / 起始行对不对。 */
static void draw_frame_stage(void)
{
    NT7534_Clear();
    NT7534_DrawRect(0, 0, (int16_t) (NT7534_WIDTH - 1u), (int16_t) (NT7534_HEIGHT - 1u));
    NT7534_DrawString(4, 24, "EDGE OK?");
    NT7534_Refresh();
}

/** 自检主流程：探线 → Init → 5 段画面。返回后由主循环画版面 A。
 *
 *  ⚠ 引脚自检必须在 `NT7534_Init()` **之前**：Init 会把那 5 根脚配成推挽输出，
 *    之后再读电平就没有意义了（读回来全是自己驱动的值）。 */
static void nt_selftest_run(void)
{
    RTT_WriteString("SELFTEST: NT7534_SELFTEST=1\n");

    oled_wiring_page();
    R_BSP_SoftwareDelay(2000u, BSP_DELAY_UNITS_MILLISECONDS);

    /* ---- 0) 引脚自检（纯输入 + 上拉, 不驱动）---- */
    NT7534_PinProbe();
    RTT_Printf("pinprobe: RS(P403)=%u RES(P409)=%u CLK(P302)=%u SID(P207)=%u CS(P301)=%u  bad=%u\n",
               (unsigned) g_nt_probe_levels[0], (unsigned) g_nt_probe_levels[1],
               (unsigned) g_nt_probe_levels[2], (unsigned) g_nt_probe_levels[3],
               (unsigned) g_nt_probe_levels[4], (unsigned) g_nt_probe_bad);

    if (0u != g_nt_probe_bad)
    {
        oled_status("PIN PROBE FAIL", "check wiring!");
        RTT_WriteString("PROBE FAIL: 有信号线被外部拉低(接错到 GND / 与其他信号短路)\n"
                        "  -> 不驱动任何引脚, LED 150ms 快闪。请断电检查接线。\n");
        fail_blink_forever();
    }

    /* ---- 1) 初始化 ---- */
    RTT_Printf("init: start_line=0x%02X col_offset=%u contrast=0x%02X\n",
               (unsigned) (0x40u | NT7534_START_LINE), (unsigned) NT7534_COL_OFFSET,
               (unsigned) NT7534_CONTRAST);
    (void) NT7534_Init();
    RTT_Printf("init done, bytes_sent=%lu\n", (unsigned long) g_nt_bytes_sent);

    /* ---- STAGE 1: 全点亮 ---- */
    RTT_WriteString("STAGE 1/5: all points ON (expect: whole panel lit)\n");
    oled_status("STG1", "STG1 ALL ON");
    NT7534_AllPointsOn(1u);
    stage_wait(2500u);

    /* ---- STAGE 2: 全灭 ---- */
    RTT_WriteString("STAGE 2/5: all points OFF + clear RAM\n");
    oled_status("STG2", "STG2 ALL OFF");
    NT7534_AllPointsOn(0u);
    NT7534_Clear();
    NT7534_Refresh();
    stage_wait(1200u);

    /* ---- STAGE 3: 竖条纹 ---- */
    RTT_WriteString("STAGE 3/5: vertical stripes 4px\n");
    oled_status("STG3", "STG3 STRIPES");
    draw_v_stripes();
    stage_wait(3000u);

    /* ---- STAGE 4: 棋盘格 ---- */
    RTT_WriteString("STAGE 4/5: 1px checkerboard\n");
    oled_status("STG4", "STG4 CHECKER");
    draw_checker();
    stage_wait(3000u);

    /* ---- STAGE 5: 边框 + 文字 ---- */
    RTT_WriteString("STAGE 5/5: frame + text (check border touches all 4 edges)\n");
    oled_status("STG5", "STG5 FRAME");
    draw_frame_stage();
    stage_wait(4000u);

    oled_status("LIVE", "layout A");
    RTT_WriteString("SELFTEST done -> layout A\n");
}

#endif /* (0 != NT7534_SELFTEST) */

void NT7534_Run(void)
{
    led_init();

    /* 第一件事: 打固件标记。放在最前面, 保证只要 MCU 进了本函数就能被探针读到,
     * 与后面屏上发生什么无关（失败路径也算"跑到了"）。 */
    g_nt_fw_tag = NT7534_FW_TAG;

    RTT_Printf("\n=== 3.5\" NT7534 VTM88723V01 wiring test (fw v%u) ===\n", NT7534_FW_VER);

    /* 晶振探测: 必须放在任何引脚被配成输出之前 —— 这时 P212/P213(EXTAL/XTAL) 空着,
     * 测到的是"晶振本身能不能起振", 不受屏的 RS/RES 驱动影响。
     * 结论决定 USB CDC 走不走得通: 有 20MHz 晶振 ⇒ PLL2 240MHz /5 = 48MHz 可行。 */
    OSC_Probe_Run();
    {
        const osc_probe_result_t * osc = OSC_Probe_Result();
        RTT_Printf("osc probe: moscsf=%u pll2sf=%u loops=%lu (max %lu)\n",
                   (unsigned) osc->moscsf, (unsigned) osc->pll2sf,
                   (unsigned long) osc->wait_loops, (unsigned long) 500000UL);
    }

    /* ⭐ v18：**USB / 串口链路最先初始化，屏幕自检已删除**。
     *
     * 历史：v17 之前 `USB_CDC_Init()` 排在 6 段屏幕自检之后，而自检光等待就有
     * 13.7 s（2500+1200+3000+3000+4000）⇒ 复位/拔插后主机要等十几秒才看到设备。
     * 3.5" 屏的显示部分已经验收完毕（版面 A + linked/unlink 都稳定），
     * 那些"全亮/全灭/条纹/棋盘/边框"自检画面已经没有价值 ⇒ 整体删除。
     *
     * 现在开机路径：初始化 USB → 初始化屏 → 直接进版面 A 主循环。
     *
     * ⚠ 必须放在 `OSC_Probe_Run()` 之后：晶振探测要求 P212/P213 无任何负载。
     */
    (void) UART_Link_Init();

    /* Type-C 虚拟串口: 板上没有 UART 桥芯片, Type-C 直连 P814/P815 原生 USB，
     * 所以数据通路必须由固件实现 CDC。失败也不影响屏幕（会一直显示 unlink）。 */
    (void) USB_CDC_Init();

    /* 1.3" OLED 只当"第二指示灯"：3.5" 屏黑屏时，它上面有字就说明 MCU 起来了。 */
    s_oled_ok = (FSP_SUCCESS == OLED_Init());

    /* 1.3" OLED 还要显示 RA6E2 芯片结温，先开 TSN（失败不影响 3.5" 屏，仅 RTT 告警）。 */
    {
        fsp_err_t terr = TSN_Init();
        RTT_Printf("TSN_Init -> err=%d, cal=%u, slope=%d uV/C\n",
                   (int) terr, (unsigned) g_tsn_calib, (int) g_tsn_slope);
    }

#if (0 != NT7534_SELFTEST)
    /* 自检模式：探线 + Init + 5 段画面（返回后进主循环画版面 A） */
    nt_selftest_run();
#else
    /* 3.5" 屏初始化（正常路径） */
    RTT_Printf("nt7534 init: start_line=0x%02X col_offset=%u contrast=0x%02X\n",
               (unsigned) (0x40u | NT7534_START_LINE), (unsigned) NT7534_COL_OFFSET,
               (unsigned) NT7534_CONTRAST);
    (void) NT7534_Init();
    RTT_Printf("nt7534 init done, bytes_sent=%lu\n", (unsigned long) g_nt_bytes_sent);
#endif
    RTT_Printf("layout A live, col_offset=%u page_rot=%u\n",
               (unsigned) NT7534_COL_OFFSET, (unsigned) NT7534_PAGE_ROT);

    /* 1 ms 心跳(内核 SysTick): 风扇转速/主循环定拍/串口轮询都按真实时间走。 */
    (void) SysTick_Config(SystemCoreClock / 1000u);

    while (1)
    {
        uint32_t t_loop;
        uint32_t t_uart;
        uint32_t t_anim;

        UART_Link_Poll();                /* 先解析已到的字节, 再画 */
#if (0 != UART_LINK_BEACON)
        UART_Link_Beacon();              /* TX 心跳: 排查"RX 一个字节都收不到"时用 */
#endif
        s_frame_no++;
        USB_CDC_ProbeUpdate();
        draw_layout_a();

        /* 1.3" OLED：显示 RA6E2 芯片结温（与 3.5" 屏上位机数据并行）。
         *   每轮（约 1 s）清屏重画一次，整屏 1 Hz 刷新。 */
        {
            int32_t t = 0;
            if (FSP_SUCCESS == TSN_ReadTemperature(&t))
            {
                char tl[24];
                (void) snprintf(tl, sizeof(tl), "T=%ld.%ld C",
                                (long) (t / 10),
                                (long) ((t < 0) ? (-(t % 10)) : (t % 10)));
                OLED_Clear();
                OLED_DrawString(0, 0, "RA6E2 CHIP T");
                OLED_DrawString(0, 16, tl);
                OLED_DrawString(0, 32, "TSN->ADC12(-3)");
                OLED_Refresh();
            }
        }

        if (0u == (uint32_t) (s_frame_no % 15u))
        {
            RTT_Printf("frames=%lu rx=%lu xor_err=%lu online=%u\n",
                       (unsigned long) g_link_frames, (unsigned long) g_link_rx_bytes,
                       (unsigned long) g_link_err_xor, (unsigned) UART_Link_Online());
        }

        /* 1 s 的等待拆成 2 ms 细片, 但**按 SysTick 真实时间定拍**:
         *   - 每片都 USB_CDC_Poll() 排空 USB 事件 —— FSP 的裸机事件缓冲只有 8 深,
         *     原来 1 s 才转一圈, 枚举突发事件必然溢出丢事件(表现为有时卡在 Address 态);
         *   - 每 50 ms 收一次串口字节 + 累计接收超时;
         *   - 每 1000 ms 结束本轮 ⇒ 数据/状态稳定 1 Hz。原来按片计数(k<500 × 2ms),
         *     但 R_BSP_SoftwareDelay 实测比标称长, 实际一轮 ~1.7 s, 数字就掉到 ~0.6 Hz。 */
        t_loop = g_ms_tick;
        t_uart = t_loop;
        t_anim = t_loop;

        for (;;)
        {
            uint32_t now;

            R_BSP_SoftwareDelay(2u, BSP_DELAY_UNITS_MILLISECONDS);
            USB_CDC_Poll();

            now = g_ms_tick;
            if ((uint32_t) (now - t_uart) >= 50u)
            {
                t_uart = now;
                UART_Link_Poll();
                UART_Link_Tick(50u);
            }
#if (0 != U8G2_PORT_ENABLE)
            /* 风扇动画: 按**真实时间**推进, 与上位机送数频率、与帧率都无关。
             *   - 每 16 ms 一帧(≈60 fps): 只重画右下两格 + 脏刷该区(约 1.5 ms);
             *   - 相位 = 真实 dt × 1.5(即"角速度 = RPM/24 度/秒 的 1.5 倍");
             *   - 门控: cf/gf 均为 -1(无数据)时静止, 但风扇照常显示。 */
            if ((uint32_t) (now - t_anim) >= 16u)
            {
                uint32_t dt = (uint32_t) (now - t_anim);
                const uart_link_data_t * pd = UART_Link_Data();

                t_anim = now;
                if (dt > 100u) { dt = 100u; }        /* 防卡顿后相位大跳 */

                if ((pd->cf >= 0) || (pd->gf >= 0))
                {
                    fan_advance(dt + (dt >> 1));     /* 1.5 倍速 */
                    draw_fan_cells();
                    /* 脏区: x54..127 / y32..63 ⇒ tile tx=6(48), tw=10(48..127) */
                    U8G2_Port_FlushArea(6u, 4u, 10u, 4u);
                }
            }
#endif
            if ((uint32_t) (now - t_loop) >= 1000u)
            {
                break;                               /* 本轮 1 s 到 */
            }
        }
    }
}
