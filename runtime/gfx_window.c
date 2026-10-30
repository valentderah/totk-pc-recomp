// Win32 window, Vulkan surface and swapchain, present blit, 60 Hz vsync thread, focus.
// Swapchain set-up ported from LAS Recomp, runtime/nvn/vulkan/device.c:254-330 (https://github.com/peiikai/LASRecomp @
// da35b83a9611d046a9a681aae668d7085954d37f, MIT, Copyright (c) 2026 Peiikai), SDL replaced by Win32. Focus and vsync
// behaviour from MacNativeGames, games/totk/runtime/frontend.m:88-97, 175-176, 350-352 (https://github.com/ly05010419/
// MacNativeGames @ b75be8cfa704df3701e86130c8ced268e79d6b11, MIT, Copyright (c) 2026 MacNativeGames contributors).
//
// Threads: the window thread owns the HWND and its message loop and never runs guest code. The vsync thread signals the
// bound system events. Presents come from any host thread (the back end's present worker) and take g_pl; every queue
// submission takes the recursive queue lock (vk_queue_lock). The swapchain is rebuilt lazily: WM_SIZE only marks it
// dirty, the next present (or gfx_window_resize) rebuilds it.
//
// The in-game menu (ui/menu.h) draws over the game image in the present's command buffer, after the blit; its keyboard
// and mouse come from the window procedure (ui/menu_win.cpp). The window's size and fullscreen state are the settings
// TOTK_WINDOW_SIZE and TOTK_FULLSCREEN, applied at start and whenever a cfg_set changes them (window_settings_live).
//
// The process is per-monitor DPI aware (totk.manifest): every size here is physical pixels, TOTK_WINDOW_SIZE included,
// and the swapchain matches the screen pixel for pixel. A window moved to a monitor at another display scale keeps its
// client size (WM_GETDPISCALEDSIZE); the menu takes its text size floor from the window's DPI (gfx_window_dpi).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "cfg.h"
#include "flight.h"
#include "fs.h"
#include "gfx.h"
#include "input.h"
#include "nvn_api.h"
#include "ui/menu.h"
#include "vk.h"

#define FRAMES 2
#define MAX_IMAGES 16
#define VSYNC_NS 1000000000ull
#define VSYNC_HZ 60

// ---- queue lock
static CRITICAL_SECTION g_ql;
__attribute__((constructor)) static void window_init_locks(void);
void vk_queue_lock(void) { EnterCriticalSection(&g_ql); }
void vk_queue_unlock(void) { LeaveCriticalSection(&g_ql); }

// ---- window state
static HWND g_hwnd;
static HANDLE g_wnd_ready;
static VkSurfaceKHR g_surface;
static volatile LONG g_dirty;               // WM_SIZE seen; swapchain stale
static volatile LONG g_focused = 1;
static volatile LONG g_focus_msgs;
static CRITICAL_SECTION g_start_lock;       // gfx_window_start / vsync start
static int g_started, g_wnd_failed;
static int g_background;                   // TOTK_BACKGROUND_WINDOW: the window stays behind the user's windows
static int g_fs_pending;                   // window thread: TOTK_FULLSCREEN waits for the first activation
static volatile LONG g_dpi = USER_DEFAULT_SCREEN_DPI;   // the window's DPI, set by the window thread

// ---- swapchain and present state (g_pl)
static CRITICAL_SECTION g_pl;
static VkSwapchainKHR g_sc;
static VkFormat g_sc_fmt;
static VkExtent2D g_sc_ext;
static int g_scaler;                        // the last present: 0 the game image 1:1, 1 scaled by the blit, 2 by FSR
static uint32_t g_sc_n;
static VkImage g_sc_img[MAX_IMAGES];
static VkImageView g_sc_view[MAX_IMAGES];   // colour attachment views for the menu overlay
static VkSemaphore g_render_done[MAX_IMAGES];
static VkCommandPool g_pool;
static VkCommandBuffer g_cb[FRAMES];
static VkFence g_fence[FRAMES];
static VkSemaphore g_acquire[FRAMES];
static unsigned g_frame;
static int g_capture;
static VkBuffer g_cap_buf;
static VmaAllocation g_cap_alloc;
static uint8_t *g_cap_ptr;
static VkDeviceSize g_cap_size;
static int g_cap_w, g_cap_h, g_cap_valid;
static int g_flip_on;            // vk_present_blit submitted the blit of swapchain image g_flip_idx; vk_present_flip presents it
static uint32_t g_flip_idx;

// ---- TOTK_FLIGHT thumbnails (flight.c; g_pl): each present also blits the swapchain image down into slot frame % g_th_n
// of a host-visible ring. A slot's tag names the frame it holds complete; recording a new frame clears the tag first,
// and the tag is set once the fence of that present has signalled.
#define TH_BYTES ((VkDeviceSize)FLIGHT_THUMB_W * FLIGHT_THUMB_H * 4)
static int g_th_n, g_th_failed;
static uint64_t g_th_frame;                // frame of the next vk_present_image call (vk_thumbs_frame)
static uint64_t *g_th_tag;                 // per ring slot
static uint64_t g_th_pending[FRAMES];      // per fence slot: the frame whose thumbnail its submission writes
static VkImage g_th_img;
static VmaAllocation g_th_img_alloc;
static VkFormat g_th_fmt;
static VkBuffer g_th_buf;
static VmaAllocation g_th_alloc;
static uint8_t *g_th_ptr;

static void window_init_locks(void) {
    InitializeCriticalSection(&g_ql);
    InitializeCriticalSection(&g_pl);
    InitializeCriticalSection(&g_start_lock);
}

// ---- vsync
static SRWLOCK g_vs_lock = SRWLOCK_INIT;
static uint64_t g_vs_ev[16];
static int g_vs_nev;
static volatile long *g_vs_ctr[4];
static int g_vs_nctr;
static volatile LONGLONG g_ticks;
static volatile LONGLONG g_vs_start_ns;
static int g_vs_started;

