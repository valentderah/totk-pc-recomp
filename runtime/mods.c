// Player mods: the scan of the mods folder, its order and switches (mods.h).
//
// One subfolder per mod, as emulators take them. The RomFS root is the first of <mod>/romfs, <mod>/romfslite (TKMM's
// name for it when it bypasses Atmosphere's RomFS build), <mod>/atmosphere/contents/<title>/romfs,
// <mod>/contents/<title>/romfs and <mod>/<title>/romfs that exists; the ExeFS and cheats folders likewise. A mod is
// listed whatever it holds; what the port cannot use is its problem, shown in the menu and logged, and never stops the
// start. romfs.c serves the RomFS files of the enabled mods, the top one winning a path.
//
// The order and the switches are totk.ini keys (cfg.c): TOTK_MODS_ORDER lists names top first, separated by '|' (a
// Windows file name cannot hold '|'; a folder whose name does on Linux is listed as unusable); a folder it does not name
// goes below the named ones, by name. TOTK_MODS_DISABLED lists the switched-off names. The menu changes the list in
// place and writes both keys; the scan's state is kept to tell the menu a restart is due.
//
// ExeFS patch files: an .ips or .pchtxt file in the exefs folder or in atmosphere/exefs_patches/<name>/ is for the game
// when its build id is main's (an IPS file's name, a pchtxt file's @nsobid line); others are another version's and
// counted. The setup builds every mod's files into patch sets and writes build-cache/exefs.status next to totk.exe
// (tools/exefs_patches.py has the format): a file whose line is missing or has another content hash waits for the
// rebuild at start (cheats.c), and its mod stays off until then.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cfg.h"
#include "host.h"
#include "mods.h"
#include "platform/platform.h"

#define TITLE "0100F2C0115B6000"
#define MAX_MODS 512

static ModInfo *mods;
static int nmods;
static char dir[2048];
static char start_order[MAX_MODS][256];   // names in the scan's order and their switches, for mods_changed
static int start_enabled[MAX_MODS];

// 1 when a file lies anywhere under path.
static int has_file_cb(const char *name, int kind, uint64_t size, void *ctx);
static int has_file(const char *path) {
    struct { const char *path; int found; } c = {path, 0};
    plat_fs_list(path, has_file_cb, &c);
    return c.found;
}
static int has_file_cb(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    struct { const char *path; int found; } *c = ctx;
    if (kind == PLAT_FS_FILE) {
        c->found = 1;
    } else if (kind == PLAT_FS_DIR) {
        char sub[2100];
        if (snprintf(sub, sizeof sub, "%s/%s", c->path, name) < (int)sizeof sub) c->found = has_file(sub);
    }
    return !c->found;
}

// The first of the emulator and Atmosphere places of <mod>/<part> that is a folder, into out ("" none).
static void find_root(const char *mod, const char *part, char *out, size_t cap) {
    static const char *const forms[] = {"%s/%s", "%s/atmosphere/contents/" TITLE "/%s", "%s/contents/" TITLE "/%s",
                                        "%s/" TITLE "/%s"};
    for (size_t i = 0; i < sizeof forms / sizeof *forms; i++) {
        if (snprintf(out, cap, forms[i], mod, part) < (int)cap && plat_dir_exists(out)) return;
    }
    out[0] = 0;
}

static int ends_with_ci(const char *s, const char *tail) {
    size_t n = strlen(s), t = strlen(tail);
    return n >= t && !strcasecmp(s + n - t, tail);
}

// What the ExeFS folder holds: MOD_EXEFS for patches (.ips, .pchtxt), MOD_SUBSDK for code modules (subsdk* plugins, a
// replaced main). main.npdm alone is the process's metadata, not code: it counts as neither.
static int exefs_cb(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    int *k = ctx;
    if (kind != PLAT_FS_FILE) return 1;
    if (ends_with_ci(name, ".ips") || ends_with_ci(name, ".pchtxt")) *k |= MOD_EXEFS;
    else if (!strncasecmp(name, "subsdk", 6) || !strcasecmp(name, "main")) *k |= MOD_SUBSDK;
    return 1;
}

