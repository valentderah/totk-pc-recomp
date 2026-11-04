// Game resolution: the size the game composes its frame in (its output: canvas, UI and swapchain) and the size of its
// 3D scene, above the shipped 1920x1080 and 1600x900. The game does the rendering itself, as with the community
// resolution mods (UltraCam, Visual Fixes): the patch sets output and scene (recomp/patches-v121.txt,
// patches-v143.txt; MacNativeGames games/totk/runtime/game_patches.c and game_patches_v143.c, MIT, Copyright (c) 2026
// MacNativeGames contributors) make the game read both sizes from the patch page, and the texture heap that holds the
// output- and scene-sized render targets grows with them.
//
// Settings, read at start: TOTK_OUTPUT_RES = <w>x<h> (default 1920x1080, the game's own), TOTK_RENDER_SCALE = the
// scene's size relative to the output: "game" (default, 5/6, the game's 1600x900 at 1080p) or a percentage; above 100
// the game supersamples its scene, which smooths edges (anti-aliasing) at the cost of GPU time. The game's own final
// pass (FSR 1 EASU) scales the scene to the output; the present scales the output to the window (vk_fsr.c).
//
// Patch page: +16 the scene size {w, h} as floats, +32 the output size and +48 the swapchain size as {w, h} in one
// 64-bit word, +40 the renderer (1.2.1) or display configuration (1.4.3) the patched code stores there.
// +56 the camera's aspect ratio as a float when the output is not 16:9 (set aspect), +60 the field-of-view factor
// (set fov, res_fov_live).
// 1.4.3 scales the scene it reads by output width / 1920 (a 2560x1440 output renders its 1600x900 scene at 2128x1200;
// MacNativeGames measured the same), so its page holds the wanted size divided by that factor and the game's 5/6 needs
// no scene patch. 1.2.1 renders the size it reads (1600x900 on a 2560x1440 output): its page holds the wanted size, and
// the scene set is on whenever the output differs from 1920x1080.
//
// Heap: agl's DynamicTextureAllocator is one fixed heap, "DynamicTextureAllocatorMem" in
// System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs (113664 KiB, carved from "Graphics", 201728 KiB). At 2560x1440
// the shipped size runs out at the title screen. Both heaps grow by the same amount: the shipped size times the output's
// pixel ratio above 1920x1080 plus the scene's above 1600x900 (MacNativeGames' measurement: a 3440x1440 canvas with a
// 4620x1935 scene crashed at +258 MiB and loaded at +400 MiB; this formula gives +750 MiB there). romfs.c serves the
// rewritten table (res_heapdef_build) in place of the original; memory.c adds the amount to the application pool, so
// no other heap shrinks. Without the rewritten table (a RomFS that lacks it, a format this code does not know) the
// patch sets stay off and the game keeps its own sizes.
#ifndef _WIN32
#include <dirent.h>
#endif
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "cfg.h"
#include "host.h"
#include "mods.h"
#include "platform/platform.h"

#define BASE_W 1920
#define BASE_H 1080
#define SCENE_W 1600
#define SCENE_H 900
#define DTA_KIB 113664u      // DynamicTextureAllocatorMem, shipped
#define TEX_HEAP_KIB 506880u // TextureHeap, shipped (1.2.1 and 1.4.3): what a mod's own HeapDef is measured against
#define SCENE_MAX 4096       // per side, as MacNativeGames' TOTK_RENDER_RES
#define SHADOW_BASE 960      // the sun shadow map's base size (its cascades: a D16 array of 4 layers)

static struct {
    int decided;
    unsigned ow, oh, sw, sh;   // output and scene
    int scene_set;             // the scene differs from the game's 5/6 of the output
    int heapdef_ok;            // romfs.c serves the grown HeapDef
    int on;                    // res_init switched the patch sets on
    unsigned shadow;           // fixed sun shadow map size (TOTK_SHADOW_RES), 0: the game's own
    double dlss_k;             // TOTK_DLSS quality/balanced/performance/ultra_performance: the scene's share of the output, else 0
    double base_k;             // the scene's share without a DLSS mode (render_scale, or the game's 5/6)
    unsigned hw, hh;           // the largest scene a DLSS mode change can ask for: the heap is sized for it
    int live;                  // 1.4.3 with DLSS able to run: the DLSS mode can change while the game runs
} R;

