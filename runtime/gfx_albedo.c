// Where a G-buffer fragment shader's albedo comes from (docs/raytracing.md, materials step 1): the texture its albedo output
// (colour target 1, R8G8B8A8_SRGB) samples, the varying the sample's coordinate reads and, for a 2D array texture,
// the uniform its layer comes from. Ray hits off the screen need that colour; the scene capture records the varying
// and the draw's texture.
//
// A backward slice over the SPIR-V the translator wrote, flow-insensitive: a load from a variable depends on every
// store to it. From the stores to the output, the image sample reached first (fewest steps) is the albedo's. Its
// coordinate is a Select(IsNan(c), 0, c) of a CompositeConstruct; each member is sliced alone: a UV member reads one
// component of one Input variable and nothing else, the layer member one constant-indexed element of a uniform block
// (the translator's vec4 data[] arrays) through the conversions the translator emits (RoundEven, FMax, FToU, UClamp,
// Bitcast, SToF), or one component of a varying, whole or a bit field (terrain packs its layer into one).
#include "gfx.h"
#include <stdlib.h>
#include <string.h>

enum {
    OP_EXT_INST = 12, OP_TYPE_VOID = 19, OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_ARRAY = 28,
    OP_TYPE_POINTER = 32, OP_TYPE_FWD = 39, OP_CONSTANT = 43, OP_VARIABLE = 59, OP_LOAD = 61, OP_STORE = 62,
    OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_AC = 66, OP_DECORATE = 71, OP_VECTOR_SHUFFLE = 79, OP_CONSTRUCT = 80,
    OP_EXTRACT = 81, OP_INSERT = 82, OP_COPY_OBJECT = 83, OP_SAMPLED_IMAGE = 86, OP_SAMPLE_FIRST = 87,
    OP_SAMPLE_LAST = 97, OP_CONVERT_F_TO_U = 109, OP_CONVERT_U_TO_F = 112, OP_BITCAST = 124, OP_IS_NAN = 156,
    OP_SELECT = 169, OP_BIT_FIELD_U_EXTRACT = 203, OP_TYPE_PTR_FWD = 322,
    DEC_LOCATION = 30, DEC_BINDING = 33,
    SC_UNIFORM_CONSTANT = 0, SC_INPUT = 1, SC_UNIFORM = 2, SC_OUTPUT = 3, SC_FUNCTION = 7,
};

typedef struct {
    const uint32_t *w;
    uint32_t bound;
    uint32_t *def;       // word index of the instruction defining an id (0: none)
    int8_t *sc;          // variables: storage class, -1 other ids
    int16_t *loc, *bind;
    uint32_t *base;      // access chains: the pointer they index (0: none)
    uint32_t *st_head, *st_next, *st_val, n_st;   // stores per root variable, a linked list
    uint32_t *mark, epoch;
    uint32_t *queue;     // BFS queue
    uint32_t *lvl;       // steps from the roots of the last BFS
    uint8_t *is_type;
} Mod;

static uint32_t op_of(const Mod *m, uint32_t id) { return m->def[id] ? m->w[m->def[id]] & 0xffff : 0; }
static uint32_t len_of(const Mod *m, uint32_t id) { return m->def[id] ? m->w[m->def[id]] >> 16 : 0; }
static const uint32_t *ops_of(const Mod *m, uint32_t id) { return m->w + m->def[id] + 1; }

static uint32_t root(const Mod *m, uint32_t p) {
    for (int k = 0; k < 64 && p < m->bound && m->base[p]; k++) p = m->base[p];
    return p;
}

static int is_sample(const Mod *m, uint32_t id) {
    uint32_t op = op_of(m, id);
    return op >= OP_SAMPLE_FIRST && op <= OP_SAMPLE_LAST;
}

