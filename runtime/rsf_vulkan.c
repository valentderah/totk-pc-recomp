#define RSF_VULKAN_IMPL
#include "rsf_vulkan.h"
#include "rsf_runtime.h"
#include "cfg.h"
#include "host.h"
#include <rescaleframe/native_streamline_vulkan.h>
#include <rescaleframe/native_latency_vulkan.h>
#include <rescaleframe/native_latency_vulkan_requirements.h>
#include <string.h>

static int prepared, active;
static VkInstance primary_instance;
static __typeof__(&rsf_latencyvk_instance_proc) instance_resolver;
static int stopped;

int rsf_vk_shutdown(void) {
    if (stopped || prepared <= 0 || !rsf_runtime_path()) return 1;
    __typeof__(&rsf_latencyvk_shutdown) shutdown = rsf_runtime_symbol("rsf_latencyvk_shutdown");
    int32_t result = shutdown ? shutdown() : RSF_BACKEND_ERROR_MISSING_ENTRY_POINT;
    if (result != RSF_BACKEND_OK) {
        rt_log("rsf: SDK shutdown refused (%d); retain Vulkan owners", result);
        return 0;
    }
    stopped = 1;
    return 1;
}

static rsf_vk_function generic_instance_proc(void *instance, const char *name) {
    return (rsf_vk_function)rsf_vk_instance_proc(instance, name);
}

int rsf_vk_device_requirements(VkPhysicalDevice physical, void *features,
    const char **extensions, uint32_t *count, uint32_t capacity, void **enabled_features) {
    *enabled_features = features;
    if (!rsf_runtime_path()) return 1;
    __typeof__(&rsf_latencyvk_require_device) require = rsf_runtime_symbol("rsf_latencyvk_require_device");
    if (!require) { rt_log("rsf: missing Vulkan device-requirements service"); return 0; }
    rsf_latencyvk_device_query query = {.struct_size = sizeof query, .abi_version = 1,
        .instance = primary_instance, .physical_device = physical,
        .instance_proc = generic_instance_proc, .features = features};
    rsf_latencyvk_device_requirements requirements = {.struct_size = sizeof requirements, .abi_version = 1};
    int32_t result = require(&query, &requirements);
    if (result != RSF_BACKEND_OK) {
        rt_log("rsf: Vulkan device requirements refused (%d)", result);
        return 0;
    }
    for (uint32_t i = 0; i < requirements.extension_count; i++) {
        const char *extension = requirements.extensions[i];
        int found = 0;
        for (uint32_t k = 0; k < *count; k++) found |= !strcmp(extensions[k], extension);
        if (!found) {
            if (*count == capacity) { rt_log("rsf: device extension list is full"); return 0; }
            extensions[(*count)++] = extension;
        }
    }
    *enabled_features = requirements.features;
    return 1;
}

int rsf_vk_prepare(void) {
    if (prepared) return prepared > 0;
    prepared = -1;
    if (!rsf_runtime_path()) { prepared = 1; return 1; }
    if (!rsf_runtime_load()) return 0;
    // Non-DLSS SR does not require Streamline. Latency will share this session
    // when its input/simulation event producer is connected.
    if (cfg_int("TOTK_RSF_BACKEND") != RSF_SR_DLSS && cfg_int("TOTK_RSF_FG") != 1) { prepared = 1; return 1; }
    __typeof__(&rsf_slvk_initialize) initialize = rsf_runtime_symbol("rsf_slvk_initialize");
    instance_resolver = rsf_runtime_symbol("rsf_latencyvk_instance_proc");
    if (!initialize || !instance_resolver) return 0;
    rsf_slvk_setup setup = {.struct_size = sizeof setup, .abi_version = RSF_SLVK_ABI_VERSION,
        .runtime_directory_utf8 = rsf_provider_directory(1),
        .project_id = "totk-pc-recomp",
        .features = (cfg_int("TOTK_RSF_BACKEND") == RSF_SR_DLSS ? RSF_SLVK_DLSS_SR : 0) |
                    (cfg_int("TOTK_RSF_FG") == 1 ? RSF_SLVK_DLSS_FG : 0)};
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
    VkResult result = create ? create(physical, info, allocator, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (result == VK_SUCCESS && rsf_runtime_path()) {
        __typeof__(&rsf_latencyvk_device_created) created = rsf_runtime_symbol("rsf_latencyvk_device_created");
        if (created) {
            rsf_latencyvk_created_device event = {.struct_size = sizeof event, .abi_version = 1,
                .instance = primary_instance, .physical_device = physical, .device = *out,
                .instance_proc = generic_instance_proc, .create_info = info};
            int32_t status = created(&event);
            if (status != RSF_BACKEND_OK) rt_log("rsf: latency device registration refused (%d)", status);
        }
    }
    return result;
}

void rsf_vk_destroy_device(VkDevice device, const VkAllocationCallbacks *allocator) {
    __typeof__(&rsf_latencyvk_device_destroyed) destroyed = rsf_runtime_symbol("rsf_latencyvk_device_destroyed");
    if (destroyed) destroyed(device);
    PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)rsf_vk_device_proc(device, "vkDestroyDevice");
    if (destroy) destroy(device, allocator);
}