// The scene size is decided before the device exists, so NGX cannot be asked yet. NGX loads DLSS from nvngx_dlss.dll
// next to totk.exe on Windows, libnvidia-ngx-dlss.so.<version> next to totk on Linux (vk_dlaa.c vk_dlaa_init):
// without that file DLSS never runs, and a DLSS mode would leave the game's plain scaling at 50..67 % of the output.
static int dlss_dll(void) {
#ifdef _WIN32
    char exe[1024], path[1024 + 32];
    if (!plat_exe_path(exe, sizeof exe)) return 1;
    char *last = plat_last_sep(exe);
    if (!last) return 1;
    snprintf(path, sizeof path, "%.*s/nvngx_dlss.dll", (int)(last - exe), exe);
    return plat_file_exists(path);
#else
    char exe[1024];
    if (!plat_exe_path(exe, sizeof exe)) return 1;
    char *last = plat_last_sep(exe);
    if (!last) return 1;
    *last = 0;
    DIR *d = opendir(exe);
    if (!d) return 1;
    int found = 0;
    for (struct dirent *e; !found && (e = readdir(d));)
        found = !strncmp(e->d_name, "libnvidia-ngx-dlss.so.", 22);
    closedir(d);
    return found;
#endif
}

// The scene size for a share k of the output, within SCENE_MAX per side.
static void scene_for(double k, unsigned *w, unsigned *h) {
    double sw = R.ow * k, sh = R.oh * k;
    double fit = fmin(1.0, fmin(SCENE_MAX / sw, SCENE_MAX / sh));
    *w = (unsigned)(sw * fit + 0.5);
    *h = (unsigned)(sh * fit + 0.5);
}

// The scene's share of the output for a TOTK_DLSS mode; 0 for dlaa (the scene keeps base_k), while DLSS/DLAA is off
// (the mode the menu still holds would shrink the scene for the plain upscaler) and where DLSS cannot run (dlss_dll).
static double dlss_share(const char *m) {
    static const struct { const char *name; double k; } dlss[] = {
        {"quality", 2.0 / 3.0}, {"balanced", 0.58}, {"performance", 0.5}, {"ultra_performance", 1.0 / 3.0}};
    static int dll = -1;
    if (!cfg_bool("TOTK_DLAA")) return 0;
    for (int i = 0; i < (int)(sizeof dlss / sizeof dlss[0]); i++)
        if (!strcmp(m, dlss[i].name)) {
            if (dll < 0 && !(dll = dlss_dll())) {
#ifdef _WIN32
                rt_log("resolution: DLSS %s needs nvngx_dlss.dll next to totk.exe, which is missing: the scene follows "
                       "render_scale", m);
#else
                rt_log("resolution: DLSS %s needs libnvidia-ngx-dlss.so.<version> next to totk, which is missing: the "
                       "scene follows render_scale", m);
#endif
            }
            return dll ? dlss[i].k : 0;
        }
    return 0;
}

