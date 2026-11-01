// Adapted from LAS Recomp runtime/loader.c @ da35b83 (MIT, Copyright (c) 2026 Peiikai) and MacNativeGames
// games/totk/runtime/loader.c and scripts/prepare_runtime.py @ b75be8c (MIT, Copyright (c) 2026 MacNativeGames
// contributors).
// NSO loading at the fixed layout bases, dynamic linking with host overrides, the nnMain identity check, sealing,
// and --probe-loader.
//
// The lifted C was generated from the unrelocated words of .text and .rodata and embeds absolute guest addresses, so
// each module must sit at its layout base and relocations may only write the data segment.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "layout.h"
#include "platform/platform.h"

// hostfn.c, for the loader only.
uint64_t hostfn_register_observer(const char *name, GuestFn fn, uint64_t original);

enum { DT_NULL = 0, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
       DT_INIT = 12, DT_FINI = 13, DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27,
       DT_FINI_ARRAYSZ = 28, DT_RELRSZ = 0x23, DT_RELR = 0x24, DT_GNU_HASH = 0x6ffffef5 };
enum { R_ABS64 = 257, R_GLOB_DAT = 1025, R_JUMP_SLOT = 1026, R_RELATIVE = 1027 };
enum { K_RELATIVE, K_ABS64, K_GLOB_DAT, K_JUMP_SLOT, K_COUNT };
enum { STB_WEAK = 2, STT_FUNC = 2 };

typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } Elf64Sym;
typedef struct { uint64_t off, info; int64_t addend; } Elf64Rela;

// How a symbol of a relocation was bound.
enum { RES_UNSET, RES_GUEST, RES_OVERRIDE, RES_OBSERVER, RES_WEAK, RES_STUB };
typedef struct { uint64_t value; uint8_t kind; } Res;

typedef struct {
    RtLoaded pub;
    uint32_t seg_off[3], seg_size[3];   // text, rodata, data (data without bss)
    const Elf64Sym *syms;
    const char *strtab;
    uint32_t nsyms;
    const Elf64Rela *rela, *jmprel;
    uint64_t nrela, njmprel;
    const uint64_t *relr;               // DT_RELR (1.4.3): packed RELATIVE relocations
    uint64_t nrelr, relr_count;
    uint32_t *hash, hash_mask;          // defined symbols by name
    Res *res_plain, *res_jump;          // per symbol; JUMP_SLOT may bind an observer
    uint32_t *funcs, nfuncs;            // defined functions sorted by value (ldr_describe)
    uint64_t *plt_slot, *plt_stub, nplt;   // PLT stubs sorted by GOT slot (ldr_plt_stub), built on first use
    // statistics
    uint64_t kinds[K_COUNT], sym_import, sym_own;
    uint32_t imports, import_to[MOD_COUNT], weak_unresolved, strong_unresolved;
    uint32_t init_off;
} Mod;

static Mod mods[MOD_COUNT];
static int nmods;
static uint64_t identity_refs, stubs;
static int *ovr_used, *obs_used;   // per linked table entry, plus probe overrides counted separately
static PlatMutex plt_lock = PLAT_MUTEX_INIT;

int ldr_count(void) { return nmods; }
const RtLoaded *ldr_module(int i) { return i >= 0 && i < nmods ? &mods[i].pub : NULL; }

static uint32_t rd32(uint64_t a) { return LD32(a); }

// ---- files
static uint8_t *read_file(const char *path, size_t *len) {
    uint64_t size = 0;
    uint8_t *p = plat_file_read(path, &size);
    if (p) *len = (size_t)size;
    return p;
}

static void hex32(const uint8_t d[32], char out[65]) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

uint8_t *ldr_read_verified(const char *dir, const char *file, size_t *len) {
    const char *want = NULL;
    for (int i = 0; i < rt_exefs_sha256_count; i++)
        if (!strcmp(rt_exefs_sha256[i].file, file)) want = rt_exefs_sha256[i].sha256;
    if (!want) rt_fatal("loader: no expected SHA-256 for %s in modules.c", file);
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    uint8_t *p = read_file(path, len);
    if (!p) rt_fatal("loader: cannot read %s (extract the game first: tools/extract.py)", path);
    uint8_t d[32];
    char hex[65];
    rt_sha256(p, *len, d);
    hex32(d, hex);
    if (strcmp(hex, want)) rt_fatal("loader: %s has SHA-256 %s, expected %s", path, hex, want);
    return p;
}

