// Game shaders at ray hits (docs/raytracing.md, materials step 6): a G-buffer fragment shader's albedo (colour target 1) and glow
// (colour target 5, the light target) compiled to a small register program that rt_gi.comp interprets where a ray hits
// a triangle of a draw that used the shader: the varyings interpolated at the hit from the captured corners (vk_rt.c
// ext pool), the uniforms from a snapshot taken at the draw, the textures from the bindless table.
//
// The translator's SPIR-V: one function, no loops, no Phi; if-blocks write Function variables. Compiled in program
// order with every block executed: each block has a predicate (the AND of the branch conditions on the way, OR over
// its incoming edges) and a store in a block under a predicate is a select of the new and the old value. Values are
// scalarised (one virtual register a component), Function variables stored once become aliases of the stored value,
// dead code is dropped from the outputs back, and the virtual registers get physical ones by a linear scan.
//
// Program words (uvec4 units in vk_rt.c's code buffer; rt_gi.comp prog_run):
//   [0] instructions, constants, registers, flags      [1] [2] outputs: albedo rgb, glow rgb (operands), slots, flat
//   [3 ...] instructions: w0 = op | dst << 8 | aux << 20, w1-w3 operands (a register, or 0x10000 | a constant's index)
//   then the constants, four a uvec4.
#include "gfx.h"
#include <stdlib.h>
#include <string.h>

enum {   // SPIR-V
    S_UNDEF = 1, S_EXT_IMPORT = 11, S_EXT_INST = 12, S_TYPE_VOID = 19, S_TYPE_BOOL = 20, S_TYPE_INT = 21, S_TYPE_FLOAT = 22,
    S_TYPE_VECTOR = 23, S_TYPE_MATRIX = 24, S_TYPE_IMAGE = 25, S_TYPE_SAMPLER = 26, S_TYPE_SAMPLED_IMAGE = 27,
    S_TYPE_ARRAY = 28, S_TYPE_RUNTIME_ARRAY = 29, S_TYPE_STRUCT = 30, S_TYPE_POINTER = 32, S_TYPE_FUNCTION = 33,
    S_CONST_TRUE = 41, S_CONST_FALSE = 42, S_CONST = 43, S_CONST_COMPOSITE = 44, S_CONST_NULL = 46, S_FUNCTION = 54,
    S_FUNCTION_END = 56, S_VARIABLE = 59, S_LOAD = 61, S_STORE = 62, S_ACCESS_CHAIN = 65, S_IN_BOUNDS_AC = 66,
    S_DECORATE = 71, S_MEMBER_DECORATE = 72, S_SHUFFLE = 79, S_CONSTRUCT = 80, S_EXTRACT = 81, S_INSERT = 82, S_COPY = 83,
    S_SAMPLED_IMAGE = 86, S_SAMPLE_IMPLICIT = 87, S_SAMPLE_EXPLICIT = 88, S_FETCH = 95, S_IMAGE = 100,
    S_QUERY_SIZE_LOD = 103, S_QUERY_SIZE = 104, S_QUERY_LEVELS = 106,
    S_F2U = 109, S_F2S = 110, S_S2F = 111, S_U2F = 112, S_UCONV = 113, S_SCONV = 114, S_FCONV = 115, S_BITCAST = 124,
    S_SNEG = 126, S_FNEG = 127, S_IADD = 128, S_FADD = 129, S_ISUB = 130, S_FSUB = 131, S_IMUL = 132, S_FMUL = 133,
    S_UDIV = 134, S_SDIV = 135, S_FDIV = 136, S_UMOD = 137, S_SREM = 138, S_SMOD = 139, S_FREM = 140, S_FMOD = 141,
    S_VTS = 142, S_DOT = 148, S_ANY = 154, S_ALL = 155, S_ISNAN = 156, S_ISINF = 157, S_LEQ = 164, S_LNE = 165,
    S_LOR = 166, S_LAND = 167, S_LNOT = 168, S_SELECT = 169, S_IEQ = 170, S_INE = 171, S_UGT = 172, S_SGT = 173,
    S_UGE = 174, S_SGE = 175, S_ULT = 176, S_SLT = 177, S_ULE = 178, S_SLE = 179, S_FOEQ = 180, S_FUGE_LAST = 191,
    S_SHR = 194, S_SAR = 195, S_SHL = 196, S_OR = 197, S_XOR = 198, S_AND = 199, S_NOT = 200, S_BFI = 201, S_BFE_S = 202,
    S_BFE_U = 203, S_DPDX = 207, S_FWIDTH_COARSE = 215, S_PHI = 245, S_LOOP_MERGE = 246, S_SELECTION_MERGE = 247,
    S_LABEL = 248, S_BRANCH = 249, S_BRANCH_COND = 250, S_SWITCH = 251, S_KILL = 252, S_RETURN = 253,
    S_RETURN_VALUE = 254, S_UNREACHABLE = 255, S_TERMINATE = 4416, S_DEMOTE = 5380,
    D_BUILTIN = 11, D_FLAT = 14, D_LOCATION = 30, D_BINDING = 33,
    SC_UNIFORM_CONSTANT = 0, SC_INPUT = 1, SC_UNIFORM = 2, SC_OUTPUT = 3, SC_PRIVATE = 6, SC_FUNCTION = 7, SC_STORAGE = 12,
};

enum { K_F, K_I, K_U, K_B, K_IMG, K_NONE };   // value kinds

#define MAXV 8192          // virtual registers
#define MAXI 16384         // virtual instructions
#define MAXC 2048          // constants
#define MAXP 160           // physical registers (rt_gi.comp PROG_REGS)
#define GP_POISON 255      // (compile time only) a value the program cannot make: fails only if an output needs it
#define XLOC 32            // varyings, textures and uniform floats while compiling (the program keeps the used ones)
#define XTEX 32
#define XUNI 1024

typedef struct { int32_t c[4]; uint8_t n, kind; int16_t tex; } Val;   // tex: K_IMG: the program texture slot
typedef struct { uint8_t op; uint16_t aux; int32_t d; int32_t a, b, c; uint8_t nd; } VIns;   // operands: >= 0 a vreg,
                                                                                               // < 0 constant -(i + 1)
#define FAIL(x, why) ((x)->fail ? 0 : ((x)->fail = (why), (x)->P->fail_line = __LINE__))
typedef struct {
    const uint32_t *w;
    uint32_t bound;
    uint32_t *def;              // word index of the instruction defining an id
    uint8_t *tkind, *tn;        // types: kind and components (0: not a scalar/vector type)
    uint32_t *ptr_to;           // pointer types: the pointee; variables: their pointer type
    int8_t *sc;
    int16_t *loc, *bind, *builtin;
    uint8_t *flat;
    uint32_t glsl;              // the GLSL.std.450 import
    Val *val;                   // per id
    // access chains: base variable and constant indices (-1 dynamic)
    uint32_t *ac_base;
    uint32_t *init;             // variables: their initializer (OpVariable's fourth operand), 0 none
    int16_t (*ac_idx)[3];
    uint8_t *ac_n;
    uint32_t (*ac_val)[3];      // raw operand ids of the access chain indices (for dynamic index fallback)
    // Function / Private / Output variables: per component, the store count and the vregs or alias
    uint16_t (*nst)[4];
    int32_t (*vr)[4];
    VIns *ins;
    int nins;
    uint32_t cpool[MAXC];
    int ncon;
    int nv;
    int fail;
    GfxProg *P;
    int nloc, ntex, nuni;
    uint8_t xloc[XLOC], xflat[XLOC], tex_bind[XTEX], tex_arr[XTEX];
    uint32_t uni_key[XUNI];
} Cx;

static int32_t con(Cx *x, uint32_t bits) {
    for (int i = 0; i < x->ncon; i++)
        if (x->cpool[i] == bits) return -(i + 1);
    if (x->ncon >= MAXC) { FAIL(x, GFX_PROG_TOO_BIG); return -1; }
    x->cpool[x->ncon] = bits;
    return -(++x->ncon);
}
static int32_t vreg(Cx *x, int n) {
    if (x->nv + n > MAXV) { FAIL(x, GFX_PROG_TOO_BIG); return 0; }
    int32_t r = x->nv;
    x->nv += n;
    return r;
}
static VIns *emit(Cx *x, int op, int aux, int32_t d, int32_t a, int32_t b, int32_t c) {
    static VIns dummy;
    if (x->nins >= MAXI) { FAIL(x, GFX_PROG_TOO_BIG); return &dummy; }
    VIns *i = &x->ins[x->nins++];
    *i = (VIns){(uint8_t)op, (uint16_t)aux, d, a, b, c, 1};
    return i;
}
// a new vreg holding op(a, b, c)
static int32_t op3(Cx *x, int op, int aux, int32_t a, int32_t b, int32_t c) {
    int32_t d = vreg(x, 1);
    emit(x, op, aux, d, a, b, c);
    return d;
}
static int32_t op1(Cx *x, int op, int aux, int32_t a) { return op3(x, op, aux, a, 0, 0); }
static int32_t op2(Cx *x, int op, int aux, int32_t a, int32_t b) { return op3(x, op, aux, a, b, 0); }

