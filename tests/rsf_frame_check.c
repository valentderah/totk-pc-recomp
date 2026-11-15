// Framework source identity is opened by calc, never by an input poll or present.
#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include "host.h"
#include "layout.h"
#include "patches.h"
#include "platform/platform.h"
#include "rsf_frame.h"
#include <rescaleframe/native_latency_vulkan.h>

const int rt_game_version = 143;
unsigned rt_trace;
static GuestFn begin_hook, end_hook;
static int enabled, locked, away;
static rsf_game_cpu_event events[32];
static unsigned count;

void rt_register_host(uint64_t address, GuestFn fn) {
    if (address == RT_PATCH_PAGE + 0xf50) begin_hook = fn;
    else if (address == RT_PATCH_PAGE + 0xf40) end_hook = fn;
    else assert(0);
}
void plat_mutex_lock(PlatMutex *mutex) { (void)mutex; assert(!locked++); }
void plat_mutex_unlock(PlatMutex *mutex) { (void)mutex; assert(!--locked); }
int kern_core_away(void) { assert(!away); away = 1; return 1; }
void kern_core_back(int *state) { assert(away && *state); away = *state = 0; }
uint64_t plat_counter(void) { static uint64_t tick; return ++tick; }
uint64_t plat_counter_hz(void) { return 1000000; }
uint64_t plat_now_ns(void) { return 1; }
int cfg_int(const char *name) { return !strcmp(name, "TOTK_RSF_ENABLED") && enabled; }
uint64_t rt_original(Cpu *c) { return c->x[0]; }
int mem_guest_readable(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }
void rt_log(const char *format, ...) { (void)format; }
static void receive(const rsf_game_cpu_event *event) {
    assert(away && !locked && count < 32);
    assert(event->event_version == RSF_GAME_CPU_EVENT_VERSION && event->session_id == 1);
    assert(event->timestamp_qpc && event->qpc_frequency == 1000000);
    events[count++] = *event;
}
void *rsf_runtime_symbol(const char *name) {
    assert(!locked);
    return !strcmp(name, "rsf_latencyvk_cpu") ? receive : NULL;
}

int main(void) {
    assert(begin_hook && end_hook);
    int owner, other;
    Cpu cpu = {0}, expected;
    cpu.thread = &owner;
    cpu.x[8] = 17;
    expected = cpu;
    expected.x[8] = MOD_MAIN_BASE + 0x396f000;
    begin_hook(&cpu);
    assert(!memcmp(&cpu, &expected, sizeof cpu) && !count);
    rsf_frame_input(&cpu);
    assert(!count); // Input outside a framework calculation cannot mint an identity.
    enabled = 1;
    begin_hook(&cpu);
    assert(count == 2 && events[0].stage == RSF_GAME_CPU_FRAME_BEGIN && events[1].stage == RSF_GAME_CPU_SIMULATION_BEGIN);
    rsf_frame_input(&cpu);
    rsf_frame_input(&cpu);
    assert(count == 3 && events[2].stage == RSF_GAME_CPU_INPUT_SAMPLE);
    cpu.thread = &other;
    rsf_frame_input(&cpu);
    assert(count == 3);
    cpu.thread = &owner;
    uint8_t object[0x100] = {0};
    uint64_t original_load = 0x12345678;
    memcpy(object + 0x98, &original_load, sizeof original_load);
    cpu.x[19] = (uintptr_t)object;
    expected = cpu;
    expected.x[0] = original_load;
    end_hook(&cpu);
    assert(!memcmp(&cpu, &expected, sizeof cpu));
    assert(count == 4 && events[3].stage == RSF_GAME_CPU_SIMULATION_END);
    assert(events[0].source_frame_id == events[1].source_frame_id &&
           events[0].source_frame_id == events[2].source_frame_id && events[0].source_frame_id == events[3].source_frame_id);
    end_hook(&cpu);
    assert(count == 4);
    begin_hook(&cpu);
    end_hook(&cpu);
    assert(count == 7 && events[4].source_frame_id > events[0].source_frame_id);
    assert(events[6].stage == RSF_GAME_CPU_SIMULATION_END); // No fabricated input fact.
    begin_hook(&cpu);
    begin_hook(&cpu);
    assert(count == 12 && events[9].stage == RSF_GAME_CPU_FRAME_END &&
           (events[9].flags & RSF_GAME_CPU_RENDER_SKIPPED));
    assert(!locked && !away);
    return 0;
}
