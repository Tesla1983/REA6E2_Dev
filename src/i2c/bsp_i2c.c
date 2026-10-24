/* ============================================================
 * I2C 总线封装实现
 *
 * 调用链: I2C_Write/Read -> R_IIC_B_MASTER_Write/Read -> 中断驱动收发
 *         -> ISR 内回调 BH1750_callback() -> 置 s_xfer_done
 *
 * BH1750_callback 的名字不是我们起的: 官方 BH1750 示例的 ra_gen/hal_data.c
 * 里写死了 .p_callback = BH1750_callback。只要 hal_data.c 参与链接(一旦代码
 * 引用了 I2C 句柄就会), 这个符号必须由我们提供, 否则链接报未定义。
 * 名字沿用示例工程的模块名, 语义上它就是"本工程 I2C 主机传输回调"。
 * ============================================================ */

#include "i2c/bsp_i2c.h"

#include "hal_data.h"                  /* I2C_BH1750_ctrl / I2C_BH1750_cfg */

/** 等待轮询粒度(µs)。
 *  原来是 1 ms: 传输往往在几十 µs 内完成, 却要等到下一次 1 ms 查询才被看见,
 *  每笔平均浪费约 0.5 ms。改造前每帧 16 笔 => 8 ms, 占 108 ms 帧时间的 7%;
 *  提速到 400 kHz + 脏页后总时长只剩 9 ms, 这 8 ms 会变成主要开销, 必须先削掉。
 *  取 100 µs: 每笔平均浪费降到 50 µs, 4 笔共 0.2 ms。 */
#define I2C_WAIT_TICK_US       (100u)

/** 单次传输的等待上限(µs)。100 kHz 下 129 字节约 12 ms, 留足余量。 */
#define I2C_WAIT_TIMEOUT_US    (200000u)

/** 从机 NACK(ABORTED)时的重试次数。
 *  OLED 刷新时一帧要连发 16 笔事务(8 次设页命令 + 8 次页数据), 间隔极短,
 *  SH1106 偶尔会在上一笔刚结束时就 NACK 下一笔 —— 实测确有发生, 故重试一次。 */
#define I2C_MAX_RETRY          (1u)

volatile uint32_t g_i2c_err_count   = 0U;
volatile uint32_t g_i2c_retry_count = 0U;
volatile uint8_t  g_i2c_last_err    = 0U;

/** 上电/复位时检测到 SDA 被从机拉低(总线锁死)的次数 */
volatile uint8_t  g_i2c_bus_stuck    = 0U;
/** 总线恢复序列成功把 SDA 拉回高电平的次数 */
volatile uint8_t  g_i2c_bus_recover  = 0U;

/** 总线恢复用的 SCL / SDA 引脚。
 *  必须与 ra_gen/pin_data.c 里 IIC0 的分配一致：
 *    P408 = SCL0_B （SCL）
 *    P206 = SDA0_C （SDA，2026-10-23 由 P407 迁来；P407 让给 USB_VBUS）
 *
 *  ⚠ 2026-10-23 修正：此处原先写成 "P407=SCL / P408=SDA"，与数据手册引脚表
 *    （P407 = SDA0_B、P408 = SCL0_B）正好相反，导致总线恢复序列拉错脚，
 *    根本起不到恢复作用。现已按实际信号改正。 */
#define I2C_RECOVER_SCL_PIN   ((bsp_io_port_pin_t) BSP_IO_PORT_04_PIN_08)
#define I2C_RECOVER_SDA_PIN   ((bsp_io_port_pin_t) BSP_IO_PORT_02_PIN_06)

/** 恢复序列里每半周期时长(µs)。
 *  100 kHz 下 SCL 半周期 5 µs; 取 5 µs 保证任何速率的从机都能认出这是时钟沿。
 *  9 个脉冲 + STOP 合计约 110 µs, 对启动流程可忽略。 */
#define I2C_RECOVER_HALF_US   (5u)

/** 引脚还原成 IIC 外设功能时的配置值。
 *  必须与 ra_gen/pin_data.c 里 P408/P206 的 .pin_cfg 完全一致, 否则
 *  R_IIC_B_MASTER_Open 之后 I2C 外设拿不到这两个脚。 */
