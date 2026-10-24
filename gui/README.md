# ra6e2-monitor —— RA6E2 系统监控台（上位机 GUI）

Rust + egui 写的桌面程序：采集本机 CPU/GPU/内存指标，按 `$...*XX` 帧经 **USB CDC**
发给 RA6E2 开发板，显示在 3.5" NT7534 屏上。

单个 exe，内嵌 LibreHardwareMonitor（首次运行释放到 `%LOCALAPPDATA%\Ra6e2Monitor\lhm`）。

## 当前状态（2026-10-24）

| 项 | 状态 |
|---|---|
| 编译 | ✅ release 零 error 零 warning，23.0 MB 单文件 |
| `--selftest` 采集/组帧 | ✅ 全对（`CT/CU/CF/GT/GU/GF/MU` + XOR 校验） |
| 固件 v16（**30 s 卡点已修**）| ✅ 已烧入并验证在跑（GUI 读到 `fw=16`）|
| 扫描板子 + 开始连接 | ✅ 手动实测通过；**扫描实测 214 ms** |
| 端到端上屏（`linked` + 数值）| ✅ 用户手动确认通过（2026-10-24）|
| 板子信息缓存 / 句柄常驻 | ✅ 见「扫描为什么快」一节 |
| 启动整窗闪烁 | ✅ 已修（见下节「启动闪烁的两个成因」）|
| 顶部「设置」→ 字体大小 | ✅ 85% / 100% / 110% / 125% / 150%，写盘持久化 |

## 启动闪烁的两个成因（2026-10-24 修复）

用户反馈启动时整个窗口闪。查 eframe 0.37 源码后定位到两处，与业务逻辑无关：

### 1. `clear_color` 默认带 alpha=180（半透明）

eframe 的 `App::clear_color` 默认实现是：

```rust
egui::Color32::from_rgba_unmultiplied(12, 12, 12, 180).to_normalized_gamma_f32()
```

**alpha = 180 是半透明**。本程序窗口不是透明窗口，驱动会把它与窗口底色（白色）
合成 ⇒ 启动/重绘时出现一次**亮闪**。

修法：在 `App` 里覆盖 `clear_color`，强制 `alpha = 1.0`，颜色取 `visuals.window_fill`。

### 2. 默认渲染后端是 wgpu，surface 异步建立

`eframe` 的默认 features 里带 `wgpu`（编译产物里能看到 `libwgpu` / `libegui_wgpu`）。
wgpu 在启动时异步创建 surface + swapchain，本机（Intel Arc A770 / D3D12）
在首帧前后会整窗闪一下。

修法：`Cargo.toml` 启用 `glow` feature，`NativeOptions.renderer = Renderer::Glow`
（OpenGL，同步初始化，启动更稳，也更轻）。

> ⚠ **回退方法**：把 `main.rs` 里的 `Renderer::Glow` 改回 `Renderer::Wgpu` 即可，
> wgpu 依赖仍保留（没有关掉默认 features）。若改成 glow 后窗口**根本不出现**，
> 说明该机 OpenGL 不可用，直接回退。

### 顺带做的

- `App::new` 里显式 `set_visuals(Visuals::dark())`，
  避免跟随系统浅色主题时首帧出现"亮 → 暗"切换。
- eframe 本身已在首帧渲染前 `.with_visible(false)`（源码注释：
  *"Start hidden until we render the first frame to fix white flash on startup"*），
  所以"白屏"那条不成立，不用额外处理。

---

## 为什么这样选型

| 决策 | 原因 |
|---|---|
| **Rust + egui** | 单文件、无运行时依赖；egui 立即模式 UI 改版快 |
| **不需要 IGCL 的 FFI** | 实测 LHM 已提供 Arc A770 的 `Temperatures/GPU Core` 与 `Fans/GPU Fan`，GPU 温/风扇直接读 LHM，省掉一大块 COM 互操作 |
| **GPU 使用率单独走 PDH** | LHM 只给显存占用，不给核心使用率；`\GPU Engine` 是任务管理器同源口径 |
| **LHM 内嵌** | 0.9.6 起官方移除 WMI 发布，唯一外部通道是它内置 Web Server；不内嵌就得让用户自己装/开 |
| **不用 COM 号认板** | COM 号每次枚举都变；USB 描述符的 iSerialNumber 又写死成 `0000000000001`，不唯一 |

## 板子身份识别（固件 v14+）

```
上位机 →  $ID?*32\r\n
板子   →  $ID:RA6E2-43045A764E4B2EB4,FW:14*XX\r\n
```

