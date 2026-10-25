//! 主窗口：左「板子识别 + 连接」，右「发送数据项 + 实时帧 + 统计」。

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{channel, Receiver, Sender};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use eframe::egui;

use crate::board::{self, Board};
use crate::collector::{self, CpuUsage, GpuUsage, Sample};

// ---------------------------------------------------------------- 共享状态

#[derive(Default)]
struct Shared {
    sample: Sample,
    sent: u64,
    errors: u64,
    last_frame: String,
    started: Option<Instant>,
    link_up: bool,
    rate: f32,
}

/// 勾选状态。组开关关掉时，组内细项灰掉且不生效。
#[derive(Clone, Copy)]
pub struct Mask {
    pub cpu_group: bool,
    pub cpu_temp: bool,
    pub cpu_load: bool,
    pub cpu_fan: bool,
    pub gpu_group: bool,
    pub gpu_temp: bool,
    pub gpu_load: bool,
    pub gpu_fan: bool,
    /// 显存占用率（协议 key `GV`）。**默认不勾**（屏上暂不显示，只在上位机看）。
    pub gpu_vram: bool,
    pub mem: bool,
}

impl Default for Mask {
    fn default() -> Self {
        Self {
            cpu_group: true,
            cpu_temp: true,
            cpu_load: true,
            cpu_fan: true,
            gpu_group: true,
            gpu_temp: true,
            gpu_load: true,
            gpu_fan: true,
            gpu_vram: false,
            mem: true,
        }
    }
}

impl Mask {
    /// 真正要发的字段（顺序固定 = 协议顺序）。
    fn items(&self) -> Vec<(bool, &'static str)> {
        vec![
            (self.cpu_group && self.cpu_temp, "CT"),
            (self.cpu_group && self.cpu_load, "CU"),
            (self.cpu_group && self.cpu_fan, "CF"),
            (self.gpu_group && self.gpu_temp, "GT"),
            (self.gpu_group && self.gpu_load, "GU"),
            (self.gpu_group && self.gpu_fan, "GF"),
            (self.gpu_group && self.gpu_vram, "GV"),
            (self.mem, "MU"),
        ]
    }
}

// ---------------------------------------------------------------- App

pub struct App {
    boards: Vec<Board>,
    selected: usize,
    scanning: bool,
    running: bool,
    mask: Mask,
    log: Vec<String>,
    log_rx: Receiver<String>,
    log_tx: Sender<String>,
    scan_rx: Option<Receiver<Vec<Board>>>,
    shared: Arc<Mutex<Shared>>,
    mask_shared: Arc<Mutex<Mask>>,
    stop: Arc<AtomicBool>,
    font_note: String,
    /// 界面缩放（= 字体与控件大小）。1.0 = 100%，持久化到 %LOCALAPPDATA%。
    zoom: f32,
}

impl App {
    pub fn new(cc: &eframe::CreationContext<'_>) -> Self {
        let font_note = install_fonts(&cc.egui_ctx);
        let (log_tx, log_rx) = channel();

        // 明确用深色主题 + 自己调过的配色：
        //   · 避免跟随系统浅色主题时，首帧出现一次"亮 → 暗"的切换闪
        //   · 把默认偏暗的灰调亮（用户反馈灰色看着不舒服）
        // ⚠ egui 0.37 的 ThemePreference 默认是 **System**：系统一旦切到浅色主题，
        //   它会用浅色 visuals 覆盖掉 build_visuals() 设的深色（实测过整窗变白）。
        //   ⇒ 先钉死 Dark，再写 visuals（顺序不能反：set_visuals 写的是当前主题槽）。
        cc.egui_ctx.set_theme(egui::ThemePreference::Dark);
        cc.egui_ctx.set_visuals(build_visuals());

        let zoom = load_zoom();
        cc.egui_ctx.set_zoom_factor(zoom);

        // 先把上次见过的板子从磁盘缓存里灌进来 —— 界面**立刻**有内容，
        // 不用等扫描。随后 start_scan() 在后台核实并刷新。
        let cached = board::cached_boards();

        let mut app = Self {
            boards: cached.clone(),
            selected: 0,
            scanning: false,
            running: false,
            mask: Mask::default(),
            log: Vec::new(),
            log_rx,
            log_tx,
            scan_rx: None,
            shared: Arc::new(Mutex::new(Shared::default())),
            mask_shared: Arc::new(Mutex::new(Mask::default())),
            stop: Arc::new(AtomicBool::new(false)),
            font_note,
            zoom,
        };
        app.push_log(format!("中文字体: {}", app.font_note.clone()));
        app.push_log(format!("界面缩放: {:.0}%", app.zoom * 100.0));
        if cached.is_empty() {
            app.push_log("缓存: 空（首启动，等扫描识别）".into());
        } else {
            for b in &cached {
                app.push_log(format!(
                    "缓存: {} → {}（固件 {}）{}",
                    b.port,
                    b.id.clone().unwrap_or_else(|| "—".into()),
                    b.fw.map(|v| format!("v{v}")).unwrap_or_else(|| "—".into()),
                    if b.online { "在线" } else { "未插" }
                ));
            }
        }
        app.start_scan();
        app
    }

