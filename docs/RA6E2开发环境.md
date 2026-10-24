# RA6E2 开发环境文档

> 目标板：立创·地奇星 RA6E2 开发板  
> 调试器：DAPLink（CMSIS-DAP v1）  
> 建档日期：2026-10-05

---

## 1. 硬件结论

### 1.1 MCU 型号：RA6E2（**不是 RA4E2**）

| 证据           | 内容                                                                           |
| ------------ | ---------------------------------------------------------------------------- |
| 官方 datasheet | `RA6E2 Group`, R01DS0410EJ0130 Rev.1.30（200 MHz / 128–256 KB flash）          |
| 示例工程         | `DQX_RA6E2.7z` 内 4104 个条目全部指向 `ra6e2`，`ra4e2` 命中 **0** 次；器件头文件 `R7FA6E2BB.h` |
| 片上 ROM 表     | 含 **CoreSight ETM-M33**，RA6E2 规格书明确列出，RA4E2 无此模块                             |

> ⚠️ 引脚图 PDF 标题栏里的 `R7FA4E2B93CNE` 是 **RA4E2 / RA6E2 同族 pin-compatible 共用原理图** 的残留，不可作为型号依据。

**芯片资源**

**确切型号：`R7FA6E2BB`（256 KB flash 变体）**

| 项目             | 值                                               |
| -------------- | ----------------------------------------------- |
| 内核             | Arm Cortex-M33（ARMv8-M，含 TrustZone），r0p4-00rel0 |
| 主频             | 最高 200 MHz                                      |
| Code flash     | **256 KB** @ `0x00000000`–`0x00040000`          |
| Data flash     | 4 KB @ `0x08000000`                             |
| SRAM           | 40 KB @ `0x20000000`–`0x2000A000`               |
| Option-setting | `0x0100A000`，768 B                              |

> ⚠️ probe-rs 里 RA6E2 有两个变体，别选错：
>
> - `R7FA6E2B9` → 128 KB flash
> - **`R7FA6E2BB` → 256 KB flash** ← 本板
>
> 判定依据：器件头文件名为 `R7FA6E2BB.h`；示例工程生成的 `memory_regions.ld` 里  
> `FLASH_LENGTH = 0x40000`（256 KB）。两者内存布局不同，选错会导致烧录/校验失败。

### 1.2 SWD 接口（板上丝印）

```
SWDIO / SWCLK / RST / GND / +3.3V
```

---

## 2. 调试探针

### 2.1 本项目使用的探针

| 项目                | 值                                                |
| ----------------- | ------------------------------------------------ |
| USB VID:PID       | `0d28:0204`                                      |
| 序列号               | `0700000102620220523200063638414ba5a5a597969908` |
| MSD 盘符            | `DAPLINK`（不固定，本次 `F:`）                           |
| CDC 串口            | `COM6`                                           |
| HIC ID            | `97969908` → STM32F103xB（Cortex-M3 / 72 MHz）     |
| Interface Version | **0253**                                         |
| Local Mods        | `1`（第三方改版固件）                                     |
| USB 接口            | MSD、CDC、HID、**WebUSB**                           |

### 2.2 速率瓶颈（重要）

- CMSIS-DAP **v2 的 bulk 端点是在 interface version `0254` 才加入的**。本探针为 `0253`，  
  ⇒ **只有 CMSIS-DAP v1（HID）**。
- 设备管理器里看到的 `WebUSB: CMSIS-DAP`（WinUSB 绑定，subclass `0x03`）是 **浏览器烧录用，不是 v2**，  
  不要据此认为它是高速探针。

实测吞吐（已扣除 0.46 s 连接基线）：

| 条件             | 8 KB SRAM 读取耗时 | 吞吐            |
| -------------- | -------------- | ------------- |
| 默认 SWD 时钟      | 0.38 s         | ≈ **21 KB/s** |
| `--speed 8000` | 0.23 s         | ≈ **35 KB/s** |

烧满 128 KB flash 会比较慢（几十秒级）。这是探针固有限制，不是接线问题。

