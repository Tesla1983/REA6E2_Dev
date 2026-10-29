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
#include "nt7534/dino_frames.h"   /* 离线恐龙 2 帧 + 仙人掌, scripts/gen_dino_frames.py 生成 */
#include "oled/bsp_oled.h"             /* 1.3" OLED 兼作"第二指示灯" */
#include "rtt/rtt_log.h"
#include "uart/bsp_uart_link.h"        /* 上位机数据（SCI0 / USB CDC 共用解析） */
#include "usb/bsp_usb_cdc.h"          /* Type-C 虚拟串口（P814/P815 原生 USB） */

/* 板载 LED: P402, 低电平点亮 */
#define LED_PIN   ((bsp_io_port_pin_t) BSP_IO_PORT_04_PIN_02)

/* 1 = STAGE6 走"列偏移 0..4 全扫"自检；0 = 直接用 NT7534_COL_OFFSET 常驻方案 A。
 * 实测偏移 3 是对的（用户目视确认 4/5 那张四边齐），所以默认 0。 */
#define NT7534_OFFSET_SWEEP  (0)

/* 离线动画: USB 未枚举(场景A)时屏上显示什么。
 * 1 = Chrome 断网恐龙风格(恐龙随机跳 + 地面滚动 + 仙人掌左移, 纯装饰不碰撞);
 * 0 = 回退到原 USB 中断寄存器自证页(draw_usb_diag, 便于 USB 中断调试)。 */
#define NT7534_OFFLINE_DINO  (1)

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
#if (NT7534_OFFLINE_DINO == 0)   /* 仅宏切回(=0)时保留 USB 寄存器自证页 */
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
#endif /* (NT7534_OFFLINE_DINO == 0) */

/* ===== 离线恐龙动画（场景 A：USB 未枚举）=====
 * 替代原寄存器自证页。恐龙在地面随机跳, 地面虚线向左滚, 仙人掌从右向左飘(纯装饰, 不碰撞)。
 * 状态机由 NT7534_Run() 的 2ms 细片循环驱动(dino_advance + draw_dino_offline)。 */

extern volatile uint32_t g_ms_tick;   /* 由 SysTick 递增, 用作 dino_init 的随机种子 */

/* --- 物理/节奏参数（像素 / 毫秒）--- */
#define DINO_GROUND_Y       62          /* 地面线 y */
#define DINO_BASE_X         6           /* 恐龙站立足底 x 左上角 */
#define DINO_JUMP_H_MIN     6           /* 随机跳跃峰值下限(px) */
#define DINO_JUMP_H_MAX     19          /* 随机跳跃峰值上限(px): 站立头顶在屏 y19 ⇒ 跳 19 时 dino_y = 17-19 = -2,
                                          * 绘制时裁掉帧顶 2 行空白(y0/y1 本为空)让头顶正好贴屏 y0, 用满空间不越界。 */
#define DINO_JUMP_DUR       600         /* 一次跳跃总时长(ms): 上 300 + 下 300 */
#define DINO_JUMP_MIN       600         /* 下次跳最小间隔(ms) */
#define DINO_JUMP_MAX       2200        /* 下次跳最大间隔(ms) */
#define DINO_LEG_INTERVAL   120         /* 待机腿帧交替周期(ms) */
#define DINO_FRAME_MS       33          /* 动画帧间隔 ≈ 30fps */
#define DINO_SPEED_100      3           /* 地面/仙人掌速度: 0.03 px/ms = 30 px/s, 累积单位 1/100 px。
                                          * 说明: 原用 1/10 px 单位时最小只能到 1 = 100 px/s, 仍偏快 ⇒ 换 1/100 px
                                          * 粒度, 30 px/s 穿 128px 屏约 4.8s, 仙人掌能看清。 */
#define DINO_CACTUS_GAP_MIN 28
#define DINO_CACTUS_GAP_MAX 70

