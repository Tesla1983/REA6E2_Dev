/**
 * @file  bsp_usb_cdc.c
 * @brief USB CDC 虚拟串口实现（裸机, BSP_CFG_RTOS = 0）
 *
 * 数据流:
 *   PC --(OUT 端点)--> USB 中断 --> usb_callback(USB_STATUS_READ_COMPLETE)
 *        --> 拷进串口链路的环形缓冲（UART_Link_PushBytes）--> 主循环 UART_Link_Poll() 解析
 *   PC <--(IN 端点)--- R_USB_Write
 *
 * 为什么不自己再写一份帧解析: 屏上那 7 个字段的解析已经在 src/uart/bsp_uart_link.c 里,
 * USB 只换"字节从哪儿来", 解析/校验/超时判掉线全部复用, 免得两份逻辑走偏。
 */

#include "usb/bsp_usb_cdc.h"

#include "r_usb_basic.h"
#include "r_usb_pcdc_api.h"

#include "rtt/rtt_log.h"
#include "uart/bsp_uart_link.h"

/* 单次接收请求的长度。64 = FS 下 CDC 数据端点的最大包长, 一包一次中断, 不攒不长等。 */
#define USB_CDC_RX_CHUNK    (64u)

/* 发送缓冲: FSP 的 R_USB_Write 是异步的（回调 USB_STATUS_WRITE_COMPLETE 才算完）,
 * 传进去的指针在完成前不能失效 ⇒ 用 static 的暂存区, 不用调用者的栈。 */
#define USB_CDC_TX_STAGE    (256u)

/* SETUP 死锁解锁开关（见 USB_CDC_ProbeUpdate 里的说明）。0 = 关闭（已找到真因后不需要）。 */
#ifndef USB_CDC_SETUP_KICK
#define USB_CDC_SETUP_KICK  (0)
#endif

/* FSP 设备栈内部量/函数（原型见 r_usb_basic/src/driver/inc/r_usb_extern.h）。 */
extern uint16_t g_usb_pstd_req_type;
extern void     usb_pstd_save_request(uint8_t usb_ip);
extern uint16_t g_usb_pstd_pipe0_request;
extern uint32_t g_usb_pstd_data_cnt[];

/* ⚠⚠ 裸机下 USB 事件派发入口（见下方 USB_CDC_ProbeUpdate 的详细说明）。
 * r_usb_extern.h 里对 BSP_CFG_RTOS==0 的声明是 `void usb_pstd_pcd_task(void);` */
extern void     usb_pstd_pcd_task(void);

/** SETUP 解锁被触发的次数。 */
volatile uint32_t g_usb_setup_kick = 0u;

/** 类请求诊断：收到 USB_STATUS_REQUEST 的次数 / 收到 REQUEST_COMPLETE 的次数 / 最后一次请求。 */
volatile uint32_t g_usb_req_cnt      = 0u;
volatile uint32_t g_usb_req_done_cnt = 0u;
volatile uint32_t g_usb_req_last     = 0u;

/* ---- 2026-10-25 新增：类请求环形日志 ----
 *
 * 背景（实测，见 scripts/measure_com_delay.ps1 的三口径计时）：
 *   PnP 建好 COM17 devnode 只要 **0.34 s**，状态全程 OK/CM_PROB_NONE；
 *   但 COM 口登记进 SERIALCOMM 要 **29.4 s**，且首次能打开也是 28.9 s
 *   ⇒ 不是"能打开但扫描不到"，端口是真的 29 s 才可用。
 *   对照组 CH340(VID_1A86) 同样拔插：SERIALCOMM **0.37 s** 就好
 *   ⇒ 不是 Windows 通用行为，是**本设备 + usbser** 特有。
 *   29 s 这个数字与本项目 v15 的 30.00 s 超时同量级，怀疑仍是某个控制请求没收尾。
 *
 * 用法：烧录后拔插，等 ~35 s（端口已可用），再用 probe-rs 读这些全局量
 *      （符号地址每次重编译都会变，读前必须重新 nm），看最后一条是什么请求。
 */
#define USB_REQ_LOG_N   16u
volatile uint32_t g_usb_req_log_head   = 0u;                    /**< 下一条要写的位置 */
volatile uint8_t  g_usb_req_log_bmrt   [USB_REQ_LOG_N];         /**< bmRequestType */
volatile uint8_t  g_usb_req_log_breq   [USB_REQ_LOG_N];         /**< bRequest */
volatile uint16_t g_usb_req_log_value  [USB_REQ_LOG_N];         /**< wValue */
volatile uint16_t g_usb_req_log_index  [USB_REQ_LOG_N];         /**< wIndex */
volatile uint16_t g_usb_req_log_length [USB_REQ_LOG_N];         /**< wLength（关键：>0 就有数据阶段） */
volatile uint32_t g_usb_req_log_seq    [USB_REQ_LOG_N];         /**< 当时的 g_usb_req_cnt */
volatile uint32_t g_usb_req_nodata_cnt = 0u;                    /**< 走“无数据”收尾的次数 */
volatile uint32_t g_usb_req_data_cnt   = 0u;                    /**< 走了数据阶段的次数（本轮新增） */

/* ---- v15 新增：控制传输"阶段"诊断（用来判定状态阶段到底完没完） ----
 *
 * USBFS 的 INTSTS0.CTRT[15:13] 是**控制传输阶段**状态机：
 *   000 = Idle（默认态，说明一次控制传输已经彻底结束）
 *   001 = Setup stage
 *   010 = Data stage (IN)   011 = Data stage (OUT)
 *   100 = Status stage (IN) 101 = Status stage (OUT)
 *
 * 主循环每 2 ms 采一次。判据：
 *   - 正常设备：一次 open/close 之后 `g_usb_ctrt_nonidle_cnt` 几乎不涨
 *     （状态阶段在硬件里几十 µs 就完成，2 ms 的采样根本抓不到）。
 *   - 卡住 30 s 的设备：会看到 CTRT 长时间停在 100/101 ⇒ 状态阶段真的没完成。
 *   - 如果 CTRT 一直是 000 而主机仍卡 30 s ⇒ **设备侧无辜**，30 s 在主机侧
 *     等别的东西（例如中断 IN 通知），需要换方向查。
 */
volatile uint32_t g_usb_ctrt_last        = 0u;   /**< 最近一次采样到的 CTRT[2:0] */
volatile uint32_t g_usb_ctrt_nonidle_cnt = 0u;   /**< 采样到"非 Idle"的次数（累计） */
volatile uint32_t g_usb_brdysts_at_req   = 0u;   /**< 类请求到来时 pipe0 的陈旧 BRDYSTS */
volatile uint32_t g_usb_bempsts_at_req   = 0u;   /**< 类请求到来时 pipe0 的陈旧 BEMPSTS */
volatile uint32_t g_usb_dcptr_after     = 0u;   /**< v16: 打完 PID=BUF 之后的 DCPCTR */


/** R_USB_PeriControlDataGet / StatusSet 的返回码与调用后的现场（排障用）。 */
volatile uint32_t g_usb_get_err      = 0xFFFFFFFFu;
volatile uint32_t g_usb_stat_err     = 0xFFFFFFFFu;
volatile uint32_t g_usb_cfifo_after  = 0u;   /**< 调用后 CFIFOCTR  @0x4009002A */
volatile uint32_t g_usb_pipe0_after  = 0u;   /**< 调用后 g_usb_pstd_pipe0_request */
volatile uint32_t g_usb_dcnt_after   = 0u;   /**< 调用后 g_usb_pstd_data_cnt[pipe0] */

