// totk-config: the window (SDL3 renderer, Nuklear immediate-mode widgets). Layout: a header with the game's icon, the
// title, the game folder and the window's language; on the left a panel with the profiles and the tabs (the in-game
// menu's, then Expert and Other keys), each with its icon and the count of keys totk.ini sets there; on the right the
// rows of the tab (or of every tab while searching); at the bottom the status and Revert, Save, Save and launch.
//
// A row is a card: label and marks (set in totk.ini, unsaved, set by an environment variable), a line with the key, its
// default, its value in the Steam Deck profile and whether the game applies it at once or after a restart, the wrapped
// description, and on the right the value's widget and a Reset button when totk.ini sets the key. Colours and font
// follow the in-game menu (ui/menu.cpp style_init, ui/option_rows.cpp); icons are drawn shapes (cfgr_icons.c).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "configurator/cfgr.h"
#include "configurator/cfgr_icons.h"
#include "configurator/cfgr_nk.h"
#include "input_map.h"
#include "platform/platform.h"

#ifndef CR_VERSION
#define CR_VERSION ""
#endif

// Roboto Medium (Apache-2.0, ui/fonts/LICENSE-Roboto.txt), the in-game menu's font.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
static const unsigned char roboto_ttf[] = {
#embed "../ui/fonts/Roboto-Medium.ttf"
};
#pragma clang diagnostic pop

// the game's icon, 64x64 RGBA (tools/build_configurator.py icon_source, from runtime/totk.ico)
extern const int cfgr_icon_px;
extern const unsigned char cfgr_icon_rgba[];

#define RGB(r, g, b) nk_rgb(r, g, b)
static const struct nk_color C_BG = {13, 20, 17, 255}, C_PANEL = {18, 28, 23, 255}, C_POPUP = {24, 37, 31, 255},
                             C_CARD = {24, 36, 31, 255}, C_CARD_HI = {29, 44, 37, 255}, C_LINE = {40, 57, 48, 255},
                             C_TEXT = {245, 240, 224, 255}, C_DIM = {150, 156, 142, 255}, C_DESC = {196, 198, 184, 255},
                             C_GOLD = {236, 194, 84, 255}, C_BUTTON = {36, 56, 46, 255}, C_HOVER = {46, 74, 60, 255},
                             C_ACTIVE = {54, 128, 87, 255}, C_GREEN = {54, 128, 87, 255}, C_GREEN_HI = {66, 150, 103, 255},
                             C_SEL = {38, 70, 53, 255}, C_WARN = {242, 168, 102, 255}, C_ENV = {120, 170, 230, 255},
                             C_EDIT = {10, 16, 13, 255}, C_RED = {232, 110, 90, 255}, C_NONE = {0, 0, 0, 0};

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static struct nk_context *g_ctx;
static struct nk_font_atlas *g_atlas;
static struct nk_font *g_body, *g_small, *g_head, *g_title;
static float g_scale = 1, g_font_scale;
static int g_font_cjk = -1;
static int g_quit;
static struct nk_image g_icon;
static int g_have_icon;

// what the window shows
static int g_tab;
static char g_search[128];
static int g_only_set;
static char g_status[512];
static Uint64 g_status_until;
static int g_status_bad;
static int g_scroll_reset = 1;
static Uint64 g_next_file_check;
static int g_file_changed;

enum { POP_NONE, POP_NEW, POP_RENAME, POP_DELETE, POP_CLOSE };
static int g_pop;
static char g_pop_name[CR_NAME];
static int g_pop_preset;
static nk_bool g_pop_activate = 1;
static int g_pop_error;
int cr_shot_popup;   // --popup: the screenshot shows this dialog (POP_*)

// a text field per row: its buffer while it has the cursor, filled from the value otherwise
typedef struct { char buf[512]; int active; } Edit;
static Edit *g_edits;
static char g_other_key[128], g_other_value[512];
static char g_default_binds[64][160];

// folder dialogs answer on another thread: the path waits here for the next frame
static SDL_Mutex *g_pick_lock;
static char g_pick_path[CR_PATH];
static int g_pick_row = -2;   // -2 nothing, -1 the game folder, else a row index

static float S(float v) { return v * g_scale; }
static struct nk_color alpha(struct nk_color c, int a) { return nk_rgba(c.r, c.g, c.b, a); }

static void status(int bad, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void status(int bad, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof g_status, fmt, ap);
    va_end(ap);
    g_status_bad = bad;
    g_status_until = SDL_GetTicks() + 6000;
}

// ---- fonts: Roboto for Latin and Cyrillic; for Simplified Chinese the system's CJK font (platform.h plat_font_path),
// baked with the glyphs of that language's texts only, as the in-game menu does (ui/menu.cpp fonts_up)
static int menu_is_cjk(void) { return cr_text(UI_MENU_TITLE) == ui_text_in("zh-Hans", UI_MENU_TITLE); }

static nk_rune *g_cjk_ranges;
static int g_cjk_n, g_cjk_cap;
static unsigned char *g_cjk_font;
static size_t g_cjk_size;

static void add_rune(nk_rune r) {
    for (int i = 0; i < g_cjk_n; i++)
        if (g_cjk_ranges[i] == r) return;
    if (g_cjk_n == g_cjk_cap) {
        g_cjk_cap = g_cjk_cap ? g_cjk_cap * 2 : 1024;
        g_cjk_ranges = realloc(g_cjk_ranges, sizeof *g_cjk_ranges * (size_t)g_cjk_cap);
    }
    g_cjk_ranges[g_cjk_n++] = r;
}

static void add_text_runes(void *ctx, const char *text) {
    (void)ctx;
    for (int len; *text; text += len) {
        nk_rune r;
        len = nk_utf_decode(text, &r, 4);
        if (!len) break;
        if (r > 0x2000) add_rune(r);
    }
}

static int rune_cmp(const void *a, const void *b) { return *(const nk_rune *)a > *(const nk_rune *)b ? 1 : -1; }

// pairs of [r, r] per glyph, after the Latin and Cyrillic ranges, zero-terminated
static const nk_rune *cjk_ranges(void) {
    static nk_rune *out;
    g_cjk_n = 0;
    ui_text_visit("zh-Hans", add_text_runes, NULL);
    add_text_runes(NULL, "…×—–«»·");
    qsort(g_cjk_ranges, (size_t)g_cjk_n, sizeof *g_cjk_ranges, rune_cmp);
    free(out);
    out = malloc(sizeof *out * (size_t)(g_cjk_n * 2 + 8));
    int k = 0;
    out[k++] = 0x20, out[k++] = 0x24F;
    for (int i = 0; i < g_cjk_n; i++) out[k++] = g_cjk_ranges[i], out[k++] = g_cjk_ranges[i];
    out[k] = 0;
    return out;
}

static void fonts_build(void) {
    int cjk = menu_is_cjk();
    if (g_font_scale == g_scale && g_font_cjk == cjk) return;
    static const nk_rune latin[] = {0x20, 0x24F, 0x370, 0x3FF, 0x400, 0x52F, 0x2010, 0x2027, 0x2030, 0x205E,
                                    0x2190, 0x21FF, 0};
    const void *data = roboto_ttf;
    size_t size = sizeof roboto_ttf;
    const nk_rune *ranges = latin;
    if (cjk) {   // without a system CJK font the Chinese text shows as boxes, as in the game's menu
        char path[CR_PATH];
        if (!g_cjk_font && plat_font_path("zh-Hans", path, sizeof path)) {
            uint64_t n = 0;
            g_cjk_font = plat_file_read(path, &n);
            g_cjk_size = (size_t)n;
        }
        if (g_cjk_font) data = g_cjk_font, size = g_cjk_size, ranges = cjk_ranges();
    }
    if (g_atlas) nk_font_atlas_clear(g_atlas);
    g_atlas = nk_sdl_font_stash_begin(g_ctx);
    struct nk_font_config cfg = nk_font_config(0);
    cfg.range = ranges;
    cfg.oversample_h = 2, cfg.oversample_v = 1;
    g_body = nk_font_atlas_add_from_memory(g_atlas, (void *)data, size, roundf(S(16.5f)), &cfg);
    g_small = nk_font_atlas_add_from_memory(g_atlas, (void *)data, size, roundf(S(13.5f)), &cfg);
    g_head = nk_font_atlas_add_from_memory(g_atlas, (void *)data, size, roundf(S(19)), &cfg);
    g_title = nk_font_atlas_add_from_memory(g_atlas, (void *)data, size, roundf(S(23)), &cfg);
    nk_sdl_font_stash_end(g_ctx);
    nk_style_set_font(g_ctx, &g_body->handle);
    g_font_scale = g_scale, g_font_cjk = cjk;
}

