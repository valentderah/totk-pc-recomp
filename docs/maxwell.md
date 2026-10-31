# Maxwell / NVN behaviour a port must reproduce

Reference for diagnosing graphics bugs. Each item: what Maxwell (GM20B, 3D class `B197`, compute `B1C0`) and NVN do,
what Vulkan / AMD (RDNA4, our GPU0) do instead, how emulators and ports handle it, and the source. Items are ordered by
how likely they are to cause visible artifacts. "Ours:" marks what our runtime does (grep-level look, see section 7;
our code is cited by file and function). Licensing: yuzu/suyu are GPL, cited for behaviour only.

Source shorthands:

- **OGD**: NVIDIA open-gpu-doc `classes/3d/clb197.h`, `classes/compute/clb1c0.h` (MIT-style),
  <https://github.com/NVIDIA/open-gpu-doc/tree/master/classes>. Offsets below are method offsets in `clb197.h`.
- **G80TEX**: Mesa nouveau `src/gallium/drivers/nouveau/nv50/g80_texture.xml.h` (MIT),
  <https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/gallium/drivers/nouveau/nv50/g80_texture.xml.h>.
- **RYU-SU**: Ryujinx `Ryujinx.Graphics.Gpu/Engine/Threed/StateUpdater.cs` (MIT), mirror at 1.1.1137:
  <https://git.dawidpotocki.com/mirror/ryujinx/plain/src/Ryujinx.Graphics.Gpu/Engine/Threed/StateUpdater.cs?h=1.1.1137>.
- **RYU-SH**: Ryujinx shader translator vendored at `refs/MacNativeGames/vendor/ryujinx/src/Ryujinx.Graphics.Shader/`
  (Ryujinx 1.3.2, MIT). Paths below are relative to it.
- **NXN**: nxnative's graphics notes, `refs/nxnative/docs/agents/` (MIT). nxnative has rewritten them since; each
  row states the behaviour and fix it took from them.
- **MNG**: `refs/MacNativeGames` @ `b75be8cfa704df3701e86130c8ced268e79d6b11` (MIT); line numbers below are at that
  commit.
- **DK3D**: `refs/deko3d` @ `373bfd43d8b2d805fa2eb7d406dbb823032df958` (zlib, fincs), the homebrew Maxwell driver for Switch. Written
  against the hardware, not the NVN API: what it programs is what the GPU needs. Paths relative to `source/`. `.def` files give
  methods as word indices (`engine_3d.def`); byte offset = index x 4, so `0x5A1` = OGD `0x1684`. No conditional rendering, no
  provoking-vertex conversion, no 8-bit indices in it.
- **YZ-2305 / YZ-2306**: yuzu progress reports May / June 2023,
  <https://yuzu-mirror.github.io/entry/yuzu-progress-report-may-2023/>,
  <https://yuzu-mirror.github.io/entry/yuzu-progress-report-jun-2023/> (behaviour only).
- **VKSPEC**: Vulkan 1.4 specification, <https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html>;
  **GLSL450**: SPIR-V GLSL.std.450 extended instructions, <https://registry.khronos.org/SPIR-V/specs/unified1/GLSL.std.450.html>.

## 1. Render targets

