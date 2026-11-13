// The recorder thread: see vk_rec.h. One producer (the submitting thread, between rec_begin and rec_end) and one
// consumer (the recorder) share a ring of 64-bit words. A packet is a header word (op, length in words) and the
// call's arguments; its pointers point into the packet itself, so the recorder passes the payload to the driver as
// it is. A packet never wraps: one that does not fit before the end is preceded by a pad. The producer publishes head
// after each packet; the recorder makes the calls and advances tail, which the producer waits on when the ring is
// full and in rec_drain. While a part is recorded the recorder spins on head; idle, it sleeps on an event. Packets also
// begin, end and submit the parts (rec_begin, rec_end_cb, rec_submit), so each part stays on the recorder throughout.
// Design: Savage Wind Recomp #189 and LAS Recomp runtime/nvn/vulkan/record.c (https://github.com/peiikai/LASRecomp
// @ a4b53a3, MIT, Copyright (c) 2026 Peiikai); the code is ours.
#define VK_REC_IMPL
#include <immintrin.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "platform/platform.h"

#define RING_WORDS (1u << 21)   // 16 MiB: a part of a submission is ~1-2 MiB of packets in the town
static uint64_t ring[RING_WORDS];
// The queue state, a cache line per writer. The recorder spins on head while it waits for packets, so a line the
// producer writes on every packet and the recorder reads or writes goes back and forth between their cores each time:
// with tail, Q.put_h, head and the producer's statistics in one line (the linker's order of these statics), handing
// packets over took 6% of the replay thread in the fort, more than building the descriptors.
static struct {
    _Alignas(64) volatile uint64_t head;      // words published by the producer; the recorder reads it
    _Alignas(64) volatile uint64_t tail;      // words consumed by the recorder; the producer reads it when it seems full
    _Alignas(64) volatile int32_t sleeping;   // the recorder waits for wake_ev; written only around a sleep
    _Alignas(64) uint64_t put_h;              // producer only: words written
    uint64_t pub_h;                           // the last head published
    uint64_t tail_seen;                       // tail as last read, at most the real one: the room it leaves is there
    uint64_t pub_words;                       // TOTK_REC_PUB: publish head once this many words wait (pub)
} Q;
static PlatEvent wake_ev;              // auto-reset
static VkCommandBuffer rec_cb;         // producer: the part being recorded, whose vkCmd* become packets
static VkCommandBuffer r_cb;           // recorder: the command buffer its packets go into (set by a BeginCb packet)
static int rec_live;                   // producer: rec_begin started the recorder for rec_cb
static int end_mode = -1;              // TOTK_REC_END (rec_own)
static int sub_pending;                // producer: the recorder ended a part that rec_submit has not handed over yet
static volatile VkResult r_sub_res;    // recorder: the result of its last vkQueueSubmit2, read after the drain
static volatile VkResult r_sub_err;    // recorder: the first vkQueueSubmit2 that failed (rec_submit reports it)
static volatile uint64_t q_subs;       // producer: submissions queued for the recorder (rec_submit)
static volatile uint64_t r_subs;       // recorder: submissions it handed to the driver (submit_run)
static _Thread_local int on_rec;       // the calling thread is the recorder
static int rec_async = -1;             // TOTK_REC_ASYNC (rec_submit)
static struct { uint64_t n, ns; } RQ_; // waits in rec_queue_wait, logged by rec_stats_log
static PlatThread *rec_thread;
static int rec_prio;                   // the submitting thread's priority, which the recorder takes
static int rec_chk;                    // TOTK_REC_CHECK (rec_check_handle)
static uint64_t rec_delay_ns;          // TOTK_REC_DELAY_US (rec_main)
static uint64_t rec_spin_ns;           // TOTK_REC_SPIN_US (rec_main); 0: 4096 pauses
static int rec_spin_ab;                // TOTK_REC_SPIN_US=ab: the timed poll in the B blocks of TOTK_AB only
static volatile uint64_t part_h0;      // Q.put_h at rec_begin: where the packets of the part being recorded start
static volatile uint64_t part_fence;   // the value of the part being recorded (rec_begin)
static uint64_t r_sleeps;              // recorder: sleeps on wake_ev, read by rec_stats_log

// Producer statistics, logged by rec_stats_log.
static struct { _Alignas(64) uint64_t packets, words, drains, fallbacks, drain_ns, full_waits, parts, parts_on, submits, wakes; } RS_;

enum {
    R_PAD, R_BeginQuery, R_BeginRendering, R_BindDescriptorSets, R_BindIndexBuffer, R_BindPipeline, R_BindVertexBuffers2,
    R_BlitImage, R_ClearAttachments, R_ClearColorImage, R_ClearDepthStencilImage, R_CopyBuffer, R_CopyBufferToImage,
    R_CopyImage, R_CopyImageToBuffer, R_Dispatch, R_DispatchIndirect, R_Draw, R_DrawIndexed, R_DrawIndexedIndirect,
    R_EndQuery, R_EndRendering, R_ExecuteCommands, R_FillBuffer, R_PipelineBarrier2, R_PushConstants, R_ResetQueryPool, R_ResolveImage,
    R_SetBlendConstants, R_SetCullMode, R_SetDepthBias, R_SetDepthBiasEnable, R_SetDepthCompareOp, R_SetDepthTestEnable,
    R_SetDepthWriteEnable, R_SetFrontFace, R_SetPrimitiveRestartEnable, R_SetPrimitiveTopology,
    R_SetRasterizerDiscardEnable, R_SetScissorWithCount, R_SetStencilCompareMask, R_SetStencilOp, R_SetStencilReference,
    R_SetStencilTestEnable, R_SetStencilWriteMask, R_SetViewportWithCount, R_WriteTimestamp, R_WriteTimestamp2,
    R_PushTpl, R_Push, R_VertexInput, R_Bool, R_PolygonMode, R_BlendEnable, R_BlendEquation, R_WriteMask, R_EndCb,
    R_PassMark, R_BeginCb, R_Submit,
};

static uint64_t now_ns(void) { return plat_now_ns(); }

// ---- consumer

static void exec_packet(uint32_t op, void *p);

static void rec_main(void *arg) {
    (void)arg;
    plat_thread_priority(rec_prio);
    on_rec = 1;
    perf_core_thread(PCORE_RECORDER);
    uint64_t t = 0;
    for (;;) {
        uint64_t h = __atomic_load_n(&Q.head, __ATOMIC_ACQUIRE);
        if (t == h) {
            // Poll for the next packets before sleeping: a sleep costs the producer an event set and the recorder
            // an OS wake-up. The poll is timed (TOTK_REC_SPIN_US, default 60) because a pause takes 8-150 cycles
            // depending on the CPU: 4096 of them are ~140 us on a Coffee Lake i7 and a few us on Zen 4. With fewer than
            // 6 logical processors it stays 4096 pauses. Savage Wind Recomp's recorder polls 60 us the same way.
            if (rec_spin_ns && (!rec_spin_ab || rt_ab())) {
                uint64_t t0 = now_ns();
                for (unsigned i = 1; t == h; i++) {
                    _mm_pause();
                    h = __atomic_load_n(&Q.head, __ATOMIC_ACQUIRE);
                    if (!(i & 31) && now_ns() - t0 >= rec_spin_ns) break;
                }
            } else {
                for (int i = 0; i < 4096 && t == h; i++) _mm_pause(), h = __atomic_load_n(&Q.head, __ATOMIC_ACQUIRE);
            }
            if (t == h) {
                __atomic_store_n(&Q.sleeping, 1, __ATOMIC_SEQ_CST);
                h = __atomic_load_n(&Q.head, __ATOMIC_SEQ_CST);
                if (t == h) {
                    __atomic_add_fetch(&r_sleeps, 1, __ATOMIC_RELAXED);
                    plat_event_wait_ns(&wake_ev, 2000000);   // a missed wake costs at most 2 ms
                }
                __atomic_store_n(&Q.sleeping, 0, __ATOMIC_RELAXED);
                continue;
            }
        }
        // TOTK_REC_DELAY_US (debug): the recorder falls this far behind each batch it finds, as on a slow machine,
        // so objects the replay lets go of meet packets it has not made yet (TOTK_REC_CHECK).
        if (rec_delay_ns) plat_sleep_ns(rec_delay_ns);
        for (int k = 0; t != h; k++) {
            uint64_t w = ring[t % RING_WORDS];
            uint32_t op = (uint32_t)(w & 0xffff), n = (uint32_t)(w >> 32);
            if (op != R_PAD) exec_packet(op, &ring[t % RING_WORDS + 1]);
            t += n;
            if ((k & 63) == 63) __atomic_store_n(&Q.tail, t, __ATOMIC_RELEASE);
        }
        __atomic_store_n(&Q.tail, t, __ATOMIC_RELEASE);
    }
    return;
}

