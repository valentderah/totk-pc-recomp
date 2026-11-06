// The entries of the members of the mods' packs (restbl.c restbl_pack_raise), on archives and tables of its own; part
// of --probe-mods (mods_probe.c). Needs no game files.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "restbl.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { fails++; fprintf(stderr, "FAIL restbl: %s\n", what); } } while (0)

// A member of a probe archive: its name and size; a .zs member is a zstd frame header recording `content` bytes:
// fcs 4, a frame with a window descriptor and a 4-byte field; 2, a single-segment frame with a 2-byte field holding
// content - 256.
typedef struct { const char *name; size_t size, content; int fcs; } Member;

static void wr16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v & 0xffff), wr16(p + 2, v >> 16); }

// A SARC archive of the members m[0..n); unnamed: the index of a member without a name, -1 for none. malloc'd.
static uint8_t *sarc(const Member *m, int n, int unnamed, size_t *out_n) {
    size_t names = 0, data = 0;
    for (int i = 0; i < n; i++) names += (strlen(m[i].name) + 4) & ~(size_t)3, data += (m[i].size + 7) & ~(size_t)7;
    size_t sfat = 0x14, sfnt = sfat + 12 + 16 * (size_t)n, start = (sfnt + 8 + names + 15) & ~(size_t)15;
    size_t total = start + data;
    uint8_t *p = calloc(1, total);
    if (!p) return NULL;
    memcpy(p, "SARC", 4), wr16(p + 4, 0x14), wr16(p + 6, 0xfeff);
    wr32(p + 8, (uint32_t)total), wr32(p + 12, (uint32_t)start);
    memcpy(p + sfat, "SFAT", 4), wr16(p + sfat + 4, 12), wr16(p + sfat + 6, (unsigned)n), wr32(p + sfat + 8, 0x65);
    memcpy(p + sfnt, "SFNT", 4), wr16(p + sfnt + 4, 8);
    size_t name_at = 0, data_at = 0;
    for (int i = 0; i < n; i++) {
        uint8_t *node = p + sfat + 12 + 16 * (size_t)i, *d = p + start + data_at;
        wr32(node + 4, i == unnamed ? 0 : 0x01000000u | (uint32_t)(name_at / 4));
        wr32(node + 8, (uint32_t)data_at), wr32(node + 12, (uint32_t)(data_at + m[i].size));
        memcpy(p + sfnt + 8 + name_at, m[i].name, strlen(m[i].name));
        memset(d, 0x5a, m[i].size);
        if (m[i].fcs == 4) wr32(d, 0xFD2FB528u), d[4] = 0x80, d[5] = 0x58, wr32(d + 6, (uint32_t)m[i].content);
        if (m[i].fcs == 2) wr32(d, 0xFD2FB528u), d[4] = 0x60, wr16(d + 5, (unsigned)(m[i].content - 256));
        name_at += (strlen(m[i].name) + 4) & ~(size_t)3;
        data_at += (m[i].size + 7) & ~(size_t)7;
    }
    *out_n = total;
    return p;
}

// An empty table with the entries keys[i] = sizes[i].
static Restbl *table(const char *const *keys, const uint32_t *sizes, int n) {
    uint8_t head[22] = "RESTBL";
    wr32(head + 6, 1), wr32(head + 10, 160), wr32(head + 14, 0), wr32(head + 18, 0);
    Restbl *t = restbl_parse(head, sizeof head);
    for (int i = 0; t && i < n; i++) restbl_raise(t, keys[i], sizes[i]);
    return t;
}

static uint32_t get(const Restbl *t, const char *key) {
    uint32_t v = 0;
    return restbl_get(t, key, &v) ? v : 0;
}

static int visited;
static void count(const char *name, const uint8_t *data, size_t size, void *ctx) {
    (void)name, (void)data, (void)size, (void)ctx;
    visited++;
}

static int notes;
static void note(const char *key, uint32_t was, uint32_t now, void *ctx) {
    (void)key, (void)ctx;
    notes += now > was;
}