#define I2C_PIN_CFG_IIC       ((uint32_t) ((uint32_t) IOPORT_CFG_PERIPHERAL_PIN \
                                         | (uint32_t) IOPORT_PERIPHERAL_IIC))

static volatile bool  s_xfer_done;     /* 传输结束(成功或中止)标志, ISR 中置位 */
static volatile uint8_t s_xfer_event;  /* 结束事件: I2C_MASTER_EVENT_* */
static uint8_t        s_cur_slave;     /* 当前已设置的从机地址, 避免重复下发 */

/* ------------------------------------------------------------
 * I2C 传输完成回调 —— 由 ISR 调用
 * ------------------------------------------------------------ */
void BH1750_callback (i2c_master_callback_args_t * p_args)
{
    if (NULL != p_args)
    {
        if ((I2C_MASTER_EVENT_ABORTED     == p_args->event) ||
            (I2C_MASTER_EVENT_RX_COMPLETE == p_args->event) ||
            (I2C_MASTER_EVENT_TX_COMPLETE == p_args->event))
        {
            s_xfer_event = (uint8_t) p_args->event;
            s_xfer_done  = true;
        }
    }
}

/* ------------------------------------------------------------
 * 等待一次传输结束
 * ------------------------------------------------------------ */
static bool i2c_wait_done (void)
{
    uint32_t waited_us = 0U;

    while (!s_xfer_done)
    {
        if (waited_us >= I2C_WAIT_TIMEOUT_US)
        {
            return false;
        }

        R_BSP_SoftwareDelay(I2C_WAIT_TICK_US, BSP_DELAY_UNITS_MICROSECONDS);
        waited_us += I2C_WAIT_TICK_US;
    }

    return true;
}

/* ------------------------------------------------------------
 * 启动一次传输前的公共准备
 * ------------------------------------------------------------ */
static fsp_err_t i2c_begin (uint8_t slave7)
{
    fsp_err_t err;

    if (slave7 != s_cur_slave)
    {
        err = R_IIC_B_MASTER_SlaveAddressSet(&I2C_BH1750_ctrl,
                                             (uint32_t) slave7,
                                             I2C_MASTER_ADDR_MODE_7BIT);
        if (FSP_SUCCESS != err)
        {
            return err;
        }

        s_cur_slave = slave7;
    }

    s_xfer_done  = false;
    s_xfer_event = 0U;

    return FSP_SUCCESS;
}

/* ------------------------------------------------------------
 * 判定一次传输的结果
 * ------------------------------------------------------------ */
static fsp_err_t i2c_end (fsp_err_t err)
{
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    if (!i2c_wait_done())
    {
        return FSP_ERR_TIMEOUT;
    }

    if (I2C_MASTER_EVENT_ABORTED == s_xfer_event)
    {
        return FSP_ERR_ABORTED;
    }

    return FSP_SUCCESS;
}

/* ------------------------------------------------------------
 * I2C 总线恢复(bus clear)
 * ------------------------------------------------------------ */

/**
 * 把引脚临时切成"GPIO 开漏输出 + 内部上拉"。
 *
 * 【安全性】只使能 NMOS 开漏, 高电平是**释放**(由上拉电阻拉高)而不是推挽驱动。
 * 拉低时电流 = 3.3V / R_pullup —— 内部上拉典型 30~50 kΩ 约 0.1 mA,
 * 即便模块自带 4.7 kΩ 外部上拉也只有约 0.7 mA, 远低于引脚与 USB 额定值。
 * 不存在"把 GPIO 设成推挽去强灌"的情形。
 */
static void i2c_pin_gpio_od (bsp_io_port_pin_t pin)
{
    (void) R_IOPORT_PinCfg(&g_ioport_ctrl, pin,
                           (uint32_t) (IOPORT_CFG_PORT_DIRECTION_OUTPUT
                                     | IOPORT_CFG_NMOS_ENABLE
                                     | IOPORT_CFG_PULLUP_ENABLE));
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, pin, BSP_IO_LEVEL_HIGH);
}

