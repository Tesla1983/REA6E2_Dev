//! 内嵌的 LibreHardwareMonitor 数据源：首次运行释放到 %LOCALAPPDATA%，需要时拉起。
//!
//! 这里用的是**自编译精简版 `lhm-lite`**（见 `tools/lhm-lite/`）：仅链接未修改的
//! `LibreHardwareMonitorLib`，自己写极简 host——关掉 Storage/Network/PSU/Battery 等
//! 无关硬件组，去掉 WinForms GUI / 3.28MB 绘图 config / OxyPlot 等纯开销，
//! 在 127.0.0.1:8085 `/data.json` 输出与原版 LHM **同结构**的 JSON，
//! 让 `collector.rs` 零改动对接。MPL-2.0（库未修改，随包附 LICENSE）。
//!
//! 唯一外部通道是它内置的 Web Server（`/data.json`）。
//!
//! 为什么要自己拉起：LHM 是独立进程，不跑就没有任何 SuperIO/MSR 数据。
//! 它读 SuperIO 需要内核驱动（PawnIO）⇒ 必须管理员。本程序 manifest 已声明
//! requireAdministrator，子进程会继承提权，所以这里直接 spawn 即可。

use std::io::Read;
use std::path::PathBuf;
use std::process::Command;

use crate::collector::lhm_alive;

/// 内嵌的精简版 LHM 压缩包（构建时打进去，运行时释放）。
/// 内容 = `tools/lhm-lite/bin/Release` 的 `lhm-lite.exe` + 依赖 DLL + LICENSE，约 2.8 MB。
static LHM_ZIP: &[u8] = include_bytes!("../assets/lhm-lite.zip");

const LHM_EXE: &str = "lhm-lite.exe";

/// lhm-lite 版本标记：**每次重新打包 lhm-lite 时必须手动递增**。
/// 用途：`ensure_installed` 原先是"exe 在就跳过"，改了 lhm-lite 也不会替换已安装的旧 exe；
/// 有了版本标记才能识别旧版本 → 停旧实例 → 重装 → 拉起新版。
const LHM_LITE_VERSION: &str = "2026-10-28-250ms";

pub fn install_dir() -> PathBuf {
    let base = std::env::var("LOCALAPPDATA").unwrap_or_else(|_| ".".to_string());
    PathBuf::from(base).join("Ra6e2Monitor").join("lhm")
}

fn exe_path() -> PathBuf {
    install_dir().join(LHM_EXE)
}

/// 记录已安装版本的标记文件。
fn version_marker() -> PathBuf {
    install_dir().join(".lhm-lite.version")
}

/// 磁盘上的 lhm-lite 是否为当前期望版本（且 exe 存在）。
fn installed_version_ok() -> bool {
    exe_path().exists()
        && std::fs::read_to_string(version_marker())
            .map(|s| s.trim() == LHM_LITE_VERSION)
            .unwrap_or(false)
}

/// 停掉正在运行的 lhm-lite（只用我们专用的 exe 名），释放 exe 文件句柄与 8085 端口。
/// 本程序 manifest 是 requireAdministrator，与 lhm-lite 同权限，故可杀。
fn stop_running_lhm() {
    let mut cmd = Command::new("taskkill");
    cmd.args(["/IM", LHM_EXE, "/F"]);
    hide_console(&mut cmd); // 不闪黑框
    let _ = cmd.output(); // 没在跑就报"找不到"，忽略
    // 给系统时间释放文件句柄与端口
    std::thread::sleep(std::time::Duration::from_millis(300));
}

/// 确保磁盘上是**当前版本**的 lhm-lite：版本不符（或未装/目录被删）时，
/// 先停旧实例，再整包解压，并写版本标记。
pub fn ensure_installed() -> Result<(), String> {
    if installed_version_ok() {
        return Ok(());
    }

    // 旧版本可能还在跑（锁着 exe、占着 8085）→ 先停掉再覆盖
    stop_running_lhm();

    let dir = install_dir();
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
        return Err("解压后仍找不到 lhm-lite.exe".to_string());
    }
    std::fs::write(version_marker(), LHM_LITE_VERSION)
        .map_err(|e| format!("写版本标记失败: {e}"))?;
    Ok(())
}

/// LHM 的拉起结果（供日志区分）。
pub enum LhmAction {
    /// 已经在跑（Web Server 可用），无需等待。
    AlreadyRunning,
    /// 本次新拉起（进程已 spawn，Web Server 稍后就绪）。
    Launched,
}

