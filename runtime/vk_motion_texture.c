// Narrow one-binding terrain position history. Native copies stay adjacent to their reads in the recorder's part.
#include "vk.h"
#include "host.h"
#include "vk_motion_texture.h"
#include "vk_native_pass.h"
#include <string.h>

#define READS 8
typedef struct {
    uint64_t key, frame, image_generation;
    VkSampler sampler;
    VkSnapshotShape shape;
    VkSnapshotToken token;
    VkSnapshotBacking backing;
    int used, ambiguous;
} Read;
static struct { VkSnapshotPool *pool; uint64_t frame; Read reads[2][READS]; } M;

static void release_rows(Read *rows) {
    for (int i = 0; i < READS; i++) if (rows[i].used) vk_snapshot_pool_release(M.pool, rows[i].token);
    memset(rows, 0, sizeof(Read) * READS);
}
static int frame_begin(uint64_t frame) {
    if (!frame || frame < M.frame) return 0;
    if (frame != M.frame) {
        release_rows(M.reads[1]);
        if (frame == M.frame + 1) memcpy(M.reads[1], M.reads[0], sizeof M.reads[0]);
        else release_rows(M.reads[0]);
        memset(M.reads[0], 0, sizeof M.reads[0]); M.frame = frame;
    }
    vk_snapshot_pool_collect(M.pool, vk_sync_fence_done());
    return 1;
}
int vk_motion_texture_active(void) { return M.pool != NULL; }
int vk_motion_texture_start(int enabled) {
    if (M.pool) return 1;
    if (!enabled) return 1;
    VkSnapshotLimits limits = {64ull << 20, 4ull << 20, READS};
    M.pool = vk_snapshot_pool_start_vulkan(&limits);
    return M.pool != NULL;
}
int vk_motion_texture_stop(void) {
    if (!M.pool) return 1;
    release_rows(M.reads[0]); release_rows(M.reads[1]);
    vk_snapshot_pool_collect(M.pool, vk_sync_fence_done());
    if (!vk_snapshot_pool_stop(M.pool)) return 0;
    memset(&M, 0, sizeof M); return 1;
}
void vk_motion_texture_submitted(VkCommandBuffer commands, uint64_t final_fence) {
    if (M.pool) vk_snapshot_pool_submitted(M.pool, (uint64_t)(uintptr_t)commands, final_fence);
}
void vk_motion_texture_commands_reset(VkCommandBuffer commands) {
    // The owner reports real reset before that command buffer can begin its next generation.
    if (M.pool) vk_snapshot_pool_commands_reset(M.pool, (uint64_t)(uintptr_t)commands);
}
static VkImageMemoryBarrier2 image_barrier(VkImage image, uint32_t levels, VkImageLayout old,
                                         VkPipelineStageFlags2 source, VkAccessFlags2 access,
                                         VkPipelineStageFlags2 dest, VkAccessFlags2 use) {
    return (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = source, .srcAccessMask = access, .dstStageMask = dest, .dstAccessMask = use,
        .oldLayout = old, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1}};
}
int vk_motion_texture_capture(VkExec *ex, uint64_t key, const VkMotionTextureFact *fact, VkSampler sampler) {
    if (!M.pool || !key || !fact || !sampler || !frame_begin(ex->source_frame)) return 0;
    Read *row = NULL;
    for (int i = 0; i < READS; i++) {
        if (M.reads[0][i].used && M.reads[0][i].key == key) { M.reads[0][i].ambiguous = 1; return 0; }
        if (!M.reads[0][i].used && !row) row = &M.reads[0][i];
    }
    if (!row) return 0;
    VkSnapshotToken token; VkSnapshotBacking backing;
    if (!vk_snapshot_pool_acquire(M.pool, &fact->shape, ex->source_frame, &token, &backing)) return 0;
    VkNativePass pass;
    if (!vk_exec_native_suspend(ex, &pass)) { vk_snapshot_pool_cancel(M.pool, token); return 0; }
    // No native state/slot lookup, upload recording, callback or command-buffer split may intervene from here.
    if (!vk_snapshot_pool_record(M.pool, token, (uint64_t)(uintptr_t)ex->cb)) {
        vk_snapshot_pool_cancel(M.pool, token);
        if (!vk_exec_native_resume(ex, &pass)) rt_fatal("snapshot: native pass resume failed before copy");
        return 0;
    }
    VkImageMemoryBarrier2 before[2] = {
        image_barrier(fact->image, fact->shape.levels, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT),
        image_barrier(backing.image, fact->shape.levels, backing.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT)};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = before};
    vkCmdPipelineBarrier2(ex->cb, &dep);
    VkImageCopy regions[32]; uint32_t w = fact->shape.width, h = fact->shape.height;
    for (uint32_t i = 0; i < fact->shape.levels; i++) {
        regions[i] = (VkImageCopy){.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1}, .extent = {w, h, 1}};
        w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
    }
    vkCmdCopyImage(ex->cb, fact->image, VK_IMAGE_LAYOUT_GENERAL, backing.image, VK_IMAGE_LAYOUT_GENERAL, fact->shape.levels, regions);
    VkImageMemoryBarrier2 after[2] = {
        image_barrier(fact->image, fact->shape.levels, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
        image_barrier(backing.image, fact->shape.levels, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT)};
    dep.pImageMemoryBarriers = after; vkCmdPipelineBarrier2(ex->cb, &dep); ex->recorded += 3;
    if (!vk_exec_native_resume(ex, &pass)) rt_fatal("snapshot: native pass identity changed during transfer-only section");
    *row = (Read){.key = key, .frame = ex->source_frame, .image_generation = fact->generation, .sampler = sampler,
        .shape = fact->shape, .token = token, .backing = backing, .used = 1};
    return 1;
}
int vk_motion_texture_previous(VkExec *ex, uint64_t key, const VkMotionTextureFact *fact,
                               VkSampler sampler, VkDescriptorImageInfo *out) {
    if (!M.pool || !fact || !out || !frame_begin(ex->source_frame)) return 0;
    // A duplicate native draw identity in this frame cannot choose a previous occurrence honestly.
    for (int i = 0; i < READS; i++) if (M.reads[0][i].used && M.reads[0][i].key == key && M.reads[0][i].ambiguous) return 0;
    for (int i = 0; i < READS; i++) {
        Read *r = &M.reads[1][i];
        if (!r->used || r->ambiguous || r->key != key || r->frame + 1 != ex->source_frame ||
            r->image_generation != fact->generation || r->sampler != sampler ||
            r->shape.format != fact->shape.format || r->shape.width != fact->shape.width ||
            r->shape.height != fact->shape.height || r->shape.levels != fact->shape.levels) continue;
        if (!vk_snapshot_pool_publish(M.pool, r->token, r->frame) ||
            !vk_snapshot_pool_consume(M.pool, r->token, (uint64_t)(uintptr_t)ex->cb)) return 0;
        *out = (VkDescriptorImageInfo){sampler, r->backing.view, VK_IMAGE_LAYOUT_GENERAL}; return 1;
    }
    return 0;
}
