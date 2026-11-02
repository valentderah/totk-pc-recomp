#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../runtime/vk_dlaa_select.h"

static VkDlaaDepthKey depth(uintptr_t image, uint32_t w, uint32_t h, uint32_t level, uint32_t layer) {
    return (VkDlaaDepthKey){image, level, layer, 1, w, h, 2};
}

static int color_ok(VkDlaaSceneSelection *s, const VkDlaaDepthKey *z, uintptr_t image,
                    uint32_t w, uint32_t h, int format, uint32_t samples, int view_2d, uint32_t serial) {
    return vk_dlaa_scene_color_allowed(s, s->anchor.valid, 1, format, 1, image, w, h, samples,
                                       view_2d, z, serial);
}

int main(void) {
    VkDlaaSceneSelection s = {0};
    VkDlaaDepthKey main_depth = depth(0x100, 2133, 1199, 0, 0);
    VkDlaaDepthKey later_aux = depth(0x200, 1066, 599, 0, 0);

    assert(!vk_dlaa_gbuffer_coherent(2, 0x10, 1, 1, 1, 2133, 1199, 1, 0, 1, &main_depth));
    assert(!vk_dlaa_gbuffer_coherent(3, 0x10, 1, 1, 1, 2132, 1199, 1, 0, 1, &main_depth));
    assert(vk_dlaa_gbuffer_coherent(3, 0x10, 1, 1, 1, 2133, 1199, 1, 0, 1, &main_depth));
    VkDlaaDepthKey too_small = depth(0x90, 639, 359, 0, 0);
    assert(!vk_dlaa_gbuffer_coherent(3, 0x10, 1, 1, 1, 639, 359, 1, 0, 1, &too_small));

    assert(!color_ok(&s, &main_depth, 0x300, 2133, 1199, 1, 1, 1, 10));
    assert(vk_dlaa_anchor_gbuffer(&s, 1, &main_depth, 20) == 1);
    assert(vk_dlaa_anchor_gbuffer(&s, 1, &main_depth, 21) == 2);
    assert(!color_ok(&s, &main_depth, 0x300, 2133, 1199, 1, 1, 1, 20));
    assert(color_ok(&s, &main_depth, 0x301, 2133, 1199, 1, 1, 1, 21));
    assert(vk_dlaa_scene_select_color(&s, 1, 0x301));
    assert(s.armed && s.color_image == 0x301);

    // A later auxiliary G-buffer, its color, and even a same-sized wrong-depth target cannot replace the first epoch.
    assert(!vk_dlaa_anchor_gbuffer(&s, 1, &later_aux, 30));
    assert(!color_ok(&s, &later_aux, 0x401, 1066, 599, 1, 1, 1, 31));
    VkDlaaDepthKey wrong_identity = depth(0x201, 2133, 1199, 0, 0);
    assert(!color_ok(&s, &wrong_identity, 0x402, 2133, 1199, 1, 1, 1, 32));
    VkDlaaDepthKey wrong_subresource = depth(0x100, 2133, 1199, 1, 0);
    assert(!color_ok(&s, &wrong_subresource, 0x408, 2133, 1199, 1, 1, 1, 32));
    VkDlaaDepthKey wrong_layer = depth(0x100, 2133, 1199, 0, 1);
    assert(!color_ok(&s, &wrong_layer, 0x409, 2133, 1199, 1, 1, 1, 32));
    assert(s.armed && s.color_image == 0x301);
    assert(!color_ok(&s, &main_depth, 0x403, 2132, 1199, 1, 1, 1, 33));
    assert(!color_ok(&s, &main_depth, 0x404, 2133, 1199, 1, 2, 1, 34));
    assert(!color_ok(&s, &main_depth, 0x405, 2133, 1199, 1, 1, 0, 35));
    assert(!color_ok(&s, &main_depth, 0x406, 2133, 1199, 0, 1, 1, 36));
    assert(s.armed && s.color_image == 0x301);

    // Both supported HDR formats use this same predicate; vk_dlaa_scene_color_format owns their Vulkan allowlist.
    assert(color_ok(&s, &main_depth, 0x407, 2133, 1199, 1, 1, 1, 37)); // B10G11R11_UFLOAT_PACK32
    assert(vk_dlaa_scene_select_color(&s, 1, 0x407));
    assert(s.color_image == 0x407);
    assert(color_ok(&s, &main_depth, 0x408, 2133, 1199, 1, 1, 1, 38)); // R16G16B16A16_SFLOAT

    // A dynamic scene extent above the existing 640px floor and rotating depth handles are valid first epochs.
    vk_dlaa_scene_selection_reset(&s);
    VkDlaaDepthKey dynamic_depth = depth(0x500, 1066, 599, 0, 0);
    assert(vk_dlaa_anchor_gbuffer(&s, 1, &dynamic_depth, 1));
    assert(color_ok(&s, &dynamic_depth, 0x501, 1066, 599, 1, 1, 1, 2));
    assert(vk_dlaa_scene_select_color(&s, 1, 0x501));
    vk_dlaa_scene_selection_reset(&s);
    VkDlaaDepthKey rotated_dynamic_depth = depth(0x600, 800, 450, 0, 0);
    assert(vk_dlaa_anchor_gbuffer(&s, 1, &rotated_dynamic_depth, 3));
    assert(color_ok(&s, &rotated_dynamic_depth, 0x601, 800, 450, 1, 1, 1, 4));

    // Known limitation: if an auxiliary multi-colour pass is first and has a matching later color, structural
    // provenance alone cannot prove it is not the scene. The real-GPU scene matrix must test this ordering.
    vk_dlaa_scene_selection_reset(&s);
    VkDlaaDepthKey early_aux = depth(0x610, 800, 450, 0, 0);
    VkDlaaDepthKey later_scene = depth(0x620, 2133, 1199, 0, 0);
    assert(vk_dlaa_anchor_gbuffer(&s, 1, &early_aux, 5));
    assert(!vk_dlaa_anchor_gbuffer(&s, 1, &later_scene, 6));
    assert(!color_ok(&s, &later_scene, 0x621, 2133, 1199, 1, 1, 1, 7));
    assert(color_ok(&s, &early_aux, 0x611, 800, 450, 1, 1, 1, 8));

    // If no trusted same-frame color follows the first G-buffer, the selector stays unarmed.
    vk_dlaa_scene_selection_reset(&s);
    assert(vk_dlaa_anchor_gbuffer(&s, 1, &main_depth, 5));
    assert(!color_ok(&s, &wrong_identity, 0x701, 2133, 1199, 1, 1, 1, 6));
    assert(!vk_dlaa_scene_select_color(&s, 0, 0x701));
    assert(!s.armed && !s.color_image);

    puts("DLAA scene selection tests passed");
    return 0;
}
