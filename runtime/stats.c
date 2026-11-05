// Statistics and achievements from the game's own save (the Achievements tab of the settings menu, ui/menu.cpp).
//
// The game keeps its progress in GameData flags, and progress.sav holds every flag as a table of (hash, value) pairs:
// a 32-bit word 0x01020304, a version, the end of the table at offset 8, then 8-byte pairs from offset 0x20 to there,
// the hash the MurmurHash3 x86_32 (seed 0) of the flag's name. Scalars (Int, Bool, Float, Enum as the hash of the
// value's name) are stored in the pair itself. The flags read here, each named after an actor of Pack/Actor:
//   DefeatedEnemyNum.<actor>         Int, monsters of that kind the player defeated
//   IsGet.<actor>                    Bool, the player has had that weapon, armor piece or material
//   PictureBookData.<actor>.State    Enum, the actor's compendium entry: Unopened, TakePhoto or Buy
//   PlayerStatus.MaxLife             Int, quarter hearts; PlayerStatus.MaxStamina, Float, 1000 a wheel
//   RecipeCard.Content.<dish>.IsCooked  Bool, the recipe book: the player has cooked that dish (no flag counts
//                                    how many times); the dishes are the PouchContent labels Item_Cook_*,
//                                    Item_Roast*, Item_Chilled*, Item_Boiled* and the elixir of each effect
// A flag counts only when the save holds it, so actors without one (most of Pack/Actor) drop out.
//
// The names shown are the game's own, in the game's language: <actor>_Name of ActorMsg/PictureBook.msbt (monsters)
// and ActorMsg/PouchContent.msbt (weapons, armor, materials) in Mals/<language>.Product.<version>.sarc.zs. Weapons,
// armor pieces and sets are counted by those names, so the variants of one item (a set's dyed or upgraded copies,
// a weapon's decayed one) count once; an actor without a name in the game's text is left out.
//
// One host thread reads the newest progress.sav of <profile>/save/<uid> (the slot the game wrote last) at start and
// again after each save commit that changed files (fs_host.c calls stats_save_changed); once a save is loaded it
// reads the same flags from the game's memory instead, every two seconds (live flags below). Achievements unlock
// when the statistics reach them and stay unlocked: <profile>/achievements/unlocked.txt holds one "<id> <unix time>" line
// each, and <profile>/achievements/goals.ini the goals, which the player may edit (achs_define).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zstd.h>

#include "fs.h"
#include "host.h"
#include "ov.h"
#include "platform/platform.h"
#include "stats.h"

// ---- MurmurHash3 x86_32 (public domain, Austin Appleby), the hash of the game's flag names
static uint32_t rotl(uint32_t x, int r) { return x << r | x >> (32 - r); }

static uint32_t mm3(const char *s) {
    const uint8_t *p = (const uint8_t *)s;
    size_t n = strlen(s), i = 0;
    uint32_t h = 0, k;
    for (; i + 4 <= n; i += 4) {
        memcpy(&k, p + i, 4);
        k *= 0xcc9e2d51u, k = rotl(k, 15), k *= 0x1b873593u;
        h ^= k, h = rotl(h, 13), h = h * 5 + 0xe6546b64u;
    }
    k = 0;
    switch (n & 3) {
    case 3: k ^= (uint32_t)p[i + 2] << 16; /* fall through */
    case 2: k ^= (uint32_t)p[i + 1] << 8;  /* fall through */
    case 1: k ^= p[i], k *= 0xcc9e2d51u, k = rotl(k, 15), k *= 0x1b873593u, h ^= k;
    }
    h ^= (uint32_t)n;
    h ^= h >> 16, h *= 0x85ebca6bu, h ^= h >> 13, h *= 0xc2b2ae35u, h ^= h >> 16;
    return h;
}

static uint32_t flagf(const char *fmt, const char *actor) {
    char name[256];
    snprintf(name, sizeof name, fmt, actor);
    return mm3(name);
}

// ---- SARC and zstd with the game's dictionaries (Pack/ZsDic.pack.zs)
static int sarc_find(const uint8_t *p, size_t n, const char *name, size_t *at, size_t *size) {
    if (n < 0x20 || memcmp(p, "SARC", 4)) return 0;
    uint32_t data = (uint32_t)p[12] | p[13] << 8 | p[14] << 16 | (uint32_t)p[15] << 24;
    uint16_t count = (uint16_t)(p[0x1a] | p[0x1b] << 8);
    size_t nodes = 0x20, names = nodes + 16u * count + 8;
    if (names > n) return 0;
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *e = p + nodes + 16u * i;
        uint32_t attr = (uint32_t)e[4] | e[5] << 8, lo, hi;
        memcpy(&lo, e + 8, 4), memcpy(&hi, e + 12, 4);
        size_t off = names + 4u * attr;
        if (off >= n || data + (size_t)hi > n || lo > hi) continue;
        if (!strncmp((const char *)p + off, name, n - off)) {
            *at = data + lo, *size = hi - lo;
            return 1;
        }
    }
    return 0;
}

static ZSTD_DDict *dicts[8];
static unsigned dict_ids[8], ndicts;

static void dicts_load(void) {
    if (ndicts) return;
    size_t n;
    uint8_t *pack = romfs_load("Pack/ZsDic.pack.zs", &n, "no statistics");
    unsigned long long dn = pack ? ZSTD_getFrameContentSize(pack, n) : 0;
    uint8_t *raw = dn && dn < (64u << 20) ? malloc((size_t)dn) : NULL;
    if (raw && ZSTD_decompress(raw, (size_t)dn, pack, n) == dn) {
        static const char *const names[] = {"zs.zsdic", "pack.zsdic", "bcett.byml.zsdic"};
        for (int i = 0; i < 3; i++) {
            size_t at, size;
            if (!sarc_find(raw, (size_t)dn, names[i], &at, &size)) continue;
            dicts[ndicts] = ZSTD_createDDict(raw + at, size);
            dict_ids[ndicts] = ZSTD_getDictID_fromDict(raw + at, size);
            if (dicts[ndicts]) ndicts++;
        }
    }
    free(raw);
    free(pack);
}

static uint8_t *zs_unpack(const uint8_t *in, size_t n, size_t *out_n) {
    unsigned long long size = ZSTD_getFrameContentSize(in, n);
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > (256u << 20)) return NULL;
    unsigned id = ZSTD_getDictID_fromFrame(in, n);
    ZSTD_DDict *d = NULL;
    for (unsigned i = 0; i < ndicts; i++)
        if (dict_ids[i] == id) d = dicts[i];
    if (id && !d) return NULL;
    uint8_t *out = malloc(size ? (size_t)size : 1);
    ZSTD_DCtx *ctx = ZSTD_createDCtx();
    size_t got = out && ctx ? (d ? ZSTD_decompress_usingDDict(ctx, out, (size_t)size, in, n, d)
                                 : ZSTD_decompressDCtx(ctx, out, (size_t)size, in, n))
                            : 0;
    ZSTD_freeDCtx(ctx);
    if (!out || ZSTD_isError(got) || got != size) {
        free(out);
        return NULL;
    }
    *out_n = (size_t)size;
    return out;
}