static void decide(void) {
    if (R.decided) return;
    R.decided = 1;
    R.ow = BASE_W;
    R.oh = BASE_H;
    const char *o = cfg_value("TOTK_OUTPUT_RES");
    unsigned w = 0, h = 0;
    char tail;
    if (sscanf(o, "%ux%u%c", &w, &h, &tail) == 2 && w >= 1280 && h >= 720 && w <= 7680 && h <= 4320) {
        R.ow = w & ~1u;
        R.oh = h & ~1u;
    } else {
        rt_log("resolution: TOTK_OUTPUT_RES=%s is not <width>x<height> in 1280x720..7680x4320; 1920x1080", o);
    }
    const char *s = cfg_value("TOTK_RENDER_SCALE");
    double k = 5.0 / 6.0;
    if (strcmp(s, "game")) {
        char *end;
        double pct = strtod(s, &end);
        if (end != s && !*end && pct >= 25 && pct <= 400) {
            k = pct / 100.0;
            R.scene_set = 1;
        } else {
            rt_log("resolution: TOTK_RENDER_SCALE=%s is not game or a percentage 25..400; game", s);
        }
    }
    // A DLSS mode other than dlaa sets the scene size instead. On 1.4.3 with DLSS able to run (DLAA on, the DLL there)
    // the mode can change while the game runs (res_dlss_mode): the scene patch is then always on and the heap is sized
    // for the largest scene the modes ask for. Everyone else keeps the game's own scene size and its dynamic resolution
    // unless render_scale or a DLSS mode asks for another, and a heap sized for that scene alone.
    R.base_k = k;
    R.live = RT_V143 && cfg_bool("TOTK_DLAA") && dlss_dll();
    const char *m = cfg_value("TOTK_DLSS");
    R.dlss_k = dlss_share(m);
    if (R.dlss_k) rt_log("resolution: DLSS %s: the scene at %.0f %% of the output (render_scale ignored)", m, R.dlss_k * 100);
    double sk = R.dlss_k ? R.dlss_k : k;
    if (R.live || R.dlss_k || R.ow * sk > SCENE_MAX || R.oh * sk > SCENE_MAX) R.scene_set = 1;
    scene_for(sk, &R.sw, &R.sh);
    if (R.live) scene_for(fmax(k, 2.0 / 3.0), &R.hw, &R.hh);
    else R.hw = R.sw, R.hh = R.sh;
    const char *sr = cfg_value("TOTK_SHADOW_RES");
    if (strcmp(sr, "game")) {
        unsigned n = (unsigned)atoi(sr);
        if (n >= 256 && n <= 4096 && !(n & 7)) R.shadow = n;
        else rt_log("resolution: TOTK_SHADOW_RES=%s is not game or a size 256..4096 (a multiple of 8); game", sr);
    }
}

// Texture heap for a sun shadow map above the base size: the four layers of 2 bytes, three maps in flight (the engine
// makes the array anew each frame).
static uint64_t shadow_kib(void) {
    return R.shadow > SHADOW_BASE ? ((uint64_t)R.shadow * R.shadow - SHADOW_BASE * SHADOW_BASE) * 8 * 3 / 1024 : 0;
}

static int bigger(void) {
    decide();
    return R.ow != BASE_W || R.oh != BASE_H || R.scene_set || shadow_kib();
}

uint64_t res_heap_extra_kib(void) {
    if (!bigger()) return 0;
    double grow = fmax(0.0, (double)R.ow * R.oh / (BASE_W * BASE_H) - 1.0) +
                  fmax(0.0, (double)R.hw * R.hh / (SCENE_W * SCENE_H) - 1.0);
    uint64_t kib = grow > 0 ? (uint64_t)ceil(DTA_KIB * (1.0 + grow) / 1024.0) * 1024 - DTA_KIB : 0;
    return kib + ((shadow_kib() + 1023) & ~1023ull);   // whole MiB
}

uint64_t res_heap_extra_bytes(void) { return ((res_heap_extra_kib() << 10) + 0x1fffff) & ~0x1fffffull; }

// TOTK_TEX_HEAP_EXTRA_MB (0 to 4096): more room in "TextureHeap" and its parent "ゲームグラフィックス", the heap the
// world's textures stream into. HD texture packs made for emulators with 8 GB of DRAM grow it by about 3.3 GiB in their
// own HeapDef; without the room the game runs out and starts its panic Blood Moon over and over. The pool and the heap
// cap grow by the same amount (memory.c mem_init).
static uint64_t mod_tex_heap_kib(void);

static uint64_t cfg_tex_heap_kib(void) {
    const char *v = cfg_get("TOTK_TEX_HEAP_EXTRA_MB");
    if (!v || !*v) return 0;
    char *end;
    long mb = strtol(v, &end, 10);
    if (end == v || *end || mb < 0 || mb > 4096) {
        static int logged;
        if (!logged++) rt_log("resolution: TOTK_TEX_HEAP_EXTRA_MB=%s is not 0..4096; 0", v);
        return 0;
    }
    return (uint64_t)mb << 10;
}

