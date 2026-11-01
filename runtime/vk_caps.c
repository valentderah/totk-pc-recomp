// What the runtime branches on, logged once at device creation, and the fallbacks for GPUs other than the RX 9070.
//
// Fallbacks (each also forced on any GPU by an environment switch, to test it where the GPU does not need it):
//   - no shaderFloat64 (TOTK_VK_NO_FLOAT64=1 clears the feature before vkCreateDevice): the translator lowers FP64 to
//     FP32 ("softfloat64"), so no shader declares Float64.
//   - minStorageBufferOffsetAlignment above 4 (TOTK_VK_SSBO_ALIGN=<power of two> raises it): the translator computes
//     storage offsets from the guest address rounded down to that alignment (Ryujinx's unaligned storage path), and
//     vk_exec_draw.c ssbo_info binds the rounded-down address, so shader writes reach guest memory instead of a copy.
//   - no graphics pipeline library with fast linking (TOTK_GPL=0 forces it; the default on AMD's RDNA 1/2 driver):
//     vk_caps.gpl = 0, the extension stays disabled, and pipelines are built whole.
//   - no 32-lane pin (no subgroupSizeControl, 32 outside min..max, or TOTK_VK_SUBGROUP_PIN=0): stages run at the
//     device's subgroupSize (SPIR-V 1.3 without a required size) and the translator is told that size.
//     TOTK_VK_SUBGROUP=<n> pins n instead of 32 where the driver allows it.
// The defaults (Float64, no storage rounding, 32 everywhere) leave the shader cache keys as they were (gfx_shader.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "vk.h"

VkCaps vk_caps = {.float64 = 1, .sg_size = 32};
static int f64_supported;
static VkBool32 gpl_lib, gpl_fast;   // the driver's graphicsPipelineLibrary (feature), ...FastLinking (property)

static uint32_t env_u32(const char *name, uint32_t def) {
    const char *e = cfg_get(name);
    return e && *e ? (uint32_t)strtoul(e, NULL, 0) : def;
}

void vk_caps_features(VkPhysicalDeviceFeatures *f) {
    f64_supported = f->shaderFloat64;
    if (env_u32("TOTK_VK_NO_FLOAT64", 0)) f->shaderFloat64 = VK_FALSE;
    vk_caps.float64 = f->shaderFloat64;
}

void vk_caps_driver(VkPhysicalDevice p) {
    VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
    vkGetPhysicalDeviceProperties2(p, &p2);
    vk_caps.amd = p2.properties.vendorID == 0x1002;
    // AMD split RDNA 1 and 2 off into a driver branch of their own; its driverInfo reads
    // "26.10.1 (AMD proprietary shader compiler)" where the RDNA 3 and 4 driver names LLPC.
    vk_caps.amd_sc = drv.driverID == VK_DRIVER_ID_AMD_PROPRIETARY && strstr(drv.driverInfo, "shader compiler");
}

// TOTK_GPL=0/1 forces graphics pipeline libraries off or on. Default off on AMD's RDNA 1/2 driver branch: every boot
// that lost the device there had a cold pipeline cache and draws waiting on fast-linked pipelines, and a run with GPL
// off booted (issue #5); Savage Wind Recomp turns GPL off on RDNA 4 under RADV for a GPU hang at the title screen.
int vk_caps_gpl(const VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT *f,
                const VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT *p) {
    gpl_lib = f->graphicsPipelineLibrary;
    gpl_fast = p->graphicsPipelineLibraryFastLinking;
    const char *e = cfg_get("TOTK_GPL");   // no fixed default: it depends on the driver, see above
    vk_caps.gpl = gpl_lib && gpl_fast && (e && *e ? strcmp(e, "0") != 0 : !vk_caps.amd_sc);
    return vk_caps.gpl;
}

static int has_ext(const VkExtensionProperties *ep, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(ep[i].extensionName, name)) return 1;
    return 0;
}

static char depth_ok(VkPhysicalDevice p, VkFormat f) {
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(p, f, &fp);
    return fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ? 'y' : 'n';
}

