/********************************************************************
 * SEGGER RTT 兼容的日志输出（自研实现，未使用 SEGGER 官方源码）
 *
 * 原理：在 RAM 里放一个「控制块」，主机端（probe-rs / J-Link / RTT Viewer）
 * 通过 SWD 周期读这个结构和环形缓冲，就拿到目标板的日志 —— 不占用 UART、
 * 不需要额外引脚，也不打断程序执行。
 *
 * 控制块的布局必须与 SEGGER RTT 一致，主机端才能识别。布局是公开的
 * 事实标准（主机端按固定偏移解析），本文件按该布局自行实现。
 *
 * 用法：
 *     rtt_printf("frames=%u\n", g_oled_frames);
 *   主机侧：probe-rs attach --chip R7FA6E2BB --probe 0d28:0204 build/ra6e2_fw.elf
 *********************************************************************/
#ifndef RTT_LOG_H
#define RTT_LOG_H

#include <stdint.h>
#include <stddef.h>

/** up 通道数（目标 → 主机）。1 个够用，多开会占 RAM */
#define RTT_NUM_UP_BUFFERS     (1)
/** down 通道数（主机 → 目标）。本项目不用，留 1 个占位以满足主机端解析 */
#define RTT_NUM_DOWN_BUFFERS   (1)
/** up 通道 0 的缓冲大小。主机不读时会写满并丢弃旧数据，不会卡死程序。
 *
 * 1024 B -> 4096 B（2026-10-05 改）：帧率提到 164 fps 后，程序在主机 attach 之前
 * 就会积压几百行日志，1024 B 环形缓冲回绕时会把**行截断**（实测 "off" 被截成 "of"，
 * 两行粘成 `frames=960 offrames=17280`）。4 KB 能保住启动段的完整尾部。
 * RAM 只用了 22%，代价可忽略。注意这不是根治 —— 启动头几秒的行仍会丢，
 * 要看完整启动日志请用 rtt.sh --fresh（先复位再挂）。 */
#define RTT_UP_BUF_SIZE        (4096u)

/** 通道满时的行为：2 = NO_BLOCK_SKIP（丢弃写不下的部分，绝不阻塞） */
#define RTT_MODE_NO_BLOCK_SKIP (2u)

/**
 * 环形缓冲描述子 —— 24 字节，字段顺序与 SEGGER RTT 一致。
 * WrOff/RdOff 必须 volatile：主机端会改 RdOff。
 */
typedef struct
{
    const char *      sName;         /* 通道名, 主机端显示用 */
    char *            pBuffer;       /* 缓冲区起始 */
    uint32_t          SizeOfBuffer;  /* 缓冲字节数（实际可用 -1, 用于区分满/空） */
    volatile uint32_t WrOff;         /* 目标写位置, 主机读 */
    volatile uint32_t RdOff;         /* 主机读位置, 目标写前检查 */
    uint32_t          Flags;         /* RTT_MODE_* */
} rtt_ring_t;

/** RTT 控制块。符号名必须是 _SEGGER_RTT —— 主机端按这个名字从 ELF 找地址 */
typedef struct
{
    char       acID[16];                       /* "SEGGER RTT" 识别串 */
    int32_t    MaxNumUpBuffers;
    int32_t    MaxNumDownBuffers;
    rtt_ring_t aUp[RTT_NUM_UP_BUFFERS];
    rtt_ring_t aDown[RTT_NUM_DOWN_BUFFERS];
} rtt_cb_t;

/** RTT 控制块本体。非 static —— 主机端要按符号定位 */
extern rtt_cb_t _SEGGER_RTT;

/** 写数据到 up 通道 0。返回实际写入字节数（缓冲不足时会少于 len） */
uint32_t RTT_Write (const void * p_data, uint32_t len);

/** 写字符串到 up 通道 0 */
uint32_t RTT_WriteString (const char * p_str);

/** 格式化输出，语义同 printf。受限：nano.specs 不支持 %f，单次最长 128 字节 */
int      RTT_Printf (const char * p_fmt, ...);

#endif /* RTT_LOG_H */
