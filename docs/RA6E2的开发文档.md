# RA6E2 开发文档

> 面向日常开发：怎么改代码、编译、加外设、烧录、调试。  
> 环境搭建 / 探针参数 / 型号判定等一次性结论，见同目录 `RA6E2开发环境.md`。  
> 最后更新：2026-10-06（新增 §0.2 编译；烧录/调试前移到编译之后，全文重新编号）  
> 当前工具链：**FSP 6.6.0 + RASC 生成**（`projects/ra6e2_66_app`），编译/烧录已跑通。

---

## 0. 三条命令起步

```
┌────────────────────────┬──────────────┬──────────┬────────────────┬───────────────┐
│内存类型 / 段           │   已用 [字节]│  占比 [%]│     剩余 [字节]│  总容量 [字节]│
├────────────────────────┼──────────────┼──────────┼────────────────┼───────────────┤
│FLASH 代码+常量         │              │          │                │               │
│  .text/.rodata         │        13,076│          │                │               │
│  .vectors              │            96│          │                │               │
│  小计                  │        13,188│      5.03│         248,956│        262,144│
├────────────────────────┼──────────────┼──────────┼────────────────┼───────────────┤
│RAM 数据                │              │          │                │               │
│  .bss                  │         6,860│          │                │               │
│  .noinit               │         5,144│          │                │               │
│  .data                 │           152│          │                │               │
│  小计                  │        12,164│     29.70│          28,796│         40,960│
└────────────────────────┴──────────────┴──────────┴────────────────┴───────────────┘

镜像总大小: 13,340 字节（.bin 可能因对齐而略大）
```

说明：

- 用 Unicode 制表符画的**实线**表格（`┌─┬─┐` `│` `├─┼─┤` `└─┴─┘`），边框统一黄色；  
  占比 ≥75% 黄色、≥90% 红色，否则绿色。
- ⚠ Unicode 制表符在**中文 locale + Nerd Font** 的终端下可能显示异常：  
  横线被按双宽渲染导致错位，或 `│` 被字体替换成图标/菱形。  
  若遇到，把 VSCode 终端字体换成普通等宽字体（Cascadia Mono / Consolas / 更纱黑体）。
- 该表由 `scripts/size_report.py` 生成（在 `cmake/PostBuild.cmake` 里自动调用），  
  想关颜色就加 `--no-color`（重定向日志时建议加上，免得混进 ANSI 转义序列）。
- 链接器原来的 `-Wl,--print-memory-usage` 已移除，这张表的信息是它的超集。
- **没有改动时增量编译不会重跑这张表**（Ninja 判定 ELF 没变）—— 想看表就  
  `--clean`，或改任意一个源文件再编译。

#### 什么时候需要 `--clean`

- 改了 `CMakeLists.txt`（尤其 `FSP_ROOT`、宏定义、编译/链接选项）
- 换 FSP 根、换工具链
- 出现莫名其妙的链接错误

> 增删 `.c` 文件**一般不用** `--clean`：`file(GLOB_RECURSE ... CONFIGURE_DEPENDS)`  
> 会自动重扫（见 §5.4.1）。`.h` 更不用管 —— 它压根不进编译列表，靠 `#include` 找。

#### 三个坑

1. **`.sh` 不是可执行类型**，在非 bash 的终端里直接敲会被 VS Code 当文本打开（§0.1）。
2. **必须先 `cd` 到工程根**：脚本里的 ELF 与 CMake 源目录都是相对路径，  
   在别的目录跑会报「未找到源文件」或烧到错误的文件。`.ps1` 入口已帮你处理。
3. **Git Bash 默认 PATH 缺 `C:\Windows\System32`**，会让 CMake 生成的嵌套  
   `cmd.exe /C` 报「'cmd.exe' 不是内部或外部命令」。  
   `build.sh` 已内置补 PATH；如果你手动敲 `cmake` 撞上这个，先执行  
   `export PATH="/c/Windows/System32:/c/Windows:$PATH"`。

---

## 1. 烧录

> ⚠ **先看清 shell**：`.sh` 在 PowerShell 里**不是可执行类型**。  
> 敲 `\scripts\flash.sh` 或 `.\scripts\flash.sh` **都不会执行脚本** ——  
> Windows 会用 VS Code 把它当文本打开，终端一声不响就回到提示符，看着像「命令无效」。  
> 原理与解决办法见 §0.1。下面按 shell 分开写，挑你终端对应的那一份。

```bash
# Git Bash
./scripts/flash.sh             # 烧录 build/ra6e2_fw.elf（6.6.0）
./scripts/flash.sh --verify    # 烧录 + 回读校验
```

```powershell
# VSCode 默认终端（Windows 上默认是它）—— 改用 .ps1 入口
.\scripts\flash.ps1
.\scripts\flash.ps1 --verify
```

### 1.0 只用 6.6.0 产物烧录（安全前提，务必遵守）

| 产物                                 | option-setting 段 | 烧录风险                                |
| ---------------------------------- | ---------------- | ----------------------------------- |
| **`build/ra6e2_fw.elf`（6.6.0，默认）** | **没有**           | ✅ 不触碰 `0x0100A100` 区，**不可能锁死 SWD**  |
| `build58/ra6e2_fw.elf`（5.8.0 回退）   | 有 5 个            | ⚠ 必须 `--skip-section`；漏掉就会尝试写 OFS 区 |

**为什么 6.6.0 天然安全**：因为不定义 `BSP_CFG_OPTION_SETTING_OFS0/1/OSIS` 宏（§5.3 坑 1），  
`bsp_linker.c` 根本不生成 OFS 数据，ELF 里也就没有 `.option_setting_*` 段 ——  
probe-rs 无从下手，自然不会去写那片**决定调试口是否锁死**的区域。

规矩：

1. **日常烧录一律用 `build/`（6.6.0）**。`build58/` 只在排查 FSP 兼容性时用，  
   且必须走 `scripts/flash.sh`（skip 名单已内置），不要自己拼 `probe-rs download`。
2. 烧前自检（**应当没有输出**）：
   ```bash
   arm-none-eabi-objdump -h build/ra6e2_fw.elf | grep option_setting
   ```
   有输出 ⇒ 这不是 6.6.0 产物，先搞清楚再烧。
3. 不要用 RFP / J-Flash 之类工具「顺手」写 option-setting 区。芯片上现有 OFS 值  
   （仅 HOCO 20 MHz 一位，见 §1.1）已经够用，写错就是锁死 SWD。
4. 真锁死了的救法见 §1.1 末尾（SCI/USB 启动模式 + RFP 擦除）。

**脚本已把这条规矩做成硬闸门**：`scripts/flash.sh` 先用 `objdump -h` 扫 elf，  
发现 option-setting 段就**直接拒绝烧录**（退出码 1），除非显式 `--allow-ofs`  
（这时它自动补上 `--skip-section`）：

```bash
./scripts/flash.sh --check-only                            # 只检查不烧
./scripts/flash.sh --elf build58/ra6e2_fw.elf              # ✘ 被拒绝并提示
./scripts/flash.sh --elf build58/ra6e2_fw.elf --allow-ofs  # 允许, 自动跳过 5 个段
```

`scripts/rtt.sh` 有**同一道闸门**（2026-10-05 补上）—— 因为它走的 `attach` 也会烧录，  
以前是绕过闸门直接烧的：

```bash
./scripts/rtt.sh --elf build58/ra6e2_fw.elf              # ✘ 被拒绝, 列出 5 个段, exit 1
./scripts/rtt.sh --elf build58/ra6e2_fw.elf --allow-ofs  # 允许, 自动 --skip-section
```

等价命令（`CMakeLists.txt` 的 `flash` / `verify` 目标就是这么拼的；  
对 6.6.0 产物这些 `--skip-section` 是空操作，段不存在会被忽略）：