// ---- producer

static void wake(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&Q.sleeping, __ATOMIC_RELAXED)) plat_event_set(&wake_ev), RS_.wakes++;
}

// Room for need words: the tail read last leaves it in all but about one packet in 16 MiB of them.
static void wait_space(uint64_t need) {
    if (Q.put_h + need - Q.tail_seen <= RING_WORDS) return;
    Q.tail_seen = __atomic_load_n(&Q.tail, __ATOMIC_ACQUIRE);
    if (Q.put_h + need - Q.tail_seen <= RING_WORDS) return;
    RS_.full_waits++;
    Q.pub_h = Q.put_h;
    __atomic_store_n(&Q.head, Q.put_h, __ATOMIC_RELEASE);
    wake();
    while (Q.put_h + need - (Q.tail_seen = __atomic_load_n(&Q.tail, __ATOMIC_ACQUIRE)) > RING_WORDS) _mm_pause();
}

// Room for a packet of bytes of arguments; the caller fills it and calls pub().
static void *put(uint32_t op, size_t bytes) {
    uint64_t n = 1 + (bytes + 7) / 8, pos = Q.put_h % RING_WORDS;
    if (n > RING_WORDS / 4) rt_fatal("vk_rec: a packet of %llu bytes", (unsigned long long)bytes);
    if (pos + n > RING_WORDS) {
        uint64_t pad = RING_WORDS - pos;
        wait_space(pad);
        ring[pos] = (uint64_t)R_PAD | pad << 32;
        Q.put_h += pad, pos = 0;
    }
    wait_space(n);
    ring[pos] = (uint64_t)op | n << 32;
    // Packers reserve room for optional arrays they may leave unwritten (BeginRendering's depth and stencil); with
    // TOTK_REC_CHECK those words would still hold handles from the ring's previous lap.
    if (rec_chk) memset(&ring[pos + 1], 0, (n - 1) * 8);
    Q.put_h += n;
    RS_.packets++, RS_.words += n;
    return &ring[pos + 1];
}

// After a packet: head moves once Q.pub_words words wait, so the recorder reads a line the producer is still filling
// once per batch instead of once per packet. Whatever the producer waits for publishes everything first (wait_space,
// rec_drain), and the packets that begin and end a part go out at once (pub_now), so the recorder begins and ends
// command buffers as early as before; TOTK_REC_CHECK publishes every packet, since rec_check_handle searches the ring
// up to head.
// The batch is 64 words (512 bytes) where the recorder polls before it sleeps, 8192 (64 KiB) where it does not (fewer
// than 6 usable logical processors, rec_spin_ns): there it is mostly asleep when a batch goes out, and each publish
// cost the producer an event set and the recorder a wake-up. Savage Wind Recomp measured that at 6.7% of its render
// thread's busy time on 4 cores and moved to 64 KiB there (runtime/nvn/vulkan/record.c rec_init, publish_every).
// TOTK_REC_PUB=<words> sets the batch; =ab: 64 words in the A blocks of TOTK_AB, 8192 in the B blocks.
static int pub_ab;
static void pub_now(void) {
    Q.pub_h = Q.put_h;
    __atomic_store_n(&Q.head, Q.put_h, __ATOMIC_RELEASE);
    if (__atomic_load_n(&Q.sleeping, __ATOMIC_RELAXED)) plat_event_set(&wake_ev), RS_.wakes++;
}

static void pub(void) {
    uint64_t w = pub_ab ? (rt_ab() ? 8192 : 64) : Q.pub_words;
    if (Q.put_h - Q.pub_h >= w) pub_now();
}

void rec_drain(void) {
    if (!rec_thread || __atomic_load_n(&Q.tail, __ATOMIC_ACQUIRE) == Q.put_h) return;
    uint64_t t0 = now_ns();
    Q.pub_h = Q.put_h;
    __atomic_store_n(&Q.head, Q.put_h, __ATOMIC_RELEASE);
    wake();
    for (int i = 0; __atomic_load_n(&Q.tail, __ATOMIC_ACQUIRE) != Q.put_h; i++)
        if (i < 20000) _mm_pause();
        else plat_yield();
    RS_.drains++, RS_.drain_ns += now_ns() - t0;
}

static int rec_own(void) {
    if (end_mode < 0) end_mode = rt_knob("REC_END", "1");
    return end_mode;
}

// vkBeginCommandBuffer (one submission) and the breadcrumb, on the thread that records cb.
static void begin_now(VkCommandBuffer cb, int crumb, uint64_t n) {
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    if (crumb) vk_crumb(cb, crumb, n);
}

// The command buffer the following packets go into; begin: the recorder begins it (TOTK_REC_END=1).
typedef struct { VkCommandBuffer cb; int begin, crumb; uint64_t n; } A_BeginCb;

