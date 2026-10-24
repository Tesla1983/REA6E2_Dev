//! 板子识别与串口链路。
//!
//! 两条硬约束（都是本项目实测踩出来的）：
//!   1. **不能用 COM 号当身份**：COM 号每次枚举/插拔都会变（实测从 COM4 变到 COM15）。
//!      而且 USB 描述符里的 iSerialNumber 写死成 "0000000000001"，所有板一样、不唯一。
//!      ⇒ 唯一可靠的标识是**芯片出厂 UID**，由固件的 `$ID?` 查询返回。
//!   2. **打开串口不能走"完整 open"**：pyserial / 常规 DCB 初始化会在 DTR 上卡 30 s
//!      （usbser 等 SET_CONTROL_LINE_STATE 的状态阶段）。
//!      ⇒ 这里用 `std::fs` 直接打开 `\\.\COMx` 拿到句柄，只设超时、**不碰任何调制解调器线**，
//!      所以 open 是瞬时的。

use std::fs::{File, OpenOptions};
use std::io::{Read, Write};
use std::os::windows::io::AsRawHandle;
use std::time::{Duration, Instant};

use crate::collector::{purge, set_comm_timeouts};

/// 本项目的 USB VID:PID。
pub const RA6E2_VID: u16 = 0x045B;
pub const RA6E2_PID: u16 = 0x0263;

#[derive(Clone, Debug)]
pub struct Board {
    pub port: String,
    /// 固件 `$ID?` 返回的完整 ID，例如 `RA6E2-43045A764E4B2EB4`；老固件为 None。
    pub id: Option<String>,
    pub fw: Option<u32>,
    /// 这条信息来自**磁盘缓存**而不是本次实测（界面会打「缓存」标记）。
    pub cached: bool,
    /// 该端口**此刻**是否真的在系统串口列表里。
    pub online: bool,
}

impl Board {
    /// 界面上显示的短 ID（取末尾 8 位十六进制）。
    pub fn short_id(&self) -> String {
        match &self.id {
            Some(s) => match s.rsplit('-').next() {
                Some(hex) if hex.len() >= 8 => hex[hex.len() - 8..].to_string(),
                _ => s.clone(),
            },
            None => "未识别".to_string(),
        }
    }
}

// ---------------------------------------------------------------- 原始串口

pub struct RawSerial {
    file: File,
}

impl RawSerial {
    /// 打开 `COM15` / `\\.\COM15`。**不设 DTR/RTS、不调用 EscapeCommFunction**。
    pub fn open(port: &str) -> Result<Self, String> {
        let path = if port.starts_with(r"\\.\") || port.starts_with(r"\\?\") {
            port.to_string()
        } else {
            format!(r"\\.\{port}")
        };

        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .open(&path)
            .map_err(|e| format!("打开 {path} 失败: {e}"))?;

        let handle = file.as_raw_handle() as isize;
        if !set_comm_timeouts(handle, 200, 1000) {
            return Err(format!("{port}: SetCommTimeouts 失败"));
        }
        purge(handle);

        Ok(Self { file })
    }

    pub fn write_all(&mut self, data: &[u8]) -> Result<(), String> {
        self.file
            .write_all(data)
            .map_err(|e| format!("写 {data:?} 失败: {e}"))?;
        self.file.flush().map_err(|e| format!("flush 失败: {e}"))
    }

    /// 句柄背后的设备对象是否还在（见 `collector::comm_alive` 的说明）。
    pub fn alive(&self) -> bool {
        crate::collector::comm_alive(self.file.as_raw_handle() as isize)
    }

    /// 读一批字节；超时返回 Ok(0)（已通过 COMMTIMEOUTS 设定，不会永久阻塞）。
    pub fn read_some(&mut self, buf: &mut [u8]) -> Result<usize, String> {
        match self.file.read(buf) {
            Ok(n) => Ok(n),
            Err(e) if e.kind() == std::io::ErrorKind::TimedOut => Ok(0),
            Err(e) => Err(format!("读失败: {e}")),
        }
    }
}

// ---------------------------------------------------------------- 帧协议

/// XOR 校验：`$` 与 `*` 之间所有字符的异或（与固件 `parse_line()` 一致）。
pub fn xor_checksum(payload: &str) -> u8 {
    payload.bytes().fold(0u8, |a, b| a ^ b)
}

/// 组一帧：`$<payload>*XX\r\n`
pub fn frame(payload: &str) -> Vec<u8> {
    format!("${}*{:02X}\r\n", payload, xor_checksum(payload)).into_bytes()
}

/// 身份查询帧（固件 v14+ 支持）。
pub fn id_query_frame() -> Vec<u8> {
    frame("ID?")
}