// ---- NSO
static size_t lz4_decompress(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen) {
    const uint8_t *s = src, *se = src + srclen;
    uint8_t *d = dst, *de = dst + dstlen;
    while (s < se) {
        uint8_t tok = *s++;
        size_t lit = tok >> 4;
        if (lit == 15) { uint8_t b; do { if (s >= se) return 0; b = *s++; lit += b; } while (b == 255); }
        if ((size_t)(de - d) < lit || (size_t)(se - s) < lit) return 0;
        memcpy(d, s, lit);
        d += lit;
        s += lit;
        if (s >= se) break;
        if (se - s < 2) return 0;
        size_t off = s[0] | (s[1] << 8);
        s += 2;
        size_t ml = tok & 15;
        if (ml == 15) { uint8_t b; do { if (s >= se) return 0; b = *s++; ml += b; } while (b == 255); }
        ml += 4;
        if (!off || off > (size_t)(d - dst) || (size_t)(de - d) < ml) return 0;
        uint8_t *m = d - off;
        while (ml--) *d++ = *m++;
    }
    return (size_t)(d - dst);
}

typedef struct { uint32_t file_off, mem_off, size, csize; int compressed; } Seg;

static uint64_t nso_segments(const uint8_t *raw, size_t len, const char *name, Seg seg[3]) {
    if (len < 0x100 || memcmp(raw, "NSO0", 4)) rt_fatal("loader: %s is not an NSO", name);
    uint32_t flags = *(const uint32_t *)(raw + 0xC);
    for (int i = 0; i < 3; i++) {
        memcpy(&seg[i], raw + 0x10 + i * 0x10, 12);
        seg[i].csize = *(const uint32_t *)(raw + 0x60 + i * 4);
        seg[i].compressed = (flags >> i) & 1;
        if ((uint64_t)seg[i].file_off + (seg[i].compressed ? seg[i].csize : seg[i].size) > len)
            rt_fatal("loader: %s segment %d lies outside the file", name, i);
    }
    uint32_t bss = *(const uint32_t *)(raw + 0x3C);
    return ((uint64_t)seg[2].mem_off + seg[2].size + bss + 0xFFF) & ~0xFFFull;
}

static void nso_segment(const uint8_t *raw, const Seg *s, uint8_t *dst, const char *name, int i) {
    if (!s->compressed) {
        memcpy(dst, raw + s->file_off, s->size);
    } else if (lz4_decompress(raw + s->file_off, s->csize, dst, s->size) != s->size) {
        rt_fatal("loader: %s segment %d: LZ4 data is corrupt", name, i);
    }
}

// ---- symbols
static const char *sym_name(const Mod *m, uint32_t i) { return m->strtab + m->syms[i].name; }
static int sym_defined(const Mod *m, uint32_t i) { return m->syms[i].shndx != 0; }

