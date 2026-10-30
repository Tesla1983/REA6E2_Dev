/* ============================================================
 * 上位机 → MCU 串口链路实现（单字节中断接收 + 环形缓冲 + 帧解析）
 * 协议与设计理由见 bsp_uart_link.h
 * ============================================================ */

#include "uart/bsp_uart_link.h"

#include "rtt/rtt_log.h"
#include "trng/ra6e2_trng.h"        /* 片上真随机: $RND 指令的数据源 */
#include "usb/bsp_usb_cdc.h"        /* USB_CDC_Write: 身份应答走 Type-C 回 */

/* 固件版本魔数（在 nt7534.c 里写入, 这里只读低 8 位）。
 * 用 extern 而不是 include 那个头: 保持 uart 层不依赖 nt7534, 且版本只有一处真值。 */
extern volatile uint32_t g_nt_fw_tag;

/* 1 ms 系统心跳（SysTick_Handler 递增），用于流式限速的间隔门控。 */
extern volatile uint32_t g_ms_tick;

/* ---------------- 运行统计 ---------------- */

volatile uint32_t g_link_frames    = 0u;
volatile uint32_t g_link_rx_bytes  = 0u;
volatile uint32_t g_link_err_xor   = 0u;
volatile uint32_t g_link_err_fmt   = 0u;
volatile uint8_t  g_link_last[40]  = { 0u };
volatile int32_t  g_link_tx_err   = 0;   /* 最近一次 beacon write 的返回码（0=成功）*/
volatile uint32_t g_link_tx_cnt   = 0u;  /* beacon 已发起次数 */

/** 屏显示语言：0 = 英文（linked / unlink），1 = 中文（已连接 / 未连接）。
 *  **默认 1（中文）**，与上位机默认语言一致（2026-10-28 用户确定）。
 *  ⚠ 不能放进 s_data：link_data_clear() 在每帧解析前和断链时都会清 s_data，
 *    语言若在其中会被清掉，断链后状态行会退回英文。这里只在下发 LG 时更新。 */
volatile uint8_t g_screen_lang = 1u;

/* ---------------- 环形缓冲（单生产者 / 单消费者）---------------- */
/* 生产者 = 串口中断（只写 s_head），消费者 = 主循环（只写 s_tail）。
 * 256 字节: 一帧最长 60 字节左右, 够缓冲 4 帧, 主循环 250 ms 必被调一次。 */

#define RING_SIZE   (256u)

static volatile uint8_t  s_ring[RING_SIZE];
static volatile uint16_t s_head = 0u;
static volatile uint16_t s_tail = 0u;

/** 单字节接收的目标地址。必须是 static（不能放栈上）: 中断/DMAC 会持续写它。
 *  不加 volatile —— FSP 的 read() 形参是 uint8_t *, 加了只会产生限定符丢失警告。 */
static uint8_t s_rx_byte = 0u;

/* ---------------- 行缓冲与解析状态 ---------------- */

#define LINE_MAX    (64u)

static uint8_t  s_line[LINE_MAX];
static uint16_t s_len = 0u;
static bool     s_in_frame = false;

static uart_link_data_t s_data;

/** 接收超时（毫秒）。收到合法帧时重置为 2000, 每 Tick 递减。 */
static volatile uint16_t s_alive_ms = 0u;

/** 把全部字段置 -1（= 无数据）。
 *  三处用：① 上电初始化；② 每帧解析前先清，未上报的键自然留 -1；③ **断链时清一次**。
 *  断链必须清：否则屏上保留上一次的读数、风扇按旧 RPM 继续转，
 *  看起来像"还在实时刷新"，与 unlink 状态自相矛盾（2026-10-26 用户反馈）。 */
static void link_data_clear(void)
{
    s_data.ct = -1;
    s_data.cu = -1;
    s_data.cf = -1;
    s_data.gt = -1;
    s_data.gu = -1;
    s_data.gf = -1;
    s_data.mu = -1;
}

/* ---------------- 小工具 ---------------- */

