// TOTK_MV_CHECK=<frames> (test): measures how well DLSS's motion vectors explain the picture, without screenshots or
// judging by eye. Every <frames> DLAA frames, vk_dlaa.c mv_pass copies to host memory the scene light target of that
// frame and of the frame before (before DLSS, so jittered), and the motion image twice: after the camera pass (camera
// only) and after the per-object pass (what DLSS gets). Once the GPU is done, the completion thread compares them:
//   warp error: the frame before, sampled bilinearly at p + mv + d, against this frame at p, as a relative luminance
//     difference |a - b| / (a + b) averaged over pixels; d = last frame's jitter minus this frame's (the vectors are
//     between unjittered positions, the pictures are jittered: content at x is drawn at x + jitter). Lower is better,
//     and the correct vectors give the lowest. Over every pixel: zero motion, the camera's and the final vectors, and
//     the final ones with d subtracted or left out (the jitter convention). Over the object pixels (final and camera
//     differ by more than a quarter pixel): the camera's, the final ones and variants of the object part (final -
//     camera) scaled by k = -1, 0.5, 1.5, 2 or with one axis negated. The variant with the lowest error names the
//     correction the object vectors need, if any ("best").
//   flow: on up to FLOW_MAX object pixels with some texture, the offset whose 5x5 luminance patch of the frame before
//     matches best, searched over a window around both the camera and the final vector and refined to a sub-pixel by
//     a parabola: the measured motion. The end-point error of the camera and final vectors (+ d) against it, median in
//     render pixels, and how often the final vector is the nearer.
//   output (DLSS's picture, which the vectors are for): on about OUT_SAMPLES pixels picked independently of the vectors,
//     the picture's own motion is measured with a wide search (12 px around the camera vector); where it differs from
//     the camera's by over 1.5 px the pixel moves on its own. At each sample, DLSS's output (at the same unjittered
//     place) against this frame's input, as a relative luminance difference: on the moving samples it grows where
//     DLSS drags old colour along wrong vectors (ghosting, smearing), on the static ones it is the plain difference
//     anti-aliasing and upscaling make. Their ratio compares runs with the per-object pass on and off (it does not
//     depend on the pass's own vectors) and modes.
// One "mv_check:" line per check. Disocclusion and changing light add error to every candidate alike, so the comparison
// between candidates counts, not the absolute values. Copies and analysis cost a few ms on check frames only.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_dlaa.h"

#define FLOW_MAX 20000
#define OBJ_MIN 0.25f   // final and camera vectors differ by more than this (render pixels): an object pixel

#define OUT_SAMPLES 4000
enum { PREV, CUR, MV_CAM, MV_FIN, OUT, NBUF };
static struct {
    int period;                  // 0 off, -1 not read yet
    VkBuffer buf[NBUF];
    VmaAllocation mem[NBUF];
    uint8_t *map[NBUF];
    uint64_t cap[NBUF];
    uint32_t w, h, ow, oh;       // ow, oh: DLSS's output in OUT, 0 none this check
    int want_out;                // the check frame's output is to be copied (vk_mvcheck_output)
    char masks[400];             // the check frame's motion draws (vk_mvobj_frame_masks)
    VkFormat cfmt;
    uint64_t n;                  // camera-stage calls (DLAA frames with motion)
    uint64_t prev_frame;         // gfx frame the PREV colour is of, 0 none
    float prev_u[2], u[2], uprev[2];
    int check;                   // this frame is a check frame (camera stage copied MV_CAM)
    int busy;                    // analysis pending: the buffers are not touched
    uint64_t done;
} K = {.period = -1};

static int on(void) {
    if (K.period < 0) {
        const char *v = cfg_get("TOTK_MV_CHECK");
        K.period = v && *v ? atoi(v) : 0;
        if (K.period < 0) K.period = 0;
        if (K.period && K.period < 4) K.period = 4;
        if (K.period) rt_log("mv_check: motion vectors measured every %d frames", K.period);
    }
    return K.period > 0;
}

