// totk.exe --probe-movie: the host movie::Decoder (ov_movie.c) on one TotK movie from the RomFS.
// A small Matroska/WebM demuxer reads the VP9 frames of Event/Movie/<name>.webm (the smallest one, or the first whose
// path contains TOTK_PROBE_MOVIE). The probe then drives the decoder through the same HOST_OVERRIDE entries the game
// calls, with a Cpu of its own; the guest functions the decoder calls (nn::os::SignalEvent, movie::Buffer,
// movie::MediaData) are host stand-ins registered as host functions, so rt_call reaches them as it reaches the SDK's.
//   1. unpaced: every frame in, every frame out: frame count, size, output format, PTS order, events, decode cost;
//   2. paced at 60 presents per second (fake clock): at most one frame per present, one frame per 2 presents, no drops;
//   3. paced at 15 presents per second: late frames are dropped, the frame handed over is the one due.
// Without the RomFS (no game data) it prints why and exits 0. TOTK_PROBE_MOVIE_SHOT=<file.ppm> saves the middle frame.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "fs.h"
#include "host.h"
#include "movie.h"
#include "platform/platform.h"

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("probe-movie: FAILED: ");                                                                           \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)
#define U(x) ((uint64_t)(uintptr_t)(x))

// ---- WebM: the VP9 track's frames
typedef struct { uint32_t off, size; int64_t pts; } Frame;   // pts in microseconds
typedef struct {
    Frame *f;
    int n, cap, w, h;
    char codec[32];
} Track;

// EBML variable-length integer: an element ID keeps its length marker, a size drops it (all ones = unknown size).
static int vint(const uint8_t *p, const uint8_t *end, int id, uint64_t *v, int *len, int *unknown) {
    if (p >= end) return 0;
    int n = 1;
    uint8_t mask = 0x80;
    while (n <= 8 && !(p[0] & mask)) { mask >>= 1; n++; }
    if (n > 8 || p + n > end) return 0;
    uint64_t x = id ? p[0] : (uint64_t)(p[0] & (mask - 1));
    for (int i = 1; i < n; i++) x = x << 8 | p[i];
    *v = x;
    *len = n;
    if (unknown) *unknown = !id && x == (1ull << (7 * n)) - 1;
    return 1;
}
static uint64_t be_uint(const uint8_t *p, uint64_t n) {
    uint64_t x = 0;
    for (uint64_t i = 0; i < n && i < 8; i++) x = x << 8 | p[i];
    return x;
}

// Walks the file linearly, entering the master elements on the way to the frames and skipping the rest.
static const char *demux(const uint8_t *d, size_t size, Track *t) {
    const uint8_t *end = d + size, *p = d;
    uint64_t scale = 1000000, cluster = 0, cur_track = 0, vtrack = 0;
    int have_cluster = 0;
    while (p < end) {
        uint64_t id, n;
        int il, sl, unknown;
        if (!vint(p, end, 1, &id, &il, NULL) || !vint(p + il, end, 0, &n, &sl, &unknown)) return "bad element header";
        const uint8_t *body = p + il + sl;
        switch (id) {
        case 0x18538067: case 0x1549A966: case 0x1654AE6B: case 0xAE: case 0xE0: case 0xA0:   // Segment, Info, Tracks,
            p = body;                                                                         // TrackEntry, Video,
            continue;                                                                         // BlockGroup
        case 0x1F43B675: p = body; have_cluster = 1; continue;                               // Cluster
        }
        if (unknown || n > (uint64_t)(end - body)) return "element past the end of the file";
        switch (id) {
        case 0x2AD7B1: scale = be_uint(body, n); break;                                      // TimecodeScale (ns)
        case 0xD7: cur_track = be_uint(body, n); break;                                      // TrackNumber
        case 0x86:                                                                           // CodecID
            if (n < sizeof t->codec && !t->codec[0]) {
                memcpy(t->codec, body, n);
                t->codec[n] = 0;
                vtrack = cur_track;
            }
            break;
        case 0xB0: t->w = (int)be_uint(body, n); break;                                      // PixelWidth
        case 0xBA: t->h = (int)be_uint(body, n); break;                                      // PixelHeight
        case 0xE7: cluster = be_uint(body, n); break;                                        // Cluster Timecode
        case 0xA3: case 0xA1: {                                                              // SimpleBlock, Block
            uint64_t track;
            int tl;
            if (!have_cluster || !vint(body, body + n, 0, &track, &tl, NULL) || n < (uint64_t)tl + 3) return "bad block";
            if (track != vtrack) break;
            int16_t rel = (int16_t)(body[tl] << 8 | body[tl + 1]);
            if (body[tl + 2] & 0x06) return "laced video block";
            if (t->n == t->cap) {
                t->cap = t->cap ? 2 * t->cap : 1024;
                if (!(t->f = realloc(t->f, (size_t)t->cap * sizeof *t->f))) return "frame list allocation";
            }
            int64_t tc = (int64_t)cluster + rel;
            t->f[t->n++] = (Frame){(uint32_t)(body + tl + 3 - d), (uint32_t)(n - tl - 3), tc * (int64_t)scale / 1000};
            break;
        }
        }
        p = body + n;
    }
    return NULL;
}