// ---- style: the in-game menu's colours
static void style_apply(void) {
    struct nk_color table[NK_COLOR_COUNT];
    for (int i = 0; i < NK_COLOR_COUNT; i++) table[i] = C_TEXT;
    table[NK_COLOR_WINDOW] = C_POPUP;   // popups, combo lists, tooltips; the window and its panels push C_NONE
    table[NK_COLOR_HEADER] = C_PANEL;
    table[NK_COLOR_BORDER] = C_LINE;
    table[NK_COLOR_BUTTON] = C_BUTTON;
    table[NK_COLOR_BUTTON_HOVER] = C_HOVER;
    table[NK_COLOR_BUTTON_ACTIVE] = C_ACTIVE;
    table[NK_COLOR_TOGGLE] = C_EDIT;
    table[NK_COLOR_TOGGLE_HOVER] = RGB(30, 44, 37);
    table[NK_COLOR_TOGGLE_CURSOR] = C_GOLD;
    table[NK_COLOR_SELECT] = C_BUTTON;
    table[NK_COLOR_SELECT_ACTIVE] = C_SEL;
    table[NK_COLOR_SLIDER] = RGB(44, 62, 52);
    table[NK_COLOR_SLIDER_CURSOR] = C_GOLD;
    table[NK_COLOR_SLIDER_CURSOR_HOVER] = RGB(246, 214, 120);
    table[NK_COLOR_SLIDER_CURSOR_ACTIVE] = RGB(255, 226, 140);
    table[NK_COLOR_PROPERTY] = C_EDIT;
    table[NK_COLOR_EDIT] = C_EDIT;
    table[NK_COLOR_EDIT_CURSOR] = C_GOLD;
    table[NK_COLOR_COMBO] = C_BUTTON;
    table[NK_COLOR_CHART] = C_PANEL;
    table[NK_COLOR_CHART_COLOR] = C_GOLD;
    table[NK_COLOR_CHART_COLOR_HIGHLIGHT] = C_GOLD;
    table[NK_COLOR_SCROLLBAR] = C_NONE;
    table[NK_COLOR_SCROLLBAR_CURSOR] = RGB(52, 74, 62);
    table[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = C_HOVER;
    table[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = C_ACTIVE;
    table[NK_COLOR_TAB_HEADER] = C_PANEL;
    table[NK_COLOR_KNOB] = C_BUTTON;
    table[NK_COLOR_KNOB_CURSOR] = C_GOLD;
    table[NK_COLOR_KNOB_CURSOR_HOVER] = C_GOLD;
    table[NK_COLOR_KNOB_CURSOR_ACTIVE] = C_GOLD;
    nk_style_from_table(g_ctx, table);
    struct nk_style *s = &g_ctx->style;
    s->window.padding = nk_vec2(S(16), S(10));
    s->window.spacing = nk_vec2(S(8), S(8));
    s->window.group_padding = nk_vec2(S(4), S(4));
    s->window.popup_padding = nk_vec2(S(6), S(6));
    s->window.combo_padding = nk_vec2(S(6), S(6));
    s->window.tooltip_padding = nk_vec2(S(10), S(6));
    s->window.tooltip_border = S(1);
    s->window.tooltip_border_color = C_LINE;
    s->window.combo_border = S(1);
    s->window.combo_border_color = C_LINE;
    s->window.popup_border = S(1);
    s->window.popup_border_color = C_LINE;
    s->window.rounding = S(8);
    s->window.scrollbar_size = nk_vec2(S(10), S(10));
    s->window.group_border = 0;
    s->window.border = 0;
    s->button.rounding = S(8);
    s->button.padding = nk_vec2(S(10), S(4));
    s->button.border = 0;
    s->combo.rounding = S(8);
    s->combo.border = 0;
    s->combo.normal = nk_style_item_color(C_BUTTON);
    s->combo.hover = nk_style_item_color(C_HOVER);
    s->combo.active = nk_style_item_color(C_HOVER);
    s->combo.symbol_normal = C_DIM;
    s->combo.symbol_hover = C_GOLD;
    s->combo.symbol_active = C_GOLD;
    s->combo.sym_normal = NK_SYMBOL_TRIANGLE_DOWN;
    s->combo.sym_hover = NK_SYMBOL_TRIANGLE_DOWN;
    s->combo.sym_active = NK_SYMBOL_TRIANGLE_UP;
    s->combo.button.normal = nk_style_item_color(C_NONE);
    s->combo.button.hover = nk_style_item_color(C_NONE);
    s->combo.button.active = nk_style_item_color(C_NONE);
    s->combo.content_padding = nk_vec2(S(12), S(4));
    s->combo.button_padding = nk_vec2(S(8), S(10));
    s->combo.spacing = nk_vec2(S(4), 0);
    s->contextual_button.padding = nk_vec2(S(10), S(4));
    s->contextual_button.rounding = S(6);
    s->contextual_button.hover = nk_style_item_color(C_HOVER);
    s->contextual_button.active = nk_style_item_color(C_SEL);
    s->edit.rounding = S(8);
    s->edit.border = S(1);
    s->edit.border_color = C_LINE;
    s->edit.padding = nk_vec2(S(10), S(4));
    s->slider.rounding = S(4);
    s->slider.cursor_size = nk_vec2(S(18), S(18));
    s->slider.bar_height = S(6);
    s->slider.bar_normal = RGB(44, 62, 52);
    s->slider.bar_hover = RGB(52, 74, 62);
    s->slider.bar_active = RGB(52, 74, 62);
    s->slider.bar_filled = C_GREEN;
    s->selectable.rounding = S(8);
    s->selectable.padding = nk_vec2(S(10), S(4));
    s->selectable.normal = nk_style_item_color(C_NONE);
    s->selectable.hover = nk_style_item_color(C_BUTTON);
    s->selectable.pressed = nk_style_item_color(C_HOVER);
    s->selectable.normal_active = nk_style_item_color(C_SEL);
    s->selectable.hover_active = nk_style_item_color(C_SEL);
    s->selectable.pressed_active = nk_style_item_color(C_SEL);
    s->selectable.text_normal = C_TEXT;
    s->selectable.text_hover = C_TEXT;
    s->selectable.text_normal_active = C_GOLD;
    s->selectable.text_hover_active = C_GOLD;
    s->checkbox.padding = nk_vec2(S(3), S(3));
    s->scrollv.rounding = S(5);
    s->scrollv.rounding_cursor = S(5);
    s->scrollv.border = 0;
}

// ---- text helpers
static float text_w(const struct nk_font *f, const char *s, int n) {
    if (n < 0) n = (int)strlen(s);
    return f->handle.width(f->handle.userdata, f->handle.height, s, n);
}

// Greedy line breaks of s for width w: after a space, or between two characters of a script without spaces (CJK).
// Fills out[] with up to max lines; returns the line count.
typedef struct { const char *p; int n; } Span;
static int wrap(const struct nk_font *f, const char *s, float w, Span *out, int max) {
    int lines = 0;
    while (*s) {
        if (*s == '\n') { s++; continue; }
        const char *p = s, *brk = NULL;
        float x = 0;
        while (*p && *p != '\n') {
            nk_rune r;
            int len = nk_utf_decode(p, &r, 4);
            if (!len) len = 1;
            float cw = text_w(f, p, len);
            if (x + cw > w && p > s) break;
            x += cw;
            p += len;
            if (r == ' ' || r >= 0x2E80) brk = p;
        }
        const char *end = *p && *p != '\n' && brk ? brk : p;
        if (lines < max) out[lines] = (Span){s, (int)(end - s)};
        lines++;
        s = end;
        while (*s == ' ') s++;
    }
    return lines;
}

static void draw_text(struct nk_command_buffer *c, const struct nk_font *f, float x, float y, float w, const char *s, int n,
                      struct nk_color col) {
    if (n < 0) n = (int)strlen(s);
    nk_draw_text(c, nk_rect(x, y, w, f->handle.height + S(4)), s, n, &f->handle, C_NONE, col);
}

// text vertically centred in r, from its left edge
static void draw_text_mid(struct nk_command_buffer *c, const struct nk_font *f, struct nk_rect r, const char *s,
                          struct nk_color col) {
    draw_text(c, f, r.x, r.y + (r.h - f->handle.height) / 2 - S(1), r.w, s, -1, col);
}

// The end of s that fits in w, with "…" in front when cut: a path keeps its last folders.
static const char *fit_tail(const struct nk_font *f, const char *s, float w, char *buf, size_t cap) {
    if (text_w(f, s, -1) <= w) return s;
    const char *p = s;
    while (*p) {
        p++;
        while ((*p & 0xC0) == 0x80) p++;
        snprintf(buf, cap, "…%s", p);
        if (text_w(f, buf, -1) <= w) return buf;
    }
    return "…";
}

// The start of s that fits in w, with "…" after it when cut.
static const char *fit_head(const struct nk_font *f, const char *s, float w, char *buf, size_t cap) {
    if (text_w(f, s, -1) <= w) return s;
    size_t n = strlen(s);
    while (n > 0) {
        n--;
        while (n > 0 && (s[n] & 0xC0) == 0x80) n--;
        snprintf(buf, cap, "%.*s…", (int)n, s);
        if (text_w(f, buf, -1) <= w) return buf;
    }
    return "…";
}

static int contains(const char *hay, const char *needle) {
    if (!hay) return 0;
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!SDL_strncasecmp(hay, needle, n)) return 1;
    return 0;
}

// ---- drawn widgets: a button whose look the window draws itself (Nuklear does the click), switches, icons
// Fills the next slot with nothing. (nk_spacing on a one-column row also allocates the row after it.)
static void blank(void) { nk_label(g_ctx, "", NK_TEXT_LEFT); }

static int hovering(struct nk_rect r) {
    struct nk_window *w = g_ctx->current;
    return !w->widgets_disabled && !(w->popup.win && w->popup.active && w != w->popup.win) &&
           nk_input_is_mouse_hovering_rect(&g_ctx->input, r);
}

static void tip(struct nk_rect r, const char *text) {
    if (text && *text && hovering(r)) nk_tooltip(g_ctx, text);
}

// Takes the next widget slot as an invisible button; returns its click. Its bounds go to *b.
static int hit(struct nk_rect *b) {
    static struct nk_style_button ghost;
    ghost = g_ctx->style.button;
    ghost.normal = ghost.hover = ghost.active = nk_style_item_color(C_NONE);
    ghost.border = 0;
    *b = nk_widget_bounds(g_ctx);
    return nk_button_label_styled(g_ctx, &ghost, "");
}

enum { BTN_NORMAL, BTN_PRIMARY, BTN_GO, BTN_GHOST, BTN_DANGER, BTN_SELECTED };

static float btn_w(const char *label, Icon icon) {
    float w = (label && *label ? text_w(g_body, label, -1) : 0) + S(28);
    if (icon) w += S(16) + (label && *label ? S(8) : 0);
    return label && *label ? w : S(34);
}

// A button with an optional icon before its label. kind: the colours (primary gold for Save, green for launching).
static int btn(const char *label, Icon icon, int kind) {
    struct nk_rect b;
    int dis = g_ctx->current->widgets_disabled;
    int click = hit(&b);
    int hov = hovering(b), down = hov && nk_input_is_mouse_down(&g_ctx->input, NK_BUTTON_LEFT);
    struct nk_color fill = C_BUTTON, text = C_TEXT;
    switch (kind) {
    case BTN_PRIMARY: fill = hov ? RGB(246, 210, 110) : C_GOLD, text = C_BG; break;
    case BTN_GO: fill = hov ? C_GREEN_HI : C_GREEN; break;
    case BTN_GHOST: fill = hov ? C_BUTTON : C_NONE, text = hov ? C_TEXT : C_DIM; break;
    case BTN_DANGER: fill = hov ? RGB(120, 50, 42) : C_BUTTON, text = C_RED; break;
    case BTN_SELECTED: fill = C_SEL, text = C_GOLD; break;
    default: fill = hov ? C_HOVER : C_BUTTON; break;
    }
    if (down && kind != BTN_GHOST) fill = kind == BTN_PRIMARY ? RGB(214, 170, 64) : C_ACTIVE;
    if (dis) {
        fill = kind == BTN_GHOST ? C_NONE : RGB(28, 42, 35);
        text = RGB(96, 104, 96);
    }
    struct nk_command_buffer *c = nk_window_get_canvas(g_ctx);
    if (fill.a) nk_fill_rect(c, b, S(8), fill);
    int has_label = label && *label;
    float iw = icon ? S(16) : 0, gap = icon && has_label ? S(8) : 0, tw = has_label ? text_w(g_body, label, -1) : 0;
    float x = b.x + (b.w - iw - gap - tw) / 2;
    if (icon) icon_draw(c, icon, nk_rect(x, b.y + (b.h - iw) / 2, iw, iw), text);
    if (has_label) draw_text_mid(c, g_body, nk_rect(x + iw + gap, b.y, tw + S(4), b.h), label, text);
    return click;
}

// a pill switch; returns its click
static int switch_widget(int on) {
    struct nk_rect b;
    int dis = g_ctx->current->widgets_disabled;
    int click = hit(&b);
    int hov = hovering(b);
    float w = S(44), h = S(24);
    struct nk_rect p = nk_rect(b.x + b.w - w, b.y + (b.h - h) / 2, w, h);
    struct nk_command_buffer *c = nk_window_get_canvas(g_ctx);
    struct nk_color bg = on ? (hov ? C_GREEN_HI : C_GREEN) : (hov ? RGB(70, 82, 75) : RGB(56, 66, 60));
    if (dis) bg = RGB(40, 48, 44);
    nk_fill_rect(c, p, h / 2, bg);
    if (hov) nk_stroke_rect(c, p, h / 2, S(1), alpha(C_GOLD, 160));
    float r = h / 2 - S(4), cx = on ? p.x + p.w - h / 2 : p.x + h / 2;
    nk_fill_circle(c, nk_rect(cx - r, p.y + h / 2 - r, 2 * r, 2 * r), dis ? RGB(110, 112, 104) : on ? C_GOLD : RGB(206, 206, 196));
    return click;
}

static void icon_at(Icon icon, float x, float y, float size, struct nk_color col) {
    icon_draw(nk_window_get_canvas(g_ctx), icon, nk_rect(x, y, size, size), col);
}

// a small rounded label at x, y (the top of a line of the small font); returns its width
static float pill(float x, float y, const char *text, struct nk_color fg, struct nk_color bg) {
    float w = text_w(g_small, text, -1) + S(12), h = g_small->handle.height + S(4);
    struct nk_command_buffer *c = nk_window_get_canvas(g_ctx);
    nk_fill_rect(c, nk_rect(x, y, w, h), S(5), bg);
    draw_text(c, g_small, x + S(6), y + S(1), w, text, -1, fg);
    return w;
}

// a full-width line of the current layout
static void separator(float pad) {
    nk_layout_row_dynamic(g_ctx, pad * 2 + S(1), 1);
    struct nk_rect b = nk_widget_bounds(g_ctx);
    blank();
    nk_fill_rect(nk_window_get_canvas(g_ctx), nk_rect(b.x, b.y + pad, b.w, S(1)), 0, C_LINE);
}

// ---- choices of a row: what the menu row offers, in its order
typedef struct { const char *value; const char *name; } Item;
enum { MAX_ITEMS = 64 };

static int items_of(const CrRow *r, Item *it) {
    int n = 0;
    static char lang_names[32][64];
    if (r->flags & CR_DYNAMIC) {
        if (!strcmp(r->key, "LANG")) {
            UiGameLang langs[24];
            int k = ui_text_game_langs(143, langs, 24);
            it[n++] = (Item){"", cr_text(UI_MENU_GAME_LANG_AUTO)};
            for (int i = 0; i < k && n < MAX_ITEMS; i++) it[n++] = (Item){langs[i].code, cr_text(langs[i].name)};
        } else if (!strcmp(r->key, "AUDIO_DEVICE")) {
            it[n++] = (Item){"", cr_text(UI_MENU_AUDIO_DEVICE_DEFAULT)};
            static int audio = -1;
            if (audio < 0) audio = SDL_InitSubSystem(SDL_INIT_AUDIO);
            int count = 0;
            SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&count);
            for (int i = 0; ids && i < count && n < 32; i++) {
                const char *name = SDL_GetAudioDeviceName(ids[i]);
                if (!name) continue;
                snprintf(lang_names[n], sizeof lang_names[n], "%s", name);
                it[n] = (Item){lang_names[n], lang_names[n]};
                n++;
            }
            SDL_free(ids);
        } else if (!strcmp(r->key, "GPU_INDEX")) {
            static const char *const idx[] = {"0", "1", "2", "3"};
            it[n++] = (Item){"", cr_text(UI_MENU_GPU_AUTO)};
            for (int i = 0; i < 4; i++) it[n++] = (Item){idx[i], idx[i]};
        }
        return n;
    }
    if (r->n_choices && r->choices[0].value) {
        for (int i = 0; i < r->n_choices && n < MAX_ITEMS; i++) {
            const CrChoice *c = &r->choices[i];
            it[n++] = (Item){c->value, c->name >= 0 ? cr_text(c->name) : c->text ? c->text : c->value};
        }
        return n;
    }
    // opt_choice: one name per value of the cfg_meta list, Automatic (unset) first when the key has no fixed default
    const CfgMeta *m = cfg_meta(r->key);
    static char vals[MAX_ITEMS][48];
    int k = 0;
    if (m && !*m->def) it[n++] = (Item){"", NULL};
    for (const char *p = m && m->values ? m->values : ""; *p && n < MAX_ITEMS; k++) {
        size_t len = strcspn(p, "|");
        snprintf(vals[k], sizeof vals[k], "%.*s", (int)len, p);
        it[n++] = (Item){vals[k], NULL};
        p += len + (p[len] == '|');
    }
    for (int i = 0; i < n; i++) {
        const CrChoice *c = i < r->n_choices ? &r->choices[i] : NULL;
        it[i].name = c && c->name >= 0 ? cr_text(c->name) : c && c->text ? c->text : *it[i].value ? it[i].value
                                                                                                    : cr_text(UI_MENU_AUTO);
    }
    return n;
}

