// NVN front end, command-buffer slice: command buffers, their memory, recording
// as op records (nvn_obj.h), recording copies behind command handles, and the submit-time resolution of deferred binds
// (nvn_submit_prepare, called by nvnQueueSubmitCommands in nvn_api_core.c). Ported from MacNativeGames
// games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c @ b75be8cf (MIT, Copyright (c) 2026 jhaizhou; origin d2r-mobile),
// lines 647-1062, 1261-1286, 1486-1760 and 1975-1990, and the *Deferred and memory-callback entry points of
// totk_nvn_ext.c in the same directory. Metal, mach, the translation thread, Apple diagnostics, the per-frame
// statistics and the copy marks in control memory dropped; the texture pin sweep replaced by counting (texture pins).
//
// Records are written into a growable host buffer per command buffer, never into the game's command memory: games hand
// the same command memory to several command buffers recording at once (budgeted for real NVN encodings), and our
// records are larger. The game's command and control memory is accounted only. EndRecording makes the finished
// recording a copy in host memory (a copy of its bytes, or the recording buffer itself: end_recording); the handle is
// that copy (a flat OP_BEGIN ... OP_END sequence) and lives until the game adds the same control memory again.
// CallCommands and CopyCommands inline the called recording or, for a large one, record an OP_CALL that names it
// (inline_recording); copies hold no OP_JUMP.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "platform/platform.h"

#define CMD_MAGIC MAGIC('C', 'M', 'D', 'B')
#define COPY_MAGIC 0x59504f43444d43ull     // "CMDCOPY": prefix of a live recording copy
#define OP_RESERVE 24                       // room always kept at the end of the recording buffer
#define MIN_COMMAND_SIZE 512                // NVN_DEVICE_INFO_COMMAND_BUFFER_MIN_COMMAND_SIZE (nvn_api_core.c devinfo)
#define MIN_CONTROL_SIZE 256                // NVN_DEVICE_INFO_COMMAND_BUFFER_MIN_CONTROL_SIZE
#define CTL_PER_RECORDING 64                // control memory one recording is reported to use
#define CTL_PER_CALL 24                     // control memory one called handle is reported to use (CallCommands)
#define COPY_PAGE_SHIFT 16                  // recording copy index granularity (release_copies)

static cmd_t *cmd_of(const Cpu *c) { return arg_p(c, 0); }

// ---------- memory callbacks
// NVN calls the game's memory callback when a recording needs command or control memory and none is left. Records
// live in host memory here, so the callback is called only where the hardware certainly would: a recording starts
// without any command memory, or ends without any control memory. It runs on the calling guest thread through
// rt_call; whatever memory it adds is accounted. Set{Command,Control}MemoryCallbackEnabled(false) suppress it.
enum { CB_NO_COMMAND = 1, CB_NO_CONTROL = 2 };
static PlatMutex cb_lock = PLAT_MUTEX_INIT;
static nvn_hmap cb_off = {.klen = 8};       // command buffer -> disabled callback kinds | 0x100

static unsigned cb_disabled(const cmd_t *o) {
    plat_mutex_lock_shared(&cb_lock);
    uintptr_t v = (uintptr_t)nvn_hm_get(&cb_off, &o);
    plat_mutex_unlock_shared(&cb_lock);
    return (unsigned)v & 0xff;
}

static void cb_set_disabled(const cmd_t *o, unsigned kind, int disabled) {
    plat_mutex_lock(&cb_lock);
    unsigned v = (unsigned)(uintptr_t)nvn_hm_get(&cb_off, &o) & 0xff;
    v = disabled ? v | kind : v & ~kind;
    if (v) nvn_hm_put(&cb_off, &o, (void *)(uintptr_t)(v | 0x100));
    else nvn_hm_del(&cb_off, &o);
    plat_mutex_unlock(&cb_lock);
}

static void memory_event(Cpu *c, cmd_t *o, int event, uint64_t min) {
    if (!o->cb || (cb_disabled(o) & (event == NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY ? CB_NO_COMMAND
                                                                                                     : CB_NO_CONTROL)))
        return;
    NVN_LOG("command buffer %p: memory callback %p (%s memory, %llu bytes)", (void *)o, (void *)o->cb,
            event == NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY ? "command" : "control",
            (unsigned long long)min);
    rt_call(c, (uint64_t)(uintptr_t)o->cb, 4, (uint64_t)(uintptr_t)o, (uint64_t)event, min,
            (uint64_t)(uintptr_t)o->cb_data);
}

// ---------- recording buffer
static void put_hdr(uint8_t *p, uint32_t op, uint32_t size) {
    op_hdr h = {op, size};
    memcpy(p, &h, sizeof h);
}

// 1.4.3 initializes and finalizes command buffers about 2000 times a second while a scene loads and several times a
// frame in play, and its two largest per-frame recordings take 1-2 MiB. Freeing the recording buffer at Finalize made
// every new recording grow again from 64 KiB through up to five reallocations and copies, the last ones fresh 1-2 MiB
// blocks the heap takes from the system: new commit and zero-filled pages twice a frame. Those growths were most of the
// process's new commit requests, so when the system ran short for a moment (a Depths load at render scale 200 on 16 GB)
// a 1 MiB growth was the allocation that failed. Finalized buffers therefore wait in a small pool for the next
// recording, and a failed allocation waits for memory instead of ending the game (realloc_wait).
//
// Every recording buffer starts with a copy_prefix (o->seg points past it). The lists a recording copy
// needs (the texture pins its records name, its deferred binds, the copies its OP_CALL records name) grow in that
// prefix while the records are made, so EndRecording walks nothing; and EndRecording gives a buffer that a large
// recording fills well away as the copy itself (end_recording), so the records of the big per-frame recordings are
// written once and never copied. The command buffer then takes another buffer from the pool: the smallest one that
// holds what this command buffer recorded before (rec_hint), so a buffer and its recording keep about one size.
typedef struct { void *k; uint32_t off, idx; } dkid_t;   // the named copy, the record's offset, its place in kid_v
typedef struct { uint64_t magic, bytes, made_at, used_at, deferred, pins, cap, hw, fsync; const tex_t **pin_v;
                 uint64_t state, nkids; void **kid_v; uint64_t ndef, ndkids, res_at; uint32_t *def_v; dkid_t *dkid_v;
                 uint64_t recbuf;                                // record bytes the block holds when it is (or was) a
                                                                 // recording buffer: it goes back to the buffer pool
                 uint32_t pin_cap, kid_cap, def_cap, dkid_cap;   // room in the four lists (they grow while recording)
               } copy_prefix;
_Static_assert(sizeof(copy_prefix) % 8 == 0, "records stay 8-byte aligned");

#define POOL_SLOTS 32
#define POOL_MAX_BYTES ((size_t)4 << 20)   // a larger buffer (a recording that inlined a big one) goes back to the heap
static PlatMutex pool_lock = PLAT_MUTEX_INIT;
static struct { uint8_t *p; size_t size; } pool[POOL_SLOTS];   // p: the buffer's records (past its prefix)
static uint32_t npool;

// A recording buffer with its lists, back to the heap.
static void recbuf_free(uint8_t *seg) {
    if (!seg) return;
    copy_prefix *pf = (copy_prefix *)seg - 1;
    free((void *)pf->pin_v), free(pf->kid_v), free(pf->def_v), free(pf->dkid_v);
    free(pf);
}

// The smallest pooled buffer of at least want bytes and its size, or NULL: a command buffer that recorded 40 KiB last
// time gets a 64 KiB buffer and leaves the megabyte ones to the recordings that fill them.
static uint8_t *pool_take(size_t want, size_t *size) {
    uint8_t *p = NULL;
    plat_mutex_lock(&pool_lock);
    uint32_t best = POOL_SLOTS;
    for (uint32_t i = 0; i < npool; i++)
        if (pool[i].size >= want && (best == POOL_SLOTS || pool[i].size < pool[best].size)) best = i;
    if (best < POOL_SLOTS) {
        p = pool[best].p;
        *size = pool[best].size;
        pool[best] = pool[--npool];
    }
    plat_mutex_unlock(&pool_lock);
    return p;
}

// Keeps a buffer no recording uses (its lists empty) when the pool has room or holds a smaller one (freed instead).
static void pool_put(uint8_t *p, size_t size) {
    if (!p) return;
    copy_prefix *pf = (copy_prefix *)p - 1;
    pf->magic = 0;
    pf->pins = pf->nkids = pf->ndef = pf->ndkids = pf->fsync = 0;
    if (size <= POOL_MAX_BYTES) {
        plat_mutex_lock(&pool_lock);
        if (npool < POOL_SLOTS) {
            pool[npool].p = p;
            pool[npool++].size = size;
            p = NULL;
        } else {
            uint32_t small = 0;
            for (uint32_t i = 1; i < npool; i++)
                if (pool[i].size < pool[small].size) small = i;
            if (pool[small].size < size) {
                uint8_t *drop = pool[small].p;
                pool[small].p = p;
                pool[small].size = size;
                p = drop;
            }
        }
        plat_mutex_unlock(&pool_lock);
    }
    recbuf_free(p);
}

static void pool_drain(void) {
    plat_mutex_lock(&pool_lock);
    while (npool) recbuf_free(pool[--npool].p);
    plat_mutex_unlock(&pool_lock);
}

// The largest recording a command buffer made lately, by its address: a hint only (two threads may write a slot at
// once; a wrong hint costs one growth or one copy). A slot holds the address above 16 bits of KiB. A smaller recording
// lowers it by an eighth of the difference, once a submission at most: a command buffer that records one large command
// set and thirty small ones a frame keeps the large one's size.
static uint64_t nsubmits;                   // QueueSubmitCommands calls so far
static uint64_t rec_hint_v[256];
static uint32_t rec_hint_at[256];
static inline uint32_t rec_hint_slot(const void *o) { return (uint32_t)(((uintptr_t)o >> 4) * 0x9E3779B1u) >> 24; }
static size_t rec_hint(const void *o) {
    uint64_t v = __atomic_load_n(&rec_hint_v[rec_hint_slot(o)], __ATOMIC_RELAXED);
    return v >> 16 == ((uintptr_t)o & 0xffffffffffffull) ? (size_t)(v & 0xffff) << 10 : 0;
}
static void rec_hint_set(const void *o, size_t n) {
    uint32_t s = rec_hint_slot(o), now = (uint32_t)__atomic_load_n(&nsubmits, __ATOMIC_RELAXED);
    size_t old = rec_hint(o) >> 10, kib = (n + 1023) >> 10;
    if (old >= kib) {
        if (old == kib || __atomic_load_n(&rec_hint_at[s], __ATOMIC_RELAXED) == now) return;
        kib = old - (old - kib + 7) / 8;
    }
    if (kib > 0xffff) kib = 0xffff;
    __atomic_store_n(&rec_hint_at[s], now, __ATOMIC_RELAXED);
    __atomic_store_n(&rec_hint_v[s], ((uintptr_t)o & 0xffffffffffffull) << 16 | kib, __ATOMIC_RELAXED);
}

// realloc that waits out a short commit shortage: Windows refuses commit beyond the current limit while it grows a
// system-managed page file, so the same request succeeds a moment later, and a stall of some frames is better than
// ending the game. Frees the pooled buffers first. NULL only after about 30 s without memory; the caller ends the game.
// Never called with a lock held.
static void big_drain(void);

static void *realloc_wait(void *old, size_t n, const char *what) {
    void *p = realloc(old, n);
    if (p) return p;
    pool_drain();
    big_drain();
    rt_oom_request();   // the Vulkan back end gives memory back at its next submit (oom.c)
    static uint32_t reports;
    int report = __atomic_fetch_add(&reports, 1, __ATOMIC_RELAXED) < 8;
    if (report) {
        double proc, used, limit;
        plat_commit_info(&proc, &used, &limit);
        rt_log("nvn: %s of %zu bytes failed (memory commit: this process %.0f MB, the system %.0f of %.0f MB); waiting "
               "for memory", what, n, proc, used, limit);
    }
    uint64_t t0 = plat_now_ms();
    while (plat_now_ms() - t0 < 30000) {
        plat_sleep_ms(10);
        if ((p = realloc(old, n))) {
            if (report)
                rt_log("nvn: %s of %zu bytes succeeded after %llu ms", what, n,
                       (unsigned long long)(plat_now_ms() - t0));
            return p;
        }
    }
    return NULL;
}

static void cmd_reserve(cmd_t *o, size_t need) {
    if (o->seg && o->cur + need + OP_RESERVE <= o->end) return;
    size_t want = 0;
    if (!o->seg) {   // the first record since Initialize or since EndRecording gave the buffer away: a pooled buffer
        size_t size = 0;   // that holds what this command buffer recorded before, when one waits
        want = rec_hint(o);
        want += want / 8;
        if (want < need + OP_RESERVE) want = need + OP_RESERVE;
        uint8_t *p = pool_take(want, &size);
        if (p) {
            o->seg = o->cur = p;
            o->end = p + size;
            return;
        }
    }
    size_t used = o->seg ? (size_t)(o->cur - o->seg) : 0;
    size_t cap = o->seg ? (size_t)(o->end - o->seg) : 0;
    if (!cap) cap = 64 << 10;
    while (used + need + OP_RESERVE > cap || cap < want) cap *= 2;
    copy_prefix *blk = realloc_wait(o->seg ? (copy_prefix *)o->seg - 1 : NULL, sizeof(copy_prefix) + cap,
                                    "command recording buffer");
    if (blk && !o->seg) memset(blk, 0, sizeof *blk);
    uint8_t *b = blk ? (uint8_t *)(blk + 1) : NULL;
    // Still failing after the wait: the system has no commit left (the fatal report states it), or seg is no block of
    // ours: a command buffer object the game overwrote or copied shows a wrong magic or more used than its old size.
    if (!b)
        rt_fatal("nvn: command recording buffer of %zu bytes (command buffer %p, magic %#x, buffer %p of %zu bytes, %zu "
                 "used, %zu asked)", cap, (void *)o, o->magic, (void *)o->seg, o->seg ? (size_t)(o->end - o->seg) : 0,
                 used, need);
    o->seg = b;
    o->cur = b + used;
    o->end = b + cap;
}

// Space for one record (header included); the header's op is filled by the caller.
static uint8_t *cmd_alloc(cmd_t *o, uint32_t size) {
    size = (size + 7) & ~7u;
    cmd_reserve(o, size);
    uint8_t *p = o->cur;
    o->cur += size;
    o->cmd_used += size;
    o->hw_used += size;
    o->last_rec = size;
    put_hdr(p, 0, size);
    return p;
}