// ---- stand-ins for the guest functions
typedef struct { uint64_t base, cap; int off, size; } PBuf;    // movie::Buffer
typedef struct {
    int n;
    struct { char key[32]; int kind; int i; int r[4]; char s[64]; } e[32];
} PMd;                                                         // movie::MediaData
enum { K_INT = 1, K_STR, K_RECT };

static void *md_slot(PMd *m, const char *k, int create) {
    for (int i = 0; i < m->n; i++)
        if (!strcmp(m->e[i].key, k)) return &m->e[i];
    if (!create || m->n == 32 || strlen(k) >= 32) return NULL;
    memset(&m->e[m->n], 0, sizeof m->e[0]);
    strcpy(m->e[m->n].key, k);
    return &m->e[m->n++];
}
#define MDE(p) ((__typeof__(&((PMd *)0)->e[0]))(p))
static void s_signal(Cpu *c) { plat_inc32((volatile int32_t *)arg_p(c, 0)); }
static void s_set_range(Cpu *c) { PBuf *b = arg_p(c, 0); b->off = (int)arg_x(c, 1); b->size = (int)arg_x(c, 2); }
static void s_set_data(Cpu *c) { PBuf *b = arg_p(c, 0); b->base = arg_x(c, 1); b->cap = arg_x(c, 2); }
static void s_base(Cpu *c) { ret_x(c, ((PBuf *)arg_p(c, 0))->base); }
static void s_capacity(Cpu *c) { ret_x(c, ((PBuf *)arg_p(c, 0))->cap); }
static void s_find_int32(Cpu *c) {
    void *e = md_slot(arg_p(c, 0), arg_p(c, 1), 0);
    int ok = e && MDE(e)->kind == K_INT;
    if (ok) *(int *)arg_p(c, 2) = MDE(e)->i;
    ret_x(c, (uint64_t)ok);
}
static void s_find_string(Cpu *c) {
    void *e = md_slot(arg_p(c, 0), arg_p(c, 1), 0);
    int ok = e && MDE(e)->kind == K_STR;
    if (ok) *(uint64_t *)arg_p(c, 2) = U(MDE(e)->s);
    ret_x(c, (uint64_t)ok);
}
static void s_set_int32(Cpu *c) {
    void *e = md_slot(arg_p(c, 0), arg_p(c, 1), 1);
    if (e) { MDE(e)->kind = K_INT; MDE(e)->i = (int)arg_x(c, 2); }
}
static void s_set_rect(Cpu *c) {
    void *e = md_slot(arg_p(c, 0), arg_p(c, 1), 1);
    if (e) {
        MDE(e)->kind = K_RECT;
        for (int i = 0; i < 4; i++) MDE(e)->r[i] = (int)arg_x(c, 2 + i);
    }
}
static void s_set_string(Cpu *c) {
    void *e = md_slot(arg_p(c, 0), arg_p(c, 1), 1);
    if (e) {
        MDE(e)->kind = K_STR;
        snprintf(MDE(e)->s, sizeof MDE(e)->s, "%s", (const char *)arg_p(c, 2));
    }
}
static volatile int32_t g_d0, g_d2;
static void s_orig_d0(Cpu *c) { (void)c; plat_inc32(&g_d0); }   // the library's deleting destructor
static void s_orig_d2(Cpu *c) { (void)c; plat_inc32(&g_d2); }
static void s_vtable(Cpu *c) { (void)c; rt_fatal("probe-movie: the decoder vtable was called"); }

