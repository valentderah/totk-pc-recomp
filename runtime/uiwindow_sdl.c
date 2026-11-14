#include <stddef.h>
// The stub does not depend on guest CPU/SIMD headers.
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include "uiwindow.h"

UiWindow *uiwindow_open(const char *title, int w, int h, int hz, UiWindowDrawFn draw, void *user) {
    (void)title; (void)w; (void)h; (void)hz; (void)draw; (void)user;
    static int warned;
    if (!__atomic_exchange_n(&warned, 1, __ATOMIC_RELAXED)) rt_log("uiwindow: separate panels are only supported on Windows");
    return NULL;
}
void uiwindow_close(UiWindow *w) { (void)w; }
int uiwindow_is_open(const UiWindow *w) { (void)w; return 0; }
int uiwindow_take_close_requested(UiWindow *w) { (void)w; return 0; }
void uiwindow_repaint(UiWindow *w) { (void)w; }
void uiwindow_set_title(UiWindow *w, const char *title) { (void)w; (void)title; }