    fn push_log(&mut self, s: String) {
        let ts = chrono_like_now();
        self.log.push(format!("{ts} {s}"));
        if self.log.len() > 300 {
            self.log.drain(..100);
        }
    }

    fn start_scan(&mut self) {
        if self.scanning || self.running {
            return;
        }
        self.scanning = true;
        self.push_log("扫描 RA6E2 候选串口（按 VID:PID 045B:0263，不认 COM 号）...".into());

        let (tx, rx) = channel();
        self.scan_rx = Some(rx);
        let log = self.log_tx.clone();
        std::thread::spawn(move || {
            let t0 = Instant::now();
            let list = board::scan();
            for b in &list {
                let _ = log.send(format!(
                    "发现 {} → {}（固件 {}）",
                    b.port,
                    b.id.clone().unwrap_or_else(|| "无 $ID 应答".into()),
                    b.fw.map(|v| format!("v{v}")).unwrap_or_else(|| "无".into())
                ));
            }
            if list.is_empty() {
                let _ = log.send("没有找到 045B:0263 的设备（板子没插 / 没跑固件）".into());
            }
            let _ = log.send(format!(
                "扫描完成：{} 块，耗时 {} ms（缓存命中时只需一次 $ID? 往返）",
                list.len(),
                t0.elapsed().as_millis()
            ));
            let _ = tx.send(list);
        });
    }

    fn start_connect(&mut self) {
        let Some(b) = self.boards.get(self.selected).cloned() else {
            self.push_log("没有可连接的板子，先点「扫描板子」".into());
            return;
        };

        if !b.online {
            self.push_log(format!(
                "{} 当前不在线（来自缓存）。插上 Type-C 后重新扫描再连接。",
                b.port
            ));
            return;
        }

        self.stop.store(false, Ordering::Relaxed);
        self.running = true;
        self.push_log(format!(
            "开始连接 {}（ID {}）...",
            b.port,
            b.id.clone().unwrap_or_else(|| "未识别".into())
        ));

        let stop = self.stop.clone();
        let shared = self.shared.clone();
        let mask = self.mask_shared.clone();
        let log = self.log_tx.clone();
        std::thread::spawn(move || worker(b.port, stop, shared, mask, log));
    }

    fn stop_connect(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        self.running = false;
        if let Ok(mut s) = self.shared.lock() {
            s.link_up = false;
        }
        self.push_log("已断开".into());
    }
}

// ---------------------------------------------------------------- 工作线程