// The game sums nvnCommandBufferGetCommandMemoryUsed over recordings to size resource heaps, so usage must be what the
// real NVN push-buffer encoding takes, not our record size. Known encodings (Maxwell methods, from a real recording):
// DrawElements 12 words, DrawElementsBaseVertex 11, BindVertexAttribState 1+16, BindVertexStreamState 16+n,
// BindVertexBuffer 6 words each.
static void hw_fix(cmd_t *o, size_t bytes) {
    o->hw_used += bytes;
    o->hw_used -= o->last_rec;
    o->last_rec = bytes;
}

// A zeroed payload of one record.
static void *rec(cmd_t *o, uint32_t op, size_t payload) {
    uint32_t sz = (uint32_t)((8 + payload + 7) & ~(size_t)7);
    uint8_t *p = cmd_alloc(o, sz);
    put_hdr(p, op, sz);
    memset(p + 8, 0, sz - 8);
    return p + 8;
}
#define REC(o, OP, T) ((T *)rec((o), (OP), sizeof(T)))
// Array-carrying binds take the prefix fields plus n elements; the replay reads only the first n.
#define RECN(o, OP, T, ARR, N) ((T *)rec((o), (OP), offsetof(T, ARR) + sizeof(((T *)0)->ARR[0]) * (size_t)(N)))

static void copy_view(view_t *dst, uint8_t *has, const NVNtextureView *v) {
    *has = v != NULL;
    if (v) *dst = *(const view_t *)v;
}

// ---------- texture pins
// Recordings refer to textures by pointer, but the game may finalize or reuse an NVNtexture after recording and before
// the submit (NVN itself captured only GPU state). Records therefore point at an immutable host copy per texture
// initialization (tex_t.serial). The game initializes its transient targets again every frame, about 29 new serials a
// frame on 1.4.3, so copies kept for the run grew the heap by about 44 MiB per minute of play, standing still too, until
// the commit limit ended the game after a few hours. A copy is freed once nothing can read it any more:
// - refs counts the recording copies whose ops name it: live ones, released ones a queued submission still reads, and
//   per-submission clones (pin_refs, at end_recording and wherever such a copy is freed or cloned);
// - a recording in progress holds pins no copy counts yet, through its own tex_pin calls and through ops inlined from a
//   copy that was released since: a pin goes only NVN_PIN_AGE submits after its last tex_pin and after refs fell to 0;
// - the replay's current render targets carry over to later submissions and are compared by pointer (vk_exec.c set_rt),
//   so a freed address reused by a new pin would read as the same target: nvn_pins_sweep runs between submissions on
//   the thread that replays them and keeps the pins it is given.
// MacNativeGames hit the same growth (3.16 million copies, 405 MB after 27 minutes) and marks pins by walking every live
// recording each window (hle_nvn.c pin_scan); counting at copy and release walks each copy's ops once more, when it is
// freed. TOTK_PIN_SWEEP=0 keeps every copy.
typedef struct {
    tex_t t;              // first: ops hold pins as tex_t pointers
    uint64_t last_use;    // nsubmits at the last tex_pin or the last drop of refs
    uint32_t refs;
    uint8_t queued;       // in pin_q
} pin_t;
_Static_assert(offsetof(pin_t, t) == 0, "ops hold pins as tex_t pointers");
typedef struct { pin_t *p; uint64_t at; } pin_due;
static PlatMutex pin_lock = PLAT_MUTEX_INIT;   // lock order: copy_lock, then pin_lock
static nvn_hmap pins = {.klen = 8};         // serial -> pin_t
static pin_due *pin_q;                      // pins with no counted copy, oldest first: [pin_qh, pin_qn)
static size_t pin_qh, pin_qn, pin_qcap;
static uint64_t pins_live, pins_freed;

static void pin_queue(pin_t *p, uint64_t at) {   // pin_lock held
    if (p->queued) return;
    if (pin_qh && pin_qh * 2 >= pin_qn) {
        memmove(pin_q, pin_q + pin_qh, sizeof *pin_q * (pin_qn - pin_qh));
        pin_qn -= pin_qh, pin_qh = 0;
    }
    if (pin_qn == pin_qcap) {
        pin_qcap = pin_qcap ? pin_qcap * 2 : 4096;
        if (!(pin_q = realloc(pin_q, sizeof *pin_q * pin_qcap))) rt_fatal("nvn: out of memory (texture pin queue)");
    }
    pin_q[pin_qn++] = (pin_due){p, at};
    p->queued = 1;
}

static void note_pin(cmd_t *o, const tex_t *t);

static const tex_t *tex_pin_get(const NVNtexture *nt) {
    const tex_t *t = (const tex_t *)nt;
    if (!t) return NULL;
    uint64_t serial = t->serial;
    if (!serial) rt_fatal("nvn: a recording references the uninitialized texture object %p", (const void *)t);
    uint64_t now = __atomic_load_n(&nsubmits, __ATOMIC_RELAXED);
    plat_mutex_lock_shared(&pin_lock);   // nvn_pins_sweep frees under the exclusive lock
    pin_t *r = nvn_hm_get(&pins, &serial);
    if (r) __atomic_store_n(&r->last_use, now, __ATOMIC_RELAXED);
    plat_mutex_unlock_shared(&pin_lock);
    if (r) return &r->t;
    plat_mutex_lock(&pin_lock);
    r = nvn_hm_get(&pins, &serial);
    if (!r) {
        if (!(r = malloc(sizeof *r))) rt_fatal("nvn: texture pin copy");
        *r = (pin_t){.t = *t};
        nvn_hm_put(&pins, &serial, r);
        pins_live++;
        pin_queue(r, now);   // freed like any other if no copy ever names it
    }
    __atomic_store_n(&r->last_use, now, __ATOMIC_RELAXED);
    plat_mutex_unlock(&pin_lock);
    return &r->t;
}

// The pin of nt for a record of the open recording in o, listed in its buffer's prefix (note_pin).
static const tex_t *tex_pin(cmd_t *o, const NVNtexture *nt) {
    const tex_t *t = tex_pin_get(nt);
    if (t) note_pin(o, t);
    return t;
}

// The pins an op's payload names, into out (at most 9); returns how many.
static int op_pins(uint32_t op, const void *a, const tex_t *out[9]) {
    int k = 0;
    switch (op) {
    case OP_SET_RT: {
        const p_set_rt *r = a;
        int n = r->n < 0 ? 0 : r->n > 8 ? 8 : r->n;
        for (int i = 0; i < n; i++)
            if (r->color[i]) out[k++] = r->color[i];
        if (r->depth) out[k++] = r->depth;
        break;
    }
    case OP_COPY_B2T: case OP_COPY_T2B:
        if (((const p_copy_b2t *)a)->tex) out[k++] = ((const p_copy_b2t *)a)->tex;
        break;
    case OP_CLEAR_TEX:
        if (((const p_clear_tex *)a)->tex) out[k++] = ((const p_clear_tex *)a)->tex;
        break;
    case OP_COPY_T2T: {
        const p_copy_t2t *c = a;
        if (c->src) out[k++] = c->src;
        if (c->dst) out[k++] = c->dst;
        break;
    }
    default: break;
    }
    return k;
}

static void pin_ref(const tex_t *t, int delta, uint64_t now) {
    pin_t *p = (pin_t *)(uintptr_t)t;
    if (delta > 0) {
        __atomic_add_fetch(&p->refs, 1, __ATOMIC_RELAXED);
        return;
    }
    __atomic_store_n(&p->last_use, now, __ATOMIC_RELAXED);
    if (__atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL)) return;
    plat_mutex_lock(&pin_lock);
    pin_queue(p, now);
    plat_mutex_unlock(&pin_lock);
}

// Counts (delta 1) or uncounts (-1) the pins named by the copy at handle, OP_BEGIN to OP_END; returns how many.
static uint32_t pin_refs(const uint8_t *copy, int delta) {
    uint64_t now = __atomic_load_n(&nsubmits, __ATOMIC_RELAXED);
    uint32_t n = 0;
    for (const op_hdr *h = nvn_op_first((uintptr_t)copy); h->op != OP_END; h = nvn_op_next(h)) {
        const tex_t *p[9];
        int k = op_pins(h->op, nvn_op_payload(h), p);
        for (int i = 0; i < k; i++) pin_ref(p[i], delta, now);
        n += (uint32_t)k;
    }
    return n;
}

void nvn_pins_sweep(uint64_t now, const tex_t *const *keep, int nkeep) {
    if (now < NVN_PIN_AGE) return;
    static int on = -1;
    if (on < 0) {
        const char *s = cfg_get("TOTK_PIN_SWEEP");
        on = !(s && !strcmp(s, "0"));
        if (!on) rt_log("nvn: TOTK_PIN_SWEEP=0: texture pin copies are kept for the run (the heap grows in long sessions)");
    }
    if (!on) return;
    static uint64_t logged;
    plat_mutex_lock(&pin_lock);
    for (int budget = 1024; pin_qh < pin_qn && budget; budget--) {
        pin_due d = pin_q[pin_qh];
        if (d.at > now - NVN_PIN_AGE) break;
        pin_qh++;
        pin_t *p = d.p;
        p->queued = 0;
        if (__atomic_load_n(&p->refs, __ATOMIC_ACQUIRE)) continue;   // queued again when the count falls to 0
        uint64_t last = __atomic_load_n(&p->last_use, __ATOMIC_RELAXED);
        int kept = 0;
        for (int i = 0; i < nkeep; i++) kept |= keep[i] == &p->t;
        if (kept || last > now - NVN_PIN_AGE) {
            pin_queue(p, kept ? now : last);
            continue;
        }
        nvn_hm_del(&pins, &p->t.serial);
        free(p);
        pins_live--, pins_freed++;
    }
    if (now / 7200 != logged) {
        logged = now / 7200;
        rt_log("nvn: texture pins at submit %llu: %llu live, %llu waiting, %llu freed so far", (unsigned long long)now,
               (unsigned long long)pins_live, (unsigned long long)(pin_qn - pin_qh), (unsigned long long)pins_freed);
    }
    plat_mutex_unlock(&pin_lock);
}

void nvn_pins_stats(uint64_t *live, uint64_t *freed) {
    plat_mutex_lock_shared(&pin_lock);
    *live = pins_live, *freed = pins_freed;
    plat_mutex_unlock_shared(&pin_lock);
}

// ---------- recording copies
// A handle points just past a copy_prefix. Copies are indexed by the 64 KiB block of their control-memory position, so
// adding control memory again releases the copies recorded there without walking every live copy. 4 KiB pages cost
// about 500 lookups per AddControlMemory under the exclusive lock (1.4.3 field: 53 calls a frame, 2 MiB ranges, 3.7
// copies per page), and the recording threads waited on it.
// deferred: 0 unknown, 1 none, 2 some; pins: texture pins its ops name, counted in their refs; pin_v: those pins, in
// record order, or NULL (then pin_refs walks the records); cap: bytes of the block from the large-block cache
// (copy_block), 0 for a plain heap block; hw: the recording's command memory as the real driver counts it (set at
// EndRecording), what CopyCommands of it adds to nvnCommandBufferGetCommandMemoryUsed (inline_recording); fsync: 1 when
// the records, or those of a copy they call, hold an OP_FENCE_SYNC, which nvn_submit_fence_syncs signals at submit
// Calls by reference (TOTK_NVN_CALL, inline_recording): state counts the OP_CALL records in other copies and
// in open recordings that name this copy (2 each) and holds bit 0 once the game released it; kid_v lists the copies
// its own OP_CALL records name (nkids of them, one count each); def_v: the offsets of its own deferred binds (ndef);
// dkid_v: its OP_CALL records that name a copy with deferred binds (ndkids); res_at: the newest submission that reads
// the handles its deferred binds hold now, or that reads it through a call while it names copies with deferred binds
// (nvn_submit_prepare_async, resolve_shared).
// (dkid_t and copy_prefix are defined above the recording buffer, which carries a prefix too)
typedef struct { uintptr_t lo, hi; uint8_t *copy; } cmd_copy;
typedef struct { cmd_copy *v; uint32_t n, cap; } copy_bucket;
static PlatMutex copy_lock = PLAT_MUTEX_INIT;    // guards the two maps, the counters and every prefix
static nvn_hmap copy_pages = {.klen = 8};   // page -> copy_bucket
static nvn_hmap copy_live = {.klen = 8};    // handle -> copy_prefix of a live copy
static uint64_t ncopies, copy_bytes, copies_made;
static int fence_sync_recorded;   // 1 once nvnCommandBufferFenceSync recorded an op (never set by the game itself)
// Asynchronous replay: copies released by the game, and per-submission clones, that a queued submission still reads.
// Freed once gfx_replayed_submits() reaches their used_at. Empty with synchronous replay.
static copy_prefix **zombies;
static uint32_t nzombies, cap_zombies;

static void zombie_add(copy_prefix *pf) {   // copy_lock held
    if (nzombies == cap_zombies) {
        cap_zombies = cap_zombies ? cap_zombies * 2 : 64;
        zombies = realloc(zombies, sizeof *zombies * cap_zombies);
        if (!zombies) rt_fatal("nvn: out of memory (released recording copies)");
    }
    zombies[nzombies++] = pf;
}

// Blocks of large copies are kept for the next ones. 1.4.3 makes two copies of 0.5-2 MiB a frame (the per-frame
// recordings that inline many others); above the heap's direct-allocation threshold (about 512 KiB) every malloc mapped
// fresh zero-filled pages and every free unmapped them, so the copy ran at half the speed of a small one and the free
// held copy_lock for tens of microseconds. A block is reused for a copy that fills at least half of it.
#define BIG_MIN ((size_t)256 << 10)        // smaller blocks come from the heap and go back to it
#define BIG_MAX ((size_t)16 << 20)         // a larger block is never kept
#define BIG_SLOTS 8
static PlatMutex big_lock = PLAT_MUTEX_INIT;   // lock order: copy_lock, then big_lock
static struct { copy_prefix *p; size_t cap; } big[BIG_SLOTS];
static uint32_t nbig;

static void big_drain(void) {
    plat_mutex_lock(&big_lock);
    while (nbig) free(big[--nbig].p);
    plat_mutex_unlock(&big_lock);
}

// A block for a copy of n bytes after its prefix; prefix.cap says where it goes back (copy_block_free). wait: a failed
// allocation waits for memory (realloc_wait), which is never done with a lock held; else it ends the game at once.
static copy_prefix *copy_block(size_t n, int wait, const char *what) {
    size_t total = sizeof(copy_prefix) + n, cap = 0;
    copy_prefix *pf = NULL;
    if (total >= BIG_MIN && total <= BIG_MAX) {
        plat_mutex_lock(&big_lock);
        uint32_t best = BIG_SLOTS;
        for (uint32_t i = 0; i < nbig; i++)
            if (big[i].cap >= total && big[i].cap / 2 <= total && (best == BIG_SLOTS || big[i].cap < big[best].cap))
                best = i;
        if (best < BIG_SLOTS) {
            pf = big[best].p;
            cap = big[best].cap;
            big[best] = big[--nbig];
        }
        plat_mutex_unlock(&big_lock);
        if (!pf) cap = (total + total / 8 + 0xffff) & ~(size_t)0xffff;   // room for the next frame's slightly larger one
    }
    if (!pf) pf = wait ? realloc_wait(NULL, cap ? cap : total, what) : malloc(cap ? cap : total);
    if (!pf) rt_fatal("nvn: %s allocation (%zu bytes)", what, cap ? cap : total);
    pf->cap = cap;
    return pf;
}