```bash
# 写成一行：bash 的续行符是 \，PowerShell 的是反引号 `，
# 多行写法换个 shell 就废了，单行两种终端都能直接粘。
probe-rs download --chip R7FA6E2BB --probe 0d28:0204 --skip-section .option_setting_ofs --skip-section .option_setting_sas --skip-section .option_setting_ns --skip-section .option_setting_s --skip-section .id_code build/ra6e2_fw.elf
```

### 1.1 option-setting 段与芯片上的现有值（2026-10-05 实测）

> 6.6.0 产物没有这些段，本节内容是**为 5.8.0 回退（§5.4）准备的**，默认用不上。  
> 上面的 `--skip-section` 对 6.x 是空操作（段不存在会被忽略），所以脚本里统一带着。

probe-rs 的 RA6E2 烧录算法**不覆盖 option-setting 区**（`0x0100A100`–`0x0100A2FF`）。  
不跳过会直接报错退出：

```
Error: No flash memory contains the entire requested memory range 0x0100A100..0x0100A110.
```

且该区决定调试口锁定，本不该由我们写。实测**芯片上已有的值与官方工程生成的值完全一致**：

| 区域           | 地址            | 内容                                            |
| ------------ | ------------- | --------------------------------------------- |
| OFS0         | `0x0100A100`  | 全 `0xFF`（默认）                                  |
| SAS          | `0x0100A134`  | 全 `0xFF`（默认）                                  |
| OFS1（secure） | `0x0100A200`  | `0xFFFFFDFF` —— **仅 HOCOFRQ 位**（= 2 → 20 MHz） |
| BPS / PBPS   | `0x0100A204`… | 全 `0xFF`（无块保护）                                |

OFS1 里那一位的来源：`BSP_ROM_REG_OFS1_SETTING = (OFS1 & 0xFFFFF9FF) | (HOCOFRQ << 9)`，  
HOCOFRQ = 2 ⇒ `0xFFFFF9FF | 0x400 = 0xFFFFFDFF`，与 `bsp_clock_cfg.h` 的 20 MHz 一致。  
**没有任何调试锁定位**，所以跳过不写是安全的，也省掉锁死 SWD 的风险。

真锁死了的救法：走 SCI/USB 启动模式，用 Renesas Flash Programmer  
（`1串口&USB下载软件安装包\` 里的 RFP v3.20）连上后擦除。

### 1.2 烧完必须显式 reset

`probe-rs download` / `verify` 结束后**内核处于停机状态**，程序不会自动跑。  
必须：

```bash
probe-rs reset --chip R7FA6E2BB --probe 0d28:0204
```

判断依据：不 reset 时读 `.bss` 里的全局变量，读到的是**烧录算法的残留机器码**  
（probe-rs 会把 flash 算法下进目标 RAM 执行）——看起来像常量且恒定不变，很有迷惑性。

反过来，只想让**已在 flash 里的程序重新跑一遍**、不想再擦写，用 `probe-rs reset`  
就够了 —— `./scripts/rtt.sh --fresh` 内部就是这么做的（先 reset，再挂 RTT，  
于是能抓到完整启动 banner），全程不碰 flash。

### 1.3 其它约束

- **会整片擦除**。原始出厂固件在  
  `E:\BaiduNetdiskDownload\地奇星RA6E2开发板资料\5、电压电流表\voltammeter.hex`，需要时可恢复。
- 只用 `0d28:0204`。FireDAP（`0416:5021`，接 STM32H743）和 ESP JTAG（`303a:1001`）  
  与本项目无关，**禁止对其发起任何操作**。
- 实测耗时：8.6 KB 的固件约 **5.4 秒**（含擦除）。

---

## 2. 调试

### GDB 会话

```bash
# 终端 1
cmake --build build -t gdbserver          # probe-rs gdb，监听 :1337

# 终端 2
arm-none-eabi-gdb build/ra6e2_fw.elf
(gdb) target remote :1337
(gdb) monitor halt                        # 暂停内核
(gdb) info registers                      # 看寄存器
(gdb) break hal_entry
(gdb) continue
(gdb) detach
```

已实测：`monitor halt` 能暂停并读出 PC / SP / R0–R2 / xPSR。退出前记得 `detach`，  
必要时 `probe-rs reset --chip R7FA6E2BB --probe 0d28:0204` 让板子重新跑起来。

### 不能用 / 别踩的做法

| 做法                                     | 问题                                                           |
| -------------------------------------- | ------------------------------------------------------------ |
| `probe-rs write` 写 `0xE000EDF0`(DHCSR) | `write` 只支持 RAM 地址，写调试寄存器是**空操作**，测 halt 会得出假结论              |
| `probe-rs debug` 当交互调试器用               | 它是 DAP server，独立跑会报 `Failed to deserialize attach arguments` |
| `probe-rs download --verify`           | 该版本**没有 `--verify` 选项**；校验要用独立的 `probe-rs verify`            |

### RTT / SWO 日志（2026-10-05 实测）

结论先给：

| 通道                 | 结论                                         |
| ------------------ | ------------------------------------------ |
| **RTT**            | ✅ **可用，已实测稳定抓到日志**（推荐，见 §2.3.1）            |
| ITM → TRACESWO 引脚  | ❌ **不可用** —— 芯片 ROM 表里**没有 TPIU**，物理上送不出去  |
| ITM → TMC/ETF 片上缓冲 | ⚠ 命令能跑通、芯片侧也确认在写，但 probe-rs **读不出任何数据**，别用 |

#### 2.3.1 RTT：已实测可用（推荐）

**原理**：在 RAM 里放一个控制块（`_SEGGER_RTT`）+ 环形缓冲，主机周期通过 SWD 读走。  
不占 UART、不用额外引脚、不打断程序执行。

**实现位置**：`src/rtt/rtt_log.c/.h` —— 自己写的，控制块布局与 SEGGER RTT 一致  
（主机端按固定偏移解析），没用 SEGGER 官方源码。up 通道 0，缓冲 1024 B，  
满时丢弃（NO_BLOCK_SKIP），绝不阻塞业务。开销：Flash +3.4 KB、RAM 1 KB。

```bash
# 原生命令（Git Bash）
probe-rs attach --chip R7FA6E2BB --probe 0d28:0204 --speed 1000 --preverify build/ra6e2_fw.elf

