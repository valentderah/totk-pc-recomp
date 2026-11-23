// totk-config: the window's icons (cfgr_icons.c), drawn with Nuklear's shapes into a square.
#pragma once
#include "configurator/cfgr_nk.h"

typedef enum {
    ICON_NONE,
    // tabs
    ICON_SCREEN, ICON_GAME, ICON_SOUND, ICON_CONTROLS, ICON_AMIIBO, ICON_MENU, ICON_ADVANCED, ICON_MODS, ICON_EXPERT,
    ICON_OTHER, ICON_UPDATES,
    // controls and marks
    ICON_SEARCH, ICON_FOLDER, ICON_GLOBE, ICON_RESET, ICON_RESTART, ICON_BOLT, ICON_PLAY, ICON_PLUS, ICON_PENCIL,
    ICON_TRASH, ICON_WARN, ICON_CLOSE, ICON_CHECK, ICON_DOT, ICON_RING, ICON_INFO, ICON_UP, ICON_DOWN,
    ICON_COUNT
} Icon;

// Draws icon into the square centred in r, in col.
void icon_draw(struct nk_command_buffer *c, Icon icon, struct nk_rect r, struct nk_color col);
