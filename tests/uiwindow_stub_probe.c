// Non-Windows window-stub regression: portable C, no SDL library, graphics or game dependency.
// From the repository root, create build/test or replace the output directory below.
// clang -O2 -Wall -Wextra -Werror -I runtime tests/uiwindow_stub_probe.c runtime/uiwindow_sdl.c -o build/test/uiwindow_stub_probe
// Run: build/test/uiwindow_stub_probe
#include <stdarg.h>
#include <stdio.h>
#include "uiwindow.h"
static int logs;
void rt_log(const char *fmt, ...) { (void)fmt; logs++; }
static void draw(void *p, void *dc, int w, int h) { (void)p; (void)dc; (void)w; (void)h; }
int main(void) {
    int failures=0;
    for(int i=0;i<3;i++) if(uiwindow_open("stub",300,200,30,draw,NULL)) failures++;
    uiwindow_close(NULL); uiwindow_set_title(NULL,"noop"); uiwindow_repaint(NULL);
    if(uiwindow_is_open(NULL) || uiwindow_take_close_requested(NULL) || logs!=1) failures++;
    printf("uiwindow stub probe: %d failures\n",failures); return failures?1:0;
}