- ID 由 **芯片出厂 UID**（`0x01008190`，取首尾两字合成 64 bit）生成，全球唯一且不会变
- 上位机 `board::scan()` 枚举所有 `VID:PID = 045B:0263` 的端口，**逐个探活**，收到 `$ID:` 才算命中
- 老固件（v13 及以前）不认这条命令 ⇒ 界面显示「无 $ID 应答」，仍可手动连接（兼容）

## 串口打开为什么不卡 30 秒

用 pyserial / 常规 DCB 初始化时，`usbser.sys` 会在 `SETDTR` 上同步等
`SET_CONTROL_LINE_STATE` 的状态阶段，实测卡 **30.0 s**。

本程序用 `std::fs` 直接 `open(r"\\.\COM15")` 拿句柄，**只设 COMMTIMEOUTS，
完全不碰 DTR/RTS**，所以打开是瞬时的。写入也不需要 DTR（固件不据此门控）。

**2026-10-24：这个问题已经不必绕了。** 固件 v16 修掉了根因
（见下节「30 s 的根因」），`SETDTR` / `CloseHandle` 现在都是 **2 ms** 级。

于是 `board.rs` 恢复成最简单的模型：

```rust
pub fn acquire(port: &str) -> Result<RawSerial, String>   // 直接 open，最多重试 3 次
pub fn release(_port: &str, ser: RawSerial)               // 正常 drop（关闭是瞬时的）
```

唯一保留的技巧是**重试**：刚关掉的端口立刻再打开，偶发 `ERROR_GEN_FAILURE (31)`
（"连到系统上的设备没有发挥作用"），隔 150 ms 重试即可（实测第 1 次失败、第 2 次成功）。

> ⛔ 曾经为了躲 30 s 做过"句柄永不关闭"的单槽复用，已撤销：
> 设备重新枚举后缓存句柄会指向已消失的设备对象，表现为"设备管理器里 COM15 正常，
> 但 GUI 连不上"，还会让 `SERIALCOMM` 出现两条同名 COM15。
> **教训：绕开症状的方案，代价往往比症状本身更难查。**

## 30 s 的根因：FSP 漏了一步（v16 修复）

Windows 上 open/close 端口各卡 30 s 的根因，是**无数据阶段的类请求**
（`0x22 SET_CONTROL_LINE_STATE`）状态阶段发不出去：

- FSP 标准请求路径（`usb_pstd_set_configuration3()` 等）在 `ctrl_end` 之前会
  `usb_cstd_set_buf(pipe0)`，把 pipe0 的 PID 从 NAK 改成 **BUF**；
- 类请求路径 `usb_peri_class_request_wnss()` **漏了这一步**，直接 `ctrl_end`；
- ⇒ 状态阶段的 0 长度 IN 包拿不到 ACK ⇒ 设备停在 `CTSQ = 5`（无数据状态阶段）
  ⇒ 主机等到 30 s 超时。

铁证（FireDAP 直读 `INTSTS0`，DTR 操作期间连采 40 次）：恒为 `0x20B5`
（CTSQ=5 / DVSQ=3 已配置 / VBSTS=1）。

修法是应用侧补一行 `PID = BUF`（`usb_pipe0_nodata_arm()`，
详见 `src/usb/bsp_usb_cdc.c` 与 `docs`、`.workbuddy/memory/NOTES-usb-cdc.md`）。

## 扫描为什么快（板子信息缓存）

`%LOCALAPPDATA%\Ra6e2Monitor\boards.json`，**按芯片 UID 索引**（不是 COM 号）：

```json
{ "boards": { "RA6E2-43045A764E4B2EB4": { "port": "COM15", "fw": 15, "last_seen": 1792841564 } } }
```

- 启动时 `cached_boards()` 立刻把上次见过的板子灌进界面，**不等扫描就有内容**。
- `scan()` 里**缓存记录过的端口先探**，通常第一个就命中（探测超时 900 ms → 400 ms）。
- 缓存里有、当前没插的板子也会列出来，标红「未插」，连接按钮禁用。
- 实测：首次扫描 **214 ms**，二次 **218 ms** —— 瓶颈已是 SetupAPI 枚举本身。

## 数据来源对照

| 指标 | 来源 |
|---|---|
| CPU 温度 | LHM：`CPU Core Max` 优先，退到 `CPU Package` |
| CPU 使用率 | Win32 `GetSystemTimes` 差分 |
| CPU 风扇 | LHM：**`System Fan #3`**（见下方注意） |
| GPU 温度 | LHM：`Temperatures/GPU Core` |
| GPU 使用率 | PDH `\GPU Engine(*)\Utilization Percentage`（`engtype_3D` 跨进程求和） |
| GPU 风扇 | LHM：`Fans/GPU Fan` |
| 内存占用 | Win32 `GlobalMemoryStatusEx` |