static const char *shown_value(const CrRow *r, const char *value, char *buf, size_t cap) {
    if (r->kind == CR_TOGGLE) {
        int on = strcmp(value, "0") != 0 && *value;
        if (r->flags & CR_INVERTED) on = !on;
        return cr_text(on ? UI_MENU_ON : UI_MENU_OFF);
    }
    if (r->kind == CR_CHOICE || (r->kind == CR_TEXT && r->n_choices)) {
        Item it[MAX_ITEMS];
        int n = items_of(r, it);
        for (int i = 0; i < n; i++)
            if (!strcmp(it[i].value, value)) return it[i].name;
    }
    if (r->kind == CR_SLIDER && *value) {
        snprintf(buf, cap, r->fmt ? r->fmt : "%g", atof(value));
        return buf;
    }
    return *value ? value : cr_text(UI_CFG_UNSET);
}

static void lower_key(char *out, size_t cap, const char *key) {
    size_t i = 0;
    for (; key[i] && i + 1 < cap; i++) out[i] = (char)((key[i] >= 'A' && key[i] <= 'Z') ? key[i] + 32 : key[i]);
    out[i] = 0;
}

// the value the Steam Deck profile gives key (cfg.c DECK), NULL none
static const char *deck_value(const char *key) {
    const char *k, *v;
    for (int i = 0; cfg_deck_at(i, &k, &v); i++)
        if (!strcmp(k, key)) return v;
    return NULL;
}

static const CrRow *row_of(const char *key) {
    for (int i = 0; i < cr_row_count; i++)
        if (!strcmp(cr_rows[i].key, key)) return &cr_rows[i];
    return NULL;
}

// ---- widgets
// The height of a combo's list of n items: the item rows with the popup's spacing and padding.
static float combo_h(int n) {
    const struct nk_style *st = &g_ctx->style;
    return (S(28) + st->window.spacing.y) * (float)n + st->window.combo_padding.y * 2 + S(8);
}

static void folder_picked(void *userdata, const char *const *files, int filter) {
    (void)filter;
    SDL_LockMutex(g_pick_lock);
    if (files && files[0]) {
        snprintf(g_pick_path, sizeof g_pick_path, "%s", files[0]);
        g_pick_row = (int)(intptr_t)userdata;
    }
    SDL_UnlockMutex(g_pick_lock);
}

static void pick_folder(int row, const char *start) {
    SDL_ShowOpenFolderDialog(folder_picked, (void *)(intptr_t)row, g_win, start && *start ? start : model_dir(), false);
}

static void set_value(const CrRow *r, const char *v) { model_set(r->key, v); }

// Nuklear names a text field by its place among the window's text fields (nk_window edit.seq) and drops the focus when
// their number changes between frames. A row drawn off screen still takes its place (skip_edits), and every frame ends
// on the same count (EDIT_SLOTS), so the field with the cursor keeps it while the search filters rows or the page
// scrolls.
enum { EDIT_SLOTS = 4096 };
static int is_text_row(const CrRow *r) {
    return r->kind == CR_TEXT || r->kind == CR_KEY || r->kind == CR_FOLDER || r->kind == CR_BIND;
}
static void skip_edits(int n) { g_ctx->current->edit.seq += (unsigned)n; }

// The segments of a choice of up to four values, right-aligned in w as one control; returns the clicked index or -1.
static int segments(const Item *it, int n, int sel, struct nk_rect w, float total, const float *bw) {
    float x = w.x + w.w - total, h = w.h;
    struct nk_command_buffer *c = nk_window_get_canvas(g_ctx);
    struct nk_rect all = nk_layout_space_rect_to_screen(g_ctx, nk_rect(x, w.y, total, h));
    nk_fill_rect(c, all, S(8), g_ctx->current->widgets_disabled ? RGB(22, 32, 27) : C_EDIT);
    nk_stroke_rect(c, all, S(8), S(1), C_LINE);
    int got = -1;
    for (int i = 0; i < n; i++) {
        nk_layout_space_push(g_ctx, nk_rect(x, w.y, bw[i], h));
        struct nk_rect b;
        if (hit(&b) && i != sel) got = i;
        int hov = hovering(b), dis = g_ctx->current->widgets_disabled;
        struct nk_rect in = nk_rect(b.x + S(3), b.y + S(3), b.w - S(6), b.h - S(6));
        if (i == sel) nk_fill_rect(c, in, S(6), dis ? RGB(34, 48, 41) : C_SEL);
        else if (hov) nk_fill_rect(c, in, S(6), C_BUTTON);
        struct nk_color tc = dis ? RGB(110, 114, 106) : i == sel ? C_GOLD : hov ? C_TEXT : C_DIM;
        float tw = text_w(g_body, it[i].name, -1);
        draw_text_mid(c, g_body, nk_rect(b.x + (b.w - tw) / 2, b.y, tw + S(4), b.h), it[i].name, tc);
        x += bw[i];
    }
    return got;
}

