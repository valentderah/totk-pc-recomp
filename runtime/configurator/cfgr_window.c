// totk-config: the window (SDL3 renderer, Nuklear immediate-mode widgets). Layout: a header with the game folder, the
// profile and the window's language; the tabs of the in-game menu on the left, then Expert and Other keys; the rows of
// the tab (or of every tab while searching) on the right; Save, Save and launch, Revert at the bottom.
//
// A row is a card: label, a line with the key, its default and marks (set in totk.ini, unsaved, after restart, set by an
// environment variable), the wrapped description, and on the right the value's widget and a Reset button when totk.ini
// sets the key. Colours and font follow the in-game menu (ui/menu.cpp style_init, ui/option_rows.cpp).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "configurator/cfgr.h"
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

#define RGB(r, g, b) nk_rgb(r, g, b)
static const struct nk_color C_BG = {15, 23, 20, 255}, C_PANEL = {21, 32, 27, 255}, C_CARD = {27, 40, 34, 255},
                             C_CARD_HI = {33, 49, 41, 255}, C_TEXT = {245, 240, 224, 255}, C_DIM = {150, 150, 140, 255},
                             C_GOLD = {236, 194, 84, 255}, C_BUTTON = {36, 56, 46, 255}, C_HOVER = {38, 92, 64, 255},
                             C_ACTIVE = {54, 128, 87, 255}, C_SEL = {54, 112, 79, 255}, C_WARN = {242, 168, 102, 255},
                             C_ENV = {120, 170, 230, 255}, C_EDIT = {12, 18, 16, 255};

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static struct nk_context *g_ctx;
static struct nk_font_atlas *g_atlas;
static struct nk_font *g_body, *g_small, *g_head;
static float g_scale = 1, g_font_scale;
static int g_font_cjk = -1;
static int g_quit;

// what the window shows
static int g_tab;
static char g_search[128];
static nk_bool g_only_set;
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
    add_text_runes(NULL, "…×—–«»");
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
    g_body = nk_font_atlas_add_from_memory(g_atlas, (void *)data, size, roundf(S(17)), &cfg);
    g_small = nk_font_atlas_add_from_memory(g_atlas, (void *)data, size, roundf(S(14)), &cfg);
    g_head = nk_font_atlas_add_from_memory(g_atlas, (void *)data, size, roundf(S(22)), &cfg);
    nk_sdl_font_stash_end(g_ctx);
    nk_style_set_font(g_ctx, &g_body->handle);
    g_font_scale = g_scale, g_font_cjk = cjk;
}

