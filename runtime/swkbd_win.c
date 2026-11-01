// The Win32 front end of the software keyboard (swkbd.h): a modal window owned by the game window, run on the guest
// thread that called. The text starts as the game's initial text, else a default name from the catalog (ui_text.c),
// selected, so a controller can confirm it: A = OK, B = cancel (each after it was seen released); Enter / Esc and the
// buttons work too.
//   TOTK_SWKBD_TEST=ok|cancel   open the window, then press OK / Cancel on the first timer tick
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "input_win.h"
#include "swkbd.h"
#include "ui_text.h"
#include "vk.h"

enum { B_A = 0, B_B = 1, B_PLUS = 10 };   // input.h bits

#define TIMER_ID 1
#define TIMER_MS 16
typedef struct {
    const SwkbdRequest *req;
    HWND wnd, edit;
    WNDPROC edit_proc;
    uint16_t text[KB_LIMIT * 2 + 1];
    size_t n;
    int result;                          // 0 open, 1 OK, -1 cancelled
    uint64_t armed;                      // buttons seen released since the window opened
    int test;                            // TOTK_SWKBD_TEST: 1 press OK, -1 press Cancel
} Entry;

static wchar_t *wide(const char *utf8, wchar_t *buf, int cap) {
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buf, cap)) buf[0] = 0;
    return buf;
}

static void finish(Entry *e, int result) {
    if (e->result) return;
    if (result > 0) {
        int n = GetWindowTextW(e->edit, (wchar_t *)e->text, KB_LIMIT * 2 + 1);
        e->n = n > 0 ? (size_t)n : 0;
    }
    e->result = result;
}

static LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Entry *e = (Entry *)GetWindowLongPtrW(GetParent(h), GWLP_USERDATA);
    if (m == WM_CHAR && w >= 0x20 && !(w >= 0xd800 && w < 0xe000) &&
        !swkbd_allowed((uint16_t)w, e->req->disabled, e->req->numeric)) {
        MessageBeep(MB_OK);
        return 0;
    }
    return CallWindowProcW(e->edit_proc, h, m, w, l);
}

static LRESULT CALLBACK entry_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Entry *e = (Entry *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) finish(e, 1);
        else if (LOWORD(w) == IDCANCEL) finish(e, -1);
        return 0;
    case WM_CLOSE:
        finish(e, -1);
        return 0;
    case WM_TIMER: {
        if (e->test) { finish(e, e->test); return 0; }
        uint64_t b;
        int32_t lx, ly, rx, ry;
        input_poll_ui(&b, &lx, &ly, &rx, &ry);
        uint64_t pressed = b & e->armed;   // down now, up at the previous tick
        e->armed = ~b;
        if (pressed & (1ull << B_A | 1ull << B_PLUS)) finish(e, 1);
        else if (pressed & 1ull << B_B) finish(e, -1);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

static HWND child(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id,
                  HFONT font) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent, (HMENU)(intptr_t)id,
                             GetModuleHandleW(NULL), NULL);
    if (c) SendMessageW(c, WM_SETFONT, (WPARAM)font, FALSE);
    return c;
}

