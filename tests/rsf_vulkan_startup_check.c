// Real SDK/loader probe, independent of the game and its content.
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <windows.h>
#include "rsf_vulkan.h"

static HMODULE runtime;
static const char *provider_directory;
static int backend = 1, disabled;
unsigned rt_trace;
int rsf_runtime_load(void) { return runtime != NULL; }
const char *rsf_runtime_path(void) { return runtime ? "probe" : NULL; }
const char *rsf_provider_directory(unsigned backend) { (void)backend; return provider_directory; }
void *rsf_runtime_symbol(const char *name) { return (void *)GetProcAddress(runtime, name); }
const char *cfg_get(const char *name) {
    if (!strcmp(name, "TOTK_RSF_RUNTIME")) return "probe";
    if (!strcmp(name, "TOTK_RSF_PROVIDER_DIR")) return provider_directory;
    return NULL;
}
int cfg_int(const char *name) {
    if (!strcmp(name, "TOTK_RSF_BACKEND")) return backend;
    return !strcmp(name, "TOTK_RSF_ENABLED") && backend != 0;
}
void rt_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
}
int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) return 2;
    if (argc == 4) backend = atoi(argv[3]);
    runtime = LoadLibraryA(argv[1]);
    if (!runtime) { printf("Runtime load failed: %lu\n", GetLastError()); return 1; }
    provider_directory = argv[2];
    if (!rsf_vk_prepare()) return 1;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "TotK RSF startup probe", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&info, NULL, &instance);
    printf("RSF Vulkan instance creation: %d\n", result);
    if (result != VK_SUCCESS) return 1;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties families = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    uint32_t count = 0;
    if (!families || vkEnumeratePhysicalDevices(instance, &count, NULL) != VK_SUCCESS || !count) return 1;
    VkPhysicalDevice *physical = calloc(count, sizeof *physical);
    if (!physical || vkEnumeratePhysicalDevices(instance, &count, physical) != VK_SUCCESS) return 1;
    uint32_t family_count = 0;
    families(physical[0], &family_count, NULL);
    VkQueueFamilyProperties *queues = calloc(family_count, sizeof *queues);
    if (!queues) return 1;
    families(physical[0], &family_count, queues);
    uint32_t family = 0;
    while (family < family_count && !(queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family++;
    free(queues);
    if (family == family_count) return 1;
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    void *enabled = &features;
    const char *extensions[8];
    uint32_t extension_count = 0;
    if (!rsf_vk_device_requirements(physical[0], &features, extensions, &extension_count, 8, &enabled)) return 1;
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = enabled,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
        .enabledExtensionCount = extension_count, .ppEnabledExtensionNames = extensions};
    VkDevice device;
    result = vkCreateDevice(physical[0], &device_info, NULL, &device);
    free(physical);
    printf("RSF Vulkan device creation: %d\n", result);
    if (result != VK_SUCCESS) return 1;
    int shutdown_ok = rsf_vk_shutdown();
    printf("RSF SDK shutdown: %s\n", shutdown_ok ? "success" : "failed");
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return !shutdown_ok;
}

int rsf_config_enabled(void) { return !disabled && cfg_int("TOTK_RSF_ENABLED"); }

int rsf_config_fg_backend(void) { return cfg_int("TOTK_RSF_FG"); }
int rsf_config_sr_backend(void) { return cfg_int("TOTK_RSF_BACKEND"); }
int rsf_config_low_latency(void) { return cfg_int("TOTK_RSF_LOW_LATENCY"); }
int rsf_requested(void) { return rsf_config_enabled() || rsf_config_fg_backend() || rsf_config_low_latency(); }
int rsf_temporal_requested(void) { return rsf_config_enabled() || rsf_config_fg_backend(); }

void *rsf_runtime_cpu_sink(void) { return rsf_runtime_symbol("rsf_latencyvk_cpu"); }
void *rsf_runtime_execution_sink(void) { return rsf_runtime_symbol("rsf_latencyvk_execution"); }

void rsf_runtime_disable(const char *reason) { disabled=1; rt_log("RSF disabled: %s",reason); }
