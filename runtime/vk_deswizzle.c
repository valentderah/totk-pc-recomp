// Block-linear textures deswizzled by the GPU (runtime/shaders/bl_deswizzle.comp): vk_texture.c upload binds the
// texture's storage in its pool's imported buffer and lets a compute pass write the linear copy that a buffer-to-image
// copy (or the ASTC decoder) then takes, instead of tex_deswizzle.c walking every 16 bytes on the replay thread
// (50-236 ms frames on entering new areas, 2026-11-07 play sessions). The same design as MacNativeGames' TotK port
// (d2r_gfx nvn_mtl.m: deswizzle from the pool at GPU execution time), written for Vulkan from our CPU layout code.
//
// TOTK_DESWIZZLE = auto (default) / gpu: the compute pass where the storage is an imported pool (vk_mem_span_live),
// the CPU otherwise; cpu: always the CPU; check: gpu, and every upload is also deswizzled on the CPU and compared
// (waits for the GPU; for testing only).
#include <stdlib.h>
#include <string.h>

#include "bl_deswizzle_spv.h"
#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_rec.h"
#include "platform/platform.h"

typedef struct {
    uint32_t src_base, dst_base, segs, pitch16, rows, slices, th, td, gobs_x, blocks_y, nx;
} Push;

static PlatMutex g_lock = PLAT_MUTEX_INIT;
static struct {
    VkDevice dev;
    int mode;   // VK_DESWZ_*; -1 before the first vk_deswz_mode with a device
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    PFN_vkCmdPushDescriptorSetKHR push;
} G = {.mode = -1};
static volatile int64_t n_regions, n_bytes;

static int create_pipeline(VkCtx *c) {
    G.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    if (!G.push) return 0;
    VkDescriptorSetLayoutBinding b[2];
    for (int i = 0; i < 2; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 2, .pBindings = b};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &G.dsl) != VK_SUCCESS) return 0;
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &G.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &G.layout) != VK_SUCCESS) return 0;
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof bl_deswizzle_spv,
                                   .pCode = bl_deswizzle_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = G.layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &G.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) G.pipe = VK_NULL_HANDLE;
    return r == VK_SUCCESS;
}

int vk_deswz_mode(void) {
    VkCtx *c = vk_ctx();
    if (!c) return VK_DESWZ_CPU;
    plat_mutex_lock_shared(&g_lock);
    int m = G.dev == c->device ? G.mode : -1;
    plat_mutex_unlock_shared(&g_lock);
    if (m >= 0) return m;
    plat_mutex_lock(&g_lock);
    if (G.dev != c->device) {   // the old device's objects went with it (probes recreate the device)
        memset(&G, 0, sizeof G);
        G.dev = c->device;
        G.mode = -1;
    }
    if (G.mode < 0) {
        const char *e = cfg_get("TOTK_DESWIZZLE");
        if (!e || !*e) e = "auto";
        int want = !strcmp(e, "cpu") || !strcmp(e, "0") ? VK_DESWZ_CPU : !strcmp(e, "check") ? VK_DESWZ_CHECK : VK_DESWZ_GPU;
        if (strcmp(e, "auto") && strcmp(e, "gpu") && strcmp(e, "1") && strcmp(e, "cpu") && strcmp(e, "0") &&
            strcmp(e, "check"))
            rt_log("vk: TOTK_DESWIZZLE=%s is not auto, gpu, cpu or check: auto", e);
        if (want != VK_DESWZ_CPU && !create_pipeline(c)) {
            rt_log("vk: the deswizzle compute pass is unavailable on %s: textures deswizzle on the CPU", c->props.deviceName);
            want = VK_DESWZ_CPU;
        }
        G.mode = want;
        rt_log("vk: block-linear textures deswizzle on the %s%s", want == VK_DESWZ_CPU ? "CPU" : "GPU where the storage is "
               "an imported pool", want == VK_DESWZ_CHECK ? ", checked against the CPU (TOTK_DESWIZZLE=check)" : "");
    }
    m = G.mode;
    plat_mutex_unlock(&g_lock);
    return m;
}

static uint32_t max_groups_x(const VkCtx *c) {
    uint32_t m = c->props.limits.maxComputeWorkGroupCount[0];
    return m > 65535 ? 65535 : m;
}

// The bound source range starts at the alignment below src_off.
static VkDeviceSize src_lo(const VkCtx *c, VkDeviceSize src_off) {
    VkDeviceSize al = c->props.limits.minStorageBufferOffsetAlignment;
    if (al < 16) al = 16;
    return src_off / al * al;
}