/// 让控制台子进程**不弹窗**：设置 Windows `CreateProcess` 的 `CREATE_NO_WINDOW`（0x08000000）。
///
/// `lhm-lite` 与 `schtasks` 都是控制台子系统程序；从 GUI（windows 子系统）里 spawn 时，
/// 若不加此标志，Windows 会给子进程**新分配一个控制台窗口**（黑框一闪/常驻）。
/// 加上后子进程无窗口分配，上位机观感 = 单程序、无多余窗口。
#[cfg(windows)]
fn hide_console(cmd: &mut Command) {
    use std::os::windows::process::CommandExt;
    const CREATE_NO_WINDOW: u32 = 0x0800_0000;
    cmd.creation_flags(CREATE_NO_WINDOW);
}
#[cfg(not(windows))]
fn hide_console(_cmd: &mut Command) {}

/// 只确保安装并 spawn LHM，**不等待**其 Web Server 就绪。
/// 真正的"等就绪"交给每帧 `collector::lhm_sample`（连不上就返回 `--`，
/// 就绪后自动有值）——这样连 LHM 冷启动的几秒也不会阻塞任何线程。
fn spawn_lhm() -> Result<(), String> {
    ensure_installed()?;
    let exe = exe_path();
    let dir = install_dir();
    let mut cmd = Command::new(&exe);
    cmd.current_dir(&dir);
    hide_console(&mut cmd); // 关键：不加会弹控制台黑框
    cmd.spawn()
        .map_err(|e| format!("启动 {exe:?} 失败（需要管理员权限?）: {e}"))?;
    Ok(())
}

/// 确保 LHM 可用：**非阻塞**。
/// - 已在跑 → 立刻 `AlreadyRunning`；
/// - 没跑 → 后台 `spawn_lhm()`，返回 `Launched`（不阻塞等待那 25 秒冷启动）。
///
/// 为什么不再阻塞等 25s：LHM 是 .NET Framework 应用，冷启动（CLR + 38 DLL JIT +
/// 全硬件枚举 + 内核驱动）要几秒到十几秒，这是固有开销，等它也省不掉。
/// 与其卡住监控线程（让 CPU/内存/串口采样也跟着延误），不如让它先跑起来，
/// 数据在 LHM 就绪后由每帧采样自然填充。
pub fn ensure_running() -> Result<LhmAction, String> {
    // 版本不符（升级后首次运行）：**跳过**"已在跑"的短路，让 spawn_lhm → ensure_installed
    // 先停旧实例、重装新版再拉起；否则会一直沿用磁盘上的旧 lhm-lite，改动不生效。
    if installed_version_ok() && lhm_alive() {
        return Ok(LhmAction::AlreadyRunning);
    }
    spawn_lhm()?;
    Ok(LhmAction::Launched)
}

// ---------------------------------------------------------------- 开机自启（计划任务）

/// 计划任务名（专用，避免和别的冲突）。
pub const TASK_NAME: &str = "Ra6e2MonitorLHM";

/// 创建"登录时以最高权限自启 lhm-lite"的计划任务。
///
/// 把 LHM 的冷启动从"开上位机时才发生"提前到"用户登录时就发生"——
/// 等你开上位机时它早已就绪，温度/风扇数据零延迟。
/// 需要管理员（本程序 manifest 已是 requireAdministrator，子进程继承提权）。
pub fn install_autostart() -> Result<(), String> {
    ensure_installed()?;
    let exe = exe_path();
    // /tr 的值整体用引号包裹（路径可能含空格）。lhm-lite 是 headless，无需 /minimized。
    let tr = format!("\"{}\"", exe.display());
    let mut cmd = Command::new("schtasks");
    cmd.args(["/create", "/tn", TASK_NAME, "/tr", &tr, "/sc", "onlogon", "/rl", "highest", "/f"]);
    hide_console(&mut cmd); // 勾选/取消勾选时不闪黑框
    let out = cmd
        .output()
        .map_err(|e| format!("创建计划任务失败（需要管理员?）: {e}"))?;
    if !out.status.success() {
        return Err(format!(
            "schtasks 创建失败: {}",
            String::from_utf8_lossy(&out.stderr).trim()
        ));
    }
    Ok(())
}

/// 删除开机自启计划任务（幂等，不存在也不报错）。
pub fn remove_autostart() -> Result<(), String> {
    let mut cmd = Command::new("schtasks");
    cmd.args(["/delete", "/tn", TASK_NAME, "/f"]);
    hide_console(&mut cmd);
    let out = cmd
        .output()
        .map_err(|e| format!("删除计划任务失败: {e}"))?;
    // 退出码非 0 通常是"任务不存在"，视为成功（幂等）
    let stderr = String::from_utf8_lossy(&out.stderr);
    if out.status.success() || stderr.contains("不存在") {
        Ok(())
    } else {
        Err(format!("schtasks 删除失败: {}", stderr.trim()))
    }
}

