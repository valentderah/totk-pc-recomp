// Conservative write lanes of scalar u32 SSBO stores, modulo a 16-byte block. Unknown writes stay dense.
// The NVN c0 address-table assumption is used only with all c0 buffer bases aligned to 16 bytes at replay time.
// Bits.mask identifies known bits of an integer bit pattern. Addition/negation keep a known low prefix; shifts,
// bitwise operations and aligned multiplication can establish the low two bits of a u32 array index. Joining all
// assignments to a local variable deliberately ignores branch feasibility, retaining a superset of possible stores.
#include "gfx.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t mask, value;
} Bits;
typedef struct {
    const uint32_t *op;
    uint32_t stores, descriptor_set;
    uint8_t state, stride4, offset0;
    Bits bits;
} Def;
typedef struct {
    uint32_t value, next;
} Store;
typedef struct {
    Def *id;
    Store *store;
    uint32_t bound, c0;
} Analysis;

static unsigned prefix(uint32_t m) {
    unsigned n = 0;
    while (n < 32 && (m & 1))
        n++, m >>= 1;
    return n;
}
static unsigned zeros(uint32_t v) {
    unsigned n = 0;
    if (!v)
        return 32;
    while (!(v & 1))
        n++, v >>= 1;
    return n;
}
static Bits low_bits(unsigned n, uint32_t v) {
    uint32_t m = n >= 32 ? UINT32_MAX : (1u << n) - 1;
    return (Bits){m, v & m};
}
static Bits join_bits(Bits x, Bits y) {
    uint32_t m = x.mask & y.mask & ~(x.value ^ y.value);
    return (Bits){m, x.value & m};
}
static unsigned min_u(unsigned a, unsigned b) {
    return a < b ? a : b;
}

static Bits evaluate(Analysis *s, uint32_t id, unsigned depth) {
    Bits r = {0};
    if (!id || id >= s->bound || depth > 128)
        return r;
    Def *d = s->id + id;
    if (d->state == 2)
        return d->bits;
    if (d->state == 1 || !d->op)
        return r; // recursive/unsupported expressions are unknown
    d->state = 1;
    const uint32_t *a = d->op;
    unsigned op = a[0] & 65535, n = a[0] >> 16;
#define E(i) evaluate(s, (i), depth + 1)
    if (op == 43 && n == 4)
        r = (Bits){UINT32_MAX, a[3]};
    else if ((op == 83 || op == 124) && n == 4)
        r = E(a[3]);
    else if (op == 61 && n >= 4 && a[3] < s->bound) {
        Def *p = s->id + a[3];
        const uint32_t *q = p->op;
        if (q && (q[0] & 65535) == 59 && q[3] == 7) {
            int first = 1;
            for (uint32_t k = p->stores; k; k = s->store[k].next) {
                Bits v = E(s->store[k].value);
                r = first ? v : join_bits(r, v);
                first = 0;
            }
        } else if (q && (q[0] & 65535) == 65 && q[0] >> 16 == 7 && q[3] == s->c0 && !s->id[s->c0].descriptor_set) {
            Bits row = E(q[5]), component = E(q[6]);
            if (row.mask == UINT32_MAX && row.value >= 33 && row.value <= 64 && component.mask == UINT32_MAX &&
                !component.value)
                r = (Bits){15, 0};
        }
    } else if (op == 126 && n == 4) {
        Bits x = E(a[3]);
        r = low_bits(prefix(x.mask), 0u - x.value);
    } else if (op == 200 && n == 4) {
        Bits x = E(a[3]);
        r = (Bits){x.mask, ~x.value & x.mask};
    } else if ((op == 128 || op == 130 || op == 132 || (op >= 194 && op <= 199)) && n == 5) {
        Bits x = E(a[3]), y = E(a[4]);
        unsigned nx = prefix(x.mask), ny = prefix(y.mask);
        if (op == 128)
            r = low_bits(min_u(nx, ny), x.value + y.value);
        else if (op == 130)
            r = low_bits(min_u(nx, ny), x.value - y.value);
        else if (op == 132) {
            unsigned tx = min_u(zeros(x.value), nx), ty = min_u(zeros(y.value), ny);
            r = low_bits(min_u(32, min_u(nx + ty, min_u(ny + tx, nx + ny))), x.value * y.value);
        } else if ((op == 194 || op == 195 || op == 196) && y.mask == UINT32_MAX && y.value < 32) {
            unsigned shift = y.value;
            if (op == 196)
                r = (Bits){(x.mask << shift) | (shift ? (1u << shift) - 1 : 0), x.value << shift};
            else if (op == 194)
                r = (Bits){(x.mask >> shift) | ~(UINT32_MAX >> shift), x.value >> shift};
            else
                r = (Bits){x.mask >> shift, x.value >> shift};
            if (!shift)
                r = x;
        } else if (op == 199)
            r = (Bits){(x.mask & ~x.value) | (y.mask & ~y.value) | (x.mask & y.mask), x.value & y.value};
        else if (op == 197)
            r = (Bits){(x.mask & x.value) | (y.mask & y.value) | (x.mask & y.mask), x.value | y.value};
        else if (op == 198)
            r = (Bits){x.mask & y.mask, x.value ^ y.value};
    } else if (op == 169 && n == 6)
        r = join_bits(E(a[4]), E(a[5]));
    else if (op == 245 && n >= 5 && (n & 1)) {
        r = E(a[3]);
        for (unsigned i = 5; i < n; i += 2)
            r = join_bits(r, E(a[i]));
    }
#undef E
    r.value &= r.mask;
    d->bits = r;
    d->state = 2;
    return r;
}

