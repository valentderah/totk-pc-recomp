#include <assert.h>
#include "rsf_handoff.h"
#include "platform/platform.h"

static int locked;
void plat_mutex_lock(PlatMutex *mutex) { (void)mutex; assert(!locked++); }
void plat_mutex_unlock(PlatMutex *mutex) { (void)mutex; assert(!--locked); }

int main(void) {
    int publisher, foreign;
    uint64_t source;
    assert(!rsf_handoff_publish(0x100, 1, &publisher, 7, 1)); // Initialization must be observed.
    rsf_handoff_initialize(0x100);
    assert(rsf_handoff_publish(0x100, 1, &publisher, 7, 0)); // First draw precedes any completed calc.
    assert(rsf_handoff_publish(0x100, 1, &publisher, 7, 10));
    assert(rsf_handoff_publish(0x100, 1, &publisher, 7, 11));
    assert(rsf_handoff_receive(0x100, 1, &source) && source == 0);
    assert(rsf_handoff_receive(0x100, 1, &source) && source == 10);
    assert(rsf_handoff_receive(0x100, 1, &source) && source == 11);
    for (uint64_t i = 1; i <= 64; i++) assert(rsf_handoff_publish(0x100, 1, &publisher, 7, i));
    for (uint64_t i = 1; i <= 32; i++) assert(rsf_handoff_receive(0x100, 1, &source) && source == i);
    for (uint64_t i = 65; i <= 96; i++) assert(rsf_handoff_publish(0x100, 1, &publisher, 7, i));
    for (uint64_t i = 33; i <= 96; i++) assert(rsf_handoff_receive(0x100, 1, &source) && source == i);
    assert(rsf_handoff_publish(0x100, 1, &publisher, 7, 100));
    rsf_handoff_foreign(0x100);
    assert(rsf_handoff_receive(0x100, 1, &source) && !source);
    assert(!rsf_handoff_publish(0x100, 1, &publisher, 7, 101));
    rsf_handoff_initialize(0x100);
    assert(rsf_handoff_publish(0x100, 1, &publisher, 7, 102));
    assert(!rsf_handoff_publish(0x100, 1, &foreign, 7, 103));
    assert(rsf_handoff_receive(0x100, 1, &source) && !source);
    rsf_handoff_initialize(0x100);
    assert(rsf_handoff_publish(0x100, 1, &publisher, 7, 104));
    assert(rsf_handoff_receive(0x100, 2, &source) && !source);
    rsf_handoff_initialize(0x100);
    for (uint64_t i = 1; i <= 64; i++) assert(rsf_handoff_publish(0x100, 1, &publisher, 7, i));
    assert(!rsf_handoff_publish(0x100, 1, &publisher, 7, 65));
    assert(rsf_handoff_receive(0x100, 1, &source) && !source);
    rsf_handoff_initialize(0x100);
    assert(rsf_handoff_publish(0x100, 1, &publisher, 7, 105));
    rsf_handoff_foreign(0x100);
    assert(rsf_handoff_receive(0x100, 1, &source) && !source);
    rsf_handoff_initialize(0x200);
    assert(!rsf_handoff_receive(0x200, 1, &source)); // Unobserved prior publication invalidates freshness.
    assert(!rsf_handoff_publish(0x200, 1, &publisher, 7, 1));
    rsf_handoff_initialize(0x300);
    assert(rsf_handoff_publish(0x300, 1, &publisher, 7, 1));
    assert(!rsf_handoff_publish(0x300, 1, &publisher, 8, 2));
    assert(rsf_handoff_receive(0x300, 1, &source) && !source);
    for (uint64_t i = 1; i <= 1024; i++) rsf_handoff_initialize(0x10000 + i * 0x10);
    assert(!rsf_handoff_publish(0x1000000, 1, &publisher, 7, 1));
    rsf_handoff_initialize(0x100); // Reinitialization still works when the table is full.
    assert(rsf_handoff_publish(0x100, 1, &publisher, 7, 106));
    assert(rsf_handoff_receive(0x100, 1, &source) && source == 106);
    assert(!locked);
    return 0;
}
