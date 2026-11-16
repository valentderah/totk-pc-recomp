// Vulkan image allocator for the motion-texture snapshot pool.
#include "vk.h"
#include "vk_snapshot_pool.h"
#include <limits.h>

static int payload(const VkSnapshotShape *s, uint64_t *bytes) {
    uint32_t block = 1, bpp;
    switch (s->format) {
    case VK_FORMAT_R8_UNORM: bpp = 1; break;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_SFLOAT: bpp = 2; break;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R32_SFLOAT: bpp = 4; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: bpp = 8; break;
    case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK: block = 4; bpp = 8; break;
    case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK: block = 4; bpp = 16; break;
    default: return 0;
    }
    uint64_t total = 0, w = s->width, h = s->height;
    for (uint32_t i = 0; i < s->levels; i++) {
        uint64_t x = (w + block - 1) / block, y = (h + block - 1) / block;
        if (x > UINT64_MAX / y / bpp || total > UINT64_MAX - x * y * bpp) return 0;
        total += x * y * bpp; w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
    }
    *bytes = total; return total != 0;
}
static VkImageCreateInfo image_info(const VkSnapshotShape *s) {
    return (VkImageCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = s->format, .extent = {s->width, s->height, 1}, .mipLevels = s->levels, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
}
static int measure(void *user, const VkSnapshotShape *s, uint64_t *allocation, uint64_t *copy) {
    VkCtx *c = user; VkImageCreateInfo ci = image_info(s);
    if (!payload(s, copy)) return 0;
    VkImageFormatProperties fp;
    if (vkGetPhysicalDeviceImageFormatProperties(c->phys, s->format, ci.imageType, ci.tiling, ci.usage, 0, &fp) ||
        s->width > fp.maxExtent.width || s->height > fp.maxExtent.height || s->levels > fp.maxMipLevels ||
        !(fp.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) return 0;
    VkFormatProperties features;
    vkGetPhysicalDeviceFormatProperties(c->phys, s->format, &features);
    if (!(features.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
        !(features.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) return 0;
    VkDeviceImageMemoryRequirements q = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, .pCreateInfo = &ci};
    VkMemoryRequirements2 req = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetDeviceImageMemoryRequirements(c->device, &q, &req);
    *allocation = req.memoryRequirements.size;
    return *allocation != 0;
}
static void destroy(void *user, VkSnapshotBacking *b) {
    VkCtx *c = user;
    if (b->view) vkDestroyImageView(c->device, b->view, NULL);
    if (b->image) vmaDestroyImage(c->vma, b->image, (VmaAllocation)b->allocation);
    *b = (VkSnapshotBacking){0};
}
static int create(void *user, const VkSnapshotShape *s, VkSnapshotBacking *b) {
    VkCtx *c = user; VkImageCreateInfo ci = image_info(s);
    // Dedicated memory keeps the pool's strict requirement-byte budget independent of VMA block slack.
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                 .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT};
    VmaAllocation allocation; VmaAllocationInfo info;
    if (vmaCreateImage(c->vma, &ci, &ai, &b->image, &allocation, &info)) return 0;
    b->allocation = allocation; b->allocation_bytes = info.size;
    if (!payload(s, &b->copy_bytes)) { destroy(user, b); return 0; }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = b->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = s->format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, s->levels, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &b->view)) { destroy(user, b); return 0; }
    return 1;
}
VkSnapshotPool *vk_snapshot_pool_start_vulkan(const VkSnapshotLimits *limits) {
    VkCtx *c = vk_ctx();
    if (!c || !c->device || !c->vma) return NULL;
    VkSnapshotBackend backend = {measure, create, destroy, c};
    return vk_snapshot_pool_start(limits, &backend);
}