// ---- names: <actor>_Name labels of an MSBT, as UTF-8, keyed by the actor name's hash
typedef struct { uint32_t h; char *text; } Name;
static Name *names;
static size_t nnames, cap_names;

static int name_cmp(const void *a, const void *b) {
    uint32_t x = ((const Name *)a)->h, y = ((const Name *)b)->h;
    return x < y ? -1 : x > y;
}

static const char *name_of(const char *actor) {
    Name key = {mm3(actor), NULL};
    Name *n = nnames ? bsearch(&key, names, nnames, sizeof *names, name_cmp) : NULL;
    return n && n->text[0] ? n->text : NULL;
}

static size_t utf8_put(char *o, size_t cap, size_t at, uint32_t c) {
    char b[4];
    size_t k = c < 0x80 ? (b[0] = (char)c, 1)
             : c < 0x800 ? (b[0] = (char)(0xc0 | c >> 6), b[1] = (char)(0x80 | (c & 0x3f)), 2)
                         : (b[0] = (char)(0xe0 | c >> 12), b[1] = (char)(0x80 | (c >> 6 & 0x3f)),
                            b[2] = (char)(0x80 | (c & 0x3f)), 3);
    if (at + k >= cap) return at;
    memcpy(o + at, b, k);
    return at + k;
}

// The visible text of a UTF-16LE message: tags (0x0E group type size params, 0x0F end tags) dropped, except the
// grammatical forms of a noun (group 0xC9 type 0x0A: length-prefixed UTF-16 strings, the first the plain form), which
// stand for the word in the languages that inflect it.
static void msg_text(const uint8_t *p, size_t n, char *o, size_t cap) {
    size_t at = 0;
    for (size_t i = 0; i + 1 < n;) {
        uint16_t c = (uint16_t)(p[i] | p[i + 1] << 8);
        if (c == 0) break;
        if (c == 0x0e && i + 8 <= n) {
            uint16_t group = (uint16_t)(p[i + 2] | p[i + 3] << 8), type = (uint16_t)(p[i + 4] | p[i + 5] << 8);
            uint16_t size = (uint16_t)(p[i + 6] | p[i + 7] << 8);
            if (group == 0xc9 && type == 0x0a && size >= 2 && i + 8 + size <= n) {
                uint16_t len = (uint16_t)(p[i + 8] | p[i + 9] << 8);
                for (size_t j = 0; j + 1 < len && 10 + j + 1 < (size_t)size + 8; j += 2)
                    at = utf8_put(o, cap, at, (uint32_t)(p[i + 10 + j] | p[i + 11 + j] << 8));
            }
            i += 8 + size;
            continue;
        }
        if (c == 0x0f) {
            i += 6;
            continue;
        }
        if (c >= 0x20 || c == ' ') at = utf8_put(o, cap, at, c == '\n' ? ' ' : c);
        i += 2;
    }
    o[at] = 0;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// ---- the recipe book's dishes: the cooked, roasted, frozen and boiled foods the game names, and the elixir of each
// effect (the book keeps one entry per elixir effect, Item_Cook_C_17_<effect>, while the text names only "Elixir")
static char **cooks;
static size_t ncooks, cap_cooks;

static void cook_add(const char *actor) {
    if (ncooks == cap_cooks) {
        cap_cooks = cap_cooks ? cap_cooks * 2 : 256;
        char **g = realloc(cooks, cap_cooks * sizeof *cooks);
        if (!g) return;
        cooks = g;
    }
    if ((cooks[ncooks] = strdup(actor))) ncooks++;
}

static void cook_note(const char *actor) {
    static const char *const prefixes[] = {"Item_Cook_", "Item_Roast", "Item_Chilled", "Item_Boiled"};
    for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; i++)
        if (!strncmp(actor, prefixes[i], strlen(prefixes[i]))) {
            if (!strcmp(actor, "Item_Cook_C_17")) {
                static const char *const effects[] = {"ResistHot", "ResistBurn", "ResistCold", "ResistElectric",
                                                      "AttackUp", "QuietnessUp", "DefenseUp", "AllSpeed", "LifeMaxUp",
                                                      "StaminaRecover", "ExStaminaMaxUp", "NotSlippy", "LightEmission"};
                for (size_t e = 0; e < sizeof effects / sizeof *effects; e++) {
                    char name[64];
                    snprintf(name, sizeof name, "Item_Cook_C_17_%s", effects[e]);
                    cook_add(name);
                }
            } else {
                cook_add(actor);
            }
            return;
        }
}

static void msbt_names(const uint8_t *p, size_t n) {
    if (n < 0x20 || memcmp(p, "MsgStdBn", 8) || p[8] != 0xff) return;   // little-endian files only
    uint16_t sections = (uint16_t)(p[0x0e] | p[0x0f] << 8);
    const uint8_t *lbl = NULL, *txt = NULL;
    size_t lbl_n = 0, txt_n = 0, o = 0x20;
    for (uint16_t s = 0; s < sections && o + 16 <= n; s++) {
        uint32_t size = rd32(p + o + 4);
        if (o + 16 + size > n) break;
        if (!memcmp(p + o, "LBL1", 4)) lbl = p + o + 16, lbl_n = size;
        if (!memcmp(p + o, "TXT2", 4)) txt = p + o + 16, txt_n = size;
        o += 16 + ((size + 15) & ~15u);
    }
    if (!lbl || !txt || lbl_n < 4 || txt_n < 4) return;
    uint32_t buckets = rd32(lbl), count = rd32(txt);
    for (uint32_t b = 0; b < buckets && 4 + 8 * (size_t)b + 8 <= lbl_n; b++) {
        uint32_t k = rd32(lbl + 4 + 8 * b), at = rd32(lbl + 8 + 8 * b);
        for (uint32_t j = 0; j < k && at < lbl_n; j++) {
            uint8_t len = lbl[at];
            if (at + 1 + len + 4 > lbl_n) break;
            const char *label = (const char *)lbl + at + 1;
            uint32_t idx = rd32(lbl + at + 1 + len);
            at += 1 + len + 4;
            if (len <= 5 || memcmp(label + len - 5, "_Name", 5) || idx >= count) continue;
            uint32_t from = rd32(txt + 4 + 4 * idx), to = idx + 1 < count ? rd32(txt + 8 + 4 * idx) : (uint32_t)txt_n;
            if (from >= to || to > txt_n) continue;
            char actor[128], text[128];
            if (len - 5 >= (int)sizeof actor) continue;
            memcpy(actor, label, len - 5), actor[len - 5] = 0;
            msg_text(txt + from, to - from, text, sizeof text);
            if (nnames == cap_names) {
                cap_names = cap_names ? cap_names * 2 : 4096;
                Name *g = realloc(names, cap_names * sizeof *names);
                if (!g) return;
                names = g;
            }
            names[nnames].h = mm3(actor);
            names[nnames].text = strdup(text);
            if (names[nnames].text) nnames++;
            cook_note(actor);
        }
    }
}

