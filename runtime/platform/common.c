// The parts of platform.h built on its locks rather than on the OS: recursive mutexes, one-time initialisation and
// events. Every OS shares them.
#include "platform.h"

// ---- recursive mutex: owner is the holder's thread id (0 when free); only the holder writes depth.

void plat_rmutex_lock(PlatRMutex *m) {
    uint32_t me = plat_thread_id();
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == me) {
        m->depth++;
        return;
    }
    plat_mutex_lock(&m->m);
    __atomic_store_n(&m->owner, me, __ATOMIC_RELAXED);
    m->depth = 1;
}

void plat_rmutex_unlock(PlatRMutex *m) {
    if (--m->depth) return;
    __atomic_store_n(&m->owner, 0, __ATOMIC_RELAXED);
    plat_mutex_unlock(&m->m);
}

// ---- once: state 0 not run, 1 running, 2 done. Waiters for a running initialiser sleep on one process-wide
// condition variable; initialisers are rare and short.

static PlatMutex once_lock = PLAT_MUTEX_INIT;
static PlatCond once_done = PLAT_COND_INIT;

void plat_once(PlatOnce *o, void (*fn)(void *arg), void *arg) {
    if (__atomic_load_n(&o->state, __ATOMIC_ACQUIRE) == 2) return;
    int32_t expected = 0;
    if (__atomic_compare_exchange_n(&o->state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        fn(arg);
        plat_mutex_lock(&once_lock);
        __atomic_store_n(&o->state, 2, __ATOMIC_RELEASE);
        plat_cond_broadcast(&once_done);
        plat_mutex_unlock(&once_lock);
        return;
    }
    plat_mutex_lock(&once_lock);
    while (__atomic_load_n(&o->state, __ATOMIC_ACQUIRE) != 2) plat_cond_wait(&once_done, &once_lock);
    plat_mutex_unlock(&once_lock);
}

// ---- events

void plat_event_init(PlatEvent *e, int manual, int initially_set) {
    plat_mutex_init(&e->m);
    plat_cond_init(&e->c);
    e->manual = manual;
    e->set = initially_set;
}

void plat_event_set(PlatEvent *e) {
    plat_mutex_lock(&e->m);
    e->set = 1;
    if (e->manual) plat_cond_broadcast(&e->c);
    else plat_cond_signal(&e->c);
    plat_mutex_unlock(&e->m);
}

void plat_event_reset(PlatEvent *e) {
    plat_mutex_lock(&e->m);
    e->set = 0;
    plat_mutex_unlock(&e->m);
}

void plat_event_wait(PlatEvent *e) {
    plat_mutex_lock(&e->m);
    while (!e->set) plat_cond_wait(&e->c, &e->m);
    if (!e->manual) e->set = 0;
    plat_mutex_unlock(&e->m);
}

int plat_event_wait_ns(PlatEvent *e, uint64_t ns) {
    uint64_t end = plat_now_ns() + ns;
    plat_mutex_lock(&e->m);
    while (!e->set) {
        uint64_t now = plat_now_ns();
        if (now >= end) break;
        plat_cond_wait_ns(&e->c, &e->m, end - now);
    }
    int got = e->set;
    if (got && !e->manual) e->set = 0;
    plat_mutex_unlock(&e->m);
    return got;
}