static void copy_block_free(copy_prefix *pf) {
    size_t cap = pf->cap;
    if (cap) {
        plat_mutex_lock(&big_lock);
        if (nbig < BIG_SLOTS) {
            big[nbig].p = pf;
            big[nbig++].cap = cap;
            pf = NULL;
        } else {
            uint32_t small = 0;
            for (uint32_t i = 1; i < nbig; i++)
                if (big[i].cap < big[small].cap) small = i;
            if (big[small].cap < cap) {
                copy_prefix *drop = big[small].p;
                big[small].p = pf;
                big[small].cap = cap;
                pf = drop;
            }
        }
        plat_mutex_unlock(&big_lock);
    }
    free(pf);
}

// Counts (delta 1) or uncounts (-1) the pins a copy names: from its list, without walking its records. A copy of a
// per-frame recording is 0.5-2 MiB of records naming a few pins; walking them again at every free cost about 40 us.
static void copy_pins_ref(const copy_prefix *pf, int delta) {
    if (!pf->pins) return;
    if (!pf->pin_v) {
        pin_refs((const uint8_t *)(pf + 1), delta);
        return;
    }
    uint64_t now = __atomic_load_n(&nsubmits, __ATOMIC_RELAXED);
    for (uint64_t i = 0; i < pf->pins; i++) pin_ref(pf->pin_v[i], delta, now);
}

// A copy in no map and no zombie list: nothing else can reach it, so copy_lock need not be held.
static void copy_free(copy_prefix *pf);
static void zombie_add(copy_prefix *pf);

// One OP_CALL record that named k is gone. The last one of a copy the game released frees it, or leaves it to the
// submissions that still read it. copy_lock not held.
static int64_t kept_refs;   // copies the game released (or clones) that OP_CALL records still name

static void kid_unref(copy_prefix *k) {
    if (__atomic_sub_fetch(&k->state, 2, __ATOMIC_ACQ_REL) != 1) return;
    __atomic_fetch_sub(&kept_refs, 1, __ATOMIC_RELAXED);
    plat_mutex_lock(&copy_lock);
    int queued = k->used_at > gfx_replayed_submits();
    if (queued) zombie_add(k);
    plat_mutex_unlock(&copy_lock);
    if (!queued) copy_free(k);
}

static void copy_free(copy_prefix *pf) {
    copy_pins_ref(pf, -1);
    free((void *)pf->pin_v);
    void **kids = pf->kid_v;
    uint64_t nkids = pf->nkids;
    free(pf->def_v), free(pf->dkid_v);
    if (pf->recbuf) {   // a recording buffer given away at EndRecording: the next recording takes it
        size_t cap = (size_t)pf->recbuf;
        memset(pf, 0, sizeof *pf);
        pool_put((uint8_t *)(pf + 1), cap);
    } else {
        copy_block_free(pf);
    }
    for (uint64_t i = 0; i < nkids; i++) kid_unref(kids[i]);
    free(kids);
}

// Copies retired under copy_lock and freed after it is released: the frees, the unpinning walk over their records and
// the large blocks kept the lock exclusive for most of release_copies, while the game's other recording threads waited
// for it in copy_touch (1.4.3 town: three threads, 0.6 ms a frame of waiting).
typedef struct { copy_prefix **v; uint32_t n, cap; } dead_list;
static __thread dead_list dead;

static void dead_add(copy_prefix *pf) {
    if (dead.n == dead.cap) {
        uint32_t cap = dead.cap ? dead.cap * 2 : 256;
        copy_prefix **v = realloc(dead.v, sizeof *v * cap);
        if (!v) { if (!pf->nkids) copy_free(pf); return; }   // out of memory: free it here, under the lock, as before
                                                              // (one that names other copies stays: kid_unref locks)
        dead.v = v;
        dead.cap = cap;
    }
    dead.v[dead.n++] = pf;
}

static void dead_free(void) {   // copy_lock not held
    for (uint32_t i = 0; i < dead.n; i++) copy_free(dead.v[i]);
    dead.n = 0;
}

static void zombie_sweep(uint64_t replayed) {   // copy_lock held; the swept copies go to the dead list
    for (uint32_t i = 0; i < nzombies;) {
        if (zombies[i]->used_at <= replayed) {
            dead_add(zombies[i]);
            zombies[i] = zombies[--nzombies];
        } else {
            i++;
        }
    }
}

static copy_prefix *copy_find(uint64_t handle) {   // copy_lock held
    return nvn_hm_get(&copy_live, &handle);
}

int nvn_handle_live(uint64_t handle) {
    plat_mutex_lock_shared(&copy_lock);
    int live = copy_find(handle) != NULL;
    plat_mutex_unlock_shared(&copy_lock);
    return live;
}

uint64_t nvn_handle_bytes(uint64_t handle) {
    plat_mutex_lock_shared(&copy_lock);
    const copy_prefix *pf = copy_find(handle);
    uint64_t n = pf ? pf->bytes : 0;
    plat_mutex_unlock_shared(&copy_lock);
    return n;
}

// Marks a live copy used by submission now; its size from OP_BEGIN to OP_END inclusive.
// The handle points just past its prefix, and release_copies clears the magic under the exclusive lock when it takes a
// copy out of copy_live, so under the shared lock a prefix with the magic is a live copy: the hash lookup (a cache miss
// per call, about 1% of the game threads' time on the Deck) is only the fallback that names a bad handle.
static uint64_t copy_touch(uint64_t handle, uint64_t now, const char *what, uint64_t *hw) {
    plat_mutex_lock_shared(&copy_lock);
    copy_prefix *pf = (copy_prefix *)(uintptr_t)handle - 1;
    if (!handle || pf->magic != COPY_MAGIC) pf = copy_find(handle);
    uint64_t bytes = 0;
    if (pf) __atomic_store_n(&pf->used_at, now, __ATOMIC_RELAXED), bytes = pf->bytes;
    if (hw) *hw = pf ? pf->hw : 0;
    plat_mutex_unlock_shared(&copy_lock);
    if (!pf) rt_fatal("nvn: %s: command handle %#llx is not a live recording (released or never recorded)", what,
                      (unsigned long long)handle);
    return bytes;
}

// One more element in a list of the recording buffer's prefix (the lists live there while a command buffer records).
static void *list_room(void *v, uint32_t *cap, uint64_t n, size_t elem, uint32_t first) {
    if (n < *cap) return v;
    uint32_t c = *cap ? *cap * 2 : first;
    v = realloc(v, elem * c);
    if (!v) rt_fatal("nvn: out of memory (lists of a recording)");
    *cap = c;
    return v;
}

static inline copy_prefix *rec_pf(const cmd_t *o) { return (copy_prefix *)o->seg - 1; }

// A record of the open recording in o names the pin t / is a deferred bind at offset off / is an OP_CALL of k.
// Called after the record's cmd_alloc (the buffer, and its prefix, may have moved there).
static void note_pin(cmd_t *o, const tex_t *t) {
    copy_prefix *pf = rec_pf(o);
    pf->pin_v = list_room((void *)pf->pin_v, &pf->pin_cap, pf->pins, sizeof *pf->pin_v, 16);
    pf->pin_v[pf->pins++] = t;
}

static void note_def(cmd_t *o, size_t off) {
    copy_prefix *pf = rec_pf(o);
    pf->def_v = list_room(pf->def_v, &pf->def_cap, pf->ndef, sizeof *pf->def_v, 64);
    pf->def_v[pf->ndef++] = (uint32_t)off;
}

static uint32_t note_kid(cmd_t *o, copy_prefix *k) {
    copy_prefix *pf = rec_pf(o);
    pf->kid_v = list_room(pf->kid_v, &pf->kid_cap, pf->nkids, sizeof *pf->kid_v, 16);
    pf->kid_v[pf->nkids] = k;
    pf->fsync |= k->fsync;   // k's prefix does not change after its EndRecording
    return (uint32_t)pf->nkids++;
}

static void note_dkid(cmd_t *o, copy_prefix *k, size_t off, uint32_t idx) {
    copy_prefix *pf = rec_pf(o);
    pf->dkid_v = list_room(pf->dkid_v, &pf->dkid_cap, pf->ndkids, sizeof *pf->dkid_v, 16);
    pf->dkid_v[pf->ndkids++] = (dkid_t){k, (uint32_t)off, idx};
}

// TOTK_NVN_LIST_CHECK=1 (a test): the lists made while recording against a walk over the finished records, which also
// checks every record header as EndRecording did before the lists were made at record time.
static void lists_check(const copy_prefix *pf, const uint8_t *p, size_t n) {
    uint64_t pins = 0, nkids = 0, ndef = 0, ndk = 0;
    for (size_t at = 0;;) {
        op_hdr h;
        memcpy(&h, p + at, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_ || h.op == OP_JUMP || at + h.size > n ||
            (!at && h.op != OP_BEGIN))
            rt_fatal("nvn: corrupt recording at %p (op %u size %u)", (const void *)(p + at), h.op, h.size);
        if (h.op == OP_CALL) {
            uint64_t t[2] = {0, 0};
            memcpy(t, p + at + 8, h.size >= 24 ? 16 : 8);
            copy_prefix *k = (copy_prefix *)(uintptr_t)t[0] - 1;
            if (t[1] & 1) {
                if (ndk >= pf->ndkids || pf->dkid_v[ndk].k != k || pf->dkid_v[ndk].off != at || pf->dkid_v[ndk].idx != nkids)
                    rt_fatal("nvn: list check: deferred call %llu at %zu", (unsigned long long)ndk, at);
                ndk++;
            }
            if (nkids >= pf->nkids || pf->kid_v[nkids] != k) rt_fatal("nvn: list check: call %llu at %zu", (unsigned long long)nkids, at);
            nkids++;
        } else if (h.op == OP_BIND_TEX || h.op == OP_BIND_IMG || h.op == OP_BIND_SAMP) {
            const p_bind_handles *b = (const p_bind_handles *)(const void *)(p + at + 8);
            if (b->src && b->n > 0 && b->n <= 32) {
                if (ndef >= pf->ndef || pf->def_v[ndef] != at) rt_fatal("nvn: list check: deferred bind %llu at %zu", (unsigned long long)ndef, at);
                ndef++;
            }
        }
        const tex_t *pinned[9];
        int k = op_pins(h.op, p + at + 8, pinned);
        for (int i = 0; i < k; i++) {
            if (pins >= pf->pins || pf->pin_v[pins] != pinned[i]) rt_fatal("nvn: list check: pin %llu at %zu (op %u)", (unsigned long long)pins, at, h.op);
            pins++;
        }
        at += h.size;
        if (h.op == OP_END) {
            if (at != n) rt_fatal("nvn: recording at %p ends at %zu of %zu bytes", (const void *)p, at, n);
            break;
        }
    }
    if (pins != pf->pins || nkids != pf->nkids || ndef != pf->ndef || ndk != pf->ndkids)
        rt_fatal("nvn: list check: %llu/%llu pins, %llu/%llu calls, %llu/%llu deferred binds, %llu/%llu deferred calls",
                 (unsigned long long)pins, (unsigned long long)pf->pins, (unsigned long long)nkids, (unsigned long long)pf->nkids,
                 (unsigned long long)ndef, (unsigned long long)pf->ndef, (unsigned long long)ndk, (unsigned long long)pf->ndkids);
}

// The default of TOTK_NVN_GIVE and TOTK_NVN_CALL: on only where their paths ran, the Linux build on a Steam Deck (the
// device profile, cfg.c). Elsewhere a switch is off until its path runs there.
static const char *ran_default(void) {
#ifdef _WIN32
    return "0";
#else
    return strcmp(cfg_device_profile(), "deck") ? "0" : "1";
#endif
}

// TOTK_NVN_GIVE (ran_default): EndRecording gives the recording buffer away as the copy, but only for a recording
// of GIVE_MIN bytes or more that also fills a quarter of its buffer; a smaller one is copied into a block of its own
// size and the command buffer keeps its buffer (most recordings are a few hundred bytes and live for minutes: a
// 64 KiB buffer each would take gigabytes).
#define GIVE_MIN (16u << 10)
static int give_on(void) {
    static int v = -1;
    if (v < 0) {
        v = rt_knob("NVN_GIVE", ran_default());
        rt_log("nvn: EndRecording gives a large recording buffer away as the copy: %s (TOTK_NVN_GIVE)",
               v == 2 ? "ab" : v ? "on" : "off");
    }
    return RT_KNOB_ON(v);
}

