// Interface of the graphics layer: window, shader service, back ends and the NVN front end.
#pragma once
#include <stdint.h>
#include "host.h"
#include "nvn_obj.h"

// ---- moved from ov.h
enum { OE_MSG_FOCUS_STATE_CHANGED = 15, OE_MSG_OPERATION_MODE_CHANGED = 30, OE_MSG_PERFORMANCE_MODE_CHANGED = 31 };
void oe_push_message(uint32_t message);                   // ov_system.c
void oe_set_focus(int in_focus);                          // ov_system.c: GetCurrentFocusState; TOTK_ASSUME_FOCUS pins 1
typedef struct { uint64_t alloc, free, realloc, user; } OvNvAllocator;
typedef struct { OvNvAllocator graphics, devtools; uint64_t memory, memory_size; int initialized; } OvNvState;
const OvNvState *ov_nv_state(void);                        // ov_gfx.c

// ---- GPU address window for CPU-invisible pools (section 3.1)
#define RT_GPU_VA_BASE 0x8000000000ull                     // 2^39, never mapped on the host
#define RT_GPU_VA_SIZE (64ull << 30)

// ---- window, vsync, focus (gfx_present.c and the window back end, gfx_wsys.h; vk_device.c). Host threads only; never run guest code.
void gfx_window_start(void);                               // window thread, Vulkan device, swapchain; idempotent
void gfx_vsync_bind(uint64_t system_event);                // nn::vi vsync event; signalled at 60 Hz
uint64_t gfx_vsync_ticks(void);                            // ticks since start
uint64_t gfx_vsync_time_ns(uint64_t tick);                 // kern_now_ns() time of a tick (pacing)
double gfx_window_refresh_hz(void);                        // refresh rate of the window's monitor; 0 unknown
int gfx_window_focused(void);

// ---- shader service client (gfx_shader.c)
enum { GFX_STAGE_VS, GFX_STAGE_TCS, GFX_STAGE_TES, GFX_STAGE_GS, GFX_STAGE_FS, GFX_STAGE_CS };
enum { GFX_TEX_1D = 1, GFX_TEX_1D_ARRAY, GFX_TEX_BUFFER, GFX_TEX_2D, GFX_TEX_2D_ARRAY, GFX_TEX_2D_MS,
       GFX_TEX_2D_MS_ARRAY, GFX_TEX_3D, GFX_TEX_CUBE, GFX_TEX_CUBE_ARRAY };   // gfx_shader.c tex_names order
