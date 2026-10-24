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
fn lhm_sample() -> (Option<i32>, Option<i32>, Option<i32>, Option<i32>) {
    let text = match http_get(LHM_URL, "/data.json", 2500) {
        Ok(t) => t,
        Err(_) => return (None, None, None, None),
    };
    let root: Value = match serde_json::from_str(&text) {
        Ok(v) => v,
        Err(_) => return (None, None, None, None),
    };

    // 摊平成 (路径, 叶子名, 值)；路径形如
    //   Sensor / <机器名> / <硬件名> / <分组> / <叶子>
    let mut leaves: Vec<(Vec<String>, String, String)> = Vec::new();
    walk(&root, &mut Vec::new(), &mut leaves);

    let mut cpu_core_max: Option<f64> = None;
    let mut cpu_package: Option<f64> = None;
    let mut cpu_fan: Option<i32> = None;
    let mut gpu_temp: Option<f64> = None;
    let mut gpu_fan: Option<i32> = None;

    for (path, leaf, value) in &leaves {
        let hw = hw_name(path).to_lowercase();
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

        if lv.contains("°c") {
            // 阈值/余量不是实测温度
            if ["warning temperature", "critical temperature", "distance to tjmax",
                "resolution", "offset"]
                .iter()
                .any(|k| ll.contains(k))
            {
                continue;
            }
            if is_gpu && ll.contains("gpu core") && gpu_temp.is_none() {
                gpu_temp = Some(num);
            } else if !is_gpu && !is_superio {
                if ll.contains("core max") {
                    cpu_core_max = Some(num);
                } else if ll.contains("package") {
                    cpu_package = Some(num);
                }
            }
            continue;
        }

        if lv.contains("rpm") {
            if is_gpu && ll.contains("gpu fan") && gpu_fan.is_none() {
                gpu_fan = Some(num.round() as i32);
            } else if is_superio && leaf.eq_ignore_ascii_case(CPU_FAN_NAME) {
                cpu_fan = Some(num.round() as i32);
            }
        }
    }

    let ct = cpu_core_max.or(cpu_package).map(|v| v.round() as i32);
    let gt = gpu_temp.map(|v| v.round() as i32);
    (ct, cpu_fan, gt, gpu_fan)
}

fn walk(node: &Value, parents: &mut Vec<String>, out: &mut Vec<(Vec<String>, String, String)>) {
    let text = node.get("Text").and_then(|v| v.as_str()).unwrap_or("").trim().to_string();
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
pub struct GpuUsage {
    query: isize,
    counter: isize,
}

impl GpuUsage {
    pub fn new() -> Option<Self> {
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
        Some(Self { query, counter })
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

            for it in items {
                if it.name.is_null() {
                    continue;
                }
                // 实例名形如
                //   pid_1234_luid_0x00000000_0x00014890_phys_0_eng_0_engtype_3D
                let name = pcwstr_to_string(it.name);
                if !name.to_ascii_lowercase().contains("engtype_3d") {
                    continue;
                }
                seen_3d = true;
                total_3d += it.value.double_value.max(0.0);
            }

            if !seen_3d {
                return None;
            }
            Some(total_3d.round().clamp(0.0, 100.0) as i32)
        }
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
pub fn collect(cpu: &mut CpuUsage, gpu: &mut Option<GpuUsage>) -> Sample {
    let (ct, cf, gt, gf) = lhm_sample();
    let gu = gpu.as_mut().and_then(|g| g.sample());
    Sample {
        ct,
        cu: cpu.sample(),
        cf,
        gt,
        gu,
        gf,
        mu: mem_percent(),
    }
}
