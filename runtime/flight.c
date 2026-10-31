// TOTK_FLIGHT=1 flight recorder: a ring of the last TOTK_FLIGHT_FRAMES (600) presents, one record each (interval,
// render-thread blocked time per perf kind, event counts) and a 320x180 thumbnail each (gfx_present.c, blitted on the
// GPU by the present thread). A dump goes to <dir>/<YYYYMMDD-HHMMSS>-f<frame>/: frames.tsv and f<frame>.png per
// thumbnail, written by a background thread. Triggers: F8 while the game window has focus, a frame interval over
// TOTK_FLIGHT_SPIKE_MS (100; 0 off; at most one dump per 10 s), and TOTK_FLIGHT_AT=<frame>[,<frame>...]. A dump holds
// the frames up to FLIGHT_POST presents after the trigger, so the aftermath is in it too. tools/flight_view.py reads it.
//
// Records come from perf_present (prof_win.c) on the render thread; the counts are differences of the TOTK_TRACE=perf
// running totals and of vk_exec_frame. Off, the only cost is the flight_on branch in perf_present and vk_backend.c.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "flight.h"
#include "gfx.h"
#include "host.h"
#include "input.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"

#define FLIGHT_POST 30                 // presents recorded after the trigger before the dump starts
#define FLIGHT_SPIKE_GAP_NS 10000000000ull

// Record columns after frame, time and interval: blocked time per PERF_* kind (us), then the counts.
enum { C_SUBMITS, C_PASSES, C_DRAWS, C_DRAWS_SEEN, C_CLEARS, C_COPIES, C_DISPATCHES, C_XLATED, C_PERF0, C_N = C_PERF0 + PERF_CNT_N };
static const char *const blocked_names[PERF_N] = {"pace_ms", "fence_ms", "drain_ms", "pipe_ms", "xlate_ms", "tex_ms", "astc_ms"};
static const char *const count_names[C_N] = {
    "submits", "passes", "draws", "draws_seen", "clears", "copies", "dispatches", "shaders_xlated",
    // PERF_CNT_* order (host.h)
    "pipe_bg_used", "draws_skip_pipe", "tex_up", "tex_reup", "draws_skip_shader", "pipe_waited", "oneshot_waited",
    "pipe_built", "tex_new", "tex_alias", "wb_done", "wb_skip"};
_Static_assert(PERF_CNT_N == 12, "count_names follows the PERF_CNT_* list");

typedef struct {
    uint64_t frame, t_ns;
    uint32_t interval_us, blocked_us[PERF_N], n[C_N];
} Rec;

typedef struct {
    int n;                      // records, oldest first
    uint64_t trig_frame, trig_interval_us;
    const char *why;
    char dir[PLAT_PATH_MAX];
    Rec r[];
} DumpJob;

int flight_on;
static int ring_n, thumbs_on;
static Rec *ring;
static uint64_t recorded;          // records written so far
static uint64_t spike_ns, t0_ns;
static uint64_t dump_at, trig_frame, trig_interval_us, last_spike_ns;
static const char *trig_why;
static uint64_t at_frames[16];
static int n_at;
static char base_dir[PLAT_PATH_MAX];
static uint64_t prev_cnt[PERF_CNT_N], prev_xlated;
static VkExecFrame prev_exec;
static volatile uint64_t cost_ns, cost_max_ns;   // render-thread time in flight_frame, for the dump log line

static unsigned env_u(const char *name, unsigned def) {
    const char *e = cfg_get(name);
    return e && *e ? (unsigned)strtoul(e, NULL, 10) : def;
}

