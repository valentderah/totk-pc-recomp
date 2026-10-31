// Graphics and compute pipelines and descriptor layouts of the Vulkan replay. The op-stream replay (vk_exec.c)
// collects NVN state into a VkPipeState and asks for the pipeline of each draw; this file maps that state to Vulkan,
// numbers the descriptor bindings and keeps the caches.
//
// Binding model. The translator (tools/xlate/Program.cs, Ryujinx 1.3.2's SPIR-V back end) decorates every resource of a
// shader with DescriptorSet 0 and numbers the bindings of each shader from 1 in the order it meets them (Accessor
// Create*Binding); binding 0 is Ryujinx's support buffer (ResourceManager: SupportBuffer.Binding, a std140 uniform
// buffer every shader declares). The numbering is per shader, so the vertex and fragment stages of one program collide.
// The scheme here:
//   - one descriptor set, set 0, per pipeline; no push constants (Ryujinx's SPIR-V uses none);
//   - binding 0: the support buffer, one uniform buffer shared by all stages of the pipeline;
//   - vertex stage: its own bindings 1..n unchanged; fragment stage: its bindings b >= 1 move to b + n (the SPIR-V
//     Binding decorations are rewritten when the module is created); compute: its own bindings;
//   - descriptor types from the reflection: constant buffers UNIFORM_BUFFER, storage buffers STORAGE_BUFFER, textures
//     COMBINED_IMAGE_SAMPLER (UNIFORM_TEXEL_BUFFER for buffer textures, SAMPLED_IMAGE for separate ones), images
//     STORAGE_IMAGE (STORAGE_TEXEL_BUFFER for buffer images), array lengths as descriptor counts;
//   - set 0 is a push-descriptor set (VK_KHR_push_descriptor) when its descriptors fit maxPushDescriptors (32 on the RX
//     9070; a pipeline layout may hold one push set only, which rules out a set per stage); larger ones (a vertex and a
//     fragment stage near the census maxima of 15 and 27) are ordinary sets the replay allocates (VkPipeInfo.push 0).
// A fixed layout per stage combination (Ryujinx's own back end: a binding per constant buffer, storage buffer, texture
// and image slot per stage) would hold about 150 descriptors and could never be pushed, so layouts are made per distinct
// binding list and shared by every pipeline with that list; VkPipeInfo.bindings tells the replay which NVN slot feeds
// each binding.
//
// Fragment inputs the vertex stage does not write (stages are translated separately; MNG nvn_mtl.m msl_varyings) get
// outputs added to the vertex module, as Vulkan rejects such pairs, initialised to (0, 0, 0, 1) as Maxwell reads an
// unwritten output (docs/maxwell.md 1.2). The unwritten components of an output the vertex stage does write
// start at (0, 0, 0, 1) in the translator (tools/xlate/patches/0006).
//
// Pipeline key. Everything Vulkan 1.3 can take as dynamic state is dynamic: viewports and scissors with count, depth
// bias, blend constants, stencil masks and references, cull mode, front face, topology (within its class), depth and
// stencil tests and ops, depth bias enable, rasterizer discard; vertex input (VK_EXT_vertex_input_dynamic_state, else
// binding strides only); and, where VK_EXT_extended_dynamic_state3 offers them, depth clamp, polygon mode, blend enables,
// equations, write masks and alpha to coverage. The key holds the rest: shaders, target formats, sample count, topology
// class, depth clip mode, logic op and what is not dynamic on the GPU. The VkPipelineCache persists to
// pipeline-cache.bin next to totk.exe (TOTK_PIPELINE_CACHE overrides the path).
//
// Provenance: the NVN -> Vulkan enum mappings (blend factors and equations, compare functions, stencil ops), the vertex
// format table with its 3-component fallback and the pipeline cache file handling follow LAS Recomp
// runtime/nvn/vulkan/exec.c and device.c @ da35b83a (MIT, Copyright (c) 2026 Peiikai); the pipeline state the key holds
// (program, target formats, blend and write masks, vertex layout, sample count) and the varying fix-up follow
// MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m pso_key_fill / pso_desc / msl_varyings and its vfmt
// table @ b75be8cf (MIT, Copyright (c) 2026 jhaizhou); the binding conventions are those of Ryujinx's shader translator
// (MIT, tools/vendor/ryujinx).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_hmap.h"
#include "platform/platform.h"
#include "tex.h"

#define LOG_ONCE(...) do { static volatile int32_t once_; if (!plat_xchg32(&once_, 1)) rt_log(__VA_ARGS__); } while (0)

enum { SPV_OP_ENTRY_POINT = 15, SPV_OP_TYPE_INT = 21, SPV_OP_TYPE_FLOAT = 22, SPV_OP_TYPE_VECTOR = 23,
       SPV_OP_TYPE_POINTER = 32, SPV_OP_CONSTANT = 43, SPV_OP_CONSTANT_COMPOSITE = 44,
       SPV_OP_CONSTANT_NULL = 46, SPV_OP_FUNCTION = 54, SPV_OP_VARIABLE = 59,
       SPV_OP_DECORATE = 71, SPV_DEC_BUILTIN = 11, SPV_DEC_LOCATION = 30, SPV_DEC_INDEX = 32, SPV_DEC_BINDING = 33, SPV_DEC_DESCRIPTOR_SET = 34,
       SPV_SC_OUTPUT = 3 };

#define MAX_BINDINGS 128
#define PIPE_BUCKETS 4096
#define LAYOUT_BUCKETS 256

// ---- device capabilities and the pipeline cache (set up on first use, under P.lock)

static struct {
    PlatMutex lock;
    volatile int32_t ready;
    VkDevice device;
    VkPipelineCache cache;
    uint32_t max_push;
    int indep;                         // stage-independent layouts (vk_caps.gpl): vertex set 0, fragment set 1
    int push_stage;                    // GFX_STAGE_* whose set is pushed with independent layouts
    int gpl;                           // draws whose pipeline is building get a fast-linked one (vk_caps.gpl)
    struct { uint64_t links, wlinks, dlinks, requeued, link_fail, defer, lib_n[4], lib_fail, swaps, slow, pre_used[4]; double link_ms, link_max, lib_ms[4], lib_max[4], dlink_ms, dlink_max; } g;
    int vs_outs_all;                   // TOTK_VS_OUTS: 0 union of paired fragment inputs, 1 (all) all 32 output locations
    int dyn_vi, dyn_clamp, dyn_poly, dyn_blend_en, dyn_blend_eq, dyn_wmask, dyn_a2c;
    int f_logic_op, f_depth_clamp, f_fill_non_solid, f_dual_src;
    int pv_last;                       // provokingVertexLast enabled: NVN's default is the last vertex
    VkShaderStageFlags sg32_stages;    // stages that accept requiredSubgroupSize = 32 (0: not available)
    VkDynamicState dyn[32];
    uint32_t n_dyn;
    PFN_vkCmdPushDescriptorSetKHR push;
    int tpl;                           // descriptor update templates on (make_tpl)
    PFN_vkCreateDescriptorUpdateTemplate tpl_create;
    PFN_vkDestroyDescriptorUpdateTemplate tpl_destroy;
    PFN_vkUpdateDescriptorSetWithTemplate tpl_update;
    PFN_vkCmdPushDescriptorSetWithTemplateKHR tpl_push;
    PFN_vkCmdSetVertexInputEXT set_vi;
    PFN_vkCmdSetDepthClampEnableEXT set_clamp;
    PFN_vkCmdSetPolygonModeEXT set_poly;
    PFN_vkCmdSetColorBlendEnableEXT set_blend_en;
    PFN_vkCmdSetColorBlendEquationEXT set_blend_eq;
    PFN_vkCmdSetColorWriteMaskEXT set_wmask;
    PFN_vkCmdSetAlphaToCoverageEnableEXT set_a2c;
    char cache_path[PLAT_PATH_MAX * 2];
    VkPipeStats stats;
} P = {.lock = PLAT_MUTEX_INIT};

typedef struct Layout {
    struct Layout *next;
    uint64_t hash;
    VkDescriptorSetLayout set;
    VkPipelineLayout layout;
    int push;
    uint32_t n;
    VkDescriptorSetLayout set1;        // independent layouts: the fragment set (set 1); VK_NULL_HANDLE otherwise
    int push1;                         // set 1 is a push-descriptor set
    uint32_t n0;                       // independent layouts: b[0..n0) are set 0, the rest set 1
    VkPipelineLayout half[2];          // GPL libraries: {set 0, null}, {null, set 1} (made on first use)
    VkDescriptorUpdateTemplate tpl[2]; // per set (make_tpl); VK_NULL_HANDLE: written with write structs
    VkDescriptorSetLayoutBinding b[];
} Layout;

typedef struct Pipe {
    struct Pipe *next;
    uint64_t hash;
    VkPipeInfo info;
    VkPipeBinding *bindings;
    int building;                      // a background job owns info.pipeline (under P.lock)
    struct Job *job;                   // that job while building
    const struct Layout *lay;          // graphics: the layout it was entered with
    VkPipeline fast;                   // GPL: the fast-linked pipeline (kept until shutdown: recorded draws use it)
    uint8_t fast_tried;
    size_t key_size;
    uint8_t key[];
} Pipe;

static Layout *layouts[LAYOUT_BUCKETS];
static Pipe *pipes[PIPE_BUCKETS];

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}
#define FNV0 0xcbf29ce484222325ull

// Hash of a pipeline table key, 8 bytes per step: a draw hashes its whole GKey, and byte-wise FNV over it cost more
// than the lookup. Only the in-memory table uses it; the key file keeps fnv.
static uint64_t key_hash(const void *k, size_t n) { return nvn_hm_hash(k, (uint32_t)n); }

// The cache file lives next to totk.exe, so every build dir (the main build and each private fork) has its own:
// forks that shared build\v121's cache broke each other's runs (2026-10-22).
static void cache_path_init(void) {
    const char *cfg_path = cfg_get("TOTK_PIPELINE_CACHE");
    if (cfg_path && *cfg_path && strlen(cfg_path) < sizeof P.cache_path) {
        strcpy(P.cache_path, cfg_path);
        return;
    }
    P.cache_path[0] = 0;
    char exe[PLAT_PATH_MAX * 2];
    int n = plat_exe_path(exe, sizeof exe);
    if (!n || n >= sizeof exe) return;
    char *cut = plat_last_sep(exe);
    if (!cut) return;
    *cut = 0;
    snprintf(P.cache_path, sizeof P.cache_path, "%s/pipeline-cache.bin", exe);
}

// Cache data whose header names another device or driver is dropped before Vulkan sees it.
static void cache_load(VkCtx *c) {
    cache_path_init();
    void *data = NULL;
    size_t size = 0;
    FILE *f = P.cache_path[0] ? fopen(P.cache_path, "rb") : NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len > (long)sizeof(VkPipelineCacheHeaderVersionOne) && (data = malloc((size_t)len)) &&
            fread(data, 1, (size_t)len, f) == (size_t)len)
            size = (size_t)len;
        fclose(f);
        VkPipelineCacheHeaderVersionOne h;
        if (size) memcpy(&h, data, sizeof h);
        if (size && (h.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || h.vendorID != c->props.vendorID ||
                     h.deviceID != c->props.deviceID || memcmp(h.pipelineCacheUUID, c->props.pipelineCacheUUID, VK_UUID_SIZE))) {
            rt_log("vk_pipeline: %s was written by another device or driver; starting empty", P.cache_path);
            size = 0;
        }
    }
    VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = size,
                                    .pInitialData = size ? data : NULL};
    if (vkCreatePipelineCache(c->device, &ci, NULL, &P.cache) != VK_SUCCESS) {
        ci.initialDataSize = 0, ci.pInitialData = NULL;
        if (vkCreatePipelineCache(c->device, &ci, NULL, &P.cache) != VK_SUCCESS) P.cache = VK_NULL_HANDLE;
    }
    free(data);
}

static void keys_open(void);
static void prebuild_start(void);

static int ensure_init(void) {
    if (P.ready) return 1;
    if (!vk_init()) return 0;
    plat_mutex_lock(&P.lock);
    if (P.ready) {
        plat_mutex_unlock(&P.lock);
        return 1;
    }
    VkCtx *c = vk_ctx();
    P.device = c->device;
    VkPhysicalDevicePushDescriptorPropertiesKHR pd = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pd};
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    P.max_push = pd.maxPushDescriptors;
    P.indep = vk_caps.gpl;
    P.gpl = vk_caps.gpl;
    rt_log(P.gpl ? "vk_pipeline: graphics pipeline library on: a draw whose pipeline is building gets a fast-linked one, "
                   "the optimised build replaces it when done"
                 : "vk_pipeline: graphics pipeline library off: draws wait for or skip pipelines that are building");
    const char *ps = cfg_get("TOTK_PUSH_STAGE"), *vo = cfg_get("TOTK_VS_OUTS");
    P.push_stage = ps && !strcmp(ps, "fs") ? GFX_STAGE_FS : GFX_STAGE_VS;
    P.vs_outs_all = vo && !strcmp(vo, "all");
    if (P.indep) rt_log("vk_pipeline: stage-independent layouts: vertex set 0, fragment set 1, %s set pushed; vertex outputs: %s",
                        P.push_stage == GFX_STAGE_VS ? "vertex" : "fragment", P.vs_outs_all ? "all 32 locations" : "union of paired fragment inputs");
    // vk_device.c enables every feature the GPU reports in this chain, so the query tells what is enabled.
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT e3 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceProvokingVertexFeaturesEXT pvf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT,
                                                      .pNext = c->has_eds3 ? &e3 : NULL};
    VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .pNext = c->has_provoking_vertex ? (void *)&pvf : (c->has_eds3 ? (void *)&e3 : NULL)};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v13};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    P.pv_last = c->has_provoking_vertex && pvf.provokingVertexLast;
    if (!P.pv_last) rt_log("vk_pipeline: provokingVertexLast unavailable: flat outputs come from the first vertex (NVN: last)");
    // Maxwell warps have 32 lanes; the translator assumes it (maxwell.md 3.4). Pin 32 where the driver allows it.
    if (v13.subgroupSizeControl) {
        VkPhysicalDeviceVulkan13Properties p13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
        VkPhysicalDeviceProperties2 q2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p13};
        vkGetPhysicalDeviceProperties2(c->phys, &q2);
        P.sg32_stages = vk_caps.sg_pin_stages;   // vk_caps.c: 32 unless TOTK_VK_SUBGROUP; 0 without the pin
        rt_log("vk_pipeline: subgroup size %u-%u, requiredSubgroupSizeStages %#x, pinning %u on %#x", p13.minSubgroupSize,
               p13.maxSubgroupSize, (unsigned)p13.requiredSubgroupSizeStages, vk_caps.sg_size, (unsigned)P.sg32_stages);
    }
    P.f_logic_op = f2.features.logicOp;
    P.f_depth_clamp = f2.features.depthClamp;
    P.f_fill_non_solid = f2.features.fillModeNonSolid;
    P.f_dual_src = f2.features.dualSrcBlend;
#define PROC(field, name) (P.field = (void *)vkGetDeviceProcAddr(c->device, name))
    PROC(push, "vkCmdPushDescriptorSetKHR");
    // Descriptor update templates (core 1.1; the push variant comes with VK_KHR_push_descriptor on 1.1): the driver
    // walks a layout prepared when the layout is made instead of parsing write structs every draw (DXVK does so for the
    // same reason). TOTK_DESC_TEMPLATE=0 keeps the write structs.
    if (!cfg_bool("TOTK_DESC_TEMPLATE")) {
        rt_log("vk_pipeline: descriptor update templates off (TOTK_DESC_TEMPLATE=0)");
    } else if (c->props.apiVersion >= VK_API_VERSION_1_1 && PROC(tpl_create, "vkCreateDescriptorUpdateTemplate") &&
               PROC(tpl_destroy, "vkDestroyDescriptorUpdateTemplate") && PROC(tpl_update, "vkUpdateDescriptorSetWithTemplate")) {
        P.tpl = 1;
        if (!PROC(tpl_push, "vkCmdPushDescriptorSetWithTemplateKHR"))
            rt_log("vk_pipeline: vkCmdPushDescriptorSetWithTemplateKHR missing: push sets are written with write structs");
    } else {
        rt_log("vk_pipeline: descriptor update templates unavailable: sets are written with write structs");
    }
    // TOTK_VK_DYN_VERTEX=0/1 forces dynamic vertex input off or on. Default off on Intel's frozen Windows driver for its
    // older integrated GPUs (UHD 630, Ice Lake: Vulkan 1.3.215 and older), which draws nothing with it; LAS Recomp
    // turns it off there for the same reason.
    const char *dv = cfg_get("TOTK_VK_DYN_VERTEX");   // no fixed default: it depends on the GPU, see above
    int intel_legacy = c->props.vendorID == 0x8086 && c->props.apiVersion <= VK_MAKE_API_VERSION(0, 1, 3, 215);
    int dv_on = dv && *dv ? strcmp(dv, "0") != 0 : !intel_legacy;
    P.dyn_vi = dv_on && c->has_vertex_input_dynamic_state && PROC(set_vi, "vkCmdSetVertexInputEXT");
    if (!dv_on && c->has_vertex_input_dynamic_state)
        rt_log("vk_pipeline: dynamic vertex input off (%s)", dv && *dv ? "TOTK_VK_DYN_VERTEX=0" : "Intel's older Windows driver draws nothing with it");
    if (c->has_eds3) {
        P.dyn_clamp = e3.extendedDynamicState3DepthClampEnable && P.f_depth_clamp && PROC(set_clamp, "vkCmdSetDepthClampEnableEXT");
        P.dyn_poly = e3.extendedDynamicState3PolygonMode && PROC(set_poly, "vkCmdSetPolygonModeEXT");
        P.dyn_blend_en = e3.extendedDynamicState3ColorBlendEnable && PROC(set_blend_en, "vkCmdSetColorBlendEnableEXT");
        P.dyn_blend_eq = e3.extendedDynamicState3ColorBlendEquation && PROC(set_blend_eq, "vkCmdSetColorBlendEquationEXT");
        P.dyn_wmask = e3.extendedDynamicState3ColorWriteMask && PROC(set_wmask, "vkCmdSetColorWriteMaskEXT");
        P.dyn_a2c = e3.extendedDynamicState3AlphaToCoverageEnable && PROC(set_a2c, "vkCmdSetAlphaToCoverageEnableEXT");
    }
    // TOTK_VK_DYN_A2C=0/1 forces dynamic alpha to coverage off or on. Default off on AMD's Windows driver branch for
    // RDNA 2 and older (vk_caps.amd_sc), which applies alpha to coverage while the dynamic state says off: on an RX 590
    // (24.9.1) TotK, which never turns it on, drew Link and the NPCs with depth but no colour (black silhouettes) and
    // dithered other surfaces into checkerboards (issue #67, RX 470/570/580). Taken out of the dynamic state, it goes
    // into the pipeline key and the characters draw.
    const char *da = cfg_get("TOTK_VK_DYN_A2C");   // no fixed default: it depends on the driver, see above
    int da_on = da && *da ? strcmp(da, "0") != 0 : !vk_caps.amd_sc;
    if (!da_on && P.dyn_a2c) {
        P.dyn_a2c = 0;
        rt_log("vk_pipeline: dynamic alpha to coverage off (%s)", da && *da ? "TOTK_VK_DYN_A2C=0" : "AMD's driver for RDNA 2 and older");
    }
#undef PROC
    static const VkDynamicState core[] = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
        VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
    P.n_dyn = 0;
    for (size_t i = 0; i < sizeof core / sizeof *core; i++) P.dyn[P.n_dyn++] = core[i];
    P.dyn[P.n_dyn++] = P.dyn_vi ? VK_DYNAMIC_STATE_VERTEX_INPUT_EXT : VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
    if (P.dyn_clamp) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT;
    if (P.dyn_poly) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_POLYGON_MODE_EXT;
    if (P.dyn_blend_en) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT;
    if (P.dyn_blend_eq) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT;
    if (P.dyn_wmask) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT;
    if (P.dyn_a2c) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT;
    cache_load(c);
    rt_log("vk_pipeline: max push descriptors %u; dynamic vertex input %d, depth clamp %d, polygon mode %d, blend enable %d, "
           "blend equation %d, write mask %d, alpha to coverage %d; cache %s",
           P.max_push, P.dyn_vi, P.dyn_clamp, P.dyn_poly, P.dyn_blend_en, P.dyn_blend_eq, P.dyn_wmask, P.dyn_a2c,
           P.cache_path[0] ? P.cache_path : "<memory only>");
    keys_open();
    plat_xchg32(&P.ready, 1);
    plat_mutex_unlock(&P.lock);
    prebuild_start();
    return 1;
}

// ---- NVN -> Vulkan mappings (LAS Recomp exec.c blend_factor, blend_op, compare_fn, stencil_op)

static VkBlendFactor blend_factor(unsigned f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: LOG_ONCE("vk_pipeline: unknown NVN blend factor %#x, using ONE", f); return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(unsigned e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

// NVN_DEPTH_FUNC_* / NVN_STENCIL_FUNC_* are NEVER = 1 .. ALWAYS = 8 in VkCompareOp order.
static VkCompareOp compare_op(unsigned f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }
// NVN_STENCIL_OP_* are KEEP = 1 .. DECR_WRAP = 8 in VkStencilOp order.
static VkStencilOp stencil_op(unsigned o) { return o >= 1 && o <= 8 ? (VkStencilOp)(o - 1) : VK_STENCIL_OP_KEEP; }
// NVN_POLYGON_MODE_POINT/LINE/FILL = 0/1/2, VkPolygonMode FILL/LINE/POINT = 0/1/2.
static VkPolygonMode polygon_mode(unsigned m) {
    if (!P.f_fill_non_solid || m > 2) return VK_POLYGON_MODE_FILL;
    return (VkPolygonMode)(2 - m);
}

VkPrimitiveTopology vk_pipeline_topology(int prim) {
    switch (prim) {
    case NVN_DRAW_PRIMITIVE_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case NVN_DRAW_PRIMITIVE_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case NVN_DRAW_PRIMITIVE_LINE_LOOP:
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP:
    case NVN_DRAW_PRIMITIVE_QUAD_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;   // same triangles, same coverage
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN:
    case NVN_DRAW_PRIMITIVE_POLYGON: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_PATCHES: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // TRIANGLES; QUADS after the replay's index rewrite
    }
}

// Topology class (a pipeline's static topology only fixes the class): 0 point, 1 line, 2 triangle, 3 patch.
static int topology_class(VkPrimitiveTopology t) {
    switch (t) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return 0;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: return 1;
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST: return 3;
    default: return 2;
    }
}

int vk_pipeline_tex_type(int target) {
    switch (target) {
    case NVN_TEXTURE_TARGET_1D: return GFX_TEX_1D;
    case NVN_TEXTURE_TARGET_3D: return GFX_TEX_3D;
    case NVN_TEXTURE_TARGET_1D_ARRAY: return GFX_TEX_1D_ARRAY;
    case NVN_TEXTURE_TARGET_2D_ARRAY: return GFX_TEX_2D_ARRAY;
    case NVN_TEXTURE_TARGET_2D_MULTISAMPLE: return GFX_TEX_2D_MS;
    case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: return GFX_TEX_2D_MS_ARRAY;
    case NVN_TEXTURE_TARGET_CUBEMAP: return GFX_TEX_CUBE;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: return GFX_TEX_CUBE_ARRAY;
    case NVN_TEXTURE_TARGET_BUFFER: return GFX_TEX_BUFFER;
    default: return GFX_TEX_2D;   // 2D, RECTANGLE
    }
}

// Numeric type of a colour format as a shader sees it: GFX_TYPE_*.
static int format_type(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_B8G8R8A8_UINT: case VK_FORMAT_S8_UINT:
        return GFX_TYPE_UINT;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8_SINT: case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16_SINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_A2B10G10R10_SINT_PACK32: case VK_FORMAT_B8G8R8A8_SINT:
        return GFX_TYPE_SINT;
    default: return GFX_TYPE_FLOAT;
    }
}

