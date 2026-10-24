/* generated vector source file - do not edit */
        #include "bsp_api.h"
        /* [DIAG 2026-10-23] USBFS 入口探针: 数 ISR 到底进没进。见 src/usb/usb_isr_probe.c。
         * 原始生成物备份: .workbuddy/backup/vector_data.c.20261023 */
        extern void usbfs_isr_probe(void);
        /* Do not build these data structures if no interrupts are currently allocated because IAR will have build errors. */
        #if VECTOR_DATA_IRQ_COUNT > 0
        BSP_DONT_REMOVE const fsp_vector_t g_vector_table[BSP_ICU_VECTOR_NUM_ENTRIES] BSP_PLACE_IN_SECTION(BSP_SECTION_APPLICATION_VECTORS) =
        {
                        [0] = usbfs_isr_probe, /* USBFS INT (USBFS interrupt) — DIAG wrapper */
            [1] = usbfs_resume_handler, /* USBFS RESUME (USBFS resume interrupt) */
            [2] = sci_uart_rxi_isr, /* SCI0 RXI (Receive data full) */
            [3] = sci_uart_txi_isr, /* SCI0 TXI (Transmit data empty) */
            [4] = sci_uart_tei_isr, /* SCI0 TEI (Transmit end) */
            [5] = sci_uart_eri_isr, /* SCI0 ERI (Receive error) */
            [6] = iic_b_master_rxi_isr, /* IICB0 RXI (Receive) */
            [7] = iic_b_master_txi_isr, /* IICB0 TXI (Transmit) */
            [8] = iic_b_master_tei_isr, /* IICB0 TEI (Transmit end) */
            [9] = iic_b_master_eri_isr, /* IICB0 ERI (Error) */
        };
        #if BSP_FEATURE_ICU_HAS_IELSR
        const bsp_interrupt_event_t g_interrupt_event_link_select[BSP_ICU_VECTOR_NUM_ENTRIES] =
        {
            [0] = BSP_PRV_VECT_ENUM(EVENT_USBFS_INT,GROUP0), /* USBFS INT (USBFS interrupt) */
            [1] = BSP_PRV_VECT_ENUM(EVENT_USBFS_RESUME,GROUP1), /* USBFS RESUME (USBFS resume interrupt) */
            [2] = BSP_PRV_VECT_ENUM(EVENT_SCI0_RXI,GROUP2), /* SCI0 RXI (Receive data full) */
            [3] = BSP_PRV_VECT_ENUM(EVENT_SCI0_TXI,GROUP3), /* SCI0 TXI (Transmit data empty) */
            [4] = BSP_PRV_VECT_ENUM(EVENT_SCI0_TEI,GROUP4), /* SCI0 TEI (Transmit end) */
            [5] = BSP_PRV_VECT_ENUM(EVENT_SCI0_ERI,GROUP5), /* SCI0 ERI (Receive error) */
            [6] = BSP_PRV_VECT_ENUM(EVENT_IICB0_RXI,GROUP6), /* IICB0 RXI (Receive) */
            [7] = BSP_PRV_VECT_ENUM(EVENT_IICB0_TXI,GROUP7), /* IICB0 TXI (Transmit) */
            [8] = BSP_PRV_VECT_ENUM(EVENT_IICB0_TEI,GROUP0), /* IICB0 TEI (Transmit end) */
            [9] = BSP_PRV_VECT_ENUM(EVENT_IICB0_ERI,GROUP1), /* IICB0 ERI (Error) */
        };
        #endif
        #endif
