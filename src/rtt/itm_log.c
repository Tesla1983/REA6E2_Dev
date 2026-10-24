/********************************************************************
 * ITM 输出 —— 实现（直接操作寄存器，不依赖 CMSIS 的 ITM 宏，
 * 因为不同 CMSIS 版本宏名不一致，地址则是架构固定的）
 *
 * 安全性：CMSIS 官方的 ITM_SendChar 是 while(PORT0 == 0) 死等，
 * 主机没在抓的时候会把程序卡死。这里改成非阻塞 —— FIFO 不可用就丢弃，
 * 保证业务永远不被调试输出拖住。
 *********************************************************************/
#include "rtt/itm_log.h"

#include <stdarg.h>
#include <stdio.h>

/** ITM 基址（Cortex-M33 固定） */
#define ITM_BASE_ADDR    (0xE0000000UL)
/** 刺激端口 0, 按 32 位读: 非 0 表示 FIFO 可写 */
#define ITM_STIM0        (*(volatile uint32_t *) (ITM_BASE_ADDR + 0x000UL))
/** Trace Enable Register: bit0 = 端口 0 使能（由主机设置） */
#define ITM_TER          (*(volatile uint32_t *) (ITM_BASE_ADDR + 0xE00UL))
/** Trace Control Register: bit0 = ITMENA（由主机设置） */
#define ITM_TCR          (*(volatile uint32_t *) (ITM_BASE_ADDR + 0xE80UL))

#define ITM_TCR_ITMENA   (0x1UL)
#define ITM_TER_PORT0    (0x1UL)

/** CoreSight 解锁钥匙: 写进每个组件的 LAR 才能改它的寄存器 */
#define CS_LAR_KEY       (0xC5ACCE55UL)
/** ITM Lock Access Register */
#define ITM_LAR          (*(volatile uint32_t *) (ITM_BASE_ADDR + 0xFB0UL))

/** TraceFunnel 基址(ROM 表实测 0xe0047000): ITM 数据经它进 TMC/ETF */
#define FUNNEL_BASE      (0xE0047000UL)
/** Funnel Control: bit n = 第 n 路输入使能（ITM 接在第 0 路） */
#define FUNNEL_CTRL      (*(volatile uint32_t *) (FUNNEL_BASE + 0x000UL))
#define FUNNEL_LAR       (*(volatile uint32_t *) (FUNNEL_BASE + 0xFB0UL))

/** 单次 ITM_Printf 的临时缓冲 */
#define ITM_PRINTF_BUF   (128u)

volatile uint32_t g_itm_dropped = 0U;

/**
 * 芯片侧自己打开 ITM 通路。
 *
 * 为什么必须自己开：主机工具（如 probe-rs 的 `itm memory`）只配置 TMC/ETF，
 * **不会**帮芯片使能 ITM_TCR.ITMENA 和 ITM_TER[0]，也不会开 TraceFunnel。
 * 不使能的话读刺激端口恒为 0，看起来就像"FIFO 永远满"，数据一个也出不去。
 *
 * 注意：ITM/Funnel 的寄存器受 LAR 保护，要先写解锁钥匙才能改。
 */
void ITM_Init (void)
{
    ITM_LAR      = CS_LAR_KEY;        /* 解锁 ITM */
    ITM_TCR      = ITM_TCR_ITMENA;    /* bit0 ITMENA = 1 */
    ITM_TER      = ITM_TER_PORT0;     /* 使能刺激端口 0 */

    FUNNEL_LAR   = CS_LAR_KEY;        /* 解锁 TraceFunnel */
    FUNNEL_CTRL |= ITM_TER_PORT0;     /* 打通 ITM -> TMC/ETF 这一路 */
}

/** 主机是否已使能 ITM 与端口 0（诊断用：为 0 说明主机没在抓） */
int ITM_IsEnabled (void)
{
    return ((0U != (ITM_TCR & ITM_TCR_ITMENA)) && (0U != (ITM_TER & ITM_TER_PORT0))) ? 1 : 0;
}

void ITM_PutChar (char c)
{
    /*
     * 曾经在这里检查 ITM_TCR.ITMENA 与 ITM_TER[0]，主机没连就丢弃。
     * 实测发现 probe-rs 的 `itm memory` **不会**帮芯片使能这两个位，
     * 于是数据全被我们自己丢掉了 —— 改成无条件写。
     * 未使能时写刺激端口是无效操作，对程序没有任何副作用。
     */
    if (0U == ITM_STIM0)
    {
        g_itm_dropped++;              /* FIFO 满: 丢弃, 不等待 */
        return;
    }

    *(volatile uint8_t *) ITM_BASE_ADDR = (uint8_t) c;
}

void ITM_PutString (const char * p_str)
{
    if (NULL == p_str)
    {
        return;
    }

    while ('\0' != *p_str)
    {
        ITM_PutChar(*p_str);
        p_str++;
    }
}

int ITM_Printf (const char * p_fmt, ...)
{
    char     buf[ITM_PRINTF_BUF];
    va_list  ap;
    int      n;

    va_start(ap, p_fmt);
    n = vsnprintf(buf, (size_t) ITM_PRINTF_BUF, p_fmt, ap);
    va_end(ap);

    if (n <= 0)
    {
        return n;
    }

    {
        uint32_t len = ((uint32_t) n < ITM_PRINTF_BUF) ? (uint32_t) n : (ITM_PRINTF_BUF - 1U);
        for (uint32_t i = 0U; i < len; i++)
        {
            ITM_PutChar(buf[i]);
        }
    }

    return n;
}
