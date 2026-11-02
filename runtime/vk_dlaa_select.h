// Frame-local scene-target selection for DLAA (vk_dlaa.c).
#pragma once
#include <stdint.h>

typedef struct {
    uintptr_t image;
    uint32_t level, base_layer, layers;
    uint32_t width, height, aspect;
} VkDlaaDepthKey;

typedef struct {
    VkDlaaDepthKey depth;
    uint32_t pass_serial;
    int valid;
} VkDlaaSceneAnchor;

typedef struct {
    VkDlaaSceneAnchor anchor;
    uintptr_t color_image;
    int armed;
} VkDlaaSceneSelection;

static inline int vk_dlaa_depth_key_equal(const VkDlaaDepthKey *a, const VkDlaaDepthKey *b) {
    return a->image && a->image == b->image && a->level == b->level && a->base_layer == b->base_layer &&
           a->layers == b->layers && a->width == b->width && a->height == b->height && a->aspect == b->aspect;
}

static inline int vk_dlaa_gbuffer_coherent(uint32_t color_targets, uintptr_t color_image, int depth_format,
                                           int color_aspect, int depth_aspect, uint32_t color_width,
                                           uint32_t color_height, uint32_t color_samples, int color_is_3d,
                                           uint32_t depth_samples, const VkDlaaDepthKey *depth) {
    return color_targets >= 3 && color_image && depth_format && color_aspect && depth_aspect && depth &&
           depth->image && depth->width >= 640 && color_width == depth->width && color_height == depth->height &&
           color_samples == depth_samples && !color_is_3d;
}

static inline int vk_dlaa_anchor_gbuffer(VkDlaaSceneSelection *s, int qualifying, const VkDlaaDepthKey *depth,
                                         uint32_t pass_serial) {
    if (!qualifying || !depth || !depth->image || !depth->width || !depth->height || !depth->layers) return 0;
    if (!s->anchor.valid) {
        s->anchor.depth = *depth;
        s->anchor.pass_serial = pass_serial;
        s->anchor.valid = 1;
        return 1;
    }
    return vk_dlaa_depth_key_equal(&s->anchor.depth, depth) ? 2 : 0;
}

static inline int vk_dlaa_scene_color_allowed(const VkDlaaSceneSelection *s, int after_anchor, int one_color_pass,
                                              int scene_format, int depth_format, uintptr_t color_image,
                                              uint32_t color_width, uint32_t color_height, uint32_t color_samples,
                                              int color_view_2d, const VkDlaaDepthKey *depth,
                                              uint32_t pass_serial) {
    return s->anchor.valid && after_anchor && pass_serial > s->anchor.pass_serial && one_color_pass && scene_format &&
           depth_format && color_image && depth && vk_dlaa_depth_key_equal(&s->anchor.depth, depth) &&
           color_width >= 640 && color_width == depth->width && color_height == depth->height &&
           color_samples <= 1 && color_view_2d;
}

static inline int vk_dlaa_scene_select_color(VkDlaaSceneSelection *s, int allowed, uintptr_t image) {
    if (!allowed || !image) return 0;
    s->armed = 1;
    s->color_image = image;
    return 1;
}

static inline void vk_dlaa_scene_selection_reset(VkDlaaSceneSelection *s) {
    *s = (VkDlaaSceneSelection){0};
}