static void driver_version(char *o, size_t n, uint32_t vendor, uint32_t v) {
    if (vendor == 0x10de) snprintf(o, n, "%u.%u.%u.%u", v >> 22, v >> 14 & 0xff, v >> 6 & 0xff, v & 0x3f);
    else if (vendor == 0x8086) snprintf(o, n, "%u.%u", v >> 14, v & 0x3fff);
    else snprintf(o, n, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

void vk_caps_init(VkCtx *c) {
    VkPhysicalDeviceLimits *l = &c->props.limits;
    VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceSubgroupProperties sg = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, .pNext = &drv};
    VkPhysicalDeviceVulkan13Properties p13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, .pNext = &sg};
    VkPhysicalDevicePushDescriptorPropertiesKHR pd = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR,
                                                      .pNext = &p13};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pd};
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    VkPhysicalDeviceProvokingVertexFeaturesEXT pvf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .pNext = c->has_provoking_vertex ? &pvf : NULL};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(c->phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(c->phys, NULL, &ne, ep);

    uint32_t real_align = (uint32_t)l->minStorageBufferOffsetAlignment, a = env_u32("TOTK_VK_SSBO_ALIGN", 0);
    if (a && !(a & (a - 1)) && a > real_align) l->minStorageBufferOffsetAlignment = a;
    vk_caps.sbuf_align = l->minStorageBufferOffsetAlignment > 4 ? (uint32_t)l->minStorageBufferOffsetAlignment : 0;

    uint32_t want = env_u32("TOTK_VK_SUBGROUP", 32);
    int pin = f13.subgroupSizeControl && cfg_bool("TOTK_VK_SUBGROUP_PIN") && p13.minSubgroupSize <= want &&
              want <= p13.maxSubgroupSize;
    vk_caps.sg_size = want;
    vk_caps.sg_pin_stages = pin ? p13.requiredSubgroupSizeStages : 0;
    static const VkShaderStageFlags stage[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_COMPUTE_BIT};
    uint32_t told[3];
    for (int i = 0; i < 3; i++) told[i] = vk_caps.sg_pin_stages & stage[i] ? want : sg.subgroupSize;
    // Gathers (TLD4) pick their four texels with Maxwell's rounding; AMD and Intel round a coordinate that sits between
    // two texels the other way, which draws dark lines along object edges in shadow-map filtering (docs/maxwell.md 2.10;
    // BotW and TotK on AMD). The translator biases gather coordinates by the host's sub-texel precision; NVIDIA rounds
    // as Maxwell does. TOTK_GATHER_BIAS=<bits> forces it (0: off).
    uint32_t gather = env_u32("TOTK_GATHER_BIAS", c->props.vendorID == 0x10de ? 0 : l->subTexelPrecisionBits);
    gfx_shader_set_host(vk_caps.float64, vk_caps.sbuf_align, told, gather);
    if (gather) rt_log("vk caps: gather coordinates biased for %u sub-texel bits (TOTK_GATHER_BIAS)", gather);

    char dv[32];
    driver_version(dv, sizeof dv, c->props.vendorID, c->props.driverVersion);
    uint32_t v = c->props.apiVersion;
    VkPhysicalDevice p = c->phys;
    rt_log("vk caps: %s, vendor %#06x device %#06x, driver %s \"%s\" (id %d, version %s, raw %#x), api %u.%u.%u",
           c->props.deviceName, c->props.vendorID, c->props.deviceID, drv.driverName, drv.driverInfo, (int)drv.driverID, dv,
           c->props.driverVersion, VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
    rt_log("vk caps: subgroup size %u, stages %#x, ops %#x; size control %d (%u-%u, required stages %#x)", sg.subgroupSize,
           (unsigned)sg.supportedStages, (unsigned)sg.supportedOperations, (int)f13.subgroupSizeControl, p13.minSubgroupSize,
           p13.maxSubgroupSize, (unsigned)p13.requiredSubgroupSizeStages);
    rt_log("vk caps: shaderFloat64 %d (enabled %d), storage buffer offset alignment %u (device %u), uniform %llu, "
           "maxPushDescriptors %u, provoking vertex ext %d last %d",
           f64_supported, vk_caps.float64, (unsigned)l->minStorageBufferOffsetAlignment, real_align,
           (unsigned long long)l->minUniformBufferOffsetAlignment, pd.maxPushDescriptors, c->has_provoking_vertex,
           (int)pvf.provokingVertexLast);
    rt_log("vk caps: ext index_type_uint8 khr %d ext %d, attachment_feedback_loop_layout %d dynamic_state %d, "
           "graphics_pipeline_library %d; depth attachment D16 %c D24S8 %c X8D24 %c D32 %c D32S8 %c",
           has_ext(ep, ne, "VK_KHR_index_type_uint8"), has_ext(ep, ne, "VK_EXT_index_type_uint8"),
           has_ext(ep, ne, "VK_EXT_attachment_feedback_loop_layout"), has_ext(ep, ne, "VK_EXT_attachment_feedback_loop_dynamic_state"),
           has_ext(ep, ne, "VK_EXT_graphics_pipeline_library"), depth_ok(p, VK_FORMAT_D16_UNORM),
           depth_ok(p, VK_FORMAT_D24_UNORM_S8_UINT), depth_ok(p, VK_FORMAT_X8_D24_UNORM_PACK32), depth_ok(p, VK_FORMAT_D32_SFLOAT),
           depth_ok(p, VK_FORMAT_D32_SFLOAT_S8_UINT));
    const char *ge = cfg_get("TOTK_GPL");
    rt_log("vk caps: gpl %s fast-link %s (graphicsPipelineLibrary %u%s)", vk_caps.gpl ? "yes" : "no",
           gpl_fast ? "yes" : "no", (unsigned)gpl_lib,
           !gpl_lib || !gpl_fast || vk_caps.gpl ? "" : ge && *ge ? ", off by TOTK_GPL=0" : ", off on AMD's RDNA 1/2 driver");
    rt_log("vk caps: fallbacks: float64 %s, storage buffers %s %u, subgroup pin %u on stages %#x, translator subgroup vs/fs/cs %u/%u/%u",
           vk_caps.float64 ? "native" : "softfloat64", vk_caps.sbuf_align ? "rounded down to" : "exact, alignment",
           (unsigned)l->minStorageBufferOffsetAlignment, want, (unsigned)vk_caps.sg_pin_stages, told[0], told[1], told[2]);
    free(ep);
}
