// The Vulkan graphics back end: gfx_vulkan_backend wires the memory and
// texture layer (vk_memory.c, vk_texture.c), submission and fences (vk_sync.c) and the present. Every entry starts the
// device on first use.
//
// Present: the game texture is resolved on the calling thread and its blit is handed to a present thread, so
// nvnQueuePresentTexture returns at once (section 5). Queue order does the rest: the submissions that wrote the texture
// are already on gfx_queue, and every call that may write or free GPU state the blit reads (submit, finish, pool and
// texture teardown, texel writes) first waits until the queued blit was submitted (vk_present_drain).
//
// Back-edge pacing: the present thread submits the blit at once, then holds the host present (vkQueuePresentKHR) until
// the frame's display time from the pacer (nvn_api_obj.c), so present-to-present intervals stay even whatever the
// replay took. The blit is already on the queue, so the wait blocks no submission.
//
// Frame generation (vk_fg.cpp, frame_gen = fsr3): a frame that comes with a generated picture (the one between the
// last frame and this) is presented as a pair, both blits submitted at once (present_pair). The display shows a
// picture when the GPU has drawn it, not when the CPU hands it over, and both pictures of a pair are drawn within a
// millisecond of each other: spaced by the CPU clock alone they went on screen together and then left a gap (Savage
// Wind on FIFO and G-SYNC). So each present waits on the timeline until the GPU is done: the generated picture once
// the frame is drawn (and not before the pacer's display time), the game's half the smoothed interval between
// generated pictures after it, as AMD's frame interpolation swapchain and Savage Wind's present_wait_turn do. The game
// draws at half the frame limit meanwhile (frame_rate.c frame_step_cap_ns, gfx_frame_gen_live). The spacing is ported
// from Savage Wind (https://github.com/Revivedx/SavageWindRecomp @ bf6146c14c37fee8781a601e5a544b93dff6d75e, MIT,
// Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors, Copyright (c) 2026 Peiikai),
// runtime/nvn/vulkan/device.c present_wait_turn.
//
// Asynchronous replay (TOTK_REPLAY_ASYNC, default 0; 1 turns it on): nvnQueueSubmitCommands
// and nvnQueuePresentTexture only queue. Each item gets its timeline value at enqueue (vk_sync_reserve), in queue
// order; the replay thread runs the synchronous submit and present bodies in that order, so every value is signalled
// by the submission it was handed out for. The game runs at most one frame ahead: queueing present N waits until the
// replay thread handled present N-1. Calls that read or free GPU state that queued work may use drain the queue first
// (replay_drain), each cause counted.
//
// Two of those calls are queued instead (section "Queued pool teardown and texel writes"): nvnMemoryPoolFinalize
// (TOTK_REPLAY_QUEUE_POOL_FINI, default 1) and the image half of nvnTextureWriteTexels (TOTK_REPLAY_QUEUE_WRITE_TEXELS,
// default 1); 0 drains as before. Their guest-memory work happens at once, the rest in queue order, and the game-thread
// calls that could see the difference (a pool over a range being torn down, texel reads, CPU write-backs) drain only
// when their range overlaps a queued one.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "flight.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"
#include "vk_dlaa.h"
#include "vk_fg.h"

#define WEAK __attribute__((weak))

typedef struct { VkImage image, gen; VkFormat format; int w, h, has_crop, crop[4]; uint64_t frame, at, queued, value, pid; } PresentJob;   // pid: the frame's present ID (vk_reflex_frame); gen: its generated picture (vk_fg_present_gen)

static PlatMutex pl = PLAT_MUTEX_INIT;
static PlatCond pcv = PLAT_COND_INIT;   // job queued, job taken, job submitted
static PresentJob job;
static int pending, busy, present_thread;

// Sleeps on a high-resolution timer to 1 ms before at, then yields until at: the timer alone wakes up to ~0.5 ms late.
static void wait_until(uint64_t at) {
    uint64_t now = kern_now_ns();
    if (at > now + 1500000) plat_wait_ns(at - now - 1000000);
    while (kern_now_ns() < at) plat_yield();
}

// ---- Frame generation's presents (present thread only)
static volatile int32_t fg_live;   // the last frame went out with a generated picture before it (gfx_frame_gen_live)
int gfx_frame_gen_live(void) { return __atomic_load_n(&fg_live, __ATOMIC_RELAXED); }

// Present intervals in the world while frame generation is on, logged every FS_EVERY presents and over the run: all of them, from
// a generated picture to the game's frame after it, and from the game's frame to the next generated picture.
// Fixed-size histograms of 0.05 ms bins (bounded whatever the session's length).
#define FS_BINS 2000
#define FS_BIN_MS 0.05
#define FS_EVERY 2400
enum { FS_ALL, FS_GEN_REAL, FS_REAL_GEN, FS_N };
typedef struct {
    uint32_t hist[FS_N][FS_BINS];
    double sum[FS_N], sq[FS_N];
    uint64_t n[FS_N];
    uint64_t pairs, alone, t0, t1;   // frames presented as a pair and alone, the window's first and last present
    double gpu_wait, late;           // ms: waited for the GPU before the generated picture; the game's frame past its time
} FgStats;
static FgStats fs_win, fs_run;
static uint64_t fs_last, fs_last_gen;   // the last present (0 after a gap) and the last generated one
static double fs_iv;                    // ns between generated pictures, smoothed (factor 0.1, Savage Wind's)

static void fs_add(FgStats *s, int k, double ms) {
    unsigned b = ms <= 0 ? 0 : ms / FS_BIN_MS >= FS_BINS ? FS_BINS - 1 : (unsigned)(ms / FS_BIN_MS);
    s->hist[k][b]++, s->sum[k] += ms, s->sq[k] += ms * ms, s->n[k]++;
}

static double fs_p99(const FgStats *s, int k) {
    uint64_t want = s->n[k] - s->n[k] / 100, got = 0;
    for (unsigned b = 0; b < FS_BINS; b++)
        if ((got += s->hist[k][b]) >= want) return (b + 1) * FS_BIN_MS;
    return FS_BINS * FS_BIN_MS;
}