// The ids an id's value depends on, appended to out (at most cap); returns the count.
static int deps(const Mod *m, uint32_t id, uint32_t *out, int cap) {
    uint32_t op = op_of(m, id), len = len_of(m, id);
    if (!op || op == OP_VARIABLE || op == OP_CONSTANT) return 0;
    const uint32_t *a = ops_of(m, id);
    int n = 0;
#define ADD(x) do { uint32_t x_ = (x); if (x_ < m->bound && m->def[x_] && n < cap) out[n++] = x_; } while (0)
    if (op == OP_LOAD) {
        uint32_t v = root(m, a[2]);
        for (uint32_t k = v < m->bound ? m->st_head[v] : 0; k; k = m->st_next[k]) ADD(m->st_val[k]);
        for (uint32_t p = a[2]; p < m->bound && m->base[p]; p = m->base[p]) {
            uint32_t pl = len_of(m, p);
            const uint32_t *pa = ops_of(m, p);
            for (uint32_t j = 3; j + 1 < pl; j++) ADD(pa[j]);
        }
        ADD(v);
        return n;
    }
    uint32_t first = 2;
    if (op == OP_EXTRACT) { ADD(a[2]); return n; }
    if (op == OP_VECTOR_SHUFFLE || op == OP_INSERT) { ADD(a[2]); ADD(a[3]); return n; }
    if (op == OP_EXT_INST) first = 4;
    for (uint32_t j = first; j + 1 < len; j++) ADD(a[j]);
#undef ADD
    return n;
}

// Breadth-first slice from r; marks every id reached with the current epoch. stop_samples: do not go through
// samples. Calls visit(m, id, ctx) per id in BFS order; a nonzero return stops the walk and is returned.
typedef int (*VisitFn)(const Mod *m, uint32_t id, void *ctx);
static int bfs_roots(Mod *m, const uint32_t *roots, int nr, int stop_samples, VisitFn visit, void *ctx) {
    m->epoch++;
    uint32_t qh = 0, qt = 0, buf[64];
    for (int k = 0; k < nr; k++)
        if (roots[k] < m->bound && m->mark[roots[k]] != m->epoch) m->queue[qt++] = roots[k], m->mark[roots[k]] = m->epoch, m->lvl[roots[k]] = 0;
    uint32_t nroot = qt;
    while (qh < qt) {
        uint32_t id = m->queue[qh++];
        int v = visit ? visit(m, id, ctx) : 0;
        if (v) return v;
        if (stop_samples && qh > nroot && is_sample(m, id)) continue;
        int n = deps(m, id, buf, 64);
        for (int j = 0; j < n; j++)
            if (m->mark[buf[j]] != m->epoch && qt < m->bound)
                m->mark[buf[j]] = m->epoch, m->lvl[buf[j]] = m->lvl[id] + 1, m->queue[qt++] = buf[j];
    }
    return 0;
}

static int bfs(Mod *m, uint32_t r, int stop_samples, VisitFn visit, void *ctx) {
    return bfs_roots(m, &r, 1, stop_samples, visit, ctx);
}

typedef struct { uint32_t id[16]; int n; } Samples;   // the first 16 in BFS order (nearest first)
static int near_samples(const Mod *m, uint32_t id, void *ctx) {
    Samples *c = ctx;
    if (is_sample(m, id)) c->id[c->n++] = id;
    return c->n == 16;
}

static int first_tex_var(const Mod *m, uint32_t id, void *ctx) {
    if (m->sc[id] == SC_UNIFORM_CONSTANT) { *(uint32_t *)ctx = id; return 1; }
    return 0;
}

static int any_sample(const Mod *m, uint32_t id, void *ctx) {
    (void)ctx;
    return is_sample(m, id);
}

// One coordinate member: the Input components and uniform elements it reads, and whether anything but plumbing
// (loads, chains, selects, NaN checks, conversions, rounding and clamps) touches it.
typedef struct {
    int n_in, n_ubo, math;
    int in_loc, in_comp;           // the last input read
    int ubo_bind, ubo_off;         // the last uniform element read (byte offset)
    int ext_shift, ext_bits, n_ext; // BitFieldUExtract with constant offset and count (a layer packed in a varying)
} Member;

static int const_val(const Mod *m, uint32_t id, uint32_t *v) {
    if (id >= m->bound || op_of(m, id) != OP_CONSTANT || len_of(m, id) < 4) return 0;
    *v = ops_of(m, id)[2];
    return 1;
}

