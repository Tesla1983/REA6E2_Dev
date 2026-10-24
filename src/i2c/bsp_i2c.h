/* ============================================================
 * I2C 总线封装 —— 基于 FSP r_iic_b_master
 *
 * 硬件: RA6E2 的 IIC0, 引脚 P408(SCL0_B) / P206(SDA0_C)
 *       引脚复用由 ra_gen/pin_data.c 配置为 IOPORT_PERIPHERAL_IIC(开漏), 非推挽
 *       时钟: I2C_MASTER_RATE_STANDARD = 100 kHz
 *
 * 句柄来源: 本工程 FSP 根是官方 BH1750 示例, 其 ra_gen/hal_data.c 生成的
 *           I2C 实例名就叫 I2C_BH1750(BH1750 传感器用的那一路), 名字沿用不改。
 *
 * 注意: I2C 传输完成靠回调置标志, 不用死等固定延时。
 * ============================================================ */
#ifndef BSP_I2C_H_
#define BSP_I2C_H_

#include <stdbool.h>
#include <stdint.h>
#include "bsp_api.h"

/* ---- 总线层诊断计数(volatile, 便于探针直接读) ---- */

/** 传输失败累计次数(重试前的原始失败次数)。持续增长说明总线不稳定。 */
extern volatile uint32_t g_i2c_err_count;
/** 已执行的重试次数。err_count 大但 last_err == 0 => 重试全部救回。 */
extern volatile uint32_t g_i2c_retry_count;
/** 最后一次(重试后仍然)失败的错误码; 0 = 至今没有失败过的传输 */
extern volatile uint8_t  g_i2c_last_err;
/** 上电/复位时检测到 SDA 被从机拉低(总线锁死)的次数。>0 说明复位确实撞上了传输中途。 */
extern volatile uint8_t  g_i2c_bus_stuck;
/** 总线恢复序列成功把 SDA 拉回高电平的次数。应与 bus_stuck 相等才说明每次都救回来了。 */
extern volatile uint8_t  g_i2c_bus_recover;

/** 初始化 I2C 主机(Open + 安装回调)。使用任何其它接口前必须先调用。 */
fsp_err_t I2C_Bus_Init(void);

/** 向 7 位地址 slave7 写 len 字节, 结束后发停止位。内部等待传输完成。 */
fsp_err_t I2C_Write(uint8_t slave7, const uint8_t * p_buf, uint32_t len);

/** 从 7 位地址 slave7 读 len 字节。内部等待传输完成。 */
fsp_err_t I2C_Read(uint8_t slave7, uint8_t * p_buf, uint32_t len);

/** 探测 7 位地址上是否有器件应答(只发地址+1 个空操作字节, 不写有效数据)。 */
bool I2C_Probe(uint8_t slave7);

#endif /* BSP_I2C_H_ */
