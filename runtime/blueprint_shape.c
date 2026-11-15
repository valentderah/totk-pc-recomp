// The collision shapes of a blueprint's parts (blueprint_shape.h): resolve an actor name to a triangle mesh by reading
// its pack from the RomFS, unpacking it, and walking the ShapeParam BYML files.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "blueprint_shape.h"
#include "byml.h"
#include "fs.h"
#include "host.h"
#include "hull.h"

#define PACK_MAX (256u << 20)   // the largest pack taken, decompressed (matches romfs.c's bound)
#define CACHE_MAX 32            // distinct part names remembered; a 21-part blueprint fits, and one body per name

// ---- the SARC members of a decompressed pack

// The members of the SARC at p[0..n): name, data pointer and size, in SFAT order, visiting the callback until it
// returns 0. A member whose name starts with `prefix` but contains a word of `exclude` ("|"-separated) is skipped.
// Returns the number visited.
int sarc_each_matching(const uint8_t *p, size_t n, const char *prefix, const char *exclude,
                       int (*fn)(const char *name, const uint8_t *data, size_t size, void *ctx), void *ctx) {
    uint16_t head, count, bom;
    uint32_t base;
    if (n < 0x14 || memcmp(p, "SARC", 4)) return 0;
    memcpy(&head, p + 4, 2);
    memcpy(&bom, p + 6, 2);
    if (bom != 0xfeff || head + 12u > n || memcmp(p + head, "SFAT", 4)) return 0;
    memcpy(&count, p + head + 6, 2);
    memcpy(&base, p + 12, 4);
    size_t names = head + 12 + (size_t)count * 16;
    if (names + 8 > n || memcmp(p + names, "SFNT", 4)) return 0;
    names += 8;
    int seen = 0;
    for (unsigned i = 0; i < count; i++) {
        uint32_t attr, lo, hi;
        size_t e = head + 12 + (size_t)i * 16;
        if (e + 16 > n) break;
        memcpy(&attr, p + e + 4, 4);
        memcpy(&lo, p + e + 8, 4);
        memcpy(&hi, p + e + 12, 4);
        size_t s = names + (size_t)(attr & 0xffffff) * 4;
        if (!(attr >> 24) || s >= n) continue;
        const char *name = (const char *)p + s;
        if (!memchr(name, 0, n - s)) continue;
        if (strncmp(name, prefix, strlen(prefix))) continue;
        if (exclude && *exclude) {
            int bad = 0;
            for (const char *w = exclude; *w;) {
                const char *bar = strchr(w, '|');
                size_t wl = bar ? (size_t)(bar - w) : strlen(w);
                // A substring match without memmem (not in the mingw headers).
                for (const char *q = name; *q; q++)
                    if (!strncmp(q, w, wl)) { bad = 1; break; }
                if (bad) break;
                if (!bar) break;
                w = bar + 1;
            }
            if (bad) continue;
        }
        if (lo > hi || base + (size_t)hi > n) continue;
        if (!fn(name, p + base + lo, hi - lo, ctx)) break;
        seen++;
    }
    return seen;
}

// ---- primitive point clouds (the tessellation the Python prototype proved fills 86-99% of the game's framing box)

static void cloud_add(float *cloud, int *nv, const float p[3]) {
    if (*nv >= BP_SHAPE_MAX_VERTS) return;
    cloud[3 * (*nv)] = p[0];
    cloud[3 * (*nv) + 1] = p[1];
    cloud[3 * (*nv) + 2] = p[2];
    (*nv)++;
}

static void cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static void norm3(float v[3]) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-12f) {
        v[0] /= l; v[1] /= l; v[2] /= l;
    }
}

// n points of a circle around `c` in the plane spanned by unit vectors u and v, radius r.
static void ring(float *cloud, int *nv, const float c[3], const float u[3], const float v[3], float r, int n) {
    for (int k = 0; k < n; k++) {
        float a = 2.0f * 3.14159265f * k / n, ca = cosf(a), sa = sinf(a);
        float p[3] = {c[0] + r * (ca * u[0] + sa * v[0]), c[1] + r * (ca * u[1] + sa * v[1]),
                      c[2] + r * (ca * u[2] + sa * v[2])};
        cloud_add(cloud, nv, p);
    }
}

