//! The look: the in-game menu's colours (runtime/ui/menu.cpp style_init: dark green panels, gold accents) and fonts
//! (Roboto Medium from runtime/ui/fonts, Apache-2.0), with the system's fonts behind it for symbols and CJK text.
use eframe::egui::{self, Color32, CornerRadius, FontData, FontDefinitions, FontFamily, FontId, Stroke, TextStyle};
use std::sync::Arc;

pub const GOLD: Color32 = Color32::from_rgb(237, 194, 84);
pub const GREEN: Color32 = Color32::from_rgb(38, 92, 64);
pub const GREEN_BRIGHT: Color32 = Color32::from_rgb(54, 128, 87);
pub const BG: Color32 = Color32::from_rgb(13, 20, 17);
pub const PANEL: Color32 = Color32::from_rgb(17, 26, 22);
pub const CARD: Color32 = Color32::from_rgb(24, 36, 31);
pub const CARD_HOVER: Color32 = Color32::from_rgb(28, 42, 36);
pub const TEXT: Color32 = Color32::from_rgb(245, 240, 224);
pub const WEAK: Color32 = Color32::from_rgb(158, 163, 148);
pub const RED: Color32 = Color32::from_rgb(232, 110, 90);
pub const BUTTON: Color32 = Color32::from_rgb(36, 56, 46);

const ROBOTO: &[u8] = include_bytes!("../../runtime/ui/fonts/Roboto-Medium.ttf");

/// System font files that fill what Roboto lacks: symbols, Chinese, Japanese and Korean (the language names of the
/// game's language list). Missing files are skipped.
const FALLBACKS: &[&str] = &[
    "C:\\Windows\\Fonts\\seguisym.ttf",
    "C:\\Windows\\Fonts\\msyh.ttc",
    "C:\\Windows\\Fonts\\malgun.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
];
const MONO: &[&str] = &["C:\\Windows\\Fonts\\consola.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"];

pub fn install(ctx: &egui::Context) {
    let mut fonts = FontDefinitions::empty();
    fonts.font_data.insert("roboto".into(), Arc::new(FontData::from_static(ROBOTO)));
    let mut prop = vec!["roboto".to_string()];
    for (i, path) in FALLBACKS.iter().enumerate() {
        if let Ok(bytes) = std::fs::read(path) {
            let name = format!("fallback{i}");
            fonts.font_data.insert(name.clone(), Arc::new(FontData::from_owned(bytes)));
            prop.push(name);
        }
    }
    let mut mono = Vec::new();
    for path in MONO {
        if let Ok(bytes) = std::fs::read(path) {
            fonts.font_data.insert("mono".into(), Arc::new(FontData::from_owned(bytes)));
            mono.push("mono".to_string());
            break;
        }
    }
    mono.extend(prop.iter().cloned());
    fonts.families.insert(FontFamily::Proportional, prop);
    fonts.families.insert(FontFamily::Monospace, mono);
    ctx.set_fonts(fonts);

    ctx.set_theme(egui::ThemePreference::Dark);
    ctx.all_styles_mut(|style| {
        style.text_styles = [
            (TextStyle::Heading, FontId::proportional(22.0)),
            (TextStyle::Body, FontId::proportional(15.0)),
            (TextStyle::Button, FontId::proportional(15.0)),
            (TextStyle::Small, FontId::proportional(12.5)),
            (TextStyle::Monospace, FontId::monospace(12.5)),
        ]
        .into();
        style.spacing.item_spacing = egui::vec2(10.0, 8.0);
        style.spacing.button_padding = egui::vec2(12.0, 6.0);
        style.spacing.interact_size.y = 28.0;
        style.spacing.slider_width = 210.0;
        style.spacing.combo_width = 230.0;
        style.spacing.scroll.bar_width = 10.0;
        let v = &mut style.visuals;
        *v = egui::Visuals::dark();
        v.panel_fill = PANEL;
        v.window_fill = Color32::from_rgb(20, 31, 26);
        v.extreme_bg_color = Color32::from_rgb(10, 16, 13);
        v.faint_bg_color = CARD;
        v.override_text_color = Some(TEXT);
        v.hyperlink_color = GOLD;
        v.selection.bg_fill = Color32::from_rgb(54, 112, 79);
        v.selection.stroke = Stroke::new(1.5, GOLD);
        v.window_corner_radius = CornerRadius::same(12);
        v.menu_corner_radius = CornerRadius::same(8);
        v.window_stroke = Stroke::new(1.0, Color32::from_rgb(52, 70, 60));
        let r = CornerRadius::same(8);
        let w = &mut v.widgets;
        w.noninteractive.bg_fill = PANEL;
        w.noninteractive.weak_bg_fill = PANEL;
        w.noninteractive.bg_stroke = Stroke::new(1.0, Color32::from_rgb(52, 66, 58));
        w.noninteractive.fg_stroke = Stroke::new(1.0, TEXT);
        w.noninteractive.corner_radius = r;
        for (state, fill) in [(&mut w.inactive, BUTTON), (&mut w.hovered, GREEN), (&mut w.active, GREEN_BRIGHT), (&mut w.open, GREEN)] {
            state.bg_fill = fill;
            state.weak_bg_fill = fill;
            state.corner_radius = r;
            state.fg_stroke = Stroke::new(1.2, TEXT);
            state.bg_stroke = Stroke::NONE;
        }
        w.hovered.bg_stroke = Stroke::new(1.0, Color32::from_rgb(90, 140, 110));
        w.active.bg_stroke = Stroke::new(1.0, GOLD);
    });
}

/// A pill switch (egui's own demo toggle, in the theme's colours).
pub fn toggle(ui: &mut egui::Ui, on: &mut bool) -> egui::Response {
    let size = egui::vec2(46.0, 24.0);
    let (rect, mut resp) = ui.allocate_exact_size(size, egui::Sense::click());
    if resp.clicked() {
        *on = !*on;
        resp.mark_changed();
    }
    if ui.is_rect_visible(rect) {
        let t = ui.ctx().animate_bool(resp.id, *on);
        let off = Color32::from_rgb(58, 68, 62);
        let bg = lerp_color(off, GREEN_BRIGHT, t);
        let radius = 0.5 * rect.height();
        let stroke = if resp.hovered() { Stroke::new(1.0, GOLD) } else { Stroke::NONE };
        ui.painter().rect(rect, radius, bg, stroke, egui::StrokeKind::Inside);
        let x = egui::lerp(rect.left() + radius..=rect.right() - radius, t);
        let knob = lerp_color(Color32::from_rgb(200, 200, 190), GOLD, t);
        ui.painter().circle_filled(egui::pos2(x, rect.center().y), radius - 4.0, knob);
    }
    resp
}

fn lerp_color(a: Color32, b: Color32, t: f32) -> Color32 {
    let l = |x: u8, y: u8| (x as f32 + (y as f32 - x as f32) * t).round() as u8;
    Color32::from_rgb(l(a.r(), b.r()), l(a.g(), b.g()), l(a.b(), b.b()))
}

/// A small rounded label.
pub fn badge(ui: &mut egui::Ui, text: &str, fg: Color32, bg: Color32) -> egui::Response {
    egui::Frame::new()
        .fill(bg)
        .corner_radius(CornerRadius::same(5))
        .inner_margin(egui::Margin::symmetric(6, 1))
        .show(ui, |ui| ui.label(egui::RichText::new(text).small().color(fg)))
        .response
}
