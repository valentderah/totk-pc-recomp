// totk-config: the game folder, its totk.ini, the profiles and presets, starting the game (cfgr.h).
//
// totk.ini goes through cfg.c as the game reads and writes it: cfg_load parses it, model_set / model_reset are cfg_set
// / cfg_reset, and model_save is cfg_save_to, which rewrites only the lines of changed keys. The window's marks need
// two views cfg.c keeps internal: the key's own value (file or unsaved change, without the environment) and the value
// the file held when it was read; both are copied out of cfg_each_active.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "configurator/cfgr.h"
#include "platform/platform.h"

#ifdef _WIN32
const char *const cr_game_exe = "totk.exe";
#else
const char *const cr_game_exe = "totk";
#endif

typedef struct { char *key, *value; } Pair;
typedef struct { Pair *p; int n, cap; } Pairs;

static char g_dir[CR_PATH], g_ini[CR_PATH];
static Pairs g_file;              // what totk.ini held when last read or written
static Pairs g_own;               // own values now; rebuilt when cfg_version moves
static unsigned g_own_version = ~0u;
static int g_own_stale = 1;
static uint8_t *g_bytes;          // totk.ini's bytes when last read or written (model_file_changed)
static uint64_t g_bytes_n;
static int g_bytes_had;

static void pairs_clear(Pairs *s) {
    for (int i = 0; i < s->n; i++) free(s->p[i].key), free(s->p[i].value);
    s->n = 0;
}

static void pairs_add(Pairs *s, const char *k, const char *v) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->p = realloc(s->p, sizeof *s->p * (size_t)s->cap);
        if (!s->p) abort();
    }
    s->p[s->n++] = (Pair){strdup(k), strdup(v)};
}

static const char *pairs_get(const Pairs *s, const char *k) {
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->p[i].key, k)) return s->p[i].value;
    return NULL;
}

static void collect(const char *key, const char *value, int from_env, void *ctx) {
    if (!from_env && value) pairs_add(ctx, key, value);
}

static void own_refresh(void) {
    unsigned v = cfg_version();
    if (!g_own_stale && v == g_own_version) return;
    pairs_clear(&g_own);
    cfg_each_active(collect, &g_own);
    g_own_version = v, g_own_stale = 0;
}

static void remember_file(void) {
    pairs_clear(&g_file);
    cfg_each_active(collect, &g_file);
    g_own_stale = 1;
    free(g_bytes);
    g_bytes = plat_file_read(g_ini, &g_bytes_n);
    g_bytes_had = g_bytes != NULL;
}

static void join(char *out, size_t cap, const char *dir, const char *name) {
    size_t n = strlen(dir);
    snprintf(out, cap, "%s%s%s", dir, n && (dir[n - 1] == '/' || dir[n - 1] == '\\') ? "" : "/", name);
}

void model_open(const char *dir) {
    if (dir && *dir) snprintf(g_dir, sizeof g_dir, "%s", dir);
    else {
        char exe[CR_PATH];
        if (!plat_exe_path(exe, sizeof exe)) snprintf(exe, sizeof exe, "./x");
        char *slash = strrchr(exe, '/'), *back = strrchr(exe, '\\');
        if (back > slash) slash = back;
        if (slash) *slash = 0;
        else snprintf(exe, sizeof exe, ".");
        snprintf(g_dir, sizeof g_dir, "%s", exe);
    }
    size_t n = strlen(g_dir);
    while (n > 1 && (g_dir[n - 1] == '/' || g_dir[n - 1] == '\\') && !(n == 3 && g_dir[1] == ':')) g_dir[--n] = 0;
    join(g_ini, sizeof g_ini, g_dir, "totk.ini");
    model_reload();
}

const char *model_dir(void) { return g_dir; }
const char *model_ini_path(void) { return g_ini; }

int model_has_game(void) {
    char p[CR_PATH];
    join(p, sizeof p, g_dir, cr_game_exe);
    return plat_file_exists(p);
}

int model_reload(void) {
    int ok = cfg_load(g_ini);
    remember_file();
    return ok;
}