/** 十六进制字符 → 数值；非法返回 -1 */
static int16_t hexval(uint8_t c)
{
    if ((c >= (uint8_t) '0') && (c <= (uint8_t) '9'))
    {
        return (int16_t) (c - (uint8_t) '0');
    }
    if ((c >= (uint8_t) 'A') && (c <= (uint8_t) 'F'))
    {
        return (int16_t) (c - (uint8_t) 'A' + 10);
    }
    if ((c >= (uint8_t) 'a') && (c <= (uint8_t) 'f'))
    {
        return (int16_t) (c - (uint8_t) 'a' + 10);
    }
    return -1;
}

/** 解析十进制整数（可带负号）。返回值有效长度, 0 表示没有数字。 */
static uint16_t parse_int(const uint8_t * p, uint16_t n, int16_t * p_out)
{
    uint16_t i = 0u;
    bool     neg = false;
    int32_t  v = 0;

    if ((i < n) && ((uint8_t) '-' == p[i]))
    {
        neg = true;
        i++;
    }

    if ((i >= n) || (p[i] < (uint8_t) '0') || (p[i] > (uint8_t) '9'))
    {
        return 0u;
    }

    while ((i < n) && (p[i] >= (uint8_t) '0') && (p[i] <= (uint8_t) '9'))
    {
        v = (v * 10) + (int32_t) (p[i] - (uint8_t) '0');
        if (v > 30000)
        {
            v = 30000;                 /* 限幅: 屏幕上最多 4 位, 溢出没有意义 */
        }
        i++;
    }

    *p_out = (int16_t) (neg ? -v : v);

    return i;
}

/** 把一段 "KEY:VALUE" 写进对应的字段。key 必须是 2 个大写字母。 */
static void store_field(const uint8_t * p_key, const uint8_t * p_val, uint16_t vlen)
{
    int16_t  v = 0;
    uint16_t used = parse_int(p_val, vlen, &v);

    if (0u == used)
    {
        return;                        /* 值不合法 -> 当作缺项, 保持 -1 */
    }

    if ((uint8_t) 'C' == p_key[0])
    {
        if ((uint8_t) 'T' == p_key[1]) { s_data.ct = v; return; }
        if ((uint8_t) 'U' == p_key[1]) { s_data.cu = v; return; }
        if ((uint8_t) 'F' == p_key[1]) { s_data.cf = v; return; }
    }
    else if ((uint8_t) 'G' == p_key[0])
    {
        if ((uint8_t) 'T' == p_key[1]) { s_data.gt = v; return; }
        if ((uint8_t) 'U' == p_key[1]) { s_data.gu = v; return; }
        if ((uint8_t) 'F' == p_key[1]) { s_data.gf = v; return; }
    }
    else if ((uint8_t) 'M' == p_key[0])
    {
        if ((uint8_t) 'U' == p_key[1]) { s_data.mu = v; return; }
    }
    else if ((uint8_t) 'L' == p_key[0])
    {
        /* LG: 屏显示语言（1=中文 / 0=英文），写独立全局变量, 不进 s_data。 */
        if ((uint8_t) 'G' == p_key[1]) { g_screen_lang = (0 != v) ? 1u : 0u; return; }
    }

    /* 未定义的 key: 忽略（上位机将来加字段不会让本固件出错） */
}

/** 复制一段到 g_link_last（诊断用, 最长 39 字节, 保证可读） */
static void remember_bad_line(void)
{
    uint16_t i;
    uint16_t n = (s_len < 39u) ? s_len : 39u;

    for (i = 0u; i < n; i++)
    {
        uint8_t c = s_line[i];

        /* 不可见字符在探针里没法看, 换成 '.' */
        g_link_last[i] = ((c >= 0x20u) && (c <= 0x7Eu)) ? c : (uint8_t) '.';
    }
    g_link_last[n] = 0u;
}