/* ============================================================
 * CDC SERIAL_STATE 通知 —— 消除 Windows usbser 打开/关闭端口各卡 30 s 的问题。
 *
 * 实测（2026-10-23，Win32 API 级拆分计时，host/usb_cdc_timing.py）：
 *   CreateFileW        0.00 s
 *   SetCommState       0.02 s
 *   EscapeCommFunction(SETDTR)   **30.01 s**
 *   EscapeCommFunction(SETRTS)    0.00 s
 *   EscapeCommFunction(CLRDTR)   **30.01 s**
 *   EscapeCommFunction(CLRRTS)    0.00 s
 *   CloseHandle                 **30.00 s**（关端口时要把 DTR 拉低，同上）
 * ⇒ 卡点**只在 DTR 变化**上，RTS 瞬时返回。usbser.sys 在 DTR 变化时同步等设备
 *   从**中断 IN 端点（本工程 pipe6 / EP3-IN）**发来的 SERIAL_STATE 通知，
 *   等不到就整整等 30 s 超时后继续（所以数据照常能通，只是每次开关端口慢半分钟）。
 *
 * 怎么发（不需要自己写 FIFO，FSP 已经铺好路）:
 *   r_usb_cdataio.c 的 usb_data_write() 里有一句:
 *       if (g_usb_pcdc_int_in_pipe[ip] != pipe) { 常规批量写 }
 *       else { 把 buf[0]/buf[1] 填进 g_usb_pcdc_serialstate_table[8..9], tranlen=10 }
 *   而 usb_get_usepipe(p_ctrl, USB_TRANSFER_WRITE) 取的是
 *       g_usb_pipe_peri[(p_ctrl->type * 2) + 1]
 *   中断 IN 管道在 usb_pstd_get_pipe_no() 里登记的正是
 *       g_usb_pipe_peri[(USB_CLASS_INTERNAL_PCDCC * 2) + !USB_PIPE_DIR_IN] = [3]
 *   ⇒ 只要让 p_ctrl->type == USB_CLASS_INTERNAL_PCDCC，R_USB_Write 就自动走上
 *      "包装成 10 字节 SERIAL_STATE 通知"那条路。
 *   而 R_USB_Write 的第四个参数 destination 在设备模式下会直接赋给 p_ctrl->type
 *   （`p_ctrl->type = destination & 0x7F`），所以传 **USB_CLASS_PCDCC(0x81)**
 *   即可（0x81 & 0x7F == 1 == USB_CLASS_INTERNAL_PCDCC）。
 *   ⚠ 长度必须传 2：那 2 字节就是 SERIAL_STATE 的 UART 状态位图，
 *     FSP 会自己在前面拼 8 字节请求头变成 10 字节的通知。
 *   完成回调也已就位：g_usb_callback[3]（PCDCC 的写完成）=
 *   usb_pcdc_write_complete，会照常发 USB_STATUS_WRITE_COMPLETE 给应用。
 * ============================================================ */

/** SERIAL_STATE 的 UART 状态位图（小端 2 字节）。bit0=bDCD(RxCarrier) bit1=bDSR(TxCarrier)。 */
static uint8_t       s_serial_state[2]  = {0x00u, 0x00u};
/** 有待发的通知（由 SET_CONTROL_LINE_STATE 或枚举完成置位）。 */
static volatile bool s_serial_pending   = false;
/** 上一次通知还在飞（等 USB_STATUS_WRITE_COMPLETE 清零）。 */
static volatile bool s_serial_busy      = false;
/** busy 已经持续了多少次 Poll，用于兜底解锁。 */
static uint16_t      s_serial_busy_tick = 0u;
/** 距离下一次发送还差几个 Poll（burst 的间隔）。 */
static uint16_t      s_serial_gap       = 0u;
/** burst 还要再发几次。 */
static uint8_t       s_serial_burst_left = 0u;
/** 最近一次 0x22 里 DTR 的状态（决定位图里要不要带 DSR）。 */
static bool          s_serial_want_dsr  = false;

/* ============================================================
 * ⭐ 运行时可调的实验开关（探针直接改 RAM 即可，不用重烧）
 *
 * 背景：首版实现（收到 0x22 就发一次 DSR/DCD 通知）**没能消掉 30 s**：
 *   实测 g_usb_line_cnt=12（0x22 收到 12 次）、g_usb_ss_sent=12（通知也发了 12 次）、
 *   g_usb_ss_err=0（R_USB_Write 全部成功），host 照样等满 30 s。
 *   ⇒ 说明"发一次"这个动作本身不够，缺的是 **时机** 或 **位图内容**。
 * 所以把这两件事都做成可调，用探针改几个变量就能穷举，不用反复烧录。
 *
 * g_usb_ss_mode 位定义：
 *   bit0 burst      收到 0x22 后连发 g_usb_ss_burst_n 次（间隔 g_usb_ss_gap_ticks）
 *   bit1 固定位图   忽略 wValue，一律用 g_usb_ss_bitmap
 *   bit2 持续模式   不等 0x22，每 g_usb_ss_gap_ticks 次 Poll 主动发一次
 *   bit3 忽略 busy   即使上一笔还在飞也照发（用来看 QOVR 错误）
 * ============================================================ */
volatile uint32_t g_usb_ss_mode       = 0u;
/** wValue 里代表 DTR 的位（1 或 2）。实测 Windows usbser 用 bit1= DTR、bit0 = RTS。 */
volatile uint32_t g_usb_dtr_bit      = 0x0002u;
/** 最近一次看到 pipe0 的 BEMPENB 值（验证补丁是否生效）。 */
volatile uint32_t g_usb_pipe0_enb    = 0u;
/** 设备侧看到的控制管道状态：pending request / data_cnt[0]。 */
volatile uint32_t g_usb_pipe0_req    = 0u;
volatile uint32_t g_usb_dcnt0        = 0u;
/** 要发的 UART 状态位图（bit0=bDCD bit1=bDSR）。 */
volatile uint32_t g_usb_ss_bitmap     = 0x0003u;
/** burst 次数 / 间隔（Poll 次数，1 次 Poll ≈ 2 ms）。 */
volatile uint32_t g_usb_ss_burst_n    = 5u;
volatile uint32_t g_usb_ss_gap_ticks  = 25u;
/** Poll 计数器（时间轴用，1 次 ≈ 2 ms）。 */
volatile uint32_t g_usb_ss_tick       = 0u;

/** 事件时间线（环形，16 条）。kind: 1=收到 0x22 2=已发通知 3=写完成 4=busy 超时。 */
volatile uint32_t g_usb_ss_hist_kind[16] = {0u};
volatile uint32_t g_usb_ss_hist_val[16]  = {0u};
volatile uint32_t g_usb_ss_hist_tick[16] = {0u};
volatile uint32_t g_usb_ss_hist_idx      = 0u;

/** 诊断：已发出的 SERIAL_STATE 条数 / 最后一次 R_USB_Write 的返回码 / busy 兜底解锁次数。 */
volatile uint32_t g_usb_ss_sent        = 0u;
volatile uint32_t g_usb_ss_err         = 0xFFFFFFFFu;
volatile uint32_t g_usb_ss_stuck       = 0u;
/** 非零返回码的累计次数。 */
volatile uint32_t g_usb_ss_err_cnt     = 0u;
/** 最近一次 SET_CONTROL_LINE_STATE 的 wValue（bit0=DTR bit1=RTS）及收到次数。 */
volatile uint32_t g_usb_line_value     = 0u;
volatile uint32_t g_usb_line_cnt       = 0u;

