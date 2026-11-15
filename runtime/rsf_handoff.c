#include "rsf_handoff.h"
#include "platform/platform.h"
#include <string.h>

enum { QUEUES = 1024, PENDING = 64 };
static struct Queue {
    uint64_t address, framework;
    void *publisher;
    uint64_t sources[PENDING];
    unsigned head, count;
    int fresh, tracked, invalid;
} queues[QUEUES];
static PlatMutex lock = PLAT_MUTEX_INIT;

static struct Queue *find(uint64_t address, int create) {
    if (!address) return NULL;
    unsigned h = (unsigned)((address >> 4) * 0x9e3779b97f4a7c15ull) & (QUEUES - 1);
    for (unsigned n = 0; n < QUEUES; n++, h = (h + 1) & (QUEUES - 1)) {
        if (queues[h].address == address) return &queues[h];
        if (!queues[h].address) {
            if (!create) return NULL;
            queues[h].address = address;
            return &queues[h];
        }
    }
    return NULL;
}

void rsf_handoff_initialize(uint64_t address) {
    plat_mutex_lock(&lock);
    struct Queue *q = find(address, 1);
    if (q) { memset(q, 0, sizeof *q); q->address = address; q->fresh = 1; }
    plat_mutex_unlock(&lock);
}

void rsf_handoff_foreign(uint64_t address) {
    plat_mutex_lock(&lock);
    struct Queue *q = find(address, 0);
    if (q) { q->fresh = 0; q->invalid = 1; q->count = 0; }
    plat_mutex_unlock(&lock);
}

void rsf_handoff_finalize(uint64_t address) { rsf_handoff_foreign(address); }

int rsf_handoff_publish(uint64_t address, uint64_t message, void *publisher, uint64_t framework, uint64_t source) {
    int accepted = 0;
    plat_mutex_lock(&lock);
    struct Queue *q = find(address, 0);
    if (!q) goto done;
    if (!framework || !publisher || message != 1) goto invalidate;
    if (!q->tracked) {
        // Earlier unobserved/native work cannot acquire a later source's tag.
        if (!q->fresh) goto done;
        q->tracked = 1; q->framework = framework; q->publisher = publisher;
    }
    q->fresh = 0;
    if (q->invalid) goto done;
    if (q->framework != framework || q->publisher != publisher || q->count == PENDING) goto invalidate;
    q->sources[(q->head + q->count++) % PENDING] = source;
    accepted = 1;
    goto done;
invalidate:
    q->fresh = 0; q->invalid = 1; q->count = 0;
done:
    plat_mutex_unlock(&lock);
    return accepted;
}

int rsf_handoff_receive(uint64_t address, uint64_t message, uint64_t *source) {
    if (!source) return 0;
    *source = 0;
    int tracked = 0;
    plat_mutex_lock(&lock);
    struct Queue *q = find(address, 0);
    if (!q) goto done;
    if (!q->tracked) { q->fresh = 0; q->invalid = 1; goto done; }
    tracked = 1;
    if (q->invalid) goto done;
    if (message != 1 || !q->count) { q->invalid = 1; q->count = 0; goto done; }
    *source = q->sources[q->head];
    q->head = (q->head + 1) % PENDING;
    q->count--;
done:
    plat_mutex_unlock(&lock);
    return tracked;
}
