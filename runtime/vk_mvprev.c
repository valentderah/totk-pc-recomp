// Last frame's object and bone blocks, for per-object motion vectors (TOTK_DLAA_OBJMV, on with DLAA). The banks kept
// start as c5, c7 and c14 and grow with what the vertex shader variants read (vk_mvprev_want); c7 and c5 are described
// here, the others follow the same two-copy rule.
//
// The scene's vertex shaders read an object's place from bank c7 (gsys_shape: one 3x4 matrix at the start of a
// 0x100 block) and a skinned object's bones from c5 (gsys_skeleton: a run of 3x4 matrices), of this frame only. The
// game writes both into two copies that take turns, the second right after the first: an object's block is at addr in
// one frame and at addr +- step in the next, step its size rounded up to 256 bytes (vk_mvsurvey.c measured it: every
// c7 pair 0x100 apart, c5 pairs their rounded size apart, the pair's contents the same object moved a little). The
// copy this frame does not write still holds last frame's when the frame is submitted, so the uniform snapshot
// (vk_usnap.c) also copies both neighbours of these blocks, and vk_mvprev_find picks last frame's:
//   self    the block was bound at the same address last frame: retained immutable snapshot, not today's bytes
//   side    one neighbour was bound in that bank last frame: that one
//   nearer  both were (the copies of adjacent objects interleave): the one with more of its first 16 words equal to
//           this frame's
//   none    neither: a new object, no motion of its own
// The pick is checked against what was snapshotted of that block last frame (intact: the same first CHECK bytes), and
// a summary goes to the log every SUMMARY frames:
//   mv_prev: c<n>: <blocks> blocks: self a %, side b %, nearer c %, none d %; intact e %; moved: still f %, under
//            one unit g %, more h %
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_dlaa.h"
#include "vk_mvprev.h"

#define CHECK 128u
#define TABLE (1u << 14)
#define SUMMARY 600

enum { BONES = 2, PLACE = 4, CLOCK = 11, BANKS = 14 };   // NVN binding index: banks c5, c7 and c14; c3..c16

// Address lookup plus a lease on the uploaded submission snapshot. No object bytes are allocated/copied per draw.
typedef struct {
    uint64_t addr;   // 0: empty
    int index;
    uint32_t have;
    uint64_t size, snap_gen;
    VkDescriptorBufferInfo snapshot;
    const uint8_t *data;
    void *lease;
    uint64_t target; // storage pointer owners can select a different pointee without changing identity or range
} Seen;

typedef struct { uint64_t blocks, self, side, nearer, none, checked, intact, still, small, big; } Stat;

static struct {
    int on;          // 1 knob on and DLAA on this frame
    int knob;        // -1 not read
    Seen *t[2];      // this frame's bound blocks, last frame's
    uint8_t (*bytes[2])[CHECK];   // TOTK_MVPREV_CHECK: the first CHECK bytes of each entry of t[k], NULL without the check
    int timed;       // TOTK_MVOBJ_TIMES: the summary's times
    uint32_t frames;
    Stat st[BANKS];  // per binding index
    uint64_t last[BANKS];   // the block the last scene draw bound per binding index
    uint64_t last_size[BANKS], last_gen[BANKS];
    uint32_t mask;          // binding indices kept (vk_mvprev_want): bones, place and clock from the start
    uint64_t ns_draw, ns_frame;   // time in vk_mvprev_draw and vk_mvprev_frame since the last summary
} P = {.knob = -1, .mask = 1u << BONES | 1u << PLACE | 1u << CLOCK};

static uint32_t slot_of(uint64_t addr, int index) {
    uint64_t h = (addr ^ (uint64_t)index * 0x9E3779B97F4A7C15ull) * 0xff51afd7ed558ccdull;
    return (uint32_t)(h >> 40) & (TABLE - 1);
}

static Seen *find(Seen *t, uint64_t addr, int index, int add) {
    for (uint32_t i = slot_of(addr, index), k = 0; k < TABLE; i = (i + 1) & (TABLE - 1), k++) {
        if (t[i].addr == addr && t[i].index == index) return &t[i];
        if (!t[i].addr) {
            if (!add) return NULL;
            t[i].addr = addr, t[i].index = index;
            return &t[i];
        }
    }
    return NULL;
}

int vk_mvprev_on(void) { return P.on > 0; }

static void clear_table(Seen *t) {
    if (!t) return;
    for (uint32_t i = 0; i < TABLE; i++) vk_usnap_release(t[i].lease);
    memset(t, 0, TABLE * sizeof *t);
}

void vk_mvprev_clear(void) { clear_table(P.t[0]); clear_table(P.t[1]); }

