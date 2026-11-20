// AMD FSR 3 frame generation (vk_fg.cpp): the FidelityFX SDK 1.1.4's frame interpolation and optical flow
// (third_party/FidelityFX, MIT) on its Vulkan backend. Plain C interface: the SDK is C++.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// vk_device.c, after the device exists: with TOTK_FRAME_GEN=1 (totk.ini frame_gen = 1) makes the shared backend, the
// optical flow and the frame interpolation contexts and logs one line; otherwise does nothing.
void vk_fg_init(void);
// 1 while the contexts exist.
int vk_fg_active(void);
// vk_device.c destroy_all, before the device goes away; harmless when nothing was made.
void vk_fg_release(void);

#ifdef __cplusplus
}
#endif
