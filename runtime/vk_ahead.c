// TOTK_SYNC_AHEAD=check: what answering the game's frame wait one frame late without shadow storage would race, with
// the wait left exact. In that form (Savage Wind's overlap) the game is told frame N is done when only N-1 is, and the
// GPU runs N while the game builds N+1, so every byte of guest memory N's GPU work reads after its recording (or writes
// back into guest memory) must not change between the wait for N and the next frame wait, which then waits for N; and
// every GPU result the game reads back after the wait for N must not come from N.
// - Read set: the replay notes each GPU-timeline access of guest memory with its submission's value (vk_ahead_read):
//   in-place bindings of imported pools without a VRAM mirror, the mirror's sync copies (the pages and flush ranges
//   copied in the prologue), GPU deswizzle sources and the mirror write-backs. Uniform snapshots and CPU texture uploads
//   copy at record time, before the wait: not noted.
// - At the frame wait for value A whose one-frame-late form would have waited for L < A, the notes of values in (L, A]
//   are N's read set: merged per class and over all classes, and gap-joined into at most IV_MAX write-watch queries. The
//   queries stamp every write made so far into the shared page generations (vk_memory.c vk_mem_ahead_observe); G is the
//   newest generation then. At the next frame-wait entry the same queries run again, and every page of the set stamped
//   after G is a CPU write after the wait that the late form would have raced: counted in bytes by class, and the first
//   offenders logged by pool. A range in memory without a write watch is hashed by page instead.
// - Readbacks: GPU writes into CPU-visible memory and reports are listed with their value (vk_ahead_gpu_write). An
//   invalidate of a range a GPU write of a value in (L, A] of the last frame wait lands in would have read stale data;
//   so would the fence write-back of a small target last rendered by such a value (vk_ahead_small).
// Bounded: fixed tables, a note list that is compacted when full, one frame set at a time. One log line per second of
// host time and a summary at exit, prefixed "sync-ahead check:".
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_ahead.h"
#include "vk_exec.h"
#include "platform/platform.h"

int vk_ahead_on;

static const char *const cls_name[AH_N + 1] = {"vb", "ib", "ubo", "ssbo", "ind", "copy", "msync", "deswz", "wb", "wbm",
                                               "bound", "all"};

#define NOTE_MAX 65536    // notes not yet taken by a frame wait
#define SET_MAX 8192      // merged ranges of one frame set, per class and the union together
#define IV_MAX 512        // write-watch queries per set (as Savage Wind's gsnap list)
#define IV_GAP 0x10000ull // ranges closer than this share a query
#define HASH_MAX 4096     // pages hashed per set (memory without a write watch)
#define GW_MAX 8192       // listed GPU writes and reports
#define RD_N 64           // invalidated ranges remembered (as vk_memory.c sread)
#define RD_AGE 64         // ... known when invalidated within this many values
#define SMALL_N 64        // small targets remembered (Savage Wind: one-off = not read back in 16 frames, 64 entries)
#define SMALL_FRAMES 16
#define OFF_N 32          // offender table
#define OFF_LOG 24        // first-sighting lines
#define COPY_MAX (32ull << 20)   // bytes of a set's union copied at its snap, for the byte compare

typedef struct { uint64_t lo, hi, fence; uint8_t cls; } Note;
// coff: in copybuf; rd: union ranges, readable at the check (-1 not looked at yet: one VirtualQuery walk per range)
typedef struct { uint64_t lo, hi, coff; uint8_t cls, untracked; int8_t rd; uint32_t hash0, nhash; } SetR;
typedef struct { uint64_t lo, hi, fence; uint8_t kind; } GWrite;

static PlatMutex ah_lock = PLAT_MUTEX_INIT;   // notes, gw, rd, small, lag context
static Note *notes;
static int nnotes;
static uint64_t n_compact;
static GWrite gw[GW_MAX];
static int ngw;
static uint64_t gw_over;
static uint64_t win_lo, win_hi;               // (L, A] of the last frame wait; win_lo == win_hi: none
static int lag_live;                          // inside cpu_synced of a frame wait (vk_ahead_frame_lag .. done)
static uint64_t cur_asked, cur_lagged;
static uint64_t frame_no;
static struct { uint64_t addr, size, at; } rd[RD_N];
static struct { uint64_t addr, frame; } small_t[SMALL_N];

// Game thread only (the frame wait):
static Note *take;
static SetR set[SET_MAX];
static int nset;
static uint64_t iv[IV_MAX][2];
static uint8_t iv_untr[IV_MAX];
static int niv, armed, nclass;   // set: [0, nclass) per class by (class, address), then the union by address
static int bound0, bound1;        // set[bound0, bound1): the BOUND ranges
static uint8_t *copybuf;
static uint32_t set_gen;
static uint64_t hashes[HASH_MAX];
static int nhash;