void flight_init(void) {
    if (strcmp(cfg_value("TOTK_FLIGHT"), "1")) return;
    ring_n = (int)env_u("TOTK_FLIGHT_FRAMES", 600);
    if (ring_n < 64) ring_n = 64;
    if (ring_n > 7200) ring_n = 7200;
    if (!(ring = calloc((size_t)ring_n, sizeof *ring))) { rt_log("flight: no memory for %d records, off", ring_n); return; }
    spike_ns = (uint64_t)cfg_int("TOTK_FLIGHT_SPIKE_MS") * 1000000ull;
    for (const char *p = cfg_get("TOTK_FLIGHT_AT"); p && *p && n_at < 16;) {
        char *end;
        unsigned long long f = strtoull(p, &end, 10);
        if (end == p) break;
        at_frames[n_at++] = f;
        p = *end == ',' ? end + 1 : end;
    }
    const char *d = cfg_get("TOTK_FLIGHT_DIR");
    char tmp[PLAT_PATH_MAX];
    if (d && *d) snprintf(tmp, sizeof tmp, "%s", d);
    else {   // <exe dir>/../flight: build/flight for build/v121*/totk.exe
        char *slash = plat_exe_path(tmp, sizeof tmp) ? plat_last_sep(tmp) : NULL;
        if (slash) snprintf(slash, sizeof tmp - (size_t)(slash - tmp), "/../flight");
        else snprintf(tmp, sizeof tmp, "flight");
    }
    if (!plat_full_path(tmp, base_dir, sizeof base_dir)) snprintf(base_dir, sizeof base_dir, "%s", tmp);
    thumbs_on = env_u("TOTK_FLIGHT_THUMBS", 1) != 0;
    if (thumbs_on) vk_thumbs_enable(ring_n);
    flight_on = 1;
    rt_log("flight: on: last %d frames, %s; dump on F8, on a frame over %llu ms (one per 10 s)%s, to %s", ring_n,
           thumbs_on ? "thumbnails 320x180" : "no thumbnails", (unsigned long long)(spike_ns / 1000000),
           spike_ns ? "" : " (spike trigger off)", base_dir);
    if (thumbs_on)
        rt_log("flight: memory: records %llu KB, thumbnail ring %llu MB (host-visible)",
               (unsigned long long)((size_t)ring_n * sizeof *ring >> 10),
               (unsigned long long)((uint64_t)ring_n * FLIGHT_THUMB_W * FLIGHT_THUMB_H * 4 >> 20));
}

// ---------------------------------------------------------------- PNG (RGB, Paeth rows, fixed-Huffman deflate)
typedef struct { uint8_t *p; size_t n; uint64_t bits; int nb; } Bits;

static void put(Bits *b, uint32_t v, int n) {
    b->bits |= (uint64_t)v << b->nb;
    b->nb += n;
    while (b->nb >= 8) b->p[b->n++] = (uint8_t)b->bits, b->bits >>= 8, b->nb -= 8;
}

static uint32_t rev(uint32_t v, int n) {
    uint32_t r = 0;
    while (n--) r = r << 1 | (v & 1), v >>= 1;
    return r;
}

static void put_sym(Bits *b, int v) {   // RFC 1951 3.2.6 fixed literal/length code
    if (v < 144) put(b, rev(0x30u + (uint32_t)v, 8), 8);
    else if (v < 256) put(b, rev(0x190u + (uint32_t)v - 144, 9), 9);
    else if (v < 280) put(b, rev((uint32_t)v - 256, 7), 7);
    else put(b, rev(0xC0u + (uint32_t)v - 280, 8), 8);
}

static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
                                   131, 163, 195, 227, 258};
static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
                                   1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

