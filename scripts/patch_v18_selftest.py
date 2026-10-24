# -*- coding: utf-8 -*-
"""v18：把屏幕自检做成宏开关 NT7534_SELFTEST（默认 0 = 关闭），并重建自检函数。

一次性脚本。改完可删。
"""
import io

C_FILE = "src/nt7534/nt7534_test.c"
H_FILE = "src/nt7534/nt7534_test.h"

BLOCK = r'''/* ============================================================
 * 屏幕自检（v18 起改为**宏开关**：NT7534_SELFTEST，默认 0 = 关闭）
 *
 * 为什么默认关：自检光等待就 13.7 s（2500+1200+3000+3000+4000），
 * 开机时它跑在前面 ⇒ 拔插/复位后主机要等十几秒才看得到 USB 设备。
 * 3.5" 屏的显示部分已验收完毕（版面 A + linked/unlink 稳定），
 * 所以自检改成"排查时才打开"的开关。
 *
 * 打开方式：`nt7534_test.h` 里把 `NT7534_SELFTEST` 置 1。
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

'''

s = io.open(C_FILE, encoding="utf-8").read()

anchor = "void NT7534_TestRun(void)"
assert anchor in s
s = s.replace(anchor, BLOCK + anchor, 1)

# boot 段：自检开启时由 nt_selftest_run 内部完成 PinProbe + Init
old_boot = """    /* 3.5" 屏初始化 */
    RTT_Printf("nt7534 init: start_line=0x%02X col_offset=%u contrast=0x%02X\\n",
               (unsigned) (0x40u | NT7534_START_LINE), (unsigned) NT7534_COL_OFFSET,
               (unsigned) NT7534_CONTRAST);
    (void) NT7534_Init();
    RTT_Printf("nt7534 init done, bytes_sent=%lu\\n", (unsigned long) g_nt_bytes_sent);
    RTT_Printf("layout A live, col_offset=%u page_rot=%u\\n",
               (unsigned) NT7534_COL_OFFSET, (unsigned) NT7534_PAGE_ROT);"""

new_boot = """#if (0 != NT7534_SELFTEST)
    /* 自检模式：探线 + Init + 5 段画面（返回后进主循环画版面 A） */
    nt_selftest_run();
#else
    /* 3.5" 屏初始化（正常路径） */
    RTT_Printf("nt7534 init: start_line=0x%02X col_offset=%u contrast=0x%02X\\n",
               (unsigned) (0x40u | NT7534_START_LINE), (unsigned) NT7534_COL_OFFSET,
               (unsigned) NT7534_CONTRAST);
    (void) NT7534_Init();
    RTT_Printf("nt7534 init done, bytes_sent=%lu\\n", (unsigned long) g_nt_bytes_sent);
#endif
    RTT_Printf("layout A live, col_offset=%u page_rot=%u\\n",
               (unsigned) NT7534_COL_OFFSET, (unsigned) NT7534_PAGE_ROT);"""

assert old_boot in s
s = s.replace(old_boot, new_boot, 1)
io.open(C_FILE, "w", encoding="utf-8", newline="\n").write(s)

# 头文件加宏
h = io.open(H_FILE, encoding="utf-8").read()
macro = '''

/** 3.5" 屏开机自检开关。
 *
 *  `0`（默认）= 开机直接进版面 A 主循环 —— USB 约 1~2 s 内可枚举。
 *  `1`         = 开机先跑 5 段接线自检（全亮/全灭/条纹/棋盘/边框），约 14 s。
 *
 *  什么时候打开：换屏、改接线、怀疑列偏移/起始行不对时。
 *  ⚠ 打开后开机变慢（自检期间会持续排空 USB 事件，所以枚举本身不受影响）。
 *  ⚠ 自检代码在 `#if (0 != NT7534_SELFTEST)` 里，**关闭时不参与编译**；
 *     如果你动了 `NT7534_*` 或 `OLED_*` 的接口，记得把开关打开编一次再关回去。
 */
#define NT7534_SELFTEST   (0)
'''
h = h.rstrip("\n") + macro
io.open(H_FILE, "w", encoding="utf-8", newline="\n").write(h)
print("已完成：自检块插入 + 宏定义")