// The finished recording of o, OP_BEGIN to OP_END (the recording buffer is one block: recording emits no OP_JUMP), as a
// recording copy: the buffer itself with its prefix (given away; o takes another buffer at its next record), or one
// host block of the recording's size (copy_block) with copies of the lists. Its pins are counted here. The records are
// not walked: the code of this file made them, and the lists were made with them.
static uint8_t *end_recording(cmd_t *o) {
    static int check = -1;
    if (check < 0) check = rt_knob("NVN_LIST_CHECK", "0");
    copy_prefix *rp = rec_pf(o);
    size_t n = (size_t)(o->cur - o->seg), cap = (size_t)(o->end - o->seg);
    if (check) lists_check(rp, o->seg, n);
    for (uint64_t i = 0; i < rp->pins; i++) pin_ref(rp->pin_v[i], 1, 0);
    rec_hint_set(o, n);
    int deferred = rp->ndef || rp->ndkids ? 2 : 1;
    if (n >= GIVE_MIN && n * 4 >= cap && give_on()) {
        rp->magic = COPY_MAGIC;
        rp->bytes = n;
        rp->made_at = rp->used_at = rp->res_at = 0;
        rp->deferred = (uint64_t)deferred;
        rp->cap = 0;
        rp->hw = 0;
        rp->state = 0;
        rp->recbuf = cap;
        uint8_t *copy = o->seg;
        o->seg = o->cur = o->end = NULL;
        return copy;
    }
    copy_prefix *block = copy_block(n, 1, "command copy");
    memcpy(block + 1, o->seg, n);
    const tex_t **pin_v = NULL;
    void **kid_v = NULL;
    uint32_t *def_v = NULL;
    dkid_t *dkid_v = NULL;
    if (rp->pins) {
        if (!(pin_v = malloc(sizeof *pin_v * rp->pins))) rt_fatal("nvn: out of memory (pins of a copy)");
        memcpy((void *)pin_v, (const void *)rp->pin_v, sizeof *pin_v * rp->pins);
    }
    if (rp->nkids) {
        if (!(kid_v = malloc(sizeof *kid_v * rp->nkids))) rt_fatal("nvn: out of memory (called recordings of a copy)");
        memcpy(kid_v, rp->kid_v, sizeof *kid_v * rp->nkids);
    }
    if (rp->ndef) {
        if (!(def_v = malloc(sizeof *def_v * rp->ndef))) rt_fatal("nvn: out of memory (deferred binds of a copy)");
        memcpy(def_v, rp->def_v, sizeof *def_v * rp->ndef);
    }
    if (rp->ndkids) {
        if (!(dkid_v = malloc(sizeof *dkid_v * rp->ndkids))) rt_fatal("nvn: out of memory (called recordings of a copy)");
        memcpy(dkid_v, rp->dkid_v, sizeof *dkid_v * rp->ndkids);
    }
    *block = (copy_prefix){.magic = COPY_MAGIC, .bytes = n, .pins = rp->pins, .cap = block->cap, .pin_v = pin_v,
                           .deferred = (uint64_t)deferred, .nkids = rp->nkids, .kid_v = kid_v, .ndef = rp->ndef, .def_v = def_v,
                           .ndkids = rp->ndkids, .dkid_v = dkid_v, .fsync = rp->fsync, .pin_cap = (uint32_t)rp->pins, .kid_cap = (uint32_t)rp->nkids,
                           .def_cap = (uint32_t)rp->ndef, .dkid_cap = (uint32_t)rp->ndkids};
    rp->pins = rp->nkids = rp->ndef = rp->ndkids = rp->fsync = 0;   // the copy counts the called copies now
    return (uint8_t *)(block + 1);
}

static void copy_add(uintptr_t lo, uintptr_t hi, uint8_t *copy) {
    copy_prefix *pf = (copy_prefix *)copy - 1;
    uint64_t handle = (uintptr_t)copy;
    uintptr_t page = lo >> COPY_PAGE_SHIFT;
    plat_mutex_lock(&copy_lock);
    pf->made_at = nsubmits;
    copy_bucket *b = nvn_hm_get(&copy_pages, &page);
    if (!b) {
        b = calloc(1, sizeof *b);
        if (!b) rt_fatal("nvn: command copy index");
        nvn_hm_put(&copy_pages, &page, b);
    }
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 8;
        b->v = realloc(b->v, b->cap * sizeof *b->v);
        if (!b->v) rt_fatal("nvn: command copy index");
    }
    b->v[b->n++] = (cmd_copy){lo, hi, copy};
    nvn_hm_put(&copy_live, &handle, pf);
    ncopies++;
    copy_bytes += pf->bytes;
    if (++copies_made % 20000 == 0)
        NVN_LOG("recording copies: %llu made, %llu live, %.1f MiB, submit %llu", (unsigned long long)copies_made,
                (unsigned long long)ncopies, copy_bytes / 1048576.0, (unsigned long long)nsubmits);
    plat_mutex_unlock(&copy_lock);
}

// Adding control memory again retires every recording copy made in it (the driver's handle lived there).
static void release_copies(uintptr_t lo, uintptr_t hi) {
    if (hi <= lo) return;
    uint64_t replayed = gfx_replayed_submits();
    plat_mutex_lock(&copy_lock);
    if (nzombies) zombie_sweep(replayed);
    for (uintptr_t pg = lo >> COPY_PAGE_SHIFT; pg <= (hi - 1) >> COPY_PAGE_SHIFT; pg++) {
        copy_bucket *b = nvn_hm_get(&copy_pages, &pg);
        if (!b) continue;
        for (uint32_t i = 0; i < b->n;) {
            cmd_copy *k = &b->v[i];
            if (!(k->lo < hi && lo < k->hi)) { i++; continue; }
            uint64_t handle = (uintptr_t)k->copy;
            copy_prefix *pf = (copy_prefix *)k->copy - 1;
            nvn_hm_del(&copy_live, &handle);
            ncopies--;
            copy_bytes -= pf->bytes;
            pf->magic = 0;
            // Named by OP_CALL records still: it stays until the last of them goes (kid_unref).
            if (__atomic_fetch_or(&pf->state, 1, __ATOMIC_ACQ_REL) >> 1) __atomic_fetch_add(&kept_refs, 1, __ATOMIC_RELAXED);
            else if (pf->used_at > replayed) zombie_add(pf);   // a queued submission still reads it
            else dead_add(pf);
            b->v[i] = b->v[--b->n];
        }
        if (!b->n) {
            nvn_hm_del(&copy_pages, &pg);
            free(b->v);
            free(b);
        }
    }
    plat_mutex_unlock(&copy_lock);
    dead_free();
}

// ---------- CommandBuffer: object and memory
static void recording_drop_calls(cmd_t *o);
// NVNboolean nvnCommandBufferInitialize(NVNcommandBuffer *cmd, NVNdevice *device): the inline fast-path fields are kept
// at ~0 / 0 so NVN 55 inline code never finds room and calls the API (nvn_obj.h cmd_t)
NVN_API(nvnCommandBufferInitialize) {
    cmd_t *o = cmd_of(c);
    // Initialize over an object that was never finalized drops its recording buffer (seg lives in host memory). Logged
    // only: the old bytes may be a copy whose buffer another object owns, so freeing it here is not safe.
    static uint32_t reinits;
    if (o->magic == CMD_MAGIC && o->seg && __atomic_fetch_add(&reinits, 1, __ATOMIC_RELAXED) < 8)
        rt_log("nvn: Initialize over the command buffer %p, which was not finalized; its %zu-byte recording buffer %p "
               "is dropped", (void *)o, (size_t)(o->end - o->seg), (void *)o->seg);
    memset(o, 0, sizeof *o);
    o->magic = CMD_MAGIC;
    o->dev = arg_p(c, 1);
    o->inline_cur = ~0ull;
    o->inline_end = 0;
    cb_set_disabled(o, CB_NO_COMMAND | CB_NO_CONTROL, 0);
    ret_x(c, 1);
}
// void nvnCommandBufferFinalize(NVNcommandBuffer *cmd): handles outlive their recorder
NVN_API(nvnCommandBufferFinalize) {
    cmd_t *o = cmd_of(c);
    if (o->recording) recording_drop_calls(o);
    pool_put(o->seg, (size_t)(o->end - o->seg));
    o->seg = o->cur = o->end = NULL;
    o->magic = 0;
    cb_set_disabled(o, CB_NO_COMMAND | CB_NO_CONTROL, 0);
}
// void nvnCommandBufferSetMemoryCallback(NVNcommandBuffer *cmd, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC callback)
NVN_API(nvnCommandBufferSetMemoryCallback) { cmd_of(c)->cb = arg_p(c, 1); }
// void nvnCommandBufferSetMemoryCallbackData(NVNcommandBuffer *cmd, void *data)
NVN_API(nvnCommandBufferSetMemoryCallbackData) { cmd_of(c)->cb_data = arg_p(c, 1); }
// PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC nvnCommandBufferGetMemoryCallback(const NVNcommandBuffer *cmd)
NVN_API(nvnCommandBufferGetMemoryCallback) { ret_x(c, (uintptr_t)cmd_of(c)->cb); }
// void *nvnCommandBufferGetMemoryCallbackData(const NVNcommandBuffer *cmd) (nvn.h declares it void)
NVN_API(nvnCommandBufferGetMemoryCallbackData) { ret_x(c, (uintptr_t)cmd_of(c)->cb_data); }
// void nvnCommandBufferSetCommandMemoryCallbackEnabled / SetControlMemoryCallbackEnabled(NVNcommandBuffer *cmd,
// NVNboolean enabled) (NVN 55, no prototype in nvn.h)
NVN_API(nvnCommandBufferSetCommandMemoryCallbackEnabled) { cb_set_disabled(cmd_of(c), CB_NO_COMMAND, !nvn_arg_bool(c, 1)); }
NVN_API(nvnCommandBufferSetControlMemoryCallbackEnabled) { cb_set_disabled(cmd_of(c), CB_NO_CONTROL, !nvn_arg_bool(c, 1)); }
// void nvnCommandBufferAddCommandMemory(NVNcommandBuffer *cmd, const NVNmemoryPool *pool, ptrdiff_t offset, size_t size):
// accounting only; command memory can be shared by independent recordings with distinct control memory
NVN_API(nvnCommandBufferAddCommandMemory) {
    cmd_t *o = cmd_of(c);
    const pool_t *p = arg_p(c, 1);
    o->cmd_base = (uintptr_t)(p ? p->mem : NULL) + (uintptr_t)arg_x(c, 2);
    o->game_size = arg_x(c, 3);
    // Memory added while recording (the out-of-memory callback): hw_start moves with hw_used, so hw_used - hw_start
    // stays the whole recording's size at EndRecording (unsigned wrap-around cancels out).
    if (o->recording) o->hw_start -= o->hw_used;
    o->cmd_used = 0;
    o->hw_used = 0;
}
// void nvnCommandBufferAddControlMemory(NVNcommandBuffer *cmd, void *memory, size_t size): the SDK encodes handles in
// control memory (sdk+0x444530), so reusing that storage retires the copies recorded there
NVN_API(nvnCommandBufferAddControlMemory) {
    cmd_t *o = cmd_of(c);
    void *m = arg_p(c, 1);
    size_t n = arg_x(c, 2);
    release_copies((uintptr_t)m, (uintptr_t)m + n);
    o->ctl = m;
    o->ctl_size = n;
    o->ctl_used = 0;
}
// size_t nvnCommandBufferGet{Control,Command}Memory{Size,Used,Free}(const NVNcommandBuffer *cmd): usage is bounded by
// the memory the game gave, so the game never sees more used than it provided
static size_t min_size(size_t a, size_t b) { return a < b ? a : b; }
NVN_API(nvnCommandBufferGetControlMemorySize) { ret_x(c, cmd_of(c)->ctl_size); }
NVN_API(nvnCommandBufferGetControlMemoryUsed) { const cmd_t *o = cmd_of(c); ret_x(c, min_size(o->ctl_used, o->ctl_size)); }
NVN_API(nvnCommandBufferGetControlMemoryFree) { const cmd_t *o = cmd_of(c); ret_x(c, o->ctl_size - min_size(o->ctl_used, o->ctl_size)); }
NVN_API(nvnCommandBufferGetCommandMemorySize) { ret_x(c, cmd_of(c)->game_size); }
NVN_API(nvnCommandBufferGetCommandMemoryUsed) { const cmd_t *o = cmd_of(c); ret_x(c, min_size(o->hw_used, o->game_size)); }
NVN_API(nvnCommandBufferGetCommandMemoryFree) { const cmd_t *o = cmd_of(c); ret_x(c, o->game_size - min_size(o->hw_used, o->game_size)); }

// ---------- CommandBuffer: recording
// void nvnCommandBufferBeginRecording(NVNcommandBuffer *cmd)
NVN_API(nvnCommandBufferBeginRecording) {
    cmd_t *o = cmd_of(c);
    if (o->recording) {
        rt_log("nvn: BeginRecording on command buffer %p while it records", (void *)o);
        recording_drop_calls(o);
    }
    o->recording = 1;
    o->cur = o->seg;   // the previous recording was copied at EndRecording, or went with its buffer
    if (o->seg) {
        copy_prefix *pf = rec_pf(o);
        pf->pins = pf->nkids = pf->ndef = pf->ndkids = pf->fsync = 0;
    }
    o->rec_start = o->cmd_used;
    put_hdr(cmd_alloc(o, 8), OP_BEGIN, 8);
    o->hw_used -= 8;   // BEGIN and END have no hardware equivalent
    o->hw_start = o->hw_used;
    if (!o->game_size) memory_event(c, o, NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY, MIN_COMMAND_SIZE);
}
// NVNcommandHandle nvnCommandBufferEndRecording(NVNcommandBuffer *cmd)
NVN_API(nvnCommandBufferEndRecording) {
    cmd_t *o = cmd_of(c);
    if (!o->recording || !o->seg) rt_fatal("nvn: EndRecording on command buffer %p, which does not record", (void *)o);
    if (!o->ctl_size) memory_event(c, o, NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_CONTROL_MEMORY, MIN_CONTROL_SIZE);
    put_hdr(cmd_alloc(o, 8), OP_END, 8);
    o->hw_used -= 8;
    o->recording = 0;
    uint8_t *copy = end_recording(o);   // o->seg is NULL after it when the buffer went with the copy
    // Finalizing the recorder does not retire its handle; only reusing the control memory it was recorded in does.
    uintptr_t lo = (uintptr_t)o->ctl + (o->ctl_used < o->ctl_size ? o->ctl_used : 0);
    uintptr_t hi = o->ctl ? lo + 1 : 0;
    o->ctl_used += CTL_PER_RECORDING;
    ((copy_prefix *)copy - 1)->hw = o->hw_used - o->hw_start;   // BEGIN and END excluded; what a copy of it adds
    copy_add(lo, hi, copy);
    ret_x(c, (uintptr_t)copy);
}
// NVNboolean nvnCommandBufferIsRecording(const NVNcommandBuffer *cmd)
NVN_API(nvnCommandBufferIsRecording) { ret_x(c, cmd_of(c)->recording != 0); }

