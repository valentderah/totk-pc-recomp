// Hide the game's HUD (vk_hud.c): TOTK_HIDE_HUD (totk.ini hide_hud = 0, 1 or auto) and TOTK_HIDE_HUD_IDLE, read at each
// present.
#pragma once
#include "vk.h"

typedef struct VkExec VkExec;
// vk_exec.c, at each pass in replay order (vk_exec_pass_begin in the sequential replay, the scan of a chunked one):
// follows the scene through the frame until the bloom chain ends it. Looks only at the targets; asking again for the
// same pass changes nothing.
void vk_hud_pass(const VkExec *ex);
// vk_exec_draw.c, once a draw's fragment shader is known: 1 skips the draw (a HUD draw while hide_hud is on).
int vk_hud_skip_draw(VkExec *ex, const GfxShader *fs);
// vk_backend.c present_image, beside vk_dlaa_frame: ends the frame and reads the setting for the next one.
void vk_hud_frame(void);
