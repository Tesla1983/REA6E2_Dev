//! 中英双语文案。
//!
//! 默认中文（零回归），可在顶栏按钮切换并随 `Settings` 持久化。
//! 数值、厂商名（NVIDIA/AMD/Intel）、LibreHardwareMonitor 等专有名词不翻译。

/// 界面语言。默认中文。
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum Lang {
    #[default]
    Zh,
    En,
}

impl Lang {
    /// 切换按钮上的字：显示「将要切换到的语言」。
    pub fn switch_label(self) -> &'static str {
        match self {
            Lang::Zh => "EN",
            Lang::En => "中文",
        }
    }
    pub fn toggle(self) -> Self {
        match self {
            Lang::Zh => Lang::En,
            Lang::En => Lang::Zh,
        }
    }
    pub fn from_u8(v: u8) -> Self {
        if v == 1 {
            Lang::En
        } else {
            Lang::Zh
        }
    }
    pub fn to_u8(self) -> u8 {
        match self {
            Lang::Zh => 0,
            Lang::En => 1,
        }
    }
}

/// 界面静态标签（无动态参数）。
#[derive(Clone, Copy, Debug)]
pub enum T {
    Title,
    Settings,
    FontSize,
    ToTray,
    TrayHover,
    BoardHeader,
    Port,
    Firmware,
    Uid,
    NoBoard,
    NoBoardHint,
    Offline,
    Cached,
    ScanBoards,
    Connect,
    Disconnect,
    LogHeader,
    Telemetry,
    Cpu,
    Gpu,
    Temp,
    Load,
    Fan,
    Vram,
    Memory,
    GpuSelect,
    Source,
    LiveFrame,
    NotSent,
    Sent,
    Fps,
    Timeouts,
}

impl T {
    pub fn t(self, lang: Lang) -> &'static str {
        match (self, lang) {
            (T::Title, Lang::Zh) => "RA6E2 系统监控台",
            (T::Title, Lang::En) => "RA6E2 System Monitor",
            (T::Settings, Lang::Zh) => "设置",
            (T::Settings, Lang::En) => "Settings",
            (T::FontSize, Lang::Zh) => "界面字体大小",
            (T::FontSize, Lang::En) => "UI font size",
            (T::ToTray, Lang::Zh) => "收进托盘",
            (T::ToTray, Lang::En) => "To tray",
            (T::TrayHover, Lang::Zh) => "隐藏到系统托盘（采样与发帧继续）。双击托盘图标可恢复",
            (T::TrayHover, Lang::En) => {
                "Hide to system tray (sampling & framing continue). Double-click tray icon to restore"
            }
            (T::BoardHeader, Lang::Zh) => "板子识别",
            (T::BoardHeader, Lang::En) => "Board detection",
            (T::Port, Lang::Zh) => "端口",
            (T::Port, Lang::En) => "Port",
            (T::Firmware, Lang::Zh) => "固件",
            (T::Firmware, Lang::En) => "Firmware",
            (T::Uid, Lang::Zh) => "UID",
            (T::Uid, Lang::En) => "UID",
            (T::NoBoard, Lang::Zh) => "未发现板子",
            (T::NoBoard, Lang::En) => "No board found",
            (T::NoBoardHint, Lang::Zh) => "插上 Type-C 后点「扫描板子」",
            (T::NoBoardHint, Lang::En) => "Plug in Type-C, then click Scan",
            (T::Offline, Lang::Zh) => "未插",
            (T::Offline, Lang::En) => "Offline",
            (T::Cached, Lang::Zh) => "缓存",
            (T::Cached, Lang::En) => "Cached",
            (T::ScanBoards, Lang::Zh) => "扫描板子",
            (T::ScanBoards, Lang::En) => "Scan boards",
            (T::Connect, Lang::Zh) => "开始连接",
            (T::Connect, Lang::En) => "Connect",
            (T::Disconnect, Lang::Zh) => "断开",
            (T::Disconnect, Lang::En) => "Disconnect",
            (T::LogHeader, Lang::Zh) => "日志",
            (T::LogHeader, Lang::En) => "Log",
            (T::Telemetry, Lang::Zh) => "发送数据项",
            (T::Telemetry, Lang::En) => "Telemetry items",
            (T::Cpu, Lang::Zh) => "CPU",
            (T::Cpu, Lang::En) => "CPU",
            (T::Gpu, Lang::Zh) => "GPU",
            (T::Gpu, Lang::En) => "GPU",
            (T::Temp, Lang::Zh) => "温度",
            (T::Temp, Lang::En) => "Temp",
            (T::Load, Lang::Zh) => "占用",
            (T::Load, Lang::En) => "Load",
            (T::Fan, Lang::Zh) => "风扇",
            (T::Fan, Lang::En) => "Fan",
            (T::Vram, Lang::Zh) => "显存",
            (T::Vram, Lang::En) => "VRAM",
            (T::Memory, Lang::Zh) => "内存",
            (T::Memory, Lang::En) => "Memory",
            (T::GpuSelect, Lang::Zh) => "选卡:",
            (T::GpuSelect, Lang::En) => "GPU:",
            (T::Source, Lang::Zh) => "数据来源:",
            (T::Source, Lang::En) => "Source:",
            (T::LiveFrame, Lang::Zh) => "实时帧",
            (T::LiveFrame, Lang::En) => "Live frame",
            (T::NotSent, Lang::Zh) => "（尚未发送）",
            (T::NotSent, Lang::En) => "(not sent yet)",
            (T::Sent, Lang::Zh) => "已发送",
            (T::Sent, Lang::En) => "Sent",
            (T::Fps, Lang::Zh) => "实际帧率",
            (T::Fps, Lang::En) => "FPS",
            (T::Timeouts, Lang::Zh) => "写超时",
            (T::Timeouts, Lang::En) => "Timeouts",
        }
    }
}

