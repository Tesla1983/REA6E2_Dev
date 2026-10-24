/********************************************************************
 * RA6E2 + 1.3 寸 I2C OLED(SH1106, 0x3C) —— helloworld 滚动显示
 *
 * 上电流程:
 *   1) I2C + OLED 初始化
 *   2) 信息屏 5 秒: 画满屏边框 + 三行参数
 *      —— 边框用于目视确认列偏移 OLED_COL_OFFSET 是否正确:
 *         SH1106 内部是 132 列 RAM, 可见的 128 列对应 RAM 列 2..129。
 *         四条边都紧贴屏幕边缘 = 偏移正确;
 *         左边框不在最左(左边空 2 像素)且右边框不见 = 偏移应为 0, 改宏重烧。
 *   3) 循环滚动 "helloworld"
 *
 * 板载 LED(P402, 低电平点亮)作心跳:
 *   慢闪(约 0.5s) = OLED 正常;  快闪(150ms) = 没找到 OLED
 *********************************************************************/
#include "Apply/app.h"

#include "hal_data.h"                  /* I2C_BH1750_cfg.rate —— banner 里打印真实波特率 */

#include "i2c/bsp_i2c.h"
#include "oled/bsp_oled.h"
#include "tsn/bsp_tsn.h"
#include "rtt/rtt_log.h"
#include "rtt/itm_log.h"
#include "Ascii/xjp.h"
#include "nt7534/nt7534.h"

/* ★★★ 启动目标选择 ★★★
 *   1 = 3.5" NT7534 屏（接线自检 6 幕 + 方案A 演示）
 *   0 = 1.3" OLED（信息屏/位图屏 + TSN 温度屏）
 * 两套代码互不影响, 改这一个宏即可来回切。 */
#define APP_USE_NT7534_3IN5    (1)

/** 滚动文本 */
#define MARQUEE_TEXT     "helloworld"
/** 两轮之间的间隔像素, 避免首尾相接 */
#define MARQUEE_GAP      (24)
/** 每帧左移像素数。
 *  帧时间随优化档位变化: 改造前 108 ms/帧(约 9 fps), 400 kHz + 脏页后约 5 ms/帧(约 195 fps)。
 *  滚动速度是"像素/秒", 会随帧率一起变快 —— 想固定视觉速度, 调下面的
 *  MARQUEE_FRAME_DELAY_MS(真正的调速旋钮), 这里最小只能到 1, 光靠它降不了多少。 */
#define MARQUEE_STEP     (2)

/** ★★★ 想让 helloworld 滚慢一点, 改这一个数 ★★★
 *
 *  每多少帧才把文字左移一次。1 = 每帧都动(最快, 出厂默认)。
 *
 *  【为什么不直接加延时降帧率】"屏幕刷新率"和"文字移动速度"是两件事。
 *    加延时是把**整帧**拖慢, 连带 LED 心跳、RTT 打印节奏全被一起拉慢, 属于误伤;
 *    这里只把"移动"这个动作分频, 刷新率保持约 195 fps 不变, 只让字走得慢。
 *
 *  速度怎么算:  像素/秒 = MARQUEE_STEP × 帧率 / 本值
 *               一圈    = (文本宽 80 + MARQUEE_GAP 24) / 像素每秒
 *
 *  实测基线帧率约 195 fps, MARQUEE_STEP = 2:
 *
 *      本值     速度        一圈      观感
 *      ----  ----------  -------   -------------------------------
 *        1    390 px/s    0.27 s   ← 现在, 快得看不清
 *        4     98 px/s    1.1 s
 *        8     49 px/s    2.1 s    ← 推荐, 读得舒服
 *       16     24 px/s    4.3 s    ← 明显慢
 *
 *  ⚠ 单色屏的最小单位就是 1 个像素, 所以"慢"必然表现为"隔几帧才跳 1 像素",
 *    这不是抖动也不是 bug —— 想再平滑只能降帧率(不推荐)或换更高分辨率的屏。
 *
 *  ✅ 附带好处: 内容没变化的帧会被脏页 diff 整页跳过, 这个值调大反而更省 I2C 带宽。 */