// Optimal-tiling (o = 1) or buffer (o = 0) features of a core format, cached.
static VkFormatFeatureFlags format_features(VkFormat f, int optimal) {
    static VkFormatFeatureFlags known[2][256];
    static uint8_t have[2][256];
    VkCtx *c = vk_ctx();
    if (!c) return 0;
    if ((unsigned)f < 256 && have[optimal][f]) return known[optimal][f];
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(c->phys, f, &p);
    VkFormatFeatureFlags r = optimal ? p.optimalTilingFeatures : p.bufferFeatures;
    if ((unsigned)f < 256) known[optimal][f] = r, have[optimal][f] = 1;
    return r;
}

// Vertex attribute formats by NVNformat (LAS Recomp exec.c vertex_format; MNG nvn_mtl.m vfmt for the TotK additions).
typedef struct { VkFormat vk; uint8_t type; } VtxFmt;
#define F(v) {VK_FORMAT_##v, GFX_TYPE_FLOAT}
#define U(v) {VK_FORMAT_##v, GFX_TYPE_UINT}
#define I(v) {VK_FORMAT_##v, GFX_TYPE_SINT}
static const VtxFmt vtx_formats[0x9B] = {
    [0x01] = F(R8_UNORM), [0x02] = F(R8_SNORM), [0x03] = U(R8_UINT), [0x04] = I(R8_SINT), [0x05] = F(R16_SFLOAT),
    [0x06] = F(R16_UNORM), [0x07] = F(R16_SNORM), [0x08] = U(R16_UINT), [0x09] = I(R16_SINT), [0x0A] = F(R32_SFLOAT),
    [0x0B] = U(R32_UINT), [0x0C] = I(R32_SINT), [0x0D] = F(R8G8_UNORM), [0x0E] = F(R8G8_SNORM), [0x0F] = U(R8G8_UINT),
    [0x10] = I(R8G8_SINT), [0x11] = F(R16G16_SFLOAT), [0x12] = F(R16G16_UNORM), [0x13] = F(R16G16_SNORM),
    [0x14] = U(R16G16_UINT), [0x15] = I(R16G16_SINT), [0x16] = F(R32G32_SFLOAT), [0x17] = U(R32G32_UINT),
    [0x18] = I(R32G32_SINT), [0x19] = F(R8G8B8_UNORM), [0x1A] = F(R8G8B8_SNORM), [0x1B] = U(R8G8B8_UINT),
    [0x1C] = I(R8G8B8_SINT), [0x1D] = F(R16G16B16_SFLOAT), [0x1E] = F(R16G16B16_UNORM), [0x1F] = F(R16G16B16_SNORM),
    [0x20] = U(R16G16B16_UINT), [0x21] = I(R16G16B16_SINT), [0x22] = F(R32G32B32_SFLOAT), [0x23] = U(R32G32B32_UINT),
    [0x24] = I(R32G32B32_SINT), [0x25] = F(R8G8B8A8_UNORM), [0x26] = F(R8G8B8A8_SNORM), [0x27] = U(R8G8B8A8_UINT),
    [0x28] = I(R8G8B8A8_SINT), [0x29] = F(R16G16B16A16_SFLOAT), [0x2A] = F(R16G16B16A16_UNORM),
    [0x2B] = F(R16G16B16A16_SNORM), [0x2C] = U(R16G16B16A16_UINT), [0x2D] = I(R16G16B16A16_SINT),
    [0x2E] = F(R32G32B32A32_SFLOAT), [0x2F] = U(R32G32B32A32_UINT), [0x30] = I(R32G32B32A32_SINT),
    [0x3D] = F(A2B10G10R10_UNORM_PACK32), [0x3E] = U(A2B10G10R10_UINT_PACK32), [0x3F] = F(B10G11R11_UFLOAT_PACK32),
    // *_UI2F / *_I2F: integer data read as float; the 8- and 16-bit ones are Vulkan's SCALED formats
    [0x51] = F(R8_USCALED), [0x52] = F(R8_SSCALED), [0x53] = F(R16_USCALED), [0x54] = F(R16_SSCALED),
    [0x57] = F(R8G8_USCALED), [0x58] = F(R8G8_SSCALED), [0x59] = F(R16G16_USCALED), [0x5A] = F(R16G16_SSCALED),
    [0x5D] = F(R8G8B8_USCALED), [0x5E] = F(R8G8B8_SSCALED), [0x5F] = F(R16G16B16_USCALED), [0x60] = F(R16G16B16_SSCALED),
    [0x63] = F(R8G8B8A8_USCALED), [0x64] = F(R8G8B8A8_SSCALED), [0x65] = F(R16G16B16A16_USCALED),
    [0x66] = F(R16G16B16A16_SSCALED),
    // 32-bit ones have no SCALED format: read as integers (values are not converted; logged when used)
    [0x55] = U(R32_UINT), [0x56] = I(R32_SINT), [0x5B] = U(R32G32_UINT), [0x5C] = I(R32G32_SINT),
    [0x61] = U(R32G32B32_UINT), [0x62] = I(R32G32B32_SINT), [0x67] = U(R32G32B32A32_UINT), [0x68] = I(R32G32B32A32_SINT),
    [0x69] = F(A2B10G10R10_SNORM_PACK32), [0x6A] = I(A2B10G10R10_SINT_PACK32), [0x6B] = F(A2B10G10R10_USCALED_PACK32),
    [0x6C] = F(A2B10G10R10_SSCALED_PACK32), [0x9A] = F(B8G8R8A8_UNORM),
};
#undef F
#undef U
#undef I

// 3-component 8/16-bit formats are not vertex formats on every GPU: read 4 components (LAS vertex_format_supported).
static VkFormat widen(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R8G8B8_USCALED: return VK_FORMAT_R8G8B8A8_USCALED;
    case VK_FORMAT_R8G8B8_SSCALED: return VK_FORMAT_R8G8B8A8_SSCALED;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    case VK_FORMAT_R16G16B16_USCALED: return VK_FORMAT_R16G16B16A16_USCALED;
    case VK_FORMAT_R16G16B16_SSCALED: return VK_FORMAT_R16G16B16A16_SSCALED;
    default: return VK_FORMAT_UNDEFINED;
    }
}

VkFormat vk_vertex_format(int nvn_format, int *type) {
    if (type) *type = GFX_TYPE_FLOAT;
    if (nvn_format <= 0 || nvn_format >= (int)(sizeof vtx_formats / sizeof *vtx_formats)) return VK_FORMAT_UNDEFINED;
    const VtxFmt *v = &vtx_formats[nvn_format];
    if (v->vk == VK_FORMAT_UNDEFINED) return VK_FORMAT_UNDEFINED;
    if (type) *type = v->type;
    if ((nvn_format >= 0x55 && nvn_format <= 0x68) && v->type != GFX_TYPE_FLOAT)
        LOG_ONCE("vk_pipeline: 32-bit *_UI2F/*_I2F vertex formats are read as integers (first: %#x)", nvn_format);
    VkFormat f = v->vk;
    if (vk_ctx() && !(format_features(f, 0) & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
        VkFormat w = widen(f);
        if (w == VK_FORMAT_UNDEFINED || !(format_features(w, 0) & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
            LOG_ONCE("vk_pipeline: vertex format %d (NVN %#x) unsupported", (int)f, nvn_format);
            return VK_FORMAT_UNDEFINED;
        }
        LOG_ONCE("vk_pipeline: vertex format %d unsupported, read as 4 components (first: NVN %#x)", (int)f, nvn_format);
        f = w;
    }
    return f;
}

// Attachment format of an NVN format (tex_vk_format: the format the images are created with).
static VkFormat target_format(int nvn_format, int *depth, int *stencil) {
    TexVkFormat t;
    if (depth) *depth = 0;
    if (stencil) *stencil = 0;
    if (!nvn_format || !tex_vk_format(nvn_format, &t)) return VK_FORMAT_UNDEFINED;
    if (depth) *depth = t.depth;
    if (stencil) *stencil = t.stencil;
    return t.vk;
}

void vk_pipe_state_defaults(VkPipeState *s) {
    memset(s, 0, sizeof *s);
    s->samples = 1;
    s->prim = NVN_DRAW_PRIMITIVE_TRIANGLES;
    for (int i = 0; i < 8; i++)
        s->blend[i] = (blend_st){(uint64_t)i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE,
                                 NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    s->color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS};
    s->chmask.mask = 0xffffffffu;
    s->ms.enable = 1;
    s->poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    s->ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS,
                    NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP,
                    NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP};
}

void vk_pipeline_answers(const VkPipeState *s, GfxShaderAnswers *vs, GfxShaderAnswers *fs) {
    if (vs)
        for (int i = 0; i < 16; i++) {
            va_st a;
            memcpy(&a, &s->vattr[i], sizeof a);
            int type = GFX_TYPE_FLOAT;
            if (a.fmt) vk_vertex_format((int)a.fmt, &type);
            vs->attr_type[i] = (uint8_t)type;
        }
    if (fs)
        for (int i = 0; i < 8; i++) {
            VkFormat f = target_format(s->color_fmt[i], NULL, NULL);
            fs->frag_out_type[i] = (uint8_t)(f == VK_FORMAT_UNDEFINED ? GFX_TYPE_FLOAT : format_type(f));
        }
}

// ---- vertex input (pipeline state, or vkCmdSetVertexInputEXT)

typedef struct {
    uint32_t n_attr, n_bind;
    VkVertexInputAttributeDescription2EXT a[32];
    VkVertexInputBindingDescription2EXT b[17];
} VtxInput;

// An attribute the game left without a format is a fixed one on Maxwell, a single 32-bit component of 0 (deko3d
// gpu_3d_vbo.cpp binds unused attributes as IsFixed 1x32 float): it reads (0, 0, 0, 1), as Vulkan expands a
// one-component format. TOTK's foliage shaders scale wind sway by 1 - colour.w; shapes without a vertex colour
// (Brightbloom stem and leaves) must not sway, or they bend away from the parts that have one (the bulb).
static VkFormat zero_format(int type) {
    return type == GFX_TYPE_UINT ? VK_FORMAT_R32_UINT : type == GFX_TYPE_SINT ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_SFLOAT;
}

// Attributes for the Locations the vertex shader reads; one without a usable NVN attribute reads VK_PIPE_ZERO_BINDING.
static void vertex_input(const VkPipeState *s, VtxInput *v) {
    GfxShaderIo io[32];
    uint32_t used = gfx_shader_inputs(s->vs, io), streams = 0;
    int zero = 0;
    memset(v, 0, sizeof *v);
    for (int l = 0; l < 32; l++) {
        if (!(used >> l & 1)) continue;
        VkVertexInputAttributeDescription2EXT *a = &v->a[v->n_attr++];
        *a = (VkVertexInputAttributeDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
                                                     .location = (uint32_t)l};
        va_st va = {0};
        if (l < 16) memcpy(&va, &s->vattr[l], sizeof va);
        VkFormat f = va.fmt ? vk_vertex_format((int)va.fmt, NULL) : VK_FORMAT_UNDEFINED;
        if (f != VK_FORMAT_UNDEFINED && va.stream < 16) {
            a->binding = va.stream, a->format = f, a->offset = va.offset;
            streams |= 1u << va.stream;
        } else {
            a->binding = VK_PIPE_ZERO_BINDING, a->format = zero_format(io[l].type);
            zero = 1;
        }
    }
    for (uint32_t b = 0; b < 16; b++) {
        if (!(streams >> b & 1)) continue;
        uint32_t div = s->vstream[b].divisor;
        if (div > 1) LOG_ONCE("vk_pipeline: instance divisor %u treated as 1 (no VK_EXT_vertex_attribute_divisor)", div);
        v->b[v->n_bind++] = (VkVertexInputBindingDescription2EXT){
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, .binding = b, .stride = s->vstream[b].stride,
            .inputRate = div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, .divisor = 1};
    }
    if (zero)
        v->b[v->n_bind++] = (VkVertexInputBindingDescription2EXT){
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, .binding = VK_PIPE_ZERO_BINDING,
            .stride = 0, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX, .divisor = 1};
}

int vk_pipeline_vertex_input_dynamic(void) { return ensure_init() && P.dyn_vi; }

PFN_vkCmdPushDescriptorSetKHR vk_pipeline_push_fn(void) { return ensure_init() ? P.push : NULL; }
PFN_vkCmdPushDescriptorSetWithTemplateKHR vk_pipeline_push_tpl_fn(void) { return ensure_init() && P.tpl ? P.tpl_push : NULL; }
PFN_vkUpdateDescriptorSetWithTemplate vk_pipeline_update_tpl_fn(void) { return ensure_init() && P.tpl ? P.tpl_update : NULL; }

int vk_pipeline_set_indep(int on) {
    if (!ensure_init()) return 0;
    plat_mutex_lock(&P.lock);
    int was = P.indep;
    P.indep = on && vk_caps.gpl;
    plat_mutex_unlock(&P.lock);
    return was;
}

// ---- SPIR-V rewrites

// Binding decorations b >= 1 become b + off (the fragment stage's share of set 0).
static uint32_t *spirv_rebind(const uint32_t *w, uint64_t n, uint32_t off) {
    uint32_t *o = malloc(n * 4);
    if (!o) return NULL;
    memcpy(o, w, n * 4);
    for (uint64_t i = 5; i < n;) {
        uint32_t op = o[i] & 0xffff, len = o[i] >> 16;
        if (!len || i + len > n) break;
        if (op == SPV_OP_DECORATE && len >= 4 && o[i + 2] == SPV_DEC_BINDING && o[i + 3] >= 1) o[i + 3] += off;
        i += len;
    }
    return o;
}

// DescriptorSet decorations become set (the fragment stage's resources move to set 1 with independent layouts).
static uint32_t *spirv_set(const uint32_t *w, uint64_t n, uint32_t set) {
    uint32_t *o = malloc(n * 4);
    if (!o) return NULL;
    memcpy(o, w, n * 4);
    for (uint64_t i = 5; i < n;) {
        uint32_t op = o[i] & 0xffff, len = o[i] >> 16;
        if (!len || i + len > n) break;
        if (op == SPV_OP_DECORATE && len >= 4 && o[i + 2] == SPV_DEC_DESCRIPTOR_SET) o[i + 3] = set;
        i += len;
    }
    return o;
}

// Dual-source blending (docs/maxwell.md 5.6): SRC1 blend factors read the fragment shader's colour output 1,
// which Vulkan takes from Location 0, Index 1. The Output variable at Location 1 moves there, as Ryujinx declares it
// with DualSourceBlendEnable (CodeGen/Spirv/Declarations.cs); the pipeline is keyed by GKey.dsb, so only pipelines
// that blend with SRC1 get this module. NULL when the module has no Output at Location 1.
static uint32_t *spirv_dual_source(const uint32_t *w, uint64_t n, uint64_t *out_n) {
    uint32_t outs[64];
    int nout = 0;
    uint64_t at = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return NULL;
        if (op == SPV_OP_VARIABLE && len >= 4 && w[i + 3] == SPV_SC_OUTPUT && nout < 64) outs[nout++] = w[i + 2];
        i += len;
    }
    for (uint64_t i = 5; i < n && !at;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == SPV_OP_DECORATE && len == 4 && w[i + 2] == SPV_DEC_LOCATION && w[i + 3] == 1)
            for (int k = 0; k < nout; k++)
                if (outs[k] == w[i + 1]) at = i;
        i += len;
    }
    if (!at) return NULL;
    uint32_t *o = malloc((n + 4) * 4);
    if (!o) return NULL;
    memcpy(o, w, (at + 4) * 4);
    o[at + 3] = 0;
    o[at + 4] = 4u << 16 | SPV_OP_DECORATE, o[at + 5] = w[at + 1], o[at + 6] = SPV_DEC_INDEX, o[at + 7] = 1;
    memcpy(o + at + 8, w + at + 4, (n - at - 4) * 4);
    *out_n = n + 4;
    return o;
}

// Adds an Output variable for each Location of `missing` (type and width from io) to a module, declared in the entry
// point interface and initialised as Maxwell reads an unwritten output: (0, 0, 0, 1), the w as the bits of 1.0f whatever
// the type (attribute memory is untyped); fewer than 4 components are all zero. Existing scalar, vector and pointer types
// are reused. NULL when the module does not have the expected layout.
static uint32_t *spirv_add_outputs(const uint32_t *w, uint64_t n, uint32_t missing, const GfxShaderIo io[32],
                                   uint64_t *out_n) {
    uint64_t ep_at = 0, ann_end = 0, fn_at = 0;
    uint32_t bound = w[3], scalar[3] = {0, 0, 0};          // GFX_TYPE_FLOAT, SINT, UINT
    struct { uint32_t id, comp, count; } vec[64];
    struct { uint32_t id, type; } ptr[64];
    int nvec = 0, nptr = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return NULL;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_ENTRY_POINT && !ep_at) ep_at = i;
        else if (op == SPV_OP_DECORATE || op == 72 || op == 73 || op == 74 || op == 75 || op == 332 || op == 5632 || op == 5633)
            ann_end = i + len;
        else if (op == SPV_OP_TYPE_FLOAT && len >= 3 && a[1] == 32) scalar[GFX_TYPE_FLOAT] = a[0];
        else if (op == SPV_OP_TYPE_INT && len >= 4 && a[1] == 32) scalar[a[2] ? GFX_TYPE_SINT : GFX_TYPE_UINT] = a[0];
        else if (op == SPV_OP_TYPE_VECTOR && len >= 4 && nvec < 64) vec[nvec].id = a[0], vec[nvec].comp = a[1], vec[nvec++].count = a[2];
        else if (op == SPV_OP_TYPE_POINTER && len >= 4 && a[1] == SPV_SC_OUTPUT && nptr < 64) ptr[nptr].id = a[0], ptr[nptr++].type = a[2];
        else if (op == SPV_OP_FUNCTION) { fn_at = i; break; }
        i += len;
    }
    if (!ep_at || !ann_end || !fn_at || ann_end < ep_at || fn_at < ann_end) return NULL;
    uint32_t types[32 * 40], decs[32 * 4], vars[32], zero[3] = {0, 0, 0}, one[3] = {0, 0, 0};
    int nt = 0, nd = 0, nv = 0;
    for (int l = 0; l < 32; l++) {
        if (!(missing >> l & 1)) continue;
        int t = io[l].type <= GFX_TYPE_UINT ? io[l].type : GFX_TYPE_FLOAT;
        uint32_t comps = io[l].comps ? io[l].comps : 4;
        if (!scalar[t]) {
            scalar[t] = bound++;
            if (t == GFX_TYPE_FLOAT) types[nt++] = 3u << 16 | SPV_OP_TYPE_FLOAT, types[nt++] = scalar[t], types[nt++] = 32;
            else types[nt++] = 4u << 16 | SPV_OP_TYPE_INT, types[nt++] = scalar[t], types[nt++] = 32, types[nt++] = t == GFX_TYPE_SINT;
        }
        uint32_t type = scalar[t];
        if (comps > 1) {
            int k = 0;
            while (k < nvec && !(vec[k].comp == type && vec[k].count == comps)) k++;
            if (k == nvec) {
                if (nvec == 64) return NULL;
                vec[nvec].id = bound++, vec[nvec].comp = type, vec[nvec++].count = comps;
                types[nt++] = 4u << 16 | SPV_OP_TYPE_VECTOR, types[nt++] = vec[k].id, types[nt++] = type, types[nt++] = comps;
            }
            type = vec[k].id;
        }
        int k = 0;
        while (k < nptr && ptr[k].type != type) k++;
        if (k == nptr) {
            if (nptr == 64) return NULL;
            ptr[nptr].id = bound++, ptr[nptr++].type = type;
            types[nt++] = 4u << 16 | SPV_OP_TYPE_POINTER, types[nt++] = ptr[k].id, types[nt++] = SPV_SC_OUTPUT, types[nt++] = type;
        }
        uint32_t init = bound++, var = bound++;
        if (comps == 4) {
            if (!zero[t]) {
                zero[t] = bound++, one[t] = bound++;
                types[nt++] = 4u << 16 | SPV_OP_CONSTANT, types[nt++] = scalar[t], types[nt++] = zero[t], types[nt++] = 0;
                types[nt++] = 4u << 16 | SPV_OP_CONSTANT, types[nt++] = scalar[t], types[nt++] = one[t], types[nt++] = 0x3f800000;
            }
            types[nt++] = 7u << 16 | SPV_OP_CONSTANT_COMPOSITE, types[nt++] = type, types[nt++] = init;
            types[nt++] = zero[t], types[nt++] = zero[t], types[nt++] = zero[t], types[nt++] = one[t];
        } else {
            types[nt++] = 3u << 16 | SPV_OP_CONSTANT_NULL, types[nt++] = type, types[nt++] = init;
        }
        types[nt++] = 5u << 16 | SPV_OP_VARIABLE, types[nt++] = ptr[k].id, types[nt++] = var, types[nt++] = SPV_SC_OUTPUT,
        types[nt++] = init;
        decs[nd++] = 4u << 16 | SPV_OP_DECORATE, decs[nd++] = var, decs[nd++] = SPV_DEC_LOCATION, decs[nd++] = (uint32_t)l;
        vars[nv++] = var;
    }
    uint64_t total = n + (uint64_t)(nt + nd + nv);
    uint32_t *o = malloc(total * 4), *p = o;
    if (!o) return NULL;
    uint32_t ep_len = w[ep_at] >> 16;
    memcpy(p, w, 5 * 4), p[3] = bound, p += 5;
    memcpy(p, w + 5, (ep_at - 5) * 4), p += ep_at - 5;
    *p++ = (ep_len + (uint32_t)nv) << 16 | SPV_OP_ENTRY_POINT;
    memcpy(p, w + ep_at + 1, (ep_len - 1) * 4), p += ep_len - 1;
    memcpy(p, vars, (size_t)nv * 4), p += nv;
    memcpy(p, w + ep_at + ep_len, (ann_end - ep_at - ep_len) * 4), p += ann_end - ep_at - ep_len;
    memcpy(p, decs, (size_t)nd * 4), p += nd;
    memcpy(p, w + ann_end, (fn_at - ann_end) * 4), p += fn_at - ann_end;
    memcpy(p, types, (size_t)nt * 4), p += nt;
    memcpy(p, w + fn_at, (n - fn_at) * 4), p += n - fn_at;
    *out_n = (uint64_t)(p - o);
    return o;
}

