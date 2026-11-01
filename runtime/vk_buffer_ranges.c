#include "vk_buffer_ranges.h"
#include <stdlib.h>

static int append(VkBufferCopy **regions, int *count, int *capacity, uint64_t off, uint64_t size) {
    if (*count == *capacity) {
        if (*capacity > 0x1fffffff)
            return 0;
        int next = *capacity ? *capacity * 2 : 1024;
        VkBufferCopy *p = realloc(*regions, (size_t)next * sizeof *p);
        if (!p)
            return 0;
        *regions = p;
        *capacity = next;
    }
    (*regions)[(*count)++] = (VkBufferCopy){off, off, size};
    return 1;
}

int vk_buffer_ranges_append(VkBufferCopy **regions, int *count, int *capacity, uint64_t offset, uint64_t bytes,
                            unsigned mask) {
    if (!bytes)
        return 1;
    if (offset + bytes < offset)
        return 0;
    if (!mask || mask >= 15 || (offset & 15) || (bytes & 3))
        return append(regions, count, capacity, offset, bytes);
    uint64_t end = offset + bytes;
    while (offset < end) {
        if (!(mask >> ((offset & 15) / 4) & 1)) {
            offset += 4;
            continue;
        }
        uint64_t lo = offset;
        do
            offset += 4;
        while (offset < end && (mask >> ((offset & 15) / 4) & 1));
        if (!append(regions, count, capacity, lo, offset - lo))
            return 0;
    }
    return 1;
}

static int by_offset(const void *a, const void *b) {
    const VkBufferCopy *x = a, *y = b;
    return x->srcOffset < y->srcOffset ? -1 : x->srcOffset > y->srcOffset;
}
int vk_buffer_ranges_merge(VkBufferCopy *regions, int count) {
    if (count < 2)
        return count;
    qsort(regions, (size_t)count, sizeof *regions, by_offset);
    int n = 0;
    for (int i = 0; i < count; i++) {
        uint64_t lo = regions[i].srcOffset, hi = lo + regions[i].size;
        if (n && lo <= regions[n - 1].srcOffset + regions[n - 1].size) {
            uint64_t last = regions[n - 1].srcOffset + regions[n - 1].size;
            if (hi > last)
                regions[n - 1].size = hi - regions[n - 1].srcOffset;
        } else
            regions[n++] = regions[i];
    }
    return n;
}
