// Two-pane settings pages (option_rows.h): rows on the left, the selected or hovered row's description on the right.
//
// Adapted from SavageWindRecomp's runtime/ui/option_rows.mm (https://github.com/Revivedx/SavageWindRecomp @
// 575ad88e02f9a89a9a885d90039d48af33c982f4, MIT, Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors,
// Copyright (c) 2026 Peiikai): the row look, the two panes, the left / right stepping and the hold speed-up. The rows
// read and write cfg keys instead of a settings struct, take their text from the menu catalog (menu_text), and show
// the default, "after restart" and environment notes; the text field, radio and on-screen keyboard rows are not ported.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"   // SetKeyOwner, SetFocusID, NavCursorVisible

#include "ui/option_rows.h"

extern "C" {
#include "cfg.h"
#include "platform/platform.h"
#include "ui_text.h"
// host.h pulls in cpu.h, which is C only: the one function needed from it, as declared there.
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

static const ImU32 kGold = IM_COL32(236, 194, 84, 255);
static const ImU32 kDim = IM_COL32(150, 150, 140, 255);
static const ImU32 kGrey = IM_COL32(110, 110, 102, 255);

static float g_scale = 1;
static bool g_focus_req, g_take_first;   // opt_focus_first: requested / this page's first row takes it
static char g_focus_key[64];             // opt_focus_key: the row that takes the cursor when it is next drawn
static bool g_held;                      // the last row's left / right has been held a while (steppers go faster)
static float g_bottom;                   // opt_reserve_bottom: kept free under the panes
static bool g_disable_next;              // opt_disable_next: the next row is greyed out with g_disable_note
static UiTextId g_disable_note;

// The description pane: this frame's hovered and selected rows, the page's first row, and the last one shown.
struct Desc { char text[UI_DESC_BYTES]; };
static Desc g_hover, g_focus, g_first, g_shown;

// disabled: the note of a row opt_disable_next greyed out, else NULL.
static void desc_set(Desc &d, UiTextId desc, const char *key, const CfgMeta *m, const char *disabled) {
    int n = snprintf(d.text, sizeof d.text, "%s", menu_text(desc));
    if (n < 0 || n >= (int)sizeof d.text) return;
    if (disabled)
        snprintf(d.text + n, sizeof d.text - n, "\n\n%s", disabled);
    else if (cfg_from_env(key))
        snprintf(d.text + n, sizeof d.text - n, "\n\n%s %s", menu_text(UI_MENU_SET_BY_ENV), key);
    else if (m && !m->live)
        snprintf(d.text + n, sizeof d.text - n, "\n\n%s", menu_text(UI_MENU_AFTER_RESTART));
}

void opt_focus_first(void) { g_focus_req = true; }

void opt_focus_key(const char *key) { snprintf(g_focus_key, sizeof g_focus_key, "%s", key); }

void opt_reserve_bottom(float h) { g_bottom = h; }

void opt_disable_next(UiTextId note) {
    g_disable_next = true;
    g_disable_note = note;
}

void opt_begin(const char *id, float scale) {
    g_scale = scale;
    g_take_first = g_focus_req;
    g_focus_req = false;
    if (g_take_first) ImGui::NavMoveRequestCancel();   // this frame's move goes to the first row instead
    g_hover.text[0] = g_focus.text[0] = g_first.text[0] = 0;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y -= g_bottom;
    ImGui::PushID(id);
    ImGui::BeginChild("##rows", ImVec2(avail.x * 0.58f, avail.y), ImGuiChildFlags_NavFlattened);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 5 * scale));
}