/* ============================================================
 * 板子身份应答（`$ID?`）—— 给上位机"按 ID 认板子"用
 *
 * 为什么需要:
 *   COM 号每次枚举/插拔都会变（本项目实测从 COM4 变到 COM15）；
 *   USB 描述符里的 iSerialNumber 又写死成 "0000000000001"（所有板一样, 不唯一）。
 *   ⇒ 只有芯片出厂 UID 是稳的。上位机枚举所有候选口, 逐个发 `$ID?`,
 *     收到本应答的才是目标板。
 *
 * UID 位置: 0x01008190（RA6E2 出厂唯一 ID, 16 字节）。
 *   取首尾两个字合成 64 bit, 对现场几块板足够唯一, 也够短。
 *   本板实测: 43045a76 35333832 612a4553 4e4b2eb4
 *             ↑w0                          ↑w3
 *
 * 应答格式（与数据帧同一套 `$...*XX\r\n` 框架）:
 *   $ID:RA6E2-43045A764E4B2EB4,FW:14*XX\r\n
 * ============================================================ */

#define LINK_UID_ADDR       (0x01008190UL)

/** 8 位大写十六进制（不带前缀）。返回写入长度。 */
static uint16_t put_hex32(char * p, uint32_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    uint16_t i;

    for (i = 0u; i < 8u; i++)
    {
        p[i] = hx[(v >> ((7u - i) * 4u)) & 0x0Fu];
    }
    return 8u;
}

/** 十进制（不带前导 0）。返回写入长度。 */
static uint16_t put_dec32(char * p, uint32_t v)
{
    char     tmp[10];
    uint16_t d = 0u;
    uint16_t i;

    do
    {
        tmp[d++] = (char) ('0' + (v % 10u));
        v /= 10u;
    } while ((0u != v) && (d < 10u));

    for (i = 0u; i < d; i++)
    {
        p[i] = tmp[d - 1u - i];
    }
    return d;
}

/** 回一帧身份信息。优先走 USB CDC, 失败再走 SCI0。 */
static void link_send_identity(void)
{
    static char s_frame_usb[56];        /* 必须 static: FSP 的 write 是异步的 */
    static uint8_t s_frame_uart[56];    /* SCI0 的 write 形参是 uint8_t const * */
    uint32_t    uid_w0 = *(volatile uint32_t *) LINK_UID_ADDR;
    uint32_t    uid_w3 = *(volatile uint32_t *) (LINK_UID_ADDR + 12u);
    const char *pfx    = "ID:RA6E2-";
    uint16_t    n      = 0u;
    uint16_t    i;
    uint8_t     x      = 0u;
    uint8_t     hi;
    uint8_t     lo;

    /* ---- 组帧: '$' + payload + '*' + XX + CRLF ---- */
    s_frame_usb[n++] = '$';

    for (i = 0u; '\0' != pfx[i]; i++)
    {
        s_frame_usb[n++] = pfx[i];
    }
    n = (uint16_t) (n + put_hex32(&s_frame_usb[n], uid_w0));
    n = (uint16_t) (n + put_hex32(&s_frame_usb[n], uid_w3));

    s_frame_usb[n++] = ',';
    s_frame_usb[n++] = 'F';
    s_frame_usb[n++] = 'W';
    s_frame_usb[n++] = ':';
    n = (uint16_t) (n + put_dec32(&s_frame_usb[n], g_nt_fw_tag & 0xFFu));

    /* 校验 = '$' 之后到 '*' 之前所有字节的 XOR（与数据帧一致, 用 s_line 那套不算） */
    for (i = 1u; i < n; i++)
    {
        x ^= (uint8_t) s_frame_usb[i];
    }

    s_frame_usb[n++] = '*';
    hi = (uint8_t) ((x >> 4) & 0x0Fu);
    lo = (uint8_t) (x & 0x0Fu);
    s_frame_usb[n++] = (char) ((hi < 10u) ? ('0' + (int) hi) : ('A' + (int) hi - 10));
    s_frame_usb[n++] = (char) ((lo < 10u) ? ('0' + (int) lo) : ('A' + (int) lo - 10));
    s_frame_usb[n++] = '\r';
    s_frame_usb[n++] = '\n';

    /* ---- 发送: 先用 Type-C（上位机主通路）, 失败再退到 SCI0 ---- */
    if (FSP_SUCCESS != USB_CDC_Write((uint8_t const *) s_frame_usb, n))
    {
        for (i = 0u; i < n; i++)
        {
            s_frame_uart[i] = (uint8_t) s_frame_usb[i];
        }
        (void) g_uart0.p_api->write(&g_uart0_ctrl, s_frame_uart, n);
    }

    g_link_tx_cnt++;
}