static uint32_t fnv(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static void make_hash(Mod *m) {
    uint32_t cap = 16;
    while (cap < m->nsyms * 2) cap <<= 1;
    m->hash = calloc(cap, sizeof *m->hash);
    if (!m->hash) rt_fatal("loader: symbol hash allocation");
    m->hash_mask = cap - 1;
    for (uint32_t i = 1; i < m->nsyms; i++) {
        if (!sym_defined(m, i) || !*sym_name(m, i)) continue;
        const char *s = sym_name(m, i);
        uint32_t p = fnv(s) & m->hash_mask;
        while (m->hash[p] && strcmp(s, sym_name(m, m->hash[p]))) p = (p + 1) & m->hash_mask;
        if (!m->hash[p]) m->hash[p] = i;
    }
}

static uint64_t module_symbol(const Mod *m, const char *name) {
    for (uint32_t p = fnv(name) & m->hash_mask; m->hash[p]; p = (p + 1) & m->hash_mask) {
        uint32_t i = m->hash[p];
        if (!strcmp(name, sym_name(m, i))) return m->pub.base + m->syms[i].value;
    }
    return 0;
}

// The defined symbol in load order (main, subsdk0, sdk); *which is its module index.
static uint64_t find_symbol(const char *name, int *which) {
    for (int i = 0; i < nmods; i++) {
        uint64_t a = module_symbol(&mods[i], name);
        if (a) {
            if (which) *which = i;
            return a;
        }
    }
    return 0;
}

uint64_t ldr_symbol(const char *name) { return find_symbol(name, NULL); }

static const Mod *sort_mod;
static int by_value(const void *a, const void *b) {
    uint64_t x = sort_mod->syms[*(const uint32_t *)a].value, y = sort_mod->syms[*(const uint32_t *)b].value;
    return x < y ? -1 : x > y;
}

static void make_funcs(Mod *m) {
    m->funcs = malloc((m->nsyms ? m->nsyms : 1) * sizeof *m->funcs);
    if (!m->funcs) rt_fatal("loader: function table allocation");
    for (uint32_t i = 1; i < m->nsyms; i++)
        if (sym_defined(m, i) && (m->syms[i].info & 0xF) == STT_FUNC && *sym_name(m, i)) m->funcs[m->nfuncs++] = i;
    sort_mod = m;
    qsort(m->funcs, m->nfuncs, sizeof *m->funcs, by_value);
}

// ---- loading
static void parse_dynamic(Mod *m) {
    uint64_t base = m->pub.base;
    uint64_t mod0 = base + rd32(base + 4);
    if (memcmp((const void *)(uintptr_t)mod0, "MOD0", 4)) rt_fatal("loader: %s has no MOD0 header", m->pub.name);
    uint64_t dyn = mod0 + (int64_t)(int32_t)rd32(mod0 + 4);
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0, hash = 0, gnu_hash = 0, relr = 0, relrsz = 0;
    for (const uint64_t *d = (const uint64_t *)(uintptr_t)dyn; d[0] != DT_NULL; d += 2) {
        switch (d[0]) {
        case DT_SYMTAB: m->syms = (const Elf64Sym *)(uintptr_t)(base + d[1]); break;
        case DT_STRTAB: m->strtab = (const char *)(uintptr_t)(base + d[1]); break;
        case DT_HASH: hash = base + d[1]; break;
        case DT_GNU_HASH: gnu_hash = base + d[1]; break;
        case DT_RELR: relr = base + d[1]; break;
        case DT_RELRSZ: relrsz = d[1]; break;
        case DT_RELA: rela = base + d[1]; break;
        case DT_RELASZ: relasz = d[1]; break;
        case DT_JMPREL: jmprel = base + d[1]; break;
        case DT_PLTRELSZ: pltsz = d[1]; break;
        case DT_INIT: m->pub.init = base + d[1]; m->init_off = (uint32_t)d[1]; break;
        case DT_INIT_ARRAY: m->pub.init_array = base + d[1]; break;
        case DT_INIT_ARRAYSZ: m->pub.init_array_size = d[1]; break;
        }
    }
    if (!m->syms || !m->strtab || (!hash && !gnu_hash))
        rt_fatal("loader: %s lacks DT_SYMTAB, DT_STRTAB or a symbol hash", m->pub.name);
    if (hash) {
        m->nsyms = rd32(hash + 4);   // nchain
    } else {
        // DT_GNU_HASH only (1.4.3 sdk, subsdk0): one past the last symbol the highest bucket's chain reaches.
        uint32_t nbuckets = rd32(gnu_hash), symoffset = rd32(gnu_hash + 4), bloom = rd32(gnu_hash + 8);
        uint64_t buckets = gnu_hash + 16 + 8ull * bloom, chains = buckets + 4ull * nbuckets;
        uint32_t last = 0;
        for (uint32_t i = 0; i < nbuckets; i++)
            if (rd32(buckets + 4ull * i) > last) last = rd32(buckets + 4ull * i);
        if (last < symoffset) {
            m->nsyms = symoffset;
        } else {
            while (!(rd32(chains + 4ull * (last - symoffset)) & 1)) last++;
            m->nsyms = last + 1;
        }
    }
    m->relr = (const uint64_t *)(uintptr_t)relr;
    m->nrelr = relrsz / 8;
    m->rela = (const Elf64Rela *)(uintptr_t)rela;
    m->nrela = relasz / sizeof(Elf64Rela);
    m->jmprel = (const Elf64Rela *)(uintptr_t)jmprel;
    m->njmprel = pltsz / sizeof(Elf64Rela);
}

// The constructor state flag DT_INIT checks: ADRP xN at DT_INIT+16, LDR wM, [xN, #imm] at +20 or +24
// (MNG scripts/prepare_runtime.py).
static uint64_t decode_ctor_flag(const Mod *m) {
    uint64_t init = m->pub.init;
    if (!init) return 0;
    uint32_t a = rd32(init + 16);
    if ((a & 0x9f000000u) != 0x90000000u) return 0;
    uint32_t reg = a & 31;
    for (int at = 20; at <= 24; at += 4) {
        uint32_t w = rd32(init + at);
        if ((w & 0xffc00000u) != 0xb9400000u || ((w >> 5) & 31) != reg) continue;
        int64_t imm = (int64_t)(((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2));
        imm = (imm ^ (1 << 20)) - (1 << 20);
        uint64_t flag = ((init + 16) & ~0xFFFull) + (uint64_t)(imm << 12) + ((w >> 10) & 0xfff) * 4;
        uint64_t data = m->pub.base + m->pub.data_off;
        return flag >= data && flag < m->pub.base + m->pub.size ? flag : 0;
    }
    return 0;
}

static void load_module(Mod *m, const char *dir, const RtLayoutEntry *lay) {
    size_t len;
    uint8_t *raw = ldr_read_verified(dir, lay->name, &len);
    Seg seg[3];
    uint64_t size = nso_segments(raw, len, lay->name, seg);
    if (size != lay->size)
        rt_fatal("loader: %s image is %#llx bytes, layout.h says %#llx: generated C and game files differ", lay->name,
                 (unsigned long long)size, (unsigned long long)lay->size);
    if (!plat_mem_commit(lay->base, size))
        rt_fatal("loader: cannot commit %s at %#llx", lay->name, (unsigned long long)lay->base);
    for (int i = 0; i < 3; i++) {
        if ((uint64_t)seg[i].mem_off + seg[i].size > size) rt_fatal("loader: %s segment %d exceeds the image", lay->name, i);
        nso_segment(raw, &seg[i], (uint8_t *)(uintptr_t)(lay->base + seg[i].mem_off), lay->name, i);
        m->seg_off[i] = seg[i].mem_off;
        m->seg_size[i] = seg[i].size;
    }
    free(raw);
    m->pub.name = lay->name;
    m->pub.base = lay->base;
    m->pub.size = size;
    m->pub.text_end = seg[0].mem_off + seg[0].size;
    m->pub.data_off = seg[2].mem_off;
    if (m->pub.data_off & 0xFFF) rt_fatal("loader: %s data segment is not page aligned", lay->name);
    if (seg[1].mem_off < m->pub.text_end || seg[1].mem_off + seg[1].size > m->pub.data_off)
        rt_fatal("loader: %s segments are out of order", lay->name);
    parse_dynamic(m);
    make_hash(m);
    make_funcs(m);
    m->res_plain = calloc(m->nsyms, sizeof(Res));
    m->res_jump = calloc(m->nsyms, sizeof(Res));
    if (!m->res_plain || !m->res_jump) rt_fatal("loader: resolution table allocation");
}

// ---- relocation
// A strong import nothing defines: calling it is fatal with its name (expected count 0).
static void unresolved_stub(Cpu *c) {
    char lr[96];
    rt_fatal("call to unresolved import %s from %s", hostfn_name(c->pc), ldr_describe(c->x[30] - 4, lr, sizeof lr));
}

static void mark_used(int observers, const char *name) {
    int n = hostfn_table_size(observers);
    for (int i = 0; i < n; i++)
        if (!strcmp(hostfn_table_name(observers, i), name)) (observers ? obs_used : ovr_used)[i] = 1;
}

// Order (MNG loader.c:216-224): observer (JUMP_SLOT only), override by name, own definition, other modules in load
// order, weak -> 0, strong -> named fatal stub. Overrides also replace a module's own symbols.
static Res resolve(Mod *m, uint32_t si, int jump) {
    Res *cache = jump ? &m->res_jump[si] : &m->res_plain[si];
    if (cache->kind != RES_UNSET) return *cache;
    const char *name = sym_name(m, si);
    uint64_t guest = sym_defined(m, si) ? m->pub.base + m->syms[si].value : find_symbol(name, NULL);
    GuestFn f;
    Res r;
    if (jump && (f = hostfn_observer(name))) {
        r = (Res){hostfn_register_observer(name, f, guest), RES_OBSERVER};
        mark_used(1, name);
    } else if ((f = hostfn_override(name))) {
        r = (Res){hostfn_register(name, f), RES_OVERRIDE};
        mark_used(0, name);
    } else if (guest) {
        r = (Res){guest, RES_GUEST};
    } else if ((m->syms[si].info >> 4) == STB_WEAK) {
        r = (Res){0, RES_WEAK};
    } else {
        r = (Res){hostfn_register(name, unresolved_stub), RES_STUB};
        stubs++;
        rt_log("%s: strong import %s is unresolved", m->pub.name, name);
    }
    *cache = r;
    return r;
}

static void relocate_table(Mod *m, const Elf64Rela *r, uint64_t n) {
    uint64_t base = m->pub.base;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t type = (uint32_t)r[i].info, si = (uint32_t)(r[i].info >> 32);
        if (r[i].off < m->pub.data_off || r[i].off > m->pub.size - 8)
            rt_fatal("loader: %s relocation %llu writes +%#llx, outside the data segment", m->pub.name,
                     (unsigned long long)i, (unsigned long long)r[i].off);
        uint64_t v;
        if (type == R_RELATIVE) {
            m->kinds[K_RELATIVE]++;
            v = base + (uint64_t)r[i].addend;
        } else {
            int k = type == R_ABS64 ? K_ABS64 : type == R_GLOB_DAT ? K_GLOB_DAT : type == R_JUMP_SLOT ? K_JUMP_SLOT : -1;
            if (k < 0) rt_fatal("loader: %s relocation %llu has type %u", m->pub.name, (unsigned long long)i, type);
            if (si == 0 || si >= m->nsyms) rt_fatal("loader: %s relocation %llu names symbol %u", m->pub.name,
                                                    (unsigned long long)i, si);
            m->kinds[k]++;
            if (sym_defined(m, si)) m->sym_own++;
            else m->sym_import++;
            Res s = resolve(m, si, type == R_JUMP_SLOT);
            v = s.kind == RES_WEAK ? 0 : s.value + (type == R_JUMP_SLOT ? 0 : (uint64_t)r[i].addend);
        }
        ST64(base + r[i].off, v);
    }
}

// DT_RELR: an even entry is an offset to relocate (the next bitmap starts after it); an odd entry is a bitmap of the
// 63 words that follow. Each relocated word holds its addend, as R_AARCH64_RELATIVE with an implicit addend.
static void relocate_relr(Mod *m) {
    uint64_t base = m->pub.base, where = 0;
    for (uint64_t i = 0; i < m->nrelr; i++) {
        uint64_t e = m->relr[i];
        for (int k = -1; k < 63; k++) {
            uint64_t off;
            if (k < 0) {
                if (e & 1) continue;
                off = e;
                where = e + 8;
            } else {
                if (!(e & 1) || !((e >> (k + 1)) & 1)) continue;
                off = where + 8ull * k;
            }
            if (off < m->pub.data_off || off > m->pub.size - 8)
                rt_fatal("loader: %s RELR entry %llu writes +%#llx, outside the data segment", m->pub.name,
                         (unsigned long long)i, (unsigned long long)off);
            ST64(base + off, base + LD64(base + off));
            m->kinds[K_RELATIVE]++;
            m->relr_count++;
        }
        if (e & 1) where += 63 * 8;
    }
}

// Import table statistics: every undefined named symbol, bound by guest definition (overrides aside).
static void count_imports(Mod *m) {
    for (uint32_t i = 1; i < m->nsyms; i++) {
        if (sym_defined(m, i) || !*sym_name(m, i)) continue;
        m->imports++;
        int which = -1;
        if (find_symbol(sym_name(m, i), &which)) m->import_to[which]++;
        else if ((m->syms[i].info >> 4) == STB_WEAK) m->weak_unresolved++;
        else m->strong_unresolved++;
    }
}

// Every non-JUMP_SLOT reference to nnMain holds the real address (MNG loader.c:229-239).
static void verify_identity(void) {
    uint64_t real = ldr_symbol("nnMain");
    if (!real) rt_fatal("loader: nnMain is not defined");
    for (int i = 0; i < nmods; i++) {
        Mod *m = &mods[i];
        for (int t = 0; t < 2; t++) {
            const Elf64Rela *r = t ? m->jmprel : m->rela;
            uint64_t n = t ? m->njmprel : m->nrela;
            for (uint64_t j = 0; j < n; j++) {
                uint32_t type = (uint32_t)r[j].info, si = (uint32_t)(r[j].info >> 32);
                if (type == R_RELATIVE || type == R_JUMP_SLOT || strcmp(sym_name(m, si), "nnMain")) continue;
                uint64_t actual = LD64(m->pub.base + r[j].off);
                if (actual != real)
                    rt_fatal("loader: %s +%#llx refers to nnMain as %#llx, not %#llx", m->pub.name,
                             (unsigned long long)r[j].off, (unsigned long long)actual, (unsigned long long)real);
                identity_refs++;
            }
        }
    }
    if (!identity_refs) rt_fatal("loader: no reference to nnMain to check");
}

// .text and .rodata read-only: guest code never executes from guest memory, and nothing may write them.
static void seal(Mod *m) {
    if (!plat_mem_protect(m->pub.base, m->pub.data_off, PLAT_PROT_READ))
        rt_fatal("loader: cannot seal %s", m->pub.name);
}

void ldr_load(const char *exefs_dir) {
    if (nmods) rt_fatal("ldr_load called twice");
    if (!mem_layout()->pool) rt_fatal("ldr_load before mem_init");
    if (rt_layout_count > MOD_COUNT) rt_fatal("loader: %d modules in the layout", rt_layout_count);
    for (int i = 0; i < rt_layout_count; i++) load_module(&mods[i], exefs_dir, &rt_layout[i]);
    nmods = rt_layout_count;
    ovr_used = calloc(hostfn_table_size(0) + 1, sizeof(int));
    obs_used = calloc(hostfn_table_size(1) + 1, sizeof(int));
    for (int i = 0; i < nmods; i++) {
        relocate_table(&mods[i], mods[i].rela, mods[i].nrela);
        relocate_table(&mods[i], mods[i].jmprel, mods[i].njmprel);
        relocate_relr(&mods[i]);
        count_imports(&mods[i]);
        mods[i].pub.ctor_flag = decode_ctor_flag(&mods[i]);
    }
    verify_identity();
    for (int i = 0; i < nmods; i++) seal(&mods[i]);
    for (int i = 0; i < nmods; i++) {
        const Mod *m = &mods[i];
        rt_log("loaded %-8s at %#llx size %#llx: %llu relocations, %u imports", m->pub.name,
               (unsigned long long)m->pub.base, (unsigned long long)m->pub.size,
               (unsigned long long)(m->nrela + m->njmprel + m->relr_count), m->imports);
    }
}

// ---- lookups
static int module_of(uint64_t a) {
    for (int i = 0; i < nmods; i++)
        if (a - mods[i].pub.base < mods[i].pub.size) return i;
    return -1;
}

const char *ldr_describe(uint64_t a, char *buf, int n) {
    int i = module_of(a);
    if (i < 0) {
        const char *h = hostfn_name(a);
        if (h) snprintf(buf, n, "host %s", h);
        else snprintf(buf, n, "%#llx", (unsigned long long)a);
        return buf;
    }
    const Mod *m = &mods[i];
    uint64_t off = a - m->pub.base;
    uint32_t lo = 0, hi = m->nfuncs;   // last function with value <= off
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (m->syms[m->funcs[mid]].value <= off) lo = mid + 1;
        else hi = mid;
    }
    if (lo) {
        const Elf64Sym *s = &m->syms[m->funcs[lo - 1]];
        if (off - s->value < (s->size ? s->size : 0x100000)) {
            snprintf(buf, n, "%s+%#llx (%s+%#llx)", m->pub.name, (unsigned long long)off, m->strtab + s->name,
                     (unsigned long long)(off - s->value));
            return buf;
        }
    }
    snprintf(buf, n, "%s+%#llx", m->pub.name, (unsigned long long)off);
    return buf;
}

