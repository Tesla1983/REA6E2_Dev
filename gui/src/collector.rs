//! 数据采集：CPU/内存走 Win32，GPU 使用率走 PDH，其余指标走 LHM 的 data.json。
//!
//! 为什么 GPU 温度/风扇也走 LHM 而不是 IGCL：
//!   实测 LHM 对 Intel Arc A770 提供 `Temperatures/GPU Core` 与 `Fans/GPU Fan`，
//!   所以整个工程**不需要 IGCL 的 FFI**（Python 版那套 ctypes 可以丢掉），
//!   唯一 LHM 不给的是 GPU **核心使用率**（它只有显存占用），那个用 PDH `\GPU Engine`。

use std::io::{Read, Write};
use std::net::TcpStream;
use std::time::Duration;

use serde_json::Value;
use crate::gpu::Source;

/// LHM 的 Web Server 地址（端口与 LHM 配置里的 listenerPort 一致）。
pub const LHM_URL: &str = "127.0.0.1:8085";

/// 主板丝印 CPU_FAN1 在 LHM 里的通道名。
/// ⚠ LHM 对 NCT6687D 用通用通道名，与丝印不一定对应：
///   本机（MSI H610M BOMBER DDR4）实测 CPU_FAN1 == "System Fan #3"，
///   而 LHM 里那个叫 "CPU Fan" 的通道是**空口**（0 RPM 且 PWM 恒 0%）。
pub const CPU_FAN_NAME: &str = "System Fan #3";

#[derive(Default, Clone, Debug)]
pub struct Sample {
    pub ct: Option<i32>,
    pub cu: Option<i32>,
    pub cf: Option<i32>,
    pub gt: Option<i32>,
    pub gu: Option<i32>,
    pub gf: Option<i32>,
    pub mu: Option<i32>,
    /// 显存占用率（%）。协议 key = `GV`；固件 v24 不认这个 key（忽略），屏上暂不显示。
    pub gv: Option<i32>,
    /// 主卡功耗（W）。协议 key = `GP`（Phase 5；固件 v24 忽略）。
    pub gp: Option<i32>,
    /// 副卡（显存第二大且非软件适配器）温度/利用率/风扇（协议 key `T2`/`U2`/`F2`，Phase 5）。
    /// 只有一张卡时为 `None`。
    pub gt2: Option<i32>,
    pub gu2: Option<i32>,
    pub gf2: Option<i32>,
    /// 当前 GPU 数据来源（UI "数据来源" 标签用，Phase 4）。
    pub source: Source,
    /// 风扇值的原始单位（只影响 UI 后缀显示）
    pub cf_unit: FanUnit,
    pub gf_unit: FanUnit,
    /// 风扇的**显示值**（RPM 原值 / 百分比 0..100）。
    /// `cf/gf` 是协议值（百分比已 ×20），**不能直接拿去显示**。
    pub cf_display: Option<i32>,
    pub gf_display: Option<i32>,
}

// ---------------------------------------------------------------- 风扇单位语义

/// 风扇读数的单位。
///
/// 固件把 `CF/GF` 当 **RPM** 用：`fan_advance()` 钳 0..3000，转速 = 值/24×1.5 °/s。
/// 而现实里不是每家都给 RPM：
///   · SuperIO（主板风扇）、多数 A 卡走 ADL、部分 N 卡 AIB ⇒ 有真实 RPM；
///   · NVML 对桌面 N 卡多数只给 **百分比**（`nvmlDeviceGetFanSpeed_v2`）。
///
/// 约定（2026-10-25 定）：**有 RPM 发 RPM；只有百分比时发 `%×20`（0..2000）**，
/// 这样屏上风扇转速落在合理区间（100% ⇒ 2000，接近 3000 钳位内），
/// 同时 UI 在后面标 `%` —— 屏幕转得合理，界面也不骗人。
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum FanUnit {
    #[default]
    Rpm,
    Percent,
}

impl FanUnit {
    pub fn suffix(self) -> &'static str {
        match self {
            // ⚠ 2026-10-28 用户要求：RPM 也要显式带单位（原来返回 ""，界面上只剩裸数字）。
            FanUnit::Rpm => " RPM",
            FanUnit::Percent => "%",
        }
    }
}

/// 一条风扇读数（带单位），上线前用 `wire()` 转成协议值。
#[derive(Clone, Copy, Debug)]
pub struct FanReading {
    /// 原始读数：RPM 单位时是转/分，Percent 单位时是 0..100
    pub raw: i32,
    pub unit: FanUnit,
}

impl FanReading {
    pub fn rpm(v: f64) -> Self {
        Self { raw: v.round() as i32, unit: FanUnit::Rpm }
    }

    pub fn percent(v: f64) -> Self {
        Self { raw: v.round().clamp(0.0, 100.0) as i32, unit: FanUnit::Percent }
    }

    /// 送进链路的数值（RPM 直发；百分比 ×20）。固件再钳一次 0..3000。
    pub fn wire(&self) -> i32 {
        match self.unit {
            FanUnit::Rpm => self.raw.clamp(0, 3000),
            FanUnit::Percent => ((self.raw as f64) * 20.0).round().clamp(0.0, 2000.0) as i32,
        }
    }

    /// 给人看的数值（RPM 原值；百分比还原成 0..100）。
    /// ⚠ 和 `wire()` 不是一回事：协议值是 %×20，直接显示会变成 "260%"。
    pub fn display(&self) -> i32 {
        match self.unit {
            FanUnit::Rpm => self.raw,
            FanUnit::Percent => self.raw.clamp(0, 100),
        }
    }
}


impl Sample {
    pub fn get(&self, key: &str) -> Option<i32> {
        match key {
            "CT" => self.ct,
            "CU" => self.cu,
            "CF" => self.cf,
            "GT" => self.gt,
            "GU" => self.gu,
            "GF" => self.gf,
            "MU" => self.mu,
            "GV" => self.gv,
            "GP" => self.gp,
            "T2" => self.gt2,
            "U2" => self.gu2,
            "F2" => self.gf2,
            _ => None,
        }
    }
}

// ---------------------------------------------------------------- LHM HTTP