// The larger of TOTK_TEX_HEAP_EXTRA_MB and what an enabled mod's own HeapDef adds to the texture heap (mod_tex_heap_kib).
static uint64_t tex_heap_extra_kib(void) {
    uint64_t c = cfg_tex_heap_kib(), m = mod_tex_heap_kib();
    return c > m ? c : m;
}

uint64_t res_tex_heap_extra_bytes(void) { return ((tex_heap_extra_kib() << 10) + 0x1fffff) & ~0x1fffffull; }

// ---- the HeapDef table. BYML version 2..7, little endian ("YB"): a hash node is {u8 0xC1, u24 count} and count
// entries {u24 key index, u8 type, u32 value}; an int (type 0xD1) is stored in the entry, so a size changes in place.
typedef struct { uint8_t *p; size_t n; uint32_t keys; } Byml;

static int u32_at(const Byml *b, size_t off, uint32_t *v) {
    if (off + 4 > b->n) return 0;
    memcpy(v, b->p + off, 4);
    return 1;
}

static const char *key_name(const Byml *b, uint32_t k) {
    uint32_t head, at;
    if (!u32_at(b, b->keys, &head) || (head & 0xff) != 0xc2 || k >= head >> 8 || !u32_at(b, b->keys + 4 + 4 * k, &at) ||
        b->keys + at >= b->n || !memchr(b->p + b->keys + at, 0, b->n - b->keys - at))
        return NULL;
    return (const char *)b->p + b->keys + at;
}

// The offset of the value of hash node `node`'s entry `name`, which must have type `type`; 0 when there is none.
static size_t field(const Byml *b, uint32_t node, const char *name, uint8_t type) {
    uint32_t head;
    if (!u32_at(b, node, &head) || (head & 0xff) != 0xc1) return 0;
    for (uint32_t i = 0; i < head >> 8; i++) {
        uint32_t e;
        if (!u32_at(b, node + 4 + 8 * (size_t)i, &e)) return 0;
        const char *k = key_name(b, e & 0xffffff);
        if (k && !strcmp(k, name)) return e >> 24 == type ? node + 8 + 8 * (size_t)i : 0;
    }
    return 0;
}

static size_t heap_size_field(const Byml *b, uint32_t heaps, const char *heap) {
    size_t at = field(b, heaps, heap, 0xc1);
    uint32_t node;
    return at && u32_at(b, at, &node) ? field(b, node, "Size", 0xd1) : 0;
}

// A HeapDef in b's place (zstd frame in[0..n) with its content size, no dictionary): b.p malloc'd and *heaps its
// "Heaps" node; 0 when it is not one (b.p freed). why: what the log line says the caller keeps then.
static int heapdef_open(const void *in, size_t n, Byml *b, uint32_t *heaps, const char *why) {
    b->p = NULL;
    if (ZSTD_getDictID_fromFrame(in, n)) {
        rt_log("resolution: the HeapDef table needs a zstd dictionary; %s", why);
        return 0;
    }
    unsigned long long raw = ZSTD_getFrameContentSize(in, n);
    if (raw == ZSTD_CONTENTSIZE_UNKNOWN || raw == ZSTD_CONTENTSIZE_ERROR || raw > (64u << 20)) {
        rt_log("resolution: the HeapDef table is not a zstd frame with a size; %s", why);
        return 0;
    }
    *b = (Byml){malloc((size_t)raw), (size_t)raw, 0};
    if (!b->p) return 0;
    size_t got = ZSTD_decompress(b->p, b->n, in, n);
    uint32_t root;
    uint16_t ver = 0;
    if (!ZSTD_isError(got) && got == b->n && b->n >= 16) memcpy(&ver, b->p + 2, 2);
    size_t heaps_at = 0;
    if (ver >= 2 && ver <= 7 && !memcmp(b->p, "YB", 2) && u32_at(b, 4, &b->keys) && u32_at(b, 12, &root) &&
        (heaps_at = field(b, root, "Heaps", 0xc1)) && u32_at(b, heaps_at, heaps))
        return 1;
    free(b->p);
    b->p = NULL;
    return 0;
}

