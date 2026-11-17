// Stubs for the platform and host surface blueprint_import.c touches, with one difference from
// blueprint_rotation_stubs.c: guest memory here is a real buffer the test owns.
//
// The two shape scans walk guest memory through plat_mem_state, so a test that wants to drive them has to make a range
// look committed. bp_test_guest points at the test's own buffer and bp_test_guest_len says how long it is; everything
// outside it reads as uncommitted, which is what the scans already handle.
//
// Built and run by tests/blueprint_import_scan_test.py.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "host.h"
#include "platform/platform.h"

static int g_logged;

void rt_log(const char *fmt, ...) {
    (void)fmt;
    g_logged++;   // counted so a test can see that a refusal was reported, not silent
}

int bp_test_log_lines(void) { return g_logged; }

uint64_t plat_now_ns(void) { return 0; }

// The range the test owns. Committed read-write; nothing else is.
static uint8_t *g_guest;
static uint64_t g_guest_len;

void bp_test_guest(void *p, uint64_t n) {
    g_guest = (uint8_t *)p;
    g_guest_len = n;
}

int plat_mem_state(uint64_t a, uint64_t b, uint64_t *end) {
    (void)b;
    const uint64_t base = (uint64_t)(uintptr_t)g_guest;
    const uint64_t stop = base + g_guest_len;
    if (g_guest && a >= base && a < stop) {
        if (end) *end = stop;
        return PLAT_PROT_RW;
    }
    if (end) *end = a;   // the scan advances past an uncommitted address by one byte, which terminates the walk
    return PLAT_MEM_FREE;
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
