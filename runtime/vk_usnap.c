// Uniform blocks as of the submit call. NVN starts the GPU when nvnQueueSubmitCommands returns and the GPU reads the
// constants soon after; here the replay records for several milliseconds, and the GPU reads a pool's bytes when it
// runs the part that uses them (the VRAM mirror's sync copies run then, vk_memory.c "VRAM mirror"). Meanwhile the game,
// having handed the frame over, rewrites per-object uniform blocks on other threads with no fence: late draws read
// another object's matrix (phantom rocks, a flat far field; TotK 1.2.1's producer f_70008c6460, 14-24 ms after the
// submit). So at the start of a submission's recording every uniform range it binds is copied into an arena of its own,
// and the uniform bindings of the replay use the copy.
// - Only ranges of imported pools (memory the CPU writes) are copied, aligned out to 256 bytes (the largest
//   minUniformBufferOffsetAlignment) and at most 64 KiB from the bound address, overlapping ones merged.
// - A range the GPU wrote in a submission that has not completed is left out: its bytes reach guest memory only with
//   that submission's write-back, the mirror holds them until then.
// - A GPU write within the submission (UpdateUniformBuffer, a storage buffer a shader writes, a buffer copy or fill,
//   an image-to-buffer copy: vk_usnap_drop) drops the copies it touches from then on, so later draws read the result.
// - The arena goes back to a free list once the submission's fence is done (completion thread).
// The design follows MacNativeGames' TOTK_UBO_SNAPSHOT (games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m usnap_take,
// usnap_drop; MIT, Copyright (c) 2026 jhaizhou); the code is ours.
// - An UpdateUniformBuffer within a snapshot region (vk_usnap_update) gets an overlay: a fresh copy of its block in
//   the arena with the new bytes, which the bindings recorded after it use (vk_usnap_find looks there first), while
//   the earlier ones keep the region's copy. The replay then copies the bytes into the buffer itself at the pass end
//   instead of at once (vk_exec_draw.c op_update_ub): the terrain updates one block before each of about 500 draws a
//   frame, and each copy in place meant ending the pass and a full barrier.
// The copy is taken where vk_exec_record starts: at the submit call when the replay runs on the submitting thread, later
// with TOTK_REPLAY_ASYNC. TOTK_UBO_SNAPSHOT=0 binds live memory as before; =ab alternates (TOTK_AB).
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "platform/platform.h"

#define ALIGN 256u
#define MAX_RANGE (64u << 10)
#define ARENA_MIN (4u << 20)
#define OVL_ROOM (4u << 20)   // arena bytes kept for overlays
#define OVL_N 256             // overlays at a time: the terrain's updates all go to one block, replaced in place
#define CALL_DEPTH 16

typedef struct Arena {
    struct Arena *next;
    VkBuffer buf;
    VmaAllocation alloc;
    uint8_t *map;
    uint8_t *host;     // the same bytes in ordinary memory: map is write-combined device memory (the BAR, an APU's
                       // carve-out), which the CPU reads at a few tens of MB/s, and an overlay copies a block's bytes
    uint64_t size;
} Arena;

typedef struct { uint64_t lo, hi; } Rng;
typedef struct { uint64_t lo, hi, off; int dropped; } Reg;
typedef struct { uint64_t lo, hi, fence; } Wrote;
typedef struct { uint64_t lo, hi, off; } Ovl;

static struct {
    int mode;                   // rt_knob: 0 off, 1 on, 2 ab
    int on;                     // this submission has a snapshot
    uint64_t fence, gen;
    Arena *arena;
    uint64_t used;              // arena bytes taken: the regions, then the overlays
    Rng *col;                   // collected ranges
    int ncol, ccol;
    Reg *reg;                   // merged, sorted by lo, disjoint
    int nreg, creg;
    Ovl ovl[OVL_N];             // blocks updated in this submission, newest last; the newest copy of each
    int novl;
    uint64_t ovl_lo, ovl_hi;    // the span of every overlay made in this submission (vk_usnap_overlay_at)
    Wrote *wr;                  // GPU writes of submissions not known to be complete
    int nwr, cwr;
    // since the last summary
    uint64_t subs, regions, bytes, copy_ns, hits, drops, inflight, refused, off_subs, overlays, ovl_hits, ovl_refused;
} U;