typedef struct {
    // hit: bytes in pages written after the wait; hitb: bytes that changed; mb: MSYNC bytes N also binds (BOUND)
    uint64_t frames, lagged, behind, read[AH_N + 1], hit[AH_N + 1], hit_frames[AH_N + 1], hit_max[AH_N + 1];
    uint64_t hitb[AH_N + 1], hitb_frames[AH_N + 1], hitb_max[AH_N + 1], read_mb, hit_mb, hit_mb_frames, hit_mb_max;
    uint64_t uncopied, notes, t_check, t_build, t_snap;   // notes taken; ns in the three steps
    uint64_t inval, inval_stale, inval_stale_unknown, inval_stale_bytes, late[AHW_N], small, small_stale, small_oneoff;
    uint64_t cost_ns, set_joined, set_lost, untracked, hashed, hash_skipped, ivq;
} Stat;
static Stat win, tot;   // ah_lock for the readback counters, game thread for the rest (summed under ah_lock)

typedef struct { uint64_t pool, size, bytes, changed, frames, first; uint32_t flags; int cls, mirrored; } Offender;
static Offender off[OFF_N];
static int noff, nofflog;
static uint64_t last_log_ns;

void vk_ahead_init(void) {
    vk_ahead_on = !strcmp(cfg_value("TOTK_SYNC_AHEAD"), "check");
    if (!vk_ahead_on) return;
    notes = malloc(sizeof *notes * NOTE_MAX);
    take = malloc(sizeof *take * NOTE_MAX);
    if (!notes || !take) rt_fatal("vk: out of memory (sync-ahead check)");
    rt_log("sync-ahead check: the frame wait stays exact; logging what a one-frame-late wait without shadow storage "
           "would race (%s)", vk_mem_shadow_on() ? "shadow storage is on: the counts describe the in-place form only "
           "approximately" : "in place");
}

static uint64_t note_fence(uint64_t fence) {
    if (fence) return fence;
    uint64_t f = vk_exec_recording_fence();
    return f ? f : vk_sync_issued() + 1;
}

static int note_cmp(const void *x, const void *y) {
    const Note *a = x, *b = y;
    if (a->cls != b->cls) return a->cls < b->cls ? -1 : 1;
    return a->lo < b->lo ? -1 : a->lo > b->lo;
}

// ah_lock held, list full: overlapping and touching notes of one class merge (the newest value kept).
static void compact(void) {
    qsort(notes, (size_t)nnotes, sizeof *notes, note_cmp);
    int o = 0;
    for (int i = 1; i < nnotes; i++) {
        Note *p = &notes[o];
        if (notes[i].cls == p->cls && notes[i].lo <= p->hi) {
            if (notes[i].hi > p->hi) p->hi = notes[i].hi;
            if (notes[i].fence > p->fence) p->fence = notes[i].fence;
        } else notes[++o] = notes[i];
    }
    nnotes = o + 1;
    n_compact++;
    if (nnotes > NOTE_MAX * 3 / 4) {   // still full: join each class's neighbours (coarser, never smaller)
        o = 0;
        for (int i = 1; i < nnotes; i++) {
            Note *p = &notes[o];
            if (notes[i].cls == p->cls && notes[i].lo - p->hi <= IV_GAP * 16) {
                if (notes[i].hi > p->hi) p->hi = notes[i].hi;
                if (notes[i].fence > p->fence) p->fence = notes[i].fence;
            } else notes[++o] = notes[i];
        }
        nnotes = o + 1;
    }
}

// Per thread: the notes made last, by range, so a binding the replay resolves again in the same submission (most do,
// draw after draw) costs no lock and no entry in the list.
#define SEEN_N 1024
static _Thread_local Note seen[SEEN_N];

void vk_ahead_read(uint64_t addr, uint64_t size, int cls, uint64_t fence) {
    if (!vk_ahead_on || !size || (unsigned)cls >= AH_N || addr + size < addr) return;
    fence = note_fence(fence);
    Note *sn = &seen[((addr * 0x9E3779B97F4A7C15ull) ^ size ^ (uint64_t)cls << 7) >> 54 & (SEEN_N - 1)];
    if (sn->lo == addr && sn->hi == addr + size && sn->fence == fence && sn->cls == cls) return;
    *sn = (Note){addr, addr + size, fence, (uint8_t)cls};
    plat_mutex_lock(&ah_lock);
    Note *l = nnotes ? &notes[nnotes - 1] : NULL;
    if (l && l->cls == cls && l->fence == fence && addr <= l->hi && addr + size >= l->lo) {
        if (addr < l->lo) l->lo = addr;
        if (addr + size > l->hi) l->hi = addr + size;
    } else {
        if (nnotes == NOTE_MAX) compact();
        notes[nnotes++] = (Note){addr, addr + size, fence, (uint8_t)cls};
    }
    plat_mutex_unlock(&ah_lock);
}

