//! 系统托盘（Windows / Shell_NotifyIconW，纯 Win32，不引第三方库）。
//!
//! 设计要点：
//! - 托盘图标跑在**自己的线程**里，带自己的消息循环。这样完全不依赖 winit/eframe
//!   的消息泵（把托盘塞进 UI 线程容易被 eframe 的 repaint 逻辑搅乱）。
//! - 线程里建一个**隐藏的顶层窗口**（不能用 HWND_MESSAGE：`TrackPopupMenu` 在
//!   message-only 窗口上不可靠）作为托盘回调的宿主。
//! - 和 UI 之间只用一条 mpsc 通道传 `TrayEvent`，UI 每帧 `try_recv` 一次。
//! - 关窗语义（2026-10-25 定）：**点窗口 X = 直接退出**；只有顶栏「收进托盘」按钮
//!   才会把窗口隐藏到托盘（隐藏后双击托盘图标/菜单「显示主窗口」可恢复）。
//!   App 侧不再拦截 `close_requested`，所以 X 和托盘菜单「退出」都是正常关闭流程。

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::Sender;
use std::sync::{Arc, OnceLock};

use windows::core::PCWSTR;
use windows::Win32::Foundation::{HWND, LPARAM, LRESULT, POINT, WPARAM};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::Shell::{
    Shell_NotifyIconW, NIF_ICON, NIF_MESSAGE, NIF_TIP, NIM_ADD, NIM_DELETE, NOTIFYICONDATAW,
};
use windows::Win32::UI::WindowsAndMessaging::{
    AppendMenuW, CreatePopupMenu, CreateWindowExW, DefWindowProcW, DestroyWindow,
    DispatchMessageW, EnumWindows, GetCursorPos, GetWindowTextLengthW, GetWindowTextW,
    GetWindowThreadProcessId, GetMessageW, IsWindowVisible, LoadImageW, PostMessageW,
    PostQuitMessage, RegisterClassW, SetForegroundWindow, ShowWindow, TrackPopupMenu,
    TranslateMessage, HMENU, IMAGE_ICON, LR_DEFAULTSIZE, LR_LOADFROMFILE, MF_SEPARATOR, MF_STRING,
    MSG, SW_RESTORE, SW_SHOW, TPM_RETURNCMD, TPM_RIGHTBUTTON, WINDOW_EX_STYLE, WINDOW_STYLE,
    WM_APP, WM_CLOSE, WM_DESTROY, WM_LBUTTONDBLCLK, WM_LBUTTONUP, WM_NULL, WM_RBUTTONUP, WNDCLASSW,
};

/// 托盘 → UI 的事件。
#[derive(Clone, Copy, Debug)]
pub enum TrayEvent {
    /// 左键单击/双击托盘图标，或菜单「显示主窗口」。
    Show,
    /// 菜单「退出」——真正结束进程。
    Quit,
}

const WM_TRAY: u32 = WM_APP + 1;
const MENU_SHOW: usize = 1;
const MENU_QUIT: usize = 2;

/// 托盘图标句柄（线程退出/程序结束时删掉，避免留下"幽灵图标"）。
pub struct Tray {
    /// 隐藏窗口句柄（按整数存，避免把裸指针塞进结构体影响 Send/Sync 语义）
    hwnd: usize,
}