static uint32_t opc(const Cx *x, uint32_t id) { return id < x->bound && x->def[id] ? x->w[x->def[id]] & 0xffff : 0; }
static uint32_t lenof(const Cx *x, uint32_t id) { return id < x->bound && x->def[id] ? x->w[x->def[id]] >> 16 : 0; }
static const uint32_t *ops(const Cx *x, uint32_t id) { return x->w + x->def[id] + 1; }

// a type's kind and components
static int type_of(const Cx *x, uint32_t t, int *n) {
    if (t >= x->bound || !x->tn[t]) return K_NONE;
    *n = x->tn[t];
    return x->tkind[t];
}

static Val *V(Cx *x, uint32_t id) {
    static Val none = {{0, 0, 0, 0}, 0, K_NONE, -1};
    if (id >= x->bound) return &none;
    return &x->val[id];
}
static int32_t poison(Cx *x, int line);
static int32_t comp(Cx *x, uint32_t id, int i) {
    Val *v = V(x, id);
    if (v->kind == K_NONE || v->kind == K_IMG || !v->n) return poison(x, __LINE__);
    return v->c[v->n == 1 ? 0 : i < v->n ? i : v->n - 1];
}

static int32_t c_true(Cx *x) { return con(x, 1); }

// ---- the program's resources
// (indices while compiling: finish keeps the ones the program uses and numbers them again)
static int add_loc(Cx *x, int loc, int flat) {
    for (int i = 0; i < x->nloc; i++)
        if (x->xloc[i] == loc) return i;
    if (x->nloc >= XLOC) { FAIL(x, GFX_PROG_TOO_BIG); return 0; }
    x->xloc[x->nloc] = (uint8_t)loc, x->xflat[x->nloc] = (uint8_t)flat;
    return x->nloc++;
}
static int add_uni(Cx *x, int binding, int elem, int c) {
    if (binding < 0 || binding > 255 || elem < 0 || elem >= 1024 || c < 0 || c > 3) return -1;
    uint32_t key = (uint32_t)binding << 12 | (uint32_t)(elem * 4 + c);
    for (int i = 0; i < x->nuni; i++)
        if (x->uni_key[i] == key) return i;
    if (x->nuni >= XUNI) { FAIL(x, GFX_PROG_TOO_BIG); return 0; }
    x->uni_key[x->nuni] = key;
    return x->nuni++;
}
static int add_tex(Cx *x, int binding, int arrayed) {
    for (int i = 0; i < x->ntex; i++)
        if (x->tex_bind[i] == binding) return i;
    if (x->ntex >= XTEX) { FAIL(x, GFX_PROG_TOO_BIG); return 0; }
    x->tex_bind[x->ntex] = (uint8_t)binding, x->tex_arr[x->ntex] = (uint8_t)arrayed;
    return x->ntex++;
}
static int32_t poison(Cx *x, int line) { return op1(x, GP_POISON, line & 0xfff, 0); }
static void poison_val(Cx *x, Val *out, int n, int line) {
    if (out->kind == K_NONE || out->kind == K_IMG) out->kind = K_F;
    out->n = (uint8_t)(n ? n : 1), out->tex = -1;
    for (int i = 0; i < out->n; i++) out->c[i] = poison(x, line);
}

// the image variable behind a texture operand: arrayed and dim
static int image_info(const Cx *x, uint32_t t, int *dim, int *arrayed, int *sampled_kind) {
    for (int k = 0; k < 6 && t < x->bound && x->def[t]; k++) {
        uint32_t op = opc(x, t);
        const uint32_t *a = ops(x, t);
        if (op == S_TYPE_POINTER) t = a[2];
        else if (op == S_TYPE_ARRAY || op == S_TYPE_SAMPLED_IMAGE || op == S_TYPE_RUNTIME_ARRAY) t = a[1];
        else if (op == S_TYPE_IMAGE) {
            *dim = (int)a[2], *arrayed = (int)a[4];
            int n = 0;
            *sampled_kind = type_of(x, a[1], &n);
            return 1;
        } else break;
    }
    return 0;
}

// ---- function variables
static int var_comps(Cx *x, uint32_t var) {
    int n = 0;
    uint32_t pt = x->ptr_to[var];
    if (pt >= x->bound || opc(x, pt) != S_TYPE_POINTER) return 0;
    if (type_of(x, ops(x, pt)[2], &n) == K_NONE) return 0;
    return n;
}
static int is_reg_var(const Cx *x, uint32_t v) {
    return v < x->bound && (x->sc[v] == SC_FUNCTION || x->sc[v] == SC_PRIVATE || x->sc[v] == SC_OUTPUT);
}
// the current value of a variable's component
static int32_t var_get(Cx *x, uint32_t var, int c) {
    if (x->nst[var][c] <= 1) return x->vr[var][c] == INT32_MIN ? con(x, 0) : x->vr[var][c];   // alias (0 stores: 0)
    if (x->vr[var][c] == INT32_MIN) x->vr[var][c] = vreg(x, 1);
    return x->vr[var][c];
}
static void var_set(Cx *x, uint32_t var, int c, int32_t val, int32_t pred) {
    if (x->nst[var][c] <= 1) { x->vr[var][c] = val; return; }   // stored once: an alias
    if (x->vr[var][c] == INT32_MIN) x->vr[var][c] = vreg(x, 1);
    int32_t d = x->vr[var][c];
    if (pred == INT32_MIN) emit(x, GP_MOV, 0, d, val, 0, 0);
    else emit(x, GP_SEL, 0, d, pred, val, d);
}

// resolve a pointer: variable and constant component (-1 whole, -2 unsupported)
static uint32_t ptr_res(Cx *x, uint32_t p, int *c, int *n_idx, int16_t idx[3]) {
    *c = -1, *n_idx = 0;
    if (p < x->bound && x->ac_base[p]) {
        *n_idx = x->ac_n[p];
        memcpy(idx, x->ac_idx[p], sizeof x->ac_idx[p]);
        return x->ac_base[p];
    }
    return p;
}

// Extract constant offset K from dynamic byte offset `(X + K) >> 2` (or `+ 1` word) used by terrain palette lookups
// (`(idx * 16 + 192) >> 2` or `(idx * 16 + 200) >> 2`), resolving `idx = 0` (the base terrain layer) so terrain shaders compile.
static int dyn_ubo_const(const Cx *x, uint32_t id, int depth) {
    if (depth > 8 || id >= x->bound || !x->def[id]) return -1;
    uint32_t op = opc(x, id), len = lenof(x, id);
    const uint32_t *a = ops(x, id);
    if (op == S_CONST && len >= 4) return (int)a[2];
    if (op == S_BITCAST && len >= 4) return dyn_ubo_const(x, a[2], depth + 1);
    if (op == S_LOAD && len >= 4) {
        uint32_t v = a[2];
        if (v < x->bound && x->ac_base[v]) v = x->ac_base[v];
        if (is_reg_var(x, v) && x->nst[v][0] == 1) {
            for (uint64_t i = 5; i < x->def[id]; i += x->w[i] >> 16)
                if ((x->w[i] & 0xffff) == S_STORE && (x->w[i] >> 16) >= 3 && x->w[i + 1] == v)
                    return dyn_ubo_const(x, x->w[i + 2], depth + 1);
        }
        return -1;
    }
    if (op == S_IADD && len >= 5) {
        int c0 = dyn_ubo_const(x, a[2], depth + 1), c1 = dyn_ubo_const(x, a[3], depth + 1);
        if (c0 >= 0 && c1 >= 0) return c0 + c1;
        return c0 >= 0 ? c0 : c1;
    }
    if (op == S_SHR && len >= 5) {
        int c0 = dyn_ubo_const(x, a[2], depth + 1), c1 = dyn_ubo_const(x, a[3], depth + 1);
        if (c0 >= 0 && c1 >= 0 && c1 < 31) return c0 >> c1;
    }
    if (op == S_AND && len >= 5) {
        int c0 = dyn_ubo_const(x, a[2], depth + 1), c1 = dyn_ubo_const(x, a[3], depth + 1);
        if (c0 >= 0 && c1 >= 0) return c0 & c1;
    }
    return -1;
}

