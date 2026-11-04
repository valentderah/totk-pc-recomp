// The game's current eui screen, for the PC scheme's context (TOTK_SCREEN_WATCH, 1.4.3). Read at the pad read on the
// game thread, like the menu cursor; the scheme's context in input.c comes from screen_watch_ctx.
#pragma once

#include <stdint.h>

// Once per pad read, on the game thread: buttons is the merged pad input (B_ZR selects AIM when no screen has focus).
void screen_watch_poll(uint64_t buttons);
// The KBM scope for the next scheme frame: the host F1 menu's PAUSE, else the game's screen, else AIM/WORLD.
int screen_watch_ctx(int host_menu_open);