static Arena *free_arenas;
static PlatMutex free_lock = PLAT_MUTEX_INIT;

static void *grow(void *p, int *cap, size_t elem) {
    int n = *cap ? *cap * 2 : 1024;
    void *q = realloc(p, (size_t)n * elem);
    if (!q) rt_fatal("vk_usnap: out of memory");
    *cap = n;
    return q;
}

static Arena *arena_get(uint64_t need) {
    plat_mutex_lock(&free_lock);
    Arena **pp = &free_arenas, *a = NULL;
    for (; *pp; pp = &(*pp)->next)
        if ((*pp)->size >= need) {
            a = *pp, *pp = a->next;
            break;
        }
    plat_mutex_unlock(&free_lock);
    if (a) return a;
    a = calloc(1, sizeof *a);
    if (!a) rt_fatal("vk_usnap: out of memory");
    a->size = need < ARENA_MIN ? ARENA_MIN : (need + (1u << 20) - 1) & ~(uint64_t)((1u << 20) - 1);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = a->size,
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    // Device-local through the BAR when the device has it (the GPU reads constants on every draw), else host memory.
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                           VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VkResult res = vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &a->buf, &a->alloc, &info);
    if (res != VK_SUCCESS) {
        rt_log("vk_usnap: arena of %llu bytes failed (%d): this submission binds live memory",
               (unsigned long long)a->size, (int)res);
        free(a);
        return NULL;
    }
    a->map = info.pMappedData;
    if (!(a->host = malloc(a->size))) rt_fatal("vk_usnap: out of memory");
    rt_log("vk_usnap: new arena of %.1f MiB", a->size / 1048576.0);
    return a;
}

static void arena_done(void *arg) {   // completion thread: the GPU has read it
    Arena *a = arg;
    plat_mutex_lock(&free_lock);
    a->next = free_arenas, free_arenas = a;
    plat_mutex_unlock(&free_lock);
}

static void collect(const op_hdr *h, int depth) {
    for (;;) {
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_)
            rt_fatal("vk_usnap: corrupt command record at %p: op %u size %u", (const void *)h, h->op, h->size);
        const void *a = nvn_op_payload(h);
        if (h->op == OP_END) return;
        if (h->op == OP_JUMP) {
            h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target;
            continue;
        }
        if (h->op == OP_CALL && depth < CALL_DEPTH) collect(nvn_op_first(((const p_jump *)a)->target), depth + 1);
        if (h->op == OP_BIND_UBO) {
            const p_bind_buf *b = a;
            if (b->addr && b->size) {
                if (U.ncol == U.ccol) U.col = grow(U.col, &U.ccol, sizeof *U.col);
                uint64_t n = b->size < MAX_RANGE ? b->size : MAX_RANGE;
                U.col[U.ncol++] = (Rng){b->addr & ~(uint64_t)(ALIGN - 1), b->addr + n};
            }
        }
        h = nvn_op_next(h);
    }
}

static int by_lo(const void *a, const void *b) {
    uint64_t x = ((const Rng *)a)->lo, y = ((const Rng *)b)->lo;
    return x < y ? -1 : x > y;
}
static int wr_by_lo(const void *a, const void *b) {
    uint64_t x = ((const Wrote *)a)->lo, y = ((const Wrote *)b)->lo;
    return x < y ? -1 : x > y;
}