// Shows the window and waits for the answer. Returns 1 with the text in out (n units), 0 when cancelled.
int text_entry_run(const SwkbdRequest *req, uint16_t *out, size_t cap, size_t *n) {
    static const wchar_t cls[] = L"TotKTextEntry";
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = entry_proc, .hInstance = inst,
                      .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1),
                      .lpszClassName = cls};
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        rt_fatal("nn::swkbd::ShowKeyboard: RegisterClassEx (error %lu)", GetLastError());

    Entry e = {.req = req, .armed = 0};
    const char *test = cfg_get("TOTK_SWKBD_TEST");
    if (test && *test) e.test = !strcmp(test, "cancel") ? -1 : 1;
    // The process is per-monitor DPI aware: the font and the layout below are measured at the game window's DPI, the
    // monitor the dialog opens on, not at the system DPI that plain SystemParametersInfo and AdjustWindowRectEx use.
    HWND owner = gfx_window_handle();
    UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    NONCLIENTMETRICSW ncm = {.cbSize = sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    HDC dc = GetDC(NULL);
    HGDIOBJ old = SelectObject(dc, font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    int u = tm.tmHeight > 8 ? tm.tmHeight : 16, pad = u, cw = 26 * u, bw = 6 * u, bh = u * 9 / 5;
    int y_prompt = pad, h_prompt = u * 5 / 2, y_edit = y_prompt + h_prompt + u / 4, h_edit = u * 9 / 5;
    int y_hint = y_edit + h_edit + u / 2, y_btn = y_hint + u * 3 / 2, ch = y_btn + bh + pad;

    RECT wr = {0, 0, cw + 2 * pad, ch};
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    AdjustWindowRectExForDpi(&wr, style, FALSE, ex, dpi);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top, x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    RECT orc;
    if (owner && GetWindowRect(owner, &orc)) {
        x = orc.left + (orc.right - orc.left - ww) / 2;
        y = orc.top + (orc.bottom - orc.top - wh) / 2;
    }
    wchar_t title[128], ok[64], cancel[64], hint[160];
    e.wnd = CreateWindowExW(ex, cls, wide(ui_text(UI_SWKBD_TITLE), title, 128), style, x, y, ww, wh, owner, NULL, inst,
                            NULL);
    if (!e.wnd) rt_fatal("nn::swkbd::ShowKeyboard: CreateWindowEx (error %lu)", GetLastError());
    SetWindowLongPtrW(e.wnd, GWLP_USERDATA, (LONG_PTR)&e);
    wchar_t prompt[0x102];
    if (req->prompt[0]) memcpy(prompt, req->prompt, sizeof req->prompt), prompt[0x101] = 0;
    else wide(ui_text(UI_SWKBD_PROMPT), prompt, 0x102);
    child(e.wnd, L"STATIC", prompt, SS_LEFT | SS_NOPREFIX, pad, y_prompt, cw, h_prompt, -1, font);
    e.edit = child(e.wnd, L"EDIT", (const wchar_t *)req->initial,
                   WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | (req->password ? ES_PASSWORD : 0) | (req->numeric ? ES_NUMBER : 0),
                   pad, y_edit, cw, h_edit, 100, font);
    child(e.wnd, L"STATIC", wide(ui_text(UI_SWKBD_PAD_HINT), hint, 160), SS_LEFT | SS_NOPREFIX, pad, y_hint, cw, u * 3 / 2,
          -1, font);
    child(e.wnd, L"BUTTON", wide(ui_text(UI_SWKBD_OK), ok, 64), WS_TABSTOP | BS_DEFPUSHBUTTON, pad + cw - 2 * bw - u / 2,
          y_btn, bw, bh, IDOK, font);
    child(e.wnd, L"BUTTON", wide(ui_text(UI_SWKBD_CANCEL), cancel, 64), WS_TABSTOP | BS_PUSHBUTTON, pad + cw - bw, y_btn,
          bw, bh, IDCANCEL, font);
    SendMessageW(e.edit, EM_LIMITTEXT, (WPARAM)req->max_len, 0);
    e.edit_proc = (WNDPROC)SetWindowLongPtrW(e.edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);

    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(e.wnd, SW_SHOW);
    SetForegroundWindow(e.wnd);
    SetFocus(e.edit);
    SendMessageW(e.edit, EM_SETSEL, 0, -1);
    SetTimer(e.wnd, TIMER_ID, TIMER_MS, NULL);
    MSG msg;
    int quit = 0;
    while (!e.result) {
        BOOL got = GetMessageW(&msg, NULL, 0, 0);
        if (got <= 0) { quit = got == 0; finish(&e, -1); break; }
        if (msg.message == WM_KEYDOWN && (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE) &&
            (msg.hwnd == e.wnd || IsChild(e.wnd, msg.hwnd))) {
            // Enter on the Cancel button cancels; elsewhere it confirms
            finish(&e, msg.wParam == VK_ESCAPE || GetDlgCtrlID(msg.hwnd) == IDCANCEL ? -1 : 1);
            continue;
        }
        if (IsDialogMessageW(e.wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    KillTimer(e.wnd, TIMER_ID);
    if (owner) EnableWindow(owner, TRUE);   // before the window goes, so activation returns to the game window
    DestroyWindow(e.wnd);
    if (owner) SetForegroundWindow(owner);
    DeleteObject(font);
    if (quit) PostQuitMessage((int)msg.wParam);
    if (e.result < 0) return 0;
    *n = e.n < cap ? e.n : cap;
    memcpy(out, e.text, *n * 2);
    return 1;
}