/** 往事件时间线里记一条。 */
static void usb_ss_hist(uint32_t kind, uint32_t val)
{
    uint32_t i = g_usb_ss_hist_idx;

    g_usb_ss_hist_kind[i] = kind;
    g_usb_ss_hist_val[i]  = val;
    g_usb_ss_hist_tick[i] = g_usb_ss_tick;
    g_usb_ss_hist_idx     = (i + 1u) & 15u;
}

static uint8_t           s_rx_buf[USB_CDC_RX_CHUNK];
static uint8_t           s_tx_stage[USB_CDC_TX_STAGE];
static volatile bool     s_configured = false;

/** 待重挂接收的标志。
 *  ⚠ 由 `usb_callback`（中断上下文）**只置位**，实际 `R_USB_Read` 在 `USB_CDC_Poll()`
 *  （主循环）里做 —— FSP 禁止在回调里发起传输，否则重挂会静默失败、pipe5 不再 armed，
 *  主机后续 write 全部 NAK（实测：只收到第一包 50 B，之后 write timeout）。 */
static volatile bool     s_rx_need_arm = false;

/** 接收挂起的诊断：最后一次 R_USB_Read 的返回码 / 累计成功挂起次数。 */
volatile uint32_t        g_usb_read_err     = 0xFFFFFFFFu;
volatile uint32_t        g_usb_read_arm_cnt = 0u;

/* CDC 的 7 字节线路编码（GET_LINE_CODING 的应答 / SET_LINE_CODING 的接收缓冲）。
 * 默认 115200 8N1：dwDTERate=115200(0x0001C200 小端) / bCharFormat=0 / bParityType=0 / bDataBits=8 */
static uint8_t           s_line_coding[7] = { 0x00u, 0xC2u, 0x01u, 0x00u, 0x00u, 0x00u, 0x08u };

/* 2026-10-25: "其它类请求"的数据阶段暂存区。
 * 用于那些我们本来不认识、但**带数据阶段**的请求（如 GET_ENCAPSULATED_RESPONSE），
 * 避免按"无数据"收尾导致主机干等 ~30 s 超时。64 字节覆盖常见控制请求长度。 */
#define USB_REQ_SCRATCH_N   64u
static uint8_t           s_req_scratch[USB_REQ_SCRATCH_N];

volatile uint32_t g_usb_open_err = 0xFFFFFFFFu;
volatile uint32_t g_usb_events   = 0u;
volatile uint32_t g_usb_rx_bytes = 0u;
volatile uint32_t g_usb_tx_bytes = 0u;
volatile uint32_t g_usb_cfg_cnt  = 0u;
/* P814/P815 复用成 USB_FS 的返回码（0 = FSP_SUCCESS）。这两脚若是固定功能而不可复用,
 * PinCfg 会返回错误 —— 那时 USB 仍可能工作, 但排障时要能一眼看到。 */
volatile uint32_t g_usb_pin_err_dp = 0xFFFFFFFFu;
volatile uint32_t g_usb_pin_err_dm = 0xFFFFFFFFu;
/* P407 复用成 USB_VBUS 的返回码（0 = FSP_SUCCESS）。见下方 USB_CDC_CLAIM_VBUS_PIN 说明。 */
volatile uint32_t g_usb_pin_err_vbus = 0xFFFFFFFFu;
/** 最近一次读到的 INTSTS0.VBSTS(bit7)：1 = 控制器认为 VBUS 已接入。 */
volatile uint32_t g_usb_vbsts = 0u;
/** 最近一次读到的 SYSCFG（应 = 0x0411: USBE|DPRPU|SCKE, DCFM=0 设备模式）。 */
volatile uint32_t g_usb_syscfg = 0u;

/* ============================================================
 * 实验/修复开关: 1 = 把 P407 从 IIC0(SDA0) 切成 USB_FS(USB_VBUS)。
 *
 * 背景（2026-10-23 寄存器级查实）:
 *   RA6E2 的 USB VBUS 检测输入 **只在 P407 这一个引脚上**（数据手册引脚表:
 *   `P407 = SDA0_B/SSIBCK0_A/USB_VBUS`），没有独立的 USBVBUS 专用脚。
 *   本工程把 P407 配成 IIC0 的 SDA0（给 1.3" OLED / BH1750 用），一旦 PFS 选中 IIC，
 *   USB 模块的 VBUS 输入就被物理断开 ⇒ INTSTS0.VBSTS 恒 0 ⇒ 控制器判定"VBUS 不在"
 *   ⇒ DVSQ 进 Suspend ⇒ 收到 SETUP 后停在读数据阶段、CTRT 永不产生、枚举必失败
 *   （Windows 报"设备描述符请求失败 / 错误43"）。
 *
 * ✅ 2026-10-23 已彻底解耦：I2C0 的 SDA 已从 P407 迁到 **P206（SDA0_C）**，
 *    SCL 保持 **P408（SCL0_B）** 不动（见 ra_gen/pin_data.c 与 configuration.xml）。
 *    所以这个开关置 1 **不再影响 1.3" OLED** —— P407 现在专供 USB_VBUS。
 * 回退: 若哪天要 P407 回去做 I2C，置 0 并把 pin_data.c 的 SDA 改回 P407。
 * ============================================================ */
#ifndef USB_CDC_CLAIM_VBUS_PIN
#define USB_CDC_CLAIM_VBUS_PIN   (1)
#endif
/** SYSCFG.DPRPU 的最终状态（1 = D+ 内部上拉已打开 = 电气上真的挂在总线上）。
 *  0 表示设备对主机"不存在", 此时插不插线都没有任何反应。 */
volatile uint32_t g_usb_dprpu      = 0u;

/* 调试探针快照(详见 bsp_usb_cdc.h 顶部注释)。这些全局由 USB_CDC_ProbeUpdate() 每帧代读,
 * 因为调试器只能读 RAM, 读不到外设寄存器(0x400xxxxx)和向量表(0x0)。 */
volatile uint32_t g_usb_ispr0   = 0u;
volatile uint32_t g_usb_elcr    = 0u;
volatile uint32_t g_usb_vt_usbfs= 0u;
volatile uint32_t g_usb_ielsr0  = 0u;
volatile uint32_t g_usb_iser0   = 0u;
volatile uint32_t g_usb_intsts0 = 0u;
volatile uint32_t g_usb_intenb0 = 0u;

/** 挂下一次接收。必须每收完一包就重挂 —— FSP 的 read 是一次性的。
 *
 * ⚠⚠⚠ 只能在**主循环（线程上下文）**调用，绝不能在 `usb_callback` 里调。
 *   FSP 明确禁止在中断/回调里发起传输（同 R_USB_PeriControlDataGet/Set 那条禁令）。
 *   实测后果（2026-10-24）：回调里重挂**失败且不报错**，pipe5 从此不再 armed
 *   ⇒ 主机后续 write 全部 NAK ⇒ `write timeout`（pyserial 2 s 超时）
 *   ⇒ 设备只收到**第一包**就断流 ⇒ 屏上一直显示 `--`。
 *   ⇒ 正确做法：回调只置 `s_rx_need_arm`，由 `USB_CDC_Poll()` 真正挂。
 */
static void usb_read_arm(void)
{
    g_usb_read_err = (uint32_t) R_USB_Read(&g_basic0_ctrl, s_rx_buf,
                                           USB_CDC_RX_CHUNK, USB_CLASS_PCDC);
    g_usb_read_arm_cnt++;
}