/// 极简 HTTP/1.1 GET：只访问 127.0.0.1，不需要 TLS，所以不用拉 reqwest/ureq。
fn http_get(host: &str, path: &str, timeout_ms: u64) -> Result<String, String> {
    let addr = host.to_string();
    let mut s = TcpStream::connect(&addr).map_err(|e| format!("connect {addr}: {e}"))?;
    s.set_read_timeout(Some(Duration::from_millis(timeout_ms))).ok();
    s.set_write_timeout(Some(Duration::from_millis(timeout_ms))).ok();

    let req = format!(
        "GET {path} HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\nUser-Agent: ra6e2-monitor\r\n\r\n"
    );
    s.write_all(req.as_bytes()).map_err(|e| format!("write: {e}"))?;

    let mut buf = Vec::with_capacity(256 * 1024);
    let mut chunk = [0u8; 16384];
    loop {
        match s.read(&mut chunk) {
            Ok(0) => break,
            Ok(n) => {
                buf.extend_from_slice(&chunk[..n]);
                if buf.len() > 8 * 1024 * 1024 {
                    break;
                }
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
            Err(e) if e.kind() == std::io::ErrorKind::TimedOut => break,
            Err(e) => return Err(format!("read: {e}")),
        }
    }

    let text = String::from_utf8_lossy(&buf).to_string();
    let body_at = text.find("\r\n\r\n").map(|i| i + 4).unwrap_or(0);
    Ok(text[body_at..].to_string())
}

/// LHM 是否在跑（端口能连上且返回 JSON）。
pub fn lhm_alive() -> bool {
    matches!(http_get(LHM_URL, "/data.json", 1500), Ok(b) if b.trim_start().starts_with('{'))
}

/// 从 LHM 拿 CPU 温度、CPU 风扇、GPU 温度、GPU 风扇。
/// 返回 (ct, cf, gt, gf)；拿不到的项为 None（**不要用 0 冒充**）。
/// 风扇带单位（`FanReading`）：LHM 的叶子值自带单位后缀 —— `1050 RPM` 或 `60 %`，
/// 这里按后缀判定，不再假设"一定是 RPM"。
///
/// `gpu_name`：Phase 4 起按选中卡的名字去匹配 LHM 里的显卡分组（不再是"第一个命中关键词"），
/// 多卡机器（iGPU + dGPU）才能正确挑中独显。`None` 退回旧的"首张 GPU"行为。
pub fn lhm_sample(gpu_name: Option<&str>) -> (Option<i32>, Option<FanReading>, Option<i32>, Option<FanReading>) {
    let text = match http_get(LHM_URL, "/data.json", 2500) {
        Ok(t) => t,
        Err(_) => return (None, None, None, None),
    };
    let root: Value = match serde_json::from_str(&text) {
        Ok(v) => v,
        Err(_) => return (None, None, None, None),
    };
    lhm_parse(&root, gpu_name)
}

/// 纯解析（把 JSON 树 → 4 个读数），**不碰网络** —— 便于用固定样本做回归测试。
fn lhm_parse(root: &Value, gpu_name: Option<&str>) -> (Option<i32>, Option<FanReading>, Option<i32>, Option<FanReading>) {
    // 摊平成 (路径, 叶子名, 值)；路径形如
    //   Sensor / <机器名> / <硬件名> / <分组> / <叶子>
    let mut leaves: Vec<(Vec<String>, String, String)> = Vec::new();
    walk(&root, &mut Vec::new(), &mut leaves);

    let want = gpu_name.map(normalize_name);
    let mut target_seen = false; // 有没有 LHM 分组真正匹配上选中的卡名

    let mut cpu_core_max: Option<f64> = None;
    let mut cpu_package: Option<f64> = None;
    // ⚠ 同名叶子会同时出现在 `Fans`（RPM）和 `Controls`（PWM %）两个分组里，
    //   实测：`Fans/System Fan #3 = 713 RPM`、`Controls/System Fan #3 = 19.0 %`。
    //   ⇒ 分开收集，最后 **RPM 优先**（PWM 只是"没有转速线/只有百分比"时的兜底）。
    let mut cpu_fan_rpm: Option<FanReading> = None;
    let mut cpu_fan_pct: Option<FanReading> = None;
    // GPU 读数分两路：目标卡（名字匹配）与兜底（首张 GPU）。
    // 名字没匹配上时退回兜底，保证单卡机器不丢失数据（零回归）。
    let mut t_gpu_temp: Option<f64> = None;
    let mut t_gpu_fan_rpm: Option<FanReading> = None;
    let mut t_gpu_fan_pct: Option<FanReading> = None;
    let mut f_gpu_temp: Option<f64> = None;
    let mut f_gpu_fan_rpm: Option<FanReading> = None;
    let mut f_gpu_fan_pct: Option<FanReading> = None;

    for (path, leaf, value) in &leaves {
        let hw = hw_name(path).to_lowercase();
        let hw_norm = normalize_name(&hw);
        let ll = leaf.to_lowercase();
        let lv = value.to_lowercase();
        let num = match first_num(value) {
            Some(n) => n,
            None => continue,
        };

        let is_superio = ["nuvoton", "winbond", "fintek", "superio", "it86", "it87", "nct"]
            .iter()
            .any(|k| hw.contains(k));
        let is_gpu = ["arc", "geforce", "radeon", "nvidia", "intel(r) arc"]
            .iter()
            .any(|k| hw.contains(k));

        // 选中卡名匹配：DXGI 名（如 "Intel(R) Arc(TM) A770 Graphics"）与 LHM 名
        // （如 "Intel Arc A770 Graphics"）去噪后双向子串匹配。
        let is_target = match &want {
            Some(w) => hw_matches(&hw_norm, w),
            None => true,
        };
        if want.is_some() && is_gpu && hw_matches(&hw_norm, want.as_ref().unwrap()) {
            target_seen = true;
        }

        if lv.contains("°c") {
            // 阈值/余量不是实测温度
            if ["warning temperature", "critical temperature", "distance to tjmax",
                "resolution", "offset"]
                .iter()
                .any(|k| ll.contains(k))
            {
                continue;
            }
            if is_gpu && ll.contains("gpu core") {
                if is_target {
                    if t_gpu_temp.is_none() {
                        t_gpu_temp = Some(num);
                    }
                } else if f_gpu_temp.is_none() {
                    f_gpu_temp = Some(num);
                }
            } else if !is_gpu && !is_superio {
                if ll.contains("core max") {
                    cpu_core_max = Some(num);
                } else if ll.contains("package") {
                    cpu_package = Some(num);
                }
            }
            continue;
        }

        // 风扇：叶子值带单位（`713 RPM` / `19.0 %`），按后缀判定单位。
        // ⚠ 必须区分 Fans(RPM) 与 Controls(PWM %)：两者叶子名相同，
        //   若不加区分，Controls 的值会覆盖掉真实的 RPM（2026-10-25 实测踩过）。
        if lv.contains("rpm") || lv.contains('%') {
            let is_rpm = lv.contains("rpm");
            let read = if is_rpm {
                FanReading::rpm(num)
            } else {
                FanReading::percent(num)
            };

            if is_gpu && ll.contains("gpu fan") {
                if is_target {
                    if is_rpm {
                        let _ = t_gpu_fan_rpm.get_or_insert(read);
                    } else {
                        let _ = t_gpu_fan_pct.get_or_insert(read);
                    }
                } else if is_rpm {
                    let _ = f_gpu_fan_rpm.get_or_insert(read);
                } else {
                    let _ = f_gpu_fan_pct.get_or_insert(read);
                }
            } else if is_superio && leaf.eq_ignore_ascii_case(CPU_FAN_NAME) {
                if is_rpm {
                    let _ = cpu_fan_rpm.get_or_insert(read);
                } else {
                    let _ = cpu_fan_pct.get_or_insert(read);
                }
            }
        }
    }

    let ct = cpu_core_max.or(cpu_package).map(|v| v.round() as i32);
    // 目标卡有值用目标卡；目标卡某字段缺失且整张卡名都没匹配上（选卡名与 LHM 对不上）
    // 时，退回首张 GPU 的兜底值，避免单卡机器因名字对不齐而丢数据。
    let gt = pick_target(t_gpu_temp, &want, target_seen, f_gpu_temp).map(|v| v.round() as i32);
    let gf_target = t_gpu_fan_rpm.or(t_gpu_fan_pct);
    let gf_fb = f_gpu_fan_rpm.or(f_gpu_fan_pct);
    let gf = if gf_target.is_none() && want.is_some() && !target_seen {
        gf_fb
    } else {
        gf_target
    };
    (ct, cpu_fan_rpm.or(cpu_fan_pct), gt, gf)
}

/// 归一化卡名：先去掉括号及其内容（DXGI 常写 `Intel(R) Arc(TM) A770`），
/// 再只保留字母数字并小写，使 `Intel(R) Arc(TM) A770 Graphics` 与
/// `Intel Arc A770 Graphics` 变成同一个串，便于双向子串比对。
fn normalize_name(s: &str) -> String {
    let mut cleaned = String::new();
    let mut depth: i32 = 0;
    for c in s.chars() {
        match c {
            '(' => depth += 1,
            ')' => {
                if depth > 0 {
                    depth -= 1;
                }
            }
            _ if depth == 0 => cleaned.push(c),
            _ => {}
        }
    }
    cleaned
        .chars()
        .filter(|c| c.is_ascii_alphanumeric())
        .map(|c| c.to_ascii_lowercase())
        .collect()
}

/// 双向子串匹配：DXGI 名与 LHM 名可能差括号/空格/后缀（"Graphics"），
/// 只要一方包含另一方即视为同一张卡。太短的名字（<6）不可靠，直接判否。
fn hw_matches(hw_norm: &str, want_norm: &str) -> bool {
    if hw_norm.len() < 6 || want_norm.len() < 6 {
        return false;
    }
    want_norm.contains(hw_norm) || hw_norm.contains(want_norm)
}

/// 目标卡字段优先；仅当"整张卡名都没匹配上"时才退回兜底（防止选卡名与 LHM 对不齐时丢数据）。
fn pick_target(target: Option<f64>, want: &Option<String>, seen: bool, fb: Option<f64>) -> Option<f64> {
    if target.is_some() {
        target
    } else if want.is_some() && !seen {
        fb
    } else {
        target
    }
}

/// 调试用（`--probe-gpu`）：把 LHM `data.json` 摊平成 (硬件名, 分组, "叶子=值")。
/// 目的：加 AMD/NVIDIA 支持前先看清"每家显卡在 LHM 里到底叫什么、有哪些叶子"。
/// LHM 没跑或超时则返回空。
pub fn lhm_dump_lines() -> Vec<(String, String, String)> {
    let text = match http_get(LHM_URL, "/data.json", 2500) {
        Ok(t) => t,
        Err(_) => return Vec::new(),
    };
    let root: Value = match serde_json::from_str(&text) {
        Ok(v) => v,
        Err(_) => return Vec::new(),
    };
    let mut leaves: Vec<(Vec<String>, String, String)> = Vec::new();
    walk(&root, &mut Vec::new(), &mut leaves);

    leaves
        .iter()
        .map(|(path, leaf, value)| {
            // 分组 = 叶子前面那一段（Temperatures / Fans / Load / Powers ...）
            let group = if path.len() >= 2 {
                path[path.len() - 2].clone()
            } else {
                String::new()
            };
            (hw_name(path), group, format!("{leaf}={value}"))
        })
        .collect()
}

fn walk(node: &Value, parents: &mut Vec<String>, out: &mut Vec<(Vec<String>, String, String)>) {    let text = node.get("Text").and_then(|v| v.as_str()).unwrap_or("").trim().to_string();
    let value = node.get("Value").and_then(|v| v.as_str()).unwrap_or("").trim().to_string();
    parents.push(text.clone());

    let children = node.get("Children").and_then(|v| v.as_array());
    match children {
        Some(ch) if !ch.is_empty() => {
            for c in ch {
                walk(c, parents, out);
            }
        }
        _ => {
            if !value.is_empty() {
                out.push((parents.clone(), text, value));
            }
        }
    }
    parents.pop();
}

/// 路径里"分组节点"（Fans/Temperatures/...）前一段就是硬件名。
fn hw_name(path: &[String]) -> String {
    const GROUPS: [&str; 12] = [
        "temperatures", "fans", "controls", "load", "data", "powers", "clocks",
        "voltages", "throughput", "level", "factors", "timings",
    ];
    for i in (1..path.len()).rev() {
        if GROUPS.contains(&path[i].trim().to_lowercase().as_str()) {
            return path[i - 1].clone();
        }
    }
    String::new()
}

fn first_num(s: &str) -> Option<f64> {
    let mut cur = String::new();
    for ch in s.chars() {
        if ch.is_ascii_digit() || ch == '.' || ch == '-' {
            cur.push(ch);
        } else if !cur.is_empty() {
            if let Ok(v) = cur.parse::<f64>() {
                return Some(v);
            }
            cur.clear();
        }
    }
    cur.parse::<f64>().ok()
}

// ---------------------------------------------------------------- Win32 直接调用
// 用裸 extern 而不是 windows crate 的封装：这些函数签名几年不变，
// 裸声明反而不会被 crate 版本升迁打乱（`windows` 每次小版本都可能改签名）。

#[repr(C)]
#[derive(Default)]
struct FileTime {
    low: u32,
    high: u32,
}

impl FileTime {
    fn u64(&self) -> u64 {
        ((self.high as u64) << 32) | self.low as u64
    }
}

#[repr(C)]
#[derive(Default)]
struct MemoryStatusEx {
    length: u32,
    memory_load: u32,
    total_phys: u64,
    avail_phys: u64,
    total_page_file: u64,
    avail_page_file: u64,
    total_virtual: u64,
    avail_virtual: u64,
    avail_extended_virtual: u64,
}

#[repr(C)]
struct CommTimeouts {
    read_interval_timeout: u32,
    read_total_timeout_multiplier: u32,
    read_total_timeout_constant: u32,
    write_total_timeout_multiplier: u32,
    write_total_timeout_constant: u32,
}

#[repr(C)]
struct ComStat {
    flags: u32,
    cb_in_que: u32,
    cb_out_que: u32,
    cb_in_que_extra: u32,
    _pad: u32,
}

#[link(name = "kernel32")]
unsafe extern "system" {
    fn GetSystemTimes(idle: *mut FileTime, kernel: *mut FileTime, user: *mut FileTime) -> i32;
    fn GlobalMemoryStatusEx(buf: *mut MemoryStatusEx) -> i32;
    fn SetCommTimeouts(handle: isize, timeouts: *const CommTimeouts) -> i32;
    fn PurgeComm(handle: isize, flags: u32) -> i32;
    fn ClearCommError(handle: isize, errors: *mut u32, stat: *mut ComStat) -> i32;
}

/// 系统级 CPU 占用（GetSystemTimes 差分）。
pub struct CpuUsage {
    prev_idle: u64,
    prev_total: u64,
}

impl CpuUsage {
    pub fn new() -> Self {
        let mut s = Self { prev_idle: 0, prev_total: 0 };
        let _ = s.sample();
        s
    }

    /// 返回 0..100。
    pub fn sample(&mut self) -> Option<i32> {
        let (mut i, mut k, mut u) = (FileTime::default(), FileTime::default(), FileTime::default());
        let ok = unsafe { GetSystemTimes(&mut i, &mut k, &mut u) };
        if ok == 0 {
            return None;
        }
        let idle = i.u64();
        let total = k.u64() + u.u64();
        if self.prev_total == 0 {
            self.prev_idle = idle;
            self.prev_total = total;
            return None;
        }
        let d_total = total.saturating_sub(self.prev_total);
        let d_idle = idle.saturating_sub(self.prev_idle);
        self.prev_idle = idle;
        self.prev_total = total;
        if d_total == 0 {
            return None;
        }
        let busy = d_total.saturating_sub(d_idle);
        Some(((busy as f64) * 100.0 / (d_total as f64)).round().clamp(0.0, 100.0) as i32)
    }
}

pub fn mem_percent() -> Option<i32> {
    let mut m = MemoryStatusEx::default();
    m.length = std::mem::size_of::<MemoryStatusEx>() as u32;
    let ok = unsafe { GlobalMemoryStatusEx(&mut m) };
    if ok == 0 { None } else { Some(m.memory_load as i32) }
}

/// 给串口句柄设置超时（只设超时，**不碰 DTR** —— 这正是避开 open 卡 30 s 的关键）。
pub fn set_comm_timeouts(handle: isize, read_ms: u32, write_ms: u32) -> bool {
    let t = CommTimeouts {
        read_interval_timeout: 0,
        read_total_timeout_multiplier: 0,
        read_total_timeout_constant: read_ms,
        write_total_timeout_multiplier: 0,
        write_total_timeout_constant: write_ms,
    };
    unsafe { SetCommTimeouts(handle, &t) != 0 }
}

pub fn purge(handle: isize) {
    const PURGE_RXCLEAR: u32 = 0x0008;
    const PURGE_TXCLEAR: u32 = 0x0004;
    unsafe {
        PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
    }
}

/// 这个句柄现在还能用吗？
///
/// 用途：本工程的串口句柄是**故意不关闭**的（CloseHandle 会卡 30 s），
/// 于是句柄会被长期持有。但设备一旦**重新枚举**（拔插、复位、重烧），
/// 旧句柄就指向一个**已经消失的设备对象**了 —— 此时写数据会失败，
/// 而对 `\\.\COM15` 的**新**打开却完全正常 ⇒ 表现为"COM15 明明好的，上位机就是连不上"。
///
/// `ClearCommError` 是开销极小的 ioctl：句柄指向的设备对象还在就返回非 0，
/// 设备已被移除 / 句柄失效则返回 0（`ERROR_ACCESS_DENIED` / `ERROR_INVALID_HANDLE`）。
pub fn comm_alive(handle: isize) -> bool {
    let mut err: u32 = 0;
    let mut stat = ComStat {
        flags: 0,
        cb_in_que: 0,
        cb_out_que: 0,
        cb_in_que_extra: 0,
        _pad: 0,
    };
    unsafe { ClearCommError(handle, &mut err, &mut stat) != 0 }
}

// ---------------------------------------------------------------- GPU 适配器枚举（DXGI）

/// 一张显卡（来自 DXGI）。
///
/// `LUID` 是跨 API 的唯一钥匙：PDH 的实例名里带 `luid_0x<hi>_0x<lo>`，
/// 厂商库（NVML / ADL）则按 PCI BDF 对卡；将来两者的翻译都靠这张表。
#[derive(Clone, Debug)]
pub struct AdapterInfo {
    pub index: u32,
    pub name: String,
    pub vendor_id: u32,
    pub device_id: u32,
    pub dedicated_vram: u64,
    pub luid_high: u32,
    pub luid_low: u32,
    /// DXGI_ADAPTER_FLAG_SOFTWARE：WARP / Basic Render Driver 之类，不是真显卡。
    pub software: bool,
}

impl AdapterInfo {
    /// PDH 实例名里的 LUID 片段。
    /// 实测形如 `pid_10540_luid_0x00000000_0x00011ce4_phys_0_eng_0_engtype_3d`
    /// ⇒ **小写、各 8 位、先 High 后 Low**。
    pub fn luid_tag(&self) -> String {
        format!("luid_0x{:08x}_0x{:08x}", self.luid_high, self.luid_low)
    }

    pub fn vendor_name(&self) -> &'static str {
        match self.vendor_id {
            0x10DE => "NVIDIA",
            0x1002 | 0x1022 => "AMD",
            0x8086 => "Intel",
            _ => "其它",
        }
    }

    pub fn vram_gib(&self) -> f64 {
        self.dedicated_vram as f64 / (1024.0 * 1024.0 * 1024.0)
    }
}

