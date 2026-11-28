// Real host filesystem operations with deterministic failures in directory enumeration.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static HANDLE fault_handle = INVALID_HANDLE_VALUE;
static wchar_t fault_pattern[2048];
static int fault_mode, delivered;
static int ordinary(const wchar_t *s) { return wcscmp(s, L".") && wcscmp(s, L".."); }
static HANDLE WINAPI fixture_first(LPCWSTR pattern, LPWIN32_FIND_DATAW data) {
    HANDLE h = FindFirstFileW(pattern, data);
    if (fault_mode && !wcscmp(pattern, fault_pattern)) {
        fault_handle = h;
        delivered = h != INVALID_HANDLE_VALUE && ordinary(data->cFileName);
    } else {
        fault_handle = INVALID_HANDLE_VALUE;
    }
    return h;
}
static BOOL WINAPI fixture_next(HANDLE h, LPWIN32_FIND_DATAW data) {
    if (fault_mode && h == fault_handle && delivered) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    BOOL ok = FindNextFileW(h, data);
    if (ok && h == fault_handle && ordinary(data->cFileName)) delivered++;
    return ok;
}
#define FindFirstFileW fixture_first
#define FindNextFileW fixture_next
#include "platform/win32.c"
#undef FindFirstFileW
#undef FindNextFileW
#else
#include <dirent.h>
#include <sys/stat.h>
static DIR *fault_dir;
static char fault_path[2048];
static int fault_mode, delivered;
static DIR *fixture_opendir(const char *path) {
    DIR *d = opendir(path);
    if (fault_mode && !strcmp(path, fault_path)) fault_dir = d, delivered = 0;
    else fault_dir = NULL;
    return d;
}
static struct dirent *fixture_readdir(DIR *d) {
    if (fault_mode == 1 && d == fault_dir && delivered) { errno = EIO; return NULL; }
    struct dirent *e = readdir(d);
    if (e && d == fault_dir && strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) delivered++;
    return e;
}
static int fixture_fstatat(int fd, const char *name, struct stat *st, int flags) {
    if (fault_mode == 2 && fault_dir && fd == dirfd(fault_dir)) { errno = EIO; return -1; }
    return fstatat(fd, name, st, flags);
}
#define opendir fixture_opendir
#define readdir fixture_readdir
#define fstatat fixture_fstatat
#include "platform/linux.c"
#undef opendir
#undef readdir
#undef fstatat
#endif
#include "host.h"
#undef HOST_OVERRIDE
#define HOST_OVERRIDE(sym, fn)
#ifndef DIRECTORY_FS_SOURCE
#define DIRECTORY_FS_SOURCE "fs_host.c"
#endif
#include DIRECTORY_FS_SOURCE

static int expect_fatal;
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_fatal(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    exit(expect_fatal ? 76 : 77);
}
static int checks, failures;
#define EXPECT(c, name) do { checks++; if (!(c)) { failures++; printf("FAIL %s\n", name); } } while (0)

