// Synthetic loader for a host-check subprocess that crashes after printing complete JSON. No GPU is accessed.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

static VKAPI_ATTR VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo *info,
        const VkAllocationCallbacks *allocator, VkInstance *instance) {
    (void)info; (void)allocator;
    *instance = (VkInstance)(uintptr_t)1;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL enumerate_devices(VkInstance instance, uint32_t *count,
        VkPhysicalDevice *devices) {
    (void)instance;
    if (*count && devices) devices[0] = (VkPhysicalDevice)(uintptr_t)2;
    *count = 1;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice device, VkPhysicalDeviceProperties *out) {
    (void)device;
    memset(out, 0, sizeof *out);
    out->apiVersion = VK_API_VERSION_1_3;
    strcpy(out->deviceName, "Synthetic GPU");
}

static VKAPI_ATTR void VKAPI_CALL destroy_instance(VkInstance instance, const VkAllocationCallbacks *allocator) {
    (void)instance; (void)allocator;
    const char *mode = getenv("HOSTCHECK_FIXTURE");
    if (mode && !strcmp(mode, "crash-destroy")) {
        // The probe and this fixture use the same C runtime. Make the probe's buffered JSON visible before the fault.
        fflush(NULL);
        RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, NULL);
    }
}

__declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
        const char *name) {
    (void)instance;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)create_instance;
    if (!strcmp(name, "vkEnumeratePhysicalDevices")) return (PFN_vkVoidFunction)enumerate_devices;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties")) return (PFN_vkVoidFunction)properties;
    if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)destroy_instance;
    return NULL;
}