static int by_slot(const void *a, const void *b) {
    uint64_t x = ((const uint64_t *)a)[0], y = ((const uint64_t *)b)[0];
    return x < y ? -1 : x > y;
}

// PLT stubs: adrp x16, page; ldr x17, [x16, #slot]; add x16, x16, #slot; br x17.
static void find_plt(Mod *m) {
    uint64_t text = m->pub.base + m->seg_off[0], end = text + m->seg_size[0];
    uint64_t cap = 1024, n = 0, *pairs = malloc(cap * 16);
    for (uint64_t a = text; a + 16 <= end && pairs; a += 4) {
        uint32_t w0 = rd32(a), w1 = rd32(a + 4), w3 = rd32(a + 12);
        if ((w0 & 0x9f00001fu) != 0x90000010u || (w1 & 0xffc003ffu) != 0xf9400211u || w3 != 0xd61f0220u) continue;
        int64_t imm = (int64_t)(((w0 >> 29) & 3) | (((w0 >> 5) & 0x7ffff) << 2));
        imm = (imm ^ (1 << 20)) - (1 << 20);
        uint64_t slot = (a & ~0xFFFull) + (uint64_t)(imm << 12) + ((w1 >> 10) & 0xfff) * 8;
        if (n == cap) pairs = realloc(pairs, (cap *= 2) * 16);
        if (!pairs) break;
        pairs[2 * n] = slot;
        pairs[2 * n + 1] = a;
        n++;
    }
    if (!pairs) rt_fatal("loader: PLT table allocation");
    qsort(pairs, n, 16, by_slot);
    m->plt_slot = pairs;
    m->nplt = n;
}

