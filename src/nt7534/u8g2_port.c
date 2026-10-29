/* ============================================================
 * u8g2 → 本板（RA6E2 软件 SPI + NT7534）移植层
 *
 * 目的: 不再自己写画点/画字/画框。u8g2 官方支持列表里有 NT7534，
 *       `u8x8_d_nt7534_tg12864r` 就是给 12864(STN) 这种屏写的驱动，
 *       连"这块玻璃行方向整体错位"都在它的注释里写着。
 *
 * 分工:
 *   u8g2        —— 帧缓冲、字库、线段/矩形/圆/位图、UTF-8、任意字号
 *   本文件      —— 只做三件事:
 *                  ① 把 u8g2 的"一个字节"转成软件 SPI 时序
 *                  ② 把 u8g2 的 CS / RS(DC) / RESET 电平转到引脚
 *                  ③ 页地址补偿（写第 p 页实际写 p+NT7534_PAGE_ROT）
 *   bsp_nt7534  —— 引脚配置 + 硬复位 + 我们实测定的初始化序列（0xA1/0xC0/列偏移 3）
 *
 * ⚠ 不调用 u8g2_InitDisplay():
 *   u8g2 内置的 NT7534 初始化序列是 0xA0/0xC8/起始行 0x60，方向与这块屏相反，
 *   而起始行命令对本模块又不起作用（实测两次）。所以硬件初始化仍旧走
 *   NT7534_Init()，只借 u8g2 的缓冲与绘图。
 * ============================================================ */

#include "nt7534/u8g2_port.h"

#include "hal_data.h"                  /* g_ioport_ctrl */
#include "nt7534/bsp_nt7534.h"

/** 全局唯一的 u8g2 句柄。全缓冲 128x64 = 1024 B（由 u8g2_m_16_8_f 提供）。 */
static u8g2_t s_u8g2;

/** 已刷新帧数。u8g2 路径不走旧驱动的 nt_data, 所以 g_nt_bytes_sent 不涨;
 *  探针读这个变量在涨 ⇒ 界面确实在实时刷新（判新旧固件/是否卡死用）。 */
volatile uint32_t g_u8g2_frames = 0u;

/** u8g2 的帧缓冲（128x64/8 = 1024 B），故意做成全局符号:
 *  屏只写不读, 但有了这个数组, 探针可以直接把它 dump 出来在电脑上还原成图,
 *  等于"看不见屏也能截图", 判断排版/字符位置不必再靠目视。 */
uint8_t g_u8g2_fb[1024];

/** 当前正在发的是命令(1)还是数据(0)。u8g2 先 SET_DC 再 SEND，用它区分。 */
static uint8_t s_is_cmd = 1u;

static void pin_level(bsp_io_port_pin_t pin, uint8_t high)
{
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, pin,
                             (0u != high) ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW);
}

/* ---------------- 字节通道 ---------------- */

