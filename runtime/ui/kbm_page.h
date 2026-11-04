// The Keyboard & mouse tab (ui/kbm_page.cpp): the PC scheme's actions, per context or globally, with one key cell
// each. The override goes into TOTK_KBM_BINDS as "action=key" or "context.action=key"; the scheme applies it live.
#pragma once

// Inside the page's opt_begin / opt_end: the context picker and the action rows.
void kbm_rows(float scale);
// After opt_end: the window that waits for the new key, and the clear of an override.
void kbm_after_page(void);
// A key cell waits for a press: the tab keys, Esc and the menu stay on the page until it ends.
bool kbm_modal(void);
// The tab's own Chinese strings (the action and context names are not catalog rows): the font atlas builder visits
// them so their glyphs are in the merged system font (ui/menu.cpp fonts_up).
void kbm_labels_visit(void (*visit)(void *ctx, const char *text), void *ctx);
