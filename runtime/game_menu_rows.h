// The port's rows of the game's options screen (game_menu.c includes this once): the settings of the F1 menu's tabs
// (ui/menu.cpp) in their order, a separator between tabs and groups. What a row of the game cannot show stays in the
// F1 menu: the graphics card list, key bindings, amiibo files, the Advanced tab.
//
// The game's rows are a switch (two values) or a selection between arrows that stops at both ends; a setting with a
// range (a slider in the F1 menu) becomes a selection over at most MAX_CHOICES steps of it.
#pragma once

enum { ROW_TOGGLE = 0, ROW_CHOICE = 1, ROW_SEPARATOR = 3 };   // the game's ButtonType values
typedef struct {
    int kind;
    const char *key;        // the cfg setting
    const char *fallback;   // the setting that applies while key is unset (the sticks' dead zones), or NULL
    UiTextId label, desc;
    int n;                            // choices
    const char *vals[MAX_CHOICES];    // "" unsets the key (the automatic choice)
    UiTextId names[MAX_CHOICES];      // catalog names, or 0 with the text in lit
    char lit[MAX_CHOICES][32];
    char val_buf[MAX_CHOICES][16];
} Row;

static Row rows[128];
static int n_rows;

static Row *add(int kind, const char *key, UiTextId label, UiTextId desc) {
    if (n_rows >= (int)(sizeof rows / sizeof *rows)) return &rows[n_rows - 1];   // (never: the table below is fixed)
    Row *r = &rows[n_rows++];
    memset(r, 0, sizeof *r);
    r->kind = kind, r->key = key, r->label = label, r->desc = desc;
    return r;
}
static void separator(void) {
    if (n_rows && rows[n_rows - 1].kind != ROW_SEPARATOR) add(ROW_SEPARATOR, NULL, 0, 0);
    if (!n_rows) add(ROW_SEPARATOR, NULL, 0, 0);
}
static void toggle(const char *key, UiTextId label, UiTextId desc) { add(ROW_TOGGLE, key, label, desc); }

// A choice over the key's cfg_meta values ("a|b|c"), names in their order; a key without a fixed default has the
// automatic choice (unset) first, as opt_choice shows it.
static void meta_choice(const char *key, UiTextId label, UiTextId desc, const UiTextId *names, int n) {
    const CfgMeta *m = cfg_meta(key);
    if (!m || !m->values) return;
    Row *r = add(ROW_CHOICE, key, label, desc);
    int k = 0;
    if (!*m->def && k < n) r->vals[k] = "", r->names[k] = names[k], k++;
    for (const char *p = m->values; *p && k < n && k < MAX_CHOICES; k++) {
        const char *e = strchr(p, '|');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len >= sizeof r->val_buf[k]) len = sizeof r->val_buf[k] - 1;
        memcpy(r->val_buf[k], p, len);
        r->val_buf[k][len] = 0;
        r->vals[k] = r->val_buf[k], r->names[k] = names[k];
        p = e ? e + 1 : p + len;
    }
    r->n = k;
}

// A choice over values the row lists, named by catalog ids (an id of 0: by texts, else the value itself)
static void list_choice(const char *key, UiTextId label, UiTextId desc, int n, const char *const *vals,
                        const UiTextId *ids, const char *const *texts) {
    Row *r = add(ROW_CHOICE, key, label, desc);
    r->n = n > MAX_CHOICES ? MAX_CHOICES : n;
    for (int i = 0; i < r->n; i++) {
        r->vals[i] = vals[i];
        if (ids && ids[i]) r->names[i] = ids[i];
        else snprintf(r->lit[i], sizeof r->lit[i], "%s", texts ? texts[i] : vals[i]);
    }
}

// Numbers: lo..hi in steps, or the n_list values of list; each named by fmt
static void numbers(const char *key, UiTextId label, UiTextId desc, double lo, double hi, double step, const char *fmt,
                    const double *list, int n_list) {
    Row *r = add(ROW_CHOICE, key, label, desc);
    int n = list ? n_list : (int)((hi - lo) / step + 1.5);
    for (int k = 0; k < n && k < MAX_CHOICES; k++, r->n++) {
        double v = list ? list[k] : lo + step * k;
        snprintf(r->val_buf[k], sizeof r->val_buf[k], "%g", v);
        r->vals[k] = r->val_buf[k];
        snprintf(r->lit[k], sizeof r->lit[k], fmt, v);
    }
}
#define NUMS(key, label, desc, fmt, ...)                                                                              \
    do {                                                                                                              \
        static const double l_[] = {__VA_ARGS__};                                                                     \
        numbers(key, label, desc, 0, 0, 0, fmt, l_, (int)(sizeof l_ / sizeof *l_));                                   \
    } while (0)