int restbl_probe(void) {
    fails = 0;
    static const char *const keys[] = {"AI/Grow.root.ainb", "AI/Same.root.ainb", "AI/Shrink.root.ainb",
                                       "Component/Shared.bgyml", "Model/Mesh.bfres", "AI/Zs.ainb", "AI/Zs2.ainb"};
    static const uint32_t sizes[] = {13512, 5000, 6000, 9000, 30000, 4400, 3500};
    // The game's copy: an .ainb member a mod grows (by 288 bytes, past its entry's slack), one it keeps, one it
    // shrinks, one the table does not size, a mesh codec member and two compressed ones.
    static const Member orig[] = {{"AI/Grow.root.ainb", 13088, 0, 0}, {"AI/Same.root.ainb", 4600, 0, 0},
                                  {"AI/Shrink.root.ainb", 5600, 0, 0}, {"AI/NotSized.ainb", 100, 0, 0},
                                  {"Model/Mesh.bfres.mc", 1000, 0, 0}, {"AI/Zs.ainb.zs", 64, 4000, 4},
                                  {"AI/Zs2.ainb.zs", 64, 3900, 2}};
    // The mod's copy: those, plus members new to the pack (one the game sizes elsewhere, one it lacks, a streamed one).
    static const Member mod[] = {{"AI/Grow.root.ainb", 13376, 0, 0}, {"AI/Same.root.ainb", 4600, 0, 0},
                                 {"AI/Shrink.root.ainb", 5000, 0, 0}, {"AI/NotSized.ainb", 300, 0, 0},
                                 {"Component/New/X.bgyml", 1000, 0, 0}, {"Component/Shared.bgyml", 2000, 0, 0},
                                 {"Sound/Stream.bwav", 500, 0, 0}, {"Model/Mesh.bfres.mc", 1500, 0, 0},
                                 {"AI/Zs.ainb.zs", 64, 4500, 4}, {"AI/Zs2.ainb.zs", 64, 4200, 2}};
    size_t on = 0, mn = 0;
    uint8_t *op = sarc(orig, 7, -1, &on), *mp = sarc(mod, 10, 4, &mn);   // the mod's New/X has no name: left alone
    Restbl *base = table(keys, sizes, 7), *t = table(keys, sizes, 7);
    CHECK(op && mp && base && t, "probe archives and tables");
    if (op && mp && base && t) {
        visited = 0;
        CHECK(restbl_sarc_each(mp, mn, count, NULL) && visited == 9, "SARC members: every named one");
        int r = restbl_pack_raise(t, base, mp, mn, op, on, note, NULL);
        CHECK(r == 5 && notes == 5, "five entries raised, each noted");
        CHECK(get(t, "AI/Grow.root.ainb") == 13824, "a member that grew: its entry plus the growth, 32-byte steps");
        CHECK(get(t, "AI/Same.root.ainb") == 5000 && get(t, "AI/Shrink.root.ainb") == 6000,
              "members that kept or lost bytes keep their entries");
        CHECK(!get(t, "AI/NotSized.ainb"), "a member the game sizes without the table gets no entry");
        CHECK(!get(t, "Component/New/X.bgyml"), "a member without a name is not looked up");
        CHECK(get(t, "Component/Shared.bgyml") == 16384, "a member new to the pack: the estimate of its type");
        CHECK(!get(t, "Sound/Stream.bwav"), "a streamed member new to the pack gets no entry");
        CHECK(get(t, "Model/Mesh.bfres") == 45000, "a mesh codec member's entry grows in proportion");
        CHECK(get(t, "AI/Zs.ainb") == 4928 && get(t, "AI/Zs2.ainb") == 3808,
              "a compressed member: the growth of the size its frame records (4- and 2-byte fields)");
        notes = 0;
        CHECK(restbl_pack_raise(t, base, mp, mn, op, on, note, NULL) == 0 && !notes, "a second pass raises nothing");
        Restbl *shipped = table(keys, (const uint32_t[]){20000, 5000, 6000, 9000, 30000, 4400, 3500}, 7);
        CHECK(shipped && restbl_pack_raise(shipped, base, mp, mn, op, on, NULL, NULL) == 4 &&
                  get(shipped, "AI/Grow.root.ainb") == 20000,
              "an entry a mod's own table raised further stays");
        restbl_free(shipped);
        Restbl *added = table(keys, sizes, 7);
        CHECK(added && restbl_pack_raise(added, base, mp, mn, NULL, 0, NULL, NULL) == 7 &&
                  get(added, "AI/Grow.root.ainb") == 24576 && get(added, "AI/NotSized.ainb") == 12288 &&
                  get(added, "AI/Zs2.ainb") == 16384 && get(added, "Model/Mesh.bfres") == 30000,
              "a pack the game lacks: every member gets the estimate of its type, an entry above it stays");
        restbl_free(added);
        CHECK(restbl_pack_raise(t, base, (const uint8_t *)"SARC", 4, NULL, 0, NULL, NULL) == -1 &&
                  restbl_pack_raise(t, base, mp, mn, mp, 0x20, NULL, NULL) == -1,
              "an archive that is not one, or whose game copy is cut short, raises nothing");
    }
    free(op);
    free(mp);
    restbl_free(base);
    restbl_free(t);
    printf("restbl: %s\n", fails ? "FAIL" : "ok");
    return fails;
}
