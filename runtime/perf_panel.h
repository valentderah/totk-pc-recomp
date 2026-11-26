#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void perf_panel_tick(void);
// Permanently stop the panel, including concurrent ticks, then join the drawing thread.
void perf_panel_close(void);
int perf_panel_active(void);
#ifdef __cplusplus
}
#endif
