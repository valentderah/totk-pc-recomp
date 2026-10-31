// totk.exe --probe-vkmem: texture memory checks. The CPU part: block-linear copies against an
// independent reference, ASTC void-extent blocks, and the NVN -> VkFormat table against GPU0. The GPU part:
// guest heap imported as a VkBuffer, write watch against CPU, GPU and ReadFile writes, the virtual-pool page table,
// and textures uploaded from block-linear storage and read back.
// The reference swizzler walks the GOBs in storage order and places each byte by bit operations, so it shares no
// addressing code with nvn_bl_offset.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "nvn_api.h"
#include "tex.h"
#include "vk.h"
#include "platform/platform.h"

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint8_t rnd8(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint8_t)(g_rng >> 32);
}

// Byte offset inside a 64 x 8 GOB from the bit layout: x[3:0] -> 0-3, y[0] -> 4, x[4] -> 5, y[2:1] -> 6-7, x[5] -> 8.
static unsigned gob_bits(unsigned x, unsigned y) {
    return (x & 15) | (y & 1) << 4 | (x >> 4 & 1) << 5 | (y >> 1 & 3) << 6 | (x >> 5 & 1) << 8;
}

// lin: slices * rows * row_bytes (tight). bl: block-linear storage, pre-filled with a marker.
static void ref_swizzle(const uint8_t *lin, uint8_t *bl, uint32_t row_bytes, uint32_t rows, uint32_t slices,
                        unsigned th, unsigned td) {
    uint32_t bh = 1u << th, bd = 1u << td;
    uint32_t gx = (row_bytes + 63) / 64, by = (rows + 8 * bh - 1) / (8 * bh), bz = (slices + bd - 1) / bd;
    uint8_t *p = bl;
    for (uint32_t kz = 0; kz < bz; kz++)
        for (uint32_t ky = 0; ky < by; ky++)
            for (uint32_t kx = 0; kx < gx; kx++)
                for (uint32_t z = 0; z < bd; z++)          // GOBs of a block: y fastest, then z
                    for (uint32_t g = 0; g < bh; g++) {
                        for (unsigned y = 0; y < 8; y++)
                            for (unsigned x = 0; x < 64; x++) {
                                uint32_t ax = kx * 64 + x, ay = (ky * bh + g) * 8 + y, az = kz * bd + z;
                                if (ax < row_bytes && ay < rows && az < slices)
                                    p[gob_bits(x, y)] = lin[((size_t)az * rows + ay) * row_bytes + ax];
                            }
                        p += 512;
                    }
}

static size_t bl_size(uint32_t row_bytes, uint32_t rows, uint32_t slices, unsigned th, unsigned td) {
    uint32_t bh = 1u << th, bd = 1u << td;
    return (size_t)((row_bytes + 63) / 64) * ((rows + 8 * bh - 1) / (8 * bh)) * ((slices + bd - 1) / bd) * 512 * bh * bd;
}

static unsigned g_cases;

static int check_swizzle(uint32_t row_bytes, uint32_t rows, uint32_t slices, unsigned th, unsigned td, uint32_t bpb) {
    size_t n = (size_t)row_bytes * rows * slices, bn = bl_size(row_bytes, rows, slices, th, td);
    uint8_t *lin = malloc(n), *back = malloc(n), *ref = malloc(bn), *got = malloc(bn);
    for (size_t i = 0; i < n; i++) lin[i] = rnd8();
    memset(ref, 0xEE, bn);
    memset(got, 0xEE, bn);
    ref_swizzle(lin, ref, row_bytes, rows, slices, th, td);
    tex_bl_copy(got, lin, row_bytes, row_bytes, rows, slices, th, td, 0);
    int bad = 0;
    if (memcmp(ref, got, bn)) { printf("probe-vkmem: FAIL swizzle bpb=%u th=%u td=%u %ux%ux%u differs from the reference\n", bpb, th, td, row_bytes, rows, slices); bad = 1; }
    memset(back, 0, n);
    tex_bl_copy(ref, back, row_bytes, row_bytes, rows, slices, th, td, 1);
    if (!bad && memcmp(lin, back, n)) { printf("probe-vkmem: FAIL deswizzle bpb=%u th=%u td=%u %ux%ux%u differs from the source\n", bpb, th, td, row_bytes, rows, slices); bad = 1; }
    g_cases++;
    free(lin); free(back); free(ref); free(got);
    return bad;
}