// Sorts the collected ranges by lo. A town frame binds about 10,000 uniform ranges and qsort took half a millisecond
// of the copy's 0.96 ms on the game thread, so they go through an LSD radix sort on (lo - lowest) / ALIGN, three
// passes of 11 bits; qsort is left for a span that does not fit in 33 bits (2 TiB).
#define RADIX_BITS 11
static void sort_col(void) {
    static Rng *tmp;
    static int ctmp;
    int n = U.ncol;
    if (n < 2) return;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < n; i++) {
        if (U.col[i].lo < lo) lo = U.col[i].lo;
        if (U.col[i].lo > hi) hi = U.col[i].lo;
    }
    if ((hi - lo) / ALIGN >> (3 * RADIX_BITS)) {
        qsort(U.col, (size_t)n, sizeof *U.col, by_lo);
        return;
    }
    if (ctmp < n) {
        free(tmp);
        ctmp = U.ccol;
        if (!(tmp = malloc((size_t)ctmp * sizeof *tmp))) rt_fatal("vk_usnap: out of memory");
    }
    Rng *src = U.col, *dst = tmp;
    for (int pass = 0; pass < 3; pass++) {
        int shift = pass * RADIX_BITS, count[1 << RADIX_BITS] = {0};
        for (int i = 0; i < n; i++) count[(src[i].lo - lo) / ALIGN >> shift & ((1 << RADIX_BITS) - 1)]++;
        for (int k = 0, sum = 0; k < 1 << RADIX_BITS; k++) {
            int c = count[k];
            count[k] = sum, sum += c;
        }
        for (int i = 0; i < n; i++) dst[count[(src[i].lo - lo) / ALIGN >> shift & ((1 << RADIX_BITS) - 1)]++] = src[i];
        Rng *t = src;
        src = dst, dst = t;
    }
    if (src != U.col) memcpy(U.col, src, (size_t)n * sizeof *src);   // three passes end in tmp
}

// Keeps the GPU writes of submissions that are not done, merged into disjoint ranges sorted by lo (the terrain rewrites
// one block before every draw: hundreds of equal entries), each with the newest fence among its parts.
static void wr_prune(void) {
    uint64_t done = vk_sync_fence_done();
    int n = 0;
    for (int i = 0; i < U.nwr; i++)
        if (U.wr[i].fence > done) U.wr[n++] = U.wr[i];
    qsort(U.wr, (size_t)n, sizeof *U.wr, wr_by_lo);
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (m && U.wr[i].lo <= U.wr[m - 1].hi) {
            if (U.wr[i].hi > U.wr[m - 1].hi) U.wr[m - 1].hi = U.wr[i].hi;
            if (U.wr[i].fence > U.wr[m - 1].fence) U.wr[m - 1].fence = U.wr[i].fence;
        } else {
            U.wr[m++] = U.wr[i];
        }
    }
    U.nwr = m;
}
static int wr_hits(uint64_t lo, uint64_t hi) {   // wr_prune ran: disjoint, sorted
    int a = 0, b = U.nwr;
    while (a < b) {
        int k = (a + b) / 2;
        if (U.wr[k].hi <= lo) a = k + 1;
        else b = k;
    }
    return a < U.nwr && U.wr[a].lo < hi;
}

static void summary(void) {
    if (U.subs + U.off_subs < 3600) return;
    double s = U.subs ? (double)U.subs : 1.0;
    rt_log("vk_usnap: %llu submissions with a snapshot, %llu without; per submission %.0f ranges, %.1f KiB copied in "
           "%.3f ms, %.0f uniform bindings from the copy, %.1f dropped by GPU writes, %.1f left out (GPU writes in "
           "flight), %.1f refused (not an imported pool); %.1f update overlays, %.0f bindings from them, %.1f updates "
           "refused",
           (unsigned long long)U.subs, (unsigned long long)U.off_subs, U.regions / s, U.bytes / s / 1024.0,
           U.copy_ns / s / 1e6, U.hits / s, U.drops / s, U.inflight / s, U.refused / s, U.overlays / s, U.ovl_hits / s,
           U.ovl_refused / s);
    U.subs = U.off_subs = U.regions = U.bytes = U.copy_ns = U.hits = U.drops = U.inflight = U.refused = 0;
    U.overlays = U.ovl_hits = U.ovl_refused = 0;
}