int vk_deswz_fits(VkDeviceSize src_off, VkDeviceSize src_size, const VkDeswzRegion *rg, int n, VkDeviceSize max_dst) {
    VkCtx *c = vk_ctx();
    if (!c || !n || (src_off & 15) || vk_deswz_mode() == VK_DESWZ_CPU) return 0;
    VkDeviceSize range = src_off + src_size - src_lo(c, src_off), lim = c->props.limits.maxStorageBufferRange;
    if (range > lim || max_dst > lim) return 0;
    uint32_t maxg = max_groups_x(c);
    for (int i = 0; i < n; i++) {   // the 32-bit segment indices of the shader stay inside the bindings
        uint64_t groups = ((uint64_t)rg[i].segs + 63) / 64, out = (uint64_t)rg[i].pitch16 * rg[i].rows * rg[i].slices;
        if ((rg[i].src & 15) || rg[i].src + (uint64_t)rg[i].segs * 16 > src_size || !rg[i].segs ||
            (src_off - src_lo(c, src_off) + rg[i].src) / 16 + rg[i].segs > UINT32_MAX || out > UINT32_MAX ||
            out * 16 > max_dst || (groups + maxg - 1) / maxg > c->props.limits.maxComputeWorkGroupCount[1])
            return 0;
    }
    return 1;
}

int vk_deswz_record(VkCommandBuffer cb, VkBuffer src, VkDeviceSize src_off, VkDeviceSize src_size, VkBuffer dst,
                    VkDeviceSize dst_size, const VkDeswzRegion *rg, int n) {
    VkCtx *c = vk_ctx();
    if (!vk_deswz_fits(src_off, src_size, rg, n, dst_size)) return 0;
    for (int i = 0; i < n; i++)
        if ((rg[i].dst & 15) || rg[i].dst + (uint64_t)rg[i].pitch16 * 16 * rg[i].rows * rg[i].slices > dst_size) return 0;
    plat_mutex_lock_shared(&g_lock);
    VkPipeline pipe = G.dev == c->device ? G.pipe : VK_NULL_HANDLE;
    plat_mutex_unlock_shared(&g_lock);
    if (!pipe) return 0;
    VkDeviceSize lo = src_lo(c, src_off), range = src_off + src_size - lo;
    uint32_t maxg = max_groups_x(c);
    VkDescriptorBufferInfo dbi[2] = {{src, lo, range}, {dst, 0, dst_size}};
    VkWriteDescriptorSet w[2];
    for (int i = 0; i < 2; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &dbi[i]};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    rec_push(G.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, G.layout, 0, 2, w);
    uint64_t bytes = 0;
    for (int i = 0; i < n; i++) {
        const VkDeswzRegion *r = &rg[i];
        uint64_t groups = ((uint64_t)r->segs + 63) / 64;
        uint32_t nx = groups < maxg ? (uint32_t)groups : maxg, ny = (uint32_t)((groups + nx - 1) / nx);
        Push pc = {.src_base = (uint32_t)((src_off - lo + r->src) / 16), .dst_base = (uint32_t)(r->dst / 16),
                   .segs = r->segs, .pitch16 = r->pitch16, .rows = r->rows, .slices = r->slices, .th = r->th, .td = r->td,
                   .gobs_x = r->gobs_x, .blocks_y = r->blocks_y, .nx = nx};
        vkCmdPushConstants(cb, G.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cb, nx, ny, 1);
        bytes += (uint64_t)r->segs * 16;
    }
    int64_t before = plat_add64(&n_regions, n) - n;
    plat_add64(&n_bytes, (int64_t)bytes);
    if (!before || (before + n) / 4096 != before / 4096)
        rt_log("vk: deswizzle: %lld regions, %.1f MiB of block-linear storage deswizzled on the GPU so far",
               (long long)(before + n), (double)n_bytes / 1048576.0);
    return 1;
}

void vk_deswz_release(void) {
    VkCtx *c = vk_ctx();
    plat_mutex_lock(&g_lock);
    if (c && G.dev == c->device) {
        if (G.pipe) vkDestroyPipeline(c->device, G.pipe, NULL);
        if (G.layout) vkDestroyPipelineLayout(c->device, G.layout, NULL);
        if (G.dsl) vkDestroyDescriptorSetLayout(c->device, G.dsl, NULL);
    }
    memset(&G, 0, sizeof G);
    G.mode = -1;
    plat_mutex_unlock(&g_lock);
}