| # | Maxwell / NVN | Vulkan / AMD | Emulators / ports | Source |
|---|---|---|---|---|
| 1.1 | **Colour target the fragment shader does not write keeps its old texel**, even with blending enabled. | Undefined value written/blended. | NXN: zero write mask and no blend for unwritten outputs (`reflection colors`). Already fixed here. | NXN; MNG `games/totk/tests/metal_semantics/probe.m:5` |
| 1.2 | **Unwritten vertex outputs read as (0, 0, 0, 1)** in the next stage, including the unwritten components of a partly written output (VS writes xyz, FS divides by w). | FS input with no VS output: interface mismatch, value undefined; an unwritten component of a written output is undefined (AMD: whatever the register holds, often 0). | Ryujinx initialises exactly the components the next stage reads, but only when stages are linked (`Translation/Translator.cs:212-227`, `NextInputAttributesComponents`); position starts (0,0,0,1) (`:259`). NXN Link's Awakening: sun shadow lookups divided by w = 0, scene dim, no shadows. Ours: missing outputs padded with (0, 0, 0, 1), unlinked outputs initialised by translator patch 0006 (section 7, check 2). | NXN |
| 1.3 | **Clears honour the scissor and the clear's own channel / stencil write mask**, and may come between draws inside a pass. `SET_CLEAR_SURFACE_CONTROL` (0x10f8) has `RESPECT_STENCIL_MASK`, `USE_CLEAR_RECT`, `USE_SCISSOR0`, `USE_VIEWPORT_CLIP0`. | `vkCmdClearAttachments` takes a rect but **ignores the colour write mask**; load-op clears are whole-attachment. | NXN/MNG draw a triangle with the clear's mask (`clear_color_tri`, `clear_ds_tri`); TotK uses mid-pass, partial-mask and partial-scissor clears (NXN). yuzu: "red dot" was clears hitting the wrong RT index (YZ-2305, PR 10243). Hardware recipe (DK3D): `CLEAR_BUFFERS` (`0x674` = 0x19d0) carries the colour channel mask (bits 2-5), depth (0), stencil (1), target id (6-9) and layer (10-20) in one word; `ClearBufferFlags` (`0x43E` = 0x10f8) is set once to `StencilMask | Scissor` (`maxwell/gpu_3d_base.cpp:69`), so every clear is cut by scissor 0 and its stencil bits by the stencil front write mask register, and DK3D's colour clear does not program `ColorWriteMask` (`0x680`). A stencil clear writes `StencilFrontMask` (`0x4E7`) and `ClearStencil` (`0x368`) just before the clear and restores the shadowed state after (`gpu_3d_base.cpp:497-510`). Multisample raster is switched off around a colour clear (`:459, :465`). Arrays: an MME macro repeats the clear for every layer (`maxwell/clear_buffers.mme:5-22`). No pass concept: clears sit between draws. Ours: masked colour and stencil clears are **skipped** (check 3). | OGD 0x10f8; NXN; DK3D `maxwell/gpu_3d_base.cpp:444-512`, `engine_3d.def:305, 644` |
| 1.4 | **Conditional rendering** (`SET_RENDER_ENABLE_A..C` 0x1550-0x1558, NVN `SetRenderEnableConditional`) compares two 64-bit counters at `addr` and `addr+16` on the GPU, in command order. TotK wraps nearly every clear in it. A `TIMESTAMP` report writes 0 to the counter field. | `VK_EXT_conditional_rendering` tests one 32-bit word against zero. | NXN: honouring it fixed the black 3D scene (light-volume depth never cleared). deko3d has no conditional rendering at all (no `SET_RENDER_ENABLE` in `engine_3d.def`), so it cannot settle the compare rule. Ours: evaluated on the CPU at record time; a report from an unfinished submission counts as 0 (check 5). | OGD; NXN |
| 1.5 | **Depth mode** is runtime state: `nvnDeviceSetDepthMode` near = -w (GL, NVN default) or 0. Register `SET_VIEWPORT_Z_CLIP` (0x0d7c) `NEGATIVE_W_TO_POSITIVE_W` / `ZERO_TO_POSITIVE_W`. | Vulkan clips z < 0 unless `VK_EXT_depth_clip_control` `negativeOneToOne`. | Ryujinx infers the mode from the viewport transform, falling back to the register (`GetDepthMode`, RYU-SU). NXN: wrong mode either clips everything (black window) or puts all depth in 0.5..1 (DOF blurs the screen). Ours: `negativeOneToOne` from NVN state. DK3D programs both halves: `SetDepthMode` (`0x35F` = 0xd7c) selects the pre-transform clip range, and the viewport transform maps z to [near, far] itself: scale z = `0.5 * (far - near)` and offset z = `0.5 * (near + far)` for -1..1, scale `far - near` and offset `near` for 0..1 (`maxwell/gpu_3d_base.cpp:143-147, 336-343`). Same maths as Vulkan `negativeOneToOne`. | OGD 0x0d7c; NXN-64, NXN; DK3D `maxwell/gpu_3d_base.cpp:136-147, 321-362` |
| 1.6 | **Depth clamp / clip**: `SET_VIEWPORT_CLIP_CONTROL` (0x193c) has separate `PIXEL_MIN_Z`/`PIXEL_MAX_Z` CLIP or CLAMP and geometry-clip modes. | `depthClampEnable` also disables z clipping (unless `VK_EXT_depth_clip_enable`). RADV had a dynamic depth clamp driver bug causing TotK vertex explosions. | Ryujinx: clamp = `!(clip & DepthClampDisabled)` (RYU-SU `UpdateDepthClampState`). yuzu disabled dynamic depth clamp on affected RADV (YZ-2305, PR 10262). DK3D has one switch, `depthClampEnable`, and writes `VIEW_VOLUME_CLIP_CONTROL` (`0x64F` = 0x193c) with `0x101A` when on and `0x181D` when off (default at setup is `0x181D`); it labels the bits "todo: figure out what this actually does". Bits 3-4 (`DepthClampNear/Far`) are set in both values, bits 0-2 and 11 differ. There is no separate depth-clip toggle. The viewport near/far registers (`Viewport` array words 2-3) carry the clamp range and are swapped when far < near (`:349-350`). Vulkan clamp to [min, max] of the viewport depth matches. | OGD 0x193c; DK3D `maxwell/gpu_3d_state.cpp:42`, `gpu_3d_base.cpp:135-139, 349-350`, `engine_3d.def:621` |
| 1.7 | **Depth formats**: D24S8 is native; D24 quantises to 2^-24 everywhere in 0..1. Polygon offset units are in D24 steps for D24 targets; `SET_DEPTH_BIAS_CONTROL` (0x1110) `DEPTH_FORMAT_DEPENDENT`. | RX 9070 has no D24S8 attachment; D32F has finer steps near 0 and a per-primitive exponent-based r. EQUAL / LEQUAL tests between a pre-pass and a later pass, and decal biases, behave differently. | yuzu rescales depth-bias units when D24 is emulated as D32F; Ryujinx passes `units / 2` (register is twice the API value) (RYU-SU `UpdateDepthBiasState`). MNG: D16 load-clear 0.5 → 32767 vs fragment 0.5 → 32768 made Ultrahand/Recall classification tiles visible. Ours: D24S8 → D32_SFLOAT_S8, bias not rescaled (check 4). DK3D writes `PolygonOffsetUnits` (`0x56F` = 0x15bc) as `constantFactor * 2.0f`, clamp to `0x61F`, factor to `0x55B` (`maxwell/gpu_3d_state.cpp:148-157`), confirming the x2 register. | OGD 0x1110, 0x15bc, 0x156c; MNG `docs/how-it-works.md:57-61`; DK3D `maxwell/gpu_3d_state.cpp:154` |
| 1.8 | **Position invariance**: identical position math gives identical depth across programs (pre-pass then EQUAL / LEQUAL). | Only for outputs declared `Invariant`. | Ryujinx decorates position `Invariant` (`CodeGen/Spirv/Declarations.cs:571`) and marks float ops `NoContraction` unless `QueryHostReducedPrecision` (`CodeGen/Spirv/Instructions.cs:2047`). NXN: Odyssey ground z-fought without it; LA's foliage drawn with depth EQUAL vanished when the pre-pass failed. Ours: translator default (all NoContraction). | NXN |
| 1.9 | **Sampling a texture that is also a bound render target**: Maxwell's texture cache is not coherent with ROP writes; NVN requires `nvnCommandBufferBarrier(INVALIDATE_TEXTURE)` between writing and sampling (methods `INVALIDATE_TEXTURE_DATA_CACHE` 0x1338, `..._NO_WFI` 0x1288). Reading a different level/layer, or read-only depth while testing, is fine. | Same image as attachment and sampled in one pass is a feedback loop: undefined unless `VK_EXT_attachment_feedback_loop_layout`; on AMD DCC/HTILE-compressed data is not readable by the sampler without decompression. | yuzu: pixelated Ultrahand glow fixed with barriers on attachment feedback loops (YZ-2305, PR 10402). BotW black textures on GCN worked around with `AMD_DEBUG=nohyperz` (HTILE off) (<https://git.openpunk.com/OpenPunk/yuzu/wiki/commit/bd401539420182cbbf8bc638b042536a7e690c3f>). Ours: an NVN barrier ends the pass and inserts a full memory barrier; no detection of a sampled texture that is a current attachment (check 10). DK3D barrier recipe (`maxwell/gpu_base.cpp:49-115`): the ordering part and the cache part are separate. Ordering: Tiles = `TiledCacheBarrier` (`0x3DF`), Fragments = `FragmentBarrier` (`0x378`), Primitives = `WaitForIdle` (`0x44`), Full = gpfifo `SetReference` + new gpfifo entry with `NoPrefetch`, so later command and indirect-parameter fetch happens after everything finished. Tiles and Fragments need an implicit WFI before a cache invalidate. Caches (each opt-in): texture data `InvalidateTextureDataCache` (`0x3DD`, WFI) or `..NoWfi` (`0x4A2`); shader `InvalidateShaderCaches` (`0x369`: instruction, global data, constant); descriptors `InvalidateTextureHeaderCacheNoWfi` (`0x50A`) + `InvalidateSamplerCacheNoWfi` (`0x509`); zcull `0x4B2`; L2 gpfifo `MemOpB` `L2FlushDirty` + `L2SysmemInvalidate`. The texture-cache invalidate is its own flag; ordering alone does not refresh it. Every queue flush ends with image + shader + descriptor + L2 invalidation "to ensure the visibility of CPU updates between flushes" (`:28-47`): CPU writes into textures and descriptors reach the GPU at the next submit boundary, not mid-submit (2.2). | OGD; DK3D `maxwell/gpu_base.cpp:28-115` |
| 1.10 | **Duplicate attachments**: the same texture bound at two colour slots with write mask 0 on one does not interfere (ROP writes only enabled channels). | Tile-based or compressed attachments may store the unwritten copy over the written one. | NXN BotW G-buffer (c1 = c2, c3 = c4): leaves the duplicate slot unattached (`rt_dup_slot`). | NXN |
| 1.11 | **Per-target write masks**: `RtColorMask[i]`, or entry 0 for all when `RtColorMaskShared`. `SET_SINGLE_CT_WRITE_CONTROL` (0x0f90). A colour target with format 0 is disabled. | Per-attachment `colorWriteMask`. | RYU-SU `UpdateRtColorMask`, `IsRtEnabled` ("Colors are disabled by writing 0 to the format"). | OGD 0x0f90 |
| 1.12 | **sRGB render target views**: NVN encodes on write when the view format is sRGB over a linear texture. Maxwell also has a global `SET_SRGB_WRITE` (0x15b8). | Format of the attachment view decides; needs `MUTABLE_FORMAT`. | NXN Mario: attaching the base texture stored linear values, picture darker. Ours: RT views take the view format (`view_format`). | OGD 0x15b8; NXN |
| 1.13 | **Compressed and 3D targets**: agl's GPU BC4 encoder renders blocks through an `RGBA16UI` view, one plane of a 32×32×32 BC4 3D texture per pass. NVN renders a 3D texture slice by slice. | Needs `BLOCK_TEXEL_VIEW_COMPATIBLE` + `EXTENDED_USAGE`, `2D_ARRAY_COMPATIBLE`. | NXN shadow textures (`bcrt_attach`). yuzu: wrong block depth on 3D slices gave Gloom textures on terrain (YZ-2305, PR 10433). Already fixed here. | NXN |
| 1.14 | **Draw with no render target** (fragment shader only writes an SSBO, 1×1 viewport) runs normally. | Needs an attachment-less pass with explicit render area. | NXN `bare_dims`: cave ambient light came back. Ours: area of viewport 0 (`vk_exec.c` `resolve_rt`). | NXN |
| 1.15 | **Window origin**: NVN default lower-left (GL); TotK switches to upper-left. Y flip and scissor flip follow. | Vulkan upper-left; negative viewport height to flip. | Ryujinx flips scissor against screen height when Y-negate is on (RYU-SU `UpdateScissorState`). | NXN |
| 1.16 | **Multiple viewports / scissors** (16), selected by `ViewportIndex` output. | Needs `multiViewport` and `shaderOutputViewportIndex`. | Ours: one viewport, one scissor; differing scissors logged (`nvn_api_cmd.c`). | OGD 0x192c |
| 1.17 | **Fragment output saturation** per target: `SET_PS_SATURATE` (0x13a8) clamps output n to 0..1 or -1..1 in fixed function. NVN does not expose it (GLSLC emits `.SAT` in code). | None. | Not emulated by Ryujinx; listed so a clamp difference is not chased in the wrong place. | OGD 0x13a8 |
| 1.18 | **New memory reads as zero** (fresh pool pages); feedback effects never clear their state texture. | New image content undefined. | NXN `tex_zero_fill`. | NXN |
| 1.19 | **MSAA**: `nvnCommandBufferDownsample` resolves; depth-stencil downsamples are copies the game does itself. | `vkCmdResolveImage` colour only; integer formats pick one sample. | NXN LA: stencil lost in the half-size depth copy, no AO. | NXN |