// Two fixes for what the translator declares but a shader does not use (NULL: nothing to change):
//  - the fragment shader interlock capability and extension, which Ryujinx adds to every fragment shader when the host
//    reports interlock support (SpirvGenerator.cs) and vk_device.c does not enable; kept when the shader interlocks;
//  - the CullDistance[1] member of gl_PerVertex next to ClipDistance[8] (Declarations.cs CreatePerVertexStructType):
//    9 distances exceed maxCombinedClipAndCullDistances (8 on the RX 9070). Ryujinx never accesses CullDistance; the
//    member goes when it is the struct's last and no access chain selects it.
static uint32_t *spirv_fixup(const uint32_t *w, uint64_t n, uint64_t *out_n) {
    enum { OP_EXTENSION = 10, OP_CAPABILITY = 17, OP_MEMBER_NAME = 6, OP_EXECUTION_MODE = 16, OP_TYPE_ARRAY = 28,
           OP_TYPE_STRUCT = 30, OP_CONSTANT = 43, OP_MEMBER_DECORATE = 72, OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66,
           OP_BEGIN_INTERLOCK = 5364, OP_END_INTERLOCK = 5365, CAP_SAMPLE_INTERLOCK = 5363, CAP_PIXEL_INTERLOCK = 5378,
           CAP_RATE_INTERLOCK = 5372, BUILTIN_CLIP = 3, BUILTIN_CULL = 4 };
    if (n < 5) return NULL;
    uint32_t bound = w[3] < (1u << 22) ? w[3] : (1u << 22);
    uint32_t *val = calloc(bound, sizeof *val), *elem = calloc(bound, sizeof *elem), *len = calloc(bound, sizeof *len),
             *pointee = calloc(bound, sizeof *pointee), *var_type = calloc(bound, sizeof *var_type);
    uint32_t *o = NULL;
    int interlock_used = 0, interlock_declared = 0, cull_ok = 0;
    uint32_t cull_struct = 0, cull_member = 0, clip_member = ~0u, members = 0, member_types[8] = {0};
    if (!val || !elem || !len || !pointee || !var_type) goto out;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (!l || i + l > n) goto out;
        const uint32_t *a = w + i + 1;
        if (op == OP_BEGIN_INTERLOCK || op == OP_END_INTERLOCK) interlock_used = 1;
        else if (op == OP_EXECUTION_MODE && l >= 3 && a[1] >= 5366 && a[1] <= 5371) interlock_used = 1;
        else if (op == OP_CAPABILITY && l >= 2 && (a[0] == CAP_PIXEL_INTERLOCK || a[0] == CAP_SAMPLE_INTERLOCK || a[0] == CAP_RATE_INTERLOCK))
            interlock_declared = 1;
        else if (op == OP_CONSTANT && l == 4 && a[1] < bound) val[a[1]] = a[2];
        else if (op == OP_TYPE_ARRAY && l >= 4 && a[0] < bound) elem[a[0]] = a[1], len[a[0]] = a[2] < bound ? val[a[2]] : 0;
        else if (op == SPV_OP_TYPE_POINTER && l >= 4 && a[0] < bound) pointee[a[0]] = a[2];
        else if (op == SPV_OP_VARIABLE && l >= 4 && a[1] < bound) var_type[a[1]] = a[0] < bound ? pointee[a[0]] : 0;
        else if (op == OP_MEMBER_DECORATE && l >= 5 && a[2] == SPV_DEC_BUILTIN && a[3] == BUILTIN_CULL) cull_struct = a[0], cull_member = a[1];
        else if (op == OP_MEMBER_DECORATE && l >= 5 && a[2] == SPV_DEC_BUILTIN && a[3] == BUILTIN_CLIP) clip_member = a[1];
        else if (op == OP_TYPE_STRUCT && cull_struct && a[0] == cull_struct && l >= 2) {
            members = l - 2;
            for (uint32_t m = 0; m < members && m < 8; m++) member_types[m] = a[1 + m];
        } else if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && l >= 5 && cull_struct && a[2] < bound) {
            uint32_t t = var_type[a[2]], k = 3;
            if (t < bound && t && elem[t] == cull_struct) k = 4;   // arrayed per-vertex block
            if ((t == cull_struct || k == 4) && k + 1 < l && a[k] < bound && val[a[k]] == cull_member) cull_struct = 0;
        }
        i += l;
    }
    VkCtx *c = vk_ctx();
    uint32_t limit = c ? c->props.limits.maxCombinedClipAndCullDistances : 8;
    if (cull_struct && members && members <= 8 && cull_member == members - 1 && clip_member < members) {
        uint32_t clip_t = member_types[clip_member], cull_t = member_types[cull_member];
        uint32_t clip_n = clip_t < bound ? len[clip_t] : 0, cull_n = cull_t < bound ? len[cull_t] : 0;
        cull_ok = clip_n + cull_n > limit && clip_n <= limit;
    }
    int strip = interlock_declared && !interlock_used;
    if (!strip && !cull_ok) goto out;
    o = malloc(n * 4);
    if (!o) goto out;
    uint64_t on = 5;
    memcpy(o, w, 5 * 4);
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        int drop = 0;
        if (strip && op == OP_CAPABILITY && (a[0] == CAP_PIXEL_INTERLOCK || a[0] == CAP_SAMPLE_INTERLOCK || a[0] == CAP_RATE_INTERLOCK)) drop = 1;
        if (strip && op == OP_EXTENSION && l >= 2 && !strncmp((const char *)a, "SPV_EXT_fragment_shader_interlock", (l - 1) * 4)) drop = 1;
        if (cull_ok && (op == OP_MEMBER_DECORATE || op == OP_MEMBER_NAME) && l >= 3 && a[0] == cull_struct && a[1] == cull_member) drop = 1;
        if (!drop) {
            memcpy(o + on, w + i, l * 4);
            if (cull_ok && op == OP_TYPE_STRUCT && a[0] == cull_struct) o[on] = (l - 1) << 16 | op;   // last member gone
            on += op == OP_TYPE_STRUCT && cull_ok && a[0] == cull_struct ? l - 1 : l;
        }
        i += l;
    }
    *out_n = on;
out:
    free(val), free(elem), free(len), free(pointee), free(var_type);
    return o;
}

// True when the module declares a GroupNonUniform* or SubgroupBallot-style capability (lane masks assume 32 lanes).
static int uses_subgroup_ops(const uint32_t *w, uint64_t words) {
    for (uint64_t i = 5; i < words;) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
        if (!n || i + n > words) return 0;
        if (op != 17) return 0;   // OpCapability; capabilities come first, stop at the first other instruction
        uint32_t cap = w[i + 1];
        if ((cap >= 61 && cap <= 68) || cap == 4423 || cap == 4431) return 1;   // NonUniform*, SubgroupBallotKHR, SubgroupVoteKHR
        i += n;
    }
    return 0;
}

// Chain {size 32} into a stage create info when the stage uses subgroup ops and the driver allows the pin.
static void pin_subgroup32(VkPipelineShaderStageCreateInfo *st, VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *rq,
                           const uint32_t *w, uint64_t words) {
    if (!(P.sg32_stages & st->stage) || !w || !uses_subgroup_ops(w, words)) return;
    *rq = (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = vk_caps.sg_size};
    st->pNext = rq;
}

static VkShaderModule make_module(const uint32_t *w, uint64_t words) {
    uint64_t fixed_n = 0;
    uint32_t *fixed = spirv_fixup(w, words, &fixed_n);
    if (fixed) w = fixed, words = fixed_n;
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)words * 4, .pCode = w};
    VkShaderModule m = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(P.device, &ci, NULL, &m);
    free(fixed);
    return r == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

// ---- descriptor layouts

typedef struct {
    uint32_t n;
    VkDescriptorSetLayoutBinding b[MAX_BINDINGS];
    VkPipeBinding pb[MAX_BINDINGS];
} Bindings;

static VkDescriptorType descriptor_type(int kind, const GfxShaderRes *r) {
    int buffer = (r->type & 0xff) == 2;
    switch (kind) {
    case GFX_RES_UBO: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case GFX_RES_SSBO: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case GFX_RES_TEX:
        if (buffer) return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        if (r->separate) LOG_ONCE("vk_pipeline: a separate texture handle: bound as SAMPLED_IMAGE");
        return r->separate ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    default: return buffer ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
}

static VkShaderStageFlags stage_bit(int stage) {
    return stage == GFX_STAGE_VS ? VK_SHADER_STAGE_VERTEX_BIT : stage == GFX_STAGE_FS ? VK_SHADER_STAGE_FRAGMENT_BIT
                                                                                      : VK_SHADER_STAGE_COMPUTE_BIT;
}

// One past the highest binding a shader uses (1 with no resources: binding 0 is the support buffer).
static uint32_t binding_span(const GfxShader *s) {
    uint32_t span = 1;
    for (int k = 0; k < GFX_RES_KINDS; k++) {
        const GfxShaderRes *r;
        int n = gfx_shader_resources(s, k, &r);
        for (int i = 0; i < n; i++)
            if (r[i].binding + (uint32_t)(r[i].count ? r[i].count : 1) > span) span = r[i].binding + (r[i].count ? r[i].count : 1);
    }
    return span;
}

// Adds a stage's resources with bindings b >= 1 moved by off; binding 0 (support buffer) is shared.
static int add_stage(Bindings *B, const GfxShader *s, uint32_t off) {
    int stage = gfx_shader_stage(s);
    VkShaderStageFlags bit = stage_bit(stage);
    if (!B->n) {
        B->b[0] = (VkDescriptorSetLayoutBinding){0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, 0, NULL};
        B->pb[0] = (VkPipeBinding){0, (uint8_t)stage, VK_PIPE_SUPPORT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, 0, NULL};
        B->n = 1;
    }
    B->b[0].stageFlags |= bit;
    B->pb[0].stages |= bit;
    for (int k = 0; k < GFX_RES_KINDS; k++) {
        const GfxShaderRes *r;
        int n = gfx_shader_resources(s, k, &r);
        for (int i = 0; i < n; i++) {
            if (B->n == MAX_BINDINGS || !r[i].binding) return 0;
            VkDescriptorType t = descriptor_type(k, &r[i]);
            uint32_t binding = r[i].binding + off, count = r[i].count ? r[i].count : 1;
            B->b[B->n] = (VkDescriptorSetLayoutBinding){binding, t, count, bit, NULL};
            B->pb[B->n] = (VkPipeBinding){(uint16_t)binding, (uint8_t)stage, (uint8_t)k, t, (uint16_t)count, bit, &r[i]};
            B->n++;
        }
    }
    return 1;
}

// The first binding a module's resource variables use that the binding list b[0..n) lacks (module bindings >= 1 moved
// by off), or -1. A reflection that misses a resource would otherwise hand the driver an invalid layout.
static int module_binding_missing(const GfxShader *s, const VkDescriptorSetLayoutBinding *b, uint32_t n, uint32_t off) {
    uint64_t words = 0;
    const uint32_t *w = gfx_shader_spirv(s, &words);
    for (uint64_t i = 5; w && i < words;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > words) break;
        if (op == SPV_OP_DECORATE && len >= 4 && w[i + 2] == SPV_DEC_BINDING) {
            uint32_t want = w[i + 3] + (w[i + 3] >= 1 ? off : 0), j = 0;
            while (j < n && b[j].binding != want) j++;
            if (j == n) return (int)w[i + 3];
        }
        i += len;
    }
    return -1;
}

// Checks both stages of a graphics pipeline against its layout; 0 with why filled when one uses a missing binding.
static int layout_covers(const VkPipeState *s, const Layout *l, uint32_t fs_off, char *why, size_t cap) {
    uint32_t nv = l->set1 ? l->n0 : l->n;
    int m = module_binding_missing(s->vs, l->b, nv, 0);
    const char *stage = "vertex";
    if (m < 0 && s->fs) {
        m = l->set1 ? module_binding_missing(s->fs, l->b + l->n0, l->n - l->n0, 0) : module_binding_missing(s->fs, l->b, l->n, fs_off);
        stage = "fragment";
    }
    if (m >= 0) snprintf(why, cap, "the %s shader uses binding %d, which its reflection and layout lack", stage, m);
    return m < 0;
}

// The update template of a set whose bindings are b[0..n): one entry per binding, in order, over the packed array the
// replay fills from the set's first descriptor, a VkDescSlot per descriptor. The bind point follows the stages (compute
// layouts hold the compute stage only). VK_NULL_HANDLE when templates are off, for an empty set, for a push set
// without vkCmdPushDescriptorSetWithTemplateKHR or when creation fails: the replay then writes the set with write
// structs.
static VkDescriptorUpdateTemplate make_tpl(const VkDescriptorSetLayoutBinding *b, uint32_t n, VkDescriptorSetLayout sl,
                                           int push, VkPipelineLayout pl, uint32_t set) {
    if (!P.tpl || !n || (push && !P.tpl_push)) return VK_NULL_HANDLE;
    VkDescriptorUpdateTemplateEntry e[MAX_BINDINGS];
    size_t off = 0;
    VkShaderStageFlags stages = 0;
    for (uint32_t i = 0; i < n; i++) {
        e[i] = (VkDescriptorUpdateTemplateEntry){b[i].binding, 0, b[i].descriptorCount, b[i].descriptorType, off, sizeof(VkDescSlot)};
        off += sizeof(VkDescSlot) * b[i].descriptorCount;
        stages |= b[i].stageFlags;
    }
    VkDescriptorUpdateTemplateCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO, .descriptorUpdateEntryCount = n,
        .pDescriptorUpdateEntries = e,
        .templateType = push ? VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR : VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,
        .descriptorSetLayout = sl,
        .pipelineBindPoint = stages & VK_SHADER_STAGE_COMPUTE_BIT ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
        .pipelineLayout = pl, .set = set};
    VkDescriptorUpdateTemplate t;
    if (P.tpl_create(P.device, &ci, NULL, &t) != VK_SUCCESS) {
        LOG_ONCE("vk_pipeline: descriptor update template creation failed: that set is written with write structs");
        return VK_NULL_HANDLE;
    }
    return t;
}

// Finds or creates the layout of a binding list (sorted here); NULL on failure. Caller holds P.lock.
// Sorts a binding list by binding and hashes it into *h; the descriptor total, or -1 when a binding repeats.
static int64_t bindings_sort(Bindings *B, uint64_t *h) {
    for (uint32_t i = 1; i < B->n; i++)                     // insertion sort by binding; the lists are short
        for (uint32_t j = i; j > 0 && B->b[j - 1].binding > B->b[j].binding; j--) {
            VkDescriptorSetLayoutBinding t = B->b[j]; B->b[j] = B->b[j - 1]; B->b[j - 1] = t;
            VkPipeBinding u = B->pb[j]; B->pb[j] = B->pb[j - 1]; B->pb[j - 1] = u;
        }
    int64_t total = 0;
    for (uint32_t i = 0; i < B->n; i++) {
        if (i && B->b[i].binding == B->b[i - 1].binding) return -1;
        uint32_t k[4] = {B->b[i].binding, B->b[i].descriptorType, B->b[i].descriptorCount, B->b[i].stageFlags};
        *h = fnv(*h, k, sizeof k);
        total += B->b[i].descriptorCount;
    }
    return total;
}

static Layout *layout_get(Bindings *B) {
    uint64_t h = FNV0;
    int64_t total = bindings_sort(B, &h);
    if (total < 0) return NULL;
    Layout **head = &layouts[h % LAYOUT_BUCKETS];
    for (Layout *l = *head; l; l = l->next)
        if (l->hash == h && l->n == B->n && !memcmp(l->b, B->b, B->n * sizeof *B->b)) return l;
    Layout *l = calloc(1, sizeof *l + B->n * sizeof *B->b);
    if (!l) return NULL;
    l->hash = h, l->n = B->n;
    memcpy(l->b, B->b, B->n * sizeof *B->b);
    l->push = total <= (int64_t)P.max_push;
    VkDescriptorSetLayoutCreateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = l->push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
                                          .bindingCount = l->n, .pBindings = l->b};
    if (vkCreateDescriptorSetLayout(P.device, &si, NULL, &l->set) != VK_SUCCESS) {
        free(l);
        return NULL;
    }
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &l->set};
    if (vkCreatePipelineLayout(P.device, &pi, NULL, &l->layout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(P.device, l->set, NULL);
        free(l);
        return NULL;
    }
    l->tpl[0] = make_tpl(l->b, l->n, l->set, l->push, l->layout, 0);
    l->next = *head;
    *head = l;
    P.stats.layouts++;
    P.stats.push_layouts += (uint64_t)l->push;
    return l;
}

// Independent layouts (P.indep): the vertex stage's bindings in set 0, the fragment stage's in set 1 (empty without a
// fragment shader), each with its own support buffer at binding 0, the pipeline layout created with
// VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT so stage libraries can use it. Only the P.push_stage set is a
// push-descriptor set (a layout holds one), when it fits maxPushDescriptors; the other is allocated per draw from the
// command buffer's pools. On success *B holds both lists, set 0 first, VkPipeBinding.set filled in. Caller holds P.lock.
static Layout *layout_pair(Bindings *B, Bindings *F) {
    uint64_t h = fnv(FNV0, "indep", 5);
    int64_t tv = bindings_sort(B, &h);
    h = fnv(h, "|", 1);
    int64_t tf = bindings_sort(F, &h);
    if (tv < 0 || tf < 0 || B->n + F->n > MAX_BINDINGS) return NULL;
    uint32_t n0 = B->n, n = B->n + F->n;
    memcpy(B->b + n0, F->b, F->n * sizeof *F->b);
    memcpy(B->pb + n0, F->pb, F->n * sizeof *F->pb);
    for (uint32_t i = n0; i < n; i++) B->pb[i].set = 1;
    B->n = n;
    Layout **head = &layouts[h % LAYOUT_BUCKETS];
    for (Layout *l = *head; l; l = l->next)
        if (l->hash == h && l->set1 && l->n == n && l->n0 == n0 && !memcmp(l->b, B->b, n * sizeof *B->b)) return l;
    Layout *l = calloc(1, sizeof *l + n * sizeof *B->b);
    if (!l) return NULL;
    l->hash = h, l->n = n, l->n0 = n0;
    memcpy(l->b, B->b, n * sizeof *B->b);
    l->push = P.push_stage == GFX_STAGE_VS && tv <= (int64_t)P.max_push;
    l->push1 = P.push_stage == GFX_STAGE_FS && F->n && tf <= (int64_t)P.max_push;
    VkDescriptorSetLayoutCreateInfo s0 = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = l->push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
                                          .bindingCount = n0, .pBindings = l->b};
    VkDescriptorSetLayoutCreateInfo s1 = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = l->push1 ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
                                          .bindingCount = n - n0, .pBindings = l->b + n0};
    if (vkCreateDescriptorSetLayout(P.device, &s0, NULL, &l->set) != VK_SUCCESS) {
        free(l);
        return NULL;
    }
    if (vkCreateDescriptorSetLayout(P.device, &s1, NULL, &l->set1) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(P.device, l->set, NULL);
        free(l);
        return NULL;
    }
    VkDescriptorSetLayout sets[2] = {l->set, l->set1};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, .setLayoutCount = 2,
                                     .pSetLayouts = sets};
    if (vkCreatePipelineLayout(P.device, &pi, NULL, &l->layout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(P.device, l->set, NULL);
        vkDestroyDescriptorSetLayout(P.device, l->set1, NULL);
        free(l);
        return NULL;
    }
    l->tpl[0] = make_tpl(l->b, n0, l->set, l->push, l->layout, 0);
    l->tpl[1] = make_tpl(l->b + n0, n - n0, l->set1, l->push1, l->layout, 1);
    l->next = *head;
    *head = l;
    P.stats.layouts++;
    P.stats.push_layouts += (uint64_t)(l->push | l->push1);
    return l;
}

// Vertex outputs with independent layouts: each vertex shader declares the union of the inputs of every fragment
// shader met with it (pipeline-keys.bin at the prewarm, then each pipeline built), so its module does not depend on the
// fragment shader of one pipeline. Unwritten ones read (0, 0, 0, 1) as before. TOTK_VS_OUTS=all declares all 32.
#define VS_OUTS_CAP 8192
static struct {
    PlatMutex lock;
    const GfxShader *vs[VS_OUTS_CAP];
    uint32_t mask[VS_OUTS_CAP];
    GfxShaderIo io[VS_OUTS_CAP][32];
    uint32_t n, full;
} VO = {.lock = PLAT_MUTEX_INIT};

// Adds fs's inputs (packed ones excepted) to vs's union; returns the union, types in io (the first type met wins).
static uint32_t vs_outs(const GfxShader *vs, const GfxShader *fs, GfxShaderIo io[32]) {
    GfxShaderIo fin[32];
    uint32_t need = fs ? gfx_shader_inputs(fs, fin) : 0, mask;
    for (int i = 0; i < 32; i++)
        if (need >> i & 1 && fin[i].packed) need &= ~(1u << i);
    plat_mutex_lock(&VO.lock);
    uint32_t j = (uint32_t)(((uintptr_t)vs >> 4) * 0x9e3779b1u) & (VS_OUTS_CAP - 1);
    while (VO.vs[j] && VO.vs[j] != vs) j = (j + 1) & (VS_OUTS_CAP - 1);
    if (!VO.vs[j] && VO.n + 1 >= VS_OUTS_CAP * 3 / 4) {   // full: this pair alone
        VO.full++;
        plat_mutex_unlock(&VO.lock);
        LOG_ONCE("vk_pipeline: vertex output union table full: outputs from the pair");
        memset(io, 0, 32 * sizeof *io);
        for (int i = 0; i < 32; i++)
            if (need >> i & 1) io[i] = fin[i];
        return need;
    }
    if (!VO.vs[j]) VO.vs[j] = vs, VO.n++;
    for (int i = 0; i < 32; i++)
        if (!(need >> i & 1)) continue;
        else if (!(VO.mask[j] >> i & 1)) VO.io[j][i] = fin[i], VO.mask[j] |= 1u << i;
        else if (VO.io[j][i].type == fin[i].type && VO.io[j][i].comps < fin[i].comps) VO.io[j][i].comps = fin[i].comps;
    mask = VO.mask[j];
    memcpy(io, VO.io[j], 32 * sizeof *io);
    plat_mutex_unlock(&VO.lock);
    return mask;
}

// ---- pipeline cache table

static Pipe *pipe_find(const void *key, size_t size, uint64_t h) {
    for (Pipe *p = pipes[h % PIPE_BUCKETS]; p; p = p->next)
        if (p->hash == h && p->key_size == size && !memcmp(p->key, key, size)) return p;
    return NULL;
}

