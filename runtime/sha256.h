// SHA-256 (FIPS 180-4), streaming. Uses the x86 SHA extensions when the CPU has them, portable C otherwise.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t bytes;      // message bytes so far
    uint8_t buf[64];     // a partial block
    uint32_t used;
} Sha256;

void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t n);
void sha256_final(Sha256 *s, uint8_t out[32]);
void sha256(const void *data, size_t n, uint8_t out[32]);   // the three above in one call
