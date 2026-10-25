/* 来源: FSP 6.6.0 的 ra/fsp/src/r_usb_pcdc/r_usb_pcdc_descriptor.c.template
 * (RASC 在工程里配好 USB PCDC 后会把它复制成 usb_descriptor.c 供用户改; 本工程手写在 src/usb/)。
 *
 * 设备描述符的 bDeviceClass 是 0x02 (CDC), 所以 Windows 无需 INF 就会自动挂 usbser.sys
 * —— 这正是"Type-C 当串口用"能成立的前提。仅改了 VID/PID。 */
/******************************************************************************
 Includes   <System Includes> , "Project Includes"
 ******************************************************************************/
#include "r_usb_basic.h"
#include "r_usb_basic_api.h"
#include "r_usb_basic_cfg.h"

/* 在 R_USB_Open 之前调用: 用芯片出厂 UID 填充唯一 USB 序列号(定义见文件末尾) */
void usb_cdc_fill_serial_from_uid(void);

/******************************************************************************
 Macro definitions
 ******************************************************************************/
/* bcdUSB */
/* 维持 0x0200(USB 2.0)。
 *
 * 曾两度尝试升 0x0201 以启用 Microsoft OS 2.0 描述符(CompatID=CDCACM), 意图根治
 * "重启后等十几秒才出 COM"。两次(外置描述符集 + 内嵌描述符集)实测均导致 Windows 把设备
 * 标记为 CM_PROB_FAILED_START、且实例 ID 退回 hub-port 形式(读不到唯一序列号)。字节与
 * FSP 的 BOS 应答逻辑逐字节核对均规范, 故判定为 FSP 6.6.0 栈对 bcdUSB>=0x0201/BOS 的
 * 兼容问题(Windows 枚举死锁), 而非描述符写错。MS OS 这条路在本栈暂不可行, 已回退。
 *
 * 当前状态 = 唯一序列号(usb_cdc_fill_serial_from_uid): 它**确实解决了身份问题** —— 设备实例 ID
 * 由 hub-port 形式(5&16B1B90B&0&1)变成稳定的 USB\VID_045B&PID_0263\43045A764E4B2EB4,
 * 不再 CM_PROB_FAILED_START, COM 号固定。
 *
 * ⛔ 那 30s 的最终结论(2026-10-25 实测归档, 勿再重复排查):
 *    设备 PRESENT(devnode OK)只需 ~0.3~0.9s, 但 COM 号登记进
 *    HKLM\HARDWARE\DEVICEMAP\SERIALCOMM 要 ~29.6s, 且**首次能打开**端口也在 28.9s ⇒
 *    端口是真的不可用, 不是"扫描滞后", 上位机无法绕过。
 *    固件侧已证明无责: 请求日志显示枚举只有 4 条标准 CDC 请求(GET/SET_LINE_CODING、
 *    SET_CONTROL_LINE_STATE), 之后 USB 总线**完全静默** ⇒ 这 30s 发生在主机内部
 *    (usbser.sys / msports 的 COM 名登记路径), 与固件、描述符、驱动绑定均无关。
 *
 *    以下假设均已实测证伪/排除, 别再走:
 *      ✘ Windows 通用行为      —— 同机 CH340(厂商驱动)只要 0.37s
 *      ✘ WMI 测量假象          —— PnP/注册表/WMI 三口径一致, 都是 ~30s
 *      ✘ 驱动加载慢            —— devnode 0.9s 就 OK(CM_PROB_NONE)
 *      ✘ 查 Windows Update     —— 驱动 0.9s 已绑好, 不存在联网查找
 *      ✘ 硬件 ID 无本地驱动    —— 同上; 且 usbser.inf 按兼容 ID 也匹配
 *      ✘ 调制解调器检测        —— usbser.inf 的 ModemFilter 无硬件 ID; 且本机串口类
 *                                 所有实例 UpperFilters/LowerFilters 均为空(serenum 未挂)
 *      ✘ 封装请求(SEND/GET_ENCAPSULATED) —— 日志里一次都没出现, data_cnt=0
 *      ✘ 陈旧 devnode / COM 号库膨胀     —— 删掉 13 个僵尸后仍是 29.6s
 *      ✘ bInterfaceProtocol 触发检测     —— 0x01 与 0x00 两版实测都是 ~29.6s
 *    绕过建议: 上位机轮询等待端口就绪(最多 ~35s)并给出进度, 不要"扫描一次就放弃"。
 *
 * bInterfaceProtocol 保持 0x01(标准 CDC-ACM 控制接口值)。改动成 0x00 的对照实验已完成
 * (见上), 与 30s 无关, 故不偏离标准值。
 *
 * 下方 g_apl_bos_descriptor / g_apl_msos20_set 与 FSP 的 case 0x0F、应用层 vendor 分支
 * 保留作**惰性兜底**(bcdUSB<0x0201 时 Windows 永不请求 BOS, 不会被执行), 勿误以为生效;
 * 将来若升级 FSP 想再试 MS OS 2.0, 只需把 USB_BCDNUM 改成 0x0201 即可复现。 */
