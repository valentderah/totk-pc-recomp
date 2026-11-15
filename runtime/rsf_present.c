#include <stdlib.h>
#include "rsf_runtime.h"
#include "rsf_present.h"

// The graphics owner serializes these calls with its queue/WSI lock. SDK callbacks
// may reenter that recursive queue lock; no private adapter lock spans callbacks.
struct RsfPresent {
    rsf_fgvk *provider;
    uint64_t chain;
    uint32_t layout;
    int ready;
    __typeof__(&rsf_fgvk_create_with_host) create;
    __typeof__(&rsf_fgvk_image_layout) image_layout;
    __typeof__(&rsf_fgvk_enable_options) enable;
    __typeof__(&rsf_fgvk_images) images;
    __typeof__(&rsf_fgvk_acquire) acquire;
    __typeof__(&rsf_fgvk_present) present;
    __typeof__(&rsf_fgvk_prepare) prepare;
    __typeof__(&rsf_fgvk_hudless) hudless;
    __typeof__(&rsf_fgvk_submitted) submitted;
    __typeof__(&rsf_fgvk_drain) drain;
    __typeof__(&rsf_fgvk_destroy) destroy;
};

int32_t rsf_present_drain(RsfPresent *s) {
    return s && s->provider ? s->drain(s->provider) : RSF_BACKEND_OK;
}

int32_t rsf_present_close(RsfPresent **owner) {
    if (!owner || !*owner) return RSF_BACKEND_OK;
    RsfPresent *s = *owner;
    s->ready = 0;
    if (s->chain && !s->provider) return RSF_BACKEND_ERROR_FEATURE_FAILED;
    int32_t result = rsf_present_drain(s);
    if (result != RSF_BACKEND_OK) return result;
    if (s->provider && (result = s->destroy(s->provider)) != RSF_BACKEND_OK) return result;
    free(s);
    *owner = NULL;
    return RSF_BACKEND_OK;
}

int32_t rsf_present_open(const rsf_fgvk_setup *setup, const rsf_fgvk_host_options *host,
    uint32_t backend, uint32_t count, RsfPresent **out) {
    if (!setup || !out || *out || !count) return RSF_BACKEND_ERROR_INVALID_ARGUMENT;
    if (!rsf_runtime_load()) return RSF_BACKEND_ERROR_LOAD_FAILED;
    RsfPresent *s = calloc(1, sizeof *s);
    if (!s) return RSF_BACKEND_ERROR_FEATURE_FAILED;
#define BIND(field, symbol) s->field = rsf_runtime_symbol(symbol); if (!s->field) { free(s); return RSF_BACKEND_ERROR_MISSING_ENTRY_POINT; }
    BIND(create, "rsf_fgvk_create_with_host");
    BIND(image_layout, "rsf_fgvk_image_layout");
    BIND(enable, "rsf_fgvk_enable_options");
    BIND(images, "rsf_fgvk_images");
    BIND(acquire, "rsf_fgvk_acquire");
    BIND(present, "rsf_fgvk_present");
    BIND(prepare, "rsf_fgvk_prepare");
    BIND(hudless, "rsf_fgvk_hudless");
    BIND(submitted, "rsf_fgvk_submitted");
    BIND(drain, "rsf_fgvk_drain");
    BIND(destroy, "rsf_fgvk_destroy");
#undef BIND
    int32_t result = s->create(setup, backend, count, host, &s->provider, &s->chain);
    if (result != RSF_BACKEND_OK || !s->provider || !s->chain) {
        if (s->provider || s->chain) *out = s;
        else free(s);
        return result != RSF_BACKEND_OK ? result : RSF_BACKEND_ERROR_FEATURE_FAILED;
    }
    *out = s;
    result = s->image_layout(s->provider, &s->layout);
    if (result == RSF_BACKEND_OK && !s->layout) return RSF_BACKEND_ERROR_FEATURE_FAILED;
    if (result == RSF_BACKEND_OK) s->ready = 1;
    return result;
}

uint64_t rsf_present_chain(const RsfPresent *s) { return s && s->ready ? s->chain : 0; }
uint32_t rsf_present_layout(const RsfPresent *s) { return s && s->ready ? s->layout : 0; }
int32_t rsf_present_enable(RsfPresent *s, uint32_t enabled, uint32_t count) {
    return s && s->ready ? s->enable(s->provider, enabled, count) : RSF_BACKEND_ERROR_NOT_READY;
}
int32_t rsf_present_images(RsfPresent *s, uint32_t *count, uint64_t *images) {
    return s && s->ready ? s->images(s->provider, count, images) : RSF_BACKEND_ERROR_NOT_READY;
}
int32_t rsf_present_acquire(RsfPresent *s, uint64_t timeout, uint64_t semaphore, uint64_t fence, uint32_t *index) {
    return s && s->ready ? s->acquire(s->provider, timeout, semaphore, fence, index) : RSF_BACKEND_ERROR_NOT_READY;
}
int32_t rsf_present_flip(RsfPresent *s, const void *info) {
    return s && s->ready ? s->present(s->provider, info) : RSF_BACKEND_ERROR_NOT_READY;
}
int32_t rsf_present_prepare(RsfPresent *s, void *commands, const rsf_fgvk_frame *frame) {
    return s && s->ready ? s->prepare(s->provider, commands, frame) : RSF_BACKEND_ERROR_NOT_READY;
}
int32_t rsf_present_hudless(RsfPresent *s, void *commands, const rsf_fgvk_frame *frame) {
    return s && s->ready ? s->hudless(s->provider, commands, frame) : RSF_BACKEND_ERROR_NOT_READY;
}
void rsf_present_submitted(RsfPresent *s, void *const *commands, uint32_t count) {
    if (s && s->ready) s->submitted(s->provider, commands, count);
}
