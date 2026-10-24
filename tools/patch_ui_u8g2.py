#!/usr/bin/env python3
"""把方案 A 的界面从"手写绘图 API"切到 u8g2。

只改 src/nt7534/nt7534.c 里两段:
  - build_metric_line + draw_layout_a  ->  用 U8G2_PORT_ENABLE 分成新旧两套
  - 固件版本号 v5 -> v6
旧的 fmt2/fmt4 保留（新路径也用，作为定长数字格式化）。
"""
import io
import sys

P = 'src/nt7534/nt7534.c'
s = io.open(P, encoding='utf-8').read()

START = "/** 拼\"标签 + 温度℃ + 占用% + 风扇\"一行"
END = "/* ---------------- 主流程 ---------------- */"

i = s.index(START)
j = s.index(END)
old = s[i:j]

# 旧版本体原样保留在 #if (0 == U8G2_PORT_ENABLE) 分支里
old_indented = '\n'.join(('    ' + ln) if ln.strip() else ln for ln in old.rstrip().split('\n'))

NEW = '''#if (0 == U8G2_PORT_ENABLE)

/* ===== 回退路径: 手写绘图 API（bsp_nt7534.c 自带的 8x16 字库 + 合成 ℃）===== */

''' + old.rstrip() + '''

#else  /* U8G2_PORT_ENABLE */

/* ===== 正路: u8g2 ===== */

/** "°C" 的 UTF-8 字节。必须拆成两个字面量: 写成 "\\xB0C" 时那个 C 是十六进制数字,
 *  编译器会把它并进转义序列得到 0x0C, 不是 0xB0。 */
#define U8G2_DEG_C   "\\xC2\\xB0" "C"

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

/** 画一行 `CPU 48°C 12% 1050`。
 *  ° 是 u8g2 字体里真实存在的字形(U+00B0), 不再是拼出来的小圈,
 *  所以它相对 C 的位置由字体决定, 不会"偏左偏右"。 */
static void draw_metric_row(u8g2_t * g, int16_t y, const char * p_label,
                            int16_t temp, int16_t usage, int16_t fan)
{
    char ts[8];
    char us[8];
    char fs[8];
    char line[32];

    num2(ts, sizeof(ts), temp);
    num2(us, sizeof(us), usage);
    num4(fs, sizeof(fs), fan);

    (void) snprintf(line, sizeof(line), "%s %s" U8G2_DEG_C " %s%% %s",
                    p_label, ts, us, fs);
    u8g2_DrawUTF8(g, 0, y, line);
}

/** 画方案 A 的一帧（真实数据, 来自串口链路）。
 *  行 0 CPU / 行 1 GPU / 行 2 RAM / 行 3 链路状态, 行距 16 px。 */
static void draw_layout_a(void)
{
    static u8g2_t         * g = NULL;
    const uart_link_data_t * d = UART_Link_Data();
    char                    line[24];
    char                    ms[8];

    if (NULL == g)
    {
        /* 第一次调用时才初始化: 让屏幕上电到出第一帧之间不留空白 */
        g = U8G2_Port_Init();
        u8g2_SetFont(g, u8g2_font_7x13_tf);
        u8g2_SetFontPosTop(g);      /* DrawStr 的 y 变成"顶部"而不是基线 */
    }

    u8g2_ClearBuffer(g);

    draw_metric_row(g, 0,  "CPU", d->ct, d->cu, d->cf);
    draw_metric_row(g, 16, "GPU", d->gt, d->gu, d->gf);

    num2(ms, sizeof(ms), d->mu);
    (void) snprintf(line, sizeof(line), "RAM %s%%", ms);
    u8g2_DrawUTF8(g, 0, 32, line);

    /* 状态行: 固件版本 + 链路状态 + 已收到的帧数。
     * "v6" = 用 u8g2 绘图的版本（对应 g_nt_fw_tag）; 帧数在涨 ⇒ 画面在刷新。 */
    if (UART_Link_Online())
    {
        num4(ms, sizeof(ms), (int16_t) (g_link_frames % 10000u));
        (void) snprintf(line, sizeof(line), "v6 LK %s", ms);
    }
    else
    {
        (void) snprintf(line, sizeof(line), "v6 NO LINK");
    }
    u8g2_DrawUTF8(g, 0, 48, line);

    U8G2_Port_Flush();
}

#endif /* U8G2_PORT_ENABLE */

'''

s = s[:i] + NEW + s[j:]

# 固件版本 v5 -> v6
s = s.replace("line[1] = '5';", "line[1] = '6';")
s = s.replace('wiring test (fw v5)', 'wiring test (fw v6)')

# 头文件包含
if '#include "nt7534/u8g2_port.h"' not in s:
    s = s.replace('#include "oled/bsp_oled.h"',
                  '#include "nt7534/u8g2_port.h"     /* u8g2 移植层: 真 ° 字形 + 线段/矩形/圆 */\n'
                  '#include "oled/bsp_oled.h"', 1)

io.open(P, 'w', encoding='utf-8', newline='\n').write(s)
print('patched, size =', len(s.encode('utf-8')))
