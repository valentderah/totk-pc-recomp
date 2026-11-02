// Adapted from MacNativeGames games/totk/runtime/romfs.c @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames
// contributors).
// Read-only game RomFS backed by the player's extracted image.
//
// Input: <romfs>/romfs.bin (rt_config()->romfs), whose six IVFC levels and SHA-256 tools/extract.py verified, and
// romfs-index.bin next to totk.exe (TOTK_ROMFS_INDEX overrides), which tools/romfs_index.py wrote together with
// romfs_config.h. fs_init checks the index's SHA-256 and layout and the image size. The full 17 GB hash runs only with
// TOTK_ROMFS_VERIFY=full; a proof file (.romfs-verified-v1 next to the index, or in TOTK_ROMFS_PROOF_DIR) keyed by
// the file's volume, file id, size and times skips it on later starts, TOTK_ROMFS_VERIFY=force ignores the proof.
//
// Reads are positional (platform.h plat_file_pread), so threads read the image in parallel without a file-pointer
// lock. TOTK_ROMFS_MMAP=1 maps the image instead.
//
// Overlays: files listed in a directory's manifest.tsv are served in place of the originals. Each line names a RomFS
// path, the overlay's size and SHA-256 and the original's SHA-256 ("-" adds a file the RomFS lacks; it is numbered
// after the RomFS's own files, file_count + k in path order). The directories, in this order: TOTK_ROMFS_PATCHES and
// TOTK_ROMFS_MODS; unset or "0" = none. A missing path, a size or hash mismatch or a changed original stops the start.
// An overlay file is opened when the game opens it; its identity (volume, file id, size, last write) is kept from the
// start check, and an open that finds another file there stops the game.
//
// Player mods (mods.c): the files under each enabled mod's RomFS root are overlays too, checked by identity only (no
// hash, no manifest). A path several overlays give is served by one: a player mod higher in the order beats a lower
// one, any player mod beats the TOTK_ROMFS_MODS manifest, which beats romfs-patches (overlay_contest). A mod path that
// matches a RomFS path only without case is served under the RomFS spelling.
//
// The resource size table: with a player mod enabled, the game's table is rebuilt at start with the tables the mods
// ship merged in and the entries of the mods' files raised to what they need (restbl_overlay), and served from
// <profile>/mods/ in place of the game's or a mod's table.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cfg.h"
#include "fs.h"
#include "host.h"
#include "input_map.h"
#include "mods.h"
#include "romfs_config.h"
#include "romfs_path.h"
#include "platform/platform.h"
#include "restbl.h"
#include "sha256.h"

#include <zstd.h>

#define NH 65536                     // RomFS handle slots
#define READ_CHUNK (64u << 20)       // largest single read
#define HASH_CHUNK (8u << 20)
#define ADDED UINT64_MAX             // an added overlay file's index until overlay_init numbers it

typedef struct { uint64_t offset, size; uint32_t name, len; } Entry;
_Static_assert(sizeof(Entry) == 24, "index entry layout");

typedef struct { uint32_t volume, pad; uint64_t file_id, size, write_time, create_time; } Identity;   // no padding
// prio: which overlay serves a path two of them give (the higher); romfs-patches 0, the TOTK_ROMFS_MODS manifest 1, a
// player mod 2 + its distance from the bottom of the order. mod: its index in mods_at + 1, 0 for any other overlay.
typedef struct { uint64_t index, size; char *path, *name; Identity id; int prio, mod; } Overlay;

static uint8_t *index_bytes;
static size_t index_size;
static uint64_t file_count, strings_offset;
static PlatFile *image;
static const uint8_t *image_map;
static char image_file[2048];
static char index_dir[2048];         // the directory of romfs-index.bin: the proof file's default place
static char mount_name[32];
static PlatMutex lock = PLAT_MUTEX_INIT;
static uint64_t handles[NH];         // file index + 1; 0 is free
static uint32_t handle_ovl[NH];      // overlay slot + 1 of an open handle, 0 = original
static PlatFile *handle_file[NH];    // the handle's open overlay file, when handle_ovl is set
static Overlay *overlays;            // sorted by RomFS index after overlay_init
static size_t noverlay, overlay_cap;
static char **added;                 // RomFS paths of the added files, sorted; added[k] is index file_count + k
static size_t nadded;
static int patches_loaded;           // a romfs-patches manifest gave a file (romfs_patches_loaded)

static void need(int c, const char *m) {
    if (!c) rt_fatal("RomFS: %s", m);
}

static Entry entry(uint64_t i) {
    Entry e;
    memcpy(&e, index_bytes + 24 + i * 24, 24);
    return e;
}
static const char *entry_name(Entry e) { return (const char *)index_bytes + strings_offset + e.name; }

static double now_s(void) { return (double)plat_now_ns() / 1e9; }

// ---- positional reads
// Bytes read at off (0 at or past the end of the file), -1 on an I/O error.
static int64_t read_at(PlatFile *f, void *buf, uint32_t n, uint64_t off) {
    mem_thaw((uintptr_t)buf, n);   // the kernel's write would fail on a frozen page instead of faulting
    int64_t got = plat_file_pread(f, buf, n, off);
    if (got == -2)   // a read-only page in the buffer: a frozen region missed its thaw
        rt_log("RomFS: read of %u bytes into %p refused: the buffer is not writable", n, buf);
    return got < 0 ? -1 : got;
}

static PlatFile *open_read(const char *path) { return plat_file_open(path); }

static Identity identity_of(PlatFile *f) {
    PlatFileId i;
    need(plat_file_id(f, &i), "file information");
    return (Identity){(uint32_t)i.volume, 0, i.file_id, i.size, i.write_time, i.create_time};
}

// ---- SHA-256 over a file range, streaming
static void hex32(const uint8_t d[32], char out[65]) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static void sha_range(PlatFile *f, uint64_t offset, uint64_t size, char hex[65], int progress) {
    Sha256 h;
    sha256_init(&h);
    uint8_t *buf = malloc(HASH_CHUNK);
    need(buf != NULL, "hash buffer");
    double t0 = now_s();
    for (uint64_t done = 0; done < size;) {
        uint32_t want = size - done > HASH_CHUNK ? HASH_CHUNK : (uint32_t)(size - done);
        int64_t n = read_at(f, buf, want, offset + done);
        need(n > 0, "read of hash input");
        sha256_update(&h, buf, (size_t)n);
        done += (uint64_t)n;
        if (progress && (done % (2ull << 30) < (uint64_t)n || done == size))
            rt_log("RomFS: hashed %.1f of %.1f GiB (%.0f MiB/s)", done / 1073741824.0, size / 1073741824.0,
                   done / 1048576.0 / (now_s() - t0 + 1e-9));
    }
    uint8_t d[32];
    sha256_final(&h, d);
    free(buf);
    hex32(d, hex);
}

// ---- full image verification with a proof file
typedef struct { char magic[16], sha256[65]; uint8_t pad[7]; Identity id; } RomfsProof;

