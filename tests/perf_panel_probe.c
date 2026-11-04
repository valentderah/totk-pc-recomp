// Windows interactive-desktop regression: real GDI/catalog/window, configuration and metrics test doubles.
// Opens non-activating windows; no game, SDL, Vulkan or user ini reads/writes. Optional argv[1] writes a BMP.
// From the repository root, create build/test or replace the output directory below.
// clang -O2 -Wall -Wextra -Werror -D_WIN32_WINNT=0x0A00 -I runtime tests/perf_panel_probe.c runtime/uiwindow_win.c runtime/ui_text.c -luser32 -lgdi32 -o build/test/perf_panel_probe.exe
// Run: build/test/perf_panel_probe.exe [build/test/perf-panel.bmp]
// The test includes perf_panel.c to interpose the close/alive race; do not compile that source twice.
// Optional headless snapshot regression: runtime-supported Clang, no desktop, GPU, SDL or game data.
// This mode includes the real perfmon.c; clock/lock scheduling, the GPU counter and unused sampler services are test doubles.
// clang -O2 -Wall -Wextra -Werror -DPERFMON_SNAPSHOT_TEST -I runtime tests/perf_panel_probe.c -o build/test/perfmon_snapshot_probe.exe
// Run: build/test/perfmon_snapshot_probe.exe (create build/test first, or choose another output directory).
#ifdef PERFMON_SNAPSHOT_TEST
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int failures, held, scheduled, unlocked_clock_reads;
static unsigned clock_reads;
static uint64_t clock_ns, gpu_ns;
static void competing_owner(int operation);
#define CHECK(x, name) do { if(!(x)) { printf("FAIL %s\n",name); failures++; } else printf("PASS %s\n",name); } while(0)
#include "../runtime/perfmon.c"

void plat_mutex_lock(PlatMutex *m) {
    CHECK(m==&g_lock && !held,"snapshot lock acquisition is serialized");
    if(scheduled) { int operation=scheduled; scheduled=0; competing_owner(operation); }
    held=1;
}
void plat_mutex_unlock(PlatMutex *m) { CHECK(m==&g_lock && held,"snapshot lock release"); held=0; }
uint64_t plat_now_ns(void) { clock_reads++; if(!held) unlocked_clock_reads++; return clock_ns; }
uint64_t perf_gpu_busy_total(void) { CHECK(held,"GPU counter read under the statistics lock"); return gpu_ns; }
void perf_set_overlay(int mode) { (void)mode; abort(); }
void *plat_lib_open(const char *name) { (void)name; abort(); }
void *plat_lib_sym(void *lib, const char *name) { (void)lib; (void)name; abort(); }
PlatThread *plat_thread_new(PlatThreadFn fn,void *arg,const char *name) { (void)fn;(void)arg;(void)name; abort(); }
void plat_thread_detach(PlatThread *thread) { (void)thread; abort(); }
void plat_cond_signal(PlatCond *cond) { (void)cond; abort(); }
int plat_cond_wait_ns(PlatCond *cond,PlatMutex *m,uint64_t ns) { (void)cond;(void)m;(void)ns; abort(); }
PlatLoad *plat_load_open(const uint8_t *luid,int bus) { (void)luid;(void)bus; abort(); }
void plat_load_close(PlatLoad *load) { (void)load; abort(); }
void plat_cpu_load(PlatLoad *load,float *total,float *process,float *temp) { (void)load;(void)total;(void)process;(void)temp; abort(); }
void plat_gpu_load(PlatLoad *load,float *usage,float *vram) { (void)load;(void)usage;(void)vram; abort(); }
void plat_mem_info(float *used,float *total,float *process) { (void)used;(void)total;(void)process; abort(); }
void sysinfo_cpu_name(char *out,size_t cap) { (void)out;(void)cap; abort(); }