// HD texture packs made for the emulators ship their own HeapDef with "TextureHeap" and its parent grown (HDPacks.com's
// 4K pack for TotK: by 3,419,000 KiB, 3.3 GiB, for the emulators' 8 GB memory option). Served as it is, the game asks
// for that room and the application pool has none: it crashed at start (issue #673). So before mem_init sizes the pool,
// every switched-on mod's HeapDef is read (mods.c mods_peek_romfs: mods_scan needs paths main sets later) and the
// largest TextureHeap growth over the shipped size counts as TOTK_TEX_HEAP_EXTRA_MB (the larger of the two, at most
// 4 GiB). romfs.c heapdef_overlay then serves the game's own table grown by that amount in the mod's place, so the
// other heaps keep the sizes of the game version that runs.
#define HEAPDEF_REL "System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs"
static struct { int done; uint64_t kib; } MT;

static void mod_heapdef(const char *name, const char *romfs, void *ctx) {
    (void)ctx;
    char path[2300];
    if (snprintf(path, sizeof path, "%s/%s", romfs, HEAPDEF_REL) >= (int)sizeof path) return;
    uint64_t n = 0;
    uint8_t *in = plat_file_read(path, &n);
    if (!in) return;
    Byml b;
    uint32_t heaps;
    size_t th_at = 0;
    int32_t th = 0;
    if (heapdef_open(in, (size_t)n, &b, &heaps, "its texture heap is left out") &&
        (th_at = heap_size_field(&b, heaps, "TextureHeap")))
        memcpy(&th, b.p + th_at, 4);
    free(b.p);
    free(in);
    int64_t grow = (int64_t)th - (int64_t)TEX_HEAP_KIB;
    if (grow > (int64_t)(4096u << 10)) grow = 4096u << 10;
    if (grow > 0 && (uint64_t)grow > MT.kib) MT.kib = (uint64_t)grow;
    rt_log("resolution: mod %s ships a HeapDef with TextureHeap %d KiB (the game's %u KiB)%s", name, th, TEX_HEAP_KIB,
           grow > 0 ? ": the port grows the texture heap and the pool by the difference" : "");
}

static uint64_t mod_tex_heap_kib(void) {
    if (!MT.done) MT.done = 1, mods_peek_romfs(mod_heapdef, NULL);
    return MT.kib;
}

int res_tex_heap_from_mod(void) { return mod_tex_heap_kib() > 0; }

int res_heapdef_build(const void *in, size_t n, void **out, size_t *out_n) {
    uint64_t extra = res_heap_extra_kib(), tex = tex_heap_extra_kib();
    *out = NULL;
    if (!extra && !tex) return 0;
    Byml b;
    uint32_t heaps = 0;
    size_t dta_at = 0, gfx_at = 0;
    if (heapdef_open(in, n, &b, &heaps, "the game keeps its own resolution")) {
        dta_at = heap_size_field(&b, heaps, "DynamicTextureAllocatorMem");
        gfx_at = heap_size_field(&b, heaps, "Graphics");
    }
    if (!dta_at || !gfx_at) {
        rt_log("resolution: the HeapDef table has no DynamicTextureAllocatorMem / Graphics sizes; the game keeps its own "
               "resolution");
        free(b.p);
        return 0;
    }
    int32_t dta, gfx;
    memcpy(&dta, b.p + dta_at, 4);
    memcpy(&gfx, b.p + gfx_at, 4);
    int32_t dta2 = dta + (int32_t)extra, gfx2 = gfx + (int32_t)extra;
    memcpy(b.p + dta_at, &dta2, 4);
    memcpy(b.p + gfx_at, &gfx2, 4);
    int32_t th = 0, th2 = 0, gg = 0, gg2 = 0;
    if (tex) {   // TOTK_TEX_HEAP_EXTRA_MB: the world texture heap and its parent grow by the same amount
        static const char parent[] = "\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0\xe3\x82\xb0\xe3\x83\xa9\xe3\x83\x95"
                                     "\xe3\x82\xa3\xe3\x83\x83\xe3\x82\xaf\xe3\x82\xb9";   // ゲームグラフィックス
        size_t th_at = heap_size_field(&b, heaps, "TextureHeap"), gg_at = heap_size_field(&b, heaps, parent);
        if (th_at && gg_at) {
            memcpy(&th, b.p + th_at, 4);
            memcpy(&gg, b.p + gg_at, 4);
            th2 = th + (int32_t)tex, gg2 = gg + (int32_t)tex;
            memcpy(b.p + th_at, &th2, 4);
            memcpy(b.p + gg_at, &gg2, 4);
        } else {
            rt_log("resolution: the HeapDef table has no TextureHeap sizes; TOTK_TEX_HEAP_EXTRA_MB left out");
            tex = 0;
            if (!extra) {
                free(b.p);
                return 0;
            }
        }
    }
    size_t cap = ZSTD_compressBound(b.n);
    void *z = malloc(cap);
    size_t zn = z ? ZSTD_compress(z, cap, b.p, b.n, 9) : 0;
    free(b.p);
    if (!z || ZSTD_isError(zn)) {
        free(z);
        return 0;
    }
    *out = z;
    *out_n = zn;
    if (extra)
        rt_log("resolution: HeapDef DynamicTextureAllocatorMem %d -> %d KiB, Graphics %d -> %d KiB", dta, dta2, gfx,
               gfx2);
    if (tex)
        rt_log("resolution: HeapDef TextureHeap %d -> %d KiB and its parent %d -> %d KiB (TOTK_TEX_HEAP_EXTRA_MB or a "
               "mod's HeapDef)", th, th2, gg, gg2);
    return 1;
}

