#include <stdio.h>
#include <string.h>
#include "cfg.h"
// These host declarations do not depend on the guest CPU/SIMD implementation.
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern const int rt_game_version;
extern const char rt_port_version[];
#include "perf_panel.h"
#include "perfmon.h"
#include "platform/platform.h"
#include "ui_text.h"
#include "uiwindow.h"

#ifdef _WIN32
#include <windows.h>
#include <math.h>

static void text(HDC dc, int x, int y, int width, const char *s, UINT align) {
    wchar_t wide[512];
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, wide, 512)) return;
    RECT r = {x, y, x + width, y + 80};
    DrawTextW(dc, wide, -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | align);
}
static float gb(float v) { return v < 0 ? -1 : v / 1024; }
static void cell(HDC dc, int x, int y, int width, UiTextId fmt, float v) {
    char b[128];
    if (v < 0 || !isfinite(v)) snprintf(b, sizeof b, "%s", menu_text(UI_MENU_OV_NONE));
    else snprintf(b, sizeof b, menu_text(fmt), v);
    text(dc, x, y, width, b, DT_RIGHT);
}
static void row(HDC dc, int y, int width, int line, UiTextId label,
                UiTextId f1, float v1, UiTextId f2, float v2, UiTextId f3, float v3) {
    int margin = line / 2, col = (width - 2 * margin) / 4;
    text(dc, margin, y, col, menu_text(label), DT_LEFT);
    cell(dc, margin+col, y, col, f1, v1);
    if (f2) cell(dc, margin+2*col, y, col, f2, v2);
    if (f3) cell(dc, margin+3*col, y, col, f3, v3);
}
static void draw(void *user, void *rawdc, int width, int height) {
    (void)user;
    HDC dc = rawdc;
    PerfStats s; perfmon_snapshot(&s);
    RECT bg = {0, 0, width, height};
    HBRUSH brush = CreateSolidBrush(RGB(18, 22, 27));
    FillRect(dc, &bg, brush); DeleteObject(brush);
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    int line = tm.tmHeight + 7, margin = line/2, y = margin;
    char game[16], b[256];
    snprintf(game, sizeof game, "%d.%d.%d", rt_game_version/100, rt_game_version/10%10, rt_game_version%10);
    snprintf(b, sizeof b, menu_text(UI_MENU_VERSION), rt_port_version, game);
    text(dc, margin, y, width-2*margin, b, DT_LEFT); y += line;
    SetTextColor(dc, RGB(140, 211, 80));
    row(dc,y,width,line,UI_MENU_OV_GPU,UI_MENU_OV_FMT_PCT,s.gpu_pct,UI_MENU_OV_FMT_TEMP,s.gpu_temp_c,UI_MENU_OV_FMT_POWER,s.gpu_power_w); y+=line;
    row(dc,y,width,line,UI_MENU_OV_VRAM,UI_MENU_OV_FMT_GB,gb(s.vram_used_mb),UI_MENU_OV_FMT_GB_OF,gb(s.vram_total_mb),UI_MENU_OV_FMT_MHZ,s.gpu_clock_mhz); y+=line;
    SetTextColor(dc, RGB(70, 165, 225));
    row(dc,y,width,line,UI_MENU_OV_CPU,UI_MENU_OV_FMT_PCT,s.cpu_total_pct,UI_MENU_OV_FMT_TEMP,s.cpu_temp_c,UI_MENU_OV_GAME_PCT,s.cpu_game_pct); y+=line;
    SetTextColor(dc, RGB(210, 130, 190));
    row(dc,y,width,line,UI_MENU_OV_RAM,UI_MENU_OV_FMT_GB,gb(s.ram_used_mb),UI_MENU_OV_FMT_GB_OF,gb(s.ram_total_mb),UI_MENU_OV_GAME_GB,gb(s.ram_game_mb)); y+=line+margin;
    SetTextColor(dc, RGB(240, 240, 240));
    row(dc,y,width,line,UI_MENU_OV_FPS_LABEL,UI_MENU_OV_FMT_FPS,s.fps,UI_MENU_OV_LOW1,s.low1_fps,UI_MENU_OV_LOW01,s.low01_fps); y+=line;
    row(dc,y,width,line,UI_MENU_OV_FRAME_LABEL,UI_MENU_OV_FRAME_MS,s.frame_ms_avg,UI_MENU_OV_MAX_MS,s.frame_ms_max,0,0); y+=line;
    int graph_h = line*3;
    float top = fmaxf(50, s.frame_ms_max);
    POINT points[PERF_HISTORY];
    for (int i=0; i<PERF_HISTORY; i++) {
        float v = isfinite(s.frame_ms[i]) ? fmaxf(0, fminf(top, s.frame_ms[i])) : 0;
        points[i].x = margin + i*(width-2*margin)/(PERF_HISTORY-1);
        points[i].y = y+graph_h-(int)(v*graph_h/top);
    }
    HPEN mean = CreatePen(PS_DOT, 1, RGB(110, 120, 135));
    HGDIOBJ old = SelectObject(dc, mean);
    int average_y = y+graph_h-(int)(fmaxf(0,fminf(top,s.frame_ms_avg))*graph_h/top);
    MoveToEx(dc,margin,average_y,NULL); LineTo(dc,width-margin,average_y);
    SelectObject(dc,old); DeleteObject(mean);
    HPEN curve = CreatePen(PS_SOLID, 1, RGB(80, 175, 255));
    old = SelectObject(dc, curve); Polyline(dc,points,PERF_HISTORY); SelectObject(dc,old); DeleteObject(curve);
    y+=graph_h+margin;
    SetTextColor(dc, RGB(238, 194, 85));
    const UiTextId labels[] = {UI_MENU_OV_REPLAY,UI_MENU_OV_GPU_TIME,UI_MENU_OV_PIPE_WAIT,UI_MENU_OV_SHADER_WAIT,UI_MENU_OV_TEX_UPLOADS};
    const float values[] = {s.replay_ms,s.gpu_ms,s.pipe_wait_ms,s.xlate_ms,s.tex_uploads};
    for (int i=0;i<5;i++,y+=line) {
        text(dc,margin,y,(width-2*margin)*2/3,menu_text(labels[i]),DT_LEFT);
        cell(dc,margin+(width-2*margin)*2/3,y,(width-2*margin)/3,i==4?UI_MENU_OV_PER_FRAME:UI_MENU_OV_FMT_MS,values[i]);
    }
    SetTextColor(dc, RGB(150, 160, 175));
    if (s.gpu_name[0]) { text(dc,margin,y,width-2*margin,s.gpu_name,DT_LEFT); y+=line; }
    if (s.cpu_name[0]) text(dc,margin,y,width-2*margin,s.cpu_name,DT_LEFT);
}
#else
static void draw(void *user, void *dc, int w, int h) { (void)user; (void)dc; (void)w; (void)h; }
#endif