// The value's widget in the rect (local to the row's layout space). Changes go through model_set.
static void value_widget(int idx, const CrRow *r, struct nk_rect w, int disabled) {
    const char *value = model_value(r);
    Edit *e = &g_edits[idx];
    if (disabled) nk_widget_disable_begin(g_ctx);
    float h = S(32);
    switch (r->kind) {
    case CR_TOGGLE: {
        int on = strcmp(value, "0") != 0 && *value;
        if (r->flags & CR_INVERTED) on = !on;
        const char *word = cr_text(on ? UI_MENU_ON : UI_MENU_OFF);
        float tw = text_w(g_body, word, -1), sw = S(44);
        nk_layout_space_push(g_ctx, nk_rect(w.x + w.w - sw - tw - S(12), w.y, tw + S(12) + sw, h));
        struct nk_rect b = nk_widget_bounds(g_ctx);
        if (switch_widget(on)) set_value(r, on == !(r->flags & CR_INVERTED) ? "0" : "1");
        draw_text_mid(nk_window_get_canvas(g_ctx), g_body, nk_rect(b.x, b.y, tw + S(4), b.h), word,
                      disabled ? C_DIM : on ? C_GOLD : C_DIM);
        break;
    }
    case CR_CHOICE: {
        Item it[MAX_ITEMS + 1];
        int n = items_of(r, it), sel = -1;
        const char *own = model_own(r->key);
        if (r->flags & CR_SET_OR_NOT) sel = own && *own ? 1 : 0;
        else
            for (int i = 0; i < n; i++)
                if (!strcmp(it[i].value, value) || (!*it[i].value && !(own && *own) && !cfg_meta(r->key)) ||
                    (!*it[i].value && !*value))
                    sel = sel < 0 ? i : sel;
        if (sel < 0 && n < MAX_ITEMS + 1) it[n] = (Item){value, value}, sel = n++;   // a value the list lacks, as written
        float total = 0, bw[4];
        for (int i = 0; i < n && i < 4; i++) total += bw[i] = fmaxf(S(62), text_w(g_body, it[i].name, -1) + S(30));
        if (n <= 4 && total <= w.w && !(r->flags & CR_DYNAMIC)) {
            int got = segments(it, n, sel, nk_rect(w.x, w.y, w.w, h), total, bw);
            if (got >= 0) set_value(r, it[got].value);
        } else {
            const char *names[MAX_ITEMS + 1];
            for (int i = 0; i < n; i++) names[i] = it[i].name;
            nk_layout_space_push(g_ctx, nk_rect(w.x, w.y, w.w, h));
            int got = nk_combo(g_ctx, names, n, sel, (int)S(28), nk_vec2(w.w, combo_h(n < 12 ? n : 12)));
            if (got != sel) set_value(r, it[got].value);
        }
        break;
    }
    case CR_SLIDER: {
        float v = (float)atof(value), step = r->step > 0 ? r->step : 1;
        char shown[64];
        snprintf(shown, sizeof shown, r->fmt ? r->fmt : "%g", (double)v);
        float tw = S(80);
        nk_layout_space_push(g_ctx, nk_rect(w.x, w.y, w.w - tw - S(10), h));
        float before = v;
        if (nk_slider_float(g_ctx, r->lo, &v, r->hi, step) && v != before) {
            char out[48];
            int digits = step >= 1 ? 0 : step >= 0.1f ? 1 : 2;
            snprintf(out, sizeof out, "%.*f", digits, (double)v);
            set_value(r, out);
        }
        // the value in a box at the right
        struct nk_rect vb = nk_layout_space_rect_to_screen(g_ctx, nk_rect(w.x + w.w - tw, w.y + S(2), tw, h - S(4)));
        struct nk_command_buffer *c = nk_window_get_canvas(g_ctx);
        nk_fill_rect(c, vb, S(6), C_EDIT);
        float sw = text_w(g_body, shown, -1);
        draw_text_mid(c, g_body, nk_rect(vb.x + (vb.w - sw) / 2, vb.y, sw + S(4), vb.h), shown, disabled ? C_DIM : C_TEXT);
        break;
    }
    case CR_FOLDER:
    case CR_TEXT:
    case CR_KEY:
    case CR_BIND: {
        float side = r->kind == CR_FOLDER ? btn_w(cr_text(UI_CFG_BROWSE), ICON_FOLDER) : r->n_choices ? S(150) : 0;
        if (!e->active) snprintf(e->buf, sizeof e->buf, "%s", value);
        nk_layout_space_push(g_ctx, nk_rect(w.x, w.y, w.w - side - (side ? S(8) : 0), h));
        nk_flags f = nk_edit_string_zero_terminated(g_ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, e->buf, sizeof e->buf,
                                                    NULL);
        if (f & NK_EDIT_ACTIVATED) e->active = 1;
        // Enter, a click elsewhere, or the focus gone with the field off screen: the text is the value
        if (e->active && (f & (NK_EDIT_COMMITED | NK_EDIT_DEACTIVATED | NK_EDIT_INACTIVE))) {
            if (strcmp(e->buf, value)) set_value(r, e->buf);
            e->active = 0;
            if (f & NK_EDIT_COMMITED) nk_edit_unfocus(g_ctx);
        }
        if (r->kind == CR_FOLDER) {
            nk_layout_space_push(g_ctx, nk_rect(w.x + w.w - side, w.y, side, h));
            if (btn(cr_text(UI_CFG_BROWSE), ICON_FOLDER, BTN_NORMAL)) pick_folder(idx, model_own(r->key));
        } else if (r->n_choices) {   // suggestions (window sizes)
            Item it[MAX_ITEMS];
            int n = items_of(r, it), sel = -1;
            const char *names[MAX_ITEMS];
            for (int i = 0; i < n; i++) {
                names[i] = it[i].name;
                if (!strcmp(it[i].value, value)) sel = i;
            }
            nk_layout_space_push(g_ctx, nk_rect(w.x + w.w - side, w.y, side, h));
            if (nk_combo_begin_label(g_ctx, sel >= 0 ? names[sel] : "…", nk_vec2(side + S(40), combo_h(n)))) {
                nk_layout_row_dynamic(g_ctx, S(28), 1);
                for (int i = 0; i < n; i++)
                    if (nk_combo_item_label(g_ctx, names[i], NK_TEXT_LEFT)) set_value(r, it[i].value), e->active = 0;
                nk_combo_end(g_ctx);
            }
        }
        break;
    }
    }
    if (disabled) nk_widget_disable_end(g_ctx);
}

// ---- rows
static int row_matches(const CrRow *r) {
    if (g_only_set && !model_own(r->key)) return 0;
    if (!g_search[0]) return 1;
    return contains(r->key, g_search) || contains(cr_text(r->label), g_search) || contains(cr_text(r->desc), g_search) ||
           contains(r->doc, g_search);
}

static const char *row_desc(const CrRow *r, char *buf, size_t cap) {
    if (r->kind == CR_BIND) {
        int a = -1;
        for (int i = 0; i < ACTION_COUNT; i++)
            if (input_action_name(i) && !strcmp(input_action_name(i), r->key + 5)) a = i;
        snprintf(buf, cap, "%s ", cr_text(r->desc));
        size_t n = strlen(buf);
        snprintf(buf + n, cap - n, cr_text(UI_CFG_BIND_HELP), a >= 0 ? g_default_binds[a] : "");
        return buf;
    }
    if (r->desc >= 0) return cr_text(r->desc);
    return r->doc ? r->doc : "";
}

static void heading(const char *text) {
    nk_layout_row_dynamic(g_ctx, S(40), 1);
    struct nk_rect b = nk_widget_bounds(g_ctx);
    blank();
    struct nk_command_buffer *c = nk_window_get_canvas(g_ctx);
    draw_text(c, g_head, b.x + S(2), b.y + S(12), b.w, text, -1, C_GOLD);
}

// A wrapped note, or a tinted box with an icon when icon is set.
static void note(const char *text, struct nk_color col, Icon icon) {
    struct nk_rect area = nk_window_get_content_region(g_ctx);
    float inset = icon ? S(44) : S(4), w = area.w - g_ctx->style.window.scrollbar_size.x - inset - S(20);
    Span lines[32];
    int n = wrap(g_body, text, w, lines, 32);
    if (n > 32) n = 32;
    float lh = g_body->handle.height + S(4), pad = icon ? S(12) : S(4);
    nk_layout_row_dynamic(g_ctx, lh * (float)n + pad * 2, 1);
    struct nk_rect b = nk_widget_bounds(g_ctx);
    blank();
    struct nk_command_buffer *c = nk_window_get_canvas(g_ctx);
    if (icon) {
        b.w -= g_ctx->style.window.scrollbar_size.x + S(8);
        nk_fill_rect(c, b, S(10), alpha(col, 26));
        nk_stroke_rect(c, b, S(10), S(1), alpha(col, 90));
        icon_draw(c, icon, nk_rect(b.x + S(14), b.y + pad + (lh - S(18)) / 2, S(18), S(18)), col);
    }
    for (int i = 0; i < n; i++)
        draw_text(c, g_body, b.x + inset, b.y + pad + lh * (float)i, w, lines[i].p, lines[i].n, icon ? C_TEXT : col);
}

// A piece of a row's second line: text with an optional icon before it and a tooltip.
typedef struct { const char *text; struct nk_color col; Icon icon; const char *tip; } Piece;

static float piece_w(const Piece *p) {
    float is = g_small->handle.height - S(1);
    return (p->icon ? is + (*p->text ? S(5) : 0) : 0) + (*p->text ? text_w(g_small, p->text, -1) : 0);
}

// Lays the pieces out left to right in w, starting a new line where the next one does not fit; their offsets go to
// x[] and y[]. Returns the line count.
static int flow(const Piece *p, int n, float w, float lh, float *x, float *y) {
    float cx = 0, cy = 0, gap = S(14);
    int lines = n ? 1 : 0;
    for (int i = 0; i < n; i++) {
        float pw = piece_w(&p[i]);
        if (cx > 0 && cx + pw > w) cx = 0, cy += lh, lines++;
        x[i] = cx, y[i] = cy;
        cx += pw + gap;
    }
    return lines;
}

static void draw_row(int idx) {
    const CrRow *r = &cr_rows[idx];
    char envname[160], lk[160], dbuf[2048], vbuf[64], tbuf[256];
    snprintf(envname, sizeof envname, "TOTK_%s", r->key);
    int env = cfg_from_env(envname);
    const char *own = model_own(r->key);
    int set = own != NULL, changed = model_changed(r->key);
    const CfgMeta *m = cfg_meta(r->key);
    struct nk_rect area = nk_window_get_content_region(g_ctx);
    float pad = S(16), cw = area.w - g_ctx->style.window.scrollbar_size.x - S(10);
    float rw = set && !env ? S(34) + S(8) : 0;   // Reset
    float ww = fminf(S(430), cw * 0.44f), tw = cw - ww - rw - pad * 3;
    lower_key(lk, sizeof lk, r->key);
    const char *label = r->label >= 0 && r->kind != CR_FOLDER ? cr_text(r->label) : lk;
    int show_key = r->label >= 0 && r->kind != CR_FOLDER;
    const char *desc = row_desc(r, dbuf, sizeof dbuf);
    Span lines[48];
    int nl = wrap(g_small, desc, tw, lines, 48);
    if (nl > 48) nl = 48;
    float lh_small = g_small->handle.height + S(4);
    float title_h = g_body->handle.height + S(8);
    // the line under it: key, default, Steam Deck value; it wraps between its pieces
    Piece pc[4];
    int np = 0;
    char dtext[256], deck[256], v2[64];
    if (show_key) pc[np++] = (Piece){lk, RGB(124, 150, 132), ICON_NONE, NULL};
    const char *def = model_default(r);
    if (*def || m) {
        snprintf(dtext, sizeof dtext, cr_text(UI_CFG_DEFAULT),
                 *def ? shown_value(r, def, vbuf, sizeof vbuf) : cr_text(UI_MENU_AUTO));
        pc[np++] = (Piece){dtext, C_DIM, ICON_NONE, NULL};
    }
    const char *dv = deck_value(r->key);
    if (dv) {
        snprintf(deck, sizeof deck, cr_text(UI_CFG_DECK_VALUE), shown_value(r, dv, v2, sizeof v2));
        pc[np++] = (Piece){deck, C_DIM, ICON_NONE, NULL};
    }
    float mx[4], my[4];
    int meta_lines = flow(pc, np, tw, lh_small, mx, my);
    float meta_h = lh_small * (float)meta_lines + S(4);
    float h = pad + title_h + meta_h + (float)nl * lh_small + pad - S(4);
    float wh = S(32) + pad * 2;
    if (h < wh) h = wh;
    nk_layout_space_begin(g_ctx, NK_STATIC, h, 12);
    struct nk_rect b = nk_layout_space_rect_to_screen(g_ctx, nk_rect(0, 0, cw, h));
    struct nk_rect clip = g_ctx->current->layout->clip;
    if (b.y + b.h < clip.y || b.y > clip.y + clip.h) {   // off screen: the space only
        nk_layout_space_end(g_ctx);
        if (is_text_row(r)) skip_edits(1);
        return;
    }
    struct nk_command_buffer *c = nk_window_get_canvas(g_ctx);
    int hover = hovering(b);
    nk_fill_rect(c, b, S(10), hover ? C_CARD_HI : C_CARD);
    if (set && !env) nk_fill_rect(c, nk_rect(b.x, b.y + S(8), S(4), b.h - S(16)), S(2), C_GOLD);
    float x = b.x + pad, y = b.y + pad - S(2);
    // the label and its marks
    const char *shown = fit_head(g_body, label, tw, tbuf, sizeof tbuf);
    draw_text(c, g_body, x, y, tw, shown, -1, env ? C_DIM : C_TEXT);
    float lx = x + text_w(g_body, shown, -1) + S(10), py = y + (g_body->handle.height - g_small->handle.height) / 2 - S(1);
    if (env && lx < x + tw) lx += pill(lx, py, cr_text(UI_MENU_SET_BY_ENV), C_BG, C_ENV) + S(6);
    else if (set && lx < x + tw) lx += pill(lx, py, cr_text(UI_CFG_SET_MARK), C_BG, C_GOLD) + S(6);
    // whether the game's own menu applies a change at once (cfg.c LIVE) or the game needs a restart
    float is = g_small->handle.height;
    if (m && m->live && lx + is < x + tw) {
        icon_at(ICON_BOLT, lx, py + S(2), is, RGB(150, 190, 140));
        tip(nk_rect(lx, py, is, is + S(4)), cr_text(UI_CFG_LIVE));
        lx += is + S(8);
    } else if (m && lx + is < x + tw) {   // a pill with the words, or the icon alone where they do not fit
        const char *rt = cr_text(UI_MENU_AFTER_RESTART);
        float pw = is + S(5) + text_w(g_small, rt, -1) + S(14), ph = is + S(4);
        if (lx + pw > x + tw) rt = "", pw = is + S(12);
        tip(nk_rect(lx, py, pw, ph), cr_text(UI_MENU_AFTER_RESTART));
        nk_fill_rect(c, nk_rect(lx, py, pw, ph), S(5), alpha(C_WARN, 28));
        icon_at(ICON_RESTART, lx + S(6), py + S(2), is - S(1), C_WARN);
        draw_text(c, g_small, lx + S(6) + is + S(4), py + S(1), pw, rt, -1, C_WARN);
        lx += pw + S(6);
    }
    if (changed && lx < x + tw) {
        icon_at(ICON_DOT, lx, py + S(1), is, C_WARN);
        tip(nk_rect(lx, py, is, is), cr_text(UI_CFG_UNSAVED_ONE));
    }
    y += title_h;
    for (int i = 0; i < np; i++) {
        float ix = x + mx[i], iy = y + my[i], is = g_small->handle.height - S(1);
        if (pc[i].icon) icon_draw(c, pc[i].icon, nk_rect(ix, iy + S(1), is, is), pc[i].col), ix += is + S(5);
        if (*pc[i].text) draw_text(c, g_small, ix, iy, x + tw - ix, pc[i].text, -1, pc[i].col);
        if (pc[i].tip) tip(nk_rect(x + mx[i] - S(2), iy - S(2), piece_w(&pc[i]) + S(4), lh_small), pc[i].tip);
    }
    y += meta_h;
    for (int i = 0; i < nl; i++) draw_text(c, g_small, x, y + lh_small * (float)i, tw, lines[i].p, lines[i].n, C_DESC);
    // the widget, right-aligned, and Reset when totk.ini sets the key
    float wx = b.w - pad - rw - ww, wy = pad - S(2);
    value_widget(idx, r, nk_rect(wx, wy, ww, S(32)), env);
    if (set && !env) {
        nk_layout_space_push(g_ctx, nk_rect(b.w - pad - S(34), wy, S(34), S(32)));
        struct nk_rect rb = nk_widget_bounds(g_ctx);
        if (btn("", ICON_RESET, BTN_NORMAL)) model_reset(r->key), g_edits[idx].active = 0;
        tip(rb, cr_text(UI_CFG_RESET_ROW));
    }
    nk_layout_space_end(g_ctx);
}