static void verify_image(const char *dir) {
    const char *mode = cfg_get("TOTK_ROMFS_VERIFY");
    if (!mode || (strcmp(mode, "full") && strcmp(mode, "force"))) return;
    const char *pdir = cfg_get("TOTK_ROMFS_PROOF_DIR");
    if (pdir && *pdir) dir = pdir;
    RomfsProof want = {0}, saved;
    memcpy(want.magic, "TOTKRFS-PROOF-1", 16);
    memcpy(want.sha256, ROMFS_IMAGE_SHA256, sizeof ROMFS_IMAGE_SHA256 < 65 ? sizeof ROMFS_IMAGE_SHA256 : 65);
    want.id = identity_of(image);
    char path[2100];
    need(snprintf(path, sizeof path, "%s/.romfs-verified-v1", dir) < (int)sizeof path, "proof path");
    if (strcmp(mode, "force")) {
        PlatFile *p = open_read(path);
        if (p) {
            int ok = plat_file_pread(p, &saved, sizeof saved, 0) == sizeof saved;
            plat_file_close(p);
            if (ok && !memcmp(&saved, &want, sizeof want)) {
                rt_log("RomFS unchanged since its full SHA-256 check (%s)", path);
                return;
            }
        }
    }
    rt_log("RomFS: verifying the SHA-256 of %s (%llu bytes)", image_file, (unsigned long long)want.id.size);
    char hex[65];
    sha_range(image, 0, want.id.size, hex, 1);
    if (strcmp(hex, ROMFS_IMAGE_SHA256))
        rt_fatal("RomFS image %s has SHA-256 %s, the extraction recorded %s (extract it again)", image_file, hex,
                 ROMFS_IMAGE_SHA256);
    Identity after = identity_of(image);
    need(!memcmp(&after, &want.id, sizeof after), "image changed during verification");
    plat_file_write_atomic(path, &want, sizeof want);
    rt_log("RomFS: image SHA-256 ok");
}

// ---- index lookup
static uint64_t lower(const char *name) {
    uint64_t lo = 0, hi = file_count;
    while (lo < hi) {
        uint64_t m = lo + (hi - lo) / 2;
        if (strcmp(entry_name(entry(m)), name) < 0) lo = m + 1;
        else hi = m;
    }
    return lo;
}

static size_t added_lower(const char *p) {
    size_t lo = 0, hi = nadded;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (strcmp(added[m], p) < 0) lo = m + 1;
        else hi = m;
    }
    return lo;
}

// The index of RomFS path p (an added file: file_count + k), or UINT64_MAX.
static uint64_t file_index(const char *p) {
    uint64_t i = lower(p);
    if (i < file_count && !strcmp(p, entry_name(entry(i)))) return i;
    size_t k = added_lower(p);
    return k < nadded && !strcmp(added[k], p) ? file_count + k : UINT64_MAX;
}

// 1 when a RomFS file or an added one lies under folder (a path ending in '/').
static int folder_exists(const char *folder) {
    size_t n = strlen(folder);
    uint64_t i = lower(folder);
    if (i < file_count && !strncmp(entry_name(entry(i)), folder, n)) return 1;
    size_t k = added_lower(folder);
    return k < nadded && !strncmp(added[k], folder, n);
}

// The overlay slot + 1 that replaces RomFS file i, or 0.
static uint32_t overlay_slot(uint64_t i) {
    size_t lo = 0, hi = noverlay;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (overlays[m].index < i) lo = m + 1;
        else hi = m;
    }
    return lo < noverlay && overlays[lo].index == i ? (uint32_t)(lo + 1) : 0;
}

// ---- overlays
// Loads and checks one overlay folder. A failed check stops the game, except with optional set (the Xbox prompts
// folder): the folder is dropped, the reason logged, and 0 returned. Returns 1 when the folder is loaded or has no
// manifest. prio: the Overlay field (0 for the port's own folders, 1 for the TOTK_ROMFS_MODS manifest).
static int overlay_load(const char *dir, int optional, int prio) {
#define BAD(...) do { if (!optional) rt_fatal(__VA_ARGS__); rt_log(__VA_ARGS__); goto bad; } while (0)
    char path[2100];
    need(snprintf(path, sizeof path, "%s/manifest.tsv", dir) < (int)sizeof path, "overlay path");
    FILE *m = fopen(path, "rb");
    if (!m) {
        rt_log("RomFS overlay: none (%s)", path);
        return 1;
    }
    char line[8192];
    size_t first = noverlay;
    uint64_t hashed = 0;
    double t0 = now_s();
    while (fgets(line, sizeof line, m)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char name[4096], sha[65], orig[65], hex[65];
        unsigned long long size = 0;
        need(sscanf(line, "%4095[^\t]\t%llu\t%64[0-9a-f]\t%64[0-9a-f-]", name, &size, sha, orig) == 4 &&
             strlen(sha) == 64 && (strlen(orig) == 64 || !strcmp(orig, "-")), "overlay manifest line");
        need(romfs_path_valid_relative(name), "overlay path");
        uint64_t i = lower(name);
        int known = i < file_count && !strcmp(entry_name(entry(i)), name), add = !strcmp(orig, "-");
        if (!known && !add)
            BAD("RomFS overlay: %s/manifest.tsv names %s, which the RomFS lacks; a new file has \"-\" as its "
                "original's SHA-256", dir, name);
        if (known && add)
            BAD("RomFS overlay: %s/manifest.tsv adds %s, which the RomFS already has; give its original's SHA-256",
                dir, name);
        Entry e = {0};
        if (known) {
            e = entry(i);
            sha_range(image, e.offset, e.size, hex, 0);
            if (strcmp(hex, orig)) BAD("RomFS overlay: the original of %s has SHA-256 %s, %s/manifest.tsv says %s",
                                       name, hex, dir, orig);
        } else {
            i = ADDED;
        }
        char file[8200];
        need(snprintf(file, sizeof file, "%s/%s", dir, name) < (int)sizeof file, "overlay file path");
        PlatFile *f = open_read(file);
        if (!f) BAD("RomFS overlay: %s missing", file);
        Identity id = identity_of(f);
        if (id.size != size) {
            plat_file_close(f);
            BAD("RomFS overlay: %s has %llu bytes, its manifest says %llu", file, (unsigned long long)id.size, size);
        }
        sha_range(f, 0, size, hex, 0);
        plat_file_close(f);
        if (strcmp(hex, sha)) BAD("RomFS overlay: %s has SHA-256 %s, its manifest says %s", file, hex, sha);
        hashed += e.size + size;
        if (noverlay == overlay_cap) {
            overlay_cap = overlay_cap ? overlay_cap * 2 : 16;
            overlays = realloc(overlays, overlay_cap * sizeof *overlays);
            need(overlays != NULL, "overlay table");
        }
        char *copy = strdup(file), *rel = strdup(name);
        need(copy && rel, "overlay path copy");
        overlays[noverlay++] = (Overlay){i, size, copy, rel, id, prio, 0};
        if (!prio) patches_loaded = 1;
        if (noverlay - first <= 16)
            rt_log("RomFS overlay: %s (%llu bytes) %s", name, size, add ? "is added" : "replaces the original");
    }
    fclose(m);
    if (noverlay - first > 16) rt_log("RomFS overlay: %zu files from %s (%zu not listed)", noverlay - first, dir,
                                      noverlay - first - 16);
    if (noverlay > first) rt_log("RomFS overlay: %s checked, %zu files, %.1f MiB hashed in %.2f s", dir,
                                 noverlay - first, hashed / 1048576.0, now_s() - t0);
    return 1;
bad:
    fclose(m);
    for (size_t k = first; k < noverlay; k++) {
        free((void *)overlays[k].path);
        free((void *)overlays[k].name);
    }
    noverlay = first;
    return 0;
#undef BAD
}