// The Mals archive of the game's language ("en-US" -> Mals/USen.Product.*), else US English.
static void mals_found(const char *rel, void *ctx) {
    char *out = ctx;
    if (!*out) snprintf(out, 256, "%s", rel);
}

static void names_load(void) {
    static const struct { const char *code, *mals; } langs[] = {
        {"ja", "JPja"}, {"en-US", "USen"}, {"fr", "EUfr"}, {"de", "EUde"}, {"it", "EUit"}, {"es", "EUes"},
        {"ko", "KRko"}, {"nl", "EUnl"}, {"ru", "EUru"}, {"en-GB", "EUen"}, {"fr-CA", "USfr"}, {"es-419", "USes"},
        {"zh-Hans", "CNzh"}, {"zh-Hant", "TWzh"}};
    const char *lang = ov_language(), *mals = "USen";
    for (size_t i = 0; i < sizeof langs / sizeof *langs; i++)
        if (lang && !strcmp(lang, langs[i].code)) mals = langs[i].mals;
    char prefix[64], rel[256] = "";
    snprintf(prefix, sizeof prefix, "Mals/%s.Product.", mals);
    romfs_list(prefix, mals_found, rel);
    if (!*rel) romfs_list("Mals/USen.Product.", mals_found, rel);
    size_t n, raw_n;
    uint8_t *zs = *rel ? romfs_load(rel, &n, "statistics show internal names") : NULL;
    uint8_t *raw = zs ? zs_unpack(zs, n, &raw_n) : NULL;
    free(zs);
    static const char *const files[] = {"ActorMsg/PictureBook.msbt", "ActorMsg/PouchContent.msbt"};
    for (int i = 0; raw && i < 2; i++) {
        size_t at, size;
        if (sarc_find(raw, raw_n, files[i], &at, &size)) msbt_names(raw + at, size);
    }
    free(raw);
    if (nnames) qsort(names, nnames, sizeof *names, name_cmp);
    rt_log("stats: %zu actor names from %s", nnames, *rel ? rel : "(no Mals archive)");
}

// ---- actors of Pack/Actor
static char **actors;
static size_t nactors, cap_actors;

static void actor_found(const char *rel, void *ctx) {
    (void)ctx;
    const char *base = rel + strlen("Pack/Actor/");
    size_t n = strlen(base);
    if (n <= 8 || strcmp(base + n - 8, ".pack.zs")) return;
    if (nactors == cap_actors) {
        cap_actors = cap_actors ? cap_actors * 2 : 16384;
        char **g = realloc(actors, cap_actors * sizeof *actors);
        if (!g) return;
        actors = g;
    }
    actors[nactors] = malloc(n - 7);
    if (!actors[nactors]) return;
    memcpy(actors[nactors], base, n - 8), actors[nactors][n - 8] = 0;
    nactors++;
}

// ---- the save's flags
typedef struct { uint32_t h, v; } Flag;
static Flag *flags;
static size_t nflags;

static int flag_cmp(const void *a, const void *b) {
    uint32_t x = ((const Flag *)a)->h, y = ((const Flag *)b)->h;
    return x < y ? -1 : x > y;
}

static const Flag *flag(uint32_t h) {
    Flag key = {h, 0};
    return nflags ? bsearch(&key, flags, nflags, sizeof *flags, flag_cmp) : NULL;
}

static int load_save(const char *path) {
    uint64_t n = 0;
    uint8_t *d = plat_file_read(path, &n);
    if (!d || n < 0x28 || rd32(d) != 0x01020304u) {
        free(d);
        return 0;
    }
    uint32_t end = rd32(d + 8);
    if (end > n) end = (uint32_t)n;
    size_t cnt = end > 0x20 ? (end - 0x20) / 8 : 0;
    Flag *f = malloc((cnt ? cnt : 1) * sizeof *f);
    if (!f) {
        free(d);
        return 0;
    }
    size_t k = 0;
    for (size_t i = 0; i < cnt; i++) {
        f[k].h = rd32(d + 0x20 + 8 * i), f[k].v = rd32(d + 0x24 + 8 * i);
        if (f[k].h) k++;
    }
    free(d);
    qsort(f, k, sizeof *f, flag_cmp);
    free(flags);
    flags = f, nflags = k;
    return 1;
}

// ---- live flags: the game's own GameData manager, read while the game runs
// The manager keeps one section per flag type, 0x60 bytes apart: Bool at +0x10, Int at +0xd0, Float at +0x190, Enum
// at +0x250. A section holds its record count (+0), the records (+8), an open-addressing table of (hash, record
// index) pairs (+0x10, hash modulo its slot count +0x1c, linear probing, hash 0 ends a chain), and the count again
// (+0x18). A record starts with a vtable; its current value sits at +0x1c in the 0x28-byte Int and Float records
// and the 0x38-byte Enum records, and is the byte +0x16 of the 0x18-byte Bool records. (Read from the game's getter,
// main+0x61fdd0 on 1.4.3, and checked against the save's values.) The manager is found by scanning main's data for a
// pointer whose Int section finds PlayerStatus.MaxLife, so no address is tied to a game version. Each value's
// address is worked out once; then the values are read every LIVE_MS, so a defeated monster or a new item counts at
// once instead of at the next save. Without a manager (another layout, before a save is loaded) the save is read.
enum { SEC_BOOL = 0x10, SEC_INT = 0xd0, SEC_FLOAT = 0x190, SEC_ENUM = 0x250, LIVE_MS = 2000 };
enum { LV_U32, LV_U8 };
typedef struct { uint32_t h; uint8_t kind; uint64_t at; } Live;
static Live *live;
static size_t nlive;
static uint64_t live_slot, live_mgr;   // the main address holding the manager pointer, and the manager it held
static uint64_t live_recs[4];          // the sections' record arrays when the addresses were worked out

// Host pages that can be read without a fault: a guest region mem_query calls mapped may still have pages the host
// has not committed (an access violation at 0x18000000e8 while scanning showed it).
static int readable(uint64_t a, uint64_t n) { return a && a + n >= a && plat_mem_readable(a, n); }