static int cbytes(VkFormat f) {
    return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ? 4 : f == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 0;
}

// Host buffers of at least the size asked for; a larger size gets new ones and leaves the old (test only).
static int ensure(int i, uint64_t bytes) {
    if (K.cap[i] >= bytes) return 1;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &K.buf[i], &K.mem[i], &info) != VK_SUCCESS || !info.pMappedData) {
        rt_log("mv_check: host buffer of %llu bytes failed: off", (unsigned long long)bytes);
        K.period = 0;
        return 0;
    }
    K.map[i] = info.pMappedData, K.cap[i] = bytes;
    return 1;
}

static void barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 m = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
                          .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT |
                                           VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m};
    vkCmdPipelineBarrier2(cb, &di);
}

static void copy(VkCommandBuffer cb, int i, VkImage img, uint32_t level, uint32_t layer, uint32_t w, uint32_t h) {
    barrier(cb);
    VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, K.buf[i], 1, &r);
    barrier(cb);
}

// ---- decoding

static float half(uint16_t v) {
    uint32_t s = (uint32_t)(v >> 15) << 31, e = v >> 10 & 31, m = v & 1023, f;
    if (!e) return (s ? -1.0f : 1.0f) * ldexpf((float)m, -24);
    f = e == 31 ? s | 0x7f800000u | m << 13 : s | (e + 112) << 23 | m << 13;
    float r;
    memcpy(&r, &f, 4);
    return r;
}

static float ufloat(uint32_t v, int mbits) {   // unsigned 5-bit exponent mini-float
    uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);
    if (e == 31) return 65504.0f;
    return e ? ldexpf(1.0f + (float)m / (float)(1u << mbits), (int)e - 15) : ldexpf((float)m / (float)(1u << mbits), -14);
}

static void luminance(const uint8_t *src, VkFormat f, uint32_t n, float *out) {
    for (uint32_t i = 0; i < n; i++) {
        float r, g, b;
        if (f == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
            uint32_t v;
            memcpy(&v, src + 4 * (size_t)i, 4);
            r = ufloat(v & 0x7ff, 6), g = ufloat(v >> 11 & 0x7ff, 6), b = ufloat(v >> 22, 5);
        } else {
            uint16_t c[4];
            memcpy(c, src + 8 * (size_t)i, 8);
            r = half(c[0]), g = half(c[1]), b = half(c[2]);
        }
        float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        out[i] = isfinite(y) && y > 0 ? y : 0;
    }
}

// ---- analysis

typedef struct { const float *cur, *prev; uint32_t w, h; } Pic;

static int sample(const Pic *p, float x, float y, float *out) {   // bilinear, pixel index coordinates
    if (!(x >= 0 && y >= 0 && x <= (float)(p->w - 1) && y <= (float)(p->h - 1))) return 0;
    uint32_t x0 = (uint32_t)x, y0 = (uint32_t)y, x1 = x0 + 1 < p->w ? x0 + 1 : x0, y1 = y0 + 1 < p->h ? y0 + 1 : y0;
    float fx = x - (float)x0, fy = y - (float)y0;
    const float *a = p->prev + (size_t)y0 * p->w, *b = p->prev + (size_t)y1 * p->w;
    *out = (a[x0] * (1 - fx) + a[x1] * fx) * (1 - fy) + (b[x0] * (1 - fx) + b[x1] * fx) * fy;
    return 1;
}

typedef struct { double sum; uint64_t n; } Acc;
static void acc(Acc *a, const Pic *p, uint32_t x, uint32_t y, float mx, float my) {
    float b;
    if (!sample(p, (float)x + mx, (float)y + my, &b)) return;
    float c = p->cur[(size_t)y * p->w + x];
    a->sum += fabsf(c - b) / (c + b + 1e-4f), a->n++;
}
static double mean(const Acc *a) { return a->n ? a->sum / (double)a->n : NAN; }