fn worker(
    port: String,
    stop: Arc<AtomicBool>,
    shared: Arc<Mutex<Shared>>,
    mask: Arc<Mutex<Mask>>,
    log: Sender<String>,
) {
    let say = |m: String| {
        let _ = log.send(m);
    };

    // LHM 没跑就没有温度/风扇（它要读 MSR/SuperIO 内核驱动）
    match crate::lhm::ensure_running() {
        Ok(()) => say("LHM 已就绪".into()),
        Err(e) => say(format!("LHM 不可用: {e}（温度/风扇会缺失）")),
    }

    // ⚠ 用 acquire/release（句柄槽）而不是 RawSerial::open：
    //   若这里直接 open 再在函数结束时 drop ⇒ CloseHandle 会拉低 DTR ⇒ 卡 30 s。
    let mut ser = match board::acquire(&port) {
        Ok(s) => s,
        Err(e) => {
            say(format!("打开串口失败: {e}"));
            if let Ok(mut sh) = shared.lock() {
                sh.link_up = false;
            }
            return;
        }
    };
    say(format!("串口已打开 {port}（未设置 DTR/RTS）"));

    let mut cpu = CpuUsage::new();
    let mut gpu = GpuUsage::new();
    if gpu.is_none() {
        say("PDH \\GPU Engine 不可用，GPU 使用率将缺失".into());
    }

    if let Ok(mut sh) = shared.lock() {
        sh.started = Some(Instant::now());
        sh.sent = 0;
        sh.errors = 0;
        sh.link_up = true;
    }

    let period = Duration::from_millis(1000);

    while !stop.load(Ordering::Relaxed) {
        let tick = Instant::now();

        let sample = collector::collect(&mut cpu, &mut gpu);

        let items = mask.lock().map(|m| m.items()).unwrap_or_default();
        let payload = board::build_payload(&sample, &items);

        if !payload.is_empty() {
            let f = board::frame(&payload);
            let mut ok = true;
            if let Err(e) = ser.write_all(&f) {
                ok = false;
                say(format!("写失败: {e}"));
                // 设备可能刚被重新枚举（拔插 Type-C / 复位 / 重烧固件）：
                // 句柄指向已消失的设备对象，此时"重开 COM 口"是唯一解。
                if !ser.alive() {
                    say("句柄已失效（设备重新枚举过）→ 重新打开串口".into());
                    match board::acquire(&port) {
                        // v16 起 CloseHandle 是 2 ms 级，旧句柄正常析构即可
                        Ok(s2) => ser = s2,
                        Err(ee) => say(format!("重开串口失败: {ee}")),
                    }
                }
            }
            if let Ok(mut sh) = shared.lock() {
                if ok {
                    sh.sent += 1;
                    sh.last_frame = String::from_utf8_lossy(&f).trim_end().to_string();
                    if let Some(t0) = sh.started {
                        let secs = t0.elapsed().as_secs_f32().max(0.001);
                        sh.rate = sh.sent as f32 / secs;
                    }
                } else {
                    sh.errors += 1;
                }
            }
        }

        if let Ok(mut sh) = shared.lock() {
            sh.sample = sample;
        }

        let spent = tick.elapsed();
        if spent < period {
            // 切成小片睡，断开能及时响应
            let mut left = period - spent;
            while left > Duration::ZERO && !stop.load(Ordering::Relaxed) {
                let step = left.min(Duration::from_millis(100));
                std::thread::sleep(step);
                left -= step;
            }
        }
    }

    if let Ok(mut sh) = shared.lock() {
        sh.link_up = false;
    }
    // 句柄放回槽里复用，**不关闭**（关闭 = 30 s）。
    board::release(&port, ser);
    say("发送线程已退出".into());
}

// ---------------------------------------------------------------- 字体