// The vertices of an n-gon prism between a and b, radius r: two rings, which hull_build turns into a solid.
static void cylinder_cloud(float *cloud, int *nv, const float a[3], const float b[3], float r, int n) {
    float axis[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    float len = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (len < 1e-9f) return;
    axis[0] /= len; axis[1] /= len; axis[2] /= len;
    float u[3] = {fabsf(axis[0]) < 0.9f ? 1.0f : 0.0f, fabsf(axis[0]) < 0.9f ? 0.0f : 1.0f, 0.0f};
    float v[3];
    cross(axis, u, v);
    norm3(v);
    cross(v, axis, u);
    ring(cloud, nv, a, u, v, r, n);
    ring(cloud, nv, b, u, v, r, n);
}

// A coarse sphere: 7 parallels of 8 longitudes plus the poles (58 points), enough for a wheel hub or a cannon ball.
static void sphere_cloud(float *cloud, int *nv, const float c[3], float r) {
    for (int i = 1; i < 8; i++) {
        float phi = 3.14159265f * i / 8.0f;
        float rr = r * sinf(phi), y = r * cosf(phi);
        for (int k = 0; k < 8; k++) {
            float th = 2.0f * 3.14159265f * k / 8.0f;
            cloud_add(cloud, nv, (float[]){c[0] + rr * cosf(th), c[1] + y, c[2] + rr * sinf(th)});
        }
    }
    cloud_add(cloud, nv, (float[]){c[0], c[1] + r, c[2]});
    cloud_add(cloud, nv, (float[]){c[0], c[1] - r, c[2]});
}

// The eight corners of a box: centre c, half extents h, with rotation r (3x3 row-major) applied to the corner offsets.
static void box_cloud(float *cloud, int *nv, const float c[3], const float h[3], const float r[9]) {
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
            for (int sz = -1; sz <= 1; sz += 2) {
                float p[3] = {sx * h[0], sy * h[1], sz * h[2]};
                if (r) {
                    float q[3] = {r[0] * p[0] + r[1] * p[1] + r[2] * p[2],
                                  r[3] * p[0] + r[4] * p[1] + r[5] * p[2],
                                  r[6] * p[0] + r[7] * p[1] + r[8] * p[2]};
                    p[0] = q[0]; p[1] = q[1]; p[2] = q[2];
                }
                cloud_add(cloud, nv, (float[]){c[0] + p[0], c[1] + p[1], c[2] + p[2]});
            }
}

// A point transformed by a 3x3 row-major rotation r and a translation t, in that order.
static void xform(float *out, const float p[3], const float r[9], const float t[3]) {
    out[0] = r[0] * p[0] + r[1] * p[1] + r[2] * p[2] + t[0];
    out[1] = r[3] * p[0] + r[4] * p[1] + r[5] * p[2] + t[1];
    out[2] = r[6] * p[0] + r[7] * p[1] + r[8] * p[2] + t[2];
}

// The rotation matrix of an OffsetRotation, which the game stores in degrees applied X then Y then Z.
static void euler_degrees(const float deg[3], float r[9]) {
    float x = deg[0] * 3.14159265f / 180.0f, y = deg[1] * 3.14159265f / 180.0f, z = deg[2] * 3.14159265f / 180.0f;
    float cx = cosf(x), sx = sinf(x), cy = cosf(y), sy = sinf(y), cz = cosf(z), sz = sinf(z);
    r[0] = cy * cz;            r[1] = cz * sx * sy - cx * sz;  r[2] = cx * cz * sy + sx * sz;
    r[3] = cy * sz;            r[4] = cx * cz + sx * sy * sz;  r[5] = -cz * sx + cx * sy * sz;
    r[6] = -sy;                r[7] = cy * sx;                 r[8] = cx * cy;
}

// ---- the BYML shape walk

// The X/Y/Z floats of the hash node `node` named `field`, all three zero when any is missing or not a float hash.
static void v3_of(const BymlDoc *d, uint32_t node, const char *field, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    uint32_t v = byml_key(d, node, field);
    if (!v || byml_type(d, v) != BYML_HASH) return;
    out[0] = (float)byml_number(d, byml_key(d, v, "X"), 0.0);
    out[1] = (float)byml_number(d, byml_key(d, v, "Y"), 0.0);
    out[2] = (float)byml_number(d, byml_key(d, v, "Z"), 0.0);
}

// The offset a primitive applies: its OffsetTranslation and its OffsetRotation as a row-major matrix (identity when
// absent or all-zero).
static void primitive_xform(const BymlDoc *d, uint32_t node, float r[9], float t[3]) {
    v3_of(d, node, "OffsetTranslation", t);
    float deg[3];
    v3_of(d, node, "OffsetRotation", deg);
    if (deg[0] == 0.0f && deg[1] == 0.0f && deg[2] == 0.0f) {
        memset(r, 0, sizeof(float) * 9);
        r[0] = r[4] = r[8] = 1.0f;
    } else {
        euler_degrees(deg, r);
    }
}

// One primitive `node` (a hash inside a "Cylinder"/"Sphere"/"Box"/"Capsule"/"Polytope" array) added to the actor-space
// point set. The primitive is built in its own frame, then its OffsetRotation and OffsetTranslation are applied.
static void add_primitive(const BymlDoc *d, uint32_t node, const char *kind, float *cloud, int *nv) {
    float r[9], t[3];
    primitive_xform(d, node, r, t);
    float local[BP_SHAPE_MAX_VERTS * 3];
    int ln = 0;
    if (!strcmp(kind, "Cylinder") || !strcmp(kind, "Capsule")) {
        float a[3], b[3];
        v3_of(d, node, "CenterA", a);
        v3_of(d, node, "CenterB", b);
        float radius = (float)byml_number(d, byml_key(d, node, "Radius"), 0.0);
        cylinder_cloud(local, &ln, a, b, radius, 16);
    } else if (!strcmp(kind, "Sphere")) {
        float c[3];
        v3_of(d, node, "Center", c);
        float radius = (float)byml_number(d, byml_key(d, node, "Radius"), 0.0);
        sphere_cloud(local, &ln, c, radius);
    } else if (!strcmp(kind, "Box")) {
        float c[3], h[3];
        v3_of(d, node, "Center", c);
        v3_of(d, node, "HalfExtents", h);
        box_cloud(local, &ln, c, h, NULL);
    } else if (!strcmp(kind, "Polytope")) {
        uint32_t verts = byml_key(d, node, "Vertices");
        size_t count = verts && byml_type(d, verts) == BYML_ARRAY ? byml_count(d, verts) : 0;
        for (size_t i = 0; i < count && ln < BP_SHAPE_MAX_VERTS; i++) {
            uint32_t v = byml_at(d, verts, i);   // each vertex is a hash with X/Y/Z floats
            if (!v) continue;
            local[3 * ln] = (float)byml_number(d, byml_key(d, v, "X"), 0.0);
            local[3 * ln + 1] = (float)byml_number(d, byml_key(d, v, "Y"), 0.0);
            local[3 * ln + 2] = (float)byml_number(d, byml_key(d, v, "Z"), 0.0);
            ln++;
        }
    }
    for (int i = 0; i < ln; i++) {
        float p[3] = {local[3 * i], local[3 * i + 1], local[3 * i + 2]}, q[3];
        xform(q, p, r, t);
        cloud_add(cloud, nv, q);
    }
}

// Whether a ShapeParam member name says the shape is not the body: attacks, targets, chemical volumes, navigation
// meshes, sensors and attachment/animation-only shapes are not the visible body.
static int is_body_name(const char *name) {
    static const char *const words[] = {"Atk", "Tgt", "Chemical", "NavMeshCut", "EntitySensor", "Sensor", "Attach",
                                        "Attention", "Counter"};
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++)
        if (strstr(name, words[i])) return 0;
    return 1;
}

