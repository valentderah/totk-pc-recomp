// Two processes exercise the production save startup on a private synthetic profile.
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "platform/win32.c"
#else
#include "platform/linux.c"
#endif
#include "save_startup.c"
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); exit(76);
}
const RtConfig *rt_config(void) { abort(); }
void account_uid(uint64_t uid[2]) { uid[0] = 1; uid[1] = 2; }
int main(int argc, char **argv) {
    assert(argc == 3);
#ifdef _WIN32
    if (!strcmp(argv[2], "copy")) {
        char src[1200], dst[1200]; wchar_t a[2048], b[2048];
        snprintf(src, sizeof src, "%s/.totk.lock", argv[1]);
        snprintf(dst, sizeof dst, "%s/.totk.lock-copy", argv[1]);
        assert(MultiByteToWideChar(CP_UTF8, 0, src, -1, a, 2048));
        assert(MultiByteToWideChar(CP_UTF8, 0, dst, -1, b, 2048));
        if (!CopyFileW(a, b, FALSE)) { printf("CopyFileW failed: %lu\n", GetLastError()); return 1; }
        return 0;
    }
#endif
#ifdef PROFILE_LOCK_AVAILABLE
    int busy;
    PlatProfileLock *held = plat_profile_lock(argv[1], &busy);
    if (!held) { printf("profile refused before storage startup, busy=%d\n", busy); return busy ? 73 : 74; }
#endif
    fs_host_set_profile(argv[1]);
    storage_init(NULL);
    if (!strcmp(argv[2], "owner")) {
        work_create();
        char mark[1200], ready_file[1200];
        snprintf(mark, sizeof mark, "%s/pending.txt", work_root);
        snprintf(ready_file, sizeof ready_file, "%s/owner-ready", argv[1]);
        assert(plat_file_write(mark, "uncommitted", 11));
        assert(plat_file_write(ready_file, "ready", 5));
        getchar();
        printf("pending working file survives: %s\n", plat_file_exists(mark) ? "yes" : "NO");
        if (!plat_file_exists(mark)) return 1;
    }
    return 0;
}