void rec_begin(VkCommandBuffer cb, uint64_t fence, int crumb, uint64_t n) {
    static int mode = -1;
    if (mode < 0) {
        // On by default on every GPU. 0.2.8 turned it on for AMD too, and some RDNA 2, 3 and 4 systems lost the device
        // or hung the GPU at startup (issues #5, #40, #277 and #283: a black screen, or VK_ERROR_DEVICE_LOST with a
        // breadcrumb naming a part of a replay submission), so 0.3.x kept it off on AMD. Since then the recorder
        // begins, ends and submits its own parts (TOTK_REC_END, which ended a GPU hang on AMD at split submissions),
        // and AMD players who turned it on reported more FPS, so it is on again; TOTK_VK_RECORDER=0 turns it off
        // where the device is still lost.
        mode = rt_knob("VK_RECORDER", "1");
        if (mode) {
            plat_event_init(&wake_ev, 0, 0);
            rec_prio = plat_thread_priority_get();
            rec_chk = rt_knob("REC_CHECK", "0");   // debug switches, read before the recorder starts
            const char *d = cfg_get("TOTK_REC_DELAY_US");
            rec_delay_ns = d && *d && atoi(d) > 0 ? (uint64_t)atoi(d) * 1000 : 0;
            const char *sp = cfg_get("TOTK_REC_SPIN_US");
            rec_spin_ab = sp && !strcmp(sp, "ab");
            int spin_us = sp && *sp && !rec_spin_ab ? atoi(sp) : 60;
            rec_spin_ns = spin_us > 0 && plat_cpu_usable() >= 6 ? (uint64_t)spin_us * 1000 : 0;
            const char *pw = cfg_get("TOTK_REC_PUB");   // words per publish (pub); 0: every packet
            pub_ab = !rec_chk && pw && !strcmp(pw, "ab");
            Q.pub_words = rec_chk ? 0 : pw && *pw && !pub_ab ? (uint64_t)atoi(pw) : rec_spin_ns ? 64 : 8192;
            if (Q.pub_words > RING_WORDS / 8) Q.pub_words = RING_WORDS / 8;
            rec_thread = plat_thread_new(rec_main, NULL, "vk recorder");
            if (!rec_thread) {
                rt_log("vk_rec: no recorder thread: the replay makes its Vulkan calls itself");
                mode = 0;
            } else {
                rt_log("vk_rec: the replay's Vulkan commands are made by a recorder thread%s",
                       mode == 2 ? " in the B blocks (TOTK_AB)" : "");
                if (rec_chk) rt_log("vk_rec: TOTK_REC_CHECK: deferred and destroyed objects are searched in the ring");
                if (rec_spin_ns)
                    rt_log("vk_rec: the recorder polls %llu us before it sleeps%s (TOTK_REC_SPIN_US)",
                           (unsigned long long)(rec_spin_ns / 1000), rec_spin_ab ? " in the B blocks" : "");
                else
                    rt_log("vk_rec: the recorder polls 4096 pauses before it sleeps (TOTK_REC_SPIN_US)");
                if (pub_ab) rt_log("vk_rec: head published every 64 words, 8192 in the B blocks (TOTK_REC_PUB=ab)");
                else rt_log("vk_rec: head published every %llu words (TOTK_REC_PUB)", (unsigned long long)Q.pub_words);
                if (rec_delay_ns) rt_log("vk_rec: TOTK_REC_DELAY_US: the recorder waits %llu us before each batch",
                                         (unsigned long long)(rec_delay_ns / 1000));
            }
        }
    }
    RS_.parts++;
    int on = RT_KNOB_ON(mode), own = on && rec_own();
    if (!own) {
        // The submitting thread begins cb. The part pool is the recorder's while it has packets (an A/B switch
        // between parts): it finishes them first.
        rec_drain();
        begin_now(cb, crumb, n);
        if (!on) return;
    }
    RS_.parts_on++;
    __atomic_store_n(&part_h0, Q.put_h, __ATOMIC_RELEASE);
    __atomic_store_n(&part_fence, fence, __ATOMIC_RELEASE);
    A_BeginCb *a = put(R_BeginCb, sizeof *a);
    *a = (A_BeginCb){cb, own, crumb, n};
    pub_now();
    rec_cb = cb;
    rec_live = 1;
}

void rec_end(void) {
    if (!rec_live) return;
    rec_drain();
    rec_live = 0;
}

// TOTK_REC_END (default 1): a part the recorder records is the recorder's from its begin to its submission: it begins
// it, makes every command, ends it after the breadcrumb crumb (kind 0: none) that vk_sync.c writes at the end of a
// submission, and submits it. The submitting thread touches the part pool only while the recorder is idle. That is
// LAS Recomp's and Savage Wind Recomp's model (runtime/nvn/vulkan/record.h: command buffers allocated once, then only
// the recorder begins, records, ends and submits them). Before, the submitting thread began and submitted parts the
// recorder recorded and, before that, also ended them. The handoff in rec_drain orders the two threads, so Vulkan
// allows that split, but on an RX 6700 XT (driver 26.10.1, issue #5) parts ended by the other thread broke when a
// submission was split (vk_sync.c vk_sync_split): the GPU hung or the scene turned green, then white. Moving the end
// fixed that; begin and submit followed. TOTK_REC_END=0 brings the old split back: the submitting thread begins,
// ends and submits every part.
// The line is written only when a part was recorded by the recorder: with the recorder off (TOTK_VK_RECORDER=0) the
// switch changes nothing, and reports read an unconditional line as proof that the recorder was running (#315).
typedef struct { int kind; uint64_t n; } A_EndCb;
int rec_end_cb(VkCommandBuffer cb, int crumb, uint64_t n) {
    static int said;
    int own = rec_own();
    if (rec_live && cb == rec_cb && !said) {
        said = 1;
        rt_log("vk_rec: a part the recorder records is begun, ended and submitted by %s (TOTK_REC_END=%d)",
               own ? "the recorder" : "the submitting thread", own);
    }
    if (!rec_live || cb != rec_cb || !own) {
        rec_end();
        if (crumb) vk_crumb(cb, crumb, n);
        return 0;
    }
    A_EndCb *a = put(R_EndCb, sizeof *a);
    *a = (A_EndCb){crumb, n};
    pub_now();
    rec_live = 0;
    sub_pending = 1;
    return 1;
}

// At most two command buffers and one signal: the shape vk_sync.c submits.
typedef struct { uint32_t ncb, nsig; VkCommandBufferSubmitInfo cbi[2]; VkSemaphoreSubmitInfo sig; } A_Submit;
VkResult rec_submit(const VkSubmitInfo2 *si) {
    if (!sub_pending) {
        vk_queue_lock();
        VkResult r = vkQueueSubmit2(vk_ctx()->gfx_queue, 1, si, VK_NULL_HANDLE);
        vk_queue_unlock();
        return r;
    }
    sub_pending = 0;
    if (si->commandBufferInfoCount > 2 || si->signalSemaphoreInfoCount > 1 || si->waitSemaphoreInfoCount || si->pNext)
        rt_fatal("vk_rec: a submission of %u command buffers, %u waits and %u signals the recorder cannot carry",
                 si->commandBufferInfoCount, si->waitSemaphoreInfoCount, si->signalSemaphoreInfoCount);
    A_Submit *a = put(R_Submit, sizeof *a);
    memset(a, 0, sizeof *a);
    a->ncb = si->commandBufferInfoCount, a->nsig = si->signalSemaphoreInfoCount;
    for (uint32_t i = 0; i < a->ncb; i++) a->cbi[i] = si->pCommandBufferInfos[i];
    if (a->nsig) a->sig = si->pSignalSemaphoreInfos[0];
    __atomic_store_n(&q_subs, q_subs + 1, __ATOMIC_RELEASE);
    RS_.submits++;
    // TOTK_REC_ASYNC=1 (default): the submitting thread goes on while the recorder catches up with the part and submits
    // it; it waited here for 0.29 ms a frame in the town on a Ryzen 7900X, 0.34 ms limited to 4 cores. The queue order
    // the present, uploads, readbacks and device waits rely on holds through vk_queue_lock (rec_queue_wait). The values
    // the part signals are counted as issued at once (vk_sync.c): waiting for one on the host before its submission is
    // legal for a timeline semaphore. =0: waits here for the submission, as before; =ab with the A/B blocks.
    if (rec_async < 0) rec_async = rt_knob("REC_ASYNC", "1");
    if (RT_KNOB_ON(rec_async)) {
        pub_now();
        return __atomic_load_n(&r_sub_err, __ATOMIC_ACQUIRE);
    }
    pub();
    rec_drain();
    return r_sub_res;
}

static void submit_run(const A_Submit *a) {
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = a->ncb,
                        .pCommandBufferInfos = a->cbi, .signalSemaphoreInfoCount = a->nsig,
                        .pSignalSemaphoreInfos = a->nsig ? &a->sig : NULL};
    vk_queue_lock();
    VkResult r = vkQueueSubmit2(vk_ctx()->gfx_queue, 1, &si, VK_NULL_HANDLE);
    vk_queue_unlock();
    r_sub_res = r;
    if (r != VK_SUCCESS && __atomic_load_n(&r_sub_err, __ATOMIC_RELAXED) == VK_SUCCESS)
        __atomic_store_n(&r_sub_err, r, __ATOMIC_RELEASE);
    __atomic_store_n(&r_subs, r_subs + 1, __ATOMIC_RELEASE);
}

