// Real pool queue/worker/ownership code, fake Vulkan allocation backend, real worker thread. No device.
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include "../runtime/vk_snapshot_pool.c"

struct PlatThread { pthread_t id; PlatThreadFn fn; void *arg; };
static pthread_mutex_t mutexes[32]; static int nm;
static pthread_cond_t conditions[32]; static int nc;
void plat_mutex_init(PlatMutex *m) { m->p = &mutexes[nm++]; pthread_mutex_init(m->p, NULL); }
void plat_cond_init(PlatCond *c) { c->p = &conditions[nc++]; pthread_cond_init(c->p, NULL); }
void plat_mutex_lock(PlatMutex *m) { pthread_mutex_lock(m->p); }
int plat_mutex_trylock(PlatMutex *m) { return !pthread_mutex_trylock(m->p); }
void plat_mutex_unlock(PlatMutex *m) { pthread_mutex_unlock(m->p); }
void plat_cond_wait(PlatCond *c, PlatMutex *m) { pthread_cond_wait(c->p, m->p); }
void plat_cond_signal(PlatCond *c) { pthread_cond_signal(c->p); }
void plat_cond_broadcast(PlatCond *c) { pthread_cond_broadcast(c->p); }
static void *run_thread(void *arg) { struct PlatThread *t = arg; t->fn(t->arg); return NULL; }
PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    (void)name; PlatThread *t = calloc(1, sizeof *t); t->fn = fn; t->arg = arg;
    if (pthread_create(&t->id, NULL, run_thread, t)) { free(t); return NULL; } return t;
}
void plat_thread_join(PlatThread *t) { pthread_join(t->id, NULL); free(t); }
static pthread_t recording;
static atomic_int created, destroyed, fail_create;
static int fake_measure(void *u, const VkSnapshotShape *s, uint64_t *bytes, uint64_t *copy) {
    (void)u; assert(!pthread_equal(recording, pthread_self())); *bytes = *copy = (uint64_t)s->width*s->height*4; return 1;
}
static int fake_create(void *u, const VkSnapshotShape *s, VkSnapshotBacking *b) {
    (void)u; assert(!pthread_equal(recording, pthread_self()));
    int id = atomic_fetch_add(&created, 1)+1;
    *b = (VkSnapshotBacking){.image = (VkImage)(uintptr_t)id, .view = (VkImageView)(uintptr_t)id,
        .allocation_bytes = (uint64_t)s->width*s->height*4, .copy_bytes = (uint64_t)s->width*s->height*4};
    return !atomic_load(&fail_create);
}
static void fake_destroy(void *u, VkSnapshotBacking *b) {
    (void)u; assert(!pthread_equal(recording, pthread_self())); assert(b->image); atomic_fetch_add(&destroyed, 1); *b = (VkSnapshotBacking){0};
}
static VkSnapshotPool *test_pool(uint64_t memory, uint64_t copies, uint32_t resources) {
    VkSnapshotLimits lim = {memory, copies, resources}; VkSnapshotBackend backend = {fake_measure, fake_create, fake_destroy, NULL};
    return vk_snapshot_pool_start(&lim, &backend);
}
static void wait_shape(VkSnapshotPool *p, VkSnapshotShape shape, int expected) {
    plat_mutex_lock(&p->lock);
    for (;;) {
        int found = 0;
        for (int i=0;i<VK_SNAPSHOT_SLOTS;i++) if (vk_snapshot_shape_equal(&p->slots[i].shape,&shape) && p->slots[i].state==expected) found=1;
        if (found) break;
        plat_cond_wait(&p->changed,&p->lock);
    }
    plat_mutex_unlock(&p->lock);
}
#ifndef SNAPSHOT_NO_MAIN
int main(void) {
    recording = pthread_self(); VkSnapshotPool *p = test_pool(128,128,1); assert(p);
    VkSnapshotShape shape = {VK_FORMAT_R8G8B8A8_UNORM,4,4,1}, big = {VK_FORMAT_R8G8B8A8_UNORM,8,4,1};
    VkSnapshotToken a,b; VkSnapshotBacking backing;
    assert(!vk_snapshot_pool_acquire(p,&shape,1,&a,&backing)); wait_shape(p,shape,READY);
    plat_mutex_lock(&p->lock); assert(!vk_snapshot_pool_acquire(p,&shape,1,&a,&backing)); plat_mutex_unlock(&p->lock);
    assert(vk_snapshot_pool_acquire(p,&shape,1,&a,&backing)); assert(!backing.initialized);
    assert(!vk_snapshot_pool_acquire(p,&shape,1,&b,&backing)); // frame resource cap
    assert(!vk_snapshot_pool_publish(p,a,1)); assert(!vk_snapshot_pool_consume(p,a,10));
    assert(vk_snapshot_pool_record(p,a,10)); assert(!vk_snapshot_pool_cancel(p,a));
    assert(!vk_snapshot_pool_stop(p)); vk_snapshot_pool_submitted(p,10,5);
    assert(!vk_snapshot_pool_publish(p,a,2)); assert(vk_snapshot_pool_publish(p,a,1));
    assert(vk_snapshot_pool_consume(p,a,20)); assert(vk_snapshot_pool_consume(p,a,20));
    assert(vk_snapshot_pool_release(p,a)); vk_snapshot_pool_collect(p,100); // pending consumer still blocks reuse
    assert(!vk_snapshot_pool_stop(p)); vk_snapshot_pool_commands_reset(p,20);
    assert(vk_snapshot_pool_acquire(p,&shape,2,&b,&backing)); assert(backing.initialized && b.generation!=a.generation);
    assert(!vk_snapshot_pool_release(p,a)); assert(vk_snapshot_pool_record(p,b,30));
    vk_snapshot_pool_submitted(p,30,105); assert(vk_snapshot_pool_publish(p,b,2));
    assert(vk_snapshot_pool_consume(p,b,40)); vk_snapshot_pool_submitted(p,40,110); assert(vk_snapshot_pool_release(p,b));
    vk_snapshot_pool_collect(p,109); assert(!vk_snapshot_pool_stop(p)); vk_snapshot_pool_collect(p,110);
    assert(!vk_snapshot_pool_acquire(p,&big,3,&a,&backing)); wait_shape(p,big,REJECTED); // memory remaining 64, needs 128
    assert(p->memory==64 && atomic_load(&created)==1);
    assert(vk_snapshot_pool_acquire(p,&shape,3,&a,&backing)); assert(vk_snapshot_pool_record(p,a,50));
    assert(vk_snapshot_pool_release(p,a)); vk_snapshot_pool_collect(p,1000); assert(!vk_snapshot_pool_stop(p));
    vk_snapshot_pool_commands_reset(p,50); assert(vk_snapshot_pool_stop(p)); assert(created==destroyed);
    atomic_store(&fail_create,1); p=test_pool(128,128,1);
    assert(!vk_snapshot_pool_acquire(p,&shape,1,&a,&backing)); wait_shape(p,shape,REJECTED);
    assert(!p->memory && created==destroyed); assert(vk_snapshot_pool_stop(p));
    return 0;
}
#endif
