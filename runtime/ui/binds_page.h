// The remapping rows of the menu's Controls tab (ui/binds_page.cpp). Present path only (ImGui).
#pragma once

// Inside the page's opt_begin / opt_end: every action's two keys and its controller button, remapped by selecting a
// cell and pressing the new one, and the reset of them all. binds_after_page, after opt_end: the window that waits for
// the press.
void binds_rows(float scale);
void binds_after_page(void);
// A cell waits for a press (or its press is still reaching the menu): B, Esc, L / R and the tab keys stay on the page.
bool binds_modal(void);
// A gamepad input by position (PAD_*): the label the open pad has on a face button, else its place on the pad.
const char *binds_pad_label(int pad);