static DWORD WINAPI vsync_main(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HANDLE t = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!t) { rt_log("gfx: no high-resolution timer; vsync falls back to the system timer tick"); t = CreateWaitableTimerW(NULL, TRUE, NULL); }
    uint64_t n = 0;
    for (;;) {
        uint64_t start = (uint64_t)g_vs_start_ns;
        uint64_t due = start + (n + 1) * VSYNC_NS / VSYNC_HZ;
        uint64_t now = kern_now_ns();
        if (now > due + 100000000ull) {                  // the process stalled: restart the schedule, no catch-up burst
            g_vs_start_ns = (LONGLONG)(now - (n + 1) * VSYNC_NS / VSYNC_HZ);
        } else if (due > now) {
            LARGE_INTEGER d;
            d.QuadPart = -(LONGLONG)((due - now) / 100);
            if (d.QuadPart == 0) d.QuadPart = -1;
            SetWaitableTimer(t, &d, 0, NULL, NULL, FALSE);
            WaitForSingleObject(t, INFINITE);
        }
        n++;
        InterlockedExchange64(&g_ticks, (LONGLONG)n);
        uint64_t ev[16];
        volatile long *ctr[4];
        int ne, nc;
        AcquireSRWLockShared(&g_vs_lock);
        ne = g_vs_nev;
        nc = g_vs_nctr;
        memcpy(ev, g_vs_ev, sizeof ev);
        memcpy(ctr, g_vs_ctr, sizeof ctr);
        ReleaseSRWLockShared(&g_vs_lock);
        for (int i = 0; i < nc; i++) InterlockedIncrement(ctr[i]);
        for (int i = 0; i < ne; i++) kern_signal_system_event(ev[i]);
    }
    return 0;
}

static void vsync_ensure(void) {
    EnterCriticalSection(&g_start_lock);
    if (!g_vs_started) {
        g_vs_started = 1;
        g_vs_start_ns = (LONGLONG)kern_now_ns();
        HANDLE h = CreateThread(NULL, 0, vsync_main, NULL, 0, NULL);
        if (!h) rt_fatal("gfx: cannot start the vsync thread (error %lu)", GetLastError());
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_start_lock);
}

void gfx_vsync_bind(uint64_t system_event) {
    AcquireSRWLockExclusive(&g_vs_lock);
    int have = 0;
    for (int i = 0; i < g_vs_nev; i++) have |= g_vs_ev[i] == system_event;
    if (!have) {
        if (g_vs_nev == 16) rt_fatal("gfx: more than 16 vsync events");
        g_vs_ev[g_vs_nev++] = system_event;
    }
    ReleaseSRWLockExclusive(&g_vs_lock);
    vsync_ensure();
}

void gfx_vsync_bind_counter(volatile long *counter) {
    AcquireSRWLockExclusive(&g_vs_lock);
    if (g_vs_nctr == 4) rt_fatal("gfx: more than 4 vsync counters");
    g_vs_ctr[g_vs_nctr++] = counter;
    ReleaseSRWLockExclusive(&g_vs_lock);
    vsync_ensure();
}

uint64_t gfx_vsync_ticks(void) { return (uint64_t)InterlockedCompareExchange64(&g_ticks, 0, 0); }
uint64_t gfx_vsync_time_ns(uint64_t tick) { return (uint64_t)g_vs_start_ns + tick * VSYNC_NS / VSYNC_HZ; }

// ---- focus
int gfx_window_focused(void) { return g_focused; }
unsigned gfx_window_focus_messages(void) { return (unsigned)g_focus_msgs; }
HWND gfx_window_handle(void) { return g_hwnd; }

// ---- window thread
#define WND_STYLE WS_OVERLAPPEDWINDOW
#define WM_APP_SETTINGS (WM_APP + 1)
#define WM_APP_FULLSCREEN (WM_APP + 2)   // the fullscreen setting, held back until the window is first activated

static WINDOWPLACEMENT g_saved_place = {sizeof g_saved_place};   // the window behind fullscreen (window thread)

static int is_fullscreen(HWND h) { return !(GetWindowLongW(h, GWL_STYLE) & WS_OVERLAPPEDWINDOW); }

unsigned gfx_window_dpi(void) { return (unsigned)InterlockedCompareExchange(&g_dpi, 0, 0); }

// The outer size of a framed window whose client area is cw x ch. The frame is measured at the window's DPI, or the
// system DPI before the window exists: plain AdjustWindowRect always takes the system DPI, which is wrong on a monitor
// at another display scale.
static void outer_size(HWND h, int cw, int ch, int *ow, int *oh) {
    RECT r = {0, 0, cw, ch};
    AdjustWindowRectExForDpi(&r, WND_STYLE, FALSE, 0, h ? GetDpiForWindow(h) : GetDpiForSystem());
    *ow = r.right - r.left;
    *oh = r.bottom - r.top;
}

// TOTK_FULLSCREEN=1: borderless window over the monitor. TOTK_WINDOW_SIZE=<w>x<h>: client area of the window (the
// swapchain follows the window; the game image is scaled to it).
static int cfg_fullscreen(void) {
    return cfg_bool("TOTK_FULLSCREEN");
}

static void cfg_window_size(int *w, int *h) {
    int x = 0, y = 0;
    if (sscanf(cfg_value("TOTK_WINDOW_SIZE"), "%dx%d", &x, &y) != 2 || x < 640 || y < 360 || x > 7680 || y > 4320)
        sscanf(cfg_meta("WINDOW_SIZE")->def, "%dx%d", &x, &y);   // unparsable or out of range: the default
    *w = x;
    *h = y;
}