/// 枚举所有 DXGI 适配器（**含软件适配器**，用 `software` 标出，调用方自行过滤）。
pub fn enumerate_adapters() -> Vec<AdapterInfo> {
    use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1};

    let mut out = Vec::new();
    unsafe {
        let factory: IDXGIFactory1 = match CreateDXGIFactory1() {
            Ok(f) => f,
            Err(_) => return out,
        };
        let mut i = 0u32;
        // EnumAdapters1 走到头会返回 DXGI_ERROR_NOT_FOUND，以此作为结束条件。
        while let Ok(ad) = factory.EnumAdapters1(i) {
            if let Ok(d) = ad.GetDesc1() {
                let end = d.Description.iter().position(|&c| c == 0).unwrap_or(128);
                out.push(AdapterInfo {
                    index: i,
                    name: String::from_utf16_lossy(&d.Description[..end]),
                    vendor_id: d.VendorId,
                    device_id: d.DeviceId,
                    dedicated_vram: d.DedicatedVideoMemory as u64,
                    luid_high: d.AdapterLuid.HighPart as u32,
                    luid_low: d.AdapterLuid.LowPart,
                    // DXGI_ADAPTER_FLAG_SOFTWARE == 2
                    software: (d.Flags & 2) != 0,
                });
            }
            i += 1;
            if i > 32 {
                break;                       // 防御：不该有这么多
            }
        }
    }
    out
}