static PlatMutex lock = PLAT_MUTEX_INIT;
static UiWindow *window;
static int suppressed, stopping, active;
static const char *title;

void perf_panel_tick(void) {
    plat_mutex_lock(&lock);
    if (stopping) { plat_mutex_unlock(&lock); return; }
    int requested = window && uiwindow_take_close_requested(window);
    int closed = window && !uiwindow_is_open(window);
    // WM_CLOSE publishes its notification before the closed state; collect a close between these reads.
    if (closed && !requested) requested = uiwindow_take_close_requested(window);
    if (requested) {
        suppressed = 1;
        cfg_set("TOTK_PERF_WINDOW", "0"); cfg_save();
    }
    int enabled = cfg_bool("TOTK_PERF_WINDOW");
    if (!enabled) suppressed = 0;
    if (window && (!enabled || closed)) {
        if (enabled) suppressed = 1;   // An unexpected thread exit also requires an explicit off/on to retry.
        uiwindow_close(window); window = NULL;
        __atomic_store_n(&active, 0, __ATOMIC_RELEASE);
    }
    if (enabled && !suppressed && !window) {
        title = menu_text(UI_MENU_PERF_WINDOW);
        window = uiwindow_open(title, 680, 620, 30, draw, NULL);
        if (!window) { suppressed = 1; rt_log("perf panel: window unavailable; keeping the in-game overlay setting"); }
        __atomic_store_n(&active, window != NULL, __ATOMIC_RELEASE);
    }
    if (window && title != menu_text(UI_MENU_PERF_WINDOW)) {
        title = menu_text(UI_MENU_PERF_WINDOW); uiwindow_set_title(window, title);
    }
    plat_mutex_unlock(&lock);
}
void perf_panel_close(void) {
    plat_mutex_lock(&lock);
    stopping = 1;
    if (window) { uiwindow_close(window); window = NULL; }
    __atomic_store_n(&active, 0, __ATOMIC_RELEASE);
    plat_mutex_unlock(&lock);
}
int perf_panel_active(void) { return __atomic_load_n(&active, __ATOMIC_ACQUIRE); }
