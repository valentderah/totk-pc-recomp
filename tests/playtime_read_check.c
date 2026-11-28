// Real platform I/O and the production playtime loader, without game data.
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "platform/win32.c"
#else
#include "platform/linux.c"
#endif
#include "playtime.c"
void rt_log(const char *fmt, ...) { (void)fmt; }
const RtConfig *rt_config(void) { abort(); }
int main(int argc, char **argv) {
    assert(argc == 2);
    char file[1200];
    snprintf(file, sizeof file, "%s/playtime.txt", argv[1]);
    plat_file_mkdir_p(argv[1]);
    static const char original[] = "total_seconds 360000\n";
    assert(plat_file_write_atomic(file, original, sizeof original - 1));
#ifdef _WIN32
    wchar_t wide[2048];
    assert(MultiByteToWideChar(CP_UTF8, 0, file, -1, wide, 2048));
    HANDLE held = CreateFileW(wide, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    assert(held != INVALID_HANDLE_VALUE);
    reset();
    load(argv[1]);
    CloseHandle(held);
    uint64_t last_save = 0;
    tick_locked(31000, 1000, 1, &last_save);
    playtime_save();
    int preserved = file_is(file, original);
    printf("locked existing counter preserved after later save: %s\n", preserved ? "yes" : "NO");
    if (!preserved) {
        uint64_t size = 0;
        uint8_t *written = plat_file_read(file, &size);
        if (written) printf("counter after save: %s", written);
        free(written);
        return 1;
    }
#else
    puts("Windows sharing lock case not run on this platform");
#endif
    reset();
    load(argv[1]);
    uint64_t total, session;
    assert(playtime_get(&total, &session) && total == 360000 && session == 0);
    puts("next run reads the original total");
    assert(!probe_playtime());
    assert(plat_file_write_atomic(file, "", 0));
    reset();
    load(argv[1]);
    assert(playtime_get(&total, &session) && total == 0);
    char bad[1220];
    snprintf(bad, sizeof bad, "%s.bad", file);
    assert(plat_file_exists(bad) && plat_file_size(bad) == 0);
    puts("empty damaged counter is backed up before counting from zero");
#ifdef _WIN32
    static const char damaged[] = "total_seconds twelve\n";
    assert(plat_file_write_atomic(file, damaged, sizeof damaged - 1));
    assert(MultiByteToWideChar(CP_UTF8, 0, bad, -1, wide, 2048));
    held = CreateFileW(wide, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    assert(held != INVALID_HANDLE_VALUE);
    reset();
    load(argv[1]);
    CloseHandle(held);
    last_save = 0;
    tick_locked(31000, 1000, 1, &last_save);
    playtime_save();
    assert(file_is(file, damaged));
    assert(!playtime_get(&total, &session));
    puts("failed damaged-counter backup leaves the original unchanged");
#endif
    return 0;
}
