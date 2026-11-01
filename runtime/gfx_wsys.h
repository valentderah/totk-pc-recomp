// Between gfx_present.c (swapchain, present, vsync) and the window system's back end (gfx_window_win.c with Win32,
// gfx_window_sdl.c with SDL). Host threads only.
#pragma once
#include "vk.h"

// ---- what the back end provides
int wsys_open(void);                       // makes the window (and its thread); 0 on failure. Once.
VkSurfaceKHR wsys_surface(VkInstance inst);   // a new Vulkan surface of the window; VK_NULL_HANDLE on failure
void wsys_client_size(int *w, int *h);     // the drawable area in pixels
int wsys_minimized(void);                  // nothing to present into
void wsys_resize(int w, int h);            // gfx_window_resize: the drawable area becomes w x h
// --probe-window (gfx_window_probe.c): synthetic window events, handled on the window thread before these return.
void wsys_probe_focus(int in);             // the window gains (1) or loses (0) the focus
void wsys_probe_escape(void);              // Esc pressed
// A left click: 0 in the client area, 1 activating the window from its client area, 2 activating it from the title bar.
// 0 when the window system has no such event (then the caller skips that check).
int wsys_probe_click(int where);

// ---- what gfx_present.c provides the back end
void gfx_window_mark_dirty(void);          // the window changed size: the next present rebuilds the swapchain
void gfx_window_focus_changed(int in);     // the window gained (1) or lost (0) the focus
void gfx_window_closed(void);              // the player closed the window: saves, then ends the process
void gfx_window_set_refresh(double hz);    // the refresh rate of the window's monitor, 0 unknown
int gfx_cfg_fullscreen(void);              // TOTK_FULLSCREEN
void gfx_cfg_window_size(int *w, int *h);  // TOTK_WINDOW_SIZE, the default when unparsable or out of range