/// 选"主 GPU"：跳过软件适配器，取**显存最大**的那张。
/// 核显+独显、双卡机器上都按这条走；将来界面里会允许手动覆盖。
pub fn pick_primary_adapter(list: &[AdapterInfo]) -> Option<&AdapterInfo> {
    list.iter()
        .filter(|a| !a.software)
        .max_by_key(|a| a.dedicated_vram)
}

/// 取某张 DXGI 适配器的**专用显存规格**（字节），用作占用率的分母。
pub fn adapter_dedicated_vram(adapter_index: u32) -> Option<u64> {
    use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1};
    unsafe {
        let factory: IDXGIFactory1 = CreateDXGIFactory1().ok()?;
        let ad = factory.EnumAdapters1(adapter_index).ok()?;
        let desc = ad.GetDesc1().ok()?;
        Some(desc.DedicatedVideoMemory as u64)
    }
}

/// 整机显存占用（字节）：PDH `\GPU Adapter Memory(*)\Dedicated Usage`。
///
/// ⚠ **别用 DXGI `QueryVideoMemoryInfo`**：它返回的是**本进程**的预算/用量（实测监控程序
/// 恒为 0%），不是整机占用。PDH 这个计数器才是任务管理器同源的口径，且厂商无关。
/// 实例名形如 `luid_0x00000000_0x00011ce4_phys_0`，按 LUID 片段挑目标卡。
pub struct GpuVram {
    query: isize,
    counter: isize,
    luid_tag: String,
}