impl Tray {
    /// 起托盘线程。返回 `None` 表示系统拒绝（例如 Explorer 没跑）。
    pub fn spawn(tx: Sender<TrayEvent>, quit: Arc<AtomicBool>) -> Option<Self> {
        let (ready_tx, ready_rx) = std::sync::mpsc::channel::<Option<usize>>();

        std::thread::Builder::new()
            .name("ra6e2-tray".into())
            .spawn(move || unsafe {
                let hwnd = create_tray_window();
                let Some(hwnd) = hwnd else {
                    let _ = ready_tx.send(None);
                    return;
                };
                let Some(nid) = add_icon(hwnd) else {
                    let _ = DestroyWindow(hwnd);
                    let _ = ready_tx.send(None);
                    return;
                };
                let menu = build_menu();

                CTX.with(|c| {
                    *c.borrow_mut() = Some(Ctx { tx, menu, nid, quit });
                });

                let _ = ready_tx.send(Some(hwnd.0 as usize));

                // 自己的消息循环：托盘回调、菜单命令都在这儿处理
                let mut msg = MSG::default();
                while GetMessageW(&mut msg, None, 0, 0).as_bool() {
                    let _ = TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            })
            .ok()?;

        let hwnd = ready_rx.recv_timeout(std::time::Duration::from_secs(3)).ok()??;
        Some(Self { hwnd })
    }

    /// 删托盘图标并结束托盘线程（关窗/退出时调用）。
    pub fn shutdown(&self) {
        if self.hwnd != 0 {
            let hwnd = HWND(self.hwnd as *mut core::ffi::c_void);
            unsafe {
                let _ = PostMessageW(Some(hwnd), WM_CLOSE, WPARAM(0), LPARAM(0));
            }
        }
    }
}

// ---------------------------------------------------------------- 内部

struct Ctx {
    tx: Sender<TrayEvent>,
    menu: HMENU,
    nid: NOTIFYICONDATAW,
    /// 托盘菜单点了「退出」——UI 线程即使没在跑（窗口隐藏时可能不刷新），
    /// 也能靠这个共享标志 + 直接发 WM_CLOSE 把程序关掉。
    quit: Arc<AtomicBool>,
}

thread_local! {
    /// 只在托盘线程里用（wndproc 与建窗函数同线程）。
    static CTX: std::cell::RefCell<Option<Ctx>> = const { std::cell::RefCell::new(None) };
}

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

/// 把内嵌的 ico 释放到 %LOCALAPPDATA%\Ra6e2Monitor\tray.ico 再交给 LoadImageW。
///
/// 为什么不 `CreateIconFromResourceEx`：那要求传"图标资源（RT_ICON）"的原始数据，
/// 而 `.ico` 是容器格式（ICONDIR + 图像块），需要额外拆包。落盘一次更省事。
fn ensure_icon_file() -> Option<std::path::PathBuf> {
    static PATH: OnceLock<Option<std::path::PathBuf>> = OnceLock::new();
    PATH.get_or_init(|| {
        let base = std::env::var("LOCALAPPDATA").unwrap_or_else(|_| ".".into());
        let dir = std::path::PathBuf::from(base).join("Ra6e2Monitor");
        let path = dir.join("tray.ico");
        let bytes: &[u8] = include_bytes!("../assets/tray.ico");
        if std::fs::read(&path).map(|v| v == bytes).unwrap_or(false) {
            return Some(path);
        }
        std::fs::create_dir_all(&dir).ok()?;
        std::fs::write(&path, bytes).ok()?;
        Some(path)
    })
    .clone()
}

unsafe fn create_tray_window() -> Option<HWND> {
    let hinst = unsafe { GetModuleHandleW(None) }.ok()?;
    let class = wide("Ra6e2MonitorTrayWnd");

    let wc = WNDCLASSW {
        lpfnWndProc: Some(wndproc),
        hInstance: hinst.into(),
        lpszClassName: PCWSTR(class.as_ptr()),
        ..Default::default()
    };
    // 重复注册（上次进程残留）会失败，但不影响 CreateWindowExW，忽略返回值
    let _ = unsafe { RegisterClassW(&wc) };

    unsafe {
        CreateWindowExW(
            WINDOW_EX_STYLE(0),
            PCWSTR(class.as_ptr()),
            PCWSTR(wide("Ra6e2MonitorTray").as_ptr()),
            WINDOW_STYLE(0), // 不带 WS_VISIBLE ⇒ 隐藏窗口
            0, 0, 0, 0,
            None,
            None,
            Some(hinst.into()),
            None,
        )
    }
    .ok()
}

unsafe fn load_icon() -> Option<windows::Win32::UI::WindowsAndMessaging::HICON> {
    let path = ensure_icon_file()?;
    let w = wide(&path.to_string_lossy());
    unsafe {
        LoadImageW(
            None,
            PCWSTR(w.as_ptr()),
            IMAGE_ICON,
            0,
            0,
            LR_LOADFROMFILE | LR_DEFAULTSIZE,
        )
    }
    .ok()
    .map(|h| windows::Win32::UI::WindowsAndMessaging::HICON(h.0))
}

unsafe fn add_icon(hwnd: HWND) -> Option<NOTIFYICONDATAW> {
    let hicon = unsafe { load_icon() };
    let mut nid = NOTIFYICONDATAW {
        cbSize: std::mem::size_of::<NOTIFYICONDATAW>() as u32,
        hWnd: hwnd,
        uID: 1,
        uFlags: NIF_ICON | NIF_MESSAGE | NIF_TIP,
        uCallbackMessage: WM_TRAY,
        hIcon: hicon.unwrap_or_default(),
        ..Default::default()
    };
    let tip: Vec<u16> = "RA6E2 系统监控台".encode_utf16().collect();
    nid.szTip[..tip.len().min(127)].copy_from_slice(&tip[..tip.len().min(127)]);

    let ok = unsafe { Shell_NotifyIconW(NIM_ADD, &nid) };
    ok.as_bool().then_some(nid)
}

/// 找主窗口：同进程、标题含 "RA6E2" 的顶层窗口。
/// 用作"窗口隐藏后 UI 不再刷新"时的兜底——直接 ShowWindow 比等 UI 轮询可靠。
fn find_main_window() -> Option<HWND> {
    struct Search {
        pid: u32,
        found: HWND,
    }
    unsafe extern "system" fn cb(hwnd: HWND, lp: LPARAM) -> windows::core::BOOL {
        let st = unsafe { &mut *(lp.0 as *mut Search) };
        let mut pid = 0u32;
        unsafe { GetWindowThreadProcessId(hwnd, Some(&mut pid)) };
        if pid != st.pid {
            return true.into();
        }
        let len = unsafe { GetWindowTextLengthW(hwnd) };
        if len <= 0 {
            return true.into();
        }
        let mut buf = vec![0u16; len as usize + 1];
        let n = unsafe { GetWindowTextW(hwnd, &mut buf) } as usize;
        let title = String::from_utf16_lossy(&buf[..n]);
        if title.contains("RA6E2") {
            st.found = hwnd;
            return false.into(); // 找到就停
        }
        true.into()
    }

    let mut st = Search {
        pid: std::process::id(),
        found: HWND(std::ptr::null_mut()),
    };
    unsafe {
        let _ = EnumWindows(Some(cb), LPARAM(&mut st as *mut Search as isize));
    }
    (!st.found.0.is_null()).then_some(st.found)
}

/// 显示并前置主窗口（含从最小化还原）。
fn show_main_window() {
    if let Some(h) = find_main_window() {
        unsafe {
            let _ = ShowWindow(h, SW_SHOW);
            let _ = ShowWindow(h, SW_RESTORE);
            let _ = SetForegroundWindow(h);
        }
        let _ = IsWindowVisible; // 保底：确保该导入被用到（诊断时可打印可见性）
    }
}

unsafe fn build_menu() -> HMENU {
    unsafe {
        let menu = CreatePopupMenu().unwrap_or_default();
        let show = wide("显示主窗口");
        let quit = wide("退出");
        let _ = AppendMenuW(menu, MF_STRING, MENU_SHOW, PCWSTR(show.as_ptr()));
        let _ = AppendMenuW(menu, MF_SEPARATOR, 0, PCWSTR::null());
        let _ = AppendMenuW(menu, MF_STRING, MENU_QUIT, PCWSTR(quit.as_ptr()));
        menu
    }
}

unsafe extern "system" fn wndproc(hwnd: HWND, msg: u32, wp: WPARAM, lp: LPARAM) -> LRESULT {
    match msg {
        WM_TRAY => {
            // 旧式回调：lParam 低 16 位是鼠标消息
            let ev = (lp.0 as u32) & 0xFFFF;
            if ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK {
                show_main_window();
                CTX.with(|c| {
                    if let Some(ctx) = c.borrow().as_ref() {
                        let _ = ctx.tx.send(TrayEvent::Show);
                    }
                });
            } else if ev == WM_RBUTTONUP {
                CTX.with(|c| {
                    let borrowed = c.borrow();
                    let Some(ctx) = borrowed.as_ref() else { return };
                    unsafe {
                        let mut pt = POINT::default();
                        let _ = GetCursorPos(&mut pt);
                        // 经典 Win32 坑：不先 SetForegroundWindow，菜单点外面不会消失
                        let _ = SetForegroundWindow(hwnd);
                        let cmd = TrackPopupMenu(
                            ctx.menu,
                            TPM_RETURNCMD | TPM_RIGHTBUTTON,
                            pt.x,
                            pt.y,
                            None,
                            hwnd,
                            None,
                        );
                        let _ = PostMessageW(Some(hwnd), WM_NULL, WPARAM(0), LPARAM(0));
                        match cmd.0 as usize {
                            MENU_SHOW => {
                                let _ = ctx.tx.send(TrayEvent::Show);
                                drop(borrowed);
                                show_main_window();
                                return;
                            }
                            MENU_QUIT => {
                                // 置共享标志 + 直接给主窗口发 WM_CLOSE：
                                // 窗口隐藏时 UI 可能不刷新，光靠通道事件可能没人处理。
                                ctx.quit.store(true, Ordering::Relaxed);
                                let _ = ctx.tx.send(TrayEvent::Quit);
                                if let Some(w) = find_main_window() {
                                    let _ = PostMessageW(Some(w), WM_CLOSE, WPARAM(0), LPARAM(0));
                                }
                            }
                            _ => {}
                        }
                    }
                });
            }
            LRESULT(0)
        }
        WM_CLOSE => {
            unsafe {
                let _ = DestroyWindow(hwnd);
            }
            LRESULT(0)
        }
        WM_DESTROY => {
            // 先删图标再退出消息循环，避免任务栏留"幽灵图标"
            CTX.with(|c| {
                if let Some(ctx) = c.borrow_mut().take() {
                    unsafe {
                        let _ = Shell_NotifyIconW(NIM_DELETE, &ctx.nid);
                    }
                }
            });
            unsafe { PostQuitMessage(0) };
            LRESULT(0)
        }
        _ => unsafe { DefWindowProcW(hwnd, msg, wp, lp) },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 冒烟：真的往任务栏塞一次图标再删掉。
    /// 默认 `#[ignore]`（会在通知区闪一下）；验证 Win32 代码路径时手动跑：
    ///   cargo test --release -- --ignored tray_smoke --nocapture
    #[test]
    #[ignore]
    fn tray_smoke() {
        let (tx, rx) = std::sync::mpsc::channel();
        let quit = Arc::new(AtomicBool::new(false));
        let tray = Tray::spawn(tx, quit).expect("Shell_NotifyIcon 应能创建托盘图标");
        println!("tray created, hwnd=0x{:X}", tray.hwnd);
        std::thread::sleep(std::time::Duration::from_millis(400));
        tray.shutdown();
        std::thread::sleep(std::time::Duration::from_millis(200));
        // 关掉后不应该再有事件进来
        assert!(rx.try_recv().is_err(), "shutdown 后不应再有托盘事件");
        println!("tray shut down cleanly");
    }
}