/// 按勾选结果把 Sample 拼成 payload。**没勾的项整项省略**——
/// 固件按 key 解析，缺项会显示 `--`，这正是我们要的语义。
pub fn build_payload(s: &crate::collector::Sample, mask: &[(bool, &str)]) -> String {
    let mut parts: Vec<String> = Vec::new();
    for (on, key) in mask {
        if !*on {
            continue;
        }
        if let Some(v) = s.get(key) {
            parts.push(format!("{key}:{v}"));
        }
    }
    parts.join(",")
}

// ---------------------------------------------------------------- 发现与识别

/// 枚举所有 RA6E2 的候选串口（按 VID:PID 过滤，不认 COM 号）。
pub fn candidate_ports() -> Vec<String> {
    let mut out = Vec::new();
    if let Ok(list) = serialport::available_ports() {
        for p in list {
            if let serialport::SerialPortType::UsbPort(usb) = p.port_type {
                if usb.vid == RA6E2_VID && usb.pid == RA6E2_PID {
                    out.push(p.port_name);
                }
            }
        }
    }
    out
}

// ---------------------------------------------------------------- 打开串口

/// 打开串口，带**重试**。
///
/// 历史（重要）：v15 之前这块板的 `CloseHandle` 会卡 30 s（usbser 关闭时拉低 DTR，
/// 触发 0x22 SET_CONTROL_LINE_STATE，而该请求的状态阶段永远发不出去）。
/// 为了绕开它，这里曾经做过"句柄永不关闭"的单槽复用 —— 结果引出了更麻烦的问题：
/// 设备重新枚举后缓存句柄指向已消失的设备对象，表现为
/// "设备管理器里 COM15 好好的，但上位机就是连不上"。
///
/// **固件 v16 已修掉根因**（无数据类请求补上 FSP 漏掉的 `PID=BUF`，见
/// `src/usb/bsp_usb_cdc.c` 的 `usb_pipe0_nodata_arm()`）：现在
/// `SETDTR` / `CloseHandle` 都是 **2 ms** 级，句柄可以正常关闭了。
/// ⇒ 因此这里恢复成"每次打开、用完就关"的最简单模型，不再有常驻与泄漏。
///
/// 唯一保留的技巧是**重试**：刚关掉的端口立刻再打开，偶发 `ERROR_GEN_FAILURE (31)`，
/// 隔 150 ms 重试即可（实测第 1 次失败、第 2 次成功）。
const OPEN_TRIES: u32 = 3;

pub fn acquire(port: &str) -> Result<RawSerial, String> {
    let mut last = String::new();

    for i in 0..OPEN_TRIES {
        match RawSerial::open(port) {
            Ok(ser) => return Ok(ser),
            Err(e) => {
                last = e;
                if i + 1 < OPEN_TRIES {
                    std::thread::sleep(Duration::from_millis(150));
                }
            }
        }
    }
    Err(last)
}

/// 用完关掉。**现在关闭是瞬时的**，不需要任何特殊处理（见上）。
/// 保留这个函数只是为了调用点读起来对称。
pub fn release(_port: &str, ser: RawSerial) {
    drop(ser);
}

// ---------------------------------------------------------------- 板子信息缓存

/// 探测等待时间。固件回 `$ID?` 只有几十微秒，400 ms 足够且几乎不影响体感。
const PROBE_TIMEOUT: Duration = Duration::from_millis(400);

#[derive(Clone, Debug)]
pub struct CacheEntry {
    pub id: String,
    pub port: String,
    pub fw: Option<u32>,
}

fn cache_file() -> Option<std::path::PathBuf> {
    let base = std::env::var_os("LOCALAPPDATA")?;
    let mut p = std::path::PathBuf::from(base);
    p.push("Ra6e2Monitor");
    let _ = std::fs::create_dir_all(&p);
    p.push("boards.json");
    Some(p)
}

/// 读缓存。文件不存在 / 坏掉都只是返回空，不影响功能。
pub fn cache_entries() -> Vec<CacheEntry> {
    let Some(path) = cache_file() else {
        return Vec::new();
    };
    let Ok(txt) = std::fs::read_to_string(&path) else {
        return Vec::new();
    };
    let Ok(v) = serde_json::from_str::<serde_json::Value>(&txt) else {
        return Vec::new();
    };

    let mut out = Vec::new();
    if let Some(map) = v.get("boards").and_then(|b| b.as_object()) {
        for (id, e) in map {
            let port = e
                .get("port")
                .and_then(|p| p.as_str())
                .unwrap_or("")
                .to_string();
            if port.is_empty() {
                continue;
            }
            let fw = e.get("fw").and_then(|f| f.as_u64()).map(|f| f as u32);
            out.push(CacheEntry {
                id: id.clone(),
                port,
                fw,
            });
        }
    }
    out
}