static uint8_t port_byte_cb(u8x8_t * u8x8, uint8_t msg, uint8_t arg_int, void * arg_ptr)
{
    (void) u8x8;

    switch (msg)
    {
        case U8X8_MSG_BYTE_INIT:
            return 1u;

        case U8X8_MSG_BYTE_START_TRANSFER:
            s_is_cmd = 1u;
            pin_level(NT7534_PIN_CS, 0u);          /* 片选有效（低） */
            return 1u;

        case U8X8_MSG_BYTE_END_TRANSFER:
            pin_level(NT7534_PIN_CS, 1u);
            return 1u;

        case U8X8_MSG_BYTE_SET_DC:
            s_is_cmd = (0u == arg_int) ? 1u : 0u;
            pin_level(NT7534_PIN_RS, arg_int);
            return 1u;

        case U8X8_MSG_BYTE_SEND:
        {
            const uint8_t * p = (const uint8_t *) arg_ptr;
            /* 整块转交给传输层: 硬件 SPI 下一次调用 = 一次 SPI 传输（一页最多 128 B）。
             * 逐字节调 NT7534_BusWrite 会把 128 字节拆成 128 次传输，开销放大两个数量级。 */
            static uint8_t s_xfer[NT7534_WIDTH];

            uint32_t  i = 0u;
            uint32_t  n = (uint32_t) arg_int;

            while (i < n)
            {
                uint32_t k = 0u;

                while ((i < n) && (k < (uint32_t) sizeof(s_xfer)))
                {
                    uint8_t b = p[i];

                    i++;

                    /* 页地址补偿: 命令 0xB0..0xB7 = "set page"。
                     * 本模块 RAM→玻璃固定上移 32 行（= 4 页），起始行命令对它无效，
                     * 只能在这里把页号转一下。见 bsp_nt7534.h 的 NT7534_PAGE_ROT。
                     * 0xB8..0xBF 是别的命令, 不能当页号处理, 所以用掩码精确匹配。 */
                    if ((0u != s_is_cmd) && (0xB0u == (b & 0xF8u)))
                    {
                        b = (uint8_t) (0xB0u | (((uint8_t) (b + NT7534_PAGE_ROT)) & 0x07u));
                    }

                    s_xfer[k] = b;
                    k++;
                }

                NT7534_BusWriteBuf(s_xfer, k);
            }
            return 1u;
        }

        default:
            return 0u;
    }
}

/* ---------------- 引脚 / 延时 ---------------- */

static uint8_t port_gpio_delay_cb(u8x8_t * u8x8, uint8_t msg, uint8_t arg_int, void * arg_ptr)
{
    (void) u8x8;
    (void) arg_ptr;

    switch (msg)
    {
        case U8X8_MSG_GPIO_AND_DELAY_INIT:
            /* 空闲电平: CS/RS/RES 高（CS 未选中、RS 命令、RES 不复位） */
            pin_level(NT7534_PIN_CS,  1u);
            pin_level(NT7534_PIN_RS,  1u);
            pin_level(NT7534_PIN_RES, 1u);
            return 1u;

        case U8X8_MSG_DELAY_MILLI:
            R_BSP_SoftwareDelay((uint32_t) arg_int, BSP_DELAY_UNITS_MILLISECONDS);
            return 1u;

        case U8X8_MSG_DELAY_10MICRO:
            R_BSP_SoftwareDelay((uint32_t) arg_int * 10u, BSP_DELAY_UNITS_MICROSECONDS);
            return 1u;

        case U8X8_MSG_DELAY_100NANO:
            /* 半位本来就 ~300 ns, 再插空转只会更慢; 直接当成"已满足" */
            return 1u;

        case U8X8_MSG_GPIO_RESET:
            /* 复位低有效: arg_int = 1 表示"处于复位" */
            pin_level(NT7534_PIN_RES, (0u == arg_int) ? 1u : 0u);
            return 1u;

        case U8X8_MSG_GPIO_CS:
            pin_level(NT7534_PIN_CS, arg_int);
            return 1u;

        case U8X8_MSG_GPIO_DC:
            pin_level(NT7534_PIN_RS, arg_int);
            return 1u;

        default:
            return 0u;
    }
}

/* ---------------- 对外接口 ---------------- */

