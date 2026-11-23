// The blueprint picker's draw layer: it paints only the rectangles and text runtime/blueprint_picker.c computed, through
// ImGui's draw list. There is no ImGui widget and no ImGui layout here on purpose: the picker's state, hit tests, layout
// and every visual distinction (icon, hover, selection) live in C, so this file cannot trip ImGui's layout checks. The
// earlier all-widget version aborted the game there.
//
// The look follows the port's settings menu (ui/menu.cpp style_init / option_rows.cpp): a dark green panel, gold accents,
// white text, and selection shown by a border rather than a different plate. The window is deliberately three bands —
// title, breadcrumb, footer — around one scrolling file list, so its purpose and the current location are always visible
// (Nielsen Norman Group: visibility of system status, recognition rather than recall, minimalist design).
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"

#include "ui/blueprint_picker.h"
#include "ui/preview3d.h"

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

// The palette the game's own viewer gives a draft's parts, indexed by the actor's place in the list.
static const unsigned kActorPalette[21] = {
    0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF, 0xFFA500, 0x800080, 0xFFC0CB, 0x00FF7F,
    0xFFD700, 0x008080, 0x9400D3, 0x00CED1, 0xFF4500, 0x00FF99, 0xFF1493, 0xFF6347, 0x87CEEB, 0xFF69B4,
    0x32CD32,
};

// A palette entry darkened by the face's own brightness: the hue is which part it belongs to, the shade is the lighting.
static ImU32 actor_color(int index, float shade) {
    const unsigned rgb = kActorPalette[(unsigned)index % 21u];
    const float r = (float)((rgb >> 16) & 0xFF) * shade;
    const float g = (float)((rgb >> 8) & 0xFF) * shade;
    const float b = (float)(rgb & 0xFF) * shade;
    return IM_COL32((int)r, (int)g, (int)b, 235);
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
    // position and angle rather than its real model; the C layer has already projected every visible face, shaded it and
    // sorted the lot far to near. This only fills them in.
    if (f.preview) {
        const float px = (float)f.preview_x, py = (float)f.preview_y;
        const float pw = (float)f.preview_w, ph = (float)f.preview_h;
        // A recessed panel for the view, with a faint grid floor so the build reads as standing on something.
        plate(dl, px - 8 * s, py - 8 * s, pw + 16 * s, ph + 16 * s, 10 * s, IM_COL32(0, 0, 0, 70), kRowEdge, 1 * s);
        for (int i = 1; i < 4; i++) {
            const float gy = py + ph * (float)i / 4.0f;
            dl->AddLine(ImVec2(px, gy), ImVec2(px + pw, gy), IM_COL32(255, 255, 255, 10), 1 * s);
        }
        // The 3D view. When the GPU preview is ready it is one textured quad: the parts were rendered offscreen with a
        // real depth buffer, which is what makes the build read as solid. The CPU face list stays as the fallback for a
        // machine where the preview could not be created.
        const int gpu = preview3d_ready();
        bp_picker_use_gpu(gpu);
        if (gpu) {
            // The camera is the picker's own state: feed it to the renderer every frame, which re-renders only when it
            // actually changed. A different file also invalidates the cached image through the picker's stamp.
            float yaw = 0, pitch = 0;
            bp_picker_camera(&yaw, &pitch);
            preview3d_set_camera(yaw, pitch, bp_picker_zoom());
            const unsigned stamp = bp_picker_preview_stamp();
            static unsigned last_stamp;
            if (stamp != last_stamp) {
                last_stamp = stamp;
                preview3d_invalidate();
            }
        }
        if (gpu && preview3d_resize((int)pw, (int)ph) && preview3d_texture()) {
            dl->PushClipRect(ImVec2(px, py), ImVec2(px + pw, py + ph), true);
            float u = 1.0f, v = 1.0f;
            preview3d_uv(&u, &v);
            dl->AddImage((ImTextureID)preview3d_texture(), ImVec2(px, py), ImVec2(px + pw, py + ph),
                         ImVec2(0, 0), ImVec2(u, v));
            dl->PopClipRect();
        } else if (f.preview_faces > 0) {
            // Quad corners are placed analytically and can land outside the box when a build is viewed at an angle, so
            // the draw is clipped to the view: AddQuadFilled itself does not clip.
            dl->PushClipRect(ImVec2(px, py), ImVec2(px + pw, py + ph), true);
            for (int k = 0; k < f.preview_faces; k++) {
                const float sh = f.preview_face[k].shade;
                // The actor's own colour, the way the game's viewer paints a draft when "Show Colors" is on; the shade
                // is the lighting. A real model and a collision-shape fallback share the colour, so the build reads as one
                // object rather than as two kinds of thing.
                const ImU32 fill = actor_color(f.preview_face[k].color, sh);
                dl->AddQuadFilled(ImVec2(px + f.preview_face[k].x[0] * pw, py + f.preview_face[k].y[0] * ph),
                                  ImVec2(px + f.preview_face[k].x[1] * pw, py + f.preview_face[k].y[1] * ph),
                                  ImVec2(px + f.preview_face[k].x[2] * pw, py + f.preview_face[k].y[2] * ph),
                                  ImVec2(px + f.preview_face[k].x[3] * pw, py + f.preview_face[k].y[3] * ph), fill);
            }
            dl->PopClipRect();
        }
        // The headline (part count or "not a blueprint") under the box, then the file name or the reason. Both are clipped
        // to the column: a long file name used to run out of the panel and across the 3D view, which reads as the picture
        // being broken rather than as a long name.
        const float tx = px - 8 * s;
        float ty = py + ph + 20 * s;
        dl->PushClipRect(ImVec2(tx - 2 * s, ty - fs * 0.3f), ImVec2(px + pw, ty + fs * 2.1f), true);
        text(dl, font, fs * 0.94f, tx, ty, f.preview_faces > 0 ? kGold : kTextDim, f.preview_title);
        ty += fs * 1.15f;
        text(dl, font, fs * 0.80f, tx, ty, kTextDim, f.preview_note);
        dl->PopClipRect();
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
            // A name that does not fit scrolls, the way every file manager does it: the text slides left while the row is
            // hovered or selected and wraps round after a gap, instead of being cut or running across the preview column.
            // The clip keeps it inside its own row, which the C layer already narrowed for the preview column.
            const float text_left = (float)rx + 34 * s;
            const float avail = (float)rw - 34 * s - 8 * s;
            const float tw = ImGui::CalcTextSize(r.text).x;
            const float text_y = ry + (rh - fs * 0.96f) * 0.5f;
            dl->PushClipRect(ImVec2((float)rx + 2 * s, (float)ry),
                             ImVec2((float)rx + (float)rw - 6 * s, (float)ry + (float)rh), true);
            if (tw <= avail) {
                text(dl, font, fs * 0.96f, text_left, text_y, fg, r.text);
            } else {
                // Scrolling only on the row the player is looking at, so a list at rest stays still and readable.
                const float speed = 34 * s;   // pixels per second
                const float gap = 60 * s;     // between the end of the name and its own start again
                const float span = tw + gap;
                float off = 0.0f;
                if (active) off = fmodf((float)ImGui::GetTime() * speed, span);
                text(dl, font, fs * 0.96f, text_left - off, text_y, fg, r.text);
                if (active) text(dl, font, fs * 0.96f, text_left - off + span, text_y, fg, r.text);
            }
            dl->PopClipRect();
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