// Stores a result (a failed one too, so it is not retried every draw). Caller holds P.lock.
static Pipe *pipe_add(const void *key, size_t size, uint64_t h, VkPipeline pipeline, const Layout *l, const Bindings *B,
                      VkPipelineBindPoint bp, uint32_t vs_inputs) {
    Pipe *p = calloc(1, sizeof *p + size);
    if (!p) return NULL;
    p->hash = h, p->key_size = size;
    memcpy(p->key, key, size);
    if (l && B) {
        p->bindings = malloc(B->n * sizeof *p->bindings);
        if (p->bindings) memcpy(p->bindings, B->pb, B->n * sizeof *p->bindings);
    }
    p->info = (VkPipeInfo){pipeline, l ? l->layout : VK_NULL_HANDLE, l ? l->set : VK_NULL_HANDLE, l ? l->push : 0, bp,
                           p->bindings ? B->n : 0, p->bindings, vs_inputs};
    p->info.set_layout1 = l ? l->set1 : VK_NULL_HANDLE;
    p->info.push1 = l ? l->push1 : 0;
    p->info.n_sets = l && l->set1 ? 2 : 1;
    if (l) p->info.tpl[0] = l->tpl[0], p->info.tpl[1] = l->tpl[1];
    p->lay = l;
    Pipe **head = &pipes[h % PIPE_BUCKETS];
    p->next = *head;
    *head = p;
    return p;
}

// ---- graphics pipelines

typedef struct {                       // normalized: zero where the state is dynamic or unused
    const GfxShader *vs, *fs;
    uint32_t color[8], depth, stencil; // VkFormat
    uint8_t n_color, samples, topo_class, minus_w;
    uint8_t clamp, poly_mode, a2c, logic_op;   // logic_op: VkLogicOp + 1 when enabled
    uint8_t blend_en, dsb, fbl, pad;  // dsb: an enabled target blends with a SRC1 factor; fbl: VkPipeState.feedback
    uint32_t wmask;
    uint8_t blend[8][6];               // src, dst, op, src alpha, dst alpha, alpha op (VkBlendFactor / VkBlendOp)
    uint32_t attr_fmt[32], attr_off[32];
    uint8_t attr_bind[32];
    uint8_t rate[17];                  // per binding: 0 unused, 1 vertex, 2 instance
    uint8_t indep;                     // P.indep when built: the probe draws through both layouts
    uint8_t pad2[2];
} GKey;

static int blendable(VkFormat f) {
    return f != VK_FORMAT_UNDEFINED && (format_features(f, 1) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
}

static int target_count(const VkPipeState *s) {
    int n = 0;
    for (int i = 0; i < 8; i++)
        if (s->color_fmt[i]) n = i + 1;
    return n;
}

// SRC1 blend factors read the fragment shader's second output (dual-source blending); the pipeline then needs a
// fragment module whose output 1 is Location 0, Index 1 (spirv_dual_source).
static int is_src1(unsigned f) { return f >= NVN_BLEND_FUNC_SRC1_COLOR && f <= NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA; }
// Runs per draw (gkey_fill, target_wmask): only the blend factors of blend-enabled targets are read until one is SRC1,
// which TotK never sets (0 of 876 pipelines in the field and fight saves).
static int uses_src1(const VkPipeState *s) {
    for (uint32_t m = s->color.blend_en & 0xffu; m; m &= m - 1) {
        int i = __builtin_ctz(m);
        const blend_st *b = &s->blend[i];
        if ((is_src1(b->src_rgb) || is_src1(b->dst_rgb) || is_src1(b->src_a) || is_src1(b->dst_a)) &&
            i < target_count(s) && blendable(target_format(s->color_fmt[i], NULL, NULL)))
            return 1;
    }
    return 0;
}

// Colour write mask of target i: NVN's channel mask, zero for a target outside the fragment shader's output map (SPH
// omap). Maxwell leaves such a target untouched (nxnative 04-graphics.md, "Targets the fragment shader does not
// write"); Vulkan writes undefined values, and on AMD they lit grass in front of Link as flat lime. Zeros are wrong
// too: TOTK's half-res transparency pass binds its linear depth (R32F) as target 7, samples it in the same draws and
// writes only targets 0-2, 4 and 5; zeroed, the depth that later passes rebuild from it was 0 in tiles, and the fog
// over distant geometry showed rectangles of clear sky.
static uint32_t target_wmask(const VkPipeState *s, int i) {
    uint32_t m = s->chmask.mask >> (4 * i) & 0xf;
    if (s->fs && !(gfx_shader_outputs(s->fs, NULL) >> i & 1)) m = 0;
    if (i == 1 && s->fs && P.f_dual_src && uses_src1(s)) m = 0;   // output 1 is the second blend source (spirv_dual_source)
    return m;
}

uint32_t vk_pipeline_target_wmask(const VkPipeState *s, int i) { return target_wmask(s, i); }

static void gkey_fill(const VkPipeState *s, GKey *k) {
    memset(k, 0, sizeof *k);
    k->vs = s->vs, k->fs = s->fs;
    k->indep = (uint8_t)P.indep;
    k->n_color = (uint8_t)target_count(s);
    for (int i = 0; i < k->n_color; i++) k->color[i] = target_format(s->color_fmt[i], NULL, NULL);
    int d, st;
    VkFormat df = target_format(s->depth_fmt, &d, &st);
    k->depth = d ? df : VK_FORMAT_UNDEFINED;
    k->stencil = st ? df : VK_FORMAT_UNDEFINED;
    k->samples = s->samples > 1 ? s->samples : 1;
    k->topo_class = (uint8_t)topology_class(vk_pipeline_topology(s->prim));
    k->minus_w = s->depth_minus_w != 0;
    k->dsb = s->fs && P.f_dual_src && uses_src1(s);
    k->fbl = s->fs ? s->feedback : 0;
    if (!P.dyn_clamp) k->clamp = s->depth_clamp && P.f_depth_clamp;
    if (!P.dyn_poly) k->poly_mode = (uint8_t)polygon_mode(s->poly.mode);
    if (!P.dyn_a2c) k->a2c = s->ms.a2c != 0;
    if (P.f_logic_op && s->color.logic_op != NVN_LOGIC_OP_COPY) k->logic_op = (uint8_t)(s->color.logic_op + 1);
    for (int i = 0; i < k->n_color; i++) {
        int on = (s->color.blend_en >> i & 1) && blendable(k->color[i]);
        if (!P.dyn_blend_en && on) k->blend_en |= (uint8_t)(1u << i);
        if (!P.dyn_wmask) k->wmask |= target_wmask(s, i) << (4 * i);
        if (!P.dyn_blend_eq && on) {
            const blend_st *b = &s->blend[i];
            uint8_t *o = k->blend[i];
            o[0] = (uint8_t)blend_factor(b->src_rgb), o[1] = (uint8_t)blend_factor(b->dst_rgb), o[2] = (uint8_t)blend_op(b->eq_rgb);
            o[3] = (uint8_t)blend_factor(b->src_a), o[4] = (uint8_t)blend_factor(b->dst_a), o[5] = (uint8_t)blend_op(b->eq_a);
        }
    }
    if (!P.dyn_vi) {
        VtxInput v;
        vertex_input(s, &v);
        for (uint32_t i = 0; i < v.n_attr; i++) {
            uint32_t l = v.a[i].location;
            k->attr_fmt[l] = v.a[i].format, k->attr_off[l] = v.a[i].offset, k->attr_bind[l] = (uint8_t)v.a[i].binding;
        }
        for (uint32_t i = 0; i < v.n_bind; i++)
            k->rate[v.b[i].binding] = v.b[i].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? 2 : 1;
    }
}

// Independent layouts: the outputs the vertex shader lacks from its union with s->fs's inputs added (vs_outs), types in
// fin. The vertex module (and its pre-rasterization library) depends on the vertex shader and this mask only.
static uint32_t vs_union_missing(const VkPipeState *s, GfxShaderIo fin[32]) {
    GfxShaderIo vout[32];
    uint32_t need = vs_outs(s->vs, s->fs, fin), have = gfx_shader_outputs(s->vs, vout), missing = 0;
    if (s->fs) {   // a location another fragment shader reads as another type: this pair's type (rare)
        GfxShaderIo fi[32];
        uint32_t fneed = gfx_shader_inputs(s->fs, fi);
        for (int i = 0; i < 32; i++)
            if (fneed >> i & 1 && !fi[i].packed &&
                (!(need >> i & 1) || fin[i].type != fi[i].type || fin[i].comps < fi[i].comps)) {
                if (need >> i & 1 && fin[i].type != fi[i].type)
                    LOG_ONCE("vk_pipeline: varying %d read as type %d and %d by fragment shaders of one vertex shader", i, fin[i].type, fi[i].type);
                fin[i] = fi[i], need |= 1u << i;
            }
    }
    if (P.vs_outs_all)
        for (int i = 0; i < 32; i++)
            if (!(need >> i & 1)) fin[i] = (GfxShaderIo){GFX_TYPE_FLOAT, 4, 0}, need |= 1u << i;
    for (int i = 0; i < 32; i++)
        if (need >> i & 1 && !(have >> i & 1)) missing |= 1u << i;
    return missing;
}

static VkPipeline create_graphics(const VkPipeState *s, const GKey *k, const Layout *l, uint32_t fs_off) {
    VkShaderModule vm = VK_NULL_HANDLE, fm = VK_NULL_HANDLE;
    uint64_t vw, fw;
    const uint32_t *vcode = gfx_shader_spirv(s->vs, &vw);
    uint32_t *vpatched = NULL, *fpatched = NULL;
    if (l->set1) {   // independent layouts: outputs from the vertex shader's union, not from this fragment shader
        GfxShaderIo fin[32];
        uint32_t missing = vs_union_missing(s, fin);
        if (missing) {
            uint64_t n2 = 0;
            vpatched = spirv_add_outputs(vcode, vw, missing, fin, &n2);
            if (!vpatched) LOG_ONCE("vk_pipeline: cannot add missing vertex outputs (mask %#x)", missing);
            else vcode = vpatched, vw = n2;
        }
    }
    if (s->fs) {
        GfxShaderIo fin[32], vout[32];
        uint32_t need = gfx_shader_inputs(s->fs, fin), have = gfx_shader_outputs(s->vs, vout), missing = 0;
        for (int i = 0; i < 32; i++) {
            if (!(need >> i & 1) || fin[i].packed) continue;
            if (!(have >> i & 1)) missing |= 1u << i;
            else if (vout[i].type != fin[i].type)
                LOG_ONCE("vk_pipeline: varying %d is type %d in the vertex stage, %d in the fragment stage", i, vout[i].type, fin[i].type);
        }
        if (missing && !l->set1) {
            uint64_t n2 = 0;
            vpatched = spirv_add_outputs(vcode, vw, missing, fin, &n2);
            if (!vpatched) LOG_ONCE("vk_pipeline: cannot add missing vertex outputs (mask %#x)", missing);
            else vcode = vpatched, vw = n2;
        }
        const uint32_t *fcode = gfx_shader_spirv(s->fs, &fw);
        if (k->dsb) {
            uint64_t n2 = 0;
            uint32_t *d = spirv_dual_source(fcode, fw, &n2);
            if (d) fcode = fpatched = d, fw = n2;
            else LOG_ONCE("vk_pipeline: SRC1 blend factors but the fragment shader writes no colour output 1");
        }
        if (l->set1 || fs_off) {
            uint32_t *r = !fcode ? NULL : l->set1 ? spirv_set(fcode, fw, 1) : spirv_rebind(fcode, fw, fs_off);
            free(fpatched);
            fcode = fpatched = r;
        }
        // A bound colour target outside the shader's output map gets write mask 0 (target_wmask)
        if (fcode) fm = make_module(fcode, fw);
    }
    vm = make_module(vcode, vw);
    free(vpatched), free(fpatched);
    VkPipeline pipe = VK_NULL_HANDLE;
    if (!vm || (s->fs && !fm)) goto out;

    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main"}};
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rqv, rqf;
    uint64_t sw;
    const uint32_t *scode = gfx_shader_spirv(s->vs, &sw);   // vcode may be a patched copy that is freed by now
    pin_subgroup32(&stages[0], &rqv, scode, sw);
    if (s->fs) pin_subgroup32(&stages[1], &rqf, gfx_shader_spirv(s->fs, &sw), sw);
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    uint32_t na = 0, nb = 0;
    if (!P.dyn_vi) {
        VtxInput v;
        vertex_input(s, &v);
        for (uint32_t i = 0; i < v.n_attr; i++) va[na++] = (VkVertexInputAttributeDescription){v.a[i].location, v.a[i].binding, v.a[i].format, v.a[i].offset};
        for (uint32_t i = 0; i < v.n_bind; i++) vb[nb++] = (VkVertexInputBindingDescription){v.b[i].binding, v.b[i].stride, v.b[i].inputRate};
    }
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    static const VkPrimitiveTopology class_base[4] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = class_base[k->topo_class]};
    VkPipelineViewportDepthClipControlCreateInfoEXT clip = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT,
                                                            .negativeOneToOne = k->minus_w};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .pNext = &clip};
    // NVN's default is the last vertex (SetProvokingVertexMode, nvn_ext.c); Vulkan's is the first.
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pvs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
        .provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .pNext = P.pv_last ? &pvs : NULL,
                                                 .depthClampEnable = k->clamp, .polygonMode = (VkPolygonMode)k->poly_mode,
                                                 .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples,
                                               .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .maxDepthBounds = 1.0f};
    VkPipelineColorBlendAttachmentState cba[8];
    memset(cba, 0, sizeof cba);
    for (int i = 0; i < k->n_color; i++) {
        const uint8_t *b = k->blend[i];
        cba[i].blendEnable = k->blend_en >> i & 1;
        cba[i].srcColorBlendFactor = (VkBlendFactor)b[0], cba[i].dstColorBlendFactor = (VkBlendFactor)b[1];
        cba[i].colorBlendOp = (VkBlendOp)b[2], cba[i].srcAlphaBlendFactor = (VkBlendFactor)b[3];
        cba[i].dstAlphaBlendFactor = (VkBlendFactor)b[4], cba[i].alphaBlendOp = (VkBlendOp)b[5];
        cba[i].colorWriteMask = k->wmask >> (4 * i) & 0xf;
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .logicOpEnable = k->logic_op != 0,
                                              .logicOp = k->logic_op ? (VkLogicOp)(k->logic_op - 1) : VK_LOGIC_OP_COPY,
                                              .attachmentCount = k->n_color, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = P.n_dyn, .pDynamicStates = P.dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->n_color,
                                        .pColorAttachmentFormats = (const VkFormat *)k->color,
                                        .depthAttachmentFormat = (VkFormat)k->depth, .stencilAttachmentFormat = (VkFormat)k->stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .stageCount = s->fs ? 2 : 1, .pStages = stages, .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
                                       .pDynamicState = &dy, .layout = l->layout};
    if (k->fbl & 1) pi.flags |= VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    if (k->fbl & 2) pi.flags |= VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    uint64_t p0 = perf_begin();
    VkResult r = vkCreateGraphicsPipelines(P.device, P.cache, 1, &pi, NULL, &pipe);
    perf_end(PERF_PIPE, p0);   // counts on the render thread only, so background builds stay out of it
    if (r != VK_SUCCESS) {
        rt_log("vk_pipeline: vkCreateGraphicsPipelines failed (%d)", (int)r);
        pipe = VK_NULL_HANDLE;
    } else {
        perf_count(PERF_CNT_PIPE_BUILT);
    }
out:
    if (vm) vkDestroyShaderModule(P.device, vm, NULL);
    if (fm) vkDestroyShaderModule(P.device, fm, NULL);
    return pipe;
}

// ---- graphics pipeline library (vk_caps.gpl)
// A pipeline is linked from four libraries, each cached by the part of the pipeline key it depends on: vertex input
// (topology class, vertex attributes without dynamic vertex input), pre-rasterization (vertex shader, the outputs its
// union adds, static rasterization state), fragment shader (fragment shader, dual-source rewrite, samples) and fragment
// output (targets, blend, samples). With independent layouts each stage's set depends on its shader alone, so the
// pre-rasterization library uses {set 0, null} and the fragment shader library {null, set 1}. The fast link runs on the
// thread of the draw that needs it (no link-time optimisation); the optimised pipeline is the job pipe_miss queued
// (a monolithic build with the same modules), which replaces the fast one in the Pipe when done. Libraries and fast
// pipelines live until vk_pipeline_shutdown.
// Background build signalling, both with P.lock: work_cv wakes the pipeline workers, done_cv wakes threads waiting for a
// pipeline or a stage library to be published.
static PlatCond work_cv = PLAT_COND_INIT, done_cv = PLAT_COND_INIT;
#define LIB_BUCKETS 1024
enum { LIB_VI, LIB_PR, LIB_FS, LIB_FO };
typedef struct Lib { struct Lib *next; uint64_t hash; VkPipeline pipe; uint32_t size; uint8_t pre, used, building, pad;
                     struct LibJob *job; uint8_t key[]; } Lib;
// pre: built by the boot prebuild ("stage library prebuild" below); used: a draw's link found it; building: a thread
// builds it or job holds it queued (pipe not set yet); entries live until libs_destroy
static Lib *libs[LIB_BUCKETS];

typedef struct { uint32_t kind, topo_class, attr_fmt[32], attr_off[32]; uint8_t attr_bind[32], rate[17], pad[3]; } ViKey;
typedef struct { uint32_t kind, missing; const GfxShader *vs; uint8_t clamp, poly_mode, minus_w, pad; GfxShaderIo fin[32]; } PrKey;
typedef struct { uint32_t kind; uint8_t dsb, samples, a2c, pad; const GfxShader *fs; } FsKey;
typedef struct { uint32_t kind; uint8_t n_color, samples, a2c, logic_op, blend_en, pad[3]; uint32_t color[8], depth, stencil, wmask;
                 uint8_t blend[8][6]; } FoKey;

static uint64_t qpc(void) { return plat_now_ns(); }
static double ms_since(uint64_t t0) { return (double)(qpc() - t0) / 1e6; }

static VkPipelineLayout half_layout(Layout *l, int h) {
    if (l->half[h]) return l->half[h];
    VkDescriptorSetLayout sets[2] = {h ? VK_NULL_HANDLE : l->set, h ? l->set1 : VK_NULL_HANDLE};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, .setLayoutCount = 2,
                                     .pSetLayouts = sets};
    if (vkCreatePipelineLayout(P.device, &pi, NULL, &l->half[h]) != VK_SUCCESS) l->half[h] = VK_NULL_HANDLE;
    return l->half[h];
}