// ---- player mods (mods.c): every file under an enabled mod's RomFS root, by identity (no hash: a texture pack is
// gigabytes and the player changes it at will; romfs_open still stops on a file that changed after the start)
static uint32_t *folded;   // RomFS indices sorted by name ignoring ASCII case; built on the first mod path that misses

static int folded_order(const void *a, const void *b) {
    return strcasecmp(entry_name(entry(*(const uint32_t *)a)), entry_name(entry(*(const uint32_t *)b)));
}

// The RomFS index of name, or of the RomFS path equal to it but for case (a mod made on Windows, where an emulator's
// folder lookup ignores case); UINT64_MAX for a path the RomFS lacks.
static uint64_t index_any_case(const char *name) {
    uint64_t i = lower(name);
    if (i < file_count && !strcmp(entry_name(entry(i)), name)) return i;
    if (!folded) {
        folded = malloc(file_count * sizeof *folded);
        need(folded != NULL, "case-folded index");
        for (uint64_t k = 0; k < file_count; k++) folded[k] = (uint32_t)k;
        qsort(folded, file_count, sizeof *folded, folded_order);
    }
    uint64_t lo = 0, hi = file_count;
    while (lo < hi) {
        uint64_t m = lo + (hi - lo) / 2;
        if (strcasecmp(entry_name(entry(folded[m])), name) < 0) lo = m + 1;
        else hi = m;
    }
    return lo < file_count && !strcasecmp(entry_name(entry(folded[lo])), name) ? folded[lo] : UINT64_MAX;
}

static void overlay_push(Overlay o) {
    if (noverlay == overlay_cap) {
        overlay_cap = overlay_cap ? overlay_cap * 2 : 16;
        overlays = realloc(overlays, overlay_cap * sizeof *overlays);
        need(overlays != NULL, "overlay table");
    }
    overlays[noverlay++] = o;
}

// 1 when path names a file of one game version only (Product.<n> in a name: the resource size table, the actor
// database and other version-bound files); the RomFS lacking it means it is another version's.
static int other_version(const char *path) {
    for (const char *p = strstr(path, ".Product."); p; p = strstr(p + 1, ".Product.")) {
        const char *d = p + strlen(".Product.");
        if (*d >= '0' && *d <= '9') return 1;
    }
    return 0;
}

// ---- the resource size table's inputs: the tables the overlays carry, before overlay_contest keeps one of them
static char restbl_rel[256];   // the RomFS path of the game's table, "" when it has none
typedef struct {
    char *file;
    int mod, other;   // mod: ModInfo index + 1, 0 for the port's folders; other: made for another game version
} Shipped;
static Shipped *shipped;
static size_t nshipped;

static int ends_with(const char *s, const char *tail) {
    size_t n = strlen(s), k = strlen(tail);
    return n >= k && !strcmp(s + n - k, tail);
}

// 1 when rel is a resource size table of any game version.
static int is_restbl(const char *rel) {
    return !strncmp(rel, "System/Resource/ResourceSizeTable.", 34) && ends_with(rel, ".rsizetable.zs");
}

static void shipped_add(const char *file, int mod, int other) {
    Shipped *grown = realloc(shipped, (nshipped + 1) * sizeof *shipped);
    char *copy = strdup(file);
    need(grown && copy, "resource size table list");
    shipped = grown;
    shipped[nshipped++] = (Shipped){copy, mod, other};
}

// The game's table: the one System/Resource/ResourceSizeTable.*.rsizetable.zs of the RomFS.
static void restbl_find(void) {
    for (uint64_t i = lower("System/Resource/ResourceSizeTable."); i < file_count; i++) {
        const char *n = entry_name(entry(i));
        if (strncmp(n, "System/Resource/ResourceSizeTable.", 34)) break;
        if (is_restbl(n) && strlen(n) < sizeof restbl_rel) {
            snprintf(restbl_rel, sizeof restbl_rel, "%s", n);
            return;
        }
    }
}

// The tables the overlays carry for this game version, every one of them (overlay_contest keeps one).
static void restbl_collect(void) {
    for (size_t k = 0; k < noverlay; k++)
        if (restbl_rel[0] && !strcmp(overlays[k].name, restbl_rel)) shipped_add(overlays[k].path, overlays[k].mod, 0);
}

typedef struct { int k, prio; const char *dir, *rel; } ModWalk;

static void mod_walk(int k, int prio, const char *dir, const char *rel);

// One entry of a mod's RomFS folder (mod_walk).
static int mod_walk_cb(const char *leaf, int kind, uint64_t size, void *ctx) {
    (void)size;
    const ModWalk *w = ctx;
    ModInfo *m = mods_at(w->k);
    char file[4200], path[4200];
    if (snprintf(file, sizeof file, "%s/%s", w->dir, leaf) >= (int)sizeof file ||
        snprintf(path, sizeof path, "%s%s%s", w->rel, *w->rel ? "/" : "", leaf) >= (int)sizeof path) {
        rt_log("mods: %s: path too long under %s; left out", m->name, w->dir);
        return 1;
    }
    if (kind == PLAT_FS_DIR) {
        mod_walk(w->k, w->prio, file, path);
        return 1;
    }
    if (kind != PLAT_FS_FILE) return 1;
    if (!romfs_path_valid_relative(path)) {
        rt_log("mods: %s: %s is not a RomFS path; left out", m->name, path);
        return 1;
    }
    uint64_t i = index_any_case(path);
    if (i == UINT64_MAX && other_version(path)) {
        if (is_restbl(path)) shipped_add(file, w->k + 1, 1);   // its entries for this mod's files still apply
        m->other_version++;
        return 1;
    }
    PlatFile *f = open_read(file);
    if (!f) {
        rt_log("mods: %s: cannot open %s; left out", m->name, file);
        return 1;
    }
    Identity id = identity_of(f);
    plat_file_close(f);
    char *copy = strdup(file), *rp = strdup(i == UINT64_MAX ? path : entry_name(entry(i)));
    need(copy && rp, "overlay path copy");
    overlay_push((Overlay){i == UINT64_MAX ? ADDED : i, id.size, copy, rp, id, w->prio, w->k + 1});
    return 1;
}

