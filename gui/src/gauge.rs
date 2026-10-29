//! 半圆刻度盘控件：**三段色区间版**。
//!
//! 每个指标 = 一个半圆表盘，两层颜色：
//!   · **轨道**（背景）= 按阈值切成 绿 / 黄 / 红 三段暗色标尺，固定不动（像仪表的红线区）；
//!   · **进度弧 + 游标 + 中心数值** = 当前值所在区间的**亮色**（正常绿 / 接近极限黄 / 超限红）。
//!
//! 阈值由调用方给（绝对值，与 `max` 同单位）：`warn` = 进入黄区的值，`crit` = 进入红区的值。
//! 未选中上报（`enabled == false`）时整体 dim：轨道单色灰、不画进度、数值灰——语义不变。
//!
//! 布局：表盘半径由调用方传入的 `radius_cap` 统一约束（而非各自按列宽浮动），
//! 从而保证多张卡片等高；水平居中，调用方用 `ui.columns(n)` 即可让 n 个表盘等距铺开。

use eframe::egui::{Align2, Color32, FontId, Pos2, Response, Sense, Shape, Stroke, Ui, Vec2};
use crate::app::accent_for_bg;

/// 区间亮色（进度弧 / 游标 / 数值）
pub const ZONE_OK: Color32 = Color32::from_rgb(0x4C, 0xE0, 0x70);
pub const ZONE_WARN: Color32 = Color32::from_rgb(0xFF, 0xD2, 0x3F);
pub const ZONE_CRIT: Color32 = Color32::from_rgb(0xFF, 0x5B, 0x5B);
// 卡片标题霓虹色（CPU 绿 / GPU 蓝）
pub const CPU_TITLE: Color32 = Color32::from_rgb(0x4C, 0xE0, 0x70);
pub const GPU_TITLE: Color32 = Color32::from_rgb(0x29, 0xB6, 0xF6);
/// 内存卡片标题色（橙，与表盘区间色无关）
pub const MEM_TITLE: Color32 = Color32::from_rgb(0xFF, 0xAA, 0x00);

const TRACK: Color32 = Color32::from_rgb(42, 42, 53);
const LIGHT_TRACK: Color32 = Color32::from_rgb(214, 216, 224);
const MUTED: Color32 = Color32::from_rgb(138, 138, 157);
const LIGHT_MUTED: Color32 = Color32::from_rgb(110, 118, 135);
// 深色主题下三段轨道的暗版（"标尺"底色）
const TRACK_OK: Color32 = Color32::from_rgb(0x1C, 0x4A, 0x2A);
const TRACK_WARN: Color32 = Color32::from_rgb(0x4D, 0x41, 0x17);
const TRACK_CRIT: Color32 = Color32::from_rgb(0x4D, 0x1F, 0x1F);

/// 颜色向白色混合 `t`（0 = 原色，1 = 纯白）；浅色主题下把轨道淡化成浅底标尺。
fn blend_white(c: Color32, t: f32) -> Color32 {
    let f = |v: u8| ((v as f32) * (1.0 - t) + 255.0 * t).min(255.0) as u8;
    Color32::from_rgb(f(c.r()), f(c.g()), f(c.b()))
}