void opt_end(void) {
    ImGui::PopStyleVar();
    ImGui::EndChild();
    // the hovered row (mouse) wins, else the selected one (controller), else the last shown, else the first row
    if (g_hover.text[0]) g_shown = g_hover;
    else if (g_focus.text[0]) g_shown = g_focus;
    else if (!g_shown.text[0]) g_shown = g_first;
    ImGui::SameLine(0, 0);
    ImVec2 p = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    avail.y -= g_bottom;
    float gap = 24 * g_scale;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + gap * 0.5f, p.y), ImVec2(p.x + gap * 0.5f, p.y + avail.y),
                                        IM_COL32(236, 194, 84, 110), 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + gap, p.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * g_scale, 6 * g_scale));
    ImGui::BeginChild("##desc", ImVec2(0, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(g_shown.text);
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopID();
}

// What a row shows besides its label.
struct RowView {
    const char *value;   // right side; "" for none
    bool is_default;     // the value is the setting's default: the "default" mark, the value not highlighted
    bool arrows;         // a stepper: < value >, the arrows click too
    const char *note;    // under the value instead of the "default" mark; NULL for none
};

// One row: draws it, handles A / click and left / right while selected. Returns 2 (A / click), -1 / +1 (left / right) or
// 0; always 0 for a row the environment fixes (greyed out, still selectable so its description shows).
static int row(const char *key, UiTextId label, UiTextId desc, const RowView &v) {
    const CfgMeta *m = cfg_meta(key);
    const char *disabled = g_disable_next ? menu_text(g_disable_note) : NULL;
    g_disable_next = false;
    bool off = disabled || cfg_from_env(key) != 0;
    ImGui::PushID(key);
    ImVec2 p = ImGui::GetCursorScreenPos();
    // h whole pixels: ImGui truncates cursor positions, which rounds a fractional height the other way above the window's
    // top (negative y), so the page grew a pixel per row scrolled out and a row scrolled to the bottom was cut off
    float fs = ImGui::GetFontSize(), w = ImGui::GetContentRegionAvail().x, h = floorf(fs * 2.2f);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));   // the row's own border shows the selection
    bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    bool to_key = g_focus_key[0] && !strcmp(key, g_focus_key);
    if (g_take_first || to_key) {
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
        if (g_take_first) ImGui::SetScrollY(0);   // the page's top: text and a heading above the first row show
        else ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);   // as a cursor move would (the last row whole)
        g_take_first = false;
        if (to_key) g_focus_key[0] = 0;
    }
    bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
    int step = clicked && !off ? 2 : 0;
    g_held = false;
    if (foc && !off) {
        // the left stick arrives as the D-pad (menu.cpp pad_events)
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) step = 1;
        static const ImGuiKey steps[] = {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_LeftArrow,
                                         ImGuiKey_RightArrow};
        for (ImGuiKey k : steps) {
            if (ImGui::GetKeyData(k)->DownDuration > 1.0f) g_held = true;
            // left and right belong to the row (navigation would otherwise move sideways into the description pane)
            ImGui::SetKeyOwner(k, ImGui::GetItemID());
        }
    }
    if (foc) desc_set(g_focus, desc, key, m, disabled);
    if (hov) desc_set(g_hover, desc, key, m, disabled);
    if (!g_first.text[0]) desc_set(g_first, desc, key, m, disabled);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    float r = 9 * g_scale, pad = 16 * g_scale, small = fs * 0.72f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov || foc ? IM_COL32(255, 255, 255, 22) : IM_COL32(0, 0, 0, 40), r);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h),
                hov || foc ? (off ? IM_COL32(236, 194, 84, 120) : kGold) : IM_COL32(255, 255, 255, off ? 18 : 38), r, 0,
                hov || foc ? 2.0f : 1.0f);
    float ty = p.y + fs * 0.25f, ny = p.y + fs * 1.3f;   // the label line and the note line under it
    float right = p.x + w - pad;                         // the value's right edge
    float value_left = right;
    if (v.arrows && v.value[0]) {   // < value >, drawn (the font has no arrow glyphs); a click on one steps
        float aw = fs * 0.42f, gap = fs * 0.55f, cy = ty + fs * 0.52f;
        float rx = right - aw, lx = rx - gap - ImGui::CalcTextSize(v.value).x - gap - aw;
        ImU32 ac = off ? kGrey : hov || foc ? kGold : IM_COL32(170, 170, 160, 255);
        dl->AddTriangleFilled(ImVec2(lx, cy), ImVec2(lx + aw, cy - aw * 0.75f), ImVec2(lx + aw, cy + aw * 0.75f), ac);
        dl->AddTriangleFilled(ImVec2(rx + aw, cy), ImVec2(rx, cy - aw * 0.75f), ImVec2(rx, cy + aw * 0.75f), ac);
        if (clicked && !off) {
            float mx = ImGui::GetIO().MousePos.x;
            if (mx >= lx - gap && mx <= lx + aw + gap) step = -1;
            else if (mx >= rx - gap && mx <= rx + aw + gap) step = 1;
        }
        right = rx - gap;
        value_left = lx;
    }
    if (v.value[0]) {
        float vx = right - ImGui::CalcTextSize(v.value).x;
        dl->AddText(ImVec2(vx, ty), off ? kGrey : v.is_default ? IM_COL32(205, 205, 195, 255) : IM_COL32(255, 255, 255, 255),
                    v.value);
        if (!v.arrows) value_left = vx;
    }
    if (v.note || v.is_default) {   // under the value, right-aligned with it
        const char *t = v.note ? v.note : menu_text(UI_MENU_DEFAULT_MARK);
        float tw = font->CalcTextSizeA(small, FLT_MAX, 0, t).x;
        dl->AddText(font, small, ImVec2(right - tw, ny), v.note ? IM_COL32(220, 170, 100, 255) : kDim, t);
    }
    // the label and its note, clipped before the value
    ImVec4 clip(p.x, p.y, value_left - fs * 0.6f, p.y + h);
    dl->AddText(font, fs, ImVec2(p.x + pad, ty), off ? kGrey : IM_COL32(240, 236, 224, 255), menu_text(label), NULL, 0,
                &clip);
    const char *note = disabled        ? disabled
                     : off             ? menu_text(UI_MENU_SET_BY_ENV)
                     : m && !m->live ? menu_text(UI_MENU_AFTER_RESTART)
                                     : NULL;
    if (note) dl->AddText(font, small, ImVec2(p.x + pad, ny), off ? IM_COL32(200, 150, 90, 255) : kDim, note);
    ImGui::PopID();
    return off ? 0 : step;
}