impl GpuVram {
    pub fn new(luid_tag: &str) -> Option<Self> {
        let mut query: isize = 0;
        let mut counter: isize = 0;
        let path = wide(r"\GPU Adapter Memory(*)\Dedicated Usage");
        unsafe {
            if PdhOpenQueryW(std::ptr::null(), 0, &mut query) != 0 {
                return None;
            }
            if PdhAddEnglishCounterW(query, path.as_ptr(), 0, &mut counter) != 0 {
                return None;
            }
            let _ = PdhCollectQueryData(query);
        }
        Some(Self {
            query,
            counter,
            luid_tag: luid_tag.to_string(),
        })
    }

    /// 当前专用显存占用（字节）。这是即时值（rawcount），不需要两次采集。
    pub fn used_bytes(&mut self) -> Option<u64> {
        unsafe {
            if PdhCollectQueryData(self.query) != 0 {
                return None;
            }
            let mut size: u32 = 0;
            let mut count: u32 = 0;
            let mut rc = PdhGetFormattedCounterArrayW(
                self.counter, PDH_FMT_DOUBLE, &mut size, &mut count, std::ptr::null_mut(),
            );
            if rc != PDH_MORE_DATA || size == 0 {
                return None;
            }
            let mut buf = vec![0u8; size as usize];
            rc = PdhGetFormattedCounterArrayW(
                self.counter,
                PDH_FMT_DOUBLE,
                &mut size,
                &mut count,
                buf.as_mut_ptr() as *mut PdhItem,
            );
            if rc != 0 {
                return None;
            }
            let items = std::slice::from_raw_parts(buf.as_ptr() as *const PdhItem, count as usize);
            for it in items {
                if it.name.is_null() {
                    continue;
                }
                let name = pcwstr_to_string(it.name).to_ascii_lowercase();
                if name.contains(self.luid_tag.as_str()) {
                    let v = it.value.double_value;
                    if v >= 0.0 {
                        return Some(v as u64);
                    }
                }
            }
            None
        }
    }
}

// ---------------------------------------------------------------- PDH: GPU 使用率

#[repr(C)]
struct PdhValue {
    status: u32,
    _pad: u32,
    double_value: f64,
}

#[repr(C)]
struct PdhItem {
    name: *mut u16,
    value: PdhValue,
}

#[link(name = "pdh")]
unsafe extern "system" {
    fn PdhOpenQueryW(datasource: *const u16, userdata: usize, query: *mut isize) -> u32;
    fn PdhAddEnglishCounterW(query: isize, path: *const u16, userdata: usize, counter: *mut isize) -> u32;
    fn PdhCollectQueryData(query: isize) -> u32;
    fn PdhGetFormattedCounterArrayW(
        counter: isize,
        format: u32,
        buf_size: *mut u32,
        item_count: *mut u32,
        item_buf: *mut PdhItem,
    ) -> u32;
}

const PDH_FMT_DOUBLE: u32 = 0x0000_0200;
const PDH_MORE_DATA: u32 = 0x8000_07D2;

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