enum { GFX_TYPE_FLOAT, GFX_TYPE_SINT, GFX_TYPE_UINT };
typedef struct {                                           // the translator's answers (Program.cs Accessor)
    uint8_t tex_type[32], img_type[32], img_format[32];    // per c2 slot; 0 = not asked; types are GFX_TEX_*,
                                                           // img_format is Ryujinx TextureFormat
    uint8_t attr_type[32], frag_out_type[8];               // GFX_TYPE_*
    uint16_t local[3]; uint32_t shared_bytes;
    uint8_t depth_minus_w;
} GfxShaderAnswers;
typedef struct GfxShader GfxShader;                        // SPIR-V words + parsed reflection; owned by the cache
const GfxShader *gfx_shader_get(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                const uint8_t *c1, const GfxShaderAnswers *a);   // blocks; NULL on failure (counted)
// As gfx_shader_get with c1 inside the code, but a shader in neither cache is queued to a background translation and
// NULL returned with *pending = 1 until it is ready. A failed translation stays failed (*pending = 0).
const GfxShader *gfx_shader_get_async(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                      const GfxShaderAnswers *a, int *pending);
// As gfx_shader_get_async, but a shader queued for translation is moved to the front of the queue and waited for.
// NULL when the translation failed.
const GfxShader *gfx_shader_await(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                  const GfxShaderAnswers *a);
uint64_t gfx_shader_async_done(void);                      // translations finished by the background worker
void gfx_shader_counts(uint64_t *keys, uint64_t *translated);   // distinct keys in memory, translations done (TOTK_TRACE=perf)
// Translator answers for this device (vk_caps.c): shaderFloat64, storage offset rounding, subgroup size per vs/fs/cs.
void gfx_shader_set_host(int float64, uint32_t sbuf_align, const uint32_t subgroup[3], uint32_t gather_bits);
const uint8_t *gfx_shader_key(const GfxShader *s);          // the 32-byte cache key (stable while the translator is)
const uint8_t *gfx_shader_input_id(const GfxShader *s);     // the 32-byte input id (stable across translators)
// By input id from memory or disk; xlate 1 also translates it from its stored inputs. NULL if absent.
const GfxShader *gfx_shader_load_input(const uint8_t iid[32], int xlate);
// Boot prewarm: out[i] = gfx_shader_load_input(iids[i], 1) on `threads` threads with up to `procs` translators, then
// every other stored input not cached under this translator is translated. Logs progress and the total.
void gfx_shader_prewarm(const uint8_t (*iids)[32], size_t n, const GfxShader **out, int threads, int procs);
// Answers met in play: from this call on, every (program, answers) a gfx_shader_get* call resolves is appended
// to shader-answers.bin, and its program to shader-programs.bin, in the directory of `keys_path` (pipeline-keys.bin).
void gfx_shader_record_at(const char *keys_path);
void gfx_shader_record_flush(int force);                   // appends pending records; without force at most every 2 s
// Translates every recorded (program, answers) on `threads` threads with up to `procs` translators (TOTK_TEST_PREPARE_SHADERS).
void gfx_shader_prepare_recorded(int threads, int procs);
// totk.exe --prepare-shaders: translates <dir>/seed-answers.bin + seed-programs.bin (tools/prepare_shaders.py) and
// writes <dir>/shader-prepared.bin (stage, priority, input id per variant). 0 on success.
int gfx_shader_prepare_seed(const char *dir, int threads, int procs);
typedef struct { uint8_t iid[32]; uint8_t stage, prio; } GfxWarmEnt;   // prio 0: met in play; 1..3: prepared
size_t gfx_shader_warm_list(GfxWarmEnt **out);              // malloc'd; recorded first, then prepared; needs recording on
// Disk-cache load the caller owns (*owned 1: release it), or the memory cache's entry (*owned 0). NULL if not cached.
const GfxShader *gfx_shader_load_private(const uint8_t iid[32], int *owned);
void gfx_shader_release_private(const GfxShader *s);
const uint32_t *gfx_shader_spirv(const GfxShader *s, uint64_t *words);
uint64_t gfx_shader_failures(void);                         // failed translations; feeds GfxStats.shader_failures
// Reflection: parsed once per shader from the translator's .res.json and the SPIR-V before it enters the cache.
enum { GFX_RES_UBO, GFX_RES_SSBO, GFX_RES_TEX, GFX_RES_IMG, GFX_RES_KINDS };
typedef struct {
    uint16_t set, binding;      // as the SPIR-V decorates them: set 0, bindings from 1 per shader (0: support buffer)
    int16_t slot;               // UBO: c[slot]; SSBO: storage buffer index; TEX: c2 combined slot (handle at 0x20 + 8 slot)
                                // or, separate, texture slot (0x168 + 8 slot); IMG: image slot (0x120 + 8 slot); -1: other
    uint16_t type;              // TEX/IMG: Ryujinx SamplerType: 1 1D, 2 buffer, 3 2D, 4 3D, 5 cube | 0x100 array,
                                // 0x200 multisample, 0x400 shadow
    uint8_t format;             // IMG: declared format as a GfxShaderAnswers.img_format code, 0 unknown
    uint8_t flags;              // SSBO: 1 written; TEX/IMG: Ryujinx TextureUsageFlags (4 image store, 8 coherent)
    int8_t cbuf_slot;           // TEX/IMG: constant buffer of the handle (-1: bound texture buffer); SSBO: of its address
    uint16_t handle;            // TEX/IMG: raw handle (word offset; separate: texture | sampler << 14); SSBO: address offset
    uint16_t count;             // descriptors (array length)
    uint8_t separate;           // TEX: texture and sampler handles are separate
} GfxShaderRes;
typedef struct { uint8_t type, comps, packed; } GfxShaderIo;   // GFX_TYPE_*, components, 1 with a Component decoration
int gfx_shader_stage(const GfxShader *s);                   // GFX_STAGE_VS, GFX_STAGE_FS or GFX_STAGE_CS
int gfx_shader_resources(const GfxShader *s, int kind, const GfxShaderRes **res);   // count of GFX_RES_* kind; array in *res
uint32_t gfx_shader_slot_mask(const GfxShader *s);                    // texture/image slots whose type its translation read
uint32_t gfx_shader_inputs(const GfxShader *s, GfxShaderIo io[32]);    // Input Locations (VS attributes, FS varyings) as a mask
uint32_t gfx_shader_outputs(const GfxShader *s, GfxShaderIo io[32]);   // Output Locations (VS varyings, FS colour targets)
void gfx_shader_local_size(const GfxShader *s, uint32_t local[3]);     // CS: the LocalSize execution mode; 1, 1, 1 otherwise