// Builds one library from the pipeline's state; key is the library's key (PrKey: fin holds the added outputs).
static VkPipeline lib_create(int kind, const VkPipeState *s, const GKey *k, VkPipelineLayout lay, const void *key) {
    static const VkGraphicsPipelineLibraryFlagsEXT part[4] = {
        VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
    VkGraphicsPipelineLibraryCreateInfoEXT gl = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, .flags = part[kind]};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .pNext = &gl};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = P.n_dyn, .pDynamicStates = P.dyn};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR, .pDynamicState = &dy};
    VkShaderModule m = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pName = "main"};
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rq;
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    static const VkPrimitiveTopology class_base[4] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = class_base[k->topo_class]};
    VkPipelineViewportDepthClipControlCreateInfoEXT clip = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT,
                                                            .negativeOneToOne = k->minus_w};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .pNext = &clip};
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pvs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
        .provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .pNext = P.pv_last ? &pvs : NULL, .depthClampEnable = k->clamp,
                                                 .polygonMode = (VkPolygonMode)k->poly_mode, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples,
                                               .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .maxDepthBounds = 1.0f};
    VkPipelineColorBlendAttachmentState cba[8];
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    uint32_t *patched = NULL;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (kind == LIB_VI) {
        if (!P.dyn_vi) {
            VtxInput v;
            vertex_input(s, &v);
            for (uint32_t i = 0; i < v.n_attr; i++)
                va[i] = (VkVertexInputAttributeDescription){v.a[i].location, v.a[i].binding, v.a[i].format, v.a[i].offset};
            for (uint32_t i = 0; i < v.n_bind; i++)
                vb[i] = (VkVertexInputBindingDescription){v.b[i].binding, v.b[i].stride, v.b[i].inputRate};
            vi.vertexAttributeDescriptionCount = v.n_attr, vi.pVertexAttributeDescriptions = va;
            vi.vertexBindingDescriptionCount = v.n_bind, vi.pVertexBindingDescriptions = vb;
        }
        pi.pVertexInputState = &vi, pi.pInputAssemblyState = &ia;
    } else if (kind == LIB_PR) {
        const PrKey *pk = key;
        uint64_t vw, sw, n2 = 0;
        const uint32_t *vcode = gfx_shader_spirv(s->vs, &vw), *scode = vcode;
        sw = vw;
        if (pk->missing && (patched = spirv_add_outputs(vcode, vw, pk->missing, pk->fin, &n2))) vcode = patched, vw = n2;
        else if (pk->missing) LOG_ONCE("vk_pipeline: cannot add missing vertex outputs (mask %#x)", pk->missing);
        if (!(m = make_module(vcode, vw))) goto out;
        st.stage = VK_SHADER_STAGE_VERTEX_BIT, st.module = m;
        pin_subgroup32(&st, &rq, scode, sw);
        pi.stageCount = 1, pi.pStages = &st, pi.pViewportState = &vp, pi.pRasterizationState = &rs;
        pi.layout = lay;
    } else if (kind == LIB_FS) {
        if (s->fs) {
            uint64_t fw, sw, n2 = 0;
            const uint32_t *fcode = gfx_shader_spirv(s->fs, &fw), *scode = fcode;
            sw = fw;
            uint32_t *d = NULL;
            if (k->dsb && fcode) {
                if ((d = spirv_dual_source(fcode, fw, &n2))) fcode = d, fw = n2;
                else LOG_ONCE("vk_pipeline: SRC1 blend factors but the fragment shader writes no colour output 1");
            }
            patched = fcode ? spirv_set(fcode, fw, 1) : NULL;
            free(d);
            if (!patched || !(m = make_module(patched, fw))) goto out;
            st.stage = VK_SHADER_STAGE_FRAGMENT_BIT, st.module = m;
            pin_subgroup32(&st, &rq, scode, sw);
            pi.stageCount = 1, pi.pStages = &st;
        }
        pi.pMultisampleState = &ms, pi.pDepthStencilState = &ds;
        pi.layout = lay;
    } else {
        memset(cba, 0, sizeof cba);
        for (int i = 0; i < k->n_color; i++) {
            const uint8_t *b = k->blend[i];
            cba[i].blendEnable = k->blend_en >> i & 1;
            cba[i].srcColorBlendFactor = (VkBlendFactor)b[0], cba[i].dstColorBlendFactor = (VkBlendFactor)b[1];
            cba[i].colorBlendOp = (VkBlendOp)b[2], cba[i].srcAlphaBlendFactor = (VkBlendFactor)b[3];
            cba[i].dstAlphaBlendFactor = (VkBlendFactor)b[4], cba[i].alphaBlendOp = (VkBlendOp)b[5];
            cba[i].colorWriteMask = k->wmask >> (4 * i) & 0xf;
        }
        cb.logicOpEnable = k->logic_op != 0, cb.logicOp = k->logic_op ? (VkLogicOp)(k->logic_op - 1) : VK_LOGIC_OP_COPY;
        cb.attachmentCount = k->n_color, cb.pAttachments = cba;
        ri.colorAttachmentCount = k->n_color, ri.pColorAttachmentFormats = (const VkFormat *)k->color;
        ri.depthAttachmentFormat = (VkFormat)k->depth, ri.stencilAttachmentFormat = (VkFormat)k->stencil;
        pi.pColorBlendState = &cb, pi.pMultisampleState = &ms;
    }
    if (vkCreateGraphicsPipelines(P.device, P.cache, 1, &pi, NULL, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
out:
    if (m) vkDestroyShaderModule(P.device, m, NULL);
    free(patched);
    return pipe;
}

// The cached library for key. Caller holds P.lock. build 0: lookup only (VK_NULL_HANDLE and *miss when absent);
// build 1: a missing one is built with P.lock released (a failed build is cached too).
// The library entry for key, NULL when absent. Caller holds P.lock.
static Lib *lib_find(uint64_t h, const void *key, uint32_t size) {
    for (Lib *b = libs[h % LIB_BUCKETS]; b; b = b->next)
        if (b->hash == h && b->size == size && !memcmp(b->key, key, size)) return b;
    return NULL;
}

// Enters a library built with P.lock released; another thread's entry made meanwhile wins and pipe is destroyed.
// Returns the entered handle. Caller holds P.lock.
static VkPipeline lib_put(uint64_t h, const void *key, uint32_t size, VkPipeline pipe) {
    Lib *b = lib_find(h, key, size);
    if (b && b->building) {   // another thread's build publishes the entry
        if (pipe) vkDestroyPipeline(P.device, pipe, NULL);
        return VK_NULL_HANDLE;
    }
    if (b) {
        if (pipe) vkDestroyPipeline(P.device, pipe, NULL);
        return b->pipe;
    }
    if (!(b = calloc(1, sizeof *b + size))) return pipe;
    b->hash = h, b->size = size, b->pipe = pipe;
    memcpy(b->key, key, size);
    b->next = libs[h % LIB_BUCKETS];
    libs[h % LIB_BUCKETS] = b;
    return pipe;
}

// Shader libraries a waiting draw needs (lib_wait): queued here ahead of every pipeline job, so an idle worker builds
// each at once and the vertex and fragment libraries of one draw build in parallel. A job no worker took is built by
// the first thread that needs its library (lib_get, lib_wait). Under P.lock.
typedef struct LibJob { struct LibJob *next; Lib *b; int kind; Layout *l; VkPipeState s; GKey k; } LibJob;
static struct {
    LibJob *head, *tail;
    int idle;                          // workers asleep on work_cv
    int running;                       // jobs workers are building
    int on;                            // TOTK_PIPE_WAIT: 1 workers (default), 0 the drawing thread builds them (draw);
                                       // set by vk_pipeline_async_enable, before the replay runs
    uint64_t waits, queued, by_worker, stolen, timeouts;
} U;

// A building entry for key, NULL when out of memory. Caller holds P.lock and found none.
static Lib *lib_new(uint64_t h, const void *key, uint32_t size) {
    Lib *b = calloc(1, sizeof *b + size);
    if (!b) return NULL;
    b->hash = h, b->size = size, b->building = 1;
    memcpy(b->key, key, size);
    b->next = libs[h % LIB_BUCKETS];
    libs[h % LIB_BUCKETS] = b;
    return b;
}

// Builds b (building, not queued) with P.lock released and publishes it; a failed build is entered as VK_NULL_HANDLE.
static VkPipeline lib_build(Lib *b, int kind, const VkPipeState *s, const GKey *k, Layout *l) {
    VkPipelineLayout lay = kind == LIB_PR ? half_layout(l, 0) : kind == LIB_FS ? half_layout(l, 1) : VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (lay || kind == LIB_VI || kind == LIB_FO) {
        plat_mutex_unlock(&P.lock);
        uint64_t t0 = qpc();
        pipe = lib_create(kind, s, k, lay, b->key);
        double ms = ms_since(t0);
        plat_mutex_lock(&P.lock);
        P.g.lib_n[kind]++, P.g.lib_ms[kind] += ms;
        if (ms > P.g.lib_max[kind]) P.g.lib_max[kind] = ms;
    }
    if (!pipe && ++P.g.lib_fail <= 8) rt_log("vk_pipeline: gpl: stage library %d not built", kind);
    b->pipe = pipe, b->building = 0;
    plat_cond_broadcast(&done_cv);
    return pipe;
}

// Takes b's queued job out of the queue and builds it on this thread. Caller holds P.lock.
static void lib_steal(Lib *b) {
    LibJob *j = b->job, **pp = &U.head, *prev = NULL;
    while (*pp && *pp != j) prev = *pp, pp = &(*pp)->next;
    if (*pp) *pp = j->next;
    if (U.tail == j) U.tail = prev;
    b->job = NULL;
    lib_build(b, j->kind, &j->s, &j->k, j->l);
    free(j);
}

static VkPipeline lib_get(int kind, const VkPipeState *s, const GKey *k, Layout *l, const void *key, uint32_t size,
                          int build, int *miss) {
    uint64_t h = key_hash(key, size);
    Lib *have = lib_find(h, key, size);
    while (have && have->building) {   // being built (wait) or queued (build it here)
        if (!build) {
            *miss = 1;
            return VK_NULL_HANDLE;
        }
        if (have->job) U.stolen++, lib_steal(have);
        else plat_cond_wait(&done_cv, &P.lock);
    }
    if (have) {
        if (have->pre && !have->used) have->used = 1, P.g.pre_used[kind]++;
        return have->pipe;
    }
    VkPipelineLayout lay = kind == LIB_PR ? half_layout(l, 0) : kind == LIB_FS ? half_layout(l, 1) : VK_NULL_HANDLE;
    if (!build || ((kind == LIB_PR || kind == LIB_FS) && !lay)) {
        *miss = 1;
        return VK_NULL_HANDLE;
    }
    Lib *b = lib_new(h, key, size);   // entered first, so other threads wait for this build instead of repeating it
    if (b) return lib_build(b, kind, s, k, l);
    plat_mutex_unlock(&P.lock);
    uint64_t t0 = qpc();
    VkPipeline pipe = lib_create(kind, s, k, lay, key);
    double ms = ms_since(t0);
    plat_mutex_lock(&P.lock);
    if ((have = lib_find(h, key, size)) && !have->building) {   // another thread built it meanwhile
        if (pipe) vkDestroyPipeline(P.device, pipe, NULL);
        return have->pipe;
    }
    P.g.lib_n[kind]++, P.g.lib_ms[kind] += ms;
    if (ms > P.g.lib_max[kind]) P.g.lib_max[kind] = ms;
    if (!pipe && ++P.g.lib_fail <= 8) rt_log("vk_pipeline: gpl: stage library %d not built", kind);
    return lib_put(h, key, size, pipe);
}

static void libs_destroy(void) {
    for (int i = 0; i < LIB_BUCKETS; i++) {
        for (Lib *b = libs[i], *n; b; b = n) {
            n = b->next;
            if (b->pipe) vkDestroyPipeline(P.device, b->pipe, NULL);
            free(b);
        }
        libs[i] = NULL;
    }
}

// The four library keys of pipeline s (key k).
static void lib_keys(const VkPipeState *s, const GKey *k, ViKey *v, PrKey *pk, FsKey *fk, FoKey *ok) {
    memset(v, 0, sizeof *v);
    v->kind = LIB_VI, v->topo_class = k->topo_class;
    memcpy(v->attr_fmt, k->attr_fmt, sizeof v->attr_fmt), memcpy(v->attr_off, k->attr_off, sizeof v->attr_off);
    memcpy(v->attr_bind, k->attr_bind, sizeof v->attr_bind), memcpy(v->rate, k->rate, sizeof v->rate);
    memset(pk, 0, sizeof *pk);
    GfxShaderIo fin[32];
    pk->kind = LIB_PR, pk->vs = s->vs, pk->clamp = k->clamp, pk->poly_mode = k->poly_mode, pk->minus_w = k->minus_w;
    pk->missing = vs_union_missing(s, fin);
    for (int i = 0; i < 32; i++)
        if (pk->missing >> i & 1) pk->fin[i] = fin[i];
    memset(fk, 0, sizeof *fk);
    fk->kind = LIB_FS, fk->fs = s->fs, fk->dsb = k->dsb, fk->samples = k->samples, fk->a2c = k->a2c;
    memset(ok, 0, sizeof *ok);
    ok->kind = LIB_FO, ok->n_color = k->n_color, ok->samples = k->samples, ok->a2c = k->a2c, ok->logic_op = k->logic_op;
    ok->blend_en = k->blend_en, ok->depth = k->depth, ok->stencil = k->stencil, ok->wmask = k->wmask;
    memcpy(ok->color, k->color, sizeof ok->color), memcpy(ok->blend, k->blend, sizeof ok->blend);
}

// A fast-linked pipeline for s from its stage libraries. Caller holds P.lock.
// build 0 (drawing thread): links only when the shader libraries exist (vertex input and fragment output libraries,
// which hold no shader, are built here); *final 0 = not yet, a worker builds them. build 1 (worker): builds the
// missing libraries and links with P.lock released. VK_NULL_HANDLE with *final 1 when GPL cannot express the pipeline
// (shared layout, feedback loop) or a build failed.
static VkPipeline gpl_link(const VkPipeState *s, const GKey *k, const Layout *lc, int build, int *final) {
    Layout *l = (Layout *)lc;
    *final = 1;
    if (!l->set1 || k->fbl) return VK_NULL_HANDLE;
    uint64_t t0 = qpc();
    ViKey vk_;
    PrKey pk;
    FsKey fk;
    FoKey ok;
    lib_keys(s, k, &vk_, &pk, &fk, &ok);
    int miss = 0;
    VkPipeline lib[4];
    lib[1] = lib_get(LIB_PR, s, k, l, &pk, sizeof pk, build, &miss);   // shader libraries first: no work when absent
    lib[2] = miss ? VK_NULL_HANDLE : lib_get(LIB_FS, s, k, l, &fk, sizeof fk, build, &miss);
    if (miss) {
        if (!build) *final = 0, P.g.defer++;
        return VK_NULL_HANDLE;
    }
    lib[0] = lib_get(LIB_VI, s, k, l, &vk_, sizeof vk_, 1, &miss);
    lib[3] = lib_get(LIB_FO, s, k, l, &ok, sizeof ok, 1, &miss);
    VkPipeline pipe = VK_NULL_HANDLE;
    if (lib[0] && lib[1] && lib[2] && lib[3]) {
        VkPipelineLibraryCreateInfoKHR li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, .libraryCount = 4,
                                             .pLibraries = lib};
        VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &li,
                                           .layout = l->layout};
        if (build) plat_mutex_unlock(&P.lock);
        uint64_t t1 = qpc();
        if (vkCreateGraphicsPipelines(P.device, VK_NULL_HANDLE, 1, &pi, NULL, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
        double ms = ms_since(t1);   // the link alone: library builds are counted apart
        if (build) plat_mutex_lock(&P.lock);
        P.g.link_ms += ms;
        if (ms > P.g.link_max) P.g.link_max = ms;
    }
    if (pipe) P.g.links++;
    else if (!P.g.link_fail++) rt_log("vk_pipeline: gpl: fast link failed; such draws wait or skip as without GPL");
    double total = ms_since(t0);
    if (!build && total > 5.0 && P.g.slow++ < 16)
        rt_log("vk_pipeline: gpl: fast link on the drawing thread took %.1f ms", total);
    return pipe;
}

// ---- background graphics pipeline builds (vk_pipeline_async_enable)
// A miss in vk_pipeline_graphics_async enters the Pipe as building and queues a job; the replay skips draws with it
// until a worker publishes the handle. The job holds copies of the state and the key. What they point to outlives it:
// shaders (gfx_shader_get results) live for the process (gfx_shader_drop_memory is the shader probe's, which builds no
// pipelines), layouts until vk_pipeline_shutdown, which drains the queue first. Workers call vkCreateGraphicsPipelines
// without P.lock: P.cache is created without VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT, so the driver
// synchronises it, vkGetPipelineCacheData in vk_pipeline_save included. Compute pipelines stay synchronous: a skipped
// dispatch can change what the game computes.
//
// Two queues: draws' misses (A.head) and the boot prebuild (A.low, see "pipeline keys" below). Workers take the low
// queue only when the other is empty and run its jobs at PLAT_PRIO_BELOW_NORMAL, so guest threads win. A draw
// that finds a prebuild job still queued moves it to the draw queue; one that finds it running raises its worker.
typedef struct Job {
    struct Job *next, *prev;           // prev: low queue only
    Pipe *p;
    const Layout *l;
    uint32_t fs_off;
    uint8_t low;                       // in the low queue, or running at below-normal priority
    uint8_t prebuild, demanded;        // from the key file; a draw asked for it before it was ready
    int worker;                        // running on A.thread[worker]; -1 queued
    VkPipeState s;
    GKey k;
} Job;

#define MAX_WORKERS 4

static struct {
    int on;                            // set once by vk_pipeline_async_enable, before the replay runs
    int threads;
    PlatThread *thread[MAX_WORKERS];
    uint8_t prio_low[MAX_WORKERS];     // worker runs below normal (under P.lock)
    Job *head, *tail;                  // draws' misses
    Job *low, *low_tail;               // prebuild
    uint32_t inflight;                 // queued or running, both queues
    int low_running;                   // pool workers running a job taken from the low queue
    PlatCond idle;           // with P.lock: no job queued or running
} A = {.idle = PLAT_COND_INIT};

// ---- pipeline keys (pipeline-keys.bin next to the pipeline cache)
// Every graphics pipeline built in a game run appends a KeyRec: the VkPipeState it was built from with the shader
// pointers zeroed and the shaders named by their gfx_shader input ids instead, which do not depend on the translator,
// so the record rebuilds the pipeline without guest memory. At the next boot vk_pipeline_prewarm (rt_boot, before the
// guest runs; TOTK_PIPE_PREBUILD=0 skips it) loads each record's shaders, translating the ones not cached under the
// current translator from their stored inputs (gfx_shader_prewarm; a record whose shader has neither is dropped),
// enters each Pipe as building under the key a draw would compute, queues it to the low queue and builds the queue on
// every core. Records are deduplicated by a hash of their bytes; they are collected under P.lock by the worker that
// built the pipeline and appended by the present thread (vk_pipeline_autosave) or at exit (vk_pipeline_save). A file
// with another magic, version or record size is ignored and rewritten. Bump KEYS_VERSION when VkPipeState or the
// record changes meaning without changing size.
#define KEYS_VERSION 3                 // 3: shaders named by input id (gfx_shader.c)
typedef struct { char magic[8]; uint32_t version, rec_size; } KeysHeader;
typedef struct {
    uint8_t vs[32], fs[32];            // gfx_shader_input_id of each stage; fs all zero with has_fs 0
    uint32_t has_fs, pad;
    VkPipeState s;                     // vs, fs zero
} KeyRec;
static struct {
    char path[PLAT_PATH_MAX * 2];
    int header_ok;                     // the file exists with our header: append
    uint64_t *seen;                    // open addressing set of record hashes (0 empty)
    uint32_t seen_cap, seen_n;
    KeyRec *pend;                      // records not written yet
    uint32_t pend_n, pend_cap;
    uint8_t *file;                     // the file as read at boot, until the prewarm is done with it
    size_t file_n;                     // records in it
    PlatMutex io;                        // one writer
    // prebuild progress (under P.lock)
    int loading;                       // the prewarm still queues records
    int warm;                          // vk_pipeline_prewarm owns the file (prebuild_start leaves it)
    uint32_t left;                     // prebuild jobs queued or running
    uint32_t built, failed, promoted;
    double t0;                         // now_ms() when the prebuild started
} K = {.io = PLAT_MUTEX_INIT};

static uint64_t rec_hash(const KeyRec *r) {
    uint64_t h = fnv(FNV0, r, sizeof *r);
    return h ? h : 1;
}

// 1 if h was new. Caller holds P.lock.
static int seen_add(uint64_t h) {
    if ((K.seen_n + 1) * 2 > K.seen_cap) {
        uint32_t cap = K.seen_cap ? K.seen_cap * 2 : 4096;
        uint64_t *t = calloc(cap, sizeof *t);
        if (!t) return 1;
        for (uint32_t i = 0; i < K.seen_cap; i++) {
            if (!K.seen[i]) continue;
            uint32_t j = (uint32_t)K.seen[i] & (cap - 1);
            while (t[j]) j = (j + 1) & (cap - 1);
            t[j] = K.seen[i];
        }
        free(K.seen);
        K.seen = t, K.seen_cap = cap;
    }
    uint32_t j = (uint32_t)h & (K.seen_cap - 1);
    while (K.seen[j]) {
        if (K.seen[j] == h) return 0;
        j = (j + 1) & (K.seen_cap - 1);
    }
    K.seen[j] = h;
    K.seen_n++;
    return 1;
}

static void rec_fill(KeyRec *r, const VkPipeState *s) {
    memset(r, 0, sizeof *r);
    memcpy(r->vs, gfx_shader_input_id(s->vs), 32);
    if (s->fs) memcpy(r->fs, gfx_shader_input_id(s->fs), 32), r->has_fs = 1;
    r->s = *s;
    r->s.vs = r->s.fs = NULL;
    r->s.prim_restart = 0;   // dynamic state: the same pipeline either way
}

// A pipeline was built for a draw: queue its record for the file. Caller holds P.lock.
static void keys_note(const VkPipeState *s) {
    if (!A.on || !K.path[0]) return;
    KeyRec r;
    rec_fill(&r, s);
    if (!seen_add(rec_hash(&r))) return;
    if (K.pend_n == K.pend_cap) {
        uint32_t cap = K.pend_cap ? K.pend_cap * 2 : 64;
        KeyRec *t = realloc(K.pend, cap * sizeof *t);
        if (!t) return;
        K.pend = t, K.pend_cap = cap;
    }
    K.pend[K.pend_n++] = r;
}

// Appends the pending records (present thread or exit; never the render thread).
static void keys_flush(void) {
    if (!K.path[0]) return;
    plat_mutex_lock(&K.io);
    plat_mutex_lock(&P.lock);
    KeyRec *r = K.pend;
    uint32_t n = K.pend_n;
    K.pend = NULL, K.pend_n = K.pend_cap = 0;
    plat_mutex_unlock(&P.lock);
    if (n) {
        FILE *f = fopen(K.path, K.header_ok ? "ab" : "wb");
        int ok = f != NULL;
        if (ok && !K.header_ok) {
            KeysHeader h = {{'T', 'O', 'T', 'K', 'P', 'K', 'E', 'Y'}, KEYS_VERSION, sizeof(KeyRec)};
            ok = fwrite(&h, sizeof h, 1, f) == 1;
        }
        if (ok) ok = fwrite(r, sizeof *r, n, f) == n;
        if (ok) ok = plat_file_sync(f);   // appended records on the disk before the next append
        if (f) ok = !fclose(f) && ok;
        if (ok) K.header_ok = 1;
        else rt_log("vk_pipeline: cannot write %s", K.path);
    }
    free(r);
    plat_mutex_unlock(&K.io);
}

// Reads the key file into K.file and its record hashes into the set. Caller holds P.lock (ensure_init).
static void keys_open(void) {
    if (!A.on || !P.cache_path[0]) return;
    snprintf(K.path, sizeof K.path, "%s", P.cache_path);
    char *slash = strrchr(K.path, '\\'), *fslash = strrchr(K.path, '/');
    if (fslash > slash) slash = fslash;
    snprintf(slash ? slash + 1 : K.path, sizeof K.path - (size_t)(slash ? slash + 1 - K.path : 0), "pipeline-keys.bin");
    gfx_shader_record_at(K.path);   // shader-answers.bin next to it (gfx_shader.c)
    FILE *f = fopen(K.path, "rb");
    if (!f) return;
    KeysHeader h;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len >= (long)sizeof h && fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "TOTKPKEY", 8) &&
        h.version == KEYS_VERSION && h.rec_size == sizeof(KeyRec)) {
        K.header_ok = 1;
        size_t n = ((size_t)len - sizeof h) / sizeof(KeyRec);
        if (n && (K.file = malloc(n * sizeof(KeyRec))) && fread(K.file, sizeof(KeyRec), n, f) == n) K.file_n = n;
        else free(K.file), K.file = NULL;
        for (size_t i = 0; i < K.file_n; i++) seen_add(rec_hash((const KeyRec *)K.file + i));
        if (len != (long)(sizeof h + n * sizeof(KeyRec)))   // a torn append: rewrite the file from what was read
            K.header_ok = 0;
    } else {
        rt_log("vk_pipeline: %s has another format or version; it is rewritten", K.path);
    }
    fclose(f);
    if (!K.header_ok && K.file_n) {   // keep the whole records of a torn file
        KeyRec *t = malloc(K.file_n * sizeof *t);
        if (t) memcpy(t, K.file, K.file_n * sizeof *t), K.pend = t, K.pend_n = K.pend_cap = (uint32_t)K.file_n;
    }
}

static double now_ms(void) { return (double)plat_now_ns() / 1e6; }

// Caller holds P.lock.
static void prebuild_report(void) {
    rt_log("vk_pipeline: prebuild done: %u built, %u failed, %u asked for by a draw first; %.0f ms after the device was "
           "ready", K.built, K.failed, K.promoted, now_ms() - K.t0);
}