static void inject(const char *path, int mode) {
    fault_mode = mode;
    delivered = 0;
#ifdef _WIN32
    fault_handle = INVALID_HANDLE_VALUE;
    wchar_t dir[1800];
    assert(MultiByteToWideChar(CP_UTF8, 0, path, -1, dir, 1800));
    swprintf(fault_pattern, 2048, L"%ls\\*", dir);
#else
    fault_dir = NULL;
    snprintf(fault_path, sizeof fault_path, "%s", path);
#endif
}
static void poison_error(void) {
#ifdef _WIN32
    SetLastError(ERROR_ACCESS_DENIED);
#else
    errno = EIO;
#endif
}
static int count_entry(const char *name, int kind, uint64_t size, void *arg) {
    (void)name; (void)kind; (void)size;
    ++*(int *)arg;
    poison_error();
    return 1;
}
static int stop_entry(const char *name, int kind, uint64_t size, void *arg) {
    count_entry(name, kind, size, arg);
    return 0;
}
static void write_text(const char *dir, const char *name, const char *text) {
    char *p = join(dir, name);
    PlatFile *f = plat_fs_create(p, NULL);
    assert(f && plat_file_pwrite(f, text, (uint32_t)strlen(text), 0) == (int64_t)strlen(text));
    plat_file_close(f);
    free(p);
}
static int has_text(const char *dir, const char *name, const char *text) {
    char *p = join(dir, name);
    PlatFile *f = plat_fs_open(p, 0, NULL);
    char buf[64] = {0};
    int ok = f && plat_file_pread(f, buf, sizeof buf, 0) == (int64_t)strlen(text) && !strcmp(buf, text);
    if (f) plat_file_close(f);
    free(p);
    return ok;
}
int main(int argc, char **argv) {
    assert(argc == 2 || argc == 3);
    snprintf(work_root, sizeof work_root, "%s/source", argv[1]);
    snprintf(save_root, sizeof save_root, "%s/committed", argv[1]);
    snprintf(tx_root, sizeof tx_root, "%s/tx", argv[1]);
    snprintf(new_root, sizeof new_root, "%s/new", tx_root);
    snprintf(old_root, sizeof old_root, "%s/old", tx_root);
    snprintf(backup_root, sizeof backup_root, "%s/backups", argv[1]);
    make_dirs(work_root); make_dirs(save_root); make_dirs(tx_root); make_dirs(backup_root);
    write_text(work_root, "a", "new-a"); write_text(work_root, "b", "new-b");
    write_text(save_root, "a", "old-a"); write_text(save_root, "b", "old-b");
    if (argc == 3 && !strcmp(argv[2], "recovery")) {
        char *one = join(tx_root, "ready-0000000001"), *two = join(tx_root, "ready-0000000002");
        make_dirs(one); make_dirs(two);
        write_text(one, "a", "ready-one"); write_text(two, "a", "ready-two");
        free(one); free(two);
        inject(tx_root, 1);
        expect_fatal = 1;
        save_recover();
        printf("FAIL startup continued after an incomplete ready scan\n");
        return 1;
    }
    if (argc == 3 && !strcmp(argv[2], "backups")) {
        make_dirs(old_root);
        write_text(old_root, "a", "old-backup");
        for (int i = 0; i < 4; i++) {
            char name[16]; snprintf(name, sizeof name, "%d", i + 1);
            char *path = join(backup_root, name);
            make_dirs(path); write_text(path, "a", name); free(path);
        }
        inject(backup_root, 1);
        rotate_backup();
        inject(backup_root, 0);
        EXPECT(has_text(old_root, "a", "old-backup"), "backup scan failure retains the pending old save");
        int count = 0;
        EXPECT(plat_fs_list(backup_root, count_entry, &count) == PLAT_FS_OK && count == 4,
               "backup scan failure does not add or prune backups");
        printf("backup enumeration: %d checks, %d failures\n", checks, failures);
        return failures != 0;
    }
    int count = 0;
    EXPECT(plat_fs_list(work_root, count_entry, &count) == PLAT_FS_OK && count == 2, "normal listing ignores callback error state");
    count = 0;
    EXPECT(plat_fs_list(work_root, stop_entry, &count) == PLAT_FS_OK && count == 1, "callback stop succeeds");
    inject(work_root, 1);
    count = 0;
    EXPECT(plat_fs_list(work_root, count_entry, &count) != PLAT_FS_OK && count == 1, "enumeration error is not successful partial listing");
    inject(work_root, 1);
    char copy[1100]; snprintf(copy, sizeof copy, "%s/copy", argv[1]);
    int nf = 0, nl = 0; uint64_t bytes = 0;
    EXPECT(copy_tree(work_root, copy, NULL, 0, &nf, &nl, &bytes) != 0, "copy rejects incomplete listing");
    inject(work_root, 1);
    EXPECT(save_commit_locked() != 0, "commit rejects incomplete listing");
    inject(work_root, 0);
    EXPECT(has_text(save_root, "a", "old-a") && has_text(save_root, "b", "old-b"), "failed commit preserves both previous files");
#ifndef _WIN32
    inject(work_root, 2);
    count = 0;
    EXPECT(plat_fs_list(work_root, count_entry, &count) != PLAT_FS_OK && count == 0, "metadata failure is not skipped");
    inject(work_root, 0);
#endif
    EXPECT(save_commit_locked() == 0, "normal commit succeeds after failure");
    EXPECT(has_text(save_root, "a", "new-a") && has_text(save_root, "b", "new-b"), "normal commit publishes both new files");
    printf("directory enumeration: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}