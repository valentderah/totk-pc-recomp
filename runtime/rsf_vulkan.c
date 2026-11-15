#define RSF_VULKAN_IMPL
#include "rsf_vulkan.h"
#include "rsf_runtime.h"
#include "cfg.h"
#include "host.h"
#include <rescaleframe/native_streamline_vulkan.h>
#include <rescaleframe/native_latency_vulkan.h>
#include <rescaleframe/native_latency_vulkan_requirements.h>
#include <rescaleframe/native_xess_vulkan_requirements.h>
#include <string.h>

static int prepared, active, runtime_loaded;
static VkInstance primary_instance;
static __typeof__(&rsf_latencyvk_instance_proc) instance_resolver;
static int stopped;
static int xess_active;

static void execution_event(uint64_t source, uint32_t role, uint32_t begin) {
    if (!source) return;
    __typeof__(&rsf_latencyvk_execution) report = rsf_runtime_symbol("rsf_latencyvk_execution");
    if (!report) return;
    rsf_game_render_pass pass = {.struct_size = sizeof pass, .role = role,
        .session_id = 1, .source_frame_id = source};
    int32_t result = report(&pass, begin);
    if ((rt_trace & RT_TRACE_PRESENT) && (source <= 16 || !(source % 600)))
        rt_log("rsf execution: source %llu role %u begin %u result %d", (unsigned long long)source, role, begin, result);
}

void rsf_vk_submission(uint64_t source, uint32_t begin) { execution_event(source, RSF_GAME_RENDER_SUBMISSION, begin); }
void rsf_vk_present_event(uint64_t source, uint32_t begin) { execution_event(source, RSF_GAME_RENDER_PRESENT, begin); }

static int append_extensions(const char **extensions, uint32_t *count, uint32_t capacity,
                             const char *const *names, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        int found = 0;
        for (uint32_t k = 0; k < *count; k++) found |= !strcmp(extensions[k], names[i]);
        if (!found) {
            if (*count >= capacity) { rt_log("rsf: Vulkan extension list is full"); return 0; }
            extensions[(*count)++] = names[i];
        }
    }
    return 1;
}

int rsf_vk_shutdown(void) {
    if (stopped || !runtime_loaded) return 1;
    if (xess_active) {
        __typeof__(&rsf_xessvk_shutdown) shutdown_xess = rsf_runtime_symbol("rsf_xessvk_shutdown");
        int32_t result = shutdown_xess ? shutdown_xess() : RSF_BACKEND_ERROR_MISSING_ENTRY_POINT;
        if (result != RSF_BACKEND_OK) {
            rt_log("rsf: XeSS shutdown refused (%d); retain Vulkan owners", result);
            return 0;
        }
        xess_active = 0;
    }
    __typeof__(&rsf_latencyvk_shutdown) shutdown = rsf_runtime_symbol("rsf_latencyvk_shutdown");
    int32_t result = shutdown ? shutdown() : RSF_BACKEND_ERROR_MISSING_ENTRY_POINT;
    if (result != RSF_BACKEND_OK) {
        rt_log("rsf: SDK shutdown refused (%d); retain Vulkan owners", result);
        return 0;
    }
    stopped = 1;
    return 1;
}

// Called only after checked SDK shutdown and destruction of the old Vulkan
// instance. A refused teardown keeps the resolver and all owners intact.
void rsf_vk_reset(void) {
    if (primary_instance || (runtime_loaded && !stopped)) return;
    prepared = active = runtime_loaded = stopped = xess_active = 0;
    instance_resolver = NULL;
}

static rsf_vk_function generic_instance_proc(void *instance, const char *name) {
    return (rsf_vk_function)rsf_vk_instance_proc(instance, name);
}

int rsf_vk_device_requirements(VkPhysicalDevice physical, void *features,
    const char **extensions, uint32_t *count, uint32_t capacity, void **enabled_features) {
    *enabled_features = features;
    if (!runtime_loaded) return 1;
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
    if (!append_extensions(extensions, count, capacity, requirements.extensions, requirements.extension_count)) return 0;
    *enabled_features = requirements.features;
    if (xess_active) {
        __typeof__(&rsf_xessvk_device_requirements) require_xess = rsf_runtime_symbol("rsf_xessvk_device_requirements");
        uint32_t n = 0;
        const char *const *names = NULL;
        result = require_xess ? require_xess(primary_instance, physical, &n, &names, enabled_features)
                              : RSF_BACKEND_ERROR_MISSING_ENTRY_POINT;
        if (result != RSF_BACKEND_OK) { rt_log("rsf: XeSS device requirements refused (%d)", result); return 0; }
        if (!append_extensions(extensions, count, capacity, names, n)) return 0;
    }
    return 1;
}