void rec_queue_wait(void) {
    if (!rec_thread || on_rec) return;
    uint64_t want = __atomic_load_n(&q_subs, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&r_subs, __ATOMIC_ACQUIRE) >= want) return;
    uint64_t t0 = now_ns();
    for (int i = 0; __atomic_load_n(&r_subs, __ATOMIC_ACQUIRE) < want; i++)
        if (i < 20000) _mm_pause();
        else plat_yield();
    __atomic_add_fetch(&RQ_.n, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&RQ_.ns, now_ns() - t0, __ATOMIC_RELAXED);
}

void rec_stats_log(void) {
    if (!(rt_trace & RT_TRACE_PERF) || !RS_.parts) return;
    rt_log("vk_rec: %llu of %llu parts recorded by the recorder: %llu packets (%.1f MiB), %llu drains (%llu for a call "
           "without a packet) waited %.2f ms in all, ring full %llu times, %llu submissions made by the recorder, "
           "recorder slept %llu times and was woken %llu times, queue users waited for its submissions %llu times (%.2f ms)",
           (unsigned long long)RS_.parts_on, (unsigned long long)RS_.parts, (unsigned long long)RS_.packets,
           RS_.words * 8 / 1048576.0, (unsigned long long)RS_.drains, (unsigned long long)RS_.fallbacks,
           RS_.drain_ns / 1e6, (unsigned long long)RS_.full_waits, (unsigned long long)RS_.submits,
           (unsigned long long)__atomic_exchange_n(&r_sleeps, 0, __ATOMIC_RELAXED), (unsigned long long)RS_.wakes,
           (unsigned long long)__atomic_exchange_n(&RQ_.n, 0, __ATOMIC_RELAXED),
           __atomic_exchange_n(&RQ_.ns, 0, __ATOMIC_RELAXED) / 1e6);
    memset(&RS_, 0, sizeof RS_);
}

// TOTK_REC_CHECK=1 (debug, recorder on): the packets of the part being recorded are searched word by word for an object
// vk_memory.c lets go of. put() zeroes each packet's room, so only words a packer wrote can match. A hit is a hazard
// - at a deferral (destroy 0) when the stamp tl is below the part's value: vk_memory.c collect_locked may destroy the
//   object once the GPU reaches tl, before the part ran and, with the recorder behind, before it made the call;
// - at a destruction (destroy 1) always: a packet not made yet hands the driver a dead handle, a made one leaves it
//   in a command buffer not yet submitted.
// "Made" means before the tail the recorder published, which trails it by up to 64 packets. Hazards are logged (the
// first 64) and counted by kind, packet and case for rec_check_log; named deferrals that wait long enough are only
// counted. Reads race the producer only in words below head, which stay put until the ring
// wraps (16 MiB later).
static const char *const kind_name[REC_KINDS] = {"image", "view", "buffer"};
static volatile int32_t chk_seen[2], chk_named, chk_logged, chk_hz[2][2][REC_KINDS][64];   // [destroy][made][kind][op]
void rec_check_handle(int kind, uint64_t handle, int destroy, uint64_t tl, uint64_t gpu) {
    if (!rec_chk || !handle || kind < 0 || kind >= REC_KINDS || !__atomic_load_n(&rec_live, __ATOMIC_RELAXED)) return;
    uint64_t h = __atomic_load_n(&Q.head, __ATOMIC_ACQUIRE), p = __atomic_load_n(&part_h0, __ATOMIC_ACQUIRE);
    uint64_t fence = __atomic_load_n(&part_fence, __ATOMIC_ACQUIRE);
    if (p > h || h - p > RING_WORDS / 2) return;   // a part this large is not searched
    plat_inc32(&chk_seen[destroy != 0]);
    while (p < h) {
        uint64_t w = ring[p % RING_WORDS];
        uint32_t op = (uint32_t)(w & 0xffff), n = (uint32_t)(w >> 32);
        if (!n) return;   // overwritten meanwhile
        if (op != R_PAD)
            for (uint32_t i = 1; i < n; i++) {
                if (ring[(p + i) % RING_WORDS] != handle) continue;
                int made = p < __atomic_load_n(&Q.tail, __ATOMIC_ACQUIRE);
                if (!destroy && tl >= fence) { plat_inc32(&chk_named); return; }
                plat_inc32(&chk_hz[destroy][made][kind][op & 63]);
                if (plat_inc32(&chk_logged) <= 64)
                    rt_log("vk_rec: check: HAZARD: %s %#llx %s while packet op %u (byte %u of its arguments) of the "
                           "part being recorded names it, %s; deferral stamp %llu, part value %llu, GPU at %llu",
                           kind_name[kind], (unsigned long long)handle, destroy ? "destroyed" : "deferred", op,
                           8 * (i - 1), made ? "made" : "not known made", (unsigned long long)tl,
                           (unsigned long long)fence, (unsigned long long)gpu);
                return;
            }
        p += n;
    }
}

void rec_check_log(void) {
    static volatile int32_t said;
    if (!rec_chk || plat_xchg32(&said, 1)) return;
    int any = 0;
    for (int d = 0; d < 2; d++)
        for (int m = 0; m < 2; m++)
            for (int k = 0; k < REC_KINDS; k++)
                for (int op = 0; op < 64; op++)
                    if (chk_hz[d][m][k][op]) {
                        any = 1;
                        rt_log("vk_rec: check summary: %d hazards: %s %s, packet op %d %s", (int)chk_hz[d][m][k][op],
                               kind_name[k], d ? "destroyed" : "deferred", op, m ? "made" : "not known made");
                    }
    rt_log("vk_rec: check summary: %s; searched for %d objects deferred and %d destroyed while a part was recorded; %d "
           "deferred ones the part named waited for its value", any ? "hazards above" : "no hazards", (int)chk_seen[0],
           (int)chk_seen[1], (int)chk_named);
}

#define OWNED(cb) (rec_live && (cb) == rec_cb)

int rec_owns(VkCommandBuffer cb) { return OWNED(cb); }
// A call the ring takes no packet for: everything queued first, then the call here.
#define FALLBACK() (rec_drain(), RS_.fallbacks++)

// Copies n elements of arr after the fixed part at *at and returns where they went (NULL for none).
static void *tail_copy(uint8_t **at, const void *arr, size_t bytes) {
    if (!arr || !bytes) return NULL;
    void *d = *at;
    memcpy(d, arr, bytes);
    *at += (bytes + 7) & ~(size_t)7;
    return d;
}
#define A8(x) (((x) + 7) & ~(size_t)7)

// ---- fixed-size commands: the arguments after cb, as a struct

#define FIXED(name, params, call_args, ...)                                                                             \
    typedef struct { __VA_ARGS__; } A_##name;                                                                          \
    void rc_vkCmd##name params {                                                                                       \
        if (!OWNED(cb)) { vkCmd##name call_args; return; }                                                             \
        A_##name *a = put(R_##name, sizeof *a);                                                                        \
        PACK_##name;                                                                                                    \
        pub();                                                                                                          \
    }

#define PACK_BeginQuery *a = (A_BeginQuery){p, q, f}
FIXED(BeginQuery, (VkCommandBuffer cb, VkQueryPool p, uint32_t q, VkQueryControlFlags f), (cb, p, q, f),
      VkQueryPool p; uint32_t q; VkQueryControlFlags f)
#define PACK_BindIndexBuffer *a = (A_BindIndexBuffer){b, o, t}
FIXED(BindIndexBuffer, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t), (cb, b, o, t), VkBuffer b;
      VkDeviceSize o; VkIndexType t)