// The files under dir (a mod's RomFS root; rel: the RomFS path of dir, "" at the root) as overlays of mod k.
static void mod_walk(int k, int prio, const char *dir, const char *rel) {
    ModWalk w = {k, prio, dir, rel};
    plat_fs_list(dir, mod_walk_cb, &w);
}

// What overlays compete on: the RomFS index, an added file's path; the higher prio first.
static int contest_order(const void *a, const void *b) {
    const Overlay *x = a, *y = b;
    if (x->index != y->index) return (x->index > y->index) - (x->index < y->index);
    int c = x->index == ADDED ? strcmp(x->name, y->name) : 0;
    return c ? c : y->prio - x->prio;
}

static const char *owner_name(const Overlay *o) {
    return o->mod ? mods_at(o->mod - 1)->name : o->prio ? "romfs_mods" : "the port's patches";
}

// One overlay per path: the highest prio serves it; the others are dropped and counted on their mod.
static void overlay_contest(void) {
    if (!noverlay) return;
    qsort(overlays, noverlay, sizeof *overlays, contest_order);
    size_t kept = 0;
    for (size_t k = 0; k < noverlay; k++) {
        Overlay *o = &overlays[k], *w = kept ? &overlays[kept - 1] : NULL;
        if (w && w->index == o->index && (o->index != ADDED || !strcmp(w->name, o->name))) {
            if (o->mod) {
                ModInfo *m = mods_at(o->mod - 1);
                m->overridden++;
                if (!m->overridden_by[0]) snprintf(m->overridden_by, sizeof m->overridden_by, "%s", owner_name(w));
            } else if (w->mod) {
                rt_log("RomFS overlay: %s from mod %s replaces the one of %s", o->name, owner_name(w), owner_name(o));
            }
            free(o->path);
            free(o->name);
            continue;
        }
        if (o->mod) {
            if (o->index == ADDED) mods_at(o->mod - 1)->added++;
            else mods_at(o->mod - 1)->replaced++;
        }
        overlays[kept++] = *o;
    }
    noverlay = kept;
}

static void mods_overlay_init(void) {
    mods_scan();
    int n = mods_count();
    for (int k = 0; k < n; k++) {
        const ModInfo *m = mods_at(k);
        // a mod with ExeFS patches waits for the rebuild that applies them: half of a mod is worse than none
        if (m->enabled && (m->kind & MOD_ROMFS) && m->problem == MOD_OK) mod_walk(k, 2 + (n - k), m->romfs, "");
    }
}

static void mods_overlay_log(void) {
    for (int k = 0; k < mods_count(); k++) {
        const ModInfo *m = mods_at(k);
        if (!m->enabled || !(m->kind & MOD_ROMFS) || m->problem != MOD_OK) continue;
        rt_log("mods: %s: %u files served (%u replaced, %u added), %u overridden%s%s, %u for another game version",
               m->name, m->replaced + m->added, m->replaced, m->added, m->overridden, m->overridden ? " by " : "",
               m->overridden_by, m->other_version);
    }
}

static int overlay_order(const void *a, const void *b) {
    uint64_t x = ((const Overlay *)a)->index, y = ((const Overlay *)b)->index;
    return (x > y) - (x < y);
}

static int added_order(const void *a, const void *b) {
    return strcmp((*(Overlay *const *)a)->name, (*(Overlay *const *)b)->name);
}

// Number the added files in path order after the RomFS's own (file_count + k); overlay_contest left one per path.
static void added_init(void) {
    Overlay **list = NULL;
    for (size_t k = 0; k < noverlay; k++)
        if (overlays[k].index == ADDED) {
            list = realloc(list, (nadded + 1) * sizeof *list);
            need(list != NULL, "added files");
            list[nadded++] = &overlays[k];
        }
    if (!nadded) return;
    qsort(list, nadded, sizeof *list, added_order);
    added = malloc(nadded * sizeof *added);
    need(added != NULL, "added files");
    for (size_t k = 0; k < nadded; k++) {
        list[k]->index = file_count + k;
        added[k] = list[k]->name;
    }
    free(list);
    rt_log("RomFS overlay: %zu files added to the RomFS", nadded);
}

// RomFS file `rel` as the game would read it: from an overlay that replaces it (*base), else from the image. malloc'd;
// NULL when it is missing, unreadable or 16 MiB or more (logged with `keeps`, what the game does without it).
static void *read_served(const char *rel, Overlay **base, size_t *n, const char *keeps) {
    uint64_t i = lower(rel);
    *base = NULL;
    if (i >= file_count || strcmp(entry_name(entry(i)), rel)) {
        rt_log("RomFS: %s missing; %s", rel, keeps);
        return NULL;
    }
    for (size_t k = 0; k < noverlay; k++)
        if (overlays[k].index == i) *base = &overlays[k];
    PlatFile *f = *base ? open_read((*base)->path) : image;
    uint64_t off = *base ? 0 : entry(i).offset, size = *base ? (*base)->size : entry(i).size;
    void *in = size && size < (16u << 20) && f ? malloc((size_t)size) : NULL;
    int ok = in && read_at(f, in, (uint32_t)size, off) == (int64_t)size;
    if (*base && f) plat_file_close(f);
    if (!ok) {
        free(in);
        rt_log("RomFS: cannot read %s; %s", rel, keeps);
        return NULL;
    }
    *n = (size_t)size;
    return in;
}

// Serves out[0..out_n) (malloc'd, freed here) in place of RomFS file `rel` and of an overlay that replaces it (base):
// written to <profile>/<sub>/<file name of rel>. `what` names the change in the log. 0 when it cannot be written.
static int serve_generated(const char *rel, Overlay *base, void *out, size_t out_n, const char *sub, const char *what,
                           const char *keeps) {
    char dir[2100], file[2200];
    const char *leaf = strrchr(rel, '/') ? strrchr(rel, '/') + 1 : rel;
    need(snprintf(dir, sizeof dir, "%s/%s", rt_config()->profile, sub) < (int)sizeof dir, "generated overlay path");
    need(snprintf(file, sizeof file, "%s/%s", dir, leaf) < (int)sizeof file, "generated overlay path");
    plat_file_mkdir_p(dir);
    int ok = plat_file_write(file, out, out_n);
    free(out);
    PlatFile *r = ok ? open_read(file) : NULL;
    if (!r) {
        rt_log("RomFS: cannot write %s; %s", file, keeps);
        return 0;
    }
    Identity id = identity_of(r);
    plat_file_close(r);
    char *copy = strdup(file);
    need(copy != NULL, "overlay path copy");
    if (base) {
        free(base->path);
        base->path = copy;
        base->size = out_n;
        base->id = id;
    } else {
        if (noverlay == overlay_cap) {
            overlay_cap = overlay_cap ? overlay_cap * 2 : 16;
            overlays = realloc(overlays, overlay_cap * sizeof *overlays);
            need(overlays != NULL, "overlay table");
        }
        char *name = strdup(rel);
        need(name != NULL, "overlay path copy");
        overlays[noverlay++] = (Overlay){lower(rel), out_n, copy, name, id};
    }
    rt_log("RomFS overlay: %s (%zu bytes, %s) replaces %s", file, out_n, what, base ? "the mod's file" : "the original");
    return 1;
}

