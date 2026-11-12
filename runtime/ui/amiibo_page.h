// Amiibo / NFC menu page (ui/amiibo_page.cpp).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void tab_amiibo(float scale);
void amiibo_quick_tap(void);
void amiibo_quick_next(void);
void amiibo_quick_prev(void);
void amiibo_quick_menu(void);
void amiibo_toast_draw(float screen_w, float screen_h, float scale);

// Collect amiibo filenames for font glyph building (called from menu.cpp fonts_up)
void amiibo_collect_filenames(void (*callback)(void *, const char *), void *ctx);

// Request font atlas rebuild on next frame (called when amiibo directory is refreshed)
void amiibo_request_font_rebuild(void);

#ifdef __cplusplus
}
#endif
