//! 声明"必须管理员运行"。
//!
//! 为什么不用自定义 .manifest 文件：
//!   实测 `cargo:rustc-link-arg=/MANIFESTINPUT:app.manifest` 会和 rustc 自己生成的
//!   manifest 片段合并，两处的 `requestedExecutionLevel level` 不一致 ⇒
//!   `manifest authoring error c1010001` + `LNK1327`。
//!   ⇒ 改用链接器自带的 `/MANIFESTUAC`，它直接替换掉生成的 manifest 里的提权等级，
//!     不存在合并冲突。
//!
//! DPI 感知不在这里声明：winit 启动时会自己调 `SetProcessDpiAwarenessContext`，
//! egui 也能拿到正确的缩放比，不需要 manifest 里的 dpiAwareness 条目。

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=RA6E2_NO_UAC");

    let target_env = std::env::var("CARGO_CFG_TARGET_ENV").unwrap_or_default();
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();

    // 仅用于本地截图/调试：设了这个环境变量就不嵌入提权清单，
    // 这样非提权的 shell 也能把窗口拉起来截图。
    // ⚠ 交付用的 exe **不要**带这个变量编译（否则 LHM 读不到 SuperIO）。
    if std::env::var("RA6E2_NO_UAC").is_ok() {
        println!("cargo:warning=RA6E2_NO_UAC: 未嵌入提权清单（仅供截图/调试）");
        return;
    }

    if target_os == "windows" && target_env == "msvc" {
        // LHM 要读 MSR / SuperIO（内核驱动），非管理员拿不到 CPU 温度和风扇。
        // ⚠ 必须同时给 /MANIFEST:EMBED：只给 /MANIFESTUAC 时链接器不会把清单写进 exe
        //   （实测 exe 里搜不到 requireAdministrator），提权不会生效。
        println!("cargo:rustc-link-arg=/MANIFEST:EMBED");
        println!("cargo:rustc-link-arg=/MANIFESTUAC:level='requireAdministrator' uiAccess='false'");
    }
}