uint64_t ldr_plt_stub(const char *module, const char *name) {
    Mod *m = NULL;
    for (int i = 0; i < nmods; i++)
        if (!strcmp(mods[i].pub.name, module)) m = &mods[i];
    if (!m) return 0;
    plat_mutex_lock(&plt_lock);
    if (!m->plt_slot) find_plt(m);
    plat_mutex_unlock(&plt_lock);
    for (uint64_t j = 0; j < m->njmprel; j++) {
        uint32_t si = (uint32_t)(m->jmprel[j].info >> 32);
        if ((uint32_t)m->jmprel[j].info != R_JUMP_SLOT || strcmp(sym_name(m, si), name)) continue;
        uint64_t slot = m->pub.base + m->jmprel[j].off;
        uint64_t lo = 0, hi = m->nplt;
        while (lo < hi) {
            uint64_t mid = (lo + hi) / 2;
            if (m->plt_slot[2 * mid] < slot) lo = mid + 1;
            else hi = mid;
        }
        if (lo < m->nplt && m->plt_slot[2 * lo] == slot) return m->plt_slot[2 * lo + 1];
    }
    return 0;
}

// ---- --probe-loader
// Expected values, measured on the 1.1.1, 1.2.1 and 1.4.3 modules; the 1.4.3 relocation totals match the ones
// MacNativeGames' port notes give for that version. 1.1.1 shares subsdk0 and sdk with 1.2.1 down to every count.
typedef struct {
    const char *name;
    uint64_t kinds[K_COUNT], total, sym_import, sym_own;
    uint32_t imports, to[MOD_COUNT], weak, init_off, init_array;
} Expect;
static const Expect expect_121[] = {
    {"main", {508816, 1415, 32, 641}, 510904, 2088, 0, 675, {0, 16, 647}, 12, 0x30, 10673},
    {"subsdk0", {1623, 12820, 1030, 3924}, 19397, 6576, 11198, 667, {9, 0, 654}, 4, 0x30, 45},
    {"sdk", {10917, 13976, 1207, 8932}, 35032, 16, 24099, 13, {8, 0, 0}, 5, 0x30, 116},
};
static const Expect expect_111[] = {
    {"main", {508604, 1412, 32, 639}, 510687, 2083, 0, 673, {0, 16, 645}, 12, 0x30, 10668},
    {"subsdk0", {1623, 12820, 1030, 3924}, 19397, 6576, 11198, 667, {9, 0, 654}, 4, 0x30, 45},
    {"sdk", {10917, 13976, 1207, 8932}, 35032, 16, 24099, 13, {8, 0, 0}, 5, 0x30, 116},
};
static const Expect expect_143[] = {
    {"main", {499078, 1393, 28, 650}, 501149, 2071, 0, 680, {0, 16, 656}, 8, 0x10b4f10, 10717},
    {"subsdk0", {3424, 10927, 905, 3817}, 19073, 6557, 9092, 663, {9, 0, 654}, 0, 0x20, 45},
    {"sdk", {15013, 10135, 1079, 7832}, 34059, 10, 19036, 8, {8, 0, 0}, 0, 0x400, 83},
};
#define expect (RT_V143 ? expect_143 : RT_V111 ? expect_111 : expect_121)

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

