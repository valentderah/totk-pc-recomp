// Right-to-left lines for Dear ImGui (rtl_imgui.h). ImGui draws the code points of a string one after the other, left
// to right. The menu's catalog strings come to it already shaped (ui_text.c menu_text: joined letter forms, logical
// order), so every width ImGui measures and every wrap point it finds is the width and the wrap point of the text as
// shown: only the order of the glyphs on a line is wrong. This hook, called at the top of ImFont::RenderText
// (third_party/imgui/imgui_draw.cpp, GImFontRenderTextHook), splits the text into the lines RenderText would draw and
// draws each one in display order (rtl_text.c rtl_visual) through RenderText itself. A line keeps its glyphs and so
// its width; the lines of a wrapped block are moved to the right edge of the wrap width.
#include "ui/rtl_imgui.h"

#include <float.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"   // ImTextCharFromUtf8

extern "C" {
#include "rtl_text.h"
}

extern bool (*GImFontRenderTextHook)(ImFont *font, ImDrawList *draw_list, float size, const ImVec2 &pos, ImU32 col,
                                     const ImVec4 &clip_rect, const char *text_begin, const char *text_end,
                                     float wrap_width, bool cpu_fine_clip);

namespace {

bool g_inside;   // ImGui draws on one thread; the lines go back through RenderText, which must not come here again

// The blanks after a wrap point and one line break: imgui_draw.cpp CalcWordWrapNextLineStartA.
const char *next_line_start(const char *s, const char *end) {
    while (s < end && (*s == ' ' || *s == '\t')) s++;
    if (s < end && *s == '\n') s++;
    return s;
}

void draw_line(ImFont *font, ImDrawList *dl, float size, float x, float y, float right, ImU32 col, const ImVec4 &clip,
               const char *b, const char *e, bool cpu_fine_clip) {
    char stack[1024];
    size_t cap = (size_t)(e - b) + 1;
    char *vis = cap <= sizeof stack ? stack : (char *)IM_ALLOC(cap);
    size_t n = rtl_visual(b, e, vis, cap);
    if (right > 0) {   // wrapped block: right-aligned within the wrap width
        float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, vis, vis + n).x;
        if (w < right) x += right - w;
    }
    font->RenderText(dl, size, ImVec2(x, y), col, clip, vis, vis + n, 0.0f, cpu_fine_clip);
    if (vis != stack) IM_FREE(vis);
}

// The line structure of ImFont::RenderText: a wrap point is found at the start of a line and kept across a '\n'
// inside it, a wrap skips the blanks after it, '\n' starts a new line.
bool render_text_rtl(ImFont *font, ImDrawList *dl, float size, const ImVec2 &pos, ImU32 col, const ImVec4 &clip,
                     const char *text_begin, const char *text_end, float wrap_width, bool cpu_fine_clip) {
    if (g_inside) return false;
    if (!text_end) text_end = text_begin + strlen(text_begin);
    if (!rtl_has(text_begin, text_end)) return false;
    g_inside = true;
    const float scale = size / font->FontSize, line_height = font->FontSize * scale;
    const bool wrap = wrap_width > 0.0f;
    float y = pos.y;
    const char *s = text_begin, *line = s, *eol = nullptr;
    while (s < text_end && y <= clip.w) {
        if (wrap) {
            if (!eol) eol = font->CalcWordWrapPositionA(scale, s, text_end, wrap_width);
            if (s >= eol) {
                if (y + line_height >= clip.y)
                    draw_line(font, dl, size, pos.x, y, wrap_width, col, clip, line, s, cpu_fine_clip);
                y += line_height;
                s = line = next_line_start(s, text_end);
                eol = nullptr;
                continue;
            }
        }
        if (*s == '\n') {
            if (y + line_height >= clip.y)
                draw_line(font, dl, size, pos.x, y, wrap ? wrap_width : 0.0f, col, clip, line, s, cpu_fine_clip);
            y += line_height;
            line = ++s;
            continue;
        }
        unsigned int c;
        s += (unsigned char)*s < 0x80 ? 1 : ImTextCharFromUtf8(&c, s, text_end);
    }
    if (line < text_end && y <= clip.w && y + line_height >= clip.y)
        draw_line(font, dl, size, pos.x, y, wrap ? wrap_width : 0.0f, col, clip, line, text_end, cpu_fine_clip);
    g_inside = false;
    return true;
}

} // namespace

void rtl_imgui_install(void) { GImFontRenderTextHook = render_text_rtl; }