// ---- writing a setting: at once in memory (live settings follow), to totk.ini a moment later (opt_save_tick)
static uint64_t g_save_at;   // plat_now_ns deadline of the pending write; 0 none
static bool g_save_logged;

static void apply(const char *key, const char *value) {
    if (*value) {
        cfg_set(key, value);
    } else {   // the automatic choice: the key unset, its line removed from totk.ini
        const char *k = key;
        cfg_reset(&k, 1);
    }
    opt_save_soon();
}

void opt_save_soon(void) { g_save_at = plat_now_ns() + 400000000ull; }

int opt_save_tick(int force) {
    if (!g_save_at || (!force && plat_now_ns() < g_save_at)) return g_save_at != 0;
    g_save_at = 0;
    if (!cfg_save() && !g_save_logged) {
        g_save_logged = true;
        rt_log("menu: could not write totk.ini; the changed settings hold for this session");
    }
    return 0;
}

// ---- rows

bool opt_toggle(const char *key, UiTextId label, UiTextId desc, bool inverted) {
    const CfgMeta *m = cfg_meta(key);
    bool set = cfg_bool(key) != 0, def = m && strcmp(m->def, "0") != 0;
    bool on = set != inverted;   // what the row shows
    RowView v = {menu_text(on ? UI_MENU_ON : UI_MENU_OFF), set == def, false, NULL};
    if (!row(key, label, desc, v)) return false;   // any step flips it
    apply(key, set ? "0" : "1");
    return true;
}

bool opt_button(const char *id, UiTextId label, UiTextId desc, const char *value) {
    RowView v = {value ? value : "", false, false, NULL};
    return row(id, label, desc, v) == 2;   // A or a click; left and right do nothing
}

void opt_group(UiTextId title) {
    ImGui::Dummy(ImVec2(0, 6 * g_scale));
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", menu_text(title));
}

void opt_text(UiTextId text, bool warn) {
    ImGui::PushTextWrapPos(0);
    if (warn) ImGui::TextColored(ImVec4(0.95f, 0.66f, 0.40f, 1), "%s", menu_text(text));
    else ImGui::TextUnformatted(menu_text(text));
    ImGui::PopTextWrapPos();
}