static void pipe_worker(void *arg) {
    int me = (int)(intptr_t)arg;
    plat_mutex_lock(&P.lock);
    for (;;) {
        // With 3 or more workers one never takes low-queue work, so a waiting draw's library starts at once
        // (not with TOTK_PIPE_WAIT=draw).
        while (!U.head && !A.head && !(A.low && (A.threads < 3 || !U.on || A.low_running < A.threads - 1))) {
            U.idle++;
            plat_cond_wait(&work_cv, &P.lock);
            U.idle--;
        }
        if (U.head) {   // a library a draw waits for
            LibJob *lj = U.head;
            if (!(U.head = lj->next)) U.tail = NULL;
            lj->b->job = NULL;
            if (A.prio_low[me]) A.prio_low[me] = 0, plat_thread_priority(PLAT_PRIO_NORMAL);
            U.by_worker++, U.running++;
            lib_build(lj->b, lj->kind, &lj->s, &lj->k, lj->l);   // releases P.lock while building
            U.running--;
            free(lj);
            continue;
        }
        Job *j = A.head;
        int lowrun = !j;
        if (j) {
            if (!(A.head = j->next)) A.tail = NULL;
        } else {
            j = A.low;
            if ((A.low = j->next)) A.low->prev = NULL;
            else A.low_tail = NULL;
            A.low_running++;
        }
        j->next = j->prev = NULL;
        j->worker = me;
        if (j->low != A.prio_low[me]) {   // under P.lock, so a draw raising this thread (promote) is not undone
            A.prio_low[me] = j->low;
            plat_thread_priority(j->low ? PLAT_PRIO_BELOW_NORMAL : PLAT_PRIO_NORMAL);
        }
        // Step 1 for a job a draw waits for: its stage libraries and the fast link. Step 2, the optimised build, goes to
        // the head of the low queue, so a burst of new pipelines gets every fast link before any optimised build.
        if (P.gpl && (!j->prebuild || j->demanded) && !j->p->info.pipeline && !j->p->fast_tried && j->p->lay) {
            int final;
            VkPipeline f = gpl_link(&j->s, &j->k, j->p->lay, 1, &final);   // releases P.lock while building
            j->p->fast_tried = 1;
            if (f && !j->p->info.pipeline) {
                j->p->fast = j->p->info.pipeline = f, P.g.wlinks++;
                plat_cond_broadcast(&done_cv);
                j->low = 1, j->worker = -1, P.g.requeued++;
                j->prev = NULL, j->next = A.low;
                if (A.low) A.low->prev = j;
                else A.low_tail = j;
                A.low = j;
                A.low_running -= lowrun;
                continue;
            } else if (f) {
                vkDestroyPipeline(P.device, f, NULL);
            }
        }
        plat_mutex_unlock(&P.lock);
        VkPipeline pipe = create_graphics(&j->s, &j->k, j->l, j->fs_off);
        plat_mutex_lock(&P.lock);
        if (pipe || !j->p->fast) j->p->info.pipeline = pipe;   // a failed optimised build keeps the fast-linked one
        if (pipe && j->p->fast) P.g.swaps++;
        j->p->building = 0;
        j->p->job = NULL;
        if (pipe) {
            P.stats.graphics++, P.stats.async_built++;
            if (!j->prebuild) keys_note(&j->s);
        } else {
            P.stats.failures++;
            rt_log("vk_pipeline: graphics pipeline not built: pipeline creation failed");
        }
        if (j->prebuild) {
            if (pipe) K.built++;
            else K.failed++;
            K.promoted += j->demanded;
            if (!--K.left && !K.loading) prebuild_report();
        }
        A.low_running -= lowrun;
        if (!--A.inflight) plat_cond_broadcast(&A.idle);
        plat_cond_broadcast(&done_cv);
        if (pipe && (!j->prebuild || j->demanded)) perf_count(PERF_CNT_PIPE_ASYNC);   // a draw waited for it
        free(j);
    }
    return;
}

// Starts the pool on first use: a quarter of the logical processors, 2 to 4. Caller holds P.lock.
static int workers_start(void) {
    if (A.threads) return 1;
    uint32_t cpus = plat_cpu_usable();
    int n = cpus / 4 < 2 ? 2 : cpus / 4 > MAX_WORKERS ? MAX_WORKERS : (int)(cpus / 4);
    for (int i = 0; i < n; i++) {
        PlatThread *t = plat_thread_new(pipe_worker, (void *)(intptr_t)A.threads, "pipe worker");
        if (!t) break;
        A.thread[A.threads++] = t;   // kept: promote() raises a worker running a prebuild job
    }
    if (A.threads) rt_log("vk_pipeline: %d background graphics pipeline workers", A.threads);
    else rt_log("vk_pipeline: cannot start pipeline workers; building synchronously");
    return A.threads != 0;
}

static void workers_drain(void) {
    plat_mutex_lock(&P.lock);
    K.loading = 0;
    for (Job *j = A.low, *n; j; j = n) {   // queued prebuild jobs are dropped, not built
        n = j->next;
        j->p->building = 0, j->p->job = NULL;
        A.inflight--, K.left--;
        free(j);
    }
    A.low = A.low_tail = NULL;
    for (LibJob *j = U.head, *n; j; j = n) {   // queued library jobs too: their entries read as failed builds
        n = j->next;
        j->b->job = NULL, j->b->building = 0;
        free(j);
    }
    U.head = U.tail = NULL;
    while (A.inflight) plat_cond_wait(&A.idle, &P.lock);
    while (U.running) plat_cond_wait(&done_cv, &P.lock);
    plat_mutex_unlock(&P.lock);
}

void vk_pipeline_async_enable(void) {
    U.on = strcmp(cfg_value("TOTK_PIPE_WAIT"), "draw") != 0;
    const char *e = cfg_get("TOTK_PIPE_SYNC");
    if (e && *e && strcmp(e, "0")) return;
    A.on = 1;
}

int vk_pipeline_async_on(void) { return A.on; }

// A draw needs a pipeline a prebuild job owns: the job moves to the draw queue, or its worker to normal priority.
// Caller holds P.lock.
static void promote(Pipe *p) {
    Job *j = p->job;
    if (!j || !j->low) return;
    j->low = 0, j->demanded = 1;
    if (j->worker < 0) {
        if (j->prev) j->prev->next = j->next;
        else A.low = j->next;
        if (j->next) j->next->prev = j->prev;
        else A.low_tail = j->prev;
        j->next = j->prev = NULL;
        if (A.tail) A.tail->next = j;
        else A.head = j;
        A.tail = j;
        plat_cond_signal(&work_cv);
    } else if (A.prio_low[j->worker]) {
        A.prio_low[j->worker] = 0;
        plat_thread_set_priority(A.thread[j->worker], PLAT_PRIO_NORMAL);
    }
}

// Enters a missing pipeline: built here (mode 0), or queued for a draw (1) or for the prebuild (2). Failures are
// entered too. *queued: 1 when a job owns it. Caller holds P.lock.
static Pipe *pipe_miss(const VkPipeState *s, const GKey *k, uint64_t h, int mode, int *queued) {
    *queued = 0;
    const char *why = NULL;
    Bindings B = {0};
    Layout *l = NULL;
    uint32_t span = binding_span(s->vs);
    if (gfx_shader_stage(s->vs) != GFX_STAGE_VS || (s->fs && gfx_shader_stage(s->fs) != GFX_STAGE_FS)) why = "stage mismatch";
    else if (k->topo_class == 3) why = "patches need tessellation stages, which are not translated";
    else if (P.indep) {
        Bindings F = {0};
        if (!add_stage(&B, s->vs, 0) || (s->fs && !add_stage(&F, s->fs, 0))) why = "too many bindings";
        else if (!(l = layout_pair(&B, &F))) why = "descriptor layout creation failed";
        span = 1;
    } else if (!add_stage(&B, s->vs, 0) || (s->fs && !add_stage(&B, s->fs, span - 1))) why = "too many bindings";
    else if (!(l = layout_get(&B))) why = "descriptor layout creation failed";
    char lack[128];
    if (!why && !layout_covers(s, l, span - 1, lack, sizeof lack)) why = lack, l = NULL;
    VkPipeline pipe = VK_NULL_HANDLE;
    Job *j = NULL;
    if (!why && mode && workers_start() && (j = calloc(1, sizeof *j))) {
        *j = (Job){.l = l, .fs_off = span - 1, .low = mode == 2, .prebuild = mode == 2, .worker = -1, .s = *s, .k = *k};
    } else if (!why && !(pipe = create_graphics(s, k, l, span - 1))) {
        why = "pipeline creation failed";
    }
    if (why) {
        P.stats.failures++;
        rt_log("vk_pipeline: graphics pipeline not built: %s", why);
    } else if (!j) {
        P.stats.graphics++;
        if (mode != 2) keys_note(s);
    }
    Pipe *p = pipe_add(k, sizeof *k, h, pipe, l, l ? &B : NULL, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_shader_inputs(s->vs, NULL));
    if (!j || !p) {
        free(j);
        return p;
    }
    j->p = p;
    p->building = 1, p->job = j;
    if (mode == 2) {
        j->prev = A.low_tail;
        if (A.low_tail) A.low_tail->next = j;
        else A.low = j;
        A.low_tail = j;
        K.left++;
    } else {
        if (A.tail) A.tail->next = j;
        else A.head = j;
        A.tail = j;
    }
    A.inflight++;
    plat_cond_signal(&work_cv);
    *queued = 1;
    return p;
}

// A job still in the draw queue goes to the head of the low queue: its pipeline has (or is about to have) a fast-linked
// one, so the optimised build waits behind other draws' misses. Caller holds P.lock.
static void job_lower(Job *j) {
    if (!j || j->worker >= 0 || j->low) return;
    if (A.head == j) {
        if (!(A.head = j->next)) A.tail = NULL;
    } else {
        Job *q = A.head;
        while (q && q->next != j) q = q->next;
        if (!q) return;
        if (!(q->next = j->next)) A.tail = q;
    }
    j->low = 1, P.g.requeued++;
    j->prev = NULL, j->next = A.low;
    if (A.low) A.low->prev = j;
    else A.low_tail = j;
    A.low = j;
}

// A draw waits for pipeline s: its missing pre-rasterization and fragment shader libraries go to the head of the
// worker queues (U), so they build in parallel on idle workers, and this thread sleeps until both are published or
// end (now_ms) passes. A job no worker took within 1 ms (all busy) is built here. Caller holds P.lock.
static void lib_wait(const VkPipeState *s, const GKey *k, Layout *l, double end) {
    ViKey vk_;
    PrKey pk;
    FsKey fk;
    FoKey ok;
    lib_keys(s, k, &vk_, &pk, &fk, &ok);
    U.waits++;
    const void *key[2] = {&pk, &fk};
    uint32_t size[2] = {sizeof pk, sizeof fk};
    Lib *b[2];
    int queued = 0;
    for (int i = 0; i < 2; i++) {
        uint64_t h = key_hash(key[i], size[i]);
        if ((b[i] = lib_find(h, key[i], size[i]))) continue;
        if (!half_layout(l, i)) return;   // gpl_link reports it
        LibJob *j = calloc(1, sizeof *j);
        if (!j || !(b[i] = lib_new(h, key[i], size[i]))) {
            free(j);
            return;
        }
        *j = (LibJob){.b = b[i], .kind = i ? LIB_FS : LIB_PR, .l = l, .s = *s, .k = *k};
        b[i]->job = j;
        if (U.tail) U.tail->next = j;
        else U.head = j;
        U.tail = j;
        U.queued++, queued++;
    }
    if (queued) plat_cond_broadcast(&work_cv);
    double steal_at = now_ms() + 1.0;
    for (;;) {
        if (!b[0]->building && !b[1]->building) return;
        double now = now_ms();
        if (now >= end) {
            U.timeouts++;
            return;
        }
        Lib *q = b[0]->job ? b[0] : b[1]->job ? b[1] : NULL;
        if (q && (now >= steal_at || !U.idle)) {
            U.stolen++;
            lib_steal(q);   // releases P.lock while building
            continue;
        }
        double until = q && steal_at < end ? steal_at : end;
        plat_cond_wait_ms(&done_cv, &P.lock, (uint32_t)(until - now) + 1);
    }
}

static VkPipeline graphics(const VkPipeState *s, const VkPipeInfo **info, int async, int *pending, uint32_t wait_ms) {
    if (info) *info = NULL;
    if (pending) *pending = 0;
    if (!s->vs || !ensure_init()) return VK_NULL_HANDLE;
    GKey k;
    gkey_fill(s, &k);
    uint64_t h = key_hash(&k, sizeof k);
    plat_mutex_lock(&P.lock);
    Pipe *p = pipe_find(&k, sizeof k, h);
    int queued;
    if (p) {
        if (!p->building) P.stats.hits++;
    } else {
        p = pipe_miss(s, &k, h, async, &queued);
    }
    // A draw that would wait (wait_ms) gets its missing shader libraries built at once, the vertex and fragment ones in
    // parallel on idle workers (lib_wait; about 0.12 ms per library from the driver's disk cache, 5-180 ms cold), and
    // links them here: its own job may queue behind optimised builds of earlier pipelines. The job then only does the
    // optimised build, from the low queue. Other draws leave the libraries to the job and are skipped meanwhile.
    // TOTK_PIPE_WAIT=draw builds them on this thread one after the other instead, as before; TOTK_GPL_DRAW_BUILD=0
    // leaves them to the job.
    static int draw_build = -1;
    if (draw_build < 0) {
        const char *e = cfg_get("TOTK_GPL_DRAW_BUILD");
        draw_build = !(e && !strcmp(e, "0"));
        rt_log("vk_pipeline: a draw that waits for its pipeline %s", !draw_build ? "leaves its libraries to its job"
               : U.on ? "has its libraries built in parallel on the workers, ahead of other jobs"
                      : "builds its libraries itself (TOTK_PIPE_WAIT=draw)");
    }
    double end = wait_ms ? now_ms() + wait_ms : 0;
    if (p && p->building && P.gpl && !p->info.pipeline && !p->fast_tried && p->lay) {
        int final = 0, build = wait_ms != 0 && draw_build;
        uint64_t p0 = perf_begin(), t0 = qpc();
        if (build && U.on) {
            job_lower(p->job);   // its worker would only wait for the same libraries; it links them if this draw cannot
            lib_wait(s, &k, (Layout *)p->lay, end);   // releases P.lock while waiting
            build = 0;                                // links here when both libraries exist (gpl_link build 0)
        } else if (build) {
            p->fast_tried = 1;   // the job, if a worker takes it meanwhile, goes straight to the optimised build
        }
        VkPipeline f = p->info.pipeline ? VK_NULL_HANDLE : gpl_link(s, &k, p->lay, build, &final);   // build 1 releases P.lock
        perf_end(PERF_PIPE, p0);
        if (wait_ms && draw_build) {
            double ms = ms_since(t0);
            P.g.dlinks++, P.g.dlink_ms += ms;
            if (ms > P.g.dlink_max) P.g.dlink_max = ms;
            if (ms > 5.0 && P.g.slow++ < 16)
                rt_log("vk_pipeline: gpl: a waiting draw %s its libraries in %.1f ms", U.on ? "got" : "built", ms);
        }
        if (final) p->fast_tried = 1;
        if (f && !p->info.pipeline) {
            p->fast = p->info.pipeline = f;
            job_lower(p->job);
            plat_cond_broadcast(&done_cv);
        } else if (f) {
            vkDestroyPipeline(P.device, f, NULL);   // a worker published one while the lock was free
        }
    }
    if (p && p->building && !p->info.pipeline) {
        promote(p);
        uint64_t p0 = wait_ms ? perf_begin() : 0;   // the wait counts as the render thread's pipeline time
        double now;
        while (p->building && !p->info.pipeline && wait_ms && (now = now_ms()) < end)
            plat_cond_wait_ms(&done_cv, &P.lock, (uint32_t)(end - now) + 1);
        if (wait_ms) perf_end(PERF_PIPE, p0);
        if (wait_ms && !p->building) perf_count(PERF_CNT_PIPE_WAIT);
    }
    int wait = p && p->building && !p->info.pipeline;
    if (wait) P.stats.pending_skips++;
    VkPipeline pipe = p ? p->info.pipeline : VK_NULL_HANDLE;
    plat_mutex_unlock(&P.lock);
    if (wait) {
        perf_count(PERF_CNT_PIPE_SKIP);
        if (pending) *pending = 1;
        return VK_NULL_HANDLE;
    }
    if (!p) return VK_NULL_HANDLE;
    if (info) *info = &p->info;
    return pipe;
}

VkPipeline vk_pipeline_graphics(const VkPipeState *s, const VkPipeInfo **info) {
    return graphics(s, info, 0, NULL, 0);
}

VkPipeline vk_pipeline_graphics_async(const VkPipeState *s, const VkPipeInfo **info, int *pending, uint32_t wait_ms) {
    return graphics(s, info, A.on, pending, wait_ms);
}

// ---- boot prewarm from the key file (comment at "pipeline keys")

static int id_cmp(const void *x, const void *y) { return memcmp(x, y, 32); }

// Builds queued prebuild jobs until the low queue is empty, beside the pipe_worker pool (same bookkeeping).
static void warm_builder(void *arg) {
    (void)arg;
    plat_mutex_lock(&P.lock);
    while (A.low) {
        Job *j = A.low;
        if ((A.low = j->next)) A.low->prev = NULL;
        else A.low_tail = NULL;
        j->next = j->prev = NULL;
        j->low = 0;                    // not on a pool thread: promote() has nothing to raise
        plat_mutex_unlock(&P.lock);
        VkPipeline pipe = create_graphics(&j->s, &j->k, j->l, j->fs_off);
        plat_mutex_lock(&P.lock);
        if (pipe || !j->p->fast) j->p->info.pipeline = pipe;   // a failed optimised build keeps the fast-linked one
        if (pipe && j->p->fast) P.g.swaps++;
        j->p->building = 0;
        j->p->job = NULL;
        if (pipe) {
            P.stats.graphics++, P.stats.async_built++, K.built++;
        } else {
            P.stats.failures++, K.failed++;
            rt_log("vk_pipeline: graphics pipeline not built: pipeline creation failed");
        }
        K.promoted += j->demanded;
        K.left--;
        if (!--A.inflight) plat_cond_broadcast(&A.idle);
        plat_cond_broadcast(&done_cv);
        free(j);
    }
    plat_mutex_unlock(&P.lock);
    return;
}

static void lb_start(const uint32_t pr_hist[16], const uint32_t fs_hist[4]);

void vk_pipeline_prewarm(void) {
    const char *gpu = rt_config()->gpu;
    uint32_t pr_hist[16] = {0}, fs_hist[4] = {0};   // library state of the recorded pipelines (lb_start)
    if (!A.on || (gpu && !strcmp(gpu, "null"))) return;
    if (!cfg_bool("TOTK_PIPE_PREBUILD")) {   // prebuild_start logs the off case
        if (ensure_init()) lb_start(pr_hist, fs_hist);
        return;
    }
    double t0 = now_ms();
    K.warm = 1;
    if (!ensure_init() || !K.file_n) {
        rt_log("vk_pipeline: prewarm: no pipeline keys (%s)", K.path[0] ? K.path : "<none>");
        if (P.ready) lb_start(pr_hist, fs_hist);
        return;
    }
    const KeyRec *r = (const KeyRec *)K.file;
    size_t n = K.file_n, m = 0;
    uint8_t (*ids)[32] = malloc(2 * n * 32);
    const GfxShader **sh = calloc(2 * n, sizeof *sh);
    if (!ids || !sh) {
        free(ids), free(sh);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        memcpy(ids[m++], r[i].vs, 32);
        if (r[i].has_fs) memcpy(ids[m++], r[i].fs, 32);
    }
    qsort(ids, m, 32, id_cmp);
    size_t u = 0;
    for (size_t i = 0; i < m; i++)
        if (!u || memcmp(ids[u - 1], ids[i], 32)) memcpy(ids[u++], ids[i], 32);
    int cpus = (int)plat_cpu_usable();
    const char *pe = cfg_get("TOTK_PREWARM_XLATE");   // no fixed default: half the CPUs, at most 16, when unset
    int procs = pe && *pe ? atoi(pe) : cpus / 2 > 16 ? 16 : cpus / 2;
    gfx_shader_prewarm((const uint8_t(*)[32])ids, u, sh, cpus, procs < 1 ? 1 : procs);
    double t1 = now_ms();

    if (P.indep) {   // vertex output unions first: workers may take a job before the queue is complete
        uint64_t pair = 0, uni = 0, pairs = 0;
        for (size_t i = 0; i < n; i++) {
            const uint8_t (*v)[32] = bsearch(r[i].vs, ids, u, 32, id_cmp);
            const uint8_t (*f)[32] = r[i].has_fs ? bsearch(r[i].fs, ids, u, 32, id_cmp) : NULL;
            if (!v || !f || !sh[v - ids] || !sh[f - ids]) continue;
            GfxShaderIo io[32];
            vs_outs(sh[v - ids], sh[f - ids], io);
            pair += (uint64_t)__builtin_popcount(gfx_shader_inputs(sh[f - ids], NULL)), pairs++;
        }
        for (uint32_t j = 0; j < VS_OUTS_CAP; j++) uni += VO.vs[j] ? (uint64_t)__builtin_popcount(VO.mask[j]) : 0;
        rt_log("vk_pipeline: vertex output unions: %u vertex shaders, %.2f locations each; %llu pairs read %.2f each",
               VO.n, VO.n ? (double)uni / VO.n : 0.0, (unsigned long long)pairs, pairs ? (double)pair / pairs : 0.0);
    }
    uint32_t queued = 0, dropped = 0, known = 0;
    plat_mutex_lock(&P.lock);
    K.t0 = t1;
    K.loading = 1;
    for (size_t i = 0; i < n; i++) {
        const uint8_t (*v)[32] = bsearch(r[i].vs, ids, u, 32, id_cmp);
        const uint8_t (*f)[32] = r[i].has_fs ? bsearch(r[i].fs, ids, u, 32, id_cmp) : NULL;
        const GfxShader *vs = v ? sh[v - ids] : NULL, *fs = f ? sh[f - ids] : NULL;
        if (!vs || (r[i].has_fs && !fs)) {
            dropped++;
            continue;
        }
        VkPipeState s = r[i].s;
        s.vs = vs, s.fs = fs;
        GKey k;
        gkey_fill(&s, &k);
        pr_hist[(k.clamp & 1) | (k.poly_mode & 3) << 1 | (k.minus_w & 1) << 3]++;
        fs_hist[k.samples >= 8 ? 3 : k.samples >= 4 ? 2 : k.samples >= 2 ? 1 : 0]++;
        uint64_t h = key_hash(&k, sizeof k);
        int q = 0;
        if (pipe_find(&k, sizeof k, h)) known++;
        else pipe_miss(&s, &k, h, 2, &q), queued += (uint32_t)q;
    }
    K.loading = 0;
    plat_mutex_unlock(&P.lock);
    free(ids), free(sh);

    PlatThread *t[64];
    int nt = 0;
    for (int i = 0; i < cpus && i < 64 && queued; i++)
        if ((t[nt] = plat_thread_new(warm_builder, NULL, "pipe prewarm"))) nt++;
    if (!nt) warm_builder(NULL);
    for (int i = 0; i < nt; i++)
        while (!plat_thread_join_ns(t[i], 2000000000u))
            rt_log("vk_pipeline: prewarm: %u of %u pipelines, %.0f ms", K.built + K.failed, queued, now_ms() - t1);
    plat_mutex_lock(&P.lock);
    while (K.left) plat_cond_wait(&done_cv, &P.lock);   // jobs a pool worker took
    rt_log("vk_pipeline: prewarm: %zu records in %s: %u pipelines built, %u failed, %u already built, %u dropped "
           "(shader neither cached nor stored); shaders %.0f ms, pipelines %.0f ms on %d threads, total %.0f ms",
           n, K.path, K.built, K.failed, known, dropped, t1 - t0, now_ms() - t1, nt, now_ms() - t0);
    free(K.file);
    K.file = NULL;
    plat_mutex_unlock(&P.lock);
    lb_start(pr_hist, fs_hist);
}

// ensure_init's hook: without a prewarm (TOTK_PIPE_PREBUILD=0, or the device came up first elsewhere) the records
// are not built. Caller holds no lock.
static void prebuild_start(void) {
    plat_mutex_lock(&P.lock);
    if (K.file && !K.warm) {
        rt_log("vk_pipeline: prebuild off%s; %zu records not built",
               !cfg_bool("TOTK_PIPE_PREBUILD") ? " (TOTK_PIPE_PREBUILD=0)" : " (no boot prewarm)", K.file_n);
        free(K.file);
        K.file = NULL;
    }
    plat_mutex_unlock(&P.lock);
}

