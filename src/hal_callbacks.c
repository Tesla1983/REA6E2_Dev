/* ============================================================
 * FSP 生成代码要求的回调函数
 *
 * ra_gen/hal_data.c 里写死了两个回调符号, 只要 hal_data.c 参与链接
 * (一旦代码引用了 I2C / UART 句柄就会) 这两个符号就必须存在, 否则链接报未定义:
 *
 *   .p_callback = BH1750_callback   -> I2C 主机传输回调, 定义在 src/i2c/bsp_i2c.c
 *   .p_callback = uart0_callback    -> SCI0 UART 回调, 定义在下面
 *
 * 名字沿用官方 BH1750 示例工程里的模块名, 不是我们起的。
 * SCI0 (P100 RXD0 / P101 TXD0) 现在真的在用: 接收上位机每秒一帧的监控数据。
 * 逻辑全部在 src/uart/bsp_uart_link.c, 这里只做转发（跑在中断上下文, 必须极短）。
 * ============================================================ */

#include "hal_data.h"
#include "uart/bsp_uart_link.h"

void uart0_callback (uart_callback_args_t * p_args)
{
    UART_Link_OnIsr(p_args);
}