bool opt_choice_list(const char *key, UiTextId label, const char *const *values, const char *const *names, int n,
                     UiTextId desc, int cur, const char *note) {
    const CfgMeta *m = cfg_meta(key);
    const char *now = cfg_value(key);
    if (cur < 0)
        for (int i = 0; i < n; i++)
            if (!strcmp(values[i], now)) cur = i;
    int def = -1;
    for (int i = 0; m && i < n; i++)
        if (!strcmp(values[i], m->def)) def = i;
    RowView v = {cur >= 0 ? names[cur] : now, cur == def, true, note};
    int step = row(key, label, desc, v);
    if (!step || n <= 0) return false;
    int nv;
    if (cur < 0) nv = 0;
    else if (step == 2) nv = (cur + 1) % n;               // A / click: the next value, round to the first
    else nv = cur + step < 0 ? 0 : cur + step >= n ? n - 1 : cur + step;   // left / right stop at the ends
    if (nv == cur) return false;
    apply(key, values[nv]);
    return true;
}

bool opt_choice(const char *key, UiTextId label, const UiTextId *names, int n, UiTextId desc, const char *note) {
    enum { MAX = 32 };
    const CfgMeta *m = cfg_meta(key);
    const char *values[MAX], *shown[MAX];
    char buf[256];
    int k = 0;
    if (m && !m->def[0]) values[k++] = "";   // the automatic choice
    snprintf(buf, sizeof buf, "%s", m && m->values ? m->values : "");
    for (char *s = buf, *e; *s && k < MAX; s = e) {
        e = strchr(s, '|');
        if (e) *e++ = 0;
        else e = s + strlen(s);
        values[k++] = s;
    }
    if (n > k) n = k;
    for (int i = 0; i < n; i++) shown[i] = menu_text(names[i]);
    return opt_choice_list(key, label, values, shown, n, desc, -1, note);
}

bool opt_slider(const char *key, UiTextId label, float lo, float hi, UiTextId desc, float step, const char *fmt,
                const char *note, const char *fallback) {
    const char *set = cfg_get(key);
    const char *src = fallback && !(set && *set) ? fallback : key;   // unset: show the value in effect, the fallback's
    const CfgMeta *m = cfg_meta(src);
    double v = cfg_double(src), def = m ? strtod(m->def, NULL) : lo;
    char shown[48];
    snprintf(shown, sizeof shown, fmt, v);
    RowView rv = {shown, fabs(v - def) < step * 0.01, true, note};
    int s = row(key, label, desc, rv);
    if (!s) return false;
    int steps = g_held && (hi - lo) > 40 * step ? 10 : 1;   // held: ten steps at a time over a long range
    double n = round(v / step) * step + (s == 2 ? step : s * step * steps);
    if (s == 2 && n > hi + step * 0.01) n = lo;   // A / click past the top goes round; left / right stop at the ends
    n = n < lo ? lo : n > hi ? hi : n;
    if (fabs(n - v) < step * 0.01) return false;
    char out[32];
    if (step >= 1) snprintf(out, sizeof out, "%d", (int)lround(n));
    else snprintf(out, sizeof out, "%g", round(n / step) * step);
    apply(key, out);
    return true;
}

// ---- binding rows (the remapping rows of the Controls tab): a label and three cells, each its own nav item

// Text centred (left-aligned when centred is false) in [x0, x1] on a row from y to y + h, at the font size or smaller so
// a long name ("Нажатие правого стика", "Стрелка вправо") fits; under 65 % it is clipped instead.
static void fit_text(ImDrawList *dl, float x0, float x1, float y, float h, ImU32 col, const char *t, bool centred) {
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), room = x1 - x0, tw = font->CalcTextSizeA(fs, FLT_MAX, 0, t).x;
    if (tw > room) {
        fs = fmaxf(fs * room / tw, fs * 0.65f);
        tw = font->CalcTextSizeA(fs, FLT_MAX, 0, t).x;
    }
    float x = centred && tw < room ? x0 + (room - tw) * 0.5f : x0;
    ImVec4 clip(x0, y, x1, y + h);
    dl->AddText(font, fs, ImVec2(x, y + (h - fs) * 0.5f), col, t, NULL, 0, &clip);
}