// ---- driving the entries as the game does
static GuestFn E[MOVIE_ENTRIES];
static Cpu *C;
static uint64_t call(int which, int n, ...) {
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) C->x[i] = va_arg(ap, uint64_t);
    va_end(ap);
    E[which](C);
    return C->x[0];
}
static uint64_t fake_now;
static uint64_t fake_clock(void) { return fake_now; }
static int md_int(PMd *m, const char *k) {
    void *e = md_slot(m, k, 0);
    return e && MDE(e)->kind == K_INT ? MDE(e)->i : -1;
}

// TOTK_PROBE_MOVIE_SHOT=<file.ppm>: the middle frame as RGB (BT.709, limited range), to look at the NV12 output.
static void save_ppm(const char *path, const uint8_t *nv12, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) { printf("probe-movie: cannot write %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    const uint8_t *uv = nv12 + (size_t)w * h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            double Y = 1.164 * (nv12[(size_t)y * w + x] - 16), u = uv[(size_t)(y / 2) * w + (x & ~1)] - 128.0,
                   v = uv[(size_t)(y / 2) * w + (x & ~1) + 1] - 128.0;
            double rgb[3] = {Y + 1.793 * v, Y - 0.213 * u - 0.533 * v, Y + 2.112 * u};
            for (int k = 0; k < 3; k++) fputc(rgb[k] < 0 ? 0 : rgb[k] > 255 ? 255 : (int)rgb[k], f);
        }
    fclose(f);
    printf("probe-movie: middle frame written to %s\n", path);
}

static int wait_quiet(uint64_t obj) {
    for (int i = 0; i < 5000; i++) {
        if (movie_quiet(obj)) return 1;
        plat_sleep_ms(1);
    }
    return 0;
}

static int feed(uint64_t obj, const uint8_t *data, const Track *t, int *sent, int limit, int *eos_sent) {
    int idx, rem;
    while (*sent < limit && (uint32_t)call(MOVIE_ACQUIRE_INPUT, 3, obj, U(&idx), U(&rem)) == 0) {
        PBuf in = {0};
        CHECK((uint32_t)call(MOVIE_GET_INPUT, 3, obj, (uint64_t)idx, U(&in)) == 0 && in.base,
              "GetInputBuffer(%d)", idx);
        const Frame *f = &t->f[*sent];
        CHECK(in.cap >= f->size, "input buffer of %llu bytes for a %u-byte frame", (unsigned long long)in.cap, f->size);
        memcpy((void *)(uintptr_t)in.base, data + f->off, f->size);
        CHECK((uint32_t)call(MOVIE_SEND_INPUT, 6, obj, (uint64_t)idx, (uint64_t)0, (uint64_t)f->size, (uint64_t)f->pts,
                             (uint64_t)0) == 0, "SendInputBufferForDecode frame %d", *sent);
        ++*sent;
    }
    if (*sent == limit && !*eos_sent && (uint32_t)call(MOVIE_ACQUIRE_INPUT, 3, obj, U(&idx), U(&rem)) == 0) {
        CHECK((uint32_t)call(MOVIE_SEND_INPUT, 6, obj, (uint64_t)idx, (uint64_t)0, (uint64_t)0,
                             (uint64_t)t->f[limit - 1].pts, (uint64_t)MOVIE_FLAG_EOS) == 0, "end of stream input");
        *eos_sent = 1;
    }
    return 0;
}

