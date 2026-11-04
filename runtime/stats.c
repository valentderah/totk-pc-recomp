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
// A flag counts only when the save holds it, so actors without one (most of Pack/Actor) drop out.
//
// The names shown are the game's own, in the game's language: <actor>_Name of ActorMsg/PictureBook.msbt (monsters)
// and ActorMsg/PouchContent.msbt (weapons, armor, materials) in Mals/<language>.Product.<version>.sarc.zs. Weapons,
// armor pieces and sets are counted by those names, so the variants of one item (a set's dyed or upgraded copies,
// a weapon's decayed one) count once; an actor without a name in the game's text is left out.
//
// One host thread reads the newest progress.sav of <profile>/save/<uid> (the slot the game wrote last) at start and
// again after each save commit that changed files (fs_host.c calls stats_save_changed). Achievements unlock when the
// statistics reach them and stay unlocked: <profile>/achievements.txt holds one "<id> <unix time>" line each.
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

// ---- statistics
static const struct { const char *prefix; uint32_t target; } kFamilies[STATS_FAMILIES] = {
    {"Enemy_Bokoblin_", 100}, {"Enemy_Moriblin_", 50}, {"Enemy_Lizalfos_", 50}, {"Enemy_Horablin_", 30},
    {"Enemy_Lynel_", 10},     {"Enemy_Giant_", 10},    {"Enemy_Golem_", 10},    {"Enemy_CaveMaster_", 5},
    {"Enemy_Drake_", 3},      {"Enemy_Mogurudo_", 3},  {"Enemy_LikeLike_", 25}, {"Enemy_Wizzrobe_", 10},
    {"Enemy_Zonau_", 100},    {"Enemy_Keese", 50},     {"Enemy_Chuchu", 50},    {"Enemy_Octarock", 25},
    {"Enemy_Zombie_", 25},    {"Enemy_Assassin_", 20}};

static PlatMutex lock = PLAT_MUTEX_INIT;   // view, unlocked
static StatsView view;
static uint64_t unlocked[STATS_ACH_MAX];   // unix time, 0 locked
static char ach_path[1100];

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
    const Flag *life = flag(mm3("PlayerStatus.MaxLife")), *stamina = flag(mm3("PlayerStatus.MaxStamina"));
    s->hearts = life ? life->v / 4 : 0;
    float st = 0;
    if (stamina) memcpy(&st, &stamina->v, 4);
    s->stamina_wheels = st / 1000.0f;
}

// ---- achievements
typedef struct { const char *id; StatsKind kind; int family; uint32_t target; } Ach;
static Ach achs[STATS_ACH_MAX];
static int nachs;

static void ach_add(const char *id, StatsKind kind, int family, uint32_t target) {
    if (nachs < STATS_ACH_MAX) achs[nachs++] = (Ach){id, kind, family, target};
}

static void achs_define(void) {
    ach_add("kills_1", STATS_KILLS, -1, 1);
    ach_add("kills_100", STATS_KILLS, -1, 100);
    ach_add("kills_1000", STATS_KILLS, -1, 1000);
    ach_add("kills_5000", STATS_KILLS, -1, 5000);
    ach_add("kinds_25", STATS_KINDS, -1, 25);
    ach_add("kinds_50", STATS_KINDS, -1, 50);
    ach_add("kinds_75", STATS_KINDS, -1, 75);
    static char ids[STATS_FAMILIES][40];
    for (int f = 0; f < STATS_FAMILIES; f++) {
        snprintf(ids[f], sizeof ids[f], "family_%s%u", kFamilies[f].prefix + 6, kFamilies[f].target);
        ach_add(ids[f], STATS_FAMILY, f, kFamilies[f].target);
    }
    ach_add("weapons_25", STATS_WEAPONS, -1, 25);
    ach_add("weapons_75", STATS_WEAPONS, -1, 75);
    ach_add("armor_25", STATS_ARMOR, -1, 25);
    ach_add("armor_100", STATS_ARMOR, -1, 100);
    ach_add("sets_1", STATS_SETS, -1, 1);
    ach_add("sets_10", STATS_SETS, -1, 10);
    ach_add("sets_25", STATS_SETS, -1, 25);
    ach_add("items_50", STATS_ITEMS, -1, 50);
    ach_add("items_150", STATS_ITEMS, -1, 150);
    ach_add("compendium_100", STATS_COMPENDIUM, -1, 100);
    ach_add("compendium_250", STATS_COMPENDIUM, -1, 250);
    ach_add("hearts_10", STATS_HEARTS, -1, 10);
    ach_add("hearts_20", STATS_HEARTS, -1, 20);
    ach_add("hearts_30", STATS_HEARTS, -1, 30);
    ach_add("stamina_2", STATS_STAMINA, -1, 2);
    ach_add("stamina_3", STATS_STAMINA, -1, 3);
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
    case STATS_HEARTS: return s->hearts;
    case STATS_STAMINA: return (uint32_t)(s->stamina_wheels + 1e-3f);
    }
    return 0;
}

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
            for (int i = 0; i < nachs; i++)
                if (!strcmp(achs[i].id, id)) unlocked[i] = when ? when : 1;
        }
        free(buf);
    }
    free(t);
}

static void ach_save(void) {
    char *out = malloc(64u * STATS_ACH_MAX);
    if (!out) return;
    size_t at = 0;
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
        plat_sleep_ms(2000);
        if (!plat_xchg32(&pending, 0)) continue;
        if (!ready) {
            dicts_load();
            romfs_list("Pack/Actor/", actor_found, NULL);
            if (!nactors) {
                plat_xchg32(&pending, 1);   // the RomFS is not open yet
                continue;
            }
            names_load();
            ready = 1;
        }
        char path[1200];
        if (!newest_save(path, sizeof path) || !load_save(path)) continue;
        StatsView s;
        plat_mutex_lock(&lock);
        s = view;
        plat_mutex_unlock(&lock);
        compute(&s);
        s.valid = 1;
        s.when = (uint64_t)plat_file_mtime(path);
        plat_mutex_lock(&lock);
        view = s;
        int changed = ach_update();
        plat_mutex_unlock(&lock);
        if (changed) ach_save();
        rt_log("stats: %s: %u monsters defeated (%u of %u kinds), weapons %u/%u, armor %u/%u, sets %u/%u, materials "
               "%u/%u, compendium %u/%u, %u hearts, %.1f stamina wheels",
               path, s.kills, s.kinds, s.kinds_max, s.weapons, s.weapons_max, s.armor, s.armor_max, s.sets, s.sets_max,
               s.items, s.items_max, s.compendium, s.compendium_max, s.hearts, s.stamina_wheels);
    }
}

void stats_init(void) {
    static int32_t started;
    if (plat_xchg32(&started, 1)) return;
    const char *dir = rt_config()->profile;
    if (!dir || !*dir) return;
    snprintf(ach_path, sizeof ach_path, "%s/achievements.txt", dir);
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