#define PACK_BindPipeline *a = (A_BindPipeline){bp, p}
FIXED(BindPipeline, (VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p), (cb, bp, p), VkPipelineBindPoint bp;
      VkPipeline p)
#define PACK_Dispatch *a = (A_Dispatch){x, y, z}
FIXED(Dispatch, (VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z), (cb, x, y, z), uint32_t x, y, z)
#define PACK_DispatchIndirect *a = (A_DispatchIndirect){b, o}
FIXED(DispatchIndirect, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o), (cb, b, o), VkBuffer b; VkDeviceSize o)
#define PACK_Draw *a = (A_Draw){n, i, f, fi}
FIXED(Draw, (VkCommandBuffer cb, uint32_t n, uint32_t i, uint32_t f, uint32_t fi), (cb, n, i, f, fi), uint32_t n, i, f, fi)
#define PACK_DrawIndexed *a = (A_DrawIndexed){n, i, f, v, fi}
FIXED(DrawIndexed, (VkCommandBuffer cb, uint32_t n, uint32_t i, uint32_t f, int32_t v, uint32_t fi), (cb, n, i, f, v, fi),
      uint32_t n, i, f; int32_t v; uint32_t fi)
#define PACK_DrawIndexedIndirect *a = (A_DrawIndexedIndirect){b, o, n, s}
FIXED(DrawIndexedIndirect, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s), (cb, b, o, n, s),
      VkBuffer b; VkDeviceSize o; uint32_t n, s)
#define PACK_EndQuery *a = (A_EndQuery){p, q}
FIXED(EndQuery, (VkCommandBuffer cb, VkQueryPool p, uint32_t q), (cb, p, q), VkQueryPool p; uint32_t q)
#define PACK_EndRendering (void)a
FIXED(EndRendering, (VkCommandBuffer cb), (cb), int unused)
#define PACK_FillBuffer *a = (A_FillBuffer){b, o, n, v}
FIXED(FillBuffer, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize n, uint32_t v), (cb, b, o, n, v),
      VkBuffer b; VkDeviceSize o, n; uint32_t v)
#define PACK_ResetQueryPool *a = (A_ResetQueryPool){p, f, n}
FIXED(ResetQueryPool, (VkCommandBuffer cb, VkQueryPool p, uint32_t f, uint32_t n), (cb, p, f, n), VkQueryPool p;
      uint32_t f, n)
#define PACK_SetBlendConstants memcpy(a->c, c, sizeof a->c)
FIXED(SetBlendConstants, (VkCommandBuffer cb, const float c[4]), (cb, c), float c[4])
#define PACK_SetCullMode a->v = m
FIXED(SetCullMode, (VkCommandBuffer cb, VkCullModeFlags m), (cb, m), VkCullModeFlags v)
#define PACK_SetDepthBias *a = (A_SetDepthBias){c, cl, s}
FIXED(SetDepthBias, (VkCommandBuffer cb, float c, float cl, float s), (cb, c, cl, s), float c, cl, s)
#define PACK_SetDepthBiasEnable a->v = v
FIXED(SetDepthBiasEnable, (VkCommandBuffer cb, VkBool32 v), (cb, v), VkBool32 v)
#define PACK_SetDepthCompareOp a->v = v
FIXED(SetDepthCompareOp, (VkCommandBuffer cb, VkCompareOp v), (cb, v), VkCompareOp v)
#define PACK_SetDepthTestEnable a->v = v
FIXED(SetDepthTestEnable, (VkCommandBuffer cb, VkBool32 v), (cb, v), VkBool32 v)
#define PACK_SetDepthWriteEnable a->v = v
FIXED(SetDepthWriteEnable, (VkCommandBuffer cb, VkBool32 v), (cb, v), VkBool32 v)
#define PACK_SetFrontFace a->v = v
FIXED(SetFrontFace, (VkCommandBuffer cb, VkFrontFace v), (cb, v), VkFrontFace v)
#define PACK_SetPrimitiveRestartEnable a->v = v
FIXED(SetPrimitiveRestartEnable, (VkCommandBuffer cb, VkBool32 v), (cb, v), VkBool32 v)
#define PACK_SetPrimitiveTopology a->v = v
FIXED(SetPrimitiveTopology, (VkCommandBuffer cb, VkPrimitiveTopology v), (cb, v), VkPrimitiveTopology v)
#define PACK_SetRasterizerDiscardEnable a->v = v
FIXED(SetRasterizerDiscardEnable, (VkCommandBuffer cb, VkBool32 v), (cb, v), VkBool32 v)
#define PACK_SetStencilCompareMask *a = (A_SetStencilCompareMask){f, v}
FIXED(SetStencilCompareMask, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v), (cb, f, v), VkStencilFaceFlags f;
      uint32_t v)
#define PACK_SetStencilOp *a = (A_SetStencilOp){f, x, y, z, d}
FIXED(SetStencilOp, (VkCommandBuffer cb, VkStencilFaceFlags f, VkStencilOp x, VkStencilOp y, VkStencilOp z, VkCompareOp d),
      (cb, f, x, y, z, d), VkStencilFaceFlags f; VkStencilOp x, y, z; VkCompareOp d)
#define PACK_SetStencilReference *a = (A_SetStencilReference){f, v}
FIXED(SetStencilReference, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v), (cb, f, v), VkStencilFaceFlags f;
      uint32_t v)
#define PACK_SetStencilTestEnable a->v = v
FIXED(SetStencilTestEnable, (VkCommandBuffer cb, VkBool32 v), (cb, v), VkBool32 v)
#define PACK_SetStencilWriteMask *a = (A_SetStencilWriteMask){f, v}
FIXED(SetStencilWriteMask, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v), (cb, f, v), VkStencilFaceFlags f;
      uint32_t v)
#define PACK_WriteTimestamp *a = (A_WriteTimestamp){s, p, q}
FIXED(WriteTimestamp, (VkCommandBuffer cb, VkPipelineStageFlagBits s, VkQueryPool p, uint32_t q), (cb, s, p, q),
      VkPipelineStageFlagBits s; VkQueryPool p; uint32_t q)
#define PACK_WriteTimestamp2 *a = (A_WriteTimestamp2){s, p, q}
FIXED(WriteTimestamp2, (VkCommandBuffer cb, VkPipelineStageFlags2 s, VkQueryPool p, uint32_t q), (cb, s, p, q),
      VkPipelineStageFlags2 s; VkQueryPool p; uint32_t q)

// ---- commands with arrays: a struct with the call's arguments, the arrays after it, pointers into the packet

typedef struct { uint32_t n; const VkViewport *v; } A_SetViewportWithCount;
void rc_vkCmdSetViewportWithCount(VkCommandBuffer cb, uint32_t n, const VkViewport *v) {
    if (!OWNED(cb)) { vkCmdSetViewportWithCount(cb, n, v); return; }
    A_SetViewportWithCount *a = put(R_SetViewportWithCount, sizeof *a + sizeof *v * n);
    uint8_t *at = (uint8_t *)(a + 1);
    a->n = n, a->v = tail_copy(&at, v, sizeof *v * n);
    pub();
}

typedef struct { uint32_t n; const VkRect2D *r; } A_SetScissorWithCount;
void rc_vkCmdSetScissorWithCount(VkCommandBuffer cb, uint32_t n, const VkRect2D *r) {
    if (!OWNED(cb)) { vkCmdSetScissorWithCount(cb, n, r); return; }
    A_SetScissorWithCount *a = put(R_SetScissorWithCount, sizeof *a + sizeof *r * n);
    uint8_t *at = (uint8_t *)(a + 1);
    a->n = n, a->r = tail_copy(&at, r, sizeof *r * n);
    pub();
}