// The called recording is copied into the caller at the call position: the game recycles command memory in small steps,
// so a handle may be gone by the time the caller is submitted. Deferred binds stay deferred. A copy is OP_BEGIN, its
// records and OP_END (end_recording made the copy from records of this file), so the records go over as one block; cmd_used grows by the same
// bytes as record by record.
// nvnCommandBufferGetCommandMemoryUsed grows as on the real driver: a call by nothing (the driver puts a call in
// control memory, nvnCommandBufferCallCommands) and a copy by the copied recording's hardware size (copy_prefix.hw), not by our record bytes. The
// game measures small command sets it records and sizes renderer memory pools from them: MacNativeGames commit 6909dbb7
// measured one recording with two uniform buffer binds and one CallCommands at 40 bytes on the original driver
// (Ryujinx, 1.0.0) and 88 in their port, whose larger sizes doubled a renderer's pool and ran its private heap dry
// (NULL write crash near Zora's Domain). 1.4.3 calls in recordings it queries every frame (copies were not seen).
// Calls by reference (TOTK_NVN_CALL): 1.4.3 records about 70 command sets a frame and calls 2,000-3,000,
// most of them recorded many frames ago, so the copies above moved 2 MiB a frame into the callers and their copies
// (Lookout Landing: 1.1-1.7 ms a frame on the game's threads with the walks over them). A called recording does not
// change after EndRecording, except for the handles of its deferred binds, so the caller takes an OP_CALL record that
// names it instead (the replay follows it: vk_exec.c walk, vk_usnap.c collect), and a count in the callee
// (copy_prefix.state) keeps it until every record that names it is gone, also after the game released its handle.
// One with deferred binds, its own or below it, is named with flag bit 0 and resolved at the submit: in place, or in
// a clone for that submission when a queued submission still reads it (resolve_deferred, resolve_shared). One
// smaller than CALL_MIN bytes is copied: the replay reads a small one faster from the caller's stream than from its
// own block somewhere in the heap (with every call by reference, 1,200 of them a frame cost the replay thread more
// than their copies cost the game's threads).
#define CALL_MIN 4096
static int call_on(void) {
    static int v = -1;
    // The chunked replay cuts a submission at its top level only and names its jobs by record address, so a submission
    // made of a few calls would record on one thread, and a recording called twice has one address for two jobs: by
    // reference only with the sequential replay, whatever the switch says.
    if (v < 0) {
        int chunked = cfg_int("TOTK_REPLAY_WORKERS") != 0;
        v = chunked ? 0 : rt_knob("NVN_CALL", ran_default());
        rt_log("nvn: CallCommands of a large recording is a call by reference: %s (TOTK_NVN_CALL%s)",
               v == 2 ? "ab" : v ? "on" : "off", chunked ? ", always off with replay_workers" : "");
    }
    return RT_KNOB_ON(v);
}

// The OP_CALL records of the unfinished recording in o are dropped with it.
static void recording_drop_calls(cmd_t *o) {
    if (!o->seg) return;
    copy_prefix *pf = rec_pf(o);
    for (uint64_t i = 0; i < pf->nkids; i++) kid_unref(pf->kid_v[i]);
    pf->pins = pf->nkids = pf->ndef = pf->ndkids = pf->fsync = 0;
}

static void inline_recording(cmd_t *o, uint64_t handle, int call) {
    const char *what = call ? "CallCommands" : "CopyCommands";
    if (!handle) rt_fatal("nvn: %s with a null handle", what);
    uint64_t hw = 0, now = __atomic_load_n(&nsubmits, __ATOMIC_RELAXED), bytes = 0;
    int by_ref = 0;
    uint64_t flags = 0;   // of the OP_CALL record; bit 0: the named copy holds deferred binds
    // As copy_touch, and under the same shared lock (the copy cannot be released meanwhile): the count for an OP_CALL
    // record, or one more count in every copy that the copied records name.
    plat_mutex_lock_shared(&copy_lock);
    copy_prefix *pf = (copy_prefix *)(uintptr_t)handle - 1;
    if (pf->magic != COPY_MAGIC) pf = copy_find(handle);
    if (pf) {
        __atomic_store_n(&pf->used_at, now, __ATOMIC_RELAXED);
        bytes = pf->bytes, hw = pf->hw;
        // CopyCommands and a small called recording are copied, with the OP_CALL records they hold.
        by_ref = call && bytes >= CALL_MIN && call_on();
        flags = pf->ndef || pf->ndkids;
        if (by_ref) __atomic_add_fetch(&pf->state, 2, __ATOMIC_ACQ_REL);
        else if (bytes > 16)
            for (uint64_t i = 0; i < pf->nkids; i++) __atomic_add_fetch(&((copy_prefix *)pf->kid_v[i])->state, 2, __ATOMIC_ACQ_REL);
    }
    plat_mutex_unlock_shared(&copy_lock);
    if (!pf) rt_fatal("nvn: %s: command handle %#llx is not a live recording (released or never recorded)", what,
                      (unsigned long long)handle);
    if (bytes <= 16) return;
    if (by_ref) {
        uint8_t *r = cmd_alloc(o, 24);
        put_hdr(r, OP_CALL, 24);
        memcpy(r + 8, &handle, 8);
        memcpy(r + 16, &flags, 8);
        o->hw_used -= 24;   // a call takes no command memory on the console (see above)
        o->last_rec = 0;
        uint32_t idx = note_kid(o, pf);   // (counted above; the list is for the free and for the submit)
        if (flags) note_dkid(o, pf, (size_t)(r - o->seg), idx);
        return;
    }
    size_t body = (size_t)bytes - 16;
    cmd_reserve(o, body);
    memcpy(o->cur, (const uint8_t *)(uintptr_t)handle + 8, body);
    // The copied records' pins, deferred binds and calls join this recording's lists (pf's offsets count from its
    // OP_BEGIN, which is not copied). pf is read after the lock like its records above.
    if (pf->pins | pf->ndef | pf->nkids) {
        size_t base = (size_t)(o->cur - o->seg) - 8;
        if (pf->pins && !pf->pin_v) rt_fatal("nvn: %s: the copy %#llx has no pin list", what, (unsigned long long)handle);
        for (uint64_t i = 0; i < pf->pins; i++) note_pin(o, pf->pin_v[i]);
        for (uint64_t i = 0; i < pf->ndef; i++) note_def(o, base + pf->def_v[i]);
        uint32_t first = (uint32_t)rec_pf(o)->nkids;
        for (uint64_t i = 0; i < pf->nkids; i++) note_kid(o, pf->kid_v[i]);
        for (uint64_t i = 0; i < pf->ndkids; i++)
            note_dkid(o, pf->dkid_v[i].k, base + pf->dkid_v[i].off, first + pf->dkid_v[i].idx);
    }
    rec_pf(o)->fsync |= pf->fsync;
    o->cur += body;
    o->cmd_used += body;
    if (!call) o->hw_used += hw;
    o->last_rec = 0;
}
// void nvnCommandBuffer{Call,Copy}Commands(NVNcommandBuffer *cmd, int n, const NVNcommandHandle *handles)
// A call takes control memory instead: the game's inline copy of the driver's call (1.4.3 main+0x2d33a4, in the
// function that measures a renderer's command set) writes one 24-byte control entry per segment of the called handle,
// after an 8-byte block header. The game reads a buffer whose command and control usage are both 0 as empty
// (main+0x83f8c clears bit 0 of its flags), so a buffer that only called recordings must show control usage.
NVN_API(nvnCommandBufferCallCommands) {
    cmd_t *o = cmd_of(c);
    int n = (int)arg_x(c, 1);
    const uint64_t *h = arg_p(c, 2);
    for (int i = 0; i < n; i++) inline_recording(o, h[i], 1);
    if (n > 0) o->ctl_used += CTL_PER_CALL * (size_t)n;
}
NVN_API(nvnCommandBufferCopyCommands) {
    cmd_t *o = cmd_of(c);
    int n = (int)arg_x(c, 1);
    const uint64_t *h = arg_p(c, 2);
    for (int i = 0; i < n; i++) inline_recording(o, h[i], 0);
}

// ---------- CommandBuffer: commands with CPU-visible results (performed by the back end in GPU order)
// void nvnCommandBufferSignalEvent(NVNcommandBuffer *cmd, const NVNevent *event, NVNeventSignalMode mode,
// NVNeventSignalLocation location, int flags, uint32_t value)
NVN_API(nvnCommandBufferSignalEvent) {
    const ev_t *e = arg_p(c, 1);
    *REC(cmd_of(c), OP_SIGNAL_EVENT, p_signal_event) = (p_signal_event){e->val, (uint32_t)arg_x(c, 2), (uint32_t)arg_x(c, 5)};
}
// void nvnCommandBufferResetCounter(NVNcommandBuffer *cmd, NVNcounterType type)
NVN_API(nvnCommandBufferResetCounter) { REC(cmd_of(c), OP_RESET_COUNTER, p_reset_counter)->type = (uint32_t)arg_x(c, 1); }
// void nvnCommandBufferReportCounter(NVNcommandBuffer *cmd, NVNcounterType type, NVNbufferAddress address)
NVN_API(nvnCommandBufferReportCounter) {
    *REC(cmd_of(c), OP_REPORT_COUNTER, p_report_counter) = (p_report_counter){(uint32_t)arg_x(c, 1), arg_x(c, 2)};
}
// void nvnCommandBufferCopyBufferToBuffer(NVNcommandBuffer *cmd, NVNbufferAddress src, NVNbufferAddress dst,
// size_t size, int flags)
NVN_API(nvnCommandBufferCopyBufferToBuffer) {
    *REC(cmd_of(c), OP_COPY_B2B, p_copy_b2b) = (p_copy_b2b){arg_x(c, 1), arg_x(c, 2), arg_x(c, 3)};
}
// void nvnCommandBufferUpdateUniformBuffer(NVNcommandBuffer *cmd, NVNbufferAddress buffer, size_t buffer_size,
// ptrdiff_t offset, size_t size, const void *data): the data is copied at record time (the caller may change it
// afterwards) and written into the buffer at replay
NVN_API(nvnCommandBufferUpdateUniformBuffer) {
    uint64_t dst = arg_x(c, 1) + arg_x(c, 3), n = arg_x(c, 4);
    const void *data = arg_p(c, 5);
    if (n > (16u << 20)) rt_fatal("nvn: UpdateUniformBuffer of %#llx bytes", (unsigned long long)n);
    p_update_ub *r = rec(cmd_of(c), OP_UPDATE_UB, offsetof(p_update_ub, data) + n);
    r->dst = dst;
    r->size = n;
    memcpy(r->data, data, n);
}

