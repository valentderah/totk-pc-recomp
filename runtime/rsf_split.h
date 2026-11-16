// Host-owned submission sections for the external SR timeline. No SDK source.
#pragma once
#include <vulkan/vulkan.h>
typedef struct RsfSplitSection {
    VkCommandBuffer commands;
    VkSemaphore timeline;
    uint64_t wait_value, signal_value;
} RsfSplitSection;
typedef VkResult (*RsfSplitSubmit)(const VkSubmitInfo2 *, void *);
// The original last command is replaced by ordered sections. Prologues and
// acquire waits stay first; ordinary signals (including retirement) stay last.
VkResult rsf_split_submit(const VkSubmitInfo2 *, const RsfSplitSection *, uint32_t,
                         RsfSplitSubmit, void *);
