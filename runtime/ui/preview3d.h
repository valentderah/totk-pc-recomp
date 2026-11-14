// The GPU preview of a blueprint (ui/preview3d.cpp): an offscreen Vulkan render of the loaded blueprint's parts that the
// picker shows as one textured quad.
//
// It is optional at every step: init returns 0 when the device, the pipeline or the target cannot be made, and the
// picker then keeps drawing through ImGui's draw list. Nothing here allocates on the heap per frame: a part's vertex and
// index buffers are uploaded once and reused, and the image is only re-rendered when the blueprint or the camera changed.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Creates the pipeline and the offscreen targets if they are not there yet. 1 when the preview can be used.
int preview3d_init(void);
int preview3d_ready(void);

// The camera the picker is showing; a change marks the image for re-rendering.
void preview3d_set_camera(float yaw, float pitch, float zoom);

// A different blueprint is loaded: the next record rebuilds the image.
void preview3d_invalidate(void);

// The panel changed size: recreates the targets. 1 on success, 0 when the preview cannot be used at that size.
int preview3d_resize(int width, int height);

// The finished image as an ImGui texture (an ImTextureID), or NULL.
void *preview3d_texture(void);

// The fraction of the texture that holds the image (the rest of the fixed target is unused), so the quad is not stretched.
void preview3d_uv(float *u, float *v);

// Renders the image when it is stale. Recorded on the overlay's command buffer, so it costs nothing on a frame where
// nothing changed.
void preview3d_record(void *command_buffer);

#ifdef __cplusplus
}
#endif