/// 记住一块板子（按芯片 UID 索引，**不用 COM 号当 key**）。
pub fn cache_store(b: &Board) {
    let Some(id) = b.id.as_deref() else {
        return;
    };
    let Some(path) = cache_file() else {
        return;
    };

    let mut root = std::fs::read_to_string(&path)
        .ok()
        .and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok())
        .unwrap_or_else(|| serde_json::json!({ "boards": {} }));
    if root.get("boards").is_none() {
        root["boards"] = serde_json::json!({});
    }

    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);

    root["boards"][id] = serde_json::json!({
        "port": b.port,
        "fw": b.fw,
        "last_seen": now,
    });

    if let Ok(txt) = serde_json::to_string_pretty(&root) {
        let _ = std::fs::write(&path, txt);
    }
}

/// 启动时立刻拿来显示的"上次见过"的板子（不等扫描）。
pub fn cached_boards() -> Vec<Board> {
    let online = candidate_ports();
    cache_entries()
        .into_iter()
        .map(|e| Board {
            online: online.iter().any(|p| p.eq_ignore_ascii_case(&e.port)),
            port: e.port,
            id: Some(e.id),
            fw: e.fw,
            cached: true,
        })
        .collect()
}

// ---------------------------------------------------------------- 探测与扫描

/// 对单个口发 `$ID?` 并等应答。最多等 `timeout`。
///
/// ⚠ 走 [`acquire`] / [`release`]：句柄测完**放回槽里不关闭**，
///   所以这个函数**不会**踩到 `CloseHandle` 的 30 s。
pub fn probe_identity(
    port: &str,
    timeout: Duration,
) -> Result<(Option<String>, Option<u32>), String> {
    let mut ser = acquire(port)?;
    let res = (|| -> Result<(Option<String>, Option<u32>), String> {
        ser.write_all(&id_query_frame())?;

        let start = Instant::now();
        let mut buf: Vec<u8> = Vec::new();
        let mut chunk = [0u8; 256];

        while start.elapsed() < timeout {
            match ser.read_some(&mut chunk) {
                Ok(0) => {}
                Ok(n) => buf.extend_from_slice(&chunk[..n]),
                Err(e) => return Err(e),
            }
            if let Some(parsed) = parse_id_reply(&buf) {
                return Ok(parsed);
            }
        }
        Ok((None, None))
    })();

    release(port, ser);
    res
}

/// 从读到的字节里找 `$ID:...,FW:n*XX`。
fn parse_id_reply(buf: &[u8]) -> Option<(Option<String>, Option<u32>)> {
    let text = String::from_utf8_lossy(buf);
    for line in text.split(|c| c == '\n' || c == '\r') {
        let line = line.trim();
        let body = match line.strip_prefix('$') {
            Some(b) => b,
            None => continue,
        };
        if !body.starts_with("ID:") {
            continue;
        }
        // 校验：可选（固件按 $...*XX，这里严格校验，避免把噪声当 ID）
        let (payload, ok) = match body.rfind('*') {
            Some(i) => {
                let p = &body[..i];
                let ck = body[i + 1..].get(..2).unwrap_or("");
                let want = u8::from_str_radix(ck, 16).ok();
                (p, want == Some(xor_checksum(p)))
            }
            None => (body, true),
        };
        if !ok {
            continue;
        }

        let mut id = None;
        let mut fw = None;
        for part in payload["ID:".len()..].split(',') {
            let part = part.trim();
            if let Some(v) = part.strip_prefix("FW:") {
                fw = v.trim().parse::<u32>().ok();
            } else if !part.is_empty() {
                id = Some(part.to_string());
            }
        }
        return Some((id, fw));
    }
    None
}

/// 扫描全部候选口。
///
/// 顺序经过优化：**缓存里记录过的端口先探**（通常第一个就命中），
/// 所以复扫耗时 ≈ 一次 `$ID?` 往返（几十毫秒），而不是"每个口都等满超时"。
pub fn scan() -> Vec<Board> {
    let ports = candidate_ports();

    let mut order: Vec<String> = Vec::new();
    for e in cache_entries() {
        if ports.iter().any(|p| p.eq_ignore_ascii_case(&e.port))
            && !order.iter().any(|q| q.eq_ignore_ascii_case(&e.port))
        {
            order.push(e.port.clone());
        }
    }
    for p in &ports {
        if !order.iter().any(|q| q.eq_ignore_ascii_case(p)) {
            order.push(p.clone());
        }
    }

    let mut out: Vec<Board> = Vec::new();
    for port in &order {
        let (id, fw) = probe_identity(port, PROBE_TIMEOUT).unwrap_or((None, None));
        let b = Board {
            port: port.clone(),
            id,
            fw,
            cached: false,
            online: true,
        };
        if b.id.is_some() {
            cache_store(&b); // 认到了就更新缓存（同 UID、新端口也会被纠正）
        }
        out.push(b);
    }

    // 缓存里有、但当前没插的板子也列出来（标成离线），避免用户误以为"板子丢了"。
    for e in cache_entries() {
        let already = out.iter().any(|b| b.id.as_deref() == Some(e.id.as_str()));
        if !already {
            out.push(Board {
                port: e.port,
                id: Some(e.id),
                fw: e.fw,
                cached: true,
                online: false,
            });
        }
    }
    out
}
