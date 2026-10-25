> 其它语言：[English](README_EN.md)

# RA6E2 系统监控屏

基于 Renesas RA6E2 开发板的「PC 系统监控外设」：一块 128×64 单色点阵屏实时显示本机
CPU / GPU 的温度、占用率、风扇转速与内存占用率，数据由 Windows 上位机经 USB CDC 串口下传。

把一台电脑的实时运行状态，做成一个摆在桌上的小硬件仪表盘。

---

## 系统架构

```
┌────────────────────────┐                        ┌────────────────────────┐
│       Windows PC       │                        │      RA6E2 开发板      │
│     ra6e2-monitor      │USB CDC（虚拟串口）     │   裸机固件 (FSP 6.6.0) │
│     (Rust + egui)      │─────────────────────-->│   NT7534 128×64 屏渲染 │
│  采集 CPU/GPU/内存指标 │                        │                        │
└────────────────────────┘                        └────────────────────────┘
       PDH / DXGI / LHM                           (Cortex-M33 @ 200MHz)
```

上位机负责「采集 + 下发」，开发板负责「接收 + 渲染」，两者通过一条文本协议解耦。

---

## 硬件

| 项目 | 说明 |
| --- | --- |
| 开发板 | 地奇星 RA6E2（R7FA6E2BB3CNE，QFN48） |
| MCU | Renesas RA6E2，ARM Cortex-M33，200 MHz，256 KB Flash |
| 显示屏 | 128×64 单色点阵屏，NT7534 控制器（软件 SPI 驱动） |
| 烧录/调试 | FireDAP（CMSIS-DAP，VID:PID = `0416:5021`），SWD 接口 |

引脚分配：

| 外设 | 引脚 |
| --- | --- |
| USB CDC | D+ = P814，D- = P815，VBUS = P407 |
| NT7534 屏 | RS = P403，RES = P409，CLK = P302，SID = P207，CS = P301 |
| 状态 LED | P402 |
| SWD | SWCLK = P108，SWDIO = P300 |

---

## 功能特性

### 固件（RA6E2 端）

- 实时显示 **CPU 温度 / 占用率 / 风扇**、**GPU 温度 / 占用率 / 风扇**、**内存占用率**（表格化布局）。
- 右下角双风扇动画（3 叶）：仅在已连接（linked）时转动，转速随实际 RPM 变化；断链时风扇静止但仍照常显示。
- 动画帧率与上位机送数频率**解耦**（独立时基）：数字约 1 Hz 刷新，屏幕约 60 fps 局部脏区刷新。
- USB CDC 通信，支持 `$ID?` 身份查询（返回芯片出厂 UID + 固件版本），便于上位机**按芯片 UID 认板**，
  而非依赖每次插拔都会变的 COM 号。
- 断链保留最后读数。
- 当前固件版本：**v24**（`0x4E540018`）。

### 上位机（ra6e2-monitor，Windows）

- Rust + egui 桌面程序，采集本机 CPU / GPU / 内存指标（PDH、DXGI、LibreHardwareMonitor）。
- 通过 USB CDC 下发到开发板；按芯片 UID 自动识别目标板。
- 暗色主题；记忆窗口缩放、位置、大小（`ui.json`，支持便携模式）。
- 系统托盘：点窗口 **X = 退出**；仅「收进托盘」按钮才缩到托盘。
- GPU 显存占用率卡片（默认不勾选）。
- 风扇单位自适应：有 RPM 发 RPM，只有百分比时发 `%`（UI 标注 `%`）。
- 未连接时也有读数（亮绿色）。

---

## 通信协议

上位机 → 固件，每行一条，以 `CRLF` 结尾，载荷带 8 位 XOR 校验：

```
$CT:..,CU:..,CF:..,GT:..,GU:..,GF:..,MU:..,GV:..*XX\r\n
```

| 字段 | 含义 | 单位 |
| --- | --- | --- |
| `CT` | CPU 温度 | °C |
| `CU` | CPU 占用率 | % |
| `CF` | CPU 风扇转速 | RPM |
| `GT` | GPU 温度 | °C |
| `GU` | GPU 占用率 | % |
| `GF` | GPU 风扇转速 | RPM |
| `MU` | 内存占用率 | % |
| `GV` | 显存占用率 | %（固件 v24 暂未消费，已预留） |

- `XX` 为 `$` 与 `*` 之间载荷的 8 位 XOR 校验（十六进制）。
- 缺项（如取不到某风扇 RPM）发 `-1`，固件按缺项处理（屏上显示 `--`）。
- 身份查询：上位机发 `$ID?*XX\r\n`，固件回 `$ID:RA6E2-<UID>,FW:<ver>*XX\r\n`
  （`FW` 为本固件次版本号，如 `24`）。

---

## 构建与烧录

### 固件

依赖：Renesas FSP 6.6.0、Arm GNU Toolchain（GCC）、CMake + Ninja、RASC（代码生成）、probe-rs。

```bash
# 1) 用 RASC 打开 configuration.xml，以 GCC + R7FA6E2BB 生成代码（如尚未生成）
# 2) 构建
./scripts/build.sh                 # 产物: build/ra6e2_fw.elf
# 3) 烧录（FireDAP，默认不自动复位）
./scripts/flash.sh --speed 1000 --verify
# 4) 让固件运行：物理拔插一次 Type-C（冷启动，避免 USB 枚举丢失）
```

> 烧录脚本默认**不复位内核**（否则会使已枚举的 USB CDC 串口消失）。烧录完成后请拔插 Type-C 启动固件。

### 上位机

依赖：Rust 工具链（edition 2024）、Windows（指标采集使用 Windows API）。

```bash
cd gui
cargo build --release             # 产物: gui/target/release/ra6e2-monitor.exe
```

> 部分性能计数器需要以**管理员权限**运行。

---

## 使用

1. 烧录并上电 RA6E2，用 Type-C 连接电脑（出现 CDC 串口）。
2. 运行 `ra6e2-monitor.exe`，程序自动枚举串口、按 UID 认板并连接。
3. 本机指标实时显示到开发板屏幕上。

---

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `src/` | 固件源码（NT7534 屏驱动、USB CDC、数据链路解析） |
| `gui/` | 上位机（Rust + egui） |
| `scripts/` | 构建 / 烧录脚本 |
| `docs/` | 开发环境、屏、USB CDC 等相关文档（含 `3.5寸屏开发文档.md`） |
| `vendor/`, `third_party/` | FSP 与第三方组件 |
| `host/`, `tools/` | 辅助脚本与工具 |

---

## 许可证

本项目采用 **MIT License**，详见仓库根目录的 `LICENSE` 文件。