typedef struct { float *cloud; int nv; } CloudCtx;

// One ShapeParam member: walk its BYML and add every primitive array's entries to the cloud.
static int shape_member(const char *name, const uint8_t *data, size_t size, void *ctx) {
    if (!is_body_name(name)) return 1;   // keep enumerating
    CloudCtx *cc = ctx;
    BymlDoc d;
    if (!byml_open(&d, data, size)) return 1;
    uint32_t root = byml_root(&d);
    if (byml_type(&d, root) != BYML_HASH) return 1;
    static const char *const kinds[] = {"Box", "Sphere", "Cylinder", "Capsule", "Polytope"};
    for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        uint32_t arr = byml_key(&d, root, kinds[k]);
        size_t count = arr && byml_type(&d, arr) == BYML_ARRAY ? byml_count(&d, arr) : 0;
        for (size_t i = 0; i < count; i++) {
            uint32_t item = byml_at(&d, arr, i);   // a container: the primitive hash
            if (item) add_primitive(&d, item, kinds[k], cc->cloud, &cc->nv);
        }
    }
    return 1;
}

// ---- the cache and the dictionary

typedef struct { char name[128]; BpShapeMesh mesh; int valid; } ShapeEntry;
static ShapeEntry g_cache[CACHE_MAX];
static uint8_t *g_dict;
static size_t g_dict_n;
static int g_dict_tried;