static uint64_t rq(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
static uint32_t rw(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }

typedef struct { uint64_t recs, map; uint32_t count, slots; } Section;

static int section(uint64_t mgr, uint32_t at, uint32_t stride, Section *s) {
    if (!readable(mgr + at, 0x20)) return 0;
    s->count = rw(mgr + at), s->recs = rq(mgr + at + 8), s->map = rq(mgr + at + 0x10), s->slots = rw(mgr + at + 0x1c);
    return s->count && s->count < 1000000 && rw(mgr + at + 0x18) == s->count && s->slots >= s->count &&
           s->slots < 4000000 && readable(s->map, (uint64_t)s->slots * 8) &&
           readable(s->recs, (uint64_t)s->count * stride);
}

// The record index of hash h in section s, or -1.
static int64_t section_find(const Section *s, uint32_t h) {
    if (!h) return -1;
    for (uint32_t i = h % s->slots, n = 0; n < s->slots; n++, i = i + 1 == s->slots ? 0 : i + 1) {
        uint32_t k = rw(s->map + 8ull * i);
        if (k == h) {
            uint32_t idx = rw(s->map + 8ull * i + 4);
            return idx < s->count ? (int64_t)idx : -1;
        }
        if (!k) return -1;
    }
    return -1;
}

// A manager: all four sections hold up, and the Int one finds PlayerStatus.MaxLife (other objects of the game keep
// an Int table of the same shape).
static int manager_ok(uint64_t mgr, Section *ints) {
    Section other;
    if ((mgr & 7) || mgr <= 0x100000 || !section(mgr, SEC_INT, 0x28, ints) || !section(mgr, SEC_BOOL, 0x18, &other) ||
        !section(mgr, SEC_ENUM, 0x38, &other) || !section(mgr, SEC_FLOAT, 0x28, &other))
        return 0;
    int64_t i = section_find(ints, mm3("PlayerStatus.MaxLife"));
    return i >= 0 && rq(ints->recs + 0x28ull * (uint64_t)i) != 0;   // a record with its vtable
}

static void live_add(Live **out, size_t *n, size_t *cap, const Section *s, uint32_t stride, uint32_t off, uint8_t kind,
                     uint32_t h) {
    int64_t i = section_find(s, h);
    if (i < 0) return;
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8192;
        Live *g = realloc(*out, *cap * sizeof **out);
        if (!g) return;
        *out = g;
    }
    (*out)[(*n)++] = (Live){h, kind, s->recs + (uint64_t)stride * (uint64_t)i + off};
}

static int live_cmp(const void *a, const void *b) {
    uint32_t x = ((const Live *)a)->h, y = ((const Live *)b)->h;
    return x < y ? -1 : x > y;
}

// Finds the manager (once a save is loaded) and the address of every value the statistics read. 1 when ready.
static const uint32_t kSecs[4] = {SEC_INT, SEC_BOOL, SEC_ENUM, SEC_FLOAT};

// The manager and its record arrays are where the addresses were worked out for (a reload may move them).
static int live_same(void) {
    if (!live_slot || !live_mgr || rq(live_slot) != live_mgr || !readable(live_mgr, 0x300)) return 0;
    for (int i = 0; i < 4; i++)
        if (rq(live_mgr + kSecs[i] + 8) != live_recs[i]) return 0;
    return 1;
}

static int live_find(void) {
    if (live_same()) return 1;
    nlive = 0, live_mgr = 0;
    if (frame_step_world_us() < 0) return 0;
    Section ints;
    if (live_slot && manager_ok(rq(live_slot), &ints)) {
        live_mgr = rq(live_slot);
    } else {
        live_slot = 0;
        const RtLoaded *m = ldr_count() ? ldr_module(0) : NULL;
        if (!m || m->data_off >= m->size) return 0;
        uint64_t from = m->base + m->data_off, to = m->base + m->size;
        for (uint64_t a = from & ~7ull; a + 8 <= to && !live_slot; a += 8) {
            if (((a - from) & 0xfff) == 0 && !readable(a, 0x1000 < to - a ? 0x1000 : to - a)) {
                a += 0xff8;
                continue;
            }
            uint64_t p = rq(a);
            if (p && readable(p, 0x300) && manager_ok(p, &ints)) live_slot = a, live_mgr = p;
        }
        if (!live_slot) return 0;
        rt_log("stats: the game's GameData manager at %#llx (pointer at main+%#llx): statistics read live",
               (unsigned long long)live_mgr, (unsigned long long)(live_slot - ldr_module(0)->base));
    }
    Section bools, enums, floats;
    if (!section(live_mgr, SEC_BOOL, 0x18, &bools) || !section(live_mgr, SEC_ENUM, 0x38, &enums) ||
        !section(live_mgr, SEC_FLOAT, 0x28, &floats)) {
        live_mgr = 0;
        return 0;
    }
    for (int i = 0; i < 4; i++) live_recs[i] = rq(live_mgr + kSecs[i] + 8);
    Live *out = NULL;
    size_t n = 0, cap = 0;
    for (size_t i = 0; i < nactors; i++) {
        live_add(&out, &n, &cap, &ints, 0x28, 0x1c, LV_U32, flagf("DefeatedEnemyNum.%s", actors[i]));
        live_add(&out, &n, &cap, &enums, 0x38, 0x1c, LV_U32, flagf("PictureBookData.%s.State", actors[i]));
        live_add(&out, &n, &cap, &bools, 0x18, 0x16, LV_U8, flagf("IsGet.%s", actors[i]));
    }
    for (size_t i = 0; i < ncooks; i++)
        live_add(&out, &n, &cap, &bools, 0x18, 0x16, LV_U8, flagf("RecipeCard.Content.%s.IsCooked", cooks[i]));
    live_add(&out, &n, &cap, &ints, 0x28, 0x1c, LV_U32, mm3("PlayerStatus.MaxLife"));
    live_add(&out, &n, &cap, &floats, 0x28, 0x1c, LV_U32, mm3("PlayerStatus.MaxStamina"));
    qsort(out, n, sizeof *out, live_cmp);
    free(live);
    live = out, nlive = n;
    rt_log("stats: %zu flags read live", nlive);
    return nlive > 0;
}

// The live values in place of the save's flags (the same sorted table compute reads).
static int live_read(void) {
    if (!live_same()) return 0;
    Flag *f = malloc((nlive ? nlive : 1) * sizeof *f);
    if (!f) return 0;
    for (size_t i = 0; i < nlive; i++) {
        f[i].h = live[i].h;
        f[i].v = live[i].kind == LV_U8 ? *(const volatile uint8_t *)(uintptr_t)live[i].at
                                       : *(const volatile uint32_t *)(uintptr_t)live[i].at;
    }
    free(flags);
    flags = f, nflags = nlive;
    return 1;
}

// ---- statistics
// Defeat goals by how often the family turns up: the common ones (Bokoblins, constructs, Keese) take several tiers up
// to hundreds, the rare ones (Lynels, Hinoxes, Taluses, Gleeoks, Frox, Wizzrobes) one goal. A tier's id keeps its
// goal ("family_Bokoblin_100"), so goals added later leave unlocked ones in place.
static const struct { const char *prefix, *key; uint32_t tiers[3]; } kFamilies[STATS_FAMILIES] = {
    {"Enemy_Bokoblin_", "bokoblin", {100, 500, 1000}}, {"Enemy_Moriblin_", "moblin", {50, 250, 500}},
    {"Enemy_Lizalfos_", "lizalfos", {50, 250, 500}},   {"Enemy_Horablin_", "horriblin", {30, 150}},
    {"Enemy_Lynel_", "lynel", {10}},                   {"Enemy_Giant_", "hinox", {10}},
    {"Enemy_Golem_", "talus", {10}},                   {"Enemy_CaveMaster_", "bubbulfrog", {5, 50}},
    {"Enemy_Drake_", "gleeok", {3}},                   {"Enemy_Mogurudo_", "frox", {3}},
    {"Enemy_LikeLike_", "like_like", {25, 100}},       {"Enemy_Wizzrobe_", "wizzrobe", {10}},
    {"Enemy_Zonau_", "construct", {100, 500, 1000}},   {"Enemy_Keese", "keese", {50, 250}},
    {"Enemy_Chuchu", "chuchu", {50, 250}},             {"Enemy_Octarock", "octorok", {25, 100}},
    {"Enemy_Zombie_", "gibdo", {25, 100}},             {"Enemy_Assassin_", "yiga", {20, 100}}};