### 2.3 其它探针（**禁止使用**）

| 探针       | VID:PID     | 用途                                         |
| -------- | ----------- | ------------------------------------------ |
| FireDAP  | `0416:5021` | 已接 **STM32H743VIT6**，与本项目无关，**禁止对其发起任何操作** |
| ESP JTAG | `303a:1001` | ESP32-S3 内置 JTAG，与本项目无关，**禁止对其发起任何操作**     |

> 本项目**唯一允许使用的探针是 `0d28:0204`**。所有 probe-rs / pyocd 命令都必须带  
> `--probe 0d28:0204`，避免误连到 FireDAP 或 ESP JTAG。

---

## 3. 连通性测试结果（2026-10-05，已通过）

工具：`probe-rs 0.29.1`（`E:/.cargo/bin/probe-rs`）  
命令：`--probe 0d28:0204 --chip R7FA6E2BB`

| 检查项                | 结果                                                                        |
| ------------------ | ------------------------------------------------------------------------- |
| 探针枚举               | `DAPLink CMSIS-DAP -- 0d28:0204` ✔                                        |
| Debug Port         | **DPv2, Designer: Renesas Electronics, Part `0x4d0`** ✔                   |
| CPUID              | IMPLEMENTER=ARM, **PARTNO=Cortex-M33**, REVISION=4 ✔                      |
| ROM 表              | Renesas ROM Table + ETM-M33 / CTI / TraceFunnel / TMC / ITM / DWT / FPB ✔ |
| 读 Flash 向量表 @0x0   | `20001a10 00001981 000017a9 ...`（MSP 落 SRAM、复位向量落 Flash）✔                 |
| 读 SRAM @0x20000000 | 正常 ✔                                                                      |
| DHCSR @0xE000EDF0  | `0x05100001`，`C_DEBUGEN=1` → **调试口开放，芯片未锁死** ✔                            |

### 补充验证：暂停内核 / 读寄存器（2026-10-05，已通过）

经 `probe-rs gdb` + `arm-none-eabi-gdb` 验证，调试控制权完整可用：

```
(gdb) target remote :1337
(gdb) monitor halt
0x0000038a in R_BSP_Init_RTC () at .../bsp_clocks.c:3392
r0   0x20000278    r1  0x0         r2  0x22400000
sp   0x200019b8    pc  0x38a       xpsr 0x29000000
```

- 内核成功暂停并读出 PC / SP / R0–R2 / xPSR ✔
- 验证后已 `probe-rs reset` 恢复运行，复查 Flash 向量表未变动 ✔

> 说明：`probe-rs write` **只支持 RAM 地址**，写 `0xE000EDF0`(DHCSR) 是空操作，  
> 用它测 halt 无效；`probe-rs debug` 是 DAP server，不能独立交互使用。  
> 正确路径是 `probe-rs gdb` + `arm-none-eabi-gdb`。

### 已验证：烧录链路（2026-10-05，已通过）

probe-rs **确实随包提供了 RA6E2 的 flash 算法**，实测烧录 + 回读校验通过：

```
[1/1] 烧录 ELF 到 RA6E2 (将擦除板上现有程序; 跳过 option-setting 段)
     Finished in 5.42s
[1/1] 校验片上内容与 ELF 是否一致
Verification successful
```

实测中确认的两条硬约束（详细数据在 `RA6E2的开发文档.md` §6）：

1. **必须跳过 option-setting 段**。probe-rs 的 RA6E2 算法不覆盖 `0x0100A100`–`0x0100A2FF`，  
   不跳过会报 `No flash memory contains the entire requested memory range`。  
   实测 chip 上已有的 OFS 值（OFS0/SAS 全 `0xFF`，OFS1 = `0xFFFFFDFF` 仅 HOCOFRQ 位）  
   与官方工程生成的完全一致，**无调试锁定位**，因此跳过不写既安全又必需。
2. **烧完必须显式 `probe-rs reset`**。`download` / `verify` 结束后内核是停机状态，  
   不复位程序不会跑。