/** 给"带数据阶段的控制请求"准备 pipe0 的收发中断。
 *
 * ⚠⚠ 顺序很关键：**必须先清陈旧状态，再开使能**。
 * `BRDYSTS` / `BEMPSTS` 是**写 0 清除**（写 1 无效），而且会留着上一次传输的位。
 * 如果只开使能不清状态，一个陈旧位会立刻触发伪中断：
 *   伪 BRDY → FSP `usb_pstd_brdy_pipe()` → `usb_pstd_read_data(pipe0)`；
 *            此时 `data_cnt[0]==0` ⇒ 走 "Null packet is end of read" ⇒ 可能
 *            直接 `usb_pstd_ctrl_end(USB_DATA_ERR)` 把控制传输打断。
 * 结果就是主机侧看到"状态阶段不完成" ⇒ usbser 干等 30 s。
 *
 * 只在 0x21 / 0x20 这两个**有数据阶段**的分支调用；
 * 0x22 无数据阶段，用不到 pipe0 收发中断，一律不碰（见 v15 说明）。 */
static void usb_pipe0_ctrl_isr_arm(void)
{
    g_usb_brdysts_at_req = (uint32_t) R_USB_FS0->BRDYSTS;
    g_usb_bempsts_at_req = (uint32_t) R_USB_FS0->BEMPSTS;

    /* 写 0 清除：只清 bit0，其余位写 1 即"不动"。 */
    R_USB_FS0->BRDYSTS = (uint16_t) ~0x0001u;
    R_USB_FS0->BEMPSTS = (uint16_t) ~0x0001u;

    R_USB_FS0->BRDYENB |= (uint16_t) 0x0001u;
    R_USB_FS0->BEMPENB |= (uint16_t) 0x0001u;
    g_usb_pipe0_enb = (uint32_t) R_USB_FS0->BEMPENB;
}

/** ⭐ v16 根因修复：让"无数据控制请求"的状态阶段真能发出去。
 *
 * 无数据类请求（典型：0x22 SET_CONTROL_LINE_STATE）的状态阶段是一个
 * **0 字节 IN 事务**。要让它被主机 ACK，pipe0（DCP）的 PID 必须是 `BUF`；
 * 若停留在 NAK，主机永远拿不到 ACK ⇒ 控制传输停在
 * `CTSQ = 5 (Ctrl write nodata status stage)` ⇒ usbser 干等 30 s 超时。
 *
 * FSP 的**标准请求**路径都有这一步（例如 `usb_pstd_set_configuration3()`）：
 * ```c
 * usb_cstd_set_buf(p_utr, USB_PIPE0);   // 本质 = hw_usb_set_pid(pipe0, USB_PID_BUF)
 * ```
 * 而**类请求**路径 `usb_peri_class_request_wnss()` 直接 `usb_pstd_ctrl_end()`，
 * **漏了这一步** ⇒ 只有类请求的无数据阶段会挂住。
 *
 * 实测证据（2026-10-24，FireDAP 直读 INTSTS0）：
 *   DTR 操作期间 `INTSTS0` 恒为 `0x20B5`，连采 40 次（8 s）一模一样：
 *     CTSQ  = 5  → 卡在"无数据状态阶段"
 *     DVSQ  = 3  → 设备已配置
 *     VBSTS = 1  → VBUS 正常
 *   而 0x20 SET_LINE_CODING（有数据阶段）只要 0.021 s ⇒ 差异正是这一步。
 *
 * 实现上等价于 `hw_usb_set_pid(pipe0, USB_PID_BUF)`：只改 DCPCTR 的 PID[1:0]
 * （USB_PID=0x0003 / USB_PID_BUF=0x0001），不碰 CCPL/BSTS 等位。
 * 调用时机：应用回调里（此时 FSP 还没执行 `usb_pstd_ctrl_end()`），
 * 之后 ctrl_end 置 CCPL，硬件就能把 0 长度 IN 包发出去。
 */
static void usb_pipe0_nodata_arm(void)
{
    uint16_t dc = (uint16_t) R_USB_FS0->DCPCTR;

    dc = (uint16_t) ((dc & (uint16_t) ~0x0003u) | 0x0001u);   /* PID = BUF */
    R_USB_FS0->DCPCTR = dc;
    g_usb_dcptr_after = (uint32_t) R_USB_FS0->DCPCTR;
}

/** 发 CDC 的 SERIAL_STATE 通知（走中断 IN 管道 pipe6）。
 *
 * 传 USB_CLASS_PCDCC 作 destination ⇒ p_ctrl->type 被置成 USB_CLASS_INTERNAL_PCDCC
 * ⇒ usb_get_usepipe(WRITE) 取到 g_usb_pipe_peri[3] = 中断 IN 管道
 * ⇒ usb_data_write() 自动把 2 字节位图拼进 g_usb_pcdc_serialstate_table 发 10 字节。
 *
 * ⚠ 只在主循环（线程上下文）调用：FSP 明令禁止在中断/回调里发起传输。 */
static void usb_ss_send(uint32_t bitmap)
{
    s_serial_state[0] = (uint8_t) (bitmap & 0xFFu);
    s_serial_state[1] = (uint8_t) (bitmap >> 8);

    g_usb_ss_err = (uint32_t) R_USB_Write(&g_basic0_ctrl, s_serial_state, 2u,
                                          (uint8_t) USB_CLASS_PCDCC);
    if (FSP_SUCCESS != g_usb_ss_err)
    {
        g_usb_ss_err_cnt++;
    }
    else
    {
        s_serial_busy      = true;
        s_serial_busy_tick = 0u;
        g_usb_ss_sent++;
    }
    usb_ss_hist(2u, bitmap);
}

/** SERIAL_STATE 通知的调度器。
 *
 * 默认行为（g_usb_ss_mode = 0）：收到 0x22 就发一次，位图 = DTR ? (DCD|DSR) : 0。
 * 可用 g_usb_ss_mode 的各位打开 burst / 持续 / 固定位图 / 忽略 busy（见上面的说明）。
 */
static void usb_serial_state_service(void)
{
    uint32_t bitmap;

    g_usb_ss_tick++;

    if (false == s_configured)
    {
        return;
    }

    /* 持续模式：不等 0x22，周期性主动发。用来验证"host 到底等的是不是通知本身"。 */
    if (0u != (g_usb_ss_mode & 0x04u))
    {
        if (0u == s_serial_gap)
        {
            usb_ss_send(g_usb_ss_bitmap);
            s_serial_gap = (uint16_t) g_usb_ss_gap_ticks;
        }
        else
        {
            s_serial_gap--;
        }
        return;
    }

    if (s_serial_busy)
    {
        /* 兜底: 完成回调没来时别把自己永久锁死。20 × 2 ms ≈ 40 ms。 */
        s_serial_busy_tick++;
        if (s_serial_busy_tick > 20u)
        {
            s_serial_busy      = false;
            s_serial_busy_tick = 0u;
            g_usb_ss_stuck++;
            usb_ss_hist(4u, 0u);
        }
        return;
    }

    if (false == s_serial_pending)
    {
        return;
    }

    if (0u != s_serial_gap)
    {
        s_serial_gap--;
        return;
    }

    bitmap = (0u != (g_usb_ss_mode & 0x02u)) ? g_usb_ss_bitmap
                                             : (s_serial_want_dsr ? 0x0003u : 0x0000u);
    usb_ss_send(bitmap);

    if (s_serial_burst_left > 0u)
    {
        s_serial_burst_left--;
        s_serial_gap = (uint16_t) g_usb_ss_gap_ticks;
    }
    else
    {
        s_serial_pending = false;
    }
}