static int member_visit(const Mod *m, uint32_t id, void *ctx) {
    Member *r = ctx;
    uint32_t op = op_of(m, id);
    const uint32_t *a = op ? ops_of(m, id) : NULL;
    switch (op) {
    case OP_LOAD: {
        uint32_t p = a[2], v = root(m, p);
        if (v < m->bound && m->sc[v] == SC_INPUT) {
            uint32_t c = 0;
            r->n_in++, r->in_loc = m->loc[v];
            r->in_comp = p != v && op_of(m, p) && len_of(m, p) == 5 && const_val(m, ops_of(m, p)[3], &c) ? (int)c : -1;
        } else if (v < m->bound && m->sc[v] == SC_UNIFORM) {
            uint32_t blk = 0, idx = 0, comp = 0;   // AC(var, 0 (the data member), element, component)
            r->n_ubo++, r->ubo_bind = m->bind[v];
            if (p != v && len_of(m, p) == 7 && const_val(m, ops_of(m, p)[3], &blk) && !blk &&
                const_val(m, ops_of(m, p)[4], &idx) && const_val(m, ops_of(m, p)[5], &comp) && comp < 4)
                r->ubo_off = (int)(idx * 16 + comp * 4);
            else r->ubo_off = -1;
        } else if (v < m->bound && m->sc[v] != SC_FUNCTION && m->sc[v] != SC_UNIFORM_CONSTANT) {
            r->math = 1;   // another kind of memory (storage buffers, private globals)
        }
        return 0;
    }
    case 0: case OP_VARIABLE: case OP_CONSTANT: case 44: case 41: case 42: case OP_ACCESS_CHAIN: case OP_IN_BOUNDS_AC:
    case OP_SELECT: case OP_IS_NAN: case OP_EXTRACT: case OP_CONSTRUCT: case OP_COPY_OBJECT: case OP_BITCAST:
        return 0;
    case OP_BIT_FIELD_U_EXTRACT: {
        uint32_t off = 0, cnt = 0;
        if (const_val(m, a[3], &off) && const_val(m, a[4], &cnt) && off < 32 && cnt > 0 && cnt <= 16)
            r->n_ext++, r->ext_shift = (int)off, r->ext_bits = (int)cnt;
        else r->math = 1;
        return 0;
    }
    case OP_EXT_INST: {   // GLSL.std.450: Round 1, RoundEven 2, Trunc 3, FMin 37, FMax 40, UClamp 44 and the min/max family
        uint32_t e = a[3];
        if (!(e == 1 || e == 2 || e == 3 || (e >= 37 && e <= 45))) r->math = 1;
        return 0;
    }
    default:
        if (op >= OP_CONVERT_F_TO_U && op <= OP_CONVERT_U_TO_F) return 0;
        r->math = 1;
        return 0;
    }
}

static Member member_of(Mod *m, uint32_t id) {
    Member r = {0, 0, 0, -1, -1, -1, -1, 0, 0, 0};
    bfs(m, id, 1, member_visit, &r);
    return r;
}

// The image type behind a UniformConstant variable: arrayed (its OpTypeImage Arrayed operand), -1 unknown.
static int var_arrayed(const Mod *m, uint32_t var, int *dim) {
    uint32_t t = ops_of(m, var)[0];   // pointer type
    for (int k = 0; k < 4 && t < m->bound && m->def[t]; k++) {
        uint32_t op = m->w[m->def[t]] & 0xffff;
        const uint32_t *a = m->w + m->def[t] + 1;
        if (op == OP_TYPE_POINTER) t = a[2];
        else if (op == OP_TYPE_ARRAY || op == OP_TYPE_SAMPLED_IMAGE) t = a[1];
        else if (op == OP_TYPE_IMAGE) { *dim = (int)a[2]; return (int)a[4]; }
        else break;
    }
    return -1;
}