// MOD_CHEATS when the cheats folder holds an Atmosphere cheat file (<build id>.txt).
static int cheats_cb(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    if (kind == PLAT_FS_FILE && ends_with_ci(name, ".txt")) *(int *)ctx |= MOD_CHEATS;
    return 1;
}

// 1 when name is one of the '|'-separated names of list.
static int listed(const char *list, const char *name) {
    size_t n = strlen(name);
    for (const char *p = list; p && *p;) {
        const char *bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (len == n && !memcmp(p, name, n)) return 1;
        p = bar ? bar + 1 : NULL;
    }
    return 0;
}

// The position of name in the '|'-separated list, or -1.
static int list_pos(const char *list, const char *name) {
    size_t n = strlen(name);
    int k = 0;
    for (const char *p = list; p && *p; k++) {
        const char *bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (len == n && !memcmp(p, name, n)) return k;
        p = bar ? bar + 1 : NULL;
    }
    return -1;
}

static const char *order_list;   // TOTK_MODS_ORDER while sorting
static int by_order(const void *a, const void *b) {
    const ModInfo *x = a, *y = b;
    int px = list_pos(order_list, x->name), py = list_pos(order_list, y->name);
    if (px >= 0 && py >= 0) return px - py;
    if (px >= 0 || py >= 0) return px >= 0 ? -1 : 1;
    return strcmp(x->name, y->name);
}

static const char *problem_name(ModProblem p) {
    switch (p) {
    case MOD_EMPTY: return "empty";
    case MOD_PLUGIN: return "has a code module (subsdk or main), not supported; the whole mod stays off";
    case MOD_EXEFS_LATER: return "ExeFS patches wait for a rebuild of the game code; the whole mod stays off";
    case MOD_EXEFS_UNSUPPORTED: return "an ExeFS patch file cannot be applied; the whole mod stays off";
    case MOD_EXEFS_OTHER: return "its ExeFS patches are for another game version";
    case MOD_BAD_NAME: return "its folder name holds '|', which the order list uses; rename the folder";
    default: return "ok";
    }
}

// ---- ExeFS patch files

static uint32_t fnv(const void *p, size_t n) {
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 0x01000193u;
    return h;
}

const char *mods_main_build_id(void) {
    static char bid[65];
    static int read;
    if (read) return bid;
    read = 1;
    char path[2100];
    snprintf(path, sizeof path, "%s/main", rt_config()->exefs);
    PlatFile *f = plat_file_open(path);
    uint8_t head[0x60];
    int ok = f && plat_file_pread(f, head, sizeof head, 0) == sizeof head && !memcmp(head, "NSO0", 4);
    if (f) plat_file_close(f);
    for (int i = 0; ok && i < 32; i++) snprintf(bid + 2 * i, 3, "%02x", head[0x40 + i]);
    return bid;
}

// 1 when hex (any case; trailing zeros left out or not) names main's build id; also when main's is unknown.
static int same_build(const char *hex, size_t n) {
    const char *bid = mods_main_build_id();
    if (!*bid) return 1;
    if (!n || n > 64) return 0;
    for (size_t i = 0; i < 64; i++) {
        char c = i < n ? hex[i] : '0';
        if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
        if (c != bid[i]) return 0;
    }
    return 1;
}

// build-cache/exefs.status next to totk.exe, read once; "" when there is none.
static const char *exefs_status(void) {
    static char *text;
    if (text) return text;
    char path[2100];
    uint64_t n = 0;
    if (plat_exe_path(path, sizeof path)) {
        char *cut = strrchr(path, '/'), *back = strrchr(path, '\\');
        if (back > cut) cut = back;
        if (cut) snprintf(cut + 1, sizeof path - (size_t)(cut + 1 - path), "build-cache/exefs.status");
        text = (char *)plat_file_read(path, &n);
    }
    if (!text) text = calloc(1, 1);
    return text;
}