/* 应用层 USB 描述符表里的 MS OS 2.0 描述符集(CompatID=CDCACM), 用于应答 Windows 的
 * vendor 请求(0xC0 / bRequest=0x01 / wIndex=0x0007)。
 *
 * ⚠⚠ 惰性兜底, 当前**不会被执行**, 且原定目标已被证伪 —— 勿据此再动固件:
 *   1) 只有 bcdUSB>=0x0201 时 Windows 才会请求 BOS 并发这条 vendor 请求; 本项目
 *      USB_BCDNUM 固定 0x0200, 所以下面这段代码永不触发。
 *   2) 实测(2026-10-25)升 0x0201 启用 BOS 两次(外置 / 内嵌描述符集)都导致 Windows 把
 *      设备标记为 CM_PROB_FAILED_START, 比"慢 30s"更糟。
 *   3) 而且那 30s 已被证明与"驱动查询"无关: devnode 0.9s 就绑好 usbser 并 OK,
 *      之后 USB 总线静默 30s, 是主机内部 usbser/msports 的 COM 名登记延迟(固件无责)。
 *   详见 usb_cdc_descriptor.c 文件头的完整结论。将来若升级 FSP 想重试, 改 USB_BCDNUM 即可。
 *
 * 大小 46 = BOS 描述符里声明的 wMSOSDescriptorSetTotalLength (0x2E), 二者必须一致。 */
extern uint8_t g_apl_msos20_set[46];

