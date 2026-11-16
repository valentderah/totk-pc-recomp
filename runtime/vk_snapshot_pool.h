// Private, opt-in snapshot backing. Start/stop off recording threads; requests never allocate or wait for a slot.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

#define VK_SNAPSHOT_SLOTS 64
#define VK_SNAPSHOT_COMMANDS 8
typedef struct VkSnapshotPool VkSnapshotPool;
typedef struct { VkFormat format; uint32_t width, height, levels; } VkSnapshotShape; // 2D, one layer, one sample
typedef struct { uint32_t slot; uint64_t generation; } VkSnapshotToken;
typedef struct {
    VkImage image;
    VkImageView view;
    void *allocation;
    uint64_t allocation_bytes, copy_bytes;
    int initialized; // a previously submitted capture established GENERAL; otherwise UNDEFINED
} VkSnapshotBacking;
typedef struct { uint64_t memory_bytes, frame_copy_bytes; uint32_t frame_resources; } VkSnapshotLimits;
typedef struct {
    int (*measure)(void *, const VkSnapshotShape *, uint64_t *allocation_bytes, uint64_t *copy_bytes);
    int (*create)(void *, const VkSnapshotShape *, VkSnapshotBacking *);
    void (*destroy)(void *, VkSnapshotBacking *);
    void *user;
} VkSnapshotBackend; // called only by the dedicated allocator worker

VkSnapshotPool *vk_snapshot_pool_start(const VkSnapshotLimits *, const VkSnapshotBackend *);
VkSnapshotPool *vk_snapshot_pool_start_vulkan(const VkSnapshotLimits *); // device must outlive stop
// 1: exclusively reserved backing; 0: unavailable, invalid, busy, expired frame or budget exhausted.
// Unready exact shapes are queued in fixed storage. No resource may be shared between captures.
int vk_snapshot_pool_acquire(VkSnapshotPool *, const VkSnapshotShape *, uint64_t source_frame,
                             VkSnapshotToken *, VkSnapshotBacking *);
// Reserved but never recorded: cancel without consuming history. Recorded commands require actual reset or submit.
int vk_snapshot_pool_cancel(VkSnapshotPool *, VkSnapshotToken);
int vk_snapshot_pool_record(VkSnapshotPool *, VkSnapshotToken, uint64_t command_tag);
int vk_snapshot_pool_publish(VkSnapshotPool *, VkSnapshotToken, uint64_t source_frame);
int vk_snapshot_pool_consume(VkSnapshotPool *, VkSnapshotToken, uint64_t command_tag);
int vk_snapshot_pool_release(VkSnapshotPool *, VkSnapshotToken);
// Owner calls after actual queue acceptance, or actual command-buffer reset.
// A reused command tag requires an ordered reset notification before recording again.
void vk_snapshot_pool_submitted(VkSnapshotPool *, uint64_t command_tag, uint64_t final_fence);
void vk_snapshot_pool_commands_reset(VkSnapshotPool *, uint64_t command_tag);
void vk_snapshot_pool_collect(VkSnapshotPool *, uint64_t completed_final_fence);
// Off hot path. Refuses while references, recorded/unsubmitted commands or unfinished GPU work remain.
int vk_snapshot_pool_stop(VkSnapshotPool *);

static inline int vk_snapshot_shape_equal(const VkSnapshotShape *a, const VkSnapshotShape *b) {
    return a->format == b->format && a->width == b->width && a->height == b->height && a->levels == b->levels;
}
