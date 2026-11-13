// Read a blueprint part's model straight from the RomFS: the actor's ModelInfo names its model file, which is a
// .bfres.mc decoded by the MeshCodec door (model_meshcodec.h). The BFRES reader (bfres.h) then walks the shapes to their
// vertices and indices.
//
// The decoder itself is optional: it is GPL-2.0 and lives outside this repository, so a build without it defines no
// TOTK_MESHCODEC and this file resolves no model, which makes the preview fall back to each part's collision body (the
// same fallback the reference viewer uses for the actors it has no model for).
//
// The data path mirrors runtime/blueprint_shape.c, which already reads Pack/Actor/*.pack.zs from the RomFS, unpacks the
// SARC with the game's zstd dictionary, and walks BYML. The shared helpers (bp_romfs_file, sarc_each_matching) live
// there.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

#include "bfres.h"
#include "blueprint_shape.h"
#include "byml.h"
#include "host.h"
#include "model_meshcodec.h"

// The biggest decoded model the cache keeps (a few hundred KiB body plus the FMSH geometry the game reserves).
#define MODEL_MAX (1u << 20)

typedef struct {
    char name[96];              // the actor name or model name this entry is cached under
    int valid;
    uint8_t *buf;               // the decoded BFRES buffer: body, then the geometry pool (bfres.h expects this)
    size_t buf_n;
    BfresModel model;
    float extent;               // the largest side of the decoded geometry; a fused material is scaled to fit by it
} ModelEntry;

#define CACHE_MAX 24
static ModelEntry g_cache[CACHE_MAX];

// ---- the zstd dictionary (the same one blueprint_shape.c loads; re-read here so this file is self-contained)

static uint8_t *g_dict;
static size_t g_dict_n;
static int g_dict_tried;

static int dict_ready(void) {
    if (g_dict) return 1;
    if (g_dict_tried) return 0;
    g_dict_tried = 1;
    size_t dn = 0;
    uint8_t *dz = bp_romfs_file("Pack/ZsDic.pack.zs", &dn);
    if (!dz) return 0;
    unsigned long long rn = ZSTD_getFrameContentSize(dz, dn);
    uint8_t *raw = (rn != ZSTD_CONTENTSIZE_UNKNOWN && rn != ZSTD_CONTENTSIZE_ERROR && rn <= MODEL_MAX)
                       ? malloc((size_t)rn) : NULL;
    if (raw && ZSTD_decompress(raw, (size_t)rn, dz, dn) == rn && rn >= 6) {
        uint16_t head, count;
        uint32_t base;
        memcpy(&head, raw + 4, 2);
        // Every field this walk reads is bounds-checked first: the archive is game data and a truncated or corrupt one
        // must be refused rather than read past its end. `head` is the header size, `count` the entry count, and the
        // entry table sits at head + 12 with 16 bytes each; the name table follows it.
        if ((size_t)head + 8 > rn) { free(raw); free(dz); return 0; }
        memcpy(&count, raw + head + 6, 2);
        memcpy(&base, raw + 12, 4);
        const size_t entries = (size_t)head + 12;
        const size_t names = entries + (size_t)count * 16 + 8;
        if (entries + (size_t)count * 16 + 8 > rn || names > rn) { free(raw); free(dz); return 0; }
        for (unsigned i = 0; i < count; i++) {
            uint32_t attr, lo, hi;
            size_t e = entries + (size_t)i * 16;
            memcpy(&attr, raw + e + 4, 4);
            memcpy(&lo, raw + e + 8, 4);
            memcpy(&hi, raw + e + 12, 4);
            const size_t name_off = names + (size_t)(attr & 0xffffff) * 4;
            // The name has to be terminated INSIDE the buffer before strcmp may walk it: name_off < rn only proves its
            // first byte is in range, and a name that runs to the end of a corrupt archive would be read past it.
            if (name_off >= rn || !memchr(raw + name_off, 0, rn - name_off)) break;
            const char *name = (const char *)raw + name_off;
            if (!strcmp(name, "pack.zsdic")) {
                if (hi < lo || (uint64_t)base + hi > rn) break;
                const size_t mn = (size_t)hi - lo;
                g_dict = mn ? malloc(mn) : NULL;
                if (g_dict) { memcpy(g_dict, raw + base + lo, mn); g_dict_n = mn; }
                break;
            }
        }
    }
    free(raw);
    free(dz);
    return g_dict != NULL;
}

