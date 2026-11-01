#pragma once
#include "vk.h"
#include "ngx_eval_abi.h"
int ngx_probe_device_extensions(VkInstance instance,VkPhysicalDevice physical,char names[][VK_MAX_EXTENSION_NAME_SIZE],unsigned capacity);
void ngx_probe_once(void);
int ngx_probe_eval_enabled(void);
int ngx_probe_record_eval(const TotkNgxEvalFrame *frame);
int ngx_probe_finish_eval(void);

int ngx_probe_sequence_enabled(void);