static int typed_result(unsigned op) {
    return op == 41 || op == 42 || op == 43 || op == 44 || op == 59 || op == 61 || op == 65 || op == 66 || op == 83 ||
           op == 124 || op == 126 || op == 128 || op == 130 || op == 132 || op == 169 || (op >= 194 && op <= 200) ||
           op == 245;
}
static uint32_t root_id(const Analysis *s, uint32_t id, unsigned depth) {
    if (!id || id >= s->bound || depth > 64)
        return 0;
    const uint32_t *a = s->id[id].op;
    if (!a)
        return 0;
    unsigned op = a[0] & 65535;
    if (op == 59)
        return id;
    if (op == 65 || op == 66 || op == 83)
        return root_id(s, a[3], depth + 1);
    return 0;
}

void gfx_spirv_store_masks(const uint32_t *w, uint64_t words, GfxShaderRes *res, int count) {
    for (int k = 0; k < count; k++)
        res[k].write_mask = 0;
    if (words < 5 || w[3] > (1u << 20) || w[3] < 2 || count <= 0)
        return;
    Analysis s = {.bound = w[3]};
    s.id = calloc(s.bound, sizeof *s.id);
    s.store = calloc((size_t)(words / 3 + 1), sizeof *s.store);
    uint32_t *resource = calloc(s.bound, sizeof *resource);
    uint8_t *masks = calloc((size_t)count, 1);
    if (!s.id || !s.store || !resource || !masks)
        goto out;
    uint32_t ns = 0, glsl = 0;
    for (uint64_t i = 5; i < words;) {
        unsigned n = w[i] >> 16, op = w[i] & 65535;
        if (!n || i + n > words)
            goto out;
        const uint32_t *a = w + i;
        if ((op >= 19 && op <= 33) && n >= 2 && a[1] < s.bound)
            s.id[a[1]].op = a;
        else if (typed_result(op) && n >= 3 && a[2] < s.bound)
            s.id[a[2]].op = a;
        if (op == 5 && n >= 4 && a[1] < s.bound && memchr(a + 2, 0, (n - 2) * 4) &&
            !strcmp((const char *)(a + 2), "cp_c0"))
            s.c0 = a[1];
        if (op == 11 && n >= 4 && memchr(a + 2, 0, (n - 2) * 4) && !strcmp((const char *)(a + 2), "GLSL.std.450"))
            glsl = a[1];
        if (op == 12 && (n < 5 || a[3] != glsl || a[4] == 35 || a[4] == 51))
            goto out; // Modf/Frexp may write through pointers
        if (op == 62 && n >= 3 && a[1] < s.bound) {
            s.store[++ns] = (Store){a[2], s.id[a[1]].stores};
            s.id[a[1]].stores = ns;
        }
        if (op == 59 && n > 4 && a[3] == 7)
            goto out; // Function initializers are not included in the assignment join.
        if (op == 71 && n == 4 && a[1] < s.bound && a[2] == 6 && a[3] == 4)
            s.id[a[1]].stride4 = 1;
        if (op == 71 && n == 4 && a[1] < s.bound && a[2] == 34)
            s.id[a[1]].descriptor_set = a[3];
        if (op == 72 && n == 5 && a[1] < s.bound && !a[2] && a[3] == 35 && !a[4])
            s.id[a[1]].offset0 = 1;
        // These can write through aliases not covered by the scalar access-chain proof below.
        if (op == 55 || op == 57 || op == 63 || op == 64 || (op >= 227 && op <= 242) || op == 318 || op == 319 ||
            op >= 400)
            goto out;
        i += n;
    }
    for (uint64_t i = 5; i < words; i += w[i] >> 16) {
        const uint32_t *a = w + i;
        if ((a[0] & 65535) != 71 || a[0] >> 16 != 4 || a[1] >= s.bound || a[2] != 33)
            continue;
        for (int k = 0; k < count; k++)
            if ((res[k].flags & 1) && res[k].set == 0 && !s.id[a[1]].descriptor_set && res[k].binding == a[3])
                resource[a[1]] = k + 1;
    }
    for (uint64_t i = 5; i < words; i += w[i] >> 16) {
        const uint32_t *a = w + i;
        if ((a[0] & 65535) != 62)
            continue;
        uint32_t root = root_id(&s, a[1], 0);
        if (!root)
            goto out;
        const uint32_t *var = s.id[root].op;
        if (var[3] == 7 && a[1] != root)
            goto out; // An aliased Function store is not in the direct variable's assignment list.
        if (var[3] != 2 && var[3] != 12)
            continue;
        if (!resource[root])
            goto out;
        int k = (int)resource[root] - 1;
        const uint32_t *p = a[1] < s.bound ? s.id[a[1]].op : NULL;
        uint32_t type = var[1] < s.bound && s.id[var[1]].op ? s.id[var[1]].op[3] : 0;
        const uint32_t *st = type < s.bound ? s.id[type].op : NULL;
        uint32_t array = st && (st[0] & 65535) == 30 && st[0] >> 16 == 3 ? st[2] : 0;
        const uint32_t *ar = array < s.bound ? s.id[array].op : NULL;
        uint32_t scalar = ar && (ar[0] & 65535) == 29 && ar[0] >> 16 == 3 ? ar[2] : 0;
        const uint32_t *sc = scalar < s.bound ? s.id[scalar].op : NULL;
        if (!p || (p[0] & 65535) != 65 || p[0] >> 16 != 6 || !st || !s.id[type].offset0 || !ar ||
            !s.id[array].stride4 || !sc || (sc[0] & 65535) != 21 || sc[2] != 32) {
            masks[k] = 15;
            continue;
        }
        Bits member = evaluate(&s, p[4], 0), index = evaluate(&s, p[5], 0);
        if (member.mask != UINT32_MAX || member.value || (index.mask & 3) != 3)
            masks[k] = 15;
        else
            masks[k] |= (uint8_t)(1u << (index.value & 3));
    }
    for (int k = 0; k < count; k++)
        if (masks[k] && masks[k] != 15)
            res[k].write_mask = masks[k];
out:
    free(s.id);
    free(s.store);
    free(resource);
    free(masks);
}