static void fs_log(const FgStats *s, const char *what) {
    if (!s->n[FS_ALL] || s->t1 <= s->t0) return;
    double wall = (s->t1 - s->t0) / 1e9, m[FS_N], sd[FS_N];
    for (int k = 0; k < FS_N; k++) {
        m[k] = s->n[k] ? s->sum[k] / s->n[k] : 0;
        double v = s->n[k] ? s->sq[k] / s->n[k] - m[k] * m[k] : 0;
        sd[k] = v > 0 ? sqrt(v) : 0;
    }
    rt_log("fg present %s: %.1f fps shown, %.1f drawn (%llu frames as pairs, %llu alone) over %.1f s; intervals mean %.3f "
           "sd %.3f p99 %.2f ms; generated to game's %.3f sd %.3f, game's to generated %.3f sd %.3f; waited for the GPU "
           "%.3f ms a pair, game's frame late %.3f ms a pair (means)", what, (s->n[FS_ALL] + 1) / wall,
           (s->pairs + s->alone) / wall, (unsigned long long)s->pairs, (unsigned long long)s->alone, wall, m[FS_ALL],
           sd[FS_ALL], fs_p99(s, FS_ALL), m[FS_GEN_REAL], sd[FS_GEN_REAL], m[FS_REAL_GEN], sd[FS_REAL_GEN],
           s->pairs ? s->gpu_wait / s->pairs : 0, s->pairs ? s->late / s->pairs : 0);
}

// A present at now of kind k (FS_GEN_REAL: the game's frame after a generated picture; FS_REAL_GEN: a generated
// picture; FS_ALL: a frame alone)
static void fs_present(uint64_t now, int k) {
    if (!vk_fg_active()) return;
    if (frame_step_world_us() <= 0) {   // (the title screen and the first load: not counted)
        fs_last = 0;
        return;
    }
    FgStats *both[2] = {&fs_win, &fs_run};
    for (int i = 0; i < 2; i++) {
        FgStats *s = both[i];
        if (!s->t0) s->t0 = now;
        if (fs_last && now - fs_last < 100000000ull) {   // (a gap: a load, a menu's hitch)
            double ms = (now - fs_last) / 1e6;
            fs_add(s, FS_ALL, ms);
            if (k != FS_ALL) fs_add(s, k, ms);
        }
        s->t1 = now;
        s->pairs += k == FS_GEN_REAL, s->alone += k == FS_ALL;
    }
    fs_last = now;
    if (fs_win.n[FS_ALL] >= FS_EVERY) {
        fs_log(&fs_win, "last presents");
        memset(&fs_win, 0, sizeof fs_win);
    }
}

static void gpu_wait(uint64_t value) {
    VkCtx *c = vk_ctx();
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                              .pSemaphores = &c->timeline, .pValues = &value};
    vkWaitSemaphores(c->device, &wi, 1000000000ull);   // (drawn; a second at most)
}

// The pair's two presents (both blits submitted): the generated picture once the GPU has drawn the frame (gpu: its
// last submission's value) and the pacer's time has come, the game's frame once its blit is drawn (f) and half the
// smoothed interval between generated pictures has passed.
static void present_pair(const PresentJob *j, uint64_t gpu, uint64_t f) {
    uint64_t t0 = kern_now_ns();
    gpu_wait(gpu);
    uint64_t t1 = kern_now_ns();
    wait_until(j->at);
    uint64_t now = kern_now_ns();
    double dt = fs_last_gen ? (double)(now - fs_last_gen) : 0;
    if (dt > 0 && dt < 1e8) fs_iv = fs_iv > 0 ? fs_iv + (dt - fs_iv) * 0.1 : dt;
    fs_last_gen = now;
    fs_present(now, FS_REAL_GEN);
    vk_present_flip();
    gpu_wait(f);
    uint64_t at = now + (uint64_t)(fs_iv / 2);
    if (fs_iv > 0) wait_until(at);
    uint64_t t2 = kern_now_ns();
    fs_present(t2, FS_GEN_REAL);
    vk_present_flip();
    fs_win.gpu_wait += (t1 - t0) / 1e6, fs_run.gpu_wait += (t1 - t0) / 1e6;
    if (fs_iv > 0 && t2 > at) fs_win.late += (t2 - at) / 1e6, fs_run.late += (t2 - at) / 1e6;
}

static void present_main(void *arg) {
    (void)arg;
    perf_core_thread(PCORE_PRESENT);
    uint64_t last = 0, last_queued = 0;
    plat_mutex_lock(&pl);
    for (;;) {
        while (!pending) plat_cond_wait(&pcv, &pl);
        PresentJob j = job;
        pending = 0;
        busy = 1;
        plat_cond_broadcast(&pcv);
        plat_mutex_unlock(&pl);
        if (flight_on) vk_thumbs_frame(j.frame);
        vk_reflex_present_frame(j.pid);   // the present of this blit, and the submission after it, are the frame's
        vk_reflex_submit_frame(j.pid);
        // the frame's last submission, its interpolation included: no submission goes out while busy
        uint64_t drawn = j.gen ? vk_sync_issued() : 0;
        int pair = j.gen && vk_present_blit_pair(j.gen, j.image, j.format, j.w, j.h, j.has_crop ? j.crop : NULL);
        if (!j.gen) vk_present_blit(j.image, VK_IMAGE_LAYOUT_GENERAL, j.format, j.w, j.h, j.has_crop ? j.crop : NULL);
        uint64_t f = vk_sync_submit(NULL, 0);   // a timeline value after the blit: deferred destruction waits for the blit too
        if (j.value && f != j.value)
            rt_fatal("vk: the present of frame %llu signalled %llu, queued as %llu", (unsigned long long)j.frame,
                     (unsigned long long)f, (unsigned long long)j.value);
        plat_mutex_lock(&pl);
        busy = 0;
        plat_cond_broadcast(&pcv);
        plat_mutex_unlock(&pl);
        uint64_t t0 = kern_now_ns();
        if (pair) {
            present_pair(&j, drawn, f);
        } else {
            wait_until(j.at);
            fs_last_gen = 0;   // (the next generated picture starts the spacing anew)
            fs_present(kern_now_ns(), FS_ALL);
        }
        uint64_t t1 = kern_now_ns();
        if (!pair) vk_present_flip();
        // TOTK_TRACE=present: one line per present: the host present's interval, the interval of the game's present
        // calls (when the host present went out before back-edge pacing), the wait and how far past its target it went
        if (rt_trace & RT_TRACE_PRESENT)
            rt_log("present: frame %llu interval %.3f ms, game %.3f ms, waited %.3f ms, %.3f ms past target",
                   (unsigned long long)j.frame, last ? (t1 - last) / 1e6 : 0.0,
                   last_queued ? (j.queued - last_queued) / 1e6 : 0.0, (t1 - t0) / 1e6,
                   j.at && t1 > j.at ? (t1 - j.at) / 1e6 : 0.0);
        last = t1;
        last_queued = j.queued;
        vk_pipeline_autosave();   // starts the autosave thread once
        plat_mutex_lock(&pl);
    }
    return;
}