static PlatMutex lock = PLAT_MUTEX_INIT;   // view, unlocked
static StatsView view;
static uint64_t unlocked[STATS_ACH_MAX];   // unix time, 0 locked
static char ach_path[1100], goals_path[1100];   // <profile>/achievements/unlocked.txt and goals.ini

// Distinct names among a category's actors: the index of name in seen[], appended when new.
static int name_slot(const char **seen, int *n, int cap, const char *name) {
    for (int i = 0; i < *n; i++)
        if (!strcmp(seen[i], name)) return i;
    if (*n == cap) return -1;
    seen[*n] = name;
    return (*n)++;
}

// ---- a family's name: the game's name of its base kind (<prefix>Junior: "Bokoblin"), else the words every kind's
// name has in common ("Fire Wizzrobe", "Ice Wizzrobe" -> "Wizzrobe"; Russian matched without case), else the
// characters they end with (Chinese and Japanese write no spaces), else the first kind's name.
static char fam_names[STATS_FAMILIES][64];

// Lower case of ASCII and Cyrillic letters, the scripts the game's languages capitalise; other text is copied.
static void fold(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 3 < cap;) {
        if (*p >= 'A' && *p <= 'Z') out[o++] = (char)(*p++ + 32);
        else if (*p == 0xd0 && p[1] >= 0x90 && p[1] <= 0x9f) out[o++] = (char)0xd0, out[o++] = (char)(p[1] + 0x20), p += 2;
        else if (*p == 0xd0 && p[1] >= 0xa0 && p[1] <= 0xaf) out[o++] = (char)0xd1, out[o++] = (char)(p[1] - 0x20), p += 2;
        else if (*p == 0xd0 && p[1] == 0x81) out[o++] = (char)0xd1, out[o++] = (char)0x91, p += 2;   // Ё
        else out[o++] = (char)*p++;
    }
    out[o] = 0;
}

static void capitalise(char *s) {
    unsigned char *p = (unsigned char *)s;
    if (*p >= 'a' && *p <= 'z') *p -= 32;
    else if (p[0] == 0xd0 && p[1] >= 0xb0 && p[1] <= 0xbf) p[1] -= 0x20;
    else if (p[0] == 0xd1 && p[1] >= 0x80 && p[1] <= 0x8f) p[0] = 0xd0, p[1] += 0x20;
    else if (p[0] == 0xd1 && p[1] == 0x91) p[0] = 0xd0, p[1] = 0x81;
}

static int word_edge(const char *s, const char *at, size_t n) {
    int before = at == s || at[-1] == ' ' || at[-1] == '-';
    int after = !at[n] || at[n] == ' ' || at[n] == '-';
    return before && after;
}

static int in_all(char (*folded)[128], int n, const char *w, size_t len) {
    char key[128];
    if (len >= sizeof key) return 0;
    memcpy(key, w, len), key[len] = 0;
    for (int i = 0; i < n; i++) {
        const char *hit = folded[i], *found = NULL;
        while ((hit = strstr(hit, key))) {
            if (word_edge(folded[i], hit, len)) found = hit;
            hit++;
        }
        if (!found) return 0;
    }
    return 1;
}

static void families_name(void) {
    static int done;
    if (done) return;
    done = 1;
    for (int f = 0; f < STATS_FAMILIES; f++) {
        enum { MAXV = 48 };
        const char *v[MAXV];
        static char folded[MAXV][128];
        int n = 0;
        char junior[96];
        snprintf(junior, sizeof junior, "%sJunior", kFamilies[f].prefix);
        const char *base = name_of(junior);
        if (base) {
            snprintf(fam_names[f], sizeof fam_names[f], "%s", base);
            continue;
        }
        size_t plen = strlen(kFamilies[f].prefix);
        for (size_t i = 0; i < nactors && n < MAXV; i++) {
            const char *nm;
            if (strncmp(actors[i], kFamilies[f].prefix, plen) || !flag(flagf("DefeatedEnemyNum.%s", actors[i])) ||
                !(nm = name_of(actors[i])))
                continue;
            int dup = 0;
            for (int j = 0; j < n; j++) dup |= !strcmp(v[j], nm);
            if (!dup) fold(nm, folded[n], sizeof folded[n]), v[n++] = nm;
        }
        if (!n) continue;
        // The longest run of whole words of the first name that every name holds.
        const char *w0 = folded[0];
        size_t best_at = 0, best_len = 0, len0 = strlen(w0);
        for (size_t a = 0; a < len0; a++) {
            if (a && w0[a - 1] != ' ' && w0[a - 1] != '-') continue;
            for (size_t b = a + 1; b <= len0; b++) {
                if (w0[b] && w0[b] != ' ' && w0[b] != '-') continue;
                if (b - a > best_len && in_all(folded, n, w0 + a, b - a)) best_at = a, best_len = b - a;
            }
        }
        if (best_len && n > 1) {
            snprintf(fam_names[f], sizeof fam_names[f], "%.*s", (int)best_len, w0 + best_at);
            capitalise(fam_names[f]);
            continue;
        }
        // The characters every name ends with, whole UTF-8 sequences, at least two of them.
        size_t suf = 0, chars = 0;
        for (;;) {
            size_t k = suf + 1;
            while (k <= len0 && ((unsigned char)w0[len0 - k] & 0xc0) == 0x80) k++;
            if (k > len0) break;
            int all = 1;
            for (int i = 1; i < n && all; i++) {
                size_t li = strlen(folded[i]);
                all = li >= k && !memcmp(folded[i] + li - k, w0 + len0 - k, k);
            }
            if (!all) break;
            suf = k, chars++;
        }
        // Only in scripts without spaces (a three-byte UTF-8 lead: CJK, kana): in Latin or Cyrillic text a shared ending
        // is part of a word ("Wizzrobe", "Blizzrobe" -> "izzrobe").
        if (n > 1 && chars >= 2 && (unsigned char)w0[len0 - suf] >= 0xe0)
            snprintf(fam_names[f], sizeof fam_names[f], "%s", w0 + len0 - suf);
        else snprintf(fam_names[f], sizeof fam_names[f], "%s", v[0]);
    }
}

