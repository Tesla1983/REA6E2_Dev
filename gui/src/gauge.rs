//! 半圆刻度盘控件：1:1 复刻 HTML 渲染稿的工业面板风。
//!
//! 每个指标 = 一个半圆表盘（轨道弧 + 彩色进度弧 + 白色游标点 + 中心数值 + 底部标签）。
//! 表盘可点击以切换该指标的"是否上报"掩码：启用时显示主题色，未启用时整体 dim（灰）。
//!
//! 布局：表盘按"所在容器宽度"自适应半径（列越宽表盘越大），并水平居中于容器内，
//! 因此调用方用 `ui.columns(n)` 即可让 n 个表盘在卡片内等距铺开、互不重叠。

use eframe::egui::{Align2, Color32, FontId, Pos2, Response, Sense, Shape, Stroke, Ui, Vec2};
use crate::app::accent_for_bg;

// 与 HTML 渲染稿 / 现有 sparkline 配色保持一致
pub const TEMP_COLOR: Color32 = Color32::from_rgb(0xFF, 0x5B, 0x5B);
pub const FAN_COLOR: Color32 = Color32::from_rgb(0x29, 0xB6, 0xF6);
pub const LOAD_COLOR: Color32 = Color32::from_rgb(0x4C, 0xE0, 0x70);
pub const MEM_COLOR: Color32 = Color32::from_rgb(0xFF, 0xAA, 0x00);
// 卡片标题霓虹色（CPU 绿 / GPU 蓝 / 内存 橙）
pub const CPU_TITLE: Color32 = Color32::from_rgb(0x4C, 0xE0, 0x70);
pub const GPU_TITLE: Color32 = Color32::from_rgb(0x29, 0xB6, 0xF6);

const TRACK: Color32 = Color32::from_rgb(42, 42, 53);
const LIGHT_TRACK: Color32 = Color32::from_rgb(214, 216, 224);
const MUTED: Color32 = Color32::from_rgb(138, 138, 157);
const LIGHT_MUTED: Color32 = Color32::from_rgb(110, 118, 135);

/// 画一个半圆刻度盘，返回 `Response`（点击切换指标掩码）。
///
/// * `value` 为 `None` 时显示 `—`，进度弧不画。
/// * `max` 为量程上限（如温度/负载/显存 = 100，风扇 RPM = 3000）。
/// * `enabled` 控制是否"已选中上报"；关闭时整体 dim。
///
/// 表盘尺寸随调用容器的可用宽度自适应（半径 clamp 在 20~54），并水平居中于容器，
/// 因此用 `ui.columns(n)` 调用即可让 n 个表盘在卡片内等距、均匀展开。
pub fn gauge(
    ui: &mut Ui,
    label: &str,
    unit: &str,
    value: Option<f32>,
    max: f32,
    color: Color32,
    enabled: bool,
) -> Response {
    let pad: f32 = 6.0; // 给弧线宽度留的边距
    let col_w = ui.available_width().max(40.0);
    let radius = ((col_w - 2.0 * pad) / 2.0).clamp(20.0, 54.0);
    let arc_stroke = (radius * 0.22).clamp(7.0, 11.0);
    let val_font = (radius * 0.34).clamp(12.0, 18.0);
    // 高度 = 半圆 + 底部标签区；整个容器（含左右留白）都可点击
    let h = radius + 28.0;
    let (rect, resp) = ui.allocate_exact_size(Vec2::new(col_w, h), Sense::click());
    let painter = ui.painter_at(rect);
    // 居中于容器：列比表盘宽时左右留白自动均分 → 等距展开
    let cx = rect.center().x;
    let cy = rect.top() + radius + pad;

    // 主题感知：浅色背景下把轨道/弱色/霓虹色都换成对比度友好的版本
    let dark = ui.visuals().dark_mode;
    let track = if dark { TRACK } else { LIGHT_TRACK };
    let muted = if dark { MUTED } else { LIGHT_MUTED };
    let active = if enabled {
        accent_for_bg(color, dark)
    } else {
        track
    };
    let cursor = if dark { Color32::WHITE } else { Color32::from_rgb(40, 44, 52) };

    // 半圆角度：180°(左) → 0°(右)，向上凸（屏幕 y 向下，故减 sin）
    let arc = |deg: f32| -> Pos2 {
        let a = deg.to_radians();
        Pos2::new(cx + radius * a.cos(), cy - radius * a.sin())
    };
    let steps = 64;
    let track_pts: Vec<Pos2> = (0..=steps)
        .map(|i| arc(180.0 - (i as f32 / steps as f32) * 180.0))
        .collect();
    painter.add(Shape::line(track_pts, Stroke::new(arc_stroke, track)));

    let pct = match value {
        Some(v) if max > 0.0 => (v / max).clamp(0.0, 1.0),
        _ => 0.0,
    };
    if pct > 0.001 {
        let n = (pct * steps as f32).round().max(1.0) as i32;
        let prog_pts: Vec<Pos2> = (0..=n)
            .map(|i| arc(180.0 - (i as f32 / steps as f32) * 180.0))
            .collect();
        painter.add(Shape::line(prog_pts, Stroke::new(arc_stroke, active)));
        // 端点圆点模拟 round line-cap，游标叠在末端（浅色底用深色游标，否则白点看不见）
        let end = arc(180.0 - pct * 180.0);
        painter.circle_filled(end, arc_stroke * 0.5 + 1.0, active);
        painter.circle_filled(end, arc_stroke * 0.32, cursor);
    }
    // 起点圆点
    let start = arc(180.0);
    painter.circle_filled(start, arc_stroke * 0.5 + 1.0, active);

    // 中心数值：放在半圆"盆底"最宽处（贴近直径线），此处水平净空最大，
    // 即使 "667 RPM" 这类带单位的宽字符串也不会压到弧线两侧。
    let val_str = match value {
        Some(v) => format!("{}{}", v as i32, unit),
        None => format!("—{}", unit),
    };
    let val_pos = Pos2::new(cx, cy - radius * 0.18);
    painter.text(
        val_pos,
        Align2::CENTER_CENTER,
        val_str,
        FontId::proportional(val_font),
        if enabled { active } else { muted },
    );

    // 底部标签（在直径线之外）
    let lab_pos = Pos2::new(cx, rect.bottom() - 7.0);
    painter.text(
        lab_pos,
        Align2::CENTER_CENTER,
        label,
        FontId::proportional(11.0),
        muted,
    );

    resp
}