typedef struct {
    VkPipelineBindPoint bp;
    VkPipelineLayout l;
    uint32_t first, n, nd;
    const VkDescriptorSet *s;
    const uint32_t *d;
} A_BindDescriptorSets;
void rc_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first,
                                uint32_t n, const VkDescriptorSet *s, uint32_t nd, const uint32_t *d) {
    if (!OWNED(cb)) { vkCmdBindDescriptorSets(cb, bp, l, first, n, s, nd, d); return; }
    A_BindDescriptorSets *a = put(R_BindDescriptorSets, sizeof *a + A8(sizeof *s * n) + A8(sizeof *d * nd));
    uint8_t *at = (uint8_t *)(a + 1);
    *a = (A_BindDescriptorSets){bp, l, first, n, nd, NULL, NULL};
    a->s = tail_copy(&at, s, sizeof *s * n), a->d = tail_copy(&at, d, sizeof *d * nd);
    pub();
}

typedef struct { uint32_t first, n; const VkBuffer *b; const VkDeviceSize *o, *sz, *st; } A_BindVertexBuffers2;
void rc_vkCmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o,
                                const VkDeviceSize *sz, const VkDeviceSize *st) {
    if (!OWNED(cb)) { vkCmdBindVertexBuffers2(cb, first, n, b, o, sz, st); return; }
    size_t e = sizeof(VkDeviceSize) * n;
    A_BindVertexBuffers2 *a = put(R_BindVertexBuffers2, sizeof *a + A8(sizeof *b * n) + 3 * A8(e));
    uint8_t *at = (uint8_t *)(a + 1);
    a->first = first, a->n = n;
    a->b = tail_copy(&at, b, sizeof *b * n), a->o = tail_copy(&at, o, e);
    a->sz = sz ? tail_copy(&at, sz, e) : NULL, a->st = st ? tail_copy(&at, st, e) : NULL;
    pub();
}

typedef struct { VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; uint32_t n; const void *r; VkFilter f; } A_Img;
#define IMG_CMD(name, rtype, params, call, ...)                                                                         \
    void rc_vkCmd##name params {                                                                                       \
        if (!OWNED(cb)) { vkCmd##name call; return; }                                                                  \
        A_Img *a = put(R_##name, sizeof *a + sizeof(rtype) * n);                                                       \
        uint8_t *at = (uint8_t *)(a + 1);                                                                              \
        *a = (A_Img){__VA_ARGS__};                                                                                      \
        a->r = tail_copy(&at, r, sizeof(rtype) * n);                                                                   \
        pub();                                                                                                          \
    }
IMG_CMD(BlitImage, VkImageBlit, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n,
        const VkImageBlit *r, VkFilter f), (cb, s, sl, d, dl, n, r, f), s, sl, d, dl, n, NULL, f)
IMG_CMD(CopyImage, VkImageCopy, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n,
        const VkImageCopy *r), (cb, s, sl, d, dl, n, r), s, sl, d, dl, n, NULL, 0)
IMG_CMD(ResolveImage, VkImageResolve, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl,
        uint32_t n, const VkImageResolve *r), (cb, s, sl, d, dl, n, r), s, sl, d, dl, n, NULL, 0)

typedef struct { uint64_t s, d; VkImageLayout l; uint32_t n; const void *r; } A_Xfer;   // buffer or image handles
#define XFER_CMD(name, rtype, params, call, S, D, L)                                                                    \
    void rc_vkCmd##name params {                                                                                       \
        if (!OWNED(cb)) { vkCmd##name call; return; }                                                                  \
        A_Xfer *a = put(R_##name, sizeof *a + sizeof(rtype) * n);                                                      \
        uint8_t *at = (uint8_t *)(a + 1);                                                                              \
        *a = (A_Xfer){(uint64_t)(S), (uint64_t)(D), L, n, NULL};                                                        \
        a->r = tail_copy(&at, r, sizeof(rtype) * n);                                                                   \
        pub();                                                                                                          \
    }
XFER_CMD(CopyBuffer, VkBufferCopy, (VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *r),
         (cb, s, d, n, r), s, d, 0)
XFER_CMD(CopyBufferToImage, VkBufferImageCopy, (VkCommandBuffer cb, VkBuffer s, VkImage d, VkImageLayout dl, uint32_t n,
         const VkBufferImageCopy *r), (cb, s, d, dl, n, r), s, d, dl)
XFER_CMD(CopyImageToBuffer, VkBufferImageCopy, (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkBuffer d, uint32_t n,
         const VkBufferImageCopy *r), (cb, s, sl, d, n, r), s, d, sl)

typedef struct { uint32_t na, nr; const VkClearAttachment *a; const VkClearRect *r; } A_ClearAttachments;
void rc_vkCmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *ca, uint32_t nr, const VkClearRect *r) {
    if (!OWNED(cb)) { vkCmdClearAttachments(cb, na, ca, nr, r); return; }
    A_ClearAttachments *a = put(R_ClearAttachments, sizeof *a + A8(sizeof *ca * na) + A8(sizeof *r * nr));
    uint8_t *at = (uint8_t *)(a + 1);
    a->na = na, a->nr = nr;
    a->a = tail_copy(&at, ca, sizeof *ca * na), a->r = tail_copy(&at, r, sizeof *r * nr);
    pub();
}

typedef struct { VkImage i; VkImageLayout l; union { VkClearColorValue c; VkClearDepthStencilValue ds; } v; uint32_t n;
                 const VkImageSubresourceRange *r; } A_ClearImage;
void rc_vkCmdClearColorImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue *c, uint32_t n,
                             const VkImageSubresourceRange *r) {
    if (!OWNED(cb)) { vkCmdClearColorImage(cb, i, l, c, n, r); return; }
    A_ClearImage *a = put(R_ClearColorImage, sizeof *a + sizeof *r * n);
    uint8_t *at = (uint8_t *)(a + 1);
    a->i = i, a->l = l, a->v.c = *c, a->n = n, a->r = tail_copy(&at, r, sizeof *r * n);
    pub();
}
void rc_vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue *c,
                                    uint32_t n, const VkImageSubresourceRange *r) {
    if (!OWNED(cb)) { vkCmdClearDepthStencilImage(cb, i, l, c, n, r); return; }
    A_ClearImage *a = put(R_ClearDepthStencilImage, sizeof *a + sizeof *r * n);
    uint8_t *at = (uint8_t *)(a + 1);
    a->i = i, a->l = l, a->v.ds = *c, a->n = n, a->r = tail_copy(&at, r, sizeof *r * n);
    pub();
}

typedef struct { uint32_t n; const VkCommandBuffer *c; } A_ExecuteCommands;
void rc_vkCmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *c) {
    if (!OWNED(cb)) { vkCmdExecuteCommands(cb, n, c); return; }
    A_ExecuteCommands *a = put(R_ExecuteCommands, sizeof *a + sizeof *c * n);
    uint8_t *at = (uint8_t *)(a + 1);
    a->n = n, a->c = tail_copy(&at, c, sizeof *c * n);
    pub();
}

typedef struct { VkPipelineLayout l; VkShaderStageFlags st; uint32_t o, n; const void *v; } A_PushConstants;
void rc_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags st, uint32_t o, uint32_t n,
                           const void *v) {
    if (!OWNED(cb)) { vkCmdPushConstants(cb, l, st, o, n, v); return; }
    A_PushConstants *a = put(R_PushConstants, sizeof *a + n);
    uint8_t *at = (uint8_t *)(a + 1);
    *a = (A_PushConstants){l, st, o, n, NULL};
    a->v = tail_copy(&at, v, n);
    pub();
}