// A member of a SARC, passed to find_fmdb's scan.
//
// A ModelInfo can name more than one model: 87 of the catalogue's actors do. A weapon lists its base variant first and its
// own model later (Weapon_Shield_103 carries Weapon_Shield_101 and Weapon_Shield_103), a balloon lists the base envelope
// then the cloth. Taking the first name draws the base variant - a different, smaller object than the part really is,
// which is what made an imported shield look wrong. So every name is collected, and the one that matches the actor wins;
// an actor whose list never names it (a tree trunk lists Obj_Log_* and the tree) keeps the first.
#define FMDB_MAX 8
typedef struct {
    const char *actor;
    char names[FMDB_MAX][64];
    int count;
} FmdbCtx;

// Adds one name to the list, ignoring duplicates.
static void fmdb_add(FmdbCtx *ctx, const char *s) {
    if (!s || !*s || ctx->count >= FMDB_MAX) return;
    for (int i = 0; i < ctx->count; i++)
        if (!strcmp(ctx->names[i], s)) return;
    snprintf(ctx->names[ctx->count], sizeof ctx->names[0], "%s", s);
    ctx->count++;
}

// Descend one BYML node collecting every "FmdbName" hash entry with a string value.
// Descends the document looking for FmdbName. A BYML may point a container entry at a node that is already on the way
// down (a self-referencing hash, or an array whose element offset is the array), and nothing in byml_open rejects that:
// the walk therefore carries a depth budget, which a real ModelInfo stays far below. Without it such a document
// recurses until the stack runs out.
static void byml_find_fmdb_depth(const BymlDoc *doc, uint32_t node, FmdbCtx *ctx, int depth) {
    if (depth <= 0) return;
    uint8_t t = byml_type(doc, node);
    if (t == BYML_HASH) {
        size_t n = byml_count(doc, node);
        for (size_t i = 0; i < n; i++) {
            const char *k = byml_entry_key(doc, node, i);
            if (k && !strcmp(k, "FmdbName")) {
                // The entry's node offset (byml_key returns the value node's offset, not a container).
                uint32_t v = byml_key(doc, node, "FmdbName");
                if (v && byml_type(doc, v) == BYML_STRING) fmdb_add(ctx, byml_string(doc, v));
            }
            // Descend into container values only (byml_at is 0 for value-typed entries).
            uint32_t v = byml_at(doc, node, i);
            if (v) byml_find_fmdb_depth(doc, v, ctx, depth - 1);
        }
    } else if (t == BYML_ARRAY) {
        size_t n = byml_count(doc, node);
        for (size_t i = 0; i < n; i++) {
            uint32_t v = byml_at(doc, node, i);
            if (v) byml_find_fmdb_depth(doc, v, ctx, depth - 1);
        }
    }
}

static void byml_find_fmdb(const BymlDoc *doc, uint32_t node, FmdbCtx *ctx) {
    byml_find_fmdb_depth(doc, node, ctx, 32);
}

// Every ModelInfo member of the SARC, collecting its model names.
static int fmdb_member(const char *name, const uint8_t *data, size_t size, void *vctx) {
    FmdbCtx *ctx = vctx;
    if (!strstr(name, "ModelInfo")) return 1;
    BymlDoc doc;
    if (!byml_open(&doc, data, size)) return 1;
    byml_find_fmdb(&doc, byml_root(&doc), ctx);
    return 1;
}

