// Host check of the installer's first step, compiled with the bundled clang at install time: does the CPU run
// -march=x86-64-v3 code (or only x86-64-v2), and which runtime GPU requirements are missing. Prints JSON.
// The Vulkan loader is opened at run time, so a machine without one gets "loader": false instead of a failed start.
#include <cpuid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#ifdef _WIN32
#include <windows.h>
#define open_lib() LoadLibraryA("vulkan-1.dll")
#define lib_sym(l, n) ((void *)GetProcAddress((l), (n)))
#else
#include <dlfcn.h>
#define open_lib() dlopen("libvulkan.so.1", RTLD_NOW)
#define lib_sym(l, n) dlsym((l), (n))
#endif

static void json_string(const char *s) {
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') printf("\\%c", *s);
        else if ((unsigned char)*s < 0x20) printf("\\u%04x", *s);
        else putchar(*s);
    }
    putchar('"');
}

// The x86-64-v2 level from CPUID: SSE3, SSSE3, SSE4.1, SSE4.2, POPCNT, CMPXCHG16B and LAHF/SAHF. Every CPU with
// SSE4.2 has the others, but the level is what the v2 build is compiled for, so check them all.
static int x86_64_v2(void) {
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
    const unsigned sse3 = 1u << 0, ssse3 = 1u << 9, cx16 = 1u << 13, sse41 = 1u << 19, sse42 = 1u << 20,
                   popcnt = 1u << 23;
    if ((c & (sse3 | ssse3 | cx16 | sse41 | sse42 | popcnt)) != (sse3 | ssse3 | cx16 | sse41 | sse42 | popcnt))
        return 0;
    if (!__get_cpuid(0x80000001, &a, &b, &c, &d)) return 0;
    return (c & (1u << 0)) != 0;   // LAHF/SAHF
}

// The x86-64-v3 level from CPUID: AVX, AVX2, BMI1, BMI2, F16C, FMA, LZCNT and MOVBE, with the OS saving the YMM state.
// Not __builtin_cpu_supports("x86-64-v3"): clang reads that from the C runtime's CPU model, and libgcc before GCC 12
// (the Linux release's sysroot has GCC 10) has no level bits, so it answers false on every CPU.
static int x86_64_v3(void) {
    if (!x86_64_v2()) return 0;
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
    const unsigned fma = 1u << 12, movbe = 1u << 22, osxsave = 1u << 27, avx = 1u << 28, f16c = 1u << 29;
    if ((c & (fma | movbe | osxsave | avx | f16c)) != (fma | movbe | osxsave | avx | f16c)) return 0;
    unsigned lo, hi;
    __asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 6) != 6) return 0;   // XMM and YMM state enabled by the OS
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return 0;
    const unsigned bmi1 = 1u << 3, avx2 = 1u << 5, bmi2 = 1u << 8;
    if ((b & (bmi1 | avx2 | bmi2)) != (bmi1 | avx2 | bmi2)) return 0;
    if (!__get_cpuid(0x80000001, &a, &b, &c, &d)) return 0;
    return (c & (1u << 5)) != 0;   // LZCNT (ABM)
}

int main(void) {
    printf("{\"x86_64_v2\": %s, \"x86_64_v3\": %s, ", x86_64_v2() ? "true" : "false",
           x86_64_v3() ? "true" : "false");
    void *lib = open_lib();
    PFN_vkGetInstanceProcAddr gipa = lib ? (PFN_vkGetInstanceProcAddr)lib_sym(lib, "vkGetInstanceProcAddr") : NULL;
    if (!gipa) {
        printf("\"loader\": false, \"devices\": []}\n");
        return 0;
    }
    printf("\"loader\": true, ");
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "totk-install",
                             .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = create ? create(&ci, NULL, &inst) : VK_ERROR_INITIALIZATION_FAILED;
    if (r != VK_SUCCESS) {
        printf("\"instance\": %d, \"devices\": []}\n", (int)r);
        return 0;
    }
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkEnumerateDeviceExtensionProperties extensions =
        (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");
    PFN_vkGetPhysicalDeviceFeatures2 features =
        (PFN_vkGetPhysicalDeviceFeatures2)gipa(inst, "vkGetPhysicalDeviceFeatures2");
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    VkPhysicalDevice devs[16];
    uint32_t n = 16;
    if (enumerate(inst, &n, devs) < 0) n = 0;
    printf("\"instance\": 0, \"devices\": [");
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        props(devs[i], &p);
        printf("%s{\"name\": ", i ? ", " : "");
        json_string(p.deviceName);
        printf(", \"api\": [%u, %u, %u], \"type\": %d, \"missing_extensions\": [", VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), (int)p.deviceType);
        const char *required[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
                                 VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
        uint32_t count = 0;
        VkResult er = extensions ? extensions(devs[i], NULL, &count, NULL) : VK_ERROR_INITIALIZATION_FAILED;
        VkExtensionProperties *ep = NULL;
        if (er == VK_SUCCESS && count) {
            ep = calloc(count, sizeof *ep);
            er = ep ? extensions(devs[i], NULL, &count, ep) : VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        int comma = 0;
        for (unsigned j = 0; j < sizeof required / sizeof *required; j++) {
            int found = 0;
            if (er == VK_SUCCESS)
                for (uint32_t k = 0; k < count; k++)
                    if (!strcmp(ep[k].extensionName, required[j])) found = 1;
            if (!found) {
                if (comma++) putchar(',');
                json_string(required[j]);
            }
        }
        free(ep);
        printf("], \"extension_query_ok\": %s, \"missing_features\": [", er == VK_SUCCESS ? "true" : "false");
        VkPhysicalDeviceDepthClipControlFeaturesEXT clip = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
        VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                               .pNext = &clip};
        VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                               .pNext = &f13};
        VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
        if (features) features(devs[i], &f2);
        // Keep this list aligned with runtime/vk_device.c create_device's required-feature gate.
        const struct { const char *name; VkBool32 supported; } required_features[] = {
            {"timelineSemaphore", f12.timelineSemaphore}, {"dynamicRendering", f13.dynamicRendering},
            {"synchronization2", f13.synchronization2},
            {"shaderStorageImageWriteWithoutFormat", f2.features.shaderStorageImageWriteWithoutFormat},
            {"depthClipControl", clip.depthClipControl}, {"shaderInt64", f2.features.shaderInt64},
            {"samplerAnisotropy", f2.features.samplerAnisotropy}, {"independentBlend", f2.features.independentBlend},
            {"geometryShader", f2.features.geometryShader}, {"textureCompressionBC", f2.features.textureCompressionBC}};
        comma = 0;
        for (unsigned j = 0; j < sizeof required_features / sizeof *required_features; j++) {
            if (!required_features[j].supported) {
                if (comma++) printf(", ");
                json_string(required_features[j].name);
            }
        }
        printf("], \"feature_query_ok\": %s}", features ? "true" : "false");
    }
    printf("]}\n");
    destroy(inst, NULL);
    return 0;
}
