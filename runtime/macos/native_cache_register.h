#ifndef TOTK_NATIVE_CACHE_REGISTER_H
#define TOTK_NATIVE_CACHE_REGISTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// The signal fallback and direct reads must describe the same 128-byte lines.
#define NATIVE_CTR_EL0 UINT32_C(0x8555c005)
// Main's real-time/aspect veneers and three input adapters occupy this prefix.
#define NATIVE_MAIN_TEXT_RESERVE 256

static inline bool native_ctr_branch(size_t from, size_t to, uint32_t *word) {
    if ((from | to) & 3) return false;
    if (to >= from) {
        if (to - from >= (UINT64_C(1) << 27)) return false;
    } else if (from - to > (UINT64_C(1) << 27)) return false;
    *word = UINT32_C(0x14000000) | (uint32_t)(((to - from) >> 2) & 0x3ffffff);
    return true;
}

// Replace trapped CTR_EL0 reads with MOVZ/MOVK in unused text padding. Cache
// maintenance, barriers, registers other than Rt and flags are unchanged.
// Validate every site and the entire destination before changing the image.
// A negative result leaves the image intact, so the signal fallback still works.
static inline ptrdiff_t native_fold_ctr(uint8_t *image, size_t text_end,
                                       size_t padding_begin, size_t padding_end, uint32_t value) {
    if (!image || ((text_end | padding_begin) & 3) || text_end > padding_end ||
        padding_begin < text_end) return -1;
    size_t first = padding_begin, next = first, count = 0;
    for (size_t offset = 0; offset < text_end; offset += 4) {
        uint32_t insn, branch;
        memcpy(&insn, image + offset, 4);
        if ((insn & ~31u) != UINT32_C(0xd53b0020)) continue;
        if (next > padding_end || padding_end - next < 12 ||
            !native_ctr_branch(offset, next, &branch) ||
            !native_ctr_branch(next + 8, offset + 4, &branch)) return -1;
        next += 12;
        count++;
    }
    if (!count) return 0;
    for (size_t offset = first; offset < next; offset++)
        if (image[offset]) return -1;
    next = first;
    for (size_t offset = 0; offset < text_end; offset += 4) {
        uint32_t insn, branch, back;
        memcpy(&insn, image + offset, 4);
        if ((insn & ~31u) != UINT32_C(0xd53b0020)) continue;
        unsigned reg = insn & 31;
        native_ctr_branch(offset, next, &branch);
        native_ctr_branch(next + 8, offset + 4, &back);
        uint32_t code[] = {
            UINT32_C(0x52800000) | ((value & 0xffff) << 5) | reg,
            UINT32_C(0x72a00000) | ((value >> 16) << 5) | reg,
            back,
        };
        memcpy(image + next, code, sizeof code);
        memcpy(image + offset, &branch, 4);
        next += sizeof code;
    }
    return (ptrdiff_t)count;
}

#endif