/* --- 状态 --- */
static uint8_t  s_dino_prev_offline = 0u;   /* 上一轮是否场景A(检测进入以 init) */
static uint32_t s_dino_tick         = 0u;   /* 上次动画刷新时刻(ms) */
static uint32_t s_dino_next_jump    = 0u;   /* 距下次跳剩余(ms), 在跳时为 0 */
static uint32_t s_dino_jump_t       = 0u;   /* 当前跳跃已进行(ms), 0 = 未在跳 */
static uint32_t s_dino_jump_h       = 6u;   /* 本次跳跃峰值高度(px), 起跳时在 [MIN,MAX] 内随机 */
static uint32_t s_dino_leg_t        = 0u;   /* 腿帧计时 */
static uint8_t  s_dino_leg          = 0u;   /* 当前腿帧 0/1 */
static uint32_t s_dino_ground       = 0u;   /* 地面滚动累积(1/100 px) */
static int32_t  s_cactus_pos[2]     = { 4800, 10400 };  /* 仙人掌 x(1/100 px), 初值 48.0 / 104.0 */
static uint8_t  s_cactus_kind[2]    = { 1u, 2u };       /* 每株仙人掌的高度档位(0..CACTUS_N-1) */
/* 云: 背景装饰, 只出现在恐龙站立头顶(y19)以上的 y0..18 区域; 三种尺寸随机; 缓慢左移(视差) */
#define DINO_CLOUD_CNT  2
static int32_t  s_cloud_pos[DINO_CLOUD_CNT]  = { 2600, 8200 };  /* 1/100 px: 26.0 / 82.0 */
static uint8_t  s_cloud_kind[DINO_CLOUD_CNT] = { 0u, 2u };      /* 云尺寸档位(0..CLOUD_N-1) */
static uint8_t  s_cloud_y[DINO_CLOUD_CNT]    = { 2u, 8u };      /* 云顶部 y(0..18-h) */
static uint32_t s_dino_rng          = 0x12345678u;    /* xorshift32 状态 */

/* ---- 片上 TRNG（RA6E2 硬件有 TRNG，FSP 6.6 无 r_trng 驱动 ⇒ 裸寄存器）----
 * R_TRNG @0x40162000: +0x00 TRNGSDR(SEED 数据), +0x02 TRNGSCR0(bit2 SGSTART / bit3 SGCEN / bit7 RDRDY),
 *   +0x03 TRNGSCR1(bit0 INTEN)。
 * RDRDY=1 表示 32 位 SEED 就绪；连读 TRNGSDR 4 次取完整 32 位，读完后 RDRDY 自动清 0。
 * 模块启停照 FSP system.c: 先解锁 LPC 寄存器保护再 R_BSP_MODULE_START，用完再 stop。
 * ⚠ 全程带超时，绝不无限阻塞；失败返回 0，由调用方回落到 g_ms_tick 抖动。 */
#define DINO_TRNG_SDR   (*(volatile uint8_t *) 0x40162000UL)
#define DINO_TRNG_SCR0  (*(volatile uint8_t *) 0x40162002UL)
#define DINO_TRNG_SCR1  (*(volatile uint8_t *) 0x40162003UL)

volatile uint32_t g_dino_trng_dbg = 0u;   /* 调试: 最近一次 TRNG 读值(0 = 超时/失败, 可用 probe-rs 读 RAM 验证) */

static uint32_t dino_trng_u32(void)
{
    uint32_t guard;
    uint32_t v;

    R_BSP_RegisterProtectDisable(BSP_REG_PROTECT_OM_LPC_BATT);
    R_BSP_MODULE_START(FSP_IP_TRNG, 0);
    R_BSP_RegisterProtectEnable(BSP_REG_PROTECT_OM_LPC_BATT);

    DINO_TRNG_SCR1 = 0x00u;                                   /* 关 TRNG 中断 */
    DINO_TRNG_SCR0 = (uint8_t)(DINO_TRNG_SCR0 | (1u << 3));   /* SGCEN=1: 使能 SEED 生成电路 */

    /* SGCEN 置位后至少等 3 个 TRNGCLK(手册), 用几次 volatile 读留足余量 */
    (void) DINO_TRNG_SCR0; (void) DINO_TRNG_SCR0; (void) DINO_TRNG_SCR0; (void) DINO_TRNG_SCR0;

    DINO_TRNG_SCR0 = (uint8_t)(DINO_TRNG_SCR0 | (1u << 2));   /* SGSTART=1: 启动一次生成 */

    for (guard = 0u; guard < 500000u; guard++)                /* 等 RDRDY=1, 带超时 */
    {
        if ((DINO_TRNG_SCR0 & 0x80u) != 0u) { break; }
    }
    if ((DINO_TRNG_SCR0 & 0x80u) == 0u)
    {
        DINO_TRNG_SCR0 = (uint8_t)(DINO_TRNG_SCR0 & ~(1u << 3));
        g_dino_trng_dbg = 0u;
        return 0u;                                            /* 超时: 调用方回落 */
    }

    v  =  (uint32_t) DINO_TRNG_SDR;
    v |= ((uint32_t) DINO_TRNG_SDR) << 8;
    v |= ((uint32_t) DINO_TRNG_SDR) << 16;
    v |= ((uint32_t) DINO_TRNG_SDR) << 24;

    DINO_TRNG_SCR0 = (uint8_t)(DINO_TRNG_SCR0 & ~(1u << 3));  /* 关电路省电 */
    g_dino_trng_dbg = v;
    return v;
}

