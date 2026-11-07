// The blueprint picker's draw layer: it paints only the rectangles and text runtime/blueprint_picker.c computed, through
// ImGui's draw list. There is no ImGui widget and no ImGui layout here on purpose: the picker's state, hit tests, layout
// and every visual distinction (icon, hover, selection) live in C, so this file cannot trip ImGui's layout checks. The
// earlier all-widget version aborted the game there.
//
// The look follows the port's settings menu (ui/menu.cpp style_init / option_rows.cpp): a dark green panel, gold accents,
// white text, and selection shown by a border rather than a different plate. The window is deliberately three bands —
// title, breadcrumb, footer — around one scrolling file list, so its purpose and the current location are always visible
// (Nielsen Norman Group: visibility of system status, recognition rather than recall, minimalist design).
#include <stdio.h>
#include <string.h>

#include "imgui.h"

#include "ui/blueprint_picker.h"

extern "C" {
#include "host.h"
}

// The settings menu's palette: dark green chrome, a gold accent for the active things, white for the rest.
static const ImU32 kPanel = IM_COL32(15, 23, 20, 244);      // 0.06/0.09/0.08 at ~0.96 alpha
static const ImU32 kPanelEdge = IM_COL32(236, 194, 84, 90); // thin gold frame
static const ImU32 kBand = IM_COL32(21, 33, 27, 255);       // title / footer bands
static const ImU32 kCrumb = IM_COL32(30, 44, 36, 255);      // breadcrumb band
static const ImU32 kRule = IM_COL32(236, 194, 84, 55);      // separators
static const ImU32 kGold = IM_COL32(236, 194, 84, 255);
static const ImU32 kGreen = IM_COL32(56, 128, 92, 255);
static const ImU32 kGreenHover = IM_COL32(76, 158, 112, 255);
static const ImU32 kRow = IM_COL32(255, 255, 255, 16);      // idle row plate (option_rows.cpp's 22)
static const ImU32 kRowHover = IM_COL32(255, 255, 255, 40);
static const ImU32 kRowEdge = IM_COL32(255, 255, 255, 46);
static const ImU32 kText = IM_COL32(246, 244, 236, 255);
static const ImU32 kTextDim = IM_COL32(168, 178, 168, 255);
static const ImU32 kFolder = IM_COL32(150, 205, 255, 255);
static const ImU32 kFile = IM_COL32(226, 228, 220, 255);
static const ImU32 kTrack = IM_COL32(255, 255, 255, 24);
static const ImU32 kThumb = IM_COL32(236, 194, 84, 200);

static void text(ImDrawList *dl, ImFont *font, float size, float x, float y, ImU32 col, const char *s) {
    dl->AddText(font, size, ImVec2(x, y), col, s);
}

// A small vector icon drawn from primitives: a folder tab, a page with a folded corner, a drive disc, or a back arrow.
// The font has no such glyphs, so the distinction is geometric instead of a punctuation suffix like "dir/".
static void icon(ImDrawList *dl, int kind, float x, float cy, float s, ImU32 col) {
    switch (kind) {
    case BP_ICON_FOLDER: {
        ImVec2 a(x, cy - 4 * s), b(x + 13 * s, cy + 5 * s);
        dl->AddRectFilled(ImVec2(a.x, a.y + 3 * s), b, col, 2 * s);
        dl->AddRectFilled(ImVec2(a.x, a.y), ImVec2(a.x + 6 * s, a.y + 3 * s), col, 1 * s);
        break;
    }
    case BP_ICON_FILE: {
        ImVec2 a(x, cy - 5 * s), b(x + 10 * s, cy + 6 * s);
        dl->AddRectFilled(a, b, col, 1 * s);
        dl->AddTriangleFilled(ImVec2(b.x - 4 * s, a.y), ImVec2(b.x, a.y), ImVec2(b.x, a.y + 4 * s), kPanel);
        break;
    }
    case BP_ICON_DRIVE: {
        ImVec2 c(x + 6 * s, cy);
        dl->AddCircleFilled(c, 6 * s, col, 20);
        dl->AddCircleFilled(c, 2 * s, kPanel, 12);
        break;
    }
    case BP_ICON_UP: {
        dl->AddTriangleFilled(ImVec2(x + 5 * s, cy - 5 * s), ImVec2(x + 5 * s, cy + 5 * s),
                              ImVec2(x - 4 * s, cy), col);
        break;
    }
    default: break;
    }
}