/* ============================================================
 * 真随机数应答（`$RND,<n>`）—— 板子作为硬件 TRNG 源
 *
 * 设计: 板子只负责"按请求返回 n 字节真随机", 不解释随机数的语义(轮/线/赔付都由上位机定)。
 * 老虎机/任意需要真随机的上位机应用都走这个指令, 与现有性能监视系统的数据流完全解耦。
 *
 * 请求:  `$RND,<n>*XX`     (n = 十进制字节数, 1..RND_MAX_BYTES)
 * 应答:  `$RND:<2n hex>*XX` (成功, 每字节两位大写十六进制)
 *        `$RND:E*XX`        (TRNG 失败, 让上位机知道本次不可用、自行回落)
 * ============================================================ */

#define RND_MAX_BYTES   (64u)   /* 单次最多取 64 字节: 防滥用 + 缓冲上限 */

/** 回 n 字节真随机（hex 编码）。优先 USB CDC, 失败退 SCI0, 与 link_send_identity 同构。 */
static void link_send_random(uint16_t n)
{
    /* 必须 static: FSP 的 write 是异步的, 缓冲不能在栈上 */
    static char    s_frame_usb[8u + (RND_MAX_BYTES * 2u) + 8u];
    static uint8_t s_frame_uart[8u + (RND_MAX_BYTES * 2u) + 8u];
    static uint8_t s_rnd[RND_MAX_BYTES];
    uint8_t  x = 0u;
    uint16_t k = 0u;
    uint16_t j;
    bool     ok = trng_bytes(s_rnd, (size_t) n);

    s_frame_usb[k++] = '$';
    s_frame_usb[k++] = 'R'; s_frame_usb[k++] = 'N'; s_frame_usb[k++] = 'D';
    s_frame_usb[k++] = ':';

    if (ok)
    {
        for (j = 0u; j < n; j++)
        {
            uint8_t b = s_rnd[j];
            uint8_t h = (uint8_t) ((b >> 4) & 0x0Fu);
            uint8_t l = (uint8_t) (b & 0x0Fu);
            s_frame_usb[k++] = (char) ((h < 10u) ? ('0' + h) : ('A' + h - 10));
            s_frame_usb[k++] = (char) ((l < 10u) ? ('0' + l) : ('A' + l - 10));
        }
    }
    else
    {
        s_frame_usb[k++] = 'E';                   /* TRNG 失败标记 */
    }

    /* XOR 覆盖 '$' 之后到 '*' 之前（与数据帧一致） */
    for (j = 1u; j < k; j++)
    {
        x ^= (uint8_t) s_frame_usb[j];
    }
    s_frame_usb[k++] = '*';
    {
        uint8_t hi = (uint8_t) ((x >> 4) & 0x0Fu);
        uint8_t lo = (uint8_t) (x & 0x0Fu);
        s_frame_usb[k++] = (char) ((hi < 10u) ? ('0' + hi) : ('A' + hi - 10));
        s_frame_usb[k++] = (char) ((lo < 10u) ? ('0' + lo) : ('A' + lo - 10));
    }
    s_frame_usb[k++] = '\r';
    s_frame_usb[k++] = '\n';

    if (FSP_SUCCESS != USB_CDC_Write((uint8_t const *) s_frame_usb, k))
    {
        for (j = 0u; j < k; j++)
        {
            s_frame_uart[j] = (uint8_t) s_frame_usb[j];
        }
        (void) g_uart0.p_api->write(&g_uart0_ctrl, s_frame_uart, k);
    }
    g_link_tx_cnt++;
}

