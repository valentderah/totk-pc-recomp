#include "gfx.h"
#include "vk_buffer_ranges.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(GfxShaderRes) == 18, "Shader resource ABI changed");
int main(int argc, char **argv) {
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f)
            return 2;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        rewind(f);
        uint32_t *w = malloc((size_t)n);
        if (!w)
            return 3;
        if (fread(w, 1, (size_t)n, f) != (size_t)n)
            return 4;
        fclose(f);
        GfxShaderRes r[16] = {0};
        for (int i = 0; i < 16; i++)
            r[i].binding = i, r[i].flags = 1;
        gfx_spirv_store_masks(w, (uint64_t)n / 4, r, 16);
        for (int i = 0; i < 16; i++)
            printf("%d:%u\n", i, r[i].write_mask);
        free(w);
        return 0;
    }
    VkBufferCopy *r = NULL;
    int n = 0, cap = 0;
    if (!vk_buffer_ranges_append(&r, &n, &cap, 0, 64, 7))
        return 5;
    n = vk_buffer_ranges_merge(r, n);
    if (n != 4)
        return 6;
    for (int i = 0; i < 4; i++)
        if (r[i].srcOffset != (uint64_t)i * 16 || r[i].size != 12)
            return 7;
    // A later overlapping dense write must restore the holes it actually writes.
    if (!vk_buffer_ranges_append(&r, &n, &cap, 8, 32, 15))
        return 8;
    n = vk_buffer_ranges_merge(r, n);
    if (n != 2 || r[0].srcOffset != 0 || r[0].size != 44 || r[1].srcOffset != 48 || r[1].size != 12)
        return 9;
    // The correct union is [0,44), [48,60): two regions.
    n = 0;
    if (!vk_buffer_ranges_append(&r, &n, &cap, 0, 64, 7) || !vk_buffer_ranges_append(&r, &n, &cap, 0, 64, 8))
        return 10;
    n = vk_buffer_ranges_merge(r, n);
    if (n != 1 || r[0].srcOffset || r[0].size != 64)
        return 11;
    n = 0;
    if (!vk_buffer_ranges_append(&r, &n, &cap, 4, 16, 7))
        return 12;
    if (n != 1 || r[0].srcOffset != 4 || r[0].size != 16)
        return 13;
    n = 0;
    if (!vk_buffer_ranges_append(&r, &n, &cap, 32, 20, 7))
        return 14;
    if (n != 2 || r[0].srcOffset != 32 || r[0].size != 12 || r[1].srcOffset != 48 || r[1].size != 4)
        return 15;
    uint32_t host[8] = {1, 2, 3, 1334, 4, 5, 6, 1335}, gpu[8] = {11, 22, 33, 0, 44, 55, 66, 0};
    n = 0;
    if (!vk_buffer_ranges_append(&r, &n, &cap, 0, sizeof host, 7))
        return 16;
    for (int i = 0; i < n; i++)
        memcpy((char *)host + r[i].dstOffset, (char *)gpu + r[i].srcOffset, (size_t)r[i].size);
    if (host[0] != 11 || host[2] != 33 || host[3] != 1334 || host[4] != 44 || host[7] != 1335)
        return 17;
    free(r);
    puts("buffer range checks passed");
    return 0;
}