// The actor's model name from its own ModelInfo. The member name is NOT always the actor name (a propeller's says
// DgnObj_SkyIsland_Propeller_A_01), so every ModelInfo member is tried.
static int find_fmdb(const char *actor, char *out, size_t cap) {
    out[0] = 0;
    char rel[160];
    snprintf(rel, sizeof rel, "Pack/Actor/%s.pack.zs", actor);
    size_t zn = 0;
    uint8_t *z = bp_romfs_file(rel, &zn);
    if (!z || !dict_ready()) { free(z); return 0; }
    uint8_t *raw = NULL;
    size_t raw_n = 0;
    unsigned long long rn = ZSTD_getFrameContentSize(z, zn);
    unsigned id = ZSTD_getDictID_fromFrame(z, zn);
    ZSTD_DDict *dd = ZSTD_createDDict(g_dict, g_dict_n);
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    if (rn != ZSTD_CONTENTSIZE_UNKNOWN && rn != ZSTD_CONTENTSIZE_ERROR && rn <= MODEL_MAX && (!id || dd)) {
        raw = malloc((size_t)rn);
        size_t got = raw && dctx ? ZSTD_decompress_usingDDict(dctx, raw, (size_t)rn, z, zn, dd) : 0;
        if (!raw || !dctx || ZSTD_isError(got) || got != rn) { free(raw); raw = NULL; }
        else raw_n = (size_t)rn;
    }
    ZSTD_freeDCtx(dctx); ZSTD_freeDDict(dd); free(z);
    if (!raw) return 0;
    FmdbCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.actor = actor;
    sarc_each_matching(raw, raw_n, "", "", fmdb_member, &ctx);
    free(raw);
    if (ctx.count == 0) return 0;
    // The actor's own name wins when its ModelInfo lists it: that entry is the model the game ships for this actor, and
    // the earlier names are the family's base variants. Everything else keeps the first name, which is how an actor whose
    // ModelInfo names only a related model (a tree trunk) still resolves.
    for (int i = 0; i < ctx.count; i++) {
        if (!strcmp(ctx.names[i], actor)) { snprintf(out, cap, "%s", ctx.names[i]); return 1; }
    }
    // Every name the ModelInfo listed, when the actor's own name was not among them. The choice below then has to be a
    // guess, so the log should show what the guess was made from: a ModelInfo that names only a scene group (Group00)
    // leaves nothing to draw, and one that also names a part has an answer that is better than the first entry.
    if (ctx.count > 1 || !strncmp(ctx.names[0], "Group", 5)) {
        char all[512] = {0};
        int off = 0;
        for (int i = 0; i < ctx.count && off < (int)sizeof all - 80; i++)
            off += snprintf(all + off, sizeof all - (size_t)off, "%s%s", i ? " " : "", ctx.names[i]);
        rt_log("model: %s ModelInfo lists %d names: %s", actor, ctx.count, all);
    }
    snprintf(out, cap, "%s", ctx.names[0]);
    return 1;
}