// ---- the resource size table (restbl.c)

// The decompressed size of a file's bytes as the game unpacks them: the content size its zstd frame records (.zs;
// the game's files and zstd's own tools write it), four times the bytes for a frame without one, else its size.
static uint64_t content_size(PlatFile *f, uint64_t off, uint64_t size, const char *rel) {
    if (!ends_with(rel, ".zs") || !f) return size;
    uint8_t head[18];
    uint32_t n = size < sizeof head ? (uint32_t)size : (uint32_t)sizeof head;
    if (read_at(f, head, n, off) != n) return size;
    unsigned long long c = ZSTD_getFrameContentSize(head, n);
    return c == ZSTD_CONTENTSIZE_ERROR ? size : c == ZSTD_CONTENTSIZE_UNKNOWN ? size * 4 : c;
}

// A table from its zstd bytes; NULL when they are not one.
static Restbl *restbl_unpack(const void *z, size_t n) {
    unsigned long long raw = ZSTD_getFrameContentSize(z, n);
    if (raw == ZSTD_CONTENTSIZE_UNKNOWN || raw == ZSTD_CONTENTSIZE_ERROR || raw > (64u << 20)) return NULL;
    uint8_t *d = malloc((size_t)raw ? (size_t)raw : 1);
    size_t got = d ? ZSTD_decompress(d, (size_t)raw, z, n) : 0;
    Restbl *t = d && !ZSTD_isError(got) && got == raw ? restbl_parse(d, (size_t)raw) : NULL;
    free(d);
    return t;
}

// With a player mod (or the TOTK_ROMFS_MODS manifest) serving files: the game's table with every shipped table of this
// version merged in (an entry larger than the game's wins), the entries a mod's table for another version gives for
// that mod's files, and each served mod file's entry raised to what its size needs (restbl_grow for a file the game
// has, restbl_estimate for an added one). Never lowers the game's entries.
static void restbl_overlay(void) {
    int player = 0;
    for (size_t k = 0; k < noverlay; k++) player |= overlays[k].mod || overlays[k].prio == 1;
    if (!player || !restbl_rel[0]) return;
    static const char keeps[] = "mod files that grew may fail to load";
    double t0 = now_s();
    uint64_t gi = lower(restbl_rel);
    Entry ge = entry(gi);
    void *gz = ge.size < (64u << 20) ? malloc((size_t)ge.size) : NULL;
    Restbl *base = NULL, *out = NULL;
    if (gz && read_at(image, gz, (uint32_t)ge.size, ge.offset) == (int64_t)ge.size) {
        base = restbl_unpack(gz, (size_t)ge.size);
        out = restbl_unpack(gz, (size_t)ge.size);
    }
    free(gz);
    if (!base || !out) {
        rt_log("RomFS: cannot read the resource size table %s; %s", restbl_rel, keeps);
        restbl_free(base);
        restbl_free(out);
        return;
    }
    int tables = 0, merged = 0, raised = 0, estimated = 0;
    for (size_t s = 0; s < nshipped; s++) {
        uint64_t n = 0;
        void *z = plat_file_read(shipped[s].file, &n);
        Restbl *t = z ? restbl_unpack(z, (size_t)n) : NULL;
        free(z);
        if (!t) {
            rt_log("RomFS: %s is not a resource size table; left out", shipped[s].file);
            continue;
        }
        tables++;
        if (!shipped[s].other) {
            merged += restbl_merge(out, base, t);
        } else {   // another version's table: only the entries of the files its mod serves
            for (size_t k = 0; k < noverlay; k++) {
                char key[4200];
                const char *kp = restbl_key(overlays[k].name, key, sizeof key);
                if (overlays[k].mod == shipped[s].mod) merged += restbl_take(out, t, &kp, 1);
            }
        }
        restbl_free(t);
    }
    Overlay *served = NULL;
    for (size_t k = 0; k < noverlay; k++) {
        Overlay *o = &overlays[k];
        if (o->index == gi) served = o;
        if ((!o->mod && o->prio != 1) || o->index == gi) continue;
        char key[4200];
        restbl_key(o->name, key, sizeof key);
        int mc = ends_with(o->name, ".mc");
        PlatFile *f = open_read(o->path);
        uint64_t now = mc ? o->size : content_size(f, 0, o->size, o->name);
        if (f) plat_file_close(f);
        uint32_t entry_size, want;
        if (o->index != ADDED) {
            if (!restbl_get(base, key, &entry_size)) continue;   // the game sizes it without the table (streamed)
            Entry e = entry(o->index);
            uint64_t was = mc ? e.size : content_size(image, e.offset, e.size, o->name);
            want = restbl_grow(key, entry_size, was, now, mc);
        } else {
            want = restbl_estimate(key, mc ? now * 8 : now);   // a mesh codec file unpacks to several times its size
            estimated += want != 0;
        }
        raised += want && restbl_raise(out, key, want);
    }
    restbl_free(base);
    if (!merged && !raised && !served) {
        restbl_free(out);
        rt_log("RomFS: resource size table: the mods' files fit the game's entries (%.0f ms)", (now_s() - t0) * 1000);
        return;
    }
    size_t rn = 0, zn = 0;
    uint8_t *raw = restbl_write(out, &rn);
    restbl_free(out);
    size_t cap = raw ? ZSTD_compressBound(rn) : 0;
    void *z = raw ? malloc(cap) : NULL;
    if (z) zn = ZSTD_compress(z, cap, raw, rn, 3);
    free(raw);
    if (!z || ZSTD_isError(zn)) {
        free(z);
        rt_log("RomFS: cannot pack the merged resource size table; %s", keeps);
        return;
    }
    if (serve_generated(restbl_rel, served, z, zn, "mods", "merged resource sizes", keeps))
        rt_log("RomFS: resource size table: %d tables merged (%d entries), %d entries raised for mod files (%d "
               "added files estimated), %.0f ms", tables, merged, raised, estimated, (now_s() - t0) * 1000);
}

// The HeapDef table a bigger game resolution needs (resolution.c): its texture heap and that heap's parent grow. The
// base is an overlay that replaces the table (a mod's heap sizes stay), else the RomFS original; the grown table is
// written to <profile>/resolution/ and served in the base's place.
#define HEAPDEF "System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs"
static void heapdef_overlay(void) {
    if (!res_heap_extra_kib()) return;
    static const char keeps[] = "the game keeps its own resolution";
    Overlay *base;
    size_t n;
    void *in = read_served(HEAPDEF, &base, &n, keeps), *out = NULL;
    size_t out_n = 0;
    int ok = in && res_heapdef_build(in, n, &out, &out_n);
    free(in);
    if (ok && serve_generated(HEAPDEF, base, out, out_n, "resolution", "grown heaps", keeps)) res_heapdef_served();
}

