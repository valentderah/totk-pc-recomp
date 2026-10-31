// DLAA (vk_dlaa.c): NVIDIA DLSS at the scene's own size, behind TOTK_DLAA (totk.ini dlaa = 1).
#pragma once
#include "vk.h"

int vk_dlaa_wanted(void);   // the setting is on (the device may still lack DLSS)
// vk_device.c: NGX's instance and device extensions join the lists (room for max names); vk_dlaa_init after the
// device exists brings NGX up and checks that DLSS is available.
void vk_dlaa_instance_exts(const char **ext, uint32_t *n, uint32_t max);
void vk_dlaa_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max);
void vk_dlaa_init(void);
// vk_exec.c vk_exec_pass_begin, once the pass's targets are resolved and before its barrier: arms the frame on the
// scene's light target or, at the first quarter-width pass after it, runs DLAA on that target. Each present ends the
// frame (vk_dlaa_frame).
typedef struct VkExec VkExec;
void vk_dlaa_pass(VkExec *ex);
void vk_dlaa_frame(void);
// vk_exec_draw.c, each draw before its fixed state is recorded: notes the scene camera block (vk_dlaa_draw), and moves
// the viewport of the scene's draws by the frame's jitter (vk_dlaa_viewport, when the viewport is recorded).
void vk_dlaa_draw(VkExec *ex);
void vk_dlaa_viewport(VkExec *ex, VkViewport *v);
// vk_exec_draw.c, once a draw's fragment shader is known: 1 skips the draw (the game's FXAA while TOTK_FXAA is 0, its
// scaling draw when DLSS wrote the scaled picture).
int vk_dlaa_skip_draw(VkExec *ex, const GfxShader *fs);
