// Provider-owned WSI lease. Never pass this owner's chain to native Vulkan WSI.
#pragma once
#include <rescaleframe/native_fg_vulkan.h>

typedef struct RsfPresent RsfPresent;
// Failure may retain *out after partial SDK creation. Keep callbacks/user data
// alive and close that owner before attempting another presentation provider.
int32_t rsf_present_open(const rsf_fgvk_setup *, const rsf_fgvk_host_options *,
    uint32_t backend, uint32_t generated_count, RsfPresent **out);
uint64_t rsf_present_chain(const RsfPresent *);
uint32_t rsf_present_layout(const RsfPresent *);
int32_t rsf_present_enable(RsfPresent *, uint32_t enabled, uint32_t generated_count);
int32_t rsf_present_images(RsfPresent *, uint32_t *count, uint64_t *images);
int32_t rsf_present_acquire(RsfPresent *, uint64_t timeout, uint64_t semaphore, uint64_t fence, uint32_t *index);
int32_t rsf_present_flip(RsfPresent *, const void *present_info, const rsf_frame_record *source);
int32_t rsf_present_prepare(RsfPresent *, void *commands, const rsf_fgvk_frame *);
int32_t rsf_present_hudless(RsfPresent *, void *commands, const rsf_fgvk_frame *);
void rsf_present_submitted(RsfPresent *, void *const *commands, uint32_t count);
// Quiesce producers first. Drain SDK presentation before releasing engine views;
// checked close preserves ownership and callbacks on either drain/destroy failure.
int32_t rsf_present_drain(RsfPresent *);
int32_t rsf_present_close(RsfPresent **);
