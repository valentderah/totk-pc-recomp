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
// rebuild at start (cheats.c), and its mod stays off until then. A file's writes into main's data (.rodata, .data,
// .bss) are lines of build-cache/exefs.data, stored into the loaded main before the game runs (mods_exefs_data), as
// emulators apply a patch file to the module before its dynamic linker runs.
//
// Code modules: an exefs/subsdk* file is a plugin that patches main from its own start-up on the Switch, which the port
// cannot load. A build plugins.c has handlers for runs as the port's own code: the setup builds its hooks in as the
// patch set of its file (tools/plugins.py, status kind plugin) and mods_exefs_switch switches it on with its mod; a
// known build the game was built without waits for the rebuild like a patch file. Any other code module, and a
// replaced main, cannot run (ModInfo.code): its mod stays off unless mods_without_code = 1.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cfg.h"
#include "host.h"
#include "mods.h"
#include "platform/platform.h"
#include "sha256.h"

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

// A code module of m the port cannot run, named in m->code.
static void code_unrun(ModInfo *m, const char *name) {
    size_t k = strlen(m->code);
    snprintf(m->code + k, sizeof m->code - k, "%s%s", k ? ", " : "", name);
}

// What the ExeFS folder holds: MOD_EXEFS for patches (.ips, .pchtxt), MOD_SUBSDK for code modules (subsdk* plugins, a
// replaced main). A replaced main never runs; a subsdk* file may be a plugin the port runs (code_file_cb). main.npdm
// alone is the process's metadata, not code: it counts as neither.
static int exefs_cb(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    ModInfo *m = ctx;
    if (kind != PLAT_FS_FILE) return 1;
    if (ends_with_ci(name, ".ips") || ends_with_ci(name, ".pchtxt")) {
        m->kind |= MOD_EXEFS;
    } else if (!strncasecmp(name, "subsdk", 6)) {
        m->kind |= MOD_SUBSDK;
    } else if (!strcasecmp(name, "main")) {
        m->kind |= MOD_SUBSDK;
        code_unrun(m, name);
    }
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
    case MOD_PLUGIN:
        return "has a code module the port cannot run (a subsdk plugin it does not know, or a replaced main); the "
               "whole mod stays off (mods_without_code = 1 uses the rest of it)";
    case MOD_EXEFS_LATER:
        return "ExeFS patches or a plugin's hooks wait for a rebuild of the game code; the whole mod stays off";
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

// build-cache/<name> next to totk.exe, read once into *text; "" when there is none (mods_probe_cache sets the text).
static char *status_text, *data_text;
static const char *build_cache(const char *name, char **text) {
    if (*text) return *text;
    char path[2100];
    uint64_t n = 0;
    if (plat_exe_path(path, sizeof path)) {
        char *cut = strrchr(path, '/'), *back = strrchr(path, '\\');
        if (back > cut) cut = back;
        if (cut && snprintf(cut + 1, sizeof path - (size_t)(cut + 1 - path), "build-cache/%s", name) <
                       (int)(sizeof path - (size_t)(cut + 1 - path)))
            *text = (char *)plat_file_read(path, &n);
    }
    if (!*text) *text = calloc(1, 1);
    return *text;
}

static const char *exefs_status(void) { return build_cache("exefs.status", &status_text); }
static const char *exefs_data(void) { return build_cache("exefs.data", &data_text); }

// The status line of key: its kind, built-in sets and note, NUL-terminated copies; 0 when none.
static int status_line(const char *key, const char *content, char *kind, char *over, char *note) {
    char want[20];
    snprintf(want, sizeof want, "%s\t%s\t", key, content);
    for (const char *line = exefs_status(); *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len > 18 && !memcmp(line, want, 18)) {
            char copy[600];   // the line after the two hashes, without the next line
            size_t rest = len - 18;
            snprintf(copy, sizeof copy, "%.*s", (int)(rest < sizeof copy ? rest : sizeof copy - 1), line + 18);
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

// A file of m the build lacks: m waits for the rebuild, which the names and contents of such files key (cheats.c).
static void pending(ModInfo *m, const char *key, const char *content) {
    m->pending++;
    m->pending_key = (m->pending_key ^ fnv(key, 8)) * 0x01000193u;
    m->pending_key = (m->pending_key ^ fnv(content, 8)) * 0x01000193u;
}

// One code module (subsdk*) of an exefs folder. Its status line (tools/exefs_patches.py) of kind plugin makes it a
// plugin the port runs as its own code ("hooks <count> <name>": the set mod_<hash> holds the hooks); without a line
// (or with another content) a build plugins.c knows waits for the rebuild; any other module is one the port cannot run.
static int code_file_cb(const char *leaf, int kind, uint64_t size, void *ctx) {
    (void)size;
    ExefsWalk *w = ctx;
    ModInfo *m = w->m;
    if (kind != PLAT_FS_FILE || strncasecmp(leaf, "subsdk", 6)) return 1;
    char path[4300], rel[600];
    snprintf(path, sizeof path, "%s/%s", w->folder, leaf);
    snprintf(rel, sizeof rel, "%s/%s/%s", m->name, w->rel, leaf);
    uint64_t n = 0;
    uint8_t *data = plat_file_read(path, &n);
    if (!data) {
        rt_log("mods: %s: cannot read %s", m->name, path);
        code_unrun(m, leaf);
        return 1;
    }
    char key[9], content[9], kind_s[16], over[256], note[256];
    uint8_t sha[32];
    snprintf(key, sizeof key, "%08X", fnv(rel, strlen(rel)));
    snprintf(content, sizeof content, "%08X", fnv(data, (size_t)n));
    sha256(data, (size_t)n, sha);
    free(data);
    int line = status_line(key, content, kind_s, over, note), hooks = 0, at = 0;
    if (line && !strcmp(kind_s, "plugin") && sscanf(note, "hooks %d %n", &hooks, &at) == 1 && at &&
        m->nplugins < (int)(sizeof m->plugin / sizeof *m->plugin)) {
        int k = m->nplugins++;
        snprintf(m->plugin[k].module, sizeof m->plugin[k].module, "%s", leaf);
        snprintf(m->plugin[k].name, sizeof m->plugin[k].name, "%s", note + at);
        m->plugin[k].hooks = hooks;
        m->plugin[k].set = (uint32_t)strtoul(key, NULL, 16);
        size_t o = strlen(m->overrides);
        if (*over) snprintf(m->overrides + o, sizeof m->overrides - o, "%s%s", o ? "," : "", over);
    } else if (!line && plugins_known(sha)) {
        pending(m, key, content);
    } else {
        code_unrun(m, leaf);
    }
    return 1;
}

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
        pending(m, key, content);
        return 1;
    }
    if (!strcmp(kind_s, "unsupported")) {
        if (!w->unsupported) snprintf(m->note, sizeof m->note, "%s: %s", leaf, note);
        w->unsupported = 1;
    } else if (!strcmp(kind_s, "code") && m->nsets < (int)(sizeof m->sets / sizeof *m->sets)) {
        m->sets[m->nsets++] = (uint32_t)strtoul(key, NULL, 16);
        size_t k = strlen(m->overrides);
        if (*over) snprintf(m->overrides + k, sizeof m->overrides - k, "%s%s", k ? "," : "", over);
    } else if (!strcmp(kind_s, "data") && m->ndata < (int)(sizeof m->data / sizeof *m->data)) {
        m->data[m->ndata++] = (uint32_t)strtoul(key, NULL, 16);
    } else if (!strcmp(kind_s, "other")) {
        m->other_version++;
    }
    unsigned long bytes;   // a code or data file's data writes: exefs.data must hold them all (mods_exefs_data)
    if ((!strcmp(kind_s, "code") || !strcmp(kind_s, "data")) && sscanf(note, "data %lu", &bytes) == 1)
        m->data_bytes += (uint32_t)bytes;
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

// The ExeFS patch files and code modules of mod m (folder path): 1 when a patch file cannot be applied.
static int exefs_check(ModInfo *m, const char *path) {
    ExefsWalk w = {m, m->exefs, m->exefs + strlen(path) + 1, 0};
    if (m->exefs[0]) plat_fs_list(m->exefs, exefs_file_cb, &w);
    if (m->exefs[0] && m->kind & MOD_SUBSDK) plat_fs_list(m->exefs, code_file_cb, &w);
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
    if (m->exefs[0]) plat_fs_list(m->exefs, exefs_cb, m);
    if (m->cheats[0]) plat_fs_list(m->cheats, cheats_cb, &m->kind);
    int unsupported = exefs_check(m, path);
    // A mod whose code part the port cannot use is off as a whole: its RomFS files may expect that code. A code
    // module the port cannot run (m->code) never loads; with mods_without_code = 1 the rest of the mod is used, when
    // it has a rest.
    int rest = m->kind & (MOD_ROMFS | MOD_EXEFS | MOD_CHEATS) || m->nplugins;
    int code_off = m->code[0] && (!cfg_bool("TOTK_MODS_WITHOUT_CODE") || !rest);
    int usable = m->nsets || m->ndata || m->nplugins || m->kind & (MOD_ROMFS | MOD_CHEATS);
    m->problem = strchr(m->name, '|')  ? MOD_BAD_NAME
               : code_off              ? MOD_PLUGIN
               : unsupported           ? MOD_EXEFS_UNSUPPORTED
               : m->pending            ? MOD_EXEFS_LATER
               : usable                ? MOD_OK
               : m->kind & MOD_EXEFS   ? MOD_EXEFS_OTHER
                                       : MOD_EMPTY;
    m->enabled = !listed(disabled, m->name);
    nmods++;
    return 1;
}

// The mods folder into out: TOTK_MODS_DIR, else <dir of totk.exe>/mods. 0 when the exe's path is unknown.
static int mods_folder(char *out, size_t cap) {
    const char *d = cfg_get("TOTK_MODS_DIR");
    if (d && *d) return snprintf(out, cap, "%s", d) < (int)cap;
    char exe[2048];
    if (!plat_exe_path(exe, sizeof exe)) return 0;
    char *cut = strrchr(exe, '/'), *back = strrchr(exe, '\\');
    if (back > cut) cut = back;
    if (!cut) return 0;
    *cut = 0;
    return snprintf(out, cap, "%s/mods", exe) < (int)cap;
}

typedef struct {
    const char *folder, *disabled;
    void (*cb)(const char *name, const char *romfs, void *ctx);
    void *ctx;
} Peek;

static int peek_cb(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    const Peek *pk = ctx;
    if (kind != PLAT_FS_DIR || listed(pk->disabled, name)) return 1;
    char path[2100], romfs[2048];
    if (snprintf(path, sizeof path, "%s/%s", pk->folder, name) >= (int)sizeof path) return 1;
    find_root(path, "romfs", romfs, sizeof romfs);
    if (!romfs[0]) find_root(path, "romfslite", romfs, sizeof romfs);
    if (romfs[0]) pk->cb(name, romfs, pk->ctx);
    return 1;
}

void mods_peek_romfs(void (*cb)(const char *name, const char *romfs, void *ctx), void *ctx) {
    char folder[2048];
    if (!mods_folder(folder, sizeof folder) || !plat_dir_exists(folder)) return;
    Peek pk = {folder, cfg_get("TOTK_MODS_DISABLED"), cb, ctx};
    plat_fs_list(folder, peek_cb, &pk);
}

static int scanned;

void mods_scan(void) {
    if (scanned) return;
    scanned = 1;
    if (!mods_folder(dir, sizeof dir)) return;
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
        if (m->code[0] && m->problem != MOD_PLUGIN)
            rt_log("mods: %s: its code module (%s) does not load (mods_without_code = 1); the rest of the mod is used",
                   m->name, m->code);
        for (int k = 0; k < m->nplugins; k++)
            rt_log("mods: %s: its code module %s runs as the port's own code: %s, %d hooks%s", m->name,
                   m->plugin[k].module, m->plugin[k].name, m->plugin[k].hooks,
                   m->plugin[k].hooks ? "" : " (none for this game version, as on the Switch)");
    }
    rt_log("mods: %d folders in %s", nmods, dir);
}

// ---- ExeFS data writes (build-cache/exefs.data; tools/exefs_patches.py data_line has the format)

// 1 when key names one of m's usable ExeFS patch files: one with a patch set, or one with data writes only.
static int owns(const ModInfo *m, uint32_t key) {
    for (int k = 0; k < m->nsets; k++)
        if (m->sets[k] == key) return 1;
    for (int k = 0; k < m->ndata; k++)
        if (m->data[k] == key) return 1;
    return 0;
}

static int hex_digit(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f'   ? c - 'a' + 10
         : c >= 'A' && c <= 'F'   ? c - 'A' + 10
                                  : -1;
}

// One line: "<file hash>\t<content hash>\t<offset in main>\t<bytes>[\tbase]", all hex. 0 when malformed.
typedef struct { char key[9], content[9]; uint64_t off; uint8_t bytes[256]; int n, base; } DataRun;
static int parse_run(const char *line, size_t len, DataRun *r) {
    char copy[1100], *f[6];
    if (len >= sizeof copy) return 0;
    memcpy(copy, line, len);
    copy[len] = 0;
    int nf = 0;
    for (char *p = copy; p && nf < 6; nf++) {
        f[nf] = p;
        p = strchr(p, '\t');
        if (p) *p++ = 0;
    }
    if (nf < 4 || nf > 5 || strlen(f[0]) != 8 || strlen(f[1]) != 8) return 0;
    memcpy(r->key, f[0], 9);
    memcpy(r->content, f[1], 9);
    char *end;
    r->off = strtoull(f[2], &end, 16);
    size_t hex = strlen(f[3]);
    if (!*f[2] || *end || !hex || hex % 2 || hex / 2 > sizeof r->bytes) return 0;
    r->n = (int)(hex / 2);
    for (int i = 0; i < r->n; i++) {
        int hi = hex_digit(f[3][2 * i]), lo = hex_digit(f[3][2 * i + 1]);
        if (hi < 0 || lo < 0) return 0;
        r->bytes[i] = (uint8_t)(hi << 4 | lo);
    }
    r->base = nf == 5;
    return nf == 4 || (!strcmp(f[4], "base") && r->n == 8);
}

// Stores n bytes into the loaded main at a: a read-only page (.rodata, loader.c seal) is writable for its part of the
// copy only, as cheats.c wr_main does. 0 when a page is neither readable nor writable.
static int store_main(uint64_t a, const uint8_t *from, int n) {
    while (n > 0) {
        uint64_t page = a & ~0xFFFull, end;
        int k = (int)(page + 0x1000 - a);
        if (k > n) k = n;
        int prot = plat_mem_state(page, page + 0x1000, &end);
        if (prot != PLAT_PROT_READ && prot != PLAT_PROT_RW) return 0;
        if (prot == PLAT_PROT_READ && !plat_mem_protect(page, 0x1000, PLAT_PROT_RW)) return 0;
        memcpy((void *)(uintptr_t)a, from, (size_t)k);
        if (prot == PLAT_PROT_READ) plat_mem_protect(page, 0x1000, PLAT_PROT_READ);
        a += (uint64_t)k;
        from += k;
        n -= k;
    }
    return 1;
}

long mods_exefs_data(const ModInfo *m, uint64_t base, uint64_t size, uint64_t code_end,
                     int (*store)(uint64_t addr, const uint8_t *bytes, int n)) {
    if (!store) store = store_main;
    long total = 0, found = 0;
    for (int pass = 0; pass < 2; pass++) {   // pass 0 checks every line, pass 1 stores
        // exefs.data must hold every byte the status lines count: one missing or cut short would leave the code
        // patches on without their data
        if (pass && found != (long)m->data_bytes) {
            rt_log("mods: %s: build-cache/exefs.data holds %ld of the %u bytes its ExeFS patch files write", m->name,
                   found, m->data_bytes);
            return -1;
        }
        for (const char *line = exefs_data(), *next; *line; line = next) {
            const char *nl = strchr(line, '\n');
            size_t len = nl ? (size_t)(nl - line) : strlen(line);
            next = nl ? nl + 1 : line + len;
            if (len && line[len - 1] == '\r') len--;
            char key[9];
            snprintf(key, sizeof key, "%.*s", (int)(len < 8 ? len : 8), line);
            if (!len || *line == '#' || !owns(m, (uint32_t)strtoul(key, NULL, 16))) continue;
            DataRun r;
            char kind[16], over[256], note[256];
            // a line whose file the status does not list as built (another content) or that leaves main's data: the
            // two build-cache files and this build disagree
            if (!parse_run(line, len, &r) || !status_line(r.key, r.content, kind, over, note) ||
                (strcmp(kind, "code") && strcmp(kind, "data")) || r.off < code_end || r.off > size ||
                (uint64_t)r.n > size - r.off) {
                rt_log("mods: %s: a line of build-cache/exefs.data does not fit this build: %.*s", m->name, (int)len,
                       line);
                return -1;
            }
            if (!pass) {
                found += r.n;
                continue;
            }
            if (r.base) {   // a DT_RELR word holds its addend; the dynamic linker adds the module's base
                uint64_t w;
                memcpy(&w, r.bytes, 8);
                w += base;
                memcpy(r.bytes, &w, 8);
            }
            if (!store(base + r.off, r.bytes, r.n)) {
                rt_log("mods: %s: cannot write main+%#llx", m->name, (unsigned long long)r.off);
                return -1;
            }
            total += r.n;
        }
    }
    return total;
}

static char *dup_text(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

void mods_probe_cache(const char *status, const char *data) {
    free(status_text);
    free(data_text);
    status_text = dup_text(status);
    data_text = dup_text(data);
}

void mods_rescan(void) {
    free(mods);
    mods = NULL;
    nmods = 0;
    scanned = 0;
}

// Switches the patch set mod_<key> on; 0 when this build lacks it (logged).
static int set_on(const ModInfo *m, uint32_t key) {
    char name[32];
    snprintf(name, sizeof name, "mod_%08x", key);
    int set = rt_patch_set(name);
    if (set >= 0) rt_patch_switch(set, 1);
    else rt_log("mods: %s: patch set %s is not in this build", m->name, name);
    return set >= 0;
}

void mods_exefs_switch(void) {
    mods_scan();
    const RtLoaded *main_mod = NULL;
    for (int i = 0; ldr_module(i); i++)
        if (!strcmp(ldr_module(i)->name, "main")) main_mod = ldr_module(i);
    // Code module names taken by a mod in use, top first: whatever number of mods have an exefs/subsdk9, the Switch
    // loads one, the top mod's, so a lower mod's plugin of the same name stays off.
    struct { char module[16]; const char *by; } taken[64];
    int ntaken = 0;
    for (int i = 0; i < nmods; i++) {
        ModInfo *m = &mods[i];
        if (!start_enabled[i] || m->problem != MOD_OK) continue;
        int on = 0;
        if (m->nsets || m->ndata) {
            // the data first: a mod whose data writes cannot all be stored keeps its code patches off too, as half
            // of a patch file is worse than none
            long stored = main_mod ? mods_exefs_data(m, main_mod->base, main_mod->size, main_mod->text_end, NULL) : 0;
            if (stored < 0) {
                rt_log("mods: %s: its ExeFS patch files stay off: their data writes could not all be stored", m->name);
            } else {
                for (int k = 0; k < m->nsets; k++) set_on(m, m->sets[k]);
                rt_log("mods: %s: %d ExeFS patch files on, %ld bytes of main's data stored", m->name,
                       m->nsets + m->ndata, stored);
                on = 1;
            }
        }
        char code[64];   // its code modules that do not run (mods_without_code = 1) take their names too
        snprintf(code, sizeof code, "%s", m->code);
        for (char *s = strtok(code, ", "); s && ntaken < 64; s = strtok(NULL, ", ")) {
            snprintf(taken[ntaken].module, sizeof taken[ntaken].module, "%s", s);
            taken[ntaken++].by = m->name;
        }
        for (int k = 0; k < m->nplugins; k++) {
            int t = 0;
            while (t < ntaken && strcasecmp(taken[t].module, m->plugin[k].module)) t++;
            if (t < ntaken) {
                snprintf(m->plugin[k].shadowed, sizeof m->plugin[k].shadowed, "%s", taken[t].by);
                rt_log("mods: %s: its code module %s does not run: %s, higher in the list, has one", m->name,
                       m->plugin[k].module, taken[t].by);
                continue;
            }
            if (ntaken < 64) {
                snprintf(taken[ntaken].module, sizeof taken[ntaken].module, "%s", m->plugin[k].module);
                taken[ntaken++].by = m->name;
            }
            if (m->plugin[k].hooks && set_on(m, m->plugin[k].set)) {
                rt_log("mods: %s: %s runs, %d hooks on", m->name, m->plugin[k].name, m->plugin[k].hooks);
                on = 1;
            }
        }
        char over[256];
        snprintf(over, sizeof over, "%s", on ? m->overrides : "");
        for (char *s = strtok(over, ","); s; s = strtok(NULL, ",")) {
            int set = rt_patch_set(s);
            if (set >= 0) rt_patch_switch(set, 0);
            rt_log("mods: %s replaces the port's patch set %s; it stays off", m->name, s);
        }
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