static int test_deswizzle(void) {
    static const uint32_t bpbs[] = {1, 2, 4, 8, 16};
    for (unsigned b = 0; b < 5; b++)
        for (unsigned th = 0; th <= 5; th++) {
            uint32_t w = 37 * bpbs[b];
            if (check_swizzle(w, 61, 1, th, 0, bpbs[b])) return 1;        // spans several blocks in x and y
            if (check_swizzle(bpbs[b], 1, 1, th, 0, bpbs[b])) return 1;   // one block
            if (check_swizzle(70 * bpbs[b], 9, 1, th, 0, bpbs[b])) return 1;
        }
    for (unsigned td = 1; td <= 3; td++)                                  // 3D: depth 5 against block depths 2, 4, 8
        for (unsigned th = 0; th <= 2; th++)
            if (check_swizzle(23 * 4, 19, 5, th, td, 4)) return 1;

    // Through a descriptor: RGBA8 2D with mip levels, level 1 and 2, layer 0, against the reference on the same geometry.
    nvn_texdesc d = {.target = 1, .format = 0x25, .width = 200, .height = 150, .depth = 1, .levels = 4};
    nvn_texlayout L;
    nvn_tex_layout(&d, &L);
    uint8_t *store = malloc(L.size), *lin;
    for (uint64_t i = 0; i < L.size; i++) store[i] = rnd8();
    for (unsigned lv = 0; lv < 4; lv++) {
        uint32_t rb, rows, sl, bpb; uint64_t base; unsigned th, td;
        if (nvn_deswizzle_geom(&d, lv, 0, &rb, &rows, &sl, &base, &th, &td, &bpb)) { printf("probe-vkmem: FAIL geometry of level %u\n", lv); return 1; }
        size_t pitch = rb + 16, n = (size_t)pitch * rows;
        lin = calloc(1, n);
        if (nvn_deswizzle(&d, lv, 0, store, lin, pitch)) { printf("probe-vkmem: FAIL nvn_deswizzle level %u\n", lv); return 1; }
        size_t bn = bl_size(rb, rows, sl, th, td);
        uint8_t *tight = malloc((size_t)rb * rows), *ref0 = malloc(bn), *ref1 = malloc(bn);
        for (uint32_t y = 0; y < rows; y++) memcpy(tight + (size_t)y * rb, lin + (size_t)y * pitch, rb);
        memset(ref0, 0x00, bn);
        memset(ref1, 0xFF, bn);
        ref_swizzle(tight, ref0, rb, rows, sl, th, td);
        ref_swizzle(tight, ref1, rb, rows, sl, th, td);
        // bytes where both fills agree were written by the reference; the untouched storage must hold the same
        for (size_t i = 0; i < bn; i++)
            if (ref0[i] == ref1[i] && store[base + i] != ref0[i]) { printf("probe-vkmem: FAIL level %u differs from the reference at byte %zu\n", lv, i); return 1; }
        // swizzling the linear copy back leaves the storage unchanged
        uint8_t *keep = malloc(bn);
        memcpy(keep, store + base, bn);
        if (nvn_swizzle(&d, lv, 0, lin, pitch, store)) { printf("probe-vkmem: FAIL nvn_swizzle level %u\n", lv); return 1; }
        if (memcmp(keep, store + base, bn)) { printf("probe-vkmem: FAIL swizzle round trip level %u\n", lv); return 1; }
        free(keep); free(ref0); free(ref1);
        g_cases++;
        free(lin); free(tight);
    }
    free(store);
    return 0;
}

// nvn_texels_box against the whole-level path on random boxes: a box read equals the box cut from the deswizzled level,
// and a box write leaves the storage as deswizzle, overwrite the box, swizzle does (nothing outside the box changes).
static int check_box(const nvn_texdesc *d, unsigned lv, unsigned layer) {
    uint32_t rb, rows, sl, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, lv, layer, &rb, &rows, &sl, &base, &th, &td, &bpb)) { printf("probe-vkmem: FAIL box geometry\n"); return 1; }
    nvn_texlayout L;
    nvn_tex_layout(d, &L);
    uint8_t *store = malloc(L.size), *want = malloc(L.size), *lin = malloc((size_t)rb * rows * sl);
    for (uint64_t i = 0; i < L.size; i++) store[i] = rnd8();
    uint32_t cols = rb / bpb;
    for (int k = 0; k < 8; k++) {
        uint32_t bx = rnd8() % cols, bn = 1 + rnd8() % (cols - bx), y = rnd8() % rows, h = 1 + rnd8() % (rows - y);
        uint32_t z = sl > 1 ? rnd8() % sl : 0, x = bx * bpb, wb = bn * bpb;
        size_t pitch = wb + 8;
        uint8_t *box = malloc(pitch * h), *ref = malloc(pitch * h);
        // read
        if (nvn_deswizzle(d, lv, layer, store, lin, rb)) { printf("probe-vkmem: FAIL box deswizzle\n"); return 1; }
        memset(box, 0xAB, pitch * h), memset(ref, 0xAB, pitch * h);
        for (uint32_t r = 0; r < h; r++) memcpy(ref + r * pitch, lin + ((size_t)z * rows + y + r) * rb + x, wb);
        if (nvn_texels_box(d, lv, layer, x, y, z, wb, h, store, box, pitch, 1) || memcmp(box, ref, pitch * h)) {
            printf("probe-vkmem: FAIL box read format %#x level %u layer %u box %u,%u,%u %ux%u\n", d->format, lv, layer, x, y, z, wb, h);
            return 1;
        }
        // write
        for (size_t i = 0; i < pitch * h; i++) box[i] = rnd8();
        memcpy(want, store, L.size);
        for (uint32_t r = 0; r < h; r++) memcpy(lin + ((size_t)z * rows + y + r) * rb + x, box + r * pitch, wb);
        if (nvn_swizzle(d, lv, layer, lin, rb, want)) { printf("probe-vkmem: FAIL box swizzle\n"); return 1; }
        if (nvn_texels_box(d, lv, layer, x, y, z, wb, h, store, box, pitch, 0) || memcmp(store, want, L.size)) {
            printf("probe-vkmem: FAIL box write format %#x level %u layer %u box %u,%u,%u %ux%u\n", d->format, lv, layer, x, y, z, wb, h);
            return 1;
        }
        g_cases++;
        free(box), free(ref);
    }
    free(store), free(want), free(lin);
    return 0;
}

