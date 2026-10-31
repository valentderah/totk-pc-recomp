// The game's zstd decoder on the host's libzstd. Most RomFS files of TotK 1.4.3 are zstd frames, many with a
// dictionary from Pack/ZsDic.pack.zs; the decoder is BSD zstd linked into main. The patch set "zstd"
// (recomp/patches-v143.txt) sends every call of ZSTD_decompressMultiFrame (main+0x3a1f0) and of ZSTD_decompress_usingDDict
// (main+0x3a1e0) to the two host functions below, on the patch page. Lifted, the decoder ran its literal and match
// copies as chains of small functions with the registers in Cpu; here the same frames decode in native code.
//
// - Results match the guest's: libzstd decodes the same format, and every result that is an error goes to the guest
//   decoder again (it writes its own error code; a frame libzstd rejects may still be one the game's version accepts).
// - A guest ZSTD_DDict (zstd's struct: dictBuffer, dictContent, dictSize first) becomes a host ZSTD_DDict made from a
//   copy of its content, cached by guest address, content address, size and a fingerprint of the content.
// - Inputs of at least AWAY_BYTES give the virtual core up while they decode (other guest threads run on it).
// TOTK_ZSTD_NATIVE=0 keeps the guest decoder (the patch set switch); TOTK_ZSTD_CHECK=1 decodes each call both ways,
// keeps the guest's result and logs every call where the two differ.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include "cfg.h"
#include "host.h"

#if __has_include("patches.h")
#include "patches.h"
#include "platform/platform.h"
#endif

#ifdef RT_PATCH_PAGE

#define GUEST_MULTIFRAME 0x700003a1f0ull   // 1.4.3 main: ZSTD_decompressMultiFrame (only 1.4.3 has the patch sites)
#define GUEST_USING_DDICT 0x700003a1e0ull  // ZSTD_decompress_usingDDict
#define HOST_MULTIFRAME (RT_PATCH_PAGE + 0xff0)
#define HOST_USING_DDICT (RT_PATCH_PAGE + 0xfe0)
#define AWAY_BYTES (64u << 10)
#define MAX_DICT (64u << 20)
#define NDICT 32

static void *gp(uint64_t a) { return (void *)(uintptr_t)a; }

typedef struct {
    uint64_t guest, content, size, print;
    ZSTD_DDict *dd;
} Dict;
static Dict dicts[NDICT];
static int ndicts;
static PlatMutex dict_lock = PLAT_MUTEX_INIT;

static struct {
    uint64_t calls, in, out, ns, guest_ns, fallbacks, away, inplace;
} S;

static int check_mode(void) {
    static int v = -1;
    if (v < 0) v = cfg_bool("TOTK_ZSTD_CHECK");
    return v;
}

// Size, both ends and a sample of the middle: what tells a new dictionary at a reused address from the old one.
static uint64_t fingerprint(const uint8_t *p, uint64_t n) {
    uint64_t h = n * 0x9E3779B97F4A7C15ull;
    uint64_t at[3] = {0, n / 2, n > 64 ? n - 64 : 0};
    for (int k = 0; k < 3; k++)
        for (uint64_t i = at[k]; i < at[k] + 64 && i < n; i += 8) {
            uint64_t v = 0;
            memcpy(&v, p + i, n - i < 8 ? n - i : 8);
            h = (h ^ v) * 0xC2B2AE3D27D4EB4Full;
            h ^= h >> 29;
        }
    return h;
}

// The host dictionary for a guest ZSTD_DDict or raw dictionary bytes (guest == 0); NULL when its fields look wrong.
static ZSTD_DDict *dict_for(uint64_t guest, uint64_t content, uint64_t size) {
    if (!content || !size || size > MAX_DICT || content >= RT_ADDRESS_LIMIT || content + size > RT_ADDRESS_LIMIT)
        return NULL;
    uint64_t print = fingerprint(gp(content), size);
    ZSTD_DDict *dd = NULL;
    plat_mutex_lock_shared(&dict_lock);
    for (int i = 0; i < ndicts && !dd; i++)
        if (dicts[i].guest == guest && dicts[i].content == content && dicts[i].size == size && dicts[i].print == print)
            dd = dicts[i].dd;
    plat_mutex_unlock_shared(&dict_lock);
    if (dd) return dd;
    // A copy: the guest may free or reuse its buffer. Entries are never freed (another thread may be decoding with
    // one); a full table sends further dictionaries to the guest decoder.
    ZSTD_DDict *made = ZSTD_createDDict(gp(content), (size_t)size);
    if (!made) return NULL;
    int slot = -1;
    plat_mutex_lock(&dict_lock);
    for (int i = 0; i < ndicts && !dd; i++)   // another thread may have made it meanwhile
        if (dicts[i].guest == guest && dicts[i].content == content && dicts[i].size == size && dicts[i].print == print)
            dd = dicts[i].dd;
    if (!dd && ndicts < NDICT) slot = ndicts, dicts[ndicts++] = (Dict){guest, content, size, print, made}, dd = made;
    plat_mutex_unlock(&dict_lock);
    if (dd != made) {
        ZSTD_freeDDict(made);
        if (!dd) rt_log("zstd: more than %d dictionaries: further ones decode in the guest", NDICT);
        return dd;
    }
    rt_log("zstd: host dictionary %d for %s %#llx: %llu bytes at %#llx, id %u", slot, guest ? "DDict" : "raw dict",
           (unsigned long long)(guest ? guest : content), (unsigned long long)size, (unsigned long long)content,
           ZSTD_getDictID_fromDDict(dd));
    return dd;
}