#define USB_BCDNUM                      (0x0200U)
/* Release Number */
#define USB_RELEASE                     (0x0200U)
/* DCP max packet size */
#define USB_DCPMAXP                     (64U)
/* Configuration number */
#define USB_CONFIGNUM                   (1U)
/* Vendor ID */
#define USB_VENDORID                    (0x045BU)
/* Product ID */
#define USB_PRODUCTID                   (0x0263U)

/* Class-Specific Configuration Descriptors */
#define     USB_PCDC_CS_INTERFACE                               (0x24U)

/* bDescriptor SubType in Communications Class Functional Descriptors */
/* Header Functional Descriptor */
#define     USB_PCDC_DT_SUBTYPE_HEADER_FUNC                     (0x00U)
/* Call Management Functional Descriptor. */
#define     USB_PCDC_DT_SUBTYPE_CALL_MANAGE_FUNC                (0x01U)
/* Abstract Control Management Functional Descriptor. */
#define     USB_PCDC_DT_SUBTYPE_ABSTRACT_CTR_MANAGE_FUNC        (0x02U)
/* Union Functional Descriptor */
#define     USB_PCDC_DT_SUBTYPE_UNION_FUNC                      (0x06U)

/* Communications Class Subclass Codes */
#define     USB_PCDC_CLASS_SUBCLASS_CODE_ABS_CTR_MDL            (0x02U)

/* USB Class Definitions for Communications Devices Specification
 release number in binary-coded decimal. */
#define     USB_PCDC_BCD_CDC          (0x0110U)

/* Descriptor length */
#define USB_PCDC_QD_LEN               (10U)
#define USB_PCDC_CD1_LEN              (67U)
#define STRING_DESCRIPTOR0_LEN        (4U)
#define STRING_DESCRIPTOR1_LEN        (16U)
#define STRING_DESCRIPTOR2_LEN        (44U)
#define STRING_DESCRIPTOR3_LEN        (46U)
#define STRING_DESCRIPTOR4_LEN        (22U)
#define STRING_DESCRIPTOR5_LEN        (18U)
#define STRING_DESCRIPTOR6_LEN        (34U)  /* 16 个 hex 字符(来自芯片 UID) + bLength + type */
#define NUM_STRING_DESCRIPTOR         (7U)

/* Descriptor data Mask */
#define USB_UCHAR_MAX                 (0xffU)
#define USB_W_TOTAL_LENGTH_MASK       (256U)
#define USB_W_MAX_PACKET_SIZE_MASK    (64U)
#define USB_PCDC_BCD_CDC_MASK         (256U)

/******************************************************************************
 Private global variables and functions
 ******************************************************************************/

/******************************************************************************
 Exported global variables
 ******************************************************************************/

/******************************************************************************
 Exported global functions (to be accessed by other files)
 ******************************************************************************/

/* Standard Device Descriptor */
uint8_t g_apl_device[USB_DD_BLENGTH + ( USB_DD_BLENGTH % 2)] =
{
    USB_DD_BLENGTH,                                     /*  0:bLength */
    USB_DT_DEVICE,                                      /*  1:bDescriptorType */
    (USB_BCDNUM & (uint8_t) USB_UCHAR_MAX),                     /*  2:bcdUSB_lo */
    ((uint8_t) (USB_BCDNUM >> 8) & (uint8_t) USB_UCHAR_MAX),    /*  3:bcdUSB_hi */
    USB_IFCLS_CDCC,                                     /*  4:bDeviceClass */
    USB_PCDC_CLASS_SUBCLASS_CODE_ABS_CTR_MDL,           /*  5:bDeviceSubClass */
    0,                                                  /*  6:bDeviceProtocol */
    (uint8_t) USB_DCPMAXP,                              /*  7:bMAXPacketSize(for DCP) */
    (USB_VENDORID & (uint8_t) USB_UCHAR_MAX),                   /*  8:idVendor_lo */
    ((uint8_t) (USB_VENDORID >> 8) & (uint8_t) USB_UCHAR_MAX),  /*  9:idVendor_hi */
    ((uint16_t) USB_PRODUCTID & (uint8_t) USB_UCHAR_MAX),       /* 10:idProduct_lo */
    ((uint8_t) (USB_PRODUCTID >> 8) & (uint8_t) USB_UCHAR_MAX), /* 11:idProduct_hi */
    (USB_RELEASE & (uint8_t) USB_UCHAR_MAX),                    /* 12:bcdDevice_lo */
    ((uint8_t) (USB_RELEASE >> 8) & (uint8_t) USB_UCHAR_MAX),   /* 13:bcdDevice_hi */
    1,                                                  /* 14:iManufacturer */
    2,                                                  /* 15:iProduct */
    6,                                                  /* 16:iSerialNumber */
    USB_CONFIGNUM /* 17:bNumConfigurations */
};