#define MARQUEE_MOVE_EVERY_N   (16)
/** 文本垂直位置: (64 - 16) / 2 = 24, 居中 */
#define MARQUEE_Y        (24)
/** 信息屏停留时间(ms)。
 *  原来是 5000: 复位后要 5s + 3s = 8s 才有动静, 屏幕上一直是静态画面,
 *  看着就像"按了 RST 卡死了"。缩短到 2s, 保留可读性又不至于误判。 */
#define SPLASH_MS        (2000)
/** 位图屏停留时间(ms)。同上, 原 3000 -> 2000。 */
#define BITMAP_MS        (2000)

void Run (void)
{
    fsp_err_t err;

#if APP_USE_NT7534_3IN5
    /* 3.5" 屏: 先自检接线, 再依次放 6 个画面, 最后停在方案A。内部死循环, 不返回。 */
    NT7534_Run();
#endif

    ITM_Init();                       /* 打通 ITM -> Funnel -> TMC/ETF */

    RTT_WriteString("\n=== RA6E2 OLED demo (RTT log) ===\n");
    ITM_PutString("=== RA6E2 OLED demo (ITM channel 0) ===\n");
    /* 波特率从 I2C_BH1750_cfg.rate 现读, 不写死字符串 ——
       改了 configuration.xml 的速率后这行不会再说谎(曾经写死 100kHz, 实际已跑到 400kHz) */
    RTT_Printf("FSP 6.6.0 | R7FA6E2BB | I2C P408(SCL)/P206(SDA) @%u kHz\n",
               (unsigned) (I2C_BH1750_cfg.rate / 1000U));
    RTT_Printf("OLED init ...\n");

    err = OLED_Init();
    RTT_Printf("OLED_Init -> err=%d, addr=0x%02X, init_err=%u\n",
               (int) err, (unsigned) g_oled_addr, (unsigned) g_oled_init_err);

    if (FSP_SUCCESS != err)
    {
        /* OLED 不可用: 快闪 LED 报错, 不做任何显示操作 */
        RTT_WriteString("OLED FAILED - LED fast blink, halt here\n");
        while (1)
        {
            R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_04_PIN_02, BSP_IO_LEVEL_LOW);
            R_BSP_SoftwareDelay(150, BSP_DELAY_UNITS_MILLISECONDS);

            R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_04_PIN_02, BSP_IO_LEVEL_HIGH);
            R_BSP_SoftwareDelay(150, BSP_DELAY_UNITS_MILLISECONDS);
        }
    }

    /* ---------------- 信息屏 ---------------- */
    OLED_Clear();
    OLED_DrawRect(0, 0, (int16_t) (OLED_WIDTH - 1U), (int16_t) (OLED_HEIGHT - 1U));
    /* 信息屏(上电后显示 2 秒):
       ADDR 0x3C   = OLED 的 I2C 总线地址(和屏通信用的编号)
       IC SH1106    = 屏幕驱动芯片型号
       COL+2 128x64 = 分辨率 128×64 像素, 列偏移 +2(SH1106 内部 132 列 RAM, 可见 128 列从列 2 起) */
    OLED_DrawString(6,  8, "ADDR 0x3C");
    OLED_DrawString(6, 24, "IC SH1106");
    OLED_DrawString(6, 40, "COL+2 128x64");
    OLED_Refresh();

    R_BSP_SoftwareDelay(SPLASH_MS, BSP_DELAY_UNITS_MILLISECONDS);

    /* ---------------- 位图屏 ---------------- */
    {
        /* 50x52 居中: x = (128-50)/2 = 39, y = (64-52)/2 = 6 */
        int16_t bx = (int16_t) ((OLED_WIDTH  - XJP_BMP_W) / 2U);
        int16_t by = (int16_t) ((OLED_HEIGHT - XJP_BMP_H) / 2U);

        OLED_Clear();
        OLED_DrawRect(0, 0, (int16_t) (OLED_WIDTH - 1U), (int16_t) (OLED_HEIGHT - 1U));
        OLED_DrawBitmap(bx, by, &g_xjp_bmp[0][0],
                        XJP_BMP_W, XJP_BMP_H, XJP_BMP_STRIDE,
                        (uint8_t) XJP_BMP_INVERT);
        OLED_Refresh();

        RTT_Printf("bitmap %ux%u @(%d,%d) invert=%u\n",
                   (unsigned) XJP_BMP_W, (unsigned) XJP_BMP_H,
                   (int) bx, (int) by, (unsigned) XJP_BMP_INVERT);

        R_BSP_SoftwareDelay(BITMAP_MS, BSP_DELAY_UNITS_MILLISECONDS);
    }

    /* ---------------- 芯片温度传感器显示 ---------------- */
    {
        int32_t  temp_x10 = 0;
        uint32_t tsn_tick = 0U;
        /* 容量按 %ld 的最坏宽度算: 20(含负号) + 1('.' ) + 20 + 2(" C") + 1('\0') = 44,
           取 48 消除 -Wformat-truncation 告警。 */
        char     line[48];

        RTT_WriteString("TSN: reading on-chip temperature sensor ...\n");
        err = TSN_Init();
        RTT_Printf("TSN_Init -> err=%d, cal=%u, slope=%d uV/C\n",
                   (int) err, (unsigned) g_tsn_calib, (int) g_tsn_slope);

        if (FSP_SUCCESS != err)
        {
            /* TSN 不可用: 快闪 LED 报错, 不做显示 */
            RTT_WriteString("TSN init FAILED - LED fast blink, halt\n");
            while (1)
            {
                R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_04_PIN_02, BSP_IO_LEVEL_LOW);
                R_BSP_SoftwareDelay(150, BSP_DELAY_UNITS_MILLISECONDS);
                R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_04_PIN_02, BSP_IO_LEVEL_HIGH);
                R_BSP_SoftwareDelay(150, BSP_DELAY_UNITS_MILLISECONDS);
            }
        }

        while (1)
        {
            fsp_err_t rerr = TSN_ReadTemperature(&temp_x10);

            OLED_Clear();
            if (FSP_SUCCESS == rerr)
            {
                int32_t whole = temp_x10 / 10;
                int32_t frac  = temp_x10 % 10;
                if (frac < 0)
                {
                    frac = -frac;   /* 温度不会为负, 但保险起见 */
                }

                /* 整数格式化, 不依赖 newlib-nano 的 %f。
                   注: OLED 字库(xjp.h 8x16)只有英文字符, 屏上无法显示中文,
                       故这三行保持英文; 各行含义见下方逐行注释。 */
                snprintf(line, sizeof(line), "T=%ld.%ld C", (long) whole, (long) frac);
                /* 第 1 行 标题: RA6E2=芯片型号 | JUNCTION=结(芯片内部硅核温度, 即"结温") | T=Temperature
                   整句意为: "RA6E2 芯片结温" */
                OLED_DrawString(0,  0, "RA6E2 JUNCTION T");
                /* 第 2 行 数值: T=温度 | 38.6=摄氏数值(会变化) | C=摄氏度
                   例: "T=38.6 C" 即 "当前结温 38.6 度" */
                OLED_DrawString(8, 24, line);
                /* 第 3 行 来源: TSN=片上温度传感器 | ->ADC12=送进内部 12 位模数转换器 | (-3)=该传感器占用的专用 ADC 通道号
                   整句意为: "温度来自: 片上传感器经 12 位 ADC 第 -3 号专用通道" */
                OLED_DrawString(0, 48, "TSN->ADC12(-3)");
                OLED_Refresh();

                RTT_Printf("temp=%ld.%ld C  raw=%u  cal=%u\n",
                           (long) whole, (long) frac,
                           (unsigned) g_tsn_raw, (unsigned) g_tsn_calib);
            }
            else
            {
                OLED_DrawString(0, 24, "TSN READ ERR");
                OLED_Refresh();
                RTT_Printf("TSN_ReadTemperature err=%d\n", (int) rerr);
            }

            /* LED 心跳: 每 1 s 翻转一次 (~0.5 Hz), 表明程序在跑 */
            if (0U == (tsn_tick % 2U))
            {
                R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_04_PIN_02, BSP_IO_LEVEL_LOW);
            }
            else
            {
                R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_04_PIN_02, BSP_IO_LEVEL_HIGH);
            }

            R_BSP_SoftwareDelay(1000, BSP_DELAY_UNITS_MILLISECONDS);
            tsn_tick++;
        }
    }
}
