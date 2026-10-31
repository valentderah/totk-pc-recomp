#pragma once
#include "vk.h"
#include "vk_exec.h"
#include "dlaa_gpu_profile.h"
typedef struct {
    DlaaGpuProfile *gpu_profile;VkExec *ex;VkImageView current_depth,previous_depth,skin_motion;VkImage depth_image;const VkExecTarget *color;uint32_t width,height;
    float reprojection[16];double current_vp[16],previous_vp[16],raw_current_vp[16],raw_previous_vp[16];float current_jitter[2],previous_jitter[2];
    int active,support,forced_invalid,color_test,motion_projection_coordinated;uint64_t frame,export_frame;const char *directory;
} CameraGpuInput;
void vk_camera_gpu_pass(const CameraGpuInput *input);
void vk_camera_gpu_shutdown(void);