/************************************************************
 *  Device Qualifier Descriptor   *
 ************************************************************/
uint8_t g_apl_qualifier_descriptor[USB_PCDC_QD_LEN + ( USB_PCDC_QD_LEN % 2)] =
{
    USB_PCDC_QD_LEN,                                    /*  0:bLength */
    USB_DT_DEVICE_QUALIFIER,                            /*  1:bDescriptorType */
    (USB_BCDNUM & (uint8_t) USB_UCHAR_MAX),                  /*  2:bcdUSB_lo */
    ((uint8_t) (USB_BCDNUM >> 8) & (uint8_t) USB_UCHAR_MAX), /*  3:bcdUSB_hi */
    0,                                                  /*  4:bDeviceClass */
    0,                                                  /*  5:bDeviceSubClass */
    0,                                                  /*  6:bDeviceProtocol */
    (uint8_t) USB_DCPMAXP,                              /*  7:bMAXPacketSize(for DCP) */
    USB_CONFIGNUM,                                      /*  8:bNumConfigurations */
    0 /*  9:bReserved */
};

/************************************************************
 *  Configuration Or Other_Speed_Configuration Descriptor   *
 ************************************************************/
/* For Full-Speed */
uint8_t g_apl_configuration[USB_PCDC_CD1_LEN + ( USB_PCDC_CD1_LEN % 2)] =
{
    USB_CD_BLENGTH,                 /*  0:bLength */
    USB_SOFT_CHANGE,                /*  1:bDescriptorType */
    USB_PCDC_CD1_LEN % USB_W_TOTAL_LENGTH_MASK, /*  2:wTotalLength(L) */
    USB_PCDC_CD1_LEN / USB_W_TOTAL_LENGTH_MASK, /*  3:wTotalLength(H) */
    2,                              /*  4:bNumInterfaces */
    1,                              /*  5:bConfigurationValue */
    0,                              /*  6:iConfiguration */
    USB_CF_RESERVED | USB_CF_SELFP, /*  7:bmAttributes */
    (10 / 2),                       /*  8:MAXPower (2mA unit) */

    /* Interface Descriptor */
    USB_ID_BLENGTH,                 /*  0:bLength */
    USB_DT_INTERFACE,               /*  1:bDescriptor */
    0,                              /*  2:bInterfaceNumber */
    0,                              /*  3:bAlternateSetting */
    1,                              /*  4:bNumEndpoints */
    USB_IFCLS_CDCC,                 /*  5:bInterfaceClass */
    USB_PCDC_CLASS_SUBCLASS_CODE_ABS_CTR_MDL,   /*  6:bInterfaceSubClass */
    1,                              /*  7:bInterfaceProtocol (标准 CDC-ACM 值; 改成 0 的对照实验已做, 与 30s 无关) */
    0,                              /*  8:iInterface */

        /* Communications Class Functional Descriptorss */
        5,                          /*  0:bLength */
        USB_PCDC_CS_INTERFACE,      /*  1:bDescriptorType */
        USB_PCDC_DT_SUBTYPE_HEADER_FUNC,    /*  2:bDescriptorSubtype */
        USB_PCDC_BCD_CDC % USB_W_TOTAL_LENGTH_MASK,     /*  3:bcdCDC_lo */
        USB_PCDC_BCD_CDC / USB_W_TOTAL_LENGTH_MASK,     /*  4:bcdCDC_hi */

        /* Communications Class Functional Descriptorss */
        4,                          /*  0:bLength */
        USB_PCDC_CS_INTERFACE,      /*  1:bDescriptorType */
        USB_PCDC_DT_SUBTYPE_ABSTRACT_CTR_MANAGE_FUNC,   /*  2:bDescriptorSubtype */
        2,                          /*  3:bmCapabilities */

        /* Communications Class Functional Descriptorss */
        5,                          /*  0:bLength */
        USB_PCDC_CS_INTERFACE,      /*  1:bDescriptorType */
        USB_PCDC_DT_SUBTYPE_UNION_FUNC, /*  2:bDescriptorSubtype */
        0,                          /*  3:bMasterInterface */
        1,                          /*  4:bSlaveInterface0 */

        /* Communications Class Functional Descriptorss */
        5,                          /*  0:bLength */
        USB_PCDC_CS_INTERFACE,      /*  1:bDescriptorType */
        USB_PCDC_DT_SUBTYPE_CALL_MANAGE_FUNC,   /*  2:bDescriptorSubtype */
        /* D1:1-Device can send/receive call management
         information over a Data Class interface. */
        /* D0:1-Device handles call management itself. */
        3,                          /*  3:bmCapabilities */
        1,                          /*  4:bDataInterface */

        /* Endpoint Descriptor 0 */
        7,                          /*  0:bLength */
        USB_DT_ENDPOINT,            /*  1:bDescriptorType */
        USB_EP_IN | USB_EP3,        /*  2:bEndpointAddress */
        USB_EP_INT,                 /*  3:bmAttribute */
        16,                         /*  4:wMAXPacketSize_lo */
        0,                          /*  5:wMAXPacketSize_hi */
        0x10,                       /*  6:bInterval */

    /* Interface Descriptor */
    USB_ID_BLENGTH,                 /*  0:bLength */
    USB_DT_INTERFACE,               /*  1:bDescriptor */
    1,                              /*  2:bInterfaceNumber */
    0,                              /*  3:bAlternateSetting */
    2,                              /*  4:bNumEndpoints */
    USB_IFCLS_CDCD,                 /*  5:bInterfaceClass */
    0,                              /*  6:bInterfaceSubClass */
    0,                              /*  7:bInterfaceProtocol */
    0,                              /*  8:iInterface */

        /* Endpoint Descriptor 0 */
        USB_ED_BLENGTH,             /*  0:bLength */
        USB_DT_ENDPOINT,            /*  1:bDescriptorType */
        USB_EP_IN | USB_EP1,        /*  2:bEndpointAddress */
        USB_EP_BULK,                /*  3:bmAttribute */
        USB_W_MAX_PACKET_SIZE_MASK, /*  4:wMAXPacketSize_lo */
        0,                          /*  5:wMAXPacketSize_hi */
        0,                          /*  6:bInterval */

        /* Endpoint Descriptor 1 */
        USB_ED_BLENGTH,             /*  0:bLength */
        USB_DT_ENDPOINT,            /*  1:bDescriptorType */
        USB_EP_OUT | USB_EP2,       /*  2:bEndpointAddress */
        USB_EP_BULK,                /*  3:bmAttribute */
        USB_W_MAX_PACKET_SIZE_MASK, /*  4:wMAXPacketSize_lo */
        0,                          /*  5:wMAXPacketSize_hi */
        0,                          /*  6:bInterval */
};