static const uint64_t epoch=10000000000ull;
static void frame(uint64_t now,int newer) {
    uint64_t waits[PERF_N]={0};
    waits[PERF_PIPE]=(newer?6:2)*1000000ull;
    waits[PERF_XLATE]=(newer?7:3)*1000000ull;
    waits[PERF_TEX]=(newer?8:4)*1000000ull;
    perfmon_frame(now,20000000ull,waits,(newer?9:5)*1000000ull,newer?8:2);
}
static void competing_owner(int operation) {
    // Run another real owner after the caller reached lock(), but before it acquires the lock.
    if(operation==2) {
        clock_ns=epoch+600000000ull;
        PerfStats other; perfmon_snapshot(&other);
        CHECK(other.replay_ms==5 && g_win.t0==clock_ns,"competing snapshot advances the summary window");
        clock_ns=epoch+620000000ull; gpu_ns=15000000ull; frame(clock_ns,1);
    } else {
        clock_ns=epoch+200000000ull; frame(clock_ns,0);
    }
}
static void reset(void) {
    memset(&g_stats,0,sizeof g_stats); memset(g_ring,0,sizeof g_ring); memset(g_ring_at,0,sizeof g_ring_at);
    memset(g_low_hist,0,sizeof g_low_hist); g_ring_n=g_ring_pos=g_low_cur=0; g_low_t0=epoch;
    memset(&g_win,0,sizeof g_win); g_win.t0=epoch;
    held=scheduled=unlocked_clock_reads=0; clock_reads=0; clock_ns=epoch+100000000ull; gpu_ns=6000000ull;
    frame(epoch,0);
}
int main(void) {
    PerfStats stats;
    reset(); scheduled=1; perfmon_snapshot(&stats);
    CHECK(!unlocked_clock_reads && clock_reads==1,"snapshot reads time only after acquiring its lock");
    CHECK(stats.fps==50 && stats.frame_ms_avg==20 && stats.frame_ms_max==20,"a frame published while waiting is retained in FPS");
    CHECK(g_win.frames==2 && g_win.t0==epoch,"a partial counter window is not cleared");
    reset(); scheduled=2; perfmon_snapshot(&stats);
    CHECK(!unlocked_clock_reads && clock_reads==2,"both snapshots obtain time while holding the lock");
    CHECK(stats.fps==50 && stats.frame_ms_avg==20,"the later snapshot does not hide a newly published frame");
    CHECK(g_win.t0==epoch+600000000ull && g_win.frames==1,"elapsed time does not wrap or reset the competing window");
    CHECK(stats.replay_ms==5 && stats.pipe_wait_ms==2 && stats.xlate_ms==3 && stats.tex_ms==4 && stats.tex_uploads==2,"previous averages survive until the next real deadline");
    clock_ns=epoch+1120000000ull; perfmon_snapshot(&stats);
    CHECK(stats.replay_ms==9 && stats.pipe_wait_ms==6 && stats.xlate_ms==7 && stats.tex_ms==8 && stats.tex_uploads==6 && stats.gpu_ms==9,"pending counters settle normally at the half-second deadline");
    CHECK(g_win.t0==clock_ns && !g_win.frames,"the completed window resets at the current locked time");
    printf("perfmon snapshot probe: %d failures\n",failures); return failures?1:0;
}
#else
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cfg.h"
#include "perf_panel.h"
#include "uiwindow.h"
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
void rt_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
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
static int close_during_alive_check;
static int test_is_open(const UiWindow *w);
// Interpose only in the panel under test, without changing the production window implementation.
#define uiwindow_is_open test_is_open
#include "../runtime/perf_panel.c"
#undef uiwindow_is_open
static int test_is_open(const UiWindow *w) {
    if (close_during_alive_check) {
        close_during_alive_check = 0;
        SendMessageW(panel(), WM_CLOSE, 0, 0);
    }
    return uiwindow_is_open(w);
}
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
    close_during_alive_check=1; perf_panel_tick();
    CHECK(!panel() && !perf_panel_active() && !setting && writes==1 && saves==1,"close updates settings once and releases sample demand");
    for(int mode=0;mode<3;mode++) CHECK(perf_sampling_mode(mode,perf_panel_active())==mode,"close restores overlay sampling");
    setting=1; perf_panel_tick(); pump(40); CHECK(panel()!=NULL,"off-on reopens");
    environment=1; close_during_alive_check=1; perf_panel_tick();
    for(int i=0;i<50;i++) perf_panel_tick();
    CHECK(!panel() && !perf_panel_active() && cfg_bool("TOTK_PERF_WINDOW") && writes==2 && saves==2,"environment override close saves once without reopening");
    environment=0; setting=0; perf_panel_tick(); setting=1; perf_panel_tick(); pump(40); CHECK(panel()!=NULL,"effective off rearms the panel");
    // Concurrent language fallback exercises the former shared base[] buffer race.
    __atomic_store_n(&language,"ru-RU",__ATOMIC_RELEASE); __atomic_add_fetch(&version,1,__ATOMIC_RELEASE);
    HANDLE readers[8];
    for(int i=0;i<8;i++) {
        readers[i]=CreateThread(NULL,0,language_reader,NULL,0,NULL);
        CHECK(readers[i]!=NULL,"language reader creation");
    }
    for(int i=0;i<8;i++) if(readers[i]) {
        CHECK(WaitForSingleObject(readers[i],30000)==WAIT_OBJECT_0,"language reader join");
        CloseHandle(readers[i]);
    }
    CHECK(!raced,"concurrent catalog fallback resolution");
    InterlockedExchange(&run,1); HANDLE thread=CreateThread(NULL,0,ticker,NULL,0,NULL);
    CHECK(thread!=NULL,"concurrent ticker creation"); pump(40);
    perf_panel_close(); InterlockedExchange(&run,0);
    if(thread) { CHECK(WaitForSingleObject(thread,30000)==WAIT_OBJECT_0,"concurrent ticker join"); CloseHandle(thread); }
    perf_panel_tick(); CHECK(!panel() && !perf_panel_active(),"concurrent exit joins and permanently stops reopening");
    printf("perf panel probe: %d failures\n",failures); return failures?1:0;
}
#endif
