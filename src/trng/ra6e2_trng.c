/* RA6E2 片上 TRNG 可复用驱动（裸寄存器，不依赖 r_sce）。
 *
 * R_TRNG @0x40162000: +0x00 TRNGSDR(SEED 数据), +0x02 TRNGSCR0(bit2 SGSTART / bit3 SGCEN / bit7 RDRDY),
 *   +0x03 TRNGSCR1(bit0 INTEN)。
 * RDRDY=1 表示 32 位 SEED 就绪；连读 TRNGSDR 4 次取完整 32 位，读完后 RDRDY 自动清 0。
 *
 * 时序要点（RA6E2 硬件手册）：
 *   - TRNG 时钟默认在 LPC 模块停止状态，必须先 R_BSP_MODULE_START 打开，否则寄存器不工作
 *     （这正是 FSP 的 HW_SCE_RNG_Read() 假设“调用方已开 SCE 时钟”而自己不做的一步）。
 *   - SGCEN 置位后至少等 3 个 TRNGCLK 再 SGSTART。
 *   - 轮询 RDRDY 必须带超时上限，绝不死等。
 */
#include "ra6e2_trng.h"
#include "bsp_api.h"   /* R_BSP_MODULE_START/STOP, R_BSP_RegisterProtect*, FSP_IP_TRNG */

#define TRNG_SDR     (*(volatile uint8_t *) 0x40162000UL)
#define TRNG_SCR0    (*(volatile uint8_t *) 0x40162002UL)
#define TRNG_SCR1    (*(volatile uint8_t *) 0x40162003UL)
#define TRNG_SGCEN   (1u << 3)   /* SEED 生成电路使能 */
#define TRNG_SGSTART (1u << 2)   /* 启动一次生成 */
#define TRNG_RDRDY   (1u << 7)   /* SEED 就绪 */
#define TRNG_TIMEOUT 500000u     /* 轮询上限，绝不死等 */

static uint32_t s_last   = 0u;
static bool     s_inited = false;

void trng_init(void)
{
    if (s_inited) { return; }                          /* 幂等 */

    R_BSP_RegisterProtectDisable(BSP_REG_PROTECT_OM_LPC_BATT);
    R_BSP_MODULE_START(FSP_IP_TRNG, 0);                /* 开时钟（HW_SCE_RNG_Read 漏掉的前提） */
    R_BSP_RegisterProtectEnable(BSP_REG_PROTECT_OM_LPC_BATT);
    s_inited = true;
}

void trng_deinit(void)
{
    if (!s_inited) { return; }

    R_BSP_RegisterProtectDisable(BSP_REG_PROTECT_OM_LPC_BATT);
    R_BSP_MODULE_STOP(FSP_IP_TRNG, 0);
    R_BSP_RegisterProtectEnable(BSP_REG_PROTECT_OM_LPC_BATT);
    s_inited = false;
}

/* 核心：产生一次 32 位 SEED（4 字节）。成功返回 true。 */
static bool trng_read32(uint32_t *out)
{
    uint32_t guard;

    TRNG_SCR1 = 0x00u;                                 /* 关 TRNG 中断，纯轮询 */
    TRNG_SCR0 = (uint8_t)(TRNG_SCR0 | TRNG_SGCEN);     /* SGCEN=1: 使能 SEED 电路 */

    /* SGCEN 置位后至少等 3 个 TRNGCLK(手册), 用几次 volatile 读留足余量 */
    (void) TRNG_SCR0; (void) TRNG_SCR0; (void) TRNG_SCR0; (void) TRNG_SCR0;

    TRNG_SCR0 = (uint8_t)(TRNG_SCR0 | TRNG_SGSTART);   /* SGSTART=1: 启动一次生成 */

    for (guard = 0u; guard < TRNG_TIMEOUT; guard++)      /* 等 RDRDY=1, 带超时 */
    {
        if ((TRNG_SCR0 & TRNG_RDRDY) != 0u) { break; }
    }
    if ((TRNG_SCR0 & TRNG_RDRDY) == 0u)
    {
        TRNG_SCR0 = (uint8_t)(TRNG_SCR0 & ~TRNG_SGCEN); /* 失败：关电路，避免卡在使能态 */
        s_last = 0u;
        return false;
    }

    {
        uint32_t v =  (uint32_t) TRNG_SDR
                    | ((uint32_t) TRNG_SDR << 8)
                    | ((uint32_t) TRNG_SDR << 16)
                    | ((uint32_t) TRNG_SDR << 24);       /* 连读 4 次取完整 32 位 SEED */
        TRNG_SCR0 = (uint8_t)(TRNG_SCR0 & ~TRNG_SGCEN);  /* 关电路省电（模块时钟留着，不用 STOP） */
        s_last = v;
        *out = v;
    }
    return true;
}

bool trng_u32(uint32_t *out)
{
    if (!s_inited) { trng_init(); }                     /* 防御：未 init 也能用 */
    return trng_read32(out);
}

bool trng_bytes(uint8_t *buf, size_t n)
{
    size_t i;

    if (!s_inited) { trng_init(); }
    for (i = 0u; i < n; )
    {
        uint32_t w;
        size_t  k;

        if (!trng_read32(&w)) { return false; }
        k = n - i; if (k > 4u) { k = 4u; }
        for (size_t j = 0u; j < k; j++) { buf[i + j] = (uint8_t)(w >> (8u * j)); }
        i += k;
    }
    return true;
}

uint32_t trng_last_value(void)
{
    return s_last;
}