int model_save(void) {
    if (!plat_dir_exists(g_dir)) plat_file_mkdir_p(g_dir);
    if (!cfg_save_to(g_ini)) return 0;
    // cfg.c keeps the keys it wrote marked as written; what the file holds now is what the window shows
    cfg_load(g_ini);
    remember_file();
    return 1;
}

int model_file_changed(void) {
    uint64_t n = 0;
    uint8_t *now = plat_file_read(g_ini, &n);
    int changed = (now != NULL) != g_bytes_had || (now && (n != g_bytes_n || memcmp(now, g_bytes, n)));
    free(now);
    return changed;
}

const char *model_own(const char *key) {
    own_refresh();
    return pairs_get(&g_own, key);
}

int model_changed(const char *key) {
    const char *a = model_own(key), *b = pairs_get(&g_file, key);
    return (a == NULL) != (b == NULL) || (a && strcmp(a, b));
}

int model_unsaved(void) {
    own_refresh();
    int n = 0;
    for (int i = 0; i < g_own.n; i++) n += model_changed(g_own.p[i].key);
    for (int i = 0; i < g_file.n; i++) n += !pairs_get(&g_own, g_file.p[i].key);
    return n;
}

static void env_name(char *out, size_t cap, const char *key) { snprintf(out, cap, "TOTK_%s", key); }

const char *model_default(const CrRow *r) {
    const CfgMeta *m = cfg_meta(r->key);
    if (m) return m->def;
    return r->def ? r->def : "";
}

const char *model_value(const CrRow *r) {
    char name[160];
    env_name(name, sizeof name, r->key);
    if (cfg_from_env(name)) {
        const char *e = cfg_get(name);
        return e ? e : "";
    }
    const char *own = model_own(r->key);
    if (own && *own) return own;
    if (r->fallback) {   // a slider over another key while unset (ui/option_rows.cpp opt_slider fallback)
        const CrRow fb = {.key = r->fallback};
        const char *v = model_own(r->fallback);
        return v && *v ? v : model_default(&fb);
    }
    return model_default(r);
}

void model_set(const char *key, const char *value) {
    if (!value || !*value) {   // empty: the key unset, as the menu's "Automatic" choices do
        model_reset(key);
        return;
    }
    char name[160];
    env_name(name, sizeof name, key);
    cfg_set(name, value);
}

void model_reset(const char *key) {
    const char *keys[1] = {key};
    cfg_reset(keys, 1);
    g_own_stale = 1;
}

int model_other_keys(const char **keys, const char **values, int max) {
    own_refresh();
    int n = 0;
    for (int i = 0; i < g_own.n && n < max; i++)
        if (!cfg_known(g_own.p[i].key)) keys[n] = g_own.p[i].key, values[n] = g_own.p[i].value, n++;
    return n;
}

// ---- profiles
static void profiles_dir(char *out, size_t cap) { join(out, cap, g_dir, "profiles"); }

static void profile_path(char *out, size_t cap, const char *name) {
    char d[CR_PATH], f[CR_NAME + 8];
    profiles_dir(d, sizeof d);
    snprintf(f, sizeof f, "%s.ini", name);
    join(out, cap, d, f);
}

typedef struct { char (*names)[CR_NAME]; int n, max; } List;

static int list_one(const char *name, void *ctx) {
    List *l = ctx;
    size_t n = strlen(name);
    if (l->n < l->max && n > 4 && n - 4 < CR_NAME) {
        memcpy(l->names[l->n], name, n - 4);
        l->names[l->n++][n - 4] = 0;
    }
    return 1;
}

static int by_name(const void *a, const void *b) { return strcmp(a, b); }

int model_profiles(char names[][CR_NAME], int max) {
    char d[CR_PATH];
    profiles_dir(d, sizeof d);
    List l = {names, 0, max};
    plat_file_list(d, ".ini", list_one, &l);
    qsort(names, (size_t)l.n, CR_NAME, by_name);
    return l.n;
}

