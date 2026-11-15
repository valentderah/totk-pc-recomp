#include <stdlib.h>
#include "host.h"
#include "rsf_runtime.h"
#include "rsf_sr.h"
#include "platform/platform.h"

struct RsfSrSession {
    struct RsfSrSession *next;
    rsf_srvk *provider;
    rsf_fsr4vk *split_provider;
    void *producer, *continuation;
    __typeof__(&rsf_srvk_fsr4_create) split_create;
    __typeof__(&rsf_srvk_fsr4_plan) split_plan;
    __typeof__(&rsf_srvk_fsr4_evaluate) split_evaluate;
    __typeof__(&rsf_srvk_fsr4_submitted) split_submitted;
    __typeof__(&rsf_srvk_fsr4_retire_commands) split_retire;
    __typeof__(&rsf_srvk_fsr4_destroy) split_destroy;
    int32_t last_result;
    __typeof__(&rsf_srvk_create) create;
    __typeof__(&rsf_srvk_plan) plan;
    __typeof__(&rsf_srvk_evaluate) evaluate;
    __typeof__(&rsf_srvk_retire_commands) retire;
    __typeof__(&rsf_srvk_destroy) destroy;
};

static PlatMutex sessions_lock = PLAT_MUTEX_INIT;
static RsfSrSession *sessions;

int rsf_sr_close(RsfSrSession **owner) {
    if (!owner || !*owner) return 1;
    RsfSrSession *s = *owner;
    plat_mutex_lock(&sessions_lock);
    if (s->provider || s->split_provider) {
        int32_t r = s->split_provider ? s->split_destroy(s->split_provider) : s->destroy(s->provider);
        if (r != RSF_BACKEND_OK) {
            rt_log("rsf: SR destruction refused (%d); retaining provider ownership", r);
            plat_mutex_unlock(&sessions_lock);
            return 0;
        }
    }
    RsfSrSession **link = &sessions;
    while (*link && *link != s) link = &(*link)->next;
    if (*link) *link = s->next;
    plat_mutex_unlock(&sessions_lock);
    free(s);
    *owner = NULL;
    return 1;
}

int rsf_sr_open(const rsf_srvk_setup *setup, RsfSrSession **out, uint32_t *width, uint32_t *height) {
    if (!setup || !out || *out || !width || !height || !rsf_runtime_load()) return 0;
    RsfSrSession *s = calloc(1, sizeof *s);
    if (!s) return 0;
    s->create = rsf_runtime_symbol("rsf_srvk_create");
    s->plan = rsf_runtime_symbol("rsf_srvk_plan");
    s->evaluate = rsf_runtime_symbol("rsf_srvk_evaluate");
    s->retire = rsf_runtime_symbol("rsf_srvk_retire_commands");
    s->destroy = rsf_runtime_symbol("rsf_srvk_destroy");
    if (!s->create || !s->plan || !s->evaluate || !s->retire || !s->destroy) {
        free(s);
        return 0;
    }
    int32_t r = s->create(setup, &s->provider);
    if (r != RSF_BACKEND_OK || !s->provider) {
        rt_log("rsf: SR creation refused (%d), backend %u quality %u", r, setup->backend, setup->quality);
        // Retain any partially created provider if cleanup itself fails.
        if (!rsf_sr_close(&s)) *out = s;
        return 0;
    }
    uint32_t w = 0, h = 0;
    r = s->plan(s->provider, &w, &h);
    if (r != RSF_BACKEND_OK || !w || !h) {
        rt_log("rsf: SR render-size plan refused (%d)", r);
        if (!rsf_sr_close(&s)) *out = s;
        return 0;
    }
    *width = w;
    *height = h;
    plat_mutex_lock(&sessions_lock);
    s->next = sessions;
    sessions = s;
    plat_mutex_unlock(&sessions_lock);
    *out = s;
    return 1;
}

int rsf_sr_record(RsfSrSession *s, void *commands, const rsf_srvk_frame *frame) {
    if (!s || !s->provider || !commands || !frame) return 0;
    plat_mutex_lock(&sessions_lock);
    int32_t r = s->evaluate(s->provider, commands, frame);
    if (r != RSF_BACKEND_OK && r != s->last_result) rt_log("rsf: SR evaluation refused (%d)", r);
    s->last_result = r;
    plat_mutex_unlock(&sessions_lock);
    return r == RSF_BACKEND_OK ? RSF_SR_RECORDED : r == RSF_BACKEND_ERROR_NOT_READY ? RSF_SR_PENDING : RSF_SR_FAILED;
}

void rsf_sr_retire(RsfSrSession *s, void *commands) {
    plat_mutex_lock(&sessions_lock);
    if (s && commands) {
        if (s->split_provider) s->split_retire(s->split_provider, commands);
        else if (s->provider) s->retire(s->provider, commands);
    }
    plat_mutex_unlock(&sessions_lock);
}

void rsf_sr_commands_reset(void *commands) {
    if (!commands) return;
    plat_mutex_lock(&sessions_lock);
    for (RsfSrSession *s = sessions; s; s = s->next) {
        if (s->split_provider) s->split_retire(s->split_provider, commands);
        else s->retire(s->provider, commands);
    }
    plat_mutex_unlock(&sessions_lock);
}