void res_heapdef_served(void) { R.heapdef_ok = 1; }

// ---- sun shadow map size: each frame the engine sizes its shadow cascades to the area they cover, down to about 400 near
// the ground, which draws foliage shadows blocky, and each change of size moves the shadow texels, so leaf shadows jump.
// A fixed size (the shadow set, recomp/patches-v121.txt) keeps them still and sharper. Page +92.
static void shadow_init(uint8_t *page) {
    if (!R.shadow) {
        rt_log("resolution: sun shadow map sized by the game (TOTK_SHADOW_RES=game)");
        return;
    }
    int set = rt_patch_set("shadow");
    if (set < 0) {
        rt_log("resolution: the generated C has no shadow patch set: the game sizes its shadow map");
        return;
    }
    int32_t n = (int32_t)R.shadow;
    memcpy(page + 92, &n, 4);
    rt_patch_switch(set, 1);
    rt_log("resolution: sun shadow map %ux%u (TOTK_SHADOW_RES)", R.shadow, R.shadow);
}

// ---- start: the patch page and the sets
static uint64_t pack(unsigned w, unsigned h) { return (uint64_t)h << 32 | w; }

// TOTK_SHADOW_RANGE (percent, 50 to 400; 100 = the game's): the sun's shadow cascades end at 20, 40, 80 and 160 m.
// Other values multiply those distances through the cascade patch set (recomp/patches-v143.txt), so the sharp first
// cascade reaches further and the visible steps between cascades move away from the player, at the cost of shadow
// texels per metre (TOTK_SHADOW_SCALE pays that back).
static void res_shadow_range(void) {
    int pct = cfg_int("TOTK_SHADOW_RANGE"), set = rt_patch_set("cascade");
    if (pct == 100) return;
    if (set < 0) {
        rt_log("resolution: the generated C has no cascade patch set; shadow_range stays 100");
        return;
    }
    float k = (float)(pct < 50 ? 50 : pct > 400 ? 400 : pct) / 100.0f, d[4] = {20 * k, 40 * k, 80 * k, 160 * k};
    memcpy((uint8_t *)rt_patch_page() + 64, d, sizeof d);
    rt_patch_switch(set, 1);
    rt_log("resolution: shadow cascades end at %g, %g, %g and %g m", d[0], d[1], d[2], d[3]);
}

