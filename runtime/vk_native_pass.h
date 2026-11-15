// Transfer-only suspension of an already resolved native pass. No producer callbacks or descriptor resolution.
#pragma once
#include "vk_exec.h"
typedef struct {
    VkCommandBuffer commands;
    uint64_t fence, frame, signature;
    uint32_t serial, textures;
    VkImageView colors[8], depth, stencil;
    int live;
} VkNativePass;
// Initial subset refuses feedback, queries, diagnostic pass marks, stand-ins, multisampling and chunks.
int vk_exec_native_suspend(VkExec *, VkNativePass *);
// Between these calls only recorder-owned transfer/barrier commands may run, without changing native bindings.
// Failure leaves rendering ended; caller must use the normal recovery path, never issue an unprepared draw.
int vk_exec_native_resume(VkExec *, VkNativePass *);