static int test_box(void) {
    // one format per block size: 1, 8 and 16 bytes per texel, 8 and 16 bytes per 4x4 block
    int fmt[5] = {0}, n = 0;
    static const int want_bpb[5] = {1, 8, 16, 8, 16}, want_bw[5] = {1, 1, 1, 4, 4};
    for (int f = 1; f < 0x200 && n < 5; f++) {
        int bw, bh, bpb = nvn_fmt_bpb(f, &bw, &bh);
        for (int i = 0; i < 5; i++)
            if (!fmt[i] && bpb == want_bpb[i] && bw == want_bw[i] && bh == bw) fmt[i] = f, n++;
    }
    for (int i = 0; i < 5; i++) {
        if (!fmt[i]) continue;
        nvn_texdesc d2 = {.target = 1, .format = fmt[i], .width = 300, .height = 170, .depth = 1, .levels = 3};
        nvn_texdesc da = {.target = 4, .format = fmt[i], .width = 96, .height = 80, .depth = 3, .levels = 2};
        nvn_texdesc d3 = {.target = 2, .format = fmt[i], .width = 72, .height = 40, .depth = 5, .levels = 2};
        for (unsigned lv = 0; lv < 2; lv++)
            if (check_box(&d2, lv, 0) || check_box(&da, lv, 2) || check_box(&d3, lv, 0)) return 1;
    }
    return 0;
}

// ---- ASTC ----
static void void_block(uint8_t b[16], uint16_t r, uint16_t g, uint16_t bl, uint16_t a, int hdr) {
    uint64_t lo = 0xFFFFFFFFFFFFFDFCull;     // 0x1FC mode, LDR, reserved bits 11, extent all ones
    if (hdr) lo |= 0x200;
    memcpy(b, &lo, 8);
    uint16_t c[4] = {r, g, bl, a};
    memcpy(b + 8, c, 8);
}

static unsigned g_astc_blocks;

