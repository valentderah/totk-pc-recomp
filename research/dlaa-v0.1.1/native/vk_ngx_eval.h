#pragma once
#include "vk_exec.h"
typedef struct {VkExec *ex;uint32_t width,height;uint64_t frame;float jitter[2];const char *directory;VkImage image[4];VkFormat format[4];} VkNgxEvalInput;
VkImage vk_ngx_eval_once(const VkNgxEvalInput *input);
int vk_ngx_eval_present(const VkNgxEvalInput *input,VkImage output);
int vk_ngx_eval_live_enabled(void);
int vk_ngx_eval_live_healthy(void);
void vk_ngx_eval_live_shutdown(void);
void vk_ngx_eval_projection_tick(VkExec *ex,uint64_t frame);