// ---------- CommandBuffer: render state, payloads of nvn_obj.h
// void nvnCommandBufferSetRenderTargets(NVNcommandBuffer *cmd, int n, const NVNtexture *const *colors,
// const NVNtextureView *const *color_views, const NVNtexture *depth, const NVNtextureView *depth_view)
NVN_API(nvnCommandBufferSetRenderTargets) {
    int n = (int)arg_x(c, 1);
    const NVNtexture *const *ct = arg_p(c, 2);
    const NVNtextureView *const *cv = arg_p(c, 3);
    const NVNtexture *dt = arg_p(c, 4);
    const NVNtextureView *dv = arg_p(c, 5);
    if (n < 0 || n > 8) {
        rt_log("nvn: SetRenderTargets with %d colour targets", n);
        n = n < 0 ? 0 : 8;
    }
    cmd_t *cb = cmd_of(c);
    p_set_rt *r = REC(cb, OP_SET_RT, p_set_rt);
    r->n = n;
    for (int i = 0; i < n; i++) {
        r->color[i] = ct ? tex_pin(cb, ct[i]) : NULL;
        copy_view(&r->cview[i], &r->has_cview[i], cv ? cv[i] : NULL);
    }
    r->depth = tex_pin(cb, dt);
    copy_view(&r->dview, &r->has_dview, dv);
}
// void nvnCommandBufferClearColor(NVNcommandBuffer *cmd, int index, const float *rgba, int mask)
NVN_API(nvnCommandBufferClearColor) {
    p_clear_color *r = REC(cmd_of(c), OP_CLEAR_COLOR, p_clear_color);
    r->index = (int)arg_x(c, 1);
    r->mask = (int)arg_x(c, 3);
    memcpy(r->rgba, arg_p(c, 2), 16);
}
// void nvnCommandBufferClearDepthStencil(NVNcommandBuffer *cmd, float depth, NVNboolean depth_mask, int stencil,
// int stencil_mask)
NVN_API(nvnCommandBufferClearDepthStencil) {
    *REC(cmd_of(c), OP_CLEAR_DS, p_clear_ds) = (p_clear_ds){arg_s(c, 0), nvn_arg_bool(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3)};
}
static void set_viewports(cmd_t *o, int first, int n, const float *v) {
    if (first < 0 || n < 0 || first + n > 16) {
        rt_log("nvn: SetViewports first %d n %d", first, n);
        return;
    }
    p_viewports *r = REC(o, OP_VIEWPORTS, p_viewports);
    r->first = first;
    r->n = n;
    for (int i = 0; i < n; i++) memcpy(r->v[i], v + 4 * i, 16);
}
// void nvnCommandBufferSetViewports(NVNcommandBuffer *cmd, int first, int n, const float *xywh)
NVN_API(nvnCommandBufferSetViewports) { set_viewports(cmd_of(c), (int)arg_x(c, 1), (int)arg_x(c, 2), arg_p(c, 3)); }
// void nvnCommandBufferSetViewport(NVNcommandBuffer *cmd, int x, int y, int w, int h)
NVN_API(nvnCommandBufferSetViewport) {
    float v[4] = {(float)(int)arg_x(c, 1), (float)(int)arg_x(c, 2), (float)(int)arg_x(c, 3), (float)(int)arg_x(c, 4)};
    set_viewports(cmd_of(c), 0, 1, v);
}
static void set_scissor(cmd_t *o, int x, int y, int w, int h) { *REC(o, OP_SCISSOR, p_scissor) = (p_scissor){x, y, w, h}; }
// void nvnCommandBufferSetScissor(NVNcommandBuffer *cmd, int x, int y, int w, int h)
NVN_API(nvnCommandBufferSetScissor) {
    set_scissor(cmd_of(c), (int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3), (int)arg_x(c, 4));
}
// void nvnCommandBufferSetScissors(NVNcommandBuffer *cmd, int first, int n, const int *xywh): the replay keeps one
// scissor (index 0); other indices matter only to shaders that select a viewport index, so differing ones are reported
NVN_API(nvnCommandBufferSetScissors) {
    int first = (int)arg_x(c, 1), n = (int)arg_x(c, 2);
    const int *r = arg_p(c, 3);
    if (first < 0 || n < 0 || first + n > 16) rt_fatal("nvn: SetScissors first %d n %d", first, n);
    for (int i = 1; i < n; i++)
        if (memcmp(r + 4 * i, r, 16)) {
            static int once;
            if (!once++) rt_log("nvn: SetScissors: differing scissor %d ignored (single-scissor replay)", first + i);
        }
    if (first == 0 && n > 0) set_scissor(cmd_of(c), r[0], r[1], r[2], r[3]);
}
static void set_depth_range(cmd_t *o, float n, float f) { *REC(o, OP_DEPTH_RANGE, p_depth_range) = (p_depth_range){n, f}; }
// void nvnCommandBufferSetDepthRange(NVNcommandBuffer *cmd, float near, float far)
NVN_API(nvnCommandBufferSetDepthRange) { set_depth_range(cmd_of(c), arg_s(c, 0), arg_s(c, 1)); }
// void nvnCommandBufferSetDepthRanges(NVNcommandBuffer *cmd, int first, int n, const float *ranges)
NVN_API(nvnCommandBufferSetDepthRanges) {
    int first = (int)arg_x(c, 1), n = (int)arg_x(c, 2);
    const float *r = arg_p(c, 3);
    if (first < 0 || n < 0 || first + n > 16) rt_fatal("nvn: SetDepthRanges first %d n %d", first, n);
    for (int i = 1; i < n; i++)
        if (memcmp(r + 2 * i, r, 8)) {
            static int once;
            if (!once++) rt_log("nvn: SetDepthRanges: differing range %d ignored (single-range replay)", first + i);
        }
    if (first == 0 && n > 0) set_depth_range(cmd_of(c), r[0], r[1]);
}
// void nvnCommandBufferSetDepthClamp / SetRenderEnable / SetRasterizerDiscard(NVNcommandBuffer *cmd, NVNboolean enable)
NVN_API(nvnCommandBufferSetDepthClamp) { REC(cmd_of(c), OP_DEPTH_CLAMP, p_flag)->value = nvn_arg_bool(c, 1) != 0; }
NVN_API(nvnCommandBufferSetRenderEnable) { REC(cmd_of(c), OP_RENDER_ENABLE, p_flag)->value = nvn_arg_bool(c, 1) != 0; }
// Rasterizer discard runs vertex work without rasterising; with no transform feedback that equals not drawing.
NVN_API(nvnCommandBufferSetRasterizerDiscard) { REC(cmd_of(c), OP_RASTER_DISCARD, p_flag)->value = nvn_arg_bool(c, 1) != 0; }
// void nvnCommandBufferSetPrimitiveRestart(NVNcommandBuffer *cmd, NVNboolean enable, int index): TOTK draws its
// terrain and cliff strips with 16-bit indices and restart at 0xffff; the replay applies it per indexed draw (op_draw_idx)
NVN_API(nvnCommandBufferSetPrimitiveRestart) {
    *REC(cmd_of(c), OP_PRIM_RESTART, p_prim_restart) = (p_prim_restart){nvn_arg_bool(c, 1) != 0, (uint32_t)arg_x(c, 2)};
}
// void nvnCommandBufferSetRenderEnableConditional(NVNcommandBuffer *cmd, NVNconditionalRenderMode mode,
// NVNbufferAddress counters): rendering is enabled when the two NVNcounterData.counter values at addr / addr+16
// compare equal (mode 0) or not equal (mode 1). TOTK gates most of its ClearColor commands with it; the replay
// evaluates it in GPU order.
NVN_API(nvnCommandBufferSetRenderEnableConditional) {
    int m = (int)arg_x(c, 1);
    if (m != 0 && m != 1) rt_fatal("nvn: SetRenderEnableConditional: unknown mode %d", m);
    p_render_cond *r = REC(cmd_of(c), OP_RENDER_COND, p_render_cond);
    r->mode = m;
    r->addr = arg_x(c, 2);
}
// void nvnCommandBufferBindProgram(NVNcommandBuffer *cmd, const NVNprogram *program, int stages)
NVN_API(nvnCommandBufferBindProgram) {
    *REC(cmd_of(c), OP_BIND_PROGRAM, p_bind_program) = (p_bind_program){arg_p(c, 1), (int)arg_x(c, 2)};
}
// void nvnCommandBufferBind{Blend,Color,ChannelMask,Multisample,Polygon,DepthStencil}State(NVNcommandBuffer *cmd,
// const NVN*State *state): the state object's bytes are copied (p_bind_state.kind order)
static void bind_state(const Cpu *c, int kind, size_t n) {
    p_bind_state *r = REC(cmd_of(c), OP_BIND_STATE, p_bind_state);
    r->kind = kind;
    memcpy(r->bytes, arg_p(c, 1), n);
}
NVN_API(nvnCommandBufferBindBlendState) { bind_state(c, 0, sizeof(blend_st)); }
NVN_API(nvnCommandBufferBindColorState) { bind_state(c, 1, sizeof(color_st)); }
NVN_API(nvnCommandBufferBindChannelMaskState) { bind_state(c, 2, sizeof(chmask_st)); }
NVN_API(nvnCommandBufferBindMultisampleState) { bind_state(c, 3, sizeof(ms_st)); }
NVN_API(nvnCommandBufferBindPolygonState) { bind_state(c, 4, sizeof(poly_st)); }
NVN_API(nvnCommandBufferBindDepthStencilState) { bind_state(c, 5, sizeof(ds_st)); }
// void nvnCommandBufferSetPointSize(NVNcommandBuffer *cmd, float size): recorded as bind-state kind 6 with the float as payload
NVN_API(nvnCommandBufferSetPointSize) {
    p_bind_state *r = REC(cmd_of(c), OP_BIND_STATE, p_bind_state);
    float s = arg_s(c, 0);
    r->kind = 6;
    memcpy(r->bytes, &s, sizeof s);
}
// void nvnCommandBufferBindVertexAttribState(NVNcommandBuffer *cmd, int n, const NVNvertexAttribState *states)
NVN_API(nvnCommandBufferBindVertexAttribState) {
    cmd_t *o = cmd_of(c);
    int n = (int)arg_x(c, 1);
    if (n < 0 || n > 16) { rt_log("nvn: BindVertexAttribState n %d", n); return; }
    p_bind_vattr *r = RECN(o, OP_BIND_VATTR, p_bind_vattr, st, n);
    r->first = 0;
    r->n = n;
    memcpy(r->st, arg_p(c, 2), 4 * (size_t)n);
    hw_fix(o, 68);   // the driver always writes all 16 attribute formats: 17 words
}
// void nvnCommandBufferBindVertexStreamState(NVNcommandBuffer *cmd, int n, const NVNvertexStreamState *states)
NVN_API(nvnCommandBufferBindVertexStreamState) {
    cmd_t *o = cmd_of(c);
    int n = (int)arg_x(c, 1);
    if (n < 0 || n > 16) { rt_log("nvn: BindVertexStreamState n %d", n); return; }
    p_bind_vstream *r = RECN(o, OP_BIND_VSTREAM, p_bind_vstream, st, n);
    r->first = 0;
    r->n = n;
    memcpy(r->st, arg_p(c, 2), 8 * (size_t)n);
    hw_fix(o, 4 * (16 + (size_t)n));
}
static void bind_vertex_buffers(cmd_t *o, int first, int n, const NVNbufferRange *rg) {
    if (first < 0 || n < 0 || first + n > 16) { rt_log("nvn: BindVertexBuffers first %d n %d", first, n); return; }
    p_bind_vbuf *r = RECN(o, OP_BIND_VBUF, p_bind_vbuf, r, n);
    r->first = first;
    r->n = n;
    memcpy(r->r, rg, sizeof *rg * (size_t)n);
    hw_fix(o, 24 * (size_t)n);
}
// void nvnCommandBufferBindVertexBuffers(NVNcommandBuffer *cmd, int first, int n, const NVNbufferRange *ranges)
NVN_API(nvnCommandBufferBindVertexBuffers) { bind_vertex_buffers(cmd_of(c), (int)arg_x(c, 1), (int)arg_x(c, 2), arg_p(c, 3)); }
// void nvnCommandBufferBindVertexBuffer(NVNcommandBuffer *cmd, int index, NVNbufferAddress address, size_t size)
NVN_API(nvnCommandBufferBindVertexBuffer) {
    NVNbufferRange r = {arg_x(c, 2), arg_x(c, 3)};
    bind_vertex_buffers(cmd_of(c), (int)arg_x(c, 1), 1, &r);
}
// Uniform and storage buffers: one record per binding (CB_SIZE/ADDRESS 4 words + CB_BIND 1 on hardware).
static void bind_buffers(const Cpu *c, uint32_t op) {   // (cmd, stage, first, n, const NVNbufferRange *ranges)
    cmd_t *o = cmd_of(c);
    int st = (int)arg_x(c, 1), first = (int)arg_x(c, 2), n = (int)arg_x(c, 3);
    const NVNbufferRange *rg = arg_p(c, 4);
    for (int i = 0; i < n; i++) {
        *REC(o, op, p_bind_buf) = (p_bind_buf){st, first + i, rg[i].address, rg[i].size};
        hw_fix(o, 20);
    }
}
static void bind_buffer(const Cpu *c, uint32_t op) {    // (cmd, stage, index, NVNbufferAddress address, size_t size)
    cmd_t *o = cmd_of(c);
    *REC(o, op, p_bind_buf) = (p_bind_buf){(int)arg_x(c, 1), (int)arg_x(c, 2), arg_x(c, 3), arg_x(c, 4)};
    hw_fix(o, 20);
}
NVN_API(nvnCommandBufferBindUniformBuffer) { bind_buffer(c, OP_BIND_UBO); }
NVN_API(nvnCommandBufferBindUniformBuffers) { bind_buffers(c, OP_BIND_UBO); }
NVN_API(nvnCommandBufferBindStorageBuffer) { bind_buffer(c, OP_BIND_SSBO); }
NVN_API(nvnCommandBufferBindStorageBuffers) { bind_buffers(c, OP_BIND_SSBO); }
// Texture, image and separate handle binds. A deferred bind (NVN 55 *Deferred entry points, the fallback of the inline
// fast paths) keeps the pointer to the game's handle array: the original driver (sdk+0x46cc00 BindTexturesDeferred)
// reads the handles when the command buffer is submitted, and the game fills its material handle tables after
// recording and before the submit (nvn_submit_prepare). hw_bytes: the hardware size of the plain variant.
static void bind_handles(cmd_t *o, uint32_t op, int st, int first, int n, const uint64_t *h, int deferred, size_t hw_bytes) {
    if (n < 0 || n > 32) { rt_log("nvn: bind of %d handles", n); return; }
    p_bind_handles *r = RECN(o, op, p_bind_handles, h, n);
    r->stage = st;
    r->first = first;
    r->n = n;
    r->src = deferred ? (uint64_t)(uintptr_t)h : 0;
    memcpy(r->h, h, 8 * (size_t)n);
    if (r->src && n > 0) note_def(o, (size_t)((uint8_t *)r - 8 - o->seg));   // resolved at the submit (resolve_list)
    hw_fix(o, hw_bytes ? hw_bytes : 16 + 4 * (size_t)n);   // macro 7 + method 0x1a2c + CB_POS + one word per binding
}
static void bind_handle_array(const Cpu *c, uint32_t op, int deferred, int wide) {   // (cmd, stage, first, n, handles)
    int n = (int)arg_x(c, 3);
    bind_handles(cmd_of(c), op, (int)arg_x(c, 1), (int)arg_x(c, 2), n, arg_p(c, 4), deferred,
                 wide && n >= 0 && n <= 32 ? 16 + 8 * (size_t)n : 0);
}
static void bind_one_handle(const Cpu *c, uint32_t op, size_t hw_bytes) {   // (cmd, stage, index, handle)
    uint64_t v = arg_x(c, 3);
    bind_handles(cmd_of(c), op, (int)arg_x(c, 1), (int)arg_x(c, 2), 1, &v, 0, hw_bytes);
}
// Combined texture handles share the texture slots with separate textures (sampler id in bits 20 and up). Images
// (writable textures): handle = texture pool index; the game does not call RegisterImage, images and textures share
// their registration.
NVN_API(nvnCommandBufferBindTexture) { bind_one_handle(c, OP_BIND_TEX, 0); }
NVN_API(nvnCommandBufferBindTextures) { bind_handle_array(c, OP_BIND_TEX, 0, 0); }
NVN_API(nvnCommandBufferBindImage) { bind_one_handle(c, OP_BIND_IMG, 24); }
NVN_API(nvnCommandBufferBindImages) { bind_handle_array(c, OP_BIND_IMG, 0, 1); }
NVN_API(nvnCommandBufferBindSeparateTexture) { bind_one_handle(c, OP_BIND_TEX, 24); }
NVN_API(nvnCommandBufferBindSeparateTextures) { bind_handle_array(c, OP_BIND_TEX, 0, 1); }
NVN_API(nvnCommandBufferBindSeparateSampler) { bind_one_handle(c, OP_BIND_SAMP, 24); }
NVN_API(nvnCommandBufferBindSeparateSamplers) { bind_handle_array(c, OP_BIND_SAMP, 0, 1); }
// void nvnCommandBufferBind{Textures,Images,SeparateTextures,SeparateSamplers}Deferred(NVNcommandBuffer *cmd,
// NVNshaderStage stage, int first, int n, const T *handles) (NVN 55, no prototype in nvn.h)
NVN_API(nvnCommandBufferBindTexturesDeferred) { bind_handle_array(c, OP_BIND_TEX, 1, 0); }
NVN_API(nvnCommandBufferBindImagesDeferred) { bind_handle_array(c, OP_BIND_IMG, 1, 1); }
NVN_API(nvnCommandBufferBindSeparateTexturesDeferred) { bind_handle_array(c, OP_BIND_TEX, 1, 1); }
NVN_API(nvnCommandBufferBindSeparateSamplersDeferred) { bind_handle_array(c, OP_BIND_SAMP, 1, 1); }
// void nvnCommandBufferBind{Uniform,Storage}BuffersDeferred(NVNcommandBuffer *cmd, NVNshaderStage stage, int first,
// int n, const NVNbufferRange *ranges): buffer ranges still bind at record time
NVN_API(nvnCommandBufferBindUniformBuffersDeferred) {
    static int once;
    if (!once++) NVN_LOG("BindUniformBuffersDeferred: buffer ranges bind at record time");
    bind_buffers(c, OP_BIND_UBO);
}
NVN_API(nvnCommandBufferBindStorageBuffersDeferred) {
    static int once;
    if (!once++) NVN_LOG("BindStorageBuffersDeferred: buffer ranges bind at record time");
    bind_buffers(c, OP_BIND_SSBO);
}
// void nvnCommandBufferSetStencil{ValueMask,Mask,Ref}(NVNcommandBuffer *cmd, NVNface face, int value)
NVN_API(nvnCommandBufferSetStencilValueMask) {
    *REC(cmd_of(c), OP_STENCIL, p_stencil) = (p_stencil){(int)arg_x(c, 1), (int)arg_x(c, 2), 0, 0, 0};
}
NVN_API(nvnCommandBufferSetStencilMask) {
    *REC(cmd_of(c), OP_STENCIL, p_stencil) = (p_stencil){(int)arg_x(c, 1), 0, (int)arg_x(c, 2), 0, 1};
}
NVN_API(nvnCommandBufferSetStencilRef) {
    *REC(cmd_of(c), OP_STENCIL, p_stencil) = (p_stencil){(int)arg_x(c, 1), 0, 0, (int)arg_x(c, 2), 2};
}
// void nvnCommandBufferSetPolygonOffsetClamp(NVNcommandBuffer *cmd, float factor, float units, float clamp)
NVN_API(nvnCommandBufferSetPolygonOffsetClamp) {
    *REC(cmd_of(c), OP_POLY_OFFSET, p_poly_offset) = (p_poly_offset){arg_s(c, 0), arg_s(c, 1), arg_s(c, 2)};
}
// void nvnCommandBufferDrawArrays(NVNcommandBuffer *cmd, NVNdrawPrimitive prim, int first, int count)
NVN_API(nvnCommandBufferDrawArrays) {
    *REC(cmd_of(c), OP_DRAW, p_draw) = (p_draw){(int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3), 0, 1};
}
// void nvnCommandBufferDrawArraysInstanced(NVNcommandBuffer *cmd, NVNdrawPrimitive prim, int first, int count,
// int base_instance, int instances): zero instances draw nothing
NVN_API(nvnCommandBufferDrawArraysInstanced) {
    int inst = (int)arg_x(c, 5);
    if (inst <= 0) return;
    *REC(cmd_of(c), OP_DRAW, p_draw) = (p_draw){(int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3), (int)arg_x(c, 4), inst};
}
// void nvnCommandBufferDrawElements(NVNcommandBuffer *cmd, NVNdrawPrimitive prim, NVNindexType type, int count,
// NVNbufferAddress indices)
NVN_API(nvnCommandBufferDrawElements) {
    cmd_t *o = cmd_of(c);
    *REC(o, OP_DRAW_IDX, p_draw_idx) = (p_draw_idx){(int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3), 0, 0, 1, arg_x(c, 4)};
    hw_fix(o, 48);
}
// void nvnCommandBufferDrawElementsBaseVertex(..., NVNbufferAddress indices, int base_vertex)
NVN_API(nvnCommandBufferDrawElementsBaseVertex) {
    cmd_t *o = cmd_of(c);
    *REC(o, OP_DRAW_IDX, p_draw_idx) =
        (p_draw_idx){(int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3), (int)arg_x(c, 5), 0, 1, arg_x(c, 4)};
    hw_fix(o, 44);
}
// void nvnCommandBufferDrawElementsInstanced(..., NVNbufferAddress indices, int base_vertex, int base_instance,
// int instances): assumed to use the 6-parameter draw macro of the base-vertex form (unverified)
NVN_API(nvnCommandBufferDrawElementsInstanced) {
    cmd_t *o = cmd_of(c);
    *REC(o, OP_DRAW_IDX, p_draw_idx) = (p_draw_idx){(int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3),
                                                    (int)arg_x(c, 5), (int)arg_x(c, 6), (int)arg_x(c, 7), arg_x(c, 4)};
    hw_fix(o, 44);
}
// void nvnCommandBufferDrawElementsIndirect(NVNcommandBuffer *cmd, NVNdrawPrimitive prim, NVNindexType type,
// NVNbufferAddress indices, NVNbufferAddress indirect): the arguments at indirect are often GPU-written, so the replay
// draws with vkCmdDrawIndexedIndirect from the pool buffer (nxnative hle_nvn.c, SavageWindRecomp cmdbuf.c)
NVN_API(nvnCommandBufferDrawElementsIndirect) {
    *REC(cmd_of(c), OP_DRAW_IDX_IND, p_draw_idx_ind) = (p_draw_idx_ind){(int)arg_x(c, 1), (int)arg_x(c, 2), arg_x(c, 3), arg_x(c, 4)};
}
// void nvnCommandBufferDispatchCompute(NVNcommandBuffer *cmd, int x, int y, int z): caller = the game's return address
NVN_API(nvnCommandBufferDispatchCompute) {
    *REC(cmd_of(c), OP_DISPATCH, p_dispatch) = (p_dispatch){(int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3), c->x[30]};
}
// void nvnCommandBufferDispatchComputeIndirect(NVNcommandBuffer *cmd, NVNbufferAddress indirect): vkCmdDispatchIndirect
NVN_API(nvnCommandBufferDispatchComputeIndirect) {
    *REC(cmd_of(c), OP_DISPATCH_IND, p_dispatch_ind) = (p_dispatch_ind){arg_x(c, 1), c->x[30]};
}
// void nvnCommandBufferClearBuffer(NVNcommandBuffer *cmd, NVNbufferAddress addr, size_t size, uint32_t value)
NVN_API(nvnCommandBufferClearBuffer) {
    *REC(cmd_of(c), OP_CLEAR_BUF, p_clear_buf) = (p_clear_buf){arg_x(c, 1), arg_x(c, 2), (uint32_t)arg_x(c, 3), 0};
}
// void nvnCommandBufferReportValue(NVNcommandBuffer *cmd, uint32_t value, NVNbufferAddress addr): the value lands when
// the GPU reaches this point. Recorded as a 4-byte ClearBuffer, so it stays in GPU order with the ClearBuffer resets of
// the same counter (main+0x7b9274 modes 1-3) and works in CPU-invisible pools; the CPU sees it after the fence, as a
// counter report.
NVN_API(nvnCommandBufferReportValue) {
    *REC(cmd_of(c), OP_CLEAR_BUF, p_clear_buf) = (p_clear_buf){arg_x(c, 2), 4, (uint32_t)arg_x(c, 1), 0};
}
// Colour reduction is a Maxwell bandwidth hint for blending into compressed targets: no effect on the result.
// void nvnCommandBufferSetColorReductionEnable(cmd, NVNboolean enable), SetColorReductionThresholds(cmd, class, const int *)
NVN_API(nvnCommandBufferSetColorReductionEnable) { (void)c; }
NVN_API(nvnCommandBufferSetColorReductionThresholds) { (void)c; }
// void nvnCommandBufferBarrier(NVNcommandBuffer *cmd, int bits)
NVN_API(nvnCommandBufferBarrier) { REC(cmd_of(c), OP_BARRIER, p_barrier)->bits = (int)arg_x(c, 1); }
// void nvnCommandBufferSetTexturePool / SetSamplerPool(NVNcommandBuffer *cmd, const NVNtexturePool / NVNsamplerPool *)
NVN_API(nvnCommandBufferSetTexturePool) { REC(cmd_of(c), OP_SET_TEX_POOL, p_set_pool)->pool = arg_p(c, 1); }
NVN_API(nvnCommandBufferSetSamplerPool) { REC(cmd_of(c), OP_SET_SAMP_POOL, p_set_pool)->pool = arg_p(c, 1); }
// void nvnCommandBufferCopyBufferToTexture(NVNcommandBuffer *cmd, NVNbufferAddress src, const NVNtexture *dst,
// const NVNtextureView *view, const NVNcopyRegion *region, int flags)
NVN_API(nvnCommandBufferCopyBufferToTexture) {
    p_copy_b2t *o = REC(cmd_of(c), OP_COPY_B2T, p_copy_b2t);
    o->src = arg_x(c, 1);
    o->tex = tex_pin(cmd_of(c), arg_p(c, 2));
    copy_view(&o->view, &o->has_view, arg_p(c, 3));
    o->r = *(const NVNcopyRegion *)arg_p(c, 4);
    o->flags = (int)arg_x(c, 5);
}
// void nvnCommandBufferCopyTextureToBuffer(NVNcommandBuffer *cmd, const NVNtexture *src, const NVNtextureView *view,
// const NVNcopyRegion *region, NVNbufferAddress dst, int flags): p_copy_b2t with src = the destination address
NVN_API(nvnCommandBufferCopyTextureToBuffer) {
    p_copy_b2t *o = REC(cmd_of(c), OP_COPY_T2B, p_copy_b2t);
    o->src = arg_x(c, 4);
    o->tex = tex_pin(cmd_of(c), arg_p(c, 1));
    copy_view(&o->view, &o->has_view, arg_p(c, 2));
    o->r = *(const NVNcopyRegion *)arg_p(c, 3);
    o->flags = (int)arg_x(c, 5);
}
// void nvnCommandBufferClearTexture{,ui}(NVNcommandBuffer *cmd, const NVNtexture *tex, const NVNtextureView *view,
// const NVNcopyRegion *region, const float / uint32_t *rgba, int mask)
static void clear_texture(const Cpu *c, int is_uint) {
    p_clear_tex *o = REC(cmd_of(c), OP_CLEAR_TEX, p_clear_tex);
    o->tex = tex_pin(cmd_of(c), arg_p(c, 1));
    copy_view(&o->view, &o->has_view, arg_p(c, 2));
    o->r = *(const NVNcopyRegion *)arg_p(c, 3);
    o->mask = (int)arg_x(c, 5);
    o->is_uint = (uint8_t)is_uint;
    memcpy(o->c.u, arg_p(c, 4), 16);
}
NVN_API(nvnCommandBufferClearTexture) { clear_texture(c, 0); }
NVN_API(nvnCommandBufferClearTextureui) { clear_texture(c, 1); }
// void nvnCommandBufferCopyTextureToTexture(NVNcommandBuffer *cmd, const NVNtexture *src, const NVNtextureView *src_view,
// const NVNcopyRegion *src_region, const NVNtexture *dst, const NVNtextureView *dst_view,
// const NVNcopyRegion *dst_region, int flags)
NVN_API(nvnCommandBufferCopyTextureToTexture) {
    p_copy_t2t *o = REC(cmd_of(c), OP_COPY_T2T, p_copy_t2t);
    o->src = tex_pin(cmd_of(c), arg_p(c, 1));
    o->dst = tex_pin(cmd_of(c), arg_p(c, 4));
    copy_view(&o->sv, &o->has_sv, arg_p(c, 2));
    copy_view(&o->dv, &o->has_dv, arg_p(c, 5));
    o->sr = *(const NVNcopyRegion *)arg_p(c, 3);
    o->dr = *(const NVNcopyRegion *)arg_p(c, 6);
    o->flags = (int)arg_x(c, 7);
}
// void nvnCommandBufferDownsample(NVNcommandBuffer *cmd, const NVNtexture *src, const NVNtexture *dst): level 0 of src
// into level 0 of dst, whole extents, filtered. Recorded as a linear-filter texture copy: copy_t2t resolves a
// multisample source into an equal-size target and blits (2x2 average at half size) between sizes. As nxnative
// hle_nvn.c and SavageWindRecomp cmdbuf.c record it; docs/maxwell.md 1.19.
NVN_API(nvnCommandBufferDownsample) {
    p_copy_t2t *o = REC(cmd_of(c), OP_COPY_T2T, p_copy_t2t);
    o->src = tex_pin(cmd_of(c), arg_p(c, 1));
    o->dst = tex_pin(cmd_of(c), arg_p(c, 2));
    if (o->src) o->sr = (NVNcopyRegion){0, 0, 0, (int)o->src->d.width, (int)o->src->d.height, 1};
    if (o->dst) o->dr = (NVNcopyRegion){0, 0, 0, (int)o->dst->d.width, (int)o->dst->d.height, 1};
    o->flags = NVN_COPY_FLAGS_LINEAR_FILTER_BIT;
}
// void nvnCommandBufferSetCopyRowStride(NVNcommandBuffer *cmd, ptrdiff_t stride)
NVN_API(nvnCommandBufferSetCopyRowStride) { REC(cmd_of(c), OP_COPY_STRIDE, p_copy_stride)->stride = (int64_t)arg_x(c, 1); }
// void nvnCommandBufferSetBlendColor(NVNcommandBuffer *cmd, const float *rgba)
NVN_API(nvnCommandBufferSetBlendColor) { memcpy(REC(cmd_of(c), OP_BLEND_COLOR, p_blend_color)->rgba, arg_p(c, 1), 16); }

