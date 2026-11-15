#pragma once
#include <vulkan/vulkan.h>

// Initialize the shared RSF SDK session before any Vulkan instance is created.
int rsf_vk_prepare(void);
// Drain/release providers first; SDK teardown precedes Vulkan device destruction.
int rsf_vk_shutdown(void);
// features is a caller-owned writable copy, leased through device creation.
int rsf_vk_device_requirements(VkPhysicalDevice physical, void *features,
    const char **extensions, uint32_t *count, uint32_t capacity, void **enabled_features);
PFN_vkVoidFunction rsf_vk_instance_proc(VkInstance instance, const char *name);
PFN_vkVoidFunction rsf_vk_device_proc(VkDevice device, const char *name);
VkResult rsf_vk_create_instance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
VkResult rsf_vk_enumerate_physical_devices(VkInstance, uint32_t *, VkPhysicalDevice *);
void rsf_vk_destroy_instance(VkInstance, const VkAllocationCallbacks *);
VkResult rsf_vk_create_device(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);
void rsf_vk_destroy_device(VkDevice, const VkAllocationCallbacks *);

#ifndef RSF_VULKAN_IMPL
#define vkGetInstanceProcAddr rsf_vk_instance_proc
#define vkGetDeviceProcAddr rsf_vk_device_proc
#define vkCreateInstance rsf_vk_create_instance
#define vkEnumeratePhysicalDevices rsf_vk_enumerate_physical_devices
#define vkDestroyInstance rsf_vk_destroy_instance
#define vkCreateDevice rsf_vk_create_device
#define vkDestroyDevice rsf_vk_destroy_device
#endif
