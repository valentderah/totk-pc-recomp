// The two shape scans of runtime/blueprint_import.c, tested against the real code.
//
// find_index_by_shape and find_fav_by_shape look for the Autobuild history's arrays by their shape rather than by an
// address. A shape is not an identity: a run of 30 int32 that is a permutation of 0..29 describes any array of ranks,
// and {30, 0} - the mask of a save with no favourites - is common in unrelated memory. Picking the nearest of several
// matches is a guess, and the guess is written to: the import rotates whichever array it was handed. So an ambiguous
// match must be refused, and that refusal is what this test pins down.
//
// Refusing is safe by construction: the caller falls back to the save-file path, which matches the file's own ages
// against guest memory and therefore knows which array is the live one; and a refused favourite mask makes the import
// keep the history order instead of evicting a card the player starred.
//
// Built and run by tests/blueprint_import_scan_test.py.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BP_IMPORT_TEST 1
#include "blueprint_import.h"

#define N BP_SLOTS

// From blueprint_import.c's BP_IMPORT_TEST block.
uint64_t bp_test_find_index(uint64_t lo, uint64_t hi);
int bp_test_find_fav(uint64_t lo, uint64_t hi, uint32_t *out);
// From the stubs.
void bp_test_guest(void *p, uint64_t n);
int bp_test_log_lines(void);

static int failures, checks;

static void check(int cond, const char *what) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

// A buffer large enough for the shapes, aligned so a 4-byte read never straddles it.
static uint8_t buf[4096] __attribute__((aligned(64)));

// Write a run of 30 int32 at byte offset off that is a permutation of 0..29, rotated by start.
static void put_ranks(uint64_t off, int start) {
    for (int i = 0; i < N; i++) {
        int32_t v = (int32_t)((i + start) % N);
        memcpy(buf + off + (uint64_t)i * 4, &v, 4);
    }
}

// Write a {count, mask} pair at byte offset off.
static void put_fav(uint64_t off, int32_t count, uint32_t mask) {
    memcpy(buf + off, &count, 4);
    memcpy(buf + off + 4, &mask, 4);
}

static uint64_t base(void) { return (uint64_t)(uintptr_t)buf; }

int main(void) {
    memset(buf, 0, sizeof buf);
    bp_test_guest(buf, sizeof buf);

    // ---- the Index array

    // One run: it is found, and it is the run's own address.
    {
        memset(buf, 0, sizeof buf);
        put_ranks(128, 0);
        const uint64_t at = bp_test_find_index(base(), base() + sizeof buf);
        check(at == base() + 128, "a single run of 30 ranks is found at its own address");
    }

    // Two runs: refused. This is the case the review named - the nearest of several is not the answer.
    {
        memset(buf, 0, sizeof buf);
        put_ranks(128, 0);
        put_ranks(2048, 7);
        const uint64_t at = bp_test_find_index(base(), base() + sizeof buf);
        check(at == 0, "two runs of 30 ranks are refused, not resolved to the nearest");
    }

    // Three runs, one of them adjacent to the anchor: still refused. Distance must not break the tie.
    {
        memset(buf, 0, sizeof buf);
        put_ranks(64, 0);
        put_ranks(1024, 3);
        put_ranks(3000, 11);
        const uint64_t at = bp_test_find_index(base(), base() + sizeof buf);
        check(at == 0, "three runs are refused even though one is nearest to the anchor");
    }

    // A run with a repeated value is not a permutation, so it is not a candidate.
    {
        memset(buf, 0, sizeof buf);
        put_ranks(256, 0);
        for (int i = 0; i < N; i++) {
            int32_t v = 5;
            memcpy(buf + 1536 + (uint64_t)i * 4, &v, 4);
        }
        const uint64_t at = bp_test_find_index(base(), base() + sizeof buf);
        check(at == base() + 256, "a repeated value is not a run of ranks");
    }

    // Nothing at all.
    {
        memset(buf, 0, sizeof buf);
        check(bp_test_find_index(base(), base() + sizeof buf) == 0, "an empty range finds nothing");
    }

    // ---- the favourite mask

    // One pair: found, with its mask.
    {
        memset(buf, 0, sizeof buf);
        put_fav(200, N, 0x5u);
        uint32_t mask = 0xFFFFFFFFu;
        const int ok = bp_test_find_fav(base(), base() + sizeof buf, &mask);
        check(ok && mask == 0x5u, "a single {30, mask} pair is found with its mask");
    }

    // {30, 0} twice: refused. This is the exact shape the review called out as common.
    {
        memset(buf, 0, sizeof buf);
        put_fav(200, N, 0);
        put_fav(1200, N, 0);
        uint32_t mask = 0xFFFFFFFFu;
        const int ok = bp_test_find_fav(base(), base() + sizeof buf, &mask);
        check(!ok, "two {30, 0} pairs are refused, not read as no favourites");
    }

    // A count other than 30 is not this array.
    {
        memset(buf, 0, sizeof buf);
        put_fav(200, 12, 0x3u);
        uint32_t mask = 0;
        check(!bp_test_find_fav(base(), base() + sizeof buf, &mask), "a count other than 30 is not this array");
    }

    // A bit outside the 30 entries is not this array.
    {
        memset(buf, 0, sizeof buf);
        put_fav(200, N, 1u << 30);
        uint32_t mask = 0;
        check(!bp_test_find_fav(base(), base() + sizeof buf, &mask), "a mask bit outside the 30 entries is not this array");
    }

    // More than eight bits is more favourites than the game allows.
    {
        memset(buf, 0, sizeof buf);
        put_fav(200, N, 0x1FFu);   // nine bits
        uint32_t mask = 0;
        check(!bp_test_find_fav(base(), base() + sizeof buf, &mask), "nine favourites is more than the game allows");
    }

    // Eight bits is allowed.
    {
        memset(buf, 0, sizeof buf);
        put_fav(200, N, 0xFFu);
        uint32_t mask = 0;
        const int ok = bp_test_find_fav(base(), base() + sizeof buf, &mask);
        check(ok && mask == 0xFFu, "eight favourites is the most the game allows and is accepted");
    }

    // ---- the refusals are reported

    // A refused match has to be visible in the log, or a silently failed import looks like a broken feature.
    {
        memset(buf, 0, sizeof buf);
        put_ranks(128, 0);
        put_ranks(2048, 7);
        const int before = bp_test_log_lines();
        bp_test_find_index(base(), base() + sizeof buf);
        check(bp_test_log_lines() > before, "an ambiguous Index match logs a line");
    }

    printf("blueprint import scan: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