// The player camera's pack with its stick speeds multiplied by TOTK_STICK_SENS (camera_speed.c). The base is an
// overlay that replaces the pack (a mod's camera stays), else the RomFS original; the dictionary is the RomFS's own.
#define CAMPACK "Pack/Actor/PlayerCamera.pack.zs"
static void camera_overlay(void) {
    if (cam_speed_wanted() == 1.0f) return;
    static const char keeps[] = "the camera keeps its own speed";
    Overlay *base, *dic_base;
    size_t n, dn;
    void *in = read_served(CAMPACK, &base, &n, keeps), *out = NULL;
    void *dic = in ? read_served("Pack/ZsDic.pack.zs", &dic_base, &dn, keeps) : NULL;
    size_t out_n = 0;
    int ok = in && dic && cam_pack_build(in, n, dic, dn, &out, &out_n);
    free(in);
    free(dic);
    if (ok && serve_generated(CAMPACK, base, out, out_n, "camera", "camera speed", keeps)) cam_pack_served();
}

// The button prompts' folders (input_prompts_folder) and whether each had a manifest next to romfs-index.bin at start;
// the one the settings asked for at start, the one overlay_init loaded (NULL: the game's own prompts) and why the first
// did not load (ROMFS_PROMPTS_*). The menu compares them with the settings in effect (ui/menu.cpp prompts_note).
static struct { const char *name; int ready; } prompts_folders[2];
static const char *prompts_wanted, *prompts_loaded;
static int prompts_off;

const char *romfs_prompts(void) { return prompts_loaded; }

int romfs_prompts_ready(const char *folder) {
    for (int i = 0; i < 2; i++)
        if (folder && prompts_folders[i].name && !strcmp(prompts_folders[i].name, folder)) return prompts_folders[i].ready;
    return 0;
}

int romfs_prompts_off(const char *folder) {
    return folder && prompts_wanted && !strcmp(folder, prompts_wanted) ? prompts_off : ROMFS_PROMPTS_OK;
}

// The first RomFS file that the overlays [a, b) and [c, d) both replace, or NULL.
static const char *overlay_clash(size_t a, size_t b, size_t c, size_t d) {
    for (size_t i = a; i < b; i++)
        for (size_t j = c; j < d; j++)
            if (overlays[i].index != ADDED && overlays[i].index == overlays[j].index &&
                strcmp(overlays[i].name, restbl_rel))   // restbl_overlay merges both tables
                return overlays[i].name;
    return NULL;
}

// Drops the overlays [a, b), the ones after them moving down.
static void overlay_drop(size_t a, size_t b) {
    for (size_t k = a; k < b; k++) {
        free((void *)overlays[k].path);
        free((void *)overlays[k].name);
    }
    memmove(overlays + a, overlays + b, (noverlay - b) * sizeof *overlays);
    noverlay -= b - a;
}

static void overlay_init(void) {
    // TOTK_ROMFS_PATCHES unset: romfs-patches next to romfs-index.bin if it has a manifest (tools/make_romfs_patches.py).
    // A game resolution that is not 16:9 takes romfs-patches-<w>x<h> instead when it is there: the same overlay with
    // the UI laid out for that shape (make_romfs_patches.py --aspect), when the game version has the camera aspect patch
    // the layout goes with (res_wants_wide_overlay).
    // With prompts = xbox, the Xbox prompts' folder for the pad layout in its place (input_prompts_folder): the same
    // patches with Xbox button icons (make_romfs_patches.py --prompts). The game reads the archives once at boot, so
    // the prompts stay until restart even though the layout itself changes live. Each folder replaces romfs-patches as
    // a whole and the setup builds no folder of another shape with Xbox prompts, so a game resolution that is not 16:9
    // keeps the Switch prompts. So does a RomFS mod (the TOTK_ROMFS_MODS manifest or a player mod) that replaces one of
    // the prompts' fonts: romfs-patches loads in their place, so the mod's file is not served beside prompts files made
    // for the game's own. A mod's resource size table is no clash: restbl_overlay merges it with the prompts' one.
    char def[2100], path[2100];
    const char *patches = cfg_get("TOTK_ROMFS_PATCHES"), *mods = cfg_get("TOTK_ROMFS_MODS");
    int wide = 0;
    unsigned out[2];
    res_wanted(out);
    if (!patches && res_wants_wide_overlay(out)) {
        if (snprintf(def, sizeof def, "%s/romfs-patches-%ux%u/manifest.tsv", index_dir, out[0], out[1]) < (int)sizeof def &&
            plat_file_exists(def)) {
            def[strlen(def) - strlen("/manifest.tsv")] = 0;
            patches = def, wide = 1;
        } else {
            rt_log("RomFS overlay: no romfs-patches-%ux%u for this game resolution, the UI keeps its 16:9 layout "
                   "(tools/make_romfs_patches.py --aspect %ux%u)", out[0], out[1], out[0], out[1]);
        }
    }
    static const char *const layouts[2] = {"nintendo", "xbox"};   // one of each folder
    for (int i = 0; i < 2; i++) {
        prompts_folders[i].name = input_prompts_folder(PROMPTS_XBOX, layouts[i]);
        prompts_folders[i].ready =
            snprintf(path, sizeof path, "%s/%s/manifest.tsv", index_dir, prompts_folders[i].name) < (int)sizeof path &&
            plat_file_exists(path);
    }
    const char *layout = cfg_value("TOTK_PAD_LAYOUT");
    prompts_wanted = input_prompts_folder(input_prompts_pick(cfg_get("TOTK_PROMPTS"), layout), layout);
    prompts_off = !prompts_wanted                         ? ROMFS_PROMPTS_OK
                  : wide                                  ? ROMFS_PROMPTS_SHAPE
                  : patches                               ? ROMFS_PROMPTS_PATCHES
                  : !romfs_prompts_ready(prompts_wanted)  ? ROMFS_PROMPTS_MISSING
                                                          : ROMFS_PROMPTS_OK;
    restbl_find();
    size_t first = noverlay;
    if (prompts_wanted && prompts_off == ROMFS_PROMPTS_OK) {
        snprintf(path, sizeof path, "%s/%s", index_dir, prompts_wanted);   // fits: its manifest path did
        if (overlay_load(path, 1, 0)) {
            prompts_loaded = prompts_wanted;
            rt_log("RomFS overlay: Xbox button prompts (%s)", path);
        } else {
            prompts_off = ROMFS_PROMPTS_FAILED;
            rt_log("RomFS overlay: %s failed its check (run the setup again); romfs-patches loads instead", path);
        }
    } else if (prompts_wanted) {
        rt_log("RomFS overlay: prompts = xbox, but %s; Switch prompts stay",
               prompts_off == ROMFS_PROMPTS_SHAPE     ? "the game resolution takes its own UI layout"
               : prompts_off == ROMFS_PROMPTS_PATCHES ? "TOTK_ROMFS_PATCHES is set"
                                                      : "their folder has no manifest.tsv (run the setup again)");
    }
    size_t prompts_end = noverlay;
    char plain[2100];   // romfs-patches, when it has a manifest
    int plain_ok = snprintf(plain, sizeof plain, "%s/romfs-patches/manifest.tsv", index_dir) < (int)sizeof plain &&
                   plat_file_exists(plain);
    if (plain_ok) plain[strlen(plain) - strlen("/manifest.tsv")] = 0;
    if (!patches && !prompts_loaded && plain_ok) patches = plain;
    if (patches && *patches && strcmp(patches, "0")) overlay_load(patches, 0, 0);
    size_t mods_first = noverlay;
    if (mods && *mods && strcmp(mods, "0")) overlay_load(mods, 0, 1);
    mods_overlay_init();
    const char *clash = prompts_loaded ? overlay_clash(first, prompts_end, mods_first, noverlay) : NULL;
    if (clash) {
        rt_log("RomFS overlay: a RomFS mod replaces %s, which the Xbox prompts change too; Switch prompts stay", clash);
        overlay_drop(first, prompts_end);
        prompts_loaded = NULL;
        prompts_off = ROMFS_PROMPTS_MODS;
        if (plain_ok) overlay_load(plain, 0, 0);
    }
    restbl_collect();
    overlay_contest();
    mods_overlay_log();
    restbl_overlay();
    heapdef_overlay();
    camera_overlay();
    added_init();
    if (noverlay) qsort(overlays, noverlay, sizeof *overlays, overlay_order);
    for (size_t k = 1; k < noverlay; k++)   // overlay_contest left one per file; the generated ones replace in place
        if (overlays[k].index == overlays[k - 1].index)
            rt_fatal("RomFS overlay: %s and %s replace the same file %s", overlays[k - 1].path, overlays[k].path,
                     overlays[k].name);
}