// Paints one rounded plate and its outline, the way option_rows.cpp draws a settings row.
static void plate(ImDrawList *dl, float x, float y, float w, float h, float r, ImU32 bg, ImU32 edge, float thick) {
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), bg, r);
    if (edge) dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), edge, r, 0, thick);
}

int bp_picker_draw(float width, float height, float scale, int px, int py) {
    if (!bp_picker_is_open()) return 0;

    // The frame is pure derived data: it is computed from the picker's state, never incrementally maintained. One pass
    // gives the rectangles to hit-test against, then a second pass after the input always reflects this frame's state
    // (a wheel turn, a drag or a pad move all change g_scroll). That is what keeps the scrollbar thumb in step with the
    // list instead of lagging a frame or only moving when a folder changed.
    static BPickFrame f;
    bp_picker_layout_at(&f, (int)width, (int)height, scale, px, py);
    bp_picker_input(&f, px, py);
    if (!bp_picker_is_open()) return 0;   // this frame's input cancelled or chose a file: nothing left to draw
    // The preview follows the row under the pointer, so the second pass takes the pointer too: without it the column
    // only ever appeared once the pad cursor happened to move.
    bp_picker_layout_at(&f, (int)width, (int)height, scale, px, py);

    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImFont *font = ImGui::GetFont();
    const float s = f.scale;
    const float fs = font->FontSize;
    const float x = (float)f.x, y = (float)f.y, w = (float)f.w, h = (float)f.h;
    const float pad = 20 * s;

    // ---- panel
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), kPanel, 14 * s);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), kPanelEdge, 14 * s, 0, 1.5f * s);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, (float)f.header_bottom), kBand, 14 * s);
    dl->AddRectFilled(ImVec2(x, (float)f.crumb_top), ImVec2(x + w, (float)f.crumb_bottom), kCrumb, 0);
    dl->AddRectFilled(ImVec2(x, (float)f.footer_top), ImVec2(x + w, y + h), kBand, 14 * s);
    dl->AddLine(ImVec2(x, (float)f.header_bottom), ImVec2(x + w, (float)f.header_bottom), kRule, 1 * s);
    dl->AddLine(ImVec2(x, (float)f.crumb_bottom), ImVec2(x + w, (float)f.crumb_bottom), kRule, 1 * s);
    dl->AddLine(ImVec2(x, (float)f.footer_top), ImVec2(x + w, (float)f.footer_top), kRule, 1 * s);

    // ---- title band: a gold accent bar, the localised title, and the folder's status on the right
    dl->AddRectFilled(ImVec2(x + pad, y + 18 * s), ImVec2(x + pad + 4 * s, (float)f.header_bottom - 18 * s), kGold, 2 * s);
    text(dl, font, fs * 1.15f, x + pad + 16 * s, y + (float)(f.header_bottom - y - fs * 1.15f) * 0.5f, kText, f.title);
    {
        const char *s2 = f.status;
        float tw = ImGui::CalcTextSize(s2).x * 0.92f;
        text(dl, font, fs * 0.92f, x + w - pad - tw, y + (float)(f.header_bottom - y - fs) * 0.5f,
             f.error ? kGold : kTextDim, s2);
    }

    // ---- the preview column: a small 3D view of the highlighted blueprint, drawn before the rows so their plates never
    // sit on top of it. The CAI format stores each part's placement and nothing else, so a part is a box at its own
    // position and angle rather than its real model; the C layer has already projected and depth-sorted them.
    if (f.preview) {
        const float px = (float)f.preview_x, py = (float)f.preview_y;
        const float pw = (float)f.preview_w, ph = (float)f.preview_h;
        // A recessed panel for the view, with a faint grid floor so the build reads as standing on something.
        plate(dl, px - 8 * s, py - 8 * s, pw + 16 * s, ph + 16 * s, 10 * s, IM_COL32(0, 0, 0, 70), kRowEdge, 1 * s);
        for (int i = 1; i < 4; i++) {
            const float gy = py + ph * (float)i / 4.0f;
            dl->AddLine(ImVec2(px, gy), ImVec2(px + pw, gy), IM_COL32(255, 255, 255, 10), 1 * s);
        }
        if (f.preview_parts > 0) {
            // Each part is a flat quad (a box seen from above) scaled by its projected size and shaded by its depth. The
            // order the C layer produced is far to near, which is the whole depth solution a draw list needs.
            for (int k = 0; k < f.preview_parts; k++) {
                const float cx = px + f.preview_box[k].x * pw;
                const float cy = py + f.preview_box[k].y * ph;
                const float half = f.preview_box[k].s * pw * 0.5f;
                if (half < 0.5f) continue;
                const float a = f.preview_box[k].a;
                const ImU32 fill = IM_COL32((int)(236 * a), (int)(194 * a), (int)(84 * a), (int)(230 * a));
                const ImU32 edge = IM_COL32((int)(255 * a), (int)(228 * a), (int)(140 * a), (int)(255 * a));
                // The box is a quad; a part tipped from upright leans it sideways by its tilt, so a wheel or a lying
                // beam does not look like an upright post. tilt is 0 for upright, 1 for lying flat, and is capped at 2.
                const float lean = (f.preview_box[k].tilt > 1.0f ? 1.0f : f.preview_box[k].tilt) * half * 1.4f;
                dl->AddQuadFilled(ImVec2(cx - half - lean, cy - half * 1.1f), ImVec2(cx + half - lean, cy - half * 1.1f),
                                  ImVec2(cx + half + lean, cy + half * 1.1f), ImVec2(cx - half + lean, cy + half * 1.1f),
                                  fill);
                dl->AddQuad(ImVec2(cx - half - lean, cy - half * 1.1f), ImVec2(cx + half - lean, cy - half * 1.1f),
                            ImVec2(cx + half + lean, cy + half * 1.1f), ImVec2(cx - half + lean, cy + half * 1.1f),
                            edge, 1 * s);
            }
        }
        // The headline (part count or "not a blueprint") under the box, then the file name or the reason.
        const float tx = px - 8 * s;
        float ty = py + ph + 20 * s;
        text(dl, font, fs * 0.94f, tx, ty, f.preview_parts > 0 ? kGold : kTextDim, f.preview_title);
        ty += fs * 1.15f;
        text(dl, font, fs * 0.80f, tx, ty, kTextDim, f.preview_note);
    }

    // ---- the rectangles from the C layer: breadcrumbs, buttons, list rows
    const int sel = bp_picker_selected_row();
    const int hov_part = bp_picker_hover_part();
    int row_seen = 0, part_seen = 0;
    float cancel_right = x + pad;   // the footer hint starts just right of the Cancel button
    for (int i = 0; i < f.nrect; i++) {
        const BPickRect &r = f.rect[i];
        const float rx = (float)r.x, ry = (float)r.y, rw = (float)r.w, rh = (float)r.h;
        switch (r.kind) {
        case BP_RECT_PART: {
            const bool active = part_seen == hov_part;
            const bool current = r.last;
            // Breadcrumbs are text on the band, not a wall of plates: only the hovered one gets a plate.
            if (active) plate(dl, rx, ry, rw, rh, 6 * s, kGreenHover, 0, 0);
            const ImU32 col = current ? kGold : active ? kText : kTextDim;
            text(dl, font, fs * 0.94f, rx + 8 * s, ry + (rh - fs * 0.94f) * 0.5f, col, r.text);
            if (!current) {   // the "›" chevron, drawn as two short lines so no font glyph is needed
                float cx = rx + rw + 3 * s, cy = ry + rh * 0.5f;
                dl->AddLine(ImVec2(cx, cy - 4 * s), ImVec2(cx + 4 * s, cy), kTextDim, 1.5f * s);
                dl->AddLine(ImVec2(cx + 4 * s, cy), ImVec2(cx, cy + 4 * s), kTextDim, 1.5f * s);
            }
            part_seen++;
            break;
        }
        case BP_RECT_BUTTON: {
            // The first button is the compact breadcrumb "back" arrow; the last is Cancel in the footer.
            const bool back = ry < (float)f.crumb_bottom;
            const bool usable = !back || !f.at_pc;   // at "This PC" there is no folder above, so dim the arrow
            const ImU32 bg = !usable ? IM_COL32(255, 255, 255, 10)
                           : r.hovered ? kGreenHover : back ? kGreen : IM_COL32(255, 255, 255, 22);
            plate(dl, rx, ry, rw, rh, 8 * s, bg, (usable && r.hovered) ? kGold : kRowEdge,
                  (usable && r.hovered) ? 2 * s : 1 * s);
            if (back) {
                icon(dl, BP_ICON_UP, rx + rw * 0.5f - 8 * s, ry + rh * 0.5f, s, usable ? kText : kTextDim);
                // At "This PC" the path has no parts: name the location instead of leaving an empty band.
                if (f.at_pc) text(dl, font, fs * 0.94f, rx + rw + 14 * s, ry + (rh - fs * 0.94f) * 0.5f, kGold,
                                  f.crumb);
            } else {
                float tw = ImGui::CalcTextSize(r.text).x;
                text(dl, font, fs * 0.94f, rx + (rw - tw) * 0.5f, ry + (rh - fs * 0.94f) * 0.5f, kText, r.text);
                cancel_right = rx + rw;
            }
            break;
        }
        case BP_RECT_ROW: {
            const int entry = bp_picker_scroll() + row_seen;
            const bool selected = entry == sel;
            const bool active = r.hovered || selected;
            plate(dl, rx, ry, rw, rh, 9 * s, r.hovered ? kRowHover : kRow,
                  active ? kGold : kRowEdge, active ? 2 * s : 1 * s);
            const ImU32 fg = r.icon == BP_ICON_FILE ? kFile : kFolder;
            icon(dl, r.icon, rx + 14 * s, ry + rh * 0.5f, s, fg);
            text(dl, font, fs * 0.96f, rx + 34 * s, ry + (rh - fs * 0.96f) * 0.5f, fg, r.text);
            row_seen++;
            break;
        }
        default: break;
        }
    }

    // ---- empty / error state, centred in the list area (#1, #9): never a blank panel
    if (f.count == 0) {
        float tw = ImGui::CalcTextSize(f.status).x * 0.94f;
        text(dl, font, fs * 0.94f, x + (w - tw) * 0.5f, (float)(f.row_top + f.row_bottom) * 0.5f - fs * 0.5f,
             f.error ? kGold : kTextDim, f.status);
    }

    // ---- scrollbar, when the list is longer than the window (#1, #7). The thumb is wider than the thin track while
    // held, so dragging gives the same feedback a desktop scrollbar does.
    if (f.scroll_track_w > 0) {
        plate(dl, (float)f.scroll_track_x, (float)f.scroll_track_y, (float)f.scroll_track_w,
              (float)f.scroll_track_h, 3 * s, kTrack, 0, 0);
        const float drag = f.scroll_dragging ? 4 * s : 0;
        plate(dl, (float)f.scroll_track_x - drag, (float)f.scroll_thumb_y,
              (float)f.scroll_track_w + drag * 2, (float)f.scroll_thumb_h, 3 * s,
              f.scroll_dragging ? kGold : kThumb, 0, 0);
    }

    // ---- footer: the button prompts, drawn the way the game's own guide bar draws them. Each is a round key cap with
    // the pad button's letter inside and its word beside it; the accept key is gold and the rest are muted, so a player
    // reads "A Choose   B Cancel" instead of parsing a sentence. (#3, #10: user control, help and documentation.)
    {
        const float cap_r = fs * 0.62f;
        const float cap_gap = 8 * s;     // between a cap and its word
        const float pair_gap = 26 * s;   // between one prompt and the next
        const float cy = (float)f.footer_top + (float)(h - (f.footer_top - y)) * 0.5f;
        float cx = cancel_right + 20 * s;
        for (int i = 0; i < f.nprompt; i++) {
            const bool primary = f.prompt[i].primary != 0;
            const ImVec2 cap(cx + cap_r, cy);
            dl->AddCircleFilled(cap, cap_r, primary ? kGold : IM_COL32(255, 255, 255, 40), 20);
            // The letter, centred on the cap. A single character, so its own width is the measure.
            const ImVec2 ks = ImGui::CalcTextSize(f.prompt[i].key);
            text(dl, font, fs * 0.82f, cap.x - ks.x * 0.41f, cap.y - fs * 0.41f,
                 primary ? IM_COL32(20, 28, 24, 255) : kText, f.prompt[i].key);
            cx += cap_r * 2 + cap_gap;
            text(dl, font, fs * 0.86f, cx, cy - fs * 0.43f, primary ? kGold : kTextDim, f.prompt[i].label);
            cx += ImGui::CalcTextSize(f.prompt[i].label).x * 0.86f + pair_gap;
        }
    }
    return 1;
}