/** USB 事件回调（BSP_CFG_RTOS=0 时签名是 void (*)(usb_callback_args_t *)） */
void usb_callback(usb_callback_args_t * p_args)
{
    g_usb_events++;

    switch (p_args->event)
    {
        case USB_STATUS_CONFIGURED:
        {
            s_configured = true;
            g_usb_cfg_cnt++;
            /* ⚠ 不在这里直接 usb_read_arm()：回调是中断上下文，FSP 禁止发起传输。
             *   只置标志，由主循环的 USB_CDC_Poll() 真正挂。 */
            s_rx_need_arm = true;
            break;
        }

        case USB_STATUS_READ_COMPLETE:
        {
            uint32_t n = p_args->data_size;

            if (n > USB_CDC_RX_CHUNK)
            {
                n = USB_CDC_RX_CHUNK;
            }
            g_usb_rx_bytes += n;
            UART_Link_PushBytes(s_rx_buf, n);
            /* ⚠ 同上：不在这里重挂。回调里 R_USB_Read 会静默失败，
             *   后果是**只收到第一包就断流**（主机 write 超时）。 */
            s_rx_need_arm = true;
            break;
        }

        case USB_STATUS_REQUEST:
        {
            /* ⚠⚠⚠ 关键: FSP **不会**替应用应答 CDC 类请求。
             * `usb_peri_class_request_rwds()` 在 BSP_CFG_RTOS==0 时只做一件事:
             *     ctrl.setup=*req; ctrl.data_size=0; ctrl.status=ACK;
             *     ctrl.type=USB_CLASS_REQUEST;
             *     usb_set_event(USB_STATUS_REQUEST, &ctrl);   // 转交给应用回调
             * 应用不在这里应答, usbser.sys 的 GET_LINE_CODING 就会一直等超时,
             * 后果是 **设备枚举正常但系统不创建 COM 端口**(SERIALCOMM 里没有该项)。
             *
             * request_type 编码: 低字节 bmRequestType, 高字节 bRequest。
             * 实测主机发的就是 0x21A1 = (0xA1 << 8) | 0x21  → GET_LINE_CODING。 */
            uint8_t breq = (uint8_t) (p_args->setup.request_type >> 8);

            g_usb_req_cnt++;
            g_usb_req_last = p_args->setup.request_type;

            /* 环形日志：把这次请求的完整 SETUP 字段记下来（事后用 probe-rs 读） */
            {
                uint32_t h = g_usb_req_log_head;
                g_usb_req_log_bmrt  [h] = (uint8_t) (p_args->setup.request_type & 0xFFU);
                g_usb_req_log_breq  [h] = breq;
                g_usb_req_log_value [h] = p_args->setup.request_value;
                g_usb_req_log_index [h] = p_args->setup.request_index;
                g_usb_req_log_length[h] = p_args->setup.request_length;
                g_usb_req_log_seq   [h] = g_usb_req_cnt;
                g_usb_req_log_head      = (h + 1u) % USB_REQ_LOG_N;
            }

            /* ⚠⚠ v15 关键修正：**不要再"每个类请求都乱开 pipe0 中断"**。
             *
             * 旧写法（v12~v14）在这里无条件执行
             *     BRDYENB(pipe0) |= 1;  BEMPENB(pipe0) |= 1;
             * 那是基于"FSP 漏开 BEMPENB"的假设。该假设**已被证伪**：
             * 补丁在板上跑了三天，`SETDTR` 依旧整整卡 30.00 s（v15 之前实测）。
             *
             * 而 BRDYSTS / BEMPSTS 是**写 0 清除**、并且会**保留上一次传输留下的位**。
             * 一旦这里只"开使能"而没清状态，随便一个陈旧位就会立刻触发一次**伪中断**：
             *   - 伪 BRDY → FSP 的 `usb_pstd_brdy_pipe()` 会对 pipe0 调
             *     `usb_pstd_read_data()`；此时 data_cnt[0]==0 ⇒ 走 "Null packet is end
             *     of read" 分支 ⇒ 可能直接把控制传输 `ctrl_end(USB_DATA_ERR)` 掉。
             *   - 伪 BEMP → `usb_pstd_write_data()` 同样在 data_cnt[0]==0 时收尾。
             * 伪中断发生在**状态阶段正要收尾**的瞬间 ⇒ 主机看到的就是"状态阶段一直
             * 不完成" ⇒ usbser 等满 30 s 超时。
             *
             * ⇒ 现在的规矩：
             *   ① **只有带数据阶段的请求**（0x21 读 / 0x20 写）才动 pipe0 中断；
             *   ② 开之前必须先把陈旧状态位清掉（见 usb_pipe0_ctrl_isr_arm()）；
             *   ③ 0x22 无数据阶段 ⇒ **完全不碰** pipe0 中断（用不到，只会引入风险）。
             */

            if ((uint8_t) (USB_PCDC_GET_LINE_CODING >> 8) == breq)
            {
                /* 0x21 GET_LINE_CODING: 控制读(设备→主机), 必须回 7 字节。
                 * ⚠ 必须用 PeriControlDataGet 走 **pipe0 控制管道**;
                 *   普通的 R_USB_Write(dst=USB_CLASS_PCDC) 会走批量 IN 管道, 对控制请求无效。 */
                usb_pipe0_ctrl_isr_arm();                      /* 先清陈旧位, 再开中断 */
                g_usb_get_err     = (uint32_t) R_USB_PeriControlDataGet(&g_basic0_ctrl, s_line_coding, 7u);

                g_usb_cfifo_after = (uint32_t) (*(volatile uint16_t *) 0x4009002AUL);   /* CFIFOCTR */
                g_usb_pipe0_after = (uint32_t) g_usb_pstd_pipe0_request;
                g_usb_dcnt_after  = g_usb_pstd_data_cnt[0];
                /* 不调 R_USB_PeriControlStatusSet：FSP 的 usb_peri_class_request_rss()
                 * 已经自己 usb_pstd_ctrl_end() 收尾，再调一次会结束两次 → 主机等 30 s 超时。 */
                g_usb_stat_err    = 0xEEEEEEEEu;
            }
            else if ((uint8_t) (USB_PCDC_SET_LINE_CODING >> 8) == breq)
            {
                /* 0x20 SET_LINE_CODING: 控制写(主机→设备), 收下 7 字节(继续用默认值也行) */
                usb_pipe0_ctrl_isr_arm();                      /* 先清陈旧位, 再开中断 */
                (void) R_USB_PeriControlDataSet(&g_basic0_ctrl, s_line_coding, 7u);
                /* 同上：不调 StatusSet，由 FSP 的 usb_peri_class_request_wss() 收尾。 */
            }
            else if ((uint8_t) (USB_PCDC_SET_CONTROL_LINE_STATE >> 8) == breq)
            {
                /* 0x22 SET_CONTROL_LINE_STATE: 无数据阶段，但要**回一个 SERIAL_STATE**。
                 *
                 * wValue: bit0 = DTR, bit1 = RTS。
                 * 实测 usbser.sys 在 DTR 变化时（正是 Open/Close 各卡 30 s 的那一步）
                 * 同步等设备的中断 IN 通知，等不到就干等 30 s 超时。
                 * 所以这里记录 wValue 并把通知挂上待发，由主循环真正发出去。
                 *
                 * ⚠ 不要在回调里直接 R_USB_Write：此刻 FSP 还没走完
                 *   usb_pstd_ctrl_end()（usb_peri_class_request_wnss 是先
                 *   usb_set_event() 再 ctrl_end），在同一栈上起批量传输容易和
                 *   控制传输的收尾抢 pipe/FIFO。延后到主循环发最稳。 */
                uint16_t wval = p_args->setup.request_value;
                uint16_t st;

                g_usb_line_value = (uint32_t) wval;
                g_usb_line_cnt++;
                usb_ss_hist(1u, (uint32_t) wval);

                /* 位图: DTR 置位 ⇒ 报 DCD+DSR 都在线; DTR 撤掉 ⇒ 报都不在线。
                 * usbser 等的是 DSR(bit1)，DCD(bit0) 一并给上更保险。
                 * ⚠ wValue 的位定义**实测与 CDC 规范文字相反**（见 NOTES-usb-cdc.md）：
                 *   Windows 的 SETDTR 落到 wValue=0x0002，SETRTS 落到 0x0003，
                 *   即 **bit1=DTR、bit0=RTS**。哪个 bit 是 DTR 由 g_usb_dtr_bit 决定，
                 *   可以在不改固件的情况下用探针改 RAM 试。 */
                s_serial_want_dsr = (0u != (wval & g_usb_dtr_bit));
                st = s_serial_want_dsr ? 0x0003u : 0x0000u;
                s_serial_state[0] = (uint8_t) (st & 0xFFu);
                s_serial_state[1] = (uint8_t) (st >> 8);
                s_serial_pending  = true;
                s_serial_gap      = 0u;
                if (0u != (g_usb_ss_mode & 0x01u))
                {
                    s_serial_burst_left = (uint8_t) g_usb_ss_burst_n;
                }
                else
                {
                    s_serial_burst_left = 0u;
                }

                /* ⭐ 补 FSP 漏掉的那一步（见 usb_pipe0_nodata_arm 的说明）。
                 * 没有它，这个请求的状态阶段永远发不出去 ⇒ open/close 各卡 30 s。 */
                usb_pipe0_nodata_arm();
            }
            else if ((0x01 == breq) &&
                     (0xC0 == (uint8_t) (p_args->setup.request_type & 0xFFU)) &&
                     (0x0007 == p_args->setup.request_index))
            {
                /* Microsoft OS 2.0 描述符集请求: vendor 读(bmRequestType=0xC0,
                 * bRequest=bMS_VendorCode=0x01, wIndex=0x0007)。回 CompatID="CDCACM"
                 * 的描述符集(见 usb_cdc_descriptor.c 的 g_apl_msos20_set)。
                 * ⚠ 惰性兜底: 只有 bcdUSB>=0x0201 时才会触发, 当前 0x0200 永不执行;
                 *   且实测启用 BOS 会导致设备 CM_PROB_FAILED_START, 详见文件头注释。
                 * 长度取主机请求与描述符集的较小值。 */
                uint16_t want = p_args->setup.request_length;
                uint16_t have = (uint16_t) sizeof(g_apl_msos20_set);
                uint16_t len  = (want < have) ? want : have;
                usb_pipe0_ctrl_isr_arm();                      /* 先清陈旧位, 再开中断 */
                g_usb_get_err = (uint32_t) R_USB_PeriControlDataGet(&g_basic0_ctrl,
                                                                     g_apl_msos20_set, len);
            }
            else
            {
                /* 2026-10-25 修正：以前这里**无条件**按"无数据"收尾。
                 * 带数据阶段的请求(bRequest 非 0x20/0x21/0x22 且 wLength>0, 例如
                 * 0x01 GET_ENCAPSULATED_RESPONSE / 0x00 SEND_ENCAPSULATED_COMMAND)
                 * 若被当成"无数据"处理, 主机会永远等不到数据 ⇒ 干等超时。
                 *
                 * ⚠ 但事后用请求日志验证: **本设备从未收到这类请求** —— 枚举全程只有
                 *   4 条标准 CDC 请求, g_usb_req_data_cnt 始终为 0。所以它不是"COM 口
                 *   晚 29.6s"的原因(那条另有出处, 见 usb_cdc_descriptor.c 文件头)。
                 *   这个修正仍保留: 它本身是正确且无害的健壮性改进(避免将来遇到带数据
                 *   阶段的请求时把控制传输挂死), 但**不要**再把它当成 30s 的解释。
                 *
                 * ⇒ 现在按 wLength 与方向分别处理：
                 *     wLength == 0            → 维持原来的"无数据"收尾
                 *     IN (bmRequestType&0x80) → 回 wLength 字节（填 0）
                 *     OUT                     → 收下 wLength 字节
                 */
                uint16_t wlen = p_args->setup.request_length;
                uint8_t  bmrt = (uint8_t) (p_args->setup.request_type & 0xFFU);

                if (0u == wlen)
                {
                    usb_pipe0_nodata_arm();
                    g_usb_req_nodata_cnt++;
                }
                else
                {
                    uint16_t len = (wlen < USB_REQ_SCRATCH_N) ? wlen : (uint16_t) USB_REQ_SCRATCH_N;
                    uint16_t i;

                    for (i = 0u; i < len; i++)
                    {
                        s_req_scratch[i] = 0u;
                    }
                    usb_pipe0_ctrl_isr_arm();                  /* 先清陈旧位, 再开中断 */
                    g_usb_req_data_cnt++;
                    if (0u != (bmrt & 0x80U))
                    {
                        (void) R_USB_PeriControlDataGet (&g_basic0_ctrl, s_req_scratch, len);
                    }
                    else
                    {
                        (void) R_USB_PeriControlDataSet (&g_basic0_ctrl, s_req_scratch, len);
                    }
                }
            }
            break;
        }

        case USB_STATUS_REQUEST_COMPLETE:
        {
            /* 类请求的状态阶段由 FSP 在 usb_peri_class_request_rss()/wss() 里自动
             * usb_pstd_ctrl_end() 完成, 这里只记账用于判定控制传输是否真的走完。 */
            g_usb_req_done_cnt++;
            break;
        }

        case USB_STATUS_WRITE_COMPLETE:
        {
            /* 批量 IN 与中断 IN（SERIAL_STATE）共用这一个完成事件，
             * 这里只把"通知通道不再忙"标记清掉即可。 */
            s_serial_busy      = false;
            s_serial_busy_tick = 0u;
            usb_ss_hist(3u, 0u);
            break;
        }

        case USB_STATUS_DETACH:
        case USB_STATUS_SUSPEND:
        {
            /* 拔线/主机挂起: 标记掉线, 但不清已收到的数值 ——
             * 屏幕上的最后一次读数比全 `--` 有用, 链路状态行会自己变 NO LINK。 */
            s_configured     = false;
            s_serial_pending = false;
            s_serial_busy    = false;
            break;
        }

        default:
        {
            break;
        }
    }
}

