#include <stdlib.h>
#include "host.h"
#include "rsf_runtime.h"
#include "rsf_sr.h"

struct RsfSrSession {
    rsf_srvk *provider;
    __typeof__(&rsf_srvk_create) create;
    __typeof__(&rsf_srvk_plan) plan;
    __typeof__(&rsf_srvk_evaluate) evaluate;
    __typeof__(&rsf_srvk_retire_commands) retire;
    __typeof__(&rsf_srvk_destroy) destroy;
};

int rsf_sr_close(RsfSrSession **owner) {
    if (!owner || !*owner) return 1;
    RsfSrSession *s = *owner;
    if (s->provider) {
        int32_t r = s->destroy(s->provider);
        if (r != RSF_BACKEND_OK) {
            rt_log("rsf: SR destruction refused (%d); retaining provider ownership", r);
            return 0;
        }
    }
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
    *out = s;
    return 1;
}

int rsf_sr_record(RsfSrSession *s, void *commands, const rsf_srvk_frame *frame) {
    if (!s || !s->provider || !commands || !frame) return 0;
    int32_t r = s->evaluate(s->provider, commands, frame);
    if (r != RSF_BACKEND_OK) rt_log("rsf: SR evaluation refused (%d)", r);
    return r == RSF_BACKEND_OK;
}

void rsf_sr_retire(RsfSrSession *s, void *commands) {
    if (s && s->provider && commands) s->retire(s->provider, commands);
}
