#pragma once
#include "vk_exec.h"
typedef struct DlaaGpuProfile DlaaGpuProfile;
DlaaGpuProfile *dlaa_gpu_profile_begin(VkExec *ex,uint64_t frame);
void dlaa_gpu_profile_mark(DlaaGpuProfile *p,VkExec *ex,unsigned index);
void dlaa_gpu_profile_live(DlaaGpuProfile *p,int evaluated);
void dlaa_gpu_profile_end(DlaaGpuProfile *p,VkExec *ex);