void vk_ahead_span(const VkMemSpan *sp, uint64_t addr, uint64_t size, int use) {
    int k = vk_mem_span_inplace(sp);
    if (!k || (k == 2 && use == VK_MUSE_GPU_WRITE)) return;   // a mirror's writes go back as WB / WBM
    if (k == 2) { vk_ahead_read(addr, size, AH_BOUND, 0); return; }
    int cls = use == VK_MUSE_VB ? AH_VB : use == VK_MUSE_IB ? AH_IB : use == VK_MUSE_UBO ? AH_UBO
            : use == VK_MUSE_IND ? AH_IND : use == VK_MUSE_COPY_SRC ? AH_COPY : use == VK_MUSE_GPU_WRITE ? AH_WB : AH_SSBO;
    vk_ahead_read(addr, size, cls, 0);
}

void vk_ahead_gpu_write(uint64_t addr, uint64_t size, int kind, uint64_t fence) {
    if (!vk_ahead_on || !size) return;
    fence = note_fence(fence);
    plat_mutex_lock(&ah_lock);
    int i = ngw;
    while (i > 0 && i > ngw - 8 && !(gw[i - 1].lo == addr && gw[i - 1].hi == addr + size && gw[i - 1].kind == kind)) i--;
    if (i > 0 && i > ngw - 8) {
        if (fence > gw[i - 1].fence) gw[i - 1].fence = fence;
    } else {
        if (ngw == GW_MAX) {   // the oldest goes
            memmove(gw, gw + 1, sizeof *gw * (GW_MAX - 1));
            ngw--, gw_over++;
        }
        gw[ngw++] = (GWrite){addr, addr + size, fence, (uint8_t)kind};
    }
    plat_mutex_unlock(&ah_lock);
}

void vk_ahead_invalidate(uint64_t addr, uint64_t size) {
    if (!vk_ahead_on || !size) return;
    plat_mutex_lock(&ah_lock);
    uint64_t now = vk_sync_issued(), stale = 0;
    for (int i = 0; i < ngw; i++)
        if (gw[i].fence > win_lo && gw[i].fence <= win_hi && gw[i].lo < addr + size && gw[i].hi > addr) {
            uint64_t a = gw[i].lo > addr ? gw[i].lo : addr, b = gw[i].hi < addr + size ? gw[i].hi : addr + size;
            stale += b - a;
        }
    int e = 0;   // the range's entry, else the oldest
    for (int i = 0; i < RD_N; i++) {
        if (rd[i].addr == addr && rd[i].size == size) { e = i; break; }
        if (rd[i].at < rd[e].at) e = i;
    }
    int known = rd[e].addr == addr && rd[e].size == size && rd[e].at + RD_AGE >= now;
    rd[e].addr = addr, rd[e].size = size, rd[e].at = now;
    win.inval++;
    if (stale) win.inval_stale++, win.inval_stale_bytes += stale, win.inval_stale_unknown += !known;
    plat_mutex_unlock(&ah_lock);
}

void vk_ahead_small(uint64_t addr, uint64_t gpu_fence) {
    if (!vk_ahead_on) return;
    plat_mutex_lock(&ah_lock);
    if (lag_live) {
        int e = 0;
        for (int i = 0; i < SMALL_N; i++) {
            if (small_t[i].addr == addr) { e = i; break; }
            if (small_t[i].frame < small_t[e].frame) e = i;
        }
        int oneoff = small_t[e].addr != addr || small_t[e].frame + SMALL_FRAMES < frame_no;
        small_t[e].addr = addr, small_t[e].frame = frame_no;
        win.small++;
        if (gpu_fence > cur_lagged) win.small_stale++, win.small_oneoff += oneoff;
    }
    plat_mutex_unlock(&ah_lock);
}

// ---- the frame set

static int lo_cmp(const void *x, const void *y) {
    const SetR *a = x, *b = y;
    return a->lo < b->lo ? -1 : a->lo > b->lo;
}

static uint64_t hash_bytes(uint64_t lo, uint64_t hi) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (hi - lo);
    const uint8_t *p = (const uint8_t *)(uintptr_t)lo;
    uint64_t n = hi - lo, i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull, h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