// Paced run over the first `limit` frames with a present every `period` ns of fake time; returns 0 on success and
// fills the per-present counts.
static int paced(uint64_t obj, const uint8_t *data, const Track *t, int limit, uint64_t period, int *got, int *ticks,
                 int *max_per_tick, int64_t *worst_lag_us) {
    CHECK((uint32_t)call(MOVIE_FLUSH, 1, obj) == 0, "Flush");
    movie_set_pace(1);
    int sent = 0, eos_sent = 0, eos = 0;
    *got = *ticks = *max_per_tick = 0;
    *worst_lag_us = 0;
    int64_t pts0 = 0;
    uint64_t t0 = 0;
    while (!eos) {
        CHECK(*ticks < 100000, "paced run did not end");
        if (feed(obj, data, t, &sent, limit, &eos_sent)) return 1;
        CHECK(wait_quiet(obj), "decoder stalled");
        fake_now += period;
        ++*ticks;
        movie_tick(C);
        int idx, rem, n = 0;
        int64_t time;
        unsigned flags;
        while ((uint32_t)call(MOVIE_ACQUIRE_OUTPUT, 5, obj, U(&idx), U(&time), U(&flags), U(&rem)) == 0) {
            if (flags & MOVIE_FLAG_EOS) eos = 1;
            else {
                if (!*got) { pts0 = time; t0 = fake_now; }
                int64_t lag = (int64_t)(fake_now - t0) / 1000 - (time - pts0);   // how late the frame is shown (us)
                if (lag > *worst_lag_us) *worst_lag_us = lag;
                CHECK(lag >= 0, "frame at %lld us handed over %lld us early", (long long)time, (long long)-lag);
                ++*got;
                n++;
            }
            CHECK((uint32_t)call(MOVIE_RELEASE_OUTPUT, 2, obj, (uint64_t)idx) == 0, "ReleaseOutputBufferIndex(%d)", idx);
        }
        if (n > *max_per_tick) *max_per_tick = n;
    }
    return 0;
}

