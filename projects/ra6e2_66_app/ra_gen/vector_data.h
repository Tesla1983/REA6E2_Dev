/* generated vector header file - do not edit */
        #ifndef VECTOR_DATA_H
        #define VECTOR_DATA_H
        #ifdef __cplusplus
        extern "C" {
        #endif
                /* Number of interrupts allocated */
        #ifndef VECTOR_DATA_IRQ_COUNT
        #define VECTOR_DATA_IRQ_COUNT    (10)
        #endif
        /* ISR prototypes */
        void usbfs_interrupt_handler(void);
        void usbfs_resume_handler(void);
        void sci_uart_rxi_isr(void);
        void sci_uart_txi_isr(void);
        void sci_uart_tei_isr(void);
        void sci_uart_eri_isr(void);
        void iic_b_master_rxi_isr(void);
        void iic_b_master_txi_isr(void);
        void iic_b_master_tei_isr(void);
        void iic_b_master_eri_isr(void);

        /* Vector table allocations */
        #define VECTOR_NUMBER_USBFS_INT ((IRQn_Type) 0) /* USBFS INT (USBFS interrupt) */
        #define USBFS_INT_IRQn          ((IRQn_Type) 0) /* USBFS INT (USBFS interrupt) */
        #define VECTOR_NUMBER_USBFS_RESUME ((IRQn_Type) 1) /* USBFS RESUME (USBFS resume interrupt) */
        #define USBFS_RESUME_IRQn          ((IRQn_Type) 1) /* USBFS RESUME (USBFS resume interrupt) */
        #define VECTOR_NUMBER_SCI0_RXI ((IRQn_Type) 2) /* SCI0 RXI (Receive data full) */
        #define SCI0_RXI_IRQn          ((IRQn_Type) 2) /* SCI0 RXI (Receive data full) */
        #define VECTOR_NUMBER_SCI0_TXI ((IRQn_Type) 3) /* SCI0 TXI (Transmit data empty) */
        #define SCI0_TXI_IRQn          ((IRQn_Type) 3) /* SCI0 TXI (Transmit data empty) */
        #define VECTOR_NUMBER_SCI0_TEI ((IRQn_Type) 4) /* SCI0 TEI (Transmit end) */
        #define SCI0_TEI_IRQn          ((IRQn_Type) 4) /* SCI0 TEI (Transmit end) */
        #define VECTOR_NUMBER_SCI0_ERI ((IRQn_Type) 5) /* SCI0 ERI (Receive error) */
        #define SCI0_ERI_IRQn          ((IRQn_Type) 5) /* SCI0 ERI (Receive error) */
        #define VECTOR_NUMBER_IICB0_RXI ((IRQn_Type) 6) /* IICB0 RXI (Receive) */
        #define IICB0_RXI_IRQn          ((IRQn_Type) 6) /* IICB0 RXI (Receive) */
        #define VECTOR_NUMBER_IICB0_TXI ((IRQn_Type) 7) /* IICB0 TXI (Transmit) */
        #define IICB0_TXI_IRQn          ((IRQn_Type) 7) /* IICB0 TXI (Transmit) */
        #define VECTOR_NUMBER_IICB0_TEI ((IRQn_Type) 8) /* IICB0 TEI (Transmit end) */
        #define IICB0_TEI_IRQn          ((IRQn_Type) 8) /* IICB0 TEI (Transmit end) */
        #define VECTOR_NUMBER_IICB0_ERI ((IRQn_Type) 9) /* IICB0 ERI (Error) */
        #define IICB0_ERI_IRQn          ((IRQn_Type) 9) /* IICB0 ERI (Error) */
        /* The number of entries required for the ICU vector table. */
        #define BSP_ICU_VECTOR_NUM_ENTRIES (10)

        #ifdef __cplusplus
        }
        #endif
        #endif /* VECTOR_DATA_H */