// Readable now (committed, not freed by the game since): hashing a page that is gone would fault.
static int readable(uint64_t lo, uint64_t hi) {
    for (uint64_t a = lo, b; a < hi; a = b) {
        int st = plat_mem_state(a, hi, &b);
        if (st != PLAT_PROT_READ && st != PLAT_PROT_RW) return 0;
        if (b <= a) return 0;
    }
    return 1;
}

// The pages of set[i] (in memory without a write watch) hashed into hashes[], or compared with them (check).
static uint64_t hash_pass(SetR *s, int check) {
    uint64_t changed = 0;
    if (!check) s->hash0 = (uint32_t)nhash, s->nhash = 0;
    uint32_t k = s->hash0;
    for (uint64_t a = s->lo; a < s->hi;) {
        uint64_t b = (a & ~0xfffull) + 0x1000;
        if (b > s->hi) b = s->hi;
        if (!check) {   // 0: the page is not compared (the table is full or the page not readable)
            if (nhash == HASH_MAX) { win.hash_skipped += s->hi - a; break; }
            int ok = readable(a, b);
            hashes[nhash++] = ok ? hash_bytes(a, b) | 1 : 0;
            s->nhash++;
            if (ok) win.hashed += b - a;
            else win.hash_skipped += b - a;
        } else {
            if (k >= s->hash0 + s->nhash) break;
            if (hashes[k] && readable(a, b) && hashes[k] != (hash_bytes(a, b) | 1)) changed += b - a;
            k++;
        }
        a = b;
    }
    return changed;
}

static void offender(int cls, uint64_t addr, uint64_t bytes, uint64_t changed) {
    uint64_t lo = 0, size = 0;
    uint32_t flags = 0;
    int mirrored = 0;
    vk_mem_pool_info(addr, &lo, &size, &flags, &mirrored);
    int i = 0;
    while (i < noff && !(off[i].cls == cls && off[i].pool == lo)) i++;
    if (i == noff) {
        if (noff == OFF_N) {   // the smallest goes
            int m = 0;
            for (int k = 1; k < noff; k++) if (off[k].bytes < off[m].bytes) m = k;
            i = m;
        } else noff++;
        off[i] = (Offender){lo, size, 0, 0, 0, addr - lo, flags, cls, mirrored};
        if (nofflog++ < OFF_LOG)
            rt_log("sync-ahead check: frame %llu: %s range written after the wait: pool %#llx (%.1f MiB, flags %#x, %s) "
                   "+%#llx, %llu bytes in written pages, %llu changed", (unsigned long long)frame_no, cls_name[cls],
                   (unsigned long long)lo, size / 1048576.0, flags, mirrored ? "mirrored" : "in place",
                   (unsigned long long)(addr - lo), (unsigned long long)bytes, (unsigned long long)changed);
    }
    off[i].bytes += bytes, off[i].changed += changed, off[i].frames++;
}

// The union range holding a (set[nclass, nset) by address), NULL: none.
static SetR *union_at(uint64_t a) {
    int lo = nclass, hi = nset;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (set[m].hi <= a) lo = m + 1;
        else hi = m;
    }
    return lo < nset && set[lo].lo <= a ? &set[lo] : NULL;
}

// Bytes of [x, y) that differ from the copy taken at the snap (0 where there is none or the memory is gone).
static uint64_t changed_bytes(uint64_t x, uint64_t y) {
    SetR *u = union_at(x);
    if (!u || u->coff == UINT64_MAX || y > u->hi) return 0;
    if (u->rd < 0) u->rd = (int8_t)readable(u->lo, u->hi);
    if (!u->rd) return 0;
    const uint8_t *m = (const uint8_t *)(uintptr_t)x, *c = copybuf + u->coff + (x - u->lo);
    uint64_t n = y - x, d = 0, i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t a, b;
        memcpy(&a, m + i, 8), memcpy(&b, c + i, 8);
        if (a == b) continue;
        for (int k = 0; k < 8; k++) d += m[i + k] != c[i + k];
    }
    for (; i < n; i++) d += m[i] != c[i];
    return d;
}

typedef struct { int cls; uint64_t first, changed, changed_bound; } Seg;

// A written page's part [x, y) of a set range: its changed bytes, and for MSYNC those inside a BOUND range too.
static void seg_cb(uint64_t x, uint64_t y, void *arg) {
    Seg *g = arg;
    if (g->first == UINT64_MAX) g->first = x;
    g->changed += changed_bytes(x, y);
    if (g->cls != AH_MSYNC) return;
    int a = bound0, b = bound1;   // the first BOUND range ending after x
    while (a < b) {
        int m = (a + b) / 2;
        if (set[m].hi <= x) a = m + 1;
        else b = m;
    }
    for (int i = a; i < bound1 && set[i].lo < y; i++) {
        uint64_t p = set[i].lo > x ? set[i].lo : x, q = set[i].hi < y ? set[i].hi : y;
        if (p < q) g->changed_bound += changed_bytes(p, q);
    }
}