static float sad5(const Pic *p, uint32_t x, uint32_t y, int dx, int dy) {
    float s = 0;
    for (int j = -2; j <= 2; j++)
        for (int i = -2; i <= 2; i++) {
            int px = (int)x + i + dx, py = (int)y + j + dy;
            if (px < 0 || py < 0 || px >= (int)p->w || py >= (int)p->h) return INFINITY;
            float c = p->cur[(size_t)(y + j) * p->w + x + i], b = p->prev[(size_t)py * p->w + px];
            s += fabsf(c - b) / (c + b + 1e-4f);
        }
    return s;
}

static float parabola(float l, float c, float r) {   // offset of the minimum of three samples at -1, 0, 1
    float den = l - 2 * c + r;
    if (!(den > 1e-9f) || !isfinite(l) || !isfinite(r)) return 0;
    float o = 0.5f * (l - r) / den;
    return o < -0.5f ? -0.5f : o > 0.5f ? 0.5f : o;   // a flat curve says nothing past the neighbours
}

static int by_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

// The object pixels' measured motion (sub-pixel), searched over the box around both vectors with a margin of 3;
// 0 when the patch is flat or the match is not clearly better than the box's average.
static int flow(const Pic *p, uint32_t x, uint32_t y, float ax, float ay, float bx, float by, float out[2]) {
    float mu = 0, var = 0;
    for (int j = -2; j <= 2; j++)
        for (int i = -2; i <= 2; i++) mu += p->cur[(size_t)(y + j) * p->w + x + i];
    mu /= 25;
    for (int j = -2; j <= 2; j++)
        for (int i = -2; i <= 2; i++) {
            float d = p->cur[(size_t)(y + j) * p->w + x + i] - mu;
            var += d * d;
        }
    if (mu <= 0 || sqrtf(var / 25) < 0.05f * mu) return 0;
    int x0 = (int)floorf(fminf(ax, bx)) - 3, x1 = (int)ceilf(fmaxf(ax, bx)) + 3;
    int y0 = (int)floorf(fminf(ay, by)) - 3, y1 = (int)ceilf(fmaxf(ay, by)) + 3;
    if (x1 - x0 > 40 || y1 - y0 > 40) return 0;
    float best = INFINITY, total = 0;
    int bdx = 0, bdy = 0, n = 0;
    for (int dy = y0; dy <= y1; dy++)
        for (int dx = x0; dx <= x1; dx++) {
            float s = sad5(p, x, y, dx, dy);
            if (!isfinite(s)) continue;
            total += s, n++;
            if (s < best) best = s, bdx = dx, bdy = dy;
        }
    if (!n || !isfinite(best) || best > 0.5f * total / (float)n) return 0;
    out[0] = (float)bdx + parabola(sad5(p, x, y, bdx - 1, bdy), best, sad5(p, x, y, bdx + 1, bdy));
    out[1] = (float)bdy + parabola(sad5(p, x, y, bdx, bdy - 1), best, sad5(p, x, y, bdx, bdy + 1));
    return 1;
}

// Totals over every check (logged every TOTAL_EVERY checks): end-point error histograms in HBIN px bins of the object
// samples (camera, final) and of the static samples (the measurement's own noise floor), and the object warp errors.
#define HBIN 0.05f
#define HN 640
#define TOTAL_EVERY 20
#define KN 120   // histogram of per-sample scale ratios, -2 to 4 in steps of 0.05
enum { H_CAM, H_FIN, H_STATIC, NH };
static struct { uint64_t h[NH][HN], n[NH], checks, wrong, kh[KN], kn; double obj[2], objn, knum, kden, out[2], outn[2]; uint64_t cal[3]; } T;   // cal: checks where k=0.5, k=1 fit the static pixels best; checks

