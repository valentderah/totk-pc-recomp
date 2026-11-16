// Mouse wheel and keyboard gameplay tweaks: Ultrahand rotation and distance, item and weapon wheel, map zoom.
#pragma once
#include <stdint.h>

int gameplay_tweaks_available(void);
int gameplay_tweaks_capture_needed(void);
int gameplay_tweaks_map_zoom(void);
int gameplay_tweaks_strip_wheel(uint64_t buttons);
void gameplay_tweaks_init(void);
void gameplay_tweaks_reset(void);
void gameplay_tweaks_wheel(double delta, int c_down, int right_down);
void gameplay_tweaks_prepare_keys(uint8_t *down, uint64_t pad_buttons);
void gameplay_tweaks_apply(uint64_t frame, int active, uint64_t *buttons,
                           int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
