#define RSF_VULKAN_IMPL
#include "rsf_vulkan.h"
#include "rsf_runtime.h"
#include "cfg.h"
#include "host.h"
#include <rescaleframe/native_streamline_vulkan.h>
#include <rescaleframe/native_latency_vulkan.h>

static int prepared, active;
static VkInstance primary_instance;
static __typeof__(&rsf_latencyvk_instance_proc) instance_resolver;

int rsf_vk_prepare(void) {
    if (prepared) return prepared > 0;
    prepared = -1;
    if (!cfg_get("TOTK_RSF_RUNTIME")) { prepared = 1; return 1; }
    if (!rsf_runtime_load()) return 0;
    // Non-DLSS SR does not require Streamline. Latency will share this session
    // when its input/simulation event producer is connected.
    if (cfg_int("TOTK_RSF_BACKEND") != RSF_SR_DLSS) { prepared = 1; return 1; }
    __typeof__(&rsf_slvk_initialize) initialize = rsf_runtime_symbol("rsf_slvk_initialize");
    instance_resolver = rsf_runtime_symbol("rsf_latencyvk_instance_proc");
    if (!initialize || !instance_resolver) return 0;
    rsf_slvk_setup setup = {.struct_size = sizeof setup, .abi_version = RSF_SLVK_ABI_VERSION,
        .runtime_directory_utf8 = cfg_get("TOTK_RSF_PROVIDER_DIR"),
        .project_id = "totk-pc-recomp", .features = RSF_SLVK_DLSS_SR};
    int32_t result = initialize(&setup);
    if (result != RSF_BACKEND_OK) {
        rt_log("rsf: early Vulkan SDK initialization refused (%d)", result);
        return 0;
    }
    active = 1;
    prepared = 1;
    return 1;
}

PFN_vkVoidFunction rsf_vk_instance_proc(VkInstance instance, const char *name) {
    if (active) return (PFN_vkVoidFunction)instance_resolver(instance, name);
    return vkGetInstanceProcAddr(instance, name);
}

PFN_vkVoidFunction rsf_vk_device_proc(VkDevice device, const char *name) {
    if (!active) return vkGetDeviceProcAddr(device, name);
    PFN_vkGetDeviceProcAddr resolver = (PFN_vkGetDeviceProcAddr)
        rsf_vk_instance_proc(primary_instance, "vkGetDeviceProcAddr");
    return resolver ? resolver(device, name) : NULL;
}

VkResult rsf_vk_create_instance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator,
                               VkInstance *out) {
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)rsf_vk_instance_proc(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = create(info, allocator, out);
    if (result == VK_SUCCESS) primary_instance = *out;
    return result;
}

VkResult rsf_vk_create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                             const VkAllocationCallbacks *allocator, VkDevice *out) {
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)rsf_vk_instance_proc(primary_instance, "vkCreateDevice");
    return create ? create(physical, info, allocator, out) : VK_ERROR_INITIALIZATION_FAILED;
}