/* 轻量 PRNG（无 libm 依赖） */
static uint32_t dino_rand_u32(void)
{
    uint32_t x = s_dino_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s_dino_rng = x;
    return x;
}
static uint32_t dino_rand_range(uint32_t lo, uint32_t hi)
{
    return lo + (dino_rand_u32() % (hi - lo + 1u));
}

static void dino_init(void)
{
    /* 用片上 TRNG 播种; TRNG 超时/失败则回落 g_ms_tick 抖动, 保证种子非空 */
    {
        uint32_t trng = dino_trng_u32();
        s_dino_rng = (trng != 0u) ? (trng | 1u) : ((0x12345678u ^ g_ms_tick) | 1u);
    }
    s_dino_jump_t    = 0u;
    s_dino_jump_h    = DINO_JUMP_H_MIN;
    s_dino_leg_t     = 0u;
    s_dino_leg       = 0u;
    s_dino_ground    = 0u;
    s_cactus_pos[0]  = 4800;   /* 48.0 px */
    s_cactus_pos[1]  = 10400;  /* 104.0 px */
    s_cactus_kind[0] = (uint8_t) dino_rand_range(0u, (uint32_t) CACTUS_N - 1u);
    s_cactus_kind[1] = (uint8_t) dino_rand_range(0u, (uint32_t) CACTUS_N - 1u);

    /* 云: 随机尺寸 + 在恐龙头顶以上(y0..18)随机高度; 初始一左一右 */
    s_cloud_pos[0]  = 2600;
    s_cloud_kind[0] = (uint8_t) dino_rand_range(0u, (uint32_t) CLOUD_N - 1u);
    s_cloud_y[0]    = (uint8_t) dino_rand_range(0u, 18u - g_cloud_hs[s_cloud_kind[0]]);
    s_cloud_pos[1]  = 8200;
    s_cloud_kind[1] = (uint8_t) dino_rand_range(0u, (uint32_t) CLOUD_N - 1u);
    s_cloud_y[1]    = (uint8_t) dino_rand_range(0u, 18u - g_cloud_hs[s_cloud_kind[1]]);
    s_dino_next_jump = dino_rand_range(DINO_JUMP_MIN, DINO_JUMP_MAX);
}

/** 推进一帧动画。dt_ms = 距上一帧真实毫秒(已 clamp <=100)。 */
static void dino_advance(uint32_t dt_ms)
{
    uint32_t i;

    /* 地面滚动 + 仙人掌左移: 用**固定每帧 1px(100 = 1.00px)**，不再按 dt 比例推进。
     * 原 `dt * DINO_SPEED_100`: dt 在 32~35ms 间抖动 ⇒ 个别帧只走 0px、个别帧走 2px,
     * 移动中的仙人掌逐帧不匀, 看起来就是"闪烁/抖动"。30fps 固定 1px/帧 = 30 px/s,
     * 与 DINO_SPEED_100=3 的标称速度一致, 但每帧位移严格均匀。 */
    s_dino_ground += 100u;
    for (i = 0u; i < 2u; i++)
    {
        s_cactus_pos[i] -= 100;
        int32_t cx = s_cactus_pos[i] / 100;
        if (cx <= -(int32_t)CACTUS_W)
        {
            int32_t other = s_cactus_pos[(i == 0u) ? 1u : 0u] / 100;
            int32_t gap   = (int32_t)dino_rand_range(DINO_CACTUS_GAP_MIN, DINO_CACTUS_GAP_MAX);
            int32_t newpx = other + gap;
            if (newpx < 128) { newpx = 128 + gap; }
            s_cactus_pos[i]  = newpx * 100;
            s_cactus_kind[i] = (uint8_t) dino_rand_range(0u, (uint32_t) CACTUS_N - 1u);
        }
    }

    /* 云: 缓慢左移(~0.33 px/帧 ≈ 10 px/s, 视差); 出屏后从右侧按随机尺寸/高度重入 */
    for (i = 0u; i < DINO_CLOUD_CNT; i++)
    {
        s_cloud_pos[i] -= 33;
        int32_t clx = s_cloud_pos[i] / 100;
        if (clx <= -(int32_t) g_cloud_ws[s_cloud_kind[i]])
        {
            int32_t other = s_cloud_pos[(i == 0u) ? 1u : 0u] / 100;
            int32_t gap   = (int32_t) dino_rand_range(24u, 64u);
            int32_t newpx = other + gap;
            if (newpx < 128) { newpx = 128 + gap; }
            s_cloud_pos[i]  = newpx * 100;
            s_cloud_kind[i] = (uint8_t) dino_rand_range(0u, (uint32_t) CLOUD_N - 1u);
            s_cloud_y[i]    = (uint8_t) dino_rand_range(0u, 18u - g_cloud_hs[s_cloud_kind[i]]);
        }
    }

    /* 腿帧交替(待机跑动感) */
    s_dino_leg_t += dt_ms;
    if (s_dino_leg_t >= DINO_LEG_INTERVAL) { s_dino_leg_t = 0u; s_dino_leg ^= 1u; }

    /* 跳跃调度 */
    if (s_dino_jump_t == 0u)
    {
        if (s_dino_next_jump > dt_ms) { s_dino_next_jump -= dt_ms; }
        else
        {
            /* 起跳: 本次峰值高度随机 [MIN,MAX], 保证不越过屏顶 */
            s_dino_jump_h = dino_rand_range(DINO_JUMP_H_MIN, DINO_JUMP_H_MAX);
            s_dino_jump_t = 1u;
        }
    }
    else
    {
        if (s_dino_jump_t + dt_ms < DINO_JUMP_DUR) { s_dino_jump_t += dt_ms; }
        else
        {
            s_dino_jump_t    = 0u;
            s_dino_next_jump = dino_rand_range(DINO_JUMP_MIN, DINO_JUMP_MAX);
        }
    }
}

