/* ============================================================
 * 片上温度传感器 TSN (Temperature Sensor) 读取模块 —— RA6E2 / FSP 6.6.0
 *
 * RA6E2 内部集成了模拟温度传感器, 输出送内部 12-bit ADC12 的专用通道
 * (ADC_CHANNEL_TEMPERATURE = -3 -> ADTSDR)。
 *
 * 出厂校准: 硬件手册 §38 确认, TSCDR 存的是芯片在
 *   Tj = 127 °C、AVCC0 = VREFH0 = 3.3 V 下的 12-bit 校准值 (CAL127)。
 * 斜率: BSP_FEATURE_TSN_SLOPE = 4000 µV/°C (负温度系数: 温度越高电压越低)。
 *
 * 换算 (VREFH0 = 3.3 V 与校准条件一致):
 *   本芯片实测 raw 随温度上升而上升 (正系数), 斜率恰为规格值 4 mV/°C,
 *   以 127 °C 处出厂校准值 CAL127 为基准:
 *   T = 127 + (raw - CAL127) * 3.3 / (4096 * 0.004)        [°C]
 *     = 127 + (raw - CAL127) * 0.201416
 *   整数(0.1 °C)版:
 *   T_x10 = 1270 + (raw - CAL127) * 33000 / 16384
 *
 * 注意: 校准数据本身就是在 3.3 V 下测的, 当前 ADC 参考也是 3.3 V,
 *   所以直接用 0.201416 系数才正确; 若用 4.096 V 假设的 /4 公式会偏大约 24%。
 *
 * FSP 的 r_adc 驱动在 ScanCfg 发现 ADC_MASK_TEMPERATURE 时会自动置
 *   TSCR.TSEN + TSCR.TSOE 使能传感器, InfoGet 会自动读 TSCDR 与斜率,
 *   因此本模块无需手动操作 TSN 寄存器。
 * ============================================================ */
#ifndef BSP_TSN_H_
#define BSP_TSN_H_

#include <stdint.h>
#include "bsp_api.h"

/* 供调试/日志用的状态, volatile 便于探针直接读 */
extern volatile uint16_t g_tsn_raw;     /* 最近一次原始 ADC 读数 (12-bit) */
extern volatile uint32_t g_tsn_calib;   /* TSCDR 出厂校准值 (CAL127) */
extern volatile int16_t  g_tsn_slope;   /* 斜率 µV/°C (应为 4000) */

/**
 * 初始化 ADC + 配置温度传感器单通道扫描。
 * @return FSP_SUCCESS 成功; 其他为 FSP 错误码
 */
fsp_err_t TSN_Init (void);

/**
 * 触发一次温度转换并读取, 换算为 0.1 °C 为单位的整数。
 *
 * @param[out] p_temp_x10  温度值, 单位 0.1 °C (例如 253 = 25.3 °C)
 * @return FSP_SUCCESS 成功; 其他为 FSP 错误码
 */
fsp_err_t TSN_ReadTemperature (int32_t * p_temp_x10);

#endif /* BSP_TSN_H_ */