static void draw_other(void) {
    const char *keys[256], *values[256];
    int n = model_other_keys(keys, values, 256);
    for (int i = 0; i < n; i++) {
        if (g_search[0] && !contains(keys[i], g_search) && !contains(values[i], g_search)) continue;
        char k[128], lk[128];
        snprintf(k, sizeof k, "%s", keys[i]);
        lower_key(lk, sizeof lk, k);
        nk_layout_row_begin(g_ctx, NK_DYNAMIC, S(34), 3);
        nk_layout_row_push(g_ctx, 0.35f);
        nk_label(g_ctx, lk, NK_TEXT_LEFT);
        nk_layout_row_push(g_ctx, 0.5f);
        static char buf[256][512];
        static int active[256];
        if (!active[i]) snprintf(buf[i], sizeof buf[i], "%s", values[i]);
        nk_flags f = nk_edit_string_zero_terminated(g_ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, buf[i], sizeof buf[i], NULL);
        if (f & NK_EDIT_ACTIVATED) active[i] = 1;
        if (f & (NK_EDIT_COMMITED | NK_EDIT_DEACTIVATED)) {
            active[i] = 0;
            if (strcmp(buf[i], values[i])) model_set(k, buf[i]);
        }
        nk_layout_row_push(g_ctx, 0.15f);
        if (btn(cr_text(UI_CFG_RESET_ROW), ICON_RESET, BTN_NORMAL)) model_reset(k);
        nk_layout_row_end(g_ctx);
    }
    nk_layout_row_dynamic(g_ctx, S(10), 1);
    blank();
    nk_layout_row_begin(g_ctx, NK_DYNAMIC, S(34), 3);
    nk_layout_row_push(g_ctx, 0.35f);
    struct nk_rect kb = nk_widget_bounds(g_ctx);
    nk_flags kf = nk_edit_string_zero_terminated(g_ctx, NK_EDIT_FIELD, g_other_key, sizeof g_other_key, nk_filter_ascii);
    if (!g_other_key[0] && (kf & NK_EDIT_INACTIVE))
        draw_text_mid(nk_window_get_canvas(g_ctx), g_body, nk_rect(kb.x + S(10), kb.y, kb.w, kb.h), cr_text(UI_CFG_KEY), C_DIM);
    nk_layout_row_push(g_ctx, 0.5f);
    struct nk_rect vb = nk_widget_bounds(g_ctx);
    nk_flags vf = nk_edit_string_zero_terminated(g_ctx, NK_EDIT_FIELD, g_other_value, sizeof g_other_value, NULL);
    if (!g_other_value[0] && (vf & NK_EDIT_INACTIVE))
        draw_text_mid(nk_window_get_canvas(g_ctx), g_body, nk_rect(vb.x + S(10), vb.y, vb.w, vb.h), cr_text(UI_CFG_VALUE),
                      C_DIM);
    nk_layout_row_push(g_ctx, 0.15f);
    if (!g_other_key[0]) nk_widget_disable_begin(g_ctx);
    if (btn(cr_text(UI_CFG_ADD), ICON_PLUS, BTN_NORMAL) && g_other_key[0]) {
        model_set(g_other_key, g_other_value);
        g_other_key[0] = g_other_value[0] = 0;
    }
    if (!g_other_key[0]) nk_widget_disable_end(g_ctx);
    nk_layout_row_end(g_ctx);
}

static const char *tab_name(int t) {
    static const UiTextId ids[CR_TAB_COUNT] = {UI_MENU_TAB_SCREEN, UI_MENU_TAB_GAME, UI_MENU_TAB_SOUND,
                                               UI_MENU_TAB_CONTROLS, UI_MENU_TAB_AMIIBO, UI_MENU_TAB_MENU,
                                               UI_MENU_TAB_ADVANCED, UI_MENU_TAB_MODS, UI_CFG_TAB_EXPERT,
                                               UI_CFG_TAB_OTHER};
    return cr_text(ids[t]);
}

static Icon tab_icon(int t) {
    static const Icon icons[CR_TAB_COUNT] = {ICON_SCREEN, ICON_GAME, ICON_SOUND, ICON_CONTROLS, ICON_AMIIBO,
                                             ICON_MENU, ICON_ADVANCED, ICON_MODS, ICON_EXPERT, ICON_OTHER};
    return icons[t];
}

static int tab_set_count(int t) {
    if (t == CR_TAB_OTHER) {
        const char *k[256], *v[256];
        return model_other_keys(k, v, 256);
    }
    int n = 0;
    for (int i = 0; i < cr_row_count; i++) n += cr_rows[i].tab == t && model_own(cr_rows[i].key) != NULL;
    return n;
}

static void draw_rows(void) {
    int shown = 0;
    if (g_tab == CR_TAB_EXPERT && !g_search[0]) note(cr_text(UI_CFG_EXPERT_NOTE), C_WARN, ICON_WARN);
    if (g_tab == CR_TAB_OTHER && !g_search[0]) {
        note(cr_text(UI_CFG_OTHER_NOTE), C_ENV, ICON_INFO);
        draw_other();
        return;
    }
    int last_tab = -1, last_group = -2;
    const char *last_doc = NULL;
    for (int i = 0; i < cr_row_count; i++) {
        const CrRow *r = &cr_rows[i];
        if (!g_search[0] && r->tab != g_tab) continue;
        if (!row_matches(r)) continue;
        if (g_search[0] && r->tab != last_tab) heading(tab_name(r->tab)), last_tab = r->tab, last_group = -2;
        if (r->tab == CR_TAB_EXPERT) {
            if (r->doc_group && (!last_doc || strcmp(last_doc, r->doc_group)) && !g_search[0]) heading(r->doc_group);
            last_doc = r->doc_group;
        } else if (r->group != last_group) {
            if (r->group >= 0) heading(cr_text(r->group));
            last_group = r->group;
        }
        draw_row(i);
        shown++;
    }
    if (g_search[0] && g_tab != CR_TAB_OTHER) {
        heading(tab_name(CR_TAB_OTHER));
        draw_other();
    }
    if (!shown) note(cr_text(UI_CFG_NOTHING), C_DIM, ICON_NONE);
    nk_layout_row_dynamic(g_ctx, S(12), 1);
    blank();
}

// ---- the header: icon, title and version, the game folder, the window's language
enum { H_HEAD = 46, H_FOOT = 40, SIDE_W = 250 };

static void header(float width) {
    nk_layout_space_begin(g_ctx, NK_STATIC, S(H_HEAD), 8);
    struct nk_rect area = nk_layout_space_rect_to_screen(g_ctx, nk_rect(0, 0, width, S(H_HEAD)));
    struct nk_command_buffer *c = nk_window_get_canvas(g_ctx);
    float x = 0, mid = S(H_HEAD) / 2;
    if (g_have_icon) nk_draw_image(c, nk_rect(area.x, area.y + mid - S(17), S(34), S(34)), &g_icon, nk_rgb(255, 255, 255));
    x += S(46);
    const char *title = cr_text(UI_CFG_TITLE);
    draw_text(c, g_title, area.x + x, area.y + mid - g_title->handle.height / 2 - S(2), width, title, -1, C_TEXT);
    x += text_w(g_title, title, -1) + S(10);
    if (*CR_VERSION) x += pill(area.x + x, area.y + mid - g_small->handle.height / 2 - S(2), CR_VERSION, C_GOLD,
                               alpha(C_GOLD, 36)) + S(28);
    // the window's language at the right: the menu language row (TOTK_MENU_LANG), as in the game
    float lw = S(270), lx = width - lw;
    const CrRow *ml = row_of("MENU_LANG");
    if (ml) {
        icon_at(ICON_GLOBE, area.x + lx - S(28), area.y + mid - S(9), S(18), C_DIM);
        Item it[MAX_ITEMS];
        int n = items_of(ml, it), cur = 0;
        const char *v = model_own("MENU_LANG");
        const char *nm[MAX_ITEMS];
        for (int k = 0; k < n; k++) {
            nm[k] = it[k].name;
            if (!strcmp(it[k].value, v ? v : "")) cur = k;
        }
        nk_layout_space_push(g_ctx, nk_rect(lx, mid - S(17), lw, S(34)));
        int pick = nk_combo(g_ctx, nm, n, cur, (int)S(28), nk_vec2(lw, combo_h(n)));
        if (pick != cur) model_set("MENU_LANG", it[pick].value);
    }
    // the game folder between them: icon, the path's end that fits, Change…
    float cbw = btn_w(cr_text(UI_CFG_CHANGE), ICON_NONE), fx = x, fend = lx - S(48) - cbw - S(10);
    if (fend - fx > S(80)) {
        icon_at(ICON_FOLDER, area.x + fx, area.y + mid - S(9), S(18), model_has_game() ? C_DIM : C_WARN);
        fx += S(28);
        char buf[CR_PATH + 8];
        const char *p = fit_tail(g_body, model_dir(), fend - fx, buf, sizeof buf);
        float pw = text_w(g_body, p, -1);
        struct nk_rect pr = nk_rect(area.x + fx, area.y, pw, S(H_HEAD));
        draw_text_mid(c, g_body, nk_rect(pr.x, pr.y, pw + S(4), pr.h), p, C_DESC);
        tip(pr, model_dir());
        nk_layout_space_push(g_ctx, nk_rect(fx + pw + S(10), mid - S(16), cbw, S(32)));
        if (btn(cr_text(UI_CFG_CHANGE), ICON_NONE, BTN_NORMAL)) pick_folder(-1, model_dir());
    }
    nk_layout_space_end(g_ctx);
}