static void split_info_log(const rsf_fsr4vk *owner) {
    if (!(rt_trace & RT_TRACE_GFX)) return;
#if defined(RSF_SRVK_FSR4_INFO_ABI_VERSION)
    __typeof__(&rsf_srvk_fsr4_info) query = rsf_runtime_symbol("rsf_srvk_fsr4_info");
    rsf_srvk_fsr4_provider_info info = {sizeof info, RSF_SRVK_FSR4_INFO_ABI_VERSION};
    int32_t result = query ? query(owner, &info) : RSF_BACKEND_ERROR_MISSING_ENTRY_POINT;
    if (!result) {
        rt_log("rsf: FSR4 effective provider 0x%016llx adapter LUID 0x%016llx vendor 0x%04x INT8 compatibility %u",
            (unsigned long long)info.provider_version, (unsigned long long)info.adapter_luid,
            info.vendor_id, info.int8_compatibility);
    } else rt_log("rsf: FSR4 effective provider unknown (info query %d)", result);
#else
    (void)owner;
    rt_log("rsf: FSR4 effective provider unknown (info query absent from SDK headers)");
#endif
}

int rsf_sr_open_split(const rsf_srvk_setup *setup, const rsf_fsr4vk_handoff *handoff,
                      RsfSrSession **out, uint32_t *width, uint32_t *height) {
    if (!setup || setup->backend != RSF_SR_FSR4 || !handoff ||
        handoff->abi_version != RSF_SRVK_SPLIT_TRACKED_ABI_VERSION ||
        !out || *out || !width || !height || !rsf_runtime_load()) return 0;
    RsfSrSession *s = calloc(1, sizeof *s);
    if (!s) return 0;
#define LOAD(member, name) s->member = rsf_runtime_symbol(name)
    LOAD(split_create, "rsf_srvk_fsr4_create"); LOAD(split_plan, "rsf_srvk_fsr4_plan");
    LOAD(split_evaluate, "rsf_srvk_fsr4_evaluate"); LOAD(split_submitted, "rsf_srvk_fsr4_submitted");
    LOAD(split_retire, "rsf_srvk_fsr4_retire_commands"); LOAD(split_destroy, "rsf_srvk_fsr4_destroy");
#undef LOAD
    if (!s->split_create || !s->split_plan || !s->split_evaluate || !s->split_submitted ||
        !s->split_retire || !s->split_destroy) { free(s); return 0; }
    int32_t r = s->split_create(setup, handoff, &s->split_provider);
    uint32_t w = 0, h = 0;
    if (!r && s->split_provider) r = s->split_plan(s->split_provider, &w, &h);
    if (r || !s->split_provider || !w || !h) {
        rt_log("rsf: FSR4 split creation/plan refused (%d)", r);
        if (!rsf_sr_close(&s)) *out = s;
        return 0;
    }
    split_info_log(s->split_provider);
    plat_mutex_lock(&sessions_lock);
    s->next = sessions; sessions = s;
    plat_mutex_unlock(&sessions_lock);
    *out = s; *width = w; *height = h;
    return 1;
}

int rsf_sr_record_split(RsfSrSession *s, void **commands, const rsf_srvk_frame *frame) {
    if (!s || !commands || !*commands || !frame) return RSF_SR_FAILED;
    if (!s->split_provider) return rsf_sr_record(s, *commands, frame);
    plat_mutex_lock(&sessions_lock);
    void *producer = *commands;
    int32_t r = s->split_evaluate(s->split_provider, producer, frame, commands);
    if (*commands != producer) {
        // Even an SDK refusal leaves queued DX12 work and recorded Vulkan leases.
        s->producer = producer; s->continuation = *commands;
    }
    if (r && r != s->last_result) rt_log("rsf: FSR4 evaluation refused (%d)", r);
    s->last_result = r;
    plat_mutex_unlock(&sessions_lock);
    // Native Vulkan NOT_READY and FidelityFX error values may overlap. Only a
    // refusal before the split is pending; any consumed graph needs fallback.
    return !r ? RSF_SR_RECORDED : *commands == producer && (r == 1 || r == RSF_BACKEND_ERROR_NOT_READY)
        ? RSF_SR_PENDING : RSF_SR_FAILED;
}

int rsf_sr_commands_pending(void *commands) {
    int pending = 0;
    plat_mutex_lock(&sessions_lock);
    for (RsfSrSession *s = sessions; s; s = s->next)
        if (commands && (s->producer == commands || s->continuation == commands)) pending = 1;
    plat_mutex_unlock(&sessions_lock);
    return pending;
}

int rsf_sr_commands_submitted(void *commands) {
    int ok = 1;
    plat_mutex_lock(&sessions_lock);
    for (RsfSrSession *s = sessions; s; s = s->next) {
        if (!commands || (s->producer != commands && s->continuation != commands)) continue;
        int32_t r = s->split_submitted(s->split_provider, commands);
        if (r) { rt_log("rsf: FSR4 successful-submit notification refused (%d)", r); ok = 0; }
        else {
            if (s->producer == commands) s->producer = NULL;
            if (s->continuation == commands) s->continuation = NULL;
        }
    }
    plat_mutex_unlock(&sessions_lock);
    return ok;
}