u8g2_t * U8G2_Port_Init(void)
{

    /* 1) 硬件: 引脚方向 -> 硬复位 -> 初始化序列 -> 0xAF 开显示。
     *    这套序列是实测调出来的（方向 0xA1/0xC0、对比度、电荷泵延时），
     *    不能让 u8g2 的通用序列覆盖。 */
    (void) NT7534_Init();

    /* 2) 绑定回调（只设结构体, 不发命令） */
    u8g2_SetupDisplay(&s_u8g2, u8x8_d_nt7534_tg12864r, u8x8_cad_001,
                      port_byte_cb, port_gpio_delay_cb);

    /* 3) 全缓冲 16 列 x 8 页 = 128x64, 用我方提供的全局数组（便于探针 dump） */
    u8g2_SetupBuffer(&s_u8g2, g_u8g2_fb, 8u,
                     u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);

    /* 4) 列偏移: 可见区从控制器 RAM 第 3 列开始（实测值）。
     *    u8g2 的 lm6059 信息里默认给 1, 那是别的模块, 这里必须覆盖。 */
    s_u8g2.u8x8.x_offset = (uint8_t) NT7534_COL_OFFSET;

    /* 5) 位图透明模式: 只画置位像素, 不清矩形背景。
     *    u8g2 默认 bitmap_transparency=0(不透明), DrawXBMP 会把 XBM 的 0 位涂成背景色 ⇒
     *    恐龙 44x47 / 风扇矩形的"空位"会擦掉底下的地面线、表格线、正从恐龙身后路过的仙人掌,
     *    表现为"恐龙有矩形边缘""地平线消失""仙人掌被切断"。
     *    字体早已用 SetFontMode(1) 解决同类问题, 位图此前漏了 —— 这里补上。 */
    u8g2_SetBitmapMode(&s_u8g2, 1u);

    /* 6) 清一次屏, 免得残留上电随机点 */
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SendBuffer(&s_u8g2);

    return &s_u8g2;
}

u8g2_t * U8G2_Port(void)
{
    return &s_u8g2;
}

void U8G2_Port_Flush(void)
{
    /* 每帧重申方向 + 起始行（3 字节, 约 0.3% 带宽）。
     * 屏只写不读, 控制器状态一旦被打乱没法察觉, 靠这个自动恢复。 */
    NT7534_SyncGeometry();

#if (NT7534_BUS_MODE == 2)
    /* S3: 异步分页刷新, 不阻塞主循环(见 NT7534_Pump_Poll)。 */
    NT7534_Pump_Start(0u, (uint8_t) (NT7534_PAGES - 1u));
#else
    u8g2_SendBuffer(&s_u8g2);
#endif
    g_u8g2_frames++;
}

void U8G2_Port_FlushArea(uint8_t tx, uint8_t ty, uint8_t tw, uint8_t th)
{
#if (NT7534_BUS_MODE == 2)
    /* S3: 脏区 = u8g2 帧缓冲里涉及的页范围(tile 行 = 页, 每页 8 行)。
     *   ⚠ 关键: 帧缓冲第 L 页的内容实际显示在 controller 第 (L - NT7534_PAGE_ROT) & 7
     *   页(玻璃上移了 NT7534_PAGE_ROT 页, 起始行命令无效, 发送侧补偿)。所以必须把
     *   u8g2 页号反变换成 controller 物理页, 再交给泵——否则会刷错页、风扇根本不被刷新。
     *     例: 风扇画在 u8g2 页 4..7 => 物理页 (4-4..7-4)&7 = 0..3, 只刷 0..3 才能让扇叶转。
     *   列地址不必重设(u8g2 缓冲里已是最终像素, commit 整页拷贝)。 */
    uint8_t  a = ty;
    uint8_t  b = (uint8_t) (ty + th - 1u);
    uint8_t  p;
    uint32_t mask = 0u;

    (void) tx;
    (void) tw;
    if (b > (NT7534_PAGES - 1u))
    {
        b = (uint8_t) (NT7534_PAGES - 1u);
    }
    for (p = a; p <= b; p++)
    {
        uint8_t phys = (uint8_t) ((p + NT7534_PAGES - NT7534_PAGE_ROT) & 7u);
        mask |= (1u << phys);
    }
    NT7534_Pump_StartBitmap(mask);
#else
    /* 只发送指定 tile 区域: 风扇动画每帧只刷右下两格(约 288B vs 全屏 1024B)。 */
    u8g2_UpdateDisplayArea(&s_u8g2, tx, ty, tw, th);
#endif
    g_u8g2_frames++;
}