/// 画一个三段色半圆表盘，返回 `Response`（点击切换指标掩码）。
///
/// * `value` 为 `None` 时显示 `—`，不画进度弧。
/// * `max` 为量程上限（温度/负载/显存/内存 = 100，风扇 RPM = 3000 或 % = 100）。
/// * `warn` / `crit` 为阈值（绝对值，与 `max` 同单位），`warn < crit`。
/// * `enabled` 控制是否"已选中上报"；关闭时整体 dim。
#[allow(clippy::too_many_arguments)]
pub fn gauge(
    ui: &mut Ui,
    label: &str,
    unit: &str,
    value: Option<f32>,
    max: f32,
    warn: f32,
    crit: f32,
    enabled: bool,
    radius_cap: f32,
) -> Response {
    let pad: f32 = 6.0; // 给弧线宽度留的边距
    let col_w = ui.available_width().max(40.0);
    let radius = ((col_w - 2.0 * pad) / 2.0).clamp(20.0, radius_cap);
    let arc_stroke = (radius * 0.22).clamp(7.0, 11.0);
    let val_font = (radius * 0.34).clamp(12.0, 18.0);
    // 高度 = 半圆 + 底部标签区；整个容器（含左右留白）都可点击
    let h = radius + 28.0;
    let (rect, resp) = ui.allocate_exact_size(Vec2::new(col_w, h), Sense::click());
    let painter = ui.painter_at(rect);
    let cx = rect.center().x;
    let cy = rect.top() + radius + pad;

    let dark = ui.visuals().dark_mode;
    let track = if dark { TRACK } else { LIGHT_TRACK };
    let muted = if dark { MUTED } else { LIGHT_MUTED };
    let cursor = if dark { Color32::WHITE } else { Color32::from_rgb(40, 44, 52) };

    // 半圆角度：180°(左) → 0°(右)，向上凸（屏幕 y 向下，故减 sin）
    let arc = |deg: f32| -> Pos2 {
        let a = deg.to_radians();
        Pos2::new(cx + radius * a.cos(), cy - radius * a.sin())
    };
    let steps = 64;
    // 按"比例区间 [p0, p1]"取一段弧顶点
    let arc_seg = |p0: f32, p1: f32| -> Vec<Pos2> {
        let span = (p1 - p0).max(0.0);
        let n = (span * steps as f32).round().max(2.0) as i32;
        (0..=n)
            .map(|i| {
                let t = p0 + span * (i as f32 / n as f32);
                arc(180.0 - t * 180.0)
            })
            .collect()
    };

    // 阈值 → 比例（clamp 到 [0,1]，并保证 crit 段不越过 warn 段）
    let m = max.max(1.0);
    let w0 = (warn / m).clamp(0.0, 1.0);
    let c0 = (crit / m).clamp(w0, 1.0);

    // ---- 轨道：三段暗色标尺（未选中时退回单色灰） ----
    if enabled {
        let (tok, twa, tcr) = if dark {
            (TRACK_OK, TRACK_WARN, TRACK_CRIT)
        } else {
            (
                blend_white(ZONE_OK, 0.72),
                blend_white(ZONE_WARN, 0.62),
                blend_white(ZONE_CRIT, 0.68),
            )
        };
        painter.add(Shape::line(arc_seg(0.0, w0), Stroke::new(arc_stroke, tok)));
        painter.add(Shape::line(arc_seg(w0, c0), Stroke::new(arc_stroke, twa)));
        painter.add(Shape::line(arc_seg(c0, 1.0), Stroke::new(arc_stroke, tcr)));
        // 折线端点是平头(butt)：只在轨道最左/最右两端补圆头，段与段之间保持平接
        // （段间补圆会互相压色、把分界糊掉）。
        let cap = arc_stroke * 0.5;
        painter.circle_filled(arc(180.0), cap, tok);
        painter.circle_filled(arc(0.0), cap, tcr);
    } else {
        painter.add(Shape::line(arc_seg(0.0, 1.0), Stroke::new(arc_stroke, track)));
    }

    // ---- 当前值所在区间的亮色（用动画后的值判断，颜色随游标同步过渡） ----
    let zone = match value {
        Some(v) if v >= crit => ZONE_CRIT,
        Some(v) if v >= warn => ZONE_WARN,
        _ => ZONE_OK,
    };
    let active = if enabled { accent_for_bg(zone, dark) } else { track };

    let pct = match value {
        Some(v) => (v / m).clamp(0.0, 1.0),
        None => 0.0,
    };
    if enabled && pct > 0.001 {
        // 进度弧是折线，端点默认平头：起止两端各补一个「直径 = 线宽」的圆点做出 round cap，
        // 末端再叠白色游标点（浅色底用深色游标）。
        let cap = arc_stroke * 0.5;
        painter.add(Shape::line(arc_seg(0.0, pct), Stroke::new(arc_stroke, active)));
        painter.circle_filled(arc(180.0), cap, active);
        let end = arc(180.0 - pct * 180.0);
        painter.circle_filled(end, cap, active);
        painter.circle_filled(end, arc_stroke * 0.32, cursor);
    }

    // 中心数值：放在半圆"盆底"最宽处，宽字符串也不压弧
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