/// GPU 核心使用率（PDH `\GPU Engine`）。
///
/// 口径与任务管理器一致：同一 engtype 下跨进程求和，再在各 engtype 之间取 max。
/// 只统计 **engtype_3D**（3D 渲染引擎），这是"GPU 占用"最接近的近似。
///
/// ⚠ 2026-10-25 修：原来**不按适配器过滤**，把所有卡的 `engtype_3D` 实例一起相加 ——
/// 核显+独显（笔记本 / AM5 带 iGPU）会虚高，双卡机器更是错的。
/// 现在按 LUID 过滤到目标卡（`luid_tag`）；`None` 才退回旧的"全机相加"行为。
pub struct PdhGpuState {
    query: isize,
    counter: isize,
    luid_tag: Option<String>,
    /// 上一次采样：命中的实例数 / 全部实例数（诊断用，`--probe-gpu` 会打印）。
    last_matched: u32,
    last_total: u32,
    /// 这张卡的名字（来自 DXGI），用于界面/日志。
    #[allow(dead_code)] // 字段经 facade(GpuUsage::adapter_name) 暴露，旧直接读取路径已迁走
    adapter_name: Option<String>,
    /// 这张卡在 DXGI 里的序号（查显存占用要用它重新取 adapter）。
    adapter_index: Option<u32>,
    /// 显存占用：分母 = 卡的专用显存规格，分子 = PDH 整机占用。
    vram_total: u64,
    vram: Option<GpuVram>,
}

#[allow(dead_code)] // new()/adapter_name()/vram_percent() 等由 GpuUsage 门面暴露，旧直接路径已迁走
impl PdhGpuState {
    /// 自动选主 GPU（显存最大的非软件适配器）。
    pub fn new() -> Option<Self> {
        let adapters = enumerate_adapters();
        match pick_primary_adapter(&adapters) {
            Some(a) => {
                let tag = a.luid_tag();
                let name = a.name.clone();
                let mut g = Self::open(Some(tag))?;
                g.adapter_name = Some(name);
                Some(g.with_index(a.index))
            }
            None => Self::open(None),
        }
    }

    /// 指定某张卡（按 LUID 的高/低 32 位）。
    pub fn new_for_luid(luid_high: u32, luid_low: u32) -> Option<Self> {
        Self::open(Some(format!("luid_0x{luid_high:08x}_0x{luid_low:08x}")))
    }

    fn open(luid_tag: Option<String>) -> Option<Self> {
        let mut query: isize = 0;
        let mut counter: isize = 0;
        let path = wide(r"\GPU Engine(*)\Utilization Percentage");
        unsafe {
            if PdhOpenQueryW(std::ptr::null(), 0, &mut query) != 0 {
                return None;
            }
            if PdhAddEnglishCounterW(query, path.as_ptr(), 0, &mut counter) != 0 {
                return None;
            }
            // 第一次采集只是建立基线，值不可用
            let _ = PdhCollectQueryData(query);
        }
        Some(Self {
            query,
            counter,
            luid_tag,
            last_matched: 0,
            last_total: 0,
            adapter_name: None,
            adapter_index: None,
            vram_total: 0,
            vram: None,
        })
    }

    /// 这张卡的名字（DXGI 描述），拿不到为 None。
    pub fn adapter_name(&self) -> Option<&str> {
        self.adapter_name.as_deref()
    }

    /// 绑定 DXGI 序号（探针按"逐卡"创建时用），顺带把显存占用的查询建好。
    pub fn with_index(mut self, index: u32) -> Self {
        self.adapter_index = Some(index);
        self.vram_total = adapter_dedicated_vram(index).unwrap_or(0);
        if let Some(tag) = self.luid_tag.clone() {
            self.vram = GpuVram::new(&tag);
        }
        self
    }

    /// 显存占用率（%，0..100）。需要先绑定 DXGI 序号（`new()` / `with_index()` 会给）。
    pub fn vram_percent(&mut self) -> Option<i32> {
        let total = self.vram_total;
        if total == 0 {
            return None;
        }
        let used = self.vram.as_mut()?.used_bytes()?;
        let pct = (used as f64) * 100.0 / (total as f64);
        Some(pct.round().clamp(0.0, 100.0) as i32)
    }

    /// (命中实例数, 全部实例数)。用于诊断"过滤条件是否真的匹配上了"。
    pub fn last_stats(&self) -> (u32, u32) {
        (self.last_matched, self.last_total)
    }

    pub fn sample(&mut self) -> Option<i32> {
        unsafe {
            if PdhCollectQueryData(self.query) != 0 {
                return None;
            }

            let mut size: u32 = 0;
            let mut count: u32 = 0;
            let mut rc = PdhGetFormattedCounterArrayW(
                self.counter, PDH_FMT_DOUBLE, &mut size, &mut count, std::ptr::null_mut(),
            );
            if rc != PDH_MORE_DATA || size == 0 {
                return None;
            }

            let mut buf = vec![0u8; size as usize];
            rc = PdhGetFormattedCounterArrayW(
                self.counter,
                PDH_FMT_DOUBLE,
                &mut size,
                &mut count,
                buf.as_mut_ptr() as *mut PdhItem,
            );
            if rc != 0 {
                return None;
            }

            let items = std::slice::from_raw_parts(buf.as_ptr() as *const PdhItem, count as usize);
            let mut total_3d = 0.0f64;
            let mut seen_3d = false;
            let mut matched = 0u32;
            let mut total_inst = 0u32;

            for it in items {
                if it.name.is_null() {
                    continue;
                }
                // 实例名形如
                //   pid_1234_luid_0x00000000_0x00011ce4_phys_0_eng_0_engtype_3d
                let name = pcwstr_to_string(it.name);
                let lower = name.to_ascii_lowercase();
                total_inst += 1;

                // 关键修复：只算目标卡。LUID 片段本身已经含 3D 之外的信息，
                // 所以先过 LUID，再过 engtype。
                if let Some(tag) = &self.luid_tag {
                    if !lower.contains(tag.as_str()) {
                        continue;
                    }
                }
                if !lower.contains("engtype_3d") {
                    continue;
                }
                matched += 1;
                seen_3d = true;
                total_3d += it.value.double_value.max(0.0);
            }

            self.last_matched = matched;
            self.last_total = total_inst;

            // 卡在但是没有任何 3D 引擎实例 ⇒ 这张卡真的闲着，报 0 比 `--` 更准。
            if !seen_3d && self.luid_tag.is_some() && total_inst > 0 {
                return Some(0);
            }
            if !seen_3d {
                return None;
            }
            Some(total_3d.round().clamp(0.0, 100.0) as i32)
        }
    }

