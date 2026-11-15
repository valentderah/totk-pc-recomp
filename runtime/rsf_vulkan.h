#pragma once
#include <vulkan/vulkan.h>

// Initialize the shared RSF SDK session before any Vulkan instance is created.
int rsf_vk_prepare(void);
PFN_vkVoidFunction rsf_vk_instance_proc(VkInstance instance, const char *name);
PFN_vkVoidFunction rsf_vk_device_proc(VkDevice device, const char *name);
VkResult rsf_vk_create_instance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
VkResult rsf_vk_create_device(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);

#ifndef RSF_VULKAN_IMPL
#define vkGetInstanceProcAddr rsf_vk_instance_proc
#define vkGetDeviceProcAddr rsf_vk_device_proc
#define vkCreateInstance rsf_vk_create_instance
#define vkCreateDevice rsf_vk_create_device
#endif
