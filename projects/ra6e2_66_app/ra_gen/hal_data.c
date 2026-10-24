/* generated HAL source file - do not edit */
#include "hal_data.h"
iic_b_master_instance_ctrl_t I2C_BH1750_ctrl;
const iic_b_master_extended_cfg_t I2C_BH1750_extend =
{
    .timeout_mode             = IIC_B_MASTER_TIMEOUT_MODE_SHORT,
    .timeout_scl_low          = IIC_B_MASTER_TIMEOUT_SCL_LOW_ENABLED,
    .smbus_operation         = 0,
    /* Actual calculated bitrate: 398936. Actual calculated duty cycle: 50%. */ .clock_settings.brl_value = 135, .clock_settings.brh_value = 135, .clock_settings.cks_value = 0, .clock_settings.sdod_value = 0, .clock_settings.sdodcs_value = 0, .iic_clock_freq = 120000000,
};
const i2c_master_cfg_t I2C_BH1750_cfg =
{
    .channel             = 0,
    .rate                = I2C_MASTER_RATE_FAST,
    .slave               = 0x23,
    .addr_mode           = I2C_MASTER_ADDR_MODE_7BIT,
#define RA_NOT_DEFINED (1)
#if (RA_NOT_DEFINED == RA_NOT_DEFINED)
                .p_transfer_tx       = NULL,
#else
                .p_transfer_tx       = &RA_NOT_DEFINED,
#endif
#if (RA_NOT_DEFINED == RA_NOT_DEFINED)
                .p_transfer_rx       = NULL,
#else
                .p_transfer_rx       = &RA_NOT_DEFINED,
#endif
#undef RA_NOT_DEFINED
    .p_callback          = BH1750_callback,
    .p_context           = NULL,
#if defined(VECTOR_NUMBER_IICB0_RXI)
    .rxi_irq             = VECTOR_NUMBER_IICB0_RXI,
#else
    .rxi_irq             = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_IICB0_TXI)
    .txi_irq             = VECTOR_NUMBER_IICB0_TXI,
#else
    .txi_irq             = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_IICB0_TEND)
    .tei_irq             = VECTOR_NUMBER_IICB0_TEND,
#elif defined(VECTOR_NUMBER_IICB0_TEI)
    .tei_irq             = VECTOR_NUMBER_IICB0_TEI,
#else
    .tei_irq             = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_IICB0_EEI)
    .eri_irq             = VECTOR_NUMBER_IICB0_EEI,
#elif defined(VECTOR_NUMBER_IICB0_ERI)
    .eri_irq             = VECTOR_NUMBER_IICB0_ERI,
#else
    .eri_irq             = FSP_INVALID_VECTOR,
#endif
    .ipl                 = (12),
    .p_extend            = &I2C_BH1750_extend,
};
/* Instance structure to use this module. */
const i2c_master_instance_t I2C_BH1750 =
{
    .p_ctrl        = &I2C_BH1750_ctrl,
    .p_cfg         = &I2C_BH1750_cfg,
    .p_api         = &g_i2c_master_on_iic_b
};
sci_uart_instance_ctrl_t     g_uart0_ctrl;

            baud_setting_t               g_uart0_baud_setting =
            {
                /* Baud rate calculated with 1.725% error. */ .semr_baudrate_bits_b.abcse = 0, .semr_baudrate_bits_b.abcs = 0, .semr_baudrate_bits_b.bgdm = 1, .cks = 0, .brr = 31, .mddr = (uint8_t) 256, .semr_baudrate_bits_b.brme = false
            };

            /** UART extended configuration for UARTonSCI HAL driver */
            const sci_uart_extended_cfg_t g_uart0_cfg_extend =
            {
                .clock                = SCI_UART_CLOCK_INT,
                .rx_edge_start          = SCI_UART_START_BIT_FALLING_EDGE,
                .noise_cancel         = SCI_UART_NOISE_CANCELLATION_DISABLE,
                .rx_fifo_trigger        = SCI_UART_RX_FIFO_TRIGGER_MAX,
                .p_baud_setting         = &g_uart0_baud_setting,
                .flow_control           = SCI_UART_FLOW_CONTROL_RTS,
                #if 0xFF != 0xFF
                .flow_control_pin       = BSP_IO_PORT_FF_PIN_0xFF,
                #else
                .flow_control_pin       = (bsp_io_port_pin_t) UINT16_MAX,
                #endif
                .rs485_setting = {
                    .enable = SCI_UART_RS485_DISABLE,
                    .polarity = SCI_UART_RS485_DE_POLARITY_HIGH,
                #if 0xFF != 0xFF
                    .de_control_pin = BSP_IO_PORT_FF_PIN_0xFF,
                #else
                    .de_control_pin       = (bsp_io_port_pin_t) UINT16_MAX,
                #endif
                },
                .irda_setting = {
                    .ircr_bits_b.ire = 0,
                    .ircr_bits_b.irrxinv = 0,
                    .ircr_bits_b.irtxinv = 0,
                },
            };

            /** UART interface configuration */
            const uart_cfg_t g_uart0_cfg =
            {
                .channel             = 0,
                .data_bits           = UART_DATA_BITS_8,
                .parity              = UART_PARITY_OFF,
                .stop_bits           = UART_STOP_BITS_1,
                .p_callback          = uart0_callback,
                .p_context           = NULL,
                .p_extend            = &g_uart0_cfg_extend,
#define RA_NOT_DEFINED (1)
#if (RA_NOT_DEFINED == RA_NOT_DEFINED)
                .p_transfer_tx       = NULL,
#else
                .p_transfer_tx       = &RA_NOT_DEFINED,
#endif
#if (RA_NOT_DEFINED == RA_NOT_DEFINED)
                .p_transfer_rx       = NULL,
#else
                .p_transfer_rx       = &RA_NOT_DEFINED,
#endif
#undef RA_NOT_DEFINED
                .rxi_ipl             = (12),
                .txi_ipl             = (12),
                .tei_ipl             = (12),
                .eri_ipl             = (12),
#if defined(VECTOR_NUMBER_SCI0_RXI)
                .rxi_irq             = VECTOR_NUMBER_SCI0_RXI,
#else
                .rxi_irq             = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_SCI0_TXI)
                .txi_irq             = VECTOR_NUMBER_SCI0_TXI,