### 仍未验证

- **串口日志通道**：DAPLink 的 CDC 是 COM6，是否接到 RA6E2 的 UART 引脚未确认。  
  当前 FSP 根把 SCI0 配在 **P100/P101**。
- **RTT / SWO**：未实测。当前用「探针直读 RAM 全局变量」替代（见开发文档 §5.4）。

---

## 4. 本机工具链清单

| 工具                      | 版本 / 路径                                                                                              | 状态                           |
| ----------------------- | ---------------------------------------------------------------------------------------------------- | ---------------------------- |
| **Arm GNU Toolchain**   | `14.3.Rel1`（gcc 14.3.1，gdb 15.2）<br />`E:\software\ArmGNUToolchain\14.3 rel1\`                       | ✔ 已在 PATH                    |
| **CMake**               | 3.27.4<br />`C:\Program Files\CMake\bin\cmake.exe`                                                   | ✔ 已在 PATH                    |
| **Ninja**               | VS2022 附带<br />`...\Microsoft VS Code\...\CMake\Ninja\ninja.exe`                                     | ✔ 已在 PATH                    |
| **probe-rs**            | 0.29.1<br />`E:\.cargo\bin\probe-rs.exe`                                                             | ✔ 已在 PATH                    |
| **FSP 6.6.0（源码）**       | `ra/fsp` + RA6E2 BSP + CMSIS_6<br />`E:\software\Renesas\FSP_6.6.0\`（git 浅克隆 tag v6.6.0）             | ✔ 已装（2026-10-05）             |
| RASC 6.6.0 配置器          | 装于 `E:\software\Renesas\RAC\`<br />（Eclipse RCP 版，含命令行 `eclipse\rascc.exe`）<br />完整用法见 `RASC开发文档.md` | ✔ 已装（2026-10-05）             |
| **当前 FSP 根（6.6.0 生成物）** | `E:\REA6E2_Dev\projects\ra6e2_66_app\`<br />（RASC 生成，自带 `ra/fsp` + `ra_gen` + 链接脚本）                  | ✔ 编译 / 烧录已跑通                 |
| FSP 5.8.0（回退基线）         | `E:\REA6E2_Dev\vendor\DQX_RA6E2\led\`<br />`E:\REA6E2_Dev\vendor\DQX_RA6E2\BH1750\`（含 I2C 配置）        | ✔ 已验证可编译，仅作回退                |
| pyocd                   | 0.45.1                                                                                               | ⚠ **不带 RA target**，RA6E2 不要用 |
| Python                  | 3.13（`.workbuddy` 托管 venv）                                                                           | ✔                            |
| CLion                   | `E:\software\Clion`                                                                                  | ✔ 可选 IDE                     |
| OpenOCD                 | —                                                                                                    | ✘ 未安装                        |
| e²studio                | —                                                                                                    | ✘ 未安装                        |
| `make`                  | —                                                                                                    | ✘ 无 → 统一用 **Ninja**          |

### FSP（Renesas Flexible Software Package）

FSP 源码有三处，**编译只认第 1 处**（RASC 生成工程自带），其余两份用于查阅与回退：

| 用途          | 位置                                                                 |
| ----------- | ------------------------------------------------------------------ |
| ★ **参与编译**  | `E:\REA6E2_Dev\projects\ra6e2_66_app\ra\`（RASC 6.6.0 生成时拷入）        |
| 查阅 / 比对 API | `E:\software\Renesas\FSP_6.6.0\`（git 浅克隆 tag v6.6.0，另补了 CMSIS_6 头） |
| 5.8.0 回退    | `E:\REA6E2_Dev\vendor\DQX_RA6E2\BH1750\`                           |

工程内 FSP 的结构：

```
projects/ra6e2_66_app/
├─ ra/fsp/                              FSP 源码 + API
│  ├─ src/bsp/mcu/ra6e2/                ← RA6E2 BSP
│  ├─ src/bsp/cmsis/Device/RENESAS/Include/R7FA6E2BB.h
│  └─ src/r_iic_b_master/ r_sci_uart/   本工程用到的外设驱动
├─ ra/arm/CMSIS_6/CMSIS/Core/Include/   CMSIS 内核头
├─ ra_cfg/fsp_cfg/bsp/                  配置头
├─ ra_gen/                              ★ RASC 生成，不要手改
├─ script/fsp.ld  fsp_gen.ld  memory_regions.ld   链接脚本（6.x 拆成三份）
└─ configuration.xml                    配置源（改外设的入口）
```

生成命令（详见 `RA6E2的开发文档.md` §3.2）：

```bash
"/e/software/Renesas/RAC/eclipse/rascc.exe" --generate \
    --compiler GCC --device R7FA6E2BB configuration.xml
