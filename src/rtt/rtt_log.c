/********************************************************************
 * SEGGER RTT 兼容日志输出 —— 实现
 *
 * 关键点：
 *  - 控制块放 .bss（RAM），符号名 _SEGGER_RTT 暴露给主机端定位；
 *  - 写入为 NO_BLOCK_SKIP：主机没连/读得慢时丢弃多余数据，绝不阻塞业务；
 *  - 缓冲区留 1 字节不用，用来区分「满」和「空」（WrOff == RdOff 表示空）。
 *********************************************************************/
#include "rtt/rtt_log.h"

#include <stdarg.h>
#include <stdio.h>

/** 单次 RTT_Printf 的临时缓冲（栈上）。别打超长行 */
#define RTT_PRINTF_BUF   (128u)

static char s_up0_buf[RTT_UP_BUF_SIZE];

/* 控制块：静态初始化即可，不需要运行时 init。
 * acID[16] 未写满的部分由编译器补 0，与 SEGGER 一致。 */
rtt_cb_t _SEGGER_RTT =
{
    "SEGGER RTT",
    RTT_NUM_UP_BUFFERS,
    RTT_NUM_DOWN_BUFFERS,
    {
        {
            "Terminal",
            s_up0_buf,
            RTT_UP_BUF_SIZE,
            0u,
            0u,
            RTT_MODE_NO_BLOCK_SKIP,
        },
    },
    {
        {
            "Terminal",
            NULL,
            0u,
            0u,
            0u,
            RTT_MODE_NO_BLOCK_SKIP,
        },
    },
};

uint32_t RTT_Write (const void * p_data, uint32_t len)
{
    const char * p_src   = (const char *) p_data;
    rtt_ring_t * p_ring  = &_SEGGER_RTT.aUp[0];
    char *       p_buf   = p_ring->pBuffer;
    uint32_t     size    = p_ring->SizeOfBuffer;
    uint32_t     wr      = p_ring->WrOff;
    uint32_t     written = 0u;

    if ((NULL == p_src) || (0u == len) || (NULL == p_buf) || (size < 2u))
    {
        return 0u;
    }

    /* 逐字节推进，每字节都重新读 RdOff —— 主机随时可能取走数据腾出空间 */
    for (uint32_t i = 0u; i < len; i++)
    {
        uint32_t rd   = p_ring->RdOff;
        uint32_t next = wr + 1u;
        if (next >= size)
        {
            next = 0u;
        }

        /* 再写一字节就会追上读指针 = 满，剩下的数据丢弃（NO_BLOCK_SKIP） */
        if (next == rd)
        {
            break;
        }

        p_buf[wr] = p_src[i];
        wr        = next;
        written++;
    }

    p_ring->WrOff = wr;

    return written;
}

uint32_t RTT_WriteString (const char * p_str)
{
    uint32_t n = 0u;

    if (NULL == p_str)
    {
        return 0u;
    }

    while ('\0' != p_str[n])
    {
        n++;
    }

    return RTT_Write(p_str, n);
}

int RTT_Printf (const char * p_fmt, ...)
{
    char     buf[RTT_PRINTF_BUF];
    va_list  ap;
    int      n;

    va_start(ap, p_fmt);
    n = vsnprintf(buf, (size_t) RTT_PRINTF_BUF, p_fmt, ap);
    va_end(ap);

    if (n <= 0)
    {
        return n;
    }

    /* vsnprintf 返回的是"本想写的长度"，超长时按实际截断写入 */
    if ((uint32_t) n < RTT_PRINTF_BUF)
    {
        RTT_Write(buf, (uint32_t) n);
    }
    else
    {
        RTT_Write(buf, RTT_PRINTF_BUF - 1u);
    }

    return n;
}