// The status line of key: its kind, built-in sets and note, NUL-terminated copies; 0 when none.
static int status_line(const char *key, const char *content, char *kind, char *over, char *note) {
    char want[20];
    snprintf(want, sizeof want, "%s\t%s\t", key, content);
    for (const char *line = exefs_status(); *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len > 18 && !memcmp(line, want, 18)) {
            char copy[600];
            snprintf(copy, sizeof copy, "%.*s", (int)(len < sizeof copy ? len : sizeof copy - 1), line + 18);
            char *t1 = strchr(copy, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
            if (!t1 || !t2) return 0;
            *t1 = *t2 = 0;
            char *r = strchr(t2 + 1, '\r');
            if (r) *r = 0;
            snprintf(kind, 16, "%s", copy);
            snprintf(over, 256, "%s", strcmp(t1 + 1, "-") ? t1 + 1 : "");
            snprintf(note, 256, "%s", t2 + 1);
            return 1;
        }
        line = end ? end + 1 : line + len;
    }
    return 0;
}

typedef struct {
    ModInfo *m;
    const char *folder, *rel;   // the folder listed and its path in the mod
    int unsupported;
} ExefsWalk;

// One file of an exefs folder: its build, then its status line.
static int exefs_file_cb(const char *leaf, int kind, uint64_t size, void *ctx) {
    (void)size;
    ExefsWalk *w = ctx;
    ModInfo *m = w->m;
    int ips = ends_with_ci(leaf, ".ips");
    if (kind != PLAT_FS_FILE || (!ips && !ends_with_ci(leaf, ".pchtxt"))) return 1;
    m->kind |= MOD_EXEFS;
    char path[4300], rel[600];
    snprintf(path, sizeof path, "%s/%s", w->folder, leaf);
    snprintf(rel, sizeof rel, "%s/%s/%s", m->name, w->rel, leaf);
    uint64_t n = 0;
    char *data = (char *)plat_file_read(path, &n);
    if (!data) {
        rt_log("mods: %s: cannot read %s", m->name, path);
        return 1;
    }
    int for_main;
    if (ips) {
        for_main = same_build(leaf, strlen(leaf) - 4);
    } else {
        char *id = NULL;   // after "@nsobid-" at the start of a line
        for (char *p = data; !id && p && data + n - p > 8;) {
            if (!memcmp(p, "@nsobid-", 8)) id = p + 8;
            p = memchr(p, '\n', (size_t)(data + n - p));
            if (p) p++;
        }
        size_t len = 0;
        while (id && id + len < data + n && isxdigit((unsigned char)id[len])) len++;
        for_main = id && same_build(id, len);
    }
    char key[9], content[9], kind_s[16], over[256], note[256];
    snprintf(key, sizeof key, "%08X", fnv(rel, strlen(rel)));
    snprintf(content, sizeof content, "%08X", fnv(data, (size_t)n));
    free(data);
    if (!for_main) {
        m->other_version++;
        return 1;
    }
    if (!status_line(key, content, kind_s, over, note)) {
        m->pending++;
        m->pending_key = (m->pending_key ^ fnv(key, 8)) * 0x01000193u;
        m->pending_key = (m->pending_key ^ fnv(content, 8)) * 0x01000193u;
        return 1;
    }
    if (!strcmp(kind_s, "unsupported")) {
        if (!w->unsupported) snprintf(m->note, sizeof m->note, "%s: %s", leaf, note);
        w->unsupported = 1;
    } else if (!strcmp(kind_s, "code") && m->nsets < (int)(sizeof m->sets / sizeof *m->sets)) {
        m->sets[m->nsets++] = (uint32_t)strtoul(key, NULL, 16);
        size_t k = strlen(m->overrides);
        if (*over) snprintf(m->overrides + k, sizeof m->overrides - k, "%s%s", k ? "," : "", over);
    } else if (!strcmp(kind_s, "other")) {
        m->other_version++;
    }
    return 1;
}