static void max_into(uint64_t *m, uint64_t v) { if (v > *m) *m = v; }

// The armed set against the writes since its snap.
static void check_set(void) {
    vk_mem_ahead_observe((const uint64_t(*)[2])iv, niv, iv_untr);
    win.ivq += (uint64_t)niv;
    uint64_t hit[AH_N + 1] = {0}, hitb[AH_N + 1] = {0}, mb = 0;
    for (int i = 0; i < nset; i++) {
        SetR *s = &set[i];
        if (s->cls == AH_BOUND) continue;
        Seg g = {s->cls, UINT64_MAX, 0, 0};
        uint64_t b;
        if (s->untracked) b = g.changed = hash_pass(s, 1), g.first = s->lo;
        else b = vk_mem_ahead_written(s->lo, s->hi, set_gen, seg_cb, &g);
        if (!b) continue;
        hit[s->cls] += b, hitb[s->cls] += g.changed, mb += g.changed_bound;
        if (s->cls != AH_N) offender(s->cls, g.first, b, g.changed);
    }
    for (int c = 0; c <= AH_N; c++) {
        win.hit[c] += hit[c], win.hitb[c] += hitb[c];
        win.hit_frames[c] += hit[c] != 0, win.hitb_frames[c] += hitb[c] != 0;
        max_into(&win.hit_max[c], hit[c]), max_into(&win.hitb_max[c], hitb[c]);
    }
    win.hit_mb += mb, win.hit_mb_frames += mb != 0;
    max_into(&win.hit_mb_max, mb);
    armed = 0;
}

// take[0..n): notes of values in (L, A]. Merged per class, then over all classes (class AH_N), then the queries.
static void build_set(int n) {
    qsort(take, (size_t)n, sizeof *take, note_cmp);
    nset = 0;
    for (int i = 0; i < n; i++) {
        SetR *p = nset ? &set[nset - 1] : NULL;
        if (p && p->cls == take[i].cls && take[i].lo <= p->hi) {
            if (take[i].hi > p->hi) p->hi = take[i].hi;
            continue;
        }
        if (nset == SET_MAX / 2) {   // full: the rest joins the last range of its class, or is left out
            if (p && p->cls == take[i].cls) {
                win.set_joined += take[i].lo - p->hi;
                if (take[i].hi > p->hi) p->hi = take[i].hi;
            } else win.set_lost += take[i].hi - take[i].lo;
            continue;
        }
        set[nset++] = (SetR){take[i].lo, take[i].hi, UINT64_MAX, take[i].cls, 0, -1, 0, 0};
    }
    int nc = nset;
    nclass = nc, bound0 = bound1 = 0;
    for (int i = 0; i < nc; i++) {
        win.read[set[i].cls] += set[i].hi - set[i].lo;
        if (set[i].cls == AH_BOUND) bound1 = i + 1;
        else if (set[i].cls < AH_BOUND) bound0 = i + 1;
    }
    if (bound1 < bound0) bound1 = bound0;
    // MSYNC bytes N also binds: both lists are sorted by address.
    for (int i = 0, j = bound0; i < nc && j < bound1; i++) {
        if (set[i].cls != AH_MSYNC) continue;
        while (j < bound1 && set[j].hi <= set[i].lo) j++;
        for (int k = j; k < bound1 && set[k].lo < set[i].hi; k++) {
            uint64_t p = set[k].lo > set[i].lo ? set[k].lo : set[i].lo, q = set[k].hi < set[i].hi ? set[k].hi : set[i].hi;
            if (p < q) win.read_mb += q - p;
        }
    }
    // The union over the classes that race (not BOUND), after them.
    SetR *u = set + nc;
    int nr = 0;
    for (int i = 0; i < nc; i++)
        if (set[i].cls != AH_BOUND) u[nr++] = set[i];
    qsort(u, (size_t)nr, sizeof *u, lo_cmp);
    int nu = 0;
    for (int i = 0; i < nr; i++) {
        if (nu && u[i].lo <= u[nu - 1].hi) {
            if (u[i].hi > u[nu - 1].hi) u[nu - 1].hi = u[i].hi;
        } else u[nu++] = (SetR){u[i].lo, u[i].hi, UINT64_MAX, AH_N, 0, -1, 0, 0};
    }
    nset = nc + nu;
    for (int i = 0; i < nu; i++) win.read[AH_N] += u[i].hi - u[i].lo;
    // Queries: the union gap-joined; a gap that does not fit doubles until it does.
    uint64_t gap = IV_GAP;
    for (;;) {
        niv = 0;
        for (int i = 0; i < nu && niv <= IV_MAX; i++) {
            if (niv && u[i].lo - iv[niv - 1][1] <= gap) {
                if (u[i].hi > iv[niv - 1][1]) iv[niv - 1][1] = u[i].hi;
            } else if (niv < IV_MAX) iv[niv][0] = u[i].lo, iv[niv][1] = u[i].hi, niv++;
            else { niv = IV_MAX + 1; break; }
        }
        if (niv <= IV_MAX) break;
        gap *= 2;
    }
}