/* For High-Speed */
uint8_t g_apl_hs_configuration[USB_PCDC_CD1_LEN + ( USB_PCDC_CD1_LEN % 2)] =
{
    9,                              /*  0:bLength */
    USB_SOFT_CHANGE,                /*  1:bDescriptorType */
    USB_PCDC_CD1_LEN % USB_W_TOTAL_LENGTH_MASK,         /*  2:wTotalLength(L) */
    USB_PCDC_CD1_LEN / USB_W_TOTAL_LENGTH_MASK,         /*  3:wTotalLength(H) */
    2,                              /*  4:bNumInterfaces */
    1,                              /*  5:bConfigurationValue */
    0,                              /*  6:iConfiguration */
    USB_CF_RESERVED | USB_CF_SELFP, /*  7:bmAttributes */
    (10 / 2),                       /*  8:MAXPower (2mA unit) */

    /* Interface Descriptor */
    9,                              /*  0:bLength */
    USB_DT_INTERFACE,               /*  1:bDescriptor */
    0,                              /*  2:bInterfaceNumber */
    0,                              /*  3:bAlternateSetting */
    1,                              /*  4:bNumEndpoints */
    USB_IFCLS_CDCC,                 /*  5:bInterfaceClass */
    USB_PCDC_CLASS_SUBCLASS_CODE_ABS_CTR_MDL,   /*  6:bInterfaceSubClass */
    1,                              /*  7:bInterfaceProtocol (标准 CDC-ACM 值; 改成 0 的对照实验已做, 与 30s 无关) */
    0,                              /*  8:iInterface */

        /* Communications Class Functional Descriptorss */
        5, /*  0:bLength */
        USB_PCDC_CS_INTERFACE,      /*  1:bDescriptorType */
        USB_PCDC_DT_SUBTYPE_HEADER_FUNC,    /*  2:bDescriptorSubtype */
        USB_PCDC_BCD_CDC % USB_PCDC_BCD_CDC_MASK,     /*  3:bcdCDC_lo */
        USB_PCDC_BCD_CDC / USB_PCDC_BCD_CDC_MASK,     /*  4:bcdCDC_hi */

        /* Communications Class Functional Descriptorss */
        4,                          /*  0:bLength */
        USB_PCDC_CS_INTERFACE,      /*  1:bDescriptorType */
        USB_PCDC_DT_SUBTYPE_ABSTRACT_CTR_MANAGE_FUNC,   /*  2:bDescriptorSubtype */
        2,                          /*  3:bmCapabilities */

        /* Communications Class Functional Descriptorss */
        5,                          /*  0:bLength */
        USB_PCDC_CS_INTERFACE,      /*  1:bDescriptorType */
        USB_PCDC_DT_SUBTYPE_UNION_FUNC, /*  2:bDescriptorSubtype */
        0,                          /*  3:bMasterInterface */
        1,                          /*  4:bSlaveInterface0 */

        /* Communications Class Functional Descriptorss */
        5,                          /*  0:bLength */
        USB_PCDC_CS_INTERFACE,      /*  1:bDescriptorType */
        USB_PCDC_DT_SUBTYPE_CALL_MANAGE_FUNC,   /*  2:bDescriptorSubtype */
        /* D1:1-Device can send/receive call management
         information over a Data Class interface. */
        /* D0:1-Device handles call management itself. */
        3,                          /*  3:bmCapabilities */
        1,                          /*  4:bDataInterface */

        /* Endpoint Descriptor 0 */
        7,                          /*  0:bLength */
        USB_DT_ENDPOINT,            /*  1:bDescriptorType */
        USB_EP_IN | USB_EP3,        /*  2:bEndpointAddress */
        USB_EP_INT,                 /*  3:bmAttribute */
        16,                         /*  4:wMAXPacketSize_lo */
        0,                          /*  5:wMAXPacketSize_hi */
        0x10,                       /*  6:bInterval */

    /* Interface Descriptor */
    9,                              /*  0:bLength */
    USB_DT_INTERFACE,               /*  1:bDescriptor */
    1,                              /*  2:bInterfaceNumber */
    0,                              /*  3:bAlternateSetting */
    2,                              /*  4:bNumEndpoints */
    USB_IFCLS_CDCD,                 /*  5:bInterfaceClass */
    0,                              /*  6:bInterfaceSubClass */
    0,                              /*  7:bInterfaceProtocol */
    0,                              /*  8:iInterface */

        /* Endpoint Descriptor 0 */
        7,                          /*  0:bLength */
        USB_DT_ENDPOINT,            /*  1:bDescriptorType */
        USB_EP_IN | USB_EP1,        /*  2:bEndpointAddress */
        USB_EP_BULK,                /*  3:bmAttribute */
        0,                          /*  4:wMAXPacketSize_lo */
        2,                          /*  5:wMAXPacketSize_hi */
        0,                          /*  6:bInterval */

        /* Endpoint Descriptor 1 */
        7,                          /*  0:bLength */
        USB_DT_ENDPOINT,            /*  1:bDescriptorType */
        USB_EP_OUT | USB_EP2,       /*  2:bEndpointAddress */
        USB_EP_BULK,                /*  3:bmAttribute */
        0,                          /*  4:wMAXPacketSize_lo */
        2,                          /*  5:wMAXPacketSize_hi */
        0,                          /*  6:bInterval */
};