#define HBITS 15
// zlib stream of src: one fixed-Huffman block, greedy matches through a hash of the last position of each 3-byte key.
static size_t deflate_fixed(const uint8_t *src, size_t n, uint8_t *out, int32_t *head) {
    Bits b = {out, 0, 0, 0};
    b.p[b.n++] = 0x78, b.p[b.n++] = 0x01;
    put(&b, 1, 1), put(&b, 1, 2);
    for (int i = 0; i < 1 << HBITS; i++) head[i] = -1;
    size_t i = 0;
    while (i < n) {
        size_t len = 0, dist = 0;
        if (i + 3 <= n) {
            uint32_t h = ((uint32_t)src[i] << 16 | (uint32_t)src[i + 1] << 8 | src[i + 2]) * 2654435761u >> (32 - HBITS);
            int32_t c = head[h];
            head[h] = (int32_t)i;
            if (c >= 0 && i - (size_t)c <= 32768) {
                size_t m = 0, lim = n - i < 258 ? n - i : 258;
                while (m < lim && src[(size_t)c + m] == src[i + m]) m++;
                if (m >= 3) len = m, dist = i - (size_t)c;
            }
        }
        if (!len) {
            put_sym(&b, src[i++]);
            continue;
        }
        int k = 28;
        while (lbase[k] > len) k--;
        put_sym(&b, 257 + k);
        if (lext[k]) put(&b, (uint32_t)(len - lbase[k]), lext[k]);
        int j = 29;
        while (dbase[j] > dist) j--;
        put(&b, rev((uint32_t)j, 5), 5);
        if (dext[j]) put(&b, (uint32_t)(dist - dbase[j]), dext[j]);
        for (size_t s = i + 1; s < i + len && s + 3 <= n; s++)
            head[((uint32_t)src[s] << 16 | (uint32_t)src[s + 1] << 8 | src[s + 2]) * 2654435761u >> (32 - HBITS)] = (int32_t)s;
        i += len;
    }
    put_sym(&b, 256);
    if (b.nb) put(&b, 0, 8 - b.nb);
    uint32_t a = 1, s2 = 0;
    for (size_t k = 0; k < n; k++) a = (a + src[k]) % 65521, s2 = (s2 + a) % 65521;
    uint32_t ad = s2 << 16 | a;
    b.p[b.n++] = (uint8_t)(ad >> 24), b.p[b.n++] = (uint8_t)(ad >> 16), b.p[b.n++] = (uint8_t)(ad >> 8), b.p[b.n++] = (uint8_t)ad;
    return b.n;
}

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t h[8] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    memcpy(h + 4, type, 4);
    uint32_t c = crc(crc(~0u, h + 4, 4), data, n) ^ ~0u;
    uint8_t t[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
    fwrite(h, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    fwrite(t, 1, 4, f);
}

static uint8_t paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

typedef struct { uint8_t *raw, *z; int32_t *head; } PngBufs;

static int write_png(const char *path, const uint8_t *rgba, PngBufs *pb) {
    const uint32_t w = FLIGHT_THUMB_W, h = FLIGHT_THUMB_H, row = 1 + 3 * w;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *o = pb->raw + (size_t)y * row;
        o[0] = 4;   // Paeth
        for (uint32_t x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                int v = rgba[4 * ((size_t)y * w + x) + (size_t)c];
                int a = x ? rgba[4 * ((size_t)y * w + x - 1) + (size_t)c] : 0;
                int up = y ? rgba[4 * ((size_t)(y - 1) * w + x) + (size_t)c] : 0;
                int ul = x && y ? rgba[4 * ((size_t)(y - 1) * w + x - 1) + (size_t)c] : 0;
                o[1 + 3 * x + (uint32_t)c] = (uint8_t)(v - paeth(a, up, ul));
            }
    }
    size_t zn = deflate_fixed(pb->raw, (size_t)row * h, pb->z, pb->head);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    uint8_t ihdr[13] = {0, 0, (uint8_t)(w >> 8), (uint8_t)w, 0, 0, (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0};
    fwrite(sig, 1, 8, f);
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", pb->z, (uint32_t)zn);
    chunk(f, "IEND", NULL, 0);
    return fclose(f) == 0;
}

// ---------------------------------------------------------------- dump thread
static void dump_main(void *arg) {
    DumpJob *j = arg;
    uint64_t t_start = plat_now_ms();
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    plat_file_mkdir_p(j->dir);
    if (!plat_dir_exists(j->dir)) {
        rt_log("flight: cannot create %s", j->dir);
        free(j);
        return;
    }
    uint8_t *have = calloc((size_t)j->n, 1);
    int n_thumbs = 0;
    if (thumbs_on && have) {
        if (!crc_table[1])
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                crc_table[i] = c;
            }
        size_t raw_n = (size_t)(1 + 3 * FLIGHT_THUMB_W) * FLIGHT_THUMB_H;
        PngBufs pb = {malloc(raw_n), malloc(raw_n * 9 / 8 + 64), malloc(sizeof(int32_t) << HBITS)};
        uint8_t *px = malloc((size_t)FLIGHT_THUMB_W * FLIGHT_THUMB_H * 4);
        // Oldest first: each slot is copied out long before the presents that go on meanwhile reach it again. The newest
        // few complete only when their present fence has been waited for, so they get a second try.
        for (int pass = 0; pass < 2 && pb.raw && pb.z && pb.head && px; pass++) {
            if (pass) plat_sleep_ms(200);
            for (int i = pass ? (j->n > 8 ? j->n - 8 : 0) : 0; i < j->n; i++) {
                if (have[i] || !vk_thumbs_read(j->r[i].frame, px)) continue;
                char path[PLAT_PATH_MAX + 32];
                snprintf(path, sizeof path, "%s/f%06llu.png", j->dir, (unsigned long long)j->r[i].frame);
                if (write_png(path, px, &pb)) have[i] = 1, n_thumbs++;
            }
        }
        free(px), free(pb.raw), free(pb.z), free(pb.head);
    }
    char path[PLAT_PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/frames.tsv", j->dir);
    FILE *f = fopen(path, "w");
    if (!f) {
        rt_log("flight: cannot write %s", path);
        free(have), free(j);
        return;
    }
    fprintf(f, "# totk flight dump: %s at frame %llu, interval %.1f ms\n", j->why, (unsigned long long)j->trig_frame,
            j->trig_interval_us / 1000.0);
    fprintf(f, "frame\tt_ms\tinterval_ms");
    for (int k = 0; k < PERF_N; k++) fprintf(f, "\t%s", blocked_names[k]);
    for (int k = 0; k < C_N; k++) fprintf(f, "\t%s", count_names[k]);
    fprintf(f, "\tthumb\n");
    for (int i = 0; i < j->n; i++) {
        const Rec *r = &j->r[i];
        fprintf(f, "%llu\t%.1f\t%.2f", (unsigned long long)r->frame, r->t_ns / 1e6, r->interval_us / 1000.0);
        for (int k = 0; k < PERF_N; k++) fprintf(f, "\t%.2f", r->blocked_us[k] / 1000.0);
        for (int k = 0; k < C_N; k++) fprintf(f, "\t%u", r->n[k]);
        fprintf(f, "\t%d\n", have ? have[i] : 0);
    }
    fclose(f);
    rt_log("flight: dump %s: %s at frame %llu (%.1f ms), frames %llu-%llu, %d thumbnails, written in %llu ms; "
           "recorder %.1f us per frame (max %.0f us)", j->dir,
           j->why, (unsigned long long)j->trig_frame, j->trig_interval_us / 1000.0, (unsigned long long)j->r[0].frame,
           (unsigned long long)j->r[j->n - 1].frame, n_thumbs, (unsigned long long)(plat_now_ms() - t_start),
           recorded ? cost_ns / 1e3 / (double)recorded : 0.0, cost_max_ns / 1e3);
    free(have);
    free(j);
    return;
}

static void start_dump(void) {
    int n = recorded < (uint64_t)ring_n ? (int)recorded : ring_n;
    DumpJob *j = n ? malloc(sizeof *j + sizeof(Rec) * (size_t)n) : NULL;
    if (!j) return;
    j->n = n;
    j->trig_frame = trig_frame;
    j->trig_interval_us = trig_interval_us;
    j->why = trig_why;
    for (int i = 0; i < n; i++) j->r[i] = ring[(recorded - (uint64_t)n + (uint64_t)i) % (uint64_t)ring_n];
    struct tm st;
    plat_local_time(&st);
    snprintf(j->dir, sizeof j->dir, "%s/%04d%02d%02d-%02d%02d%02d-f%llu", base_dir, st.tm_year + 1900, st.tm_mon + 1,
             st.tm_mday, st.tm_hour, st.tm_min, st.tm_sec, (unsigned long long)trig_frame);
    if (!plat_thread_spawn(dump_main, j, "flight dump", 0)) rt_log("flight: cannot start the dump thread"), free(j);
}

static void trigger(uint64_t frame, uint64_t interval_ns, const char *why) {
    if (dump_at) return;   // one dump pending at a time; its window covers this frame
    trig_frame = frame;
    trig_interval_us = interval_ns / 1000;
    trig_why = why;
    dump_at = frame + (FLIGHT_POST < ring_n / 4 ? FLIGHT_POST : (uint64_t)ring_n / 4);
}

// ---------------------------------------------------------------- per frame (render thread)
void flight_frame(uint64_t frame, uint64_t now_ns, uint64_t interval_ns, const uint64_t blocked_ns[],
                  const volatile int64_t counts[]) {
    uint64_t c0 = kern_now_ns();
    if (!t0_ns) t0_ns = now_ns;
    Rec *r = &ring[recorded % (uint64_t)ring_n];
    r->frame = frame;
    r->t_ns = now_ns - t0_ns;
    r->interval_us = (uint32_t)(interval_ns / 1000);
    for (int k = 0; k < PERF_N; k++) r->blocked_us[k] = (uint32_t)(blocked_ns[k] / 1000);
    // vk_exec_frame grows until vk_exec_frame_log resets it, which TOTK_TRACE=gfx does right after this call
    const VkExecFrame *x = &vk_exec_frame;
    int reset = (rt_trace & RT_TRACE_GFX) != 0;
    r->n[C_SUBMITS] = (uint32_t)(x->submits - (reset ? 0 : prev_exec.submits));
    r->n[C_PASSES] = (uint32_t)(x->passes - (reset ? 0 : prev_exec.passes));
    r->n[C_DRAWS] = (uint32_t)(x->draws - (reset ? 0 : prev_exec.draws));
    r->n[C_DRAWS_SEEN] = (uint32_t)(x->draws_seen - (reset ? 0 : prev_exec.draws_seen));
    r->n[C_CLEARS] = (uint32_t)(x->clears - (reset ? 0 : prev_exec.clears));
    r->n[C_COPIES] = (uint32_t)(x->copies - (reset ? 0 : prev_exec.copies));
    r->n[C_DISPATCHES] = (uint32_t)(x->dispatches - (reset ? 0 : prev_exec.dispatches));
    prev_exec = *x;
    uint64_t keys, xlated;
    gfx_shader_counts(&keys, &xlated);
    r->n[C_XLATED] = (uint32_t)(xlated - prev_xlated);
    prev_xlated = xlated;
    for (int k = 0; k < PERF_CNT_N; k++) {
        uint64_t v = (uint64_t)__atomic_load_n(&counts[k], __ATOMIC_RELAXED);
        r->n[C_PERF0 + k] = (uint32_t)(v - prev_cnt[k]);
        prev_cnt[k] = v;
    }
    recorded++;

    static int key_was;
    int key = input_flight_key();
    if (key && !key_was) trigger(frame, interval_ns, "F8");
    key_was = key;
    for (int i = 0; i < n_at; i++)
        if (at_frames[i] == frame) trigger(frame, interval_ns, "TOTK_FLIGHT_AT");
    if (spike_ns && interval_ns > spike_ns && recorded > 30 && (!last_spike_ns || now_ns - last_spike_ns >= FLIGHT_SPIKE_GAP_NS)) {
        if (!dump_at) last_spike_ns = now_ns;
        trigger(frame, interval_ns, "spike");
    }
    if (dump_at && frame >= dump_at) {
        start_dump();
        dump_at = 0;
    }
    uint64_t c = kern_now_ns() - c0;
    cost_ns += c;
    if (c > cost_max_ns) cost_max_ns = c;
}
