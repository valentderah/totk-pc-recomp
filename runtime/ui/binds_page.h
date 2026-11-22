// The remapping rows of the menu's Controls tab (ui/binds_page.cpp). Present path only (ImGui).
#pragma once

extern "C" {
#include "ui_text.h"
}

// Inside the page's opt_begin / opt_end: every action's two keys and its controller button, remapped by selecting a
// cell and pressing the new one, and the reset of them all. binds_after_page, after opt_end: the window that waits for
// the press.
void binds_rows(float scale);
void binds_after_page(void);
// A cell waits for a press (or its press is still reaching the menu): B, Esc, L / R and the tab keys stay on the page.
bool binds_modal(void);
// A key setting (TOTK_ name) as a row of one key cell, set by pressing the key as a binding's cell is; Delete sets it
// to none. note (NULL none) follows the description.
void binds_key_row(const char *setting, UiTextId label, UiTextId desc, const char *note);
// A gamepad input by position (PAD_*): the label the open pad has on a face button, else its place on the pad.
const char *binds_pad_label(int pad);