// The full decoded model for a model name, cached under `key`. The name is the Fmdb name the game's data uses; an actor
// reaches this through its ModelInfo, and a fused material names a model directly (its SubModel), so both paths share it.
static ModelEntry *model_entry_named(const char *key, const char *fmdb) {
    for (int i = 0; i < CACHE_MAX; i++)
        if (g_cache[i].valid && !strcmp(g_cache[i].name, key)) return &g_cache[i];
    ModelEntry *slot = NULL;
    for (int i = 0; i < CACHE_MAX; i++)
        if (!g_cache[i].valid) { slot = &g_cache[i]; break; }
    if (!slot) { static int cur; slot = &g_cache[cur]; cur = (cur + 1) % CACHE_MAX; }
    slot->valid = 0;

    // The model file is Model/<project>.<FmdbName>.bfres.mc. The `project` half is the actor's own family, NOT the
    // FmdbName: a scene-group actor is Model/HiddenKorok_SilhouettePazzle_027.Group00.bfres.mc, so the first guess has to
    // be "<key>.<fmdb>" - guessing "<fmdb>.<fmdb>" produced "Group00.Group00" and existed nowhere, which is why every
    // group-named actor fell back to its collision body. The FmdbName-only form is kept as a second try because a part
    // whose family and FmdbName agree hits it directly.
    char rel[256];
    snprintf(rel, sizeof rel, "Model/%s.%s.bfres.mc", key, fmdb);
    size_t mn = 0;
    uint8_t *mc = bp_romfs_file(rel, &mn);
    if (!mc && strcmp(key, fmdb)) {
        snprintf(rel, sizeof rel, "Model/%s.%s.bfres.mc", fmdb, fmdb);
        mc = bp_romfs_file(rel, &mn);
    }
    if (!mc) {
        extern const char *romfs_entry(uint64_t i, uint64_t *offset, uint64_t *size);
        extern uint64_t romfs_file_count(void);
        char suffix[160];
        snprintf(suffix, sizeof suffix, ".%s.bfres.mc", fmdb);
        const size_t sl = strlen(suffix);
        uint64_t nfiles = romfs_file_count();
        for (uint64_t i = 0; i < nfiles; i++) {
            uint64_t off = 0, sz = 0;
            const char *p = romfs_entry(i, &off, &sz);
            if (!p || strncmp(p, "Model/", 6)) continue;
            const size_t pl = strlen(p);
            if (pl >= sl && !strcmp(p + pl - sl, suffix)) {
                mc = bp_romfs_file(p, &mn);
                break;
            }
        }
    }
    if (!mc) {
        rt_log("model: no model for %s (FmdbName %s)", key, fmdb);
        return NULL;
    }
    if (mn < 12 || memcmp(mc, "MCPK", 4)) { free(mc); return NULL; }
    uint32_t flags; memcpy(&flags, mc + 8, 4);
    size_t need = (size_t)((flags >> 5) << (flags & 0xf));
    if (need > MODEL_MAX) { free(mc); return NULL; }
    uint8_t *buf = malloc(need);
    size_t out_n = 0;
#ifdef TOTK_MESHCODEC
    int ok = buf ? model_meshcodec_decode(mc, mn, buf, need, &out_n) : 0;
#else
    int ok = 0;   // the decoder is not linked into this build (see the file comment)
#endif
    free(mc);
    if (!ok || !bfres_open(&slot->model, buf, out_n)) {
        rt_log("model: decode failed for %s (meshcodec %d, body %zu)", key, ok, out_n);
        free(buf);
        return NULL;
    }
    // A part in an Autobuild draft is a piece a person could pick up: a few metres at most. A ModelInfo can point at a
    // scene group instead (HiddenKorok_SilhouettePazzle_006 resolves to CaveEntrance_02197d15.Group00, a 126 km terrain
    // sheet), and drawing that puts one enormous face across the whole preview. Measure the decoded geometry and refuse
    // such a model so the preview falls back to the part's collision body, which is always the right size.
    {
        const float kMaxPartExtent = 40.0f;   // metres; the largest real part (a big chassis) stays far below this
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        int any = 0;
        for (int s = 0; s < slot->model.count; s++) {
            const int nv = bfres_shape_vertices(&slot->model, s);
            const int nt = bfres_shape_triangles(&slot->model, s);
            if (nv <= 0 || nt <= 0 || nv > 65535) continue;
            float *vs = malloc((size_t)nv * 3 * sizeof(float));
            uint16_t *ts = malloc((size_t)nt * 3 * sizeof(uint16_t));
            if (!vs || !ts) { free(vs); free(ts); break; }
            const int got = bfres_shape_geometry(&slot->model, s, vs, nv, ts, nt);
            if (got > 0) {
                for (int v = 0; v < got; v++)
                    for (int a = 0; a < 3; a++) {
                        if (vs[3 * v + a] < lo[a]) lo[a] = vs[3 * v + a];
                        if (vs[3 * v + a] > hi[a]) hi[a] = vs[3 * v + a];
                    }
                any = 1;
            }
            free(vs);
            free(ts);
        }
        if (any) {
            float extent = 0.0f;
            for (int a = 0; a < 3; a++) {
                const float e = hi[a] - lo[a];
                if (e > extent) extent = e;
            }
            slot->extent = extent;   // kept even when the guard passes: a fused material is scaled to fit by it
            if (extent > kMaxPartExtent) {
                rt_log("model: %s resolves to a %.0f m scene group, not a part; using its collision body", key, extent);
                free(buf);
                return NULL;
            }
        }
    }
    // The resolved model name is logged next to the actor because that pairing is what a wrong-looking part is diagnosed
// from: an actor whose ModelInfo lists several models can be drawn from the wrong one, and the shape count alone does not
    // say which.
    rt_log("model: %s <- %s decoded at runtime (%d shapes, %zu bytes)", key, fmdb, slot->model.count, out_n);
    snprintf(slot->name, sizeof slot->name, "%s", key);
    slot->buf = buf;
    slot->buf_n = out_n;
    slot->valid = 1;
    return slot;
}