// A compute pipeline for cs in layout l; VK_NULL_HANDLE on failure. No lock needed (P.cache is driver-synchronised).
static VkPipeline compute_create(const GfxShader *cs, const Layout *l) {
    uint64_t words;
    const uint32_t *code = gfx_shader_spirv(cs, &words);
    VkShaderModule m = make_module(code, words);
    if (!m) return VK_NULL_HANDLE;
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rqc;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = l->layout};
    pin_subgroup32(&ci.stage, &rqc, code, words);
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(P.device, P.cache, 1, &ci, NULL, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
    vkDestroyShaderModule(P.device, m, NULL);
    return pipe;
}

// The layout of a compute shader in *B; NULL with *why on failure. Caller holds P.lock.
static Layout *compute_layout(const GfxShader *cs, Bindings *B, const char **why) {
    Layout *l = NULL;
    if (gfx_shader_stage(cs) != GFX_STAGE_CS) *why = "not a compute shader";
    else if (!add_stage(B, cs, 0)) *why = "too many bindings";
    else if (!(l = layout_get(B))) *why = "descriptor layout creation failed";
    return l;
}

// ---- stage library prebuild
// Two places build what a draw would otherwise build while the game waits:
// - totk.exe --prepare-shaders (lb_warm_prepared), after the translation, on every core: for each prepared variant
//   (shader-prepared.bin) the pre-rasterization library of a vertex shader, the fragment-shader library of a fragment
//   shader and the pipeline of a compute shader, each built from a private shader copy and destroyed at once. What
//   stays is the driver's own disk cache: on AMD a library built again in a later process takes 0.1 ms instead of
//   55 ms (vertex) / 28 ms (fragment). Our pipeline-cache.bin is not written there:
//   it would grow by about 10 KB per library (~240 MB) and be read into memory at every boot.
// - boot (lb_worker), after the pipeline-key prewarm, on cpus/4 threads at PLAT_PRIO_IDLE (the short P.lock
//   sections and shader loads at normal priority, so a preempted thread never holds the lock the replay needs):
//   compute pipelines of every recorded and prepared compute shader, kept (vk_pipeline_compute builds compute
//   synchronously on the replay thread; 163 prepared), and the libraries of the first TOTK_LIB_PREBUILD_KEEP variants
//   play recorded (priority 0), kept in libs[] with their shaders. KEEP defaults to 0: a kept library costs 140-460 KB
//   of process memory (10,000 kept took 9 GB in run 1) to save a rebuild the driver cache makes 0.1 ms.
// Library state: the vertex output union known now (vs_union_peek; the prewarm fills it from pipeline-keys.bin, the
// preparation has none), the clamp, polygon mode and depth mode most recorded pipelines use (defaults without a key
// file: clamp 0, fill, -w..w), single-sampled, no dual source; the state dynamic on this GPU is 0 in every key.
// A draw that meets such a variant in a new pipeline then gets its libraries from libs[] or the driver cache and
// fast-links (gpl_link). TOTK_LIB_PREBUILD=0 turns the boot part off, =N stops it after N variants;
// TOTK_LIB_PREBUILD_THREADS sets its thread count.
#define LB_MAX_THREADS 16

static struct {
    GfxWarmEnt *e;
    size_t n;
    volatile int64_t next;
    volatile int32_t stop, running;
    int threads;
    PlatThread *th[LB_MAX_THREADS];
    uint8_t clamp, poly, minus_w, samples;             // library state (GKey fields)
    double t0, ms[3];                                  // ms: build time per kind (vertex, fragment, compute)
    // warmed: driver cache only; nolib: vertex and fragment variants skipped with the graphics pipeline library off
    uint64_t built[3], have[3], fail[3], warmed[3], noshader, nolib, done, cs_draw;
    double warm_ms[3];
    volatile int32_t kept;                                // libraries entered in libs[] so far
    int32_t keep_n;
    int idle;                                          // build at PLAT_PRIO_IDLE (boot), normal (preparation)
    PlatMutex stat;
} LB = {.stat = PLAT_MUTEX_INIT};

VkPipeline vk_pipeline_compute(const GfxShader *cs, const VkPipeInfo **info) {
    if (info) *info = NULL;
    if (!cs || !ensure_init()) return VK_NULL_HANDLE;
    struct { const GfxShader *cs; uint32_t compute; } k = {cs, 1};
    uint64_t h = key_hash(&k, sizeof k);
    plat_mutex_lock(&P.lock);
    Pipe *p = pipe_find(&k, sizeof k, h);
    if (p) {
        P.stats.hits++;
    } else {
        const char *why = NULL;
        Bindings B = {0};
        Layout *l = compute_layout(cs, &B, &why);
        VkPipeline pipe = VK_NULL_HANDLE;
        if (!why) {
            uint64_t p0 = perf_begin();
            pipe = compute_create(cs, l);
            perf_end(PERF_PIPE, p0);
            if (!pipe) why = "pipeline creation failed";
        }
        if (why) {
            P.stats.failures++;
            rt_log("vk_pipeline: compute pipeline not built: %s", why);
        } else {
            P.stats.compute++;
            LB.cs_draw++;
        }
        p = pipe_add(&k, sizeof k, h, pipe, l, l ? &B : NULL, VK_PIPELINE_BIND_POINT_COMPUTE, 0);
    }
    plat_mutex_unlock(&P.lock);
    if (!p) return VK_NULL_HANDLE;
    if (info) *info = &p->info;
    return p->info.pipeline;
}

static void lb_lock(void) {
    plat_thread_priority(PLAT_PRIO_NORMAL);
    plat_mutex_lock(&P.lock);
}

static void lb_unlock(void) {
    plat_mutex_unlock(&P.lock);
    if (LB.idle) plat_thread_priority(PLAT_PRIO_IDLE);
}

static void lb_count(int kind, int what, double ms) {   // what: 0 built, 1 already there, 2 failed, 3 cache only
    plat_mutex_lock(&LB.stat);
    if (what == 3) LB.warmed[kind]++, LB.warm_ms[kind] += ms;
    else (what == 0 ? LB.built : what == 1 ? LB.have : LB.fail)[kind]++, LB.ms[kind] += ms;
    plat_mutex_unlock(&LB.stat);
}

// vs_union_missing for vs alone, without entering vs into the union table (it holds VS_OUTS_CAP shaders).
static uint32_t vs_union_peek(const GfxShader *vs, GfxShaderIo fin[32]) {
    GfxShaderIo vout[32];
    uint32_t need = 0, have = gfx_shader_outputs(vs, vout), missing = 0;
    memset(fin, 0, 32 * sizeof *fin);
    plat_mutex_lock_shared(&VO.lock);
    uint32_t j = (uint32_t)(((uintptr_t)vs >> 4) * 0x9e3779b1u) & (VS_OUTS_CAP - 1);
    while (VO.vs[j] && VO.vs[j] != vs) j = (j + 1) & (VS_OUTS_CAP - 1);
    if (VO.vs[j]) need = VO.mask[j], memcpy(fin, VO.io[j], 32 * sizeof *fin);
    plat_mutex_unlock_shared(&VO.lock);
    if (P.vs_outs_all)
        for (int i = 0; i < 32; i++)
            if (!(need >> i & 1)) fin[i] = (GfxShaderIo){GFX_TYPE_FLOAT, 4, 0}, need |= 1u << i;
    for (int i = 0; i < 32; i++)
        if (need >> i & 1 && !(have >> i & 1)) missing |= 1u << i;
    return missing;
}

static void lb_compute(const GfxShader *cs) {
    struct { const GfxShader *cs; uint32_t compute; } k = {cs, 1};
    uint64_t h = key_hash(&k, sizeof k);
    Bindings B = {0};
    const char *why = NULL;
    lb_lock();
    Layout *l = pipe_find(&k, sizeof k, h) ? NULL : compute_layout(cs, &B, &why);
    int have = !l && !why;
    lb_unlock();
    if (!l) {
        lb_count(2, have ? 1 : 2, 0);
        return;
    }
    double t0 = now_ms();
    VkPipeline pipe = compute_create(cs, l);
    double ms = now_ms() - t0;
    lb_lock();
    if (pipe && !pipe_find(&k, sizeof k, h)) {
        pipe_add(&k, sizeof k, h, pipe, l, &B, VK_PIPELINE_BIND_POINT_COMPUTE, 0);
        P.stats.compute++;
    } else if (pipe) {
        vkDestroyPipeline(P.device, pipe, NULL);
        have = 1;
    }
    lb_unlock();
    lb_count(2, pipe ? have : 2, ms);
}

// The pre-rasterization (vertex) or fragment-shader library of one shader with the LB state.
// keep 0: build into the driver's pipeline cache only (sh is a private copy; the library is destroyed).
static void lb_library(const GfxShader *sh, int vertex, int keep) {
    int kind = vertex ? LIB_PR : LIB_FS, stat = vertex ? 0 : 1;
    VkPipeState s;
    vk_pipe_state_defaults(&s);
    GKey k;
    memset(&k, 0, sizeof k);
    k.clamp = LB.clamp, k.poly_mode = LB.poly, k.minus_w = LB.minus_w, k.samples = LB.samples;
    PrKey pk;
    FsKey fk;
    memset(&pk, 0, sizeof pk), memset(&fk, 0, sizeof fk);
    const void *key;
    uint32_t size;
    if (vertex) {
        GfxShaderIo fin[32];
        s.vs = sh;
        pk.kind = LIB_PR, pk.vs = sh, pk.clamp = k.clamp, pk.poly_mode = k.poly_mode, pk.minus_w = k.minus_w;
        pk.missing = vs_union_peek(sh, fin);
        for (int i = 0; i < 32; i++)
            if (pk.missing >> i & 1) pk.fin[i] = fin[i];
        key = &pk, size = sizeof pk;
    } else {
        s.fs = sh;
        fk.kind = LIB_FS, fk.fs = sh, fk.samples = k.samples;
        key = &fk, size = sizeof fk;
    }
    uint64_t h = key_hash(key, size);
    Bindings B = {0}, F = {0};
    VkPipelineLayout lay = VK_NULL_HANDLE;
    lb_lock();
    int have = keep && lib_find(h, key, size) != NULL;
    if (!have && add_stage(vertex ? &B : &F, sh, 0)) {
        Layout *l = layout_pair(&B, &F);
        if (l) lay = half_layout(l, vertex ? 0 : 1);
    }
    lb_unlock();
    if (have || !lay) {
        lb_count(stat, have ? 1 : 2, 0);
        return;
    }
    double t0 = now_ms();
    VkPipeline pipe = lib_create(kind, &s, &k, lay, key);
    double ms = now_ms() - t0;
    if (!keep) {
        if (pipe) vkDestroyPipeline(P.device, pipe, NULL);
        lb_count(stat, pipe ? 3 : 2, ms);
        return;
    }
    lb_lock();
    have = lib_find(h, key, size) != NULL;
    VkPipeline got = lib_put(h, key, size, pipe);
    if (!have && pipe) {
        Lib *b = lib_find(h, key, size);
        if (b) b->pre = 1;
    }
    lb_unlock();
    lb_count(stat, !pipe ? 2 : have || got != pipe ? 1 : 0, ms);
}

// A vertex or fragment variant with the graphics pipeline library off: no stage library to build ahead.
static void lb_skip_lib(void) {
    plat_mutex_lock(&LB.stat);
    LB.nolib++;
    plat_mutex_unlock(&LB.stat);
}

static void lb_report(const char *how) {
    float mem_used, mem_total, mem_proc;
    plat_mem_info(&mem_used, &mem_total, &mem_proc);
    plat_mutex_lock_shared(&LB.stat);
    rt_log("vk_pipeline: stage library prebuild %s: %llu of %zu variants in %.0f ms; kept: vertex %llu built (avg %.1f ms), "
           "%llu there; fragment %llu built (avg %.1f ms), %llu there; compute %llu built (avg %.1f ms), %llu there; driver "
           "cache only: vertex %llu (avg %.1f ms), fragment %llu (avg %.1f ms), compute %llu (avg %.1f ms); %llu failed, %llu not in the shader cache, "
           "%llu vertex/fragment skipped (graphics pipeline library off); process working set %.0f MB", how, (unsigned long long)LB.done, LB.n,
           now_ms() - LB.t0, (unsigned long long)LB.built[0], LB.built[0] ? LB.ms[0] / LB.built[0] : 0.0,
           (unsigned long long)LB.have[0], (unsigned long long)LB.built[1], LB.built[1] ? LB.ms[1] / LB.built[1] : 0.0,
           (unsigned long long)LB.have[1], (unsigned long long)LB.built[2], LB.built[2] ? LB.ms[2] / LB.built[2] : 0.0,
           (unsigned long long)LB.have[2], (unsigned long long)LB.warmed[0], LB.warmed[0] ? LB.warm_ms[0] / LB.warmed[0] : 0.0,
           (unsigned long long)LB.warmed[1], LB.warmed[1] ? LB.warm_ms[1] / LB.warmed[1] : 0.0,
           (unsigned long long)LB.warmed[2], LB.warmed[2] ? LB.warm_ms[2] / LB.warmed[2] : 0.0,
           (unsigned long long)(LB.fail[0] + LB.fail[1] + LB.fail[2]),
           (unsigned long long)LB.noshader, (unsigned long long)LB.nolib, (double)mem_proc);
    plat_mutex_unlock_shared(&LB.stat);
}

// Boot: compute pipelines and kept libraries, all entered for the process.
static void lb_worker(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_IDLE);
    for (;;) {
        size_t i = (size_t)plat_inc64(&LB.next) - 1;
        if (i >= LB.n || LB.stop) break;
        plat_thread_priority(PLAT_PRIO_NORMAL);   // gfx_shader's cache lock, disk reads
        const GfxShader *sh = gfx_shader_load_input(LB.e[i].iid, 0);
        plat_thread_priority(PLAT_PRIO_IDLE);
        if (!sh) {
            plat_mutex_lock(&LB.stat);
            LB.noshader++;
            plat_mutex_unlock(&LB.stat);
        } else if (gfx_shader_stage(sh) == GFX_STAGE_CS) {
            lb_compute(sh);
        } else if (gfx_shader_stage(sh) == GFX_STAGE_VS || gfx_shader_stage(sh) == GFX_STAGE_FS) {
            if (P.gpl) lb_library(sh, gfx_shader_stage(sh) == GFX_STAGE_VS, 1);
            else lb_skip_lib();
        }
        plat_mutex_lock(&LB.stat);
        LB.done++;
        plat_mutex_unlock(&LB.stat);
    }
    if (!plat_dec32(&LB.running)) lb_report(LB.stop ? "stopped" : "done");
    return;
}

// The library state from the key file's records (prewarm); defaults without one.
static void lb_state(const uint32_t pr_hist[16], const uint32_t fs_hist[4]) {
    // state index: clamp | poly (0 fill, 1 line, 2 point -> VkPolygonMode) << 1 | minus_w << 3 (pr); samples log2 (fs)
    uint32_t best = 0, bf = 0;
    for (uint32_t i = 1; i < 16; i++) best = pr_hist[i] > pr_hist[best] ? i : best;
    for (uint32_t i = 1; i < 4; i++) bf = fs_hist[i] > fs_hist[bf] ? i : bf;
    if (!pr_hist[best]) best = 8;   // no key file: TotK clips -w..w (all 2706 records of a recorded key file)
    LB.clamp = (uint8_t)(best & 1), LB.poly = (uint8_t)(best >> 1 & 3), LB.minus_w = (uint8_t)(best >> 3 & 1);
    LB.samples = (uint8_t)(1u << bf);
}

// Boot: compute shaders of the warm list (recorded and prepared), then the first KEEP recorded graphics variants.
static void lb_start(const uint32_t pr_hist[16], const uint32_t fs_hist[4]) {
    const char *e = cfg_value("TOTK_LIB_PREBUILD"), *ke = cfg_get("TOTK_LIB_PREBUILD_KEEP");
    const char *te = cfg_get("TOTK_LIB_PREBUILD_THREADS");   // no fixed default: a quarter of the CPUs when unset
    if (!A.on || !cfg_bool("TOTK_LIB_PREBUILD")) return;
    GfxWarmEnt *all = NULL;
    size_t total = gfx_shader_warm_list(&all), n = 0, n_cs = 0;
    LB.keep_n = ke && *ke ? atoi(ke) : 0;
    LB.e = malloc((total + 1) * sizeof *LB.e);
    for (size_t i = 0; LB.e && i < total; i++)
        if (all[i].stage == GFX_STAGE_CS) LB.e[n++] = all[i];
    n_cs = n;
    for (size_t i = 0; LB.e && i < total && (int32_t)(n - n_cs) < LB.keep_n; i++)
        if (all[i].stage != GFX_STAGE_CS && all[i].prio == 0) LB.e[n++] = all[i];
    free(all);
    if (atoi(e) > 1 && (size_t)atoi(e) < n) n = (size_t)atoi(e);   // a count above 1 caps the list; 1 is plain on
    if (!n) {
        free(LB.e), LB.e = NULL;
        return;
    }
    LB.n = n;
    lb_state(pr_hist, fs_hist);
    LB.idle = 1;
    int cpus = (int)plat_cpu_usable();
    int t = te && *te ? atoi(te) : cpus / 4;
    t = t < 1 ? 1 : t > LB_MAX_THREADS ? LB_MAX_THREADS : t;
    if ((size_t)t > n) t = (int)n;
    LB.t0 = now_ms();
    LB.running = t;
    int started = 0;
    for (int i = 0; i < t; i++)
        if ((LB.th[started] = plat_thread_new(lb_worker, NULL, "lib prebuild"))) started++;
    plat_add32(&LB.running, started - t);
    LB.threads = started;
    rt_log("vk_pipeline: stage library prebuild: %zu compute pipelines and %zu recorded variants' libraries "
           "(TOTK_LIB_PREBUILD_KEEP %d) on %d idle-priority threads; library state clamp %u, polygon mode %u, depth "
           "-w..w %u, samples %u", n_cs < n ? n_cs : n, n > n_cs ? n - n_cs : 0, LB.keep_n, started, LB.clamp, LB.poly,
           LB.minus_w, LB.samples);
}

static void lb_stop(void) {
    plat_xchg32(&LB.stop, 1);
    for (int i = 0; i < LB.threads; i++)
        if (LB.th[i]) plat_thread_join(LB.th[i]), LB.th[i] = NULL;
    LB.threads = 0;
}

// Preparation: one prepared variant at a time into the driver's disk cache (private shader copy, nothing kept).
static void lb_warm_worker(void *arg) {
    (void)arg;
    for (;;) {
        size_t i = (size_t)plat_inc64(&LB.next) - 1;
        if (i >= LB.n) return;
        int owned = 0;
        const GfxShader *sh = gfx_shader_load_private(LB.e[i].iid, &owned);
        int st = sh ? gfx_shader_stage(sh) : -1;
        if (!sh) {
            plat_mutex_lock(&LB.stat);
            LB.noshader++;
            plat_mutex_unlock(&LB.stat);
        } else if (st == GFX_STAGE_CS) {
            Bindings B = {0};
            const char *why = NULL;
            lb_lock();
            Layout *l = compute_layout(sh, &B, &why);
            lb_unlock();
            double t0 = now_ms();
            VkPipeline pipe = l ? compute_create(sh, l) : VK_NULL_HANDLE;
            if (pipe) vkDestroyPipeline(P.device, pipe, NULL);
            lb_count(2, pipe ? 3 : 2, now_ms() - t0);
        } else if (st == GFX_STAGE_VS || st == GFX_STAGE_FS) {
            if (P.gpl) lb_library(sh, st == GFX_STAGE_VS, 0);
            else lb_skip_lib();
        }
        if (owned) gfx_shader_release_private(sh);
        plat_mutex_lock(&LB.stat);
        uint64_t d = ++LB.done;
        plat_mutex_unlock(&LB.stat);
        if (d % 4000 == 0) lb_report("preparation progress");
    }
}

// Preparation: every variant of <dir>/shader-prepared.bin on every core at normal priority.
static void lb_warm_prepared(const char *dir) {
    char path[PLAT_PATH_MAX * 2 + 32];
    snprintf(path, sizeof path, "%s/shader-prepared.bin", dir);
    uint64_t size = 0;
    uint8_t *f = plat_file_read(path, &size);
    size_t n = f && size >= 16 ? (size_t)(size - 16) / 36 : 0;
    LB.e = n ? calloc(n, sizeof *LB.e) : NULL;
    for (size_t i = 0; LB.e && i < n; i++)
        memcpy(LB.e[i].iid, f + 16 + 36 * i + 4, 32), LB.e[i].stage = f[16 + 36 * i], LB.e[i].prio = f[16 + 36 * i + 1];
    free(f);
    if (!LB.e) return;
    LB.n = n, LB.idle = 0, LB.t0 = now_ms();
    static const uint32_t no_pr[16], no_fs[4];
    lb_state(no_pr, no_fs);
    int cpus = (int)plat_cpu_usable(), k = 0;
    PlatThread *t[64];
    for (int i = 0; i < cpus && i < 64; i++)
        if ((t[k] = plat_thread_new(lb_warm_worker, NULL, "lib warm"))) k++;
    if (!k) lb_warm_worker(NULL);
    for (int i = 0; i < k; i++) plat_thread_join(t[i]);
    lb_report("preparation done");
    // with gpl off only compute variants are built: say so, rather than count the skipped ones as built
    if (LB.nolib)
        printf("prepare-shaders: %zu variants on %d threads in %.0f ms, %llu vertex/fragment variants skipped (graphics "
               "pipeline library off, they build at their first draw)\n", n, k, now_ms() - LB.t0, (unsigned long long)LB.nolib);
    else
        printf("prepare-shaders: %zu variants built into the driver cache on %d threads in %.0f ms\n", n, k, now_ms() - LB.t0);
    free(LB.e), LB.e = NULL, LB.n = 0, LB.next = 0, LB.done = 0;
}

// totk.exe --prepare-shaders (tools/prepare_shaders.py): the device sets the translator's host answers (they are in every
// cache key), the seed next to totk.exe is translated on every core, then every prepared variant's stage library or
// compute pipeline is built once into the driver's disk cache (lb_warm_prepared). No game runs.
int vk_pipeline_prepare_shaders(void) {
    if (!ensure_init()) {
        fprintf(stderr, "prepare-shaders: no Vulkan device\n");
        return 1;
    }
    char dir[PLAT_PATH_MAX * 2];
    snprintf(dir, sizeof dir, "%s", P.cache_path);
    char *cut = plat_last_sep(dir);
    if (!cut) return 1;
    *cut = 0;
    int cpus = (int)plat_cpu_usable();
    const char *pe = cfg_get("TOTK_PREWARM_XLATE");   // no fixed default: half the CPUs, at most 16, when unset
    int procs = pe && *pe ? atoi(pe) : cpus / 2 > 16 ? 16 : cpus / 2;
    int rc = gfx_shader_prepare_seed(dir, cpus, procs < 1 ? 1 : procs);
    if (!rc) lb_warm_prepared(dir);
    return rc;
}