// Commands without an effect on this replay (hle_nvn.c g_nops): WaitSync and WaitEvent order work against other queues
// or the CPU, and every submit replays in order on one queue; shader scratch memory and the tessellation patch size
// have no host counterpart the replay needs.
// void nvnCommandBufferWaitSync(NVNcommandBuffer *cmd, const NVNsync *sync), WaitEvent(cmd, const NVNevent *event,
// NVNeventWaitMode mode, uint32_t value), SetShaderScratchMemory(cmd, const NVNmemoryPool *pool, ptrdiff_t offset,
// size_t size), SetPatchSize(cmd, int size)
NVN_API(nvnCommandBufferWaitSync) { (void)c; }
NVN_API(nvnCommandBufferWaitEvent) { (void)c; }
NVN_API(nvnCommandBufferSetShaderScratchMemory) { (void)c; }
NVN_API(nvnCommandBufferSetPatchSize) { (void)c; }

// void nvnCommandBufferFenceSync(NVNcommandBuffer *cmd, NVNsync *sync, NVNsyncCondition condition, int flags): the sync
// is signaled once the GPU ran every command before it. Here it is fenced at the value of the whole submit that carries
// the record (nvn_submit_fence_syncs), which completes no earlier than the console's point. The game never calls it;
// mod plugins do (Zonai Fieldworks' native-font renderer).
NVN_API(nvnCommandBufferFenceSync) {
    __atomic_store_n(&fence_sync_recorded, 1, __ATOMIC_RELAXED);
    cmd_t *o = cmd_of(c);
    REC(o, OP_FENCE_SYNC, p_fence_sync)->sync = arg_p(c, 1);
    rec_pf(o)->fsync = 1;   // after the record: its room may have moved the buffer and the prefix
}

// ---------- submit
// What the console reads at the submit call is read here, on the game thread: every handle must be a live recording,
// and deferred binds take the handles the game's arrays hold now (the game fills its tables for the next frame as soon
// as the submit returns). The result is written into the recording copy.
static uint64_t def_reads, def_changed, n_clones;

static int resolve_deferred(const uint8_t *p, int depth) {   // returns the number of deferred binds
    int nd = 0;
    for (;;) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_)
            rt_fatal("nvn: corrupt recording at %p while resolving deferred binds (op %u size %u)", (const void *)p,
                     h.op, h.size);
        if (h.op == OP_END) return nd;
        if (h.op == OP_JUMP) {
            memcpy(&p, p + 8, 8);
            continue;
        }
        if (h.op == OP_CALL) {
            uint64_t t;
            memcpy(&t, p + 8, 8);
            if (depth >= 16) rt_fatal("nvn: deferred bind resolution: call depth over 16");
            // (synchronous replay only: no queued submission reads the called copy; the record's flag says whether
            // it holds deferred binds)
            uint64_t fl = 0;
            if (h.size >= 24) memcpy(&fl, p + 16, 8);
            if (fl & 1) nd += resolve_deferred((const uint8_t *)(uintptr_t)t, depth + 1);
        } else if (h.op == OP_BIND_TEX || h.op == OP_BIND_IMG || h.op == OP_BIND_SAMP) {
            p_bind_handles *b = (p_bind_handles *)(uintptr_t)(p + 8);
            if (b->src && b->n > 0 && b->n <= 32) {
                nd++;
                uint64_t cur[32];
                memcpy(cur, (const void *)(uintptr_t)b->src, 8 * (size_t)b->n);
                __atomic_fetch_add(&def_reads, 1, __ATOMIC_RELAXED);
                if (memcmp(cur, b->h, 8 * (size_t)b->n)) {
                    if (__atomic_fetch_add(&def_changed, 1, __ATOMIC_RELAXED) < 6)
                        NVN_LOG("deferred bind read new handles at submit: stage %d slot %d, %#llx at record -> %#llx",
                                b->stage, b->first, (unsigned long long)b->h[0], (unsigned long long)cur[0]);
                    memcpy(b->h, cur, 8 * (size_t)b->n);
                }
            }
        }
        p += h.size;
    }
}