static void compute(StatsView *s) {
    memset(s->family, 0, sizeof s->family);
    families_name();
    for (int f = 0; f < STATS_FAMILIES; f++) snprintf(s->family[f].name, sizeof s->family[f].name, "%s", fam_names[f]);
    s->kills = s->kinds = s->kinds_max = 0;
    s->weapons = s->weapons_max = s->armor = s->armor_max = s->sets = s->sets_max = 0;
    s->items = s->items_max = s->compendium = s->compendium_max = 0;
    uint32_t unopened = mm3("Unopened");
    enum { CAP = 4096 };
    static const char *wseen[CAP], *aseen[CAP], *iseen[CAP], *sets[CAP][3];
    static uint8_t wgot[CAP], agot[CAP], igot[CAP], sgot[CAP][3];
    int nw = 0, na = 0, ni = 0, ns = 0;
    memset(wgot, 0, sizeof wgot), memset(agot, 0, sizeof agot), memset(igot, 0, sizeof igot);
    memset(sgot, 0, sizeof sgot);
    for (size_t i = 0; i < nactors; i++) {
        const char *a = actors[i];
        const Flag *k = flag(flagf("DefeatedEnemyNum.%s", a));
        const Flag *pb = flag(flagf("PictureBookData.%s.State", a));
        if (pb) {
            s->compendium_max++;
            if (pb->v != unopened) s->compendium++;
        }
        if (k) {
            s->kills += k->v;
            if (pb && !strncmp(a, "Enemy_", 6)) {
                s->kinds_max++;
                if (k->v) s->kinds++;
            }
            for (int f = 0; f < STATS_FAMILIES; f++)
                if (!strncmp(a, kFamilies[f].prefix, strlen(kFamilies[f].prefix))) {
                    s->family[f].count += k->v;
                }
        }
        const Flag *g = flag(flagf("IsGet.%s", a));
        const char *nm = g ? name_of(a) : NULL;
        if (!nm) continue;
        int slot;
        if (!strncmp(a, "Weapon_", 7) && (slot = name_slot(wseen, &nw, CAP, nm)) >= 0) wgot[slot] |= g->v != 0;
        if (!strncmp(a, "Item_", 5) && (slot = name_slot(iseen, &ni, CAP, nm)) >= 0) igot[slot] |= g->v != 0;
        if (!strncmp(a, "Armor_", 6) && (slot = name_slot(aseen, &na, CAP, nm)) >= 0) agot[slot] |= g->v != 0;
    }
    // Sets: Armor_<n>_Head / _Upper / _Lower that all have names; one set per distinct triple of names, complete when
    // the player has had a piece of each name.
    for (size_t i = 0; i < nactors; i++) {
        const char *a = actors[i];
        size_t len = strlen(a);
        if (strncmp(a, "Armor_", 6) || len < 11 || strcmp(a + len - 5, "_Head")) continue;
        char up[128], lo[128];
        if (len >= sizeof up - 1) continue;
        snprintf(up, sizeof up, "%.*s_Upper", (int)(len - 5), a);
        snprintf(lo, sizeof lo, "%.*s_Lower", (int)(len - 5), a);
        const char *t[3] = {name_of(a), name_of(up), name_of(lo)};
        if (!t[0] || !t[1] || !t[2] || !flag(flagf("IsGet.%s", a)) || !flag(flagf("IsGet.%s", up)) ||
            !flag(flagf("IsGet.%s", lo)))
            continue;
        int found = -1;
        for (int j = 0; j < ns && found < 0; j++)
            if (!strcmp(sets[j][0], t[0]) && !strcmp(sets[j][1], t[1]) && !strcmp(sets[j][2], t[2])) found = j;
        if (found < 0 && ns < CAP) found = ns++, sets[found][0] = t[0], sets[found][1] = t[1], sets[found][2] = t[2];
    }
    for (int j = 0; j < ns; j++)
        for (int p = 0; p < 3; p++) {
            int slot = name_slot(aseen, &na, CAP, sets[j][p]);
            sgot[j][p] = slot >= 0 && agot[slot];
        }
    for (int i = 0; i < nw; i++) s->weapons += wgot[i];
    for (int i = 0; i < na; i++) s->armor += agot[i];
    for (int i = 0; i < ni; i++) s->items += igot[i];
    for (int j = 0; j < ns; j++) s->sets += sgot[j][0] && sgot[j][1] && sgot[j][2];
    s->weapons_max = (uint32_t)nw, s->armor_max = (uint32_t)na, s->items_max = (uint32_t)ni, s->sets_max = (uint32_t)ns;
    // The recipe book: meals, elixirs (one per effect), the roasted, frozen and boiled foods, and the failures
    // (Dubious Food, Rock-Hard Food), each counted when its IsCooked flag is set.
    s->meals = s->meals_max = s->elixirs = s->elixirs_max = s->simple = s->simple_max = s->dubious = 0;
    for (size_t i = 0; i < ncooks; i++) {
        const Flag *c = flag(flagf("RecipeCard.Content.%s.IsCooked", cooks[i]));
        if (!c) continue;
        const char *a = cooks[i];
        int done = c->v != 0;
        if (!strncmp(a, "Item_Cook_O_", 12)) s->dubious |= (uint32_t)done;
        else if (!strncmp(a, "Item_Cook_C_17_", 15)) s->elixirs_max++, s->elixirs += (uint32_t)done;
        else if (!strncmp(a, "Item_Cook_", 10)) s->meals_max++, s->meals += (uint32_t)done;
        else s->simple_max++, s->simple += (uint32_t)done;
    }
    const Flag *life = flag(mm3("PlayerStatus.MaxLife")), *stamina = flag(mm3("PlayerStatus.MaxStamina"));
    s->hearts = life ? life->v / 4 : 0;
    float st = 0;
    if (stamina) memcpy(&st, &stamina->v, 4);
    s->stamina_wheels = st / 1000.0f;
}

// ---- achievements
// The goals: a set per statistic (and per monster family), each goal one achievement whose id is the set's id prefix
// and the goal ("kills_100", "family_Bokoblin_500"), so an unlock stays with its goal whatever else changes.
// <profile>/achievements/goals.ini overrides the defaults below; the game writes it with them on its first run.
typedef struct { const char *id; StatsKind kind; int family; uint32_t target; } Ach;
static Ach achs[STATS_ACH_MAX];
static int nachs;

#define GOALS_MAX 6
typedef struct { const char *key, *id; StatsKind kind; int family; uint32_t goals[GOALS_MAX]; } GoalSet;
static GoalSet sets[24 + STATS_FAMILIES];
static int nsets;

static void set_add(const char *key, const char *id, StatsKind kind, int family, const uint32_t *goals, int n) {
    if (nsets == (int)(sizeof sets / sizeof *sets)) return;
    GoalSet *s = &sets[nsets++];
    *s = (GoalSet){key, id, kind, family, {0}};
    for (int i = 0; i < n && i < GOALS_MAX; i++) s->goals[i] = goals[i];
}