/**
 * 解除总线锁死。
 *
 * 【为什么需要】MCU 在运行中被复位(按板载 RST 键、或 probe-rs reset)时,
 * 如果正好打断一次 I2C 传输, SH1106 可能仍把 SDA 拉低等待剩下的时钟。
 * MCU 复位后 SDA 一直为低, 主机发不出 START 条件:
 *      I2C_Probe() 扫不到 0x3C -> g_oled_addr = 0x00
 *      -> OLED_Init 返回失败 -> 程序进"LED 快闪"死循环
 * 屏幕停在上电最后一帧, 表现就是"按了 RST 屏幕卡住不动"。
 *
 * 【做法】I2C 规范的 bus clear: SDA 保持高的同时打最多 9 个 SCL 脉冲,
 * 让从机把当前字节走完并释放 SDA; 再补一个 STOP 条件把总线复位到空闲态。
 *
 * ⚠ 必须在 R_IIC_B_MASTER_Open() **之前**调用 —— Open 会把 P408/P206
 *   切成 IIC 外设功能, 之后就没法用 GPIO 方式打脉冲了。
 */
static void i2c_bus_recovery (void)
{
    bsp_io_level_t sda = BSP_IO_LEVEL_HIGH;
    bsp_io_level_t scl = BSP_IO_LEVEL_HIGH;
    uint32_t       i;

    i2c_pin_gpio_od(I2C_RECOVER_SCL_PIN);
    i2c_pin_gpio_od(I2C_RECOVER_SDA_PIN);
    R_BSP_SoftwareDelay(I2C_RECOVER_HALF_US, BSP_DELAY_UNITS_MICROSECONDS);

    (void) R_IOPORT_PinRead(&g_ioport_ctrl, I2C_RECOVER_SDA_PIN, &sda);
    (void) R_IOPORT_PinRead(&g_ioport_ctrl, I2C_RECOVER_SCL_PIN, &scl);

    /* 两种情况都会让主机发不出 START, 都要打脉冲:
     *   SDA 低 = 从机正拉着数据线等剩下的时钟(总线锁死)
     *   SCL 低 = 从机在做时钟延展(clock stretching)时被复位打断, 一直拉着 SCL
     * ⚠ 只看 SDA 会漏掉第二种 —— 那正是单纯读 SDA 检测不到(g_i2c_bus_stuck 恒为 0)的原因。 */
    if ((BSP_IO_LEVEL_LOW == sda) || (BSP_IO_LEVEL_LOW == scl))
    {
        g_i2c_bus_stuck++;

        for (i = 0U; i < 9U; i++)
        {
            (void) R_IOPORT_PinWrite(&g_ioport_ctrl, I2C_RECOVER_SCL_PIN, BSP_IO_LEVEL_LOW);
            R_BSP_SoftwareDelay(I2C_RECOVER_HALF_US, BSP_DELAY_UNITS_MICROSECONDS);
            (void) R_IOPORT_PinWrite(&g_ioport_ctrl, I2C_RECOVER_SCL_PIN, BSP_IO_LEVEL_HIGH);
            R_BSP_SoftwareDelay(I2C_RECOVER_HALF_US, BSP_DELAY_UNITS_MICROSECONDS);

            (void) R_IOPORT_PinRead(&g_ioport_ctrl, I2C_RECOVER_SDA_PIN, &sda);
            if (BSP_IO_LEVEL_HIGH == sda)
            {
                break;                /* 从机已释放 */
            }
        }

        /* STOP 条件: SCL 为高期间 SDA 由低变高 */
        (void) R_IOPORT_PinWrite(&g_ioport_ctrl, I2C_RECOVER_SDA_PIN, BSP_IO_LEVEL_LOW);
        R_BSP_SoftwareDelay(I2C_RECOVER_HALF_US, BSP_DELAY_UNITS_MICROSECONDS);
        (void) R_IOPORT_PinWrite(&g_ioport_ctrl, I2C_RECOVER_SCL_PIN, BSP_IO_LEVEL_HIGH);
        R_BSP_SoftwareDelay(I2C_RECOVER_HALF_US, BSP_DELAY_UNITS_MICROSECONDS);
        (void) R_IOPORT_PinWrite(&g_ioport_ctrl, I2C_RECOVER_SDA_PIN, BSP_IO_LEVEL_HIGH);
        R_BSP_SoftwareDelay(I2C_RECOVER_HALF_US, BSP_DELAY_UNITS_MICROSECONDS);

        (void) R_IOPORT_PinRead(&g_ioport_ctrl, I2C_RECOVER_SDA_PIN, &sda);
        if (BSP_IO_LEVEL_HIGH == sda)
        {
            g_i2c_bus_recover++;
        }
    }

    /* ⚠ 必须还原成 IIC 外设功能 —— 这是最容易踩的一脚:
       上面把 PFS 改成了 GPIO, 而 pin_data.c 的配置只在启动 R_IOPORT_Open 时应用一次,
       R_IIC_B_MASTER_Open() **不会**重新配 PFS。不还原的话 I2C 外设根本拿不到这两个脚,
       现象是 I2C_Probe 恒扫不到器件(g_oled_addr 一直是 0x00), 比不恢复还糟。 */
    (void) R_IOPORT_PinCfg(&g_ioport_ctrl, I2C_RECOVER_SCL_PIN, I2C_PIN_CFG_IIC);
    (void) R_IOPORT_PinCfg(&g_ioport_ctrl, I2C_RECOVER_SDA_PIN, I2C_PIN_CFG_IIC);
}

