#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// The present path calls tick; operations are internally serialized. At shutdown, close joins drawing and permanently prevents reopening.
void input_panel_tick(void);
void input_panel_close(void);
#ifdef __cplusplus
}
#endif