/* ============================================================
 * 流式真随机输出（`$RNDS,<n>`）—— 高吞吐模式, 用于分布分析/大批量采样
 *
 * 与 $RND,<n> 查询-响应不同: 开启后板子直接往 USB CDC 吐裸随机字节流(无 $ 帧结构),
 * 直到发够 n 字节(n>0 有限流) 或收到 $RNDX(n=0 无限流时由上位机停)。
 * 上位机发 $RNDS 后立刻切读模式收集; 板子不回任何帧(否则会污染字节流)。
 * 每笔经 USB_CDC_Write 异步发送, 必须等上一笔完成(USB_CDC_TxReady)再写下一批,
 * 否则覆盖 s_tx_stage 缓冲。主循环 2ms 细片每轮调一次 UART_Link_Stream_Pump()。
 * ============================================================ */
static bool     s_stream_active = false;   /* 是否处于流式输出 */
static uint32_t s_stream_rem    = 0u;     /* 剩余待发字节(0 = 无限流) */
static uint32_t s_stream_sent   = 0u;     /* 已发字节(调试/自检) */
static uint32_t s_last_send_ms  = 0u;     /* 上一笔发出的时刻(g_ms_tick)，限速门控用 */

/* 每笔字节数。⚠ 必须是 64(FS 批量端点 wMaxPacketSize)的**非整数倍**:
 * FSP 的 R_USB_Write(256) 会发出 4×64 满包, 传输没有短包收尾; Windows USBD 把 IN 流按 64KB
 * 窗口聚合, 一个没有短包/ ZLP 收尾的 64KB 窗口无法判定"本窗结束", 于是连续发满 3 窗
 * (=196608 字节)后主机传输窗口卡死、不再投递(实测稳停在 196608/200000)。
 * 改成 255(=3×64+63)后每笔都以 63 字节短包收尾, USBD 每笔都能干净结束, 卡死消失。
 * 仍 ≤ USB_CDC_TX_STAGE(256), 缓冲安全。 */
#define STREAM_CHUNK   (255u)

/* 流式限速（无流控 CDC 必需）：两次发送之间至少间隔的毫秒数。
 * 依据：主机 pyserial 实测稳定接收约 37 KB/s；板子若不限速会以 ~100 KB/s 狂发，
 * 淹没主机 RX 缓冲造成丢字节（实测 200000 字节只收到 196608 且尾部停滞）。
 * 这里压到 ~16 KB/s（256B/16ms）留足余量；可用探针改 g_stream_interval_ms 现场调。 */
volatile uint32_t g_stream_interval_ms = 16u;

void UART_Link_Stream_Pump(void)
{
    static uint8_t s_chunk[STREAM_CHUNK];
    uint32_t       chunk;

    if (!s_stream_active)
    {
        return;
    }
    if (!USB_CDC_Configured())
    {
        s_stream_active = false;          /* 断链: 流式无意义 */
        return;
    }
    if (!USB_CDC_TxReady())
    {
        return;                          /* 上一笔还在传, 等下轮 */
    }

    /* 限速门控：无流控的 CDC 设备发太快会淹没主机接收缓冲导致丢字节。
     * 用 1ms 心跳做间隔判断，把速率压到主机能稳稳消化的水平。 */
    if ((g_ms_tick - s_last_send_ms) < g_stream_interval_ms)
    {
        return;
    }

    chunk = STREAM_CHUNK;
    if ((0u != s_stream_rem) && ((s_stream_rem - s_stream_sent) < chunk))
    {
        chunk = s_stream_rem - s_stream_sent;   /* 最后一笔不足 256 时只发余量 */
    }
    if (0u == chunk)
    {
        s_stream_active = false;
        return;
    }

    if (trng_bytes(s_chunk, (size_t) chunk))
    {
        if (FSP_SUCCESS == USB_CDC_Write(s_chunk, chunk))
        {
            s_stream_sent += chunk;
            s_last_send_ms = g_ms_tick;        /* 记下时刻, 下一笔受限速门控约束 */
            if ((0u != s_stream_rem) && (s_stream_sent >= s_stream_rem))
            {
                s_stream_active = false;        /* 有限流达成 */
            }
        }
        /* 写失败(busy/未枚举): 不推进 sent, 下轮重试(重新生成, 都是真随机) */
    }
    /* TRNG 失败: 跳过本轮, 不推进 sent */
}

