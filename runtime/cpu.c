#include "cpu.h"

// One epoch for every Cpu, access width and generated translation unit. Align it to a cache line.
uint64_t g_excl_epoch __attribute__((aligned(64)));

// Keep the entire commit outside generated code: kernel.c ap_try may asynchronously park a thread at any
// generated instruction. Parking an odd-epoch owner could strand every LDX on a higher-priority virtual core.
// Keep these functions out of line; kernel.c ap_init also checks their linked addresses.
#define STX(N, T) \
    __attribute__((noinline)) uint32_t stx##N(Cpu *c, uint64_t a, uint64_t v) { \
        if (c->excl_addr != a) return 1; \
        c->excl_addr = ~0ull; \
        if (!excl_write_begin(c->excl_epoch)) return 1; \
        T expected = (T)c->excl_val; \
        int stored = __atomic_compare_exchange_n((T *)(uintptr_t)a, &expected, (T)v, 0, \
                                                 __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
        return excl_write_end(c->excl_epoch, stored); \
    }
STX(8, uint8_t)
STX(16, uint16_t)
STX(32, uint32_t)
STX(64, uint64_t)

__attribute__((noinline)) uint32_t stxp128(Cpu *c, uint64_t a, uint64_t lo, uint64_t hi) {
    if (c->excl_addr != a) return 1;
    c->excl_addr = ~0ull;
    if (!excl_write_begin(c->excl_epoch)) return 1;
    __uint128_t expected = ((__uint128_t)c->excl_val_hi << 64) | c->excl_val;
    __uint128_t v = ((__uint128_t)hi << 64) | lo;
    int stored = __atomic_compare_exchange_n((__uint128_t *)(uintptr_t)a, &expected, v, 0, __ATOMIC_SEQ_CST,
                                            __ATOMIC_SEQ_CST);
    return excl_write_end(c->excl_epoch, stored);
}