// ---- the left panel: profiles, then the tabs
// A line of the panel: icon, label, a count at the right; returns its click.
static int nav_item(Icon icon, struct nk_color icol, const char *label, int count, int selected) {
    struct nk_rect b;
    int click = hit(&b), hov = hovering(b);
    struct nk_command_buffer *c = nk_window_get_canvas(g_ctx);
    if (selected) nk_fill_rect(c, b, S(8), C_SEL);
    else if (hov) nk_fill_rect(c, b, S(8), C_BUTTON);
    if (selected) nk_fill_rect(c, nk_rect(b.x, b.y + S(8), S(3), b.h - S(16)), S(2), C_GOLD);
    float is = S(18);
    if (icon) icon_draw(c, icon, nk_rect(b.x + S(12), b.y + (b.h - is) / 2, is, is), selected ? C_GOLD : icol);
    float tx = b.x + (icon ? S(40) : S(12)), cw = 0;
    char num[16] = "";
    if (count > 0) snprintf(num, sizeof num, "%d", count), cw = text_w(g_small, num, -1) + S(14);
    char buf[256];
    const char *t = fit_head(g_body, label, b.x + b.w - tx - cw - S(10), buf, sizeof buf);
    draw_text_mid(c, g_body, nk_rect(tx, b.y, b.w, b.h), t, selected ? C_GOLD : C_TEXT);
    if (count > 0) {
        float ph = g_small->handle.height + S(4);
        struct nk_rect pr = nk_rect(b.x + b.w - cw - S(8), b.y + (b.h - ph) / 2, cw, ph);
        nk_fill_rect(c, pr, ph / 2, selected ? C_GOLD : alpha(C_GOLD, 40));
        draw_text(c, g_small, pr.x + S(7), pr.y + S(1), cw, num, -1, selected ? C_BG : C_GOLD);
    }
    return click;
}

static void side_label(const char *text) {
    nk_layout_row_dynamic(g_ctx, S(26), 1);
    struct nk_rect b = nk_widget_bounds(g_ctx);
    blank();
    draw_text(nk_window_get_canvas(g_ctx), g_small, b.x + S(4), b.y + S(6), b.w, text, -1, C_GOLD);
}

static void open_new_profile(void) {
    g_pop = POP_NEW, g_pop_name[0] = 0, g_pop_preset = PRESET_CURRENT, g_pop_error = 0;
}

static void sidebar(void) {
    side_label(cr_text(UI_CFG_PROFILES));
    char names[CR_MAX_PROFILES][CR_NAME];
    int np = model_profiles(names, CR_MAX_PROFILES);
    const char *active = model_active_profile();
    if (!np) {
        Span lines[8];
        struct nk_rect area = nk_window_get_content_region(g_ctx);
        float w = area.w - S(12);
        const char *t = cr_text(UI_CFG_NO_PROFILE_NOTE);
        int n = wrap(g_small, t, w, lines, 8);
        if (n > 8) n = 8;
        float lh = g_small->handle.height + S(4);
        nk_layout_row_dynamic(g_ctx, lh * (float)n + S(4), 1);
        struct nk_rect b = nk_widget_bounds(g_ctx);
        blank();
        for (int i = 0; i < n; i++)
            draw_text(nk_window_get_canvas(g_ctx), g_small, b.x + S(4), b.y + lh * (float)i, w, lines[i].p, lines[i].n,
                      C_DIM);
    } else {
        nk_layout_row_dynamic(g_ctx, S(30), 1);
        for (int i = -1; i < np; i++) {
            const char *name = i < 0 ? cr_text(UI_CFG_NO_PROFILE) : names[i];
            int on = i < 0 ? !*active : !strcmp(names[i], active);
            if (nav_item(on ? ICON_DOT : ICON_RING, C_DIM, name, 0, on) && !on) {
                if (model_switch_profile(i < 0 ? "" : names[i])) {
                    status(0, cr_text(UI_CFG_SWITCHED), name);
                    memset(g_edits, 0, sizeof *g_edits * (size_t)cr_row_count);
                } else
                    status(1, "%s", cr_text(UI_MENU_SAVE_FAILED));
            }
        }
    }
    // + New, and Rename and Delete for the active profile
    nk_layout_row_begin(g_ctx, NK_STATIC, S(32), 3);
    struct nk_rect area = nk_window_get_content_region(g_ctx);
    float iw = S(34), nw = area.w - (*active ? (iw + S(8)) * 2 : 0) - S(4);
    nk_layout_row_push(g_ctx, nw);
    if (btn(cr_text(UI_CFG_NEW_PROFILE), ICON_PLUS, BTN_NORMAL)) open_new_profile();
    if (*active) {
        nk_layout_row_push(g_ctx, iw);
        struct nk_rect rb = nk_widget_bounds(g_ctx);
        if (btn("", ICON_PENCIL, BTN_NORMAL))
            g_pop = POP_RENAME, snprintf(g_pop_name, sizeof g_pop_name, "%s", active), g_pop_error = 0;
        tip(rb, cr_text(UI_CFG_RENAME));
        nk_layout_row_push(g_ctx, iw);
        struct nk_rect db = nk_widget_bounds(g_ctx);
        if (btn("", ICON_TRASH, BTN_DANGER)) g_pop = POP_DELETE;
        tip(db, cr_text(UI_CFG_DELETE));
    }
    nk_layout_row_end(g_ctx);
    separator(S(8));
    nk_layout_row_dynamic(g_ctx, S(34), 1);
    for (int t = 0; t < CR_TAB_COUNT; t++) {
        if (t == CR_TAB_EXPERT) {
            separator(S(6));
            nk_layout_row_dynamic(g_ctx, S(34), 1);
        }
        int on = t == g_tab && !g_search[0];
        if (nav_item(tab_icon(t), C_DIM, tab_name(t), tab_set_count(t), on)) {
            if (t != g_tab || g_search[0]) g_scroll_reset = 1;
            g_tab = t, g_search[0] = 0;
        }
    }
}

// ---- the page: banners, search, rows
static void banners(void) {
    if (!model_has_game()) {
        char warn[512];
        snprintf(warn, sizeof warn, cr_text(UI_CFG_NO_GAME), cr_game_exe);
        note(warn, C_WARN, ICON_WARN);
    }
    if (g_file_changed) {
        note(cr_text(UI_CFG_FILE_CHANGED), C_WARN, ICON_INFO);
        nk_layout_row_begin(g_ctx, NK_STATIC, S(32), 2);
        nk_layout_row_push(g_ctx, btn_w(cr_text(UI_CFG_RELOAD), ICON_RESTART));
        if (btn(cr_text(UI_CFG_RELOAD), ICON_RESTART, BTN_NORMAL)) {
            model_reload();
            g_file_changed = 0;
            memset(g_edits, 0, sizeof *g_edits * (size_t)cr_row_count);
        }
        nk_layout_row_push(g_ctx, btn_w(cr_text(UI_CFG_KEEP_MINE), ICON_NONE));
        if (btn(cr_text(UI_CFG_KEEP_MINE), ICON_NONE, BTN_GHOST)) g_file_changed = 0;
        nk_layout_row_end(g_ctx);
    }
}

static float search_bar(float width) {
    float h = S(36);
    nk_layout_space_begin(g_ctx, NK_STATIC, h, 6);
    const char *only = cr_text(UI_CFG_ONLY_SET);
    float ow = text_w(g_body, only, -1) + S(44) + S(12), sw = fminf(S(560), width - ow - S(24));
    char before[sizeof g_search];
    snprintf(before, sizeof before, "%s", g_search);
    struct nk_vec2 padding = g_ctx->style.edit.padding;
    g_ctx->style.edit.padding = nk_vec2(S(36), S(6));
    nk_layout_space_push(g_ctx, nk_rect(0, 0, sw, h));
    struct nk_rect sb = nk_widget_bounds(g_ctx);
    nk_flags sf = nk_edit_string_zero_terminated(g_ctx, NK_EDIT_FIELD, g_search, sizeof g_search, NULL);
    g_ctx->style.edit.padding = padding;
    struct nk_command_buffer *c = nk_window_get_canvas(g_ctx);
    icon_at(ICON_SEARCH, sb.x + S(12), sb.y + (h - S(16)) / 2, S(16), (sf & NK_EDIT_ACTIVE) ? C_GOLD : C_DIM);
    if (!g_search[0] && (sf & NK_EDIT_INACTIVE))
        draw_text_mid(c, g_body, nk_rect(sb.x + S(36), sb.y, sb.w, sb.h), cr_text(UI_CFG_SEARCH), C_DIM);
    if (g_search[0]) {
        nk_layout_space_push(g_ctx, nk_rect(sw - S(32), S(4), S(28), h - S(8)));
        struct nk_rect xb = nk_widget_bounds(g_ctx);
        if (btn("", ICON_CLOSE, BTN_GHOST)) g_search[0] = 0;
        tip(xb, cr_text(UI_CFG_CLEAR));
    }
    if (strcmp(before, g_search)) g_scroll_reset = 1;
    // Only set: a switch with its words
    nk_layout_space_push(g_ctx, nk_rect(sw + S(20), 0, ow, h));
    struct nk_rect ob = nk_widget_bounds(g_ctx);
    if (switch_widget(g_only_set)) g_only_set = !g_only_set, g_scroll_reset = 1;
    draw_text_mid(c, g_body, nk_rect(ob.x, ob.y, ow, ob.h), only, g_only_set ? C_GOLD : C_DIM);
    nk_layout_space_end(g_ctx);
    return h;
}

