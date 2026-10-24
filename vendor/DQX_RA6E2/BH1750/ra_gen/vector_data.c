/* generated vector source file - do not edit */
#include "bsp_api.h"
/* Do not build these data structures if no interrupts are currently allocated because IAR will have build errors. */
#if VECTOR_DATA_IRQ_COUNT > 0
        BSP_DONT_REMOVE const fsp_vector_t g_vector_table[BSP_ICU_VECTOR_MAX_ENTRIES] BSP_PLACE_IN_SECTION(BSP_SECTION_APPLICATION_VECTORS) =
        {
                        [0] = sci_uart_rxi_isr, /* SCI0 RXI (Receive data full) */
            [1] = sci_uart_txi_isr, /* SCI0 TXI (Transmit data empty) */
            [2] = sci_uart_tei_isr, /* SCI0 TEI (Transmit end) */
            [3] = sci_uart_eri_isr, /* SCI0 ERI (Receive error) */
            [4] = iic_b_master_rxi_isr, /* IICB0 RXI (Receive) */
            [5] = iic_b_master_txi_isr, /* IICB0 TXI (Transmit) */
            [6] = iic_b_master_tei_isr, /* IICB0 TEI (Transmit end) */
            [7] = iic_b_master_eri_isr, /* IICB0 ERI (Error) */
        };
        #if BSP_FEATURE_ICU_HAS_IELSR
        const bsp_interrupt_event_t g_interrupt_event_link_select[BSP_ICU_VECTOR_MAX_ENTRIES] =
        {
            [0] = BSP_PRV_VECT_ENUM(EVENT_SCI0_RXI,GROUP0), /* SCI0 RXI (Receive data full) */
            [1] = BSP_PRV_VECT_ENUM(EVENT_SCI0_TXI,GROUP1), /* SCI0 TXI (Transmit data empty) */
            [2] = BSP_PRV_VECT_ENUM(EVENT_SCI0_TEI,GROUP2), /* SCI0 TEI (Transmit end) */
            [3] = BSP_PRV_VECT_ENUM(EVENT_SCI0_ERI,GROUP3), /* SCI0 ERI (Receive error) */
            [4] = BSP_PRV_VECT_ENUM(EVENT_IICB0_RXI,GROUP4), /* IICB0 RXI (Receive) */
            [5] = BSP_PRV_VECT_ENUM(EVENT_IICB0_TXI,GROUP5), /* IICB0 TXI (Transmit) */
            [6] = BSP_PRV_VECT_ENUM(EVENT_IICB0_TEI,GROUP6), /* IICB0 TEI (Transmit end) */
            [7] = BSP_PRV_VECT_ENUM(EVENT_IICB0_ERI,GROUP7), /* IICB0 ERI (Error) */
        };
        #endif
        #endif