const char *model_active_profile(void) {
    static char name[CR_NAME];
    char d[CR_PATH], p[CR_PATH], f[CR_PATH];
    profiles_dir(d, sizeof d);
    join(p, sizeof p, d, "active.txt");
    uint64_t n = 0;
    uint8_t *data = plat_file_read(p, &n);
    name[0] = 0;
    if (data) {
        snprintf(name, sizeof name, "%.*s", (int)(n < CR_NAME ? n : CR_NAME - 1), (const char *)data);
        name[strcspn(name, "\r\n")] = 0;
        free(data);
        profile_path(f, sizeof f, name);
        if (!plat_file_exists(f)) name[0] = 0;   // a deleted profile: totk.ini is edited directly
    }
    return name;
}

static int set_active(const char *name) {
    char d[CR_PATH], p[CR_PATH];
    profiles_dir(d, sizeof d);
    plat_file_mkdir_p(d);
    join(p, sizeof p, d, "active.txt");
    char line[CR_NAME + 2];
    int n = snprintf(line, sizeof line, "%s\n", name);
    return plat_file_write_atomic(p, line, (size_t)n);
}

int model_valid_name(const char *name) {
    size_t n = strlen(name);
    if (!n || n >= CR_NAME - 1 || name[0] == '.' || name[0] == ' ' || name[n - 1] == ' ' || name[n - 1] == '.') return 0;
    for (const char *c = name; *c; c++)
        if (strchr("<>:\"/\\|?*", *c) || (unsigned char)*c < 32) return 0;
    char p[CR_PATH];
    profile_path(p, sizeof p, name);
    return !plat_file_exists(p) && strcasecmp(name, "active");
}

static int copy_file(const char *from, const char *to) {
    uint64_t n = 0;
    uint8_t *data = plat_file_read(from, &n);
    int ok = plat_file_write_atomic(to, data ? data : (const uint8_t *)"", data ? (size_t)n : 0);
    free(data);
    return ok;
}

int model_switch_profile(const char *name) {
    if (!model_save()) return 0;
    char p[CR_PATH];
    const char *active = model_active_profile();
    if (*active) {
        profile_path(p, sizeof p, active);
        if (!copy_file(g_ini, p)) return 0;
    }
    if (name && *name) {
        profile_path(p, sizeof p, name);
        if (!plat_file_exists(p) || !copy_file(p, g_ini)) return 0;
    }
    if (!set_active(name ? name : "")) return 0;
    model_reload();
    return 1;
}

static void apply_preset(Preset from) {
    if (from == PRESET_DEFAULTS) {
        const char *keys[1];
        own_refresh();
        Pairs copy = {0};
        for (int i = 0; i < g_own.n; i++) pairs_add(&copy, g_own.p[i].key, g_own.p[i].value);
        for (int i = 0; i < copy.n; i++) keys[0] = copy.p[i].key, cfg_reset(keys, 1);
        pairs_clear(&copy);
        free(copy.p);
        return;
    }
    int n = 0;
    const PresetChange *c = preset_changes(from, &n);
    for (int i = 0; i < n; i++) model_set(c[i].key, c[i].value);
    if (from == PRESET_DECK) {
        const char *k, *v;
        for (int i = 0; cfg_deck_at(i, &k, &v); i++) model_set(k, v);
    }
}

// The new profile is totk.ini's current settings with the preset's changes on top, written through cfg.c so its file
// is a totk.ini like any other. totk.ini itself keeps what it had unless the new profile is switched to.
int model_new_profile(const char *name, Preset from, int activate) {
    if (!model_valid_name(name) || !model_save()) return 0;
    char p[CR_PATH], d[CR_PATH];
    profiles_dir(d, sizeof d);
    plat_file_mkdir_p(d);
    profile_path(p, sizeof p, name);
    if (!copy_file(g_ini, p)) return 0;
    if (from != PRESET_CURRENT) {
        cfg_load(p);
        apply_preset(from);
        int ok = cfg_save_to(p);
        model_reload();
        if (!ok) return 0;
    }
    if (!*model_active_profile() && !activate) set_active("");
    return activate ? model_switch_profile(name) : 1;
}