// ---- start
// romfs-index.bin next to totk.exe, or TOTK_ROMFS_INDEX.
static void index_path(char *out, size_t cap) {
    const char *e = cfg_get("TOTK_ROMFS_INDEX");
    if (e && *e) {
        need(snprintf(out, cap, "%s", e) < (int)cap, "index path");
        return;
    }
    need(plat_exe_path(out, cap), "executable path");
    char *sep = plat_last_sep(out);
    size_t k = sep ? (size_t)(sep - out) + 1 : 0;
    need(k + 16 < cap, "executable path");
    strcpy(out + k, "romfs-index.bin");
}

static void load_index(void) {
    char path[2100];
    index_path(path, sizeof path);
    snprintf(index_dir, sizeof index_dir, "%s", path);
    char *sep = plat_last_sep(index_dir);
    if (sep) *sep = 0;
    else strcpy(index_dir, ".");
    uint64_t sz = 0;
    index_bytes = plat_file_read(path, &sz);
    if (!index_bytes) rt_fatal("RomFS index %s missing (tools/build_game.py writes it next to totk.exe)", path);
    need(sz >= 24 && sz < (128 << 20), "index size");
    index_size = (size_t)sz;
    uint8_t d[32];
    char hex[65];
    rt_sha256(index_bytes, index_size, d);
    hex32(d, hex);
    if (strcmp(hex, ROMFS_INDEX_SHA256))
        rt_fatal("RomFS index %s has SHA-256 %s, this build expects %s (rebuild with tools/build_game.py)", path, hex,
                 ROMFS_INDEX_SHA256);
    need(!memcmp(index_bytes, "TOTKRFS1", 8), "index format");
    memcpy(&file_count, index_bytes + 8, 8);
    memcpy(&strings_offset, index_bytes + 16, 8);
    need(file_count == ROMFS_FILE_COUNT && file_count < 1000000 && strings_offset == 24 + file_count * 24 &&
         strings_offset < index_size, "index layout");
    const char *previous = NULL;
    for (uint64_t i = 0; i < file_count; i++) {
        Entry e = entry(i);
        need(e.offset <= ROMFS_IMAGE_SIZE && e.size <= ROMFS_IMAGE_SIZE - e.offset, "file bounds");
        need(e.name < index_size - strings_offset && e.len > 1 && e.len <= index_size - strings_offset - e.name,
             "name bounds");
        const char *name = entry_name(e);
        need(name[e.len - 1] == 0 && strlen(name) == e.len - 1 && (!previous || strcmp(previous, name) < 0),
             "sorted paths");
        previous = name;
    }
    rt_log("RomFS index %s: SHA-256 ok, %llu files, %zu bytes", path, (unsigned long long)file_count, index_size);
}

static void init_once(void *param) {
    (void)param;
#ifdef ROMFS_MISSING
    rt_fatal("this build has no RomFS: tools/build_game.py found no index.json in its --romfs directory");
#else
    double t0 = now_s();
    load_index();
    const char *dir = rt_config()->romfs;
    need(snprintf(image_file, sizeof image_file, "%s/romfs.bin", dir) < (int)sizeof image_file, "image path");
    image = open_read(image_file);
    if (!image) rt_fatal("RomFS image %s missing; put romfs.bin into the romfs folder next to totk or set TOTK_ROMFS",
                         image_file);
    PlatFileId sz;
    need(plat_file_id(image, &sz), "image size");
    if (sz.size != ROMFS_IMAGE_SIZE)
        rt_fatal("RomFS image %s has %llu bytes, the index was built for %llu", image_file,
                 (unsigned long long)sz.size, (unsigned long long)ROMFS_IMAGE_SIZE);
    rt_log("RomFS image %s: size ok (%llu bytes)", image_file, (unsigned long long)ROMFS_IMAGE_SIZE);
    verify_image(index_dir);
    const char *mm = cfg_get("TOTK_ROMFS_MMAP");
    if (mm && strcmp(mm, "0")) {
        const void *v = plat_file_map(image);
        if (v) {
            image_map = v;
            rt_log("RomFS: mapped read path enabled");
        } else {
            rt_log("RomFS: mapping failed; using positional reads");
        }
    }
    overlay_init();
    rt_log("RomFS ready: %llu files, %zu overlays, %zu added, %.2f s", (unsigned long long)file_count, noverlay,
           nadded, now_s() - t0);
#endif
    return;
}

void fs_init(void) {
    static PlatOnce once = PLAT_ONCE_INIT;
    plat_once(&once, init_once, NULL);
}

// ---- the RomFS interface (fs.h)
void fs_trace(const char *what, const char *path) {
    static double t0;
    if (!(rt_trace & RT_TRACE_FS)) return;
    if (!t0) t0 = now_s();
    rt_log("fs %8.3f %s %s", now_s() - t0, what, path ? path : "(null)");
}

uint64_t romfs_file_count(void) { return file_count; }

const char *romfs_entry(uint64_t i, uint64_t *offset, uint64_t *size) {
    if (i >= file_count) return NULL;
    Entry e = entry(i);
    if (offset) *offset = e.offset;
    if (size) *size = e.size;
    return entry_name(e);
}

