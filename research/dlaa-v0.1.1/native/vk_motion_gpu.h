#pragma once
#include "vk_exec.h"
void vk_motion_gpu_op(uint32_t op, const void *payload);
void vk_motion_gpu_draw(VkExec *ex, const VkPipeState *state, const VkPipeInfo *info);
void vk_motion_gpu_pass_end(VkExec *ex);
void vk_motion_gpu_shutdown(void);