/// 顶栏状态文字（含动态速率）。
pub fn status_text(running: bool, link_up: bool, scanning: bool, rate: f32, lang: Lang) -> String {
    match (running, link_up, scanning) {
        (true, true, _) => match lang {
            Lang::Zh => format!("已连接 · {rate:.1} Hz"),
            Lang::En => format!("Connected · {rate:.1} Hz"),
        },
        (true, false, _) => match lang {
            Lang::Zh => "连接中...".to_string(),
            Lang::En => "Connecting...".to_string(),
        },
        (_, _, true) => match lang {
            Lang::Zh => "扫描中...".to_string(),
            Lang::En => "Scanning...".to_string(),
        },
        _ => match lang {
            Lang::Zh => "未连接".to_string(),
            Lang::En => "Disconnected".to_string(),
        },
    }
}

/// 运行时日志消息（带动态参数）。
/// 历史日志会在切换语言时跟着重新翻译 —— 所以存枚举而非拼好的字符串。
#[derive(Clone, Debug)]
pub enum LogMsg {
    Font(String),
    Zoom(u32),
    CacheEmpty,
    CacheEntry {
        port: String,
        id: String,
        fw: String,
        online: bool,
    },
    WindowRestored {
        w: i32,
        h: i32,
        x: i32,
        y: i32,
    },
    WindowRestoreFailed,
    ScanStart,
    ScanFound {
        port: String,
        id: String,
        fw: String,
    },
    ScanNone,
    ScanDone {
        n: usize,
        ms: u128,
    },
    NoBoardToConnect,
    BoardOffline(String),
    ConnectStart {
        port: String,
        id: String,
    },
    Disconnected,
    RestoredFromTray,
    WindowRestoredFirstFrame,
    ZoomClamped {
        want: u32,
        got: u32,
    },
    SerialOpenFailedBack,
    ToTray,
    LhmReady,
    LhmUnavailable(String),
    PdhGpuUnavailable,
    SerialClosed(String),
    SerialOpened(String),
    SerialOpenErr(String),
    WriteFailed(String),
    HandleStale,
    ReopenFailed(String),
}

