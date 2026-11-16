// Additional exclusive-monitor cases, in a separate translation unit from excl_mt.py's original tests.
#include <stdio.h>
#include "cpu.h"

static __uint128_t load(Cpu *c, uint64_t a, int width) {
    switch (width) {
    case 8: return ldx8(c, a);
    case 16: return ldx16(c, a);
    case 32: return ldx32(c, a);
    case 64: return ldx64(c, a);
    default: return ldxp128(c, a);
    }
}
static uint32_t store(Cpu *c, uint64_t a, int width, __uint128_t v) {
    switch (width) {
    case 8: return stx8(c, a, v);
    case 16: return stx16(c, a, v);
    case 32: return stx32(c, a, v);
    case 64: return stx64(c, a, v);
    default: return stxp128(c, a, (uint64_t)v, (uint64_t)(v >> 64));
    }
}

#define CHECK(expr) do { if (!(expr)) { printf("exclusive epoch: %s FAIL\n", #expr); fails++; } } while (0)

int excl_epoch_check(uint32_t (*store64)(Cpu *, uint64_t, uint64_t)) {
    int fails = 0;
    const int widths[] = {8, 16, 32, 64, 128};
    uint64_t cell[4] __attribute__((aligned(64))) = {0};
    uint64_t a = (uint64_t)(uintptr_t)cell;
    for (unsigned k = 0; k < sizeof widths / sizeof widths[0]; k++) {
        int width = widths[k];
        Cpu c = {0}, other = {0};
        __uint128_t v = load(&c, a, width);
        uint64_t epoch = c.excl_epoch;
        CHECK(!(epoch & 1));
        CHECK(store(&c, a, width, v) == 0); // Same-value stores still count as writes.
        CHECK(__atomic_load_n(&g_excl_epoch, __ATOMIC_ACQUIRE) == epoch + 2);
        CHECK(store(&c, a, width, v) == 1); // The reservation was consumed.

        v = load(&c, a, width);
        // A successful store in another translation unit must invalidate c, even at an unrelated address.
        uint64_t old = ldx64(&other, a + 16);
        CHECK(store64(&other, a + 16, old) == 0);
        CHECK(store(&c, a, width, v) == 1);

        v = load(&c, a, width);
        epoch = c.excl_epoch;
        old = ldx64(&other, a + 16);
        // An untracked write forces the data CAS to fail after the epoch gate was claimed.
        // This checks gate rollback, not reservation invalidation by ordinary stores.
        stl8(a, (uint8_t)v ^ 1);
        CHECK(store(&c, a, width, v) == 1);
        CHECK(__atomic_load_n(&g_excl_epoch, __ATOMIC_ACQUIRE) == epoch);
        CHECK(store64(&other, a + 16, old) == 0);

        // ABA through each width, including the byte and halfword forms missing from the original test.
        v = load(&c, a, width);
        __uint128_t changed = load(&other, a, width);
        CHECK(store(&other, a, width, changed ^ 1) == 0);
        changed = load(&other, a, width);
        CHECK(store(&other, a, width, changed ^ 1) == 0);
        CHECK(store(&c, a, width, v) == 1);
        printf("exclusive epoch width %3d: control, same-value, cross-TU, rollback, ABA checked\n", width);
    }
    Cpu pair = {0}, other = {0};
    __uint128_t v = ldxp128(&pair, a);
    // Only the high half changes, via 64-bit stores in the other translation unit, then returns to its old value.
    uint64_t hi = ldx64(&other, a + 8);
    CHECK(store64(&other, a + 8, hi ^ 2) == 0);
    hi = ldx64(&other, a + 8);
    CHECK(store64(&other, a + 8, hi ^ 2) == 0);
    CHECK(stxp128(&pair, a, (uint64_t)v, (uint64_t)(v >> 64)) == 1);
    // Deterministically stop a commit both before its data CAS and after it, before publishing the new epoch.
    Cpu owner = {0}, early = {0}, late = {0};
    uint64_t expected = ldx64(&owner, a);
    ldx64(&early, a);
    ldx64(&late, a);
    CHECK(excl_write_begin(owner.excl_epoch));
    CHECK(store64(&early, a, expected) == 1);
    int stored = __atomic_compare_exchange_n((uint64_t *)(uintptr_t)a, &expected, expected, 0,
                                             __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    CHECK(stored);
    CHECK(store64(&late, a, expected) == 1);
    CHECK(excl_write_end(owner.excl_epoch, stored) == 0);
    CHECK(__atomic_load_n(&g_excl_epoch, __ATOMIC_ACQUIRE) == owner.excl_epoch + 2);
    // Match the lifter's CLREX / fiber-switch invalidation without modifying the saved epoch.
    uint64_t untouched = ldx64(&owner, a + 16);
    ldx64(&other, a);
    other.excl_addr = ~0ull;
    CHECK(store64(&other, a, 0) == 1);
    CHECK(__atomic_load_n(&g_excl_epoch, __ATOMIC_ACQUIRE) == owner.excl_epoch);
    CHECK(store64(&owner, a + 16, untouched) == 0);
    printf("exclusive epoch: cross-width high-half ABA, busy commit window and cleared reservation checked, %d failures\n", fails);
    return fails;
}