void rc_vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di) {
    if (!OWNED(cb)) { vkCmdPipelineBarrier2(cb, di); return; }
    int chained = di->pNext != NULL;
    for (uint32_t i = 0; i < di->memoryBarrierCount; i++) chained |= di->pMemoryBarriers[i].pNext != NULL;
    for (uint32_t i = 0; i < di->bufferMemoryBarrierCount; i++) chained |= di->pBufferMemoryBarriers[i].pNext != NULL;
    for (uint32_t i = 0; i < di->imageMemoryBarrierCount; i++) chained |= di->pImageMemoryBarriers[i].pNext != NULL;
    if (chained) { FALLBACK(); vkCmdPipelineBarrier2(cb, di); return; }
    size_t m = sizeof *di->pMemoryBarriers * di->memoryBarrierCount;
    size_t b = sizeof *di->pBufferMemoryBarriers * di->bufferMemoryBarrierCount;
    size_t im = sizeof *di->pImageMemoryBarriers * di->imageMemoryBarrierCount;
    VkDependencyInfo *a = put(R_PipelineBarrier2, sizeof *a + A8(m) + A8(b) + A8(im));
    uint8_t *at = (uint8_t *)(a + 1);
    *a = *di;
    a->pMemoryBarriers = tail_copy(&at, di->pMemoryBarriers, m);
    a->pBufferMemoryBarriers = tail_copy(&at, di->pBufferMemoryBarriers, b);
    a->pImageMemoryBarriers = tail_copy(&at, di->pImageMemoryBarriers, im);
    pub();
}

void rc_vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    if (!OWNED(cb)) { vkCmdBeginRendering(cb, ri); return; }
    int chained = ri->pNext != NULL || (ri->pDepthAttachment && ri->pDepthAttachment->pNext) ||
                  (ri->pStencilAttachment && ri->pStencilAttachment->pNext);
    for (uint32_t i = 0; i < ri->colorAttachmentCount; i++) chained |= ri->pColorAttachments[i].pNext != NULL;
    if (chained) { FALLBACK(); vkCmdBeginRendering(cb, ri); return; }
    size_t c = sizeof *ri->pColorAttachments * ri->colorAttachmentCount, one = sizeof(VkRenderingAttachmentInfo);
    VkRenderingInfo *a = put(R_BeginRendering, sizeof *a + A8(c) + 2 * A8(one));
    uint8_t *at = (uint8_t *)(a + 1);
    *a = *ri;
    a->pColorAttachments = tail_copy(&at, ri->pColorAttachments, c);
    a->pDepthAttachment = ri->pDepthAttachment ? tail_copy(&at, ri->pDepthAttachment, one) : NULL;
    a->pStencilAttachment = ri->pStencilAttachment ? tail_copy(&at, ri->pStencilAttachment, one) : NULL;
    pub();
}

// ---- extension commands through function pointers

typedef struct { PFN_vkCmdPushDescriptorSetWithTemplateKHR fn; VkDescriptorUpdateTemplate tpl; VkPipelineLayout l;
                 uint32_t set; const void *data; } A_PushTpl;
void rec_push_tpl(PFN_vkCmdPushDescriptorSetWithTemplateKHR fn, VkCommandBuffer cb, VkDescriptorUpdateTemplate tpl,
                  VkPipelineLayout layout, uint32_t set, const void *data, uint32_t bytes) {
    if (!OWNED(cb)) { fn(cb, tpl, layout, set, data); return; }
    A_PushTpl *a = put(R_PushTpl, sizeof *a + bytes);
    uint8_t *at = (uint8_t *)(a + 1);
    *a = (A_PushTpl){fn, tpl, layout, set, NULL};
    a->data = tail_copy(&at, data, bytes);
    pub();
}

typedef struct { PFN_vkCmdPushDescriptorSetKHR fn; VkPipelineBindPoint bp; VkPipelineLayout l; uint32_t set, n;
                 VkWriteDescriptorSet *w; } A_Push;
void rec_push(PFN_vkCmdPushDescriptorSetKHR fn, VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout,
              uint32_t set, uint32_t n, const VkWriteDescriptorSet *w) {
    if (!OWNED(cb)) { fn(cb, bp, layout, set, n, w); return; }
    size_t bytes = A8(sizeof *w * n);
    int chained = 0;
    for (uint32_t i = 0; i < n; i++) {
        chained |= w[i].pNext != NULL;
        if (w[i].pBufferInfo) bytes += A8(sizeof *w[i].pBufferInfo * w[i].descriptorCount);
        if (w[i].pImageInfo) bytes += A8(sizeof *w[i].pImageInfo * w[i].descriptorCount);
        if (w[i].pTexelBufferView) bytes += A8(sizeof *w[i].pTexelBufferView * w[i].descriptorCount);
    }
    if (chained) { FALLBACK(); fn(cb, bp, layout, set, n, w); return; }
    A_Push *a = put(R_Push, sizeof *a + bytes);
    uint8_t *at = (uint8_t *)(a + 1);
    *a = (A_Push){fn, bp, layout, set, n, NULL};
    a->w = tail_copy(&at, w, sizeof *w * n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = w[i].descriptorCount;
        a->w[i].pBufferInfo = tail_copy(&at, w[i].pBufferInfo, w[i].pBufferInfo ? sizeof *w[i].pBufferInfo * k : 0);
        a->w[i].pImageInfo = tail_copy(&at, w[i].pImageInfo, w[i].pImageInfo ? sizeof *w[i].pImageInfo * k : 0);
        a->w[i].pTexelBufferView =
            tail_copy(&at, w[i].pTexelBufferView, w[i].pTexelBufferView ? sizeof *w[i].pTexelBufferView * k : 0);
    }
    pub();
}

typedef struct { PFN_vkCmdSetVertexInputEXT fn; uint32_t nb, na; const VkVertexInputBindingDescription2EXT *b;
                 const VkVertexInputAttributeDescription2EXT *v; } A_VertexInput;
void rec_vertex_input(PFN_vkCmdSetVertexInputEXT fn, VkCommandBuffer cb, uint32_t nb,
                      const VkVertexInputBindingDescription2EXT *b, uint32_t na,
                      const VkVertexInputAttributeDescription2EXT *v) {
    if (!OWNED(cb)) { fn(cb, nb, b, na, v); return; }
    A_VertexInput *a = put(R_VertexInput, sizeof *a + A8(sizeof *b * nb) + A8(sizeof *v * na));
    uint8_t *at = (uint8_t *)(a + 1);
    *a = (A_VertexInput){fn, nb, na, NULL, NULL};
    a->b = tail_copy(&at, b, sizeof *b * nb), a->v = tail_copy(&at, v, sizeof *v * na);   // pNext: always NULL there
    pub();
}

typedef struct { PFN_vkCmdSetDepthClampEnableEXT fn; VkBool32 v; } A_Bool;
void rec_bool(PFN_vkCmdSetDepthClampEnableEXT fn, VkCommandBuffer cb, VkBool32 v) {
    if (!OWNED(cb)) { fn(cb, v); return; }
    A_Bool *a = put(R_Bool, sizeof *a);
    *a = (A_Bool){fn, v};
    pub();
}

typedef struct { PFN_vkCmdSetPolygonModeEXT fn; VkPolygonMode m; } A_PolygonMode;
void rec_polygon_mode(PFN_vkCmdSetPolygonModeEXT fn, VkCommandBuffer cb, VkPolygonMode m) {
    if (!OWNED(cb)) { fn(cb, m); return; }
    A_PolygonMode *a = put(R_PolygonMode, sizeof *a);
    *a = (A_PolygonMode){fn, m};
    pub();
}