// Windows turns the display off and sleeps after the idle timeout, and gamepad input does not count as activity. While
// the window is up and not minimized it holds the display and the system on, as other games do. The state belongs to
// the calling thread, so only the window thread (window_main and wnd_proc) calls this; the process exit releases it.
static void keep_awake(int on) {
    static int cur = -1;
    if (on == cur) return;
    cur = on;
    SetThreadExecutionState(on ? ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
}

// F11: borderless window over the whole monitor, and back to the saved window. The swapchain follows through WM_SIZE.
// Not Alt+Enter: Enter is the Plus button (input_win.c). A background window (test runs) keeps its place behind the
// user's windows and takes no focus.
static void toggle_fullscreen(HWND h) {
    LONG style = GetWindowLongW(h, GWL_STYLE);
    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi = {sizeof mi};
        if (!GetWindowPlacement(h, &g_saved_place) || !GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi))
            return;
        SetWindowLongW(h, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | (g_background ? SWP_NOZORDER | SWP_NOACTIVATE : 0));
    } else {
        SetWindowLongW(h, GWL_STYLE, (style & ~WS_POPUP) | WND_STYLE);
        if (g_background) g_saved_place.showCmd = SW_SHOWNOACTIVATE;
        SetWindowPlacement(h, &g_saved_place);
        SetWindowPos(h, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED |
                         (g_background ? SWP_NOACTIVATE : 0));
    }
}

// The window size and fullscreen state last taken from the settings (window thread). apply_settings acts on a change of
// either value only, so a window resized by hand keeps its size when another setting changes.
static char g_applied_size[32];
static int g_applied_fs;

static void note_applied(void) {
    snprintf(g_applied_size, sizeof g_applied_size, "%s", cfg_value("TOTK_WINDOW_SIZE"));
    g_applied_fs = cfg_fullscreen();
}

// Window thread: the window takes the size and fullscreen state of the settings when they changed. In fullscreen the
// size goes into the placement restored when fullscreen ends.
static void apply_settings(HWND h) {
    int size_changed = strcmp(cfg_value("TOTK_WINDOW_SIZE"), g_applied_size) != 0, fs = cfg_fullscreen(), fs_changed = fs != g_applied_fs;
    note_applied();
    if (size_changed) {
        int cw, ch, ow, oh;
        cfg_window_size(&cw, &ch);
        outer_size(h, cw, ch, &ow, &oh);
        if (is_fullscreen(h)) {
            RECT *n = &g_saved_place.rcNormalPosition;
            n->right = n->left + ow;
            n->bottom = n->top + oh;
        } else {
            RECT have;
            if (GetClientRect(h, &have) && (have.right != cw || have.bottom != ch)) {
                if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
                SetWindowPos(h, NULL, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }
    if (fs_changed && fs != is_fullscreen(h)) {
        input_cursor_suspend(1);
        toggle_fullscreen(h);
        input_cursor_suspend(0);
    }
}

void gfx_window_apply_settings(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_SETTINGS, 0, 0);
}

// Present path (g_pl held): a settings change (cfg_version) reaches the window thread, which applies a changed size or
// fullscreen; a changed TOTK_VK_PRESENT rebuilds the swapchain at this present.
static void window_settings_live(void) {
    static unsigned seen;
    static int started;
    static char present[16];   // the TOTK_VK_PRESENT the swapchain was made with
    unsigned v = cfg_version();
    if (started && v == seen) return;
    seen = v;
    const char *e = cfg_value("TOTK_VK_PRESENT");
    if (!started || strncmp(e, present, sizeof present - 1)) {
        snprintf(present, sizeof present, "%s", e);
        if (started) InterlockedExchange(&g_dirty, 1);
    }
    if (started) gfx_window_apply_settings();
    started = 1;
}

void gfx_window_request_close(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

// ---- refresh rate of the window's monitor (TOTK_FPS_LIMIT=display). The window thread measures it when the window
// opens, lands on another monitor or the display mode changes; readers take the stored value. QueryDisplayConfig gives
// the exact rational rate (59.94, 143.998); EnumDisplaySettings' whole hertz is the fallback.
static HMONITOR g_rate_mon;
static volatile LONG g_rate_mhz;   // millihertz, 0 unknown

static double monitor_rate_hz(HMONITOR mon) {
    MONITORINFOEXW mi = {.cbSize = sizeof mi};
    if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi)) return 0;
    double hz = 0;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS && np && nm) {
        DISPLAYCONFIG_PATH_INFO *paths = calloc(np, sizeof *paths);
        DISPLAYCONFIG_MODE_INFO *modes = calloc(nm, sizeof *modes);
        if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS)
            for (UINT32 i = 0; i < np && !hz; i++) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {.header = {.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
                                                                   .size = sizeof src,
                                                                   .adapterId = paths[i].sourceInfo.adapterId,
                                                                   .id = paths[i].sourceInfo.id}};
                DISPLAYCONFIG_RATIONAL r = paths[i].targetInfo.refreshRate;
                if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS && !wcscmp(src.viewGdiDeviceName, mi.szDevice) &&
                    r.Denominator)
                    hz = (double)r.Numerator / r.Denominator;
            }
        free(paths);
        free(modes);
    }
    DEVMODEW dm = {.dmSize = sizeof dm};
    if (!hz && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        hz = dm.dmDisplayFrequency;   // 0 and 1 mean the hardware default
    return hz;
}

// Window thread. force: the display mode changed, measure even on the same monitor.
static void refresh_rate_update(HWND h, int force) {
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY);
    if (!force && mon == g_rate_mon) return;
    g_rate_mon = mon;
    double hz = monitor_rate_hz(mon);
    LONG mhz = (LONG)(hz * 1000 + 0.5);
    if (InterlockedExchange(&g_rate_mhz, mhz) != mhz) rt_log("gfx: monitor refresh rate %.3f Hz", hz);
}

double gfx_window_refresh_hz(void) { return InterlockedCompareExchange(&g_rate_mhz, 0, 0) / 1000.0; }