fsp_err_t USB_CDC_Init(void)
{
    fsp_err_t err;

    /* ⚠⚠ 根因修复: 打开 ELC(事件链接控制器)。
     * FSP 的 bsp 启动只配置 IELSR[] 和 NVIC ISER, 从不自动打开 ELC; 只有 R_ELC_Open()+R_ELC_Enable()
     * 会置 ELCR.ELCON=1。本项目没加 ELC 实例 ⇒ ELC 一直停在禁用态(模块停止里, 读回来 ELCR=0x00),
     * 所有走 ELC 的外部中断(USBFS/UART/SPI/I2C…)的事件→IRQ 路由被整体切断, 中断永远到不了 CPU ——
     * 这正是 g_usb_events 恒 0、Windows 报"错误43 / VID_0000&PID_0002"的真正原因。
     * 这里用 FSP 自己的序列打开它(与 R_ELC_Open/R_ELC_Enable 完全一致), 不碰任何生成物。
     * 仅释放 ELC 模块停止 + 置 ELCON, 不做任何供电/引脚/总线动作, 安全(µA 级)。 */
    R_BSP_MODULE_START(FSP_IP_ELC, 0);   /* 释放 ELC 的模块停止(MSTPC) */
    /* ELCR @0x40082000, ELCON = bit7 (mask 0x80)。r_elc.h 不在本工程拷贝的 FSP 树里,
     * 故直接按硬件地址写(与 R_ELC_Enable 的 *p_elcr = 0x80 完全一致)。 */
    *(volatile uint8_t *) 0x40082000UL = 0x80U;   /* ELCON = 1: 全局允许事件链接(含 IELSR→ICU) */

    /* USB_DP / USB_DM: 复用成 USB_FS。这两脚在 RASC 的 pincfg 里没配（会与自定义走线冲突),
     * 所以在这里配 —— 与 3.5" 屏的 RS/RES 同理，运行时配、不动生成物。 */
    g_usb_pin_err_dp = (uint32_t) R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_08_PIN_14,
                                                  (uint32_t) IOPORT_CFG_PERIPHERAL_PIN |
                                                  (uint32_t) IOPORT_PERIPHERAL_USB_FS);
    g_usb_pin_err_dm = (uint32_t) R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_08_PIN_15,
                                                  (uint32_t) IOPORT_CFG_PERIPHERAL_PIN |
                                                  (uint32_t) IOPORT_PERIPHERAL_USB_FS);

#if (1 == USB_CDC_CLAIM_VBUS_PIN)
    /* ⚠⚠ 根因修复(2): 把 P407 从 IIC0(SDA0) 切到 USB_FS(USB_VBUS)。
     *
     * P407 是 RA6E2 **唯一**的 USB VBUS 检测输入（数据手册引脚表: P407 = .../USB_VBUS,
     * 没有独立的 USBVBUS 专用脚）。RASC 把 P407 配成 IIC0-SDA0 给了 OLED，
     * 于是 USB 模块的 VBUS 输入被切断，INTSTS0.VBSTS 恒 0。
     * 实测(2026-10-23, 正确基址) INTSTS0=0x6059 ⇒ DVSQ=USB_DS_SPD_DFLT(挂起),
     * CTSQ=USB_CS_RDDS(收到 SETUP 后停在读数据阶段) ⇒ CTRT 永不产生 ⇒ 枚举失败。
     *
     * 这里只是把该脚选成 USB 输入功能（高阻输入，P407~P411 均为 5V 容忍），
     * 不驱动任何线、不改供电，安全。随后回读 INTSTS0.VBSTS 直接判定板上 VBUS 是否真正到达该脚。
     */
    g_usb_pin_err_vbus = (uint32_t) R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_04_PIN_07,
                                                    (uint32_t) IOPORT_CFG_PERIPHERAL_PIN |
                                                    (uint32_t) IOPORT_PERIPHERAL_USB_FS);
    R_BSP_SoftwareDelay(2u, BSP_DELAY_UNITS_MILLISECONDS);
#endif

    /* 用芯片出厂 UID 填充 USB 序列号(每块板唯一), 必须在 R_USB_Open 之前完成
     * (枚举时主机才读 string 6)。否则写死序列号会让 Windows 无法区分设备,
     * 每次重连都重建 devnode、重走"设备安装"流程 —— 表现就是"重启后等十几秒才出 COM"。 */
    {
        extern void usb_cdc_fill_serial_from_uid(void);
        usb_cdc_fill_serial_from_uid();
    }

    err = R_USB_Open(&g_basic0_ctrl, &g_basic0_cfg);
    g_usb_open_err = (uint32_t) err;

    /* ⚠⚠ FSP 自己不会无条件打开 D+ 上拉 —— 这一步必须我们补, 否则设备在电气上不存在。
     *
     * r_usb_basic.c 的 R_USB_Open -> usb_module_start 里是这么写的:
     *     if (USB_ATTACH == usb_pstd_chk_vbsts(p_ctrl->module_number))
     *     { ... hw_usb_pset_dprpu(...); }     // 只有"检测到 VBUS"才置 SYSCFG.DPRPU
     * 而 usb_pstd_chk_vbsts() 读的是 INTSTS0 的 VBSTS(bit7)，本板实测恒为 0。
     * 于是 R_USB_Open 返回 FSP_SUCCESS，但 DPRPU 一直是 0 ⇒ D+ 上没有 1.5k 上拉
     * ⇒ **主机侧根本看不到有设备插入**，插不插线表现完全一样（g_usb_events 恒 0）。
     * 这个坑极容易被误读成"固件没跑/线不对"，所以这里显式补上并留了读数。
     *
     * 安全性: 只打开 D+ 的内部 1.5k 上拉（常态电流 µA 级，最坏 ~2.2mA 也远低于任何限值），
     * 不改变供电、不驱动数据线、不做任何总线动作。之后由主机发总线复位开始枚举。 */
    if (FSP_SUCCESS == err)
    {
        R_BSP_SoftwareDelay(2u, BSP_DELAY_UNITS_MILLISECONDS);   /* 等控制器初始化落定 */

        /* 先断开再接上(D+ 上拉 0→1), 给主机一个干净的"断开→连接"边沿, 强制 Windows 重新枚举。
         * 否则主机可能还停在之前"设备描述符请求失败"的失败态, 不再主动发 SETUP,
         * 即便中断现在能进了也等不到第一个包(屏上会一直停在 USB IDLE)。 */
        R_USB_FS0->SYSCFG_b.DPRPU = 0u;
        R_BSP_SoftwareDelay(20u, BSP_DELAY_UNITS_MILLISECONDS);
        R_USB_FS0->SYSCFG_b.DPRPU = 1u;
        g_usb_dprpu = (uint32_t) R_USB_FS0->SYSCFG_b.DPRPU;

        /* ⚠ 同一个 VBUS 判定还卡住另一件事: 中断使能。
         *
         * FSP 在 hw_usb_pmodule_init() 里本该给 INTENB0 写
         *     BEMPE|BRDYE|VBSE|DVSE|CTRE = 0x0400|0x0100|0x8000|0x1000|0x0800 = 0x9D00
         * 全 FSP 里设备模式只有这一处（r_usb_preg_access.c:244）会写它。
         * 一旦它没落下去, INTSTS0 里的 VBINT/DVST/CTRT 会一直挂着但永远不进 CPU ——
         * 现象和"中断没使能"完全一样, 却又查不出哪一步漏了。
         * 这里无条件补写一次（值与原意完全相同, 幂等, 不改变任何行为）并回读留证。
         *
         * 写的是 USB 外设自己的中断使能寄存器, 不涉及供电/引脚/总线动作。 */
        R_USB_FS0->INTENB0 = (uint16_t) 0x9D00u;

        /* 若之前有任何地方关过全局中断（PRIMASK=1），中断同样永远进不来。
         * 裸机工程本应全程开中断, 这里显式放开一次, 并把结果打到屏上可查。 */
        __enable_irq();
    }

    RTT_Printf("usb cdc open err=%d dprpu=%u (cfg=%u)\n",
               (int) err, (unsigned) g_usb_dprpu, (unsigned) g_usb_cfg_cnt);

    return err;
}

