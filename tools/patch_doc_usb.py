"""把 USB CDC 数据通路章节写进 docs/3.5寸屏开发文档.md（插在 §6 之前）。"""
import io

PATH = 'docs/3.5寸屏开发文档.md'

SECTION = '''### 6.5 物理通路：Type-C 走原生 USB，不是 UART（2026-10-22 查实并实现）

**先把一件事钉死**：这块板的 Type-C **直连 MCU 原生 USB**，插上不会凭空出现 COM 口。
依据：引脚功能图上 `P814 = USB_DP`、`P815 = USB_DM`；立创开源工程 BOM 里只有 Type-C 座、
LDO、两个 LED、一个按键，**没有 CH340/CP2102 之类的 UART 桥**。

所以"Type-C 当串口用"只能在固件里实现 USB CDC 设备类。已落地：

| 环节 | 位置 | 关键点 |
|---|---|---|
| 描述符 | `src/usb/usb_cdc_descriptor.c` | 取自 FSP 的 `r_usb_pcdc_descriptor.c.template`，只改 VID:PID = `045B:0263`；`bDeviceClass=0x02` ⇒ Windows 自动挂 `usbser.sys`，不需要 INF |
| 驱动接入 | `src/usb/bsp_usb_cdc.c` | `R_USB_Open(&g_basic0_ctrl, &g_basic0_cfg)`；回调里把收到的字节交给 `UART_Link_PushBytes()`，**复用同一份帧解析与超时判据** |
| 引脚 | 同上，运行时配 | `R_IOPORT_PinCfg(... P814/P815 ... IOPORT_PERIPHERAL_USB_FS)`，不动 RASC 生成物 |
| 时钟 | `configuration.xml` → `ra_gen/bsp_clock_cfg.h` | 见下 |
| 生成方式 | `tools/patch_rasc_usb.py` + `rascc --generate` | `r_usb_basic_api.h` / `r_usb_pcdc_api.h` / `*_cfg.h` 都由 RASC 从模块模板渲染，FSP 安装包里没有这些文件，**只能让 RASC 生成** |

**时钟是这条路唯一的硬约束**

RA6E2 **没有 PLL2**（`BSP_FEATURE_CGC_HAS_PLL2 = 0`），HOCO 只有 16/18/20MHz 三档、没有 48MHz 档。
USB FS 需要 48MHz±0.25%，而 HOCO 精度只有 ±1% ⇒ **必须用外部 20MHz 晶振**。唯一组合：

```
MAIN_OSC 20MHz --PLL x12--> 240MHz --USBCKDIV /5--> 48MHz
                             └─ ICLK /2 -> 120MHz （240 超 200 上限, 只能除 2）
```

代价与连带分频（240MHz 下 /2 与 /4 都过不了各自上限）：

| 时钟 | 之前(240 不可能) | 现在 | 上限 |
|---|---|---|---|
| ICLK | 200MHz | **120MHz** | 200 |
| PCLKA | 100MHz | 60MHz | 100 |
| PCLKB | 50MHz | **30MHz** | 50（/4=60 超限） |
| PCLKC | 50MHz | **30MHz** | 50 |
| PCLKD | 100MHz | 60MHz | 100 |
| FCLK | 50MHz | **30MHz** | 50 |

**⚠ 晶振能起振的两个前提（缺一个就卡死在 `SystemInit`，现象是"两块屏全黑、RAM 全垃圾"）**

1. **P212/P213 必须空出来**给 EXTAL/XTAL —— 屏的 RS/RES 已改到 **P403/P409**（§2.5）。
2. **`BSP_CFG_AUTODRVEN=1`**。RASC 的 RA6E2 BSP 面板没有"主振驱动能力"选项，生成的
   `bsp_clock_cfg.h` 里既无 `CGC_MAINCLOCK_DRIVE` 也无 `BSP_CFG_AUTODRVEN` ⇒ `MODRV = 0`（最低驱动）
   带不动 20MHz。这个宏放在 **CMakeLists 的 `target_compile_definitions`** 里，
   所以 `rascc --generate` 重新生成也丢不掉。

实测判据（探针直接读 BSP 自己的频率表 `g_clock_freq`）：`MAIN_OSC = 20000000`、`PLL = 240000000`；
`R_USB_Open` 返回 0；`g_usb_pin_err_dp/dm = 0`。之后要等 PC 插线才有 `g_usb_events`。

**RASC 生成流程（改 USB 配置时照这个顺序跑）**

```
python tools/patch_rasc_usb.py projects/ra6e2_66_app/configuration.xml
rascc --generate --compiler GCC --device R7FA6E2BB projects/ra6e2_66_app/configuration.xml
python tools/fix_hal_data_usb.py projects/ra6e2_66_app
```

第三行是必须的：RASC 只生成 RTOS 版回调声明
（`#if 2 == BSP_CFG_RTOS` 里面的三参数版本），裸机少一个声明，
`hal_data.c` 的 `.p_usb_apl_callback = usb_callback` 会报 undeclared。
`usb_basic` 模块要用 **`basic_on_usb_nodmac` 变体**：普通版本要求 9 个管道
（`interface.mcu.usb.pipes.0.1.2.3.4.5.6.7.8.9`），而 RA6E2 只有 pipe 0/4/5/6/7。

---

'''


def main():
    s = io.open(PATH, encoding='utf-8').read()
    if '### 6.5 物理通路' in s:
        print('already there')
        return
    i = s.find('## 6. 通信协议')
    assert i > 0, '找不到 §6 标题'
    s = s[:i] + SECTION + s[i:]
    io.open(PATH, 'w', encoding='utf-8', newline='\n').write(s)
    print('doc updated')


if __name__ == '__main__':
    main()