int gfx_window_monitor_size(int *w, int *h) {
    MONITORINFO mi = {sizeof mi};
    if (!g_hwnd || !GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) return 0;
    *w = mi.rcMonitor.right - mi.rcMonitor.left;
    *h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    return 1;
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    LRESULT input_res;
    if (input_window_msg(h, m, w, l, &input_res)) return input_res;
    if (menu_window_msg(h, m, w, l, &input_res)) return input_res;
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_F11 && !(l & (1 << 30))) {   // bit 30: auto-repeat
            input_cursor_suspend(1);
            toggle_fullscreen(h);
            input_cursor_suspend(0);
            cfg_set("TOTK_FULLSCREEN", is_fullscreen(h) ? "1" : "0");   // the menu shows it; the next start keeps it
            cfg_save();
            note_applied();   // what cfg_get reads now (the environment may still override it)
        }
        break;
    case WM_APP_SETTINGS:
        apply_settings(h);
        return 0;
    case WM_APP_FULLSCREEN:
        if (cfg_fullscreen() && !is_fullscreen(h)) {
            input_cursor_suspend(1);
            toggle_fullscreen(h);
            input_cursor_suspend(0);
        }
        return 0;
    case WM_SIZE:
        keep_awake(w != SIZE_MINIMIZED);
        if (w != SIZE_MINIMIZED) InterlockedExchange(&g_dirty, 1);
        refresh_rate_update(h, 0);
        return 0;
    case WM_EXITSIZEMOVE:   // dragged onto another monitor
        refresh_rate_update(h, 0);
        break;
    case WM_DISPLAYCHANGE:
        refresh_rate_update(h, 1);
        break;
    case WM_GETDPISCALEDSIZE:   // a framed window keeps its client size in pixels on a monitor at another scale
        if (!is_fullscreen(h) && !IsZoomed(h)) {
            RECT c, r;
            if (GetClientRect(h, &c)) {
                r = (RECT){0, 0, c.right, c.bottom};
                if (AdjustWindowRectExForDpi(&r, WND_STYLE, FALSE, 0, LOWORD(w))) {
                    *(SIZE *)l = (SIZE){r.right - r.left, r.bottom - r.top};
                    return TRUE;
                }
            }
        }
        break;   // DefWindowProc: Windows scales the size with the DPI
    case WM_DPICHANGED: {   // display scale changed, or the window reached a monitor at another scale
        const RECT *s = (const RECT *)l;
        InterlockedExchange(&g_dpi, LOWORD(w));
        rt_log("gfx: window DPI %u (%u%%)", LOWORD(w), LOWORD(w) * 100u / USER_DEFAULT_SCREEN_DPI);
        if (is_fullscreen(h)) {   // keep covering the monitor the suggested rect lies on
            MONITORINFO mi = {sizeof mi};
            if (GetMonitorInfoW(MonitorFromRect(s, MONITOR_DEFAULTTONEAREST), &mi))
                SetWindowPos(h, NULL, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            SetWindowPos(h, NULL, s->left, s->top, s->right - s->left, s->bottom - s->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_ACTIVATEAPP: {
        int in = w != 0;
        InterlockedExchange(&g_focused, in);
        oe_set_focus(in);                          // pinned to 1 by TOTK_ASSUME_FOCUS inside
        oe_push_message(OE_MSG_FOCUS_STATE_CHANGED);
        InterlockedIncrement(&g_focus_msgs);
        if (in && g_fs_pending) {
            g_fs_pending = 0;
            PostMessageW(h, WM_APP_FULLSCREEN, 0, 0);
        }
        return 0;
    }
    case WM_CLOSE:
        // The process ends with the window, as in MNG, once a save Commit still running in the background is done.
        rt_log("gfx: window closed; exiting");
        fs_save_flush();
        vk_pipeline_save();
        prof_write();   // ExitProcess runs no atexit handlers
        ExitProcess(0);
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI window_main(void *arg) {
    (void)arg;
    HINSTANCE inst = GetModuleHandleW(NULL);
    // Icon resource 1 of totk.rc (runtime/totk.ico): the title bar, the taskbar button and Alt+Tab.
    WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = wnd_proc, .hInstance = inst,
                      .hIcon = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                          GetSystemMetrics(SM_CYICON), LR_SHARED),
                      .hIconSm = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                            GetSystemMetrics(SM_CYSMICON), LR_SHARED),
                      .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .lpszClassName = L"TotKRecompWindow"};
    RegisterClassExW(&wc);
    int cw, ch, ow, oh;
    cfg_window_size(&cw, &ch);
    outer_size(NULL, cw, ch, &ow, &oh);
    // The window opens behind the user's windows without taking focus: it shows the game's blank loading frames for a
    // while, and must not cover what the player works on meanwhile. The game still counts itself in focus until the
    // first WM_ACTIVATEAPP, so loading goes on, and keyboard input waits for the foreground (input_win.c). Its taskbar
    // button flashes until it is brought up, and TOTK_FULLSCREEN waits for that first activation.
    // TOTK_BACKGROUND_WINDOW=1 (test runs): no flashing, never fullscreen at start, and F11 keeps the window's place.
    int background = g_background = cfg_get("TOTK_BACKGROUND_WINDOW") && atoi(cfg_get("TOTK_BACKGROUND_WINDOW"));
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"TotK Recomp", WND_STYLE, CW_USEDEFAULT, CW_USEDEFAULT, ow, oh, NULL,
                             NULL, inst, NULL);
    if (!g_hwnd) {
        rt_log("gfx: CreateWindowEx failed (%lu)", GetLastError());
        g_wnd_failed = 1;
    } else {
        // The frame was measured at the system DPI; the window may have opened on a monitor at another scale.
        InterlockedExchange(&g_dpi, (LONG)GetDpiForWindow(g_hwnd));
        outer_size(g_hwnd, cw, ch, &ow, &oh);
        SetWindowPos(g_hwnd, HWND_BOTTOM, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (!background) {
            FLASHWINFO fw = {.cbSize = sizeof fw, .hwnd = g_hwnd, .dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG};
            FlashWindowEx(&fw);
            g_fs_pending = cfg_fullscreen();
        }
        note_applied();
        refresh_rate_update(g_hwnd, 1);
        keep_awake(1);
    }
    SetEvent(g_wnd_ready);
    if (g_wnd_failed) return 1;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// ---- swapchain
static const char *g_present_got = "";   // the TOTK_VK_PRESENT value of the mode the swapchain got; "" before one

static VkPresentModeKHR pick_present_mode(VkPhysicalDevice pd) {
    const char *e = cfg_value("TOTK_VK_PRESENT");
    VkPresentModeKHR want = !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, g_surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, g_surface, &n, m);
    VkPresentModeKHR got = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < n; i++) if (m[i] == want) got = want;
    const char *name = got == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : got == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo";
    __atomic_store_n(&g_present_got, name, __ATOMIC_RELEASE);
    static int logged = -1;   // the mode last logged: a change from the menu shows in the log
    if ((int)got != logged) {
        logged = (int)got;
        rt_log("gfx: present mode %s (TOTK_VK_PRESENT=%s)", name, e);
    }
    return got;
}

