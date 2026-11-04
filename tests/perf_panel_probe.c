// Use the real panel, GDI, catalog and window; settings, metrics and platform locks are test doubles, with no game or user ini reads.
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cfg.h"
#include "host.h"
#include "perf_panel.h"
#include "perfmon.h"
#include "platform/platform.h"
#include "ui/perf_mode.h"
#include "ui_text.h"

const int rt_game_version = 110;
const char rt_port_version[] = "probe";
static int setting, environment, saves, writes;
static unsigned version;
static const char *language = "en";
static int failures;
#define CHECK(x, name) do { if(!(x)) { printf("FAIL %s\n",name); failures++; } else printf("PASS %s\n",name); } while(0)
void rt_log(const char *format, ...) { va_list ap; va_start(ap,format); vprintf(format,ap); putchar('\n'); va_end(ap); }
void plat_mutex_lock(PlatMutex *m) { AcquireSRWLockExclusive((SRWLOCK *)m); }
void plat_mutex_unlock(PlatMutex *m) { ReleaseSRWLockExclusive((SRWLOCK *)m); }
int cfg_bool(const char *key) { (void)key; return environment || setting; }
void cfg_set(const char *key, const char *value) { (void)key; setting=atoi(value); writes++; __atomic_add_fetch(&version,1,__ATOMIC_RELEASE); }
int cfg_save(void) { saves++; return 1; }
const char *cfg_value(const char *key) { (void)key; return __atomic_load_n(&language,__ATOMIC_ACQUIRE); }
unsigned cfg_version(void) { return __atomic_load_n(&version,__ATOMIC_ACQUIRE); }
const char *plat_ui_language(void) { return "ru-RU"; }
const char *ov_language(void) { return "en-US"; }
void perfmon_snapshot(PerfStats *s) {
    memset(s,0,sizeof *s); s->fps=60; s->frame_ms_avg=16.67f; s->frame_ms_max=24;
    s->low1_fps=55; s->low01_fps=-1; s->gpu_pct=43; s->gpu_temp_c=-1; s->gpu_ms=5;
    strcpy(s->cpu_name,"CPU probe"); strcpy(s->gpu_name,"GPU probe");
    for(int i=0;i<PERF_HISTORY;i++) s->frame_ms[i]=14+(i%10);
}
static void pump(int ms) {
    ULONGLONG end=GetTickCount64()+ms;
    do { MSG m; while(PeekMessageW(&m,NULL,0,0,PM_REMOVE)) {TranslateMessage(&m); DispatchMessageW(&m);} Sleep(2); } while(GetTickCount64()<end);
}
static HWND panel(void) { return FindWindowW(L"TotkAuxiliaryPanel",NULL); }
static volatile LONG run, raced;
static DWORD WINAPI ticker(void *arg) { (void)arg; while(InterlockedCompareExchange(&run,0,0)) { perf_panel_tick(); Sleep(1); } return 0; }
static DWORD WINAPI language_reader(void *arg) {
    (void)arg;
    for(int i=0;i<100000;i++) if(strcmp(menu_text(UI_MENU_PERF_WINDOW),"Окно производительности")) InterlockedIncrement(&raced);
    return 0;
}
static void snapshot(HWND hwnd, const char *path) {
    RECT r; GetClientRect(hwnd,&r);
    HDC source=GetDC(hwnd), dc=CreateCompatibleDC(source);
    BITMAPINFO info={0}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=r.right; info.bmiHeader.biHeight=-r.bottom;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    void *pixels=NULL; HBITMAP bitmap=CreateDIBSection(source,&info,DIB_RGB_COLORS,&pixels,NULL,0);
    if(bitmap && dc) {
        HGDIOBJ old=SelectObject(dc,bitmap);
        BitBlt(dc,0,0,r.right,r.bottom,source,0,0,SRCCOPY); GdiFlush();
        BITMAPFILEHEADER head={0}; head.bfType=0x4d42; head.bfOffBits=sizeof head+sizeof info.bmiHeader;
        DWORD bytes=(DWORD)r.right*(DWORD)r.bottom*4; head.bfSize=head.bfOffBits+bytes;
        FILE *file=fopen(path,"wb");
        if(file) { fwrite(&head,sizeof head,1,file); fwrite(&info.bmiHeader,sizeof info.bmiHeader,1,file); fwrite(pixels,bytes,1,file); fclose(file); }
        SelectObject(dc,old);
    }
    if(bitmap) DeleteObject(bitmap); if(dc) DeleteDC(dc); ReleaseDC(hwnd,source);
}
int main(int argc, char **argv) {
    perf_panel_tick(); CHECK(!panel() && !perf_panel_active(),"default off");
    setting=1; perf_panel_tick(); pump(100); CHECK(panel() && perf_panel_active(),"panel opens and samples full");
    for(int mode=0;mode<3;mode++) CHECK(perf_sampling_mode(mode,perf_panel_active())==2,"window raises sampling only");
    HWND h=panel(); SetWindowPos(h,HWND_TOPMOST,600,100,720,700,SWP_NOACTIVATE); pump(80);
    DWORD gdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for(int i=0;i<100;i++) { SetWindowPos(h,NULL,0,0,680+i%4,620+i%3,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE); pump(3); }
    pump(60); CHECK(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)<=gdi,"performance drawing and resize reclaim GDI");
    __atomic_store_n(&language,"zh-Hans",__ATOMIC_RELEASE); __atomic_add_fetch(&version,1,__ATOMIC_RELEASE);
    perf_panel_tick(); pump(60); CHECK(FindWindowW(L"TotkAuxiliaryPanel",L"性能独立窗口")!=NULL,"live Chinese title");
    if(argc>1) snapshot(panel(),argv[1]);
    PostMessageW(panel(),WM_CLOSE,0,0); pump(50); perf_panel_tick();
    CHECK(!panel() && !perf_panel_active() && !setting && writes==1 && saves==1,"close updates settings once and releases sample demand");
    for(int mode=0;mode<3;mode++) CHECK(perf_sampling_mode(mode,perf_panel_active())==mode,"close restores overlay sampling");
    setting=1; perf_panel_tick(); pump(40); CHECK(panel()!=NULL,"off-on reopens");
    environment=1; PostMessageW(panel(),WM_CLOSE,0,0); pump(50); perf_panel_tick();
    for(int i=0;i<50;i++) perf_panel_tick();
    CHECK(!panel() && !perf_panel_active() && cfg_bool("TOTK_PERF_WINDOW"),"environment override does not reopen after close");
    environment=0; perf_panel_tick(); setting=1; perf_panel_tick(); pump(40); CHECK(panel()!=NULL,"effective off rearms the panel");
    // Concurrent language fallback exercises the former shared base[] buffer race.
    __atomic_store_n(&language,"ru-RU",__ATOMIC_RELEASE); __atomic_add_fetch(&version,1,__ATOMIC_RELEASE);
    HANDLE readers[8]; for(int i=0;i<8;i++) readers[i]=CreateThread(NULL,0,language_reader,NULL,0,NULL);
    WaitForMultipleObjects(8,readers,TRUE,INFINITE); for(int i=0;i<8;i++) CloseHandle(readers[i]);
    CHECK(!raced,"concurrent catalog fallback resolution");
    InterlockedExchange(&run,1); HANDLE thread=CreateThread(NULL,0,ticker,NULL,0,NULL); pump(40);
    perf_panel_close(); InterlockedExchange(&run,0); WaitForSingleObject(thread,INFINITE); CloseHandle(thread);
    perf_panel_tick(); CHECK(!panel() && !perf_panel_active(),"concurrent exit joins and permanently stops reopening");
    printf("perf panel probe: %d failures\n",failures); return failures?1:0;
}
