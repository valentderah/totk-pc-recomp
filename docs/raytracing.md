# Ray tracing

Hardware ray-traced ambient occlusion, sun shadows, one-bounce global illumination (GI), reflections and a night
tone, on GPUs with Vulkan ray queries. Everything is off by default; the settings are the `rt_*` keys of `totk.ini`
(described in [totk.ini.example](../totk.ini.example)) and the "Ray tracing" group at the top of the menu's Screen
tab.

Code: `runtime/vk_rt.c` (scene, passes, settings), `runtime/shaders/rt_*.comp`, `runtime/gfx_albedo.c` and
`runtime/gfx_prog.c` (materials), hooks in `vk_contact.c`, `vk_exec_draw.c`, `vk_pipeline.c`, `gfx_shader.c`,
`vk_dlaa.c`, `vk_device.c` and `vk_texture.c`.

## Requirements and gating

- `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations`, the
  `bufferDeviceAddress` feature and `VK_EXT_transform_feedback`. `VK_KHR_ray_tracing_position_fetch` is enabled when
  present but not needed.
- The extensions are enabled at device creation only when a ray tracing setting is on at the start (`vk_rt_wanted`).
  With all of them off, or on a GPU without the features, nothing of this runs: no extension, no pipeline variant, no
  buffer, no pass, and the frame path takes the same branches as without this code (`vk_rt_on` is 0).
- Once on at the start, every setting switches live from the menu (`vk_rt_live`, polled every 250 ms); all off stops
  the capture and every pass and gives the game its own AO and shadows back.
- While ray tracing runs, the contact shadows are off: the ray tracer takes their hook points (`vk_contact_plan`,
  `vk_contact_exec`).
- Video memory at 1080p with everything on: about 1.1 GB (buffers and acceleration structures; the log line
  `vk: ray tracing memory` lists each allocation site and how full each fixed capacity got).

Developed and measured on one GPU, an RTX 4050 Laptop (6 GB). Other GPUs and vendors are untested.

## The scene

The recomp replays the game's draws without knowing what they are, and triangles take their final shape inside the
game's vertex shaders. The ray tracer therefore captures what the game draws:

1. **Capture.** G-buffer draws and the sun's shadow-cascade draws get a transform feedback variant of their pipeline
   (`vk_pipeline_xfb`, built in the background; a draw without its variant ready is not captured that frame). The
   vertex position goes to transform feedback buffer 0; with materials on, buffer 1 holds the albedo UV and texture
   layer and buffer 2 the varyings of the draw's fragment shader program (step 6 below). `vk_exec_draw.c` brackets
   each captured draw (`vk_rt_xfb_begin` / `vk_rt_xfb_end`) and records the geometry key, culling and material words.
2. **Unprojection.** `rt_unproject.comp` turns the captured clip-space corners into world space through the inverse
   view-projection of the camera the draw used (the replay's copy, `vk_usnap`). It leaves out triangles whose shader
   culled a corner, the far terrain's kilometre-long ones and those beyond the traced range.
3. **Mesh cache.** Static meshes are kept between frames in 64 m cells (`rt_cache.comp`, `cache_upkeep` in `vk_rt.c`):
   a mesh not drawn this frame stays in the scene while it is out of the view (up to about 600 frames), while it is
   still drawn into the sun's cascades, or for 3 frames when it is in the view and not drawn (occluded, or replaced by
   another level of detail). Each cell has its own BLAS in a pool; the frame's own triangles go into a capture BLAS,
   rebuilt each frame, and one TLAS holds both.
4. **Lights.** The game's lights are not separate objects. The GI finds them as glowing pixels: the G-buffer shaders'
   glow target is copied before the composite adds the lighting (`emit_copy`), and a world-space light cache keeps
   what was seen recently.

Only what the game drew recently near the camera is in the traced scene: something never drawn cannot cast or
bounce.

## Passes in a frame

In replay order:

| When | Pass | What it does |
|---|---|---|
| G-buffer draws | capture | as above |
| before the quarter-resolution lighting | `rt_shadow.comp` (first) | sun shadows from last frame's scene, so the quarter-resolution lighting reads this frame's shadow |
| before the half-resolution lighting | scene build, `rt_shadow.comp` (second) | builds the scene with all of this frame's draws, traces the shadows again; the game's lighting shaders are patched (`gfx_shader.c`, `patch_rt_shadow`) to read the traced shadow where it is traced, in place of the cascade compare |
| before the composite | `rt_ao.comp`, `rt_aoinj.comp` | AO traced at half resolution and written into the game's own AO target (the wide SSAO term), so the composite applies it as its own |
| after the composite | `rt_gi.comp` | one-bounce diffuse GI, the sky visibility that dims the ambient light, reflections, and the night tone's measurement |
| before DLSS / the bloom (`vk_dlaa.c`) | `rt_late.comp`, `rt_tone.comp` | the appearance measure that feeds the light cache (minus what the GI itself added), and the night tone |

Each pass that writes a game image marks it (`vk_texture_storage_written`) as the contact shadows do.

## Denoising

- AO: half resolution, cosine-weighted rays, the result reprojected into last frame's history and clamped to the mean
  of this frame's neighbourhood (5x5, same depth) +- 1.5 standard deviations, over up to 24 frames.