void vk_usnap_begin(const uint64_t *handles, int n, uint64_t fence) {
    if (!U.mode) U.mode = rt_knob("UBO_SNAPSHOT", "1") + 1;   // stored +1: 0 is "not read yet"
    U.on = 0, U.fence = fence, U.gen++;
    U.novl = 0, U.ovl_lo = UINT64_MAX, U.ovl_hi = 0;
    summary();
    if (!RT_KNOB_ON(U.mode - 1)) {
        U.off_subs++;
        return;
    }
    uint64_t t0 = kern_now_ns();
    U.ncol = 0;
    for (int i = 0; i < n; i++)
        if (handles[i]) collect(nvn_op_first(handles[i]), 0);
    sort_col();
    wr_prune();
    // merge, keeping only what lies wholly in one imported pool and no GPU write in flight touches
    U.nreg = 0;
    uint64_t total = 0;
    for (int i = 0; i < U.ncol;) {
        uint64_t lo = U.col[i].lo, hi = U.col[i].hi;
        for (i++; i < U.ncol && U.col[i].lo <= hi; i++)
            if (U.col[i].hi > hi) hi = U.col[i].hi;
        VkMemSpan sp;
        if (!vk_mem_resolve(lo, hi - lo, &sp) || sp.alloc || !sp.pool || sp.avail < hi - lo ||
            (sp.cls != VK_MCLS_HOST_CACHED && sp.cls != VK_MCLS_HOST_UNC)) {
            U.refused++;
            continue;
        }
        if (wr_hits(lo, hi)) {
            U.inflight++;
            continue;
        }
        if (U.nreg == U.creg) U.reg = grow(U.reg, &U.creg, sizeof *U.reg);
        U.reg[U.nreg++] = (Reg){lo, hi, total, 0};
        total += (hi - lo + ALIGN - 1) & ~(uint64_t)(ALIGN - 1);
    }
    if (!U.nreg) return;
    if (!(U.arena = arena_get(total + OVL_ROOM))) return;
    U.used = total;
    for (int i = 0; i < U.nreg; i++) {
        memcpy(U.arena->host + U.reg[i].off, (const void *)(uintptr_t)U.reg[i].lo, U.reg[i].hi - U.reg[i].lo);
        memcpy(U.arena->map + U.reg[i].off, U.arena->host + U.reg[i].off, U.reg[i].hi - U.reg[i].lo);
    }
    vmaFlushAllocation(vk_ctx()->vma, U.arena->alloc, 0, total);   // a no-op on coherent memory
    U.on = 1;
    U.subs++, U.regions += (uint64_t)U.nreg, U.bytes += total, U.copy_ns += kern_now_ns() - t0;
}

void vk_usnap_end(uint64_t fence) {
    if (U.arena) vk_completion_add(fence, arena_done, U.arena), U.arena = NULL;
    U.on = 0, U.novl = 0, U.gen++;
}

// First region whose end is above a.
static int first_above(uint64_t a) {
    int lo = 0, hi = U.nreg;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (U.reg[m].hi <= a) lo = m + 1;
        else hi = m;
    }
    return lo;
}

int vk_usnap_find(uint64_t addr, uint64_t n, uint64_t align, VkDescriptorBufferInfo *out) {
    if (!U.on || !n) return 0;
    if (U.novl && addr >= U.ovl_lo && addr < U.ovl_hi)
        for (int k = U.novl; k-- > 0;) {   // an updated block first: the newest copy of its bytes
            const Ovl *o = &U.ovl[k];
            if (addr < o->lo || addr + n > o->hi) continue;
            uint64_t off = o->off + (addr - o->lo);
            if (off % align) return 0;
            *out = (VkDescriptorBufferInfo){U.arena->buf, off, n};
            U.hits++, U.ovl_hits++;
            return 1;
        }
    int i = first_above(addr);
    if (i >= U.nreg) return 0;
    const Reg *r = &U.reg[i];
    if (r->dropped || addr < r->lo || addr + n > r->hi) return 0;
    uint64_t off = r->off + (addr - r->lo);
    if (off % align) return 0;
    *out = (VkDescriptorBufferInfo){U.arena->buf, off, n};
    U.hits++;
    return 1;
}

// The GPU writes [addr, addr+n) in this submission: later snapshots leave the range out until the write is complete.
static void wr_note(uint64_t addr, uint64_t n) {
    if (U.nwr == U.cwr) U.wr = grow(U.wr, &U.cwr, sizeof *U.wr);
    U.wr[U.nwr++] = (Wrote){addr, addr + n, U.fence};
}