// Whether [lo, hi) lies in a query the write watch does not cover.
static int untracked_at(uint64_t lo) {
    int a = 0, b = niv;
    while (a < b) {
        int m = (a + b) / 2;
        if (iv[m][1] <= lo) a = m + 1;
        else b = m;
    }
    return a < niv && iv[a][0] <= lo && iv_untr[a];
}

static void snap_set(void) {
    set_gen = vk_mem_ahead_observe((const uint64_t(*)[2])iv, niv, iv_untr);
    win.ivq += (uint64_t)niv;
    nhash = 0;
    if (!copybuf && !(copybuf = malloc(COPY_MAX))) rt_fatal("vk: out of memory (sync-ahead check)");
    uint64_t used = 0;
    for (int i = nclass; i < nset; i++) {
        SetR *s = &set[i];
        uint64_t n = s->hi - s->lo;
        if (used + n <= COPY_MAX && readable(s->lo, s->hi)) {
            memcpy(copybuf + used, (const void *)(uintptr_t)s->lo, n);
            s->coff = used, used += n;
        } else win.uncopied += n;
    }
    for (int i = 0; i < nset; i++) {
        SetR *s = &set[i];
        if (s->cls == AH_BOUND) continue;
        s->untracked = (uint8_t)untracked_at(s->lo);
        if (!s->untracked) continue;
        win.untracked += s->cls == AH_N ? s->hi - s->lo : 0;
        hash_pass(s, 0);
    }
    armed = 1;
}

static void log_line(const Stat *s, const char *what) {
    char b[2048];   // about 1,200 bytes with every class
    int k = 0;
    double f = s->frames ? (double)s->frames : 1;
    k += snprintf(b + k, sizeof b - (size_t)k, "sync-ahead check: %s %llu frames (%llu lagged, the GPU "
                  "behind in %llu): written after the wait KiB/frame", what, (unsigned long long)s->frames,
                  (unsigned long long)s->lagged, (unsigned long long)s->behind);
    for (int c = 0; c <= AH_N; c++)
        if (s->hit[c] || c == AH_N)
            k += snprintf(b + k, sizeof b - (size_t)k, " %s %.1f (in %llu frames, max %.0f; changed %.1f in %llu, max %.0f)",
                          cls_name[c], s->hit[c] / 1024.0 / f, (unsigned long long)s->hit_frames[c], s->hit_max[c] / 1024.0,
                          s->hitb[c] / 1024.0 / f, (unsigned long long)s->hitb_frames[c], s->hitb_max[c] / 1024.0);
    k += snprintf(b + k, sizeof b - (size_t)k, " msync&bound changed %.1f (in %llu, max %.0f)", s->hit_mb / 1024.0 / f,
                  (unsigned long long)s->hit_mb_frames, s->hit_mb_max / 1024.0);
    k += snprintf(b + k, sizeof b - (size_t)k, "; read KiB/frame");
    for (int c = 0; c <= AH_N; c++)
        if (s->read[c] || c == AH_N) k += snprintf(b + k, sizeof b - (size_t)k, " %s %.1f", cls_name[c], s->read[c] / 1024.0 / f);
    k += snprintf(b + k, sizeof b - (size_t)k, " msync&bound %.1f", s->read_mb / 1024.0 / f);
    k += snprintf(b + k, sizeof b - (size_t)k,
                  "; stale: invalidates %llu of %llu (%llu not read lately, %.1f KiB), late reports counter %llu samples "
                  "%llu event %llu, writes %llu, small write-backs %llu of %llu (%llu one-off)",
                  (unsigned long long)s->inval_stale, (unsigned long long)s->inval, (unsigned long long)s->inval_stale_unknown,
                  s->inval_stale_bytes / 1024.0, (unsigned long long)s->late[AHW_COUNTER], (unsigned long long)s->late[AHW_SAMPLES],
                  (unsigned long long)s->late[AHW_EVENT], (unsigned long long)s->late[AHW_WRITE],
                  (unsigned long long)s->small_stale, (unsigned long long)s->small, (unsigned long long)s->small_oneoff);
    snprintf(b + k, sizeof b - (size_t)k, "; game-thread cost %.3f ms/frame, %.1f queries/frame, untracked %.1f "
             "KiB/frame (hashed %.1f, skipped %.1f), not copied %.1f KiB, set over its size: %.1f KiB joined, %.1f KiB "
             "left out; note compactions %llu, gpu-write list overflow %llu; per frame %.0f notes, check %.3f ms, build %.3f ms, "
             "snap %.3f ms", s->cost_ns / 1e6 / f, s->ivq / f,
             s->untracked / 1024.0 / f, s->hashed / 1024.0 / f, s->hash_skipped / 1024.0 / f, s->uncopied / 1024.0,
             s->set_joined / 1024.0, s->set_lost / 1024.0, (unsigned long long)n_compact, (unsigned long long)gw_over,
             s->notes / f, s->t_check / 1e6 / f, s->t_build / 1e6 / f, s->t_snap / 1e6 / f);
    rt_log("%s", b);
}