static void hist_add(int k, float e) {
    int b = e >= 0 && e < HBIN * HN ? (int)(e / HBIN) : HN - 1;   // NaN and huge errors in the last bin
    T.h[k][b < HN ? b : HN - 1]++, T.n[k]++;
}
static float hist_q(int k, double q) {   // quantile q of histogram k, bin centre
    uint64_t want = (uint64_t)(q * (double)T.n[k]), s = 0;
    for (int b = 0; b < HN; b++)
        if ((s += T.h[k][b]) > want) return ((float)b + 0.5f) * HBIN;
    return NAN;
}

static float kmedian(void) {
    uint64_t s = 0;
    for (int b = 0; b < KN; b++)
        if ((s += T.kh[b]) * 2 > T.kn) return -2 + ((float)b + 0.5f) * 0.05f;
    return NAN;
}

static uint32_t mix(uint32_t x, uint32_t y) {   // a fixed pseudo-random pick of pixels, spread over the picture
    uint32_t v = x * 0x9E3779B1u ^ y * 0x85EBCA77u;
    v ^= v >> 15, v *= 0x2C1B3C6Du, v ^= v >> 12;
    return v;
}

static void analyse(void *arg) {
    (void)arg;
    VmaAllocator vma = vk_ctx()->vma;
    for (int i = 0; i < NBUF; i++) vmaInvalidateAllocation(vma, K.mem[i], 0, VK_WHOLE_SIZE);
    uint32_t w = K.w, h = K.h, n = w * h;
    float *cur = malloc(sizeof(float) * n), *prev = malloc(sizeof(float) * n), *e_cam = NULL, *e_fin = NULL,
          *e_st = NULL;
    if (!cur || !prev) goto out;
    luminance(K.map[CUR], K.cfmt, n, cur);
    luminance(K.map[PREV], K.cfmt, n, prev);
    Pic p = {cur, prev, w, h};
    const uint16_t *A = (const uint16_t *)K.map[MV_CAM], *B = (const uint16_t *)K.map[MV_FIN];
    float dx = K.uprev[0] - K.u[0], dy = K.uprev[1] - K.u[1];
    enum { ZERO, CAM, FIN, FIN_NEG_D, FIN_NO_D, NALL };
    enum { O_CAM, O_FIN, O_KM1, O_K05, O_K15, O_K2, O_NX, O_NY, NOBJ };
    static const char *const oname[NOBJ] = {"camera", "final", "k=-1", "k=0.5", "k=1.5", "k=2", "-x", "-y"};
    Acc all[NALL] = {0}, obj[NOBJ] = {0}, cal[3] = {0};
    // the object pixels first, for the sampling rates: about FLOW_MAX object and FLOW_MAX / 4 static samples
    uint64_t nobj = 0, huge = 0;   // huge: camera vectors over 256 px or not finite
    double cam_len = 0;
    for (uint32_t y = 2; y + 2 < h; y++)
        for (uint32_t x = 2; x + 2 < w; x++) {
            size_t i = (size_t)y * w + x;
            float ax = half(A[2 * i]), ay = half(A[2 * i + 1]), bx = half(B[2 * i]), by = half(B[2 * i + 1]);
            if (isfinite(ax) && isfinite(ay)) cam_len += hypotf(ax, ay);
            huge += !isfinite(ax) || !isfinite(ay) || hypotf(ax, ay) > 256;
            nobj += fabsf(bx - ax) > OBJ_MIN || fabsf(by - ay) > OBJ_MIN;
        }
    uint32_t rate_obj = nobj > FLOW_MAX ? (uint32_t)(0xffffffffull * FLOW_MAX / nobj) : 0xffffffffu;
    uint64_t nst = (uint64_t)n - nobj;
    uint32_t rate_st = nst > FLOW_MAX / 4 ? (uint32_t)(0xffffffffull * (FLOW_MAX / 4) / nst) : 0xffffffffu;
    e_cam = malloc(sizeof(float) * FLOW_MAX * 2), e_fin = malloc(sizeof(float) * FLOW_MAX * 2),
    e_st = malloc(sizeof(float) * FLOW_MAX);
    if (!e_cam || !e_fin || !e_st) goto out;
    int nflow = 0, nearer = 0, tried = 0, nst_f = 0;
    double knum = 0, kden = 0;
    // object samples the final vector misses by more than 1.5 px where the camera's fits better: where and how
    struct { int n; uint32_t x0, y0, x1, y1; double ox, oy, mx, my; } bad = {0, UINT32_MAX, UINT32_MAX, 0, 0, 0, 0, 0, 0};
    for (uint32_t y = 2; y + 2 < h; y++)
        for (uint32_t x = 2; x + 2 < w; x++) {
            size_t i = (size_t)y * w + x;
            float ax = half(A[2 * i]), ay = half(A[2 * i + 1]), bx = half(B[2 * i]), by = half(B[2 * i + 1]);
            if (!isfinite(ax) || !isfinite(ay) || !isfinite(bx) || !isfinite(by)) continue;
            acc(&all[ZERO], &p, x, y, dx, dy);
            acc(&all[CAM], &p, x, y, ax + dx, ay + dy);
            acc(&all[FIN], &p, x, y, bx + dx, by + dy);
            acc(&all[FIN_NEG_D], &p, x, y, bx - dx, by - dy);
            acc(&all[FIN_NO_D], &p, x, y, bx, by);
            float ox = bx - ax, oy = by - ay, f[2];
            if (fabsf(ox) <= OBJ_MIN && fabsf(oy) <= OBJ_MIN) {
                // calibration: static pixels with a small camera vector (known right), at k = 0.5, 1, 1.5 of it
                float al = hypotf(ax, ay);
                if (al > 0.25f && al < 2) {
                    acc(&cal[0], &p, x, y, 0.5f * ax + dx, 0.5f * ay + dy), acc(&cal[1], &p, x, y, ax + dx, ay + dy);
                    acc(&cal[2], &p, x, y, 1.5f * ax + dx, 1.5f * ay + dy);
                }
                if (mix(x, y) > rate_st || nst_f >= FLOW_MAX) continue;
                if (flow(&p, x, y, ax + dx, ay + dy, ax + dx, ay + dy, f)) e_st[nst_f++] = hypotf(ax + dx - f[0], ay + dy - f[1]);
                continue;
            }
            acc(&obj[O_CAM], &p, x, y, ax + dx, ay + dy);
            acc(&obj[O_FIN], &p, x, y, bx + dx, by + dy);
            acc(&obj[O_KM1], &p, x, y, ax - ox + dx, ay - oy + dy);
            acc(&obj[O_K05], &p, x, y, ax + 0.5f * ox + dx, ay + 0.5f * oy + dy);
            acc(&obj[O_K15], &p, x, y, ax + 1.5f * ox + dx, ay + 1.5f * oy + dy);
            acc(&obj[O_K2], &p, x, y, ax + 2 * ox + dx, ay + 2 * oy + dy);
            acc(&obj[O_NX], &p, x, y, ax - ox + dx, by + dy);
            acc(&obj[O_NY], &p, x, y, bx + dx, ay - oy + dy);
            if (mix(x, y) > rate_obj || nflow >= 2 * FLOW_MAX) continue;
            tried++;
            if (!flow(&p, x, y, ax + dx, ay + dy, bx + dx, by + dy, f)) continue;
            float ec = hypotf(ax + dx - f[0], ay + dy - f[1]), ef = hypotf(bx + dx - f[0], by + dy - f[1]);
            e_cam[nflow] = ec, e_fin[nflow] = ef, nflow++;
            nearer += ef < ec;
            if (fabsf(ox) > 0.5f || fabsf(oy) > 0.5f) {   // the scale of the object part the measured motion shows
                float mx = f[0] - ax - dx, my = f[1] - ay - dy, oo = ox * ox + oy * oy, kr = (mx * ox + my * oy) / oo;
                knum += mx * ox + my * oy, kden += oo;
                int kb = (int)floorf((kr + 2) / 0.05f);
                T.kh[kb < 0 ? 0 : kb >= KN ? KN - 1 : kb]++, T.kn++;
            }
            if (ef > 1.5f && ef > ec) {
                bad.n++, bad.ox += ox, bad.oy += oy, bad.mx += f[0] - ax - dx, bad.my += f[1] - ay - dy;
                if (x < bad.x0) bad.x0 = x;
                if (y < bad.y0) bad.y0 = y;
                if (x > bad.x1) bad.x1 = x;
                if (y > bad.y1) bad.y1 = y;
            }
        }
    int best = 0;
    for (int k = 1; k < NOBJ; k++)
        if (obj[k].n && (!obj[best].n || mean(&obj[k]) < mean(&obj[best]))) best = k;
    char line[512];
    int o = snprintf(line, sizeof line, "object pixels %.1f%% (%llu):", 100.0 * (double)nobj / (double)n,
                     (unsigned long long)nobj);
    for (int k = 0; k < NOBJ && o < (int)sizeof line; k++) o += snprintf(line + o, sizeof line - o, " %s %.4f", oname[k], mean(&obj[k]));
    if (o < (int)sizeof line) snprintf(line + o, sizeof line - o, "; best %s", nobj ? oname[best] : "-");
    // DLSS's output against this frame's input, on samples moving on their own and static ones (the picture's motion)
    char outl[200] = "";
    if (K.ow && K.oh) {
        float *ol = malloc(sizeof(float) * (size_t)K.ow * K.oh);
        if (ol) {
            luminance(K.map[OUT], VK_FORMAT_R16G16B16A16_SFLOAT, K.ow * K.oh, ol);
            Pic op = {NULL, ol, K.ow, K.oh};   // sample() reads prev
            float sx = (float)K.ow / (float)w, sy = (float)K.oh / (float)h;
            uint32_t rate = (uint32_t)(0xffffffffull * OUT_SAMPLES / n);
            Acc mov = {0}, sta = {0};
            for (uint32_t y = 2; y + 2 < h; y++)
                for (uint32_t x = 2; x + 2 < w; x++) {
                    if ((mix(x, y) ^ 0x5bd1e995u) * 0x2C1B3C6Du > rate) continue;
                    size_t i = (size_t)y * w + x;
                    float ax = half(A[2 * i]) + dx, ay = half(A[2 * i + 1]) + dy, f[2], ov;
                    if (!isfinite(ax) || !isfinite(ay) || !flow(&p, x, y, ax - 12, ay - 12, ax + 12, ay + 12, f)) continue;
                    if (!sample(&op, ((float)x + 0.5f - K.u[0]) * sx - 0.5f, ((float)y + 0.5f - K.u[1]) * sy - 0.5f, &ov))
                        continue;
                    float c = cur[i], e = fabsf(ov - c) / (ov + c + 1e-4f);
                    Acc *a = hypotf(f[0] - ax, f[1] - ay) > 1.5f ? &mov : &sta;
                    a->sum += e, a->n++;
                }
            snprintf(outl, sizeof outl, "; output against input: moving on their own %.4f (%llu), static %.4f (%llu), "
                     "ratio %.3f", mean(&mov), (unsigned long long)mov.n, mean(&sta), (unsigned long long)sta.n,
                     mean(&mov) / mean(&sta));
            if (mov.n) T.out[0] += mov.sum, T.outn[0] += (double)mov.n;
            if (sta.n) T.out[1] += sta.sum, T.outn[1] += (double)sta.n;
            free(ol);
        }
    }
    float mc = NAN, mf = NAN, ms = NAN;
    for (int k = 0; k < nflow; k++) hist_add(H_CAM, e_cam[k]), hist_add(H_FIN, e_fin[k]);
    for (int k = 0; k < nst_f; k++) hist_add(H_STATIC, e_st[k]);
    if (nflow) {
        qsort(e_cam, (size_t)nflow, sizeof *e_cam, by_float), qsort(e_fin, (size_t)nflow, sizeof *e_fin, by_float);
        mc = e_cam[nflow / 2], mf = e_fin[nflow / 2];
    }
    if (nst_f) qsort(e_st, (size_t)nst_f, sizeof *e_st, by_float), ms = e_st[nst_f / 2];
    char badl[200] = "";
    if (bad.n)
        snprintf(badl, sizeof badl, "; %d missed (over 1.5 px, camera nearer) in x %u-%u y %u-%u, object part (%.2f %.2f), "
                 "measured (%.2f %.2f)", bad.n, bad.x0, bad.x1, bad.y0, bad.y1, bad.ox / bad.n, bad.oy / bad.n,
                 bad.mx / bad.n, bad.my / bad.n);
    rt_log("mv_check: #%llu %ux%u, jitter (%.2f %.2f) after (%.2f %.2f), camera motion %.2f px; warp error, every pixel: "
           "zero %.4f camera %.4f final %.4f (jitter subtracted %.4f, left out %.4f); camera vectors over 256 px %llu; %s; "
           "flow on %d of %d object "
           "samples: median error camera %.2f px, final %.2f px, final nearer %.0f%%; static %.2f px (%d); scale %.2f; calibration, static pixels under 2 px of camera motion (%llu): k=0.5 %.4f k=1 %.4f "
           "k=1.5 %.4f%s%s; motion draws (position banks/changed banks:draws)%s",
           (unsigned long long)++K.done, w, h, K.u[0], K.u[1], K.uprev[0], K.uprev[1], cam_len / (double)n,
           mean(&all[ZERO]), mean(&all[CAM]), mean(&all[FIN]), mean(&all[FIN_NEG_D]), mean(&all[FIN_NO_D]), (unsigned long long)huge, line,
           nflow, tried, mc, mf, nflow ? 100.0 * nearer / nflow : 0.0, ms, nst_f, kden > 0 ? knum / kden : NAN, (unsigned long long)cal[1].n, mean(&cal[0]), mean(&cal[1]), mean(&cal[2]), badl, outl, K.masks);
    if (cal[1].n > 1000) T.cal[0] += mean(&cal[0]) < mean(&cal[1]) && mean(&cal[0]) < mean(&cal[2]), T.cal[1] += mean(&cal[1]) <= mean(&cal[0]) && mean(&cal[1]) <= mean(&cal[2]), T.cal[2]++;
    T.knum += knum, T.kden += kden;
    T.checks++;
    if (obj[O_FIN].n) {
        T.obj[0] += obj[O_CAM].sum, T.obj[1] += obj[O_FIN].sum, T.objn += (double)obj[O_FIN].n;
        T.wrong += nobj * 200 > n && best != O_FIN;   // over 0.5 % of the picture and the vectors are not the best fit
    }
    if (T.checks % TOTAL_EVERY == 0)
        rt_log("mv_check total: %llu checks, %llu with over 0.5%% object pixels fit better by a variant; object warp error "
               "camera %.4f final %.4f; end-point error (median / 90%% / 99%%) object samples camera %.2f / %.2f / %.2f "
               "px, final %.2f / %.2f / %.2f px (%llu); static samples %.2f / %.2f / %.2f px (%llu); object scale %.3f (median %.2f of %llu); calibration: k=0.5 best in %llu, k=1 in %llu of %llu checks; "
               "output against input: moving on their own %.4f (%.0f), static %.4f (%.0f), ratio %.3f",
               (unsigned long long)T.checks, (unsigned long long)T.wrong, T.objn ? T.obj[0] / T.objn : NAN,
               T.objn ? T.obj[1] / T.objn : NAN, hist_q(H_CAM, 0.5), hist_q(H_CAM, 0.9), hist_q(H_CAM, 0.99),
               hist_q(H_FIN, 0.5), hist_q(H_FIN, 0.9), hist_q(H_FIN, 0.99), (unsigned long long)T.n[H_FIN],
               hist_q(H_STATIC, 0.5), hist_q(H_STATIC, 0.9), hist_q(H_STATIC, 0.99), (unsigned long long)T.n[H_STATIC],
               T.kden > 0 ? T.knum / T.kden : NAN, kmedian(), (unsigned long long)T.kn, (unsigned long long)T.cal[0], (unsigned long long)T.cal[1],
               (unsigned long long)T.cal[2], T.outn[0] ? T.out[0] / T.outn[0] : NAN, T.outn[0],
               T.outn[1] ? T.out[1] / T.outn[1] : NAN, T.outn[1],
               T.outn[0] && T.outn[1] ? (T.out[0] / T.outn[0]) / (T.out[1] / T.outn[1]) : NAN);
out:
    free(cur), free(prev), free(e_cam), free(e_fin), free(e_st);
    __atomic_store_n(&K.busy, 0, __ATOMIC_RELEASE);
}

