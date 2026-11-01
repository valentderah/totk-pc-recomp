// Text the host shows the player (runtime/ui_text.c): one catalog, keyed by stable ids. The software keyboard reads it
// in the game's language (ui_text); the menu and the performance overlay read it in the menu language (menu_text).
#pragma once

typedef enum {
    // Software keyboard
    UI_SWKBD_TITLE,          // caption of the text-entry window
    UI_SWKBD_PROMPT,         // prompt when the game gives no header, guide or sub text (TotK names a horse)
    UI_SWKBD_DEFAULT_NAME,   // text the entry starts with when the game gives none, so a controller can confirm it
    UI_SWKBD_OK,
    UI_SWKBD_CANCEL,
    UI_SWKBD_PAD_HINT,       // how a controller answers

    // Menu: window, tabs, bottom row. UI_MENU_FIRST..UI_MENU_LAST_ID carry English, Russian, Brazilian Portuguese and
    // Simplified Chinese texts.
    UI_MENU_FIRST,
    UI_MENU_TITLE = UI_MENU_FIRST,
    UI_MENU_TAB_SCREEN, UI_MENU_TAB_GAME, UI_MENU_TAB_MODS, UI_MENU_TAB_SOUND, UI_MENU_TAB_CONTROLS, UI_MENU_TAB_AMIIBO,
    UI_MENU_TAB_MENU, UI_MENU_TAB_ADVANCED,
    UI_MENU_CONTINUE, UI_MENU_QUIT,
    UI_MENU_ON, UI_MENU_OFF, UI_MENU_AUTO, UI_MENU_DEFAULT_MARK,      // "default" beside the default value
    UI_MENU_AFTER_RESTART,    // note on a setting that is read at start
    UI_MENU_SET_BY_ENV,       // note on a setting an environment variable fixes
    UI_MENU_SAVE_FAILED,
    UI_MENU_VERSION,          // "TotK Recomp %s, game %s": the port version and the game update, beside the title

    // Screen
    UI_MENU_FULLSCREEN, UI_MENU_FULLSCREEN_DESC,
    UI_MENU_WINDOW_SIZE, UI_MENU_WINDOW_SIZE_DESC, UI_MENU_WINDOW_SIZE_CUSTOM,
    UI_MENU_PRESENT, UI_MENU_PRESENT_DESC, UI_MENU_PRESENT_MAILBOX, UI_MENU_PRESENT_VSYNC, UI_MENU_PRESENT_IMMEDIATE,
    UI_MENU_PRESENT_IN_USE,   // "in use: %s" under the value when the swapchain got another mode than the setting
    UI_MENU_GPU, UI_MENU_GPU_DESC, UI_MENU_GPU_AUTO,
    UI_MENU_PERF_OVERLAY, UI_MENU_PERF_OVERLAY_DESC, UI_MENU_PERF_OFF, UI_MENU_PERF_FPS, UI_MENU_PERF_FULL,
    UI_PERF_LINE_FPS,
    UI_MENU_GRAPHICS,         // group heading over the resolution rows
    UI_MENU_OUTPUT_RES, UI_MENU_OUTPUT_RES_DESC,
    UI_MENU_RENDER_SCALE, UI_MENU_RENDER_SCALE_DESC, UI_MENU_RENDER_SCALE_GAME,
    UI_MENU_UPSCALER, UI_MENU_UPSCALER_DESC, UI_MENU_UPSCALER_FSR, UI_MENU_UPSCALER_BILINEAR,
    UI_MENU_ASPECT, UI_MENU_ASPECT_DESC, UI_MENU_ASPECT_FIT, UI_MENU_ASPECT_STRETCH,
    UI_MENU_FOV, UI_MENU_FOV_DESC,
    UI_MENU_SHADOW_RES, UI_MENU_SHADOW_RES_DESC, UI_MENU_SHADOW_RES_GAME,
    UI_MENU_ANISO, UI_MENU_ANISO_DESC, UI_MENU_ANISO_GAME,
    UI_MENU_SHARPNESS, UI_MENU_SHARPNESS_DESC,
    UI_MENU_COLOR,            // group heading over the colour rows
    UI_MENU_COLOR_PRESET, UI_MENU_COLOR_PRESET_DESC, UI_MENU_COLOR_DEFAULT, UI_MENU_COLOR_VIVID, UI_MENU_COLOR_CLARITY,
    UI_MENU_COLOR_CUSTOM,
    UI_MENU_COLOR_BRIGHTNESS, UI_MENU_COLOR_BRIGHTNESS_DESC, UI_MENU_COLOR_CONTRAST, UI_MENU_COLOR_CONTRAST_DESC,
    UI_MENU_COLOR_SATURATION, UI_MENU_COLOR_SATURATION_DESC, UI_MENU_COLOR_GAMMA, UI_MENU_COLOR_GAMMA_DESC,
    UI_MENU_HDR_GROUP,        // group heading over the HDR rows
    UI_MENU_HDR, UI_MENU_HDR_DESC, UI_MENU_HDR_PEAK, UI_MENU_HDR_PEAK_DESC, UI_MENU_HDR_GAME, UI_MENU_HDR_GAME_DESC,
    UI_MENU_HDR_UI, UI_MENU_HDR_UI_DESC,
    UI_MENU_SHADOW_SCALE, UI_MENU_SHADOW_SCALE_DESC,
    UI_MENU_SHADOW_RANGE, UI_MENU_SHADOW_RANGE_DESC,
    UI_MENU_CONTACT_SHADOWS, UI_MENU_CONTACT_SHADOWS_DESC,
    UI_MENU_PCSS, UI_MENU_PCSS_DESC,
    UI_MENU_DLAA, UI_MENU_DLAA_DESC, UI_MENU_DLSS, UI_MENU_DLSS_DESC, UI_MENU_DLSS_DLAA, UI_MENU_DLSS_QUALITY,
    UI_MENU_DLSS_BALANCED, UI_MENU_DLSS_PERFORMANCE, UI_MENU_FXAA, UI_MENU_FXAA_DESC,

    // Game
    UI_MENU_FPS_LIMIT, UI_MENU_FPS_LIMIT_DESC, UI_MENU_FPS_30, UI_MENU_FPS_40, UI_MENU_FPS_45, UI_MENU_FPS_60, UI_MENU_FPS_90,
    UI_MENU_FPS_120, UI_MENU_FPS_144,
    UI_MENU_FPS_DISPLAY, UI_MENU_FPS_DISPLAY_HZ,
    UI_MENU_REALTIME, UI_MENU_REALTIME_DESC,
    UI_MENU_CLOTH_REALTIME, UI_MENU_CLOTH_REALTIME_DESC,
    UI_MENU_HIGHFPS_FIX, UI_MENU_HIGHFPS_FIX_DESC,
    UI_MENU_GAME_LANG, UI_MENU_GAME_LANG_DESC, UI_MENU_GAME_LANG_AUTO,
    UI_MENU_OPERATION_MODE, UI_MENU_OPERATION_MODE_DESC, UI_MENU_MODE_DOCKED, UI_MENU_MODE_HANDHELD,
    UI_MENU_ASSUME_FOCUS, UI_MENU_ASSUME_FOCUS_DESC,

    // Mods: the port's own patches and the player's mods folder (mods.c)
    UI_MENU_MODS_BUILTIN, UI_MENU_MODS_INSTALLED,
    UI_MENU_MODS_NONE,           // no mod folder yet: where to unpack one
    UI_MENU_MODS_OPEN, UI_MENU_MODS_OPEN_DESC,
    UI_MENU_MODS_ROW_DESC,       // printf: files it replaces, files it adds (as of this start)
    UI_MENU_MODS_OVERRIDDEN,     // printf: files a higher mod serves instead, that mod's name
    UI_MENU_MODS_MOVE_HINT,
    UI_MENU_MODS_EMPTY, UI_MENU_MODS_SUBSDK, UI_MENU_MODS_EXEFS_LATER, UI_MENU_MODS_BAD_NAME,
    UI_MENU_MODS_EXEFS_UNSUPPORTED,  // printf: the file and why
    UI_MENU_MODS_EXEFS_OTHER,
    UI_MENU_MODS_EXEFS_ON,           // printf: its ExeFS patch files in use
    UI_MENU_MODS_REPLACES_BUILTIN,   // printf: its ExeFS patch files in use, the port's own patch sets they turn off
    UI_MENU_MODS_OTHER_VERSION,  // printf: files made for another game version, left out
    UI_MENU_MODS_UNUSED,         // under the switch of a mod the port cannot use
    UI_MENU_MODS_CHEATS,         // printf: its cheats as read at start
    UI_MENU_MODS_CHEATS_FILE,    // printf: <build id>.txt, when none were read
    UI_MENU_MODS_FPS60, UI_MENU_MODS_FPS60_DESC, UI_MENU_MODS_RES, UI_MENU_MODS_RES_DESC,
    UI_MENU_MODS_QUICKMENU, UI_MENU_MODS_QUICKMENU_DESC,

    // Sound
    UI_MENU_VOLUME, UI_MENU_VOLUME_DESC,

    // Controls
    UI_MENU_MOTION, UI_MENU_MOTION_DESC,
    UI_MENU_RUMBLE_STRENGTH, UI_MENU_RUMBLE_STRENGTH_DESC,
    UI_MENU_RUMBLE_CURVE, UI_MENU_RUMBLE_CURVE_DESC,
    UI_MENU_RUMBLE_TRIGGERS, UI_MENU_RUMBLE_TRIGGERS_DESC,
    UI_MENU_PAD_LAYOUT, UI_MENU_PAD_LAYOUT_DESC, UI_MENU_PAD_LAYOUT_AUTO, UI_MENU_PAD_LAYOUT_NINTENDO,
    UI_MENU_PAD_LAYOUT_XBOX, UI_MENU_PAD_LAYOUT_IN_USE,
    UI_MENU_PROMPTS, UI_MENU_PROMPTS_DESC, UI_MENU_PROMPTS_NINTENDO, UI_MENU_PROMPTS_XBOX,
    // the notes under the prompts: their files are missing or damaged, they change after restart, or why they are off
    UI_MENU_PROMPTS_MISSING, UI_MENU_PROMPTS_FAILED, UI_MENU_PROMPTS_RESTART, UI_MENU_PROMPTS_OFF_SHAPE,
    UI_MENU_PROMPTS_OFF_MODS, UI_MENU_PROMPTS_OFF_PATCHES,
    // "Restart now" under the prompts while they change after restart, and its confirmation (Cancel: UI_MENU_RESET_NO)
    UI_MENU_RESTART, UI_MENU_RESTART_DESC, UI_MENU_RESTART_CONFIRM, UI_MENU_RESTART_YES, UI_MENU_RESTART_SAVING,
    UI_MENU_LSTICK_DEADZONE, UI_MENU_RSTICK_DEADZONE, UI_MENU_STICK_DEADZONE_DESC,
    UI_MENU_STICK_DEADZONE_NOW, UI_MENU_STICK_SENS, UI_MENU_STICK_SENS_DESC,
    UI_MENU_MOUSE_SENS, UI_MENU_MOUSE_SENS_DESC,
    UI_MENU_WALK_KEY, UI_MENU_WALK_KEY_DESC, UI_MENU_WALK_KEY_TAKEN, UI_MENU_WALK_SCALE, UI_MENU_WALK_SCALE_DESC,

    // Controls tab: the remapping rows (ui/binds_page.cpp)
    UI_BINDS_HELP, UI_BINDS_COL_KEY1, UI_BINDS_COL_KEY2, UI_BINDS_COL_PAD,
    UI_BINDS_GROUP_BUTTONS, UI_BINDS_GROUP_MOVE, UI_BINDS_GROUP_CAMERA,
    UI_BINDS_PRESS_KEY,       // "Press a key or mouse button for %s"
    UI_BINDS_PRESS_PAD,       // "Press a controller button for %s"
    UI_BINDS_CANCEL_HINT,     // "Esc cancels, or wait %d s"
    UI_BINDS_TAKEN,           // "%s was taken from %s": the key, the action that lost it
    UI_BINDS_SWAPPED,         // "%s swapped with %s": the two actions
    UI_BINDS_RESET, UI_BINDS_RESET_DESC,
    // the actions, in action order (input_map.h B_A .. B_SR_DOWN)
    UI_BIND_A, UI_BIND_B, UI_BIND_X, UI_BIND_Y, UI_BIND_LCLICK, UI_BIND_RCLICK, UI_BIND_L, UI_BIND_R, UI_BIND_ZL,
    UI_BIND_ZR, UI_BIND_PLUS, UI_BIND_MINUS, UI_BIND_LEFT, UI_BIND_UP, UI_BIND_RIGHT, UI_BIND_DOWN,
    UI_BIND_LSTICK_LEFT, UI_BIND_LSTICK_UP, UI_BIND_LSTICK_RIGHT, UI_BIND_LSTICK_DOWN,
    UI_BIND_RSTICK_LEFT, UI_BIND_RSTICK_UP, UI_BIND_RSTICK_RIGHT, UI_BIND_RSTICK_DOWN,
    // what an action does in the world, in action order up to Minus
    UI_BIND_DESC_A, UI_BIND_DESC_B, UI_BIND_DESC_X, UI_BIND_DESC_Y, UI_BIND_DESC_LCLICK, UI_BIND_DESC_RCLICK,
    UI_BIND_DESC_L, UI_BIND_DESC_R, UI_BIND_DESC_ZL, UI_BIND_DESC_ZR, UI_BIND_DESC_PLUS, UI_BIND_DESC_MINUS,
    UI_BIND_DESC_DPAD, UI_BIND_DESC_MOVE, UI_BIND_DESC_CAMERA,
    // key names, in key id order KEY_SPACE .. KEY_PAGEDOWN (letters, digits, F keys and punctuation show as themselves)
    UI_KEY_SPACE, UI_KEY_TAB, UI_KEY_LSHIFT, UI_KEY_LCTRL, UI_KEY_UP, UI_KEY_DOWN, UI_KEY_LEFT, UI_KEY_RIGHT,
    UI_KEY_MOUSE_L, UI_KEY_MOUSE_R, UI_KEY_MOUSE_M, UI_KEY_MOUSE_X1, UI_KEY_MOUSE_X2,
    UI_KEY_ENTER, UI_KEY_BACKSPACE, UI_KEY_RSHIFT, UI_KEY_RCTRL, UI_KEY_LALT, UI_KEY_CAPSLOCK, UI_KEY_INSERT,
    UI_KEY_DELETE, UI_KEY_HOME, UI_KEY_END, UI_KEY_PAGEUP, UI_KEY_PAGEDOWN,
    UI_KEY_NUM_FMT,           // "Num %s": a numpad key
    // gamepad buttons, in pad id order PAD_SOUTH .. PAD_RPADDLE2, then the PlayStation face labels
    UI_PAD_SOUTH, UI_PAD_EAST, UI_PAD_WEST, UI_PAD_NORTH, UI_PAD_LSTICK, UI_PAD_RSTICK, UI_PAD_L, UI_PAD_R,
    UI_PAD_START, UI_PAD_BACK, UI_PAD_DPAD_LEFT, UI_PAD_DPAD_UP, UI_PAD_DPAD_RIGHT, UI_PAD_DPAD_DOWN, UI_PAD_ZL,
    UI_PAD_ZR, UI_PAD_LPADDLE1, UI_PAD_RPADDLE1, UI_PAD_LPADDLE2, UI_PAD_RPADDLE2,
    UI_PAD_CROSS, UI_PAD_CIRCLE, UI_PAD_SQUARE, UI_PAD_TRIANGLE,

    // Amiibo tab (ui/amiibo_page.cpp): the dump folder and its files, the emulated NFC reader and its state, the
    // amiibo on it and the last action
    UI_MENU_AMIIBO_DIR, UI_MENU_AMIIBO_OPEN, UI_MENU_AMIIBO_DEFAULT_DIR, UI_MENU_AMIIBO_REFRESH, UI_MENU_AMIIBO_PICK,
    UI_MENU_AMIIBO_NO_DIR, UI_MENU_AMIIBO_READER, UI_MENU_AMIIBO_STATE, UI_MENU_AMIIBO_STATE_IDLE,
    UI_MENU_AMIIBO_STATE_SEARCHING, UI_MENU_AMIIBO_STATE_FOUND, UI_MENU_AMIIBO_STATE_MOUNTED,
    UI_MENU_AMIIBO_STATE_OFF, UI_MENU_AMIIBO_FILE, UI_MENU_AMIIBO_NO_FILE, UI_MENU_AMIIBO_TAP, UI_MENU_AMIIBO_REMOVE,
    UI_MENU_AMIIBO_AUTO_REMOVE, UI_MENU_AMIIBO_HOLD, UI_MENU_AMIIBO_SECONDS, UI_MENU_AMIIBO_TAG,
    UI_MENU_AMIIBO_NO_TAG, UI_MENU_AMIIBO_TAG_FILE, UI_MENU_AMIIBO_TAG_ID, UI_MENU_AMIIBO_TAG_UID,
    UI_MENU_AMIIBO_READY, UI_MENU_AMIIBO_TAPPED, UI_MENU_AMIIBO_LOAD_FAILED, UI_MENU_AMIIBO_REMOVED,
    UI_MENU_AMIIBO_STATUS,

    // Menu tab
    UI_MENU_MENU_LANG, UI_MENU_MENU_LANG_DESC, UI_MENU_MENU_LANG_AUTO,

    // Language names, each in its own language and the same in every row (game language and menu language lists)
    UI_MENU_LANGNAME_EN, UI_MENU_LANGNAME_JA, UI_MENU_LANGNAME_FR, UI_MENU_LANGNAME_DE, UI_MENU_LANGNAME_IT,
    UI_MENU_LANGNAME_ES, UI_MENU_LANGNAME_NL, UI_MENU_LANGNAME_RU, UI_MENU_LANGNAME_ZH_HANS,
    UI_MENU_LANGNAME_ZH_HANT, UI_MENU_LANGNAME_KO, UI_MENU_LANGNAME_PT_BR,

    // Advanced
    UI_MENU_ADV_WARNING,
    UI_MENU_GROUP_SCHED, UI_MENU_GROUP_SHADERS, UI_MENU_GROUP_TEXMEM, UI_MENU_GROUP_FIXES, UI_MENU_GROUP_VULKAN,
    UI_MENU_GROUP_DIAG,
    UI_MENU_ASYNC_PREEMPT, UI_MENU_ASYNC_PREEMPT_DESC, UI_MENU_PREEMPT_OFF, UI_MENU_PREEMPT_ON,
    UI_MENU_PREEMPT_INHERIT,
    UI_MENU_REPLAY_ASYNC, UI_MENU_REPLAY_ASYNC_DESC,
    UI_MENU_TIMER_1MS, UI_MENU_TIMER_1MS_DESC,
    UI_MENU_PIPE_WAIT, UI_MENU_PIPE_WAIT_DESC, UI_MENU_PIPE_WAIT_DEFAULT, UI_MENU_PIPE_WAIT_DRAW,
    UI_MENU_PIPE_SKIP, UI_MENU_PIPE_SKIP_DESC, UI_MENU_PIPE_SKIP_STRICT, UI_MENU_PIPE_SKIP_ANY,
    UI_MENU_PIPE_SKIP_FULLSCREEN,
    UI_MENU_LIB_PREBUILD, UI_MENU_LIB_PREBUILD_DESC,
    UI_MENU_LIB_PREBUILD_THREADS, UI_MENU_LIB_PREBUILD_THREADS_DESC,
    UI_MENU_PIPE_PREBUILD, UI_MENU_PIPE_PREBUILD_DESC,
    UI_MENU_PREWARM_XLATE, UI_MENU_PREWARM_XLATE_DESC,
    UI_MENU_GPL, UI_MENU_GPL_DESC,
    UI_MENU_ASTC, UI_MENU_ASTC_DESC, UI_MENU_ASTC_AUTO, UI_MENU_ASTC_GPU, UI_MENU_ASTC_NATIVE, UI_MENU_ASTC_CPU,
    UI_MENU_MEMORY, UI_MENU_MEMORY_DESC, UI_MENU_MEMORY_AUTO, UI_MENU_MEMORY_IMPORT, UI_MENU_MEMORY_MIRROR,
    UI_MENU_MEMORY_VERIFY,
    UI_MENU_TEX_ALIAS, UI_MENU_TEX_ALIAS_DESC,
    UI_MENU_TEX_VRAM_PRESSURE, UI_MENU_TEX_VRAM_PRESSURE_DESC,
    UI_MENU_ASTC_BC, UI_MENU_ASTC_BC_DESC,
    UI_MENU_LOD_GUARD, UI_MENU_LOD_GUARD_DESC,
    UI_MENU_FIX_C0, UI_MENU_FIX_C0_DESC,
    UI_MENU_FIX_ZERO, UI_MENU_FIX_ZERO_DESC,
    UI_MENU_BIND_SKIP, UI_MENU_BIND_SKIP_DESC,
    UI_MENU_SHADER_MEMO, UI_MENU_SHADER_MEMO_DESC,
    UI_MENU_PIPE_MEMO, UI_MENU_PIPE_MEMO_DESC,
    UI_MENU_TEX_MEMO, UI_MENU_TEX_MEMO_DESC,
    UI_MENU_DESC_TEMPLATE, UI_MENU_DESC_TEMPLATE_DESC,
    UI_MENU_PUSH_SKIP, UI_MENU_PUSH_SKIP_DESC,
    UI_MENU_SPLIT_DRAWS, UI_MENU_SPLIT_DRAWS_DESC,
    UI_MENU_VK_VALIDATION, UI_MENU_VK_VALIDATION_DESC, UI_MENU_VK_VALIDATION_MISSING,
    UI_MENU_VK_DYN_VERTEX, UI_MENU_VK_DYN_VERTEX_DESC, UI_MENU_VK_DYN_A2C, UI_MENU_VK_DYN_A2C_DESC,
    UI_MENU_VK_SUBGROUP_PIN, UI_MENU_VK_SUBGROUP_PIN_DESC,
    UI_MENU_TRACE, UI_MENU_TRACE_DESC,
    UI_MENU_FLIGHT, UI_MENU_FLIGHT_DESC,
    UI_MENU_FLIGHT_SPIKE_MS, UI_MENU_FLIGHT_SPIKE_MS_DESC,
    UI_MENU_INPUT_RECORD, UI_MENU_INPUT_RECORD_DESC,
    UI_MENU_OPEN_LOG, UI_MENU_OPEN_LOG_DESC,
    UI_MENU_COPY_INFO, UI_MENU_COPY_INFO_DESC, UI_MENU_COPY_INFO_DONE,
    UI_MENU_ACTION_FAILED,    // beside a button whose action did not work (no clipboard, no folder)
    UI_MENU_RESET, UI_MENU_RESET_DESC, UI_MENU_RESET_CONFIRM, UI_MENU_RESET_YES, UI_MENU_RESET_NO,

    // Performance overlay
    UI_MENU_OV_FPS, UI_MENU_OV_FRAME_MS, UI_MENU_OV_GAME_PCT, UI_MENU_OV_MAX_MS,
    UI_MENU_OV_GPU, UI_MENU_OV_CPU, UI_MENU_OV_RAM, UI_MENU_OV_VRAM, UI_MENU_OV_TEMP, UI_MENU_OV_POWER,
    UI_MENU_OV_CLOCK, UI_MENU_OV_NONE,
    UI_MENU_OV_REPLAY, UI_MENU_OV_GPU_TIME, UI_MENU_OV_PIPE_WAIT, UI_MENU_OV_SHADER_WAIT, UI_MENU_OV_TEX_UPLOADS,
    // number formats of the overlay's cells (printf, one number each)
    UI_MENU_OV_FMT_PCT, UI_MENU_OV_FMT_TEMP, UI_MENU_OV_FMT_POWER, UI_MENU_OV_FMT_MHZ, UI_MENU_OV_FMT_GB,
    UI_MENU_OV_FMT_GB_OF, UI_MENU_OV_GAME_GB, UI_MENU_OV_FMT_MS, UI_MENU_OV_PER_FRAME,
    // the picture lines: window, game image and 3D scene sizes, the scaling to the window, frame limit and presentation
    UI_MENU_OV_WINDOW, UI_MENU_OV_OUTPUT, UI_MENU_OV_SCENE, UI_MENU_OV_SCALING, UI_MENU_OV_LIMIT, UI_MENU_OV_PRESENT,
    UI_MENU_OV_FMT_SIZE,      // "%ux%u"
    UI_MENU_OV_FMT_SCENE,     // "%ux%u, %.0f%%": the scene and its share of the game image's width
    UI_MENU_OV_FMT_FSR,       // "AMD FSR 1, sharpness %d%%"
    UI_MENU_OV_SCALE_NONE, UI_MENU_OV_SCALE_BILINEAR,
    UI_MENU_OV_SCALE_DOWN,    // FSR is selected but the image is larger than the window: FSR only enlarges
    // the Cheats tab (cheats.c): the file, the kinds of cheat and why one cannot be switched
    UI_MENU_TAB_CHEATS,
    UI_MENU_CHEATS_NONE,      // "%s": <build id>.txt
    UI_MENU_CHEATS_MOD,       // "%s": the mod, "%d": its cheats
    UI_MENU_CHEATS_REBUILD, UI_MENU_CHEATS_REBUILD_DESC,   // restart into the rebuild for unbuilt code cheats
    UI_MENU_CHEAT_CODE_DESC, UI_MENU_CHEAT_KEY_DESC /* "%s": the buttons */, UI_MENU_CHEAT_DATA_DESC,
    UI_MENU_CHEAT_MASTER, UI_MENU_CHEAT_MASTER_DESC, UI_MENU_CHEAT_BUILTIN, UI_MENU_CHEAT_BUILTIN_DESC,
    UI_MENU_CHEAT_RESTORE, UI_MENU_CHEAT_RESTORE_DESC,
    UI_MENU_CHEAT_REBUILD, UI_MENU_CHEAT_REBUILD_DESC, UI_MENU_CHEAT_UNSUPPORTED, UI_MENU_CHEAT_UNSUPPORTED_DESC,
    // the stick directions of a cheat's buttons (cheats_buttons; the other buttons are UI_BIND_A .. UI_BIND_DOWN)
    UI_MENU_CHEAT_LSTICK_LEFT, UI_MENU_CHEAT_LSTICK_UP, UI_MENU_CHEAT_LSTICK_RIGHT, UI_MENU_CHEAT_LSTICK_DOWN,
    UI_MENU_CHEAT_RSTICK_LEFT, UI_MENU_CHEAT_RSTICK_UP, UI_MENU_CHEAT_RSTICK_RIGHT, UI_MENU_CHEAT_RSTICK_DOWN,
    UI_MENU_LAST_ID = UI_MENU_CHEAT_RSTICK_DOWN,
    UI_TEXT_COUNT
} UiTextId;

