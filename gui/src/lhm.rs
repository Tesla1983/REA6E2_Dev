//! 内嵌的 LibreHardwareMonitor：首次运行释放到 %LOCALAPPDATA%，需要时拉起。
//!
//! 为什么不直接用 LHM 的 WMI：0.9.6 起官方**已移除 WMI 发布**
//! （`root\LibreHardwareMonitor` 只是 0.9.4 的注册残留，永远 0 条实例）。
//! 唯一外部通道是它内置的 Web Server（`/data.json`）。
//!
//! 为什么要自己拉起：LHM 是独立进程，不跑就没有任何 SuperIO/MSR 数据。
//! 它读 SuperIO 需要内核驱动 ⇒ 必须管理员。本程序 manifest 已声明 requireAdministrator，
//! 子进程会继承提权，所以这里直接 spawn 即可。

use std::io::Read;
use std::path::PathBuf;
use std::process::Command;
use std::time::{Duration, Instant};

use crate::collector::lhm_alive;

/// 内嵌的 LHM 压缩包（构建时打进去，运行时释放）。
/// 内容 = H:\IDM\LibreHardwareMonitor 去掉 *.pdb / *.xml，约 8 MB。
static LHM_ZIP: &[u8] = include_bytes!("../assets/lhm.zip");

const LHM_EXE: &str = "LibreHardwareMonitor.exe";

pub fn install_dir() -> PathBuf {
    let base = std::env::var("LOCALAPPDATA").unwrap_or_else(|_| ".".to_string());
    PathBuf::from(base).join("Ra6e2Monitor").join("lhm")
}

fn exe_path() -> PathBuf {
    install_dir().join(LHM_EXE)
}

/// 首次运行（或目录被删）时解压。
pub fn ensure_installed() -> Result<(), String> {
    let dir = install_dir();
    let exe = dir.join(LHM_EXE);
    if exe.exists() {
        return Ok(());
    }
    std::fs::create_dir_all(&dir).map_err(|e| format!("建目录 {dir:?} 失败: {e}"))?;

    let mut zip = zip::ZipArchive::new(std::io::Cursor::new(LHM_ZIP))
        .map_err(|e| format!("解开内嵌 zip 失败: {e}"))?;

    for i in 0..zip.len() {
        let mut f = zip.by_index(i).map_err(|e| format!("读 zip 第 {i} 项失败: {e}"))?;
        let Some(rel) = f.enclosed_name() else { continue };
        let out = dir.join(rel);
        if f.is_dir() {
            std::fs::create_dir_all(&out).ok();
            continue;
        }
        if let Some(parent) = out.parent() {
            std::fs::create_dir_all(parent).ok();
        }
        let mut buf = Vec::with_capacity(f.size() as usize);
        f.read_to_end(&mut buf).map_err(|e| format!("解压 {out:?} 失败: {e}"))?;
        std::fs::write(&out, &buf).map_err(|e| format!("写 {out:?} 失败: {e}"))?;
    }

    if !exe_path().exists() {
        return Err("解压后仍找不到 LibreHardwareMonitor.exe".to_string());
    }
    Ok(())
}

/// 拉起 LHM（已安装才调用）。返回它是否成功启动到 HTTP 可用。
pub fn launch_and_wait(wait: Duration) -> Result<(), String> {
    ensure_installed()?;
    let exe = exe_path();
    let dir = install_dir();

    Command::new(&exe)
        .current_dir(&dir)
        .spawn()
        .map_err(|e| format!("启动 {exe:?} 失败（需要管理员权限?）: {e}"))?;

    // LHM 的 Web Server 要过几秒才开始监听 8085，这里轮询等它。
    let start = Instant::now();
    while start.elapsed() < wait {
        if lhm_alive() {
            return Ok(());
        }
        std::thread::sleep(Duration::from_millis(400));
    }
    Err(format!(
        "LHM 已启动，但 {} 秒内 /data.json 仍不可用（检查 LHM 的 Options > Server > Run）",
        wait.as_secs()
    ))
}

/// 确保 LHM 可用：已在跑就直接返回，否则拉起。
pub fn ensure_running() -> Result<(), String> {
    if lhm_alive() {
        return Ok(());
    }
    launch_and_wait(Duration::from_secs(25))
}