void nvn_submit_prepare(const uint64_t *handles, int n) {
    uint64_t now = __atomic_add_fetch(&nsubmits, 1, __ATOMIC_RELAXED);
    for (int i = 0; i < n; i++) copy_touch(handles[i], now, "QueueSubmitCommands", NULL);
    for (int i = 0; i < n; i++) resolve_deferred((const uint8_t *)(uintptr_t)handles[i], 0);
}

// The lists of pf for its clone cl (the prefix and the records were copied): pins, called copies (counted), deferred
// binds. copy_lock held or not: pf keeps the copies it names alive.
static void clone_lists(copy_prefix *cl, const copy_prefix *pf) {
    cl->pin_v = NULL, cl->kid_v = NULL, cl->def_v = NULL, cl->dkid_v = NULL;
    cl->recbuf = 0;
    cl->pin_cap = (uint32_t)pf->pins, cl->kid_cap = (uint32_t)pf->nkids, cl->def_cap = (uint32_t)pf->ndef, cl->dkid_cap = (uint32_t)pf->ndkids;
    if (pf->pins) {
        if (!(cl->pin_v = malloc(sizeof *cl->pin_v * pf->pins))) rt_fatal("nvn: out of memory (recording clone)");
        memcpy((void *)cl->pin_v, (const void *)pf->pin_v, sizeof *cl->pin_v * pf->pins);
    }
    copy_pins_ref(cl, 1);
    if (pf->nkids) {
        if (!(cl->kid_v = malloc(sizeof *cl->kid_v * pf->nkids))) rt_fatal("nvn: out of memory (recording clone)");
        memcpy(cl->kid_v, pf->kid_v, sizeof *cl->kid_v * pf->nkids);
        for (uint64_t k = 0; k < pf->nkids; k++) __atomic_add_fetch(&((copy_prefix *)pf->kid_v[k])->state, 2, __ATOMIC_ACQ_REL);
    }
    if (pf->ndef) {
        if (!(cl->def_v = malloc(sizeof *cl->def_v * pf->ndef))) rt_fatal("nvn: out of memory (recording clone)");
        memcpy(cl->def_v, pf->def_v, sizeof *cl->def_v * pf->ndef);
    }
    if (pf->ndkids) {
        if (!(cl->dkid_v = malloc(sizeof *cl->dkid_v * pf->ndkids))) rt_fatal("nvn: out of memory (recording clone)");
        memcpy(cl->dkid_v, pf->dkid_v, sizeof *cl->dkid_v * pf->ndkids);
    }
}

// The deferred binds of pf from its list: 1 when a bind's handles differ from the game's array now; write puts the new
// ones into the records.
static int resolve_list(copy_prefix *pf, int write) {
    uint8_t *s = (uint8_t *)(pf + 1);
    int changed = 0;
    for (uint64_t i = 0; i < pf->ndef; i++) {
        p_bind_handles *b = (p_bind_handles *)(void *)(s + pf->def_v[i] + 8);
        uint64_t cur[32];
        memcpy(cur, (const void *)(uintptr_t)b->src, 8 * (size_t)b->n);
        if (!memcmp(cur, b->h, 8 * (size_t)b->n)) continue;
        changed = 1;
        if (!write) return 1;
        if (__atomic_fetch_add(&def_changed, 1, __ATOMIC_RELAXED) < 6)
            NVN_LOG("deferred bind read new handles at submit: stage %d slot %d, %#llx at record -> %#llx", b->stage,
                    b->first, (unsigned long long)b->h[0], (unsigned long long)cur[0]);
        memcpy(b->h, cur, 8 * (size_t)b->n);
    }
    __atomic_fetch_add(&def_reads, pf->ndef, __ATOMIC_RELAXED);
    return changed;
}

// The copies that the submitted copy pf calls by reference and that hold deferred binds, their own or below them
// (asynchronous replay). Such a copy is shared: other recordings call it, in other frames. Its handles stay when the
// game's arrays still hold them, and they are written in place when no queued submission reads the copy. Else this
// submission gets a clone of it with the new handles, and so does every copy on the way down to it: the clone takes
// the original's place in its caller (the OP_CALL record, the lists), so it lives as long as the caller. Only a clone
// is patched like that. The submitted copy pf can be a live copy, which another thread may copy at this moment
// (inline_recording reads its records and lists), so it gets a clone for this submission too (resolve_kids).
// copy_lock not held.
static PlatMutex res_lock = PLAT_MUTEX_INIT;   // res_at and the handles of called copies; a leaf lock

// cl takes the place of the copy that owner's dkid_v[j] names.
static void patch_kid(copy_prefix *owner, uint64_t j, copy_prefix *cl) {
    copy_prefix *old = owner->dkid_v[j].k;
    uint64_t h = (uintptr_t)(cl + 1);
    memcpy((uint8_t *)(owner + 1) + owner->dkid_v[j].off + 8, &h, 8);
    owner->dkid_v[j].k = cl;
    owner->kid_v[owner->dkid_v[j].idx] = cl;
    kid_unref(old);
}

// k as this submission reads it: k itself, or a clone that only this submission's copies call.
static copy_prefix *resolve_shared(copy_prefix *k, uint64_t now, uint64_t replayed, int depth) {
    if (depth >= 8) rt_fatal("nvn: deferred bind resolution: call depth over 8");
    int own = 0;
    if (k->ndef) {
        plat_mutex_lock(&res_lock);
        static int force = -1;   // TOTK_NVN_CALL_CLONE=1 (a test): every called copy with new handles is cloned
        if (force < 0) force = rt_knob("NVN_CALL_CLONE", "0");
        int queued = force || (k->res_at > replayed && k->res_at != now);
        int changed = resolve_list(k, 0);
        if (changed && !queued) resolve_list(k, 1);
        if (!changed || !queued) k->res_at = now;
        own = changed && queued;
        plat_mutex_unlock(&res_lock);
    }
    copy_prefix *cl = NULL;
    if (own) {
        cl = copy_block(k->bytes, 1, "called recording clone");
        size_t cap = cl->cap;
        memcpy(cl, k, sizeof *k + k->bytes);
        cl->cap = cap;
        clone_lists(cl, k);
        resolve_list(cl, 1);
    }
    for (uint64_t j = 0; j < k->ndkids; j++) {
        copy_prefix *c0 = (cl ? cl : k)->dkid_v[j].k, *c = resolve_shared(c0, now, replayed, depth + 1);
        if (c == c0) continue;
        if (!cl) {   // a copy below k was cloned: k is cloned too, to name it
            cl = copy_block(k->bytes, 1, "called recording clone");
            size_t cap = cl->cap;
            memcpy(cl, k, sizeof *k + k->bytes);
            cl->cap = cap;
            clone_lists(cl, k);
        }
        patch_kid(cl, j, c);
    }
    if (!cl) {
        // This submission reads k through its caller. A k with binds of its own has its res_at from above. A k that
        // only names copies with binds gets it here, so a later submission of k itself, while this one is queued,
        // takes a clone (nvn_submit_prepare_async) like the submission of a copy with binds of its own.
        if (!k->ndef && k->ndkids) {
            plat_mutex_lock(&res_lock);
            k->res_at = now;
            plat_mutex_unlock(&res_lock);
        }
        return k;
    }
    cl->magic = 0;      // not a live handle
    cl->state = 2 | 1;  // one OP_CALL record names it (its caller's), and no handle: the last record gone frees it
    __atomic_fetch_add(&kept_refs, 1, __ATOMIC_RELAXED);
    cl->used_at = cl->res_at = now;
    __atomic_fetch_add(&n_clones, 1, __ATOMIC_RELAXED);
    return cl;
}

// A clone of the copy pf for one submission: the game never sees it, and it is freed after the replay of that
// submission (zombie_sweep). copy_lock held.
static copy_prefix *submit_clone(copy_prefix *pf, uint64_t now) {
    copy_prefix *cl = copy_block(pf->bytes, 0, "recording clone");   // copy_lock held
    size_t cap = cl->cap;
    memcpy(cl, pf, sizeof *pf + pf->bytes);
    cl->cap = cap;
    clone_lists(cl, pf);
    cl->state = 0;
    cl->magic = 0;   // not a live handle: the game never sees it
    cl->used_at = now;
    zombie_add(cl);
    __atomic_fetch_add(&n_clones, 1, __ATOMIC_RELAXED);
    return cl;
}

// The copies with deferred binds that the submitted copy pf names, as this submission reads them. own: pf is a clone
// of this submission. Returns the copy the submission reads: pf, or a clone of a live pf made here because one of
// the copies it names was replaced by a clone.
static copy_prefix *resolve_kids(copy_prefix *pf, int own, uint64_t now, uint64_t replayed) {
    for (uint64_t j = 0; j < pf->ndkids; j++) {
        copy_prefix *c = resolve_shared(pf->dkid_v[j].k, now, replayed, 1);
        if (c == pf->dkid_v[j].k) continue;
        if (!own) {
            plat_mutex_lock(&copy_lock);
            pf = submit_clone(pf, now);
            plat_mutex_unlock(&copy_lock);
            own = 1;
        }
        patch_kid(pf, j, c);
    }
    return pf;
}

// Asynchronous replay. A copy a queued submission still reads keeps that submission's deferred binds: when it holds
// deferred binds (or is not known to hold none), this submission gets a clone, resolved here and freed after its replay.
void nvn_submit_prepare_async(const uint64_t *handles, uint64_t *out, int n) {
    uint64_t now = __atomic_add_fetch(&nsubmits, 1, __ATOMIC_RELAXED);
    uint64_t replayed = gfx_replayed_submits();
    plat_mutex_lock(&copy_lock);
    if (nzombies) zombie_sweep(replayed);
    for (int i = 0; i < n; i++) {
        copy_prefix *pf = copy_find(handles[i]);
        if (!pf) {
            plat_mutex_unlock(&copy_lock);
            rt_fatal("nvn: QueueSubmitCommands: command handle %#llx is not a live recording (released or never recorded)",
                     (unsigned long long)handles[i]);
        }
        out[i] = handles[i];
        // (res_at: a queued submission reads it through a call, resolve_shared)
        if (((pf->used_at > replayed && pf->used_at != now) || (pf->res_at > replayed && pf->res_at != now)) &&
            pf->deferred != 1) {
            out[i] = (uintptr_t)(submit_clone(pf, now) + 1);
        } else {
            pf->used_at = now;
        }
    }
    plat_mutex_unlock(&copy_lock);
    dead_free();
    for (int i = 0; i < n; i++) {
        copy_prefix *pf = (copy_prefix *)(uintptr_t)out[i] - 1;
        if (pf->deferred == 1) continue;   // known since EndRecording: its own binds and its calls' (end_recording)
        plat_mutex_lock(&res_lock);
        resolve_list(pf, 1);
        pf->res_at = now;
        plat_mutex_unlock(&res_lock);
        out[i] = (uintptr_t)(resolve_kids(pf, out[i] != handles[i], now, replayed) + 1);
    }
    if (now % 7200 == 0)
        rt_log("nvn: asynchronous replay at submit %llu: %llu recording clones so far, %u copies waiting for the replay, "
               "%lld kept by calls", (unsigned long long)now, (unsigned long long)__atomic_load_n(&n_clones, __ATOMIC_RELAXED),
               nzombies, (long long)__atomic_load_n(&kept_refs, __ATOMIC_RELAXED));
}

// The syncs that the OP_FENCE_SYNC records of a copy name, and of the copies it calls by reference (those whose prefix
// says they hold one), now wait for fence.
static void fence_syncs(const uint8_t *copy, uint64_t fence, int depth) {
    if (depth >= 8) rt_fatal("nvn: fence syncs: call depth over 8");
    for (const op_hdr *h = nvn_op_first((uintptr_t)copy); h->op != OP_END; h = nvn_op_next(h)) {
        if (h->op == OP_FENCE_SYNC) nvn_sync_fence(((const p_fence_sync *)nvn_op_payload(h))->sync, fence);
        else if (h->op == OP_CALL) {
            uint64_t kid;
            memcpy(&kid, nvn_op_payload(h), 8);
            if (((const copy_prefix *)(uintptr_t)kid - 1)->fsync) fence_syncs((const uint8_t *)(uintptr_t)kid, fence, depth + 1);
        }
    }
}

// The game's handles, not the per-submission clones of nvn_submit_prepare_async: a clone may be freed as soon as the
// replay is done with it, and the handles stay live while the game is in QueueSubmitCommands. A copy holds no OP_JUMP
// (end_recording), so each is one run of records to OP_END; the copies its OP_CALL records name live as long as it.
void nvn_submit_fence_syncs(const uint64_t *handles, int n, uint64_t fence) {
    if (!__atomic_load_n(&fence_sync_recorded, __ATOMIC_RELAXED)) return;
    for (int i = 0; i < n; i++)
        if (((const copy_prefix *)(uintptr_t)handles[i] - 1)->fsync) fence_syncs((const uint8_t *)(uintptr_t)handles[i], fence, 0);
}

uint64_t nvn_submit_count(void) { return __atomic_load_n(&nsubmits, __ATOMIC_RELAXED); }

const char *nvn_op_name(uint32_t op) {
    static const char *const names[] = {
        "END", "JUMP", "BEGIN", "CALL", "SIGNAL_EVENT", "REPORT_COUNTER", "COPY_B2B", "UPDATE_UB", "SET_RT",
        "CLEAR_COLOR", "CLEAR_DS", "VIEWPORTS", "SCISSOR", "DEPTH_RANGE", "BIND_PROGRAM", "BIND_STATE", "BIND_VATTR",
        "BIND_VSTREAM", "BIND_VBUF", "BIND_UBO", "BIND_SSBO", "BIND_TEX", "BIND_SAMP", "STENCIL", "POLY_OFFSET", "DRAW",
        "DRAW_IDX", "DISPATCH", "BARRIER", "SET_TEX_POOL", "SET_SAMP_POOL", "COPY_B2T", "COPY_STRIDE", "BLEND_COLOR",
        "COPY_T2B", "BIND_IMG", "COPY_T2T", "RESET_COUNTER", "CLEAR_TEX", "DEPTH_CLAMP", "RENDER_ENABLE",
        "RASTER_DISCARD", "RENDER_COND", "PRIM_RESTART", "DRAW_IDX_IND", "DISPATCH_IND", "CLEAR_BUF", "FENCE_SYNC",
    };
    _Static_assert(sizeof names / sizeof *names == OP_COUNT_, "one name per op");
    return op < OP_COUNT_ ? names[op] : "?";
}
