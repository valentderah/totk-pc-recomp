// Windows/Linux regression of the real platform language cache's first concurrent use; no game or desktop required.
// From the repository root, create build/test or replace the output directory below.
// Linux needs SDL3 headers on the compiler include path; unused SDL functions are discarded, not linked.
// clang -O2 -ffunction-sections -fdata-sections -Wall -Werror -I runtime tests/platform_ui_language_probe.c runtime/platform/linux.c runtime/platform/common.c -Wl,--gc-sections -lpthread -ldl -o build/test/platform_ui_language_probe
// Windows uses only the system libraries (PowerShell: quote '-Wl,--gc-sections'):
// clang -O2 -ffunction-sections -fdata-sections -Wall -D_WIN32_WINNT=0x0A00 -I runtime tests/platform_ui_language_probe.c runtime/platform/win32.c runtime/platform/common.c -Wl,--gc-sections -luser32 -lbcrypt -ldwmapi -lgdi32 -lpsapi -lshell32 -lole32 -luuid -lwinmm -o build/test/platform_ui_language_probe.exe
// Run the resulting build/test/platform_ui_language_probe[.exe]. Nonzero means any creation/join/value check failed.
#include <stdio.h>
#include <string.h>
#include "platform/platform.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif
static int start, failures;
static char tags[16][128];
static void read_language(int id) {
    while(!__atomic_load_n(&start,__ATOMIC_ACQUIRE)) {
#ifdef _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
    }
    const char *tag=plat_ui_language();
    snprintf(tags[id],sizeof tags[id],"%s",tag);
    for(int j=0;j<100000;j++) if(strcmp(tags[id],plat_ui_language())) __atomic_add_fetch(&failures,1,__ATOMIC_RELAXED);
}
#ifdef _WIN32
static DWORD WINAPI worker(void *arg) { read_language((int)(intptr_t)arg); return 0; }
#else
static void *worker(void *arg) { read_language((int)(intptr_t)arg); return NULL; }
#endif
int main(void) {
#ifdef _WIN32
    HANDLE threads[16];
    for(int i=0;i<16;i++) {
        threads[i]=CreateThread(NULL,0,worker,(void *)(intptr_t)i,0,NULL);
        if(!threads[i]) { fprintf(stderr,"thread %d creation failed\n",i); failures++; }
    }
    __atomic_store_n(&start,1,__ATOMIC_RELEASE);
    for(int i=0;i<16;i++) if(threads[i]) {
        if(WaitForSingleObject(threads[i],30000)!=WAIT_OBJECT_0) { fprintf(stderr,"thread %d join failed\n",i); __atomic_add_fetch(&failures,1,__ATOMIC_RELAXED); }
        CloseHandle(threads[i]);
    }
#else
    pthread_t threads[16]; int created[16];
    for(int i=0;i<16;i++) {
        created[i]=pthread_create(&threads[i],NULL,worker,(void *)(intptr_t)i)==0;
        if(!created[i]) { fprintf(stderr,"thread %d creation failed\n",i); failures++; }
    }
    __atomic_store_n(&start,1,__ATOMIC_RELEASE);
    for(int i=0;i<16;i++) if(created[i] && pthread_join(threads[i],NULL)) {
        fprintf(stderr,"thread %d join failed\n",i); __atomic_add_fetch(&failures,1,__ATOMIC_RELAXED);
    }
#endif
    if(__atomic_load_n(&failures,__ATOMIC_RELAXED)) {
        puts("FAIL platform language thread creation, join or value check"); return 1;
    }
    for(int i=1;i<16;i++) if(strcmp(tags[0],tags[i])) failures++;
    printf("platform UI language '%s': %d failures\n",tags[0],failures); return failures?1:0;
}
