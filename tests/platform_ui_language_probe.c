// Test concurrent first initialization of the real platform language cache without SDL or game data.
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
    for(int i=0;i<16;i++) threads[i]=CreateThread(NULL,0,worker,(void *)(intptr_t)i,0,NULL);
    __atomic_store_n(&start,1,__ATOMIC_RELEASE);
    WaitForMultipleObjects(16,threads,TRUE,INFINITE); for(int i=0;i<16;i++) CloseHandle(threads[i]);
#else
    pthread_t threads[16];
    for(int i=0;i<16;i++) pthread_create(&threads[i],NULL,worker,(void *)(intptr_t)i);
    __atomic_store_n(&start,1,__ATOMIC_RELEASE);
    for(int i=0;i<16;i++) pthread_join(threads[i],NULL);
#endif
    for(int i=1;i<16;i++) if(strcmp(tags[0],tags[i])) failures++;
    printf("platform UI language '%s': %d failures\n",tags[0],failures); return failures?1:0;
}