static int test_astc(void) {
    static const uint8_t dims[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                        {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    for (int i = 0; i < 14; i++) {
        unsigned bw = dims[i][0], bh = dims[i][1];
        // a 3x2 grid of void-extent blocks cropped to a size that cuts the last column and row
        unsigned w = bw * 3 - 1, h = bh * 2 - 1;
        uint8_t blocks[6 * 16];
        uint8_t want[6][4];
        for (int k = 0; k < 6; k++) {
            uint8_t c[4];
            for (int j = 0; j < 4; j++) c[j] = rnd8();
            // UNORM16 = c * 257 gives back c in the top 8 bits, plus noise in the low byte that must be dropped
            uint16_t v[4];
            for (int j = 0; j < 4; j++) v[j] = (uint16_t)(c[j] << 8 | rnd8());
            void_block(blocks + 16 * k, v[0], v[1], v[2], v[3], 0);
            memcpy(want[k], c, 4);
        }
        uint8_t *out = calloc(1, (size_t)w * h * 4);
        for (int s = 0; s < 2; s++) {
            astc_decode_ldr(blocks, w, h, bw, bh, s, out);
            for (unsigned y = 0; y < h; y++)
                for (unsigned x = 0; x < w; x++)
                    if (memcmp(out + ((size_t)y * w + x) * 4, want[y / bh * 3 + x / bw], 4)) {
                        printf("probe-vkmem: FAIL astc %ux%u void extent at (%u,%u)\n", bw, bh, x, y);
                        return 1;
                    }
        }
        free(out);
        // HDR void extent is an error block in LDR: magenta, and astc_decode_block reports it
        uint8_t hb[16], tmp[12 * 12 * 4];
        void_block(hb, 0x1234, 0x5678, 0x9ABC, 0xDEF0, 1);
        if (astc_decode_block(hb, bw, bh, false, tmp) || memcmp(tmp, "\xff\x00\xff\xff", 4)) {
            printf("probe-vkmem: FAIL astc %ux%u HDR void extent is not an error block\n", bw, bh);
            return 1;
        }
        g_astc_blocks += 7;
    }
    // sRGB void extent: the colour is the stored top byte as well (no transfer curve for constant blocks)
    uint8_t b[16], px[16 * 4];
    void_block(b, 0xFF00, 0x8000, 0x0100, 0xFFFF, 0);
    astc_decode_block(b, 4, 4, true, px);
    if (memcmp(px, "\xff\x80\x01\xff", 4)) { printf("probe-vkmem: FAIL astc sRGB void extent\n"); return 1; }
    g_astc_blocks++;
    // the worker-pool decode gives the same bytes: random blocks (every mode, error blocks included), cropped edges
    for (int i = 0; i < 14; i++) {
        unsigned bw = dims[i][0], bh = dims[i][1], w = 1021, h = 517, n = (w + bw - 1) / bw * ((h + bh - 1) / bh);
        uint8_t *blk = malloc((size_t)n * 16), *a = malloc((size_t)w * h * 4), *m = malloc((size_t)w * h * 4);
        for (size_t k = 0; k < (size_t)n * 16; k++) blk[k] = rnd8();
        for (int s = 0; s < 2; s++) {
            memset(a, 0x11, (size_t)w * h * 4), memset(m, 0x22, (size_t)w * h * 4);
            astc_decode_ldr(blk, w, h, bw, bh, s, a);
            astc_decode_ldr_mt(blk, w, h, bw, bh, s, m);
            if (memcmp(a, m, (size_t)w * h * 4)) {
                printf("probe-vkmem: FAIL astc %ux%u: the worker-pool decode differs from astc_decode_ldr\n", bw, bh);
                return 1;
            }
        }
        free(blk), free(a), free(m);
        g_astc_blocks += n;
    }
    return 0;
}

// ---- formats ----
static unsigned n_mapped, n_supported, n_unmapped, n_unsupported, n_fallback;

static int test_formats(void) {
    if (!vk_init()) { printf("probe-vkmem: FAIL vk_init\n"); return 1; }
    printf("probe-vkmem: GPU %s\n", vk_ctx()->props.deviceName);
    for (int f = 0; f <= 0x9C; f++) {
        TexVkFormat t;
        int ok = tex_vk_format(f, &t);
        if (!ok) {
            n_unmapped++;
            printf("probe-vkmem: unmapped %#x: %s\n", f, t.reason ? t.reason : "(no reason)");
            if (!t.reason) return 1;
            continue;
        }
        n_mapped++;
        if (t.vk != t.ideal) n_fallback++;
        int bw, bh, bpb = nvn_fmt_bpb(f, &bw, &bh);
        if (t.astc_bw && (bpb != 16 || bw != t.astc_bw || bh != t.astc_bh)) {
            printf("probe-vkmem: FAIL ASTC format %#x: block %dx%d/%d bytes, table says %ux%u\n", f, bw, bh, bpb, t.astc_bw, t.astc_bh);
            return 1;
        }
        if (tex_vk_format_supported(&t)) n_supported++;
        else {
            n_unsupported++;
            printf("probe-vkmem: unsupported %#x: VkFormat %d lacks features %#x on this GPU\n", f, (int)t.vk, (unsigned)t.need);
        }
    }
    // The GPU-side promises of the fallbacks: RGBA8 and D32S8 must be usable, otherwise the fallback is empty.
    TexVkFormat t;
    tex_vk_format(0x79, &t);
    int ok_astc = tex_vk_format_supported(&t);
    tex_vk_format(0x35, &t);
    int ok_d = tex_vk_format_supported(&t) && t.vk == VK_FORMAT_D32_SFLOAT_S8_UINT;
    if (!ok_astc || !ok_d) { printf("probe-vkmem: FAIL fallback formats (RGBA8 %d, D32S8 %d)\n", ok_astc, ok_d); return 1; }
    return 0;
}

// ---- GPU part ----
#define GCHECK(cond, ...)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("probe-vkmem: FAIL ");                                                                              \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static unsigned g_gpu_checks;
static uint64_t g_gpu_seen, g_rf_seen[2], g_rf_us[2];

static void copy_wait(VkBuffer src, VkDeviceSize so, VkBuffer dst, VkDeviceSize doff, VkDeviceSize n) {
    VkCommandBuffer cb = vk_upload_begin();
    VkBufferCopy rg = {so, doff, n};
    vkCmdCopyBuffer(cb, src, dst, 1, &rg);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
    vk_upload_end();
    vk_upload_flush(1);
}

// A CPU-visible pool over the guest heap: GPU writes land in guest memory, CPU writes are read by the GPU, write watch
// reports the CPU writes and the explicit ones. GPU writes are noted and reported as the replay does
// (vk_mem_gpu_pending, vk_gpu_written at the fence), and a GPU read follows vk_mem_shadow_sync as a submit does: with
// shadow storage that is what moves the bytes; with an import they are already in place.
static int test_import(pool_t *pool) {
    VkCtx *c = vk_ctx();
    uint8_t *m = pool->mem;
    uint64_t a = (uintptr_t)m, pages[64], n;
    GCHECK(vk_pool_init(pool), "vk_pool_init of a CPU-visible pool at %#llx", (unsigned long long)a);
    VkMemSpan sp;
    GCHECK(vk_mem_resolve(a + 0x1000, 0x20000, &sp) && sp.buffer && sp.offset == 0x1000 && sp.avail == pool->size - 0x1000,
           "resolve inside the imported pool (offset %#llx, avail %#llx)", (unsigned long long)sp.offset,
           (unsigned long long)sp.avail);
    GCHECK(!vk_mem_resolve(a + pool->size - 0x10, 0x20, &sp), "a range past the pool end resolved");
    g_gpu_checks += 2;

    VkBuffer sb;
    VmaAllocation sa;
    uint8_t *s = vk_staging(0x20000, &sb, &sa);
    for (int i = 0; i < 0x20000; i++) s[i] = rnd8();
    vmaFlushAllocation(c->vma, sa, 0, VK_WHOLE_SIZE);
    vk_mem_resolve(a + 0x1000, 0x20000, &sp);
    mem_written_pages(a, pool->size, pages, 64);
    vk_mem_gpu_pending(a + 0x1000, 0x20000);
    copy_wait(sb, 0, sp.buffer, sp.offset, 0x20000);
    vk_gpu_written(a + 0x1000, 0x20000);
    GCHECK(!memcmp(m + 0x1000, s, 0x20000), "GPU copy into the imported heap range differs from the source");
    n = mem_written_pages(a, pool->size, pages, 64);
    g_gpu_seen = n;
    printf("probe-vkmem: GPU copy of 128 KiB into the imported heap: equal; write watch reported %llu of 32 pages\n",
           (unsigned long long)n);

    // CPU writes: reported exactly, and the GPU reads them through the same buffer
    for (int i = 0; i < 0x4000; i++) m[0x30000 + i] = rnd8();
    n = mem_written_peek(a, pool->size, pages, 64);
    GCHECK(n == 4 && pages[0] == a + 0x30000 && pages[3] == a + 0x33000, "CPU writes to 4 pages: %llu reported",
           (unsigned long long)n);
    vk_mem_resolve(a + 0x30000, 0x4000, &sp);
    vk_mem_shadow_sync();
    copy_wait(sp.buffer, sp.offset, sb, 0, 0x4000);
    vmaInvalidateAllocation(c->vma, sa, 0, VK_WHOLE_SIZE);
    GCHECK(!memcmp(s, m + 0x30000, 0x4000), "GPU read of CPU-written heap differs");
    vmaDestroyBuffer(c->vma, sb, sa);

    // generations: written after `since` -> 1, then 0, explicit host write -> 1
    uint32_t g0, g1, g2, g3;
    vk_mem_changed(a, pool->size, 0, &g0);
    m[0x8000] = 1;
    GCHECK(vk_mem_changed(a + 0x8000, 0x1000, g0, &g1) == 1, "a CPU write after a generation is not seen");
    GCHECK(vk_mem_changed(a, pool->size, g1, &g2) == 0, "no write since the last generation is reported as written");
    vk_guest_written(a + 0x9000, 4);
    GCHECK(vk_mem_changed(a + 0x9000, 0x1000, g2, &g3) == 1, "an explicit guest_written range is not seen");
    GCHECK(vk_mem_changed(RT_KERNEL_BASE, 0x1000, 0, &g3) == -1, "the kernel region is reported as tracked");
    printf("probe-vkmem: write watch: CPU writes reported page-exact; generations see CPU and explicit writes\n");
    g_gpu_checks += 7;
    return 0;
}

// ReadFile into a watched page, buffered and unbuffered: timed, and whether write watch saw the kernel's write.
static int test_readfile(uint8_t *dst) {
    char dir[PLAT_PATH_MAX], path[PLAT_PATH_MAX + 64];
    GCHECK(plat_temp_dir(dir, sizeof dir), "temp directory");
    snprintf(path, sizeof path, "%s/tvk-%u.tmp", dir, plat_process_id());
    static uint8_t data[0x2000];
    for (size_t i = 0; i < sizeof data; i++) data[i] = rnd8();
    GCHECK(plat_file_write(path, data, sizeof data), "temp file write");
    for (int k = 0; k < 2; k++) {
        uint8_t *p = dst + (size_t)k * 0x2000;
        uint64_t pages[8];
        PlatFile *h = k ? plat_file_open_unbuffered(path) : plat_file_open(path);
        GCHECK(h, "temp file open");
        mem_written_pages((uintptr_t)p, 0x2000, pages, 8);
        uint64_t t0 = plat_now_ns();
        int64_t got = plat_file_pread(h, p, 0x2000, 0);
        uint64_t t1 = plat_now_ns();
        plat_file_close(h);
        GCHECK(got == 0x2000 && !memcmp(p, data, sizeof data), "ReadFile (%s) result", k ? "unbuffered" : "buffered");
        g_rf_seen[k] = mem_written_pages((uintptr_t)p, 0x2000, pages, 8);
        g_rf_us[k] = (t1 - t0) / 1000;
        printf("probe-vkmem: ReadFile %s of 8 KiB into a watched page: %llu us, write watch saw %llu of 2 pages\n",
               k ? "unbuffered" : "buffered", (unsigned long long)g_rf_us[k], (unsigned long long)g_rf_seen[k]);
    }
    plat_file_delete(path);
    g_gpu_checks += 2;
    return 0;
}

// A physical pool, a virtual pool mapping 64 KiB pages of it, resolution and unmapping; the GPU writes through a
// virtual address and the bytes are read through the physical one.
static int test_virtual(pool_t *phys, pool_t *virt) {
    VkCtx *c = vk_ctx();
    GCHECK(vk_pool_init(phys) && vk_pool_init(virt), "vk_pool_init of physical / virtual pools");
    uint64_t p = (uintptr_t)phys->mem, v = (uintptr_t)virt->mem;
    nvn_mapping_t rq[2] = {{(const NVNmemoryPool *)(const void *)phys, 0x20000, 0, 0x20000, 0, 0},
                           {(const NVNmemoryPool *)(const void *)phys, 0, 0x50000, 0x10000, 0, 0}};
    GCHECK(vk_pool_map_virtual(virt, 2, rq), "MapVirtual of 2 requests");
    VkMemSpan ps, sp;
    GCHECK(vk_mem_resolve(p, 0x40000, &ps) && ps.offset == 0 && ps.alloc, "resolve of the physical pool");
    GCHECK(vk_mem_resolve(v + 8, 4, &sp) && sp.buffer == ps.buffer && sp.offset == 0x20008, "virtual page 0 -> %#llx",
           (unsigned long long)sp.offset);
    GCHECK(vk_mem_resolve(v + 0x10010, 0x10, &sp) && sp.offset == 0x30010, "virtual page 1 -> %#llx", (unsigned long long)sp.offset);
    GCHECK(vk_mem_resolve(v + 0x8000, 0x18000, &sp) && sp.avail == 0x18000, "contiguous pages 0-1 (avail %#llx)",
           (unsigned long long)sp.avail);
    GCHECK(!vk_mem_resolve(v + 0x18000, 0x10000, &sp), "a range into unmapped page 2 resolved");
    GCHECK(!vk_mem_resolve(v + 0x20000, 4, &sp), "unmapped page 2 resolved");
    GCHECK(vk_mem_resolve(v + 0x50000, 0x10000, &sp) && sp.offset == 0 && sp.alloc_offset == 0, "virtual page 5 -> %#llx",
           (unsigned long long)sp.offset);
    VkCommandBuffer cb = vk_upload_begin();
    vkCmdFillBuffer(cb, sp.buffer, sp.offset + 0x100, 0x100, 0xA5C3A5C3u);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
    vk_upload_end();
    VkBuffer rb;
    VmaAllocation ra;
    uint32_t *r = vk_staging(0x100, &rb, &ra);
    copy_wait(ps.buffer, 0x100, rb, 0, 0x100);
    vmaInvalidateAllocation(c->vma, ra, 0, VK_WHOLE_SIZE);
    int same = 1;
    for (int i = 0; i < 0x40; i++) same &= r[i] == 0xA5C3A5C3u;
    vmaDestroyBuffer(c->vma, rb, ra);
    GCHECK(same, "a GPU fill through virtual page 5 is not in physical page 0");
    nvn_mapping_t un = {NULL, 0, 0, 0x10000, 0, 0};
    GCHECK(vk_pool_map_virtual(virt, 1, &un), "MapVirtual unmap");
    GCHECK(!vk_mem_resolve(v + 8, 4, &sp), "unmapped page 0 still resolves");
    GCHECK(vk_mem_resolve(v + 0x10000, 4, &sp) && sp.offset == 0x30000, "page 1 lost by unmapping page 0");
    printf("probe-vkmem: virtual pool: 3 pages mapped, 1 unmapped, resolution and a GPU write through the alias correct\n");
    g_gpu_checks += 12;
    return 0;
}

static tex_t make_tex(pool_t *pool, uint64_t off, int format, uint32_t w, uint32_t h, uint32_t levels, uint64_t serial) {
    tex_t t;
    memset(&t, 0, sizeof t);
    t.magic = MAGIC('T', 'E', 'X', 0);
    t.d.target = 1, t.d.format = format, t.d.width = w, t.d.height = h, t.d.depth = 1, t.d.levels = levels;
    nvn_tex_layout(&t.d, &t.L);
    t.pool = pool, t.off = (int64_t)off, t.mem = pool->mem + off, t.serial = serial;
    return t;
}

// Fills every level of t's storage with random texels through the reference swizzler; lin[lv] keeps them tight.
static int fill_storage(const tex_t *t, uint8_t **lin) {
    for (unsigned lv = 0; lv < t->d.levels; lv++) {
        uint32_t rb, rows, sl, bpb; uint64_t base; unsigned th, td;
        GCHECK(!nvn_deswizzle_geom(&t->d, lv, 0, &rb, &rows, &sl, &base, &th, &td, &bpb), "geometry of level %u", lv);
        size_t n = (size_t)rb * rows;
        if (!lin[lv]) lin[lv] = malloc(n);
        for (size_t i = 0; i < n; i++) lin[lv][i] = rnd8();
        ref_swizzle(lin[lv], t->mem + base, rb, rows, 1, th, td);
    }
    return 0;
}

static int read_back(const tex_t *t, unsigned lv, uint8_t *const *lin, int gpu, const char *what) {
    uint32_t rb, rows, sl, bpb; uint64_t base; unsigned th, td;
    nvn_deswizzle_geom(&t->d, lv, 0, &rb, &rows, &sl, &base, &th, &td, &bpb);
    view_t v;
    memset(&v, 0, sizeof v);
    v.set_levels = 1, v.base_level = (uint8_t)lv, v.nlevels = 1;
    NVNcopyRegion r = {0, 0, 0, (int)(t->d.width >> lv ? t->d.width >> lv : 1), (int)(t->d.height >> lv ? t->d.height >> lv : 1), 1};
    size_t n = (size_t)rb * rows;
    uint8_t *got = calloc(1, n);
    int ok = gpu ? vk_texture_read_image(t, &v, &r, got, rb, n) : (vk_read_texels(t, &v, &r, got, rb, n), 1);
    int same = ok && !memcmp(got, lin[lv], n);
    free(got);
    GCHECK(same, "%s level %u (%ux%u, format %#x) read back %s the CPU reference", what, lv, r.width, r.height,
           t->d.format, ok ? "differs from" : "failed against");
    g_gpu_checks++;
    return 0;
}

static int test_textures(pool_t *tpool, pool_t *phys) {
    uint8_t *lin[4] = {0};
    // block-linear RGBA8 with 3 levels in a CPU-visible pool: uploaded through the GPU, read back from the image
    tex_t t = make_tex(tpool, 0, 0x25, 200, 120, 3, 1);
    if (fill_storage(&t, lin)) return 1;
    const VkTexImage *img = vk_texture_get(&t);
    GCHECK(img && img->mirrored && img->levels == 3 && img->format == VK_FORMAT_R8G8B8A8_UNORM, "RGBA8 texture image");
    for (unsigned lv = 0; lv < 3; lv++)
        if (read_back(&t, lv, lin, 1, "RGBA8")) return 1;
    // CPU writes to the storage: write watch -> re-upload on the next use
    uint64_t up0, re0, re1;
    vk_texture_stats(NULL, &up0, &re0);
    if (fill_storage(&t, lin)) return 1;
    for (unsigned lv = 0; lv < 3; lv++)
        if (read_back(&t, lv, lin, 1, "RGBA8 rewritten")) return 1;
    vk_texture_stats(NULL, NULL, &re1);
    GCHECK(re1 == re0 + 1, "storage rewritten: %llu re-uploads (want 1)", (unsigned long long)(re1 - re0));
    if (read_back(&t, 1, lin, 0, "RGBA8 ReadTexels from storage")) return 1;

    // BC1, 64x48: blocks uploaded as they are, read back from the image
    uint8_t *bl[4] = {0};
    tex_t b = make_tex(tpool, 0x40000, 0x42, 64, 48, 1, 2);
    if (fill_storage(&b, bl)) return 1;
    img = vk_texture_get(&b);
    GCHECK(img && img->format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1 texture image");
    if (read_back(&b, 0, bl, 1, "BC1")) return 1;

    // RGBA8 in a physical pool: aliases the pool memory; WriteTexels then ReadTexels through the image
    tex_t g = make_tex(phys, 0, 0x25, 64, 64, 1, 3);
    img = vk_texture_get(&g);
    GCHECK(img && !img->mirrored && img->bound, "texture in a physical pool (mirrored %d, bound %d)", img ? img->mirrored : -1,
           img ? img->bound : -1);
    uint8_t *gl[1] = {malloc(64 * 64 * 4)};
    for (int i = 0; i < 64 * 64 * 4; i++) gl[0][i] = rnd8();
    NVNcopyRegion all = {0, 0, 0, 64, 64, 1};
    vk_write_texels(&g, NULL, &all, gl[0], 256, 256 * 64);
    if (read_back(&g, 0, gl, 0, "physical-pool RGBA8 WriteTexels")) return 1;

    // a slot of a finalized texture follows the live texture at the same storage (MNG hle_nvn.c)
    vk_texture_register(5, &b, NULL);
    vk_texture_release(&b);
    tex_t b2 = make_tex(tpool, 0x40000, 0x25, 16, 12, 1, 4);
    const VkTexImage *live = vk_texture_get(&b2);
    VkImage live_img = live ? live->image : VK_NULL_HANDLE;
    const VkTexImage *slot = vk_texture_by_id(5, NULL, NULL);
    GCHECK(live && slot && slot->image == live_img, "slot 5 does not follow the live texture at its storage");
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    sp.minf = NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR, sp.magf = NVN_MAG_FILTER_LINEAR, sp.ws = sp.wt = sp.wr = NVN_WRAP_MODE_REPEAT;
    sp.lod_max = 1000, sp.aniso = 16;
    vk_sampler_register(0, &sp);
    VkSampler s0 = vk_sampler_by_id(0), s1 = vk_sampler_for(&sp);
    GCHECK(s0 && s0 == s1 && !vk_sampler_by_id(1), "sampler slots");
    g_gpu_checks += 7;
    vk_texture_release(&t), vk_texture_release(&b2), vk_texture_release(&g);
    for (int i = 0; i < 4; i++) free(lin[i]), free(bl[i]);
    free(gl[0]);
    uint64_t images, ups, reups;
    vk_texture_stats(&images, &ups, &reups);
    printf("probe-vkmem: textures: RGBA8 3 levels and BC1 equal to the CPU reference after upload, re-upload after CPU "
           "writes, physical-pool alias WriteTexels/ReadTexels, slot redirect, samplers (%llu images, %llu uploads, "
           "%llu re-uploads)\n", (unsigned long long)images, (unsigned long long)ups, (unsigned long long)reups);
    return 0;
}

static int test_gpu(void) {
    uint64_t heap = 0;
    GCHECK(mem_set_heap_size(16ull << 20, &heap) == 0 && heap, "SetHeapSize 16 MiB");
    uint8_t *h = (uint8_t *)(uintptr_t)heap;
    pool_t pool = {.magic = NVN_POOL_MAGIC, .flags = 0x24, .mem = h + 0x100000, .size = 0x40000, .game_mem = h + 0x100000};
    pool_t tpool = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = h + 0x400000, .size = 0x100000, .game_mem = h + 0x400000};
    pool_t phys = {.magic = NVN_POOL_MAGIC, .flags = 0x189, .mem = (uint8_t *)(uintptr_t)(RT_GPU_VA_BASE + 0x100000000ull),
                   .size = 0x40000, .game_mem = h + 0x800000, .span = 0x40000};
    pool_t virt = {.magic = NVN_POOL_MAGIC, .flags = 0x2a1, .mem = (uint8_t *)(uintptr_t)(RT_GPU_VA_BASE + 0x200000000ull),
                   .size = 0x80000, .span = 0x80000};
    if (test_import(&pool) || test_readfile(pool.mem + 0x38000) || test_virtual(&phys, &virt)) return 1;
    GCHECK(vk_pool_init(&tpool), "vk_pool_init of the texture pool");
    if (test_textures(&tpool, &phys)) return 1;
    VkMemStats st;
    vk_mem_stats(&st);
    GCHECK(st.imported_pools == 2 && st.physical_pools == 1 && st.virtual_pools == 1 && !st.import_failures,
           "pool counts %llu/%llu/%llu, %llu import failures", (unsigned long long)st.imported_pools,
           (unsigned long long)st.physical_pools, (unsigned long long)st.virtual_pools, (unsigned long long)st.import_failures);
    vk_pool_fini(&virt), vk_pool_fini(&phys), vk_pool_fini(&tpool), vk_pool_fini(&pool);
    vk_mem_stats(&st);
    GCHECK(!st.imported_pools && !st.physical_pools && !st.virtual_pools, "pools left after fini");
    g_gpu_checks += 2;
    return 0;
}

int probe_vkmem(void) {
    if (test_deswizzle() || test_box() || test_astc() || test_formats()) return 1;
    printf("probe-vkmem: cpu part passed (%u deswizzle cases, %u astc blocks, formats: %u mapped, %u supported by the GPU, "
           "%u unsupported by the GPU, %u unmapped, %u with a fallback)\n",
           g_cases, g_astc_blocks, n_mapped, n_supported, n_unsupported, n_unmapped, n_fallback);
    int bad = test_gpu();
    vk_mem_shutdown();
    vk_shutdown();
    long errors = vk_validation_errors_total();
    if (bad || errors) {
        printf("probe-vkmem: FAILED (%s, %ld validation errors)\n", bad ? "a GPU check failed" : "GPU checks passed", errors);
        return 1;
    }
    printf("probe-vkmem: passed (%u deswizzle cases, %u astc blocks, %u formats mapped, %u GPU checks, GPU writes seen by "
           "write watch %llu/32 pages, ReadFile seen %llu/2 buffered and %llu/2 unbuffered, 0 validation errors)\n",
           g_cases, g_astc_blocks, n_mapped, g_gpu_checks, (unsigned long long)g_gpu_seen,
           (unsigned long long)g_rf_seen[0], (unsigned long long)g_rf_seen[1]);
    return 0;
}