### ⚠ LHM 的风扇通道名与主板丝印不对应

NCT6687D 有 6 路风扇通道，LHM 用的是**通用命名**。本机（MSI H610M BOMBER DDR4）实测：

| 主板丝印 | LHM 里的名字 | 说明 |
|---|---|---|
| **CPU_FAN1** | **`System Fan #3`** | 真的接着 CPU 散热器 |
| （空） | `CPU Fan` | 空口，0 RPM **且 PWM 恒 0%** |

改 `collector.rs` 里的 `CPU_FAN_NAME` 即可换通道。
判据：**`PWM=0%` 且 `RPM=0` ⇒ 空口**；真有风扇只是没测速线时 PWM 仍有占空比。

## 构建

```bash
cd gui
cargo build --release          # 产物: target/release/ra6e2-monitor.exe
```

- 需要 MSVC 工具链（`rustup default stable-x86_64-pc-windows-msvc`）
- UAC 提权由 `build.rs` 通过**链接器参数**嵌入（**没有** `app.manifest` 文件）：
  必须**同时**给 `/MANIFEST:EMBED` + `/MANIFESTUAC:level='requireAdministrator'`。
  只给 `/MANIFESTUAC` 时 exe 里搜不到 `requireAdministrator`，提权不生效；
  只用 `/MANIFESTINPUT:app.manifest` 会和 rustc 自带的 `asInvoker` 冲突（`LNK1327`）。
  DPI 由 winit 运行时处理，不需要清单声明。

### 截图调试开关

```bash
RA6E2_NO_UAC=1 cargo build --release     # 生成"不提权"版本，仅用于自动化截图
cargo build --release                    # 正式版，必须用这个交付
```

`RA6E2_NO_UAC=1` 只影响 `build.rs` 是否嵌入提权清单——因为提权后的进程没法被脚本
`CreateProcess` 拉起，自动化截图（`tools/shot_gui.py`）需要非提权版本。
**判据**：exe 里能搜到 `requireAdministrator` 才是交付物。不提权的版本读不到 SuperIO。

## 运行

```bash
./target/release/ra6e2-monitor.exe            # 图形界面（会弹 UAC）
./target/release/ra6e2-monitor.exe --selftest # 无界面自检：采集 + 组帧 + 扫板
```

⚠ exe 的 manifest 声明了 **requireAdministrator**（LHM 读 SuperIO 必须提权），
所以**连 `--selftest` 也要在「管理员终端」里跑**，否则会 `Permission denied`
（Windows 错误 740，UAC 提权不允许从非提权进程链式发起）。

`--selftest` 输出示例：

```
LHM 在跑          : true
PDH GPU 可用      : true
采样              : CT=Some(41) CU=Some(4) CF=Some(713) GT=Some(54) GU=Some(1) GF=Some(0) MU=Some(43)
payload           : CT:41,CU:4,CF:713,GT:54,GU:1,GF:0,MU:43
frame             : $CT:41,CU:4,CF:713,GT:54,GU:1,GF:0,MU:43*25
```

## 界面

- 左栏：板子 ID / 端口 / 固件 / UID、扫描板子、开始连接、断开、日志
- 右栏：CPU / GPU / 内存 三组勾选（组开关关掉时组内细项灰掉）、实时帧、已发送 / 实际帧率 / 写超时

**未勾的项整项不发送**（不是发 0）——固件按 key 解析，缺项屏上显示 `--`。

### 配色与字体

- **字体：优先粗体字面**，不是靠 `.strong()`。
  egui 的 `RichText::strong()` 只换颜色（`strong_text_color`），**字重不变**；
  真要加粗必须换字体文件。所以 `install_fonts()` 把粗体放进 family 的**第一顺位**：
  | 用途 | 首选 | 回落 |
  |---|---|---|
  | 比例（含中文） | `Dengb.ttf`（等线 Bold）| `msyhbd.ttc` → `Deng.ttf` → `simhei.ttf` |
  | 等宽 | `consolab.ttf`（Consolas Bold）| `consola.ttf`；中文回落到同一份 CJK 粗体 |
  ⚠ Consolas 没有中文字形，所以 Monospace 的**第二顺位必须放 CJK 字体**，
  否则日志里的中文会变成豆腐块。