static void sets_default(void) {
    static const uint32_t kills[] = {1, 100, 1000, 5000}, kinds[] = {25, 50, 75}, weapons[] = {25, 75},
                          armor[] = {25, 100}, asets[] = {1, 10, 25}, items[] = {50, 150}, pb[] = {100, 250},
                          hearts[] = {10, 20, 30}, stamina[] = {2, 3};
    set_add("monsters", "kills_", STATS_KILLS, -1, kills, 4);
    set_add("species", "kinds_", STATS_KINDS, -1, kinds, 3);
    static char ids[STATS_FAMILIES][40];
    for (int f = 0; f < STATS_FAMILIES; f++) {
        snprintf(ids[f], sizeof ids[f], "family_%s", kFamilies[f].prefix + 6);
        int n = 0;
        while (n < 3 && kFamilies[f].tiers[n]) n++;
        set_add(kFamilies[f].key, ids[f], STATS_FAMILY, f, kFamilies[f].tiers, n);
    }
    set_add("weapons", "weapons_", STATS_WEAPONS, -1, weapons, 2);
    set_add("armor", "armor_", STATS_ARMOR, -1, armor, 2);
    set_add("armor_sets", "sets_", STATS_SETS, -1, asets, 3);
    set_add("materials", "items_", STATS_ITEMS, -1, items, 2);
    set_add("compendium", "compendium_", STATS_COMPENDIUM, -1, pb, 2);
    static const uint32_t meals[] = {10, 50, 100}, elixirs[] = {5, 13}, simple[] = {20, 50}, dubious[] = {1};
    set_add("meals", "meals_", STATS_MEALS, -1, meals, 3);
    set_add("elixirs", "elixirs_", STATS_ELIXIRS, -1, elixirs, 2);
    set_add("roasted_frozen", "simple_", STATS_SIMPLE, -1, simple, 2);
    set_add("dubious_food", "dubious_", STATS_DUBIOUS, -1, dubious, 1);
    set_add("hearts", "hearts_", STATS_HEARTS, -1, hearts, 3);
    set_add("stamina", "stamina_", STATS_STAMINA, -1, stamina, 2);
}

static int u32_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static const char kGoalsHead[] =
    "# Achievement goals of the Achievements tab in the settings menu (F1).\n"
    "# Each line: a statistic, '=', then up to 6 goals separated by commas; each goal is one achievement.\n"
    "# Change, add or remove goals, then restart the game. Nothing after '=' turns a line's achievements off.\n"
    "# A line that is missing or not understood keeps its default; delete this file to get every default back.\n"
    "# Unlocked achievements are in unlocked.txt next to this file; a goal you change starts locked.\n";

static void goals_write(void) {
    size_t cap = 8192, at = 0;
    char *out = malloc(cap);
    if (!out) return;
    at += (size_t)snprintf(out + at, cap - at, "%s", kGoalsHead);
    for (int s = 0; s < nsets; s++) {
        if (s == 0) at += (size_t)snprintf(out + at, cap - at, "\n# Monsters\n");
        if (s == 2) at += (size_t)snprintf(out + at, cap - at, "\n# Monsters defeated by family, all variants together\n");
        if (!strcmp(sets[s].key, "weapons")) at += (size_t)snprintf(out + at, cap - at, "\n# Collection\n");
        if (!strcmp(sets[s].key, "meals"))
            at += (size_t)snprintf(out + at, cap - at, "\n# Cooking: kinds of dishes in the recipe book\n");
        if (!strcmp(sets[s].key, "hearts")) at += (size_t)snprintf(out + at, cap - at, "\n# The hero\n");
        at += (size_t)snprintf(out + at, cap - at, "%s =", sets[s].key);
        for (int g = 0; g < GOALS_MAX && sets[s].goals[g]; g++)
            at += (size_t)snprintf(out + at, cap - at, "%s %u", g ? "," : "", sets[s].goals[g]);
        at += (size_t)snprintf(out + at, cap - at, "\n");
    }
    if (!plat_file_write_atomic(goals_path, out, at)) rt_log("stats: cannot write %s", goals_path);
    else rt_log("stats: wrote the default achievement goals to %s", goals_path);
    free(out);
}

static int key_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if ((*a | 32) != (*b | 32)) return 0;
    return !*a && !*b;
}

// goals.ini over the defaults: "key = n, n, ..." lines; '#' or ';' starts a comment.
static void goals_read(void) {
    uint64_t n = 0;
    char *raw = (char *)plat_file_read(goals_path, &n);
    if (!raw) {
        goals_write();
        return;
    }
    char *buf = malloc(n + 1);
    if (!buf) {
        free(raw);
        return;
    }
    memcpy(buf, raw, n), buf[n] = 0;
    free(raw);
    int read = 0, bad = 0;
    for (char *line = buf, *next; line; line = next) {
        next = strpbrk(line, "\r\n");
        if (next) *next++ = 0;
        char *c = strpbrk(line, "#;");
        if (c) *c = 0;
        char *eq = strchr(line, '=');
        if (!eq) {
            for (char *p = line; *p; p++) bad |= *p != ' ' && *p != '\t';
            continue;
        }
        *eq = 0;
        char key[48];
        if (sscanf(line, " %47[A-Za-z_]", key) != 1) {
            bad = 1;
            continue;
        }
        GoalSet *set = NULL;
        for (int s = 0; s < nsets; s++)
            if (key_eq(sets[s].key, key)) set = &sets[s];
        if (!set) {
            rt_log("stats: goals.ini: unknown statistic '%s'", key);
            bad = 1;
            continue;
        }
        uint32_t goals[GOALS_MAX];
        int k = 0, ok = 1;
        for (char *p = eq + 1; *p;) {
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            if (!*p) break;
            char *end;
            unsigned long v = strtoul(p, &end, 10);
            if (end == p || v < 1 || v > 1000000000ul || k == GOALS_MAX) {
                ok = 0;
                break;
            }
            int dup = 0;
            for (int i = 0; i < k; i++) dup |= goals[i] == v;
            if (!dup) goals[k++] = (uint32_t)v;
            p = end;
        }
        if (!ok) {
            rt_log("stats: goals.ini: '%s' keeps its default goals (goals are 1 to 1000000000, at most %d)", key,
                   GOALS_MAX);
            bad = 1;
            continue;
        }
        qsort(goals, (size_t)k, sizeof *goals, u32_cmp);
        memset(set->goals, 0, sizeof set->goals);
        memcpy(set->goals, goals, (size_t)k * sizeof *goals);
        read++;
    }
    free(buf);
    rt_log("stats: achievement goals from %s (%d lines read%s)", goals_path, read, bad ? ", some ignored" : "");
}

static void achs_define(void) {
    static char ids[STATS_ACH_MAX][48];
    sets_default();
    goals_read();
    for (int s = 0; s < nsets; s++)
        for (int g = 0; g < GOALS_MAX && sets[s].goals[g] && nachs < STATS_ACH_MAX; g++) {
            snprintf(ids[nachs], sizeof ids[nachs], "%s%u", sets[s].id, sets[s].goals[g]);
            achs[nachs] = (Ach){ids[nachs], sets[s].kind, sets[s].family, sets[s].goals[g]};
            nachs++;
        }
}