impl LogMsg {
    pub fn text(&self, lang: Lang) -> String {
        match (self, lang) {
            (LogMsg::Font(n), Lang::Zh) => format!("中文字体: {n}"),
            (LogMsg::Font(n), Lang::En) => format!("CJK font: {n}"),
            (LogMsg::Zoom(p), Lang::Zh) => format!("界面缩放: {p}%"),
            (LogMsg::Zoom(p), Lang::En) => format!("UI scale: {p}%"),
            (LogMsg::CacheEmpty, Lang::Zh) => "缓存: 空（首启动，等扫描识别）".into(),
            (LogMsg::CacheEmpty, Lang::En) => "Cache: empty (first launch, awaiting scan)".into(),
            (LogMsg::CacheEntry { port, id, fw, online }, Lang::Zh) => format!(
                "缓存: {port} → {id}（固件 {fw}）{}",
                if *online { "在线" } else { "未插" }
            ),
            (LogMsg::CacheEntry { port, id, fw, online }, Lang::En) => format!(
                "Cache: {port} → {id} (fw {fw}) {}",
                if *online { "online" } else { "offline" }
            ),
            (LogMsg::WindowRestored { w, h, x, y }, Lang::Zh) => {
                format!("已恢复窗口位置/尺寸: {w}×{h} @ ({x}, {y})")
            }
            (LogMsg::WindowRestored { w, h, x, y }, Lang::En) => {
                format!("Restored window: {w}x{h} @ ({x},{y})")
            }
            (LogMsg::WindowRestoreFailed, Lang::Zh) => {
                "恢复窗口位置失败（还没找到主窗口），第一帧再试".into()
            }
            (LogMsg::WindowRestoreFailed, Lang::En) => {
                "Window restore failed (main window not found yet), retry on first frame".into()
            }
            (LogMsg::ScanStart, Lang::Zh) => {
                "扫描 RA6E2 候选串口（按 VID:PID 045B:0263，不认 COM 号）...".into()
            }
            (LogMsg::ScanStart, Lang::En) => {
                "Scanning RA6E2 candidate ports (by VID:PID 045B:0263, not by COM number)...".into()
            }
            (LogMsg::ScanFound { port, id, fw }, Lang::Zh) => {
                format!("发现 {port} → {id}（固件 {fw}）")
            }
            (LogMsg::ScanFound { port, id, fw }, Lang::En) => format!("Found {port} → {id} (fw {fw})"),
            (LogMsg::ScanNone, Lang::Zh) => {
                "没有找到 045B:0263 的设备（板子没插 / 没跑固件）".into()
            }
            (LogMsg::ScanNone, Lang::En) => {
                "No 045B:0263 device found (board unplugged / firmware not running)".into()
            }
            (LogMsg::ScanDone { n, ms }, Lang::Zh) => format!(
                "扫描完成：{n} 块，耗时 {ms} ms（缓存命中时只需一次 $ID? 往返）"
            ),
            (LogMsg::ScanDone { n, ms }, Lang::En) => format!(
                "Scan done: {n} boards, {ms} ms (cache hit needs only one $ID? round-trip)"
            ),
            (LogMsg::NoBoardToConnect, Lang::Zh) => "没有可连接的板子，先点「扫描板子」".into(),
            (LogMsg::NoBoardToConnect, Lang::En) => "No board to connect, scan first".into(),
            (LogMsg::BoardOffline(p), Lang::Zh) => {
                format!("{p} 当前不在线（来自缓存）。插上 Type-C 后重新扫描再连接。")
            }
            (LogMsg::BoardOffline(p), Lang::En) => format!(
                "{p} is offline (from cache). Plug in Type-C, re-scan, then connect."
            ),
            (LogMsg::ConnectStart { port, id }, Lang::Zh) => {
                format!("开始连接 {port}（ID {id}）...")
            }
            (LogMsg::ConnectStart { port, id }, Lang::En) => {
                format!("Connecting to {port} (ID {id})...")
            }
            (LogMsg::Disconnected, Lang::Zh) => "已断开（采样继续，只是不再发帧）".into(),
            (LogMsg::Disconnected, Lang::En) => {
                "Disconnected (sampling continues, just stopped sending frames)".into()
            }
            (LogMsg::RestoredFromTray, Lang::Zh) => "已从托盘恢复窗口".into(),
            (LogMsg::RestoredFromTray, Lang::En) => "Restored window from tray".into(),
            (LogMsg::WindowRestoredFirstFrame, Lang::Zh) => "已恢复窗口位置/尺寸（首帧补做）".into(),
            (LogMsg::WindowRestoredFirstFrame, Lang::En) => "Restored window (first-frame retry)".into(),
            (LogMsg::ZoomClamped { want, got }, Lang::Zh) => {
                format!("本机屏幕放不下 {want}% 缩放，已限制为 {got}%")
            }
            (LogMsg::ZoomClamped { want, got }, Lang::En) => {
                format!("Screen too small for {want}% zoom, clamped to {got}%")
            }
            (LogMsg::SerialOpenFailedBack, Lang::Zh) => {
                "串口未能打开，已回到未连接状态（采样继续）".into()
            }
            (LogMsg::SerialOpenFailedBack, Lang::En) => {
                "Serial port failed to open, back to disconnected (sampling continues)".into()
            }
            (LogMsg::ToTray, Lang::Zh) => "已收进系统托盘（采样与发帧继续）".into(),
            (LogMsg::ToTray, Lang::En) => "Minimized to tray (sampling and framing continue)".into(),
            (LogMsg::LhmReady, Lang::Zh) => "LHM 已就绪（未连接板子也会采样）".into(),
            (LogMsg::LhmReady, Lang::En) => "LHM ready (sampling even without a board)".into(),
            (LogMsg::LhmUnavailable(e), Lang::Zh) => format!("LHM 不可用: {e}（温度/风扇会缺失）"),
            (LogMsg::LhmUnavailable(e), Lang::En) => format!("LHM unavailable: {e} (temp/fan missing)"),
            (LogMsg::PdhGpuUnavailable, Lang::Zh) => {
                "PDH 的 GPU Engine 计数器不可用，GPU 使用率将缺失".into()
            }
            (LogMsg::PdhGpuUnavailable, Lang::En) => {
                "PDH GPU Engine counter unavailable, GPU usage missing".into()
            }
            (LogMsg::SerialClosed(p), Lang::Zh) => format!("关闭串口 {p}"),
            (LogMsg::SerialClosed(p), Lang::En) => format!("Closed serial port {p}"),
            (LogMsg::SerialOpened(w), Lang::Zh) => format!("串口已打开 {w}（未设置 DTR/RTS）"),
            (LogMsg::SerialOpened(w), Lang::En) => format!("Serial port opened {w} (DTR/RTS not set)"),
            (LogMsg::SerialOpenErr(e), Lang::Zh) => format!("打开串口失败: {e}（采样继续）"),
            (LogMsg::SerialOpenErr(e), Lang::En) => {
                format!("Failed to open serial port: {e} (sampling continues)")
            }
            (LogMsg::WriteFailed(e), Lang::Zh) => format!("写失败: {e}"),
            (LogMsg::WriteFailed(e), Lang::En) => format!("Write failed: {e}"),
            (LogMsg::HandleStale, Lang::Zh) => "句柄已失效（设备重新枚举过）→ 重新打开串口".into(),
            (LogMsg::HandleStale, Lang::En) => {
                "Handle stale (device re-enumerated) → reopen serial port".into()
            }
            (LogMsg::ReopenFailed(ee), Lang::Zh) => format!("重开串口失败: {ee}"),
            (LogMsg::ReopenFailed(ee), Lang::En) => format!("Reopen serial port failed: {ee}"),
        }
    }
}