void res_init(void) {
    res_shadow_range();
    if (!bigger()) {
        rt_log("resolution: the game's own (output 1920x1080, scene 1600x900)");
        shadow_init(rt_patch_page());
        return;
    }
    int output = rt_patch_set("output"), scene = rt_patch_set("scene");
    if (output < 0 || scene < 0) {
        rt_log("resolution: the generated C has no output and scene patch sets; the game keeps its own resolution");
        return;
    }
    fs_init();   // romfs.c serves the grown HeapDef from here on (res_heapdef_served)
    if (res_heap_extra_kib() && !R.heapdef_ok) {   // sizes at or below the shipped ones fit the shipped heaps
        rt_log("resolution: no grown HeapDef table; the game keeps its own resolution");
        return;
    }
    uint8_t *page = rt_patch_page();
    double k = RT_V143 ? (double)R.ow / BASE_W : 1.0;   // 1.4.3's own scaling of the scene with the output width
    int scene_on = R.scene_set || (!RT_V143 && (R.ow != BASE_W || R.oh != BASE_H));
    float sc[2] = {(float)(R.sw / k), (float)(R.sh / k)};
    uint64_t out = pack(R.ow, R.oh), none = 0;
    memcpy(page + 16, sc, sizeof sc);
    memcpy(page + 32, &out, 8);
    memcpy(page + 40, &none, 8);
    memcpy(page + 48, &out, 8);
    rt_patch_switch(output, R.ow != BASE_W || R.oh != BASE_H);
    rt_patch_switch(scene, scene_on);
    R.on = 1;
    if (R.live) g_fnprof = 1;   // until the game sets its render size: res_fn_hit, or res_present gives up
    // Another shape than 16:9 (more than 1% off): the camera takes the output's ratio (recomp/patches-v121.txt and
    // patches-v143.txt, set aspect), so a 21:9 output shows more of the world at the sides instead of a stretched 16:9 view. The HUD's layouts
    // for that shape come from romfs-patches-<w>x<h> (romfs.c overlay_init, tools/make_romfs_patches.py --aspect).
    double ratio = (double)R.ow / R.oh, game = (double)BASE_W / BASE_H;
    int aspect = rt_patch_set("aspect");
    if (!res_is_16x9(R.ow, R.oh)) {
        if (aspect < 0) {
            rt_log("resolution: output %ux%u is not 16:9 and this game version has no camera aspect patch; the view "
                   "is stretched", R.ow, R.oh);
        } else {
            float r = (float)ratio;
            memcpy(page + 56, &r, sizeof r);
            rt_patch_switch(aspect, 1);
            rt_log("resolution: camera aspect ratio %.4f (16:9 is %.4f)", ratio, game);
        }
    } else {
        rt_patch_switch(aspect, 0);   // the page holds no ratio
    }
    shadow_init(page);
    rt_log("resolution: output %ux%u, scene %ux%u%s; texture heap +%llu KiB", R.ow, R.oh, R.sw, R.sh,
           scene_on ? "" : " (the game's own 5/6 of the output)", (unsigned long long)res_heap_extra_kib());
}

// DLSS upscaling: textures are sampled for the output's resolution, not the scene's (NVIDIA's DLSS guide: a mip bias
// of log2(render / output)), so their detail survives the scaling. 0 without a DLSS mode.
float res_dlss_lod_bias(void) {
    decide();
    return R.dlss_k ? (float)log2(R.dlss_k) : 0.0f;
}

// 16:9 within 1% (1366x768 counts): the shape the game's own layouts and camera are made for.
int res_is_16x9(unsigned w, unsigned h) {
    double rel = ((double)w / h) / ((double)BASE_W / BASE_H);
    return rel >= 0.99 && rel <= 1.01;
}

// The game resolution asks for a romfs-patches-<w>x<h> overlay: it is not 16:9 and the game version has the camera
// aspect patch (set aspect); the UI layouts of another shape only fit a camera that follows the same shape.
int res_wants_wide_overlay(const unsigned out[2]) {
    return !res_is_16x9(out[0], out[1]) && rt_patch_set("aspect") >= 0;
}