static void rows_init(void) {
    // Screen
    separator();
    toggle("TOTK_FULLSCREEN", UI_MENU_FULLSCREEN, UI_MENU_FULLSCREEN_DESC);
    {
        static const char *const v[] = {"1280x720", "1600x900", "1920x1080", "2560x1440", "3200x1800", "3840x2160"};
        static const char *const t[] = {"1280 × 720", "1600 × 900", "1920 × 1080", "2560 × 1440", "3200 × 1800",
                                        "3840 × 2160"};
        list_choice("TOTK_WINDOW_SIZE", UI_MENU_WINDOW_SIZE, UI_MENU_WINDOW_SIZE_DESC, 6, v, NULL, t);
    }
    static const UiTextId present[] = {UI_MENU_PRESENT_MAILBOX, UI_MENU_PRESENT_VSYNC, UI_MENU_PRESENT_IMMEDIATE};
    meta_choice("TOTK_VK_PRESENT", UI_MENU_PRESENT, UI_MENU_PRESENT_DESC, present, 3);
    static const UiTextId perf[] = {UI_MENU_PERF_OFF, UI_MENU_PERF_FPS, UI_MENU_PERF_FULL};
    meta_choice("TOTK_PERF_OVERLAY", UI_MENU_PERF_OVERLAY, UI_MENU_PERF_OVERLAY_DESC, perf, 3);
    separator();   // Graphics
    {
        static const char *const v[] = {"1280x720", "1600x900", "1920x1080", "2560x1440", "3200x1800", "3840x2160",
                                        "2560x1080", "3440x1440", "3840x1600", "5120x2160", "1280x800", "1680x1050",
                                        "1920x1200", "2560x1600"};
        enum { N = sizeof v / sizeof *v };
        static char t[N][32];
        static const char *tp[N];
        for (int i = 0; i < N; i++) {
            int w = 0, h = 0;
            sscanf(v[i], "%dx%d", &w, &h);
            snprintf(t[i], sizeof t[i], "%d × %d", w, h);   // U+00D7, the multiplication sign
            tp[i] = t[i];
        }
        list_choice("TOTK_OUTPUT_RES", UI_MENU_OUTPUT_RES, UI_MENU_OUTPUT_RES_DESC, N, v, NULL, tp);
    }
    {
        static const char *const v[] = {"game", "50", "67", "75", "100", "125", "150", "200"};
        static const UiTextId id[] = {UI_MENU_RENDER_SCALE_GAME, 0, 0, 0, 0, 0, 0, 0};
        static const char *const t[] = {"", "50 %", "67 %", "75 %", "100 %", "125 %", "150 %", "200 %"};
        list_choice("TOTK_RENDER_SCALE", UI_MENU_RENDER_SCALE, UI_MENU_RENDER_SCALE_DESC, 8, v, id, t);
    }
    {
        static const char *const v[] = {"game", "256", "512", "768", "1024", "1536", "2048"};
        static const UiTextId id[] = {UI_MENU_SHADOW_RES_GAME, 0, 0, 0, 0, 0, 0};
        list_choice("TOTK_SHADOW_RES", UI_MENU_SHADOW_RES, UI_MENU_SHADOW_RES_DESC, 7, v, id, NULL);
    }
    static const UiTextId up[] = {UI_MENU_UPSCALER_FSR, UI_MENU_UPSCALER_BILINEAR};
    meta_choice("TOTK_UPSCALER", UI_MENU_UPSCALER, UI_MENU_UPSCALER_DESC, up, 2);
    numbers("TOTK_SHARPNESS", UI_MENU_SHARPNESS, UI_MENU_SHARPNESS_DESC, 0, 100, 10, "%.0f %%", NULL, 0);
    static const UiTextId aspect[] = {UI_MENU_ASPECT_FIT, UI_MENU_ASPECT_STRETCH};
    meta_choice("TOTK_ASPECT", UI_MENU_ASPECT, UI_MENU_ASPECT_DESC, aspect, 2);
    static const UiTextId movie_aspect[] = {UI_MENU_MOVIE_ASPECT_KEEP, UI_MENU_MOVIE_ASPECT_STRETCH};
    meta_choice("TOTK_MOVIE_ASPECT", UI_MENU_MOVIE_ASPECT, UI_MENU_MOVIE_ASPECT_DESC, movie_aspect, 2);
    numbers("TOTK_FOV", UI_MENU_FOV, UI_MENU_FOV_DESC, 70, 140, 5, "%.0f %%", NULL, 0);
    {
        static const char *const v[] = {"game", "2", "4", "8", "16"};
        static const UiTextId id[] = {UI_MENU_ANISO_GAME, 0, 0, 0, 0};
        static const char *const t[] = {"", "2x", "4x", "8x", "16x"};
        list_choice("TOTK_ANISO", UI_MENU_ANISO, UI_MENU_ANISO_DESC, 5, v, id, t);
    }
    {
        static const char *const v[] = {"1", "2", "3", "4"};
        static const char *const t[] = {"960 (1x)", "1920 (2x)", "2880 (3x)", "3840 (4x)"};
        list_choice("TOTK_SHADOW_SCALE", UI_MENU_SHADOW_SCALE, UI_MENU_SHADOW_SCALE_DESC, 4, v, NULL, t);
    }
    {
        static const char *const v[] = {"100", "150", "200", "300"};
        static const char *const t[] = {"160 m (1x)", "240 m (1.5x)", "320 m (2x)", "480 m (3x)"};
        list_choice("TOTK_SHADOW_RANGE", UI_MENU_SHADOW_RANGE, UI_MENU_SHADOW_RANGE_DESC, 4, v, NULL, t);
    }
    numbers("TOTK_PCSS", UI_MENU_PCSS, UI_MENU_PCSS_DESC, 0, 200, 20, "%.0f %%", NULL, 0);
    numbers("TOTK_CONTACT_SHADOWS", UI_MENU_CONTACT_SHADOWS, UI_MENU_CONTACT_SHADOWS_DESC, 0, 100, 10, "%.0f %%", NULL,
            0);
    toggle("TOTK_DLAA", UI_MENU_DLAA, UI_MENU_DLAA_DESC);
    static const UiTextId dlss[] = {UI_MENU_DLSS_DLAA, UI_MENU_DLSS_QUALITY, UI_MENU_DLSS_BALANCED,
                                    UI_MENU_DLSS_PERFORMANCE, UI_MENU_DLSS_ULTRA};
    meta_choice("TOTK_DLSS", UI_MENU_DLSS, UI_MENU_DLSS_DESC, dlss, 5);
    static const UiTextId preset[] = {UI_MENU_DLSS_PRESET_DEFAULT, UI_MENU_DLSS_PRESET_K, UI_MENU_DLSS_PRESET_J};
    meta_choice("TOTK_DLSS_PRESET", UI_MENU_DLSS_PRESET, UI_MENU_DLSS_PRESET_DESC, preset, 3);
    static const UiTextId chars[] = {UI_MENU_DLSS_CHARS_OFF, UI_MENU_DLSS_CHARS_LIGHT, UI_MENU_DLSS_CHARS_MEDIUM,
                                     UI_MENU_DLSS_CHARS_FULL};
    meta_choice("TOTK_DLSS_CHAR_MASK", UI_MENU_DLSS_CHARS, UI_MENU_DLSS_CHARS_DESC, chars, 4);
    toggle("TOTK_FXAA", UI_MENU_FXAA, UI_MENU_FXAA_DESC);
    separator();   // Colour
    static const UiTextId color[] = {UI_MENU_COLOR_DEFAULT, UI_MENU_COLOR_VIVID, UI_MENU_COLOR_CLARITY,
                                     UI_MENU_COLOR_CUSTOM};
    meta_choice("TOTK_COLOR_PRESET", UI_MENU_COLOR_PRESET, UI_MENU_COLOR_PRESET_DESC, color, 4);
    numbers("TOTK_COLOR_BRIGHTNESS", UI_MENU_COLOR_BRIGHTNESS, UI_MENU_COLOR_BRIGHTNESS_DESC, -30, 30, 5, "%+.0f %%",
            NULL, 0);
    numbers("TOTK_COLOR_CONTRAST", UI_MENU_COLOR_CONTRAST, UI_MENU_COLOR_CONTRAST_DESC, 70, 150, 5, "%.0f %%", NULL, 0);
    numbers("TOTK_COLOR_SATURATION", UI_MENU_COLOR_SATURATION, UI_MENU_COLOR_SATURATION_DESC, 50, 200, 10, "%.0f %%",
            NULL, 0);
    numbers("TOTK_COLOR_GAMMA", UI_MENU_COLOR_GAMMA, UI_MENU_COLOR_GAMMA_DESC, 0.8, 1.2, 0.05, "%.2f", NULL, 0);
    separator();   // HDR
    toggle("TOTK_HDR", UI_MENU_HDR, UI_MENU_HDR_DESC);
    NUMS("TOTK_HDR_PEAK_NITS", UI_MENU_HDR_PEAK, UI_MENU_HDR_PEAK_DESC, "%.0f", 400, 600, 800, 1000, 1500, 2000, 3000,
         4000, 10000);
    NUMS("TOTK_HDR_GAME_NITS", UI_MENU_HDR_GAME, UI_MENU_HDR_GAME_DESC, "%.0f", 80, 100, 150, 200, 250, 300, 400, 500);
    NUMS("TOTK_HDR_UI_NITS", UI_MENU_HDR_UI, UI_MENU_HDR_UI_DESC, "%.0f", 80, 100, 150, 200, 250, 300, 400, 500);
    numbers("TOTK_HDR_HUE", UI_MENU_HDR_HUE, UI_MENU_HDR_HUE_DESC, 0, 100, 10, "%.0f %%", NULL, 0);

    // Game
    separator();
    {
        static const char *const v[] = {"30", "40", "45", "60", "90", "120", "144", "display"};
        static const UiTextId id[] = {UI_MENU_FPS_30, UI_MENU_FPS_40,  UI_MENU_FPS_45,  UI_MENU_FPS_60,
                                      UI_MENU_FPS_90, UI_MENU_FPS_120, UI_MENU_FPS_144, UI_MENU_FPS_DISPLAY};
        list_choice("TOTK_FPS_LIMIT", UI_MENU_FPS_LIMIT, UI_MENU_FPS_LIMIT_DESC, 8, v, id, NULL);
    }
    toggle("TOTK_FRAME_QUEUE", UI_MENU_FRAME_QUEUE, UI_MENU_FRAME_QUEUE_DESC);
    {
        UiGameLang langs[16];
        int n = ui_text_game_langs(rt_game_version, langs, 16);
        static const char *v[17];
        static UiTextId id[17];
        v[0] = "", id[0] = UI_MENU_GAME_LANG_AUTO;
        for (int i = 0; i < n; i++) v[i + 1] = langs[i].code, id[i + 1] = langs[i].name;
        list_choice("TOTK_LANG", UI_MENU_GAME_LANG, UI_MENU_GAME_LANG_DESC, n + 1, v, id, NULL);
    }
    static const UiTextId mode[] = {UI_MENU_MODE_DOCKED, UI_MENU_MODE_HANDHELD};
    meta_choice("TOTK_OPERATION_MODE", UI_MENU_OPERATION_MODE, UI_MENU_OPERATION_MODE_DESC, mode, 2);
    toggle("TOTK_ASSUME_FOCUS", UI_MENU_ASSUME_FOCUS, UI_MENU_ASSUME_FOCUS_DESC);

    // Sound
    separator();
    numbers("TOTK_VOLUME", UI_MENU_VOLUME, UI_MENU_VOLUME_DESC, 0, 100, 10, "%.0f %%", NULL, 0);
    static const UiTextId channels[] = {UI_MENU_CHANNELS_AUTO, UI_MENU_CHANNELS_STEREO, UI_MENU_CHANNELS_51};
    meta_choice("TOTK_AUDIO_CHANNELS", UI_MENU_AUDIO_CHANNELS, UI_MENU_AUDIO_CHANNELS_DESC, channels, 3);

    // Controls
    separator();
    static const UiTextId layout[] = {UI_MENU_PAD_LAYOUT_AUTO, UI_MENU_PAD_LAYOUT_NINTENDO, UI_MENU_PAD_LAYOUT_XBOX};
    meta_choice("TOTK_PAD_LAYOUT", UI_MENU_PAD_LAYOUT, UI_MENU_PAD_LAYOUT_DESC, layout, 3);
    static const UiTextId prompts[] = {UI_MENU_PROMPTS_NINTENDO, UI_MENU_PROMPTS_XBOX, UI_MENU_PROMPTS_PC};
    meta_choice("TOTK_PROMPTS", UI_MENU_PROMPTS, UI_MENU_PROMPTS_DESC, prompts, 3);
    toggle("TOTK_MOTION", UI_MENU_MOTION, UI_MENU_MOTION_DESC);
    numbers("TOTK_RUMBLE_STRENGTH", UI_MENU_RUMBLE_STRENGTH, UI_MENU_RUMBLE_STRENGTH_DESC, 0, 300, 20, "%.0f %%", NULL,
            0);
    numbers("TOTK_RUMBLE_CURVE", UI_MENU_RUMBLE_CURVE, UI_MENU_RUMBLE_CURVE_DESC, 0.2, 1.5, 0.1, "%.2f", NULL, 0);
    toggle("TOTK_RUMBLE_TRIGGERS", UI_MENU_RUMBLE_TRIGGERS, UI_MENU_RUMBLE_TRIGGERS_DESC);
    static const UiTextId mouse_modes[] = {UI_MENU_MOUSE_STICK, UI_MENU_MOUSE_DIRECT};
    meta_choice("TOTK_MOUSE_MODE", UI_MENU_MOUSE_MODE, UI_MENU_MOUSE_MODE_DESC, mouse_modes, 2);
    NUMS("TOTK_MOUSE_SENS", UI_MENU_MOUSE_SENS, UI_MENU_MOUSE_SENS_DESC, "%.2g", 0.25, 0.5, 0.75, 1, 1.25, 1.5, 2, 2.5,
         3, 4, 5, 7, 10);
    toggle("TOTK_MENU_CURSOR", UI_MENU_MENU_CURSOR, UI_MENU_MENU_CURSOR_DESC);
    {
        static const char *const v[] = {"none", "lshift", "lalt", "lctrl", "tab", "capslock"};
        static const UiTextId id[] = {UI_MENU_OFF, 0, 0, 0, 0, 0};
        static const char *const t[] = {"", "Shift", "Alt", "Ctrl", "Tab", "Caps Lock"};
        list_choice("TOTK_WALK_KEY", UI_MENU_WALK_KEY, UI_MENU_WALK_KEY_DESC, 6, v, id, t);
    }
    numbers("TOTK_WALK_SCALE", UI_MENU_WALK_SCALE, UI_MENU_WALK_SCALE_DESC, 0.2, 0.9, 0.05, "%.2f", NULL, 0);
    separator();   // Analog sticks
    numbers("TOTK_LSTICK_DEADZONE", UI_MENU_LSTICK_DEADZONE, UI_MENU_STICK_DEADZONE_DESC, 0, 30, 2, "%.0f %%", NULL, 0);
    rows[n_rows - 1].fallback = "TOTK_STICK_DEADZONE";
    numbers("TOTK_RSTICK_DEADZONE", UI_MENU_RSTICK_DEADZONE, UI_MENU_STICK_DEADZONE_DESC, 0, 30, 2, "%.0f %%", NULL, 0);
    rows[n_rows - 1].fallback = "TOTK_STICK_DEADZONE";
    NUMS("TOTK_STICK_SENS", UI_MENU_STICK_SENS, UI_MENU_STICK_SENS_DESC, "%.2f", 0.25, 0.5, 0.75, 1, 1.25, 1.5, 1.75, 2,
         2.5, 3);

    // Menu
    separator();
    static const UiTextId langs[] = {UI_MENU_MENU_LANG_AUTO, UI_MENU_LANGNAME_EN, UI_MENU_LANGNAME_RU,
                                     UI_MENU_LANGNAME_PT_BR, UI_MENU_LANGNAME_ZH_HANS};
    meta_choice("TOTK_MENU_LANG", UI_MENU_MENU_LANG, UI_MENU_MENU_LANG_DESC, langs, 5);
    toggle("TOTK_DISCORD_RPC", UI_MENU_DISCORD_RPC, UI_MENU_DISCORD_RPC_DESC);
}