bool USB_CDC_Configured(void)
{
    return s_configured;
}

/** 裸机 FSP 的 USB 事件派发入口 —— **必须由主循环(线程上下文)反复调用**。
 *
 * `r_usb_pinthandler_usbip0.c` 在 BSP_CFG_RTOS==0 时, ISR 只把事件解码后塞进
 * `g_usb_pstd_usb_int` 环形缓冲(深度仅 8), 派发由 `usb_pstd_pcd_task()` 完成。
 * RTOS 下这是 PCD 任务的活; 裸机必须应用自己调。
 *
 * ⚠ 不能放在 ISR 里: FSP 的类请求数据阶段 API(R_USB_PeriControlDataGet/Set) 明确
 *   禁止在中断函数/回调里调用(r_usb_basic.c:3670), 否则主机的 SetCommState 会超时
 *   (错误121)—— 这是实测踩过的坑。
 * ⚠ 也不能一秒才调一次: 缓冲只有 8 深, 枚举突发事件会溢出丢事件,
 *   表现为"有时枚举成功、有时停在 Address 态"。实测 2 ms 粒度足够。 */
void USB_CDC_Poll(void)
{
    uint32_t i;

    /* 控制传输阶段采样（v15 诊断）：见 g_usb_ctrt_last 的注释。
     * 只读 INTSTS0，不写，不影响任何状态位。 */
    {
        uint32_t ctrt = ((uint32_t) R_USB_FS0->INTSTS0 >> 13) & 0x7u;

        g_usb_ctrt_last = ctrt;
        if (0u != ctrt)
        {
            g_usb_ctrt_nonidle_cnt++;
        }
    }

    for (i = 0u; i < 8u; i++)
    {
        usb_pstd_pcd_task();
    }

    /* 重挂接收（在这里做，不在回调里做）。
     * 排完事件之后 pipe 状态才是最终的，此时 R_USB_Read 才可能成功。
     * 挂不上就下一轮再试，不会丢数据（主机那边的写会被 NAK 重传）。 */
    if ((false != s_rx_need_arm) && (false != s_configured))
    {
        s_rx_need_arm = false;
        usb_read_arm();
    }

    /* 排完事件再发 SERIAL_STATE：确保 SET_CONTROL_LINE_STATE 的控制传输已经收尾，
     * 不与 pipe0 / FIFO 抢资源。 */
    usb_serial_state_service();
}

void USB_CDC_ProbeUpdate(void)
{
    uint32_t vtor = *(volatile uint32_t *) 0xE000ED08UL;   /* VTOR: 向量表可能不在 0x0 */

    /* ⚠⚠⚠ 真因修复(3) —— 裸机下必须自己排空 USB 中断事件缓冲。
     *
     * `r_usb_pinthandler_usbip0.c` 在 BSP_CFG_RTOS == 0 时, ISR 里只做两件事:
     *     usb_pstd_interrupt_handler(&type, &status, ip);   // 只"解码"事件
     *     g_usb_pstd_usb_int.buf[wp++] = {type, status, p_cfg};  // 塞进环形缓冲
     * **它不会调用 usb_pstd_interrupt() 去真正派发** —— 派发由 `usb_pstd_pcd_task()` 做
     * (r_usb_pdriver.c:996-1005: wp != rp 时弹一个事件交给 usb_pstd_interrupt())。
     * RTOS 模式下这是 PCD 任务的活; **裸机必须由主循环自己反复调用**。
     *
     * 本工程此前从未调用过它 ⇒ 所有 VBINT/DVST/CTRT 都堆在缓冲里烂掉,
     * 表现为: 中断确实进了 ISR(实测 10 次)、USBREQ 已解析成 0x0680(GET_DESCRIPTOR),
     * 但 g_usb_pstd_data_cnt[pipe0] 恒 0、CFIFOCTR 恒 0, 描述符永远发不出去。
     *
     * 一次调用只处理 1 个事件, 故循环若干次把缓冲排空(空时是 no-op, 安全)。
     * 纯软件动作: 不碰引脚/供电/总线。 */
    USB_CDC_Poll();

    g_usb_ispr0    = *(volatile uint32_t *) 0xE000E200UL;                       /* NVIC ISPR0 */
    g_usb_elcr     = *(volatile uint32_t *) 0x40082000UL;                       /* ELC ELCR  (base 0x40082000!) */
    g_usb_vt_usbfs = *(volatile uint32_t *) (vtor + 0x40UL);                    /* 向量表 IRQ0 入口 */
    g_usb_ielsr0   = *(volatile uint32_t *) 0x40006300UL;                       /* ICU IELSR0 */
    g_usb_iser0    = *(volatile uint32_t *) 0xE000E100UL;                       /* NVIC ISER0 */
    g_usb_intsts0  = (uint32_t) (*(volatile uint16_t *) 0x40090040UL);          /* USBFS INTSTS0 */
    g_usb_intenb0  = (uint32_t) (*(volatile uint16_t *) 0x40090030UL);          /* USBFS INTENB0 */
    g_usb_vbsts    = (g_usb_intsts0 >> 7) & 0x1u;                               /* INTSTS0.VBSTS */
    g_usb_syscfg   = (uint32_t) (*(volatile uint16_t *) 0x40090000UL);          /* USBFS SYSCFG */
    /* 控制管道的实时状态：pending request 非 0 = 还有控制传输没收尾。 */
    g_usb_pipe0_req = (uint32_t) g_usb_pstd_pipe0_request;
    g_usb_dcnt0     = (uint32_t) g_usb_pstd_data_cnt[0];

#if (1 == USB_CDC_SETUP_KICK)
    /* ⚠ 死锁解锁: 本板实测控制器收下 SETUP 后只置 VALID + CTSQ=RDDS, **不产生 CTRT**。
     * 而 FSP 只在 CTRT 分支里调 usb_pstd_save_request() 去清 VALID ⇒ 互相死等。
     * 这里在"SETUP 已就位但从未被取走"时主动清 VALID, 期待控制器随即补发 CTRT,
     * 让 FSP 走正常的 stand_req1(GET_DESCRIPTOR) 流程。 */
    if ((0u != (g_usb_intsts0 & 0x0008u))                     /* VALID   */
        && (0x0001u == (g_usb_intsts0 & 0x0007u))             /* CTSQ=RDDS */
        && (0u == (uint32_t) g_usb_pstd_req_type))            /* 还没解析过 */
    {
        usb_pstd_save_request(0u);
        g_usb_setup_kick++;
    }
#endif
}

fsp_err_t USB_CDC_Write(uint8_t const * p_src, uint32_t len)
{
    uint32_t i;

    if ((false == s_configured) || (0u == len))
    {
        return FSP_ERR_INVALID_STATE;
    }
    if (len > USB_CDC_TX_STAGE)
    {
        len = USB_CDC_TX_STAGE;
    }

    for (i = 0u; i < len; i++)
    {
        s_tx_stage[i] = p_src[i];
    }

    g_usb_tx_bytes += len;

    return R_USB_Write(&g_basic0_ctrl, s_tx_stage, len, USB_CLASS_PCDC);
}
