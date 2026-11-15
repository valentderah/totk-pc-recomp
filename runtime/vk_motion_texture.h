#pragma once
#include "vk_snapshot_pool.h"
#include "vk_exec.h"
typedef struct {
    VkImage image;
    uint64_t generation;
    VkSnapshotShape shape;
} VkMotionTextureFact;
// Off recording threads, device live. Stop drops CPU history, collects actual completed engine fences and may refuse.
int vk_motion_texture_start(int enabled);
int vk_motion_texture_stop(void);
void vk_motion_texture_submitted(VkCommandBuffer, uint64_t final_engine_fence);
void vk_motion_texture_commands_reset(VkCommandBuffer);
int vk_motion_texture_active(void);
// Pure cached lookup under the texture owner's lock, never creates or uploads a native image/view.
int vk_texture_motion_fact(uint64_t handle, VkMotionTextureFact *);
int vk_motion_texture_capture(VkExec *, uint64_t draw_key, const VkMotionTextureFact *, VkSampler);
int vk_motion_texture_previous(VkExec *, uint64_t draw_key, const VkMotionTextureFact *, VkSampler, VkDescriptorImageInfo *);