// Decodes on this thread's host context. Returns libzstd's result; *ok 0 when the inputs cannot be taken.
static size_t host_decode(uint64_t dst, uint64_t cap, uint64_t src, uint64_t size, uint64_t dict, uint64_t dict_size,
                          uint64_t ddict, int *ok) {
    static __thread ZSTD_DCtx *dctx;
    *ok = 0;
    if ((cap && !dst) || !src || src + size > RT_ADDRESS_LIMIT) return 0;   // dst: decode() checks a guest one
    ZSTD_DDict *dd = NULL;
    if (ddict) {
        if (ddict >= RT_ADDRESS_LIMIT - 24) return 0;
        uint64_t f[3];   // ZSTD_DDict: dictBuffer, dictContent, dictSize
        memcpy(f, gp(ddict), sizeof f);
        if (!(dd = dict_for(ddict, f[1], f[2]))) {
            static int logged;
            if (logged < 4) {
                uint64_t w[8];
                memcpy(w, gp(ddict), sizeof w);
                logged++;
                rt_log("zstd: DDict %#llx not taken: %llx %llx %llx %llx %llx %llx %llx %llx", (unsigned long long)ddict,
                       (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3],
                       (unsigned long long)w[4], (unsigned long long)w[5], (unsigned long long)w[6], (unsigned long long)w[7]);
            }
            return 0;
        }
    } else if (dict && dict_size) {
        if (!(dd = dict_for(0, dict, dict_size))) return 0;
    }
    if (!dctx && !(dctx = ZSTD_createDCtx())) return 0;
    *ok = 1;
    return dd ? ZSTD_decompress_usingDDict(dctx, gp(dst), (size_t)cap, gp(src), (size_t)size, dd)
              : ZSTD_decompressDCtx(dctx, gp(dst), (size_t)cap, gp(src), (size_t)size);
}

// The host call inside the thread's host depth, as hostfn.c counts it for asynchronous preemption.
static size_t tracked_decode(Cpu *c, uint64_t dst, uint64_t cap, uint64_t src, uint64_t size, uint64_t dict,
                             uint64_t dict_size, uint64_t ddict, int *ok) {
    int *d = kern_host_track && c->thread ? kern_host_depth(c->thread) : NULL;
    if (d) __atomic_store_n(d, *d + 1, __ATOMIC_RELAXED);
    int away = size >= AWAY_BYTES ? kern_core_away() : 0;
    if (away) __atomic_add_fetch(&S.away, 1, __ATOMIC_RELAXED);
    size_t r = host_decode(dst, cap, src, size, dict, dict_size, ddict, ok);
    kern_core_back(&away);
    if (d) {
        int n = *d - 1;
        __atomic_store_n(d, n, __ATOMIC_RELAXED);
        if (!n && kern_preempt_pending(c->thread)) kern_host_left(c->thread);
    }
    return r;
}

static void count(uint64_t in, uint64_t out, uint64_t ns) {
    uint64_t n = __atomic_add_fetch(&S.calls, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&S.in, in, __ATOMIC_RELAXED);
    __atomic_add_fetch(&S.out, out, __ATOMIC_RELAXED);
    __atomic_add_fetch(&S.ns, ns, __ATOMIC_RELAXED);
    if (n == 1 || !(n & 0x3fff))
        rt_log("zstd: %llu calls on the host, %.1f MiB in, %.1f MiB out, %.0f ms (check mode: the guest decoder %.0f ms); "
               "%llu to the guest decoder, %llu in place in the guest, %llu with the core given up", (unsigned long long)n,
               S.in / 1048576.0, S.out / 1048576.0, S.ns / 1e6, S.guest_ns / 1e6, (unsigned long long)S.fallbacks,
               (unsigned long long)S.inplace, (unsigned long long)S.away);
}