/*************************************
 *    String Descriptor              *
 *************************************/
/* UNICODE 0x0409 English (United States) */
uint8_t g_cdc_string_descriptor0[STRING_DESCRIPTOR0_LEN + ( STRING_DESCRIPTOR0_LEN % 2)] =
{
    STRING_DESCRIPTOR0_LEN,     /*  0:bLength */
    USB_DT_STRING,              /*  1:bDescriptorType */
    0x09, 0x04                  /*  2:wLANGID[0] */
};

/* iManufacturer */
uint8_t g_cdc_string_descriptor1[STRING_DESCRIPTOR1_LEN + ( STRING_DESCRIPTOR1_LEN % 2)] =
{
    STRING_DESCRIPTOR1_LEN,     /*  0:bLength */
    USB_DT_STRING,              /*  1:bDescriptorType */
    'R', 0x00,                  /*  2:wLANGID[0] */
    'E', 0x00,
    'N', 0x00,
    'E', 0x00,
    'S', 0x00,
    'A', 0x00,
    'S', 0x00,
};

/* iProduct */
uint8_t g_cdc_string_descriptor2[STRING_DESCRIPTOR2_LEN + ( STRING_DESCRIPTOR2_LEN % 2)] =
{
    STRING_DESCRIPTOR2_LEN, /*  0:bLength */
    USB_DT_STRING,          /*  1:bDescriptorType */
    'C', 0x00,
    'D', 0x00,
    'C', 0x00,
    ' ', 0x00,
    'U', 0x00,
    'S', 0x00,
    'B', 0x00,
    ' ', 0x00,
    'D', 0x00,
    'e', 0x00,
    'm', 0x00,
    'o', 0x00,
    'n', 0x00,
    's', 0x00,
    't', 0x00,
    'r', 0x00,
    'a', 0x00,
    't', 0x00,
    'i', 0x00,
    'o', 0x00,
    'n', 0x00,
};