const char *gfx_window_present_mode(void) { return __atomic_load_n(&g_present_got, __ATOMIC_ACQUIRE); }

// Queues the present of a blit vk_present_blit left waiting (g_pl held). A swapchain rebuild and the next blit flush it
// first; the device release drops it.
static void flip_locked(void) {
    if (!g_flip_on) return;
    g_flip_on = 0;
    if (!g_sc) return;
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &g_render_done[g_flip_idx], .swapchainCount = 1, .pSwapchains = &g_sc,
                           .pImageIndices = &g_flip_idx};
    vk_queue_lock();
    VkResult r = vkQueuePresentKHR(vk_ctx()->gfx_queue, &pi);
    vk_queue_unlock();
    vk_device_fault_report(r);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) InterlockedExchange(&g_dirty, 1);
}

static void free_capture(void) {
    VkCtx *c = vk_ctx();
    if (g_cap_buf && c) vmaDestroyBuffer(c->vma, g_cap_buf, g_cap_alloc);
    g_cap_buf = VK_NULL_HANDLE;
    g_cap_ptr = NULL;
    g_cap_size = 0;
    g_cap_valid = 0;
}

// Returns 1 with a usable swapchain, 0 when the window has no area (minimised). Called with g_pl held.
static int create_swapchain(void) {
    flip_locked();   // the acquired image goes out before its swapchain is replaced
    VkCtx *c = vk_ctx();
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c->phys, g_surface, &caps) != VK_SUCCESS) return 0;
    RECT rc = {0};
    GetClientRect(g_hwnd, &rc);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent
                                                            : (VkExtent2D){(uint32_t)rc.right, (uint32_t)rc.bottom};
    if (!ext.width || !ext.height) return 0;
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(c->phys, g_surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(c->phys, g_surface, &nf, fmts);
    if (!nf) return 0;
    // UNORM: the blit writes the bytes the game wrote (no sRGB encode).
    VkSurfaceFormatKHR sf = fmts[0];
    int found = 0;
    for (uint32_t i = 0; i < nf && !found; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; found = 1; }
    for (uint32_t i = 0; i < nf && !found; i++)
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; found = 1; }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;

    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    VkSwapchainKHR old = g_sc;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,   // every surface supports it (the menu overlay draws into it)
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = pick_present_mode(c->phys), .clipped = VK_TRUE,
        .oldSwapchain = old};
    VkResult r = vkCreateSwapchainKHR(c->device, &ci, NULL, &g_sc);
    if (r != VK_SUCCESS) {
        rt_log("gfx: vkCreateSwapchainKHR failed (%d)", (int)r);
        g_sc = old;
        vk_queue_unlock();
        return 0;
    }
    for (uint32_t i = 0; i < g_sc_n; i++) {
        vkDestroySemaphore(c->device, g_render_done[i], NULL);
        if (g_sc_view[i]) vkDestroyImageView(c->device, g_sc_view[i], NULL);
        g_sc_view[i] = VK_NULL_HANDLE;
    }
    if (old) vkDestroySwapchainKHR(c->device, old, NULL);
    g_sc_fmt = sf.format;
    g_sc_ext = ext;
    g_sc_n = MAX_IMAGES;
    vkGetSwapchainImagesKHR(c->device, g_sc, &g_sc_n, g_sc_img);
    VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < g_sc_n; i++) {
        vkCreateSemaphore(c->device, &semi, NULL, &g_render_done[i]);
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_sc_img[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(c->device, &vi, NULL, &g_sc_view[i]) != VK_SUCCESS) g_sc_view[i] = VK_NULL_HANDLE;
    }
    free_capture();
    vk_queue_unlock();
    InterlockedExchange(&g_dirty, 0);
    return 1;
}

static void sync_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 s0,
                         VkAccessFlags2 a0, VkPipelineStageFlags2 s1, VkAccessFlags2 a1) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = s0, .srcAccessMask = a0,
                               .dstStageMask = s1, .dstAccessMask = a1, .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

static int ensure_capture_buffer(void) {
    VkCtx *c = vk_ctx();
    VkDeviceSize need = (VkDeviceSize)g_sc_ext.width * g_sc_ext.height * 4;
    if (g_cap_buf && g_cap_size == need) return 1;
    free_capture();
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = need, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &g_cap_buf, &g_cap_alloc, &info) != VK_SUCCESS) { g_cap_buf = VK_NULL_HANDLE; return 0; }
    g_cap_ptr = info.pMappedData;
    g_cap_size = need;
    return g_cap_ptr != NULL;
}

void vk_thumbs_enable(int n) {
    EnterCriticalSection(&g_pl);
    if (!g_th_n && n > 0 && (g_th_tag = calloc((size_t)n, sizeof *g_th_tag))) g_th_n = n;
    LeaveCriticalSection(&g_pl);
}

void vk_thumbs_frame(uint64_t frame) {
    EnterCriticalSection(&g_pl);
    g_th_frame = frame;
    LeaveCriticalSection(&g_pl);
}

static void free_thumbs(void) {
    VkCtx *c = vk_ctx();
    if (g_th_img && c) vmaDestroyImage(c->vma, g_th_img, g_th_img_alloc);
    if (g_th_buf && c) vmaDestroyBuffer(c->vma, g_th_buf, g_th_alloc);
    g_th_img = VK_NULL_HANDLE, g_th_buf = VK_NULL_HANDLE, g_th_ptr = NULL;
    if (g_th_tag) memset(g_th_tag, 0, sizeof *g_th_tag * (size_t)g_th_n);
    memset(g_th_pending, 0, sizeof g_th_pending);
}

