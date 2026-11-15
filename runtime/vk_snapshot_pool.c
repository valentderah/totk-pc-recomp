#include "vk_snapshot_pool.h"
#include "platform/platform.h"
#include <stdlib.h>
#include <string.h>

enum { EMPTY, QUEUED, BUILDING, READY, LEASED, REJECTED };
typedef struct {
    VkSnapshotShape shape;
    VkSnapshotBacking backing;
    uint64_t generation, frame, producer, fence, commands[VK_SNAPSHOT_COMMANDS];
    int state, recorded, submitted, published, released;
} Slot;
struct VkSnapshotPool {
    PlatMutex lock;
    PlatCond changed;
    PlatThread *worker;
    VkSnapshotBackend backend;
    VkSnapshotLimits limits;
    uint64_t memory, frame, copy_bytes, done;
    uint32_t resources;
    int stopping;
    Slot slots[VK_SNAPSHOT_SLOTS];
};

static int same_shape(const VkSnapshotShape *a, const VkSnapshotShape *b) {
    return a->format == b->format && a->width == b->width && a->height == b->height && a->levels == b->levels;
}
static int shape_valid(const VkSnapshotShape *s) {
    if (!s || s->format == VK_FORMAT_UNDEFINED || !s->width || !s->height || !s->levels || s->levels > 32) return 0;
    uint32_t n = s->width > s->height ? s->width : s->height, levels = 1;
    while (n > 1) n >>= 1, levels++;
    return s->levels <= levels;
}
static int pending(const Slot *s) {
    for (int i = 0; i < VK_SNAPSHOT_COMMANDS; i++) if (s->commands[i]) return 1;
    return 0;
}
static void recycle(VkSnapshotPool *p, Slot *s) {
    if (s->state == LEASED && s->released && !pending(s) && s->fence <= p->done) {
        s->state = READY; s->recorded = s->submitted = s->published = 0;
    }
}
static Slot *token_slot(VkSnapshotPool *p, VkSnapshotToken t) {
    return t.slot < VK_SNAPSHOT_SLOTS && p->slots[t.slot].state == LEASED &&
           p->slots[t.slot].generation == t.generation ? &p->slots[t.slot] : NULL;
}
static int add_command(Slot *s, uint64_t tag) {
    if (!tag) return 0;
    for (int i = 0; i < VK_SNAPSHOT_COMMANDS; i++) if (s->commands[i] == tag) return 1;
    for (int i = 0; i < VK_SNAPSHOT_COMMANDS; i++) if (!s->commands[i]) { s->commands[i] = tag; return 1; }
    return 0;
}
static void allocate_worker(void *arg) {
    VkSnapshotPool *p = arg;
    plat_mutex_lock(&p->lock);
    for (;;) {
        int at = -1;
        for (int i = 0; i < VK_SNAPSHOT_SLOTS; i++) if (p->slots[i].state == QUEUED) { at = i; break; }
        if (p->stopping) break;
        if (at < 0) { plat_cond_wait(&p->changed, &p->lock); continue; }
        Slot *s = &p->slots[at];
        s->state = BUILDING;
        VkSnapshotShape shape = s->shape;
        plat_mutex_unlock(&p->lock);
        uint64_t bytes = 0, copies = 0;
        int ok = p->backend.measure(p->backend.user, &shape, &bytes, &copies);
        plat_mutex_lock(&p->lock);
        ok = ok && bytes && copies && bytes <= p->limits.memory_bytes - p->memory &&
             copies <= p->limits.frame_copy_bytes;
        if (ok) p->memory += bytes;
        plat_mutex_unlock(&p->lock);
        VkSnapshotBacking backing = {0};
        if (ok) {
            ok = p->backend.create(p->backend.user, &shape, &backing);
            if (ok && (!backing.image || !backing.view || backing.allocation_bytes != bytes || backing.copy_bytes != copies)) ok = 0;
            if (!ok && (backing.image || backing.view || backing.allocation)) p->backend.destroy(p->backend.user, &backing);
            if (!ok) { plat_mutex_lock(&p->lock); p->memory -= bytes; plat_mutex_unlock(&p->lock); }
        }
        plat_mutex_lock(&p->lock);
        if (ok) s->backing = backing;
        s->state = ok ? READY : REJECTED;
        plat_cond_broadcast(&p->changed);
    }
    plat_mutex_unlock(&p->lock);
    for (int i = 0; i < VK_SNAPSHOT_SLOTS; i++)
        if (p->slots[i].backing.image) p->backend.destroy(p->backend.user, &p->slots[i].backing);
}