/** payload 是否正好是 "ID?"（长度 = star-1）。 */
static bool is_id_query(uint16_t star)
{
    return (3u == (uint16_t) (star - 1u)) &&
           ((uint8_t) 'I' == s_line[1]) &&
           ((uint8_t) 'D' == s_line[2]) &&
           ((uint8_t) '?' == s_line[3]);
}

/** 解析一行（s_line[0..s_len), 不含 '\n'）/ 成功返回 true */
static bool parse_line(void)
{
    uint16_t star;
    uint16_t i;
    uint8_t  x = 0u;
    int16_t  hi;
    int16_t  lo;
    int16_t  chk;

    /* 去掉尾部 \r */
    while ((s_len > 0u) && ((uint8_t) '\r' == s_line[s_len - 1u]))
    {
        s_len--;
    }

    if ((s_len < 5u) || ((uint8_t) '$' != s_line[0]))
    {
        return false;
    }

    /* 从尾部找 '*'（数据里不会出现 '*'） */
    for (star = s_len; star > 1u; star--)
    {
        if ((uint8_t) '*' == s_line[star - 1u])
        {
            break;
        }
    }
    if (star <= 1u)
    {
        return false;
    }
    star--;                            /* star = '*' 的下标 */

    if ((star + 2u) >= s_len)
    {
        return false;
    }

    hi = hexval(s_line[star + 1u]);
    lo = hexval(s_line[star + 2u]);
    if ((hi < 0) || (lo < 0))
    {
        return false;
    }
    chk = (int16_t) ((hi << 4) | lo);

    /* XOR 覆盖 '$' 之后到 '*' 之前 */
    for (i = 1u; i < star; i++)
    {
        x ^= s_line[i];
    }
    if ((uint8_t) chk != x)
    {
        g_link_err_xor++;
        return false;
    }

    /* 身份查询: `$ID?*XX` —— 不是数据帧, 回一帧 ID 就返回。
     * 上位机靠它认板子（不看 COM 号）, 详见 link_send_identity() 的说明。 */
    if (is_id_query(star))
    {
        link_send_identity();
        return true;
    }

    /* 真随机数请求: `$RND,<n>*XX` —— 板子作硬件 TRNG 源, 回 n 字节随机(hex)。
     * 与性能监视数据流无关; 上位机(老虎机等)按需取真随机, 板子不解释语义。 */
    if ((star >= 6u) &&
        ('R' == s_line[1]) && ('N' == s_line[2]) && ('D' == s_line[3]) && (',' == s_line[4]))
    {
        uint16_t n = 0u;
        bool     ok = false;

        for (i = 5u; i < star; i++)
        {
            uint8_t c = s_line[i];
            if (('0' <= c) && (c <= '9'))
            {
                n = (uint16_t) (n * 10u + (uint16_t) (c - '0'));
                ok = true;
            }
            else
            {
                break;
            }
        }
        if (ok && (n > 0u) && (n <= RND_MAX_BYTES))
        {
            link_send_random(n);
        }
        return true;
    }

    /* 流式真随机: `$RNDS,<n>*XX` 开启(n=0 无限流; n>0 有限流 n 字节); `$RNDX*XX` 停止。
     * 开启后板子直接吐裸随机字节流, 不回帧。 */
    if ((star >= 5u) &&
        ('R' == s_line[1]) && ('N' == s_line[2]) && ('D' == s_line[3]))
    {
        if (('X' == s_line[4]) && (star == 5u))          /* $RNDX: 停止 */
        {
            s_stream_active = false;
            return true;
        }
        if (('S' == s_line[4]) && (star >= 7u) && (',' == s_line[5]))   /* $RNDS,<n> */
        {
            uint32_t n = 0u;
            bool     ok2 = false;
            uint16_t k;
            for (k = 6u; k < star; k++)
            {
                uint8_t c = s_line[k];
                if (('0' <= c) && (c <= '9'))
                {
                    if (n <= (0xFFFFFFFFu / 10u))
                    {
                        n = n * 10u + (uint32_t) (c - '0');
                        ok2 = true;
                    }
                }
                else
                {
                    break;
                }
            }
            if (ok2)
            {
                s_stream_rem    = n;     /* 0 = 无限流 */
                s_stream_sent   = 0u;
                s_last_send_ms  = 0u;   /* 让首笔立即发出, 不受限速门控延迟 */
                s_stream_active = true;
            }
            return true;
        }
    }

    /* 逐段解析 KEY:VALUE, 段之间用 ',' 分隔 */
    link_data_clear();

    i = 1u;
    while (i < star)
    {
        uint16_t seg_end = i;

        while ((seg_end < star) && ((uint8_t) ',' != s_line[seg_end]))
        {
            seg_end++;
        }

        /* 段形如 "CT:48", key 恰好 2 字符 + ':' */
        if ((seg_end > (i + 3u)) && ((uint8_t) ':' == s_line[i + 2u]))
        {
            store_field(&s_line[i], &s_line[i + 3u], (uint16_t) (seg_end - (i + 3u)));
        }

        i = (uint16_t) (seg_end + 1u);
    }

    g_link_frames++;
    s_alive_ms = 2000u;

    return true;
}