#else
                .txi_irq             = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_SCI0_TEI)
                .tei_irq             = VECTOR_NUMBER_SCI0_TEI,
#else
                .tei_irq             = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_SCI0_ERI)
                .eri_irq             = VECTOR_NUMBER_SCI0_ERI,
#else
                .eri_irq             = FSP_INVALID_VECTOR,
#endif
            };

/* Instance structure to use this module. */
const uart_instance_t g_uart0 =
{
    .p_ctrl        = &g_uart0_ctrl,
    .p_cfg         = &g_uart0_cfg,
    .p_api         = &g_uart_on_sci
};
usb_instance_ctrl_t g_basic0_ctrl;

#if !defined(g_usb_descriptor)
extern usb_descriptor_t g_usb_descriptor;
#endif
#define RA_NOT_DEFINED (1)
            const usb_cfg_t g_basic0_cfg =
            {
                .usb_mode  = USB_MODE_PERI,
                .usb_speed = USB_SPEED_FS,
                .module_number = 0,
                .type = USB_CLASS_PCDC,
#if defined(g_usb_descriptor)
                .p_usb_reg = g_usb_descriptor,
#else
                .p_usb_reg = &g_usb_descriptor,
#endif
                .usb_complience_cb = NULL,
#if defined(VECTOR_NUMBER_USBFS_INT)
                .irq       = VECTOR_NUMBER_USBFS_INT,
#else
                .irq       = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_USBFS_RESUME)
                .irq_r     = VECTOR_NUMBER_USBFS_RESUME,
#else
                .irq_r     = FSP_INVALID_VECTOR,
#endif
                .irq_d0    = FSP_INVALID_VECTOR,
                .irq_d1    = FSP_INVALID_VECTOR,
                .hsirq     = FSP_INVALID_VECTOR,
                .hsirq_d0  = FSP_INVALID_VECTOR,
                .hsirq_d1  = FSP_INVALID_VECTOR,
                .ipl       = (12),
                .ipl_r     = (12),
                .ipl_d0    = BSP_IRQ_DISABLED,
                .ipl_d1    = BSP_IRQ_DISABLED,
                .hsipl     = BSP_IRQ_DISABLED,
                .hsipl_d0  = BSP_IRQ_DISABLED,
                .hsipl_d1  = BSP_IRQ_DISABLED,
#if (BSP_CFG_RTOS == 0) && defined(USB_CFG_HMSC_USE)
                .p_usb_apl_callback = NULL,
#else
                .p_usb_apl_callback = usb_callback,
#endif
#if defined(NULL)
                .p_context = NULL,
#else
                .p_context = (void *) &NULL,
#endif
            };
#undef RA_NOT_DEFINED

/* Instance structure to use this module. */
const usb_instance_t g_basic0 =
{
    .p_ctrl        = &g_basic0_ctrl,
    .p_cfg         = &g_basic0_cfg,
    .p_api         = &g_usb_on_usb,
};

void g_hal_init(void) {
g_common_init();
}
