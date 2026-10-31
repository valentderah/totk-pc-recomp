#pragma once
#include "vk_exec.h"
int vk_projection_probe_enabled(void);
int vk_projection_probe_override(VkExec *ex,int stage,int slot,uint64_t address,uint64_t bytes,VkDescriptorBufferInfo *out);

int vk_projection_probe_motion_enabled(void);
int vk_projection_probe_reconstruction_enabled(void);
int vk_projection_probe_camera_offset(VkExec *ex,uint64_t address,uint64_t bytes,float jitter[2]);

int vk_projection_probe_sequence_enabled(void);
int vk_projection_probe_expected_offset(uint64_t frame,float jitter[2]);
int vk_projection_probe_export_frame(uint64_t frame,uint64_t end);
void vk_projection_probe_shaders(VkExec *ex,const GfxShader *vs,const GfxShader *fs);

unsigned vk_projection_probe_frame_count(void);
void vk_projection_probe_live_ready(uint64_t frame,int ready);