// ---- the function body
static void f_load(Cx *x, uint32_t rt, uint32_t rid, uint32_t p) {
    int c, nidx;
    int16_t idx[3] = {0, 0, 0};
    uint32_t var = ptr_res(x, p, &c, &nidx, idx);
    int n = 0, kind = type_of(x, rt, &n);
    Val *out = &x->val[rid];
    out->kind = (uint8_t)kind, out->n = (uint8_t)n, out->tex = -1;
    if (var >= x->bound || x->sc[var] < 0) { poison_val(x, out, n, __LINE__); return; }
    int sc = x->sc[var];
    if (sc == SC_UNIFORM_CONSTANT) {   // a texture: its program slot
        int dim = 0, arr = 0, sk = 0;
        if (!image_info(x, x->ptr_to[var], &dim, &arr, &sk)) { out->kind = K_NONE, out->n = 0; return; }   // a sampler
        if (dim != 1 /* 2D */ || sk != K_F ||
            (nidx && idx[0] != 0) || x->bind[var] < 0) { out->kind = K_IMG, out->n = 0, out->tex = -1; return; }
        out->kind = K_IMG, out->n = 0, out->tex = (int16_t)add_tex(x, x->bind[var], arr);
        return;
    }
    if (kind == K_NONE) return;   // (an array or struct: a use of it is poisoned)
    if (sc == SC_UNIFORM && p < x->bound && x->ac_base[p]) {
        for (int i = 1; i < nidx && i < 3; i++)
            if (idx[i] < 0) {
                int cv = dyn_ubo_const(x, x->ac_val[p][i], 0);
                if (cv >= 0 && cv < (i == 1 ? 1024 : 4)) idx[i] = (int16_t)cv;
            }
    }
    for (int i = 0; i < 3; i++) if (i < nidx && idx[i] < 0) { poison_val(x, out, n, __LINE__); return; }
    if (sc == SC_INPUT) {
        if (x->builtin[var] >= 0) {   // FragCoord (15): no pixel at a hit; FrontFacing (17) true; others 0
            uint32_t fc[4] = {0, 0, 0x3f000000u, 0x3f800000u};
            for (int i = 0; i < n; i++) {
                int ci = nidx ? idx[0] + i : i;
                out->c[i] = x->builtin[var] == 17 ? c_true(x) : x->builtin[var] == 15 ? con(x, fc[ci & 3]) : con(x, 0);
            }
            return;
        }
        if (x->loc[var] < 0 || nidx > 1) { poison_val(x, out, n, __LINE__); return; }
        int slot = add_loc(x, x->loc[var], x->flat[var]);
        for (int i = 0; i < n; i++) {
            int ci = (nidx ? idx[0] : 0) + i;
            if (ci > 3) { poison_val(x, out, n, __LINE__); return; }
            out->c[i] = op1(x, GP_LDV, slot * 4 + ci, 0);
        }
        return;
    }
    if (sc == SC_UNIFORM) {   // AccessChain(block, 0, element[, component])
        if (nidx < 2 || idx[0] != 0 || idx[1] < 0) { poison_val(x, out, n, __LINE__); return; }
        for (int i = 0; i < n; i++) {
            int ci = nidx >= 3 ? idx[2] + i : i;
            int u = add_uni(x, x->bind[var], idx[1], ci);
            out->c[i] = u < 0 ? poison(x, __LINE__) : op1(x, GP_LDU, u, 0);
        }
        return;
    }
    if (is_reg_var(x, var)) {
        if (nidx > 1) { poison_val(x, out, n, __LINE__); return; }
        for (int i = 0; i < n; i++) {
            int ci = (nidx ? idx[0] : 0) + i;
            if (ci > 3) { poison_val(x, out, n, __LINE__); return; }
            int32_t v = var_get(x, var, ci);
            // a variable stored more than once: its value now copied (later stores change the variable)
            out->c[i] = x->nst[var][ci] > 1 ? op1(x, GP_MOV, 0, v) : v;
        }
        return;
    }
    poison_val(x, out, n, __LINE__);   // storage buffers, workgroup memory
}

static void f_store(Cx *x, uint32_t p, uint32_t vid, int32_t pred) {
    int c, nidx;
    int16_t idx[3] = {0, 0, 0};
    uint32_t var = ptr_res(x, p, &c, &nidx, idx);
    if (!is_reg_var(x, var)) {
        if (var < x->bound && x->sc[var] == SC_OUTPUT) return;
        return;   // a store elsewhere (storage buffers): not in the albedo or glow
    }
    if (nidx > 1 || (nidx && idx[0] < 0)) {   // a component not known: the whole variable poisoned
        for (int ci = 0; ci < 4; ci++) if (x->nst[var][ci]) var_set(x, var, ci, poison(x, __LINE__), pred);
        return;
    }
    Val *v = V(x, vid);
    int n = nidx ? 1 : var_comps(x, var);
    for (int i = 0; i < n; i++) {
        int ci = (nidx ? idx[0] : 0) + i;
        if (ci > 3) return;
        var_set(x, var, ci, comp(x, vid, i), pred);
    }
    (void)v;
}

static int is_unordered(uint32_t op) { return op >= S_FOEQ && op <= S_FUGE_LAST && ((op - S_FOEQ) & 1); }

