// Fault injection for provider ownership and command retirement, without a GPU.
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "rsf_sr.h"
#include "platform/platform.h"

struct rsf_srvk { int unused; };
static int retire_calls, destroy_calls, refuse_destroy, refuse_plan, locks;
static void *last_commands;
void rt_log(const char *format, ...) { (void)format; }
void plat_mutex_lock(PlatMutex *mutex) { (void)mutex; assert(!locks++); }
void plat_mutex_unlock(PlatMutex *mutex) { (void)mutex; assert(!--locks); }
int rsf_runtime_load(void) { return 1; }

static int32_t create(const rsf_srvk_setup *setup, rsf_srvk **out) {
    (void)setup;
    *out = calloc(1, sizeof **out);
    return *out ? 0 : -1;
}
static int32_t plan(const rsf_srvk *provider, uint32_t *w, uint32_t *h) {
    assert(provider);
    *w = 1280; *h = 720;
    return refuse_plan ? -1 : 0;
}
static int32_t evaluate(rsf_srvk *provider, void *commands, const rsf_srvk_frame *frame) {
    assert(provider && commands && frame && locks == 1);
    return -1; // Failed evaluation may still have recorded GPU work.
}
static void retire(rsf_srvk *provider, void *commands) {
    assert(provider && commands && locks == 1);
    ++retire_calls;
    last_commands = commands;
}
static int32_t destroy(rsf_srvk *provider) {
    assert(provider && locks == 1);
    ++destroy_calls;
    if (refuse_destroy) return -1;
    free(provider);
    return 0;
}
void *rsf_runtime_symbol(const char *name) {
    if (!strcmp(name, "rsf_srvk_create")) return create;
    if (!strcmp(name, "rsf_srvk_plan")) return plan;
    if (!strcmp(name, "rsf_srvk_evaluate")) return evaluate;
    if (!strcmp(name, "rsf_srvk_retire_commands")) return retire;
    if (!strcmp(name, "rsf_srvk_destroy")) return destroy;
    return NULL;
}

int main(void) {
    rsf_srvk_setup setup = {0};
    rsf_srvk_frame frame = {0};
    RsfSrSession *first = NULL, *second = NULL;
    uint32_t w = 0, h = 0;
    int commands;
    assert(rsf_sr_open(&setup, &first, &w, &h) && w == 1280 && h == 720);
    assert(rsf_sr_open(&setup, &second, &w, &h));
    assert(!rsf_sr_record(first, &commands, &frame));
    rsf_sr_commands_reset(&commands);
    assert(retire_calls == 2 && last_commands == &commands);
    refuse_destroy = 1;
    assert(!rsf_sr_close(&first) && first);
    rsf_sr_commands_reset(&commands);
    assert(retire_calls == 4); // Refused destruction retains retirement registration.
    refuse_destroy = 0;
    assert(rsf_sr_close(&first) && !first);
    rsf_sr_commands_reset(&commands);
    assert(retire_calls == 5);
    assert(rsf_sr_close(&second) && !second);
    rsf_sr_commands_reset(&commands);
    assert(retire_calls == 5 && destroy_calls == 3);
    refuse_plan = refuse_destroy = 1;
    assert(!rsf_sr_open(&setup, &first, &w, &h) && first);
    refuse_destroy = 0;
    assert(rsf_sr_close(&first) && !first);
    assert(!locks);
    return 0;
}
