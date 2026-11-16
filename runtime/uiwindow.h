#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct UiWindow UiWindow;
typedef void (*UiWindowDrawFn)(void *user, void *dc, int w, int h);

// The caller owns and serializes the handle; close is required even after the user closes the window.
// Each window draws on its own thread; callbacks must not call close or acquire present or queue locks.
// The back buffer is retained, so unchanged content may skip drawing; callbacks must redraw after a size change.
UiWindow *uiwindow_open(const char *title, int w, int h, int hz, UiWindowDrawFn draw, void *user);
// Request exit, join the drawing thread and free the handle; do not call from a draw callback.
void uiwindow_close(UiWindow *win);
int uiwindow_is_open(const UiWindow *win);
// Consume the user-close notification; a programmatic close does not generate it.
int uiwindow_take_close_requested(UiWindow *win);
void uiwindow_repaint(UiWindow *win);
// Copy the UTF-8 title; callable by the owner thread or a draw callback.
void uiwindow_set_title(UiWindow *win, const char *title);

#ifdef __cplusplus
}
#endif