/* iInterface */
uint8_t g_cdc_string_descriptor3[STRING_DESCRIPTOR3_LEN + ( STRING_DESCRIPTOR3_LEN % 2)] =
{
    STRING_DESCRIPTOR3_LEN, /*  0:bLength */
    USB_DT_STRING,          /*  1:bDescriptorType */
    'C', 0x00,
    'o', 0x00,
    'm', 0x00,
    'm', 0x00,
    'u', 0x00,
    'n', 0x00,
    'i', 0x00,
    'c', 0x00,
    'a', 0x00,
    't', 0x00,
    'i', 0x00,
    'o', 0x00,
    'n', 0x00,
    's', 0x00,
    ' ', 0x00,
    'D', 0x00,
    'e', 0x00,
    'v', 0x00,
    'i', 0x00,
    'c', 0x00,
    'e', 0x00,
    's', 0x00
};

/* iConfiguration */
uint8_t g_cdc_string_descriptor4[STRING_DESCRIPTOR4_LEN + ( STRING_DESCRIPTOR4_LEN % 2)] =
{
    STRING_DESCRIPTOR4_LEN, /*  0:bLength */
    USB_DT_STRING,          /*  1:bDescriptorType */
    'F', 0x00,              /*  2:wLANGID[0] */
    'u', 0x00,
    'l', 0x00,
    'l', 0x00,
    '-', 0x00,
    'S', 0x00,
    'p', 0x00,
    'e', 0x00,
    'e', 0x00,
    'd', 0x00
};

/* iConfiguration */
uint8_t g_cdc_string_descriptor5[STRING_DESCRIPTOR5_LEN + ( STRING_DESCRIPTOR5_LEN % 2)] =
{
    STRING_DESCRIPTOR5_LEN, /*  0:bLength */
    USB_DT_STRING,          /*  1:bDescriptorType */
    'H', 0x00,              /*  2:wLANGID[0] */
    'i', 0x00,
    '-', 0x00,
    'S', 0x00,
    'p', 0x00,
    'e', 0x00,
    'e', 0x00,
    'd', 0x00
};

/* iSerialNumber
 * 注意: 这块板子曾经把序列号写死成 "0000000000001", 所有板子都一样、不唯一。
 * 这会导致 Windows 无法区分设备, 每次重连都重建 devnode、重走"设备安装"流程,
 * 表现就是"重启后等十几秒串口才出现"。现在序列号改为运行时用芯片出厂 UID 填充
 * (见 usb_cdc_fill_serial_from_uid), 这里是占位初值, 实际内容由该函数覆盖。 */
uint8_t g_cdc_string_descriptor6[STRING_DESCRIPTOR6_LEN + ( STRING_DESCRIPTOR6_LEN % 2)] =
{
    STRING_DESCRIPTOR6_LEN, /*  0:bLength */
    USB_DT_STRING,          /*  1:bDescriptorType */
    /* 2..33: 16 个 Unicode 字符(UTF-16LE), 运行时由 usb_cdc_fill_serial_from_uid() 填充 */
    '0', 0x00, '0', 0x00, '0', 0x00, '0', 0x00, '0', 0x00, '0', 0x00, '0', 0x00, '0', 0x00,
    '0', 0x00, '0', 0x00, '0', 0x00, '0', 0x00, '0', 0x00, '0', 0x00, '0', 0x00, '0', 0x00,
};

uint8_t *g_apl_string_table[] =
{
    g_cdc_string_descriptor0,
    g_cdc_string_descriptor1,
    g_cdc_string_descriptor2,
    g_cdc_string_descriptor3,
    g_cdc_string_descriptor4,
    g_cdc_string_descriptor5,
    g_cdc_string_descriptor6
};

