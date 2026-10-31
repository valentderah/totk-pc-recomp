// Exercise the real virtual-core scheduler without game code or guest stacks.
#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
static unsigned host_yields;
static int counted_host_yield(void) { ++host_yields; return 0; }
#define sched_yield counted_host_yield
#include "threads.c"
#undef sched_yield

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "%s\n", message); abort(); }
uint64_t runtime_now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
void *runtime_symbol(const char *name) { (void)name; return NULL; }
void runtime_thread_init(void) { abort(); }
void dm_thread_enter(uint64_t a, uint64_t b, uint64_t c, uint64_t *d) { abort(); }
_Noreturn void runtime_thread_leave(const uint64_t *a, void *b, void (*c)(void *)) { abort(); }

static atomic_int peer_ran;
static thr_t *peer_waiter;
static void *peer_run(void *arg) {
    thr_t *peer = arg;
    pthread_mutex_lock(&G);
    core_take(peer);
    assert(peer->cur_core >= 0 && g_core_owner[peer->cur_core] == peer);
    assert(core_allowed(peer, peer->cur_core));
    while (peer_waiter && g_core_wait != peer_waiter) {
        pthread_mutex_unlock(&G);
        struct timespec pause = {0, 100000};
        nanosleep(&pause, NULL);
        pthread_mutex_lock(&G);
    }
    atomic_store(&peer_ran, 1);
    core_put(peer);
    pthread_mutex_unlock(&G);
    return NULL;
}

static void sleep_svc(int64_t ns) {
    uint64_t regs[32] = {(uint64_t)ns};
    assert(svc_thread(0x0B, regs) == 1);
}

static void wait_peer_queued(thr_t *peer) {
    for (;;) {
        pthread_mutex_lock(&G);
        int queued = g_core_wait == peer;
        pthread_mutex_unlock(&G);
        if (queued) return;
        struct timespec pause = {0, 100000};
        nanosleep(&pause, NULL);
    }
}

static void check_peer(thr_t *self, int priority, int core, int64_t ns, int should_run) {
    thr_t peer = {.prio = priority, .core = core, .mask = 1u << core, .cur_core = -1};
    thr_t other_owner = {.cur_core = core};
    pthread_cond_init(&peer.cv, NULL);
    atomic_store(&peer_ran, 0);
    if (core != self->cur_core) g_core_owner[core] = &other_owner;
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, peer_run, &peer));
    // Wait for the peer to enter the real core queue, not a timing assumption.
    wait_peer_queued(&peer);
    sleep_svc(ns);
    assert(atomic_load(&peer_ran) == should_run);
    assert(self->cur_core == 0 && g_core_owner[0] == self);
    assert(host_yields == 0);
    // An ineligible peer must still run when its own core becomes available.
    if (!should_run) {
        pthread_mutex_lock(&G);
        if (core == 0) core_put(self); else core_put(&other_owner);
        pthread_mutex_unlock(&G);
    }
    assert(!pthread_join(worker, NULL));
    pthread_mutex_lock(&G);
    core_take(self);
    assert(!g_core_wait);
    pthread_mutex_unlock(&G);
    pthread_cond_destroy(&peer.cv);
}

typedef struct { thr_t peer; thr_t *self; } AffinityChange;
static void *change_waiting_affinity(void *arg) {
    AffinityChange *change = arg;
    thr_t *peer = &change->peer, *self = change->self;
    pthread_setspecific(g_keythr, peer);
    pthread_mutex_lock(&G);
    core_take(peer);
    assert(self->cur_core == -1 && self->yield_core_mask == 1);
    pthread_mutex_unlock(&G);
    uint64_t regs[32] = {0, 0, self->handle};
    svc_thread(0x0E, regs);
    assert(regs[0] == 0 && regs[2] == 7); // Public affinity stays intact while yielding.
    regs[0] = self->handle; regs[1] = 1; regs[2] = 2;
    svc_thread(0x0F, regs);
    assert(regs[0] == 0);
    pthread_mutex_lock(&G);
    assert(self->cur_core == 1 && g_core_owner[1] == self);
    core_put(peer);
    pthread_mutex_unlock(&G);
    return NULL;
}