// Which RGB components (bits 0, 1, 2) of image sample smp are actually read in the output slice (mark == slice_ep),
// either directly by OpCompositeExtract / non-identity OpVectorShuffle on smp, or after an identity OpVectorShuffle
// stored into a Function variable and loaded back.
static uint32_t sample_rgb_mask(const Mod *M, uint64_t words, uint32_t smp, uint32_t slice_ep) {
    uint32_t cm = 0, shuf = 0, fn_var = 0;
    for (uint64_t i = 5; i < words;) {
        uint32_t op = M->w[i] & 0xffff, len = M->w[i] >> 16;
        if (!len || i + len > words) break;
        const uint32_t *a = M->w + i + 1;
        if (op == OP_EXTRACT && len >= 5 && a[2] == smp && a[1] < M->bound && M->mark[a[1]] == slice_ep && a[3] < 3) {
            cm |= 1u << a[3];
        } else if (op == OP_VECTOR_SHUFFLE && len >= 5 && (a[2] == smp || a[3] == smp) && a[1] < M->bound && M->mark[a[1]] == slice_ep) {
            if (len == 8 && a[4] == 0 && a[5] == 1 && a[6] == 2 && a[7] == 3) shuf = a[1];
            else for (uint32_t j = 4; j + 1 < len; j++) if (a[j] < 3) cm |= 1u << a[j];
        } else if (shuf && op == OP_STORE && len >= 3 && a[1] == shuf) {
            uint32_t v = root(M, a[0]);
            if (v < M->bound && M->sc[v] == SC_FUNCTION) fn_var = v;
        }
        i += len;
    }
    if (fn_var) {
        for (uint64_t i = 5; i < words;) {
            uint32_t op = M->w[i] & 0xffff, len = M->w[i] >> 16;
            if (!len || i + len > words) break;
            const uint32_t *a = M->w + i + 1;
            if (op == OP_EXTRACT && len >= 5 && a[1] < M->bound && M->mark[a[1]] == slice_ep && a[3] < 3 &&
                a[2] < M->bound && op_of(M, a[2]) == OP_LOAD && root(M, ops_of(M, a[2])[2]) == fn_var)
                cm |= 1u << a[3];
            i += len;
        }
    }
    return cm;
}