// ---- the footer: status at the left, Revert, Save, Save and launch at the right
static void footer(float width) {
    int unsaved = model_unsaved();
    char left[600];
    struct nk_color col = C_DIM;
    Icon icon = ICON_CHECK;
    if (g_status[0] && SDL_GetTicks() < g_status_until)
        snprintf(left, sizeof left, "%s", g_status), col = g_status_bad ? C_RED : C_GOLD, icon = g_status_bad ? ICON_WARN : ICON_CHECK;
    else if (unsaved) snprintf(left, sizeof left, cr_text(UI_CFG_UNSAVED), unsaved), col = C_WARN, icon = ICON_DOT;
    else {
        int n = 0;
        for (int i = 0; i < cr_row_count; i++) n += model_own(cr_rows[i].key) != NULL;
        snprintf(left, sizeof left, cr_text(UI_CFG_SET_COUNT), n), icon = ICON_INFO;
    }
    const char *t1 = cr_text(UI_CFG_REVERT), *t2 = cr_text(UI_CFG_SAVE), *t3 = cr_text(UI_CFG_SAVE_LAUNCH);
    float b1 = btn_w(t1, ICON_RESET), b2 = btn_w(t2, ICON_CHECK) + S(12), b3 = btn_w(t3, ICON_PLAY) + S(8);
    float h = S(H_FOOT), gap = S(10);
    nk_layout_space_begin(g_ctx, NK_STATIC, h, 6);
    struct nk_rect area = nk_layout_space_rect_to_screen(g_ctx, nk_rect(0, 0, width, h));
    struct nk_command_buffer *c = nk_window_get_canvas(g_ctx);
    float lw = width - b1 - b2 - b3 - gap * 3 - S(30);
    icon_draw(c, icon, nk_rect(area.x, area.y + (h - S(16)) / 2, S(16), S(16)), col);
    char buf[640];
    const char *shown = fit_head(g_body, left, lw, buf, sizeof buf);
    draw_text_mid(c, g_body, nk_rect(area.x + S(26), area.y, lw, h), shown, col);
    tip(nk_rect(area.x, area.y, lw, h), model_ini_path());
    float x = width - b3;
    nk_layout_space_push(g_ctx, nk_rect(x, S(2), b3, h - S(4)));
    if (!model_has_game()) nk_widget_disable_begin(g_ctx);
    if (btn(t3, ICON_PLAY, BTN_GO)) {
        if (!model_save()) status(1, "%s", cr_text(UI_MENU_SAVE_FAILED));
        else if (model_launch()) status(0, cr_text(UI_CFG_LAUNCHED), cr_game_exe), g_file_changed = 0;
        else status(1, cr_text(UI_CFG_LAUNCH_FAILED), cr_game_exe);
    }
    if (!model_has_game()) nk_widget_disable_end(g_ctx);
    x -= gap + b2;
    if (!unsaved) nk_widget_disable_begin(g_ctx);
    nk_layout_space_push(g_ctx, nk_rect(x, S(2), b2, h - S(4)));
    if (btn(t2, ICON_CHECK, BTN_PRIMARY)) {
        if (model_save()) status(0, cr_text(UI_CFG_SAVED), model_ini_path()), g_file_changed = 0;
        else status(1, "%s", cr_text(UI_MENU_SAVE_FAILED));
    }
    x -= gap + b1;
    nk_layout_space_push(g_ctx, nk_rect(x, S(2), b1, h - S(4)));
    if (btn(t1, ICON_RESET, BTN_GHOST)) {
        model_reload();
        memset(g_edits, 0, sizeof *g_edits * (size_t)cr_row_count);
    }
    if (!unsaved) nk_widget_disable_end(g_ctx);
    nk_layout_space_end(g_ctx);
}

// ---- popups
// A button's text without the trailing dots that say it opens a dialog, for the dialog's own title and button.
static const char *no_dots(int id) {
    static char buf[4][128];
    static int k;
    char *b = buf[k++ & 3];
    snprintf(b, sizeof buf[0], "%s", cr_text(id));
    size_t n = strlen(b);
    if (n >= 3 && !strcmp(b + n - 3, "…")) b[n - 3] = 0;   // U+2026
    else if (n >= 3 && !strcmp(b + n - 3, "...")) b[n - 3] = 0;
    return b;
}

static void popup_title(const char *text, Icon icon, struct nk_color col) {
    nk_layout_row_dynamic(g_ctx, S(40), 1);
    struct nk_rect b = nk_widget_bounds(g_ctx);
    blank();
    struct nk_command_buffer *c = nk_window_get_canvas(g_ctx);
    icon_draw(c, icon, nk_rect(b.x, b.y + (b.h - S(22)) / 2, S(22), S(22)), col);
    draw_text_mid(c, g_title, nk_rect(b.x + S(34), b.y, b.w, b.h), text, C_TEXT);
}

// What a preset changes, one line per key: the menu's name of the setting and the value, in a scrolled list.
static void preset_list(Preset p, float height) {
    int nc = 0;
    const PresetChange *pc = preset_changes(p, &nc);
    const char *keys[64], *vals[64];
    int n = 0;
    for (int i = 0; i < nc && n < 64; i++) keys[n] = pc[i].key, vals[n++] = pc[i].value;
    const char *k, *v;
    for (int i = 0; p == PRESET_DECK && cfg_deck_at(i, &k, &v) && n < 64; i++) keys[n] = k, vals[n++] = v;
    nk_layout_row_dynamic(g_ctx, height, 1);
    if (!n) {
        struct nk_rect b = nk_widget_bounds(g_ctx);
        blank();
        (void)b;
        return;
    }
    struct nk_style_window *ws = &g_ctx->style.window;
    nk_style_push_style_item(g_ctx, &ws->fixed_background, nk_style_item_color(C_EDIT));
    nk_style_push_vec2(g_ctx, &ws->group_padding, nk_vec2(S(10), S(8)));
    if (nk_group_begin(g_ctx, "preset", 0)) {
        nk_style_pop_vec2(g_ctx), nk_style_pop_style_item(g_ctx);
        nk_layout_row_dynamic(g_ctx, S(26), 2);
        for (int i = 0; i < n; i++) {
            const CrRow *r = row_of(keys[i]);
            char lk[64], vb[64];
            lower_key(lk, sizeof lk, keys[i]);
            const char *name = r && r->label >= 0 ? cr_text(r->label) : lk;
            const char *val = vals[i] ? (r ? shown_value(r, vals[i], vb, sizeof vb) : vals[i]) : cr_text(UI_CFG_UNSET);
            nk_label_colored(g_ctx, name, NK_TEXT_LEFT, C_DESC);
            nk_label_colored(g_ctx, val, NK_TEXT_LEFT, vals[i] ? C_GOLD : C_DIM);
        }
        nk_group_end(g_ctx);
    } else
        nk_style_pop_vec2(g_ctx), nk_style_pop_style_item(g_ctx);
}

static void popups(float w, float h) {
    if (g_pop == POP_NONE) return;
    float pw = fminf(S(620), w - S(40)), ph = g_pop == POP_NEW ? S(640) : g_pop == POP_RENAME ? S(250) : S(250);
    ph = fminf(ph, h - S(24));
    struct nk_rect r = nk_rect((w - pw) / 2, (h - ph) / 2, pw, ph);
    // the page behind dims
    struct nk_command_buffer *c = nk_window_get_canvas(g_ctx);
    struct nk_rect clip = c->clip;
    nk_push_scissor(c, nk_rect(0, 0, w, h));
    nk_fill_rect(c, nk_rect(0, 0, w, h), 0, nk_rgba(0, 0, 0, 130));
    nk_push_scissor(c, clip);
    struct nk_style_window *ws = &g_ctx->style.window;
    nk_style_push_vec2(g_ctx, &ws->spacing, nk_vec2(S(10), S(10)));   // popped after the popup's rows
    nk_style_push_style_item(g_ctx, &ws->fixed_background, nk_style_item_color(C_POPUP));
    nk_style_push_color(g_ctx, &ws->popup_border_color, alpha(C_GOLD, 120));
    nk_style_push_float(g_ctx, &ws->popup_border, S(1));
    nk_style_push_vec2(g_ctx, &ws->popup_padding, nk_vec2(S(24), S(18)));
    int open = nk_popup_begin(g_ctx, NK_POPUP_STATIC, "##pop", NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR, r);
    nk_style_pop_vec2(g_ctx), nk_style_pop_float(g_ctx), nk_style_pop_color(g_ctx), nk_style_pop_style_item(g_ctx);
    if (!open) {
        nk_style_pop_vec2(g_ctx);
        return;
    }
    int close = 0;
    float bw = S(150);
    if (g_pop == POP_NEW || g_pop == POP_RENAME) {
        popup_title(no_dots(g_pop == POP_NEW ? UI_CFG_NEW_PROFILE : UI_CFG_RENAME), g_pop == POP_NEW ? ICON_PLUS : ICON_PENCIL,
                    C_GOLD);
        nk_layout_row_dynamic(g_ctx, S(20), 1);
        nk_label_colored(g_ctx, cr_text(UI_CFG_NAME), NK_TEXT_LEFT, C_DIM);
        nk_layout_row_dynamic(g_ctx, S(34), 1);
        nk_edit_string_zero_terminated(g_ctx, NK_EDIT_FIELD, g_pop_name, sizeof g_pop_name, NULL);
        if (g_pop == POP_NEW) {
            nk_layout_row_dynamic(g_ctx, S(20), 1);
            nk_label_colored(g_ctx, cr_text(UI_CFG_START_FROM), NK_TEXT_LEFT, C_DIM);
            // the presets as a grid of choices
            nk_layout_row_dynamic(g_ctx, S(34), 3);
            for (int i = 0; i < PRESET_COUNT; i++)
                if (btn(cr_text(preset_label((Preset)i)), ICON_NONE, i == g_pop_preset ? BTN_SELECTED : BTN_NORMAL))
                    g_pop_preset = i;
            nk_layout_row_dynamic(g_ctx, S(44), 1);
            nk_label_colored_wrap(g_ctx, cr_text(preset_desc((Preset)g_pop_preset)), C_DESC);
            preset_list((Preset)g_pop_preset, fmaxf(S(60), ph - S(470)));
            nk_layout_row_begin(g_ctx, NK_STATIC, S(30), 2);
            nk_layout_row_push(g_ctx, S(46));
            if (switch_widget(g_pop_activate)) g_pop_activate = !g_pop_activate;
            nk_layout_row_push(g_ctx, pw - S(110));
            nk_label_colored(g_ctx, cr_text(UI_CFG_ACTIVATE), NK_TEXT_LEFT, g_pop_activate ? C_TEXT : C_DIM);
            nk_layout_row_end(g_ctx);
        }
        nk_layout_row_dynamic(g_ctx, S(22), 1);
        nk_label_colored(g_ctx, g_pop_error ? cr_text(UI_CFG_NAME_TAKEN) : "", NK_TEXT_LEFT, C_RED);
        nk_layout_row_begin(g_ctx, NK_STATIC, S(36), 3);
        nk_layout_row_push(g_ctx, pw - S(48) - bw * 2 - S(24));
        blank();
        nk_layout_row_push(g_ctx, bw);
        if (btn(cr_text(UI_MENU_RESET_NO), ICON_NONE, BTN_GHOST)) close = 1;
        nk_layout_row_push(g_ctx, bw);
        if (!g_pop_name[0]) nk_widget_disable_begin(g_ctx);
        if (btn(no_dots(g_pop == POP_NEW ? UI_CFG_CREATE : UI_CFG_RENAME), ICON_CHECK, BTN_PRIMARY)) {
            const char *active = model_active_profile();
            int ok = g_pop == POP_NEW ? model_new_profile(g_pop_name, (Preset)g_pop_preset, g_pop_activate)
                                      : model_rename_profile(active, g_pop_name);
            if (ok) {
                close = 1;
                if (g_pop == POP_NEW && g_pop_activate) status(0, cr_text(UI_CFG_SWITCHED), g_pop_name);
                memset(g_edits, 0, sizeof *g_edits * (size_t)cr_row_count);
            } else
                g_pop_error = 1;
        }
        if (!g_pop_name[0]) nk_widget_disable_end(g_ctx);
        nk_layout_row_end(g_ctx);
    } else if (g_pop == POP_DELETE || g_pop == POP_CLOSE) {
        int del = g_pop == POP_DELETE;
        popup_title(no_dots(del ? UI_CFG_DELETE : UI_CFG_SAVE), del ? ICON_TRASH : ICON_WARN, del ? C_RED : C_WARN);
        char q[512];
        if (del) snprintf(q, sizeof q, cr_text(UI_CFG_DELETE_CONFIRM), model_active_profile());
        else snprintf(q, sizeof q, "%s", cr_text(UI_CFG_DISCARD_CONFIRM));
        nk_layout_row_dynamic(g_ctx, S(70), 1);
        nk_label_colored_wrap(g_ctx, q, C_DESC);
        int nb = del ? 2 : 3;
        nk_layout_row_begin(g_ctx, NK_STATIC, S(36), 4);
        nk_layout_row_push(g_ctx, fmaxf(S(1), pw - S(48) - bw * (float)nb - S(12) * (float)nb));
        blank();
        nk_layout_row_push(g_ctx, bw);
        if (btn(cr_text(UI_MENU_RESET_NO), ICON_NONE, BTN_GHOST)) close = 1;
        if (del) {
            nk_layout_row_push(g_ctx, bw);
            if (btn(no_dots(UI_CFG_DELETE), ICON_TRASH, BTN_DANGER)) model_delete_profile(model_active_profile()), close = 1;
        } else {
            nk_layout_row_push(g_ctx, bw);
            if (btn(cr_text(UI_CFG_DISCARD), ICON_NONE, BTN_DANGER)) g_quit = 1;
            nk_layout_row_push(g_ctx, bw);
            if (btn(cr_text(UI_CFG_SAVE), ICON_CHECK, BTN_PRIMARY)) {
                if (model_save()) g_quit = 1;
                else status(1, "%s", cr_text(UI_MENU_SAVE_FAILED)), close = 1;
            }
        }
        nk_layout_row_end(g_ctx);
    }
    if (close) {
        g_pop = POP_NONE;
        nk_popup_close(g_ctx);
    }
    nk_popup_end(g_ctx);
    nk_style_pop_vec2(g_ctx);
}