## 2. Textures and samplers

| # | Maxwell / NVN | Vulkan / AMD | Emulators / ports | Source |
|---|---|---|---|---|
| 2.1 | **Automatic block height**: NVN truncates `height_in_blocks * 1.5 / 8` before picking the GOB power of two (deko3d rounds up). Wrong H reads padding: 160×160 BC3 warp pad half black. Allocation size is the same, so size checks do not catch it. Mip levels shrink the block height while the level fits in half of it. | Opaque optimal tiling. | MNG fixed (40 SDK-measured cases, `games/totk/tests/texture_auto_layout/probe.c`). Ours: truncating form already (`runtime/nvn_tex.c`, the block-height pick). DK3D reads: height in block rows (rounded up), x 1.5, then `(h + 7) / 8` rounded up, then the largest power of two <= that, capped at 16 GOBs (`dk_image.cpp:444-447`, `pickTileSize` `:13-20`); 3D uses `depth + depth / 2` directly as the GOB count (`:451-454`); video images are fixed at 2 GOBs (`:438`). Per level, the shift drops while `unit << (shift - 1) >= level dimension` with unit 8 rows / 1 slice (`adjustTileSize` `:23-37`, used at `:89-91`), width tile is always 0. Array layers are padded to the block shrunk to the full height / depth (`alignLayerSize` `:57-72`). Every one of these matches `nvn_tex.c` except the rounding. | MNG `docs/how-it-works.md:47-53`; DK3D `dk_image.cpp:13-37, 57-72, 80-103, 436-455` |
| 2.2 | **Textures filled by the CPU with no NVN flush** (TotK terrain: two 121-layer ASTC 8×8 arrays, 42 MB each; BC4 material textures read back with `ReadTexels` and copied into a 5120×5120 roughness atlas). The CPU and GPU share memory, so no flush is needed on the Switch. | Host images are copies; must detect CPU writes. | NXN write-protects texture pages (`tex_dirty_upload`); zero ASTC decodes magenta, zero BC black. yuzu: small CPU writes into GPU-used pages not invalidated gave slow rain/snow particles and teleport particle corruption (YZ-2306, PR 10942). Ours: `GetWriteWatch` generations (`vk_memory.c`, `mem_written_pages`). | NXN-175 |
| 2.3 | **Same storage, new texture object every frame** (transient targets, deferred handles). Same storage with a different descriptor reinterprets the same block-linear bytes. | Two `VkImage`s over one allocation with optimal tiling do not share a defined layout (AMD tiling and DCC metadata are per image); a new image starts `UNDEFINED`. | NXN shares host textures by `(pool, offset, description)`; MNG keeps a "live texture at address" redirect. yuzu: whole areas black for a frame on camera changes from texture-cache alias/overlap ordering (YZ-2305, PR 10418). Ours: GPU-only pool textures are `VkImage`s bound onto pool memory (check 7). | NXN; MNG `docs/how-it-works.md:67-71` |
| 2.4 | **Deferred texture handles** (`BindTexturesDeferred`): the GPU reads the handle from memory when the command runs; BotW records command blocks once and writes handles later. | n/a | NXN BotW: handles copied at record time were zero, 3D scene black. | NXN |
| 2.5 | **Border colours**: any RGBA (TSC words 4-7, plus sRGB border fields). TotK/BotW grass interaction map clamps to (0.5, 0.5, 0.5, 0.5), its "untouched" value. | Three fixed colours unless `VK_EXT_custom_border_color` (+ `customBorderColorWithoutFormat`); integer formats need `INT_CUSTOM`. | MNG emulates in the shader; with transparent black, grass away from the player lay flat and showed no cut (MNG issue #49). Ours: custom border when supported, else transparent black (check 11). | G80TEX; MNG `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m:5041-5045` |
| 2.6 | **sRGB decode** is a TIC bit (`G80_TIC_2_SRGB_CONVERSION`) separate from the format; TSC has its own `SRGB_CONVERSION` (word 0 bit 13). NVN exposes sRGB as formats. | Format decides. | NVN-level translation by format is enough. | G80TEX:197, :377 |
| 2.7 | **Swizzles** per texture and per view (`TextureBuilderSetSwizzle`, `TextureViewSetSwizzle`); BotW layout captures rebuild a texture on the same storage with (1, 1, 1, R) or alpha 1. Default per-format mapping R8 → (R, 0, 0, 1). | `VkComponentMapping` on the view. | NXN: font atlas white boxes, HUD dark boxes without it. Ours: mapped (`vk_exec_draw.c` `get_view`). | NXN |
| 2.8 | **Sample dimension comes from the instruction**: a non-array sample of an array texture reads layer 0; 1,512 BotW shaders sample a cube as a cube array. | View type must match the declared type. | NXN: 2D array bound as 2D gave one pale colour (washed-out scene); cubes created as 6-layer arrays with cube / cube-array views. Ours: views per resource type (`get_view`). | NXN |
| 2.9 | **LOD**: TSC min/max LOD clamp, LOD bias, `TWO_D_NO_MIPMAP` texture type, a min filter without mips stays on the base level; TIC carries its own max anisotropy and aniso quality fields. | `minLod`/`maxLod`, `mipLodBias` limited by `maxSamplerLodBias`. | yuzu: anisotropy heuristics for mods forcing trilinear (YZ-2306, PR 10744). Ours: non-mip filter → `maxLod = minLod + 0.25`, bias clamped (`vk_texture.c` `make_sampler`). | G80TEX |
| 2.10 | **Texture gather rounding**: Maxwell sub-texel precision differs from AMD/Intel; edge texels picked by TLD4 differ by one. | Implementation-defined `subTexelPrecisionBits`. | yuzu adds a sub-pixel offset to gathers on non-NVIDIA hosts: fixed BotW shadow artifacts at object edges ("shader_recompiler: Add subpixel offset for correct rounding at ImageGather", <https://git.openpunk.com/OpenPunk/yuzu/src/commit/780240e6979b198e7bd10feaad5399b8b4b63762>, title only). Ours: the translator biases gather coordinates by the host's `subTexelPrecisionBits` on every non-NVIDIA GPU (`vk_caps.c` → job `gather_bias` → Ryujinx `TexturePass.InsertCoordGatherBias`); TotK on RADV drew dashed dark lines along wall and floor edges in the prologue caves without it. `TOTK_GATHER_BIAS=0` turns it off. | — |
| 2.11 | **Gather offsets**: `TLD4.PTP` takes four per-texel offsets, possibly from registers. | `ConstOffsets` must be constant. | Ours: `QueryHostSupportsNonConstantTextureOffset() => false` makes Ryujinx emulate register offsets (`tools/xlate/Program.cs`). | RYU-SH `Translation/Optimizations` (TexturePass) |
| 2.12 | **Depth compare** (`TSC` depth compare + `TIC_4_DEPTH_TEXTURE`), min/max reduction filtering. | `compareEnable`; reduction mode needs `samplerFilterMinmax` and is not allowed with compare. | Ours: reduction dropped when compare is on (`vk_texture.c` `make_sampler`). | G80TEX |
| 2.13 | **ASTC / BC**: native ASTC; zero ASTC block = error colour on hosts. | AMD and NVIDIA desktop GPUs lack ASTC: decode to RGBA8. yuzu offered ASTC recompression to BC1/BC3 for low VRAM (YZ-2305, PR 10398). | Ours: native upload where the device samples ASTC, else a compute-shader decode (`vk_astc.c`, `TOTK_ASTC`), CPU decode (`tex_astc.c`) as the fallback. | `runtime/vk_astc.c` |
| 2.14 | **Linear CPU-written textures** (`FLAGS_LINEAR`, small ramps, glyph caches): CPU writes visible to the GPU directly. | Must re-upload or use linear images over host memory. | NXN Mario: sky black without its 64×1 RGBA16F ramps. | NXN |

## 3. Shader math

| # | Maxwell | Vulkan / AMD | Emulators / ports | Source |
|---|---|---|---|---|
| 3.1 | **FMNMX and `.SAT` with NaN return the other operand / 0** (IEEE minNum). Games rely on this to stop NaN spreading (one NaN pixel blown up by bloom or temporal passes flickers or fills the screen). | `GLSL.std.450 FMin/FMax/FClamp`: result undefined when an operand is NaN (NMin/NMax are the NaN-dropping forms). AMD hardware min/max usually drops NaN; the compiler may still assume no NaN. | Ryujinx emits FMin/FMax/FClamp (`CodeGen/Spirv/Instructions.cs:336, 956, 972`; saturate = FClamp 0..1, `Instructions/InstEmitAluHelper.cs:419-423`). NXN translated shaders use `isnan` selects; MNG measured Metal min/max/saturate drop NaN (`probe.m:1-4`). | GLSL450; NXN |
| 3.2 | **FMZ multiply mode** (`FMUL/FFMA/...` field `Fmz`, `.FMZ`): 0 × anything = 0, including Inf/NaN (DX9 rule). | IEEE: 0 × Inf = NaN. | Ryujinx decodes `Fmz` (`Decoders/InstDecoders.cs:306-309, 1901`) but no emitter reads it. Ours: same. Census needed (check 13). | RYU-SH |
| 3.3 | **Denormals**: per-instruction `.FTZ`. | AMD flushes fp32 denormals by default; `VK_KHR_shader_float_controls` sets it per shader. | Ryujinx ignores `.FTZ`. Low risk. | RYU-SH (no `Ftz` use in `Instructions/`) |
| 3.4 | **Warp size 32**: `SHFL`, `VOTE`, `S2R LANEID`, ballots are 32-bit. | AMD RDNA runs wave32 or wave64 per shader (the driver chooses; RADV fragment default wave64); `VK_EXT_subgroup_size_control` `requiredSubgroupSize = 32` fixes it. | Ryujinx adapts lane masks to `QueryHostSubgroupSize()` (default 32, `IGpuAccessor.cs:218`; `Instructions/InstEmitWarp.cs:72, 106`, `InstEmitMove.cs:175-185`). Ours: not overridden, no required size (check 6). | <https://docs.mesa3d.org/envvars.html> (`RADV_PERFTEST=pswave32`) |
| 3.5 | **Discard (`KIL`)** keeps the quad's derivatives valid (demote semantics). | `OpKill` terminates; later derivatives in the quad undefined. | Ryujinx emits `OpKill` (`CodeGen/Spirv/Instructions.cs:508`). | — |
| 3.6 | **Implicit-LOD sampling in divergent control flow** works on Maxwell (TEX unit uses quad neighbours). | Undefined in non-uniform control flow; AMD whole-quad mode usually works. | — | VKSPEC (derivatives) |
| 3.7 | **Integer division** does not exist (compiled to `XMAD`/`IMAD` sequences); MUFU.RCP(0) = +Inf, LG2(0) = -Inf. | Same IEEE edge cases through `1/x`, `Log2`. | Low risk. | — |
| 3.8 | **FP64** in some TotK cutscene shaders. | No FP64 on some hosts. | yuzu FP64 → FP32 (YZ-2306, PR 10693); ours: `softfloat64` job flag (`tools/xlate/Program.cs` `QueryHostSupportsShaderFloat64`). | — |
| 3.9 | **Integer vertex inputs read as raw bits** (bone indices `R8UI`). | Float-declared inputs convert integers. | NXN Mario: stretched characters; re-translated with `uint` inputs. Ours: attribute types passed to `QueryGraphicsState` (`Program.cs`). | NXN |

## 4. Memory model

| # | Maxwell / Tegra | Vulkan / AMD discrete | Emulators / ports | Source |
|---|---|---|---|---|
| 4.1 | **One memory**: the CPU reads GPU-written buffers and textures after a fence; the GPU sees CPU writes after `FlushMappedRange` or, in practice, immediately (TotK skips flushes). Block-linear layout is defined, so a buffer copy into texture storage is a texture upload. | Images live in VRAM with private tiling; buffer bytes copied into image memory are not texels. | yuzu PR 10942 (small CPU writes); MNG alias visibility barriers for copies through two buffers over one allocation. Ours: GPU-written ranges of CPU-visible pools marked at fence completion; GPU-only pools nothing (check 8). | MNG `docs/how-it-works.md:67-71` |
| 4.2 | **Counter reports** (`ReportCounter`, `SET_REPORT_SEMAPHORE_A..D` 0x1b00): `{counter, timestamp}`; TIMESTAMP puts 0 in the counter; SAMPLES_PASSED counts visible samples; ZCULL stats. | Occlusion queries are separate objects; results copied to a buffer. | NXN TotK: timestamp counter 0 matters for conditional clears. Ours: every report writes `{0, time}`; occlusion counts are always 0 (check 5). DK3D writes `SET_REPORT_SEMAPHORE_OFFSET` (`0x6C0` = 0x1b00) with payload `0` and `OPERATION=Counter`, `STRUCTURE_SIZE=FourWords` (`dk_variable.cpp:193-240`): TIMESTAMP = unit `Crop`, counter `Payload`, written when the ROP has retired every earlier draw; SAMPLES_PASSED = unit `Crop`, counter `0x15`; ZCULL stats = four one-word reports at +0, +4, +8, +12. TIMESTAMP_TOP is not a 3D report: a gpfifo `Semaphore` release of 16 bytes with `ReleaseWfiDisable`, written when the command processor reaches it, without waiting for earlier work (`:246-253`). So `TIMESTAMP - TIMESTAMP_TOP` around a span is its GPU duration. Layout: two 64-bit words, counter then timestamp (`Primer.md:578`). Each report is followed by a tiled-cache flush (`:256`). `ResetCounter` (`0x54C` = 0x1530) zeroes SAMPLES_PASSED; it is the reset for the timestamp types too (`:282-300`). Unit: `dkTimestampToNs(ts) = ts * 625 / 384`, i.e. **614.4 MHz** GPU ticks (1.63 ns), not the 19.2 MHz system tick (`include/deko3d.h:1367-1373`) (check 21). | OGD 0x1b00; NXN; DK3D `dk_variable.cpp:183-300`, `include/deko3d.h:1367` |
| 4.3 | **Semaphores / events** written by the GPU in order; CPU polls them. | Writes on a completion thread after the fence. | Ours: `vk_completion_add`, submit order. | — |
| 4.4 | **Inline uniform updates** (`UpdateUniformBuffer`) happen in command order between draws; reading the buffer at record time gives the last value. | Must snapshot. | NXN: snapshotted at encode time; Mario's wave sim diverged when two writes reordered. | NXN |
| 4.5 | **GPU-written index buffers** (8-bit indices written earlier in the same submission). | No `uint8` index type without `VK_EXT_index_type_uint8`; CPU widening reads stale bytes. | NXN widens on the GPU (`wide_run`). Ours: CPU widening at record time; device-local byte indices skipped (check 9). | NXN |
| 4.6 | **Compute shared memory**: 48 KiB per block on GM20B. | AMD 64 KiB. | Ours: `QueryComputeSharedMemorySize` from job (`Program.cs`). | OGD `clb1c0.h` |
| 4.7 | **Virtual memory pools** map physical pool pages; the same bytes reachable at two GPU addresses. | Two `VkBuffer`s over one allocation need alias-aware barriers. | MNG `MTL4VisibilityOptionResourceAlias`. | MNG `docs/how-it-works.md:67-71` |
| 4.8 | **Command list calls**: a `CallCommands` / `CallList` is not a GPU subroutine. DK3D `dkCmdBufCallList` stores a `Call` control entry (`dk_cmdbuf.cpp:327-338`); `dkQueueSubmitCommands` walks the entries on the CPU and recurses into the called list at submit (`dk_queue.cpp:287-296`), emitting gpfifo entries that point at the command words, which the GPU fetches later (prefetch can be switched off per entry, `:176`). So list structure is fixed at submit, command words are read when fetched. | n/a | Ours: `OP_CALL` is walked at replay, `CallCommands` inlines the recording at record time (`vk_exec.c` `walk`, `nvn_api_cmd.c` `inline_recording`). Equivalent for control flow; deferred bind handles are the data read late (2.4). | DK3D `dk_cmdbuf.cpp:327`, `dk_queue.cpp:280-298` |

## 5. Fixed function

| # | Maxwell / NVN | Vulkan / AMD | Emulators / ports | Source |
|---|---|---|---|---|
| 5.1 | **Provoking vertex: last** (GL convention, NVN default; `SET_PROVOKING_VERTEX` 0x1684, NVN 55 `SetProvokingVertexMode`). Flat-interpolated outputs (instance/particle ids, material indices, flat normals) come from the last vertex. | **First vertex** unless `VK_EXT_provoking_vertex` with `provokingVertexMode = LAST_VERTEX`. | Ours: last-vertex mode where `provokingVertexLast` is available (`vk_pipeline.c` `create_graphics`; check 1). DK3D: `ProvokingVertexLast` (`0x5A1`) is set to 1 at queue setup and per rasterizer state from `DkProvokingVertex` (`maxwell/gpu_3d_base.cpp:76`, `gpu_3d_state.cpp:67`); `DkRasterizerState` defaults to Last (`include/deko3d.h:823`). Confirms last as the hardware-side default. | OGD 0x1684; `runtime/nvn_ext.c`; DK3D `include/deko3d.h:794-823` |
| 5.2 | **Primitive restart**: any index value, compared against the raw index for any index size; can apply to lists (`SET_DA_PRIMITIVE_RESTART_TOPOLOGY_CHANGE`) and to non-indexed draws (`..._VERTEX_ARRAY` 0x0de8). | Restart only at all-ones of the index type; lists need `primitiveTopologyListRestart`. | Ryujinx enables it for draw-arrays only on GL (RYU-SU comment). NXN: TotK terrain strips use 0xffff. Ours: non-all-ones restart not replayed, logged. DK3D: `PrimitiveRestartEnable` (`0x591` = 0x1644) + `PrimitiveRestartIndex` (`0x592` = 0x1648), index written only when enabled; any value; `PrimitiveRestartWithDrawArrays` (`0x37A` = 0x0de8) is forced 0 at setup (`maxwell/gpu_3d_ibo.cpp:21-30`, `gpu_3d_base.cpp:73`). Nothing on lists vs strips beyond that. | OGD 0x1644, 0x1648; DK3D `maxwell/gpu_3d_ibo.cpp:21-30` |
| 5.3 | **Point size**: `SET_POINT_SIZE` (0x1518) when the shader does not write it; GLSLC clamps a written size to a driver constant. | Point size must be written by the shader. | Ryujinx writes the `PointSize` from graphics state at VS start (`Translation/EmitterContext.cs:84-88`); ours reads the `SetPointSize` state from the support buffer (translator patch 0008). | NXN |
| 5.4 | **Line width**, smooth lines (`SET_LINE_WIDTH_FLOAT` 0x13b0, `SET_ANTI_ALIASED_LINE`). | `wideLines`, `VK_EXT_line_rasterization`. | Ours: 1 px, logged. | OGD |
| 5.5 | **Polygon offset units** are twice the API value in the register; NVN API takes GL units. | `depthBiasConstantFactor` in format-dependent r. | Ryujinx `units / 2f` from the register (RYU-SU). Ours takes NVN API units (right) but D32F instead of D24 (1.7). DK3D `constantFactor * 2.0f` (1.7). | OGD 0x15bc, 0x187c; DK3D `maxwell/gpu_3d_state.cpp:154` |
| 5.6 | **Dual-source blending**: SRC1 factors read the shader's second output. | Needs fragment output `Location 0, Index 1`. | Ryujinx declares `Index` only when `DualSourceBlendEnable` (`Translation/Translator.cs:196`, `CodeGen/Spirv/Declarations.cs:608-617`). Ours: SRC1 factors mapped (`vk_pipeline.c` `blend_factor`) but the translator is always given `false` (`Program.cs` `QueryGraphicsState`) (check 12). | — |
| 5.7 | **Alpha-to-coverage** with dither (`SET_ALPHA_TO_COVERAGE_DITHER_CONTROL` 0x12e0), alpha test (fixed function on Maxwell). | A2C without dither; no alpha test. | Ryujinx emulates alpha test and A2C dither in the shader from graphics state; ours passes `AlphaTestOp.Always`, A2C false to the translator (`Program.cs` `QueryGraphicsState`). | OGD 0x12e0, 0x16b4 |
| 5.8 | **Logic ops** on colour targets. | `logicOp` feature; ignored on float targets. | Ours: in the pipeline key. | OGD 0x19c4 |
| 5.9 | **Vertex formats**: `*SCALED` 8/16-bit, RGB10A2 SNORM/SCALED, 3-component formats, 32-bit scaled. | No 32-bit SCALED; 3-component formats often unsupported. | Ours: 32-bit scaled read as integers (logged), 3-component widened (`vk_pipeline.c` vertex format table). | — |
| 5.10 | **Transform feedback**, rasterizer discard. | `VK_EXT_transform_feedback`. | Ours: rasterizer discard without XFB = no draw (`nvn_api_cmd.c`). | OGD 0x0744 |
| 5.11 | **Depth bounds, sample mask, conservative raster**. | Optional features. | Ours: ignored, logged (`nvn_ext.c`). | OGD 0x0fa4 |
| 5.12 | **Blend factors on targets without alpha** (RGBX emulated as RGBA). | Destination alpha reads the stored value. | Ryujinx replaces DST_ALPHA factors on no-alpha targets (`FilterBlendFactor`, RYU-SU). | RYU-SU |

## 6. Known BotW / TotK issues in other projects

| Game | Symptom | Cause | Fix / link |
|---|---|---|---|
| TotK | 3D scene black | conditional clears ignored | NXN |
| TotK | cave magenta / black | CPU-filled ASTC/BC with no flush never uploaded | NXN |
| TotK | stone blocks unlit by torches | roughness atlas built from `ReadTexels` of never-uploaded BC4 | NXN |
| TotK | BC4 3D texture draws lost | compressed render target | NXN |
| TotK | missing ambient light, bloom halo | draw with no render target skipped | NXN |
| TotK | warp pad half black | block height rounding | MNG `docs/how-it-works.md:49-53` |
| TotK | coloured squares with Ultrahand / Recall | D16 clear vs fragment rounding with depth EQUAL | MNG `docs/how-it-works.md:57-61` |
| TotK/BotW | grass flat away from Link, no cut | custom border colour (0.5) | MNG `nvn_mtl.m:5041-5045` |
| TotK | cloud flicker, missing geometry in the Depths | shader recompiler bugs | YZ-2305 PR 10234 |
| TotK | red dot on screen | clear targeted the wrong RT index | YZ-2305 PR 10243 |
| TotK | scrambled Link/terrain at 2× | buffer-cache clears during sync | YZ-2305 PR 10249 |
| TotK | vertex explosions (RADV) | driver dynamic depth clamp bug | YZ-2305 PR 10262 |
| TotK | pixelated Ultrahand glow | attachment feedback loop without barriers | YZ-2305 PR 10402 |
| TotK | areas black for a frame on camera changes | texture-cache alias/overlap order | YZ-2305 PR 10418 |
| TotK | Gloom textures on terrain | block-depth adjustment on 3D slices | YZ-2305 PR 10433 |
| TotK | terrain gaps at distance (Depths) | one-character shader recompiler error | YZ-2305 PR 10459 |
| TotK | mangled textures on RDNA3 | not found (AMD test case) | YZ-2305 |
| BotW/TotK | slow rain/snow, particle corruption on teleport | small CPU writes into GPU-used pages not invalidated | YZ-2306 PR 10942 |
| TotK | AO broken above 1008p mods | rescale thresholds | YZ-2306 PR 10675 |
| TotK | "detective mode" Link on AMD | AMD EDS3 colour-blend driver regression | YZ-2306 PR 10946 |
| BotW | shadow artifacts at object edges (AMD) | gather rounding | yuzu commit 780240e (2.10) |
| TotK | dashed dark lines along wall and floor edges, changing with the camera angle (AMD, RADV) | gather rounding | gather bias (2.10) |
| BotW | black textures on GCN | HTILE (depth compression) | `AMD_DEBUG=nohyperz` wiki (1.9) |
| BotW | 3D scene black | deferred texture handles | NXN |
| BotW | albedo scattered squares | duplicate colour attachments | NXN |
| BotW | HUD dark boxes, black stamina wheel | capture texture swizzle rebuild | NXN |

## 7. Checks for our runtime

Grep-level look on 2026-10-22, statuses updated 2026-10-26; "saw" is what the grep and short reads showed, not a test result.

1. **Provoking vertex.** Vulkan draws flat outputs from the first vertex, NVN from the last. `vk_pipeline.c` enables
   `provokingVertexLast` and chains `VkPipelineRasterizationProvokingVertexStateCreateInfoEXT{LAST_VERTEX}` in
   `create_graphics`; without the feature it logs that flat outputs come from the first vertex. **Fixed.**
2. **Unwritten vertex outputs.** `spirv_add_outputs` (`vk_pipeline.c`) adds outputs initialised to (0, 0, 0, 1) for
   FS inputs the VS lacks. Our translator never links stages, so Ryujinx's `InitializeVertexOutputs` would initialise
   no user output; translator patch 0006 initialises the unlinked outputs. **Fixed.**
3. **Masked clears.** `vk_exec.c` skips colour clears with a partial channel mask (`W_CLEAR_MASK`); `clear_ds` skips stencil
   clears with a partial mask (`W_STENCIL_MASK`). TotK issues such clears (NXN). Check the log for
   `colour clear with channel mask` and `stencil clear with mask`. DK3D (1.3) confirms what the replay must do: colour clear = scissor 0 + channel mask, stencil clear = `value & mask` merged under the front write mask; so a draw with `colorWriteMask` / `stencilWriteMask` set to the clear mask is the exact equivalent. **Missing.**
4. **D24 → D32F.** `vk_formats.c` maps D24S8 to `D32_SFLOAT_S8_UINT`; `vk_exec_draw.c` `record_fixed` passes NVN units
   unscaled. Depth EQUAL passes and decal offsets can z-fight. Look at: per-pass depth-test op histogram, EQUAL passes
   against D24 targets. **N/A for TotK**: a census of the field and fight saves (2026-10-22) saw depth targets
   0x34 (DEPTH32F) and 0x32 (DEPTH16) only; biased draws (units -5 and 1, factor -0.5 and -0.2) are on DEPTH32F.
5. **Counters and conditional rendering.** `vk_exec_xfer.c` (as of 2026-10-22): every report writes `{0, time}`;
   SAMPLES_PASSED is always 0; a condition is read on the CPU at record time and an unfinished report reads 0. Grep the
   NVN log for counter types other than TIMESTAMP; any occlusion-driven visibility (flares, culling) is off. **Fixed 2026-10-22**:
   SAMPLES_PASSED counts come from Vulkan occlusion queries between RESET and REPORT (`vk_exec_xfer.c`, ~4 queries
   per frame in the field, ~21 in a fight); the game's CPU reads them (guest f_70010c3298) to cull objects.
6. **Subgroup size.** The translator assumes 32; RDNA4 may run fragment shaders in wave64. `vk_caps.c` and
   `vk_pipeline.c` `pin_subgroup32` pin stages to 32 lanes with `VkPipelineShaderStageRequiredSubgroupSizeCreateInfo`
   where `subgroupSizeControl` allows it, and otherwise tell the translator the GPU's own size
   (`TOTK_VK_SUBGROUP_PIN=0` forces that). **Fixed.**
7. **Images aliased onto GPU-only pool memory.** `vk_texture.c` `bind_pool_memory` binds each `VkImage` into its pool
   allocation; textures are keyed by address plus descriptor, and each new image starts `UNDEFINED` (`init_image`). Two descriptors over the
   same storage, or bytes written through a buffer, do not see each other's data on AMD. Check: count distinct keys per
   address per frame. **Risk** (yuzu PR 10418 class).
8. **GPU writes into CPU-visible pools.** `note_gpu_write` (`vk_exec_xfer.c`) stamps pages when the fence
   completes, so a texture sampled later in the same submission keeps its old upload; GPU-only pools are skipped.
   MNG saw at most one buffer copy per frame in TotK, so low priority. **Risk.**
9. **Index buffers.** `vk_exec_draw.c` `op_draw_idx`: byte indices widened on the CPU at record time, skipped in
   device-local memory; `restart_for`: a non-all-ones restart index logged and dropped. Census (2026-10-22): 130-170 byte-index draws/frame widened on the
   CPU, 0 device-local, 0 non-all-ones restarts. **OK**; `VK_KHR_index_type_uint8` (supported) would drop the CPU cost.
10. **Feedback loops.** Census (2026-10-22): 425/frame (field) to 667 (fight), G-buffer material/decal shaders
    sampling the 1600x900 targets of the open pass. The terrain G-buffer pass reads its own albedo, normal and R32F
    targets at the fragment's pixel; on the RX 9070 the reads returned stale data as ~16x14 px dark rectangles on the
    ground, and the sage spirit next to Link was not drawn. `vk_exec_draw.c` (feedback loops) now detects such a draw:
    with `VK_EXT_attachment_feedback_loop_layout` and `dynamicRenderingLocalRead` the target moves to the feedback
    layout and a by-region barrier precedes the draw; without them the pass restarts behind a full barrier
    (`TOTK_FEEDBACK`). **Fixed.**
11. **Border colours.** `vk_texture.c` `make_sampler`: custom colour only with `customBorderColorWithoutFormat`, else transparent
    black; integer formats use the float path. Confirm the RX 9070 reports the feature (grass map needs 0.5).
    **OK if supported.**
12. **Dual-source blend.** `vk_pipeline.c` `blend_factor` maps SRC1 factors; `Program.cs` `QueryGraphicsState` always passes
    `dualSourceBlendEnable = false`, so no `Index 1` output exists. Log pipelines with SRC1 factors. **Mismatch if used.**
13. **FMZ / NaN.** Count `FMUL/FFMA/FMUL32I/FFMA32I` words with the `Fmz` field set (`Decoders/InstDecoders.cs:1901`
    bit positions) in `build/v121/shaders/`; FMin/FMax/FClamp NaN results are undefined (3.1). Census (2026-10-22):
    FMZ in 1 instruction of 12397 shaders, FTZ in 12241 shaders, FMNMX in 7457. The translator emits GLSL.std.450
    FMin/FMax (Ryujinx `GenerateMinimum`), not NMin/NMax. **FMZ: no fix needed. FMNMX: OK** (RX 9070 test: FMin/FMax(NaN, 1) = 1 both orders, FClamp(NaN, 0, 1) = 0).
14. **Point size / line width.** Census: `SetLineWidth 4.5` ~1/frame (2 line draws into a 144x144 R8 target),
    `SetPointSize 5` ~14/frame; 4 point VS rely on it. Point size: translator patch 0008 reads it from the support
    buffer, **fixed**. Line width: 1 px, non-default values logged (`nvn_ext.c`), **mismatch, minor**.
15. **Block height.** `nvn_tex.c` truncates like NVN (2.1). DK3D rounds up (`dk_image.cpp:446`); every other step matches. **OK.**
16. **Depth mode and clamp.** `vk_pipeline.c` `create_graphics` (`negativeOneToOne` from `nvn_depth_minus_w`, `depthClampEnable`
    from NVN state). DK3D agrees on the z maths (1.5) and the clamp switch (1.6). **OK.**
17. **Depth bias argument order.** `vk_exec_draw.c` `record_fixed` (units, clamp, factor); DK3D takes the same three and doubles units for the register (1.7). **OK.**
18. **Sampler LOD.** `vk_texture.c` `make_sampler`. **OK.**
19. **Viewport / scissor arrays.** Single viewport (`vk_exec_draw.c` `record_fixed`), extra scissors logged. Census: 0 differing. **OK.**

20. **Depth-compare sampling in compute** (VUID-vkCmdDispatch-None-06479): one compute shader samples a 4x4 R32F
    CPU-written placeholder through an R32_SFLOAT view with compare on, ~1/frame. Needs a D32 copy. **Minor.**
21. **Report timestamps: unit and instant.** `write_counter` (`vk_exec_xfer.c`) writes host `ns * 12 / 625` (19.2 MHz
    ticks) at fence completion for every report; `nvnDeviceGetTimestampInNanoseconds` converts back with the same factor
    (`nvn_ext.c`). DK3D: GPU ticks convert at 625/384 ns (614.4 MHz), TIMESTAMP is written when the ROP has retired
    the earlier work, TIMESTAMP_TOP when the command processor reaches it (4.2). Two consequences for the engine's GPU time (dynamic resolution and speed follow it):
    (a) if the 1.2.1 consumer scales raw report ticks with its own constant rather than the NVN conversion call, our
    19.2 MHz unit is 32x off from the hardware's; find that consumer before fixing the unit; (b) the delta needs
    `vkCmdWriteTimestamp2` with `TOP_OF_PIPE` for TIMESTAMP_TOP and `BOTTOM_OF_PIPE` for TIMESTAMP, per submission
    segment, converted to whichever unit (a) shows. **Mismatch (instant); unit unverified.**

## 8. Sources not read yet

- Mesa NVK (`src/nouveau/vulkan`, MIT): how it maps Vulkan provoking vertex, depth clip, restart, feedback loops onto B197.
- envytools rnndb `graph/gf100_3d.xml`, `tsc.xml`, `tic.xml` (MIT): field-level TIC/TSC meanings (LOD snap, trilinear opt).
- Ryujinx `Ryujinx.Graphics.Gpu/Image/TextureCache.cs`, `Texture.cs` (MIT): alias/overlap rules, depth texture swizzle.
- Ryujinx TotK/BotW PRs and issues (GitHub repo gone; mirrors blocked or 404 on 2026-10-22).
- yuzu progress reports 2023-08 to 2024-02 and suyu/sudachi trackers (behaviour only).
- switchbrew wiki NVN and GPU pages.