void vk_present_drain(void) {
    uint64_t p0 = perf_begin();
    plat_mutex_lock(&pl);
    while (pending || busy) plat_cond_wait(&pcv, &pl);
    plat_mutex_unlock(&pl);
    perf_end(PERF_DRAIN, p0);
}

// The present after the frame count: resolves the image and hands the blit to the present thread. queued: when the game
// presented; value: the timeline value queued for this present (asynchronous replay), 0 for the next one (synchronous).
static void present_image(const tex_t *t, const int crop[4], uint64_t at_ns, uint64_t queued, uint64_t frame,
                          uint64_t value, uint64_t pid) {
    static volatile int32_t n_skipped;
    vk_exec_frame_log(t, frame);
    vk_dlaa_frame();
    vk_mvsurvey_frame();
    vk_mvprev_frame();
    vk_mem_census(frame);
    if (vk_dump_armed) vk_dump_present(frame);
    __atomic_store_n(&gfx_counters.validation_errors, (uint64_t)vk_validation_errors_total(), __ATOMIC_RELAXED);
    const VkTexImage *img = vk_texture_get(t);
    if (!img || !(img->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {
        if (plat_inc32(&n_skipped) <= 8)
            rt_log("vk: present of texture %p (format %#x) skipped: no colour image", (const void *)t, t ? t->d.format : 0);
        if (value) {   // the queued value is still signalled, in order
            vk_present_drain();
            uint64_t f = vk_sync_submit(NULL, 0);
            if (f != value) rt_fatal("vk: skipped present signalled %llu, queued as %llu", (unsigned long long)f, (unsigned long long)value);
        }
        return;
    }
    PresentJob j = {.image = img->image, .format = img->format, .w = (int)img->width, .h = (int)img->height, .frame = frame,
                    .at = at_ns, .queued = queued, .value = value, .pid = pid};
    vk_hdr_present_source(img, &j.image, &j.format);   // TOTK_HDR: the float picture the game copied into it, same size
    vk_fg_present_swap(&j.image, &j.format);   // TOTK_FG_TEST (frame generation checks): another picture in its place
    j.gen = vk_fg_present_gen(j.image);
    if (__atomic_load_n(&fg_live, __ATOMIC_RELAXED) != (j.gen != VK_NULL_HANDLE))
        __atomic_store_n(&fg_live, j.gen != VK_NULL_HANDLE, __ATOMIC_RELAXED);
    if (crop) {
        j.has_crop = 1;
        for (int i = 0; i < 4; i++) j.crop[i] = crop[i];
    }
    vk_upload_flush(0);   // an upload vk_texture_get just recorded goes before the blit
    plat_mutex_lock(&pl);
    if (!present_thread) {
        if (!plat_thread_spawn(present_main, NULL, "vk present", 0)) rt_fatal("vk: cannot start the present thread");
        present_thread = 1;
    }
    while (pending) plat_cond_wait(&pcv, &pl);   // one present in waiting at most
    job = j;
    pending = 1;
    plat_cond_broadcast(&pcv);
    plat_mutex_unlock(&pl);
    if (rdoc_active()) { vk_present_drain(); rdoc_after_present(frame); }
}

// ---- Asynchronous replay
enum { RI_SUBMIT, RI_PRESENT, RI_RELEASE, RI_POOL_FINI, RI_WRITE_TEXELS };
typedef struct {
    int kind, n, has_crop, crop[4];
    uint64_t value, submit_no, at, queued, pid;   // pid: the game's frame when it queued the item (present ID)
    uint64_t *h;
    const tex_t *tex;   // RI_RELEASE: a copy the item owns (the game may reuse the object's memory)
    const pool_t *pool; // RI_POOL_FINI: the pool object's address, the record's key; never dereferenced
    VkTexWrite *wt;     // RI_WRITE_TEXELS: the staged image write (vk_write_texels_begin)
    uint64_t pend;      // RI_POOL_FINI, RI_WRITE_TEXELS: its entry in the pending list
} ReplayItem;
#define RQ_CAP 1024   // a frame queues 1 submit and 1 present, and up to a few hundred releases
enum { DR_POOL_FINI, DR_MAP_VIRTUAL, DR_TEX_RELEASE, DR_PROG_RELEASE, DR_FINISH, DR_READ_TEXELS, DR_WRITE_TEXELS,
       DR_GUEST_READ, DR_FRAME, DR_FULL, DR_CHECK, DR_FINI_INIT, DR_FINI_TEXELS, DR_FINI_WB, DR_WT_READ, DR_WT_WB,
       DR_WT_CAP, DR_N };
static const char *const dr_name[DR_N] = {
    "pool_fini", "map_virtual", "texture_release (queued)", "program_release", "finish", "read_texels (writer fence)",
    "write_texels", "guest_read (writer fence)", "frame bound", "queue full", "check drain",
    "pool_init over a queued pool_fini", "texels over a queued pool_fini", "write-back over a queued pool_fini",
    "read_texels over a queued write_texels", "write-back over a queued write_texels", "write_texels staging cap"};

static PlatMutex rl = PLAT_MUTEX_INIT;
static PlatCond rcv = PLAT_COND_INIT;   // item queued, item taken, item done
static ReplayItem rq[RQ_CAP];
static int rq_head, rq_n, r_busy, r_started;
static uint64_t r_presents_queued, r_presents_done, r_submit_done, game_frames;
static uint64_t r_value_done;   // under rl: the newest timeline value the replay thread submitted
// Under rl: calls, calls that waited and the time waited per cause, since the last replay_log line and over the run.
static uint64_t dr_calls[DR_N], dr_waits[DR_N], dr_ns[DR_N], dr_tcalls[DR_N], dr_twaits[DR_N], dr_tns[DR_N];
// TOTK_REPLAY_CHECK: ReadTexels compared with a read after a full drain (under rl): checked, differing, differing
// although no submission recorded a write to the texture in between, differing with the queue idle at the first read.
static uint64_t ck_reads, ck_diff, ck_diff_unexplained, ck_diff_idle;

static int q_fini, q_wt;   // TOTK_REPLAY_QUEUE_POOL_FINI, TOTK_REPLAY_QUEUE_WRITE_TEXELS with the replay asynchronous

int vk_replay_async(void) {
    static int on = -1;
    if (__atomic_load_n(&on, __ATOMIC_ACQUIRE) < 0) {
        // Off by default. Where the game waits for the GPU it overlaps the replay with the game threads (a GTX 1650 in
        // the Depths went from 450-686 frames over 50 ms a minute to 10-13, issue #88), the only place a gain was
        // measured. While it was the default, a GTX 1660 Ti at the 60 fps cap with the GPU at 26 % reported uneven
        // motion that 0.3.4 did not have (#659). It adds up to a frame of latency.
        int v = cfg_bool("TOTK_REPLAY_ASYNC");
        q_fini = v && cfg_bool("TOTK_REPLAY_QUEUE_POOL_FINI");
        q_wt = v && cfg_bool("TOTK_REPLAY_QUEUE_WRITE_TEXELS");
        rt_log("vk: replay %s (TOTK_REPLAY_ASYNC=%d, TOTK_REPLAY_QUEUE_POOL_FINI=%d, TOTK_REPLAY_QUEUE_WRITE_TEXELS=%d)",
               v ? "on its own thread, at most one frame behind the game" : "on the submitting thread", v, q_fini, q_wt);
        __atomic_store_n(&on, v, __ATOMIC_RELEASE);
    }
    return on;
}

// The front end's view (nvn_api_core.c, nvn_api_cmd.c): replay is asynchronous only on this back end.
int gfx_replay_async(void) { return gfx_backend() == &gfx_vulkan_backend && vk_replay_async(); }

// ---- Queued pool teardown and texel writes
// A queued pool teardown or image write is in the pending list from the game thread's call until the replay thread
// finished it, so each entry has a queue item (or is the one being replayed): the list is bounded by the queue. The
// game-thread calls that could run into one drain the queue when their range overlaps it (pending_over), and only then.
enum { PD_FINI, PD_WRITE };
typedef struct { uint64_t id, lo, hi, bytes; const pool_t *key; int kind, phys; } Pending;
static Pending *pend;                   // under rl
static int npend, cpend, pend_n[2], pend_peak[2];   // under rl: entries, per kind now and the most since the last line
static uint64_t pend_next, pend_bytes, pend_bytes_peak;   // under rl: staging bytes the queued writes hold
static int32_t pend_any;                // npend, read without rl: an empty list costs one load
#define PEND_BYTES_CAP (256ull << 20)   // queued writes hold at most this much staging (DR_WT_CAP beyond it)

static uint64_t pending_add(int kind, uint64_t lo, uint64_t hi, const pool_t *key, int phys, uint64_t bytes) {
    plat_mutex_lock(&rl);
    if (npend == cpend) {
        cpend = cpend ? cpend * 2 : 64;
        pend = realloc(pend, sizeof *pend * (size_t)cpend);
        if (!pend) rt_fatal("vk: out of memory (replay queue)");
    }
    uint64_t id = ++pend_next;
    pend[npend++] = (Pending){id, lo, hi, bytes, key, kind, phys};
    if (++pend_n[kind] > pend_peak[kind]) pend_peak[kind] = pend_n[kind];
    pend_bytes += bytes;
    if (pend_bytes > pend_bytes_peak) pend_bytes_peak = pend_bytes;
    __atomic_store_n(&pend_any, npend, __ATOMIC_RELEASE);
    plat_mutex_unlock(&rl);
    return id;
}

static void pending_remove_locked(uint64_t id) {   // rl held
    for (int i = 0; i < npend; i++)
        if (pend[i].id == id) {
            pend_n[pend[i].kind]--;
            pend_bytes -= pend[i].bytes;
            pend[i] = pend[--npend];
            __atomic_store_n(&pend_any, npend, __ATOMIC_RELEASE);
            return;
        }
}

// 1 when an entry of kind overlaps [lo, hi): for a teardown also one of the same pool object (vk_memory.c index_put
// would drop its record unfreed) or, when virt (a texture in a virtual pool), any physical pool, whose memory such
// textures may sit on (vk_texture_range_gone's alloc test).
static int pending_over(int kind, uint64_t lo, uint64_t hi, const pool_t *key, int virt) {
    if (!__atomic_load_n(&pend_any, __ATOMIC_ACQUIRE)) return 0;
    int hit = 0;
    plat_mutex_lock(&rl);
    for (int i = 0; i < npend && !hit; i++) {
        const Pending *e = &pend[i];
        hit = e->kind == kind && ((e->lo < hi && lo < e->hi) || (key && e->key == key) || (virt && e->phys));
    }
    plat_mutex_unlock(&rl);
    return hit;
}

// 1 when a GPU-written mirrored texture over an entry of kind has stale storage: a write-back of all of them
// (vk_texture_write_back_small) would write it.
static int pending_stale(int kind) {
    if (!__atomic_load_n(&pend_any, __ATOMIC_ACQUIRE)) return 0;
    int hit = 0;
    plat_mutex_lock(&rl);   // before owned_lock (vk_texture_cpu_stale); texture code never takes rl
    for (int i = 0; i < npend && !hit; i++)
        hit = pend[i].kind == kind && vk_texture_cpu_stale(pend[i].lo, pend[i].hi - pend[i].lo);
    plat_mutex_unlock(&rl);
    return hit;
}

// The newest submit number (nvn_submit_count) whose replay is done: recording copies it used may be freed.
uint64_t gfx_replayed_submits(void) {
    if (!gfx_replay_async()) return nvn_submit_count();
    plat_mutex_lock_shared(&rl);
    uint64_t v = r_submit_done;
    plat_mutex_unlock_shared(&rl);
    return v;
}

// TOTK_SYNC_AHEAD (nvn_api_core.c nvnSyncWait): waits until the replay thread submitted value v, so every shadow copy
// of v was made before the game writes memory v reads. The caller has given its core up.
void gfx_replay_wait_value(uint64_t v) {
    if (!gfx_replay_async()) return;
    plat_mutex_lock(&rl);
    while (r_value_done < v) plat_cond_wait(&rcv, &rl);
    plat_mutex_unlock(&rl);
}

static uint64_t submit_sync(const uint64_t *handles, int n) {
    vk_sync_start();
    vk_present_drain();
    vk_texture_new_submit();
    return vk_sync_submit(handles, n);
}

static void replay_main(void *arg) {
    (void)arg;
    perf_replay_thread();
    perf_core_thread(PCORE_REPLAY);
    {   // TOTK_REPLAY_SPINS (4096 pauses, about 80 us; 0: the default of every thread): see plat_lock_spins_thread
        const char *e = cfg_get("TOTK_REPLAY_SPINS");
        plat_lock_spins_thread(e && *e ? atoi(e) : 4096);
    }
    plat_mutex_lock(&rl);
    for (;;) {
        while (!rq_n) plat_cond_wait(&rcv, &rl);
        ReplayItem it = rq[rq_head];
        rq_head = (rq_head + 1) % RQ_CAP;
        rq_n--;
        r_busy = 1;
        plat_cond_broadcast(&rcv);
        plat_mutex_unlock(&rl);
        if (it.kind == RI_SUBMIT) {
            vk_reflex_submit_frame(it.pid);   // the replay thread submits the frame the game was on when it queued it
            uint64_t f = submit_sync(it.h, it.n);
            if (f != it.value)
                rt_fatal("vk: replayed submission signalled %llu, queued as %llu", (unsigned long long)f,
                         (unsigned long long)it.value);
            free(it.h);
        } else if (it.kind == RI_RELEASE) {
            vk_present_drain();
            vk_texture_release(it.tex);
            free((void *)it.tex);
        } else if (it.kind == RI_POOL_FINI) {
            vk_present_drain();   // the blit may read an image cut from the pool
            vk_pool_fini_queued(it.pool);
        } else if (it.kind == RI_WRITE_TEXELS) {
            vk_present_drain();   // the blit may read the image the copy writes
            vk_write_texels_queued(it.wt);
        } else {
            uint64_t frame = __atomic_add_fetch(&gfx_counters.frames, 1, __ATOMIC_RELAXED);
            present_image(it.tex, it.has_crop ? it.crop : NULL, it.at, it.queued, frame, it.value, it.pid);
        }
        plat_mutex_lock(&rl);
        if (it.kind == RI_SUBMIT || it.kind == RI_PRESENT) r_value_done = it.value;
        if (it.kind == RI_SUBMIT) r_submit_done = it.submit_no;
        else if (it.kind == RI_PRESENT || it.kind == RI_RELEASE) r_presents_done++;
        else pending_remove_locked(it.pend);
        r_busy = 0;
        plat_cond_broadcast(&rcv);
    }
    return;
}

static void wait_counted(int why, uint64_t t0) {   // rl held
    uint64_t d = kern_now_ns() - t0;
    dr_waits[why]++, dr_twaits[why]++;
    dr_ns[why] += d, dr_tns[why] += d;
    perf_end(PERF_DRAIN, t0);
}

// A guest thread waiting for the replay gives its virtual core up, as around any blocking host wait (NVN_BLOCKING):
// guest_read (memcpy, pool invalidate), pool and texture finalize run without it, and a drain lasts up to a replay.
// Called and returning with rl held; rl is not held while the core is taken back.
static int core_away_unlocked(void) {
    plat_mutex_unlock(&rl);
    int away = kern_core_away();
    plat_mutex_lock(&rl);
    return away;
}
static void core_back_unlocked(int *away) {
    if (!*away) return;
    plat_mutex_unlock(&rl);
    kern_core_back(away);
    plat_mutex_lock(&rl);
}

// Queues an item (waits while the queue is full) and returns its value, reserved under rl so values follow queue order.
static uint64_t replay_enqueue(ReplayItem *it) {
    plat_mutex_lock(&rl);
    if (!r_started) {
        if (!plat_thread_spawn(replay_main, NULL, "vk replay", 0)) rt_fatal("vk: cannot start the replay thread");
        r_started = 1;
    }
    // rl is dropped while the core is taken back, and another thread may fill the queue again meanwhile: check again.
    while (rq_n == RQ_CAP) {
        uint64_t p0 = perf_begin();
        dr_calls[DR_FULL]++, dr_tcalls[DR_FULL]++;
        int away = core_away_unlocked();
        while (rq_n == RQ_CAP) plat_cond_wait(&rcv, &rl);
        wait_counted(DR_FULL, p0);
        core_back_unlocked(&away);
    }
    it->value = it->kind == RI_SUBMIT || it->kind == RI_PRESENT ? vk_sync_reserve() : 0;   // the others signal nothing
    rq[(rq_head + rq_n) % RQ_CAP] = *it;
    rq_n++;
    plat_cond_broadcast(&rcv);
    plat_mutex_unlock(&rl);
    return it->value;
}

// Returns once every queued item was replayed and submitted. Back-end entries only: the replay thread never calls them.
static void replay_drain(int why) {
    if (!gfx_replay_async()) return;
    plat_mutex_lock(&rl);
    dr_calls[why]++, dr_tcalls[why]++;
    if (rq_n || r_busy) {
        uint64_t p0 = perf_begin();
        int away = core_away_unlocked();
        while (rq_n || r_busy) plat_cond_wait(&rcv, &rl);
        wait_counted(why, p0);
        core_back_unlocked(&away);
    }
    plat_mutex_unlock(&rl);
}

// What made the game wait for the replay: since the last line (every 1800 game frames), or over the run (at exit).
static void replay_log(uint64_t frame, int total) {
    char buf[2048];
    int k = 0;
    plat_mutex_lock(&rl);
    const uint64_t *c = total ? dr_tcalls : dr_calls, *w = total ? dr_twaits : dr_waits, *t = total ? dr_tns : dr_ns;
    for (int i = 0; i < DR_N && k < (int)sizeof buf - 96; i++)
        if (c[i] || w[i])
            k += snprintf(buf + k, sizeof buf - (size_t)k, "%s %s %llu calls, %llu waited %.1f ms", k ? ";" : "", dr_name[i],
                          (unsigned long long)c[i], (unsigned long long)w[i], t[i] / 1e6);
    if ((q_fini || q_wt) && k < (int)sizeof buf - 200)
        k += snprintf(buf + k, sizeof buf - (size_t)k,
                      "%s queued now/most: pool_fini %d/%d, write_texels %d/%d (%.1f/%.1f MiB staging); dying-pool lookups "
                      "(run) %llu", k ? ";" : "", pend_n[PD_FINI], pend_peak[PD_FINI], pend_n[PD_WRITE], pend_peak[PD_WRITE],
                      pend_bytes / 1048576.0, pend_bytes_peak / 1048576.0, (unsigned long long)vk_mem_dying_resolves());
    if (!total) pend_peak[PD_FINI] = pend_n[PD_FINI], pend_peak[PD_WRITE] = pend_n[PD_WRITE], pend_bytes_peak = pend_bytes;
    if (ck_reads && k < (int)sizeof buf - 128)
        k += snprintf(buf + k, sizeof buf - (size_t)k, "; check (run): %llu reads, %llu differ, %llu without a recorded write, %llu with the queue idle",
                      (unsigned long long)ck_reads, (unsigned long long)ck_diff, (unsigned long long)ck_diff_unexplained,
                      (unsigned long long)ck_diff_idle);
    memset(dr_calls, 0, sizeof dr_calls);
    memset(dr_waits, 0, sizeof dr_waits);
    memset(dr_ns, 0, sizeof dr_ns);
    plat_mutex_unlock(&rl);
    rt_log("vk: replay queue %s frame %llu:%s", total ? "over the run to" : "at", (unsigned long long)frame,
           k ? buf : " no waits");
}

// The run total, once: the game exits through prof_write (boot.c _exit, the window close), probes through
// vk_backend_shutdown.
void gfx_exit_log(void) {
    static int32_t done;
    rec_check_log();
    vk_fg_exit_log();
    static int32_t fs_done;
    if (!plat_xchg32(&fs_done, 1)) fs_log(&fs_run, "over the run");
    vk_texture_eager_log();
    vk_mem_shadow_read_log();
    vk_ahead_log_final();
    vk_ahead_log_guards();
    vk_counter_log();
    if (gfx_replay_async() && r_started && !plat_xchg32(&done, 1)) replay_log(game_frames, 1);
}

static void vk_be_present(const tex_t *t, const int crop[4], uint64_t at_ns) {
    static _Thread_local int pcore;   // the game's render thread: the one that presents
    if (!pcore) pcore = 1, perf_core_thread(PCORE_RENDER);
    vk_dlaa_game_present();   // the camera jitter moves on with the game's frames (vk_dlaa.c)
    vk_sync_start();
    if (!gfx_replay_async()) {
        uint64_t frame = __atomic_add_fetch(&gfx_counters.frames, 1, __ATOMIC_RELAXED);
        perf_present(frame);   // before vk_exec_frame_log, which resets the frame's exec counts the flight recorder reads
        present_image(t, crop, at_ns, kern_now_ns(), frame, 0, vk_reflex_frame());
        return;
    }
    // Frame bound: the replay thread handled the previous present before this one is queued.
    plat_mutex_lock(&rl);
    dr_calls[DR_FRAME]++, dr_tcalls[DR_FRAME]++;
    if (r_presents_done < r_presents_queued) {
        uint64_t p0 = perf_begin();
        while (r_presents_done < r_presents_queued) plat_cond_wait(&rcv, &rl);
        wait_counted(DR_FRAME, p0);
    }
    r_presents_queued++;
    plat_mutex_unlock(&rl);
    uint64_t frame = ++game_frames;
    perf_present(frame);
    if (frame % 1800 == 0) replay_log(frame, 0);
    ReplayItem it = {.kind = RI_PRESENT, .tex = t, .at = at_ns, .queued = kern_now_ns(), .pid = vk_reflex_frame()};
    if (crop) {
        it.has_crop = 1;
        for (int i = 0; i < 4; i++) it.crop[i] = crop[i];
    }
    replay_enqueue(&it);
}

// vk_pipeline.c drops a released program's pipelines here.
WEAK void vk_program_release(const prog_t *p) { (void)p; }

// A pool over a range whose teardown is queued, or the same object again, waits for that teardown: its record would
// share the index with the dying one, and vk_memory.c index_put drops a record of the same object unfreed. Pools that
// get no record (vk_memory.c vk_pool_init: GPU-inaccessible and not physical, or empty) need no wait.
static int vk_be_pool_init(pool_t *p) {
    vk_sync_start();
    int rec = p->size && (!(p->flags & NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS) || (p->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL));
    if (rec && pending_over(PD_FINI, (uintptr_t)p->mem, (uintptr_t)p->mem + p->size, p, 0)) replay_drain(DR_FINI_INIT);
    return vk_pool_init(p);
}
// Queued (TOTK_REPLAY_QUEUE_POOL_FINI): the record is marked dying and stays for the items queued before, the replay
// thread tears it down in queue order without waiting for the GPU (vk_memory.c vk_pool_fini_begin, vk_pool_fini_queued).
// The game gets the storage back now, as from the synchronous teardown: the game-thread calls that could touch it
// through the dying record's images drain on overlap (pending_over, pending_stale).
static void vk_be_pool_fini(pool_t *p) {
    vk_sync_start();
    if (!gfx_replay_async() || !q_fini) {
        replay_drain(DR_POOL_FINI);
        vk_present_drain();
        vk_pool_fini(p);
        return;
    }
    plat_mutex_lock(&rl);
    dr_calls[DR_POOL_FINI]++, dr_tcalls[DR_POOL_FINI]++;
    plat_mutex_unlock(&rl);
    uint64_t lo, size;
    int phys;
    if (!vk_pool_fini_begin(p, &lo, &size, &phys)) return;   // no record: vk_pool_fini would do nothing
    ReplayItem it = {.kind = RI_POOL_FINI, .pool = p, .pend = pending_add(PD_FINI, lo, lo + size, p, phys, 0)};
    replay_enqueue(&it);
}
static int vk_be_pool_map_virtual(pool_t *v, int n, const void *requests) {
    vk_sync_start();
    replay_drain(DR_MAP_VIRTUAL);
    vk_present_drain();
    return vk_pool_map_virtual(v, n, requests);
}
static void vk_be_texture_register(int id, const tex_t *t, const view_t *v) { vk_sync_start(); vk_texture_register(id, t, v); }
static void vk_be_sampler_register(int id, const samp_par *s) { vk_sync_start(); vk_sampler_register(id, s); }
// Asynchronous: the release is queued behind the submissions before it, so their replay still resolves the texture's
// slots to its image (a released slot follows another live texture at the storage), and the ones after it see it
// released, the order of the synchronous replay. vk_texture_release reads only the object's key fields: a copy.
static void vk_be_texture_release(const tex_t *t) {
    vk_sync_start();
    if (!gfx_replay_async()) {
        vk_present_drain();
        vk_texture_release(t);
        return;
    }
    if (!t || !t->mem) return;
    tex_t *copy = malloc(sizeof *copy);
    if (!copy) rt_fatal("vk: out of memory (replay queue)");
    *copy = *t;
    ReplayItem it = {.kind = RI_RELEASE, .tex = copy};
    plat_mutex_lock(&rl);
    dr_calls[DR_TEX_RELEASE]++, dr_tcalls[DR_TEX_RELEASE]++;
    plat_mutex_unlock(&rl);
    replay_enqueue(&it);
}
static void vk_be_program_release(const prog_t *p) { replay_drain(DR_PROG_RELEASE); vk_program_release(p); }
// Asynchronous: the handles (nvn_submit_prepare_async ran under the front end's submit lock) are copied and queued;
// the value returned is the one the replay will signal.
static uint64_t vk_be_submit(const uint64_t *handles, int n) {
    if (!gfx_replay_async()) return submit_sync(handles, n);
    vk_sync_start();
    ReplayItem it = {.kind = RI_SUBMIT, .n = n, .submit_no = nvn_submit_count(), .pid = vk_reflex_frame()};
    if (n > 0) {
        it.h = malloc(sizeof *it.h * (size_t)n);
        if (!it.h) rt_fatal("vk: out of memory (replay queue)");
        memcpy(it.h, handles, sizeof *it.h * (size_t)n);
    }
    return replay_enqueue(&it);
}
static void vk_be_finish(void) { vk_sync_start(); replay_drain(DR_FINISH); vk_present_drain(); vk_sync_finish(); }
static int replay_check(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("TOTK_REPLAY_CHECK");
        on = e && strcmp(e, "0");
        if (on) rt_log("vk: TOTK_REPLAY_CHECK: every ReadTexels is read again after a full replay drain and compared");
    }
    return on;
}

// ReadTexels with the replay queued: waits for the newest submission recorded to write the texture (replayed and
// completed), not for the whole queue. The readback copy goes through the upload batch on the same queue, after every
// submission the replay has submitted. Submissions still queued or being recorded run after the copy, as on hardware
// when the GPU has not reached them yet: the game only sees their texels after waiting on a sync, and every sync
// value covers replay and GPU, so a read the game synchronised sees them. TOTK_REPLAY_CHECK compares with a full drain.
static void vk_be_read_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row,
                              uint64_t img) {
    vk_sync_start();
    if (!gfx_replay_async()) {
        vk_read_texels(t, v, r, dst, row, img);
        return;
    }
    uint64_t lo = (uintptr_t)t->mem, hi = lo + (t->L.size ? t->L.size : 1);
    int virt = (t->pool_flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) != 0;
    if (pending_over(PD_FINI, lo, hi, NULL, virt)) replay_drain(DR_FINI_TEXELS);   // the entry may be the dying pool's
    if (pending_over(PD_WRITE, lo, hi, NULL, 0)) replay_drain(DR_WT_READ);        // the image lacks the queued write
    uint64_t f = vk_texture_last_write(t);
    plat_mutex_lock(&rl);
    dr_calls[DR_READ_TEXELS]++, dr_tcalls[DR_READ_TEXELS]++;
    int idle = !rq_n && !r_busy;
    plat_mutex_unlock(&rl);
    if (f && vk_sync_fence_done() < f) {
        uint64_t t0 = kern_now_ns();
        vk_sync_fence_wait(f, UINT64_MAX);   // counts as a fence wait in perf
        uint64_t d = kern_now_ns() - t0;
        plat_mutex_lock(&rl);
        dr_waits[DR_READ_TEXELS]++, dr_twaits[DR_READ_TEXELS]++;
        dr_ns[DR_READ_TEXELS] += d, dr_tns[DR_READ_TEXELS] += d;
        plat_mutex_unlock(&rl);
    }
    vk_read_texels(t, v, r, dst, row, img);
    if (!replay_check()) return;
    size_t n = vk_texture_texel_span(t, v, r, row, img);
    uint8_t *again = n ? malloc(n) : NULL;
    if (!again) return;
    memcpy(again, dst, n);   // bytes between the rows stay the caller's
    uint64_t f0 = vk_texture_last_write(t);
    replay_drain(DR_CHECK);
    uint64_t f1 = vk_texture_last_write(t);
    vk_read_texels(t, v, r, again, row, img);
    int differ = memcmp(again, dst, n) != 0;
    static volatile int32_t n_logged;
    if (differ && plat_inc32(&n_logged) <= 40) {
        size_t at = 0;
        while (at < n && again[at] == dst[at]) at++;
        rt_log("vk: replay check: ReadTexels of %p (format %#x, %dx%dx%d at %d,%d,%d) differs at byte %zu of %zu; writer "
               "fence %llu at the read, %llu after the drain, done %llu; queue %s at the read",
               (void *)t->mem, t->d.format, r->width, r->height, r->depth, r->xoffset, r->yoffset, r->zoffset, at, n,
               (unsigned long long)f0, (unsigned long long)f1, (unsigned long long)vk_sync_fence_done(), idle ? "idle" : "busy");
    }
    plat_mutex_lock(&rl);
    ck_reads++;
    if (differ) {
        ck_diff++;
        if (f1 == f0) ck_diff_unexplained++;
        if (idle) ck_diff_idle++;
    }
    plat_mutex_unlock(&rl);
    free(again);
}
// Queued (TOTK_REPLAY_QUEUE_WRITE_TEXELS): guest memory is written now and the image write staged, and the replay
// thread records the copy in queue order (vk_texture.c vk_write_texels_begin, vk_write_texels_queued): the upload batch
// it goes into is submitted before the next replayed submission (vk_sync.c), after the ones before it, the order the
// drain gave. A texture over a queued teardown waits for it, as its entry may be the dying pool's; queued writes hold at
// most PEND_BYTES_CAP of staging.
static void vk_be_write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row,
                               uint64_t img) {
    vk_sync_start();
    if (!gfx_replay_async() || !q_wt) {
        replay_drain(DR_WRITE_TEXELS);
        vk_present_drain();
        vk_write_texels(t, v, r, src, row, img);
        return;
    }
    uint64_t lo = (uintptr_t)t->mem, hi = lo + (t->L.size ? t->L.size : 1);
    plat_mutex_lock(&rl);
    dr_calls[DR_WRITE_TEXELS]++, dr_tcalls[DR_WRITE_TEXELS]++;
    int full = pend_bytes > PEND_BYTES_CAP;
    plat_mutex_unlock(&rl);
    if (full) replay_drain(DR_WT_CAP);
    if (pending_over(PD_FINI, lo, hi, NULL, (t->pool_flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) != 0))
        replay_drain(DR_FINI_TEXELS);
    uint64_t bytes = 0;
    VkTexWrite *w = vk_write_texels_begin(t, v, r, src, row, img, &bytes);
    if (!w) return;
    ReplayItem it = {.kind = RI_WRITE_TEXELS, .wt = w, .pend = pending_add(PD_WRITE, lo, hi, NULL, 0, bytes)};
    replay_enqueue(&it);
}
static void vk_be_guest_written(uint64_t addr, uint64_t size) { vk_sync_start(); vk_guest_written(addr, size); }
// A CPU read of GPU-written mirrored storage (memcpy, pool invalidate). With the replay queued it waits for the newest
// submission recorded to write the textures there, not for the whole queue: the write-back copy then runs after their
// renders, and clears gpu_dirty only for content that is on the queue. Submissions not replayed yet render after the
// copy, as on hardware when the GPU has not reached them; they mark the texture dirty again for a later read.
static void vk_be_guest_read(uint64_t addr, uint64_t size) {
    if (!vk_texture_cpu_stale(addr, size)) return;
    vk_sync_start();
    if (gfx_replay_async()) {
        // A write-back of an image a queued teardown drops, or one that lacks a queued write, would put stale texels
        // into storage the game reads: the queue goes first.
        if (pending_over(PD_FINI, addr, addr + size, NULL, 0)) replay_drain(DR_FINI_WB);
        if (pending_over(PD_WRITE, addr, addr + size, NULL, 0)) replay_drain(DR_WT_WB);
        uint64_t f = vk_texture_dirty_fence(addr, size);
        plat_mutex_lock(&rl);
        dr_calls[DR_GUEST_READ]++, dr_tcalls[DR_GUEST_READ]++;
        plat_mutex_unlock(&rl);
        if (f && vk_sync_fence_done() < f) {
            uint64_t t0 = kern_now_ns();
            NVN_BLOCKING();
            vk_sync_fence_wait(f, UINT64_MAX);
            uint64_t d = kern_now_ns() - t0;
            plat_mutex_lock(&rl);
            dr_waits[DR_GUEST_READ]++, dr_twaits[DR_GUEST_READ]++;
            dr_ns[DR_GUEST_READ] += d, dr_tns[DR_GUEST_READ] += d;
            plat_mutex_unlock(&rl);
        }
    }
    vk_present_drain();
    vk_texture_write_back(addr, size);
}
// After a fence wait: the value is done, so its replay is too; a later submission the replay records meanwhile marks
// its targets with a later fence, which write_back_small leaves alone. No drain.
static void vk_be_cpu_synced(uint64_t fence) {
    if (!vk_texture_small_dirty()) return;
    vk_sync_start();
    if (pending_stale(PD_FINI)) replay_drain(DR_FINI_WB);   // as in vk_be_guest_read, for every target written back
    if (pending_stale(PD_WRITE)) replay_drain(DR_WT_WB);
    vk_present_drain();
    vk_texture_write_back_small(fence);
}

const GfxBackend gfx_vulkan_backend = {
    .name = "vulkan",
    .pool_init = vk_be_pool_init,
    .pool_fini = vk_be_pool_fini,
    .pool_map_virtual = vk_be_pool_map_virtual,
    .texture_register = vk_be_texture_register,
    .sampler_register = vk_be_sampler_register,
    .texture_release = vk_be_texture_release,
    .program_release = vk_be_program_release,
    .submit = vk_be_submit,
    .fence_wait = vk_sync_fence_wait,
    .fence_done = vk_sync_fence_done,
    .present = vk_be_present,
    .finish = vk_be_finish,
    .read_texels = vk_be_read_texels,
    .write_texels = vk_be_write_texels,
    .guest_written = vk_be_guest_written,
    .guest_read = vk_be_guest_read,
    .cpu_synced = vk_be_cpu_synced,
};

void vk_backend_shutdown(void) {
    replay_drain(DR_FINISH);
    gfx_exit_log();
    vk_present_drain();
    vk_sync_shutdown();
    vk_mem_shutdown();
    vk_shutdown();
}
