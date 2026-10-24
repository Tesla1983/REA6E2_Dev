#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod app;
mod board;
mod collector;
mod lhm;

use std::time::Duration;

use eframe::egui;

fn main() -> eframe::Result<()> {
    // 无界面自检：验证采集链路、帧组包、板子扫描，不弹窗。
    //   ra6e2-monitor.exe --selftest
    if std::env::args().any(|a| a == "--selftest") {
        selftest();
        return Ok(());
    }

    let options = eframe::NativeOptions {
        // 用 glow(OpenGL) 而不是 wgpu：wgpu 的 surface 是异步建立的，
        // 在本机（Intel Arc A770 / D3D12）启动时会出现整窗闪一下。
        // 想回退只需把这里改成 Renderer::Wgpu（Cargo 里 wgpu 依赖仍保留）。
        renderer: eframe::Renderer::Glow,
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([960.0, 600.0])
            .with_min_inner_size([840.0, 540.0])
            .with_title("RA6E2 系统监控台"),
        ..Default::default()
    };

    eframe::run_native(
        "RA6E2 系统监控台",
        options,
        Box::new(|cc| Ok(Box::new(app::App::new(cc)))),
    )
}

fn selftest() {
    // release 构建是 /SUBSYSTEM:windows（无控制台），println 会丢。
    // 所以同时写一份到临时文件，两个产物都能验证。
    let mut log = String::new();
    macro_rules! say {
        ($($a:tt)*) => {{
            let line = format!($($a)*);
            println!("{}", line);
            log.push_str(&line);
            log.push('\n');
        }};
    }

    say!("=== ra6e2-monitor selftest ===");
    say!("LHM 在跑          : {}", collector::lhm_alive());

    let mut cpu = collector::CpuUsage::new();
    let mut gpu = collector::GpuUsage::new();
    say!("PDH GPU 可用      : {}", gpu.is_some());

    // CPU 占用是差分，需要一个间隔
    std::thread::sleep(Duration::from_millis(700));
    let s = collector::collect(&mut cpu, &mut gpu);
    say!(
        "采样              : CT={:?} CU={:?} CF={:?} GT={:?} GU={:?} GF={:?} MU={:?}",
        s.ct, s.cu, s.cf, s.gt, s.gu, s.gf, s.mu
    );

    let mask = vec![
        (true, "CT"),
        (true, "CU"),
        (true, "CF"),
        (true, "GT"),
        (true, "GU"),
        (true, "GF"),
        (true, "MU"),
    ];
    let payload = board::build_payload(&s, &mask);
    say!("payload           : {payload}");
    let f = board::frame(&payload);
    say!("frame             : {}", String::from_utf8_lossy(&f).trim_end());

    say!("--- 板子扫描（按 VID:PID 045B:0263）---");
    let ports = board::candidate_ports();
    if ports.is_empty() {
        say!("  没有候选端口（板子没插 / 固件没跑）");
    }
    let t0 = std::time::Instant::now();
    let found = board::scan();
    for b in &found {
        say!(
            "  {}  id={:?}  fw={:?}{}",
            b.port,
            b.id.clone().unwrap_or_else(|| "无 $ID 应答".into()),
            b.fw,
            if b.online { "" } else { "  [离线/缓存]" }
        );
    }
    say!("  扫描耗时: {:.0} ms", t0.elapsed().as_millis());

    // 句柄验活（ClearCommError）—— 复用缓存句柄前必须过这一关。
    // 复用一次已有句柄，确认它是"活着"的（这是 2026-10-24 那次
    // "COM15 好好的但上位机连不上" 的直接防线）。
    if let Some(b) = found.iter().find(|b| b.online) {
        match board::acquire(&b.port) {
            Ok(ser) => {
                say!("  句柄验活 {}: alive={}", b.port, ser.alive());
                board::release(&b.port, ser);
            }
            Err(e) => say!("  句柄验活失败: {e}"),
        }
    }
    say!("=== done ===");

    let path = std::env::temp_dir().join("ra6e2-monitor-selftest.txt");
    let _ = std::fs::write(&path, log);
    println!("(结果也写到 {})", path.display());
}
