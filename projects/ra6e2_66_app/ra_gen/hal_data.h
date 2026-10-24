/* generated HAL header file - do not edit */
#ifndef HAL_DATA_H_
#define HAL_DATA_H_
#include <stdint.h>
#include "bsp_api.h"
#include "common_data.h"
#include "r_iic_b_master.h"
#include "r_i2c_master_api.h"
#include "r_sci_uart.h"
            #include "r_uart_api.h"
#include "r_usb_basic.h"
#include "r_usb_basic_api.h"
#include "r_usb_pcdc_api.h"
FSP_HEADER
/* I2C Master on IIC Instance. */
extern const i2c_master_instance_t I2C_BH1750;

/** Access the I2C Master instance using these structures when calling API functions directly (::p_api is not used). */
extern iic_b_master_instance_ctrl_t I2C_BH1750_ctrl;
extern const i2c_master_cfg_t I2C_BH1750_cfg;

#ifndef BH1750_callback
void BH1750_callback(i2c_master_callback_args_t * p_args);
#endif
/** UART on SCI Instance. */
            extern const uart_instance_t      g_uart0;

            /** Access the UART instance using these structures when calling API functions directly (::p_api is not used). */
            extern sci_uart_instance_ctrl_t     g_uart0_ctrl;
            extern const uart_cfg_t g_uart0_cfg;
            extern const sci_uart_extended_cfg_t g_uart0_cfg_extend;

            #ifndef uart0_callback
            void uart0_callback(uart_callback_args_t * p_args);
            #endif
/* Basic on USB Instance. */
extern const usb_instance_t g_basic0;

/** Access the USB instance using these structures when calling API functions directly (::p_api is not used). */
extern usb_instance_ctrl_t g_basic0_ctrl;
extern const usb_cfg_t g_basic0_cfg;

#ifndef NULL
void NULL(void *);
#endif

#if 2 == BSP_CFG_RTOS
#ifndef usb_callback
void usb_callback(usb_event_info_t *, usb_hdl_t, usb_onoff_t);
#endif
#endif
#if 0 == BSP_CFG_RTOS
/* bare-metal usb_callback (added by tools/fix_hal_data_usb.py) */
void usb_callback(usb_callback_args_t * p_args);
#endif
/** CDC Driver on USB Instance. */
void hal_entry(void);
void g_hal_init(void);
FSP_FOOTER
#endif /* HAL_DATA_H_ */