#define PROBE_SYMBOL "_ZN2nn2os13GetSystemTickEv"
#define PROBE_MAGIC 0x5eed0123456789abull
static volatile int32_t probe_hits;

static void probe_override(Cpu *c) {
    plat_inc32(&probe_hits);
    c->x[0] = PROBE_MAGIC;
}

// .text and .rodata in guest memory equal the NSO: relocations never touched them.
static void check_segments(const char *dir) {
    for (int i = 0; i < nmods; i++) {
        const Mod *m = &mods[i];
        size_t len;
        uint8_t *raw = ldr_read_verified(dir, m->pub.name, &len);
        Seg seg[3];
        nso_segments(raw, len, m->pub.name, seg);
        int same = 1;
        for (int s = 0; s < 2; s++) {
            uint8_t *buf = malloc(seg[s].size ? seg[s].size : 1);
            if (!buf) rt_fatal("probe: allocation");
            nso_segment(raw, &seg[s], buf, m->pub.name, s);
            if (memcmp(buf, (const void *)(uintptr_t)(m->pub.base + seg[s].mem_off), seg[s].size)) same = 0;
            free(buf);
        }
        free(raw);
        uint64_t ro_end;
        int ro = plat_mem_state(m->pub.base, m->pub.base + m->pub.data_off, &ro_end) == PLAT_PROT_READ &&
                 ro_end == m->pub.base + m->pub.data_off;
        printf("  %-8s text %#x+%#x rodata %#x+%#x: bytes %s the NSO, %s\n", m->pub.name, seg[0].mem_off, seg[0].size,
               seg[1].mem_off, seg[1].size, same ? "equal" : "DIFFER FROM", ro ? "read-only" : "NOT READ-ONLY");
        CHECK(same, "%s .text/.rodata changed after relocation", m->pub.name);
        CHECK(ro, "%s .text/.rodata are not sealed read-only", m->pub.name);
    }
}