/** 当前跳跃高度(px, 三角波: 0→本次峰值 s_dino_jump_h→0)。 */
static uint32_t dino_y_off(void)
{
    if (s_dino_jump_t == 0u) { return 0u; }
    uint32_t half = DINO_JUMP_DUR / 2u;     /* 300 */
    uint32_t d = (s_dino_jump_t < half) ? s_dino_jump_t : (DINO_JUMP_DUR - s_dino_jump_t);
    return (s_dino_jump_h * d) / half;      /* 0..s_dino_jump_h(6..12) */
}

/** 整屏画一帧离线恐龙动画（自获取 u8g2 句柄, 可在任意调用点使用）。 */
static void draw_dino_offline(void)
{
    u8g2_t * g = U8G2_Port();
    uint32_t i;
    uint32_t y_off = dino_y_off();
    /* 官方帧底部有 2 行留白(y45/46 全空), +2 让恐龙脚贴住地面线 y62 */
    int32_t  dino_y = (int32_t)(DINO_GROUND_Y - DINO_H + 2 - y_off);
    /* 跳跃时用官方"双腿着地抬头"帧; 地面待机用跑步两帧交替 */
    const uint8_t * dino_bmp = (s_dino_jump_t > 0u) ? g_dino_jump : g_dino_frames[s_dino_leg];
    /* 高处跳时 dino_y 会变负, 而 u8g2 的 y 是无符号, 直接转会回绕成巨值画到屏外 ⇒
     * 裁掉位图顶部若干行(每行 (DINO_W+7)/8 字节), 从第 -dino_y 行开始画, y 归 0。
     * 帧顶 y0/y1 本就是空行, 裁掉的正是空白, 造型不受影响。 */
    uint32_t dino_draw_h = DINO_H;
    if (dino_y < 0)
    {
        uint32_t skip = (uint32_t)(-dino_y);
        if (skip >= DINO_H) { skip = DINO_H - 1u; }
        dino_bmp   += skip * ((DINO_W + 7u) / 8u);
        dino_draw_h = DINO_H - skip;
        dino_y      = 0;
    }

    u8g2_ClearBuffer(g);

    /* 云(背景, 最先画 ⇒ 恐龙跳起来时压住云): 只出现在恐龙站立头顶 y19 以上的 y0..18 区域 */
    for (i = 0u; i < DINO_CLOUD_CNT; i++)
    {
        int32_t clx = s_cloud_pos[i] / 100;
        uint8_t ck  = s_cloud_kind[i];
        uint8_t cw  = g_cloud_ws[ck];
        if (clx > -(int32_t) cw && clx < 128)
        {
            u8g2_DrawXBMP(g, (u8g2_uint_t)clx, (u8g2_uint_t)s_cloud_y[i],
                          cw, g_cloud_hs[ck], g_cloud_bmp[ck]);
        }
    }

    /* 仙人掌(底部贴地面) */
    for (i = 0u; i < 2u; i++)
    {
        int32_t cx   = s_cactus_pos[i] / 100;
        uint8_t kind = s_cactus_kind[i];
        uint8_t ch   = g_cactus_hs[kind];
        if (cx > -(int32_t)CACTUS_W && cx < 128)
        {
            u8g2_DrawXBMP(g, (u8g2_uint_t)cx, (u8g2_uint_t)(DINO_GROUND_Y - ch + 1),
                          CACTUS_W, ch, g_cactus_bmp[kind]);
        }
    }
    /* 恐龙 */
    u8g2_DrawXBMP(g, (u8g2_uint_t)DINO_BASE_X, (u8g2_uint_t)dino_y,
                  DINO_W, dino_draw_h, dino_bmp);

    /* 地面线放在恐龙之后画(双保险): 位图已在 U8G2_Port_Init 里开透明模式, 恐龙不再擦背景;
     *   但恐龙绘制框 47 行、跑步时到 y63, 仍最后补画地面线/虚线, 确保脚下地平线绝不断。 */
    u8g2_DrawHLine(g, 0, (int16_t)DINO_GROUND_Y, 128);
    /* 地面滚动短横(每段 4px, 间隔 8px, 随滚动偏移) */
    {
        int16_t off = (int16_t)((s_dino_ground / 100u) % 8u);
        int16_t x;
        for (x = (int16_t)(-off); x < 128; x = (int16_t)(x + 8))
        {
            if (x >= 0) { u8g2_DrawHLine(g, (u8g2_uint_t)x, (int16_t)(DINO_GROUND_Y + 1), 4); }
        }
    }
    U8G2_Port_Flush();
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
 *   数据只更新 RPM 值; 动画由主循环 2ms 切片计数自行推进(本 M33 的 DWT CYCCNT 实测可用, 此处未采用)。
 */

/** 两台风扇的相位, 单位 1/100 度。3 叶对称周期 120° = 12000 cd = 24 帧。 */
static int32_t  s_fan_phase_cd[2] = { 0, 0 };

/** 1 ms 系统心跳（Cortex-M 内核 SysTick 中断驱动）。
 *  为什么要自己开:
 *   - DWT 的 CYCCNT 实测**可用**(2026-10-26 确认: 需先 DWT_LAR 解锁 + DEMCR.TRCENA + DWT_CTRL.CYCCNTENA, 见下方 DWT 块);
 *   - FSP 裸机没有系统时基, USB/串口都不依赖 SysTick, 所以这里自建一个不会打架;
 *   - 中断优先级保持默认(最低), 不会挤掉 USB 中断。
 *  用途: 风扇转速按**真实时间**推进(与帧率无关)、主循环按 1 s 定拍、串口轮询按 50 ms。 */
volatile uint32_t g_ms_tick = 0u;

/* RA6E2 实测 CPU 占用率。2026-10-26 改用 **DWT CYCCNT 周期计数法**(比早先 SysTick
 * 累加法更精确): 每 2ms 细片在 R_BSP_SoftwareDelay(2) 软件空转**结束后**读一次 CYCCNT,
 * 本片所有工作(USB/UART/风扇重绘)结束再读一次, 二者之差(=真正干活周期, 不含空转)累加进
 * s_cpu_busy_cyc; 每 1s 窗口 load = 忙周期 / 窗口总周期 * 1000 (单位 0.1%)。
 * 用 0.1% 分辨率是为了 OLED 显示一位小数(如 "19.3%"), 避免四舍五入成整数后看不出波动。 */
volatile uint16_t g_cpu_load_x10 = 0u;   /* 0..1000, 单位 0.1%, 供 1.3" OLED 显示 "x.x%" */
volatile uint8_t  g_cpu_load_pct = 0u;   /* 0..100, 整数% (=g_cpu_load_x10/10, 兼容旧调试) */

/* ================= DWT CYCCNT（2026-10-26 实测可用） =================
 * 此前注释误判"本 M33 上 CYCCNT 不走"——实为未解锁/未使能就直接读。
 * 实测(按序): 1) DWT_LAR=0xC5ACCE55 解锁  2) DEMCR.TRCENA=1  3) DWT_CTRL.CYCCNTENA=1
 *   后 CYCCNT 在自由运行下正常递增(1s 增量≈1.21亿, 对应内核~120MHz)。
 * 此处使能, 现直接用于 CPU 占用率测量(见上方 g_cpu_load_pct 注释); g_dwt_diag 存使能后的
 * CTRL/DEMCR 值, 可随时 probe-rs 直读确认使能成功。 */
#define DWT_LAR_REG       (*(volatile uint32_t *)(0xE0001000UL + 0xFB0UL))
#define DWT_CTRL_REG      (*(volatile uint32_t *)(0xE0001000UL + 0x000UL))
#define DWT_CYCCNT_REG    (*(volatile uint32_t *)(0xE0001000UL + 0x004UL))
#define DEMCR_REG         (*(volatile uint32_t *)(0xE000EDFCUL))
#define DEMCR_TRCENA_BIT  (1UL << 24)
#define DWT_CYCCNTENA_BIT (1UL << 0)

volatile uint32_t g_dwt_diag[2];   /* [0]=使能后DWT_CTRL [1]=使能后DEMCR(运行期可观测证据) */
volatile uint32_t g_cpu_dbg_busy  = 0u;   /* 上次 1s 窗口"干活"周期(不含空转), 调试观测 */
volatile uint32_t g_cpu_dbg_total = 0u;   /* 上次 1s 窗口总周期, 调试观测 */

static void dwt_enable (void)
{
    DWT_LAR_REG     = 0xC5ACCE55UL;          /* 1) 解锁 DWT (CoreSight LAR) */
    DEMCR_REG      |= DEMCR_TRCENA_BIT;       /* 2) 全局跟踪使能 */
    DWT_CTRL_REG   |= DWT_CYCCNTENA_BIT;      /* 3) 周期计数器使能 */
    DWT_CYCCNT_REG  = 0u;                     /* 清 0, 从干净起点计数 */
    g_dwt_diag[0]   = DWT_CTRL_REG;           /* 读回确认 */
    g_dwt_diag[1]   = DEMCR_REG;
}
static uint32_t  s_cpu_busy_cyc     = 0u;   /* 当前 1s 窗口累计"干活"周期(不含空转) */
static uint32_t  s_window_start_cyc = 0u;   /* 当前 1s 窗口起点的 CYCCNT */

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

/* ===== 中文点阵（16x15，宋体，2026-10-28）=====
 * 屏状态行只有 15 px 高（y48..62），故用 16x15 点阵放「已连接 / 未连接」。
 * 每字 15 个 uint16_t：bit15 = 最左列（col0），bit0 = 第 16 列。
 * 由系统字体渲染生成（simsun, size=15, 阈值 100, 墨迹居中贴格），只含本需求 4 个字
 * （已 / 连 / 接 / 未），共 4×30 = 120 B。
 * ⚠ 仅 u8g2 路径（U8G2_PORT_ENABLE=1）使用；旧 NT7534_DrawString 路径不支持中文。 */
static const uint16_t g_zh_yi[15] = {
    0x0000u, 0x3FF8u, 0x0008u, 0x0008u, 0x2008u, 0x2008u, 0x3FF8u, 0x2000u, 0x2000u, 0x2000u, 0x2000u, 0x2004u, 0x2004u, 0x1FFCu, 0x0000u
};
static const uint16_t g_zh_lian[15] = {
    0x0080u, 0x2080u, 0x17F8u, 0x1100u, 0x0240u, 0x0240u, 0x77F8u, 0x1040u, 0x1040u, 0x17FCu, 0x1040u, 0x1040u, 0x2840u, 0x47FCu, 0x0000u
};
static const uint16_t g_zh_jie[15] = {
    0x1080u, 0x1040u, 0x13F8u, 0x7D10u, 0x10A0u, 0x17FCu, 0x1080u, 0x1880u, 0x77FCu, 0x1110u, 0x1320u, 0x10C0u, 0x1130u, 0x7608u, 0x0000u
};
static const uint16_t g_zh_wei[15] = {
    0x0100u, 0x0100u, 0x3FF8u, 0x0100u, 0x0100u, 0x0100u, 0x7FFCu, 0x0380u, 0x0540u, 0x0920u, 0x1110u, 0x610Cu, 0x0100u, 0x0100u, 0x0000u
};

/** 画一个 16x15 汉字点阵（bit15 = 最左列），逐点置位。
 *  当前 FontMode = 1（透明）⇒ 只置位、不清背景，不会擦断表格线。 */
static void draw_zh15(u8g2_t * g, const uint16_t * glyph, int16_t x, int16_t y)
{
    int row;
    int col;

    for (row = 0; row < 15; row++)
    {
        uint16_t bits = glyph[row];

        for (col = 0; col < 16; col++)
        {
            if (0u != (bits & (uint16_t) (0x8000u >> col)))
            {
                u8g2_DrawPixel(g, (u8g2_uint_t) (x + col), (u8g2_uint_t) (y + row));
            }
        }
    }
}

/** 状态行：按 g_screen_lang 选中/英文。
 *  中文「已连接 / 未连接」= 3 字 × 16 px = 48 px，居中于 x1..53（宽 53）、y48..62（高 15）；
 *  英文仍走原 draw_cell（7x13），行为与之前完全一致。 */
static void draw_status_cell(u8g2_t * g)
{
    if (0u == g_screen_lang)
    {
        draw_cell(g, UART_Link_Online() ? "linked" : "unlink", 1, 53, 48, 62);
        return;
    }

    {
        const uint16_t * first = UART_Link_Online() ? g_zh_yi : g_zh_wei;
        int16_t          x     = (int16_t) (1 + (53 - 48) / 2);   /* = 3 */

        draw_zh15(g, first,     x, 48);
        draw_zh15(g, g_zh_lian, (int16_t) (x + 16), 48);
        draw_zh15(g, g_zh_jie,  (int16_t) (x + 32), 48);
    }
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

    /* USB 未枚举(场景A): 默认显示离线恐龙动画; 宏 NT7534_OFFLINE_DINO=0 时回退寄存器自证页。
     * ⚠ 场景A 分支里**绝不能提前 ClearBuffer**: 本屏是 NT7534_BUS_MODE=2 的异步分页 DMA 刷新,
     *   Flush 只是启动 DMA, 之后 8 页由 NT7534_Pump_Poll 每 2ms 推一页(~11ms 发完), 期间 DMA 一直在读
     *   framebuffer。若在此清空缓冲又不重画, 正在飞的页会把空白发上屏 ⇒ "整屏随机闪白"。
     *   清缓冲只放在"本帧后面必然重画并 Flush"的路径里。 */
    if (!UART_Link_Online() && !USB_CDC_Configured())
    {
        if (0u == s_dino_prev_offline) { dino_init(); s_dino_prev_offline = 1u; }
#if (NT7534_OFFLINE_DINO != 0)
        /* 清缓冲 + 绘制 + Flush 由 30fps 驱动(draw_dino_offline)成对完成, 这里不碰 buffer。 */
#else
        u8g2_ClearBuffer(g);
        draw_usb_diag(g);
        U8G2_Port_Flush();
#endif
        return;
    }
    s_dino_prev_offline = 0u;

    u8g2_ClearBuffer(g);

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
    draw_status_cell(g);

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

    dwt_enable();   /* DWT CYCCNT 已实测可用, 使能供将来高精度计时(见上方定义) */

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
            char    tl[24];
            char    ll[24];
            if (FSP_SUCCESS == TSN_ReadTemperature(&t))
            {
                (void) snprintf(tl, sizeof(tl), "T=%ld.%ld C",
                                (long) (t / 10),
                                (long) ((t < 0) ? (-(t % 10)) : (t % 10)));
                /* 第四排(y=48): RA6E2 实测 CPU 占用率, 一位小数(0.1% 精度, 不取整)。
                 * g_cpu_load_x10 由主循环每 1s 更新(单位 0.1%)。 */
                (void) snprintf(ll, sizeof(ll), "MCU %2u.%u%%",
                                (unsigned) (g_cpu_load_x10 / 10u),
                                (unsigned) (g_cpu_load_x10 % 10u));
                OLED_Clear();
                OLED_DrawString(0, 0, "RA6E2 CHIP T");
                OLED_DrawString(0, 16, tl);
                OLED_DrawString(0, 32, "TSN->ADC12(-3)");
                OLED_DrawString(0, 48, ll);
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
        s_window_start_cyc = DWT_CYCCNT_REG;   /* 本 1s 窗口 CYCCNT 起点 */
        t_uart = t_loop;
        t_anim = t_loop;

        for (;;)
        {
            uint32_t now;

            R_BSP_SoftwareDelay(2u, BSP_DELAY_UNITS_MILLISECONDS);
            uint32_t cyc_wd = DWT_CYCCNT_REG;/* 空转结束时刻的周期计数: 此后才计为"干活"(空转不计入) */
            USB_CDC_Poll();
#if (NT7534_BUS_MODE == 2)
            /* 推进 S3 异步分页刷新状态机(每 2ms 一轮, 8 页 ~11ms 内发完) */
            NT7534_Pump_Poll();
#endif

            now = g_ms_tick;
            if ((uint32_t) (now - t_uart) >= 50u)
            {
                t_uart = now;
                UART_Link_Poll();
                UART_Link_Tick(50u);
            }
#if (0 != U8G2_PORT_ENABLE)
            /* 风扇动画: 按**真实时间**推进, 与上位机送数频率、与帧率都无关。
             *  自适应双档(省 CPU):
             *   - 扇叶真在转(有数据 且 cf/gf 至少一个 > 0) ⇒ 高帧 NT7534_FAN_FRAME_MS_FAST
             *     (默认 8 ms ⇒ ≈125 fps, 顺滑度最好);
             *   - 静止(无数据 / 或数据在但 RPM=0) ⇒ 低帧 NT7534_FAN_FRAME_MS_IDLE
             *     (默认 250 ms ⇒ ≈4 fps): 相位不前进, 低帧仅"保活 + 兜底修正", 几乎不占 CPU;
             *     无数据时动画块内部直接跳过(0 重绘), 由 1Hz 整屏刷新维持显示。
             *  相位 = 真实 dt × 1.5(即"角速度 = RPM/24 度/秒 的 1.5 倍")。 */
            {
                const uart_link_data_t * pd = UART_Link_Data();
                uint32_t                 fan_interval;

                if (((pd->cf >= 0) || (pd->gf >= 0)) &&
                    ((pd->cf >  0) || (pd->gf >  0)))
                {
                    fan_interval = NT7534_FAN_FRAME_MS_FAST;   /* 转动中: 高帧, 求顺滑 */
                }
                else
                {
                    fan_interval = NT7534_FAN_FRAME_MS_IDLE;   /* 静止: 降帧省 CPU */
                }

                if ((uint32_t) (now - t_anim) >= fan_interval)
                {
                    uint32_t dt = (uint32_t) (now - t_anim);

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
            }
#endif
            /* 离线恐龙动画: 仅场景A(USB 未枚举) 且宏开启时驱动, ≈30fps 整屏。 */
#if (0 != U8G2_PORT_ENABLE)
            if ((!UART_Link_Online()) && (!USB_CDC_Configured()) && (NT7534_OFFLINE_DINO != 0))
            {
                if ((uint32_t) (now - s_dino_tick) >= DINO_FRAME_MS)
                {
                    uint32_t dt = (uint32_t) (now - s_dino_tick);
                    s_dino_tick = now;
                    if (dt > 100u) { dt = 100u; }
                    dino_advance(dt);
                    draw_dino_offline();
                }
            }
#endif
            /* 统计本片"干活"周期(不含 SoftwareDelay 空转)。cyc_end 抓在本片所有工作之后。 */
            {
                uint32_t cyc_end = DWT_CYCCNT_REG;
                s_cpu_busy_cyc += (cyc_end - cyc_wd);   /* 仅真正干活周期, 空转已排在外面 */
            }

            if ((uint32_t) (now - t_loop) >= 1000u)
            {
                /* 1 s 窗口到: 用 CYCCNT 算 RA6E2 实测 CPU 占用率(%)。
                 *   numerator = 本窗口累计"干活"周期(已排除空转)
                 *   denominator = 窗口内总周期(CYCCNT 自由运行)
                 *   ⇒ 空转天然不计入分母外的分子, 但分母含空转 ⇒ 占用率=真干活占比 */
                uint32_t cyc_now = DWT_CYCCNT_REG;
                uint32_t total   = cyc_now - s_window_start_cyc;
                /* 0.1% 分辨率(*1000): 保留一位小数, 不四舍五入成整数。
                 * 用 64 位乘法防溢出: busy*1000 在 busy>~4.29M 时必超 uint32(本窗口 busy 可达 ~120M)。 */
                uint32_t load_x10 = (total > 0u)
                                   ? (uint32_t) (((uint64_t) s_cpu_busy_cyc * 1000u) / total)
                                   : 0u;
                if (load_x10 > 1000u) { load_x10 = 1000u; }
                g_cpu_load_x10   = (uint16_t) load_x10;
                g_cpu_load_pct   = (uint8_t)  (load_x10 / 10u);   /* 整数部分(截断), 兼容旧调试 */
                g_cpu_dbg_busy  = s_cpu_busy_cyc;   /* 调试: 暴露真实 busy/total 供 SWD 直读 */
                g_cpu_dbg_total = total;
                s_cpu_busy_cyc   = 0u;
                s_window_start_cyc = cyc_now;

                break;                               /* 本轮 1 s 到 */
            }
        }
    }
}