/* ------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------ */
fsp_err_t I2C_Bus_Init (void)
{
    /* 先清总线再开 I2C: 上电或运行中复位都可能撞上从机还拉着 SDA/SCL 的情况 */
    i2c_bus_recovery();

    fsp_err_t err = R_IIC_B_MASTER_Open(&I2C_BH1750_ctrl, &I2C_BH1750_cfg);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    /* 生成的 cfg 已把 p_callback 指向 BH1750_callback, 这里显式再确认一次 */
    err = R_IIC_B_MASTER_CallbackSet(&I2C_BH1750_ctrl, BH1750_callback, NULL, NULL);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    s_cur_slave = I2C_BH1750_cfg.slave;

    return FSP_SUCCESS;
}

fsp_err_t I2C_Write (uint8_t slave7, const uint8_t * p_buf, uint32_t len)
{
    fsp_err_t err;
    uint32_t  attempt;

    for (attempt = 0U; attempt <= I2C_MAX_RETRY; attempt++)
    {
        err = i2c_begin(slave7);
        if (FSP_SUCCESS != err)
        {
            return err;
        }

        /* 驱动接口不是 const 限定的, 这里去掉 const 再传 */
        err = R_IIC_B_MASTER_Write(&I2C_BH1750_ctrl, (uint8_t *) p_buf, len, false);
        err = i2c_end(err);

        if (FSP_SUCCESS == err)
        {
            return FSP_SUCCESS;
        }

        g_i2c_err_count++;
        g_i2c_last_err = (uint8_t) err;

        /* 只有「从机没应答」这一类才值得重试; 超时/参数错等重试也没用 */
        if (FSP_ERR_ABORTED != err)
        {
            return err;
        }

        if (attempt < I2C_MAX_RETRY)
        {
            /* 给从机一点喘息时间再重发同一笔 */
            R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MILLISECONDS);
            g_i2c_retry_count++;
        }
    }

    return err;
}

fsp_err_t I2C_Read (uint8_t slave7, uint8_t * p_buf, uint32_t len)
{
    fsp_err_t err = i2c_begin(slave7);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = R_IIC_B_MASTER_Read(&I2C_BH1750_ctrl, p_buf, len, false);

    return i2c_end(err);
}

bool I2C_Probe (uint8_t slave7)
{
    uint8_t  probe_byte = 0x00U;   /* OLED 的"命令模式"控制字节, 后面不跟命令字节, 停在中途为空操作 */
    fsp_err_t err;

    err = I2C_Write(slave7, &probe_byte, 1U);

    return (FSP_SUCCESS == err);
}
