/* Game-data-free GDI/window regression. Uses the real catalog, configuration
 * reads, platform locks and native windows; gamepad snapshots and saving are test
 * doubles. Include input_panel.c to inspect drawing/lifecycle state and inject
 * a deterministic close race through its existing window-call boundary.
 * Platform: Windows x86-64, recent runtime-supported Clang, Win32/GDI libraries,
 * and an interactive desktop. No SDL library, game data or generated C is needed.
 * Native panels appear briefly and the test checks the current foreground window;
 * avoid concurrent desktop interaction. No user ini is read or saved. Run without
 * TOTK_* environment overrides. An optional first argument writes a synthetic BMP
 * at that explicit path; argv[2] optionally writes the initial 460x380 layout.
 * Omit both for no output file. PASS is exit 0, CHECK fails 1.
 *
 * Build/run from the repository root in PowerShell with clang on PATH. Keep
 * outputs under ignored build/test, or select an explicit temporary directory.
 *
 * $out = "build/test/input"
 * New-Item -ItemType Directory -Force $out | Out-Null
 * clang -std=gnu11 -O1 -g -D_WIN32_WINNT=0x0A00 -I runtime tests/input_panel_probe.c runtime/ui_text.c runtime/cfg.c runtime/uiwindow_win.c runtime/platform/win32.c runtime/platform/common.c -luser32 -lgdi32 -lpsapi -lbcrypt -lole32 -lshell32 -luuid -lwinmm -o "$out/input_panel_probe.exe"
 * & "$out/input_panel_probe.exe"
 * # Optional synthetic images: & "$out/input_panel_probe.exe" "$out/input-panel.bmp" "$out/input-panel-small.bmp"
 */
#include "../runtime/uiwindow.h"
static UiWindow *panel_test_open(const char *title,int w,int h,int hz,UiWindowDrawFn draw,void *user);
static int panel_test_is_open(const UiWindow *w);
#define uiwindow_is_open panel_test_is_open
#define uiwindow_open panel_test_open
#define cfg_save panel_test_save
#include "../runtime/input_panel.c"
#undef cfg_save
#undef uiwindow_open
#undef uiwindow_is_open
#include <stdarg.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); exit(1); } } while (0)
static InputOverlayState fixture;
static PlatMutex fixture_lock = PLAT_MUTEX_INIT;
static int saves, fail_open, open_calls, close_on_is_open;
static int panel_test_is_open(const UiWindow *w) {
    if (close_on_is_open) {
        close_on_is_open = 0;
        // The tick already consumed its first notification check. Close now,
        // before it observes the stopped window, without a timing-dependent sleep.
        HWND hwnd = FindWindowW(L"TotkAuxiliaryPanel",NULL);
        CHECK(hwnd);
        SendMessageW(hwnd,WM_CLOSE,0,0);
    }
    return uiwindow_is_open(w);
}
static UiWindow *panel_test_open(const char *title,int w,int h,int hz,UiWindowDrawFn fn,void *user) {
    open_calls++;
    return fail_open ? NULL : uiwindow_open(title,w,h,hz,fn,user);
}
int panel_test_save(void) { saves++; return 1; }
int input_overlay_state(InputOverlayState *s) {
    plat_mutex_lock(&fixture_lock); *s = fixture; plat_mutex_unlock(&fixture_lock); return s->connected;
}
const char *ov_language(void) { return "en-US"; }
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }

static HWND find_window(void) { return FindWindowW(L"TotkAuxiliaryPanel",NULL); }
static void wait_paint(void) { Sleep(100); }
static void bitmap_save(const char *path,HDC dc,HBITMAP bmp,int w,int h) {
    BITMAPINFO bi = {0}; bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    size_t bytes = (size_t)w*h*4;
    void *pixels = malloc(bytes); CHECK(pixels);
    CHECK(GetDIBits(dc,bmp,0,h,pixels,&bi,DIB_RGB_COLORS));
    BITMAPFILEHEADER head = {0}; head.bfType = 0x4d42; head.bfOffBits = sizeof head + sizeof bi.bmiHeader;
    head.bfSize = (DWORD)(head.bfOffBits + bytes);
    FILE *f = fopen(path,"wb"); CHECK(f);
    CHECK(fwrite(&head,sizeof head,1,f) == 1);
    CHECK(fwrite(&bi.bmiHeader,sizeof bi.bmiHeader,1,f) == 1);
    CHECK(fwrite(pixels,bytes,1,f) == 1); fclose(f); free(pixels);
}
static void competing_tick(void *arg) {
    (void)arg;
    for (int i=0;i<100;i++) { input_panel_tick(); Sleep(1); }
}
int main(int argc,char **argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    cfg_set("TOTK_MENU_LANG","en");
    fixture.connected = 1; fixture.layout = PAD_LAYOUT_NINTENDO; fixture.motion_enabled = 1;
    fixture.labels[0] = PAD_LABEL_A; fixture.labels[1] = PAD_LABEL_B;
    fixture.labels[2] = PAD_LABEL_X; fixture.labels[3] = PAD_LABEL_Y;
    fixture.deadzone[0] = 20; fixture.deadzone[1] = 15;
    fixture.raw[0] = 14000; fixture.raw[1] = 16000; fixture.out[0] = 10000; fixture.out[1] = 12000;
    fixture.buttons = (1ull << B_A) | (1ull << B_PLUS) | (1ull << B_UP);
    fixture.trigger[0] = 8192; fixture.trigger[1] = 24000;
    HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen,620,500); ReleaseDC(NULL,screen);
    CHECK(dc && bmp);
    HGDIOBJ original = SelectObject(dc,bmp);
    draw(NULL,dc,620,500);
    SetPixel(dc,0,0,RGB(255,0,255));
    draw(NULL,dc,620,500); CHECK(GetPixel(dc,0,0) == RGB(255,0,255));
    cfg_set("TOTK_MENU_LANG","zh-Hans");
    draw(NULL,dc,620,500); CHECK(GetPixel(dc,0,0) == background);
    SetPixel(dc,0,0,RGB(255,0,255));
    draw(NULL,dc,619,500); CHECK(GetPixel(dc,0,0) == background);
    fixture.layout = PAD_LAYOUT_XBOX; draw(NULL,dc,620,500);
    Canvas c = {dc,.95,15.5,12.5};
    CHECK(GetPixel(dc,px(&c,494),py(&c,166)) == active); // Xbox A is south.
    fixture.layout = PAD_LAYOUT_NINTENDO; draw(NULL,dc,620,500);
    CHECK(GetPixel(dc,px(&c,535),py(&c,125)) == active); // Nintendo A is east.
    CHECK(GetPixel(dc,px(&c,494),py(&c,166)) == idle);
    // Mapped trigger action chips and physical analog meters must be independent.
    InputOverlayState saved = fixture;
    const int levels[] = {0,8192,24576,32767};
    const int sizes[][2] = {{620,500},{460,380}};
    for (unsigned size=0;size<sizeof sizes/sizeof *sizes;size++) {
        int width = sizes[size][0], height = sizes[size][1];
        Canvas p = {dc,fmin(width/620.,height/500.)*.95,0,0};
        p.ox = (width-620*p.scale)/2; p.oy = (height-500*p.scale)/2;
        for (int i=0;i<2;i++) for (int mapped=1;mapped>=0;mapped--) {
            for (unsigned level=0;level<sizeof levels/sizeof *levels;level++) {
                fixture.buttons = mapped ? 1ull << (i ? B_ZR : B_ZL) : 0;
                fixture.trigger[0] = fixture.trigger[1] = 0;
                fixture.trigger[i] = levels[level];
                draw(NULL,dc,width,height);
                int x = i ? 555 : 25;
                CHECK(GetPixel(dc,px(&p,x-5),py(&p,22)) == (mapped ? active : idle));
                CHECK(GetPixel(dc,px(&p,x+12),py(&p,130)) == (levels[level] ? active : idle));
                CHECK(GetPixel(dc,px(&p,x+12),py(&p,50)) == (levels[level] == 32767 ? active : idle));
                CHECK(GetPixel(dc,px(&p,(i ? 25 : 555)-5),py(&p,22)) == idle);
            }
        }
    }
    fixture = saved; draw(NULL,dc,620,500);
    puts("PASS: mapped ZL/ZR action chips and physical analog trigger meters are independent");
    DWORD before = GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for (int i=0;i<300;i++) { fixture.raw[0] = i; draw(NULL,dc,620,500); }
    DWORD after = GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    CHECK(after <= before + 1);
    if (argc > 1) {
        // Show a mapped left action with its physical axis released, and the inverse on the right.
        fixture.buttons |= 1ull << B_ZL; fixture.buttons &= ~(1ull << B_ZR);
        fixture.trigger[0] = 0; draw(NULL,dc,620,500);
        bitmap_save(argv[1],dc,bmp,620,500);
        if (argc > 2) {
            HBITMAP small = CreateCompatibleBitmap(dc,460,380); CHECK(small);
            SelectObject(dc,small); draw(NULL,dc,460,380);
            bitmap_save(argv[2],dc,small,460,380);
            SelectObject(dc,bmp); DeleteObject(small);
        }
        fixture = saved; draw(NULL,dc,620,500);
    }
    fixture.connected = 0; draw(NULL,dc,620,500); CHECK(!previous.connected);
    SelectObject(dc,original); DeleteObject(bmp); DeleteDC(dc);
    valid = 0;
    // The following tests open real native panels without starting the game or writing any ini file.
    fixture.connected = 1;
    cfg_set("TOTK_INPUT_WINDOW","1");
    fail_open = 1;
    input_panel_tick(); CHECK(!window && open_calls == 1);
    input_panel_tick(); CHECK(!window && open_calls == 1);
    cfg_set("TOTK_INPUT_WINDOW","0"); input_panel_tick();
    cfg_set("TOTK_INPUT_WINDOW","1"); fail_open = 0;
    HWND foreground = GetForegroundWindow();
    input_panel_tick(); wait_paint(); CHECK(window && find_window());
    HWND hwnd = find_window();
    CHECK(GetForegroundWindow() == foreground);
    CHECK(GetWindowLongPtrW(hwnd,GWL_EXSTYLE) & WS_EX_NOACTIVATE);
    CHECK(SendMessageW(hwnd,WM_MOUSEACTIVATE,0,0) == MA_NOACTIVATE);
    SetWindowPos(hwnd,NULL,0,0,800,600,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER); wait_paint();
    wchar_t title[256]; GetWindowTextW(hwnd,title,256); CHECK(wcscmp(title,L"手柄输入") == 0);
    const char *langs[] = {"en","ru","pt-BR","zh-Hans"};
    for (int i=0;i<4;i++) {
        cfg_set("TOTK_MENU_LANG",langs[i]); input_panel_tick(); wait_paint();
        wchar_t expected[256]; MultiByteToWideChar(CP_UTF8,0,menu_text(UI_INPUT_TITLE),-1,expected,256);
        GetWindowTextW(hwnd,title,256); CHECK(wcscmp(title,expected) == 0);
    }
    SendMessageW(hwnd,WM_CLOSE,0,0); input_panel_tick(); CHECK(!window && saves == 1);
    CHECK(!cfg_bool("TOTK_INPUT_WINDOW"));
    cfg_set("TOTK_INPUT_WINDOW","1"); input_panel_tick(); CHECK(window);
    int before_race = saves;
    close_on_is_open = 1; input_panel_tick();
    CHECK(!window && saves == before_race + 1 && !cfg_bool("TOTK_INPUT_WINDOW"));
    input_panel_tick(); CHECK(!window && saves == before_race + 1);
    cfg_set("TOTK_INPUT_WINDOW","1"); input_panel_tick(); CHECK(window);
    cfg_set("TOTK_INPUT_WINDOW","0"); input_panel_tick(); CHECK(!window);
    cfg_set("TOTK_INPUT_WINDOW","1"); input_panel_tick(); CHECK(window);
    hwnd = find_window();
    SendMessageW(hwnd,WM_APP+21,0,0); // Programmatic internal close without a user notification simulates an unexpected window exit.
    input_panel_tick(); CHECK(!window);
    int stopped_calls = open_calls; input_panel_tick(); CHECK(open_calls == stopped_calls);
    cfg_set("TOTK_INPUT_WINDOW","0"); input_panel_tick();
    cfg_set("TOTK_INPUT_WINDOW","1"); input_panel_tick(); CHECK(window);
    CHECK(_putenv("TOTK_INPUT_WINDOW=1") == 0);
    before_race = saves;
    close_on_is_open = 1; input_panel_tick();
    CHECK(!window && saves == before_race + 1 && cfg_bool("TOTK_INPUT_WINDOW"));
    int closed_calls = open_calls;
    input_panel_tick(); input_panel_tick(); CHECK(!window && open_calls == closed_calls);
    puts("PASS: close notification races save once and preserve environment suppression");
    CHECK(_putenv("TOTK_INPUT_WINDOW=") == 0); input_panel_tick();
    cfg_set("TOTK_INPUT_WINDOW","1"); input_panel_tick(); CHECK(window);
    PlatThread *t = plat_thread_new(competing_tick,NULL,"panel-exit-test"); CHECK(t);
    Sleep(5); input_panel_close(); plat_thread_join(t); CHECK(!window);
    input_panel_tick(); CHECK(!window);
    HWND final_foreground = GetForegroundWindow();
    wchar_t final_class[128] = {0}; GetClassNameW(final_foreground,final_class,128);
    CHECK(wcscmp(final_class,L"TotkAuxiliaryPanel") != 0);
    if (final_foreground != foreground) fprintf(stderr,"NOTE: Other desktop activity changed the foreground during the test; initial panel creation preserved focus.\n");
    puts("PASS: GDI layout, button positions, dirty skipping, language/size redraw, 300-frame resource counts, native no-activation, four-language titles, close/save double, toggles, concurrent exit");
    return 0;
}
