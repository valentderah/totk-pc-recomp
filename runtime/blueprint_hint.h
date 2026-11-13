// The blueprint import entry on the game's own Autobuild screen (runtime/blueprint_hint.cpp): the ⊕ button and the word
// "Import" in the screen's bottom guide bar, that button opening the port's own file picker, and the chosen .cai or .txt
// going into the Autobuild history.
//
// The screen is recognised by its layout name, which runtime/menu_cursor.c reads for its cursor work: the Autobuild
// screen is AutoBuilder_00. The row is drawn by the present path's ImGui overlay, over the game image, so no archive of
// the game is edited and no mod, resolution or language of the game is affected.
//
// Threads: bp_hint_poll runs on the game thread, once per pad read (runtime/input.c), so the game's own "Receive" never
// fires from the button this entry owns. bp_hint_frame and bp_hint_draw run on the present path (ui/menu.cpp).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The focused screen this entry belongs to (menu_cursor.h layout_name).
#define BP_HINT_SCREEN "AutoBuilder_00"

// Once per pad read on the game thread, next to menu_cursor_poll: notes whether the Autobuild screen is up, and while it
// is, takes the import button out of *buttons so the game does not act on it.
void bp_hint_poll(uint64_t *buttons);

// 1 while the Autobuild screen is up and the entry is enabled (TOTK_BLUEPRINT_HINT: 0 turns it off).
int bp_hint_active(void);

// Once per presented frame, next to the menu's own per-frame work: starts the import of a file the dialog returned, and
// reads the button that opens the picker.
void bp_hint_frame(void);

// Drawn inside the overlay's ImGui frame, after ImGui::NewFrame: the row, placed on the game's picture through
// gfx.h gfx_picture_rect. width and height are the swapchain's, scale the menu's pixel scale.
void bp_hint_draw(int width, int height, float scale);

#ifdef __cplusplus
}
#endif