static void f_value(Cx *x, uint32_t op, const uint32_t *a, uint32_t len) {
    uint32_t rt = a[0], rid = a[1];
    int n = 0, kind = type_of(x, rt, &n);
    Val *out = &x->val[rid];
    out->kind = (uint8_t)kind, out->n = (uint8_t)n, out->tex = -1;
    if (op == S_SAMPLED_IMAGE || op == S_IMAGE || (op == S_COPY && V(x, a[2])->kind == K_IMG)) { *out = *V(x, a[2]); return; }
    if (kind == K_NONE) return;   // (a struct or array: a use of it is poisoned)
#define A(k, i) comp(x, a[k], i)
    switch (op) {
    case S_UNDEF: for (int i = 0; i < n; i++) out->c[i] = con(x, 0); return;
    case S_COPY: case S_BITCAST: case S_UCONV: case S_SCONV: case S_FCONV:
        for (int i = 0; i < n; i++) out->c[i] = A(2, i);
        return;
    case S_CONSTRUCT: {
        int k = 0;
        for (uint32_t j = 2; j < len - 1 && k < 4; j++) {
            Val *s = V(x, a[j]);
            for (int i = 0; i < s->n && k < 4; i++) out->c[k++] = s->c[i];
        }
        if (k < n) poison_val(x, out, n, __LINE__);
        return;
    }
    case S_EXTRACT:
        if (len != 5 || a[3] > 3) { poison_val(x, out, n, __LINE__); return; }
        out->c[0] = A(2, (int)a[3]);
        return;
    case S_INSERT:   // object, composite, index
        if (len != 6 || a[4] > 3) { poison_val(x, out, n, __LINE__); return; }
        for (int i = 0; i < n; i++) out->c[i] = i == (int)a[4] ? A(2, 0) : A(3, i);
        return;
    case S_SHUFFLE: {
        Val *v1 = V(x, a[2]);
        for (int i = 0; i < n; i++) {
            uint32_t s = a[4 + i];
            out->c[i] = s == 0xffffffffu ? con(x, 0) : (int)s < v1->n ? A(2, (int)s) : A(3, (int)s - v1->n);
        }
        return;
    }
    case S_F2U: for (int i = 0; i < n; i++) out->c[i] = op1(x, GP_F2U, 0, A(2, i)); return;
    case S_F2S: for (int i = 0; i < n; i++) out->c[i] = op1(x, GP_F2S, 0, A(2, i)); return;
    case S_U2F: for (int i = 0; i < n; i++) out->c[i] = op1(x, GP_U2F, 0, A(2, i)); return;
    case S_S2F: for (int i = 0; i < n; i++) out->c[i] = op1(x, GP_S2F, 0, A(2, i)); return;
    case S_FNEG: for (int i = 0; i < n; i++) out->c[i] = op1(x, GP_FNEG, 0, A(2, i)); return;
    case S_SNEG: for (int i = 0; i < n; i++) out->c[i] = op2(x, GP_ISUB, 0, con(x, 0), A(2, i)); return;
    case S_NOT: for (int i = 0; i < n; i++) out->c[i] = op1(x, GP_NOT, 0, A(2, i)); return;
    case S_LNOT: for (int i = 0; i < n; i++) out->c[i] = op1(x, GP_LNOT, 0, A(2, i)); return;
    case S_ISNAN: for (int i = 0; i < n; i++) out->c[i] = op1(x, GP_ISNAN, 0, A(2, i)); return;
    case S_ISINF: for (int i = 0; i < n; i++) out->c[i] = op1(x, GP_ISINF, 0, A(2, i)); return;
    case S_VTS: for (int i = 0; i < n; i++) out->c[i] = op2(x, GP_FMUL, 0, A(2, i), A(3, 0)); return;
    case S_DOT: {
        int m = V(x, a[2])->n;
        int32_t t = op2(x, GP_FMUL, 0, A(2, 0), A(3, 0));
        for (int i = 1; i < m; i++) t = op3(x, GP_FMA, 0, A(2, i), A(3, i), t);
        out->c[0] = t;
        return;
    }
    case S_ANY: case S_ALL: {
        int m = V(x, a[2])->n;
        int32_t t = A(2, 0);
        for (int i = 1; i < m; i++) t = op2(x, op == S_ANY ? GP_LOR : GP_LAND, 0, t, A(2, i));
        out->c[0] = t;
        return;
    }
    case S_SELECT:
        for (int i = 0; i < n; i++) out->c[i] = op3(x, GP_SEL, 0, A(2, i), A(3, i), A(4, i));
        return;
    case S_BFE_U: case S_BFE_S:
        for (int i = 0; i < n; i++) out->c[i] = op3(x, op == S_BFE_U ? GP_BFEU : GP_BFES, 0, A(2, i), A(3, 0), A(4, 0));
        return;
    case S_BFI: {   // base, insert, offset, count: the count through a register
        for (int i = 0; i < n; i++) {
            int32_t cnt = A(5, 0);
            if (cnt < 0) cnt = op1(x, GP_MOV, 0, cnt);
            VIns *I = emit(x, GP_BFI, 0, vreg(x, 1), A(2, i), A(3, i), A(4, 0));
            I->aux = 0;
            out->c[i] = I->d;
            // the count: in a following ARG
            emit(x, GP_ARG, 0, -1, cnt, 0, 0);
        }
        return;
    }
    default: break;
    }
    if (op >= S_DPDX && op <= S_FWIDTH_COARSE) { for (int i = 0; i < n; i++) out->c[i] = con(x, 0); return; }
    // componentwise binary
    int g = -1, aux = 0;
    switch (op) {
    case S_FADD: g = GP_FADD; break; case S_FSUB: g = GP_FSUB; break; case S_FMUL: g = GP_FMUL; break;
    case S_FDIV: g = GP_FDIV; break; case S_FMOD: g = GP_FMOD; break; case S_FREM: g = GP_FREM; break;
    case S_IADD: g = GP_IADD; break; case S_ISUB: g = GP_ISUB; break; case S_IMUL: g = GP_IMUL; break;
    case S_UDIV: g = GP_UDIV; break; case S_SDIV: g = GP_SDIV; break; case S_UMOD: g = GP_UMOD; break;
    case S_SREM: g = GP_SREM; break; case S_SMOD: g = GP_SMOD; break;
    case S_AND: g = GP_AND; break; case S_OR: g = GP_OR; break; case S_XOR: g = GP_XOR; break;
    case S_SHL: g = GP_SHL; break; case S_SHR: g = GP_SHR; break; case S_SAR: g = GP_SAR; break;
    case S_LAND: g = GP_LAND; break; case S_LOR: g = GP_LOR; break; case S_LEQ: g = GP_IEQ; break; case S_LNE: g = GP_INE; break;
    case S_IEQ: g = GP_IEQ; break; case S_INE: g = GP_INE; break; case S_UGT: g = GP_UGT; break; case S_SGT: g = GP_SGT; break;
    case S_UGE: g = GP_UGE; break; case S_SGE: g = GP_SGE; break; case S_ULT: g = GP_ULT; break; case S_SLT: g = GP_SLT; break;
    case S_ULE: g = GP_ULE; break; case S_SLE: g = GP_SLE; break;
    default:
        if (op >= S_FOEQ && op <= S_FUGE_LAST) g = GP_FEQ + (int)((op - S_FOEQ) >> 1), aux = is_unordered(op);
        break;
    }
    if (g >= 0) {
        for (int i = 0; i < n; i++) out->c[i] = op2(x, g, aux, A(2, i), A(3, i));
        return;
    }
    if (op == S_EXT_INST && a[2] == x->glsl) {
        uint32_t e = a[3];
        int na = (int)len - 5;
        switch (e) {
        case 66: case 67: case 69: case 68: case 70: case 71: {   // Length, Distance, Normalize, Cross, FaceForward, Reflect
            int m = V(x, a[4])->n;
            if (e == 68) {   // cross(a, b)
                for (int i = 0; i < 3; i++) {
                    int j = (i + 1) % 3, k = (i + 2) % 3;
                    int32_t t = op2(x, GP_FMUL, 0, A(4, j), A(5, k));
                    out->c[i] = op3(x, GP_FMA, 0, A(4, k), op1(x, GP_FNEG, 0, A(5, j)), t);
                }
                return;
            }
            if (e == 70) {   // N, I, Nref: dot(Nref, I) < 0 ? N : -N
                int32_t t = op2(x, GP_FMUL, 0, A(6, 0), A(5, 0));
                for (int i = 1; i < m; i++) t = op3(x, GP_FMA, 0, A(6, i), A(5, i), t);
                int32_t lt = op2(x, GP_FLT, 0, t, con(x, 0));
                for (int i = 0; i < n; i++) out->c[i] = op3(x, GP_SEL, 0, lt, A(4, i), op1(x, GP_FNEG, 0, A(4, i)));
                return;
            }
            if (e == 71) {   // I - 2 dot(N, I) N
                int32_t t = op2(x, GP_FMUL, 0, A(5, 0), A(4, 0));
                for (int i = 1; i < m; i++) t = op3(x, GP_FMA, 0, A(5, i), A(4, i), t);
                int32_t t2 = op2(x, GP_FMUL, 0, t, con(x, 0x40000000u));
                for (int i = 0; i < n; i++) out->c[i] = op2(x, GP_FSUB, 0, A(4, i), op2(x, GP_FMUL, 0, t2, A(5, i)));
                return;
            }
            int32_t d[4];
            for (int i = 0; i < m; i++) d[i] = e == 67 ? op2(x, GP_FSUB, 0, A(4, i), A(5, i)) : A(4, i);
            int32_t t = op2(x, GP_FMUL, 0, d[0], d[0]);
            for (int i = 1; i < m; i++) t = op3(x, GP_FMA, 0, d[i], d[i], t);
            if (e == 69) {
                int32_t r = op1(x, GP_EXT1, 32, t);   // inversesqrt
                for (int i = 0; i < n; i++) out->c[i] = op2(x, GP_FMUL, 0, d[i], r);
            } else {
                out->c[0] = op1(x, GP_EXT1, 31, t);   // sqrt
            }
            return;
        }
        case 58: out->c[0] = op2(x, GP_EXT2, 58, A(4, 0), A(4, 1)); return;   // packHalf2x16
        case 62: case 61: case 60: case 64: case 63:   // unpack: per component (b: the component)
            for (int i = 0; i < n; i++) out->c[i] = op2(x, GP_EXT2, (int)e, A(4, 0), con(x, (uint32_t)i));
            return;
        default: break;
        }
        static const uint8_t u1[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 27, 28, 29, 30, 31, 32,
                                     73, 74, 75};
        static const uint8_t u2[] = {25, 26, 37, 38, 39, 40, 41, 42, 48, 53, 79, 80};
        static const uint8_t u3[] = {43, 44, 45, 46, 49, 50, 81};
        for (unsigned k = 0; k < sizeof u1; k++)
            if (e == u1[k] && na == 1) { for (int i = 0; i < n; i++) out->c[i] = op1(x, GP_EXT1, (int)e, A(4, i)); return; }
        for (unsigned k = 0; k < sizeof u2; k++)
            if (e == u2[k] && na == 2) { for (int i = 0; i < n; i++) out->c[i] = op2(x, GP_EXT2, (int)e, A(4, i), A(5, i)); return; }
        for (unsigned k = 0; k < sizeof u3; k++)
            if (e == u3[k] && na == 3) {
                for (int i = 0; i < n; i++) out->c[i] = op3(x, GP_EXT3, (int)e, A(4, i), A(5, i), A(6, i));
                return;
            }
    }
#undef A
    poison_val(x, out, n, __LINE__ + (int)op * 0);
    out->c[0] = op1(x, GP_POISON, (int)(op & 0xfff), 0);   // (the opcode, for the report)
}

// image operands after the coordinate: the LOD mode and its operand
static void sample_lod(Cx *x, const uint32_t *a, uint32_t len, uint32_t first, int *mode, int32_t *lod) {
    *mode = 0, *lod = con(x, 0);
    if (len <= first + 1) return;
    uint32_t mask = a[first], k = first + 1;
    if (mask & 1) { *mode = 2, *lod = comp(x, a[k], 0); k++; }          // Bias
    if (mask & 2) { *mode = 1, *lod = comp(x, a[k], 0); k++; }          // Lod
    if (mask & 4) { *mode = 0; k += 2; }                               // Grad: our cone's LOD
    (void)k;   // offsets are left out (a texel or two)
}

static void f_sample(Cx *x, uint32_t op, const uint32_t *a, uint32_t len) {
    uint32_t rt = a[0], rid = a[1];
    Val *img = V(x, a[2]);
    int n = 0, kind = type_of(x, rt, &n);
    Val *out = &x->val[rid];
    if (img->kind != K_IMG || img->tex < 0 || kind != K_F) { poison_val(x, out, n, __LINE__); return; }
    int arr = x->tex_arr[img->tex];
    int32_t u = comp(x, a[3], 0), v = comp(x, a[3], 1), l = arr ? comp(x, a[3], 2) : con(x, 0), lod;
    int mode;
    sample_lod(x, a, len, 4, &mode, &lod);
    int32_t d = vreg(x, 4);
    VIns *I = emit(x, op == S_FETCH ? GP_FETCH : GP_SMP, img->tex | mode << 5, d, u, v, l);
    I->nd = 4;
    emit(x, GP_ARG, 0, -1, op == S_FETCH && mode != 1 ? con(x, 0) : lod, 0, 0);
    out->kind = K_F, out->n = (uint8_t)n, out->tex = -1;
    for (int i = 0; i < n && i < 4; i++) out->c[i] = d + i;
}