// The texture and coordinate of one image sample smp. allow_approx: when true (for a confirmed 3-channel RGB sample),
// a coordinate that adds a distortion sample or secondary varying still extracts its primary UV varying with uv_math=1.
static void one_sample(Mod *M, uint32_t smp, int allow_approx, GfxAlbedo *o) {
    memset(o, 0, sizeof *o);
    o->kind = GFX_ALB_FAILED, o->layer_loc = 0xff;
    const uint32_t *sa = ops_of(M, smp);
    uint32_t tv = 0;
    bfs(M, sa[2], 0, first_tex_var, &tv);
    if (!tv || M->bind[tv] < 0) return;
    o->tex_binding = (uint8_t)M->bind[tv];
    int dim = 0, arrayed = var_arrayed(M, tv, &dim);
    if (dim != 1 || arrayed < 0) { o->kind = GFX_ALB_OTHER_TYPE; return; }   // Dim 2D only
    uint32_t coord = sa[3];
    int nested = bfs(M, coord, 0, any_sample, NULL);
    if (nested && !allow_approx) { o->kind = GFX_ALB_NESTED; return; }
    // Select(IsNan(c), 0, c) -> c, then the Construct's members
    for (int k = 0; k < 4 && op_of(M, coord) == OP_SELECT; k++) coord = ops_of(M, coord)[4];
    if (op_of(M, coord) != OP_CONSTRUCT || len_of(M, coord) < 5) { o->kind = GFX_ALB_UV_MATH; return; }
    uint32_t nm = len_of(M, coord) - 3;
    const uint32_t *mem = ops_of(M, coord) + 2;
    if ((uint32_t)(2 + arrayed) != nm) { o->kind = GFX_ALB_UV_MATH; return; }
    Member u = member_of(M, mem[0]), v = member_of(M, mem[1]);
    int uv_ok = u.n_in == 1 && v.n_in == 1 && u.in_loc == v.in_loc && u.in_loc >= 0 && u.in_comp >= 0 && v.in_comp >= 0;
    if (!uv_ok && allow_approx && u.n_in >= 1 && v.n_in >= 1 && u.in_loc == v.in_loc && u.in_loc >= 0 && u.in_comp >= 0 && v.in_comp >= 0)
        uv_ok = 1;
    if (!uv_ok) {
        o->kind = u.n_in + v.n_in ? GFX_ALB_UV_MATH : GFX_ALB_NO_VARYING;
        return;
    }
    o->uv_loc = (uint8_t)u.in_loc, o->uv_comp = (uint8_t)(u.in_comp | v.in_comp << 4);
    o->uv_math = nested || u.n_in > 1 || v.n_in > 1 || u.n_ubo || v.n_ubo || u.math || v.math;
    if (arrayed) {
        Member l = member_of(M, mem[2]);
        if (!l.n_in && l.n_ubo == 1 && l.ubo_off >= 0 && l.ubo_bind >= 0 && !l.math && !l.n_ext) {
            o->layer_binding = (uint8_t)l.ubo_bind, o->layer_off = (uint16_t)l.ubo_off;
        } else if (l.n_in == 1 && !l.n_ubo && l.in_comp >= 0 && !l.math && l.n_ext <= 1) {
            o->layer_loc = (uint8_t)l.in_loc, o->layer_comp = (uint8_t)l.in_comp;
            o->layer_shift = (uint8_t)l.ext_shift, o->layer_bits = (uint8_t)l.ext_bits;
        } else if (allow_approx && !l.n_in && l.n_ubo >= 1 && l.ubo_off >= 0 && l.ubo_bind >= 0) {
            o->layer_binding = (uint8_t)l.ubo_bind, o->layer_off = (uint16_t)l.ubo_off, o->uv_math = 1;
        } else if (allow_approx && l.n_in >= 1 && l.in_loc >= 0 && l.in_comp >= 0 && l.n_ext <= 1) {
            o->layer_loc = (uint8_t)l.in_loc, o->layer_comp = (uint8_t)l.in_comp;
            o->layer_shift = (uint8_t)l.ext_shift, o->layer_bits = (uint8_t)l.ext_bits, o->uv_math = 1;
        } else {
            o->kind = GFX_ALB_LAYER_UNKNOWN;
            return;
        }
        o->kind = GFX_ALB_ARRAY;
    } else {
        o->kind = GFX_ALB_2D;
    }
}