- GI: a deterministic traced pixel per block (no per-frame jitter, which flickered), temporal accumulation with a 3x3
  variance clamp, and light sampling from tiles of aggregated lights (RIS, at most 64 candidates).
- Reflections: traced at half resolution, a normal averaged over same-plane neighbours (the DLAA jitter rocked the
  reflection vector), a variance-clamped history that shortens when the reflection changes fast, and an
  edge-aware, roughness-gated a-trous blur.

## Materials (`rt_mat`)

Ray hits that are off the screen or hidden need their surface's colour. Without `rt_mat` they take it from a coarse
world cache of earlier frames. With it:

1. **Find the albedo texture and its UV** (`gfx_albedo.c`): a backward slice of each G-buffer fragment shader's albedo
   output (colour target 1) finds the texture sampled, the UV varying and the texture-array layer. A texture counts
   only when all three of its colour channels reach the output, so single-channel masks and two-channel normal maps
   are never taken as albedo.
2. **Capture the UV**: transform feedback buffer 1 records each corner's UV and layer.
3. **Material table and bindless textures**: each captured draw's albedo view goes into a descriptor array; a
   material word per triangle names it.
4. **Shade hits** (`rt_gi.comp`): albedo at a mip chosen by the hit distance, lit by the sun (with a shadow ray) and the
   sky, matched per material to how the game showed it on screen (a constrained, non-negative channel fit).
5. **Use it**: off-screen and hidden hits take the material result; visible hits keep the screen.
6. **The game's own shaders at hits** (`gfx_prog.c`): where a texture lookup is not enough (glowing, animated or
   procedural materials), the G-buffer fragment shader's albedo and glow outputs are compiled to a small register
   program that `rt_gi.comp` runs at the hit (`prog_run`; the opcodes of `gfx.h` `GP_*` and the shader must stay in
   step). Its varyings are captured per corner (up to 12 slots, transform feedback buffer 2).
7. **The material targets**: the G-buffer's target 3 (smoothness) and target 0 (shading class) decide where
   reflections go and how strong they are.

## Quality levels (`rt_quality`)

| Level | Shadows | AO rays | GI ray length | Reflection ray length |
|---|---|---|---|---|
| low | to 60 m, alternate pixels | 1 | 15 m | off |
| medium | to 90 m, alternate pixels | 1 | 20 m | 40 m |
| high (default) | to 120 m, every pixel | 2 | 30 m | 60 m |
| ultra | to 160 m, every pixel | 4 | 40 m | 80 m |

Each `rt_*` setting still overrides what the level picks.

## Measured cost

RTX 4050 Laptop (6 GB), Ryzen 5 8645HS, 1080p, DLAA, quality ultra, every feature on, frame limit 60:

| Place | fps | Ray tracing GPU time a frame |
|---|---|---|
| Lookout Landing, camera orbit | ~47 | trace ~7 ms |
| Kakariko inn at night | ~39-43 | trace ~7 ms, scene build 0.2-1 ms |
| Zora's Domain at night, camera orbit | ~42-48 | trace ~7-8 ms, scene build 1-2.4 ms |

The full overlay (`perf_overlay = full`) shows "RT scene build (GPU)" and "RT trace (GPU)" while ray tracing runs;
`TOTK_RT_PROF=1` logs the GPU time of each dispatch.

## Known limits

- **Clouds still shade the world.** The game's drifting cloud shadows (over Death Mountain, for example) take away
  direct sun and every shadow for a while, with ray tracing off too.
- **HDR output is untested** (needs a display with Windows HDR on). The passes work on the scene before tone mapping.
- **Caves**: a mesh the game stops drawing behind mist or an overhang while its box is still in the view is dropped
  after 3 frames (there are no sun cascades to keep it), so a reflected rock can pop out of a water reflection on a
  small camera move.
- Cut-out foliage is solid to the rays; `rt_shadow_only = 0` keeps the game's shadow map for it.
- Fixed capacities are sized for a 6 GB GPU; they do not scale with the VRAM budget yet.

## Debug switches (environment only)

| Variable | What it does |
|---|---|
| `TOTK_RT_VIEW=<n>` | debug views: 4 the captured scene by primary rays, 20 GI alone, 23 GI lights, 26 material split, 29-31 the game's shaders at hits, 32 the glow copy, 33 the veil |
| `TOTK_RT_PROF=1` | GPU milliseconds per dispatch in the log |
| `TOTK_RT_SHADOW_LIT=1` | the traced shadow forced lit (separates shadow problems from the rest) |
| `TOTK_RT_LATE_SUB=0` | the appearance measure keeps what the GI added (the old feedback loop, for comparison) |
| `TOTK_RT_KEEPCAS=0` | meshes drawn only into the sun's cascades are not kept |
| `TOTK_RT_GI_GLOW=0` | the GI's lights by brightness over albedo instead of the glow copy |
| `TOTK_RT_TEST=1` | the first test pass: hard sun shadow and AO from the captured scene, in place of the contact shadows |
| `TOTK_RT_LIVE_SET="<s>:KEY=V,...;..."` | sets those settings (TOTK_ prefixed, never saved) that many seconds after start, as the menu would, to test live switching |
| `TOTK_RT_SURVEY=1` | logs what the game draws per pass (no capture) |

Every other `TOTK_RT_*` read in `vk_rt.c` is a tuning or debug knob documented at the line that reads it.