/******************************************************************************
 *  Microsoft OS 2.0 描述符 (根治"重启后等十几秒才出 COM")
 *  --------------------------------------------------------------------------
 *  流程: 主机读设备描述符看到 bcdUSB=0x0201 → 请求 BOS(type 0x0F) →
 *        FSP 的 usb_pstd_get_descriptor1 用 g_apl_bos_descriptor[] 应答 →
 *        主机看到 MS OS 2.0 平台能力(含 bMS_VendorCode / wIndex=0x0007) →
 *        发 vendor 请求(bmRequestType=0xC0, bRequest=bMS_VendorCode, wIndex=0x0007) →
 *        应用层 usb_callback 用 g_apl_msos20_set[] 应答 →
 *        Windows 拿到 CompatID="CDCACM", 直接匹配内置 usbser.sys, 跳过 WU 查询。
 *
 *  ⚠ 平台 UUID 必须是 Microsoft OS 2.0 的官方值(小端):
 *    D8DD60DF-4589-4CC7-9CD2-659D9E648A9F → DF 60 DD D8 89 45 C7 4C 9C D2 65 9D 9E 64 8A 9F
 *    上一轮误写成别的字节导致 Windows 根本不认, 设备启动失败, 已纠正。
 ******************************************************************************/

/* BOS 描述符(标准 type 0x0F) + 内嵌 MS OS 2.0 Platform Capability + 内嵌描述符集。
 * 非 static, FSP 的 r_usb_pstdrequest.c 通过 extern 引用它来应答 GetDescriptor(BOS)。
 *
 * 内嵌式 MS OS 2.0: 描述符集(CompatID=CDCACM)直接放在 Platform Capability 数据区,
 * Windows 从 BOS 就地读取, **不**再发 pre-config vendor 请求(wIndex=0x0007)——避免该请求
 * 在配置前阶段把 FSP 控制管道卡死导致 CM_PROB_FAILED_START。wTotalLength=0x4F(79)
 * = 5(BOS头) + 74(能力描述符: 28 头 + 46 描述符集)。 */
uint8_t g_apl_bos_descriptor[] =
{
    /* BOS 标准头 */
    0x05, 0x0F,                 /* bLength=5, bDescriptorType=BOS(0x0F) */
    0x4F, 0x00,                 /* wTotalLength = 0x4F = 79 (5 + 74) */
    0x01,                       /* bNumDeviceCaps = 1 */
    /* Device Capability: Platform (type 0x10, cap type 0x05) */
    0x4A,                       /* bLength = 74 (28 头 + 46 描述符集) */
    0x10,                       /* bDescriptorType = Device Capability */
    0x05,                       /* bDevCapabilityType = Platform */
    0x00,                       /* bReserved */
    /* PlatformCapabilityUUID = Microsoft OS 2.0 (小端) */
    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
    /* dwWindowsVersion = 0x06030000 (Windows 8.1+) */
    0x00, 0x00, 0x03, 0x06,
    /* wMSOSDescriptorSetTotalLength = 0x2E = 46 (内嵌描述符集长度) */
    0x2E, 0x00,
    /* bMS_VendorCode: 取描述符集用的 vendor 请求码(内嵌时 Windows 通常不发, 留作兜底) */
    0x01,
    /* bAltEnumCode: 不支持备选枚举 */
    0x00,
    /* ---- 内嵌 MS OS 2.0 描述符集 (与 g_apl_msos20_set 完全一致) ---- */
    /* Set Header (10 字节) */
    0x0A, 0x00,                 /* wLength = 10 */
    0x00, 0x00,                 /* wDescriptorType = Set Header (0x00) */
    0x00, 0x00, 0x03, 0x06,     /* dwWindowsVersion = 0x06030000 */
    0x2E, 0x00,                 /* wTotalLength = 46 */
    /* Configuration Subset Header (8 字节) */
    0x08, 0x00,                 /* wLength = 8 */
    0x01, 0x00,                 /* wDescriptorType = Configuration Subset (0x01) */
    0x01,                       /* bConfigurationValue = 1 */
    0x00,                       /* bReserved */
    0x24, 0x00,                 /* wTotalLength = 0x24 = 36 (8 + 8 + 20) */
    /* Function Subset Header (8 字节) */
    0x08, 0x00,                 /* wLength = 8 */
    0x02, 0x00,                 /* wDescriptorType = Function Subset (0x02) */
    0x00,                       /* bFirstInterface = 0 (CDC 控制接口) */
    0x00,                       /* bReserved */
    0x1C, 0x00,                 /* wSubsetLength = 0x1C = 28 (8 + 20) */
    /* Compatible ID Descriptor (20 字节) */
    0x14, 0x00,                 /* wLength = 20 */
    0x03, 0x00,                 /* wDescriptorType = Compatible ID (0x03) */
    'C', 'D', 'C', 'A', 'C', 'M', 0x00, 0x00,   /* CompatibleID = "CDCACM\0\0" */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00  /* SubCompatibleID = 全 0 */
};