// The decoded model of actor, or NULL (no pack, no ModelInfo, no model, or the MeshCodec bridge is missing). The model the
// actor's ModelInfo names is what is drawn, so the actor path is: pack -> ModelInfo -> model name -> this loader.
const BfresModel *model_romfs_resolve(const char *actor) {
    if (!actor || !*actor) return NULL;
    char fmdb[128];
    if (!find_fmdb(actor, fmdb, sizeof fmdb)) {
        rt_log("model: no ModelInfo for %s", actor);
        return NULL;
    }
    // A ModelInfo can name a scene group instead of a part: HiddenKorok_SilhouettePazzle_027 names Group00, which is the
    // whole area. Drawing a group puts a sheet over the preview in place of the part - one case resolved to a 126 km
    // terrain sheet - so a group is never loaded as a part's model.
    //
    // It is not the end of the search, though: that same actor DOES have a part model, and its ModelInfo simply does not
    // name it (the group is the only entry). The model file is named after the ACTOR in that case, which is the fallback
    // below. It is tried only for a group name, so an actor whose ModelInfo names a real part keeps using that.
    if (!strncmp(fmdb, "Group", 5)) {
        // The actor's own model file is named Model/<actor>.<FmdbName>.bfres.mc: the actor in the first half and the
        // group in the second. Reading the first half from the FmdbName instead produced "Model/Group00.Group00" for every
        // such actor, which exists for none of them, so all 339 group-named models fell back to their collision bodies -
        // the abstract polygons that stood in for koroks and cave entrances.
        ModelEntry *direct = model_entry_named(actor, fmdb);
        if (direct) {
            rt_log("model: %s names the scene group %s, not a part; its own model file was found and is used instead", actor,
                   fmdb);
            return &direct->model;
        }
        rt_log("model: %s names the scene group %s and has no model file of its own; using its collision body", actor, fmdb);
        return NULL;
    }
    ModelEntry *e = model_entry_named(actor, fmdb);
    return e ? &e->model : NULL;
}

// The largest side of a decoded model's geometry, in metres; 0 when it is not known. A fused material whose record carries
// BoundBoxLength is scaled to that length.
float model_romfs_extent(const BfresModel *m) {
    if (!m) return 0.0f;
    for (int i = 0; i < CACHE_MAX; i++)
        if (g_cache[i].valid && &g_cache[i].model == m) return g_cache[i].extent;
    return 0.0f;
}

// The decoded model for a model name, for callers that already have one: a fused material's SubModel points at a model
// file, and those names are not actors, so they never pass through a ModelInfo.
const BfresModel *model_romfs_resolve_model(const char *model_name) {
    if (!model_name || !*model_name) return NULL;
    ModelEntry *e = model_entry_named(model_name, model_name);
    return e ? &e->model : NULL;
}