// The RomFS mount the game gave its content archive: the game calls nn::fs::MountRom itself and TotK names it
// "content", but another name would leave every open below failing, so the first successful open decides it and is
// kept. An empty string means "not known yet".
static char g_mount[16];

// The whole file at RomFS path `rel` (no mount prefix), malloc'd; NULL when it is not in the archive. Shared with
// model_romfs.c, which reads the same way for the model files.
uint8_t *bp_romfs_file(const char *rel, size_t *out_n) {
    *out_n = 0;
    static const char *const mounts[] = {"content", "rom"};
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt == 0 && g_mount[0]) continue;             // the mount is already known: only try that one
        const char *mount = g_mount[0] ? g_mount : mounts[attempt];
        char path[256];
        if (snprintf(path, sizeof path, "%s:/%s", mount, rel) >= (int)sizeof path) return NULL;
        uint64_t h = 0;
        if (romfs_open(&h, path, FS_OPEN_READ) != 0) continue;
        int64_t n = 0;
        if (romfs_size(&n, h) != 0 || n <= 0 || n > (int64_t)PACK_MAX) {
            romfs_close(h);
            continue;
        }
        uint8_t *buf = malloc((size_t)n);
        uint64_t got = 0;
        if (buf) romfs_read(&got, h, 0, buf, (uint64_t)n);
        romfs_close(h);
        if (!buf || got != (uint64_t)n) {
            free(buf);
            return NULL;
        }
        snprintf(g_mount, sizeof g_mount, "%s", mount);   // remember what worked
        *out_n = (size_t)n;
        return buf;
    }
    return NULL;
}

// The "pack.zsdic" member of Pack/ZsDic.pack.zs, loaded once into g_dict. Returns 1 when ready (g_dict set), 0 when the
// dictionary cannot be read. It is the same dictionary camera_speed.c uses for the camera pack.
static int dict_ready(void) {
    if (g_dict) return 1;
    if (g_dict_tried) return 0;
    g_dict_tried = 1;
    size_t dn = 0;
    uint8_t *dz = bp_romfs_file("Pack/ZsDic.pack.zs", &dn);
    if (!dz) return 0;
    // Pack/ZsDic.pack.zs is itself a SARC compressed without a dictionary; unpack it, then take its "pack.zsdic".
    unsigned long long rn = ZSTD_getFrameContentSize(dz, dn);
    uint8_t *raw = (rn != ZSTD_CONTENTSIZE_UNKNOWN && rn != ZSTD_CONTENTSIZE_ERROR && rn <= PACK_MAX)
                       ? malloc((size_t)rn) : NULL;
    if (raw && ZSTD_decompress(raw, (size_t)rn, dz, dn) == rn && rn >= 6) {
        uint16_t head, count, bom;
        uint32_t base;
        memcpy(&head, raw + 4, 2);
        memcpy(&bom, raw + 6, 2);
        // Every field read below is bounds-checked first: the archive is game data, and a truncated or corrupt one must
        // be refused rather than read past its end. The entry table sits at head + 12 with 16 bytes per entry, and the
        // name table follows it.
        if ((size_t)head + 8 > rn) { free(raw); free(dz); return 0; }
        memcpy(&count, raw + head + 6, 2);
        memcpy(&base, raw + 12, 4);
        const size_t entries = (size_t)head + 12;
        const size_t names = entries + (size_t)count * 16 + 8;
        if (names > rn) { free(raw); free(dz); return 0; }
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
                if (g_dict) {
                    memcpy(g_dict, raw + base + lo, mn);
                    g_dict_n = mn;
                }
                break;
            }
        }
    }
    free(raw);
    free(dz);
    return g_dict != NULL;
}