# 脚本（默认已带 1000 kHz 与 chip/probe）
./scripts/rtt.sh                 # 热挂载：不烧不复位，看正在跑的程序
./scripts/rtt.sh --fresh         # 先复位再挂：程序从头跑，日志含启动 banner
./scripts/rtt.sh --file rtt.log  # 同时存文件
```

```powershell
# Windows PowerShell（见 §0.1）
.\scripts\rtt.ps1
.\scripts\rtt.ps1 --fresh
```

实测输出：

```text
=== RA6E2 OLED demo (RTT log) ===
FSP 6.6.0 | R7FA6E2BB | I2C P408(SCL)/P206(SDA) @100kHz
OLED init ...
OLED_Init -> err=0, addr=0x3C, init_err=0
frames=10 off=16 i2c_err=0 retry=0 last=0 xfer=0
frames=20 off=36 i2c_err=0 retry=0 last=0 xfer=0
```

**三个必知的坑**

1. **默认时钟会偶发失败** —— 用探针默认速度时，RTT 控制块的块传输偶尔错乱，  
   probe-rs 日志刷 `Expected 8 responses, got 15 responses`，  
   然后一直报 `Failed to attach - control block not found`（抓不到任何东西）。  
   **加 `--speed 1000` 后稳定**。实测 1 M / 2 M / 4 MHz 都可用，默认速度时好时坏。
2. **`attach` 不一定会重新烧录**（2026-10-05 更正，原来写错了）—— 取决于  
   flash 内容是否已一致：
   | flash 内容                                               | 实际行为                                               |
   | ------------------------------------------------------ | -------------------------------------------------- |
   | **一致**（没改代码，或上次已烧过）                                    | **既不擦写也不复位**，直接挂上正在跑的程序 —— 这才是 RTT 该有的用法，**不打断程序** |
   | **不一致**（改过代码）                                          | 烧录 + 复位 + 从头跑，日志带启动 banner                         |
   | 实测证据：热挂载那次抓到的第一条是 `frames=8070`（没有启动 banner），          |                                                    |
   | 同时 `g_oled_frames` 3 秒内从 10814 涨到 10843 —— 程序从头到尾没被打断。 |                                                    |
   - `attach` **没有 `--no-flash` 开关**；ELF 是**必填位置参数** —— 即使不烧也要给，  
     probe-rs 靠它定位 `_SEGGER_RTT` 的地址。
   - ⚠ **`--force-flash` 不是"强制重写"开关**：内容一致时 probe-rs 连 `reset`+`run`  
     一起跳过（实测 `--fresh` 的旧实现抓到的仍是 `frames=9200`，没有 banner）。  
     **想看启动日志要用 `--fresh`**，脚本会先执行 `probe-rs reset` 再挂载。
   - 默认模式长时间没输出 = 芯片多半是 halt 的 ⇒ 加 `--fresh`。
   - 既然 `attach` 自带烧录，改完代码**直接 `rtt.sh` 即可**，不用先跑 `flash.sh`。
3. 控制块符号名必须是 **`_SEGGER_RTT`** 且在 RAM（本工程在 `0x20000000`）。  
   自检方式：`probe-rs read ... b8 0x20000000 32`，应看到  
   `53 45 47 47 45 52 20 52 54 54`（= "SEGGER RTT"）。


> 退出时如果用了重定向到文件，Rust 的行缓冲可能在进程被强杀时丢最后几行；  
> 正常 Ctrl-C 退出不受影响。要长期存盘用 `--target-output-file`。

#### 2.3.2 ITM：两条路都实测过了，不通

ROM 表实测（`probe-rs info`）：

```text
0xe0000000 ITM            ✔ 存在
0xe0041000 ETM (ETM-M33)  ✔ 存在
0xe0047000 TraceFunnel     ✔ 存在
0xe0048000 TMC (可作 ETF)  ✔ 存在
0xe0001000 DWT / 0xe0002000 FPB / 0xe0042000 CTI  ✔
—— 没有 TPIU ——
```

| 路线                                                          | 实测结果                                                                                          |
| ----------------------------------------------------------- | --------------------------------------------------------------------------------------------- |
| SWO 引脚：`probe-rs itm --chip ... swo 3000 200000000 1000000` | `The requested component 'TPIU (Trace Port Interface Unit)' was not found` —— **芯片没 TPIU，死路** |
| ETF：`probe-rs itm --chip ... memory 200000000`              | 命令正常跑完、识别到 TMC、程序也确认在跑（帧计数持续增长），但**输出 0 字节**                                                  |

ETF 那条路还额外做了两步排查，都没救回来：

- 一开始芯片侧检查 `ITM_TCR.ITMENA` / `ITM_TER[0]`，发现 **probe-rs 根本不帮芯片使能这两个位**，  
  于是数据被我们自己全丢了。改成无条件写，并让 `ITM_Init()` 自己解锁  
  `ITM_LAR` / `FUNNEL_LAR`、使能 ITM 与 TraceFunnel（`src/rtt/itm_log.c`）；  
  改完 `g_itm_dropped` 归零，说明芯片侧确实在往 FIFO 写 —— **主机侧依然读不到**。
- 排除"抓取时被停机"：ITM 抓取期间读 `g_oled_frames` 仍在增长，程序没被 halt。

**结论：本项目用 RTT，ITM 相关代码保留但默认不用。**

#### 2.3.3 在自己的代码里加日志

```c
#include "rtt/rtt_log.h"