// ---- style: the in-game menu's colours
static void style_apply(void) {
    struct nk_color table[NK_COLOR_COUNT];
    for (int i = 0; i < NK_COLOR_COUNT; i++) table[i] = C_TEXT;
    table[NK_COLOR_WINDOW] = C_BG;
    table[NK_COLOR_HEADER] = C_PANEL;
    table[NK_COLOR_BORDER] = RGB(60, 78, 66);
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
    table[NK_COLOR_SCROLLBAR] = C_PANEL;
    table[NK_COLOR_SCROLLBAR_CURSOR] = RGB(64, 90, 76);
    table[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = C_HOVER;
    table[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = C_ACTIVE;
    table[NK_COLOR_TAB_HEADER] = C_PANEL;
    table[NK_COLOR_KNOB] = C_BUTTON;
    table[NK_COLOR_KNOB_CURSOR] = C_GOLD;
    table[NK_COLOR_KNOB_CURSOR_HOVER] = C_GOLD;
    table[NK_COLOR_KNOB_CURSOR_ACTIVE] = C_GOLD;
    nk_style_from_table(g_ctx, table);
    struct nk_style *s = &g_ctx->style;
    s->window.padding = nk_vec2(S(14), S(10));
    s->window.spacing = nk_vec2(S(8), S(8));
    s->window.group_padding = nk_vec2(S(4), S(4));
    s->window.scrollbar_size = nk_vec2(S(12), S(12));
    s->window.group_border = 0;
    s->window.border = 0;
    s->button.rounding = S(7);
    s->button.padding = nk_vec2(S(10), S(4));
    s->button.border = 0;
    s->combo.rounding = S(7);
    s->combo.border = 0;
    s->combo.content_padding = nk_vec2(S(10), S(4));
    s->combo.button_padding = nk_vec2(S(6), S(10));
    s->combo.spacing = nk_vec2(S(4), 0);
    s->contextual_button.padding = nk_vec2(S(10), S(4));
    s->edit.rounding = S(6);
    s->edit.border = S(1);
    s->edit.border_color = RGB(60, 78, 66);
    s->edit.padding = nk_vec2(S(8), S(4));
    s->slider.rounding = S(4);
    s->slider.cursor_size = nk_vec2(S(16), S(16));
    s->slider.bar_height = S(6);
    s->slider.bar_normal = RGB(44, 62, 52);
    s->slider.bar_hover = RGB(52, 74, 62);
    s->slider.bar_active = RGB(52, 74, 62);
    s->slider.bar_filled = RGB(54, 128, 87);
    s->selectable.rounding = S(7);
    s->selectable.padding = nk_vec2(S(10), S(4));
    s->selectable.normal = nk_style_item_color(nk_rgba(0, 0, 0, 0));
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
    s->scrollv.rounding = S(6);
    s->scrollv.rounding_cursor = S(6);
}

// ---- text helpers
static float text_w(const struct nk_font *f, const char *s, int n) {
    if (n < 0) n = (int)strlen(s);
    return f->handle.width(f->handle.userdata, f->handle.height, s, n);
}

// Greedy line breaks of s for width w: after a space, or between two characters of a script without spaces (CJK).
// Calls line(start, len) per line; returns the line count.
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
    nk_draw_text(c, nk_rect(x, y, w, f->handle.height + S(4)), s, n, &f->handle, nk_rgba(0, 0, 0, 0), col);
}

static int contains(const char *hay, const char *needle) {
    if (!hay) return 0;
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!SDL_strncasecmp(hay, needle, n)) return 1;
    return 0;
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

// ---- widgets
// The height of a combo's list of n items: the item rows with the popup's spacing and padding.
static float combo_h(int n) {
    const struct nk_style *st = &g_ctx->style;
    return (S(28) + st->window.spacing.y) * (float)n + st->window.combo_padding.y * 2 + S(8);
}

static int seg_button(const char *label, int selected) {
    struct nk_style_button *b = &g_ctx->style.button;
    if (selected) {
        nk_style_push_style_item(g_ctx, &b->normal, nk_style_item_color(C_SEL));
        nk_style_push_style_item(g_ctx, &b->hover, nk_style_item_color(C_SEL));
        nk_style_push_color(g_ctx, &b->text_normal, C_GOLD);
        nk_style_push_color(g_ctx, &b->text_hover, C_GOLD);
    }
    int hit = nk_button_label(g_ctx, label);
    if (selected) {
        nk_style_pop_style_item(g_ctx), nk_style_pop_style_item(g_ctx);
        nk_style_pop_color(g_ctx), nk_style_pop_color(g_ctx);
    }
    return hit;
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

// The value's widget in the rect (local to the row's layout space). Returns nothing; changes go through model_set.
static void value_widget(int idx, const CrRow *r, struct nk_rect w, int disabled) {
    const char *value = model_value(r);
    Edit *e = &g_edits[idx];
    if (disabled) nk_widget_disable_begin(g_ctx);
    float h = S(30);
    switch (r->kind) {
    case CR_TOGGLE: {
        int on = strcmp(value, "0") != 0 && *value;
        if (r->flags & CR_INVERTED) on = !on;
        float bw = S(78);
        nk_layout_space_push(g_ctx, nk_rect(w.x + w.w - 2 * bw - S(6), w.y, bw, h));
        if (seg_button(cr_text(UI_MENU_OFF), !on) && on) set_value(r, (r->flags & CR_INVERTED) ? "1" : "0");
        nk_layout_space_push(g_ctx, nk_rect(w.x + w.w - bw, w.y, bw, h));
        if (seg_button(cr_text(UI_MENU_ON), on) && !on) set_value(r, (r->flags & CR_INVERTED) ? "0" : "1");
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
        for (int i = 0; i < n && i < 4; i++) total += bw[i] = fmaxf(S(64), text_w(g_body, it[i].name, -1) + S(34));
        total += S(6) * (float)(n - 1);
        if (n <= 4 && total <= w.w && !(r->flags & CR_DYNAMIC)) {
            float x = w.x + w.w - total;
            for (int i = 0; i < n; i++) {
                nk_layout_space_push(g_ctx, nk_rect(x, w.y, bw[i], h));
                if (seg_button(it[i].name, i == sel) && i != sel) set_value(r, it[i].value);
                x += bw[i] + S(6);
            }
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
        float tw = S(84);
        nk_layout_space_push(g_ctx, nk_rect(w.x, w.y, w.w - tw - S(8), h));
        float before = v;
        if (nk_slider_float(g_ctx, r->lo, &v, r->hi, step) && v != before) {
            char out[48];
            int digits = step >= 1 ? 0 : step >= 0.1f ? 1 : 2;
            snprintf(out, sizeof out, "%.*f", digits, (double)v);
            set_value(r, out);
        }
        nk_layout_space_push(g_ctx, nk_rect(w.x + w.w - tw, w.y, tw, h));
        nk_label_colored(g_ctx, shown, NK_TEXT_RIGHT, C_TEXT);
        break;
    }
    case CR_FOLDER:
    case CR_TEXT:
    case CR_KEY:
    case CR_BIND: {
        float side = r->kind == CR_FOLDER ? S(96) : r->n_choices ? S(150) : 0;
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
            if (nk_button_label(g_ctx, cr_text(UI_CFG_BROWSE))) pick_folder(idx, model_own(r->key));
        } else if (r->n_choices) {   // suggestions (window sizes)
            Item it[MAX_ITEMS];
            int n = items_of(r, it), sel = -1;
            const char *names[MAX_ITEMS];
            for (int i = 0; i < n; i++) {
                names[i] = it[i].name;
                if (!strcmp(it[i].value, value)) sel = i;
            }
            nk_layout_space_push(g_ctx, nk_rect(w.x + w.w - side, w.y, side, h));
            if (nk_combo_begin_label(g_ctx, sel >= 0 ? names[sel] : "…", nk_vec2(side + S(40), S(28) * (float)n + S(16)))) {
                nk_layout_row_dynamic(g_ctx, S(26), 1);
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
    nk_layout_row_dynamic(g_ctx, S(34), 1);
    struct nk_rect b = nk_widget_bounds(g_ctx);
    nk_label(g_ctx, "", NK_TEXT_LEFT);
    struct nk_command_buffer *c = nk_window_get_canvas(g_ctx);
    draw_text(c, g_head, b.x + S(4), b.y + S(6), b.w, text, -1, C_GOLD);
}

static void note(const char *text, struct nk_color col) {
    struct nk_rect area = nk_window_get_content_region(g_ctx);
    float w = area.w - S(40);
    Span lines[32];
    int n = wrap(g_body, text, w, lines, 32);
    float lh = g_body->handle.height + S(4);
    nk_layout_row_dynamic(g_ctx, lh * (float)n + S(8), 1);
    struct nk_rect b = nk_widget_bounds(g_ctx);
    nk_label(g_ctx, "", NK_TEXT_LEFT);
    struct nk_command_buffer *c = nk_window_get_canvas(g_ctx);
    for (int i = 0; i < n && i < 32; i++)
        draw_text(c, g_body, b.x + S(4), b.y + S(4) + lh * (float)i, w, lines[i].p, lines[i].n, col);
}

static void draw_row(int idx) {
    const CrRow *r = &cr_rows[idx];
    char envname[160], lk[160], dbuf[2048], vbuf[64], meta[512];
    snprintf(envname, sizeof envname, "TOTK_%s", r->key);
    int env = cfg_from_env(envname);
    const char *own = model_own(r->key);
    int set = own != NULL, changed = model_changed(r->key);
    const CfgMeta *m = cfg_meta(r->key);
    struct nk_rect area = nk_window_get_content_region(g_ctx);
    float pad = S(14), cw = area.w - g_ctx->style.window.scrollbar_size.x - S(8);
    float ww = fminf(S(420), cw * 0.42f), tw = cw - ww - pad * 3;
    lower_key(lk, sizeof lk, r->key);
    const char *label = r->label >= 0 && r->kind != CR_FOLDER ? cr_text(r->label) : lk;
    if (r->kind == CR_FOLDER && r->label >= 0) label = lk;
    const char *desc = row_desc(r, dbuf, sizeof dbuf);
    // the meta line: key, default, marks
    const char *def = model_default(r);
    int n = snprintf(meta, sizeof meta, "%s", lk);
    if (r->label < 0 || r->kind == CR_FOLDER) n = 0, meta[0] = 0;
    if (*def || (m && !*def)) {
        char d[256];
        snprintf(d, sizeof d, cr_text(UI_CFG_DEFAULT), *def ? shown_value(r, def, vbuf, sizeof vbuf) : cr_text(UI_MENU_AUTO));
        n += snprintf(meta + n, sizeof meta - (size_t)n, "%s%s", n ? "   " : "", d);
    }
    if (m && !m->live) n += snprintf(meta + n, sizeof meta - (size_t)n, "   %s", cr_text(UI_MENU_AFTER_RESTART));
    Span lines[48];
    int nl = wrap(g_small, desc, tw, lines, 48);
    if (nl > 48) nl = 48;
    float lh_small = g_small->handle.height + S(3);
    float title_h = g_body->handle.height + S(6);
    float meta_h = meta[0] ? lh_small : 0;
    float h = pad + title_h + meta_h + (float)nl * lh_small + pad;
    float wh = S(30) + pad * 2;
    if (h < wh) h = wh;
    nk_layout_space_begin(g_ctx, NK_STATIC, h, 8);
    struct nk_rect b = nk_layout_space_rect_to_screen(g_ctx, nk_rect(0, 0, cw, h));
    struct nk_rect clip = g_ctx->current->layout->clip;
    if (b.y + b.h < clip.y || b.y > clip.y + clip.h) {   // off screen: the space only
        nk_layout_space_end(g_ctx);
        if (is_text_row(r)) skip_edits(1);
        return;
    }
    struct nk_command_buffer *c = nk_window_get_canvas(g_ctx);
    int hover = nk_input_is_mouse_hovering_rect(&g_ctx->input, b);
    nk_fill_rect(c, b, S(10), hover ? C_CARD_HI : C_CARD);
    if (set && !env) nk_fill_rect(c, nk_rect(b.x, b.y + S(6), S(4), b.h - S(12)), S(2), C_GOLD);
    float x = b.x + pad, y = b.y + pad;
    draw_text(c, g_body, x, y, tw, label, -1, env ? C_DIM : C_TEXT);
    float lx = x + text_w(g_body, label, -1) + S(10);
    const char *badge = env ? cr_text(UI_MENU_SET_BY_ENV) : changed ? "●" : set ? cr_text(UI_CFG_SET_MARK) : NULL;
    if (badge && lx < x + tw) {
        float bw = text_w(g_small, badge, -1) + S(12);
        struct nk_color bc = env ? C_ENV : changed ? C_WARN : C_GOLD;
        if (!changed)
            nk_fill_rect(c, nk_rect(lx, y + S(1), bw, g_small->handle.height + S(4)), S(4), nk_rgba(bc.r, bc.g, bc.b, 40));
        draw_text(c, g_small, lx + S(6), y + S(2), bw, badge, -1, bc);
    }
    y += title_h;
    draw_text(c, g_small, x, y, tw, meta, -1, C_DIM);
    y += meta_h;
    for (int i = 0; i < nl; i++)
        draw_text(c, g_small, x, y + lh_small * (float)i, tw, lines[i].p, lines[i].n, RGB(200, 200, 188));
    // the widget, right-aligned, and Reset when totk.ini sets the key
    float rw = S(84), wx = b.w - pad - ww, wy = pad + S(2);
    struct nk_rect wr = nk_rect(wx, wy, ww - (set && !env ? rw + S(8) : 0), S(30));
    value_widget(idx, r, wr, env);
    if (set && !env) {
        nk_layout_space_push(g_ctx, nk_rect(b.w - pad - rw, wy, rw, S(30)));
        if (nk_button_label(g_ctx, cr_text(UI_CFG_RESET_ROW))) model_reset(r->key), g_edits[idx].active = 0;
    }
    nk_layout_space_end(g_ctx);
}

static void draw_other(void) {
    const char *keys[256], *values[256];
    int n = model_other_keys(keys, values, 256);
    float pad = S(10);
    for (int i = 0; i < n; i++) {
        if (g_search[0] && !contains(keys[i], g_search) && !contains(values[i], g_search)) continue;
        char k[128], lk[128];
        snprintf(k, sizeof k, "%s", keys[i]);
        lower_key(lk, sizeof lk, k);
        nk_layout_row_begin(g_ctx, NK_DYNAMIC, S(32), 3);
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
        if (nk_button_label(g_ctx, cr_text(UI_CFG_RESET_ROW))) model_reset(k);
        nk_layout_row_end(g_ctx);
    }
    nk_layout_row_dynamic(g_ctx, pad, 1);
    nk_spacing(g_ctx, 1);
    nk_layout_row_begin(g_ctx, NK_DYNAMIC, S(32), 3);
    nk_layout_row_push(g_ctx, 0.35f);
    nk_edit_string_zero_terminated(g_ctx, NK_EDIT_FIELD, g_other_key, sizeof g_other_key, nk_filter_ascii);
    nk_layout_row_push(g_ctx, 0.5f);
    nk_edit_string_zero_terminated(g_ctx, NK_EDIT_FIELD, g_other_value, sizeof g_other_value, NULL);
    nk_layout_row_push(g_ctx, 0.15f);
    if (nk_button_label(g_ctx, cr_text(UI_CFG_ADD)) && g_other_key[0]) {
        model_set(g_other_key, g_other_value);
        g_other_key[0] = g_other_value[0] = 0;
    }
    nk_layout_row_end(g_ctx);
}

static const char *tab_name(int t) {
    static const UiTextId ids[CR_TAB_COUNT] = {UI_MENU_TAB_SCREEN, UI_MENU_TAB_GAME, UI_MENU_TAB_SOUND,
                                               UI_MENU_TAB_CONTROLS, UI_MENU_TAB_AMIIBO, UI_MENU_TAB_MENU,
                                               UI_MENU_TAB_ADVANCED, UI_MENU_TAB_MODS, UI_CFG_TAB_EXPERT,
                                               UI_CFG_TAB_OTHER};
    return cr_text(ids[t]);
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
    if (g_tab == CR_TAB_EXPERT && !g_search[0]) note(cr_text(UI_CFG_EXPERT_NOTE), C_WARN);
    if (g_tab == CR_TAB_OTHER && !g_search[0]) {
        note(cr_text(UI_CFG_OTHER_NOTE), C_DIM);
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
    if (!shown && !g_search[0] && g_tab != CR_TAB_OTHER) note(cr_text(UI_CFG_NOTHING), C_DIM);
    if (!shown && g_search[0]) note(cr_text(UI_CFG_NOTHING), C_DIM);
}

// ---- header and footer
static void header(float width) {
    char title[128];
    snprintf(title, sizeof title, "%s %s", cr_text(UI_CFG_TITLE), CR_VERSION);
    nk_layout_row_begin(g_ctx, NK_STATIC, S(40), 8);
    nk_layout_row_push(g_ctx, text_w(g_head, title, -1) + S(24));
    nk_style_push_font(g_ctx, &g_head->handle);
    nk_label_colored(g_ctx, title, NK_TEXT_LEFT, C_GOLD);
    nk_style_pop_font(g_ctx);
    // profile
    char names[CR_MAX_PROFILES][CR_NAME];
    int np = model_profiles(names, CR_MAX_PROFILES);
    const char *active = model_active_profile();
    const char *items[CR_MAX_PROFILES + 1];
    items[0] = cr_text(UI_CFG_NO_PROFILE);
    int sel = 0;
    for (int i = 0; i < np; i++) {
        items[i + 1] = names[i];
        if (!strcmp(names[i], active)) sel = i + 1;
    }
    float used = text_w(g_head, title, -1) + S(24);
    float plw = text_w(g_body, cr_text(UI_CFG_PROFILE), -1) + S(10);
    nk_layout_row_push(g_ctx, plw);
    nk_label_colored(g_ctx, cr_text(UI_CFG_PROFILE), NK_TEXT_RIGHT, C_DIM);
    nk_layout_row_push(g_ctx, S(200));
    int got = nk_combo(g_ctx, items, np + 1, sel, (int)S(28), nk_vec2(S(220), combo_h(np + 1 < 12 ? np + 1 : 12)));
    if (got != sel) {
        if (model_switch_profile(got ? names[got - 1] : "")) {
            status(0, cr_text(UI_CFG_SWITCHED), got ? names[got - 1] : cr_text(UI_CFG_NO_PROFILE));
            memset(g_edits, 0, sizeof *g_edits * (size_t)cr_row_count);
        } else
            status(1, "%s", cr_text(UI_MENU_SAVE_FAILED));
    }
    float bw1 = text_w(g_body, cr_text(UI_CFG_NEW_PROFILE), -1) + S(24);
    float bw2 = text_w(g_body, cr_text(UI_CFG_RENAME), -1) + S(24);
    float bw3 = text_w(g_body, cr_text(UI_CFG_DELETE), -1) + S(24);
    nk_layout_row_push(g_ctx, bw1);
    if (nk_button_label(g_ctx, cr_text(UI_CFG_NEW_PROFILE)))
        g_pop = POP_NEW, g_pop_name[0] = 0, g_pop_preset = PRESET_CURRENT, g_pop_error = 0;
    nk_layout_row_push(g_ctx, bw2);
    if (!*active) nk_widget_disable_begin(g_ctx);
    if (nk_button_label(g_ctx, cr_text(UI_CFG_RENAME)))
        g_pop = POP_RENAME, snprintf(g_pop_name, sizeof g_pop_name, "%s", active), g_pop_error = 0;
    nk_layout_row_push(g_ctx, bw3);
    if (nk_button_label(g_ctx, cr_text(UI_CFG_DELETE))) g_pop = POP_DELETE;
    if (!*active) nk_widget_disable_end(g_ctx);
    used += plw + S(200) + bw1 + bw2 + bw3 + S(8) * 6;
    // the window's language: the menu language row (TOTK_MENU_LANG), as in the game
    float lw = S(270);
    nk_layout_row_push(g_ctx, fmaxf(S(4), width - used - lw - S(4)));
    nk_spacing(g_ctx, 1);
    nk_layout_row_push(g_ctx, lw);
    for (int i = 0; i < cr_row_count; i++)
        if (!strcmp(cr_rows[i].key, "MENU_LANG")) {
            Item it[MAX_ITEMS];
            int n = items_of(&cr_rows[i], it), cur = 0;
            const char *v = model_own("MENU_LANG");
            const char *nm[MAX_ITEMS];
            for (int k = 0; k < n; k++) {
                nm[k] = it[k].name;
                if (!strcmp(it[k].value, v ? v : "")) cur = k;
            }
            int pick = nk_combo(g_ctx, nm, n, cur, (int)S(28), nk_vec2(lw, combo_h(n)));
            if (pick != cur) model_set("MENU_LANG", it[pick].value);
        }
    nk_layout_row_end(g_ctx);
    // the game folder
    nk_layout_row_begin(g_ctx, NK_STATIC, S(30), 4);
    const char *gf = cr_text(UI_CFG_GAME_FOLDER);
    nk_layout_row_push(g_ctx, text_w(g_body, gf, -1) + S(12));
    nk_label_colored(g_ctx, gf, NK_TEXT_LEFT, C_DIM);
    float cbw = text_w(g_body, cr_text(UI_CFG_CHANGE), -1) + S(24);
    char warn[512] = "";
    if (!model_has_game()) snprintf(warn, sizeof warn, cr_text(UI_CFG_NO_GAME), cr_game_exe);
    float dw = fminf(text_w(g_body, model_dir(), -1) + S(16), width * 0.5f);
    nk_layout_row_push(g_ctx, dw);
    nk_label_colored(g_ctx, model_dir(), NK_TEXT_LEFT, C_TEXT);
    nk_layout_row_push(g_ctx, cbw);
    if (nk_button_label(g_ctx, cr_text(UI_CFG_CHANGE))) pick_folder(-1, model_dir());
    nk_layout_row_push(g_ctx, fmaxf(S(10), width - dw - cbw - text_w(g_body, gf, -1) - S(60)));
    nk_label_colored(g_ctx, warn, NK_TEXT_LEFT, C_WARN);
    nk_layout_row_end(g_ctx);
}

static void footer(float width) {
    int unsaved = model_unsaved();
    char left[600];
    struct nk_color col = C_DIM;
    if (g_file_changed) snprintf(left, sizeof left, "%s", cr_text(UI_CFG_FILE_CHANGED)), col = C_WARN;
    else if (g_status[0] && SDL_GetTicks() < g_status_until)
        snprintf(left, sizeof left, "%s", g_status), col = g_status_bad ? C_WARN : C_GOLD;
    else if (unsaved) snprintf(left, sizeof left, cr_text(UI_CFG_UNSAVED), unsaved), col = C_WARN;
    else snprintf(left, sizeof left, "%s", model_ini_path());
    float b1 = text_w(g_body, cr_text(UI_CFG_REVERT), -1) + S(40), b2 = text_w(g_body, cr_text(UI_CFG_SAVE), -1) + S(48),
          b3 = text_w(g_body, cr_text(UI_CFG_SAVE_LAUNCH), -1) + S(40),
          b0 = g_file_changed ? text_w(g_body, cr_text(UI_CFG_RELOAD), -1) + S(40) : 0;
    nk_layout_row_begin(g_ctx, NK_STATIC, S(36), 5);
    nk_layout_row_push(g_ctx, fmaxf(S(10), width - b0 - b1 - b2 - b3 - S(40)));
    nk_label_colored(g_ctx, left, NK_TEXT_LEFT, col);
    nk_layout_row_push(g_ctx, b0);
    if (g_file_changed && nk_button_label(g_ctx, cr_text(UI_CFG_RELOAD))) {
        model_reload();
        g_file_changed = 0;
        memset(g_edits, 0, sizeof *g_edits * (size_t)cr_row_count);
    } else if (!g_file_changed)
        nk_spacing(g_ctx, 1);
    nk_layout_row_push(g_ctx, b1);
    if (!unsaved) nk_widget_disable_begin(g_ctx);
    if (nk_button_label(g_ctx, cr_text(UI_CFG_REVERT))) {
        model_reload();
        memset(g_edits, 0, sizeof *g_edits * (size_t)cr_row_count);
    }
    nk_layout_row_push(g_ctx, b2);
    if (seg_button(cr_text(UI_CFG_SAVE), unsaved > 0)) {
        if (model_save()) status(0, cr_text(UI_CFG_SAVED), model_ini_path()), g_file_changed = 0;
        else status(1, "%s", cr_text(UI_MENU_SAVE_FAILED));
    }
    if (!unsaved) nk_widget_disable_end(g_ctx);
    nk_layout_row_push(g_ctx, b3);
    if (!model_has_game()) nk_widget_disable_begin(g_ctx);
    if (nk_button_label(g_ctx, cr_text(UI_CFG_SAVE_LAUNCH))) {
        if (!model_save()) status(1, "%s", cr_text(UI_MENU_SAVE_FAILED));
        else if (model_launch()) status(0, cr_text(UI_CFG_LAUNCHED), cr_game_exe), g_file_changed = 0;
        else status(1, cr_text(UI_CFG_LAUNCH_FAILED), cr_game_exe);
    }
    if (!model_has_game()) nk_widget_disable_end(g_ctx);
    nk_layout_row_end(g_ctx);
}

// ---- popups
static void popups(float w, float h) {
    if (g_pop == POP_NONE) return;
    float pw = fminf(S(560), w - S(40)), ph = g_pop == POP_NEW ? S(380) : g_pop == POP_RENAME ? S(200) : S(220);
    struct nk_rect r = nk_rect((w - pw) / 2, (h - ph) / 2, pw, ph);
    struct nk_style_window *ws = &g_ctx->style.window;
    nk_style_push_style_item(g_ctx, &ws->fixed_background, nk_style_item_color(C_PANEL));
    nk_style_push_color(g_ctx, &ws->popup_border_color, C_GOLD);
    nk_style_push_float(g_ctx, &ws->popup_border, S(1));
    nk_style_push_vec2(g_ctx, &ws->popup_padding, nk_vec2(S(20), S(16)));
    int open = nk_popup_begin(g_ctx, NK_POPUP_STATIC, "##pop", NK_WINDOW_BORDER, r);
    nk_style_pop_vec2(g_ctx), nk_style_pop_float(g_ctx), nk_style_pop_color(g_ctx), nk_style_pop_style_item(g_ctx);
    if (!open) return;
    static const UiTextId titles[] = {0, UI_CFG_NEW_PROFILE, UI_CFG_RENAME, UI_CFG_DELETE, UI_CFG_SAVE};
    nk_layout_row_dynamic(g_ctx, S(34), 1);
    nk_style_push_font(g_ctx, &g_head->handle);
    nk_label_colored(g_ctx, cr_text(titles[g_pop]), NK_TEXT_LEFT, C_GOLD);
    nk_style_pop_font(g_ctx);
    int close = 0;
    if (g_pop == POP_NEW || g_pop == POP_RENAME) {
        nk_layout_row_begin(g_ctx, NK_DYNAMIC, S(32), 2);
        nk_layout_row_push(g_ctx, 0.25f);
        nk_label(g_ctx, cr_text(UI_CFG_NAME), NK_TEXT_LEFT);
        nk_layout_row_push(g_ctx, 0.75f);
        nk_edit_string_zero_terminated(g_ctx, NK_EDIT_FIELD, g_pop_name, sizeof g_pop_name, NULL);
        nk_layout_row_end(g_ctx);
        if (g_pop == POP_NEW) {
            const char *names[PRESET_COUNT];
            for (int i = 0; i < PRESET_COUNT; i++) names[i] = cr_text(preset_label((Preset)i));
            nk_layout_row_begin(g_ctx, NK_DYNAMIC, S(32), 2);
            nk_layout_row_push(g_ctx, 0.25f);
            nk_label(g_ctx, cr_text(UI_CFG_START_FROM), NK_TEXT_LEFT);
            nk_layout_row_push(g_ctx, 0.75f);
            g_pop_preset = nk_combo(g_ctx, names, PRESET_COUNT, g_pop_preset, (int)S(28),
                                    nk_vec2(pw * 0.7f, combo_h(PRESET_COUNT)));
            nk_layout_row_end(g_ctx);
            char text[1024];
            int n = snprintf(text, sizeof text, "%s", cr_text(preset_desc((Preset)g_pop_preset)));
            int nc = 0;
            const PresetChange *pc = preset_changes((Preset)g_pop_preset, &nc);
            const char *k, *v;
            if (nc || g_pop_preset == PRESET_DECK)
                n += snprintf(text + n, sizeof text - (size_t)n, "\n%s ", cr_text(UI_CFG_PRESET_CHANGES));
            for (int i = 0; i < nc && n < (int)sizeof text - 64; i++) {
                char lk[64];
                lower_key(lk, sizeof lk, pc[i].key);
                n += snprintf(text + n, sizeof text - (size_t)n, "%s%s %s", i ? ", " : "", lk,
                              pc[i].value ? pc[i].value : cr_text(UI_CFG_UNSET));
            }
            for (int i = 0; g_pop_preset == PRESET_DECK && cfg_deck_at(i, &k, &v) && n < (int)sizeof text - 64; i++) {
                char lk[64];
                lower_key(lk, sizeof lk, k);
                n += snprintf(text + n, sizeof text - (size_t)n, "%s%s %s", i ? ", " : "", lk, v);
            }
            nk_layout_row_dynamic(g_ctx, S(118), 1);
            nk_label_colored_wrap(g_ctx, text, C_DIM);
            nk_layout_row_dynamic(g_ctx, S(30), 1);
            nk_checkbox_label(g_ctx, cr_text(UI_CFG_ACTIVATE), &g_pop_activate);
        }
        nk_layout_row_dynamic(g_ctx, S(24), 1);
        nk_label_colored(g_ctx, g_pop_error ? cr_text(UI_CFG_NAME_TAKEN) : "", NK_TEXT_LEFT, C_WARN);
        nk_layout_row_dynamic(g_ctx, S(34), 2);
        if (nk_button_label(g_ctx, cr_text(UI_MENU_RESET_NO))) close = 1;
        if (seg_button(cr_text(g_pop == POP_NEW ? UI_CFG_CREATE : UI_CFG_RENAME), 1)) {
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
    } else if (g_pop == POP_DELETE) {
        char q[512];
        snprintf(q, sizeof q, cr_text(UI_CFG_DELETE_CONFIRM), model_active_profile());
        nk_layout_row_dynamic(g_ctx, S(80), 1);
        nk_label_wrap(g_ctx, q);
        nk_layout_row_dynamic(g_ctx, S(34), 2);
        if (nk_button_label(g_ctx, cr_text(UI_MENU_RESET_NO))) close = 1;
        if (seg_button(cr_text(UI_CFG_DELETE), 1)) model_delete_profile(model_active_profile()), close = 1;
    } else if (g_pop == POP_CLOSE) {
        nk_layout_row_dynamic(g_ctx, S(60), 1);
        nk_label_wrap(g_ctx, cr_text(UI_CFG_DISCARD_CONFIRM));
        nk_layout_row_dynamic(g_ctx, S(34), 3);
        if (nk_button_label(g_ctx, cr_text(UI_MENU_RESET_NO))) close = 1;
        if (nk_button_label(g_ctx, cr_text(UI_CFG_DISCARD))) g_quit = 1;
        if (seg_button(cr_text(UI_CFG_SAVE), 1)) {
            if (model_save()) g_quit = 1;
            else status(1, "%s", cr_text(UI_MENU_SAVE_FAILED)), close = 1;
        }
    }
    if (close) {
        g_pop = POP_NONE;
        nk_popup_close(g_ctx);
    }
    nk_popup_end(g_ctx);
}

// ---- the frame
static void frame(void) {
    int pw, ph;
    SDL_GetRenderOutputSize(g_ren, &pw, &ph);
    float w = (float)pw, h = (float)ph;
    if (nk_begin(g_ctx, "totk-config", nk_rect(0, 0, w, h), NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BACKGROUND)) {
        float inner = w - g_ctx->style.window.padding.x * 2;
        header(inner);
        float head_h = S(40) + S(30) + S(8) * 2 + g_ctx->style.window.padding.y;
        float foot_h = S(36) + S(8) * 2 + g_ctx->style.window.padding.y;
        float body_h = h - head_h - foot_h - S(8);
        nk_layout_row_template_begin(g_ctx, body_h);
        nk_layout_row_template_push_static(g_ctx, S(220));
        nk_layout_row_template_push_dynamic(g_ctx);
        nk_layout_row_template_end(g_ctx);
        if (nk_group_begin(g_ctx, "tabs", NK_WINDOW_NO_SCROLLBAR)) {
            nk_layout_row_dynamic(g_ctx, S(34), 1);
            for (int t = 0; t < CR_TAB_COUNT; t++) {
                if (t == CR_TAB_EXPERT) {
                    nk_layout_row_dynamic(g_ctx, S(12), 1);
                    nk_spacing(g_ctx, 1);
                    nk_layout_row_dynamic(g_ctx, S(34), 1);
                }
                char label[96];
                int n = tab_set_count(t);
                if (n) snprintf(label, sizeof label, "%s  · %d", tab_name(t), n);
                else snprintf(label, sizeof label, "%s", tab_name(t));
                nk_bool on = t == g_tab && !g_search[0];
                if (nk_selectable_label(g_ctx, label, NK_TEXT_LEFT, &on) && t != g_tab) g_tab = t, g_scroll_reset = 1;
                if (on && g_search[0] && t == g_tab) g_search[0] = 0, g_scroll_reset = 1;
            }
            nk_group_end(g_ctx);
        }
        if (nk_group_begin(g_ctx, "page", NK_WINDOW_NO_SCROLLBAR)) {
            nk_layout_row_begin(g_ctx, NK_DYNAMIC, S(32), 2);
            nk_layout_row_push(g_ctx, 0.7f);
            char before[sizeof g_search];
            snprintf(before, sizeof before, "%s", g_search);
            struct nk_rect sb = nk_widget_bounds(g_ctx);
            nk_flags sf = nk_edit_string_zero_terminated(g_ctx, NK_EDIT_FIELD, g_search, sizeof g_search, NULL);
            if (!g_search[0] && (sf & NK_EDIT_INACTIVE))
                draw_text(nk_window_get_canvas(g_ctx), g_body, sb.x + S(10), sb.y + (sb.h - g_body->handle.height) / 2,
                          sb.w, cr_text(UI_CFG_SEARCH), -1, C_DIM);
            if (strcmp(before, g_search)) g_scroll_reset = 1;
            nk_layout_row_push(g_ctx, 0.3f);
            nk_checkbox_label(g_ctx, cr_text(UI_CFG_ONLY_SET), &g_only_set);
            nk_layout_row_end(g_ctx);
            nk_layout_row_dynamic(g_ctx, body_h - S(32) - S(20), 1);
            if (nk_group_begin(g_ctx, "rows", 0)) {
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
    SDL_SetWindowMinimumSize(g_win, 900, 560);
    g_ren = SDL_CreateRenderer(g_win, NULL);
    if (!g_ren) {
        fprintf(stderr, "totk-config: SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(g_ren, 1);
    g_pick_lock = SDL_CreateMutex();
    g_ctx = nk_sdl_init(g_win, g_ren, nk_sdl_allocator());
    g_edits = calloc((size_t)cr_row_count, sizeof *g_edits);
    input_map_reset();
    for (int a = 0; a < ACTION_COUNT && a < 64; a++)
        input_map_line(&input_map, a, g_default_binds[a], sizeof g_default_binds[a]);
    if (tab >= 0 && tab < CR_TAB_COUNT) g_tab = tab;
    if (search) snprintf(g_search, sizeof g_search, "%s", search);
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
