// Real SDK/loader probe, independent of the game and its content.
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <windows.h>
#include "rsf_vulkan.h"

static HMODULE runtime;
static const char *provider_directory;
int rsf_runtime_load(void) { return runtime != NULL; }
void *rsf_runtime_symbol(const char *name) { return (void *)GetProcAddress(runtime, name); }
const char *cfg_get(const char *name) {
    if (!strcmp(name, "TOTK_RSF_RUNTIME")) return "probe";
    if (!strcmp(name, "TOTK_RSF_PROVIDER_DIR")) return provider_directory;
    return NULL;
}
int cfg_int(const char *name) { (void)name; return 1; }
void rt_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
}
int main(int argc, char **argv) {
    if (argc != 3) return 2;
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
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)vkGetInstanceProcAddr(instance, "vkDestroyInstance");
    if (!destroy) return 1;
    destroy(instance, NULL);
    int32_t (*shutdown)(void) = rsf_runtime_symbol("rsf_latencyvk_shutdown");
    return !shutdown || shutdown() != 0;
}
