#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "uiwindow.h"

#define UI_CLOSE (WM_APP + 21)
#define UI_TITLE (WM_APP + 22)
#define UI_REPAINT (WM_APP + 23)

struct UiWindow {
    HANDLE thread, ready;
    DWORD tid;
    HWND hwnd;
    volatile LONG open, user_closed;
    SRWLOCK title_lock;
    wchar_t title[256];
    int w, h, hz;
    UiWindowDrawFn draw;
    void *user;
    HDC back;
    HBITMAP bitmap, original;
    int bw, bh;
    HFONT font;
    UINT dpi;
    int drag;
    POINT drag_start;
    RECT drag_rect;
    ULONGLONG topmost_at;
};

static const wchar_t class_name[] = L"TotkAuxiliaryPanel";
static INIT_ONCE class_once = INIT_ONCE_STATIC_INIT;

static HWND window_handle(const UiWindow *w) { return __atomic_load_n(&w->hwnd, __ATOMIC_ACQUIRE); }
static void title_copy(UiWindow *w, const char *s) {
    wchar_t text[256];
    if (!MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, text, 256)) text[0] = 0;
    AcquireSRWLockExclusive(&w->title_lock);
    memcpy(w->title, text, sizeof text);
    ReleaseSRWLockExclusive(&w->title_lock);
}

static void back_free(UiWindow *w) {
    if (w->back) {
        if (w->original) SelectObject(w->back, w->original);
        if (w->bitmap) DeleteObject(w->bitmap);
        DeleteDC(w->back);
    }
    w->back = NULL; w->bitmap = w->original = NULL; w->bw = w->bh = 0;
}

static void paint(UiWindow *w, HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT r; GetClientRect(hwnd, &r);
    int width = r.right, height = r.bottom;
    if (width > 0 && height > 0) {
        if (width != w->bw || height != w->bh) {
            back_free(w);
            w->back = CreateCompatibleDC(dc);
            if (w->back) w->bitmap = CreateCompatibleBitmap(dc, width, height);
            if (w->bitmap) {
                w->original = SelectObject(w->back, w->bitmap);
                w->bw = width; w->bh = height;
                FillRect(w->back, &r, GetStockObject(BLACK_BRUSH));
            } else back_free(w);
        }
        if (w->back) {
            int saved = SaveDC(w->back);
            SelectObject(w->back, w->font ? w->font : GetStockObject(DEFAULT_GUI_FONT));
            SetBkMode(w->back, TRANSPARENT);
            SetTextColor(w->back, RGB(235, 238, 242));
            w->draw(w->user, w->back, width, height);
            if (saved) RestoreDC(w->back, saved);
            BitBlt(dc, 0, 0, width, height, w->back, 0, 0, SRCCOPY);
        } else FillRect(dc, &r, GetStockObject(BLACK_BRUSH));
    }
    EndPaint(hwnd, &ps);
}

