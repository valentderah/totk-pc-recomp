// Non-Windows regression: even when enabled, both panels safely fall back without creating windows or raising sampling.
#include <stdio.h>
#include "cfg.h"
#include "input_panel.h"
#include "perf_panel.h"
#include "platform/platform.h"
#include "ui/perf_mode.h"
#include "ui_text.h"
#include "uiwindow.h"
static int logs, writes;
void rt_log(const char *fmt, ...) { (void)fmt; logs++; }
int cfg_bool(const char *name) { (void)name; return 1; }
void cfg_set(const char *name, const char *value) { (void)name; (void)value; writes++; }
int cfg_save(void) { writes++; return 1; }
const char *menu_text(UiTextId id) { (void)id; return "panel"; }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
int main(void) {
    int failures=0;
    for(int i=0;i<20;i++) { perf_panel_tick(); input_panel_tick(); }
    if(perf_panel_active() || writes || logs!=2) failures++;
    for(int mode=0;mode<3;mode++) if(perf_sampling_mode(mode,perf_panel_active())!=mode) failures++;
    perf_panel_close(); input_panel_close(); perf_panel_tick(); input_panel_tick();
    if(perf_panel_active() || writes || logs!=2) failures++;
    uiwindow_close(NULL); uiwindow_set_title(NULL,"safe"); uiwindow_repaint(NULL);
    if(uiwindow_is_open(NULL) || uiwindow_take_close_requested(NULL)) failures++;
    printf("overlay stub fallback probe: %d failures\n",failures); return failures?1:0;
}