/// egui 自带字体没有中文。Windows 上直接读系统字体（不随包分发，避免字体授权问题）。
/// 装载字体：**优先用粗体字面**，这样整个界面天然是粗的。
///
/// 为什么不用 `RichText::strong()`：egui 里 `.strong()` 只是把颜色换成
/// `strong_text_color`，**字重不变**。真正的粗体必须换字体文件。
/// 所以这里把粗体字面放进 family 的**第一顺位**，全局（含日志等宽字体）一起变粗。
///
/// - 比例字体：`Dengb.ttf`（等线 Bold，含中文）→ 退 `Deng.ttf`（常规）→ 系统其他中文
/// - 等宽字体：`consolab.ttf`（Consolas Bold，只有西文）→ 中文回落到同一份 CJK 粗体
fn install_fonts(ctx: &egui::Context) -> String {
    // (路径, 是否粗体) —— 按优先级排列
    const CJK: [(&str, bool); 4] = [
        (r"C:\Windows\Fonts\Dengb.ttf", true),
        (r"C:\Windows\Fonts\msyhbd.ttc", true),
        (r"C:\Windows\Fonts\Deng.ttf", false),
        (r"C:\Windows\Fonts\simhei.ttf", false),
    ];
    const MONO: [(&str, bool); 2] = [
        (r"C:\Windows\Fonts\consolab.ttf", true),
        (r"C:\Windows\Fonts\consola.ttf", false),
    ];

    let pick = |list: &[(&str, bool)]| -> Option<(String, bool, Vec<u8>)> {
        for (path, bold) in list {
            if let Ok(bytes) = std::fs::read(path) {
                return Some((path.rsplit('\\').next().unwrap_or(path).to_string(), *bold, bytes));
            }
        }
        None
    };

    let mut fonts = egui::FontDefinitions::default();
    let mut notes: Vec<String> = Vec::new();
    let mut have_cjk = false;

    if let Some((name, bold, bytes)) = pick(&CJK) {
        fonts
            .font_data
            .insert("cjk".to_owned(), Arc::new(egui::FontData::from_owned(bytes)));
        fonts
            .families
            .entry(egui::FontFamily::Proportional)
            .or_default()
            .insert(0, "cjk".to_owned());
        notes.push(format!("{name}{}", if bold { " 粗" } else { " 常规" }));
        have_cjk = true;
    }

    if let Some((name, bold, bytes)) = pick(&MONO) {
        fonts
            .font_data
            .insert("mono".to_owned(), Arc::new(egui::FontData::from_owned(bytes)));
        let fam = fonts.families.entry(egui::FontFamily::Monospace).or_default();
        fam.insert(0, "mono".to_owned());
        // Consolas 没有中文：把 CJK 粗体放到第二顺位，否则日志里的中文会变成豆腐块
        if have_cjk {
            fam.insert(1, "cjk".to_owned());
        }
        notes.push(format!("等宽 {name}{}", if bold { " 粗" } else { " 常规" }));
    } else if have_cjk {
        fonts
            .families
            .entry(egui::FontFamily::Monospace)
            .or_default()
            .insert(0, "cjk".to_owned());
    }

    ctx.set_fonts(fonts);

    if notes.is_empty() {
        "未找到中文字体（界面会缺字）".to_string()
    } else {
        notes.join(" / ")
    }
}

/// 配色：把默认那套偏暗的灰调亮，减少眼睛疲劳。
///
/// · 正文 → 接近白（默认 dark 主题是灰 ~160，偏灰）
/// · `.weak()` 的次要文字 → 柔和冷灰（默认更暗，几乎看不出内容）
fn build_visuals() -> egui::Visuals {
    let mut v = egui::Visuals::dark();
    // 正文（Label 默认色取自 widgets.noninteractive.fg_stroke）
    v.widgets.noninteractive.fg_stroke.color = egui::Color32::from_rgb(228, 234, 243);
    // .weak() 的次要文字（默认是 text_color * weak_text_alpha，很暗）
    v.weak_text_color = Some(egui::Color32::from_rgb(172, 185, 201));
    // .strong() 的颜色取自 widgets.active（egui 里 strong_text_color() 就是它）
    v.widgets.active.fg_stroke.color = egui::Color32::from_rgb(246, 250, 255);
    v
}

/// 界面设置的落盘位置：`%LOCALAPPDATA%\Ra6e2Monitor\ui.json`
fn settings_path() -> std::path::PathBuf {
    let base = std::env::var("LOCALAPPDATA").unwrap_or_else(|_| ".".to_string());
    std::path::PathBuf::from(base).join("Ra6e2Monitor").join("ui.json")
}