// atmosphere/exefs_patches/<name>/ of a mod: each <name> folder's files.
static int exefs_patches_cb(const char *leaf, int kind, uint64_t size, void *ctx) {
    (void)size;
    ExefsWalk *w = ctx;
    if (kind != PLAT_FS_DIR) return 1;
    char folder[4300], rel[600];
    snprintf(folder, sizeof folder, "%s/%s", w->folder, leaf);
    snprintf(rel, sizeof rel, "atmosphere/exefs_patches/%s", leaf);
    ExefsWalk sub = {w->m, folder, rel, 0};
    plat_fs_list(folder, exefs_file_cb, &sub);
    w->unsupported |= sub.unsupported;
    return 1;
}

// The ExeFS patch files of mod m (folder path): 1 when one cannot be applied.
static int exefs_check(ModInfo *m, const char *path) {
    ExefsWalk w = {m, m->exefs, m->exefs + strlen(path) + 1, 0};
    if (m->exefs[0]) plat_fs_list(m->exefs, exefs_file_cb, &w);
    char patches[2200];
    snprintf(patches, sizeof patches, "%s/atmosphere/exefs_patches", path);
    ExefsWalk at = {m, patches, "", 0};
    if (plat_dir_exists(patches)) plat_fs_list(patches, exefs_patches_cb, &at);
    return w.unsupported || at.unsupported;
}

// One mod folder of the scan.
static int scan_cb(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    const char *disabled = ctx;
    if (kind != PLAT_FS_DIR) return 1;
    if (nmods == MAX_MODS) {
        rt_log("mods: more than %d folders in %s; the rest are left out", MAX_MODS, dir);
        return 0;
    }
    ModInfo *m = &mods[nmods];
    memset(m, 0, sizeof *m);
    char path[2100];
    if (snprintf(m->name, sizeof m->name, "%s", name) >= (int)sizeof m->name ||
        snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) {
        rt_log("mods: %s: name too long, left out", name);
        return 1;
    }
    find_root(path, "romfs", m->romfs, sizeof m->romfs);
    if (!m->romfs[0]) find_root(path, "romfslite", m->romfs, sizeof m->romfs);
    find_root(path, "exefs", m->exefs, sizeof m->exefs);
    find_root(path, "cheats", m->cheats, sizeof m->cheats);
    if (m->romfs[0] && has_file(m->romfs)) m->kind |= MOD_ROMFS;
    if (m->exefs[0]) plat_fs_list(m->exefs, exefs_cb, &m->kind);
    if (m->cheats[0]) plat_fs_list(m->cheats, cheats_cb, &m->kind);
    int unsupported = exefs_check(m, path);
    // a mod whose code part the port cannot use is off as a whole: its RomFS files may expect that code
    m->problem = strchr(m->name, '|')                                ? MOD_BAD_NAME
               : m->kind & MOD_SUBSDK                                ? MOD_PLUGIN
               : unsupported                                         ? MOD_EXEFS_UNSUPPORTED
               : m->pending                                          ? MOD_EXEFS_LATER
               : m->nsets || m->kind & (MOD_ROMFS | MOD_CHEATS)     ? MOD_OK
               : m->kind & MOD_EXEFS                                 ? MOD_EXEFS_OTHER
                                                                     : MOD_EMPTY;
    m->enabled = !listed(disabled, m->name);
    nmods++;
    return 1;
}