static void font_update(UiWindow *w, UINT dpi) {
    HFONT font = CreateFontW(-MulDiv(14, dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (w->font) DeleteObject(w->font);
    w->font = font; w->dpi = dpi;
}

// Handle non-client dragging and sizing directly to avoid DefWindowProc's potentially activating modal loop.
static void drag_move(UiWindow *w, HWND hwnd) {
    POINT p; GetCursorPos(&p);
    int dx = p.x - w->drag_start.x, dy = p.y - w->drag_start.y;
    RECT r = w->drag_rect;
    if (w->drag == HTCAPTION) OffsetRect(&r, dx, dy);
    else {
        if (w->drag == HTLEFT || w->drag == HTTOPLEFT || w->drag == HTBOTTOMLEFT) r.left += dx;
        if (w->drag == HTRIGHT || w->drag == HTTOPRIGHT || w->drag == HTBOTTOMRIGHT) r.right += dx;
        if (w->drag == HTTOP || w->drag == HTTOPLEFT || w->drag == HTTOPRIGHT) r.top += dy;
        if (w->drag == HTBOTTOM || w->drag == HTBOTTOMLEFT || w->drag == HTBOTTOMRIGHT) r.bottom += dy;
        int minw = MulDiv(280, w->dpi, 96), minh = MulDiv(220, w->dpi, 96);
        if (r.right - r.left < minw) {
            if (w->drag == HTLEFT || w->drag == HTTOPLEFT || w->drag == HTBOTTOMLEFT) r.left = r.right - minw;
            else r.right = r.left + minw;
        }
        if (r.bottom - r.top < minh) {
            if (w->drag == HTTOP || w->drag == HTTOPLEFT || w->drag == HTTOPRIGHT) r.top = r.bottom - minh;
            else r.bottom = r.top + minh;
        }
    }
    SetWindowPos(hwnd, HWND_TOPMOST, r.left, r.top, r.right-r.left, r.bottom-r.top, SWP_NOACTIVATE);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    UiWindow *w = (UiWindow *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        w = ((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)w);
    }
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_NCLBUTTONDBLCLK: return 0;
    case WM_NCLBUTTONDOWN:
        if (wp == HTCLOSE) { PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
        if (wp == HTCAPTION || (wp >= HTLEFT && wp <= HTBOTTOMRIGHT)) {
            w->drag = (int)wp; GetCursorPos(&w->drag_start); GetWindowRect(hwnd, &w->drag_rect);
            SetCapture(hwnd); return 0;
        }
        return 0;
    case WM_MOUSEMOVE: if (w->drag) drag_move(w, hwnd); return 0;
    case WM_LBUTTONUP: if (w->drag) { w->drag = 0; ReleaseCapture(); } return 0;
    case WM_CAPTURECHANGED: w->drag = 0; return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_CLOSE) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_DPICHANGED: {
        font_update(w, HIWORD(wp));
        RECT *r = (RECT *)lp;
        SetWindowPos(hwnd, HWND_TOPMOST, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOACTIVATE);
        if (w->drag) {
            // Continue caption or border dragging from the geometry and cursor after the DPI transition.
            GetWindowRect(hwnd, &w->drag_rect);
            GetCursorPos(&w->drag_start);
        }
        InvalidateRect(hwnd, NULL, FALSE); return 0;
    }
    case WM_SIZE: InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_TIMER:
        if (GetTickCount64() - w->topmost_at >= 1000) {
            w->topmost_at = GetTickCount64();
            // Reassert a lost state, not our position among other topmost panels on every redraw.
            if (!(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST))
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        InvalidateRect(hwnd, NULL, FALSE); return 0;
    case UI_REPAINT: InvalidateRect(hwnd, NULL, FALSE); return 0;
    case UI_TITLE: {
        wchar_t title[256];
        AcquireSRWLockShared(&w->title_lock); memcpy(title, w->title, sizeof title); ReleaseSRWLockShared(&w->title_lock);
        SetWindowTextW(hwnd, title); return 0;
    }
    case WM_PAINT: paint(w, hwnd); return 0;
    case WM_CLOSE: InterlockedExchange(&w->user_closed, 1); DestroyWindow(hwnd); return 0;
    case UI_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1); InterlockedExchange(&w->open, 0);
        __atomic_store_n(&w->hwnd, NULL, __ATOMIC_RELEASE); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static BOOL CALLBACK register_class(PINIT_ONCE once, PVOID arg, PVOID *ctx) {
    (void)once; (void)arg; (void)ctx;
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = window_proc; wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); wc.lpszClassName = class_name;
    return RegisterClassW(&wc) != 0;
}

static DWORD WINAPI window_thread(void *arg) {
    UiWindow *w = arg;
    DPI_AWARENESS_CONTEXT old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!InitOnceExecuteOnce(&class_once, register_class, NULL, NULL)) { SetEvent(w->ready); return 0; }
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
    DWORD ex = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
    UINT dpi = GetDpiForSystem();
    RECT r = {0, 0, MulDiv(w->w, dpi, 96), MulDiv(w->h, dpi, 96)};
    AdjustWindowRectExForDpi(&r, style, FALSE, ex, dpi);
    HWND hwnd = CreateWindowExW(ex, class_name, w->title, style, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right-r.left, r.bottom-r.top, NULL, NULL, GetModuleHandleW(NULL), w);
    if (hwnd) {
        font_update(w, GetDpiForWindow(hwnd));
        if (!SetTimer(hwnd, 1, (UINT)(1000 / w->hz), NULL)) { DestroyWindow(hwnd); hwnd = NULL; }
    }
    if (hwnd) {
        __atomic_store_n(&w->hwnd, hwnd, __ATOMIC_RELEASE);
        InterlockedExchange(&w->open, 1);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
    SetEvent(w->ready);
    if (hwnd) {
        MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (IsWindow(hwnd)) DestroyWindow(hwnd);
    }
    back_free(w);
    if (w->font) DeleteObject(w->font);
    InterlockedExchange(&w->open, 0);
    if (old) SetThreadDpiAwarenessContext(old);
    return 0;
}

UiWindow *uiwindow_open(const char *title, int width, int height, int hz, UiWindowDrawFn draw, void *user) {
    if (!draw || width <= 0 || height <= 0 || hz <= 0 || hz > 1000) return NULL;
    UiWindow *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->w = width; w->h = height; w->hz = hz; w->draw = draw; w->user = user;
    title_copy(w, title);
    w->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (w->ready) w->thread = CreateThread(NULL, 0, window_thread, w, 0, &w->tid);
    if (!w->thread) { if (w->ready) CloseHandle(w->ready); free(w); return NULL; }
    WaitForSingleObject(w->ready, INFINITE);
    CloseHandle(w->ready); w->ready = NULL;
    if (!uiwindow_is_open(w)) { uiwindow_close(w); return NULL; }
    return w;
}
void uiwindow_close(UiWindow *w) {
    if (!w) return;
    HWND hwnd = window_handle(w);
    if (hwnd && !PostMessageW(hwnd, UI_CLOSE, 0, 0)) PostThreadMessageW(w->tid, WM_QUIT, 0, 0);
    WaitForSingleObject(w->thread, INFINITE);
    CloseHandle(w->thread); free(w);
}
int uiwindow_is_open(const UiWindow *w) { return w && InterlockedCompareExchange((LONG *)&w->open, 0, 0); }
int uiwindow_take_close_requested(UiWindow *w) { return w && InterlockedExchange(&w->user_closed, 0); }
void uiwindow_repaint(UiWindow *w) { HWND hwnd = w ? window_handle(w) : NULL; if (hwnd) PostMessageW(hwnd, UI_REPAINT, 0, 0); }
void uiwindow_set_title(UiWindow *w, const char *title) {
    if (!w) return;
    title_copy(w, title);
    HWND hwnd = window_handle(w);
    if (hwnd) PostMessageW(hwnd, UI_TITLE, 0, 0);
}