fn load_zoom() -> f32 {
    if let Ok(text) = std::fs::read_to_string(settings_path()) {
        if let Ok(v) = serde_json::from_str::<serde_json::Value>(&text) {
            if let Some(z) = v.get("zoom").and_then(|x| x.as_f64()) {
                if (0.6..=2.5).contains(&z) {
                    return z as f32;
                }
            }
        }
    }
    1.0
}

fn save_zoom(z: f32) {
    let p = settings_path();
    if let Some(dir) = p.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    let _ = std::fs::write(p, format!("{{\"zoom\":{z}}}"));
}

fn chrono_like_now() -> String {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    let secs = (now + 8 * 3600) % 86400; // 本机在东八区
    format!("{:02}:{:02}:{:02}", secs / 3600, (secs % 3600) / 60, secs % 60)
}

// ---------------------------------------------------------------- UI

impl eframe::App for App {
    /// eframe 0.37 起 trait 方法是 `ui(&mut Ui)`（不再是 `update(&Context)`），
    /// 面板也统一成 `egui::Panel`（SidePanel/TopBottomPanel 已合并）。
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();

        // 把 UI 上的勾选同步给工作线程
        if let Ok(mut m) = self.mask_shared.lock() {
            *m = self.mask;
        }

        while let Ok(msg) = self.log_rx.try_recv() {
            self.push_log(msg);
        }
        if let Some(rx) = &self.scan_rx {
            if let Ok(list) = rx.try_recv() {
                self.boards = list;
                self.selected = 0;
                self.scanning = false;
                self.scan_rx = None;
            }
        }

        let (sample, sent, errors, last_frame, link_up, rate) = {
            let s = self.shared.lock().unwrap();
            (s.sample.clone(), s.sent, s.errors, s.last_frame.clone(), s.link_up, s.rate)
        };

        self.top_bar(ui, link_up, rate);

        egui::Panel::left("left")
            .resizable(false)
            .exact_size(288.0)
            .show(ui, |ui| self.side_panel(ui));

        egui::CentralPanel::default_margins().show(ui, |ui| {
            self.center_panel(ui, &sample, sent, errors, &last_frame, rate);
        });

        ctx.request_repaint_after(Duration::from_millis(200));
    }

    /// 清屏色。
    ///
    /// ⚠ eframe 默认实现返回 `Color32::from_rgba_unmultiplied(12, 12, 12, 180)`，
    /// **alpha = 180 是半透明**。本程序窗口不是透明窗口，驱动会把它跟窗口底色
    /// （白色）合成 ⇒ 启动/重绘时能看到一次**亮闪**。
    /// 这里强制 alpha = 1.0，颜色取当前主题的面板色，彻底消除这次闪。
    fn clear_color(&self, visuals: &egui::Visuals) -> [f32; 4] {
        let mut c = visuals.window_fill.to_normalized_gamma_f32();
        c[3] = 1.0;
        c
    }
}