void mods_scan(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *d = cfg_get("TOTK_MODS_DIR");
    if (d && *d) {
        snprintf(dir, sizeof dir, "%s", d);
    } else {
        char exe[2048];
        if (!plat_exe_path(exe, sizeof exe)) return;
        char *cut = strrchr(exe, '/'), *back = strrchr(exe, '\\');
        if (back > cut) cut = back;
        if (!cut) return;
        *cut = 0;
        snprintf(dir, sizeof dir, "%s/mods", exe);
    }
    if (!plat_dir_exists(dir)) {
        rt_log("mods: no folder %s", dir);
        return;
    }
    mods = calloc(MAX_MODS, sizeof *mods);
    if (!mods) return;
    plat_fs_list(dir, scan_cb, (void *)cfg_get("TOTK_MODS_DISABLED"));
    order_list = cfg_get("TOTK_MODS_ORDER");
    qsort(mods, nmods, sizeof *mods, by_order);
    order_list = NULL;
    for (int i = 0; i < nmods; i++) {
        snprintf(start_order[i], sizeof start_order[i], "%s", mods[i].name);
        start_enabled[i] = mods[i].enabled;
        const ModInfo *m = &mods[i];
        rt_log("mods: %s: %s,%s%s%s%s%s, %s%s%s", m->name, m->enabled ? "on" : "off", m->kind & MOD_ROMFS ? " romfs" : "",
               m->kind & MOD_EXEFS ? " exefs" : "", m->kind & MOD_SUBSDK ? " subsdk" : "",
               m->kind & MOD_CHEATS ? " cheats" : "", m->kind ? "" : " no files", problem_name(m->problem),
               m->note[0] ? ": " : "", m->note);
    }
    rt_log("mods: %d folders in %s", nmods, dir);
}

void mods_exefs_switch(void) {
    mods_scan();
    for (int i = 0; i < nmods; i++) {
        const ModInfo *m = &mods[i];
        if (!start_enabled[i] || m->problem != MOD_OK || !m->nsets) continue;
        for (int k = 0; k < m->nsets; k++) {
            char name[32];
            snprintf(name, sizeof name, "mod_%08x", m->sets[k]);
            int set = rt_patch_set(name);
            if (set >= 0) rt_patch_switch(set, 1);
            else rt_log("mods: %s: patch set %s is not in this build", m->name, name);
        }
        char over[256];
        snprintf(over, sizeof over, "%s", m->overrides);
        for (char *s = strtok(over, ","); s; s = strtok(NULL, ",")) {
            int set = rt_patch_set(s);
            if (set >= 0) rt_patch_switch(set, 0);
            rt_log("mods: %s replaces the port's patch set %s; it stays off", m->name, s);
        }
        rt_log("mods: %s: %d ExeFS patch files on", m->name, m->nsets);
    }
}

int mods_exefs_pending(uint32_t *key) {
    int n = 0;
    for (int i = 0; i < nmods; i++) {
        if (!start_enabled[i] || mods[i].problem != MOD_EXEFS_LATER) continue;
        n += (int)mods[i].pending;
        *key = (*key ^ mods[i].pending_key) * 0x01000193u;
    }
    return n;
}

const char *mods_dir(void) { return dir; }
int mods_count(void) { return nmods; }
ModInfo *mods_at(int i) { return i >= 0 && i < nmods ? &mods[i] : NULL; }

// TOTK_MODS_ORDER as every listed name top first; TOTK_MODS_DISABLED as the switched-off names. A name holding '|'
// would split into two; it is left out of both (its mod is unusable anyway).
static void write_keys(void) {
    size_t cap = (size_t)nmods * 257 + 1;
    char *order = calloc(cap, 1), *off = calloc(cap, 1);
    if (order && off) {
        for (int i = 0; i < nmods; i++) {
            if (mods[i].problem == MOD_BAD_NAME) continue;
            if (*order) strcat(order, "|");
            strcat(order, mods[i].name);
            if (!mods[i].enabled) {
                if (*off) strcat(off, "|");
                strcat(off, mods[i].name);
            }
        }
        cfg_set("TOTK_MODS_ORDER", order);
        cfg_set("TOTK_MODS_DISABLED", off);
    }
    free(order);
    free(off);
}

void mods_set_enabled(int i, int on) {
    if (i < 0 || i >= nmods) return;
    mods[i].enabled = on != 0;
    write_keys();
}

void mods_move(int i, int delta) {
    int j = i + delta;
    if (i < 0 || i >= nmods || j < 0 || j >= nmods || i == j) return;
    ModInfo t = mods[i];
    mods[i] = mods[j];
    mods[j] = t;
    write_keys();
}

int mods_changed(void) {
    for (int i = 0; i < nmods; i++)
        if (strcmp(mods[i].name, start_order[i]) || mods[i].enabled != start_enabled[i]) return 1;
    return 0;
}