// The ring and the blit target in the swapchain's format; allocated on the first present, the image again when the
// swapchain format changes. A failure turns the thumbnails off for the session.
static int ensure_thumbs(void) {
    if (!g_th_n || g_th_failed || !g_th_frame) return 0;
    VkCtx *c = vk_ctx();
    if (!g_th_buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = TH_BYTES * (VkDeviceSize)g_th_n,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(c->vma, &bi, &ai, &g_th_buf, &g_th_alloc, &info) != VK_SUCCESS || !info.pMappedData) {
            if (g_th_buf) vmaDestroyBuffer(c->vma, g_th_buf, g_th_alloc);
            g_th_buf = VK_NULL_HANDLE;
            g_th_failed = 1;
            rt_log("gfx: flight thumbnails off: no host-visible ring of %llu MB", (unsigned long long)(bi.size >> 20));
            return 0;
        }
        g_th_ptr = info.pMappedData;
    }
    if (g_th_img && g_th_fmt != g_sc_fmt) {
        vmaDestroyImage(c->vma, g_th_img, g_th_img_alloc);   // the previous present's fence was waited for: idle
        g_th_img = VK_NULL_HANDLE;
    }
    if (!g_th_img) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = g_sc_fmt,
                                .extent = {FLIGHT_THUMB_W, FLIGHT_THUMB_H, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ci, &ai, &g_th_img, &g_th_img_alloc, NULL) != VK_SUCCESS) {
            g_th_img = VK_NULL_HANDLE;
            g_th_failed = 1;
            rt_log("gfx: flight thumbnails off: no %ux%u image", FLIGHT_THUMB_W, FLIGHT_THUMB_H);
            return 0;
        }
        g_th_fmt = g_sc_fmt;
    }
    return 1;
}

// dst (TRANSFER_SRC, the swapchain image just written) -> thumbnail image -> ring slot of g_th_frame.
static void record_thumb(VkCommandBuffer cb, VkImage dst, unsigned fence_slot) {
    uint64_t f = g_th_frame;
    unsigned ring = (unsigned)(f % (uint64_t)g_th_n);
    g_th_tag[ring] = 0;   // under g_pl: vk_thumbs_read no longer hands this slot out
    sync_barrier(cb, g_th_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit region = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int)g_sc_ext.width, (int)g_sc_ext.height, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {FLIGHT_THUMB_W, FLIGHT_THUMB_H, 1}}};
    vkCmdBlitImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_th_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   VK_FILTER_LINEAR);
    sync_barrier(cb, g_th_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy bc = {.bufferOffset = TH_BYTES * ring, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {FLIGHT_THUMB_W, FLIGHT_THUMB_H, 1}};
    vkCmdCopyImageToBuffer(cb, g_th_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_th_buf, 1, &bc);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &d);
    g_th_pending[fence_slot] = f;
    g_th_frame = 0;
}

int vk_thumbs_read(uint64_t frame, uint8_t *rgba) {
    int ok = 0;
    EnterCriticalSection(&g_pl);
    if (g_th_n && g_th_ptr && frame) {
        unsigned ring = (unsigned)(frame % (uint64_t)g_th_n);
        if (g_th_tag[ring] == frame) {
            VkCtx *c = vk_ctx();
            vmaInvalidateAllocation(c->vma, g_th_alloc, TH_BYTES * ring, TH_BYTES);
            memcpy(rgba, g_th_ptr + TH_BYTES * ring, TH_BYTES);
            ok = g_th_fmt == VK_FORMAT_B8G8R8A8_UNORM || g_th_fmt == VK_FORMAT_B8G8R8A8_SRGB ? 2 : 1;
        }
    }
    LeaveCriticalSection(&g_pl);
    if (ok == 2)
        for (size_t i = 0; i < TH_BYTES; i += 4) {
            uint8_t t = rgba[i];
            rgba[i] = rgba[i + 2], rgba[i + 2] = t;
        }
    return ok != 0;
}

void vk_present_capture(int on) {
    EnterCriticalSection(&g_pl);
    g_capture = on;
    if (!on) g_cap_valid = 0;
    LeaveCriticalSection(&g_pl);
}

int vk_present_captured(const uint8_t **px, int *w, int *h, VkFormat *fmt) {
    EnterCriticalSection(&g_pl);
    int ok = g_cap_valid;
    if (ok) { *px = g_cap_ptr; *w = g_cap_w; *h = g_cap_h; *fmt = g_sc_fmt; }
    LeaveCriticalSection(&g_pl);
    return ok;
}

// The captured swapchain image as a PNG (the menu's test hook), RGBA with opaque alpha.
static void save_capture(const char *path) {
    size_t n = (size_t)g_cap_w * (size_t)g_cap_h * 4;
    uint8_t *px = malloc(n);
    if (!px) return;
    int bgra = g_sc_fmt == VK_FORMAT_B8G8R8A8_UNORM || g_sc_fmt == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0; i < n; i += 4) {
        px[i] = g_cap_ptr[i + (bgra ? 2 : 0)];
        px[i + 1] = g_cap_ptr[i + 1];
        px[i + 2] = g_cap_ptr[i + (bgra ? 0 : 2)];
        px[i + 3] = 255;
    }
    rt_write_png(path, px, (uint32_t)g_cap_w, (uint32_t)g_cap_h);
    free(px);
    rt_log("gfx: menu test shot %s (%dx%d)", path, g_cap_w, g_cap_h);
}

