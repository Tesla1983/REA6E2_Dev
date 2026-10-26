/* generated pin source file - do not edit */
#include "bsp_api.h"
#include "r_ioport.h"




const ioport_pin_cfg_t g_bsp_pin_cfg_data[] = {
    
    {
        .pin = BSP_IO_PORT_01_PIN_00,
        .pin_cfg = ((uint32_t) IOPORT_CFG_PERIPHERAL_PIN | (uint32_t) IOPORT_PERIPHERAL_SCI0_2_4_6_8)
    },
    
    {
        .pin = BSP_IO_PORT_01_PIN_01,
        .pin_cfg = ((uint32_t) IOPORT_CFG_PERIPHERAL_PIN | (uint32_t) IOPORT_PERIPHERAL_SCI0_2_4_6_8)
    },
    
    {
        .pin = BSP_IO_PORT_01_PIN_08,
        .pin_cfg = ((uint32_t) IOPORT_CFG_PERIPHERAL_PIN | (uint32_t) IOPORT_PERIPHERAL_DEBUG)
    },
    
    /* [2026-10-23] IIC0-SDA 由 P407 改为 P206：
     *   P407 是 RA6E2 唯一的 USB_VBUS 检测脚，必须让给 USB；
     *   数据手册引脚表：P408 = SCL0_B，P206 = SDA0_C（两者同为 IIC0 信号，可独立选脚）。
     *   硬件侧只需把 OLED 的 SDA 线从 P407 挪到 P206，SCL(P408) 不动。
     * [2026-10-26] RASC 每次 --generate 都会抹掉本注释，已手工回填两次 —— 再生成后记得检查。 */
    {
        .pin = BSP_IO_PORT_02_PIN_06,
        .pin_cfg = ((uint32_t) IOPORT_CFG_PERIPHERAL_PIN | (uint32_t) IOPORT_PERIPHERAL_IIC)
    },
    
    {
        .pin = BSP_IO_PORT_02_PIN_07,
        .pin_cfg = ((uint32_t) IOPORT_CFG_PERIPHERAL_PIN | (uint32_t) IOPORT_PERIPHERAL_SPI)
    },
    
    {
        .pin = BSP_IO_PORT_03_PIN_00,
        .pin_cfg = ((uint32_t) IOPORT_CFG_PERIPHERAL_PIN | (uint32_t) IOPORT_PERIPHERAL_DEBUG)
    },
    
    {
        .pin = BSP_IO_PORT_03_PIN_02,
        .pin_cfg = ((uint32_t) IOPORT_CFG_PERIPHERAL_PIN | (uint32_t) IOPORT_PERIPHERAL_SPI)
    },
    
    {
        .pin = BSP_IO_PORT_04_PIN_02,
        .pin_cfg = ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH)
    },
    
    {
        .pin = BSP_IO_PORT_04_PIN_08,
        .pin_cfg = ((uint32_t) IOPORT_CFG_PERIPHERAL_PIN | (uint32_t) IOPORT_PERIPHERAL_IIC)
    },
};




const ioport_cfg_t g_bsp_pin_cfg = {
    .number_of_pins = sizeof(g_bsp_pin_cfg_data)/sizeof(ioport_pin_cfg_t),
    .p_pin_cfg_data = &g_bsp_pin_cfg_data[0],
};

#if BSP_TZ_SECURE_BUILD

void R_BSP_PinCfgSecurityInit(void);

/* Initialize SAR registers for secure pins. */
void R_BSP_PinCfgSecurityInit(void)
{
 #if (2U == BSP_FEATURE_IOPORT_VERSION)
    uint32_t pmsar[BSP_FEATURE_BSP_NUM_PMSAR];
 #else
    uint16_t pmsar[BSP_FEATURE_BSP_NUM_PMSAR];
 #endif
    memset(pmsar, 0xFF, BSP_FEATURE_BSP_NUM_PMSAR * sizeof(R_PMISC->PMSAR[0]));


    for(uint32_t i = 0; i < g_bsp_pin_cfg.number_of_pins; i++)
    {
        uint32_t port_pin = g_bsp_pin_cfg.p_pin_cfg_data[i].pin;
        uint32_t port = port_pin >> 8U;
        uint32_t pin = port_pin & 0xFFU;
        pmsar[port] &= (uint16_t) ~(1U << pin);
    }

    for(uint32_t i = 0; i < BSP_FEATURE_BSP_NUM_PMSAR; i++)
    {
 #if (2U == BSP_FEATURE_IOPORT_VERSION)
    #if BSP_SECONDARY_CORE_BUILD
        R_PMISC->PMSAR[i].PMSAR &= (uint16_t) pmsar[i];
    #else
        R_PMISC->PMSAR[i].PMSAR = (uint16_t) pmsar[i];
    #endif
 #else
        R_PMISC->PMSAR[i].PMSAR = pmsar[i];
 #endif
    }

}
#endif