- **配色：把默认偏暗的灰调亮**（`build_visuals()`）：
  | 用途 | 字段 | 值 |
  |---|---|---|
  | 正文 | `visuals.widgets.noninteractive.fg_stroke.color` | `(228, 234, 243)` |
  | `.weak()` 次要文字 | `visuals.weak_text_color` | `(172, 185, 201)` |
  | `.strong()` | `visuals.widgets.active.fg_stroke.color` | `(246, 250, 255)` |
  ⚠ egui 里 `strong_text_color()` 是**方法**不是字段，它返回 `widgets.active.text_color()`；
  想改 `.strong()` 的颜色必须改 `widgets.active`。
- **⛔ 必须钉死深色主题，否则系统一切浅色整窗就变白**（2026-10-24 实测踩到）：
  egui 0.37 的 `ThemePreference` **默认是 `System`**，系统主题一变就用浅色 visuals
  覆盖掉 `build_visuals()`。正确写法（**顺序不能反**，`set_visuals` 写的是当前主题槽）：

  ```rust
  cc.egui_ctx.set_theme(egui::ThemePreference::Dark);
  cc.egui_ctx.set_visuals(build_visuals());
  ```

### 版式注意（踩过的坑，改界面时先看这条）

- **横向并排多个 `Frame::group` 卡片时不要手算宽度。**
  `Frame::group` 固定 `inner_margin(6)`（左右各 6px），三张卡就多出 36px；
  再漏掉两组 `item_spacing(8)`，累计比可用宽度多 **32px** ⇒ 最后一张卡被推出窗口外。
  **正确做法：`ui.columns(3, |cols| ...)`**，列宽由 egui 按可用宽度均分，
  卡片内部用 `ui.set_width(ui.available_width())` 填满本列。
- **⛔ 一行布局千万不要用 `ui.with_layout()`**：它会把**全部可用高度**都吃掉，
  而 `Frame::group` 是按子 UI 的 `min_rect` 撑开的 ⇒ 卡片被撑到整屏高、
  `Align::Max` 再把内容压到卡片最底部、后面的组和卡片**全被挤出窗口**
  （2026-10-24 实际截图确认过，整屏只剩一个巨大的 CPU 框）。
  **正确做法：`ui.allocate_ui_with_layout(vec2(available_width, 行高), layout, add)`**，
  只申请一行的高度。
- **勾选行的自动换行已取消**：`bottom_row()` 固定单行高度（`interact_size.y`），
  所以窗口极窄时会横向溢出而不折行。取舍理由：**宁可溢出（看得见、能拉宽窗口），
  也不要布局被撑坏（会连累后面所有组都看不见）**。
  150% 缩放下已验证三个复选框 + 数值完整不截断。
- **行内数值不要用 `Layout::right_to_left` 摆**：那会把它顶到容器最右端
  （内存卡片就踩过，`39%` 跑到了框体末尾）。直接 `checkbox` 后面跟
  `ui.label(...)` 即可，间距由 `item_spacing.x` 自动提供。
  `right_to_left` 只适合顶栏状态这种**明确要右对齐**的地方。
- **同一类行的间距必须用同一种写法**：CPU/GPU/内存三行的「标签 → 数值」都是
  `checkbox` 紧跟 `label`，间距由 `item_spacing.x`（默认 8px）提供；
  如果某一行额外写了 `add_space`，就会变成 16px 而和其它行不一致。
  组与组之间（如"温度"到"占用"）才用 `add_space(10.0)`。
- **「标签 + 数值」行必须底对齐**，用 `bottom_row()`（内部是
  `Layout::left_to_right(Align::Max)`）而不是 `ui.horizontal()`：
  后者默认 `Align::Center`，而数值是 11px 等宽、左侧文字更大，
  居中对齐会让数值相对左边文字"浮高"，底边看着不齐。

## 目录

```
gui/
├── Cargo.toml
├── build.rs            链接器参数嵌入 UAC 提权清单（RA6E2_NO_UAC=1 可关掉，仅截图用）
├── assets/lhm.zip      内嵌的 LHM（8.2 MB，已剔除 *.pdb/*.xml）
├── tools/shot_gui.py   自动化截图：启动 exe → 按窗口标题抓 DWM 边界 → 关窗
└── src/
    ├── main.rs         入口 + --selftest
    ├── app.rs          egui UI + 工作线程
    ├── collector.rs    LHM HTTP / Win32 / PDH 采集
    ├── board.rs        板子识别 + 原始串口 + 帧协议
    └── lhm.rs          内嵌 LHM 的释放与拉起
```
