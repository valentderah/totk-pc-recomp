// Fault injection for exclusive WSI ownership, failed recording and cleanup retry.
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "rsf_present.h"

struct rsf_fgvk { int unused; };
static int creation_error, drain_error, destroy_error, destroy_calls, submitted_calls;
static int acquire_calls, present_calls;
int rsf_runtime_load(void) { return 1; }
static int32_t create(const rsf_fgvk_setup *setup, uint32_t backend, uint32_t count,
    const rsf_fgvk_host_options *host, rsf_fgvk **out, uint64_t *chain) {
    (void)setup; (void)host;
    assert(backend == 3 && count == 1);
    *out = calloc(1, sizeof **out); assert(*out);
    *chain = 123;
    return creation_error;
}
static int32_t layout(const rsf_fgvk *owner, uint32_t *out) { assert(owner); *out = 5; return 0; }
static int32_t enable(rsf_fgvk *owner, uint32_t on, uint32_t count) {
    assert(owner && on == 1 && count == 1); return 0;
}
static int32_t images(rsf_fgvk *owner, uint32_t *count, uint64_t *out) {
    assert(owner && *count >= 1); *count = 1; *out = 456; return 0;
}
static int32_t acquire(rsf_fgvk *owner, uint64_t timeout, uint64_t semaphore, uint64_t fence, uint32_t *index) {
    assert(owner && timeout == 100 && semaphore == 7 && !fence);
    ++acquire_calls; *index = 0; return 0;
}
static int32_t present(rsf_fgvk *owner, const void *info, const rsf_frame_record *source) {
    assert(owner && info);
    if (source) assert(source->session_id == 7 && source->frame_id == 5 && source->view_id == 3 && source->resource_generation == 2);
    ++present_calls; return 0;
}
static int32_t prepare(rsf_fgvk *owner, void *commands, const rsf_fgvk_frame *frame) {
    assert(owner && commands && frame); return -9;
}
static void submitted(rsf_fgvk *owner, void *const *commands, uint32_t count) {
    assert(owner && count == 1 && commands[0]); ++submitted_calls;
}
static int32_t drain(rsf_fgvk *owner) { assert(owner); return drain_error; }
static int32_t destroy(rsf_fgvk *owner) {
    assert(owner); ++destroy_calls;
    if (destroy_error) return destroy_error;
    free(owner); return 0;
}
void *rsf_runtime_symbol(const char *name) {
    if (!strcmp(name, "rsf_fgvk_create_with_host")) return create;
    if (!strcmp(name, "rsf_fgvk_image_layout")) return layout;
    if (!strcmp(name, "rsf_fgvk_enable_options")) return enable;
    if (!strcmp(name, "rsf_fgvk_images")) return images;
    if (!strcmp(name, "rsf_fgvk_acquire")) return acquire;
    if (!strcmp(name, "rsf_fgvk_present_source")) return present;
    if (!strcmp(name, "rsf_fgvk_prepare") || !strcmp(name, "rsf_fgvk_hudless")) return prepare;
    if (!strcmp(name, "rsf_fgvk_submitted")) return submitted;
    if (!strcmp(name, "rsf_fgvk_drain")) return drain;
    if (!strcmp(name, "rsf_fgvk_destroy")) return destroy;
    return NULL;
}
int main(void) {
    RsfPresent *owner = NULL;
    rsf_fgvk_setup setup = {0};
    rsf_fgvk_frame frame = {0};
    int command;
    void *commands[] = {&command};
    assert(!rsf_present_open(&setup, NULL, 3, 1, &owner));
    assert(rsf_present_chain(owner) == 123 && rsf_present_layout(owner) == 5);
    assert(!rsf_present_enable(owner, 1, 1));
    uint32_t count = 1, index = 9;
    uint64_t image = 0;
    assert(!rsf_present_images(owner, &count, &image) && image == 456);
    assert(!rsf_present_acquire(owner, 100, 7, 0, &index) && !index);
    rsf_frame_record identity = {.struct_size = sizeof identity, .abi_version = RSF_GAME_FRAME_ABI_VERSION,
        .session_id = 7, .frame_id = 5, .view_id = 3, .resource_generation = 2};
    assert(!rsf_present_flip(owner, &index, &identity) && acquire_calls == 1 && present_calls == 1);
    assert(rsf_present_prepare(owner, &command, &frame) == -9);
    rsf_present_submitted(owner, commands, 1);
    assert(submitted_calls == 1); // Failed recording still receives submission retirement facts.
    drain_error = -9;
    assert(rsf_present_close(&owner) == -9 && owner && !destroy_calls);
    assert(!rsf_present_chain(owner));
    assert(rsf_present_flip(owner, &index, &identity) == RSF_BACKEND_ERROR_NOT_READY && present_calls == 1);
    drain_error = 0; destroy_error = -9;
    assert(rsf_present_close(&owner) == -9 && owner && destroy_calls == 1);
    destroy_error = 0;
    assert(!rsf_present_close(&owner) && !owner && destroy_calls == 2);
    creation_error = -9;
    assert(rsf_present_open(&setup, NULL, 3, 1, &owner) == -9 && owner);
    assert(!rsf_present_chain(owner)); // Partial construction cannot expose a usable native chain.
    assert(!rsf_present_close(&owner) && !owner && destroy_calls == 3);
    return 0;
}