static void stat_add(Stat *d, const Stat *s) {
    const uint64_t *x = (const uint64_t *)s;
    uint64_t *y = (uint64_t *)d;
    for (size_t i = 0; i < sizeof *s / sizeof(uint64_t); i++) y[i] += x[i];
    for (int c = 0; c <= AH_N; c++) {   // maxima, not sums
        d->hit_max[c] -= s->hit_max[c], max_into(&d->hit_max[c], s->hit_max[c]);
        d->hitb_max[c] -= s->hitb_max[c], max_into(&d->hitb_max[c], s->hitb_max[c]);
    }
    d->hit_mb_max -= s->hit_mb_max, max_into(&d->hit_mb_max, s->hit_mb_max);
}

void vk_ahead_frame_entry(void) {
    if (!vk_ahead_on || !armed) return;
    uint64_t t0 = plat_now_ns();
    check_set();
    uint64_t dt = plat_now_ns() - t0;
    win.cost_ns += dt, win.t_check += dt;
}

void vk_ahead_frame_lag(uint64_t asked, uint64_t lagged, int behind) {
    if (!vk_ahead_on) return;
    if (behind && lagged < asked) win.behind++;
    plat_mutex_lock(&ah_lock);
    frame_no++;
    cur_asked = asked, cur_lagged = lagged, lag_live = 1;
    plat_mutex_unlock(&ah_lock);
}

void vk_ahead_frame_done(int ok) {
    if (!vk_ahead_on) return;
    uint64_t t0 = plat_now_ns();
    plat_mutex_lock(&ah_lock);
    lag_live = 0;
    uint64_t a = cur_asked, l = cur_lagged;
    int n = 0, k = 0;
    if (ok) {
        for (int i = 0; i < nnotes; i++) {
            if (notes[i].fence > a) notes[k++] = notes[i];
            else if (notes[i].fence > l) take[n++] = notes[i];
        }
        nnotes = k;
        k = 0;
        for (int i = 0; i < ngw; i++) {   // writes done even with the lag go; the late ones are counted
            if (gw[i].fence > l && gw[i].fence <= a) win.late[gw[i].kind]++;
            if (gw[i].fence > l) gw[k++] = gw[i];
        }
        ngw = k;
        win_lo = l, win_hi = a;
    }
    plat_mutex_unlock(&ah_lock);
    if (ok) {
        win.frames++;
        if (l < a) win.lagged++;
        uint64_t t1 = plat_now_ns();
        build_set(n);
        uint64_t t2 = plat_now_ns();
        snap_set();
        win.notes += (uint64_t)n, win.t_build += t2 - t1, win.t_snap += plat_now_ns() - t2;
    }
    uint64_t now = plat_now_ns();
    win.cost_ns += now - t0;
    if (now - last_log_ns >= 1000000000ull && win.frames) {
        plat_mutex_lock(&ah_lock);
        Stat w = win;
        memset(&win, 0, sizeof win);
        plat_mutex_unlock(&ah_lock);
        if (last_log_ns) log_line(&w, "1 s:");
        stat_add(&tot, &w);
        last_log_ns = now;
    }
}

static int off_cmp(const void *x, const void *y) {
    const Offender *a = x, *b = y;
    return a->bytes < b->bytes ? 1 : a->bytes > b->bytes ? -1 : 0;
}