/* MS OS 2.0 描述符集: Windows 通过上面的 bMS_VendorCode(0x01)+wIndex=0x0007 来取。
 * CompatID="CDCACM" 让 Windows 直接匹配内置 usbser.sys, 不查 Windows Update。
 * 注意: 非 static, 应用层 usb_callback 通过 extern 引用它来应答 vendor 请求。 */
uint8_t g_apl_msos20_set[] =
{
    /* Set Header (10 字节) */
    0x0A, 0x00,                 /* wLength = 10 */
    0x00, 0x00,                 /* wDescriptorType = Set Header (0x00) */
    0x00, 0x00, 0x03, 0x06,     /* dwWindowsVersion = 0x06030000 */
    0x2E, 0x00,                 /* wTotalLength = 46 */
    /* Configuration Subset Header (8 字节) */
    0x08, 0x00,                 /* wLength = 8 */
    0x01, 0x00,                 /* wDescriptorType = Configuration Subset (0x01) */
    0x01,                       /* bConfigurationValue = 1 */
    0x00,                       /* bReserved */
    0x24, 0x00,                 /* wTotalLength = 0x24 = 36 (8 + 8 + 20) */
    /* Function Subset Header (8 字节) */
    0x08, 0x00,                 /* wLength = 8 */
    0x02, 0x00,                 /* wDescriptorType = Function Subset (0x02) */
    0x00,                       /* bFirstInterface = 0 (CDC 控制接口, 函数首接口) */
    0x00,                       /* bReserved */
    0x1C, 0x00,                 /* wSubsetLength = 0x1C = 28 (8 + 20) */
    /* Compatible ID Descriptor (20 字节) */
    0x14, 0x00,                 /* wLength = 20 */
    0x03, 0x00,                 /* wDescriptorType = Compatible ID (0x03) */
    'C', 'D', 'C', 'A', 'C', 'M', 0x00, 0x00,   /* CompatibleID = "CDCACM\0\0" */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00  /* SubCompatibleID = 全 0 */
};

const usb_descriptor_t g_usb_descriptor =
{
    g_apl_device,                   /* Pointer to the device descriptor */
    g_apl_configuration,            /* Pointer to the configuration descriptor for Full-speed */
    g_apl_hs_configuration,         /* Pointer to the configuration descriptor for Hi-speed */
    g_apl_qualifier_descriptor,     /* Pointer to the qualifier descriptor */
    g_apl_string_table,             /* Pointer to the string descriptor table */
    NUM_STRING_DESCRIPTOR
};

/******************************************************************************
 Renesas Abstracted Peripheral Communications Devices Class Driver API functions
 ******************************************************************************/

/******************************************************************************
 * Function Name   : usb_cdc_fill_serial_from_uid
 * Description     : 用芯片出厂 UID(0x01008190) 填充 iSerialNumber 字符串, 让每块
 *               : 板的 USB 序列号唯一。写死序列号会让 Windows 无法区分设备, 每次
 *               : 重连都重建 devnode、重走"设备安装"流程(即"等十几秒才出 COM")。
 *               : 必须在 R_USB_Open() 之前调用 (枚举时主机才读 string 6)。
 * Arguments       : none
 * Return value    : none
 ******************************************************************************/
#define CDC_UID_ADDR    (0x01008190UL)
void usb_cdc_fill_serial_from_uid (void)
{
    static const char HEX[] = "0123456789ABCDEF";
    uint32_t w0 = *(volatile uint32_t *) CDC_UID_ADDR;
    uint32_t w3 = *(volatile uint32_t *) (CDC_UID_ADDR + 12u);
    uint8_t  asc[16];
    int      i;
    int      k = 0;

    /* 与 $ID? 身份查询完全一致: w0 在前(8 hex), w3 在后(8 hex), 高位在前、大写 */
    for (i = 28; i >= 0; i -= 4)
    {
        asc[k++] = (uint8_t) HEX[(w0 >> i) & 0x0FU];
    }
    for (i = 28; i >= 0; i -= 4)
    {
        asc[k++] = (uint8_t) HEX[(w3 >> i) & 0x0FU];
    }

    g_cdc_string_descriptor6[0] = (uint8_t) (16u * 2u + 2u);     /* bLength = 34 */
    g_cdc_string_descriptor6[1] = USB_DT_STRING;
    for (i = 0; i < 16; i++)
    {
        g_cdc_string_descriptor6[(uint16_t) (2 + i * 2)] = asc[i];
        g_cdc_string_descriptor6[(uint16_t) (3 + i * 2)] = 0x00U;
    }
}

/******************************************************************************
 End  Of File
 ******************************************************************************/