    /// 专用显存（字节）：(已用, 总)。供 `LhmPdhBackend` 转 MB 用。
    pub fn mem_bytes(&mut self) -> Option<(u64, u64)> {
        let total = self.vram_total;
        if total == 0 {
            return None;
        }
        let used = self.vram.as_mut()?.used_bytes()?;
        Some((used, total))
    }
}

unsafe fn pcwstr_to_string(p: *const u16) -> String {
    let mut len = 0usize;
    // SAFETY: 调用方保证 p 指向一个以 NUL 结尾的宽字符缓冲（PDH 分配的实例名）。
    unsafe {
        while *p.add(len) != 0 {
            len += 1;
            if len > 512 {
                break;
            }
        }
        String::from_utf16_lossy(std::slice::from_raw_parts(p, len))
    }
}

/// 采集一帧完整数据。拿不到的项保持 None。
///
/// GPU 部分（温度/利用率/风扇/显存）统一走 [`crate::gpu::GpuUsage`] 门面
/// （内部经 `Resolver` 合并各后端），每帧只驱动一次采样，避免重复 LHM 取数。
pub fn collect(cpu: &mut CpuUsage, gpu: &mut Option<crate::gpu::GpuUsage>) -> Sample {
    // CPU 温度/风扇走 LHM；GPU 部分交给门面（一次 read_full）。
    let (ct, cf, _gt, _gf) = lhm_sample(None);
    let gs = gpu.as_mut().and_then(|g| g.read_full());
    let (gu, gv) = match &gs {
        Some(s) => (s.util_pct, s.mem_pct()),
        None => (None, None),
    };
    let gf_reading = gs.as_ref().and_then(|s| s.fan);
    let gp = gs.as_ref().and_then(|s| s.power_w).map(|w| w.round() as i32);
    let source = gs.as_ref().map(|s| s.source).unwrap_or(Source::LhmPdh);

    // Phase 5：副卡（显存第二大且非软件）的温度/利用率/风扇。
    let g2 = {
        let adapters = gpu.as_ref().map(|g| g.enumerate()).unwrap_or_default();
        let mut sorted: Vec<_> = adapters.iter().filter(|k| !k.software).cloned().collect();
        sorted.sort_by_key(|k| std::cmp::Reverse(k.vram_bytes));
        sorted
            .get(1)
            .and_then(|k| gpu.as_mut().and_then(|g| g.read_key(k)))
    };
    let gt2 = g2.as_ref().and_then(|s| s.temp_c);
    let gu2 = g2.as_ref().and_then(|s| s.util_pct);
    let gf2 = g2.as_ref().and_then(|s| s.fan).map(|f| f.wire());

    Sample {
        ct,
        cu: cpu.sample(),
        // 风扇按单位换算成协议值：RPM 直发，百分比 ×20（见 FanUnit 的说明）
        cf: cf.map(|f| f.wire()),
        gt: gs.as_ref().and_then(|s| s.temp_c),
        gu,
        gf: gf_reading.map(|f| f.wire()),
        mu: mem_percent(),
        gv,
        gp,
        gt2,
        gu2,
        gf2,
        source,
        cf_unit: cf.map(|f| f.unit).unwrap_or(FanUnit::Rpm),
        gf_unit: gf_reading.map(|f| f.unit).unwrap_or(FanUnit::Rpm),
        cf_display: cf.map(|f| f.display()),
        gf_display: gf_reading.map(|f| f.display()),
    }
}

/// 兼容门面：对外仍是 `collector::GpuUsage`，但实现委托给 `gpu::Resolver`
/// （多后端合并；本机只有 LHM/PDH 后端，行为不变）。app.rs / main.rs 零改动。
pub use crate::gpu::GpuUsage;

#[cfg(test)]
mod gpu_tests {
    use super::*;

    /// 验证两件事（2026-10-25 修 PDH 跨卡求和的回归测试）：
    ///   1. DXGI 能枚举到适配器，主 GPU 判定 = 显存最大的非软件卡；
    ///   2. PDH `\GPU Engine` 按该卡 LUID 过滤后**必须命中实例**（命中 0 就说明
    ///      LUID 片段格式写错了，那时数值会永远 0/虚高）。
    #[test]
    fn dxgi_enumeration_and_pdh_luid_filter() {
        let adapters = enumerate_adapters();
        assert!(!adapters.is_empty(), "DXGI 至少应枚举到一张适配器");

        for a in &adapters {
            println!(
                "[{}] {:<42} vendor={:#06x} vram={:.1}GiB luid={} software={}",
                a.index, a.name, a.vendor_id, a.vram_gib(), a.luid_tag(), a.software
            );
        }

        let p = pick_primary_adapter(&adapters).expect("应存在非软件适配器");
        println!("primary = {} ({})", p.name, p.vendor_name());

        let mut g = PdhGpuState::new_for_luid(p.luid_high, p.luid_low).expect("PDH 查询应能打开");
        // 速率计数器要两次采集（间隔 ≥200ms）才有值
        std::thread::sleep(std::time::Duration::from_millis(400));
        let v = g.sample();
        let (matched, total) = g.last_stats();
        println!("sample={v:?}  matched={matched}/{total}");

        assert!(total > 0, "PDH 应有 GPU Engine(*)\\Utilization 实例");
        assert!(matched > 0, "按 LUID 过滤后必须命中实例（否则 LUID 片段格式不对）");

        // 显存占用率（PDH \GPU Adapter Memory）：本机有真显卡就该有值
        let mut gv = PdhGpuState::new_for_luid(p.luid_high, p.luid_low)
            .expect("PDH 打开")
            .with_index(p.index);
        let vram = gv.vram_percent().expect("显存占用应能取到");
        println!("vram_percent = {vram}%");
        assert!((0..=100).contains(&vram), "显存占用率必须落在 0..100");
        if let Some(x) = v {
            assert!((0..=100).contains(&x), "利用率必须落在 0..100");
        }
    }