// ---- back end (gfx_null.c, vk_*.c). Called by the NVN front end (nvn_api_*.c, nvn_ext.c) on guest threads.
typedef struct {
    const char *name;
    int  (*pool_init)(pool_t *p);                          // 0 fails MemoryPoolInitialize
    void (*pool_fini)(pool_t *p);
    int  (*pool_map_virtual)(pool_t *v, int n, const void *requests);   // NVNmappingRequest[n], 0x28 bytes each
    void (*texture_register)(int id, const tex_t *t, const view_t *v);
    void (*sampler_register)(int id, const samp_par *s);
    void (*texture_release)(const tex_t *t);
    void (*program_release)(const prog_t *p);
    uint64_t (*submit)(const uint64_t *handles, int n);    // replays; returns this submission's fence value
    int  (*fence_wait)(uint64_t value, uint64_t timeout_ns);   // 1 reached; caller has given its core up
    uint64_t (*fence_done)(void);                          // highest value whose side effects are written
    // Queues the present and returns at once; the host present goes out at at_ns (kern_now_ns clock), at once when that
    // has passed or at_ns is 0.
    void (*present)(const tex_t *t, const int crop[4], uint64_t at_ns);
    void (*finish)(void);
    void (*read_texels)(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row, uint64_t img);
    void (*write_texels)(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row, uint64_t img);
    void (*guest_written)(uint64_t addr, uint64_t size);   // host wrote pool storage (3.2)
    void (*guest_read)(uint64_t addr, uint64_t size);      // the CPU reads pool storage: GPU-written texels go there first
    // The CPU waited for fence: small render targets written by work up to it go to their storage (vk_texture.c
    // vk_texture_write_back_small).
    void (*cpu_synced)(uint64_t fence);
} GfxBackend;
extern const GfxBackend gfx_null_backend, gfx_vulkan_backend;
const GfxBackend *gfx_backend(void);                       // TOTK_GPU=null|vulkan (default vulkan)

// ---- NVN front end (nvn_procs.c)
void nvn_procs_register(void);                             // hostfn_register every proc; before hostfn_seal (ov_gfx.c)
uint64_t nvn_proc_address(const char *name);               // synthetic host address of a proc, 0 for an unknown name

// ---- statistics and frames (nvn_api_*.c, vk_*.c; boot.c prints them)
typedef struct { uint64_t frames, nvn_calls, nvn_distinct, unimplemented, validation_errors, shader_failures,
                 draws_skipped; } GfxStats;
GfxStats gfx_stats(void);
void rt_frame_presented(const tex_t *t, uint64_t n);       // boot.c: --frames N ends the run after the Nth present
                                                           // (saves --shot first through read_texels)
