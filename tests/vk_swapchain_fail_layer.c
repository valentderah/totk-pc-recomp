// Test-only explicit Vulkan layer: fail one replacement after the driver retired the old swapchain.
// Supports one instance/device, as used by the game regression. Never installed as an implicit/system layer.
#include <stdio.h>
#include <string.h>
#include <vulkan/vk_layer.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

static VkInstance instance;
static PFN_vkGetInstanceProcAddr next_gipa;
static PFN_GetPhysicalDeviceProcAddr next_gpdpa;
static PFN_vkGetDeviceProcAddr next_gdpa;
static PFN_vkCreateSwapchainKHR next_create_swapchain;
static PFN_vkDestroySwapchainKHR next_destroy_swapchain;
static PFN_vkAcquireNextImageKHR next_acquire;
static PFN_vkAcquireNextImage2KHR next_acquire2;
static VkSwapchainKHR retired;
static int injected, recovered, said_acquire, said_reuse;

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance inst, const char *name);
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name);

static void note(const char *text) {
    fprintf(stderr, "swapchain-test: %s\n", text);
    fflush(stderr);
}

static VKAPI_ATTR VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo *info,
                                                      const VkAllocationCallbacks *allocator, VkInstance *out) {
    VkLayerInstanceCreateInfo *link = (VkLayerInstanceCreateInfo *)info->pNext;
    while (link && (link->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || link->function != VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo *)link->pNext;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    next_gpdpa = link->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)next_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult result = create(info, allocator, out);
    if (result == VK_SUCCESS) instance = *out;
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                                    const VkAllocationCallbacks *allocator, VkDevice *out) {
    VkLayerDeviceCreateInfo *link = (VkLayerDeviceCreateInfo *)info->pNext;
    while (link && (link->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO || link->function != VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo *)link->pNext;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    next_gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
    VkResult result = create(physical, info, allocator, out);
    if (result == VK_SUCCESS) {
        next_create_swapchain = (PFN_vkCreateSwapchainKHR)next_gdpa(*out, "vkCreateSwapchainKHR");
        next_destroy_swapchain = (PFN_vkDestroySwapchainKHR)next_gdpa(*out, "vkDestroySwapchainKHR");
        next_acquire = (PFN_vkAcquireNextImageKHR)next_gdpa(*out, "vkAcquireNextImageKHR");
        next_acquire2 = (PFN_vkAcquireNextImage2KHR)next_gdpa(*out, "vkAcquireNextImage2KHR");
    }
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                                                       const VkAllocationCallbacks *allocator, VkSwapchainKHR *out) {
    if (retired && info->oldSwapchain == retired) {
        if (!said_reuse++) note("INVALID: retired handle passed as oldSwapchain again");
        // Catch the application's invalid call without sending it to the driver.
        *out = VK_NULL_HANDLE;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkResult result = next_create_swapchain(device, info, allocator, out);
    if (result == VK_SUCCESS && info->oldSwapchain && !injected) {
        // The real create has retired oldSwapchain. Discard its replacement and report a transient failure so the
        // application sees exactly the lifetime rule of a failed replacement, without exhausting real device memory.
        retired = info->oldSwapchain;
        next_destroy_swapchain(device, *out, allocator);
        *out = VK_NULL_HANDLE;
        injected = 1;
        note("injected replacement failure; old swapchain is retired");
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (result == VK_SUCCESS && injected && !info->oldSwapchain && !recovered) {
        recovered = 1;
        note("recovered: fresh swapchain created without retired oldSwapchain");
    }
    return result;
}

static VKAPI_ATTR void VKAPI_CALL destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain,
                                                    const VkAllocationCallbacks *allocator) {
    if (retired && swapchain == retired) {
        note("retired swapchain destroyed");
        retired = VK_NULL_HANDLE;   // the driver may reuse the handle's numeric value for a later fresh chain
    }
    next_destroy_swapchain(device, swapchain, allocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL acquire(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                              VkSemaphore semaphore, VkFence fence, uint32_t *index) {
    if (retired && swapchain == retired) {
        if (!said_acquire++) note("INVALID: acquire from retired swapchain");
        return VK_ERROR_OUT_OF_DATE_KHR;
    }
    return next_acquire(device, swapchain, timeout, semaphore, fence, index);
}

static VKAPI_ATTR VkResult VKAPI_CALL acquire2(VkDevice device, const VkAcquireNextImageInfoKHR *info, uint32_t *index) {
    if (retired && info->swapchain == retired) {
        if (!said_acquire++) note("INVALID: acquire from retired swapchain");
        return VK_ERROR_OUT_OF_DATE_KHR;
    }
    return next_acquire2(device, info, index);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)create_swapchain;
    if (!strcmp(name, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)destroy_swapchain;
    if (!strcmp(name, "vkAcquireNextImageKHR")) return (PFN_vkVoidFunction)acquire;
    if (!strcmp(name, "vkAcquireNextImage2KHR")) return (PFN_vkVoidFunction)acquire2;
    return next_gdpa ? next_gdpa(device, name) : NULL;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance inst, const char *name) {
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)create_instance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)create_device;
    if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)create_swapchain;
    if (!strcmp(name, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)destroy_swapchain;
    if (!strcmp(name, "vkAcquireNextImageKHR")) return (PFN_vkVoidFunction)acquire;
    if (!strcmp(name, "vkAcquireNextImage2KHR")) return (PFN_vkVoidFunction)acquire2;
    return next_gipa ? next_gipa(inst, name) : NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_physical_proc(VkInstance inst, const char *name) {
    return next_gpdpa ? next_gpdpa(inst, name) : NULL;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *version) {
    if (version->loaderLayerInterfaceVersion > 2) version->loaderLayerInterfaceVersion = 2;
    version->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    version->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    version->pfnGetPhysicalDeviceProcAddr = get_physical_proc;
    return VK_SUCCESS;
}
