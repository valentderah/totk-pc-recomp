// Stubs for the platform and host surface blueprint_import.c touches, so its pure rotation rule can be linked and run
// without a game, a save, or guest memory. Every one of these is unreachable from the test: it only calls
// bp_rotate_ages, which touches none of them.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "host.h"
#include "platform/platform.h"

void rt_log(const char *fmt, ...) { (void)fmt; }

uint64_t plat_now_ns(void) { return 0; }

int plat_mem_state(uint64_t a, uint64_t b, uint64_t *end) {
    (void)a; (void)b;
    if (end) *end = 0;
    return 0;   // nothing is mapped: every guest access misses, which is fine for a test that makes none
}

int plat_mem_protect(uint64_t a, uint64_t n, int prot) { (void)a; (void)n; (void)prot; return 0; }

const RtGuestLayout *mem_layout(void) { return NULL; }

const RtConfig *rt_config(void) { return NULL; }

void account_uid(uint64_t out[2]) { out[0] = out[1] = 0; }

PlatFile *plat_file_open(const char *path) { (void)path; return NULL; }
PlatFile *plat_file_open_unbuffered(const char *path) { (void)path; return NULL; }
void plat_file_close(PlatFile *f) { (void)f; }
int plat_file_id(PlatFile *f, PlatFileId *id) { (void)f; (void)id; return 0; }
int64_t plat_file_pread(PlatFile *f, void *buf, uint32_t n, uint64_t off) {
    (void)f; (void)buf; (void)n; (void)off;
    return -1;
}

int plat_fs_kind(const char *path) { (void)path; return 0; }
int plat_fs_unlink(const char *path) { (void)path; return -1; }