// Field of view (TOTK_FOV, percent of the game's own, 70..140, live): the camera's vertical view angle times the
// factor on page +60 (recomp/patches-v143.txt, set fov). The horizontal angle follows through the aspect ratio. 100
// switches the set off, so the camera code is the game's own. Runs on a settings change (gfx_present.c
// window_settings_live), the first time at the first present.
#define FOV_MIN 70
#define FOV_MAX 140
void res_fov_live(void) {
    static int set = -2, shown = 100;
    if (set == -2) set = rt_patch_set("fov");
    int pct = cfg_int("TOTK_FOV");
    pct = pct < FOV_MIN ? FOV_MIN : pct > FOV_MAX ? FOV_MAX : pct;
    if (pct == shown) return;
    shown = pct;
    if (set < 0) {
        rt_log("resolution: this game version has no field-of-view patch; TOTK_FOV=%d does nothing", pct);
        return;
    }
    float k = pct / 100.0f;
    memcpy((uint8_t *)rt_patch_page() + 60, &k, sizeof k);   // before the switch: a site that sees it on reads k
    rt_patch_switch(set, pct != 100);
    rt_log("resolution: field of view %d%% of the game's", pct);
}

void res_wanted(unsigned out[2]) {
    decide();
    out[0] = R.ow, out[1] = R.oh;
}

// A TOTK_DLSS change while the game runs (vk_dlaa.c, at a present): the new scene size goes to the patch page, where
// the game reads it when it sets its render size (main+0x585e48). 1.4.3 does that once, at start: its renderer setup
// (main+0x7000e67028) calls main+0x585bd0(renderer, force), which picks the console mode's sizes and flags the render
// targets for re-creation. The setup call is caught through the function-entry hook (res_fn_hit, on only until then),
// and the next nvnQueuePresentTexture on the game's thread calls it again with force set (res_present), between frames
// as at start. The heap was sized for the largest scene a mode asks for. 1 when the size changed.
#define SET_RENDER_SIZE 0x7000585bd0ull
static uint64_t renderer;          // x0 of the game's own call of SET_RENDER_SIZE
static volatile int size_pending;  // the page holds a size the game has not set yet

void res_fn_hit(uint64_t addr, uint64_t x0) {
    if (addr != SET_RENDER_SIZE) return;
    renderer = x0;
    g_fnprof = 0;
    rt_log("resolution: renderer at %#llx; the scene size can change while the game runs", (unsigned long long)x0);
}

void res_present(Cpu *c) {
    // The hook runs on every guest function entry while it waits: should the game set its render size before
    // res_init or never, it stops after 1800 presents (half a minute at 60 fps) and modes change at the next start.
    static unsigned waited;
    if (g_fnprof && !renderer && ++waited == 1800) {
        g_fnprof = 0;
        rt_log("resolution: the game's render size setup was not seen; DLSS modes change at the next start");
    }
    if (!size_pending || !renderer) return;
    size_pending = 0;
    Cpu keep = *c;   // the guest's registers as at the present call: the setup runs as if it never happened
    rt_call(c, SET_RENDER_SIZE, 2, (uint64_t)renderer, (uint64_t)1);
    memcpy(c->x, keep.x, sizeof c->x);
    memcpy(c->v, keep.v, sizeof c->v);
    c->nzcv = keep.nzcv, c->fpsr = keep.fpsr;
    rt_log("resolution: the game set the scene size %ux%u", R.sw, R.sh);
}

int res_dlss_mode(const char *m) {
    decide();
    double k = dlss_share(m);
    if (k == R.dlss_k || !R.on) return 0;
    if (!RT_V143 || !renderer) {
        rt_log("resolution: DLSS %s takes effect at the next start (the game set its render size already)", m);
        return 0;
    }
    R.dlss_k = k;
    unsigned w, h;
    scene_for(k ? k : R.base_k, &w, &h);
    if (w == R.sw && h == R.sh) return 0;
    double s = RT_V143 ? (double)R.ow / BASE_W : 1.0;
    float sc[2] = {(float)(w / s), (float)(h / s)};
    memcpy((uint8_t *)rt_patch_page() + 16, sc, sizeof sc);
    R.sw = w, R.sh = h;
    size_pending = 1;
    rt_log("resolution: DLSS %s: scene %ux%u", m, w, h);
    return 1;
}

void res_sizes(unsigned out[2], unsigned scene[2]) {
    decide();
    out[0] = R.on ? R.ow : BASE_W, out[1] = R.on ? R.oh : BASE_H;
    scene[0] = R.on ? R.sw : SCENE_W, scene[1] = R.on ? R.sh : SCENE_H;
}