// ---- the frame
// Nuklear draws a panel's background when it begins; the window and its groups push a clear one and the frame paints the
// header, the left panel and the footer itself, edge to edge.
static int group_begin(const char *name, nk_flags flags, struct nk_vec2 padding) {
    struct nk_style_window *ws = &g_ctx->style.window;
    nk_style_push_style_item(g_ctx, &ws->fixed_background, nk_style_item_color(C_NONE));
    nk_style_push_vec2(g_ctx, &ws->group_padding, padding);
    int open = nk_group_begin(g_ctx, name, flags);
    nk_style_pop_vec2(g_ctx), nk_style_pop_style_item(g_ctx);
    return open;
}

static void frame(void) {
    int pw, ph;
    SDL_GetRenderOutputSize(g_ren, &pw, &ph);
    float w = (float)pw, h = (float)ph;
    struct nk_style_window *ws = &g_ctx->style.window;
    nk_style_push_style_item(g_ctx, &ws->fixed_background, nk_style_item_color(C_NONE));
    int open = nk_begin(g_ctx, "totk-config", nk_rect(0, 0, w, h), NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BACKGROUND);
    nk_style_pop_style_item(g_ctx);
    if (open) {
        struct nk_vec2 pad = ws->padding, sp = ws->spacing;
        float inner = w - pad.x * 2;
        float body_y = pad.y + S(H_HEAD) + sp.y, foot_h = S(H_FOOT);
        float body_h = h - body_y - sp.y - foot_h - pad.y;
        float foot_y = body_y + body_h + sp.y;
        float side_r = pad.x + S(SIDE_W) + sp.x / 2;
        // the panels behind the widgets
        struct nk_command_buffer *c = nk_window_get_canvas(g_ctx);
        struct nk_rect clip = c->clip;   // the window's clip leaves out its padding; the panels go edge to edge
        nk_push_scissor(c, nk_rect(0, 0, w, h));
        nk_fill_rect(c, nk_rect(0, 0, w, body_y - sp.y / 2), 0, C_PANEL);
        nk_fill_rect(c, nk_rect(0, body_y - sp.y / 2, side_r, foot_y - body_y), 0, C_PANEL);
        nk_fill_rect(c, nk_rect(0, foot_y - sp.y / 2, w, h - foot_y + sp.y / 2), 0, C_PANEL);
        nk_fill_rect(c, nk_rect(0, body_y - sp.y / 2 - S(1), w, S(1)), 0, C_LINE);
        nk_fill_rect(c, nk_rect(side_r, body_y - sp.y / 2, S(1), foot_y - body_y), 0, C_LINE);
        nk_fill_rect(c, nk_rect(side_r, foot_y - sp.y / 2, w - side_r, S(1)), 0, C_LINE);
        nk_push_scissor(c, clip);
        header(inner);
        nk_layout_row_template_begin(g_ctx, body_h);
        nk_layout_row_template_push_static(g_ctx, S(SIDE_W) - sp.x / 2);
        nk_layout_row_template_push_dynamic(g_ctx);
        nk_layout_row_template_end(g_ctx);
        // the left panel scrolls only when its lines need more height than it has (last frame's count)
        static float side_need;
        if (group_begin("side", side_need > body_h - S(16) ? 0 : NK_WINDOW_NO_SCROLLBAR, nk_vec2(S(2), S(8)))) {
            nk_style_push_vec2(g_ctx, &ws->spacing, nk_vec2(S(6), S(4)));
            nk_layout_row_dynamic(g_ctx, S(1), 1);
            float top = nk_widget_bounds(g_ctx).y;
            blank();
            sidebar();
            nk_layout_row_dynamic(g_ctx, S(1), 1);
            side_need = nk_widget_bounds(g_ctx).y - top;
            blank();
            nk_style_pop_vec2(g_ctx);
            nk_group_end(g_ctx);
        }
        if (group_begin("page", NK_WINDOW_NO_SCROLLBAR, nk_vec2(S(14), S(10)))) {
            struct nk_rect pa = nk_window_get_content_region(g_ctx);
            float top = pa.y;
            banners();
            float sh = search_bar(pa.w - S(28));
            (void)sh;
            nk_layout_row_dynamic(g_ctx, S(2), 1);
            blank();
            struct nk_rect next = nk_widget_bounds(g_ctx);
            float rows_h = pa.y + pa.h - (next.y > top ? next.y : top) - S(14);
            nk_layout_row_dynamic(g_ctx, fmaxf(S(100), rows_h), 1);
            if (group_begin("rows", 0, nk_vec2(0, S(2)))) {
                if (g_scroll_reset) nk_group_set_scroll(g_ctx, "rows", 0, 0), g_scroll_reset = 0;
                draw_rows();
                nk_group_end(g_ctx);
            }
            nk_group_end(g_ctx);
        }
        footer(inner);
        popups(w, h);
        if (g_ctx->current->edit.seq < EDIT_SLOTS) g_ctx->current->edit.seq = EDIT_SLOTS;
    }
    nk_end(g_ctx);
}

static void handle_pick(void) {
    SDL_LockMutex(g_pick_lock);
    int row = g_pick_row;
    char path[CR_PATH];
    snprintf(path, sizeof path, "%s", g_pick_path);
    g_pick_row = -2;
    SDL_UnlockMutex(g_pick_lock);
    if (row == -2) return;
    if (row == -1) {
        if (model_unsaved()) model_save();
        model_open(path);
        memset(g_edits, 0, sizeof *g_edits * (size_t)cr_row_count);
    } else if (row >= 0 && row < cr_row_count) {
        model_set(cr_rows[row].key, path);
        g_edits[row].active = 0;
    }
}

static void update_scale(void) {
    float s = SDL_GetWindowDisplayScale(g_win);
    if (s <= 0) s = 1;
    if (s != g_scale) g_scale = s, style_apply();
}

// the game's icon for the window (its title bar and task bar; on Windows the exe's own icon too) and the header
static void icon_init(void) {
    SDL_Surface *s = SDL_CreateSurfaceFrom(cfgr_icon_px, cfgr_icon_px, SDL_PIXELFORMAT_RGBA32, (void *)cfgr_icon_rgba,
                                           cfgr_icon_px * 4);
    if (!s) return;
    SDL_SetWindowIcon(g_win, s);
    SDL_Texture *t = SDL_CreateTextureFromSurface(g_ren, s);
    SDL_DestroySurface(s);
    if (!t) return;
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    g_icon = nk_image_ptr(t);
    g_have_icon = 1;
}

int window_run(const char *shot, int tab, const char *search) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "totk-config: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    g_win = SDL_CreateWindow(cr_text(UI_CFG_TITLE), 1280, 840,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (shot ? SDL_WINDOW_HIDDEN : 0));
    if (!g_win) {
        fprintf(stderr, "totk-config: SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(g_win, 960, 600);
    g_ren = SDL_CreateRenderer(g_win, NULL);
    if (!g_ren) {
        fprintf(stderr, "totk-config: SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(g_ren, 1);
    icon_init();
    g_pick_lock = SDL_CreateMutex();
    g_ctx = nk_sdl_init(g_win, g_ren, nk_sdl_allocator());
    g_edits = calloc((size_t)cr_row_count, sizeof *g_edits);
    input_map_reset();
    for (int a = 0; a < ACTION_COUNT && a < 64; a++)
        input_map_line(&input_map, a, g_default_binds[a], sizeof g_default_binds[a]);
    if (tab >= 0 && tab < CR_TAB_COUNT) g_tab = tab;
    if (search) snprintf(g_search, sizeof g_search, "%s", search);
    if (shot && cr_shot_popup) {
        g_pop = cr_shot_popup;
        if (g_pop == POP_NEW) snprintf(g_pop_name, sizeof g_pop_name, "Travel"), g_pop_preset = PRESET_DECK;
    }
    g_scale = 0;
    update_scale();
    fonts_build();
    int frames = 0;
    while (!g_quit) {
        SDL_Event ev;
        nk_input_begin(g_ctx);
        int got = shot || SDL_HasEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST) ? SDL_PollEvent(&ev) : SDL_WaitEventTimeout(&ev, 500);
        while (got) {
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                if (model_unsaved() && !shot) g_pop = POP_CLOSE;
                else g_quit = 1;
            }
            if (ev.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) update_scale();
            SDL_ConvertEventToRenderCoordinates(g_ren, &ev);
            nk_sdl_handle_event(g_ctx, &ev);
            // a press and its release in one frame would cancel out (Nuklear looks at a key's or button's state at
            // the frame's end): the events after a press wait for the next frame
            if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                ev.type == SDL_EVENT_TEXT_INPUT)
                break;
            got = SDL_PollEvent(&ev);
        }
        nk_input_end(g_ctx);
        handle_pick();
        if (SDL_GetTicks() >= g_next_file_check) {
            g_next_file_check = SDL_GetTicks() + 1000;
            if (model_file_changed()) {
                if (!model_unsaved()) model_reload(), memset(g_edits, 0, sizeof *g_edits * (size_t)cr_row_count);
                else g_file_changed = 1;
            }
        }
        fonts_build();
        frame();
        SDL_SetRenderDrawColor(g_ren, C_BG.r, C_BG.g, C_BG.b, 255);
        SDL_RenderClear(g_ren);
        nk_sdl_render(g_ctx, NK_ANTI_ALIASING_ON);
        nk_sdl_update_TextInput(g_ctx);   // after the frame that focused or left a text field, before the next key
        if (shot && ++frames == 4) {
            SDL_Surface *s = SDL_RenderReadPixels(g_ren, NULL);
            SDL_IOStream *mem = s ? SDL_IOFromDynamicMem() : NULL;
            int ok = mem && SDL_SaveBMP_IO(s, mem, false);
            if (ok) {
                void *data = SDL_GetPointerProperty(SDL_GetIOProperties(mem), SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, NULL);
                ok = data && plat_file_write(shot, data, (size_t)SDL_GetIOSize(mem));
            }
            if (mem) SDL_CloseIO(mem);
            if (s) SDL_DestroySurface(s);
            if (!ok) fprintf(stderr, "totk-config: %s: %s\n", shot, SDL_GetError());
            g_quit = 1;
        }
        SDL_RenderPresent(g_ren);
    }
    nk_sdl_shutdown(g_ctx);
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    return 0;
}