static void f_query(Cx *x, const uint32_t *a, uint32_t len) {
    uint32_t rt = a[0], rid = a[1];
    Val *img = V(x, a[2]);
    int n = 0, kind = type_of(x, rt, &n);
    Val *out = &x->val[rid];
    if (img->kind != K_IMG || img->tex < 0 || kind == K_NONE || kind == K_F) { poison_val(x, out, n, __LINE__); return; }
    int32_t lod = len > 4 ? comp(x, a[3], 0) : con(x, 0);
    int32_t d = vreg(x, 4);
    VIns *I = emit(x, GP_TSIZE, img->tex, d, lod, 0, 0);
    I->nd = 4;
    out->kind = (uint8_t)kind, out->n = (uint8_t)n, out->tex = -1;
    for (int i = 0; i < n; i++) out->c[i] = d + i;
}

// ---- dead code and registers
// Dead code: backward from the outputs, an instruction is kept when a register it writes is live (a sample writes
// four; ARG goes with the instruction before it). live and keep: scratch of nv + 1 and nins + 1.
static void dce(Cx *x, const int32_t outs[6], uint8_t *live, uint8_t *keep) {
    int ni = x->nins;
    memset(live, 0, (size_t)x->nv + 1), memset(keep, 0, (size_t)ni + 1);
    for (int k = 0; k < 6; k++) if (outs[k] >= 0) live[outs[k]] = 1;
    for (int i = ni - 1; i >= 0; i--) {
        VIns *I = &x->ins[i];
        if (I->op == GP_ARG) continue;
        int need = 0;
        for (int k = 0; k < I->nd; k++) need |= live[I->d + k];
        if (!need) continue;
        keep[i] = 1;
        int reads_d = I->op == GP_SEL && (I->c == I->d || I->b == I->d);
        for (int k = 0; k < I->nd; k++) live[I->d + k] = 0;
        int32_t o[3] = {I->a, I->b, I->c};
        int no = I->op == GP_LDV || I->op == GP_LDU ? 0 : 3;
        for (int k = 0; k < no; k++) if (o[k] >= 0) live[o[k]] = 1;
        if (reads_d) live[I->d] = 1;
        if (i + 1 < ni && x->ins[i + 1].op == GP_ARG) {
            keep[i + 1] = 1;
            if (x->ins[i + 1].a >= 0) live[x->ins[i + 1].a] = 1;
        }
    }
    int m = 0;
    for (int i = 0; i < ni; i++) if (keep[i]) x->ins[m++] = x->ins[i];
    x->nins = m;
}

// Peephole, on the kept code (values defined once are SSA; Function variables stored more than once are not):
// - Select(IsNan(v), 0, v) (the translator's guard on texture coordinates) -> v
// - a negation folded into the add, subtract, multiply, divide or fma that reads it (aux bits 0-2: operand a, b, c)
// - fma from GLSL.std.450 as GP_FMA
// - a copy of a constant or of a value defined once: its uses read the source
static void peephole(Cx *x, int32_t outs[6]) {
    int nv = x->nv, ni = x->nins;
    int32_t *defi = malloc(sizeof(int32_t) * (size_t)(nv + 1)), *sub = malloc(sizeof(int32_t) * (size_t)(nv + 1));
    uint8_t *ndef = calloc((size_t)nv + 1, 1);
    if (!defi || !sub || !ndef) { free(defi), free(sub), free(ndef); return; }
    for (int v = 0; v < nv; v++) defi[v] = -1, sub[v] = v;
    for (int i = 0; i < ni; i++) {
        VIns *I = &x->ins[i];
        for (int k = 0; k < I->nd && I->d >= 0; k++) {
            int v = I->d + k;
            if (ndef[v] < 2) ndef[v]++;
            defi[v] = i;
        }
    }
#define SUB(o) ((o) >= 0 ? sub[(o)] : (o))
#define SSA(o) ((o) < 0 || ndef[(o)] == 1)
    for (int i = 0; i < ni; i++) {
        VIns *I = &x->ins[i];
        if (I->op != GP_LDV && I->op != GP_LDU) I->a = SUB(I->a), I->b = SUB(I->b), I->c = SUB(I->c);
        if (I->op == GP_EXT3 && I->aux == 50) I->op = GP_FMA, I->aux = 0;
        if (I->op == GP_SEL && I->nd == 1 && I->a >= 0 && ndef[I->a] == 1 && x->ins[defi[I->a]].op == GP_ISNAN &&
            x->ins[defi[I->a]].a == I->c && I->b < 0 && x->cpool[-I->b - 1] == 0 && I->d != I->c)
            I->op = GP_MOV, I->a = I->c, I->b = I->c = 0;
        if (I->op == GP_FADD || I->op == GP_FSUB || I->op == GP_FMUL || I->op == GP_FDIV || I->op == GP_FMA) {
            int32_t *op_[3] = {&I->a, &I->b, &I->c};
            for (int k = 0; k < (I->op == GP_FMA ? 3 : 2); k++) {
                int32_t o = *op_[k];
                if (o >= 0 && ndef[o] == 1 && x->ins[defi[o]].op == GP_FNEG && SSA(x->ins[defi[o]].a))
                    *op_[k] = x->ins[defi[o]].a, I->aux ^= (uint16_t)(1u << k);
            }
        }
        if (I->op == GP_MOV && I->nd == 1 && I->d >= 0 && ndef[I->d] == 1 && SSA(I->a)) sub[I->d] = I->a;
    }
    for (int k = 0; k < 6; k++) outs[k] = outs[k] == INT32_MIN ? INT32_MIN : SUB(outs[k]);
    // ARG operands too (they were substituted with the rest)
#undef SUB
#undef SSA
    free(defi), free(sub), free(ndef);
}