void vk_ahead_log_final(void) {
    static int32_t done;
    if (!vk_ahead_on || plat_xchg32(&done, 1)) return;
    plat_mutex_lock(&ah_lock);
    stat_add(&tot, &win);
    memset(&win, 0, sizeof win);
    plat_mutex_unlock(&ah_lock);
    log_line(&tot, "run:");
    qsort(off, (size_t)noff, sizeof *off, off_cmp);
    for (int i = 0; i < noff && i < 12; i++)
        rt_log("sync-ahead check: offender %d: %s pool %#llx (%.1f MiB, flags %#x, %s) first at +%#llx: %.1f KiB in written "
               "pages, %.1f KiB changed, in %llu frames", i + 1, cls_name[off[i].cls], (unsigned long long)off[i].pool,
               off[i].size / 1048576.0, off[i].flags, off[i].mirrored ? "mirrored" : "in place",
               (unsigned long long)off[i].first, off[i].bytes / 1024.0, off[i].changed / 1024.0,
               (unsigned long long)off[i].frames);
}

// --probe-vkmem (tex_probe.c test_import): [a, a+size) is write-watched guest memory of an imported pool, at least
// 0x40000 bytes, that nothing else touches meanwhile. A frame's read set gets a CPU write before its wait (not counted)
// and one after it (counted in its class and the union, not in another class); a frame the late wait would not have
// lagged keeps no set; an invalidate of a GPU write of the lagged frame counts as stale. 0: passed, else the failed check.
int vk_ahead_selftest(uint64_t a, uint64_t size) {
    if (size < 0x40000) return 1;
    int was = vk_ahead_on;
    Note *n0 = notes, *t0 = take;
    Note nbuf[64], tbuf[64];
    if (!notes) notes = nbuf, take = tbuf;   // the checks add far fewer than 64 notes
    vk_ahead_on = 1;
    plat_mutex_lock(&ah_lock);
    nnotes = ngw = 0;
    plat_mutex_unlock(&ah_lock);
    armed = 0;
    Stat saved = win;
    uint64_t log_was = last_log_ns;
    last_log_ns = plat_now_ns();   // no summary line (it would reset the counters) while the checks run
    memset(&win, 0, sizeof win);
    volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)a;
    uint64_t f = vk_sync_issued() + 1000;   // values no real submission has
    int bad = 0;
    vk_ahead_read(a + 0x10000, 0x8000, AH_MSYNC, f);
    vk_ahead_read(a + 0x20000, 0x1000, AH_VB, f);
    vk_ahead_read(a + 0x28000, 0x1000, AH_VB, f + 1);   // a later frame's: stays in the list
    vk_ahead_read(a + 0x12300, 0x100, AH_BOUND, f);     // N binds 256 bytes of the synced range
    vk_ahead_gpu_write(a + 0x30000, 16, AHW_COUNTER, f);
    m[0x10010] = (uint8_t)(m[0x10010] + 1);   // before the wait: the frame wrote it before the GPU read it
    vk_ahead_frame_lag(f, f - 1, 1);
    vk_ahead_frame_done(1);
    if (!armed || win.read[AH_MSYNC] != 0x8000 || win.read[AH_VB] != 0x1000 || win.read[AH_N] != 0x9000) bad = 2;
    else if (win.late[AHW_COUNTER] != 1) bad = 3;
    vk_ahead_invalidate(a + 0x30000, 0x100);
    if (!bad && (win.inval != 1 || win.inval_stale != 1)) bad = 4;
    m[0x12345] = (uint8_t)(m[0x12345] + 1);   // after the wait, in the msync range: the late wait would race it
    m[0x3a000] = (uint8_t)(m[0x3a000] + 1);   // after the wait, outside the set
    vk_ahead_frame_entry();
    if (!bad && (armed || win.hit[AH_MSYNC] != 0x1000 || win.hit[AH_VB] || win.hit[AH_N] != 0x1000)) bad = 5;
    if (!bad && (win.hitb[AH_MSYNC] != 1 || win.hitb[AH_N] != 1 || win.hit_mb != 1 || win.read_mb != 0x100)) bad = 9;
    plat_mutex_lock(&ah_lock);
    int left = nnotes;
    plat_mutex_unlock(&ah_lock);
    if (!bad && left != 1) bad = 6;
    vk_ahead_frame_lag(f + 1, f + 1, 0);   // not lagged: nothing at risk
    vk_ahead_frame_done(1);
    if (!bad && win.read[AH_N] != 0x9000) bad = 7;
    m[0x28010] = (uint8_t)(m[0x28010] + 1);
    vk_ahead_frame_entry();
    if (!bad && win.hit[AH_N] != 0x1000) bad = 8;
    plat_mutex_lock(&ah_lock);
    nnotes = ngw = 0, win_lo = win_hi = 0;
    plat_mutex_unlock(&ah_lock);
    armed = 0, noff = 0, nofflog = 0;
    win = saved;
    last_log_ns = log_was;
    notes = n0, take = t0;
    vk_ahead_on = was;
    return bad;
}
