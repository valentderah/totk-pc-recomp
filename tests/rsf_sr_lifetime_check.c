// Fault injection for provider ownership and command retirement, without a GPU.
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "rsf_sr.h"
#include "platform/platform.h"

static int retire_calls, destroy_calls, refuse_destroy, refuse_plan, locks;
static int32_t evaluation_result = -1;
unsigned rt_trace; // Optional provider-info logging is off in this lifetime fixture.
struct rsf_srvk { int unused; };
struct rsf_fsr4vk { int unused; };
static struct rsf_fsr4vk split_owner;
static rsf_fsr4vk_handoff split_handoff;
static void *split_producer, *split_continuation;
static int split_destroy_refused, split_create_refused, split_submit_refused, split_calls, refuse_before_split;
static int32_t split_create(const rsf_srvk_setup *setup, const rsf_fsr4vk_handoff *handoff, rsf_fsr4vk **out) {
    assert(setup->backend == RSF_SR_FSR4 && handoff->abi_version == RSF_SRVK_SPLIT_TRACKED_ABI_VERSION);
    split_handoff = *handoff; *out = &split_owner;
    return split_create_refused ? -88 : 0;
}
static int32_t split_plan(const rsf_fsr4vk *owner, uint32_t *w, uint32_t *h) {
    assert(owner == &split_owner); *w = 1280; *h = 720; return 0;
}
static int32_t split_evaluate(rsf_fsr4vk *owner, void *commands, const rsf_srvk_frame *frame, void **out) {
    assert(owner == &split_owner && frame && locks == 1);
    assert(!split_producer && !split_continuation);
    if (refuse_before_split) { *out = commands; return 1; }
    split_producer = commands;
    assert(!split_handoff.split_record(split_handoff.user, commands, 7, 0, 1, 2, out));
    split_continuation = *out; split_calls++;
    return evaluation_result;
}
static int32_t split_submit(rsf_fsr4vk *owner, void *commands) {
    assert(owner == &split_owner && locks == 1);
    if (split_submit_refused) return -89;
    if (commands == split_producer) split_producer = NULL;
    else { assert(!split_producer && commands == split_continuation); split_continuation = NULL; }
    return 0;
}
static void split_retire(rsf_fsr4vk *owner, void *commands) { assert(owner == &split_owner && commands && locks == 1); retire_calls++; }
static int32_t split_destroy(rsf_fsr4vk *owner) {
    assert(owner == &split_owner && locks == 1);
    return split_producer || split_continuation || split_destroy_refused ? 1 : 0;
}
static int32_t handoff(void *user, void *commands, uint64_t timeline, uint64_t previous,
                       uint64_t ready, uint64_t done, void **out) {
    assert(user == (void *)30 && commands && timeline == 7 && !previous && ready == 1 && done == 2);
    *out = (void *)31; return 0;
}

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
    return evaluation_result; // Failed evaluation may still have recorded GPU work.
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
    if (!strcmp(name, "rsf_srvk_fsr4_create")) return split_create;
    if (!strcmp(name, "rsf_srvk_fsr4_plan")) return split_plan;
    if (!strcmp(name, "rsf_srvk_fsr4_evaluate")) return split_evaluate;
    if (!strcmp(name, "rsf_srvk_fsr4_submitted")) return split_submit;
    if (!strcmp(name, "rsf_srvk_fsr4_retire_commands")) return split_retire;
    if (!strcmp(name, "rsf_srvk_fsr4_destroy")) return split_destroy;
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
    evaluation_result = RSF_BACKEND_ERROR_NOT_READY;
    assert(rsf_sr_record(first, &commands, &frame) == RSF_SR_PENDING);
    evaluation_result = RSF_BACKEND_OK;
    assert(rsf_sr_record(first, &commands, &frame) == RSF_SR_RECORDED);
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
    setup.backend = RSF_SR_FSR4;
    rsf_fsr4vk_handoff graph = {sizeof graph, RSF_SRVK_SPLIT_TRACKED_ABI_VERSION, 1, 0, (void *)30, handoff};
    assert(rsf_sr_open_split(&setup, &graph, &first, &w, &h));
    refuse_before_split = 1;
    void *continuation = &commands;
    assert(rsf_sr_record_split(first, &continuation, &frame) == RSF_SR_PENDING && continuation == &commands);
    assert(!rsf_sr_commands_pending(&commands));
    refuse_before_split = 0;
    evaluation_result = -71;
    assert(rsf_sr_record_split(first, &continuation, &frame) == RSF_SR_FAILED && continuation == (void *)31);
    assert(split_calls == 1 && rsf_sr_commands_pending(&commands) && rsf_sr_commands_pending(continuation));
    assert(!rsf_sr_close(&first) && first); // recorded sections cannot be abandoned
    split_submit_refused = 1;
    assert(!rsf_sr_commands_submitted(&commands) && rsf_sr_commands_pending(&commands));
    split_submit_refused = 0;
    assert(rsf_sr_commands_submitted(&commands) && !rsf_sr_commands_pending(&commands));
    assert(!rsf_sr_close(&first) && first); // continuation lease is independent
    split_submit_refused = 1;
    assert(!rsf_sr_commands_submitted(continuation) && rsf_sr_commands_pending(continuation));
    split_submit_refused = 0;
    assert(rsf_sr_commands_submitted(continuation) && !rsf_sr_commands_pending(continuation));
    evaluation_result = 1; continuation = &commands;
    assert(rsf_sr_record_split(first, &continuation, &frame) == RSF_SR_FAILED);
    assert(continuation == (void *)31); // positive SDK failure after split is not pending
    assert(rsf_sr_commands_submitted(&commands) && rsf_sr_commands_submitted(continuation));
    rsf_sr_commands_reset(continuation);
    assert(rsf_sr_close(&first) && !first);
    split_create_refused = split_destroy_refused = 1;
    assert(!rsf_sr_open_split(&setup, &graph, &first, &w, &h) && first);
    split_destroy_refused = 0;
    assert(rsf_sr_close(&first) && !first);
    assert(!locks);
    return 0;
}