int rsf_vk_prepare(void) {
    if (prepared) return prepared > 0;
    prepared = -1;
    int requested = cfg_int("TOTK_RSF_ENABLED") || cfg_int("TOTK_RSF_FG") || cfg_int("TOTK_RSF_LOW_LATENCY");
    if (!requested || !rsf_runtime_path()) { prepared = 1; return 1; }
    if (!rsf_runtime_load()) return 0;
    runtime_loaded = 1;
    int sr_backend = cfg_int("TOTK_RSF_ENABLED") ? cfg_int("TOTK_RSF_BACKEND") : 0;
    if (sr_backend == RSF_SR_XESS) {
        __typeof__(&rsf_xessvk_initialize) initialize_xess = rsf_runtime_symbol("rsf_xessvk_initialize");
        rsf_xessvk_setup setup = {.struct_size = sizeof setup, .abi_version = RSF_XESSVK_REQUIREMENTS_ABI_VERSION,
            .runtime_directory_utf8 = rsf_provider_directory(RSF_SR_XESS)};
        int32_t result = initialize_xess ? initialize_xess(&setup) : RSF_BACKEND_ERROR_MISSING_ENTRY_POINT;
        if (result != RSF_BACKEND_OK) { rt_log("rsf: early XeSS initialization refused (%d)", result); return 0; }
        xess_active = 1;
    }
    // Latency-only sessions request no DLSS feature. The core chooses the
    // supported pacing owner after device creation, independently of SR.
    if (sr_backend != RSF_SR_DLSS && cfg_int("TOTK_RSF_FG") != 1 && !cfg_int("TOTK_RSF_LOW_LATENCY")) {
        prepared = 1; return 1;
    }
    __typeof__(&rsf_slvk_initialize) initialize = rsf_runtime_symbol("rsf_slvk_initialize");
    instance_resolver = rsf_runtime_symbol("rsf_latencyvk_instance_proc");
    if (!initialize || !instance_resolver) return 0;
    rsf_slvk_setup setup = {.struct_size = sizeof setup, .abi_version = RSF_SLVK_ABI_VERSION,
        .runtime_directory_utf8 = rsf_provider_directory(1),
        // NGX requires a GUID-shaped custom-engine identity. Stable UUIDv5 of
        // "totk-pc-recomp" in the DNS namespace, shared by every build.
        .project_id = "0a698e8c-91cc-5cb4-bb36-e1b402039c9e",
        .features = (sr_backend == RSF_SR_DLSS ? RSF_SLVK_DLSS_SR : 0) |
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

void rsf_vk_destroy_surface(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *allocator) {
    PFN_vkDestroySurfaceKHR destroy = (PFN_vkDestroySurfaceKHR)rsf_vk_instance_proc(instance, "vkDestroySurfaceKHR");
    if (destroy) destroy(instance, surface, allocator);
}

VkResult rsf_vk_create_instance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator,
                               VkInstance *out) {
    VkInstanceCreateInfo merged = *info;
    VkApplicationInfo app = info->pApplicationInfo ? *info->pApplicationInfo :
        (VkApplicationInfo){.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
    const char *extensions[64];
    if (xess_active) {
        __typeof__(&rsf_xessvk_instance_requirements) require_xess = rsf_runtime_symbol("rsf_xessvk_instance_requirements");
        uint32_t n = 0, minimum_api = 0, count = info->enabledExtensionCount;
        const char *const *names = NULL;
        int32_t result = require_xess ? require_xess(&n, &names, &minimum_api) : RSF_BACKEND_ERROR_MISSING_ENTRY_POINT;
        if (result != RSF_BACKEND_OK) { rt_log("rsf: XeSS instance requirements refused (%d)", result); return VK_ERROR_INITIALIZATION_FAILED; }
        if (count > 64) return VK_ERROR_EXTENSION_NOT_PRESENT;
        if (count) memcpy(extensions, info->ppEnabledExtensionNames, count * sizeof *extensions);
        if (!append_extensions(extensions, &count, 64, names, n)) return VK_ERROR_EXTENSION_NOT_PRESENT;
        merged.enabledExtensionCount = count;
        merged.ppEnabledExtensionNames = extensions;
        if (app.apiVersion < minimum_api) app.apiVersion = minimum_api;
        merged.pApplicationInfo = &app;
    }
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)rsf_vk_instance_proc(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = create(&merged, allocator, out);
    if (result == VK_SUCCESS) primary_instance = *out;
    return result;
}

VkResult rsf_vk_create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                             const VkAllocationCallbacks *allocator, VkDevice *out) {
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)rsf_vk_instance_proc(primary_instance, "vkCreateDevice");
    VkResult result = create ? create(physical, info, allocator, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (result == VK_SUCCESS && runtime_loaded) {
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

VkResult rsf_vk_enumerate_physical_devices(VkInstance instance, uint32_t *count, VkPhysicalDevice *devices) {
    // Streamline's enumeration records physical-device -> instance ownership.
    // Bypassing it leaves its device startup with a null instance dispatch table.
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)
        rsf_vk_instance_proc(instance, "vkEnumeratePhysicalDevices");
    return enumerate ? enumerate(instance, count, devices) : VK_ERROR_INITIALIZATION_FAILED;
}

void rsf_vk_destroy_instance(VkInstance instance, const VkAllocationCallbacks *allocator) {
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)rsf_vk_instance_proc(instance, "vkDestroyInstance");
    if (destroy) destroy(instance, allocator);
    if (instance == primary_instance) primary_instance = VK_NULL_HANDLE;
}

void rsf_vk_destroy_device(VkDevice device, const VkAllocationCallbacks *allocator) {
    __typeof__(&rsf_latencyvk_device_destroyed) destroyed = rsf_runtime_symbol("rsf_latencyvk_device_destroyed");
    if (runtime_loaded && destroyed) destroyed(device);
    PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)rsf_vk_device_proc(device, "vkDestroyDevice");
    if (destroy) destroy(device, allocator);
}