int probe_movie(void) {
    const char *dir = rt_config()->romfs;
    char image[PLAT_PATH_MAX];
    snprintf(image, sizeof image, "%s/romfs.bin", dir);
    if (!plat_file_exists(image)) {
        printf("probe-movie: skipped: no RomFS image at %s (game data absent; extract it with tools/extract.py)\n", image);
        return 0;
    }
    fs_init();
    const char *want = cfg_get("TOTK_PROBE_MOVIE");
    const char *name = NULL;
    uint64_t best = UINT64_MAX;
    int movies = 0;
    for (uint64_t i = 0; i < romfs_file_count(); i++) {
        uint64_t size;
        const char *p = romfs_entry(i, NULL, &size);
        size_t n = p ? strlen(p) : 0;
        if (!p || strncmp(p, "Event/Movie/", 12) || n < 5 || strcmp(p + n - 5, ".webm")) continue;
        movies++;
        if (want ? strstr(p, want) && !name : size < best) { name = p; best = size; }
    }
    if (!name) {
        printf("probe-movie: skipped: no Event/Movie/*.webm%s%s in the RomFS index (%d movies)\n",
               want ? " matching " : "", want ? want : "", movies);
        return 0;
    }
    printf("probe-movie: %d movies in Event/Movie; decoding %s (%.1f MiB)\n", movies, name, best / 1048576.0);

    CHECK(romfs_mount("probe") == 0, "RomFS mount");
    char path[512];
    snprintf(path, sizeof path, "probe:/%s", name);
    uint64_t h, got_bytes;
    int64_t size;
    CHECK(romfs_open(&h, path, FS_OPEN_READ) == 0 && romfs_size(&size, h) == 0 && size > 0, "open %s", path);
    uint8_t *data = malloc((size_t)size);
    CHECK(data, "file buffer");
    CHECK(romfs_read(&got_bytes, h, 0, data, (uint64_t)size) == 0 && got_bytes == (uint64_t)size, "read %s", path);
    romfs_close(h);

    Track t = {0};
    const char *err = demux(data, (size_t)size, &t);
    CHECK(!err, "WebM: %s", err);
    CHECK(!strcmp(t.codec, "V_VP9") && t.n > 0 && t.w > 0 && t.h > 0, "WebM: codec %s, %d frames, %dx%d", t.codec, t.n,
          t.w, t.h);
    printf("probe-movie: WebM %s %dx%d, %d frames, %.2f s, first PTS %lld us\n", t.codec, t.w, t.h, t.n,
           (t.f[t.n - 1].pts - t.f[0].pts) / 1e6, (long long)t.f[0].pts);

    // Stand-ins and entries. The vtable the constructor stores is a host address nothing calls.
    MovieGuest g = {
        .signal_event = hostfn_register("probe nn::os::SignalEvent", s_signal),
        .buf_set_range = hostfn_register("probe movie::Buffer::SetRange", s_set_range),
        .buf_set_data = hostfn_register("probe movie::Buffer::SetDataAndCapacity", s_set_data),
        .buf_base = hostfn_register("probe movie::Buffer::Base", s_base),
        .buf_capacity = hostfn_register("probe movie::Buffer::Capacity", s_capacity),
        .md_find_int32 = hostfn_register("probe movie::MediaData::FindInt32", s_find_int32),
        .md_find_string = hostfn_register("probe movie::MediaData::FindString", s_find_string),
        .md_set_int32 = hostfn_register("probe movie::MediaData::SetInt32", s_set_int32),
        .md_set_rect = hostfn_register("probe movie::MediaData::SetRect", s_set_rect),
        .md_set_string = hostfn_register("probe movie::MediaData::SetString", s_set_string),
        .vptr = hostfn_register("probe movie::Decoder vtable", s_vtable),
    };
    g.orig[MOVIE_D0] = hostfn_register("probe movie::Decoder::~Decoder D0", s_orig_d0);
    g.orig[MOVIE_D2] = hostfn_register("probe movie::Decoder::~Decoder D2", s_orig_d2);
    for (int i = 0; i < MOVIE_ENTRIES; i++) CHECK((E[i] = hostfn_override(movie_symbols[i])), "no override for %s",
                                                  movie_symbols[i]);
    hostfn_seal();
    movie_set_guest(&g);
    C = plat_aligned_alloc(sizeof(Cpu), 64);
    CHECK(C, "Cpu allocation");
    memset(C, 0, sizeof *C);

    uint64_t obj_mem[2] = {0}, obj = U(obj_mem);
    volatile int32_t ev[4] = {0};
    uint64_t events[4] = {U(&ev[0]), U(&ev[1]), U(&ev[2]), U(&ev[3])};
    PMd cfg = {0};
    C->x[0] = U(&cfg);
    C->x[1] = U("mime");
    C->x[2] = U("video/x-vnd.on2.vp9");
    s_set_string(C);
    C->x[1] = U("width");
    C->x[2] = (uint64_t)t.w;
    s_set_int32(C);
    C->x[1] = U("height");
    C->x[2] = (uint64_t)t.h;
    s_set_int32(C);

    call(MOVIE_CTOR, 5, obj, (uint64_t)MOVIE_TYPE_VP9, (uint64_t)MOVIE_FORMAT_NV12, (uint64_t)MOVIE_MODE_BUFFER,
         (uint64_t)0);
    CHECK(obj_mem[0] == g.vptr && obj_mem[1], "constructor: object holds %#llx %#llx", (unsigned long long)obj_mem[0],
          (unsigned long long)obj_mem[1]);
    CHECK((uint32_t)call(MOVIE_REGISTER_EVENTS, 2, obj, U(events)) == 0, "RegisterClientEvents");
    CHECK((uint32_t)call(MOVIE_CONFIGURE, 2, obj, U(&cfg)) == 0, "Configure");
    int osize = 0, ocount = 0, fb = t.w * t.h * 3 / 2;
    CHECK((uint32_t)call(MOVIE_GET_OUTPUT_SIZE, 3, obj, U(&osize), U(&ocount)) == 0 && osize == fb && ocount == 1,
          "GetOutputBufferSize %d x %d, want %d", osize, ocount, fb);
    PMd fmt = {0};
    CHECK((uint32_t)call(MOVIE_GET_OUTPUT_FORMAT, 2, obj, U(&fmt)) == 0, "GetOutputFormat");
    CHECK(md_int(&fmt, "width") == t.w && md_int(&fmt, "height") == t.h && md_int(&fmt, "stride") == t.w &&
              md_int(&fmt, "color-format") == 21 && md_int(&fmt, "nv12-uv-offset") == t.w * t.h,
          "output format %d x %d stride %d colour %d", md_int(&fmt, "width"), md_int(&fmt, "height"),
          md_int(&fmt, "stride"), md_int(&fmt, "color-format"));
    CHECK((uint32_t)call(MOVIE_START, 1, obj) == 0, "Start");

    // 1. unpaced
    movie_set_pace(0);
    uint8_t *frame = malloc((size_t)fb);
    CHECK(frame, "frame buffer");
    int sent = 0, eos_sent = 0, got = 0, eos = 0, formats = 0;
    int64_t prev = INT64_MIN;
    double luma_lo = 255, luma_hi = 0;
    uint64_t f0 = plat_now_ns();
    for (int idle = 0; !eos;) {
        if (feed(obj, data, &t, &sent, t.n, &eos_sent)) return 1;
        movie_tick(C);
        int idx, rem, progress = 0;
        int64_t time;
        unsigned flags;
        while ((uint32_t)call(MOVIE_ACQUIRE_OUTPUT, 5, obj, U(&idx), U(&time), U(&flags), U(&rem)) == 0) {
            progress = 1;
            if (flags & MOVIE_FLAG_EOS) eos = 1;
            else {
                PBuf out = {.base = U(frame), .cap = (uint64_t)fb};
                PMd of = {0};
                if (!got) {
                    CHECK((uint32_t)call(MOVIE_GET_OUTPUT_MD, 4, obj, (uint64_t)idx, U(&out), U(&of)) == 0,
                          "GetOutputBuffer with format");
                    formats = md_int(&of, "width") == t.w && md_int(&of, "nv12-uv-stride") == t.w;
                } else CHECK((uint32_t)call(MOVIE_GET_OUTPUT, 3, obj, (uint64_t)idx, U(&out)) == 0, "GetOutputBuffer");
                CHECK(out.off == 0 && out.size == fb, "frame %d: output range %d+%d, want %d bytes", got, out.off,
                      out.size, fb);
                CHECK(time > prev, "frame %d: PTS %lld after %lld", got, (long long)time, (long long)prev);
                CHECK(time == t.f[got].pts, "frame %d: PTS %lld, the container says %lld", got, (long long)time,
                      (long long)t.f[got].pts);
                prev = time;
                if (got == t.n / 2 && cfg_get("TOTK_PROBE_MOVIE_SHOT")) save_ppm(cfg_get("TOTK_PROBE_MOVIE_SHOT"), frame, t.w, t.h);
                if (got % 30 == 0) {   // mean luma of a centre row band: a decoded picture, not a blank buffer
                    uint64_t sum = 0;
                    for (int y = t.h / 2 - 8; y < t.h / 2 + 8; y++)
                        for (int x = 0; x < t.w; x++) sum += frame[(size_t)y * t.w + x];
                    double mean = (double)sum / (16.0 * t.w);
                    if (mean < luma_lo) luma_lo = mean;
                    if (mean > luma_hi) luma_hi = mean;
                }
                got++;
            }
            CHECK((uint32_t)call(MOVIE_RELEASE_OUTPUT, 2, obj, (uint64_t)idx) == 0, "ReleaseOutputBufferIndex(%d)", idx);
        }
        if (progress) idle = 0;
        else {
            CHECK(++idle < 20000, "no output for 20 s (%d of %d frames out, %d in)", got, t.n, sent);
            plat_sleep_ms(1);
        }
    }
    double wall = (double)(plat_now_ns() - f0) / 1e9;
    MovieStats st;
    CHECK(movie_stats(obj, &st), "stats");
    CHECK(got == t.n, "%d frames out of %d in", got, t.n);
    CHECK(st.frames == (uint64_t)t.n && st.errors == 0, "decoder counted %llu frames, %llu errors",
          (unsigned long long)st.frames, (unsigned long long)st.errors);
    CHECK(formats, "GetOutputBuffer's format does not describe the NV12 frame");
    CHECK(ev[MOVIE_EV_INPUT] > 0 && ev[MOVIE_EV_OUTPUT] > 0 && ev[MOVIE_EV_FORMAT] == 1 && ev[MOVIE_EV_ERROR] == 0,
          "events: input %d output %d format %d error %d", ev[0], ev[1], ev[2], ev[3]);
    CHECK(luma_hi - luma_lo > 1 || luma_hi > 4, "decoded pictures look blank (centre luma %.1f-%.1f)", luma_lo, luma_hi);
    double avg = st.decode_ns / 1e6 / (double)st.frames;
    printf("probe-movie: unpaced: %d frames, %dx%d NV12, PTS in order, events input %d output %d format %d; centre "
           "luma %.1f-%.1f\n", got, t.w, t.h, ev[0], ev[1], ev[2], luma_lo, luma_hi);
    printf("probe-movie: decode %.2f ms per frame (max %.2f ms) on the worker; %.1f frames/s through the API\n", avg,
           st.max_ns / 1e6, got / wall);
    CHECK(avg < 33.0, "decoding is slower than 30 fps (%.2f ms per frame)", avg);

    // 2. paced at 60 presents per second: one frame every second present
    int limit = t.n < 150 ? t.n : 150, n, ticks, maxn;
    int64_t lag;
    movie_set_clock(fake_clock);
    fake_now = 1000000000ull;
    MovieStats s0 = st;
    if (paced(obj, data, &t, limit, 1000000000ull / 60, &n, &ticks, &maxn, &lag)) return 1;
    CHECK(movie_stats(obj, &st), "stats");
    double span = (t.f[limit - 1].pts - t.f[0].pts) / 1e6 * 60.0;   // presents the frames span at 60 per second
    CHECK(n == limit && maxn == 1 && st.dropped == s0.dropped, "60 Hz: %d of %d frames, up to %d per present, %llu dropped",
          n, limit, maxn, (unsigned long long)(st.dropped - s0.dropped));
    CHECK(ticks >= span - 1 && ticks <= span + 4, "60 Hz: %d presents for %.1f presents of movie", ticks, span);
    CHECK(lag <= 16667, "60 Hz: a frame was shown %lld us late", (long long)lag);
    printf("probe-movie: paced at 60 Hz: %d frames in %d presents (movie spans %.1f), at most 1 per present, worst lag "
           "%.1f ms, none dropped\n", n, ticks, span, lag / 1000.0);

    // 3. paced at 15 presents per second: about every second frame is dropped, the shown one stays in time
    limit = t.n < 120 ? t.n : 120;
    s0 = st;
    if (paced(obj, data, &t, limit, 1000000000ull / 15, &n, &ticks, &maxn, &lag)) return 1;
    CHECK(movie_stats(obj, &st), "stats");
    uint64_t dropped = st.dropped - s0.dropped;
    CHECK(maxn == 1 && dropped > 0 && n + (int)dropped == limit && lag <= 100000,
          "15 Hz: %d shown, %llu dropped of %d, up to %d per present, worst lag %lld us", n, (unsigned long long)dropped,
          limit, maxn, (long long)lag);
    printf("probe-movie: paced at 15 Hz: %d shown, %llu dropped of %d, worst lag %.1f ms\n", n,
           (unsigned long long)dropped, limit, lag / 1000.0);
    movie_set_clock(NULL);

    CHECK((uint32_t)call(MOVIE_STOP, 1, obj) == 0, "Stop");
    call(MOVIE_D0, 1, obj);
    CHECK(!movie_stats(obj, &st) && g_d0 == 1 && obj_mem[1] == 0, "deleting destructor left the host decoder");
    free(frame);
    free(data);
    free(t.f);
    plat_aligned_free(C);
    printf("probe-movie: passed\n");
    return 0;
}