/* ---------------- 对外接口 ---------------- */

/** 把全部字段置 -1（= 无数据）。 */
fsp_err_t UART_Link_Init(void)
{
    fsp_err_t err;

    link_data_clear();

    err = g_uart0.p_api->open(&g_uart0_ctrl, &g_uart0_cfg);
    if (FSP_SUCCESS != err)
    {
        RTT_Printf("uart link open failed: %d\n", (int) err);
        return err;
    }

    /* 先挂上第一个字节的接收, 之后每个 RX 完成中断里再挂下一个 */
    err = g_uart0.p_api->read(&g_uart0_ctrl, &s_rx_byte, 1u);
    RTT_Printf("uart link ready (115200 8N1), err=%d\n", (int) err);

    return err;
}

void UART_Link_PushBytes(uint8_t const * p_src, uint32_t n)
{
    uint32_t i;

    if (NULL == p_src)
    {
        return;
    }

    for (i = 0u; i < n; i++)
    {
        uint16_t next = (uint16_t) ((s_head + 1u) % RING_SIZE);

        if (next == s_tail)
        {
            break;                              /* 满了就丢, 不覆盖未读数据 */
        }
        s_ring[s_head] = p_src[i];
        s_head = next;
        g_link_rx_bytes++;
    }
}

void UART_Link_OnIsr(uart_callback_args_t * p_args)
{
    uint16_t next;

    switch (p_args->event)
    {
        case UART_EVENT_RX_COMPLETE:
        case UART_EVENT_RX_CHAR:
        {
            next = (uint16_t) ((s_head + 1u) % RING_SIZE);
            if (next != s_tail)                     /* 满了就丢, 不覆盖未读数据 */
            {
                s_ring[s_head] = (uint8_t) p_args->data;
                s_head = next;
            }
            g_link_rx_bytes++;
            break;
        }
        default:
        {
            /* 溢出/帧错/校验错: 什么都不做, 下面重新挂接收即可（单字节模式不丢正在收的字节） */
            break;
        }
    }

    if ((UART_EVENT_RX_COMPLETE == p_args->event) ||
        (UART_EVENT_RX_CHAR     == p_args->event) ||
        (UART_EVENT_ERR_OVERFLOW == p_args->event) ||
        (UART_EVENT_ERR_FRAMING  == p_args->event) ||
        (UART_EVENT_ERR_PARITY   == p_args->event))
    {
        (void) g_uart0.p_api->read(&g_uart0_ctrl, &s_rx_byte, 1u);
    }
}