// Storage identity is its pointer's owning block (or a direct storage address), never today's pointee address.
// Only already uploaded snapshot ranges qualify; GPU-written/live-only storage has no immutable history here.
void vk_mvprev_storage(int key, uint64_t identity, uint64_t addr, uint64_t size) {
    if (P.on <= 0 || !P.t[0] || !identity) return;
    Seen *s = find(P.t[0], identity, key, 1);
    if (!s) return;
    uint64_t gen = vk_usnap_gen();
    if (s->snap_gen == gen && s->size == size && s->target == addr) return;
    vk_usnap_release(s->lease);
    s->lease = NULL, s->data = NULL, s->size = size, s->snap_gen = gen;
    s->target = addr;
    if (size) s->lease = vk_usnap_retain(addr, size, &s->snapshot, &s->data);
}

int vk_mvprev_self(int index, uint64_t addr, uint64_t size, uint64_t fence,
                   VkDescriptorBufferInfo *out, const uint8_t **bytes) {
    if (P.on <= 0 || !P.t[1] || !size) return 0;
    Seen *s = find(P.t[1], addr, index, 0);
    if (!s || !s->lease || size != s->size || !fence) return 0;
    vk_usnap_use(s->lease, fence);
    *out = s->snapshot;
    *bytes = s->data;
    return 1;
}

void vk_mvprev_want(uint32_t index_mask) {
    static int fixed = -1;
    if (fixed < 0) fixed = !rt_knob("MVPREV_GROW", "1");   // TOTK_MVPREV_GROW=0: c5, c7 and c14 only (test)
    if (!fixed) P.mask |= index_mask & ((1u << BANKS) - 1);
}

uint64_t vk_mvprev_step(int stage, int index, uint64_t size) {
    if (P.on <= 0 || stage != 0 || index < 0 || index >= BANKS || !(P.mask >> index & 1)) return 0;
    return (size + 255) & ~(uint64_t)255;
}