static void check_got(void) {
    const Mod *m = &mods[0];
    uint64_t want = 0, slot = 0, nslots = 0;
    for (uint64_t j = 0; j < m->njmprel; j++) {
        uint32_t si = (uint32_t)(m->jmprel[j].info >> 32);
        if ((uint32_t)m->jmprel[j].info == R_JUMP_SLOT && !strcmp(sym_name(m, si), PROBE_SYMBOL)) {
            slot = m->pub.base + m->jmprel[j].off;
            nslots++;
        }
    }
    for (int i = 0; i < 4096 && !want; i++) {
        const char *n = hostfn_name(RT_HOSTFN_BASE + 16ull * i);
        if (!n) break;
        if (!strcmp(n, PROBE_SYMBOL)) want = RT_HOSTFN_BASE + 16ull * i;
    }
    uint64_t got = slot ? LD64(slot) : 0;
    char d[128];
    printf("probe override %s: main GOT slot %s holds %#llx (%s)\n", PROBE_SYMBOL, ldr_describe(slot, d, sizeof d),
           (unsigned long long)got, hostfn_name(got) ? "host function" : "not a host function");
    CHECK(nslots == 1 && want && got == want, "the probe override did not reach main's GOT slot");

    uint64_t stub = ldr_plt_stub("main", PROBE_SYMBOL);
    printf("probe override: main PLT stub %s\n", ldr_describe(stub, d, sizeof d));
    CHECK(stub, "no PLT stub in main for %s", PROBE_SYMBOL);

    hostfn_seal();
    Cpu *c = plat_aligned_alloc(sizeof(Cpu), 64);
    memset(c, 0, sizeof *c);
    uint8_t *stack = mem_kernel_alloc(0x10000, 16);
    c->sp = (uint64_t)(uintptr_t)stack + 0x10000;
    c->excl_addr = ~0ull;
    c->x[19] = 0x1919;
    uint64_t r1 = rt_call(c, got, 0);
    long h1 = probe_hits;
    uint64_t r2 = stub ? rt_call(c, stub, 0) : 0;
    long h2 = probe_hits;
    printf("probe override: dispatch through the GOT value -> %#llx (%ld calls), through the lifted PLT stub -> %#llx "
           "(%ld calls)\n", (unsigned long long)r1, h1, (unsigned long long)r2, h2);
    CHECK(r1 == PROBE_MAGIC && h1 == 1, "dispatching the GOT value did not reach the host function");
    CHECK(r2 == PROBE_MAGIC && h2 == 2, "the lifted PLT stub did not reach the host function");
    CHECK(c->x[30] == 0 && c->pc == 0 && c->x[19] == 0x1919, "rt_call did not restore x30/pc or lost x19");
    mem_kernel_free(stack, 0x10000);
    plat_aligned_free(c);
}