// Where the game's picture (cw x ch) goes on the sw x sh swapchain: TOTK_ASPECT=fit (default) keeps its shape and centres
// it, with black bars at the sides (a 16:9 picture in a 21:9 window) or above and below; stretch fills the swapchain.
// Bars under a pixel are not drawn. Returns 1 when the rectangle leaves bars to clear.
static int fit_rect(int cw, int ch, int sw, int sh, int out[4]) {
    out[0] = 0, out[1] = 0, out[2] = sw, out[3] = sh;
    if (cw <= 0 || ch <= 0 || strcmp(cfg_value("TOTK_ASPECT"), "fit")) return 0;
    long long a = (long long)cw * sh, b = (long long)ch * sw;
    if (a < b) {   // narrower than the window: bars at the sides
        int fw = (int)((a + ch / 2) / ch);
        if (fw >= sw - 1) return 0;
        out[0] = (sw - fw) / 2, out[2] = fw;
    } else if (a > b) {   // wider: bars above and below
        int fh = (int)(((long long)ch * sw + cw / 2) / cw);
        if (fh >= sh - 1) return 0;
        out[1] = (sh - fh) / 2, out[3] = fh;
    } else {
        return 0;
    }
    return 1;
}

static void present_image(VkImage src, VkImageLayout layout, VkFormat fmt, int w, int h, const int crop[4], int defer) {
    int general = layout == VK_IMAGE_LAYOUT_GENERAL;
    gfx_window_start();
    VkCtx *c = vk_ctx();
    EnterCriticalSection(&g_pl);
    flip_locked();
    if (!g_sc || InterlockedCompareExchange(&g_dirty, 0, 0)) {
        if (!IsIconic(g_hwnd) && !create_swapchain() && !g_sc) goto out;
    }
    if (IsIconic(g_hwnd) || !g_sc) goto out;

    unsigned slot = g_frame % FRAMES;
    if (vkWaitForFences(c->device, 1, &g_fence[slot], VK_TRUE, 5000000000ull) != VK_SUCCESS) {
        rt_log("gfx: present fence wait timed out");
        goto out;
    }
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(c->device, g_sc, 1000000000ull, g_acquire[slot], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        InterlockedExchange(&g_dirty, 1);
        if (!create_swapchain()) goto out;
        r = vkAcquireNextImageKHR(c->device, g_sc, 1000000000ull, g_acquire[slot], VK_NULL_HANDLE, &idx);
    }
    vk_device_fault_report(r);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) goto out;     // lost, not ready: skip the frame
    if (r == VK_SUBOPTIMAL_KHR) InterlockedExchange(&g_dirty, 1);
    vkResetFences(c->device, 1, &g_fence[slot]);
    if (g_th_pending[slot]) {   // that fence has signalled: the thumbnail its submission wrote is complete
        g_th_tag[g_th_pending[slot] % (uint64_t)g_th_n] = g_th_pending[slot];
        g_th_pending[slot] = 0;
    }

    int cx = 0, cy = 0, cw = w, ch = h;
    if (crop && crop[2] > 0 && crop[3] > 0) { cx = crop[0]; cy = crop[1]; cw = crop[2]; ch = crop[3]; }
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx + cw > w) cw = w - cx;
    if (cy + ch > h) ch = h - cy;
    window_settings_live();
    // The menu overlay: laid out before recording (its first frame starts the ImGui back end and uploads the font)
    const char *menu_shot = NULL;
    MenuTarget mt = {c->instance, c->phys, c->device, c->gfx_family, c->gfx_queue, g_sc_fmt, g_sc_ext.width,
                     g_sc_ext.height, cw, ch, g_scaler};
    int overlay = g_sc_view[idx] &&
                  menu_overlay_frame(__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), &mt, &menu_shot);
    int cap = (g_capture || menu_shot) && ensure_capture_buffer();
    int thumb = g_th_n && ensure_thumbs();

    VkCommandBuffer cb = g_cb[slot];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    uint64_t crumb = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    vk_crumb(cb, VK_CRUMB_PRESENT, crumb);
    VkImage dst = g_sc_img[idx];
    sync_barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const int rect[4] = {cx, cy, cw, ch};
    int to[4];
    if (fit_rect(cw, ch, (int)g_sc_ext.width, (int)g_sc_ext.height, to)) {
        // black bars: clear the whole image, then the picture's writes below wait for the clear
        VkClearColorValue black = {{0, 0, 0, 1}};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
        sync_barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    // FSR (TOTK_UPSCALER) when the picture's place on the swapchain is larger than the crop; else the bilinear blit
    int fsr = general && vk_fsr_record(cb, slot, src, fmt, w, h, rect, dst, to);
    g_scaler = fsr ? 2 : cw != to[2] || ch != to[3];
    if (!fsr) {
        if (general)   // a GENERAL source (vk_texture.c images): earlier submissions' writes become visible to the blit here
            sync_barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkImageBlit region = {
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{cx, cy, 0}, {cx + cw, cy + ch, 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{to[0], to[1], 0}, {to[0] + to[2], to[1] + to[3], 1}}};
        vkCmdBlitImage(cb, src, general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
        if (general)   // later submissions that write the source wait for the blit's read
            sync_barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
    }
    // last: the image's layout, and the stage and access of its last write, as each step leaves it
    VkImageLayout last = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkPipelineStageFlags2 last_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    VkAccessFlags2 last_write = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (overlay) {
        sync_barrier(cb, dst, last, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, last_stage, last_write,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_sc_view[idx],
                                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_sc_ext}, .layerCount = 1,
                              .colorAttachmentCount = 1, .pColorAttachments = &ca};
        vkCmdBeginRendering(cb, &ri);
        menu_overlay_draw(cb);
        vkCmdEndRendering(cb);
        last = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        last_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        last_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if (cap) {
        sync_barrier(cb, dst, last, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, last_stage, last_write,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageExtent = {g_sc_ext.width, g_sc_ext.height, 1}};
        vkCmdCopyImageToBuffer(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_cap_buf, 1, &bc);
        last = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        last_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        last_write = 0;
    }
    if (thumb) {
        if (last != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
            sync_barrier(cb, dst, last, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, last_stage, last_write,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        last = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        last_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        last_write = 0;
        record_thumb(cb, dst, slot);
    }
    sync_barrier(cb, dst, last, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, last_stage, last_write, VK_PIPELINE_STAGE_2_NONE, 0);
    vk_crumb(cb, VK_CRUMB_PRESENT_END, crumb);
    vkEndCommandBuffer(cb);

    VkSemaphoreSubmitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_acquire[slot],
                                  .stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT};
    VkSemaphoreSubmitInfo sig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_render_done[idx],
                                 .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkCommandBufferSubmitInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = 1, .pWaitSemaphoreInfos = &wait,
                        .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi, .signalSemaphoreInfoCount = 1,
                        .pSignalSemaphoreInfos = &sig};
    vk_queue_lock();
    r = vkQueueSubmit2(c->gfx_queue, 1, &si, g_fence[slot]);
    vk_queue_unlock();
    if (r == VK_SUCCESS) {
        g_flip_on = 1;
        g_flip_idx = idx;
        if (!defer) flip_locked();
    } else {
        vk_device_fault_report(r);
        rt_log("gfx: present submit failed (%d)", (int)r);
    }
    g_frame++;
    if (cap) {
        vkWaitForFences(c->device, 1, &g_fence[slot], VK_TRUE, 5000000000ull);
        vmaInvalidateAllocation(c->vma, g_cap_alloc, 0, VK_WHOLE_SIZE);
        g_cap_w = (int)g_sc_ext.width;
        g_cap_h = (int)g_sc_ext.height;
        g_cap_valid = 1;
        if (menu_shot) save_capture(menu_shot);
    }
out:
    LeaveCriticalSection(&g_pl);
}

void vk_present_image(VkImage src, VkImageLayout layout, VkFormat fmt, int w, int h, const int crop[4]) {
    present_image(src, layout, fmt, w, h, crop, 0);
}

void vk_present_blit(VkImage src, VkImageLayout layout, VkFormat fmt, int w, int h, const int crop[4]) {
    present_image(src, layout, fmt, w, h, crop, 1);
}

void vk_present_flip(void) {
    EnterCriticalSection(&g_pl);
    flip_locked();
    LeaveCriticalSection(&g_pl);
}

void gfx_window_resize(int cw, int chh) {
    gfx_window_start();
    int ow, oh;
    outer_size(g_hwnd, cw, chh, &ow, &oh);
    SetWindowPos(g_hwnd, NULL, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    EnterCriticalSection(&g_pl);
    create_swapchain();
    LeaveCriticalSection(&g_pl);
}

// Called by vk_shutdown before the device goes away; the window and its thread stay.
void gfx_window_release_vk(void) {
    VkCtx *c = vk_ctx();
    EnterCriticalSection(&g_pl);
    if (c && g_started) {
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        g_flip_on = 0;   // its blit is done (device idle); the swapchain goes away unpresented
        menu_vk_release();
        vk_fsr_release();
        free_capture();
        free_thumbs();
        for (int i = 0; i < FRAMES; i++) {
            vkDestroySemaphore(c->device, g_acquire[i], NULL);
            vkDestroyFence(c->device, g_fence[i], NULL);
        }
        for (uint32_t i = 0; i < g_sc_n; i++) {
            vkDestroySemaphore(c->device, g_render_done[i], NULL);
            if (g_sc_view[i]) vkDestroyImageView(c->device, g_sc_view[i], NULL);
            g_sc_view[i] = VK_NULL_HANDLE;
        }
        if (g_pool) vkDestroyCommandPool(c->device, g_pool, NULL);
        if (g_sc) vkDestroySwapchainKHR(c->device, g_sc, NULL);
        if (g_surface) vkDestroySurfaceKHR(c->instance, g_surface, NULL);
        g_sc = VK_NULL_HANDLE; g_surface = VK_NULL_HANDLE; g_pool = VK_NULL_HANDLE; g_sc_n = 0;
        g_started = 0;     // the window and its thread stay; the next gfx_window_start makes a new surface
    }
    LeaveCriticalSection(&g_pl);
}

void gfx_window_start(void) {
    EnterCriticalSection(&g_start_lock);
    if (!g_started) {
        if (!vk_init()) rt_fatal("gfx: no usable Vulkan device (see the vk: lines above)");
        VkCtx *c = vk_ctx();
        if (!g_hwnd) {
            g_wnd_failed = 0;
            g_wnd_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
            HANDLE t = CreateThread(NULL, 0, window_main, NULL, 0, NULL);
            if (!t) rt_fatal("gfx: cannot start the window thread (error %lu)", GetLastError());
            CloseHandle(t);
            WaitForSingleObject(g_wnd_ready, INFINITE);
            CloseHandle(g_wnd_ready);
            if (g_wnd_failed) rt_fatal("gfx: window creation failed");
        }
        VkWin32SurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                          .hinstance = GetModuleHandleW(NULL), .hwnd = g_hwnd};
        VkResult vr = vkCreateWin32SurfaceKHR(c->instance, &si, NULL, &g_surface);
        if (vr != VK_SUCCESS) rt_fatal("gfx: vkCreateWin32SurfaceKHR failed (%d)", (int)vr);

        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = c->gfx_family};
        vkCreateCommandPool(c->device, &pci, NULL, &g_pool);
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = FRAMES};
        vkAllocateCommandBuffers(c->device, &ai, g_cb);
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (int i = 0; i < FRAMES; i++) {
            vkCreateFence(c->device, &fi, NULL, &g_fence[i]);
            vkCreateSemaphore(c->device, &semi, NULL, &g_acquire[i]);
        }
        g_frame = 0;
        g_started = 1;
        EnterCriticalSection(&g_pl);
        create_swapchain();
        LeaveCriticalSection(&g_pl);
        RECT rc = {0};
        GetClientRect(g_hwnd, &rc);
        rt_log("gfx: window %ldx%ld at DPI %u, swapchain %ux%u format %d", rc.right, rc.bottom, gfx_window_dpi(),
               g_sc_ext.width, g_sc_ext.height, (int)g_sc_fmt);
    }
    LeaveCriticalSection(&g_start_lock);
    vsync_ensure();
}