impl App {
    fn top_bar(&mut self, ui: &mut egui::Ui, link_up: bool, rate: f32) {
        egui::Panel::top("top").show(ui, |ui| {
            ui.add_space(4.0);
            ui.horizontal(|ui| {
                let (dot, txt) = if self.running && link_up {
                    (egui::Color32::from_rgb(76, 175, 80), format!("已连接 · {:.1} Hz", rate))
                } else if self.running {
                    (egui::Color32::from_rgb(255, 193, 7), "连接中...".to_string())
                } else if self.scanning {
                    (egui::Color32::from_rgb(255, 193, 7), "扫描中...".to_string())
                } else {
                    (egui::Color32::from_rgb(140, 140, 140), "未连接".to_string())
                };
                let (rect, _) = ui.allocate_exact_size(egui::vec2(9.0, 9.0), egui::Sense::hover());
                ui.painter().circle_filled(rect.center(), 4.5, dot);

                ui.label(egui::RichText::new("RA6E2 系统监控台").size(14.0).strong());
                ui.label(egui::RichText::new("v0.1").size(11.0).weak());
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    // right_to_left 布局：先加的在上层（最右），所以"设置"在最右边
                    ui.menu_button("设置", |ui| {
                        ui.set_min_width(190.0);
                        ui.label(egui::RichText::new("界面字体大小").size(11.0).weak());
                        ui.add_space(4.0);

                        for (label, z) in [
                            ("85%", 0.85_f32),
                            ("100%", 1.0),
                            ("110%", 1.1),
                            ("125%", 1.25),
                            ("150%", 1.5),
                        ] {
                            let sel = (self.zoom - z).abs() < 1e-3;
                            if ui.selectable_label(sel, label).clicked() {
                                self.zoom = z;
                                ui.ctx().set_zoom_factor(z);
                                save_zoom(z);
                                ui.close();
                            }
                        }

                        ui.separator();
                        ui.label(
                            egui::RichText::new(format!("当前 {:.0}%", self.zoom * 100.0))
                                .size(11.0)
                                .weak(),
                        );
                    });
                    ui.label(egui::RichText::new(txt).size(12.0).color(dot));
                });
            });
            ui.add_space(4.0);
        });
    }

    fn side_panel(&mut self, ui: &mut egui::Ui) {
        ui.add_space(6.0);
        ui.label(egui::RichText::new("板子识别").size(11.0).weak());
        ui.add_space(4.0);

        egui::Frame::group(ui.style()).show(ui, |ui| {
            ui.set_width(ui.available_width());
            match self.boards.get(self.selected) {
                Some(b) => {
                    ui.label(
                        egui::RichText::new(format!("RA6E2-{}", b.short_id()))
                            .size(13.0)
                            .strong(),
                    );
                    ui.add_space(3.0);
                    egui::Grid::new("board_grid").num_columns(2).spacing([8.0, 3.0]).show(ui, |ui| {
                        ui.label(egui::RichText::new("端口").size(11.0).weak());
                        ui.horizontal(|ui| {
                            ui.label(egui::RichText::new(&b.port).size(11.0).monospace());
                            if !b.online {
                                ui.label(
                                    egui::RichText::new("未插")
                                        .size(10.0)
                                        .color(egui::Color32::from_rgb(230, 110, 110)),
                                );
                            } else if b.cached {
                                ui.label(
                                    egui::RichText::new("缓存")
                                        .size(10.0)
                                        .color(egui::Color32::from_rgb(150, 170, 200)),
                                );
                            }
                        });
                        ui.end_row();
                        ui.label(egui::RichText::new("固件").size(11.0).weak());
                        ui.label(
                            egui::RichText::new(match b.fw {
                                Some(v) => format!("v{v}"),
                                None => "无 $ID 应答".to_string(),
                            })
                            .size(11.0)
                            .monospace(),
                        );
                        ui.end_row();
                        ui.label(egui::RichText::new("UID").size(11.0).weak());
                        ui.label(
                            egui::RichText::new(b.id.clone().unwrap_or_else(|| "—".into()))
                                .size(11.0)
                                .monospace(),
                        );
                        ui.end_row();
                    });
                }
                None => {
                    ui.label(egui::RichText::new("未发现板子").size(12.0));
                    ui.label(egui::RichText::new("插上 Type-C 后点「扫描板子」").size(11.0).weak());
                }
            }
        });

        ui.add_space(8.0);

        if self.boards.len() > 1 {
            egui::ComboBox::from_id_salt("board_pick")
                .selected_text(
                    self.boards
                        .get(self.selected)
                        .map(|b| format!("{} {}", b.port, b.short_id()))
                        .unwrap_or_default(),
                )
                .show_ui(ui, |ui| {
                    for (i, b) in self.boards.iter().enumerate() {
                        ui.selectable_value(&mut self.selected, i, format!("{} {}", b.port, b.short_id()));
                    }
                });
            ui.add_space(6.0);
        }

        let can_scan = !self.scanning && !self.running;
        if ui
            .add_enabled(can_scan, egui::Button::new("扫描板子").min_size(egui::vec2(ui.available_width(), 26.0)))
            .clicked()
        {
            self.start_scan();
        }

        ui.add_space(6.0);
        ui.horizontal(|ui| {
            let w = (ui.available_width() - 6.0) / 2.0;
            if ui
                .add_enabled(
                    !self.running
                        && self
                            .boards
                            .get(self.selected)
                            .map(|b| b.online)
                            .unwrap_or(false),
                    egui::Button::new(egui::RichText::new("开始连接").color(egui::Color32::from_rgb(96, 200, 120)))
                        .min_size(egui::vec2(w, 28.0)),
                )
                .clicked()
            {
                self.start_connect();
            }
            if ui
                .add_enabled(
                    self.running,
                    egui::Button::new("断开").min_size(egui::vec2(w, 28.0)),
                )
                .clicked()
            {
                self.stop_connect();
            }
        });

        ui.add_space(10.0);
        ui.label(egui::RichText::new("日志").size(11.0).weak());
        ui.add_space(3.0);
        egui::Frame::group(ui.style()).show(ui, |ui| {
            ui.set_width(ui.available_width());
            egui::ScrollArea::vertical()
                .stick_to_bottom(true)
                .max_height(150.0)
                .show(ui, |ui| {
                    for line in &self.log {
                        ui.label(egui::RichText::new(line).size(11.0).monospace().weak());
                    }
                });
        });
    }

    fn center_panel(
        &mut self,
        ui: &mut egui::Ui,
        sample: &Sample,
        sent: u64,
        errors: u64,
        last_frame: &str,
        rate: f32,
    ) {
        ui.add_space(6.0);
        ui.label(egui::RichText::new("发送数据项").size(11.0).weak());
        ui.add_space(5.0);

        let fmt = |v: Option<i32>, unit: &str| match v {
            Some(x) => format!("{x}{unit}"),
            None => "—".to_string(),
        };

        egui::Frame::group(ui.style()).show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.checkbox(&mut self.mask.cpu_group, egui::RichText::new("CPU").size(12.0).strong());
            ui.add_space(4.0);
            // bottom_row：底对齐 + 可换行（窗口窄或 zoom 调大时自动换行）
            bottom_row(ui, |ui| {
                ui.add_enabled_ui(self.mask.cpu_group, |ui| {
                    ui.checkbox(&mut self.mask.cpu_temp, "温度");
                    ui.label(egui::RichText::new(fmt(sample.ct, "°C")).size(11.0).monospace().weak());
                    ui.add_space(10.0);
                    ui.checkbox(&mut self.mask.cpu_load, "占用");
                    ui.label(egui::RichText::new(fmt(sample.cu, "%")).size(11.0).monospace().weak());
                    ui.add_space(10.0);
                    ui.checkbox(&mut self.mask.cpu_fan, "风扇");
                    ui.label(egui::RichText::new(fmt(sample.cf_display, sample.cf_unit.suffix())).size(11.0).monospace().weak());
                });
            });
        });

        ui.add_space(6.0);
        egui::Frame::group(ui.style()).show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.checkbox(&mut self.mask.gpu_group, egui::RichText::new("GPU").size(12.0).strong());
            ui.add_space(4.0);
            bottom_row(ui, |ui| {
                ui.add_enabled_ui(self.mask.gpu_group, |ui| {
                    ui.checkbox(&mut self.mask.gpu_temp, "温度");
                    ui.label(egui::RichText::new(fmt(sample.gt, "°C")).size(11.0).monospace().weak());
                    ui.add_space(10.0);
                    ui.checkbox(&mut self.mask.gpu_load, "占用");
                    ui.label(egui::RichText::new(fmt(sample.gu, "%")).size(11.0).monospace().weak());
                    ui.add_space(10.0);
                    ui.checkbox(&mut self.mask.gpu_fan, "风扇");
                    ui.label(egui::RichText::new(fmt(sample.gf_display, sample.gf_unit.suffix())).size(11.0).monospace().weak());
                    ui.add_space(10.0);
                    ui.checkbox(&mut self.mask.gpu_vram, "显存");
                    ui.label(egui::RichText::new(fmt(sample.gv, "%")).size(11.0).monospace().weak());
                });
            });
        });

        ui.add_space(6.0);
        egui::Frame::group(ui.style()).show(ui, |ui| {
            ui.set_width(ui.available_width());
            bottom_row(ui, |ui| {
                ui.checkbox(&mut self.mask.mem, egui::RichText::new("内存").size(12.0).strong());
                // 间距由 item_spacing.x 提供，别再叠加 add_space（会变成 16px）
                ui.label(egui::RichText::new(fmt(sample.mu, "%")).size(11.0).monospace().weak());
            });
        });

        ui.add_space(10.0);
        ui.label(egui::RichText::new("实时帧").size(11.0).weak());
        ui.add_space(3.0);
        egui::Frame::group(ui.style()).show(ui, |ui| {
            ui.set_width(ui.available_width());
            let text = if last_frame.is_empty() {
                "（尚未发送）"
            } else {
                last_frame
            };
            ui.label(egui::RichText::new(text).size(11.0).monospace());
        });

        ui.add_space(10.0);
        // ⚠ 用 columns 而不是"自己算宽度 / 3"。
        //   `Frame::group` 有固定 inner_margin(6)（每张卡左右各 6px），
        //   手算 (available - 20) / 3 时漏掉了这个内边距和 item_spacing，
        //   三张卡累计比可用宽度多出 ~32px ⇒ 最后一张（写超时）被推出窗口外。
        //   columns 由 egui 自己按可用宽度均分并扣掉间距，不会再溢出。
        ui.columns(3, |cols| {
            metric(&mut cols[0], "已发送", &sent.to_string(), None);
            metric(&mut cols[1], "实际帧率", &format!("{rate:.1}"), None);
            metric(
                &mut cols[2],
                "写超时",
                &errors.to_string(),
                Some(if errors == 0 {
                    egui::Color32::from_rgb(96, 200, 120)
                } else {
                    egui::Color32::from_rgb(230, 110, 110)
                }),
            );
        });
    }
}