const char *romfs_image_path(void) { return image_file; }

size_t romfs_overlay_count(void) { return noverlay; }

int romfs_patches_loaded(void) { return patches_loaded; }

const char *romfs_overlay(size_t k, const char **file, uint64_t *size, int *is_added) {
    if (k >= noverlay) return NULL;
    *file = overlays[k].path;
    *size = overlays[k].size;
    *is_added = overlays[k].index >= file_count;
    return overlays[k].name;
}

int romfs_owns(const char *path) {
    size_t n = strlen(mount_name);
    return n && path && !strncmp(path, mount_name, n) && path[n] == ':';
}

int romfs_handle(uint64_t h) { return h > ROMFS_HANDLE_BASE && h < ROMFS_HANDLE_BASE + NH; }

uint32_t romfs_mount(const char *name) {
    fs_init();
    if (!name || !romfs_path_valid_relative(name) || strlen(name) >= sizeof mount_name || strchr(name, '/') ||
        strchr(name, ':'))
        return FS_R_INVALID;
    plat_mutex_lock(&lock);
    if (*mount_name && strcmp(name, mount_name)) rt_fatal("second RomFS mount %s (%s is mounted)", name, mount_name);
    strcpy(mount_name, name);
    plat_mutex_unlock(&lock);
    rt_log("fs mount %s: -> RomFS", name);
    return 0;
}

uint32_t romfs_open(uint64_t *out, const char *path, int mode) {
    fs_trace("rom", path);
    if (!out || mode != FS_OPEN_READ) return FS_R_UNSUPPORTED_OPEN;
    const char *p = romfs_path_relative(mount_name, path);
    if (!p) return FS_R_PATH_NOT_FOUND;
    uint64_t i = file_index(p);
    if (i == UINT64_MAX) return FS_R_PATH_NOT_FOUND;
    uint32_t ov = overlay_slot(i);
    PlatFile *f = NULL;
    if (ov) {
        const Overlay *o = &overlays[ov - 1];
        f = open_read(o->path);
        Identity id;
        if (f) id = identity_of(f);
        if (!f || memcmp(&id, &o->id, sizeof id))
            rt_fatal("RomFS overlay %s is missing or changed since the start check (restart the game)", o->path);
    }
    plat_mutex_lock(&lock);
    for (unsigned j = 1; j < NH; j++)
        if (!handles[j]) {
            handles[j] = i + 1;
            handle_ovl[j] = ov;
            handle_file[j] = f;
            *out = ROMFS_HANDLE_BASE + j;
            plat_mutex_unlock(&lock);
            return 0;
        }
    plat_mutex_unlock(&lock);
    rt_fatal("RomFS file handle table full (%d)", NH);
}

// The handle's index row (an added file's is empty: its bytes are all in its overlay), and where its bytes come from:
// the overlay file, or the original's range of the image.
static int handle_source(uint64_t h, PlatFile **f, uint64_t *base, uint64_t *size) {
    if (!romfs_handle(h)) return 0;
    uint64_t j = h - ROMFS_HANDLE_BASE;
    plat_mutex_lock_shared(&lock);
    uint64_t i = handles[j];
    uint32_t ov = handle_ovl[j];
    PlatFile *of = handle_file[j];
    plat_mutex_unlock_shared(&lock);
    if (!i) return 0;
    if (ov) {
        *f = of;
        *base = 0;
        *size = overlays[ov - 1].size;
    } else {
        Entry e = entry(i - 1);
        *f = image;
        *base = e.offset;
        *size = e.size;
    }
    return 1;
}

uint32_t romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    PlatFile *f;
    uint64_t base, fsize;
    if (!out || offset < 0 || !handle_source(h, &f, &base, &fsize)) return FS_R_INVALID;
    if ((uint64_t)offset > fsize || (!buffer && size)) return FS_R_INVALID;
    uint64_t n = size < fsize - (uint64_t)offset ? size : fsize - (uint64_t)offset, done = 0;
    if (f == image && image_map) {
        memcpy(buffer, image_map + base + (uint64_t)offset, (size_t)n);
        done = n;
    }
    while (done < n) {
        uint32_t chunk = n - done > READ_CHUNK ? READ_CHUNK : (uint32_t)(n - done);
        int64_t r = read_at(f, (uint8_t *)buffer + done, chunk, base + (uint64_t)offset + done);
        if (r <= 0) rt_fatal("RomFS read failure at %#llx", (unsigned long long)(base + offset + done));
        done += (uint64_t)r;
    }
    *out = done;
    return 0;
}

uint32_t romfs_size(int64_t *out, uint64_t h) {
    PlatFile *f;
    uint64_t base, size;
    if (!out || !handle_source(h, &f, &base, &size)) return FS_R_INVALID;
    *out = (int64_t)size;
    return 0;
}

void romfs_close(uint64_t h) {
    PlatFile *f;
    uint64_t base, size;
    if (!handle_source(h, &f, &base, &size))
        rt_fatal("RomFS: close of invalid file handle %#llx", (unsigned long long)h);
    uint64_t j = h - ROMFS_HANDLE_BASE;
    plat_mutex_lock(&lock);
    PlatFile *of = handle_ovl[j] ? handle_file[j] : NULL;
    handles[j] = 0;
    handle_ovl[j] = 0;
    handle_file[j] = NULL;
    plat_mutex_unlock(&lock);
    if (of) plat_file_close(of);
}

uint32_t romfs_entry_type(int *out, const char *path) {
    if (!out) return FS_R_INVALID;
    const char *p = romfs_path_relative(mount_name, path);
    if (!p) return FS_R_PATH_NOT_FOUND;
    if (file_index(p) != UINT64_MAX) {
        *out = FS_ENTRY_FILE;
        return 0;
    }
    char dir[4096];
    if (snprintf(dir, sizeof dir, "%s/", p) >= (int)sizeof dir) return FS_R_INVALID;
    if (folder_exists(dir)) {
        *out = FS_ENTRY_DIRECTORY;
        return 0;
    }
    return FS_R_PATH_NOT_FOUND;
}

// ---- overrides
// nn::Result nn::fs::MountRom(const char *name, void *cache, size_t cache_size)
static void ov_mount_rom(Cpu *c) { ret_x(c, romfs_mount(arg_p(c, 0))); }
// nn::Result nn::fs::QueryMountRomCacheSize(size_t *out): no cache needed, the host reads the image directly
static void ov_query_cache(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    if (out) *out = 0;
    ret_x(c, out ? 0 : FS_R_INVALID);
}
// void nn::fs::InitializeForApplication(): SDK-internal; opens the index instead of the fs service session
static void ov_initialize(Cpu *c) {
    (void)c;
    fs_init();
}

HOST_OVERRIDE("_ZN2nn2fs8MountRomEPKcPvm", ov_mount_rom);
HOST_OVERRIDE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", ov_query_cache);
HOST_OVERRIDE("_ZN2nn2fs24InitializeForApplicationEv", ov_initialize);