// Translation distance between the first matrices of two blocks (rows' fourth floats).
static float moved(const uint8_t *a, const uint8_t *b) {
    const float *x = (const float *)a, *y = (const float *)b;
    float dx = x[3] - y[3], dy = x[7] - y[7], dz = x[11] - y[11];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

int vk_mvprev_find(int index, uint64_t addr, uint64_t size, uint64_t *prev, int *how) {
    *how = MVPREV_NONE;
    if (P.on <= 0 || !P.t[1]) return 0;
    if (find(P.t[1], addr, index, 0)) {
        *prev = addr, *how = MVPREV_SELF;
        return 1;
    }
    uint64_t step = (size + 255) & ~(uint64_t)255, cand[2];
    int n = 0;
    if (find(P.t[1], addr + step, index, 0)) cand[n++] = addr + step;
    if (addr >= step && find(P.t[1], addr - step, index, 0)) cand[n++] = addr - step;
    if (!n) return 0;
    if (n == 2) {   // the copies of adjacent objects interleave: the one with more words equal to this frame's
        const uint32_t *c = (const uint32_t *)vk_usnap_bytes(addr, 64), *a = (const uint32_t *)vk_usnap_bytes(cand[0], 64),
                       *b = (const uint32_t *)vk_usnap_bytes(cand[1], 64);
        if (!c || !a || !b) return 0;
        int na = 0, nb = 0;
        for (int w = 0; w < 16; w++) na += c[w] == a[w], nb += c[w] == b[w];
        if (nb > na) cand[0] = cand[1];
        *how = MVPREV_NEARER;
    } else {
        *how = MVPREV_SIDE;
    }
    *prev = cand[0];
    return 1;
}

static void mvprev_draw_(VkExec *ex);
void vk_mvprev_draw(VkExec *ex) {
    if (P.on <= 0) return;
    if (!P.timed) return mvprev_draw_(ex);   // the clock reads were 1 % of the render thread
    uint64_t t0 = kern_now_ns();
    mvprev_draw_(ex);
    P.ns_draw += kern_now_ns() - t0;
}

static void mvprev_draw_(VkExec *ex) {
    const VkExecTarget *z = &ex->depth;
    if (!z->image || z->width < DLAA_SCENE_MIN_W || z->format != VK_FORMAT_D32_SFLOAT) return;
    int check = P.bytes[0] != NULL;   // TOTK_MVPREV_CHECK=1: the summary's checks (3.6 ms a frame)
    uint64_t gen = vk_usnap_gen();
    for (int index = 0; index < BANKS; index++) {
        if (!(P.mask >> index & 1)) continue;
        const p_bind_buf *u = &ex->ubo[0][index];
        if (!u->addr || !u->size || (u->addr == P.last[index] && u->size == P.last_size[index] && gen == P.last_gen[index])) continue;
        P.last[index] = u->addr;
        P.last_size[index] = u->size, P.last_gen[index] = gen;
        Seen *k = find(P.t[0], u->addr, index, 1);
        if (!k) continue;
        if (k->snap_gen != gen || k->size != u->size) {
            vk_usnap_release(k->lease);
            k->lease = NULL, k->data = NULL, k->size = u->size, k->snap_gen = gen;
            // Uniform snapshots are capped at 64 KiB; never claim a partial retained range as full history.
            if (u->size <= (64u << 10))
                k->lease = vk_usnap_retain(u->addr, u->size, &k->snapshot, &k->data);
        }
        if (k->have || !check) continue;
        int w = index;
        uint32_t want = u->size < CHECK ? (uint32_t)u->size : CHECK;
        const uint8_t *cur = vk_usnap_bytes(u->addr, want);
        if (!cur) continue;
        memcpy(P.bytes[0][k - P.t[0]], cur, want), k->have = want;
        Stat *s = &P.st[w];
        s->blocks++;
        uint64_t prev;
        int how;
        if (!vk_mvprev_find(index, u->addr, u->size, &prev, &how)) {
            s->none++;
            continue;
        }
        if (how == MVPREV_SELF) s->self++;
        else if (how == MVPREV_SIDE) s->side++;
        else s->nearer++;
        const Seen *last = find(P.t[1], prev, index, 0);
        const uint8_t *now = vk_usnap_bytes(prev, want);
        if (last && now && last->have >= want && want >= 48) {
            s->checked++;
            if (!memcmp(now, P.bytes[1][last - P.t[1]], want)) s->intact++;
            float d = moved(cur, now);
            if (d < 0.01f) s->still++;
            else if (d < 1.0f) s->small++;
            else s->big++;
        }
    }
}

void vk_mvprev_frame(void) {
    // RSF reconstruction owns these histories. Still process an off transition
    // when availability disappears so stale object/bone blocks are discarded.
    int available = vk_dlaa_temporal_available();
    if (!available && P.on <= 0) return;
    if (P.knob < 0) {
        P.knob = rt_knob("DLAA_OBJMV", "1");
        if (P.knob) {
            P.t[0] = calloc(TABLE, sizeof(Seen)), P.t[1] = calloc(TABLE, sizeof(Seen));
            if (!P.t[0] || !P.t[1]) P.knob = 0;
            if (P.knob && rt_knob("MVPREV_CHECK", "0")) {
                P.bytes[0] = calloc(TABLE, CHECK), P.bytes[1] = calloc(TABLE, CHECK);
                if (!P.bytes[0] || !P.bytes[1]) free(P.bytes[0]), free(P.bytes[1]), P.bytes[0] = P.bytes[1] = NULL;
            }
            P.timed = rt_knob("MVOBJ_TIMES", "0");
        }
    }
    int on = available && P.knob;
    if (on != (P.on > 0)) {
        P.on = on ? 1 : -1;
        vk_mvprev_clear();
        memset(P.last, 0, sizeof P.last);
        return;
    }
    if (P.on <= 0) return;
    uint64_t t0 = P.timed ? kern_now_ns() : 0;
    Seen *t = P.t[1];
    P.t[1] = P.t[0], P.t[0] = t;
    uint8_t (*b)[CHECK] = P.bytes[1];
    P.bytes[1] = P.bytes[0], P.bytes[0] = b;
    clear_table(P.t[0]);
    memset(P.last, 0, sizeof P.last);
    if (P.timed) P.ns_frame += kern_now_ns() - t0;
    if (++P.frames % SUMMARY) return;
    if (P.timed) rt_log("mv_prev: %.3f ms a frame in the draws, %.3f ms at the present", P.ns_draw / 1e6 / SUMMARY,
           P.ns_frame / 1e6 / SUMMARY);
    P.ns_draw = P.ns_frame = 0;
    for (int w = 0; w < BANKS; w++) {
        Stat *s = &P.st[w];
        if (!s->blocks) continue;
        double b = (double)s->blocks, c = s->checked ? (double)s->checked : 1;
        rt_log("mv_prev: c%d: %llu blocks: self %.1f %%, side %.1f %%, nearer %.1f %%, none %.1f %%; intact %.1f %% of "
               "%llu; moved: still %.1f %%, under one unit %.1f %%, more %.1f %%",
               w + 3, (unsigned long long)s->blocks, 100 * s->self / b, 100 * s->side / b, 100 * s->nearer / b,
               100 * s->none / b, 100 * s->intact / c, (unsigned long long)s->checked, 100 * s->still / c,
               100 * s->small / c, 100 * s->big / c);
    }
    memset(P.st, 0, sizeof P.st);
}