// The label column and the three cells of a binding row of width w.
static void bind_columns(float x, float w, float cx[3], float *cw) {
    float lw = floorf(w * 0.34f), gap = floorf(8 * g_scale);
    *cw = floorf((w - lw - gap * 3) / 3);
    for (int c = 0; c < 3; c++) cx[c] = x + lw + gap + c * (*cw + gap);
}

void opt_bind_header(UiTextId c0, UiTextId c1, UiTextId c2) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, cx[3], cw, fs = ImGui::GetFontSize() * 0.8f;
    bind_columns(p.x, w, cx, &cw);
    const UiTextId cols[3] = {c0, c1, c2};
    ImFont *font = ImGui::GetFont();
    for (int c = 0; c < 3; c++) {
        const char *t = menu_text(cols[c]);
        float tw = font->CalcTextSizeA(fs, FLT_MAX, 0, t).x;
        ImGui::GetWindowDrawList()->AddText(font, fs, ImVec2(cx[c] + (cw - tw) * 0.5f, p.y), kDim, t);
    }
    ImGui::Dummy(ImVec2(w, fs * 1.2f));
}

int opt_bind_row(const char *key, const char *label, UiTextId desc, const char *const cells[3], const bool enabled[3],
                 const char *note) {
    ImGui::PushID(key);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float fs = ImGui::GetFontSize(), w = ImGui::GetContentRegionAvail().x, h = floorf(fs * 1.9f), cx[3], cw;
    bind_columns(p.x, w, cx, &cw);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = 8 * g_scale, pad = 6 * g_scale;
    fit_text(dl, p.x + 16 * g_scale, cx[0] - fs * 0.3f, p.y, h, IM_COL32(240, 236, 224, 255), label, false);
    int result = -1;
    bool row_foc = false, row_hov = false;
    for (int c = 0; c < 3; c++) {
        ImVec2 a(cx[c], p.y), b(cx[c] + cw, p.y + h);
        const char *t = cells[c] && cells[c][0] ? cells[c] : "-";
        if (!enabled[c]) {   // not a nav item: the cursor skips it
            dl->AddRect(a, b, IM_COL32(255, 255, 255, 14), r);
            fit_text(dl, a.x + pad, b.x - pad, a.y, h, kGrey, t, true);
            continue;
        }
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(c);
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));   // the cell's own border shows the selection
        bool clicked = ImGui::InvisibleButton("##cell", ImVec2(cw, h), ImGuiButtonFlags_EnableNav);
        ImGui::PopStyleColor();
        char id[80];
        snprintf(id, sizeof id, "%s#%d", key, c);
        bool to_key = g_focus_key[0] && !strcmp(id, g_focus_key);
        if (g_take_first || to_key) {
            ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
            ImGui::SetNavCursorVisible(true);
            if (g_take_first) ImGui::SetScrollY(0);
            else ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
            g_take_first = false;
            if (to_key) g_focus_key[0] = 0;
        }
        bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
        row_foc |= foc;
        row_hov |= hov;
        if (clicked) result = c;
        if (foc && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)))
            result = 10 + c;
        dl->AddRectFilled(a, b, hov || foc ? IM_COL32(255, 255, 255, 22) : IM_COL32(0, 0, 0, 40), r);
        dl->AddRect(a, b, hov || foc ? kGold : IM_COL32(255, 255, 255, 38), r, 0, hov || foc ? 2.0f : 1.0f);
        fit_text(dl, a.x + pad, b.x - pad, a.y, h, IM_COL32(255, 255, 255, 255), t, true);
        ImGui::PopID();
    }
    char k[64];
    snprintf(k, sizeof k, "TOTK_%s", key);
    if (row_foc) desc_set(g_focus, desc, k, NULL, NULL);
    if (row_hov) desc_set(g_hover, desc, k, NULL, NULL);
    if (!g_first.text[0]) desc_set(g_first, desc, k, NULL, NULL);
    if (note && (row_foc || row_hov)) {   // e.g. what moved here, after the description
        Desc &d = row_hov ? g_hover : g_focus;
        size_t n = strlen(d.text);
        snprintf(d.text + n, sizeof d.text - n, "\n\n%s", note);
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PopID();
    return result;
}