// The decompressed Pack/Actor/<actor>.pack.zs, with the game's own zstd dictionary applied. malloc'd, the caller frees it.
// Shared with runtime/blueprint_attach.c, which reads a material's AttachmentParam out of the same kind of pack. Returns
// NULL for a missing pack, an unreadable frame or a pack larger than PACK_MAX.
uint8_t *bp_actor_pack(const char *actor, size_t *out_n) {
    if (out_n) *out_n = 0;
    if (!actor || !*actor) return NULL;
    char rel[160];
    snprintf(rel, sizeof rel, "Pack/Actor/%s.pack.zs", actor);
    size_t zn = 0;
    uint8_t *z = bp_romfs_file(rel, &zn);
    if (!z) return NULL;
    if (!dict_ready()) {
        free(z);
        return NULL;
    }
    uint8_t *raw = NULL;
    const unsigned long long rn = ZSTD_getFrameContentSize(z, zn);
    const unsigned id = ZSTD_getDictID_fromFrame(z, zn);
    ZSTD_DDict *dd = ZSTD_createDDict(g_dict, g_dict_n);
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    if (rn != ZSTD_CONTENTSIZE_UNKNOWN && rn != ZSTD_CONTENTSIZE_ERROR && rn <= PACK_MAX && (!id || dd)) {
        raw = malloc((size_t)rn);
        size_t got = raw && dctx ? ZSTD_decompress_usingDDict(dctx, raw, (size_t)rn, z, zn, dd) : 0;
        if (!raw || !dctx || ZSTD_isError(got) || got != rn) {
            free(raw);
            raw = NULL;
        } else if (out_n) {
            *out_n = (size_t)rn;
        }
    }
    ZSTD_freeDCtx(dctx);
    ZSTD_freeDDict(dd);
    free(z);
    return raw;
}

const BpShapeMesh *bp_shape_resolve(const char *name) {
    if (!name || !*name) return NULL;
    // A cached mesh for this name is returned as-is: re-reading and re-triangulating on every camera drag would hitch the
    // drag, and a part name means the same geometry everywhere in a build.
    for (int i = 0; i < CACHE_MAX; i++)
        if (g_cache[i].valid && !strcmp(g_cache[i].name, name)) return &g_cache[i].mesh;

    // A slot: the first empty one, or the oldest when full (a simple ring). Re-resolving a part the ring evicted is still
    // correct, just not free; a 21-part blueprint never evicts anything.
    ShapeEntry *slot = NULL;
    for (int i = 0; i < CACHE_MAX; i++)
        if (!g_cache[i].valid) { slot = &g_cache[i]; break; }
    if (!slot) {
        static int cursor;
        slot = &g_cache[cursor];
        cursor = (cursor + 1) % CACHE_MAX;
    }
    slot->valid = 0;

    size_t raw_n = 0;
    uint8_t *raw = bp_actor_pack(name, &raw_n);
    if (!raw) {
        // Logged once per name: a silent fallback to a box is what made this look like it did nothing at all.
        rt_log("blueprint: no pack for %s; its preview stays a box", name);
        return NULL;
    }

    float cloud[BP_SHAPE_MAX_VERTS * 3];
    CloudCtx cc = {cloud, 0};
    sarc_each_matching(raw, raw_n, "Phive/ShapeParam/", "", shape_member, &cc);

    // No body shapes: the part stays a box in the caller.
    if (cc.nv < 4) {
        free(raw);
        return NULL;
    }

    snprintf(slot->name, sizeof slot->name, "%s", name);
    slot->mesh.nverts = cc.nv;
    for (int i = 0; i < cc.nv; i++) {
        slot->mesh.verts[3 * i] = cloud[3 * i];
        slot->mesh.verts[3 * i + 1] = cloud[3 * i + 1];
        slot->mesh.verts[3 * i + 2] = cloud[3 * i + 2];
    }
    slot->mesh.ntris = hull_build(slot->mesh.verts, cc.nv, slot->mesh.tris, BP_SHAPE_MAX_TRIS);
    free(raw);
    if (slot->mesh.ntris <= 0) return NULL;
    slot->valid = 1;
    return &slot->mesh;
}