static void check_affinity_change(thr_t *self) {
    self->mask = 7;
    AffinityChange change = {.peer = {.prio = self->prio, .core = 0, .mask = 1, .cur_core = -1}, .self = self};
    pthread_cond_init(&change.peer.cv, NULL);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, change_waiting_affinity, &change));
    wait_peer_queued(&change.peer);
    sleep_svc(0);
    assert(!pthread_join(worker, NULL));
    assert(self->cur_core == 1 && self->mask == 2 && !self->yield_core_mask);
    pthread_mutex_lock(&G);
    core_put(self); self->mask = 1; self->core = 0; core_take(self);
    pthread_mutex_unlock(&G);
    pthread_cond_destroy(&change.peer.cv);
}

static void check_waiter_preemption(thr_t *self) {
    thr_t peer = {.prio = self->prio - 1, .core = 1, .mask = 2, .cur_core = -1};
    thr_t owner = {.prio = self->prio, .core = 1, .mask = 2, .cur_core = 1};
    pthread_cond_init(&peer.cv, NULL);
    pthread_mutex_lock(&G);
    peer.handle = h_new(O_THREAD, &peer);
    g_core_owner[1] = &owner;
    pthread_mutex_unlock(&G);
    atomic_store(&peer_ran, 0);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, peer_run, &peer));
    wait_peer_queued(&peer);
    uint64_t regs[32] = {peer.handle, 0, 1};
    svc_thread(0x0F, regs);
    assert(regs[0] == 0 && self->preempt);
    runtime_core_checkpoint();
    assert(!pthread_join(worker, NULL));
    assert(atomic_load(&peer_ran) && self->cur_core == 0);
    pthread_mutex_lock(&G);
    core_put(&owner);
    g_h[peer.handle - 0x1000].type = O_FREE;
    g_h[peer.handle - 0x1000].obj = NULL;
    pthread_mutex_unlock(&G);
    pthread_cond_destroy(&peer.cv);
}

static void check_changed_grant(thr_t *self) {
    thr_t peer = {.prio = self->prio, .core = 0, .mask = 1, .cur_core = -1};
    pthread_cond_init(&peer.cv, NULL);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, peer_run, &peer));
    wait_peer_queued(&peer);
    pthread_mutex_lock(&G);
    core_put(self);
    assert(peer.cur_core == 0 && !g_core_wait);
    // Force the affinity-change window after grant, before cond_wait returns.
    peer.mask = 2;
    core_take(self);
    pthread_mutex_unlock(&G);
    assert(!pthread_join(worker, NULL));
    assert(self->cur_core == 0 && !g_core_owner[1]);
    pthread_cond_destroy(&peer.cv);
}

int main(int argc, char **argv) {
    unsetenv("TOTK_FREE_CORES");
    unsetenv("TOTK_YIELD_BACKOFF");
    // The real host allocates TLS keys before registering the guest main thread.
    pthread_key_t reserved;
    assert(!pthread_key_create(&reserved, NULL));
    svc_thread_main_init();
    thr_t *self = cur();
    if (argc == 2 && !strcmp(argv[1], "--backoff")) {
        setenv("TOTK_YIELD_BACKOFF", "1:0", 1);
        self->mask = 7;
        peer_waiter = self;
        check_peer(self, self->prio + 1, 0, 0, 1);
        assert(!self->yield_core_mask && self->mask == 7);
        puts("{\"probe\":\"guest-yield-backoff\",\"status\":\"pass\"}");
        return 0;
    }
    self->mask = 1;
    for (int ns = 0; ns >= -2; --ns)
        for (int i = 0; i < 10000; ++i) sleep_svc(ns);
    assert(host_yields == 0 && self->cur_core == 0 && g_core_owner[0] == self);
    for (int i = 0; i < 100; ++i) {
        check_peer(self, self->prio, 0, 0, 1);
        check_peer(self, self->prio - 1, 0, 0, 1);
        check_peer(self, self->prio + 1, 0, 0, 0);
        check_peer(self, self->prio + 1, 0, -1, 0);
        check_peer(self, self->prio + 1, 0, -2, 1);
        check_peer(self, self->prio, 1, 0, 0);
        check_peer(self, self->prio + 1, 0, 1000000, 1);
        self->mask = 7;
        check_peer(self, self->prio, 0, 0, 1);
        assert(self->mask == 7 && self->yield_core_mask == 0);
        self->mask = 1;
        check_affinity_change(self);
        check_waiter_preemption(self);
        check_changed_grant(self);
    }
    g_cores_on = 0;
    sleep_svc(-3);
    assert(host_yields == 0);
    sleep_svc(0);
    assert(host_yields == 1);
    puts("{\"probe\":\"guest-yield\",\"status\":\"pass\",\"empty_yields\":30000,\"peer_cases\":1100}");
    return 0;
}