void gfx_albedo_analyse(const uint32_t *w, uint64_t words, int out_location, GfxAlbedo *o) {
    memset(o, 0, sizeof *o);
    o->kind = GFX_ALB_FAILED, o->layer_loc = 0xff;
    if (words < 5) return;
    Mod m = {.w = w, .bound = w[3] < (1u << 20) ? w[3] : (1u << 20)};
    uint32_t B = m.bound;
    m.def = calloc(B, 4), m.sc = malloc(B), m.loc = malloc(B * 2), m.bind = malloc(B * 2), m.base = calloc(B, 4);
    m.st_head = calloc(B, 4), m.mark = calloc(B, 4), m.queue = malloc(B * 4 + 4), m.is_type = calloc(B, 1);
    m.lvl = calloc(B, 4);
    uint32_t n_store = 1;
    for (uint64_t i = 5; i < words;) {
        uint32_t len = w[i] >> 16;
        if (!len || i + len > words) break;
        if ((w[i] & 0xffff) == OP_STORE) n_store++;
        i += len;
    }
    m.st_next = calloc(n_store, 4), m.st_val = calloc(n_store, 4), m.n_st = 1;
    if (!m.def || !m.sc || !m.loc || !m.bind || !m.base || !m.st_head || !m.mark || !m.queue || !m.is_type ||
        !m.st_next || !m.st_val || !m.lvl)
        goto out;
    memset(m.sc, 0xff, B), memset(m.loc, 0xff, B * 2), memset(m.bind, 0xff, B * 2);
    for (uint64_t i = 5; i < words;) {   // types first: a value instruction's first operand is a type id
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > words) break;
        if (((op >= OP_TYPE_VOID && op <= OP_TYPE_FWD) || op == OP_TYPE_PTR_FWD) && len > 1 && w[i + 1] < B) {
            m.is_type[w[i + 1]] = 1, m.def[w[i + 1]] = (uint32_t)i;
        }
        i += len;
    }
    for (uint64_t i = 5; i < words;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        if (!len || i + len > words) break;
        if (op == OP_DECORATE && len >= 4 && a[0] < B) {
            if (a[1] == DEC_LOCATION) m.loc[a[0]] = (int16_t)a[2];
            else if (a[1] == DEC_BINDING) m.bind[a[0]] = (int16_t)a[2];
        } else if (op == OP_VARIABLE && len >= 4 && a[1] < B) {
            m.sc[a[1]] = (int8_t)a[2], m.def[a[1]] = (uint32_t)i;
        } else if (op == OP_STORE && len >= 3) {
            uint32_t v = root(&m, a[0]), comp = 0;
            // Target 1's alpha channel (component 3) packs a 5-bit material flag from a single-channel mask texture
            // in several shaders (e.g. 5d791509); exclude it so its mask does not beat the RGB albedo texture.
            if (v < B && m.sc[v] == SC_OUTPUT && a[0] != v && len_of(&m, a[0]) == 5 &&
                const_val(&m, ops_of(&m, a[0])[3], &comp) && comp == 3) {
                // skip out.a store
            } else if (v < B && m.n_st < n_store) {
                m.st_val[m.n_st] = a[1], m.st_next[m.n_st] = m.st_head[v], m.st_head[v] = m.n_st, m.n_st++;
            }
        } else if (len >= 3 && a[0] < B && m.is_type[a[0]] && a[1] < B && !m.is_type[a[1]]) {
            m.def[a[1]] = (uint32_t)i;
            if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_AC) && len >= 4) m.base[a[1]] = a[2];
        }
        i += len;
    }
    // the output variable, then the stores to it (one per component): the sample reached in the fewest steps from any
    uint32_t out = 0;
    for (uint32_t id = 1; id < B && !out; id++)
        if (m.sc[id] == SC_OUTPUT && m.loc[id] == out_location) out = id;
    if (!out) { o->kind = GFX_ALB_NO_OUTPUT; goto out; }
    uint32_t roots[64];
    int nr = 0;
    for (uint32_t k = m.st_head[out]; k && nr < 64; k = m.st_next[k]) roots[nr++] = m.st_val[k];
    Samples c = {.n = 0};
    bfs_roots(&m, roots, nr, 0, near_samples, &c);
    if (!c.n) { o->kind = GFX_ALB_NO_SAMPLE; goto out; }
    uint32_t slice_ep = m.epoch, cm[16];
    for (int k = 0; k < c.n; k++) cm[k] = sample_rgb_mask(&m, words, c.id[k], slice_ep);
    // Only a sample whose RGB channels (0, 1, 2) are all read in the target-1 RGB slice may be bound as an albedo
    // texture: 1-channel masks (BC4 .r/.g/.b) and 2-channel normal/offset maps (BC5 .rg) must never be bound as RGB
    // albedo. First try clean 3-channel RGB samples; if none has a direct unperturbed UV, allow extracting the primary
    // UV varying from a 3-channel RGB sample with offset/perturbed UV (uv_math=1). Shaders with no 3-channel RGB sample
    // (e.g. 0118213e, 40e3775d) return a non-texture kind so the draw uses a flat material or Step 6 shader program.
    int found_rgb = 0;
    for (int pass = 0; pass < 2 && !found_rgb; pass++) {
        for (int k = 0; k < c.n; k++) {
            if ((cm[k] & 7u) != 7u) continue;
            GfxAlbedo t;
            one_sample(&m, c.id[k], pass, &t);
            if (!found_rgb && t.kind != GFX_ALB_2D && t.kind != GFX_ALB_ARRAY) *o = t;
            if (t.kind == GFX_ALB_2D || t.kind == GFX_ALB_ARRAY) {
                *o = t, found_rgb = 1;
                break;
            }
        }
    }
out:
    free(m.def), free(m.sc), free(m.loc), free(m.bind), free(m.base), free(m.st_head), free(m.mark), free(m.queue);
    free(m.is_type), free(m.st_next), free(m.st_val), free(m.lvl);
}