    /// 回归：LHM 里 `Fans/System Fan #3 = 713 RPM` 与 `Controls/System Fan #3 = 19.0 %`
    /// 是同名叶子，必须取 **RPM**（2026-10-25 现场看到 260% 就是这个覆盖导致的）。
    #[test]
    fn lhm_prefers_fan_rpm_over_pwm_controls() {
        let json = r#"{
          "Text": "Sensor",
          "Children": [{
            "Text": "PC",
            "Children": [{
              "Text": "MSI H610M BOMBER DDR4 (MS-7D46)",
              "Children": [{
                "Text": "Nuvoton NCT6687D",
                "Children": [
                  {"Text": "Fans", "Children": [
                    {"Text": "System Fan #3", "Value": "713 RPM"},
                    {"Text": "System Fan #5", "Value": "876 RPM"}
                  ]},
                  {"Text": "Controls", "Children": [
                    {"Text": "System Fan #3", "Value": "19.0 %"},
                    {"Text": "System Fan #5", "Value": "56.0 %"}
                  ]}
                ]
              }]
            }]
          }]
        }"#;
        let root: Value = serde_json::from_str(json).unwrap();
        let (_ct, cf, _gt, _gf) = lhm_parse(&root, None);
        let cf = cf.expect("应取到 CPU 风扇");
        assert_eq!(cf.unit, FanUnit::Rpm, "必须优先 RPM，不能被 Controls 的百分比覆盖");
        assert_eq!(cf.raw, 713);
        assert_eq!(cf.wire(), 713, "RPM 直发");
        assert_eq!(cf.display(), 713);
    }

    /// 只有百分比时（NVIDIA 桌面卡的常态）：发 %×20，但显示仍是百分比本身。
    #[test]
    fn lhm_percent_only_fan_is_scaled_for_wire_but_shown_as_percent() {
        let json = r#"{
          "Text": "Sensor", "Children": [{"Text": "PC", "Children": [{
            "Text": "NVIDIA GeForce RTX 4070", "Children": [
              {"Text": "Temperatures", "Children": [{"Text": "GPU Core", "Value": "62.0 °C"}]},
              {"Text": "Fans", "Children": [{"Text": "GPU Fan", "Value": "60.0 %"}]}
            ]
          }]}]
        }"#;
        let root: Value = serde_json::from_str(json).unwrap();
        let (_ct, _cf, gt, gf) = lhm_parse(&root, None);
        assert_eq!(gt, Some(62));
        let gf = gf.expect("应取到 GPU 风扇");
        assert_eq!(gf.unit, FanUnit::Percent);
        assert_eq!(gf.wire(), 1200, "60% × 20");
        assert_eq!(gf.display(), 60, "界面必须显示 60 而不是 1200 或 1200%");
    }

    /// Phase 4 核心回归：多卡时按"选中卡的名字"挑 LHM 分组，而不是"第一个命中关键词"。
    /// 这里放两张卡：NVIDIA(70°C, 在前) + Intel Arc(53°C, 在后)。
    ///   - 不过滤 → 取第一张（NVIDIA, 70）；
    ///   - 过滤 "Intel(R) Arc(TM) A770 Graphics" → 应命中第二张（Arc, 53），
    ///     且能跨括号/空格/Graphics 后缀归一化匹配。
    #[test]
    fn lhm_parse_selects_named_gpu_among_multiple() {
        let json = r#"{
          "Text": "Sensor", "Children": [{"Text": "PC", "Children": [
            {"Text": "NVIDIA GeForce RTX 4070", "Children": [
              {"Text": "Temperatures", "Children": [{"Text": "GPU Core", "Value": "70.0 °C"}]},
              {"Text": "Fans", "Children": [{"Text": "GPU Fan", "Value": "1500 RPM"}]}
            ]},
            {"Text": "Intel Arc A770 Graphics", "Children": [
              {"Text": "Temperatures", "Children": [{"Text": "GPU Core", "Value": "53.0 °C"}]},
              {"Text": "Fans", "Children": [{"Text": "GPU Fan", "Value": "800 RPM"}]}
            ]}
          ]}]
        }"#;
        let root: Value = serde_json::from_str(json).unwrap();

        // 不过滤：取第一张 = NVIDIA 70°C
        let (_ct, _cf, gt, _gf) = lhm_parse(&root, None);
        assert_eq!(gt, Some(70), "无过滤应取第一张 GPU（NVIDIA）");

        // 按名过滤到 Arc（名字带括号/空格差异）→ 53°C，证明挑中了第二张而非第一张
        let (_ct, _cf, gt, gf) = lhm_parse(&root, Some("Intel(R) Arc(TM) A770 Graphics"));
        assert_eq!(gt, Some(53), "应跨括号/空格归一化匹配到 Intel Arc");
        let gf = gf.expect("Arc 风扇应取到");
        assert_eq!(gf.raw, 800, "风扇也应来自 Arc 而非 NVIDIA");
    }

    /// 现场验证（需要 LHM 在跑，默认 ignore）：
    ///   cargo test --release -- --ignored live_lhm --nocapture
    /// 本机（MSI H610M / NCT6687D）CPU 风扇真值在 `Fans/System Fan #3`，是 RPM。
    #[test]
    #[ignore]
    fn live_lhm_sanity() {
        assert!(lhm_alive(), "LHM 没在跑，先启动上位机");
        let (ct, cf, gt, gf) = lhm_sample(None);
        println!("live: ct={ct:?} cf={cf:?} gt={gt:?} gf={gf:?}");
        if let Some(f) = cf {
            println!("  CPU 风扇: raw={} unit={:?} wire={} display={}",
                     f.raw, f.unit, f.wire(), f.display());
            assert_eq!(f.unit, FanUnit::Rpm, "本机 CPU 风扇应为 RPM（PWM 在 Controls 分组）");
            assert!(f.raw >= 100, "RPM 不该是 PWM 那种 0..100 的数（本机实测 600~900）");
        }
        if let Some(f) = gf {
            println!("  GPU 风扇: raw={} unit={:?} wire={} display={}",
                     f.raw, f.unit, f.wire(), f.display());
        }
    }

    /// 风扇单位约定：RPM 直发，百分比 ×20。
    #[test]
    fn fan_wire_conversion() {
        assert_eq!(FanReading::rpm(1050.0).wire(), 1050);
        assert_eq!(FanReading::rpm(4000.0).wire(), 3000);      // 钳到固件上限
        assert_eq!(FanReading::percent(60.0).wire(), 1200);    // 60% × 20
        assert_eq!(FanReading::percent(100.0).wire(), 2000);
        assert_eq!(FanReading::percent(150.0).wire(), 2000);   // 越界钳位
        assert_eq!(FanUnit::Percent.suffix(), "%");
        // 2026-10-28：RPM 也带单位（原来返回 ""，界面上只有裸数字）
        assert_eq!(FanUnit::Rpm.suffix(), " RPM");
    }
}