int gfx_prog_no_peephole;   // (test) the peephole pass off
int gfx_prog_no_preload;    // (debug) varyings and uniforms by instructions (LDV, LDU), not preloaded
static void finish(Cx *x, int32_t outs[6]) {
    int nv = x->nv, ni;
    uint8_t *live = calloc((size_t)nv + 1, 1), *keep = calloc((size_t)x->nins + 1, 1);
    int32_t *first = malloc(sizeof(int32_t) * (size_t)(nv + 1)), *last = malloc(sizeof(int32_t) * (size_t)(nv + 1));
    int32_t *phys = malloc(sizeof(int32_t) * (size_t)(nv + 1));
    if (!live || !keep || !first || !last || !phys) { FAIL(x, GFX_PROG_TOO_BIG); goto out; }
    dce(x, outs, live, keep);
    ni = x->nins;
    if (0) {
    // backward: an instruction is kept when a register it writes is live (a sample writes four; ARG goes with the
    // instruction before it)
    for (int i = ni - 1; i >= 0; i--) {
        VIns *I = &x->ins[i];
        if (I->op == GP_ARG) continue;
        int need = 0;
        for (int k = 0; k < I->nd; k++) need |= live[I->d + k];
        if (!need) continue;
        keep[i] = 1;
        int reads_d = I->op == GP_SEL && (I->c == I->d || I->b == I->d);
        for (int k = 0; k < I->nd; k++) live[I->d + k] = 0;
        int32_t o[3] = {I->a, I->b, I->c};
        int no = I->op == GP_LDV || I->op == GP_LDU ? 0 : 3;
        for (int k = 0; k < no; k++) if (o[k] >= 0) live[o[k]] = 1;
        if (reads_d) live[I->d] = 1;
        if (i + 1 < ni && x->ins[i + 1].op == GP_ARG) {
            keep[i + 1] = 1;
            if (x->ins[i + 1].a >= 0) live[x->ins[i + 1].a] = 1;
        }
    }
    }
    // a value the program cannot make that an output needs: no program
    for (int i = 0; i < ni; i++)
        if (x->ins[i].op == GP_POISON) { FAIL(x, GFX_PROG_UNSUPPORTED); x->P->fail_op = x->ins[i].aux; goto out; }
    // the varyings, uniforms and textures the program uses, numbered again
    {
        GfxProg *P = x->P;
        int lmap[XLOC], tmap[XTEX], *umap = malloc(sizeof(int) * XUNI);
        if (!umap) { FAIL(x, GFX_PROG_TOO_BIG); goto out; }
        for (int k = 0; k < XLOC; k++) lmap[k] = -1;
        for (int k = 0; k < XTEX; k++) tmap[k] = -1;
        for (int k = 0; k < XUNI; k++) umap[k] = -1;
        P->nloc = P->ntex = 0, P->nuni = 0, P->flat = 0;
        for (int i = 0; i < ni && !x->fail; i++) {
            VIns *I = &x->ins[i];
            if (I->op == GP_LDV) {
                int sl = I->aux >> 2;
                if (lmap[sl] < 0) {
                    if (P->nloc >= GFX_PROG_LOCS) { FAIL(x, GFX_PROG_TOO_BIG); break; }
                    lmap[sl] = P->nloc, P->loc[P->nloc] = x->xloc[sl];
                    if (x->xflat[sl]) P->flat |= (uint8_t)(1u << P->nloc);
                    P->nloc++;
                }
                I->aux = (uint16_t)(lmap[sl] * 4 + (I->aux & 3));
            } else if (I->op == GP_LDU) {
                int u = I->aux;
                if (umap[u] < 0) {
                    if (P->nuni >= GFX_PROG_UNI) { FAIL(x, GFX_PROG_TOO_BIG); break; }
                    umap[u] = P->nuni, P->uni_key[P->nuni++] = x->uni_key[u];
                }
                I->aux = (uint16_t)umap[u];
            } else if (I->op == GP_SMP || I->op == GP_FETCH || I->op == GP_TSIZE) {
                int t = I->aux & 31;
                if (tmap[t] < 0) {
                    if (P->ntex >= GFX_PROG_TEX) { FAIL(x, GFX_PROG_TOO_BIG); break; }
                    tmap[t] = P->ntex, P->tex_bind[P->ntex] = x->tex_bind[t], P->tex_arr[P->ntex] = x->tex_arr[t];
                    P->ntex++;
                }
                I->aux = (uint16_t)((I->aux & ~31) | tmap[t]);
            }
        }
        free(umap);
        if (x->fail) goto out;
    }
    if (!gfx_prog_no_peephole) peephole(x, outs);
    dce(x, outs, live, keep);
    ni = x->nins;
    for (int v = 0; v < nv; v++) first[v] = INT32_MAX, last[v] = -1, phys[v] = -1;
    for (int i = 0; i < ni; i++) {
        VIns *I = &x->ins[i];
        int32_t o[3] = {I->a, I->b, I->c};
        int no = I->op == GP_LDV || I->op == GP_LDU ? 0 : 3;
        for (int k = 0; k < no; k++) if (o[k] >= 0) { if (first[o[k]] > i) first[o[k]] = i; last[o[k]] = i; }
        for (int k = 0; k < I->nd && I->d >= 0; k++) { int v = I->d + k; if (first[v] > i) first[v] = i; if (last[v] < i) last[v] = i; }
    }
    for (int k = 0; k < 6; k++) if (outs[k] >= 0) last[outs[k]] = ni;   // live to the end
    // linear scan: at instruction i, registers whose last use was before i are free; a sample's four destinations
    // take four free registers in a row
    uint8_t busy[MAXP];
    int32_t owner[MAXP];
    memset(busy, 0, sizeof busy);
    int top = 0;
    for (int i = 0; i < ni; i++) {
        for (int r = 0; r < MAXP; r++)
            if (busy[r] && last[owner[r]] < i) busy[r] = 0;
        VIns *I = &x->ins[i];
        // a varying: no register of its own (the program's start interpolates every varying into fixed registers)
        if (I->op == GP_LDV && !gfx_prog_no_preload) { if (I->d >= 0) phys[I->d] = -1000 - (int32_t)I->aux; continue; }
        if (I->op == GP_LDU && !gfx_prog_no_preload) { if (I->d >= 0) phys[I->d] = -2000 - (int32_t)I->aux; continue; }   // (preloaded too)
        int32_t o[3] = {I->a, I->b, I->c};
        for (int k = 0; k < 3; k++)   // read before written (undefined): a register from here
            if (o[k] >= 0 && phys[o[k]] == -1 && I->op != GP_LDU) {
                int r = 0;
                while (r < MAXP && busy[r]) r++;
                if (r == MAXP) { FAIL(x, GFX_PROG_TOO_BIG); goto out; }
                busy[r] = 1, owner[r] = o[k], phys[o[k]] = r;
                if (r + 1 > top) top = r + 1;
            }
        if (I->d < 0 || phys[I->d] >= 0) continue;
        int nd = I->nd, r = 0;
        for (; r + nd <= MAXP; r++) {
            int ok = 1;
            for (int k = 0; k < nd && ok; k++) ok = !busy[r + k];
            if (ok) break;
        }
        if (r + nd > MAXP) { FAIL(x, GFX_PROG_TOO_BIG); goto out; }
        for (int k = 0; k < nd; k++) busy[r + k] = 1, owner[r + k] = I->d + k, phys[I->d + k] = r + k;
        if (r + nd > top) top = r + nd;
    }
    // encode: the constants the program uses take the first registers, its varyings (4 a slot, interpolated) the next
    // (both loaded at its start: every operand is then a register, no test per operand, and the varyings' loads
    // overlap), the registers after them; the varyings' instructions (LDV) are left out
    GfxProg *P = x->P;
    int nu = 0;
    uint32_t used[MAXC];
    int *cmap = malloc(sizeof(int) * (size_t)(x->ncon + 1));
    if (!cmap) { FAIL(x, GFX_PROG_TOO_BIG); goto out; }
    for (int k = 0; k <= x->ncon; k++) cmap[k] = -1;
#define USEC(o) do { int32_t o_ = (o); if (o_ < 0 && o_ != INT32_MIN) { int ci_ = -o_ - 1; if (cmap[ci_] < 0) cmap[ci_] = nu, used[nu++] = x->cpool[ci_]; } } while (0)
    for (int i = 0; i < ni; i++) {
        VIns *I = &x->ins[i];
        if (I->op == GP_LDV || I->op == GP_LDU) continue;
        USEC(I->a); USEC(I->b); USEC(I->c);
    }
    for (int k = 0; k < 6; k++) USEC(outs[k]);
#undef USEC
    int zero = -1;   // an output never written: 0
    for (int k = 0; k < 6; k++)
        if (outs[k] == INT32_MIN && zero < 0) {
            for (int i = 0; i < nu && zero < 0; i++) if (!used[i]) zero = i;
            if (zero < 0) zero = nu, used[nu++] = 0;
        }
    uint32_t nc4 = ((uint32_t)nu + 3) / 4, nvr = gfx_prog_no_preload ? 0 : 4u * P->nloc + P->nuni;   // varyings, uniforms
    int ne = 0;
    for (int i = 0; i < ni; i++) ne += gfx_prog_no_preload || (x->ins[i].op != GP_LDV && x->ins[i].op != GP_LDU);
    P->words = (3 + (uint32_t)ne + nc4) * 4;
    P->code = calloc(P->words, 4);
    if (!P->code) { free(cmap); FAIL(x, GFX_PROG_TOO_BIG); goto out; }
#define ENC(o) ((o) == INT32_MIN ? (uint32_t)zero : (o) < 0 ? (uint32_t)cmap[-(o) - 1]                 : phys[(o)] <= -2000 ? (uint32_t)nu + 4u * P->nloc + (uint32_t)(-2000 - phys[(o)]) \
                : phys[(o)] <= -1000 ? (uint32_t)nu + (uint32_t)(-1000 - phys[(o)]) : (uint32_t)nu + nvr + (uint32_t)phys[(o)])
    uint32_t *c = P->code;
    c[0] = (uint32_t)ne, c[1] = (uint32_t)nu, c[2] = (uint32_t)nu + nvr + (uint32_t)top,
    c[3] = P->flags | (gfx_prog_no_preload ? 0x8000u : (uint32_t)P->nuni << 16);   // 0x8000: nothing preloaded
    for (int k = 0; k < 6; k++) c[4 + k] = ENC(outs[k]);
    c[10] = P->nloc, c[11] = P->flat;
    int e = 0;
    for (int i = 0; i < ni; i++) {
        VIns *I = &x->ins[i];
        if (!gfx_prog_no_preload && (I->op == GP_LDV || I->op == GP_LDU)) continue;
        uint32_t *q = c + 12 + e++ * 4;
        int nops = I->op == GP_LDU || I->op == GP_LDV ? 0 : 3;
        q[0] = I->op | (uint32_t)(I->d >= 0 ? ENC(I->d) : 0) << 8 | (uint32_t)(I->aux & 0xfff) << 20;
        q[1] = nops > 0 ? ENC(I->a) : 0, q[2] = nops > 1 ? ENC(I->b) : 0, q[3] = nops > 2 ? ENC(I->c) : 0;
    }
#undef ENC
    memcpy(c + 12 + ne * 4, used, (size_t)nu * 4);
    free(cmap);
    P->nins = (uint16_t)ne, P->nregs = (uint16_t)(nu + nvr + (uint32_t)top), P->ncon = (uint16_t)nu;
out:
    free(live), free(keep), free(first), free(last), free(phys);
}

