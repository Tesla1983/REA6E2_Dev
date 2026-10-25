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
            uint8_t         i;

            for (i = 0u; i < arg_int; i++)
            {
                uint8_t b = p[i];

                /* 页地址补偿: 命令 0xB0..0xB7 = "set page"。
                 * 本模块 RAM→玻璃固定上移 32 行（= 4 页），起始行命令对它无效，
                 * 只能在这里把页号转一下。见 bsp_nt7534.h 的 NT7534_PAGE_ROT。
                 * 0xB8..0xBF 是别的命令, 不能当页号处理, 所以用掩码精确匹配。 */
                if ((0u != s_is_cmd) && (0xB0u == (b & 0xF8u)))
                {
                    b = (uint8_t) (0xB0u | (((uint8_t) (b + NT7534_PAGE_ROT)) & 0x07u));
                }

                NT7534_BusWrite(b);
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

    /* 5) 清一次屏, 免得残留上电随机点 */
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
    u8g2_SendBuffer(&s_u8g2);
    g_u8g2_frames++;
}

void U8G2_Port_FlushArea(uint8_t tx, uint8_t ty, uint8_t tw, uint8_t th)
{
    /* 只发送指定 tile 区域: 风扇动画每帧只刷右下两格(约 288B vs 全屏 1024B)。 */
    u8g2_UpdateDisplayArea(&s_u8g2, tx, ty, tw, th);
    g_u8g2_frames++;
}
