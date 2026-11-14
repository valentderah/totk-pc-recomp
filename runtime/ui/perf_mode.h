#pragma once
#include <string.h>

// Display and sampling are independent: off plus a separate window does not draw an in-game overlay.
static inline int perf_display_mode(const char *value) {
    return value && !strcmp(value, "full") ? 2 : value && !strcmp(value, "fps") ? 1 : 0;
}
static inline int perf_sampling_mode(int display, int window_active) { return window_active ? 2 : display; }