int probe_loader(void) {
    const RtConfig *cfg = rt_config();
    hostfn_add_override(PROBE_SYMBOL, probe_override);
    ldr_load(cfg->exefs);
    if (nmods != (int)(sizeof expect_121 / sizeof *expect_121)) {
        printf("probe-loader: FAILED (%d modules, expected 3)\n", nmods);
        return 1;
    }
    printf("modules (exefs %s):\n", cfg->exefs);
    for (int i = 0; i < nmods; i++) {
        const Mod *m = &mods[i];
        char to[128] = "";
        for (int k = 0; k < nmods; k++)
            if (m->import_to[k])
                snprintf(to + strlen(to), sizeof to - strlen(to), "%s%s %u", *to ? ", " : "", mods[k].pub.name,
                         m->import_to[k]);
        printf("  %-8s base %#llx image %#llx DT_INIT +%#x init_array %llu (%llu B) imports %u: %s; weak unresolved "
               "%u\n", m->pub.name, (unsigned long long)m->pub.base, (unsigned long long)m->pub.size, m->init_off,
               (unsigned long long)(m->pub.init_array_size / 8), (unsigned long long)m->pub.init_array_size,
               m->imports, to, m->weak_unresolved);
        CHECK(!strcmp(m->pub.name, expect[i].name), "module %d is %s", i, m->pub.name);
        CHECK(m->imports == expect[i].imports && m->weak_unresolved == expect[i].weak &&
              !memcmp(m->import_to, expect[i].to, sizeof m->import_to), "%s import table differs from section 0.1",
              m->pub.name);
        CHECK(m->init_off == expect[i].init_off && m->pub.init_array_size / 8 == expect[i].init_array,
              "%s DT_INIT/init_array differ from section 0.1", m->pub.name);
    }
    printf("relocations:\n");
    for (int i = 0; i < nmods; i++) {
        const Mod *m = &mods[i];
        uint64_t total = m->kinds[0] + m->kinds[1] + m->kinds[2] + m->kinds[3];
        printf("  %-8s RELATIVE %llu ABS64 %llu GLOB_DAT %llu JUMP_SLOT %llu total %llu; symbolic against imports %llu, "
               "against own symbols %llu\n", m->pub.name, (unsigned long long)m->kinds[K_RELATIVE],
               (unsigned long long)m->kinds[K_ABS64], (unsigned long long)m->kinds[K_GLOB_DAT],
               (unsigned long long)m->kinds[K_JUMP_SLOT], (unsigned long long)total,
               (unsigned long long)m->sym_import, (unsigned long long)m->sym_own);
        CHECK(!memcmp(m->kinds, expect[i].kinds, sizeof m->kinds) && total == expect[i].total &&
              m->sym_import == expect[i].sym_import && m->sym_own == expect[i].sym_own,
              "%s relocations differ from section 0.2", m->pub.name);
    }
    printf("constructors:\n");
    for (int i = 0; i < nmods; i++) {
        const RtLoaded *m = &mods[i].pub;
        char d[128];
        printf("  %-8s DT_INIT %#llx init_array %#llx (%llu entries) ctor_flag %s\n", m->name,
               (unsigned long long)m->init, (unsigned long long)m->init_array,
               (unsigned long long)(m->init_array_size / 8), m->ctor_flag ? ldr_describe(m->ctor_flag, d, sizeof d)
                                                                         : "NOT DECODED");
        CHECK(m->init && m->init - m->base < m->text_end, "%s DT_INIT outside .text", m->name);
        CHECK(m->init_array - m->base >= mods[i].seg_off[1] && m->init_array + m->init_array_size <= m->base + m->size,
              "%s init_array outside rodata/data", m->name);
        CHECK(m->ctor_flag, "%s constructor flag not decoded", m->name);
    }
    check_segments(cfg->exefs);

    uint32_t strong = 0;
    for (int i = 0; i < nmods; i++) strong += mods[i].strong_unresolved;
    printf("strong unresolved %u\n", strong);
    CHECK(strong == 0 && stubs == 0, "strong imports are unresolved");
    int linked = hostfn_table_size(0), installed = 0, olinked = hostfn_table_size(1), oinstalled = 0;
    for (int i = 0; i < linked; i++) {
        installed += ovr_used[i];
        if (!ovr_used[i]) printf("  override %s matches no relocation\n", hostfn_table_name(0, i));
    }
    for (int i = 0; i < olinked; i++) {
        oinstalled += obs_used[i];
        if (!obs_used[i]) printf("  observer %s matches no JUMP_SLOT\n", hostfn_table_name(1, i));
    }
    printf("overrides installed %d (%d linked, + 1 probe override)\n", installed, linked);
    printf("observers installed %d (%d linked)\n", oinstalled, olinked);
    printf("nnMain identity ok (%llu refs)\n", (unsigned long long)identity_refs);
    check_got();
    printf("probe-loader: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