```

---

## 5. 常用命令

```bash
PR="E:/.cargo/bin/probe-rs"

# 枚举探针
$PR list

# 连通性自检（打印 DP / CPUID / ROM 表）
$PR info --chip R7FA6E2B9 --probe 0d28:0204

# 读 Flash 向量表
$PR read --chip R7FA6E2B9 --probe 0d28:0204 b32 0x00000000 8

# 读 SRAM
$PR read --chip R7FA6E2B9 --probe 0d28:0204 b32 0x20000000 8

# 读调试状态寄存器 DHCSR
$PR read --chip R7FA6E2B9 --probe 0d28:0204 b32 0xE000EDF0 1
```

---

## 6. 资料索引

`E:\BaiduNetdiskDownload\地奇星RA6E2开发板资料\`

| 路径                                                               | 说明                                                                       |
| ---------------------------------------------------------------- | ------------------------------------------------------------------------ |
| `2、引脚功能图\地奇星-功能引脚图.pdf`                                          | 引脚图，1 页                                                                  |
| `4、官方文档\datasheet.pdf`                                           | RA6E2 Group datasheet，95 页                                               |
| `4、官方文档\RA6E2 Group.pdf`                                         | 硬件用户手册                                                                   |
| `4、官方文档\寄存器V5.9.pdf`                                             | 寄存器手册                                                                    |
| `3、程序\DQX_RA6E2.7z`                                              | e²studio + FSP 示例工程 ×14（adc / UART / GPT_PWM / RTC / QSPI_FLASH / led …） |
| `5、电压电流表\voltammeter.hex`                                        | 电压电流表固件                                                                  |
| `1串口&USB下载软件安装包\Renesas_Flash_Programmer_Package_V32000-doc.zip` | RFP v3.20（串口/USB 下载）                                                     |

---

## 7. 已搭建的构建环境

### 7.1 目录结构

```
E:\REA6E2_Dev\
├─ CMakeLists.txt                    顶层工程
├─ cmake\
│  ├─ arm-none-eabi-ra6e2.cmake     工具链文件
│  ├─ PostBuild.cmake               构建后处理(生成 hex/bin/srec)
│  └─ ld\
│     ├─ fsp.ld                     Renesas FSP 链接脚本
│     └─ memory_regions.ld          存储区定义(自动生成)
├─ src\                             应用代码
│  ├─ hal_entry.c
│  └─ Apply\app.c
├─ vendor\DQX_RA6E2\led\            从示例工程提取的 FSP/CMSIS/BSP/ra_gen
├─ scripts\
│  ├─ build.sh                      构建
│  ├─ flash.sh                      烧录
│  ├─ rtt.sh                        抓 RTT 日志
│  ├─ run-sh.ps1                    Windows 入口(通用转发, 内部调 Git bash)
│  └─ build.ps1 / flash.ps1 / rtt.ps1   同名 .sh 的 PowerShell 入口
├─ build\                           构建产物
└─ docs\
```

### 7.2 编译参数（照抄 e²studio 官方工程）

从示例工程自带的 `Debug/compile_commands.json` 与 `Debug/led.elf.in` 直接提取：

```
-mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16
-ffunction-sections -fdata-sections -fno-strict-aliasing
--param=min-pagesize=0
-D_RENESAS_RA_ -D_RA_CORE=CM33 -D_RA_ORDINAL=1
-T fsp.ld  -Wl,--gc-sections  --specs=nano.specs
```

### 7.3 构建

```bash
cd /e/REA6E2_Dev
./scripts/build.sh            # 增量
./scripts/build.sh --clean    # 全量重建
```

> **Windows PowerShell** 请用 `\scripts\build.ps1`（`flash.ps1` / `rtt.ps1` 同理）。  
> 直接敲 `.sh` 路径**不会执行脚本**，只会被 VS Code 打开 —— 原理与正确用法见  
> `RA6E2的开发文档.md` §0.1。

或直接用 CMake：

```bash
export PATH="/c/Windows/System32:/c/Windows:$PATH"   # 见 7.4
cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-ra6e2.cmake
cmake --build build
```

CMake 目标：

| 目标          | 作用                                       |
| ----------- | ---------------------------------------- |
| （默认）        | 编译 → `ra6e2_fw.elf/.hex/.bin/.srec/.map` |
| `size`      | 打印 Flash/RAM 占用                          |
| `probe`     | DAPLink 连通性自检（只读）                        |
| `gdbserver` | 启动 GDB 服务器 `:1337`（不烧录）                  |
| `flash`     | 烧录（**会擦除板上现有程序**）                        |
| `verify`    | 烧录后与片上内容比对                               |

> `probe-rs download` **没有 `--verify` 选项**，校验要用独立的 `probe-rs verify` 子命令。

调试会话：

```bash
cmake --build build -t gdbserver          # 终端 1
arm-none-eabi-gdb build/ra6e2_fw.elf      # 终端 2
(gdb) target remote :1337
(gdb) monitor halt
(gdb) info registers
```

### 7.4 ⚠ 已知坑：Git Bash 的 PATH 缺 System32

Git Bash 默认 PATH 不含 `C:\Windows\System32`，CMake 生成的构建后处理是嵌套的  
`cmd.exe /C "..."`，内层 `cmd.exe` 找不到，报：

```
'cmd.exe' 不是内部或外部命令，也不是可运行的程序或批处理文件。
```

解决办法：构建前 `export PATH="/c/Windows/System32:/c/Windows:$PATH"`。  
`scripts/build.sh` 已内置处理。用 PowerShell / CMD 构建则不受影响。

### 7.5 ⚠ `.bin` 是 16 MB，别被吓到

RA 的 option-setting 区在 `0x0100A100`，`objcopy -O binary` 会把 `0x0`–`0x0100A2xx`  
整段铺开，所以 `.bin` 有 16.8 MB。实际代码只有 2740 B。  
**烧录用 `.elf` 或 `.srec` / `.hex`，不要用 `.bin`。**

### 7.6 首次构建结果（2026-10-05）

```
Memory region         Used Size  Region Size  %age Used
           FLASH:        2500 B       256 KB      0.95%
             RAM:        1176 B        40 KB      2.87%
   text    data     bss     dec     hex filename
   2732       8    1168    3908     f44 build/ra6e2_fw.elf
```

- Machine: ARM / ELF32 / EXEC，入口 `0x000000d5` = `Reset_Handler`(0xd4) | Thumb 位 ✔
- 23 个 C 文件全部编译通过，链接通过 ✔

---

## 8. 后续计划

- [x] 从示例工程提取 FSP / CMSIS / BSP / 链接脚本，落地为独立的 CMake 工程


- [x] 编写 `arm-none-eabi` 工具链文件（Cortex-M33）
- [x] 编译最小固件，验证工具链可用
- [x] 验证 halt / 寄存器读取（经 GDB 路径，已通过）
- [x] **~~实测烧录链路~~**~~ —— 会擦除板上现有程序，且 RA 的 option-setting 区写错会锁死 SWD，  
  属于对硬件的真实写操作，需用户点头。触发命令：`./scripts/flash.sh`~~

---

##
