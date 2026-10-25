#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod app;
mod board;
mod collector;
mod lhm;
mod tray;

use std::time::Duration;

use eframe::egui;

fn main() -> eframe::Result<()> {
    // 无界面自检：验证采集链路、帧组包、板子扫描，不弹窗。
    //   ra6e2-monitor.exe --selftest
    if std::env::args().any(|a| a == "--selftest") {
        selftest();
        return Ok(());
    }

    // GPU 取数探针（Phase 0）：看每张卡在 DXGI / PDH / LHM 里分别是什么样子，
    // 以及本机有没有 NVML / ADL 的 DLL。加 AMD/NVIDIA 支持前先跑它。
    //   ra6e2-monitor.exe --probe-gpu
    if std::env::args().any(|a| a == "--probe-gpu") {
        probe_gpu();
        return Ok(());
    }

    // 窗口位置/大小不在这里给：它们是**物理像素**（Win32 GetWindowRect），
    // 由 App::new() 用 SetWindowPos 摆放，避免 egui "缩放后的点" 与 DPI 换算打架。
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

fn selftest() {    // release 构建是 /SUBSYSTEM:windows（无控制台），println 会丢。
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
        "采样              : CT={:?} CU={:?} CF={:?} GT={:?} GU={:?} GF={:?} MU={:?} GV={:?}",
        s.ct, s.cu, s.cf, s.gt, s.gu, s.gf, s.mu, s.gv
    );

    say!("风扇单位          : CF={:?} GF={:?}", s.cf_unit, s.gf_unit);

    let mask = vec![
        (true, "CT"),
        (true, "CU"),
        (true, "CF"),
        (true, "GT"),
        (true, "GU"),
        (true, "GF"),
        (true, "GV"),
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

// ---------------------------------------------------------------- GPU 取数探针

/// Phase 0：把"每张卡在各数据源里长什么样"一次性打出来。
///
/// 用途：加 AMD/NVIDIA 支持前，先确认三件事
///   1. DXGI 能不能枚举到卡、LUID 是多少（PDH 过滤、厂商库对卡都靠它）；
///   2. PDH `\GPU Engine` 的实例是否真的按 LUID 过滤成功（命中数/总数）；
///   3. LHM 里这张卡的硬件名与传感器叶子叫什么（NVML/ADL 拿不到时的兜底解析要用）。
///
/// 输出同时写 `%TEMP%\ra6e2-gpu-probe.txt`，release 构建（无控制台）也能取。
fn probe_gpu() {
    let mut log = String::new();
    macro_rules! say {
        ($($a:tt)*) => {{
            let line = format!($($a)*);
            println!("{}", line);
            log.push_str(&line);
            log.push('\n');
        }};
    }

    say!("=== GPU 取数探针 (Phase 0) ===");

    // 1) DXGI 枚举
    let adapters = collector::enumerate_adapters();
    say!("DXGI 适配器 {} 张:", adapters.len());
    for a in &adapters {
        say!(
            "  [{}] {}{}\n      vendor={:#06X}({}) device={:#06X}  vram={:.1} GiB\n      luid={}  idx={}",
            a.index,
            a.name,
            if a.software { "  [软件适配器]" } else { "" },
            a.vendor_id,
            a.vendor_name(),
            a.device_id,
            a.vram_gib(),
            a.luid_tag(),
            a.index
        );
    }

    let primary = collector::pick_primary_adapter(&adapters).cloned();
    match &primary {
        Some(p) => say!("\n主 GPU 判定: [{}] {} ({})", p.index, p.name, p.vendor_name()),
        None => say!("\n主 GPU 判定: 无（只有软件适配器？）"),
    }

    // 2) PDH：逐卡采样，确认 LUID 过滤是否匹配上
    say!("\n--- PDH \\GPU Engine（3D 引擎）---");
    for a in &adapters {
        if a.software {
            continue;
        }
        match collector::GpuUsage::new_for_luid(a.luid_high, a.luid_low) {
            Some(g) => {
                let mut g = g.with_index(a.index);
                std::thread::sleep(Duration::from_millis(300));
                let v = g.sample();
                let (m, t) = g.last_stats();
                say!(
                    "  [{}] {}: 3D={}%  显存={}  命中实例 {}/{}",
                    a.index,
                    a.name,
                    v.map(|x| x.to_string()).unwrap_or_else(|| "--".into()),
                    g.vram_percent()
                        .map(|x| format!("{x}%"))
                        .unwrap_or_else(|| "--".into()),
                    m,
                    t
                );
            }
            None => say!("  [{}] {}: PDH 打开失败", a.index, a.name),
        }
    }

    // 2b) 采集侧自动选卡是否与"显存最大"一致（GpuUsage::new 内部走同一套逻辑）
    if let Some(g) = collector::GpuUsage::new() {
        say!(
            "自动选卡(GpuUsage::new, 实际采集在用的就是这张): {}",
            g.adapter_name().unwrap_or("(未命名)")
        );
    }

    // 3) 厂商库 DLL 是否在场（决定 NVML / ADL 后端能不能落地）
    say!("\n--- 厂商 API DLL ---");    let sysroot = std::env::var("SystemRoot").unwrap_or_else(|_| r"C:\Windows".into());
    for dll in ["nvml.dll", "nvapi64.dll", "atiadlxx.dll", "amdadlx64.dll", "igcl.dll"] {
        let p = std::path::Path::new(&sysroot).join("System32").join(dll);
        say!("  {:<14} {}", dll, if p.exists() { "存在" } else { "不存在" });
    }

    // 4) LHM 里这张卡的样子
    say!("\n--- LHM data.json ---");
    if !collector::lhm_alive() {
        say!("  LHM 没在跑（需要管理员 + 程序自己拉起），跳过。");
    } else {
        let want = primary
            .as_ref()
            .map(|p| p.name.to_lowercase())
            .unwrap_or_default();

        // 按硬件名分组，保持首次出现顺序（不能假设只有一张卡）
        let mut groups: Vec<(String, Vec<String>)> = Vec::new();
        for (hw, group, leaf) in collector::lhm_dump_lines() {
            let hwl = hw.to_lowercase();
            let is_gpu = ["arc", "geforce", "radeon", "nvidia", "intel(r) arc"]
                .iter()
                .any(|k| hwl.contains(k));
            if !is_gpu {
                continue;
            }
            let line = format!("{:<14} {}", group, leaf);
            match groups.iter_mut().find(|(n, _)| n == &hw) {
                Some((_, v)) => v.push(line),
                None => groups.push((hw, vec![line])),
            }
        }

        if groups.is_empty() {
            say!("  没找到 GPU 类硬件节点（LHM 未加载显卡？）");
        }
        for (hw, lines) in groups {
            // 名字互含即认为"对得上"（LHM: 'Intel(R) Arc(TM) A770 Graphics'
            //                        DXGI: 'Intel(R) Arc(TM) A770 Graphics'）
            let hwl = hw.to_lowercase();
            let hit = !want.is_empty() && (want.contains(&hwl) || hwl.contains(&want));
            say!("  硬件: {}{}", hw, if hit { "   <= 与主 GPU 名字对得上" } else { "" });
            for l in lines {
                say!("      {l}");
            }
        }
    }

    say!("=== done ===");
    let path = std::env::temp_dir().join("ra6e2-gpu-probe.txt");
    let _ = std::fs::write(&path, log);
    println!("(结果也写到 {})", path.display());
}
