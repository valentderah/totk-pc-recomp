// Right-to-left text (Persian, in the Arabic script) for a renderer that draws one glyph per code point from left to
// right, as the in-game menu's Dear ImGui does. Two steps, both on UTF-8:
//   rtl_shape   gives every Arabic-script letter its joined form (the presentation forms U+FB50-FDFF, U+FE70-FEFF)
//               and keeps the logical order, so text widths, word wrapping and printf arguments stay as they are;
//   rtl_visual  puts one line of shaped text in display order: right to left, with runs of Latin letters and numbers
//               kept left to right and brackets mirrored.
// The menu shapes its catalog strings once (ui_text.c menu_text) and reorders each drawn line (ui/rtl_imgui.cpp).
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 1 when [s, end) holds an Arabic-script letter (in the Arabic block, base or presentation form; not a digit).
// end NULL: up to the terminating NUL.
int rtl_has(const char *s, const char *end);

// Shaped copy of the NUL-terminated s in out (at most cap bytes with its NUL); returns the length written. Arabic
// yeh and kaf become the Persian letters, heh + hamza above becomes ۀ, vowel marks and the zero-width (non-)joiners
// and direction marks are dropped. Text without Arabic-script letters comes out unchanged. A shaped letter takes
// 3 bytes against the 2 of its base letter, so cap = 2 * strlen(s) + 1 is always enough.
size_t rtl_shape(const char *s, char *out, size_t cap);

// Display order of the one line [s, end) (no '\n') in out, NUL-terminated; returns the length (the same as end - s).
// A line without Arabic-script letters is copied unchanged.
size_t rtl_visual(const char *s, const char *end, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