int model_rename_profile(const char *from, const char *to) {
    if (!model_valid_name(to)) return 0;
    char a[CR_PATH], b[CR_PATH];
    profile_path(a, sizeof a, from);
    profile_path(b, sizeof b, to);
    int active = !strcmp(model_active_profile(), from);
    if (!plat_file_replace(a, b)) return 0;
    return active ? set_active(to) : 1;
}

int model_delete_profile(const char *name) {
    char p[CR_PATH];
    profile_path(p, sizeof p, name);
    int active = !strcmp(model_active_profile(), name);
    plat_file_delete(p);
    if (active) set_active("");
    return !plat_file_exists(p);
}

// ---- presets: a handful of keys over the current settings; every value is one runtime/cfg.c metas[] accepts
static const PresetChange QUALITY[] = {
    {"RENDER_SCALE", "100"}, {"DYNAMIC_RES", "0"}, {"ANISO", "16"}, {"SHADOW_RES", "2048"}, {"SHADOW_SCALE", "2"},
    {"SHADOW_RANGE", "150"}, {"GRASS_RANGE", "150"}, {"CONTACT_SHADOWS", "50"},
};
static const PresetChange PERFORMANCE[] = {
    {"RENDER_SCALE", "75"}, {"DYNAMIC_RES", "1"}, {"UPSCALER", "fsr"}, {"ANISO", NULL}, {"SHADOW_RES", NULL},
    {"SHADOW_SCALE", NULL}, {"GRASS_RANGE", "100"}, {"CONTACT_SHADOWS", "0"}, {"PCSS", "0"}, {"FRAME_QUEUE", "1"},
};
// A GPU with about 2 GB of video memory: less to draw and less to hold. The video memory keys docs/settings.md marks
// Caution stay at their defaults.
static const PresetChange POTATO[] = {
    {"RENDER_SCALE", "67"}, {"DYNAMIC_RES", NULL}, {"FPS_LIMIT", "30"}, {"GRASS_RANGE", "50"}, {"SHADOW_RES", NULL},
    {"SHADOW_SCALE", NULL}, {"SHADOW_RANGE", NULL}, {"CONTACT_SHADOWS", NULL}, {"PCSS", NULL}, {"ANISO", NULL},
    {"FXAA", NULL}, {"SCENE_UPSCALER", NULL}, {"LOD_STEADY", NULL}, {"SYNC_AHEAD", NULL}, {"REPLAY_WORKERS", NULL},
};

const PresetChange *preset_changes(Preset p, int *n) {
#define LIST(a) (*n = (int)(sizeof a / sizeof *a), a)
    switch (p) {
    case PRESET_QUALITY: return LIST(QUALITY);
    case PRESET_PERFORMANCE: return LIST(PERFORMANCE);
    case PRESET_POTATO: return LIST(POTATO);
    default: *n = 0; return NULL;
    }
#undef LIST
}

UiTextId preset_label(Preset p) {
    static const UiTextId ids[] = {UI_CFG_PRESET_CURRENT, UI_CFG_PRESET_DEFAULTS, UI_CFG_PRESET_QUALITY,
                                   UI_CFG_PRESET_PERFORMANCE, UI_CFG_PRESET_POTATO, UI_CFG_PRESET_DECK};
    return ids[p];
}

UiTextId preset_desc(Preset p) {
    static const UiTextId ids[] = {UI_CFG_PRESET_CURRENT_DESC, UI_CFG_PRESET_DEFAULTS_DESC, UI_CFG_PRESET_QUALITY_DESC,
                                   UI_CFG_PRESET_PERFORMANCE_DESC, UI_CFG_PRESET_POTATO_DESC, UI_CFG_PRESET_DECK_DESC};
    return ids[p];
}

int model_launch(void) {
    char exe[CR_PATH];
    join(exe, sizeof exe, g_dir, cr_game_exe);
    const char *argv[] = {exe, NULL};
    return plat_file_exists(exe) && plat_spawn(argv, g_dir);
}

const char *cr_text(int id) { return id < 0 ? "" : menu_text((UiTextId)id); }