/// 「标签 + 数值」一行的容器：**交叉轴底对齐**（`Align::Max`）。
///
/// ⚠⚠ 这里必须用 `allocate_ui_with_layout` 显式给"一行"的高度，
///   **不能用 `ui.with_layout()`** —— 后者会把**全部可用高度**都吃掉，
///   而 `Frame::group` 是按子 UI 的 `min_rect` 撑开的：
///   ⇒ 卡片被撑到整屏高、内容被 `Align::Max` 压到卡片最底部、
///     后面的组和卡片全被挤出窗口（2026-10-24 实测截图确认）。
///
/// 代价：这一行**不会自动换行**了（固定单行高度）。
/// 窗口很窄时宁可横向溢出，也不要布局被撑坏 —— 溢出至少看得见、能调窗口。
fn bottom_row<R>(ui: &mut egui::Ui, add: impl FnOnce(&mut egui::Ui) -> R) -> R {
    let height = ui.spacing().interact_size.y; // 标准控件行高（≈ checkbox 高度）
    ui.allocate_ui_with_layout(
        egui::vec2(ui.available_width(), height),
        egui::Layout::left_to_right(egui::Align::Max),
        add,
    )
    .inner
}

fn metric(ui: &mut egui::Ui, label: &str, value: &str, color: Option<egui::Color32>) {
    egui::Frame::group(ui.style()).show(ui, |ui| {
        // 用"本列可分到的宽度"，而不是外部传进来的固定值：
        // Frame 的 inner_margin 已经由 egui 扣过，这里填满就不会越界。
        ui.set_width(ui.available_width());
        ui.vertical(|ui| {
            ui.label(egui::RichText::new(label).size(11.0).weak());
            let mut t = egui::RichText::new(value).size(20.0).strong();
            if let Some(c) = color {
                t = t.color(c);
            }
            ui.label(t);
        });
    });
}