RTT_Printf("frames=%u err=%d
", (unsigned) g_oled_frames, (int) err);
RTT_WriteString("hello
");
```

- 语义同 printf，但 **nano.specs 不支持 `%f`**（浮点要另配 `-u _printf_float`）；
- 单次最长 128 字节，超长截断；
- 主机没连时写满即丢弃，**不会卡住程序**，可以放心放在高频路径；
- ITM 版是 `ITM_Printf` / `ITM_PutString`（`src/rtt/itm_log.h`），  
  用前要调一次 `ITM_Init()`；但按 §2.3.2 目前抓不到输出。

### 2.4 不开调试器也能看运行状态：直接读 RAM

本工程没有串口日志通道（见 §9），**最省事的观测手段是拿探针直接读全局变量**。  
把要看的量声明成 `volatile` 全局量即可：

```bash
# 1) 查地址
arm-none-eabi-nm build/ra6e2_fw.elf | grep g_oled_

# 2) 读（b32 = 按 32 位读，最后一个数是字数）
probe-rs read --chip R7FA6E2BB --probe 0d28:0204 b32 0x20000a5c 2
```

要点：

- **读数显示的是小端数值**。例如读到 `00000001 3c020001`，第二个数是 `0x3C020001`，  
  对应内存字节 `[01, 00, 02, 3C]` —— 即按变量声明顺序逐字节对回去。
- **程序必须真的在跑**。烧录/校验后要先 `probe-rs reset`，否则读到的是 flash 算法残留（§1.2）。
- 读通道本身可以这样自检（写已知值再读回）：
  ```bash
  probe-rs write --chip R7FA6E2BB --probe 0d28:0204 b32 0x20003000 0x12345678
  probe-rs read  --chip R7FA6E2BB --probe 0d28:0204 b32 0x20003000 1   # → 12345678
  ```

> 由于没有 GDB 也能读，很多调试场景不必起 gdb server。  
> GDB 那条路还有个坑：`monitor halt` 不是有效子命令（帮助里是 `reset halt`）。

---

## 3. 工程结构：哪些能改，哪些不能改

```
E:\REA6E2_Dev\
├─ src\                       ★ 你的代码写这里
│  ├─ hal_entry.c              入口：复位后经 main() 调到这里（只有 Run();）
│  ├─ Apply\app.c              主流程：OLED 初始化 → 信息屏/位图屏 → TSN 温度屏常驻（见 `1.3寸OLED开发文档.md` §7）
│  ├─ i2c\bsp_i2c.c/.h         I2C 总线封装（FSP r_iic_b_master，P408=SCL / P206=SDA）
│  ├─ oled\bsp_oled.c/.h       1.3 寸 I2C OLED 驱动（SH1106 @ 0x3C，见 `1.3寸OLED开发文档.md` §3）
│  ├─ oled\oled_font.h         8x16 ASCII 字库（从官方 OLED 工程提取，见同上 §4.4）
│  └─ hal_callbacks.c          ra_gen/hal_data.c 要求的回调存根（见同上 §6）
├─ projects\ra6e2_66_app\  ★ 当前 FSP 根（RASC 6.6.0 生成，默认只读）
│  ├─ ra\                      FSP 库源码（BSP / r_iic_b_master / r_sci_uart / r_ioport / CMSIS）
│  ├─ ra_gen\                  ★ 配置生成代码 —— I2C 已配在 P408(SCL0_B) / P206(SDA0_C)
│  ├─ ra_cfg\fsp_cfg\          配置头文件
│  ├─ script\fsp.ld            链接脚本（INCLUDE 下面两个）
│  ├─ fsp_gen.ld               6.x 新增：段定义（RASC 生成）
│  └─ memory_regions.ld        6.x 新增：存储区域（RASC 生成）
├─ vendor\DQX_RA6E2\BH1750\  ⚠ 5.8.0 回退 FSP 根（见 §5.4）
├─ cmake\patch66\              ★ 6.x 缺头补丁（bsp_mcu_ofs_cfg.h，见 §5.3）
├─ cmake\ld\                  ⚠ 5.8.0 链接脚本副本，6.x 下已不用
├─ CMakeLists.txt             源文件/编译参数/烧录参数集中在这里
├─ build\                     产物（6.6.0，★ 日常烧录用这个）
└─ build58\                   产物（5.8.0 回退，见 §1.0）
```

> 当前 `FSP_ROOT = projects/ra6e2_66_app`（FSP 6.6.0，RASC 生成）。  
> 底稿用的是 **BH1750** 的配置 —— 它原本把 `r_iic_b_master` 配在 P407/P408。  
> **2026-10-23 已把 SDA 由 P407 迁到 P206、SCL 保持 P408**，因为 P407 是 RA6E2 唯一的
> `USB_VBUS` 检测脚，被 I2C 占用会让 USB 设备枚举失败。详见 `1.3寸OLED开发文档.md` §2.3。
> 重新生成见 §5.2，回退 5.8.0 见 §5.4。

| 位置               | 能否改 | 说明                                 |
| ---------------- | --- | ---------------------------------- |
| `src/**`         | ✅   | 应用代码                               |
| `CMakeLists.txt` | ✅   | 加源文件、改宏、切 FSP_ROOT                 |
| `cmake/ld/*.ld`  | ⚠   | 改存储布局才动；改前先备份                      |
| `vendor/**`      | ❌   | 官方示例原始件，要改先复制到 `src/`              |
| `ra_gen/**`      | ⚠   | 由 RASC 生成，**不要手改**。要改外设就重新生成（§5.2） |

### 3.1 官方示例工程的标准目录结构（照抄这个约定）

每个 e²studio / FSP 工程都是这个骨架（以 `DQX_RA6E2` 下各工程为准）：

```
<工程名>\
├─ .project  .cproject  .settings\        e²studio 工程文件（我们用不到）
├─ configuration.xml                      ★ FSP 配置源：外设/引脚/时钟/中断的勾选记录
├─ ra\                                    FSP 库（只读）
│  ├─ fsp\inc\
│  │  ├─ api\                             bsp_api.h、r_ioport_api.h…
│  │  └─ instances\                       各外设实例头（r_ioport.h 等）
│  ├─ fsp\src\
│  │  ├─ bsp\mcu\ra6e2\                   ★ 本芯片的 BSP
│  │  ├─ bsp\mcu\all\                     BSP 通用实现（bsp_clocks / bsp_io / bsp_irq…）
│  │  ├─ bsp\cmsis\Device\RENESAS\Include\★ R7FA6E2BB.h 器件头
│  │  ├─ bsp\cmsis\Device\RENESAS\Source\ startup.c / system.c
│  │  └─ r_xxxx\                          各外设驱动（工程用到哪个才有哪个）
│  └─ arm\CMSIS_6\CMSIS\Core\Include\     CMSIS 内核头
├─ ra_cfg\fsp_cfg\bsp\                    配置头
├─ ra_gen\                                ★ 配置器生成，不要手改
│  ├─ hal_data.c / .h                     外设句柄与控制块（g_uart0_ctrl…）+ 回调挂载点
│  ├─ pin_data.c                          引脚配置表 g_bsp_pin_cfg_data[]
│  ├─ vector_data.c / .h                  中断向量表
│  ├─ common_data.c / .h                  各驱动配置结构体
│  ├─ main.c                              只有三行：hal_entry(); return 0;
│  └─ bsp_clock_cfg.h                     时钟配置
├─ script\fsp.ld                          链接脚本（INCLUDE memory_regions.ld）
├─ src\                                   ★ 业务代码全在这里
│  ├─ hal_entry.c                         入口，通常只有 Run();
│  ├─ Apply\app.c / app.h                 应用主循环（Run() 的实现）
│  └─ <模块>\bsp_<模块>.c / .h            一个外设/模块一个目录
└─ Debug\                                 官方构建产物（led.elf / makefile / compile_commands.json）
```

**`src/` 的命名规律**（三套工程实测对比）：

| 工程              | `src/` 下的内容                                                                                          |
| --------------- | ---------------------------------------------------------------------------------------------------- |
| `led`           | `hal_entry.c` + `Apply\app.c/.h`                                                                     |
| `UART`          | `hal_entry.c` + `Apply\app.c/.h` + `uart\bsp_uart.c/.h`                                              |
| `adc`           | `hal_entry.c` + `Apply\app.c/.h` + `adc\bsp_adc.c/.h` + `uart\bsp_uart.c/.h`                         |
| 模块移植版（如 BH1750） | `hal_entry.c` + `Applay\app.c/.h` + `BSP_Code\bsp_bh1750.c/.h` + `BSP_Code\uart_debug\bsp_uart.c/.h` |

规律总结：

- **驱动文件一律叫 `bsp_<外设>.c / .h`**，放在**以该外设命名的子目录**里
- `hal_entry.c` 保持极简，只调 `Run()`
- `Apply/app.c` 是主循环所在（模块移植版拼成了 `Applay`，是官方的拼写笔误，不是约定）
- 每个子目录里 `.c/.h` 成对出现，头文件用 `extern` 把接口暴露给 `app.c`

---

## 4. 板载资源速查

| 资源             | 引脚 / 值                      | 备注                                                                                |
| -------------- | --------------------------- | --------------------------------------------------------------------------------- |
| **板载 LED**     | **P402**                    | `R_IOPORT_PinWrite(..., BSP_IO_PORT_04_PIN_02, BSP_IO_LEVEL_LOW)` 点亮 —— **低电平点亮** |
| SWDIO          | P108                        | `IOPORT_PERIPHERAL_DEBUG`                                                         |
| SWCLK          | P300                        | `IOPORT_PERIPHERAL_DEBUG`                                                         |
| Code flash     | 256 KB @ `0x00000000`       |                                                                                   |
| Data flash     | 4 KB @ `0x08000000`         | 10 万次擦写                                                                           |
| SRAM           | 40 KB @ `0x20000000`        |                                                                                   |
| Option-setting | `0x0100A100` / `0x0100A200` | ⚠ 写错会锁死 SWD                                                                       |

内核：Cortex-M33 @ 最高 200 MHz，带 TrustZone。时钟配置见  
`projects/ra6e2_66_app/ra_gen/bsp_clock_cfg.h`。

### 版本与工程属性（从 `configuration.xml` 实测提取）

| 项           | 值                                            | 来源                                          |
| ----------- | -------------------------------------------- | ------------------------------------------- |
| 芯片完整型号      | **`R7FA6E2BB3CNE`**（64 引脚 LQFP，256 KB）       | `#TargetName#`                              |
| probe-rs 目标 | `R7FA6E2BB`                                  | `#DeviceCommand#`                           |
| 架构 / 内核     | RA6E2 / CM33（Cortex-M33）                     | `CPU` / `Core`                              |
| **FSP 版本**  | **6.6.0**（示例工程原始为 5.8.0，已迁移）                 | `#FSPVersion#` + `ra/fsp/inc/fsp_version.h` |
| RTOS        | 无（裸机 bare-metal）                             | `#RTOS# = _none`                            |
| 官方工具链       | `gcc-arm-embedded` 13.2.1（本机用 14.3.1，已验证可编译） | `#ToolchainVersion#`                        |
| ROM 大小      | 262144 B（256 KB）                             | `config.bsp.rom_size_bytes_hidden`          |

> ⚠ 芯片完整型号 `R7FA6E2BB3CNE` 在这里得到最终确认，与器件头文件 `R7FA6E2BB.h`  
> 及 `memory_regions.ld` 的 `FLASH_LENGTH = 0x40000` 三者一致。

---

## 5. 加外设驱动：用 RASC 生成 ra_gen（已确定方案）

RA 的外设要靠 **FSP 配置生成代码（`ra_gen/`）** 才能用，不能只加一个 `.c`。  
本项目已确定：**用 RA Smart Configurator 6.6.0（独立配置器，不装 e²studio）生成**，  
走命令行 `rascc.exe` 无头生成，产物即 `projects/ra6e2_66_app`（当前 `FSP_ROOT`）。  
下面记录的是**已经装好并跑通的状态**，照做即可，不需要再选型。

### 5.1 已装好的环境（2026-10-05 实测跑通）

| 组件                            | 位置                                       | 状态                                                                    |
| ----------------------------- | ---------------------------------------- | --------------------------------------------------------------------- |
| RASC 6.6.0 配置器（含 `rascc.exe`） | `E:\software\Renesas\RAC\`               | ✔ 已装                                                                  |
| FSP 6.6.0 源码（查阅用）             | `E:\software\Renesas\FSP_6.6.0\`         | ✔ 已装（git 浅克隆 tag v6.6.0）                                              |
| **实际参与编译的 FSP 源码**            | `projects/ra6e2_66_app\ra\`              | ✔ **RASC 生成时自带**，编译只认这一份                                              |
| Arm GNU 工具链                   | `E:\software\ArmGNUToolchain\14.3 rel1\` | ✔ 已装                                                                  |
| 安装器备份                         | `E:\software\Renesas\_downloads\`        | `setup_fsp_v6_6_0_rasc_v2026-07.exe`（1.35 GB）+ `FSP_Packs_v6.6.0.zip` |


> ⚠ **编译用的是工程内 `ra/`**，不是 `E:\software\Renesas\FSP_6.6.0\`。  
> RASC 生成时会把该工程需要的 FSP 模块源码整份拷进工程目录（`ra/fsp/src`、`ra/fsp/inc`、  
> `ra/arm/CMSIS_6`）。`E:\software\Renesas\FSP_6.6.0\` 只用于查源码、比对 API，改它不影响编译。

**RASC 配置器（已装）**：`E:\software\Renesas\RAC\` —— Eclipse RCP 版，自带 JRE，  
含命令行 `E:\software\Renesas\RAC\eclipse\rascc.exe`，可完全无头生成，不依赖 IDE。

> 📘 RASC 的完整用法（GUI 向导、各 IDE 集成、**全部命令行选项**）见同目录  
> **`RASC开发文档.md`** —— 那是官方 `rasc_quick_start.html` 的中文翻译版。

### 5.2 生成步骤（底稿必须用 BH1750 的 XML）

```bash
# 生成命令：在放 configuration.xml 的目录里执行
"/e/software/Renesas/RAC/eclipse/rascc.exe" --generate \
    --compiler GCC --device R7FA6E2BB configuration.xml
```

> 补充：安装器 `setup_fsp_v6_6_0_rasc_v2026-07.exe` 的 7z 负载里**没有任何 FSP 源码**  
> （0 个 `ra/fsp/src` 条目），只有 Eclipse RCP 平台（`features/` `plugins/` 自带 JRE）  
> 加一个 `installer.exe`。FSP 内容由安装器运行时通过 p2 注入，**无法靠解包代替安装** ——  
> 这一步必须跑安装器，但只需跑一次。

关于 FSP 源码本身（`E:\software\Renesas\FSP_6.6.0\`），有三个点值得记住：

1. **FSP 源码仓库不带 CMSIS** —— 仓库里没有 `ra/arm/`，CMSIS 走 CMSIS pack 分发。  
   已从 `FSP_Packs_v6.6.0.zip` 里的 `Arm.CMSIS6.6.1.0+fsp.6.6.0.pack` 抽出  
   `ra/arm/CMSIS_6/CMSIS/Core/Include/`（42 个头文件）补进去。  
   （RASC 生成的工程内 `ra/arm/` 是完整的，无需再补。）
2. **仓库里没有任何 `.ld` 链接脚本** —— 印证了 6.x 的变更：链接脚本改为  
   RASC 按「工具链 + MCU + 工程类型」生成，不再有通用模板。  
   ⇒ 想用 6.6.0 编译，**必须先有 RASC 生成的 `ra_gen/` + `ra_cfg/` + 链接脚本**。
3. `FSP_Packs_v6.6.0.zip` **不是 FSP 源码**，里面全是 `.pack` 器件包  
   （给 e²studio / Keil / IAR 的 RTE 用）。别拿它当源码目录。

**破坏性变更（6.0.0 起，逐条对照我们的构建）**

| 变更                                                                                                          | 对我们的影响                                  |
| ----------------------------------------------------------------------------------------------------------- | --------------------------------------- |
| **链接脚本重写**：`fsp.ld` 改为 include `fsp_gen.ld`；`.text` 并入 `__flash_readonly$$`，`.data` 并入 `__ram_from_flash$$` | ⚠ `cmake/ld/` 下 5.8.0 那两份脚本**整套作废**     |
| **OFS（选项设置区）改逐项 Enable/Disable**，升级不保留旧值                                                                    | ✅ 其实更安全：可关闭某些 OFS 不写，比 5.8.0 更不容易误锁 SWD |
| **移除 `BSP_PACKED` 宏**                                                                                       | ⚠ 5.8.0 示例代码直接拷过来会编译失败                  |
| **回调 `p_context` 去掉 const**（`void const * const` → `void * const`）                                          | ⚠ TrustZone guard 函数会告警，需手改             |
| **`board_sdram.h` → `bsp_sdram.h`**                                                                         | ➖ 本工程未用 SDRAM                           |
| **MCUboot 配置改走 Solution project**                                                                           | ➖ 本工程裸机 flat、无 bootloader，不受影响          |
| **6.6.0 起不再捆绑 GCC**                                                                                         | ✅ 无影响：编译仍走本机 Arm GNU 14.3.1             |
| IAR 最低 9.70.1                                                                                               | ➖ 不用 IAR                                |

> 官方已知坑：5.x → 6.x 后，若 `objcopy` 只挑 `.text`/`.data` 段，hex 会只剩几行  
> （段被合并）。**我们的 `PostBuild.cmake` 用全段 `objcopy -O ihex`，不受影响。**

**生成步骤（要加/改外设时照做一遍）**

1. 取一份 `configuration.xml` 作底稿 —— 用 **BH1750** 那份  
   （`3、程序\DQX_RA6E2\BH1750\configuration.xml`），它带 `r_iic_b_master` + `r_sci_uart` + `r_ioport`。  
   ⚠ 别用 `led` 那份：它只有 IOPORT，生成的工程会缺 I2C / UART 句柄，  
   编我们的 `src/` 会报 `I2C_BH1750_ctrl undeclared`。
2. 改版本号与组件分组，交给 RASC 生成：
   ```bash
   mkdir -p projects/ra6e2_66_app
   cp "<资料>/3、程序/DQX_RA6E2/BH1750/configuration.xml" projects/ra6e2_66_app/
   cd projects/ra6e2_66_app
   sed -i 's/5\.8\.0/6\.6\.0/g; s/group="CMSIS5"/group="Main"/g' configuration.xml
   "/e/software/Renesas/RAC/eclipse/rascc.exe" --generate \
       --compiler GCC --device R7FA6E2BB configuration.xml
   ```
3. 生成后得到 `ra_gen/` + `ra_cfg/` + `script/fsp.ld` + 工程根 `fsp_gen.ld`，  
   把它作为 FSP 根构建（`src/` 不动）：
   ```bash
   cmake -B build -DFSP_ROOT="E:/REA6E2_Dev/projects/ra6e2_66_app"
   cmake --build build
   ```
   `CMakeLists.txt` 会自动识别 6.x 布局（判据：工程根存在 `fsp_gen.ld`），  
   链接脚本与 include 路径都已按 6.x 配好，**不需要手工改 `-T/-L`**。
4. 从 5.8.0 示例拷代码时，按上表替换 `BSP_PACKED`、`p_context` 等（破坏性变更）。

### 5.3 生成后编译：三个已解决的坑

都是实测踩过的，已修进 `CMakeLists.txt`，**遇到同类报错直接对照**：

| # | 现象                                                | 根因                                                                                 | 修法                                                                                                                                                                               |
| - | ------------------------------------------------- | ---------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1 | 29 个目标全报 `bsp_mcu_ofs_cfg.h: No such file`        | 6.x 的 `bsp_cfg.h` 会 include 它，但只有 XML 启用过 OFS 组件时 RASC 才生成；我们的 XML 源自 5.8.0        | 补 `cmake/patch66/bsp_mcu_ofs_cfg.h`，**内容留空**（`bsp_linker.c` 用 `#if defined BSP_CFG_OPTION_SETTING_OFS0` 判断，不定义宏就不生成 OFS 数据）。放在 `cmake/` 下走 include 路径提供，不写进工程目录，`rascc` 重新生成也不会丢 |
| 2 | `bsp_linker_info.h: No such file`                 | RASC 把它生成在**工程根**，不在 `ra_cfg/` 下                                                   | include 列表末尾加 `${FSP_ROOT}`                                                                                                                                                      |
| 3 | 链接报 `cannot find <工程根>/script: Permission denied` | CMake 的 `target_link_options` **会去重选项**，两个 `-L <dir>` 的第二个 `-L` 被丢掉，裸路径变成 ld 的输入文件 | `-L` 改写成单参数 `-Wl,-L<dir>`                                                                                                                                                        |

### 5.4 什么时候必须跑 RASC，什么时候不用

判断标准不是「改没改引脚」，而是**要不要新增 FSP 模块实例**。

| 你要做的事                                                 | 要跑 RASC 吗 | 说明                                                                                                                |
| ----------------------------------------------------- | --------- | ----------------------------------------------------------------------------------------------------------------- |
| 用**已配好**的引脚（P402 点灯、P408/P206 的 I2C、P100/P101 的 UART） | **不用**    | 引脚在 `ra_gen/pin_data.c` 里已配好，业务代码直接调 `R_IOPORT_PinWrite` / `R_IIC_B_MASTER_Write` 就行                              |
| 加一个**纯 GPIO**（输入/输出/上拉）到没配过的脚                         | **可跑可不跑** | 不跑也行：`hal_entry()` 里调 `R_IOPORT_PinCfg()` 运行时配（官方支持的 API，见下）。但推荐跑 RASC，图形配置与代码才一致                                 |
| 加一个**新外设**（SPI / ADC / GPT / CAN / USB / 新的 UART…）    | **必须跑**   | 卡的不是引脚，是没有模块实例：要 RASC 生成 `g_xxx_ctrl` / `g_xxx_cfg`（`hal_data.c` / `common_data.c`）和 `ra_cfg/fsp_cfg/r_xxx_cfg.h` |
| 把已有外设**换到别的引脚**                                       | **必须跑**   | RASC 会校验这个脚支不支持该功能（不是任意脚都能做 SPI），并改写 `pin_data.c` 的复用值                                                            |
| 改时钟、改中断优先级、加/删 RTOS、改堆栈                               | **必须跑**   | 都在 `ra_gen/` 里                                                                                                    |

**底层机制（为什么外设非跑 RASC 不可）** —— 这是 FSP 与 STM32 HAL 最大的差别：

> STM32 的 `HAL_SPI_Init()` 会顺手把 GPIO 配成复用功能；  
> **RA 的 `R_SPI_Open()` 完全不管引脚** —— 实测 `ra/fsp/src/r_spi/r_spi.c` 里  
> 0 处引脚配置代码。

引脚是另一条独立的线：BSP 在 `hal_entry()` 之前调用 `R_IOPORT_Open()`，  
一次性把 `ra_gen/pin_data.c` 里的 `g_bsp_pin_cfg` 表写进 PFS 寄存器。  
所以「引脚」和「外设实例」**两件都得配好，外设才能工作** —— 只补一边必然失败。

**运行时改脚的官方 API**（不想跑 RASC 时用，内部已处理 PWPR 写保护）：

```c
/* 单个脚 */
R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_05_PIN_01,
                IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_HIGH);

/* 多个脚: 自己填 ioport_cfg_t 再 R_IOPORT_PinsCfg(&g_ioport_ctrl, &cfg) */
```

配置宏在 `ra/fsp/inc/instances/r_ioport.h`（`IOPORT_CFG_PORT_DIRECTION_OUTPUT`、  
`IOPORT_CFG_PORT_DIRECTION_INPUT`、`IOPORT_CFG_PULLUP_ENABLE`、  
`IOPORT_CFG_PERIPHERAL_PIN` …）。注意 `pin_data.c` 头部写着  
`generated pin source file - do not edit`，**别手改它** —— 下次 RASC 生成就冲掉了。

> ⚠ 唯一不该走的路：直接写 PFS 寄存器（`reg_led` 示例那套）。要自己处理  
> `PWPR` 写保护（`R_BSP_PinAccessEnable()`），还会和 FSP 已配的脚打架。

### 5.4.1 增删源文件后：什么时候要重新 configure

**结论先说**：`CMakeLists.txt` 第 37–39 行的三个 `file(GLOB)` **已全部加上  
`CONFIGURE_DEPENDS`**，增删 `.c` 文件会**自动**触发重新配置，正常情况你不用手动做什么。  
下面先讲为什么会有这个坑，再讲什么时候仍必须手动。

#### 为什么会卡（原理）

CMake 的 `file(GLOB)` 只在**配置那一刻**求值一次，结果固化进 `build/build.ninja`。  
之后 ninja 只盯着**已知文件的改动**，不管目录里有没有新文件出现。实测：

```text
$ echo 'volatile int x;' > src/glob_probe.c     # 新增一个 .c
$ cmake --build build
ninja: no work to do.                            ← 新文件被完全忽略, 连编译都没发生
```

这就是加 `CONFIGURE_DEPENDS` 之前的真实行为。

#### 现在的行为（已加 CONFIGURE_DEPENDS，2026-10-05 实测）

| 操作             | `cmake --build build` 的反应           |
| -------------- | ----------------------------------- |
| 新建 `src/xxx.c` | 先 `Re-running CMake...`，然后自动编译它     |
| 删掉某个 `.c`      | 自动 `Re-running CMake...`，不会报「源文件丢失」 |
| 只改已有文件内容       | 正常增量编译（这条本来就行）                      |

#### 什么时候仍必须手动 configure

| 情况                                    | 要不要手动                           |
| ------------------------------------- | ------------------------------- |
| 只增删了 `.c` 文件                          | **不用**（`CONFIGURE_DEPENDS` 已覆盖） |
| 改了 `CMakeLists.txt` / `cmake/*.cmake` | **不用**（CMake 自己会 regenerate）    |
| FSP 根换了（6.6.0 ↔ 5.8.0，或 RASC 生成到新目录）  | **要，且必须带 `-DFSP_ROOT=`**        |
| 第一次配置某个 build 目录                      | **要，两个参数都得给**                   |

```bash
# 常规重新配置 —— 不带参数即可, 缓存里已有工具链和 FSP_ROOT
cmake -B build

# 首次配置 / 换 FSP 根 —— 两个参数都要给
cmake -B build   -DCMAKE_TOOLCHAIN_FILE="E:/REA6E2_Dev/cmake/arm-none-eabi-ra6e2.cmake"   -DFSP_ROOT="E:/REA6E2_Dev/projects/ra6e2_66_app"

# 5.8.0 回退(另一个 build 目录)
cmake -B build58   -DCMAKE_TOOLCHAIN_FILE="E:/REA6E2_Dev/cmake/arm-none-eabi-ra6e2.cmake"   -DFSP_ROOT="E:/REA6E2_Dev/vendor/DQX_RA6E2/BH1750"
```

> `cmake -B build` 不带参数能行，是因为 `-DCMAKE_TOOLCHAIN_FILE` 和 `-DFSP_ROOT`  
> 都已写进 `build/CMakeCache.txt`，重新 configure 会沿用。

#### 怎么确认当前配置

```bash
grep "^FSP_ROOT:PATH" build/CMakeCache.txt        # 当前 FSP 根
grep -c "ra6e2_fw[.]elf[.]dir" build/build.ninja  # 纳入编译的源文件数
```

#### 忘了 configure 的症状

- `undefined reference to xxx` —— 新模块的 `.c` 压根没进编译，链接时自然找不到符号
- RASC 改完引脚、重新生成后，`cmake --build build` 说 `no work to do`


> ⚠ `CONFIGURE_DEPENDS` 只对**文件增删**敏感。RASC 新增的通常是头文件  
> （如 `ra_cfg/fsp_cfg/r_xxx_cfg.h`），头文件变化 ninja 本来就会重编依赖它的目标，  
> 所以走 RASC 加外设这条路，手动 configure 的概率其实不高；  
> **真正高频踩坑的是自己在 `src/` 下新建模块文件**，现在这个也已经自动了。

### 5.5 5.8.0 回退（备用，不是推荐路径）

`vendor/DQX_RA6E2/BH1750` 那份 5.8.0 工程仍保留，已做回归编译验证。需要时切 FSP 根即可：

```bash
cmake -B build58 -DFSP_ROOT="E:/REA6E2_Dev/vendor/DQX_RA6E2/BH1750"
cmake --build build58
```

> ⚠ 5.8.0 产物带 5 个 option-setting 段，**烧录必须带 `--skip-section`**（见 §1.0）。  
> 日常烧录请用 6.6.0 的 `build/`。  
> 另注：两个 FSP 根的 `ra_gen/` 是**整体生成**的，不能把 A 的 `hal_data.c` 和  
> B 的 `pin_data.c` 混着用。要么整包切，要么把需要的内容手工合进同一份 `ra_gen`。

---

## 6. 应用代码怎么写

入口在 `src/hal_entry.c`：

```c
void hal_entry(void)
{
    Run();          // 你的主循环，定义在 src/Apply/app.c
}
```

FSP 的标准用法（`led` 示例原文）：

```c
#include "hal_data.h"

void Run(void)
{
    while (1)
    {
        R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_04_PIN_02, BSP_IO_LEVEL_LOW);
        R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_SECONDS);

        R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_04_PIN_02, BSP_IO_LEVEL_HIGH);
        R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_SECONDS);
    }
}
```

下面接着回答「这些代码具体落到哪个文件」（官方工程的目录约定见 §3.1）。

**`hal_entry()` 不是"笼子"，它只是初始化跑完后交给你的第一个点。**  
你的绝大部分代码应该写在 `src/` 下自己的 `.c` 里，`hal_entry()` 只负责「调用它们」。

### 6.1 上电后的真实调用链

```
Reset_Handler()              ← startup.c，芯片上电入口
   └─ SystemInit()           ← system.c：时钟、引脚、C 运行时
        ├─ R_BSP_WarmStart(BSP_WARM_START_RESET)         ← 钩子①复位后
        ├─ R_BSP_WarmStart(BSP_WARM_START_POST_CLOCK)    ← 钩子②时钟起来后
        └─ R_BSP_WarmStart(BSP_WARM_START_POST_C)        ← 钩子③C 运行时后（示例在这里配引脚）
   └─ main()                 ← ra_gen/main.c，工具生成，就三行
        └─ hal_entry()       ← ★ 你的入口
   └─ while (1) {}           ← hal_entry 返回后会落到这里空转
```

`ra_gen/main.c` 全文只有：

```c
int main(void)
{
    hal_entry();
    return 0;
}
```

### 6.2 四个可以放代码的位置

| 位置                       | 什么时候跑               | 适合放什么                                          |
| ------------------------ | ------------------- | ---------------------------------------------- |
| `R_BSP_WarmStart(event)` | 比 `main()` 还早，三个时机  | 极早期的硬件准备（示例用它开 data flash 读取、配 IO）             |
| **`hal_entry()`**        | BSP 初始化完成后          | **模块初始化 + 主循环**，最常用                            |
| **自己的 `.c` / `.h`**      | 被 `hal_entry()` 调用时 | **绝大部分业务代码**，随便怎么分文件                           |
| **外设/中断回调**              | 中断上下文，随时触发          | 独立函数，通过 `ra_gen/hal_data.c` 的 `p_callback` 挂上去 |

### 6.3 真实范例：官方 UART 工程就是这么拆的

看 `DQX_RA6E2/UART/` 这个官方示例，代码根本没堆在 `hal_entry` 里：

| 文件                    | 内容                                         |
| --------------------- | ------------------------------------------ |
| `src/hal_entry.c`     | 只有一句 `Run();`                              |
| `src/uart/bsp_uart.c` | UART 的全部实现，**包括中断回调 `uart0_callback()`**   |
| `ra_gen/hal_data.c`   | `.p_callback = uart0_callback` —— 把回调挂到驱动上 |

回调本体（`src/uart/bsp_uart.c`），完全独立的一个函数：

```c
void uart0_callback(uart_callback_args_t *p_args)
{
    switch (p_args->event)
    {
        case UART_EVENT_RX_CHAR:        // 收到数据
            R_SCI_UART_Write(&g_uart0_ctrl, (uint8_t*) &(p_args->data), bytes);
            break;
        case UART_EVENT_RX_COMPLETE:    // 接收完成
            Uart0_Receive_Flag = true;
            break;
    }
}
```

### 6.4 推荐的代码布局

```c
/* src/hal_entry.c —— 只做初始化 + 调度，不放业务逻辑 */
#include "hal_data.h"
#include "Apply/app.h"
#include "uart/bsp_uart.h"      // 自己的模块，随便加
#include "sensor/hx711.h"

void hal_entry(void)
{
    HX711_Init();          // 各模块初始化
    Uart0_Init();

    while (1)              // 主循环只做轮询/调度
    {
        poll_sensors();
        handle_events();
    }
}
```

### 6.5 业务逻辑放哪里（具体落位）

结论：**`hal_entry.c` 留一句 `Run();`，主循环放 `Apply/app.c`，每个外设/业务模块独占一个  
`src/<模块>/bsp_xxx.c`。** 与官方 `adc` 工程同构。

本工程推荐的目标布局：

```
E:\REA6E2_Dev\src\
├─ hal_entry.c                ← 只有 Run();  （当前已如此）
├─ Apply\
│  ├─ app.c                   ← 主循环 Run()，只做「调谁」，不做具体业务
│  └─ app.h
├─ uart\                      ← 串口：一个模块一个目录
│  ├─ bsp_uart.c              ← 驱动实现 + 中断回调 uart0_callback()
│  └─ bsp_uart.h              ← 对外只暴露 Uart0_Init() / Uart0_Send() 等
├─ sensor\                    ← 例：传感器
│  ├─ bsp_hx711.c
│  └─ bsp_hx711.h
└─ display\                   ← 例：屏幕
    ├─ bsp_oled.c
    └─ bsp_oled.h
```

三层分工：

| 层级 | 文件                   | 该做                          | 不该做       |
| -- | -------------------- | --------------------------- | --------- |
| 入口 | `hal_entry.c`        | 只调 `Run()`                  | 不放业务逻辑    |
| 调度 | `Apply/app.c`        | 各模块 `xxx_Init()` + 主循环轮询/调度 | 不直接操作寄存器  |
| 模块 | `src/<模块>/bsp_xxx.c` | 该外设的全部实现 + 其中断回调            | 不互相引用私有状态 |

`hal_entry()` 与 `app.c` 的写法（照官方 `adc` 工程的思路）：

```c
/* src/hal_entry.c —— 保持极简 */
void hal_entry(void)
{
    Run();
}

/* src/Apply/app.c —— 主循环只做调度 */
void Run(void)
{
    Uart0_Init();          // 来自 src/uart/bsp_uart.h
    HX711_Init();          // 来自 src/sensor/bsp_hx711.h
    OLED_Init();           // 来自 src/display/bsp_oled.h

    while (1)
    {
        if (Uart0_Receive_Flag)      // 中断只置标志，处理在主循环
        {
            handle_uart_cmd();
            Uart0_Receive_Flag = false;
        }
        poll_sensors();
        refresh_display();
    }
}
```

**新建模块的流程**：

1. 在 `src/` 下建子目录（如 `src/hx711/`）
2. 建 `bsp_hx711.c` + `bsp_hx711.h`，头文件里 `extern` 暴露接口
3. 在 `src/Apply/app.c` 里 `#include "hx711/bsp_hx711.h"` 并调用
4. **重新跑一次 cmake 配置** —— `CMakeLists.txt` 用 `GLOB_RECURSE` 收 `src/**/*.c`，  
   新目录会自动收进去，但不重跑配置不会被发现

```bash
./scripts/build.sh            # 或 cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-ra6e2.cmake
```

> 模块化移植库（51 个）里的驱动就是现成模板：把它 `src/BSP_Code/bsp_xxx.c/.h`  
> 拷进你的 `src/xxx/`，在 `app.c` 里调用即可 —— 前提是它用到的外设实例  
> 在你工程的 `ra_gen` 里也存在（否则按 §5.2 用 RASC 重新生成一份带该外设的配置）。

### 6.6 要点

- `hal_entry()` 是**线程/任务入口**，不是 `main()`。`main()` 在 `ra_gen/main.c`，工具生成，改了也会被覆盖。
- **`hal_entry()` 理论上可以返回**，不会崩 —— 会回落进 `Reset_Handler()` 里的 `while(1){}` 空转。  
  但它等价于停机（程序不再做任何事），所以**裸机工程里都应该以主循环收尾**。
- 开了 RTOS 时，`hal_entry()` 里创建任务后可以返回，由调度器接管。
- TrustZone 工程在 `hal_entry()` 末尾有 `R_BSP_NonSecureEnter()`；本工程是 flat（非分区）工程，未启用。
- 新增 `.c` 文件放进 `src/` 即可，`CMakeLists.txt` 用 `GLOB_RECURSE` 自动收（**改完需重新 cmake 配置**）。
- **回调不要写成阻塞式**：它在中断上下文执行，别在里面 `while` 死等，  
  置标志位交给主循环处理（官方示例就是这么做的）。

---

## 7. 常见坑清单

| 现象                                                                        | 原因 / 处理                                                                                                                                            |
| ------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| 新写的 `.c` 文件没被编译 / `undefined reference`                                   | 见 §5.4.1。已启用 `CONFIGURE_DEPENDS`，增删文件会自动重新配置；若仍不生效，手动 `cmake -B build`                                                                             |
| `'cmd.exe' 不是内部或外部命令`                                                     | Git Bash 的 PATH 缺 `C:\Windows\System32`，CMake 的嵌套 `cmd.exe` 找不到。用 `scripts/build.sh`（已内置），或执行 `export PATH="/c/Windows/System32:/c/Windows:$PATH"` |
| `cannot open map file ...: Invalid argument`                              | `-Wl,-Map="路径"` 里的引号会被 ld 当字面量。写成 `-Wl,-Map=路径`                                                                                                    |
| `.bin` 有 16.8 MB                                                          | option-setting 区在 `0x0100A100`，binary 会铺满 `0x0`–`0x0100A2xx`。烧录用 elf / srec / hex                                                                  |
| `No connected probes were found`                                          | DAPLink 掉了。看 `probe-rs list`，检查 USB 线与端口                                                                                                           |
| 烧录慢（几十秒）                                                                  | 探针是 CMSIS-DAP v1（interface 0253），约 21–35 KB/s，属正常                                                                                                  |
| 选错芯片变体                                                                    | 必须 `R7FA6E2BB`（256 KB）。`R7FA6E2B9` 是 128 KB，内存布局不同                                                                                                 |
| `No flash memory contains the entire requested memory range 0x0100A100..` | probe-rs 的 RA6E2 算法不覆盖 option-setting 区。烧录要 `--skip-section` 跳过 5 个段，见 §1.1                                                                        |
| 烧录成功但程序不跑                                                                 | `download` / `verify` 后内核是**停机**状态。补一句 `probe-rs reset`，见 §1.2                                                                                     |
| 读全局变量得到一串固定不变的机器码                                                         | 同上 —— 读到的是 probe-rs 留在 RAM 里的 flash 算法代码。先 `reset` 再读                                                                                              |
| `monitor halt` 在 GDB 里无效                                                  | probe-rs 的 gdb server 不认这个子命令（帮助里是 `reset halt`）。查运行状态直接用 `probe-rs read` 读 RAM 里的全局变量更快，见 §2.4                                                    |
| 链接报 `undefined reference to BH1750_callback` / `uart0_callback`           | `ra_gen/hal_data.c` 写死了这两个 `.p_callback`。只要代码引用了 I2C 句柄，`hal_data.o` 就会参与链接，符号必须存在。定义在 `src/i2c/bsp_i2c.c` 与 `src/hal_callbacks.c`（详见 `1.3寸OLED开发文档.md` §6）                     |

---

## 8. 官方示例工程索引

### 8.1 外设示例（14 个）

**已解压**在 `E:\BaiduNetdiskDownload\地奇星RA6E2开发板资料\3、程序\DQX_RA6E2\`，  
直接用即可，不必再处理 `.7z`（本机也没有 7-Zip，之前靠 Python `py7zr` 提取）。

| 工程        | 内容            | 工程                  | 内容            |
| --------- | ------------- | ------------------- | ------------- |
| `led`     | GPIO 点灯（当前基线） | `adc`               | ADC 采样        |
| `reg_led` | 寄存器方式点灯       | `DAC`               | DAC 输出        |
| `UART`    | 串口收发          | `TSN`               | 内部温度传感器       |
| `CGC`     | 时钟配置          | `RTC` / `RTC_Alarm` | RTC 与闹钟       |
| `GPT_PWM` | GPT 输出 PWM    | `GPT_Time`          | GPT 定时        |
| `AGT_PWM` | AGT 输出 PWM    | `QSPI_FLASH`        | 外扩 QSPI Flash |
| `BH1750`  | I2C 光照传感器     |                     |               |


### 8.2 模块移植代码库（51 个）

`E:\BaiduNetdiskDownload\地奇星RA6E2开发板资料\立创·地奇星RA6E2开发板【模块移植代码】\`

**每一个都是一套完整的 e²studio 工程**（自带 `configuration.xml` + `ra_gen/`），  
涵盖传感器、显示、舵机、WS2812 等 —— BH1750、MPU6050、DS18B20、HX711、SHT30、  
SG90 舵机、多种 OLED/LCD 屏、MQ 系列气体传感器都在里面。  
另有一个 **`e2-studio工程模板`** 目录，是从零起新项目的骨架。

用法两条：

1. **重新生成配置**（外设配置差异大时）—— 按 §5.2，用它的 `configuration.xml` 作底稿  
   跑一遍 `rascc`，得到带该外设的 `ra_gen/`
2. **只拷驱动文件**（外设配置一致时）—— 把它的 `src/` 下驱动 `.c/.h` 拷进本项目  
   `src/`，在 `hal_entry()` 里调用；若用到本工程 `ra_gen` 里没有的外设实例，得走第 1 条

> ⚠ 这些模块工程大多是 **FSP 5.8.0 / e²studio 工程**，直接拿它的 `ra_gen` 当 FSP 根会与  
> 6.6.0 的 `src/` 冲突。要整包用就按 §5.4 走 5.8.0 回退；要留在 6.6.0 就用它的  
> `configuration.xml` 重新生成（§5.2）。

---

## 9. 待确认项

- **串口日志通道**：DAPLink 的 CDC 串口是 COM6，但它**是否接到了 RA6E2 的 UART 引脚**  
  尚未确认。当前 FSP 根的 `ra_gen/pin_data.c` 把 **P100/P101 配成了 SCI0**,  
  TXD0/RXD0 就在那里。要抓日志前先确认接线。
- ~~**RTT / SWO** 未实测~~ → **2026-10-05 已实测**：RTT 可用且稳定（§2.3.1），  
  一条 `probe-rs attach ... --speed 1000` 就能抓日志；  
  ITM 两条路都不通（无 TPIU；ETF 读不出数据），见 §2.3.2。
- ~~**烧录链路**未实测~~ → **2026-10-05 已跑通**：烧录 5.4 秒 + 回读校验通过，  
  程序确认在运行（`g_oled_frames` 持续增长）。
- **OLED 的目视确认**：程序跑起来了、I2C 全部无错，但**显示内容本身需要人眼确认**。  
  上电 2 秒内是信息屏（满屏边框 + 三行参数）。请看**四条边框是否都紧贴屏幕边缘** ——  
  若某一侧缺失或边缘有 2 像素杂条，说明列偏移不对，需要改驱动（见 `1.3寸OLED开发文档.md` §3、§9.3）。

---

## 10. 已接入外设：1.3 寸 I2C OLED

**这一章已独立成专题文档：`docs/1.3寸OLED开发文档.md`**（2026-10-22 迁出，原 §10.1 ~ §10.6 全部并入）。

此处只留索引，避免两处内容各自漂移：

| 想知道 | 去 `1.3寸OLED开发文档.md` 看 |
| --- | --- |
| 接哪两个脚、从机地址、总线速率、为什么是这两个脚 | §2 硬件与接线 |
| 芯片规格：SH1106 的命令集、显存布局、三条硬约束 | §3 控制芯片 |
| 帧缓冲 / 页寻址 / 列偏移 / 字库 / NACK 重试 | §4 驱动实现 |
| 对外 API 与运行时状态变量 | §5 对外接口 |
| 为什么必须有 `BH1750_callback` / `uart0_callback` | §6 回调符号 |
| 上电显示什么、LED 心跳怎么闪 | §7 应用层行为 |
| 帧率 9 → 164 fps 具体怎么做的 | §8 帧率优化 |
| 怎么验证、日志字段怎么读、出问题怎么回退 | §9 / §10 / §11 |
| 已踩过的坑 | §12 踩坑清单 |