VkSnapshotPool *vk_snapshot_pool_start(const VkSnapshotLimits *limits, const VkSnapshotBackend *backend) {
    if (!limits || !backend || !backend->measure || !backend->create || !backend->destroy ||
        !limits->memory_bytes || !limits->frame_copy_bytes || !limits->frame_resources) return NULL;
    VkSnapshotPool *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->backend = *backend; p->limits = *limits;
    plat_mutex_init(&p->lock); plat_cond_init(&p->changed);
    p->worker = plat_thread_new(allocate_worker, p, "snapshot allocation");
    if (!p->worker) { free(p); return NULL; }
    return p;
}
int vk_snapshot_pool_acquire(VkSnapshotPool *p, const VkSnapshotShape *shape, uint64_t frame,
                             VkSnapshotToken *token, VkSnapshotBacking *out) {
    if (!p || !frame || !token || !out || !shape_valid(shape) || !plat_mutex_trylock(&p->lock)) return 0;
    int ready = -1, empty = -1, requested = 0, rejected = 0, ok = 0;
    if (p->stopping || frame < p->frame) goto end;
    if (frame != p->frame) p->frame = frame, p->resources = 0, p->copy_bytes = 0;
    if (p->resources >= p->limits.frame_resources) goto end;
    for (int i = 0; i < VK_SNAPSHOT_SLOTS; i++) {
        Slot *s = &p->slots[i]; recycle(p, s);
        if (s->state == EMPTY && empty < 0) empty = i;
        if (!same_shape(&s->shape, shape)) continue;
        if (s->state == READY && s->generation != UINT64_MAX) ready = i;
        requested |= s->state == QUEUED || s->state == BUILDING;
        rejected |= s->state == REJECTED;
    }
    if (ready >= 0) {
        Slot *s = &p->slots[ready];
        if (p->resources >= p->limits.frame_resources || s->backing.copy_bytes > p->limits.frame_copy_bytes - p->copy_bytes) goto end;
        s->state = LEASED; s->generation++; s->frame = frame; s->producer = s->fence = 0;
        s->released = s->recorded = s->submitted = s->published = 0;
        *token = (VkSnapshotToken){(uint32_t)ready, s->generation}; *out = s->backing;
        p->resources++; p->copy_bytes += s->backing.copy_bytes; ok = 1;
    } else if (!requested && !rejected && empty >= 0) {
        p->slots[empty].shape = *shape; p->slots[empty].state = QUEUED; plat_cond_signal(&p->changed);
    }
end:
    plat_mutex_unlock(&p->lock); return ok;
}
int vk_snapshot_pool_cancel(VkSnapshotPool *p, VkSnapshotToken t) {
    if (!p) return 0;
    plat_mutex_lock(&p->lock); Slot *s = token_slot(p, t); int ok = s && !s->recorded;
    if (ok) { s->released = 1; recycle(p, s); }
    plat_mutex_unlock(&p->lock); return ok;
}
int vk_snapshot_pool_record(VkSnapshotPool *p, VkSnapshotToken t, uint64_t tag) {
    if (!p) return 0;
    plat_mutex_lock(&p->lock); Slot *s = token_slot(p, t); int ok = s && !s->released && !s->recorded && add_command(s, tag);
    if (ok) s->producer = tag, s->recorded = 1;
    plat_mutex_unlock(&p->lock); return ok;
}
int vk_snapshot_pool_publish(VkSnapshotPool *p, VkSnapshotToken t, uint64_t frame) {
    if (!p) return 0;
    plat_mutex_lock(&p->lock); Slot *s = token_slot(p, t); int ok = s && !s->released && s->submitted && frame == s->frame;
    if (ok) s->published = 1;
    plat_mutex_unlock(&p->lock); return ok;
}
int vk_snapshot_pool_consume(VkSnapshotPool *p, VkSnapshotToken t, uint64_t tag) {
    if (!p) return 0;
    plat_mutex_lock(&p->lock); Slot *s = token_slot(p, t); int ok = s && !s->released && s->published && add_command(s, tag);
    plat_mutex_unlock(&p->lock); return ok;
}
int vk_snapshot_pool_release(VkSnapshotPool *p, VkSnapshotToken t) {
    if (!p) return 0;
    plat_mutex_lock(&p->lock); Slot *s = token_slot(p, t); int ok = s && !s->released;
    if (ok) { s->released = 1; recycle(p, s); }
    plat_mutex_unlock(&p->lock); return ok;
}
void vk_snapshot_pool_submitted(VkSnapshotPool *p, uint64_t tag, uint64_t fence) {
    if (!p || !tag || !fence) return;
    plat_mutex_lock(&p->lock);
    for (int i = 0; i < VK_SNAPSHOT_SLOTS; i++) {
        Slot *s = &p->slots[i];
        if (s->state != LEASED) continue;
        for (int j = 0; j < VK_SNAPSHOT_COMMANDS; j++) if (s->commands[j] == tag) {
            s->commands[j] = 0; if (fence > s->fence) s->fence = fence;
            if (s->producer == tag) s->submitted = 1, s->backing.initialized = 1;
        }
        recycle(p, s);
    }
    plat_mutex_unlock(&p->lock);
}
void vk_snapshot_pool_commands_reset(VkSnapshotPool *p, uint64_t tag) {
    if (!p || !tag) return;
    plat_mutex_lock(&p->lock);
    for (int i = 0; i < VK_SNAPSHOT_SLOTS; i++) {
        Slot *s = &p->slots[i]; if (s->state != LEASED) continue;
        for (int j = 0; j < VK_SNAPSHOT_COMMANDS; j++) if (s->commands[j] == tag) s->commands[j] = 0;
        if (s->producer == tag && !s->submitted) s->recorded = s->published = 0, s->producer = 0;
        recycle(p, s);
    }
    plat_mutex_unlock(&p->lock);
}
void vk_snapshot_pool_collect(VkSnapshotPool *p, uint64_t done) {
    if (!p) return;
    plat_mutex_lock(&p->lock); if (done > p->done) p->done = done;
    for (int i = 0; i < VK_SNAPSHOT_SLOTS; i++) recycle(p, &p->slots[i]);
    plat_mutex_unlock(&p->lock);
}
int vk_snapshot_pool_stop(VkSnapshotPool *p) {
    if (!p) return 1;
    plat_mutex_lock(&p->lock);
    for (int i = 0; i < VK_SNAPSHOT_SLOTS; i++) if (p->slots[i].state == LEASED) {
        plat_mutex_unlock(&p->lock); return 0;
    }
    p->stopping = 1; plat_cond_signal(&p->changed); plat_mutex_unlock(&p->lock);
    plat_thread_join(p->worker); free(p); return 1;
}
