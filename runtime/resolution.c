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
#include "platform/platform.h"

#define BASE_W 1920
#define BASE_H 1080
#define SCENE_W 1600
#define SCENE_H 900
#define DTA_KIB 113664u      // DynamicTextureAllocatorMem, shipped
#define SCENE_MAX 4096       // per side, as MacNativeGames' TOTK_RENDER_RES
#define SHADOW_BASE 960      // the sun shadow map's base size (its cascades: a D16 array of 4 layers)
#define SHADOW_LIVE_MAX 2048 // the largest TOTK_SHADOW_RES the menu offers (ui/menu.cpp row_shadow_res)
#define SCALE_LIVE_MAX 2.0   // the largest TOTK_RENDER_SCALE the menu offers (ui/menu.cpp row_render_scale)
#define LIVE_HEAP_KIB (2048u << 10)   // at most this much heap is added for live changes beyond the start's sizes

static struct {
    int decided;
    unsigned ow, oh, sw, sh;   // output and scene
    unsigned ew, eh, esw, esh; // the output and scene the game renders now (res_sizes): ow.. once its setter ran
    unsigned bw, bh;           // the largest output: the swapchain size nnMain reads (page +48), the most the game's
                               // window textures hold, and what the texture heap is sized for
    int scene_set;             // the scene differs from the game's 5/6 of the output
    int heapdef_ok;            // romfs.c serves the grown HeapDef
    int on;                    // res_init switched the patch sets on
    unsigned shadow;           // fixed sun shadow map size (TOTK_SHADOW_RES), 0: the game's own
    double dlss_k;             // TOTK_DLSS quality/balanced/performance/ultra_performance: the scene's share of the output, else 0
    double base_k;             // the scene's share without a DLSS mode (render_scale, or the game's 5/6)
    unsigned hw, hh;           // the largest scene a DLSS mode change can ask for: the heap is sized for it
    int live;                  // the game version's render size setter is known (set_render_size): the scene size
                               // can change while the game runs (render_scale, a DLSS mode)
    unsigned shadow_cap;       // the largest fixed shadow map size the texture heap holds (shadow_kib)
    int shadow_set;            // the shadow patch set, -1 when this game version has none
    int shadow_live;           // shadow_init ran with the set: res_shadow_live may change the size
    int sparse;                // physical pools take video memory as images use them (vk_sparse_probe): a larger heap
                               // costs address space, not video memory
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

// A TOTK_SHADOW_RES value: a fixed size 256..4096 (a multiple of 8), or 0 for the game's own size (game, or a value
// that does not parse, logged unless quiet).
static unsigned shadow_parse(const char *sr, int quiet) {
    if (!strcmp(sr, "game")) return 0;
    unsigned n = (unsigned)atoi(sr);
    if (n >= 256 && n <= 4096 && !(n & 7)) return n;
    if (!quiet) rt_log("resolution: TOTK_SHADOW_RES=%s is not game or a size 256..4096 (a multiple of 8); game", sr);
    return 0;
}

// The function that sets the renderer's sizes from the page and flags its render targets for re-creation, and how it
// is called again (res_present); 0 where it is not known, and the scene keeps its size until the next start.
//   1.4.3: main+0x585bd0(renderer, force): picks the console mode, calls main+0x585da0(renderer, mode), whose scene
//          site is the scene set's main+0x585e48, and flags the render targets (+0xca0 |= 1).
//   1.2.1: main+0xcc3f10(renderer, mode): its output and scene sites (main+0xcc3f50, +0xcc3f68) read the page, and it
//          flags the render targets itself (+0xca0 |= 9). Its callers (main+0xcc01d8, +0xcc170c, +0x11accd4) pass the
//          mode they store at +0xcd0 (int), so a call again passes that.
static uint64_t set_render_size(void) {
    return rt_game_version >= 143 ? 0x7000585bd0ull : rt_game_version == 121 ? 0x7000cc3f10ull : 0;
}

// A TOTK_RENDER_SCALE value as the scene's share of the output: "game" is the game's 5/6 (*set 0), a percentage
// 25..400 sets it (*set 1); anything else is the game's, logged unless quiet.
static double scale_parse(const char *s, int quiet, int *set) {
    *set = 0;
    if (!strcmp(s, "game")) return 5.0 / 6.0;
    char *end;
    double pct = strtod(s, &end);
    if (end != s && !*end && pct >= 25 && pct <= 400) {
        *set = 1;
        return pct / 100.0;
    }
    if (!quiet) rt_log("resolution: TOTK_RENDER_SCALE=%s is not game or a percentage 25..400; game", s);
    return 5.0 / 6.0;
}

// A TOTK_OUTPUT_RES value: <width>x<height> in 1280x720..7680x4320, made even; else 1920x1080, logged unless quiet.
static void output_parse(const char *o, int quiet, unsigned *ow, unsigned *oh) {
    unsigned w = 0, h = 0;
    char tail;
    if (sscanf(o, "%ux%u%c", &w, &h, &tail) == 2 && w >= 1280 && h >= 720 && w <= 7680 && h <= 4320) {
        *ow = w & ~1u, *oh = h & ~1u;
        return;
    }
    if (!quiet) rt_log("resolution: TOTK_OUTPUT_RES=%s is not <width>x<height> in 1280x720..7680x4320; 1920x1080", o);
    *ow = BASE_W, *oh = BASE_H;
}

// The heap growth (KiB) for the largest output bw x bh and the largest scene hw x hh, as res_heap_extra_kib counts it
// without the shadow map.
static double heap_grow_kib(unsigned bw, unsigned bh, unsigned hw, unsigned hh) {
    double grow = fmax(0.0, (double)bw * bh / (BASE_W * BASE_H) - 1.0) + fmax(0.0, (double)hw * hh / (SCENE_W * SCENE_H) - 1.0);
    return DTA_KIB * grow;
}

// The largest scene share live changes may ask for: render_scale 100 (which holds every DLSS mode) where pools take
// their whole size in video memory, else the menu's largest (200), or the next smaller step whose heap stays within
// LIVE_HEAP_KIB of the start's own; never below the share asked for at start (sk).
static double scene_cap(double sk) {
    static const double steps[] = {SCALE_LIVE_MAX, 1.5, 1.25, 1.0};
    unsigned ow = R.ow, oh = R.oh, w, h;
    R.ow = R.bw, R.oh = R.bh;
    scene_for(sk, &w, &h);
    double own = heap_grow_kib(R.bw, R.bh, w, h), cap = 1.0;
    for (int i = 0; R.sparse && i < (int)(sizeof steps / sizeof *steps); i++) {
        scene_for(steps[i], &w, &h);
        if (heap_grow_kib(R.bw, R.bh, w, h) - own <= LIVE_HEAP_KIB) { cap = steps[i]; break; }
    }
    R.ow = ow, R.oh = oh;
    return fmax(cap, sk);
}

static void decide(void) {
    if (R.decided) return;
    R.decided = 1;
    output_parse(cfg_value("TOTK_OUTPUT_RES"), 0, &R.ow, &R.oh);
    R.live = set_render_size() != 0;
    R.sparse = R.live && vk_sparse_probe();
    // Where the output can change while the game runs (res_output_live) and a larger heap costs no video memory
    // (R.sparse), the swapchain and the heap are made for the largest size of the output's shape that the display
    // holds, so the menu can go up to the display's size as well as down; a smaller display, an unknown one or pools
    // that take their whole size in video memory leave the size asked for (down only).
    R.bw = R.ow, R.bh = R.oh;
    int dw, dh;
    if ((rt_game_version == 121 || RT_V143) && R.sparse && plat_screen_size(&dw, &dh) && dw > 0 && dh > 0) {
        double f = fmin((double)dw / R.ow, (double)dh / R.oh);
        unsigned cw = (unsigned)(R.ow * f) & ~1u, ch = (unsigned)(R.oh * f) & ~1u;
        if (f > 1 && cw <= 7680 && ch <= 4320) {
            R.bw = cw, R.bh = ch;
            rt_log("resolution: the display is %dx%d: the game resolution can change up to %ux%u while the game runs",
                   dw, dh, cw, ch);
        }
    }
    double k = scale_parse(cfg_value("TOTK_RENDER_SCALE"), 0, &R.scene_set);
    // A DLSS mode other than dlaa sets the scene size instead. Where the game's render size setter is known
    // (set_render_size) render_scale and the DLSS mode can change while the game runs (res_scale_live,
    // res_dlss_mode): the scene patch is then always on, and the heap is sized for the larger of the scene asked for at
    // start and the largest share live changes may ask for (scene_cap); a larger scene waits for a restart. Elsewhere
    // the game keeps its own scene size unless render_scale or a DLSS mode asks for another, and a heap sized for that
    // scene alone.
    R.base_k = k;
    const char *m = cfg_value("TOTK_DLSS");
    R.dlss_k = dlss_share(m);
    if (R.dlss_k) rt_log("resolution: DLSS %s: the scene at %.0f %% of the output (render_scale ignored)", m, R.dlss_k * 100);
    double sk = R.dlss_k ? R.dlss_k : k;
    if (R.live || R.dlss_k || R.ow * sk > SCENE_MAX || R.oh * sk > SCENE_MAX) R.scene_set = 1;
    scene_for(sk, &R.sw, &R.sh);
    if (R.live) {
        double cap = scene_cap(sk);
        unsigned ow = R.ow, oh = R.oh;
        R.ow = R.bw, R.oh = R.bh;   // the scene at the largest output
        scene_for(cap, &R.hw, &R.hh);
        R.ow = ow, R.oh = oh;
        rt_log("resolution: the scene can change up to %ux%u while the game runs (render_scale %.0f %%%s)", R.hw, R.hh,
               cap * 100, R.sparse ? "" : "; physical pools take their whole size in video memory");
    } else {
        R.hw = R.sw, R.hh = R.sh;
    }
    R.shadow = shadow_parse(cfg_value("TOTK_SHADOW_RES"), 0);
    // The size can change while the game runs (res_shadow_live), so where the shadow set exists the heap is sized for
    // the menu's largest choice as well as for the size asked for at start: 75 MiB more than the game's own at 2048.
    R.shadow_set = rt_patch_set("shadow");
    R.shadow_cap = R.shadow;
    if (R.shadow_set >= 0 && R.shadow_cap < SHADOW_LIVE_MAX) R.shadow_cap = SHADOW_LIVE_MAX;
}

// Texture heap for a sun shadow map above the base size, at the largest size the heap must hold (shadow_cap): the
// four layers of 2 bytes, three maps in flight (the engine makes the array anew each frame).
static uint64_t shadow_kib(void) {
    unsigned n = R.shadow_cap;
    return n > SHADOW_BASE ? ((uint64_t)n * n - SHADOW_BASE * SHADOW_BASE) * 8 * 3 / 1024 : 0;
}

static int bigger(void) {
    decide();
    return R.ow != BASE_W || R.oh != BASE_H || R.bw != BASE_W || R.bh != BASE_H || R.scene_set || shadow_kib();
}

uint64_t res_heap_extra_kib(void) {
    if (!bigger()) return 0;
    double grow = fmax(0.0, (double)R.bw * R.bh / (BASE_W * BASE_H) - 1.0) +
                  fmax(0.0, (double)R.hw * R.hh / (SCENE_W * SCENE_H) - 1.0);
    uint64_t kib = grow > 0 ? (uint64_t)ceil(DTA_KIB * (1.0 + grow) / 1024.0) * 1024 - DTA_KIB : 0;
    return kib + ((shadow_kib() + 1023) & ~1023ull);   // whole MiB
}

uint64_t res_heap_extra_bytes(void) { return ((res_heap_extra_kib() << 10) + 0x1fffff) & ~0x1fffffull; }

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

int res_heapdef_build(const void *in, size_t n, void **out, size_t *out_n) {
    uint64_t extra = res_heap_extra_kib();
    *out = NULL;
    if (!extra) return 0;
    if (ZSTD_getDictID_fromFrame(in, n)) {
        rt_log("resolution: the HeapDef table needs a zstd dictionary; the game keeps its own resolution");
        return 0;
    }
    unsigned long long raw = ZSTD_getFrameContentSize(in, n);
    if (raw == ZSTD_CONTENTSIZE_UNKNOWN || raw == ZSTD_CONTENTSIZE_ERROR || raw > (64u << 20)) {
        rt_log("resolution: the HeapDef table is not a zstd frame with a size; the game keeps its own resolution");
        return 0;
    }
    Byml b = {malloc((size_t)raw), (size_t)raw, 0};
    if (!b.p) return 0;
    size_t got = ZSTD_decompress(b.p, b.n, in, n);
    uint32_t root, heaps = 0;
    uint16_t ver = 0;
    if (!ZSTD_isError(got) && got == b.n && b.n >= 16) memcpy(&ver, b.p + 2, 2);
    size_t heaps_at = 0, dta_at = 0, gfx_at = 0;
    if (ver >= 2 && ver <= 7 && !memcmp(b.p, "YB", 2) && u32_at(&b, 4, &b.keys) && u32_at(&b, 12, &root) &&
        (heaps_at = field(&b, root, "Heaps", 0xc1)) && u32_at(&b, heaps_at, &heaps)) {
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
    rt_log("resolution: HeapDef DynamicTextureAllocatorMem %d -> %d KiB, Graphics %d -> %d KiB", dta, dta2, gfx, gfx2);
    return 1;
}

void res_heapdef_served(void) { R.heapdef_ok = 1; }

// ---- sun shadow map size: each frame the engine sizes its shadow cascades to the area they cover, down to about 400 near
// the ground, which draws foliage shadows blocky, and each change of size moves the shadow texels, so leaf shadows jump.
// A fixed size (the shadow set, recomp/patches-v121.txt) keeps them still and sharper. Page +92.
// The engine reads the size again every frame and makes the cascade array anew at it, so a new size on the page (or
// the set switched off for the game's own) takes effect at the next frame: res_shadow_live, up to shadow_cap.
static void shadow_init(uint8_t *page) {
    R.shadow_live = R.shadow_set >= 0;
    if (!R.shadow) {
        rt_log("resolution: sun shadow map sized by the game (TOTK_SHADOW_RES=game)");
        return;
    }
    if (R.shadow_set < 0) {
        rt_log("resolution: the generated C has no shadow patch set: the game sizes its shadow map");
        return;
    }
    int32_t n = (int32_t)R.shadow;
    memcpy(page + 92, &n, 4);
    rt_patch_switch(R.shadow_set, 1);
    rt_log("resolution: sun shadow map %ux%u (TOTK_SHADOW_RES)", R.shadow, R.shadow);
}

// TOTK_SHADOW_RES changed while the game runs (gfx_present.c window_settings_live): the new size goes to page +92
// before the set is switched on, as res_fov_live does, so a frame that sees the set on reads the new size. A size
// above shadow_cap would not fit the texture heap sized at start: it waits for the next start.
void res_shadow_live(void) {
    if (!R.shadow_live) return;
    unsigned n = shadow_parse(cfg_value("TOTK_SHADOW_RES"), 1);
    if (n == R.shadow) return;
    if (n > R.shadow_cap) {
        static unsigned told;
        if (told != n) rt_log("resolution: sun shadow map %ux%u takes effect at the next start (the texture heap holds "
                              "up to %u)", n, n, R.shadow_cap);
        told = n;
        return;
    }
    if (n) {
        int32_t v = (int32_t)n;
        memcpy((uint8_t *)rt_patch_page() + 92, &v, 4);
    }
    rt_patch_switch(R.shadow_set, n != 0);
    R.shadow = n;
    if (n) rt_log("resolution: sun shadow map %ux%u (changed in the menu)", n, n);
    else rt_log("resolution: sun shadow map sized by the game again (changed in the menu)");
}

// 1 when the TOTK_SHADOW_RES in effect now cannot apply before a restart (the menu's note under the value).
int res_shadow_needs_restart(void) {
    unsigned n = shadow_parse(cfg_value("TOTK_SHADOW_RES"), 1);
    return R.shadow_live ? n > R.shadow_cap : n != R.shadow;
}

// ---- start: the patch page and the sets
static uint64_t pack(unsigned w, unsigned h) { return (uint64_t)h << 32 | w; }

// TOTK_SHADOW_RANGE (percent, 50 to 400; 100 = the game's): the sun's shadow cascades end at 20, 40, 80 and 160 m.
// Other values multiply those distances through the cascade patch set (recomp/patches-v121.txt, patches-v143.txt),
// so the sharp first cascade reaches further and the visible steps between cascades move away from the player, at
// the cost of shadow texels per metre (TOTK_SHADOW_SCALE pays that back). The set scales the distances where the game
// copies them into its cascades, every frame, so the menu changes it while the game runs: the factor goes to page +64
// before the set is switched on, and 100 switches it off, which leaves the game's own code. Runs at start (res_init)
// and on a settings change (gfx_present.c window_settings_live).
void res_shadow_range(void) {
    static int set = -2, shown = 100;
    if (set == -2) set = rt_patch_set("cascade");
    int pct = cfg_int("TOTK_SHADOW_RANGE");
    pct = pct < 50 ? 50 : pct > 400 ? 400 : pct;
    if (pct == shown) return;
    shown = pct;
    if (set < 0) {
        rt_log("resolution: the generated C has no cascade patch set; shadow_range stays 100");
        return;
    }
    float k = (float)pct / 100.0f;
    memcpy((uint8_t *)rt_patch_page() + 64, &k, sizeof k);
    rt_patch_switch(set, pct != 100);
    rt_log("resolution: shadow cascades end at %g, %g, %g and %g m (shadow_range %d%%)", 20 * k, 40 * k, 80 * k,
           160 * k, pct);
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
    uint64_t out = pack(R.ow, R.oh), swap = pack(R.bw, R.bh), none = 0;
    memcpy(page + 16, sc, sizeof sc);
    memcpy(page + 32, &out, 8);
    memcpy(page + 40, &none, 8);
    memcpy(page + 48, &swap, 8);   // the game's window textures: the largest output (decide); it crops to the output
    rt_patch_switch(output, R.ow != BASE_W || R.oh != BASE_H || R.bw != BASE_W || R.bh != BASE_H);
    rt_patch_switch(scene, scene_on);
    R.on = 1;
    R.ew = R.ow, R.eh = R.oh, R.esw = R.sw, R.esh = R.sh;
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
    rt_log("resolution: output %ux%u (up to %ux%u), scene %ux%u%s (up to %ux%u); texture heap +%llu KiB", R.ow, R.oh,
           R.bw, R.bh, R.sw, R.sh, scene_on ? "" : " (the game's own 5/6 of the output)", R.hw, R.hh,
           (unsigned long long)res_heap_extra_kib());
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

// The scene size while the game runs (render_scale and DLSS modes from the menu): the new size goes to the patch page,
// where the game reads it when it sets its render size (set_render_size). The game does that at start and when the
// console mode changes. The first call is caught through the function-entry hook (res_fn_hit, on only until then),
// and the next nvnQueuePresentTexture on the game's thread calls it again (res_present), between frames as at start;
// the game then makes its scene render targets anew at the new size. The heap holds scenes up to hw x hh.
static uint64_t renderer;          // x0 of the game's own call of set_render_size
static volatile int size_pending;  // the page holds a size the game has not set yet
static void canvas_present(void);  // 1.4.3: the output waits for the display configuration (res_output_live)
static volatile uint64_t canvas_pending;   // 1.4.3: the output size the display configuration has not taken yet

void res_fn_hit(uint64_t addr, uint64_t x0) {
    if (!addr || addr != set_render_size()) return;
    renderer = x0;
    g_fnprof = 0;
    rt_log("resolution: renderer at %#llx; the scene size can change while the game runs", (unsigned long long)x0);
}

void res_present(Cpu *c) {
    // The hook runs on every guest function entry while it waits: should the game set its render size before
    // res_init or never, it stops after 1800 presents (half a minute at 60 fps) and sizes change at the next start.
    static unsigned waited;
    if (g_fnprof && !renderer && ++waited == 1800) {
        g_fnprof = 0;
        rt_log("resolution: the game's render size setup was not seen; the scene size changes at the next start");
    }
    if (RT_V143) canvas_present();
    if (!size_pending || !renderer) return;
    size_pending = 0;
    Cpu keep = *c;   // the guest's registers as at the present call: the setup runs as if it never happened
    if (RT_V143) {
        rt_call(c, set_render_size(), 2, (uint64_t)renderer, (uint64_t)1);   // force
    } else {
        int32_t mode;
        memcpy(&mode, (const void *)(uintptr_t)(renderer + 0xcd0), sizeof mode);   // what the game's callers store
        rt_call(c, set_render_size(), 2, (uint64_t)renderer, (uint64_t)(uint32_t)mode);
    }
    memcpy(c->x, keep.x, sizeof c->x);
    memcpy(c->v, keep.v, sizeof c->v);
    c->nzcv = keep.nzcv, c->fpsr = keep.fpsr;
    R.ew = R.ow, R.eh = R.oh, R.esw = R.sw, R.esh = R.sh;   // DLSS and the trim see the new sizes from this frame
    rt_log("resolution: the game set the scene size %ux%u", R.sw, R.sh);
    vk_texture_trim_soon();   // the old sizes' targets and the video memory under them go a moment later
}

// 1 when a scene of share k can be set while the game runs: the setter is known and was seen (or the hook still
// waits for its first call, which reads the page anyway), and the heap holds the scene.
static int scene_fits(double k, unsigned *w, unsigned *h) {
    scene_for(k, w, h);
    return R.on && R.live && (renderer || g_fnprof) && *w <= R.hw && *h <= R.hh;
}

// The scene size to the page (1.4.3 scales what it reads by output width / 1920, res_init) and the setter asked to
// run again at the next present.
static void scene_apply(unsigned w, unsigned h) {
    double s = RT_V143 ? (double)R.ow / BASE_W : 1.0;
    float sc[2] = {(float)(w / s), (float)(h / s)};
    memcpy((uint8_t *)rt_patch_page() + 16, sc, sizeof sc);
    R.sw = w, R.sh = h;
    if (!canvas_pending) size_pending = 1;   // else canvas_present asks for it once the output is in place
}

// A TOTK_DLSS change while the game runs (vk_dlaa.c, at a present). 1 when the size changed.
int res_dlss_mode(const char *m) {
    decide();
    double k = dlss_share(m);
    if (k == R.dlss_k || !R.on) return 0;
    unsigned w, h;
    if (!scene_fits(k ? k : R.base_k, &w, &h)) {
        rt_log("resolution: DLSS %s takes effect at the next start", m);
        return 0;
    }
    R.dlss_k = k;
    if (w == R.sw && h == R.sh) return 0;
    scene_apply(w, h);
    rt_log("resolution: DLSS %s: scene %ux%u", m, w, h);
    return 1;
}

// TOTK_RENDER_SCALE changed while the game runs (gfx_present.c window_settings_live). While a DLSS mode sets the
// scene, the new share waits for the mode to go back to dlaa (res_dlss_mode reads base_k).
void res_scale_live(void) {
    decide();
    int set;
    double k = scale_parse(cfg_value("TOTK_RENDER_SCALE"), 1, &set);
    if (k == R.base_k) return;
    unsigned w, h;
    if (!scene_fits(k, &w, &h)) {
        static double told;
        if (told != k) rt_log("resolution: render_scale %.0f %% (scene %ux%u) takes effect at the next start (the heap "
                              "holds up to %ux%u)", k * 100, w, h, R.hw, R.hh);
        told = k;
        return;
    }
    R.base_k = k;
    if (R.dlss_k || (w == R.sw && h == R.sh)) return;
    scene_apply(w, h);
    rt_log("resolution: render_scale %.0f %%: scene %ux%u", k * 100, w, h);
}

// The game resolution while the game runs, 1.2.1: its render size setter (main+0xcc3f10) reads the output size from
// page +32 (the output set's main+0xcc3f50 site) and lays out its canvas, crop and UI in it before it flags the render
// targets, so the output changes the way the scene does (scene_apply, res_present). What stays from start: the
// swapchain size nnMain read (page +48, the game's window textures), the texture heap, and the camera's shape and the
// HUD layouts of romfs-patches-<w>x<h>. So a new output fits when it has the shape of the one at start and is not
// larger, and its scene fits the heap.
// The display configuration 1.4.3's output site stored on page +40 (main+0x3dcb28 runs at start when the output set is
// on), 0 before.
static uint64_t display_config(void) {
    uint64_t v;
    memcpy(&v, (uint8_t *)rt_patch_page() + 40, sizeof v);
    return v;
}

static int output_fits(unsigned w, unsigned h, unsigned *sw, unsigned *sh) {
    if (!R.on || !R.live || !renderer || w > R.bw || h > R.bh) return 0;
    if (rt_game_version != 121 && !(RT_V143 && display_config())) return 0;
    double rel = ((double)w / h) / ((double)R.bw / R.bh);
    if (rel < 0.99 || rel > 1.01) return 0;
    unsigned ow = R.ow, oh = R.oh;
    R.ow = w, R.oh = h;   // scene_for scales the output
    scene_for(R.dlss_k ? R.dlss_k : R.base_k, sw, sh);
    R.ow = ow, R.oh = oh;
    return *sw <= R.hw && *sh <= R.hh;
}

// 1.4.3 keeps the output in its display configuration: canvas +0x10 and active output +0x508, which the output site
// main+0x3dcb28 sets from page +32 when the configuration's update (main+0x3dc9a8, from main+0x3db308 every frame)
// sees bit 21 of its flags (+8): it clears the bit and takes the canvas again (main+0x3dcb14), then hands the size on
// (main+0x3de5a8).
// So a new output here first goes to the page and that bit, and the render size setter runs (size_pending) only once
// +0x508 holds the new size, a frame later (canvas_pending; a scene change meanwhile waits for it too).
static unsigned canvas_tries;

static void canvas_present(void) {
    uint64_t want = canvas_pending, cfgp = display_config();
    if (!want || !cfgp) return;
    uint64_t now;
    memcpy(&now, (const void *)(uintptr_t)(cfgp + 0x508), sizeof now);
    if (now == want) {
        canvas_pending = 0;
        size_pending = 1;   // the scene and the targets follow (res_present)
        rt_log("resolution: the game took the output %llux%llu after %u frames", (unsigned long long)(want & 0xffffffff),
               (unsigned long long)(want >> 32), canvas_tries);
        return;
    }
    if (++canvas_tries > 240) {
        canvas_pending = 0;
        rt_log("resolution: the game did not take the output %llux%llu; it takes effect at the next start",
               (unsigned long long)(want & 0xffffffff), (unsigned long long)(want >> 32));
        return;
    }
    __atomic_fetch_or((volatile uint32_t *)(uintptr_t)(cfgp + 8), 0x200000u, __ATOMIC_SEQ_CST);   // until it takes it
}

// TOTK_OUTPUT_RES changed while the game runs (gfx_present.c window_settings_live).
void res_output_live(void) {
    decide();
    unsigned w, h, sw, sh;
    output_parse(cfg_value("TOTK_OUTPUT_RES"), 1, &w, &h);
    if (w == R.ow && h == R.oh) return;
    if (!output_fits(w, h, &sw, &sh)) {
        static unsigned told_w, told_h;
        if (told_w != w || told_h != h)
            rt_log("resolution: output %ux%u takes effect at the next start (live: the shape of %ux%u, at most that size)",
                   w, h, R.bw, R.bh);
        told_w = w, told_h = h;
        return;
    }
    uint64_t out = pack(w, h);
    memcpy((uint8_t *)rt_patch_page() + 32, &out, sizeof out);
    rt_patch_switch(rt_patch_set("output"), 1);   // off at start for 1920x1080; nnMain's site ran long ago
    R.ow = w, R.oh = h;
    scene_apply(sw, sh);   // 1.4.3 reads the scene relative to the new width: scene_apply divides by it
    if (RT_V143) size_pending = 0, canvas_tries = 0, canvas_pending = out;   // the canvas first (canvas_present)
    rt_log("resolution: output %ux%u, scene %ux%u (changed in the menu)", w, h, sw, sh);
}

// 1 when the TOTK_OUTPUT_RES in effect now cannot apply before a restart (the menu's note under the value).
int res_output_needs_restart(void) {
    decide();
    unsigned w, h, sw, sh;
    output_parse(cfg_value("TOTK_OUTPUT_RES"), 1, &w, &h);
    return (w != R.ow || h != R.oh) && !output_fits(w, h, &sw, &sh);
}

// 1 when the TOTK_RENDER_SCALE in effect now cannot apply before a restart (the menu's note under the value).
int res_scale_needs_restart(void) {
    decide();
    int set;
    double k = scale_parse(cfg_value("TOTK_RENDER_SCALE"), 1, &set);
    unsigned w, h;
    return k != R.base_k && !scene_fits(k, &w, &h);
}

// The sizes the game renders now: a change from the menu shows here from the frame its setter ran (res_present), so
// DLSS does not take the old size's targets for the new one in the frame between.
void res_sizes(unsigned out[2], unsigned scene[2]) {
    decide();
    out[0] = R.on ? R.ew : BASE_W, out[1] = R.on ? R.eh : BASE_H;
    scene[0] = R.on ? R.esw : SCENE_W, scene[1] = R.on ? R.esh : SCENE_H;
}