// Bytes of the menu's description pane (ui/option_rows.cpp, struct Desc): a description plus the note appended to it
// ("after restart", "set by environment variable", a disabled row's note). UTF-8 Cyrillic takes two bytes a letter, so
// the longest Russian description is about 850 bytes; the probe checks every menu text leaves room for a note.
enum { UI_DESC_BYTES = 2048 };

// UTF-8 text for id in the language of ov_language(); English when the catalog has no row for that language.
const char *ui_text(UiTextId id);
// The same for an explicit catalog language ("en", "ja", "fr", "de", "it", "es", "nl", "pt", "ru", "zh-Hans",
// "zh-Hant", "ko", "pt-BR"); an empty entry gives the English text.
const char *ui_text_in(const char *lang, UiTextId id);
// Calls visit(ctx, text) for every string the row of lang has its own text for (an id that falls back to English is not
// visited): the font builder covers the glyphs of the text a language shows (ui/menu.cpp).
void ui_text_visit(const char *lang, void (*visit)(void *ctx, const char *text), void *ctx);
// 1 when the row of lang has its own text for id (no fallback to English), for the probe.
int ui_text_defined(const char *lang, UiTextId id);
// The catalog language for a Switch language code ("en-GB" -> "en", "zh-CN" -> "zh-Hans", ...).
const char *ui_text_lang(const char *switch_code);

// Menu text: ui_text_in(menu language, id). The menu language is TOTK_MENU_LANG ("en", "ru", "pt-BR", "zh-Hans");
// unset, the system UI language when the catalog has menu text for it (English, Russian, Brazilian Portuguese,
// Simplified Chinese), else English. Follows a live change of the setting.
const char *menu_text(UiTextId id);