// totk.exe --warm-shaders: every variant of shader-prepared.bin built into the driver's disk cache again from the shader
// cache, without translating the seed. The AMD cache is keyed to the exe path (a relink keeps it: 29,587 variants in
// 1.8 s) and holds a limited amount: preparing another build dir evicted this one's libraries, and a library a draw
// needed then took 40-220 ms in play instead of 0.2 ms. Seconds when the libraries are still there.
int vk_pipeline_warm_shaders(void) {
    if (!ensure_init()) {
        fprintf(stderr, "warm-shaders: no Vulkan device\n");
        return 1;
    }
    char dir[PLAT_PATH_MAX * 2];
    snprintf(dir, sizeof dir, "%s", P.cache_path);
    char *cut = plat_last_sep(dir);
    if (!cut) return 1;
    *cut = 0;
    char prp[PLAT_PATH_MAX * 2 + 32];
    snprintf(prp, sizeof prp, "%s/shader-prepared.bin", dir);
    if (!plat_file_exists(prp)) {
        fprintf(stderr, "warm-shaders: no %s (tools/prepare_shaders.py writes it)\n", prp);
        return 1;
    }
    lb_warm_prepared(dir);
    return 0;
}

// ---- dynamic state

void vk_pipeline_set_dynamic(VkCommandBuffer cb, const VkPipeState *s) {
    if (!ensure_init()) return;
    const poly_st *pl = &s->poly;
    const ds_st *d = &s->ds;
    // The front face maps unchanged: TOTK sets the upper-left window origin, and the y-flipped viewport (y_flip) puts
    // NDC +y at row 0 as NVN does, so the framebuffer winding Vulkan tests is the one NVN tests (MacNativeGames passes
    // it to Metal unchanged; LAS Recomp exec.c mirrors it only for the lower-left origin).
    int front_cw = pl->front == NVN_FRONT_FACE_CW;
    vkCmdSetCullMode(cb, (VkCullModeFlags)(pl->cull & 3));   // NVN_FACE_* has VkCullModeFlagBits' values
    vkCmdSetFrontFace(cb, front_cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, vk_pipeline_topology(s->prim));
    vkCmdSetDepthTestEnable(cb, d->dt);
    vkCmdSetDepthWriteEnable(cb, d->dw);
    vkCmdSetDepthCompareOp(cb, compare_op(d->df));
    vkCmdSetStencilTestEnable(cb, d->st);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, stencil_op(d->fail_f), stencil_op(d->zpass_f), stencil_op(d->zfail_f),
                      compare_op(d->sf_f));
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, stencil_op(d->fail_b), stencil_op(d->zpass_b), stencil_op(d->zfail_b),
                      compare_op(d->sf_b));
    // NVN_POLYGON_OFFSET_ENABLE_* bit of the polygon mode in use: point 1, line 2, fill 4
    unsigned mode = pl->mode <= NVN_POLYGON_MODE_FILL ? pl->mode : NVN_POLYGON_MODE_FILL;
    vkCmdSetDepthBiasEnable(cb, (pl->offset_en >> mode & 1) != 0);
    vkCmdSetRasterizerDiscardEnable(cb, s->raster_discard != 0);
    vkCmdSetPrimitiveRestartEnable(cb, s->prim_restart != 0);
    if (P.dyn_vi && s->vs) {
        VtxInput v;
        vertex_input(s, &v);
        rec_vertex_input(P.set_vi, cb, v.n_bind, v.b, v.n_attr, v.a);
    }
    if (P.dyn_clamp) rec_bool(P.set_clamp, cb, s->depth_clamp != 0);
    if (P.dyn_poly) rec_polygon_mode(P.set_poly, cb, polygon_mode(pl->mode));
    if (P.dyn_a2c) rec_bool((PFN_vkCmdSetDepthClampEnableEXT)P.set_a2c, cb, s->ms.a2c != 0);   // the same signature
    int n = target_count(s);
    if (!n) return;
    VkBool32 en[8];
    VkColorBlendEquationEXT eq[8];
    VkColorComponentFlags wm[8];
    for (int i = 0; i < n; i++) {
        const blend_st *b = &s->blend[i];
        en[i] = (s->color.blend_en >> i & 1) && blendable(target_format(s->color_fmt[i], NULL, NULL));
        eq[i] = (VkColorBlendEquationEXT){blend_factor(b->src_rgb), blend_factor(b->dst_rgb), blend_op(b->eq_rgb),
                                          blend_factor(b->src_a), blend_factor(b->dst_a), blend_op(b->eq_a)};
        wm[i] = target_wmask(s, i);
    }
    if (P.dyn_blend_en) rec_blend_enable(P.set_blend_en, cb, 0, (uint32_t)n, en);
    if (P.dyn_blend_eq) rec_blend_equation(P.set_blend_eq, cb, 0, (uint32_t)n, eq);
    if (P.dyn_wmask) rec_write_mask(P.set_wmask, cb, 0, (uint32_t)n, wm);
}

// ---- statistics, persistence, teardown

void vk_pipeline_stats(VkPipeStats *s) {
    plat_mutex_lock_shared(&P.lock);
    *s = P.stats;
    plat_mutex_unlock_shared(&P.lock);
}

static PlatMutex save_lock = PLAT_MUTEX_INIT;   // one cache save at a time; shutdown takes it to destroy the cache

void vk_pipeline_save(void) {
    if (P.ready) keys_flush();
    if (P.ready && P.gpl) {
        rt_log("vk_pipeline: gpl: %llu fast links (%llu by workers; avg %.2f ms, max %.2f ms, %llu failed), %llu draws "
               "lookups found shader libraries missing (left to a worker), %llu optimised pipelines swapped in (%llu queued "
               "low after their fast link), %llu drawing-thread links over 5 ms; %llu waiting draws built their libraries "
               "(avg %.2f ms, max %.2f ms)",
               (unsigned long long)P.g.links, (unsigned long long)P.g.wlinks, P.g.links ? P.g.link_ms / P.g.links : 0.0,
               P.g.link_max, (unsigned long long)P.g.link_fail, (unsigned long long)P.g.defer,
               (unsigned long long)P.g.swaps, (unsigned long long)P.g.requeued, (unsigned long long)P.g.slow,
               (unsigned long long)P.g.dlinks, P.g.dlinks ? P.g.dlink_ms / P.g.dlinks : 0.0, P.g.dlink_max);
        if (U.waits || U.queued)
            rt_log("vk_pipeline: gpl: library jobs for waiting draws: %llu queued, %llu built by a worker, %llu by the "
                   "thread that needed them (no worker free), %llu waits ended at their limit",
                   (unsigned long long)U.queued, (unsigned long long)U.by_worker, (unsigned long long)U.stolen,
                   (unsigned long long)U.timeouts);
        static const char *const kn[4] = {"vertex input", "pre-rasterization", "fragment shader", "fragment output"};
        for (int i = 0; i < 4; i++)
            rt_log("vk_pipeline: gpl: %s libraries %llu (avg %.2f ms, max %.2f ms)", kn[i], (unsigned long long)P.g.lib_n[i],
                   P.g.lib_n[i] ? P.g.lib_ms[i] / P.g.lib_n[i] : 0.0, P.g.lib_max[i]);
        if (LB.n)
            rt_log("vk_pipeline: gpl: prebuilt libraries a draw used: pre-rasterization %llu, fragment shader %llu; "
                   "compute pipelines built on the replay thread %llu", (unsigned long long)P.g.pre_used[LIB_PR],
                   (unsigned long long)P.g.pre_used[LIB_FS], (unsigned long long)LB.cs_draw);
    }
    gfx_shader_record_flush(1);
    if (!P.ready || !P.cache || !P.cache_path[0]) return;
    // Not under P.lock: the driver synchronises P.cache, and holding P.lock through the copy and the file write (about
    // 60 ms on the 1.4.3 fight) stopped every pipeline lookup of the replay, a freeze at each 20 s autosave.
    plat_mutex_lock(&save_lock);
    if (!P.cache) {   // vk_pipeline_shutdown destroyed it
        plat_mutex_unlock(&save_lock);
        return;
    }
    size_t size = 0;
    void *data = NULL;
    if (vkGetPipelineCacheData(P.device, P.cache, &size, NULL) == VK_SUCCESS && size && (data = malloc(size)) &&
        vkGetPipelineCacheData(P.device, P.cache, &size, data) == VK_SUCCESS) {
        if (!plat_file_write_atomic(P.cache_path, data, size)) rt_log("vk_pipeline: cannot write %s", P.cache_path);
    }
    free(data);
    plat_mutex_unlock(&save_lock);
}

// The game never shuts the device down (boot.c and the window end the process), so the cache is saved while it
// runs: at most every 20 s, and only after new pipelines were built. New pipeline keys are appended every 2 s.
static void autosave_once(void) {
    static uint64_t last_n, last_ms, last_keys;
    if (!P.ready) return;
    uint64_t now = plat_now_ms();
    if (*(volatile uint32_t *)&K.pend_n && now - last_keys >= 2000) last_keys = now, keys_flush();
    gfx_shader_record_flush(0);
    if (now - last_ms < 20000) return;
    VkPipeStats s;
    vk_pipeline_stats(&s);
    if (s.graphics + s.compute == last_n) return;
    last_n = s.graphics + s.compute, last_ms = now;
    vk_pipeline_save();
}

// On its own thread: the save (vkGetPipelineCacheData and the file write, about 60 ms) and the key appends (a flush to
// the disk each) ran on the present thread, and the render thread waited for it to take the next frame.
static void autosave_main(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    for (;;) {
        plat_sleep_ms(500);
        autosave_once();
    }
}

void vk_pipeline_autosave(void) {
    static int32_t started;
    if (!P.ready || plat_xchg32(&started, 1)) return;
    if (!plat_thread_spawn(autosave_main, NULL, "pipe autosave", 0))
        rt_log("vk_pipeline: cannot start the autosave thread; the cache is saved at exit only");
}

void vk_pipeline_shutdown(void) {
    if (!P.ready) return;
    lb_stop();
    workers_drain();   // jobs point at layouts and Pipe entries freed below
    vk_pipeline_save();
    plat_mutex_lock(&P.lock);
    vkDeviceWaitIdle(P.device);
    for (int i = 0; i < PIPE_BUCKETS; i++) {
        for (Pipe *p = pipes[i], *n; p; p = n) {
            n = p->next;
            if (p->info.pipeline) vkDestroyPipeline(P.device, p->info.pipeline, NULL);
            if (p->fast && p->fast != p->info.pipeline) vkDestroyPipeline(P.device, p->fast, NULL);
            free(p->bindings), free(p);
        }
        pipes[i] = NULL;
    }
    libs_destroy();
    for (int i = 0; i < LAYOUT_BUCKETS; i++) {
        for (Layout *l = layouts[i], *n; l; l = n) {
            n = l->next;
            for (int k = 0; k < 2; k++)
                if (l->tpl[k]) P.tpl_destroy(P.device, l->tpl[k], NULL);
            vkDestroyPipelineLayout(P.device, l->layout, NULL);
            for (int h = 0; h < 2; h++)
                if (l->half[h]) vkDestroyPipelineLayout(P.device, l->half[h], NULL);
            vkDestroyDescriptorSetLayout(P.device, l->set, NULL);
            if (l->set1) vkDestroyDescriptorSetLayout(P.device, l->set1, NULL);
            free(l);
        }
        layouts[i] = NULL;
    }
    plat_mutex_lock(&save_lock);   // not while the autosave thread copies it
    if (P.cache) vkDestroyPipelineCache(P.device, P.cache, NULL);
    P.cache = VK_NULL_HANDLE;
    plat_mutex_unlock(&save_lock);
    memset(&P.stats, 0, sizeof P.stats);
    plat_xchg32(&P.ready, 0);
    plat_mutex_unlock(&P.lock);
}

// ---- self-test: census shaders (probe_census_dir, tools/shader_census.py) through gfx_shader_get into pipelines.
// Graphics pairs are a fragment shader and a vertex shader of the same source file, spread over the census; compute
// shaders are spread the same way. Every pipeline is built twice (the second call must hit the cache) and the
// dynamic state of the first graphics pipeline is recorded into a throwaway command buffer.

typedef struct { const char *key, *c1, *src; size_t c1_len, src_len; int stage; uint32_t local[3]; } CensusEntry;

static const char *st_field(const char *from, const char *to, const char *name, size_t *len) {
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\": \"", name);
    const char *p = strstr(from, pat);
    if (!p || p >= to) return NULL;
    p += strlen(pat);
    const char *e = memchr(p, '"', (size_t)(to - p));
    if (!e) return NULL;
    *len = (size_t)(e - p);
    return p;
}

static int census_cmp(const void *x, const void *y) {
    const CensusEntry *a = x, *b = y;
    size_t n = a->src_len < b->src_len ? a->src_len : b->src_len;
    int c = memcmp(a->src, b->src, n);
    if (c) return c;
    if (a->src_len != b->src_len) return a->src_len < b->src_len ? -1 : 1;
    return a->stage - b->stage;
}

static char *census_read(const char *root, size_t *size) {
    char path[PLAT_PATH_MAX * 2 + 64];
    snprintf(path, sizeof path, "%s/jobs.json", root);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) free(b), b = NULL;
    fclose(f);
    if (b) b[n] = 0, *size = (size_t)n;
    return b;
}

static int census_parse(const char *jobs, CensusEntry *out, int max) {
    int n = 0;
    for (const char *p = jobs; n < max && (p = strstr(p, "\"job\": {")); p += 8) {
        const char *end = strstr(p, "\"origin\": ");
        if (!end) break;
        size_t kl, sl, cl, rl;
        const char *k = st_field(p, end, "key", &kl), *s = st_field(p, end, "stage", &sl), *c = st_field(p, end, "c1", &cl),
                   *r = st_field(p, end, "source", &rl);
        if (!k || !s || !c || !r || kl != 64) continue;
        CensusEntry *e = &out[n++];
        e->key = k, e->c1 = c, e->c1_len = cl / 2, e->src = r, e->src_len = rl;
        e->stage = !strncmp(s, "vs", sl) ? GFX_STAGE_VS : !strncmp(s, "ps", sl) ? GFX_STAGE_FS : GFX_STAGE_CS;
        e->local[0] = e->local[1] = e->local[2] = 1;
        const char *l = strstr(p, "\"local\": [");
        if (l && l < end) {
            l += 10;
            for (int i = 0; i < 3; i++) {
                while (*l == ' ' || *l == '\n' || *l == '\r' || *l == ',') l++;
                e->local[i] = (uint32_t)strtoul(l, (char **)&l, 10);
            }
        }
    }
    return n;
}

static int hexv(char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

// The program the game would hand NVN for a census entry, translated with default answers (as gfx_shader_probe.c).
static const GfxShader *census_shader(const char *root, const CensusEntry *e) {
    char path[PLAT_PATH_MAX * 2 + 128];
    static uint8_t c1[8192];
    uint8_t control[0x880] = {0};
    if (e->c1_len > sizeof c1) return NULL;
    for (size_t b = 0; b < e->c1_len; b++) c1[b] = (uint8_t)(hexv(e->c1[b * 2]) << 4 | hexv(e->c1[b * 2 + 1]));
    snprintf(path, sizeof path, "%s/code/%.64s.code", root, e->key);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long body = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t skip = e->stage == GFX_STAGE_CS ? 0 : 0x30, total = skip + (size_t)body;
    uint8_t *code = calloc(1, total);
    int ok = code && fread(code + skip, 1, (size_t)body, f) == (size_t)body;
    fclose(f);
    if (!ok) return free(code), NULL;
    uint32_t c1_off = (uint32_t)(total - e->c1_len), length = (uint32_t)total;
    memcpy(control + 0x700, &c1_off, 4), memcpy(control + 0x704, &length, 4);
    GfxShaderAnswers a = {0};
    if (e->stage == GFX_STAGE_CS) for (int k = 0; k < 3; k++) a.local[k] = (uint16_t)e->local[k];
    const GfxShader *s = gfx_shader_get(e->stage, control, code, total, c1, &a);
    free(code);
    return s;
}

// A state for a census pair: one target per fragment output (RGBA8 of the output's type), D32S8 depth with the test on,
// alpha blending on target 0, one RGBA32 attribute per vertex input, interleaved in stream 0.
static void selftest_state(VkPipeState *s, const GfxShader *vs, const GfxShader *fs) {
    vk_pipe_state_defaults(s);
    s->vs = vs, s->fs = fs;
    GfxShaderIo io[32];
    uint32_t outs = gfx_shader_outputs(fs, io);
    for (int i = 0; i < 8; i++)
        if (outs >> i & 1)
            s->color_fmt[i] = io[i].type == GFX_TYPE_UINT ? NVN_FORMAT_RGBA8UI : io[i].type == GFX_TYPE_SINT ? NVN_FORMAT_RGBA8I
                                                                                                            : NVN_FORMAT_RGBA8;
    s->depth_fmt = NVN_FORMAT_DEPTH32F_STENCIL8;
    s->ds.dt = 1;
    s->color.blend_en = 1;
    s->blend[0].src_rgb = NVN_BLEND_FUNC_SRC_ALPHA, s->blend[0].dst_rgb = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA;
    s->poly.cull = NVN_FACE_BACK;
    uint32_t ins = gfx_shader_inputs(vs, io), off = 0;
    for (int l = 0; l < 16; l++)
        if (ins >> l & 1) {
            va_st a = {io[l].type == GFX_TYPE_UINT ? NVN_FORMAT_RGBA32UI : io[l].type == GFX_TYPE_SINT ? NVN_FORMAT_RGBA32I
                                                                                                      : NVN_FORMAT_RGBA32F,
                       off, 0};
            memcpy(&s->vattr[l], &a, sizeof a);
            off += 16;
        }
    s->vstream[0].stride = off ? off : 16;
}

// Records the dynamic state of a pipeline into a command buffer that is never submitted (validation checks the calls).
static int selftest_record(const VkPipeState *s, VkPipeline pipe) {
    VkCtx *c = vk_ctx();
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = c->gfx_family};
    VkCommandPool pool;
    if (vkCreateCommandPool(c->device, &pi, NULL, &pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    int ok = vkAllocateCommandBuffers(c->device, &ai, &cb) == VK_SUCCESS;
    if (ok) {
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb, &bi);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vk_pipeline_set_dynamic(cb, s);
        ok = vkEndCommandBuffer(cb) == VK_SUCCESS;
    }
    vkDestroyCommandPool(c->device, pool, NULL);
    return ok;
}

int vk_pipeline_selftest(void) {
    enum { MAXN = 20000, WANT_PAIRS = 64, WANT_CS = 16 };
    char root[PLAT_PATH_MAX * 2];
    if (!vk_init() || !ensure_init()) { printf("vk_pipeline selftest: FAILED: no Vulkan device\n"); return 1; }
    if (!probe_census_dir(root, sizeof root)) {   // the census folder, build/<version>/shaders
        printf("vk_pipeline selftest: FAILED: no build/v%d/shaders/jobs.json above the exe\n", rt_game_version);
        return 1;
    }
    size_t size;
    char *jobs = census_read(root, &size);
    CensusEntry *e = malloc(sizeof *e * MAXN);
    int n = jobs && e ? census_parse(jobs, e, MAXN) : 0;
    if (!n) { printf("vk_pipeline selftest: FAILED: no census entries\n"); free(jobs), free(e); return 1; }
    qsort(e, (size_t)n, sizeof *e, census_cmp);
    long err0 = vk_validation_errors_total();
    VkPipeStats st0;
    vk_pipeline_stats(&st0);

    // candidate pairs: each fragment shader of a source file with one of the file's vertex shaders (round robin)
    int (*pairs)[2] = malloc(sizeof *pairs * (size_t)n), np = 0, *cs = malloc(sizeof *cs * (size_t)n), nc = 0;
    for (int i = 0; i < n;) {
        int j = i, v0 = -1, nv = 0, nf = 0;
        for (; j < n && e[j].src_len == e[i].src_len && !memcmp(e[j].src, e[i].src, e[i].src_len); j++) {
            if (e[j].stage == GFX_STAGE_VS && v0 < 0) v0 = j;   // sorted by stage within a file: vertex shaders first
            nv += e[j].stage == GFX_STAGE_VS;
            if (e[j].stage == GFX_STAGE_CS) cs[nc++] = j;
        }
        for (int k = i; k < j && nv; k++)
            if (e[k].stage == GFX_STAGE_FS) pairs[np][0] = v0 + nf++ % nv, pairs[np][1] = k, np++;
        i = j;
    }
    int tried = 0, built = 0, cached = 1, shader_fail = 0, ctried = 0, cbuilt = 0, recorded = 0, varying_fix = 0;
    for (int t = 0; t < WANT_PAIRS && t < np; t++) {
        const int *pr = pairs[(long long)t * np / (np < WANT_PAIRS ? np : WANT_PAIRS)];
        tried++;
        const GfxShader *vs = census_shader(root, &e[pr[0]]), *fs = census_shader(root, &e[pr[1]]);
        if (!vs || !fs) { shader_fail++; continue; }
        if (gfx_shader_inputs(fs, NULL) & ~gfx_shader_outputs(vs, NULL)) varying_fix++;
        VkPipeState s;
        selftest_state(&s, vs, fs);
        GfxShaderAnswers fa = {0};
        vk_pipeline_answers(&s, NULL, &fa);
        const VkPipeInfo *info;
        VkPipeline p = vk_pipeline_graphics(&s, &info);
        if (!p) continue;
        built++;
        const VkPipeInfo *again;
        if (vk_pipeline_graphics(&s, &again) != p || again != info) cached = 0;
        if (!recorded) recorded = selftest_record(&s, p);
    }
    for (int t = 0; t < WANT_CS && t < nc; t++) {
        const CensusEntry *c = &e[cs[(long long)t * nc / (nc < WANT_CS ? nc : WANT_CS)]];
        ctried++;
        const GfxShader *s = census_shader(root, c);
        if (!s) { shader_fail++; continue; }
        const VkPipeInfo *info;
        VkPipeline p = vk_pipeline_compute(s, &info);
        if (!p) continue;
        cbuilt++;
        const VkPipeInfo *again;
        if (vk_pipeline_compute(s, &again) != p || again != info) cached = 0;
    }
    vk_pipeline_save();
    VkPipeStats st;
    vk_pipeline_stats(&st);
    long errors = vk_validation_errors_total() - err0;
    printf("vk_pipeline selftest: graphics %d/%d census pairs (%d needed vertex outputs added), compute %d/%d, "
           "%d shaders failed to translate; layouts %llu (%llu push); cache hits %llu; dynamic state recorded %d; "
           "validation errors %ld\n",
           built, tried, varying_fix, cbuilt, ctried, shader_fail, (unsigned long long)(st.layouts - st0.layouts),
           (unsigned long long)(st.push_layouts - st0.push_layouts), (unsigned long long)(st.hits - st0.hits), recorded,
           errors);
    free(pairs), free(cs), free(e), free(jobs);
    int ok = built > 0 && cbuilt > 0 && cached && recorded && errors == 0;
    printf("vk_pipeline selftest: %s\n", ok ? "passed" : "FAILED");
    return ok ? 0 : 1;
}
