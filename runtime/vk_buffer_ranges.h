#pragma once
#include <stdint.h>
#include <vulkan/vulkan_core.h>

// Append proven write lanes, then sort and union before recording a copy. Allocation failure returns 0.
int vk_buffer_ranges_append(VkBufferCopy **regions, int *count, int *capacity, uint64_t offset, uint64_t bytes,
                            unsigned mask);
int vk_buffer_ranges_merge(VkBufferCopy *regions, int count);
