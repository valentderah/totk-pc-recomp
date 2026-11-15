#pragma once
#include "vk.h"
#include "rsf_fg_map.h"
#include <rescaleframe/native_fg_vulkan.h>
typedef struct RsfFgLease RsfFgLease;
RsfFgLease *rsf_fg_gpu_prepare(VkCommandBuffer cb,uint64_t fence,const RsfFgMap *map,const RsfFgMap *previous,const rsf_fgvk_frame *input,rsf_fgvk_frame *output);
int rsf_fg_gpu_hudless(VkCommandBuffer cb,RsfFgLease *lease,const rsf_fgvk_frame *input,rsf_fgvk_frame *output);
RsfFgLease *rsf_fg_gpu_find(uint64_t source);
const RsfFgMap *rsf_fg_gpu_map(const RsfFgLease *lease);
// Caller must drain provider and wait GPU idle before clearing/releasing leases.
void rsf_fg_gpu_clear(void);
void rsf_fg_gpu_recycle(void);
void rsf_fg_gpu_release(void);

int rsf_fg_gpu_pending(void);
void rsf_fg_gpu_submitted(VkCommandBuffer cb);
