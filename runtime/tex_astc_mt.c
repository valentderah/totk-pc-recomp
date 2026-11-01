// astc_decode_ldr_mt: astc_decode_ldr with the block rows of one image spread over a persistent worker pool. Each block
// goes through astc_decode_block and lands where astc_decode_ldr puts it, so the output is the same bytes. The caller
// takes part and returns when every row is done. One image at a time: a second caller while the pool is busy, or an
// image under MT_MIN_BLOCKS, decodes on its own thread.
#include <string.h>

#include "host.h"
#include "platform/platform.h"
#include "tex.h"
#include "platform/platform.h"

#define MT_MIN_BLOCKS 1024   // below this, waking the pool costs about as much as the decode
#define MT_MAX_THREADS 8

static struct {
    PlatOnce once;
    PlatMutex busy;                    // held by the caller for the whole image
    PlatMutex lock;
    PlatCond work, done;
    int threads;                             // workers, the caller not counted
    uint64_t gen;                            // bumped per image; a worker joins each generation once
    int active;                              // workers that have not finished the current generation
    // the current image (written under lock before gen is bumped)
    const uint8_t *blocks;
    uint8_t *out;
    uint32_t width, height, bw, bh, bx, by, chunk;
    bool srgb;
    volatile int32_t next;                      // first block row not yet claimed
} P = {PLAT_ONCE_INIT, .lock = PLAT_MUTEX_INIT, .work = PLAT_COND_INIT, .done = PLAT_COND_INIT};

// Block rows [j0, j1) of the current image, as astc_decode_ldr's loop does them.
static void decode_rows(uint32_t j0, uint32_t j1) {
    uint32_t bw = P.bw, bh = P.bh, width = P.width, height = P.height;
    uint8_t tmp[12 * 12 * 4];
    for (uint32_t j = j0; j < j1; j++) {
        for (uint32_t i = 0; i < P.bx; i++) {
            const uint8_t *src = P.blocks + 16 * ((size_t)j * P.bx + i);
            uint32_t x0 = i * bw, y0 = j * bh;
            uint32_t cw = width - x0 < bw ? width - x0 : bw, ch = height - y0 < bh ? height - y0 : bh;
            uint8_t *dst = width == bw && ch == bh ? P.out + (size_t)y0 * width * 4 : tmp;
            astc_decode_block(src, bw, bh, P.srgb, dst);
            if (dst == tmp)
                for (uint32_t y = 0; y < ch; y++)
                    memcpy(P.out + ((size_t)(y0 + y) * width + x0) * 4, tmp + y * bw * 4, cw * 4);
        }
    }
}

static void claim_rows(void) {
    for (;;) {
        int32_t j0 = plat_xadd32(&P.next, (int32_t)P.chunk);
        if ((uint32_t)j0 >= P.by) return;
        uint32_t j1 = (uint32_t)j0 + P.chunk;
        decode_rows((uint32_t)j0, j1 < P.by ? j1 : P.by);
    }
}

static void worker(void *arg) {
    (void)arg;
    uint64_t seen = 0;
    plat_mutex_lock(&P.lock);
    for (;;) {
        while (P.gen == seen) plat_cond_wait(&P.work, &P.lock);
        seen = P.gen;
        plat_mutex_unlock(&P.lock);
        claim_rows();
        plat_mutex_lock(&P.lock);
        if (!--P.active) plat_cond_broadcast(&P.done);
    }
    return;
}

// Hardware threads minus 2 (the render and the main guest thread), 1 to MT_MAX_THREADS; the caller is one of them.
static void pool_init(void *param) {
    (void)param;
    int cpus = (int)plat_cpu_usable() - 2;
    int n = (cpus < 1 ? 1 : cpus > MT_MAX_THREADS ? MT_MAX_THREADS : cpus) - 1;
    for (int i = 0; i < n; i++) {
        if (!plat_thread_spawn(worker, NULL, "astc", 0)) break;
        P.threads++;
    }
    rt_log("tex_astc: %d ASTC decode workers", P.threads);
    return;
}

void astc_decode_ldr_mt(const uint8_t *blocks, uint32_t width, uint32_t height, uint32_t bw, uint32_t bh, bool srgb,
                        uint8_t *rgba_out) {
    if (bw < 4 || bh < 4 || bw > 12 || bh > 12) return;
    uint32_t bx = (width + bw - 1) / bw, by = (height + bh - 1) / bh;
    if ((uint64_t)bx * by < MT_MIN_BLOCKS || by < 2) {
        astc_decode_ldr(blocks, width, height, bw, bh, srgb, rgba_out);
        return;
    }
    plat_once(&P.once, pool_init, NULL);
    if (!P.threads || !plat_mutex_trylock(&P.busy)) {
        astc_decode_ldr(blocks, width, height, bw, bh, srgb, rgba_out);
        return;
    }
    plat_mutex_lock(&P.lock);
    P.blocks = blocks, P.out = rgba_out, P.width = width, P.height = height, P.bw = bw, P.bh = bh, P.bx = bx, P.by = by;
    P.srgb = srgb;
    uint32_t parts = (uint32_t)(P.threads + 1) * 4;   // a few chunks per thread evens out uneven blocks
    P.chunk = by / parts ? by / parts : 1;
    P.next = 0;
    P.active = P.threads;
    P.gen++;
    plat_cond_broadcast(&P.work);
    plat_mutex_unlock(&P.lock);
    claim_rows();
    plat_mutex_lock(&P.lock);
    while (P.active) plat_cond_wait(&P.done, &P.lock);
    plat_mutex_unlock(&P.lock);
    plat_mutex_unlock(&P.busy);
}