static Row *row_of_key(uint32_t key) {
    if (key < KEY_BASE || key >= KEY_BASE + (uint32_t)n_rows) return NULL;
    return &rows[key - KEY_BASE];
}
static Row *row_of_type(int type) {
    return type >= TYPE_BASE && type < TYPE_BASE + n_rows ? &rows[type - TYPE_BASE] : NULL;
}

static const char *value_of(const Row *r) {
    const char *v = cfg_get(r->key);
    if ((!v || !*v) && r->fallback) return cfg_value(r->fallback);
    return cfg_value(r->key);
}

static int choice_now(const Row *r) {
    const char *v = value_of(r);
    for (int i = 0; i < r->n; i++)
        if (!strcmp(v, r->vals[i])) return i;
    char *end;
    double d = strtod(v, &end);   // a number between the steps: the nearest
    if (end == v) return 0;
    int best = 0;
    double bd = 1e300;
    for (int i = 0; i < r->n; i++) {
        double e = strtod(r->vals[i], NULL) - d;
        if (e < 0) e = -e;
        if (e < bd) bd = e, best = i;
    }
    return best;
}

// As the F1 menu writes a setting (ui/option_rows.cpp apply, ui/menu.cpp rows_color): "" unsets the key; a colour
// preset sets the four colour values, and a colour value makes the preset Custom.
static void apply(const Row *r, const char *value) {
    if (*value) {
        cfg_set(r->key, value);
    } else {
        const char *k = r->key;
        cfg_reset(&k, 1);
    }
    if (!strcmp(r->key, "TOTK_COLOR_PRESET")) {
        static const struct { const char *name, *b, *c, *s, *g; } presets[] = {
            {"default", "0", "100", "100", "1"}, {"vivid", "0", "115", "135", "0.95"}, {"clarity", "2", "110", "115", "1"}};
        for (size_t i = 0; i < sizeof presets / sizeof *presets; i++)
            if (!strcmp(value, presets[i].name)) {
                cfg_set("TOTK_COLOR_BRIGHTNESS", presets[i].b), cfg_set("TOTK_COLOR_CONTRAST", presets[i].c);
                cfg_set("TOTK_COLOR_SATURATION", presets[i].s), cfg_set("TOTK_COLOR_GAMMA", presets[i].g);
            }
    } else if (!strncmp(r->key, "TOTK_COLOR_", 11) && strcmp(cfg_value("TOTK_COLOR_PRESET"), "custom")) {
        cfg_set("TOTK_COLOR_PRESET", "custom");
    }
    if (!cfg_save()) rt_log("game menu: could not write totk.ini; %s=%s holds for this session", r->key, value);
}

