/* ============================================================
 * 片上温度传感器 TSN 读取模块实现 —— RA6E2 / FSP 6.6.0
 *
 * 直接操作 ADC12 + TSN 寄存器 (不依赖 r_adc.c, 因为 RASC 生成的工程树
 * 不含 ADC 驱动源码)。寄存器定义来自设备头 R7FA6E2BB.h。
 *
 * 配置序列 (与 FSP r_adc 驱动等价):
 *   1) 打开 ADC / TSN 外设时钟 (清 MSTP)
 *   2) TSCR.TSEN + TSCR.TSOE 使能传感器输出到 ADC
 *   3) ADSSTRT = 传感器采样状态 (按 PCLKC 频率计算, 满足 >=4.15 us)
 *   4) ADEXICR.TSSA 把温度传感器选为 Group-A 扩展输入
 *   5) ADCSR = ADST(软件触发单扫描) 启动, 等 ADST 自动清 0, 读 ADTSDR
 *
 * 换算 (VREFH0 = 3.3 V, 与出厂校准条件一致):
 *   T      = 127 + (CAL127 - raw) * 3.3 / (4096 * 0.004)            [°C]
 *   T_x10  = 1270 + (CAL127 - raw) * 33000 / 16384
 * ============================================================ */
#include "tsn/bsp_tsn.h"

#include <stdbool.h>
#include <stddef.h>

#include "bsp_api.h"     /* R_BSP_MODULE_START / R_BSP_SoftwareDelay /
                             R_FSP_SystemClockHzGet / FSP_IP_ADC / FSP_IP_TSN /
                             BSP_FEATURE_TSN_SLOPE / R7FA6E2BB 寄存器定义 */

/* ---- 运行时状态 (volatile, 便于探针直接读) ---- */
volatile uint16_t g_tsn_raw   = 0U;
volatile uint32_t g_tsn_calib = 0U;
volatile int16_t  g_tsn_slope = 4000;

/* ---- 缓存 (避免每次读数读 TSCDR) ---- */
static uint32_t g_tsn_calib_cached = 0U;
static int32_t  g_tsn_slope_cached = 4000;
static bool     g_tsn_inited       = false;

fsp_err_t TSN_Init (void)
{
    if (g_tsn_inited)
    {
        return FSP_SUCCESS;
    }

    /* 1) 打开外设时钟 (清 module-stop) */
    R_BSP_MODULE_START(FSP_IP_ADC, 0U);
    R_BSP_MODULE_START(FSP_IP_TSN, 0U);

    /* 2) 使能温度传感器: TSEN(bit7) + TSOE(bit4) */
    R_TSN_CTRL->TSCR = (uint8_t) (R_TSN_CTRL_TSCR_TSEN_Msk | R_TSN_CTRL_TSCR_TSOE_Msk);

    /* 3) 传感器采样状态: 满足硬件手册 >= 4.15 us 采样时间。
     *    ADSSTRT 是 8 位寄存器, 先按 FSP 公式算, 再钳到 255。
     *    ss = (MIN_SAMPLING_ns * (PCLKC_Hz / 1000)) / 1e6 + 1
     *    本板 PCLKC = 200MHz/4 = 50MHz -> ss ≈ 208, 落在 8 位内。 */
    uint32_t freq_hz = R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_PCLKC);
    uint32_t ss      = ((4150UL * (freq_hz / 1000UL)) / 1000000UL) + 1UL;
    if (ss > 255UL)
    {
        ss = 255UL;
    }
    R_ADC0->ADSSTRT = (uint8_t) ss;

    /* 4) 把温度传感器选为 Group-A 扩展输入 (单扫描模式扫 Group-A)。
        ADANSA[0] 复位值即为 0, 不扫任何普通通道。 */
    R_ADC0->ADEXICR = (uint16_t) R_ADC0_ADEXICR_TSSA_Msk;

    /* ADC 停在单扫描/软件触发 (ADCS=0, TRGE=0) */
    R_ADC0->ADCSR = 0U;

    /* 5) 缓存出厂校准值 (TSCDR 低 12 位有效, CAL32 掩码 0xFFFF) */
    g_tsn_calib_cached = R_TSN_CAL->TSCDR & 0xFFFFUL;
    g_tsn_slope_cached = (int32_t) BSP_FEATURE_TSN_SLOPE;
    g_tsn_calib        = g_tsn_calib_cached;
    g_tsn_slope        = (int16_t) g_tsn_slope_cached;

    /* 传感器使能后等输出建立 (硬件手册建议短暂延时) */
    R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);

    g_tsn_inited = true;
    return FSP_SUCCESS;
}

fsp_err_t TSN_ReadTemperature (int32_t * p_temp_x10)
{
    if (NULL == p_temp_x10)
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    if (!g_tsn_inited)
    {
        fsp_err_t e = TSN_Init();
        if (FSP_SUCCESS != e)
        {
            return e;
        }
    }

    /* 软件触发单扫描: ADCS=0(单扫描) TRGE=0(软件) ADST=1 启动。
       转换完成硬件自动清 ADST。 */
    R_ADC0->ADCSR = (uint16_t) R_ADC0_ADCSR_ADST_Msk;

    /* 轮询等待转换完成 (ADST 清 0) */
    while (0U != (R_ADC0->ADCSR & R_ADC0_ADCSR_ADST_Msk))
    {
        /* busy wait, 温度转换只需几十微秒 */
    }

    /* ADTSDR 低 12 位为转换结果 (12-bit 右对齐) */
    uint16_t raw = (uint16_t) (R_ADC0->ADTSDR & 0x0FFFU);
    g_tsn_raw   = raw;

    /* ---- 温度换算 (VREFH0 = 3.3 V) ----
     *   本芯片实测: raw 随温度上升而上升 (正系数), 斜率恰为规格值 4 mV/°C。
     *   以 127 °C 处的出厂校准值 CAL127 为基准:
     *     T      = 127 + (raw - CAL127) * 3.3 / (4096 * 0.004)          [°C]
     *     T_x10  = 1270 + (raw - CAL127) * 33000 / 16384
     *   (注意: 早期按"负系数"写成 CAL127 - raw 会得到不可能的 ~217 °C,
     *    实测数据自洽地要求用 raw - CAL127, 才能给出合理的芯片结温。) */
    int32_t diff     = (int32_t) raw - (int32_t) g_tsn_calib_cached;
    int32_t temp_x10 = 1270 + (diff * 33000) / 16384;

    *p_temp_x10 = temp_x10;

    return FSP_SUCCESS;
}