static uint32_t ach_progress(const StatsView *s, const Ach *a) {
    switch (a->kind) {
    case STATS_KILLS: return s->kills;
    case STATS_KINDS: return s->kinds;
    case STATS_FAMILY: return s->family[a->family].count;
    case STATS_WEAPONS: return s->weapons;
    case STATS_ARMOR: return s->armor;
    case STATS_SETS: return s->sets;
    case STATS_ITEMS: return s->items;
    case STATS_COMPENDIUM: return s->compendium;
    case STATS_MEALS: return s->meals;
    case STATS_ELIXIRS: return s->elixirs;
    case STATS_SIMPLE: return s->simple;
    case STATS_DUBIOUS: return s->dubious;
    case STATS_HEARTS: return s->hearts;
    case STATS_STAMINA: return (uint32_t)(s->stamina_wheels + 1e-3f);
    }
    return 0;
}

// Lines of unlocked.txt whose achievement the goals no longer have: written back unchanged, so an unlock comes back
// when its goal does.
static char *other_unlocks;
static size_t other_n;

static void ach_load(void) {
    uint64_t n = 0;
    char *t = (char *)plat_file_read(ach_path, &n);
    if (!t) return;
    char *buf = malloc(n + 1);
    if (buf) {
        memcpy(buf, t, n), buf[n] = 0;
        for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
            char id[64];
            unsigned long long when = 0;
            if (sscanf(line, "%63s %llu", id, &when) != 2) continue;
            int known = 0;
            for (int i = 0; i < nachs; i++)
                if (!strcmp(achs[i].id, id)) unlocked[i] = when ? when : 1, known = 1;
            size_t len = strlen(line);
            char *g = known ? NULL : realloc(other_unlocks, other_n + len + 2);
            if (g) {
                memcpy(g + other_n, line, len), g[other_n + len] = '\n';
                other_unlocks = g, other_n += len + 1;
            }
        }
        free(buf);
    }
    free(t);
}

static void ach_save(void) {
    char *out = malloc(64u * STATS_ACH_MAX + other_n);
    if (!out) return;
    size_t at = 0;
    if (other_n) memcpy(out, other_unlocks, other_n), at = other_n;
    for (int i = 0; i < nachs; i++)
        if (unlocked[i]) at += (size_t)snprintf(out + at, 64, "%s %llu\n", achs[i].id, (unsigned long long)unlocked[i]);
    if (!plat_file_write_atomic(ach_path, out, at)) rt_log("stats: cannot write %s", ach_path);
    free(out);
}

// Holding lock.
static int ach_update(void) {
    int changed = 0;
    for (int i = 0; i < nachs; i++) {
        view.ach[i].progress = ach_progress(&view, &achs[i]);
        if (!unlocked[i] && view.ach[i].progress >= achs[i].target) {
            unlocked[i] = (uint64_t)time(NULL);
            changed = 1;
            rt_log("stats: achievement %s unlocked", achs[i].id);
        }
        view.ach[i].kind = achs[i].kind, view.ach[i].family = achs[i].family, view.ach[i].target = achs[i].target;
        view.ach[i].unlocked = unlocked[i];
    }
    view.nach = nachs;
    return changed;
}

// ---- the thread
static volatile int32_t pending = 1;   // a save changed (fs_host.c), or the first read

void stats_save_changed(void) { plat_xchg32(&pending, 1); }

// The newest <save root>/slot_*/progress.sav: the slot the game wrote last.
static int newest_save(char *out, size_t cap) {
    uint64_t uid[2];
    account_uid(uid);
    char root[1100];
    snprintf(root, sizeof root, "%s/save/%016llx%016llx", rt_config()->profile, (unsigned long long)uid[1],
             (unsigned long long)uid[0]);
    int64_t best = -1;
    for (int s = 0; s < 32; s++) {
        char p[1200];
        snprintf(p, sizeof p, "%s/slot_%02d/progress.sav", root, s);
        int64_t t = plat_file_mtime(p);
        if (t > best) best = t, snprintf(out, cap, "%s", p);
    }
    return best >= 0;
}

static void stats_main(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    int ready = 0;
    for (;;) {
        plat_sleep_ms(LIVE_MS);
        int saved = plat_xchg32(&pending, 0);
        if (!ready) {
            if (!saved) continue;
            dicts_load();
            romfs_list("Pack/Actor/", actor_found, NULL);
            if (!nactors) {
                plat_xchg32(&pending, 1);   // the RomFS is not open yet
                continue;
            }
            names_load();
            ready = 1;
        }
        char path[1200] = "";
        int is_live = live_find() && live_read();
        if (!is_live && (!saved || !newest_save(path, sizeof path) || !load_save(path))) continue;
        StatsView s;
        plat_mutex_lock(&lock);
        s = view;
        plat_mutex_unlock(&lock);
        uint32_t kills = s.kills, items = s.items, weapons = s.weapons;
        compute(&s);
        s.valid = 1, s.live = is_live;
        s.when = is_live ? (uint64_t)time(NULL) : (uint64_t)plat_file_mtime(path);
        plat_mutex_lock(&lock);
        view = s;
        int changed = ach_update();
        plat_mutex_unlock(&lock);
        if (changed) ach_save();
        if (!is_live || s.kills != kills || s.items != items || s.weapons != weapons)
            rt_log("stats: %s: %u monsters defeated (%u of %u kinds), weapons %u/%u, armor %u/%u, sets %u/%u, "
                   "materials %u/%u, compendium %u/%u, meals %u/%u, elixirs %u/%u, roasted/frozen %u/%u, %u hearts, "
                   "%.1f stamina wheels",
                   is_live ? "live" : path, s.kills, s.kinds, s.kinds_max, s.weapons, s.weapons_max, s.armor,
                   s.armor_max, s.sets, s.sets_max, s.items, s.items_max, s.compendium, s.compendium_max, s.meals,
                   s.meals_max, s.elixirs, s.elixirs_max, s.simple, s.simple_max, s.hearts,
                   s.stamina_wheels);
    }
}

void stats_init(void) {
    static int32_t started;
    if (plat_xchg32(&started, 1)) return;
    const char *dir = rt_config()->profile;
    if (!dir || !*dir) return;
    char folder[1100];
    snprintf(folder, sizeof folder, "%s/achievements", dir);
    plat_file_mkdir_p(folder);
    snprintf(ach_path, sizeof ach_path, "%s/unlocked.txt", folder);
    snprintf(goals_path, sizeof goals_path, "%s/goals.ini", folder);
    achs_define();
    ach_load();
    plat_mutex_lock(&lock);
    ach_update();
    plat_mutex_unlock(&lock);
    if (!plat_thread_spawn(stats_main, NULL, "stats", 0)) rt_log("stats: cannot start its thread");
}

void stats_view(StatsView *out) {
    plat_mutex_lock(&lock);
    *out = view;
    plat_mutex_unlock(&lock);
}