// The game's description box does not wrap: its texts carry their line breaks. WRAP_COLS characters fill a line of
// its 640-pixel text pane in the game's caption font; WRAP_LINES lines fill the box.
#define WRAP_COLS 46
#define WRAP_LINES 5

// Appends text to out (cap bytes) wrapped at WRAP_COLS, at most max_lines lines (the last one ending in "..." when the
// text goes on); returns the lines written.
static int wrap(char *out, size_t cap, const char *text, int max_lines) {
    size_t o = strlen(out);
    int lines = 0;
    const char *p = text;
    while (*p && lines < max_lines) {
        while (*p == ' ') p++;
        // the longest run of whole words that fits (UTF-8 continuation bytes take no column)
        const char *end = p, *brk = NULL;
        int cols = 0;
        while (*end && *end != '\n' && cols <= WRAP_COLS) {
            if (*end == ' ') brk = end;
            if (((uint8_t)*end & 0xc0) != 0x80) cols++;
            end++;
        }
        if (*end && *end != '\n' && cols > WRAP_COLS && brk) end = brk;
        int more = *end && lines + 1 == max_lines;
        size_t len = (size_t)(end - p);
        if (o + len + 5 >= cap) break;
        if (lines) out[o++] = '\n';
        memcpy(out + o, p, len), o += len;
        if (more) memcpy(out + o, "...", 3), o += 3;
        out[o] = 0;
        lines++;
        p = *end == '\n' ? end + 1 : end;
    }
    return lines;
}

// The description: the catalog's, then a note for a setting an environment variable fixes or one read at start
static const char *desc_of(const Row *r, char *buf, size_t n) {
    const CfgMeta *m = cfg_meta(r->key);
    const char *note = cfg_from_env(r->key) ? menu_text(UI_MENU_SET_BY_ENV)
                       : m && !m->live      ? menu_text(UI_MENU_AFTER_RESTART)
                                            : NULL;
    buf[0] = 0;
    char note_buf[256] = "";
    int note_lines = note ? wrap(note_buf, sizeof note_buf, note, 2) : 0;
    wrap(buf, n, menu_text(r->desc), WRAP_LINES - note_lines);
    if (note_lines && strlen(buf) + strlen(note_buf) + 2 < n) strcat(strcat(buf, "\n"), note_buf);
    return buf;
}
