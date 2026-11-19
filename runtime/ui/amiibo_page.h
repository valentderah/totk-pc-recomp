// Amiibo / NFC menu page (ui/amiibo_page.cpp).
#pragma once

void tab_amiibo(float scale);

// Collect amiibo filenames for font glyph building (called from menu.cpp fonts_up)
void amiibo_collect_filenames(void (*callback)(void *, const char *), void *ctx);

// Request font atlas rebuild on next frame (called when amiibo directory is refreshed)
void amiibo_request_font_rebuild(void);