// ---- capture (vk_dlaa.c mv_pass)

void vk_mvcheck_camera(VkCommandBuffer cb, VkImage mv, uint32_t w, uint32_t h, float jx, float jy) {
    K.check = 0;
    if (!on()) return;
    K.n++;
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (K.n % (uint64_t)K.period || __atomic_load_n(&K.busy, __ATOMIC_ACQUIRE) || K.prev_frame + 1 != frame ||
        K.w != w || K.h != h)
        return;
    if (!ensure(MV_CAM, (uint64_t)w * h * 4) || !ensure(MV_FIN, (uint64_t)w * h * 4)) return;
    copy(cb, MV_CAM, mv, 0, 0, w, h);
    K.check = 1, K.u[0] = jx, K.u[1] = jy, K.uprev[0] = K.prev_u[0], K.uprev[1] = K.prev_u[1];
}

void vk_mvcheck_final(VkCommandBuffer cb, uint64_t fence, VkImage mv, VkImage cimg, VkFormat cfmt, uint32_t level,
                      uint32_t layer, uint32_t w, uint32_t h, float jx, float jy) {
    if (!on()) return;
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    int b = cbytes(cfmt);
    int want_prev = (K.n + 1) % (uint64_t)K.period == 0 && !__atomic_load_n(&K.busy, __ATOMIC_ACQUIRE);
    if (!b) return (void)(K.check = 0, K.prev_frame = 0);
    if (K.check) {
        if (!ensure(CUR, (uint64_t)w * h * (uint64_t)b)) return;
        copy(cb, MV_FIN, mv, 0, 0, w, h);
        copy(cb, CUR, cimg, level, layer, w, h);
        K.check = 0, K.prev_frame = 0, K.ow = K.oh = 0, K.want_out = 1;
        snprintf(K.masks, sizeof K.masks, "%s", vk_mvobj_frame_masks());
        __atomic_store_n(&K.busy, 1, __ATOMIC_RELEASE);
        vk_completion_add(fence, analyse, NULL);
        return;
    }
    if (!want_prev) return;
    if (!ensure(PREV, (uint64_t)w * h * (uint64_t)b)) return;
    copy(cb, PREV, cimg, level, layer, w, h);
    K.prev_frame = frame, K.prev_u[0] = jx, K.prev_u[1] = jy, K.w = w, K.h = h, K.cfmt = cfmt;
}

// After DLSS ran on a check frame (vk_dlaa.c evaluate): its output (RGBA16F, ow x oh) for the analysis, which runs once
// the submission is done.
void vk_mvcheck_output(VkCommandBuffer cb, VkImage out, uint32_t ow, uint32_t oh) {
    if (!K.want_out) return;
    K.want_out = 0;
    if (!ensure(OUT, (uint64_t)ow * oh * 8)) return;
    copy(cb, OUT, out, 0, 0, ow, oh);
    K.ow = ow, K.oh = oh;
}