typedef struct { uint32_t id; int end; } A_PassMark;
void rec_pass_mark(VkCommandBuffer cb, uint32_t id, int end) {
    if (!OWNED(cb)) { vk_pass_mark_cmd(cb, id, end); return; }
    A_PassMark *a = put(R_PassMark, sizeof *a);
    *a = (A_PassMark){id, end};
    pub();
}

typedef struct { void *fn; uint32_t first, n; const void *v; } A_Targets;
#define TARGET_CMD(fname, R, PFN, T)                                                                                    \
    void fname(PFN fn, VkCommandBuffer cb, uint32_t first, uint32_t n, const T *v) {                                    \
        if (!OWNED(cb)) { fn(cb, first, n, v); return; }                                                               \
        A_Targets *a = put(R, sizeof *a + sizeof *v * n);                                                              \
        uint8_t *at = (uint8_t *)(a + 1);                                                                              \
        *a = (A_Targets){(void *)fn, first, n, NULL};                                                                   \
        a->v = tail_copy(&at, v, sizeof *v * n);                                                                       \
        pub();                                                                                                          \
    }
TARGET_CMD(rec_blend_enable, R_BlendEnable, PFN_vkCmdSetColorBlendEnableEXT, VkBool32)
TARGET_CMD(rec_blend_equation, R_BlendEquation, PFN_vkCmdSetColorBlendEquationEXT, VkColorBlendEquationEXT)
TARGET_CMD(rec_write_mask, R_WriteMask, PFN_vkCmdSetColorWriteMaskEXT, VkColorComponentFlags)

// ---- the recorder's side

static void exec_packet(uint32_t op, void *p) {
    if (op == R_BeginCb) {
        A_BeginCb *a = p;
        r_cb = a->cb;
        if (a->begin) begin_now(r_cb, a->crumb, a->n);
        return;
    }
    if (op == R_Submit) { submit_run(p); return; }
    VkCommandBuffer cb = r_cb;
    switch (op) {
#define F(name, ...) case R_##name: { A_##name *a = p; vkCmd##name(cb, __VA_ARGS__); break; }
    F(BeginQuery, a->p, a->q, a->f)
    F(BindIndexBuffer, a->b, a->o, a->t)
    F(BindPipeline, a->bp, a->p)
    F(Dispatch, a->x, a->y, a->z)
    F(DispatchIndirect, a->b, a->o)
    F(Draw, a->n, a->i, a->f, a->fi)
    F(DrawIndexed, a->n, a->i, a->f, a->v, a->fi)
    F(DrawIndexedIndirect, a->b, a->o, a->n, a->s)
    F(EndQuery, a->p, a->q)
    F(FillBuffer, a->b, a->o, a->n, a->v)
    F(ResetQueryPool, a->p, a->f, a->n)
    F(SetBlendConstants, a->c)
    F(SetCullMode, a->v)
    F(SetDepthBias, a->c, a->cl, a->s)
    F(SetDepthBiasEnable, a->v)
    F(SetDepthCompareOp, a->v)
    F(SetDepthTestEnable, a->v)
    F(SetDepthWriteEnable, a->v)
    F(SetFrontFace, a->v)
    F(SetPrimitiveRestartEnable, a->v)
    F(SetPrimitiveTopology, a->v)
    F(SetRasterizerDiscardEnable, a->v)
    F(SetStencilCompareMask, a->f, a->v)
    F(SetStencilOp, a->f, a->x, a->y, a->z, a->d)
    F(SetStencilReference, a->f, a->v)
    F(SetStencilTestEnable, a->v)
    F(SetStencilWriteMask, a->f, a->v)
    F(WriteTimestamp, a->s, a->p, a->q)
    F(WriteTimestamp2, a->s, a->p, a->q)
    F(SetViewportWithCount, a->n, a->v)
    F(SetScissorWithCount, a->n, a->r)
    F(BindDescriptorSets, a->bp, a->l, a->first, a->n, a->s, a->nd, a->d)
    F(BindVertexBuffers2, a->first, a->n, a->b, a->o, a->sz, a->st)
    F(ClearAttachments, a->na, a->a, a->nr, a->r)
    case R_ClearColorImage: { A_ClearImage *a = p; vkCmdClearColorImage(cb, a->i, a->l, &a->v.c, a->n, a->r); break; }
    case R_ClearDepthStencilImage: {
        A_ClearImage *a = p;
        vkCmdClearDepthStencilImage(cb, a->i, a->l, &a->v.ds, a->n, a->r);
        break;
    }
    F(PushConstants, a->l, a->st, a->o, a->n, a->v)
    F(ExecuteCommands, a->n, a->c)
#undef F
    case R_EndRendering: vkCmdEndRendering(cb); break;
    case R_BlitImage: { A_Img *a = p; vkCmdBlitImage(cb, a->s, a->sl, a->d, a->dl, a->n, a->r, a->f); break; }
    case R_CopyImage: { A_Img *a = p; vkCmdCopyImage(cb, a->s, a->sl, a->d, a->dl, a->n, a->r); break; }
    case R_ResolveImage: { A_Img *a = p; vkCmdResolveImage(cb, a->s, a->sl, a->d, a->dl, a->n, a->r); break; }
    case R_CopyBuffer: { A_Xfer *a = p; vkCmdCopyBuffer(cb, (VkBuffer)a->s, (VkBuffer)a->d, a->n, a->r); break; }
    case R_CopyBufferToImage: {
        A_Xfer *a = p;
        vkCmdCopyBufferToImage(cb, (VkBuffer)a->s, (VkImage)a->d, a->l, a->n, a->r);
        break;
    }
    case R_CopyImageToBuffer: {
        A_Xfer *a = p;
        vkCmdCopyImageToBuffer(cb, (VkImage)a->s, a->l, (VkBuffer)a->d, a->n, a->r);
        break;
    }
    case R_PipelineBarrier2: vkCmdPipelineBarrier2(cb, p); break;
    case R_BeginRendering: vkCmdBeginRendering(cb, p); break;
    case R_PushTpl: { A_PushTpl *a = p; a->fn(cb, a->tpl, a->l, a->set, a->data); break; }
    case R_Push: { A_Push *a = p; a->fn(cb, a->bp, a->l, a->set, a->n, a->w); break; }
    case R_VertexInput: { A_VertexInput *a = p; a->fn(cb, a->nb, a->b, a->na, a->v); break; }
    case R_Bool: { A_Bool *a = p; a->fn(cb, a->v); break; }
    case R_PolygonMode: { A_PolygonMode *a = p; a->fn(cb, a->m); break; }
    case R_BlendEnable: { A_Targets *a = p; ((PFN_vkCmdSetColorBlendEnableEXT)a->fn)(cb, a->first, a->n, a->v); break; }
    case R_BlendEquation: { A_Targets *a = p; ((PFN_vkCmdSetColorBlendEquationEXT)a->fn)(cb, a->first, a->n, a->v); break; }
    case R_WriteMask: { A_Targets *a = p; ((PFN_vkCmdSetColorWriteMaskEXT)a->fn)(cb, a->first, a->n, a->v); break; }
    case R_PassMark: { A_PassMark *a = p; vk_pass_mark_cmd(cb, a->id, a->end); break; }
    case R_EndCb: {
        A_EndCb *a = p;
        if (a->kind) vk_crumb(cb, a->kind, a->n);
        vkEndCommandBuffer(cb);
        break;
    }
    default: rt_fatal("vk_rec: unknown packet %u", op);
    }
}