// a block (its label) whose first non-debug instruction ends it with a discard
static int kill_block(const Cx *x, const uint32_t *w, uint64_t words, uint32_t L) {
    if (L >= x->bound || !x->def[L]) return 0;
    uint64_t i = x->def[L];
    if ((w[i] & 0xffff) != S_LABEL) return 0;
    for (i += w[i] >> 16; i < words && (w[i] >> 16); i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff;
        if (op == 8 || op == 317) continue;   // OpLine, OpNoLine
        return op == S_KILL || op == S_TERMINATE;
    }
    return 0;
}

void gfx_prog_compile(const uint32_t *w, uint64_t words, GfxProg *P) {
    memset(P, 0, sizeof *P);
    P->status = GFX_PROG_UNSUPPORTED;
    if (words < 5 || w[0] != 0x07230203u) return;
    Cx X;
    memset(&X, 0, sizeof X);
    Cx *x = &X;
    x->w = w, x->bound = w[3] < (1u << 18) ? w[3] : (1u << 18), x->P = P;
    uint32_t B = x->bound;
    x->def = calloc(B, 4), x->tkind = calloc(B, 1), x->tn = calloc(B, 1), x->ptr_to = calloc(B, 4), x->sc = malloc(B);
    x->loc = malloc(B * 2), x->bind = malloc(B * 2), x->builtin = malloc(B * 2), x->flat = calloc(B, 1);
    x->val = calloc(B, sizeof(Val)), x->ac_base = calloc(B, 4), x->init = calloc(B, 4), x->ac_idx = calloc(B, sizeof *x->ac_idx), x->ac_n = calloc(B, 1), x->ac_val = calloc(B, sizeof *x->ac_val);
    x->nst = calloc(B, sizeof *x->nst), x->vr = malloc(B * sizeof *x->vr), x->ins = malloc(sizeof(VIns) * MAXI);
    int32_t *edge = calloc(B, 4);   // a block label's incoming predicate so far (0: none yet; else operand + 1 shifted)
    uint8_t *has_edge = calloc(B, 1);
    if (!x->def || !x->tkind || !x->tn || !x->ptr_to || !x->sc || !x->loc || !x->bind || !x->builtin || !x->flat ||
        !x->val || !x->ac_base || !x->init || !x->ac_idx || !x->ac_n || !x->ac_val || !x->nst || !x->vr || !x->ins || !edge || !has_edge) {
        P->status = GFX_PROG_TOO_BIG;
        goto out;
    }
    memset(x->sc, 0xff, B), memset(x->loc, 0xff, B * 2), memset(x->bind, 0xff, B * 2), memset(x->builtin, 0xff, B * 2);
    for (uint32_t i = 0; i < B; i++) for (int k = 0; k < 4; k++) x->vr[i][k] = INT32_MIN;
    for (uint32_t i = 0; i < B; i++) x->val[i].kind = K_NONE, x->val[i].tex = -1;
    // pass 1: declarations, types, constants, variables, and each id's defining instruction
    uint64_t fn_start = 0;
    for (uint64_t i = 5; i < words;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > words) { P->status = GFX_PROG_UNSUPPORTED; goto out; }
        const uint32_t *a = w + i + 1;
        if (op == S_FUNCTION && !fn_start) fn_start = i;
        if (op == S_LABEL && a[0] < B) x->def[a[0]] = (uint32_t)i;
        if (op == S_EXT_IMPORT && len >= 3 && !memcmp(a + 1, "GLSL.std.450", 12)) x->glsl = a[0];
        if (op == S_DECORATE && len >= 3 && a[0] < B) {
            if (a[1] == D_LOCATION && len >= 4) x->loc[a[0]] = (int16_t)a[2];
            else if (a[1] == D_BINDING && len >= 4) x->bind[a[0]] = (int16_t)a[2];
            else if (a[1] == D_BUILTIN && len >= 4) x->builtin[a[0]] = (int16_t)a[2];
            else if (a[1] == D_FLAT) x->flat[a[0]] = 1;
        }
        if (op >= S_TYPE_VOID && op <= S_TYPE_FUNCTION && len >= 2 && a[0] < B) {
            x->def[a[0]] = (uint32_t)i;
            if (op == S_TYPE_BOOL) x->tkind[a[0]] = K_B, x->tn[a[0]] = 1;
            else if (op == S_TYPE_INT && len >= 4 && a[1] == 32) x->tkind[a[0]] = a[2] ? K_I : K_U, x->tn[a[0]] = 1;
            else if (op == S_TYPE_FLOAT && len >= 3 && a[1] == 32) x->tkind[a[0]] = K_F, x->tn[a[0]] = 1;
            else if (op == S_TYPE_VECTOR && len >= 4 && a[1] < B && x->tn[a[1]] == 1 && a[2] <= 4)
                x->tkind[a[0]] = x->tkind[a[1]], x->tn[a[0]] = (uint8_t)a[2];
        } else if (op == S_VARIABLE && len >= 4 && a[1] < B) {
            x->def[a[1]] = (uint32_t)i, x->sc[a[1]] = (int8_t)a[2], x->ptr_to[a[1]] = a[0];
            if (len >= 5) x->init[a[1]] = a[3];   // an initializer: a store at the function's start
        } else if (len >= 3 && a[1] < B && a[0] < B && x->def[a[0]] && op != S_DECORATE && op != S_MEMBER_DECORATE &&
                   op != S_STORE && op != S_EXT_IMPORT && op != 5 && op != 6 && op != 15 && op != 16) {
            x->def[a[1]] = (uint32_t)i;
        }
        if (op == S_CONST_TRUE || op == S_CONST_FALSE) {
            x->val[a[1]].kind = K_B, x->val[a[1]].n = 1;
            x->val[a[1]].c[0] = con(x, op == S_CONST_TRUE);
        } else if (op == S_CONST && len >= 4) {
            int n = 0, k = type_of(x, a[0], &n);
            x->val[a[1]].kind = (uint8_t)k, x->val[a[1]].n = 1, x->val[a[1]].c[0] = con(x, a[2]);
        } else if (op == S_CONST_NULL || (op == S_UNDEF && !fn_start)) {
            int n = 0, k = type_of(x, a[0], &n);
            x->val[a[1]].kind = (uint8_t)k, x->val[a[1]].n = (uint8_t)n;
            for (int c = 0; c < n; c++) x->val[a[1]].c[c] = con(x, 0);
        } else if (op == S_CONST_COMPOSITE) {
            int n = 0, k = type_of(x, a[0], &n);
            if (k != K_NONE) {
                x->val[a[1]].kind = (uint8_t)k, x->val[a[1]].n = (uint8_t)n;
                for (int c = 0; c < n && 2 + c < (int)len - 1; c++) x->val[a[1]].c[c] = comp(x, a[2 + c], 0);
            }
        }
        i += len;
    }
    if (!fn_start || x->fail) { P->status = x->fail ? (uint8_t)x->fail : GFX_PROG_UNSUPPORTED; goto out; }
    // pass 2: access chains with constant indices, and the store count of each variable component
    for (uint64_t i = fn_start; i < words; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        if (op == S_LOOP_MERGE || op == S_PHI || op == S_SWITCH) { P->status = GFX_PROG_CONTROL_FLOW; goto out; }
        if (op == S_FUNCTION && i != fn_start) { P->status = GFX_PROG_CONTROL_FLOW; goto out; }   // calls
        if ((op == S_ACCESS_CHAIN || op == S_IN_BOUNDS_AC) && len >= 4 && a[1] < B) {
            uint32_t base = a[2];
            int n0 = 0;
            int16_t idx[3] = {0, 0, 0};
            uint32_t val[3] = {0, 0, 0};
            if (x->ac_base[base]) {   // a chain of a chain
                n0 = x->ac_n[base];
                memcpy(idx, x->ac_idx[base], sizeof idx);
                memcpy(val, x->ac_val[base], sizeof val);
                base = x->ac_base[base];
            }
            for (uint32_t k = 3; k < len - 1; k++) {
                uint32_t cid = a[k], v = 0;
                int ok = opc(x, cid) == S_CONST && lenof(x, cid) >= 4;
                if (ok) v = ops(x, cid)[2];
                if (n0 < 3) idx[n0] = ok && v < 32768 ? (int16_t)v : -1, val[n0] = cid;
                n0++;
            }
            if (n0 > 3) n0 = 3;
            x->ac_base[a[1]] = base, x->ac_n[a[1]] = (uint8_t)n0;
            memcpy(x->ac_idx[a[1]], idx, sizeof idx);
            memcpy(x->ac_val[a[1]], val, sizeof val);
        } else if (op == S_STORE && len >= 3 && 0) {
            int c, nidx;
            int16_t idx[3] = {0, 0, 0};
            uint32_t var = ptr_res(x, a[0], &c, &nidx, idx);
            if (is_reg_var(x, var)) {
                int n = var_comps(x, var);
                if (!n) { if (x->sc[var] != SC_OUTPUT) { P->status = GFX_PROG_UNSUPPORTED; goto out; } continue; }
                if (nidx > 1 || (nidx && idx[0] < 0)) { P->status = GFX_PROG_UNSUPPORTED; goto out; }
                for (int k = 0; k < (nidx ? 1 : n); k++) {
                    int ci = (nidx ? idx[0] : 0) + k;
                    if (ci < 4 && x->nst[var][ci] < 60000) x->nst[var][ci]++;
                }
            }
        }
    }
    for (uint32_t v = 1; v < B; v++) {   // initializers count as stores
        if (!x->init[v] || !is_reg_var(x, v)) continue;
        int n = var_comps(x, v);
        for (int k = 0; k < n && k < 4; k++) if (x->nst[v][k] < 60000) x->nst[v][k]++;
    }
    for (uint64_t i = fn_start; i < words; i += w[i] >> 16) {   // the stores
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        if (op == S_STORE && len >= 3) {
            int c, nidx;
            int16_t idx[3] = {0, 0, 0};
            uint32_t var = ptr_res(x, a[0], &c, &nidx, idx);
            if (is_reg_var(x, var)) {
                int n = var_comps(x, var);
                if (!n) { if (x->sc[var] != SC_OUTPUT) { P->status = GFX_PROG_UNSUPPORTED; goto out; } continue; }
                if (nidx > 1 || (nidx && idx[0] < 0)) { P->status = GFX_PROG_UNSUPPORTED; goto out; }
                for (int k = 0; k < (nidx ? 1 : n); k++) {
                    int ci = (nidx ? idx[0] : 0) + k;
                    if (ci < 4 && x->nst[var][ci] < 60000) x->nst[var][ci]++;
                }
            }
        }
    }
    // outputs: stored in the blocks of an if as well: always a variable of their own (a select keeps the old value)
    // pass 3: the body in program order
    int32_t pred = INT32_MIN;   // the current block's predicate (INT32_MIN: always)
    int in_block = 0, first_label = 1;
    for (uint64_t i = fn_start; i < words && !x->fail; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        switch (op) {
        case S_LABEL:
            if (first_label)   // the variables' initializers, before anything else
                for (uint32_t v = 1; v < B; v++) {
                    if (!x->init[v] || !is_reg_var(x, v)) continue;
                    int n = var_comps(x, v);
                    for (int k = 0; k < n && k < 4; k++) var_set(x, v, k, comp(x, x->init[v], k), INT32_MIN);
                }
            if (first_label) pred = INT32_MIN;   // the entry block
            else if (has_edge[a[0]] == 2) pred = INT32_MIN;   // reached unconditionally
            else pred = has_edge[a[0]] ? edge[a[0]] : con(x, 0);   // no way in: never runs
            first_label = 0, in_block = 1;
            break;
        case S_BRANCH:
        case S_BRANCH_COND: {
            uint32_t tgt[2] = {a[op == S_BRANCH ? 0 : 1], op == S_BRANCH ? 0 : a[2]};
            // a branch to a block that only discards (an alpha test): a hit there is cut by the rays' alpha test
            // (vk_rt.c), so the program goes on as if the other way were taken always (no predicate on all after it)
            if (op == S_BRANCH_COND && a[1] != a[2] && (kill_block(x, w, words, a[1]) || kill_block(x, w, words, a[2]))) {
                uint32_t L = kill_block(x, w, words, a[1]) ? a[2] : a[1];
                if (L < B && !(x->def[L] && x->def[L] < i)) {
                    if (!has_edge[L]) edge[L] = pred, has_edge[L] = pred == INT32_MIN ? 2 : 1;
                    else if (has_edge[L] != 2) {
                        if (pred == INT32_MIN) has_edge[L] = 2;
                        else edge[L] = op2(x, GP_LOR, 0, edge[L], pred);
                    }
                }
                in_block = 0;
                break;
            }
            for (int k = 0; k < (op == S_BRANCH ? 1 : 2); k++) {
                uint32_t L = tgt[k];
                if (L >= B) { FAIL(x, GFX_PROG_UNSUPPORTED); break; }
                if (x->def[L] && x->def[L] < i) { FAIL(x, GFX_PROG_CONTROL_FLOW); break; }   // a back edge
                int32_t e;   // this edge's predicate
                int always = 0;
                if (op == S_BRANCH || a[1] == a[2]) {
                    if (pred == INT32_MIN) always = 1;
                    e = pred;
                } else {
                    int32_t c = comp(x, a[0], 0);
                    if (k) c = op1(x, GP_LNOT, 0, c);
                    e = pred == INT32_MIN ? c : op2(x, GP_LAND, 0, pred, c);
                }
                if (op == S_BRANCH_COND && a[1] == a[2] && k) break;
                if (!has_edge[L]) edge[L] = e, has_edge[L] = always ? 2 : 1;
                else if (has_edge[L] != 2) {
                    if (always) has_edge[L] = 2;
                    else edge[L] = op2(x, GP_LOR, 0, edge[L], e);
                }
            }
            in_block = 0;
            break;
        }
        case S_KILL: case S_RETURN: case S_RETURN_VALUE: case S_UNREACHABLE: case S_TERMINATE:
            in_block = 0;
            break;
        case S_SELECTION_MERGE: case S_FUNCTION: case S_FUNCTION_END: case S_VARIABLE: case S_DEMOTE: case 8: case 317:
        case S_ACCESS_CHAIN: case S_IN_BOUNDS_AC: case 54 + 1 /* FunctionParameter */:
            break;
        case S_LOAD: f_load(x, a[0], a[1], a[2]); break;
        case S_STORE: f_store(x, a[0], a[1], pred); break;
        case S_SAMPLE_IMPLICIT: case S_SAMPLE_EXPLICIT: case S_FETCH: f_sample(x, op, a, len); break;
        case S_QUERY_SIZE_LOD: case S_QUERY_SIZE: f_query(x, a, len); break;
        default:
            if (len >= 3) f_value(x, op, a, len);
            break;
        }
        if (x->fail && !P->fail_op) P->fail_op = (uint16_t)op;
    }
    if (x->fail) { P->status = (uint8_t)x->fail; goto out; }
    // the outputs' final values: locations 1 (albedo) and 5 (glow), rgb
    int32_t outs[6];
    for (int k = 0; k < 6; k++) outs[k] = INT32_MIN;
    int found = 0;
    for (uint32_t v = 1; v < B; v++) {
        if (x->sc[v] != SC_OUTPUT || (x->loc[v] != 1 && x->loc[v] != 5)) continue;
        int base = x->loc[v] == 1 ? 0 : 3;
        found |= x->loc[v] == 1 ? 1 : 2;
        for (int c = 0; c < 3; c++) {
            if (!x->nst[v][c]) continue;
            int32_t o = x->nst[v][c] <= 1 ? x->vr[v][c] : x->vr[v][c];
            if (o == INT32_MIN) continue;
            if (o < 0) {   // a constant: through a register (the header holds operands, a constant is fine too)
                outs[base + c] = o;
                continue;
            }
            outs[base + c] = o;
        }
    }
    if (!(found & 1)) { P->status = GFX_PROG_NO_ALBEDO; goto out; }
    int has_glow = outs[3] != INT32_MIN || outs[4] != INT32_MIN || outs[5] != INT32_MIN;
    int saved_nins = x->nins;
    VIns *saved_ins = NULL;
    int32_t saved_outs[3] = {outs[0], outs[1], outs[2]};
    if (has_glow) {
        P->flags |= GFX_PROG_GLOW;
        saved_ins = malloc(sizeof(VIns) * (size_t)saved_nins);
        if (saved_ins) memcpy(saved_ins, x->ins, sizeof(VIns) * (size_t)saved_nins);
    }
    finish(x, outs);   // (register outputs live to the end; constant ones and INT32_MIN (0) encoded by finish)
    if (has_glow && saved_ins && (x->fail || P->nregs > 112)) {
        free(P->code), P->code = NULL;
        x->fail = 0, P->fail_op = 0, P->flags &= ~GFX_PROG_GLOW;
        x->nins = saved_nins;
        memcpy(x->ins, saved_ins, sizeof(VIns) * (size_t)saved_nins);
        outs[0] = saved_outs[0], outs[1] = saved_outs[1], outs[2] = saved_outs[2];
        outs[3] = outs[4] = outs[5] = INT32_MIN;
        finish(x, outs);
    }
    free(saved_ins);
    if (x->fail) { free(P->code), P->code = NULL; P->status = (uint8_t)x->fail; goto out; }
    for (int i = 0; i < P->nins; i++) {
        uint32_t o = P->code[12 + i * 4] & 0xff;
        if (o == GP_SMP || o == GP_FETCH) P->nsmp++;
    }
    P->status = GFX_PROG_OK;
out:
    free(x->def), free(x->tkind), free(x->tn), free(x->ptr_to), free(x->sc), free(x->loc), free(x->bind), free(x->builtin);
    free(x->flat), free(x->val), free(x->ac_base), free(x->init), free(x->ac_idx), free(x->ac_n), free(x->ac_val), free(x->nst), free(x->vr), free(x->ins);
    free(edge), free(has_edge);
}
