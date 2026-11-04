// Standalone Win32 lifecycle and focus probe without the game, Vulkan, SDL or user settings.
// clang -O2 -Wall -Wextra -Werror -D_WIN32_WINNT=0x0A00 -I runtime tests/uiwindow_probe.c runtime/uiwindow_win.c -luser32 -lgdi32 -o uiwindow_probe.exe
#include <windows.h>
#include <stdio.h>
#include "uiwindow.h"

static int failures;
#define CHECK(test, name) do { if (!(test)) { printf("FAIL %s\n", name); failures++; } else printf("PASS %s\n", name); } while (0)
typedef struct { volatile LONG calls, width, height, tid; } ProbeDrawState;
static void draw(void *arg, void *rawdc, int w, int h) {
    ProbeDrawState *s = arg;
    InterlockedExchange(&s->width,w); InterlockedExchange(&s->height,h);
    InterlockedExchange(&s->tid, (LONG)GetCurrentThreadId()); InterlockedIncrement(&s->calls);
    RECT r = {0,0,w,h}; FillRect(rawdc,&r,GetStockObject(DKGRAY_BRUSH));
    TextOutW(rawdc,12,12,L"Standalone panel probe",22);
}
static void pump(int ms) {
    ULONGLONG end = GetTickCount64() + ms;
    do { MSG m; while (PeekMessageW(&m,NULL,0,0,PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } Sleep(2); } while (GetTickCount64()<end);
}
static void mouse_flag(DWORD flags) {
    INPUT i = {0}; i.type=INPUT_MOUSE; i.mi.dwFlags=flags;
    CHECK(SendInput(1,&i,sizeof i)==1,"SendInput");
}
static void gesture(HWND hwnd, int hit, int dx, int dy, HWND foreground) {
    RECT r; GetWindowRect(hwnd,&r);
    int x = hit==HTCAPTION ? r.left+80 : r.right-2;
    int y = hit==HTCAPTION ? r.top+12 : r.bottom-2;
    SetCursorPos(x,y); pump(30); mouse_flag(MOUSEEVENTF_LEFTDOWN); pump(30);
    SetCursorPos(x+dx,y+dy); pump(100); mouse_flag(MOUSEEVENTF_LEFTUP); pump(60);
    RECT after; GetWindowRect(hwnd,&after);
    CHECK(GetForegroundWindow()==foreground,"gesture keeps foreground");
    if(hit==HTCAPTION) CHECK(after.left!=r.left || after.top!=r.top,"native caption drag moves");
    else CHECK(after.right-after.left!=r.right-r.left || after.bottom-after.top!=r.bottom-r.top,"native border drag resizes");
}
int main(void) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND prior = GetForegroundWindow(); POINT cursor; GetCursorPos(&cursor);
    WNDCLASSW wc={0}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(NULL); wc.lpszClassName=L"PanelProbeFocus";
    RegisterClassW(&wc);
    HWND focus=CreateWindowW(wc.lpszClassName,L"Panel probe game surrogate",WS_OVERLAPPEDWINDOW,30,30,500,350,NULL,NULL,wc.hInstance,NULL);
    ShowWindow(focus,SW_SHOW); SetForegroundWindow(focus); pump(150);
    CHECK(GetForegroundWindow()==focus,"surrogate foreground established");
    CHECK(!uiwindow_open("invalid",0,100,30,draw,NULL),"invalid open returns NULL");
    ProbeDrawState a={0},b={0};
    // Warm up the process window class and system GDI/font caches before counting resources.
    UiWindow *warm=uiwindow_open("warm",300,230,30,draw,&a); pump(80); uiwindow_close(warm); pump(30);
    DWORD gdi_before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS), handles_before=0;
    GetProcessHandleCount(GetCurrentProcess(),&handles_before);
    UiWindow *one=uiwindow_open("probe one",400,300,30,draw,&a);
    UiWindow *two=uiwindow_open("probe two",420,320,60,draw,&b); pump(150);
    CHECK(one && two && uiwindow_is_open(one) && uiwindow_is_open(two),"two simultaneous windows");
    CHECK(a.tid && b.tid && a.tid!=b.tid && a.tid!=(LONG)GetCurrentThreadId(),"independent drawing threads");
    CHECK(GetForegroundWindow()==focus,"open does not activate");
    HWND h1=FindWindowW(L"TotkAuxiliaryPanel",L"probe one"), h2=FindWindowW(L"TotkAuxiliaryPanel",L"probe two");
    CHECK(h1 && h2,"native handles discoverable");
    CHECK((GetWindowLongPtrW(h1,GWL_EXSTYLE)&(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TOPMOST))==(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TOPMOST),"nonactivating topmost tool styles");
    uiwindow_set_title(one,"探针标题"); pump(80);
    CHECK(FindWindowW(L"TotkAuxiliaryPanel",L"探针标题")==h1,"dynamic UTF-8 title");
    SetWindowPos(h2,HWND_TOPMOST,900,100,450,350,SWP_NOACTIVATE);
    SetWindowPos(h1,HWND_TOPMOST,400,100,460,360,SWP_NOACTIVATE); pump(80);
    RECT r; GetClientRect(h1,&r); CHECK(a.width==r.right && a.height==r.bottom,"resize updates drawing dimensions");
    CHECK(SendMessageW(h1,WM_MOUSEACTIVATE,(WPARAM)focus,MAKELPARAM(HTCLIENT,WM_LBUTTONDOWN))==MA_NOACTIVATE,"mouse activation refused");
    gesture(h1,HTCAPTION,45,35,focus);
    gesture(h1,HTBOTTOMRIGHT,50,45,focus);
    GetWindowRect(h1,&r); SetCursorPos(r.left+100,r.top+100); mouse_flag(MOUSEEVENTF_LEFTDOWN); mouse_flag(MOUSEEVENTF_LEFTUP); pump(80);
    CHECK(GetForegroundWindow()==focus,"client click keeps foreground");
    RECT dpi_rect; GetWindowRect(h1,&dpi_rect);
    SendMessageW(h1,WM_DPICHANGED,MAKELONG(144,144),(LPARAM)&dpi_rect); pump(80);
    CHECK(uiwindow_is_open(one) && a.calls>2,"synthetic DPI redraw survives");
    PostMessageW(h1,WM_CLOSE,0,0); pump(80);
    CHECK(!uiwindow_is_open(one) && uiwindow_take_close_requested(one),"user close notification");
    CHECK(!uiwindow_take_close_requested(one),"close notification consumed once");
    LONG calls=a.calls; uiwindow_close(one); pump(60); CHECK(a.calls==calls,"join stops drawing before return");
    CHECK(uiwindow_is_open(two),"closing one preserves the other");
    uiwindow_close(two);
    for(int i=0;i<40;i++) {
        one=uiwindow_open("reopen",300+i%5,240+i%7,60,draw,&a); pump(18);
        CHECK(one && uiwindow_is_open(one),"reopen lifecycle"); uiwindow_repaint(one); uiwindow_close(one);
    }
    pump(80);
    DWORD handles_after=0; GetProcessHandleCount(GetCurrentProcess(),&handles_after);
    DWORD gdi_after=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    printf("resources GDI %lu -> %lu; handles %lu -> %lu\n",(unsigned long)gdi_before,(unsigned long)gdi_after,(unsigned long)handles_before,(unsigned long)handles_after);
    CHECK(gdi_after<=gdi_before,"GDI objects reclaimed"); CHECK(handles_after<=handles_before,"thread and event handles reclaimed");
    DestroyWindow(focus); if(prior) SetForegroundWindow(prior); SetCursorPos(cursor.x,cursor.y);
    printf("uiwindow probe: %d failures\n",failures); return failures?1:0;
}