// x0 dctx, x1 dst, x2 capacity, x3 src, x4 size, then (dict, dict size, ddict) as the entry has them. guest: the
// guest function with the same arguments, n of them.
static void decode(Cpu *c, uint64_t dict, uint64_t dict_size, uint64_t ddict, uint64_t guest, int n) {
    uint64_t a[8];
    memcpy(a, c->x, sizeof a);
    uint64_t dst = a[1], cap = a[2], src = a[3], size = a[4];
    // In place (the input inside the output buffer): the result depends on how the decoder's writes overtake its
    // reads, which libzstd does not promise to do as the game's version does, and a host decode that failed half way
    // would leave the guest decoder a damaged input. Such calls stay in the guest.
    if (cap && src < dst + cap && dst < src + size) {
        __atomic_add_fetch(&S.inplace, 1, __ATOMIC_RELAXED);
        rt_call(c, guest, n, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        return;
    }
    if (check_mode()) {
        uint8_t *mine = cap ? malloc((size_t)cap) : NULL;
        int ok = 0;
        uint64_t t0 = kern_now_ns();
        size_t r = cap && !mine ? 0 : tracked_decode(c, (uint64_t)(uintptr_t)mine, cap, src, size, dict, dict_size, ddict, &ok);
        uint64_t t1 = kern_now_ns();
        uint64_t g = rt_call(c, guest, n, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        __atomic_add_fetch(&S.guest_ns, kern_now_ns() - t1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&S.ns, t1 - t0, __ATOMIC_RELAXED);
        static int logged;
        if (logged < 6 && (!ok || ZSTD_isError(r))) {
            logged++;
            rt_log("zstd check: host %s, guest %llu; dctx %#llx dst %#llx cap %llu src %#llx size %llu dict %#llx/%llu "
                   "ddict %#llx", !ok ? "inputs not taken" : ZSTD_getErrorName(r), (unsigned long long)g,
                   (unsigned long long)a[0], (unsigned long long)dst, (unsigned long long)cap, (unsigned long long)src,
                   (unsigned long long)size, (unsigned long long)dict, (unsigned long long)dict_size,
                   (unsigned long long)ddict);
        }
        if (ok && !ZSTD_isError(r) && (g != r || (r && memcmp(mine, gp(dst), r)))) {
            const uint8_t *p = gp(src);
            size_t fs = ZSTD_findFrameCompressedSize(p, (size_t)size);
            unsigned long long cs = ZSTD_getFrameContentSize(p, (size_t)size);
            ZSTD_DCtx *fresh = ZSTD_createDCtx();
            size_t r2 = ZSTD_decompressDCtx(fresh, mine, (size_t)cap, p, (size_t)size);
            ZSTD_freeDCtx(fresh);
            static int nlog;
            if (nlog++ < 8)
                rt_log("zstd check: MISMATCH %llu bytes from %#llx (cap %llu, first frame %lld bytes, content %lld, magic "
                       "%02x%02x%02x%02x, header %02x, dict %#llx/%llu, ddict %#llx, entry %s, lr %#llx): the host decoded "
                       "%llu bytes, the guest %lld%s; a fresh context without a dictionary: %s", (unsigned long long)size,
                       (unsigned long long)src, (unsigned long long)cap, ZSTD_isError(fs) ? -1ll : (long long)fs,
                       (long long)cs, p[3], p[2], p[1], p[0], p[4], (unsigned long long)dict,
                       (unsigned long long)dict_size, (unsigned long long)ddict, n == 8 ? "MultiFrame" : "usingDDict",
                       (unsigned long long)c->x[30], (unsigned long long)r, (long long)g, g == r ? " (other contents)" : "",
                       ZSTD_isError(r2) ? ZSTD_getErrorName(r2) : "decodes");
        }
        free(mine);
        count(size, ok && !ZSTD_isError(r) ? r : 0, 0);
        return;
    }
    uint64_t t0 = kern_now_ns();
    int ok = 0;
    size_t r = dst + cap > RT_ADDRESS_LIMIT ? 0 : tracked_decode(c, dst, cap, src, size, dict, dict_size, ddict, &ok);
    if (!ok || ZSTD_isError(r)) {   // the guest decoder decides (and writes its own error code)
        __atomic_add_fetch(&S.fallbacks, 1, __ATOMIC_RELAXED);
        static int logged;
        if (logged < 8) {
            logged++;
            rt_log("zstd: %llu bytes at %#llx go to the guest decoder: %s", (unsigned long long)size,
                   (unsigned long long)src, !ok ? "inputs not taken" : ZSTD_getErrorName(r));
        }
        rt_call(c, guest, n, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        return;
    }
    c->x[0] = r;
    count(size, r, kern_now_ns() - t0);
}

static void host_multiframe(Cpu *c) { decode(c, c->x[5], c->x[6], c->x[7], GUEST_MULTIFRAME, 8); }
static void host_using_ddict(Cpu *c) { decode(c, 0, 0, c->x[5], GUEST_USING_DDICT, 6); }

__attribute__((constructor)) static void zstd_register(void) {
    rt_register_host(HOST_MULTIFRAME, host_multiframe);
    rt_register_host(HOST_USING_DDICT, host_using_ddict);
}

#endif