void vk_usnap_drop(uint64_t addr, uint64_t n) {
    if (!U.mode || !n) return;
    wr_note(addr, n);
    if (!U.on) return;
    for (int i = first_above(addr); i < U.nreg && U.reg[i].lo < addr + n; i++)
        if (!U.reg[i].dropped) U.reg[i].dropped = 1, U.drops++, U.gen++;
    for (int k = 0; k < U.novl;)   // overlays of the range go too: the bindings after this read the buffer itself
        if (U.ovl[k].lo < addr + n && addr < U.ovl[k].hi) U.ovl[k] = U.ovl[--U.novl], U.gen++;
        else k++;
}

int vk_usnap_update(uint64_t block, uint64_t block_n, uint64_t addr, uint64_t n, const void *data) {
    if (!U.on || !n || !block_n || addr < block || addr + n > block + block_n) return 0;
    int i = first_above(addr);
    if (i >= U.nreg || U.reg[i].dropped || addr < U.reg[i].lo || addr + n > U.reg[i].hi) return 0;
    const Reg *r = &U.reg[i];
    // The overlay covers the block as far as the region does, at most MAX_RANGE from its start, as a binding would
    uint64_t lo = block & ~(uint64_t)(ALIGN - 1), hi = block + block_n;
    if (lo < r->lo) lo = r->lo;
    if (hi > r->hi) hi = r->hi;
    if (hi > lo + MAX_RANGE) hi = lo + MAX_RANGE;
    if (addr < lo || addr + n > hi) return U.ovl_refused++, 0;
    // The bytes before the update: the block's own overlay when it has one, else the region's copy. An overlay of
    // another block over these bytes would leave two copies to choose from: the caller copies on the GPU instead.
    const uint8_t *src = U.arena->host + r->off + (lo - r->lo);   // host: never read the device memory back
    int k = 0;
    for (; k < U.novl; k++) {
        if (U.ovl[k].lo == lo && U.ovl[k].hi == hi) {
            src = U.arena->host + U.ovl[k].off;
            break;
        }
        if (U.ovl[k].lo < hi && lo < U.ovl[k].hi) return U.ovl_refused++, 0;
    }
    uint64_t size = (hi - lo + ALIGN - 1) & ~(uint64_t)(ALIGN - 1);
    if (k == U.novl && U.novl == OVL_N) return U.ovl_refused++, 0;
    if (U.used + size > U.arena->size) return U.ovl_refused++, 0;
    uint8_t *dst = U.arena->host + U.used;
    memcpy(dst, src, hi - lo);
    memcpy(dst + (addr - lo), data, n);
    memcpy(U.arena->map + U.used, dst, hi - lo);
    U.ovl[k] = (Ovl){lo, hi, U.used};
    if (k == U.novl) U.novl++;
    if (lo < U.ovl_lo) U.ovl_lo = lo;
    if (hi > U.ovl_hi) U.ovl_hi = hi;
    U.used += size;
    U.overlays++;   // not U.gen: only the draws binding this block resolve differently (vk_usnap_overlay_at)
    wr_note(addr, n);   // the deferred GPU copy writes the buffer in this submission all the same
    return 1;
}

uint64_t vk_usnap_overlay_at(uint64_t addr) {
    if (!U.novl || addr < U.ovl_lo || addr >= U.ovl_hi) return 0;
    for (int k = U.novl; k-- > 0;)
        if (addr >= U.ovl[k].lo && addr < U.ovl[k].hi) return U.ovl[k].off + 1;
    return 0;
}

uint64_t vk_usnap_gen(void) { return U.gen; }

// The arenas back on the free list (vk_exec_draw_shutdown: the device is idle, every completion ran).
void vk_usnap_shutdown(void) {
    plat_mutex_lock(&free_lock);
    for (Arena *a = free_arenas, *next; a; a = next)
        next = a->next, vmaDestroyBuffer(vk_ctx()->vma, a->buf, a->alloc), free(a->host), free(a);
    free_arenas = NULL;
    plat_mutex_unlock(&free_lock);
}