void UART_Link_Poll(void)
{
    static bool logged = false;

    while (s_tail != s_head)
    {
        uint8_t b = s_ring[s_tail];

        s_tail = (uint16_t) ((s_tail + 1u) % RING_SIZE);

        if ((uint8_t) '\n' == b)
        {
            if (s_in_frame)
            {
                if (!parse_line())
                {
                    /* 只记录最前面的几次, 免得 RTT 被刷屏 */
                    if (g_link_err_fmt < 3u)
                    {
                        remember_bad_line();
                        RTT_Printf("bad line: %s\n", (const char *) g_link_last);
                        g_link_err_fmt++;
                    }
                }
            }
            s_len = 0u;
            s_in_frame = false;
            continue;
        }

        if (!s_in_frame)
        {
            if ((uint8_t) '$' == b)             /* 帧头之前的一切都丢掉 */
            {
                s_in_frame = true;
                s_len = 0u;
                s_line[s_len++] = b;
            }
            continue;
        }

        if (s_len >= LINE_MAX)
        {
            g_link_err_fmt++;
            s_len = 0u;
            s_in_frame = false;
            continue;
        }

        s_line[s_len++] = b;
    }

    if (!logged && (g_link_frames > 0u) && (s_data.ct >= 0))
    {
        logged = true;
        RTT_Printf("first frame ok: CT=%d CU=%d CF=%d GT=%d GU=%d GF=%d MU=%d\n",
                   (int) s_data.ct, (int) s_data.cu, (int) s_data.cf,
                   (int) s_data.gt, (int) s_data.gu, (int) s_data.gf,
                   (int) s_data.mu);
    }
}

void UART_Link_Tick(uint16_t elapsed_ms)
{
    if (s_alive_ms > elapsed_ms)
    {
        s_alive_ms = (uint16_t) (s_alive_ms - elapsed_ms);
    }
    else
    {
        /* 走完 2 s 没收到合法帧 ⇒ 判定断链。
         * 只在"从在线变离线"的那一次清数据，之后每 50 ms 调一次也不会反复清。 */
        if (0u != s_alive_ms)
        {
            s_alive_ms = 0u;
            link_data_clear();   /* 读数回 --，风扇门控失效 ⇒ 停转（仍照常画静止风扇） */
        }
    }
}

bool UART_Link_Online(void)
{
    return (s_alive_ms > 0u);
}

const uart_link_data_t * UART_Link_Data(void)
{
    return &s_data;
}

void UART_Link_Beacon(void)
{
    static uint32_t seq = 0u;
    static uint8_t  buf[20];   /* 必须 static: FSP 的 write 是异步的, 传完之前指针不能失效 */
    uint8_t         n = 0u;
    uint32_t        v;

    buf[n++] = (uint8_t) 'B';
    buf[n++] = (uint8_t) 'E';
    buf[n++] = (uint8_t) 'A';
    buf[n++] = (uint8_t) 'C';
    buf[n++] = (uint8_t) 'O';
    buf[n++] = (uint8_t) 'N';
    buf[n++] = (uint8_t) ' ';

    seq++;

    /* 十进制序号, 从最高位往下写（不用 snprintf, 免得带进来一堆格式化代码） */
    v = seq % 100000u;
    if (0u == v)
    {
        buf[n++] = (uint8_t) '0';
    }
    else
    {
        uint8_t digits[6];
        uint8_t d = 0u;

        while ((v > 0u) && (d < 6u))
        {
            digits[d++] = (uint8_t) ('0' + (v % 10u));
            v /= 10u;
        }
        while (d > 0u)
        {
            buf[n++] = digits[--d];
        }
    }

    buf[n++] = (uint8_t) '\r';
    buf[n++] = (uint8_t) '\n';

    g_link_tx_err = (int32_t) g_uart0.p_api->write(&g_uart0_ctrl, buf, (uint32_t) n);
    g_link_tx_cnt++;
}
