# Reduced-resolution SR with FG producer audit, 15 November 2026

## Scope and source baseline

This source/CPU audit starts from TotK rsf 1806b94 with the clean RSF main pin
620aee7. The coordinator owns all GPU runs. The latency worker owns the mapper's
HUD-less final-fence correction and XeSS-side refusal facts; the shader worker
owns snapshot prerequisites. Neither mapper/presentation code, pinned RSF code,
vendor policy nor the camera ABI was edited for this audit.

The general shader audit remains `docs/rsf-shader-audit.md`. Static and camera
geometry, independent object/bone histories, storage-buffer resolution,
translucency/particles and bloom/exposure/DoF/reflection/volumetric consumers
remain distinct coverage questions. Source contracts and CPU fixtures below do
not prove moving-game material coverage or image quality.

## Traced contracts

`vk_dlaa.c`, `size_for` allocates RG16F motion and R32F device depth at the actual
scene dimensions, an RGBA16F reconstruction output at the game composition output
size, and a stable allocation generation. `scene_record` uses those same scene
and output extents for its camera record. `rsf_scene` supplies RGBA16F colour at
the scene extent, R32F depth, RG16F previous-minus-current motion in render pixels,
unit motion scale and current jitter in render pixels. Native providers and the
FSR4 split route plan the required input size from the selected output/quality;
their query is authoritative, including vendor-specific integer rounding.

A scene differing from the accepted plan posts an exact resize request through
resolution.c and returns pending. It must not evaluate with fabricated dimensions
or rescale only a descriptor. The asynchronous game-thread mailbox checks settings
version and heap bounds, updates the scene patch and preserves guest registers.

FG preparation copies the same source identity/camera record, retains actual
scene-sized guides, and reports host WSI output size e->fw/e->fh. The existing
mapper then remaps actual guide extents into the observed host crop/fit rectangle:
it samples depth without pretending it is output-sized, scales pixel motion by
the coordinate transform and updates existing camera/jitter fields consistently.
HUD-less colour is captured later at raw game composition e->gw/e->gh after the
known final FXAA/scaling draw, with real sRGB encoding and byte-preserving UNORM
copy. Presentation wrappers apply crop/fit/bars once; game, scene, SR output and
host WSI extents are separate facts.

Native-resolution SR writes the scene target before downstream game passes.
Reduced SR evaluates into the game scaling target and replaces later samples of
the original scene via vk_dlaa_swap_img/view. Bloom/postprocessing/tonemapping
retain their native scheduling. Their numerical behavior at each scale, plus
auxiliary depth/exposure consumers, still needs captured evidence.

## Implemented corrections

1. Exact SR plan versus game DRS: the game can choose 100/90/80 percent tiers even
   after the SR provider planned one fixed size. `res_dynres_live` now enables the
   existing fixed-tier patch while SR is requested on an active live-resolution
   owner. It does not force native size: quality/balanced/performance still use
   the provider plan. FG-only/SR-off restores the requested DYNAMIC_RES behavior.
   `res_present` applies this ownership on the game-thread boundary too, including
   activation/reset without a fresh window-side config poll. Disabled SR cannot
   consume an old provider mailbox request.
2. Accepted FG ownership: successful fg_prepare now records D.fg_last independently
   of D.last. The latter is updated only after SR completion or guide-only work;
   using it for HUD-less capture lost already accepted FG preparation when SR
   returned pending. Capture now uses the exact accepted Size and its source ID.
   Checked freeing/release clears that pointer. No provider/backing lease was
   shortened or replaced by a latest-frame guess.
3. Reduced SR scaling boundary: previously D.up skipped the known output scaling
   draw, so the after-actual-draw HUD-less hook could never observe it. With FG
   selected that recognized scaling draw remains and samples the reconstructed
   output through the existing scene-sample replacement. Without FG the prior
   skip remains. This preserves the real final boundary instead of copying an
   earlier linear scene image and calling it final encoded colour. Additional
   FXAA/filter appearance requires visual acceptance.
4. Mode lifecycle: quality/backend changes invalidate SR owners/history and stop
   inherited camera/jitter state. SR disable restores the requested base scene
   scale through res_dlss_mode("off"); reenabling reapplies the selected quality.
   The live quality changes are supported only inside the existing heap/live
   scene budget. BACKEND and ENABLED remain restart-labelled settings; this
   defensive history correction does not claim arbitrary live SDK switching or
   live heap growth for sessions started without an SR budget.
5. Jitter/history: scene_record now carries the previous recorded pixel jitter
   in the existing SDK field and zeros it on history reset. The first disabled
   or failed-SR frame cannot inject leftover SR jitter through the viewport hook.
   FG guide production remains active if SR fails while FG is requested; sampler
   bias then stops and SR retries only after a mode change. The selected scene
   allocation/budget remains intact until the requested mode is disabled/reset.

6. Texture LOD follows the negotiated scene/output ratio, including provider
   rounding, rather than the initial menu heuristic. The sampler bias is refreshed
   when the accepted plan changes even if the menu value did not. FG-only and
   failed/disabled SR retain zero injected bias.

No camera fields or ABI versions were added. Material reactivity, transparency
and the separate DLSS character history mask retain their existing contracts.

## Bounded producer evidence

The XeSS combined run `build/checks/rsf-fullscreen-xess-auto-latency.log` reports
42 preparations and 36 HUD-less tags, with no 300-frame SR completion line; the
same source-1800 log has active XeLL sleeps/markers and no latency refusals.
That does not identify the exact producer gate at runtime. Source proof exposes
the pending/accepted-owner and DRS conflicts above, but this note does not claim
they alone explain every missed later frame.

Existing RT_TRACE_PRESENT now emits at most one producer line per Present, on a
state transition or every 300 presents. It includes source, armed/done/ended,
actual scene/output extents, negotiated provider input extent, camera validity,
SR outcome and final draw/hash/capture gates. There is no new diagnostic setting
or per-draw log stream.

Stage bits: 1 evaluation point reached, 2 FG prepare accepted, 4 guide-only work,
8 SR recorded, 16 spatial fallback. Reasons: 0 no refusal (stage zero also means
no evaluation point), 1 size/allocation lease unavailable, 2 motion/projection/
camera invalid, 3 SR pending including plan convergence, 4 SR failure, 5 checked
owner retirement pending. Camera bits 1/2/4 identify valid motion/projection/
matrices independently. HUD-less gates: 0 no candidate, 1 no matching accepted
source/already captured, 2 target shape/format mismatch, 3 unknown final shader,
4 host map mismatch, 5 accepted colour, 6 SDK colour refusal. The latency worker's
mapper diagnostics distinguish busy backing from these upstream facts.

## CPU checks and remaining acceptance

Implementation was batched before focused checks, as requested. The repeatable
CPU-only entry point is `pwsh -NoProfile -File tests/rsf_reduced_quality_check.ps1`.
It builds complete vk_dlaa.c, resolution.c and rsf_sr.c with UCRT64 Clang using
production-style flags and -Wall -Werror, then runs six CPU executables under
build/checks/rsf-reduced-quality-worker. Tests execute the actual mode selector,
FG preparation, scaling-skip decision, SR scene adapter, resolution mailbox/DRS
patch controller, temporal admission and viewport-jitter functions. They cover
quality/balanced/performance provider plans across DLSS, FSR3, XeSS and split
FSR4; different integer plans per provider; pending then exact admission; guide
and output formats/extents; unit pixel motion scale; accepted FG while SR is
pending; quality/backend reset; SR-off base/DRS restoration; and independent FG
admission with failed/disabled SR unjittered.

Existing tests/rsf_split_record_check.py preserves its allocation-generation and
current/previous-jitter regressions, while adapting to the coordinator's recorder
API extension: exact split final fence 100 and actual recorder final fence 91,
independent of SDK signal 41. It verifies actual-success-only snapshot submission
notifications, failed/queued-before-execution suppression, asynchronous packet
copies and the direct submit path. The updated FG resize preparation fixture
also checks its independent accepted owner. These focused CPU executables pass.
After the coordinator's motion-texture header appeared, the final batch compiled
all three complete source objects successfully and the focused CPU regressions
passed. No GPU/game/UI, shared build, staging, commits, remote action or pinned
checkout edit was performed.

Final device acceptance should cover the supported providers at reduced scene
resolution, matching source/guide/HUD-less identities and real generated output.
Check quality changes and history resets, ordinary presentation when unsupported,
and the existing crop/resize/HUD boundaries. SDK refusal is a recorded limitation,
not successful provider acceptance. Moving object and effect quality still needs
relevant scene evidence.

The coordinator's combined Quality runs completed with no Vulkan validation
errors, shader failures or skipped draws. DLSS and FSR3 both sustained SR and FG.
XeSS sustained preparation and HUD-less admission with working XeLL pacing, but
SR stayed pending: the recommended 1130x636 plan became1128x632 in the game's
aligned renderer. That concrete input-size mismatch is the current repair;
these runs do not establish XeSS generated output or texture-history quality.
Logs remain ignored under build/checks/rsf-final-*-quality.log.

Owned edits: runtime/vk_dlaa.c; coordinator-authorized runtime/resolution.c;
tests/rsf_resolution_check.c; tests/rsf_fg_resize_check.py;
tests/rsf_split_record_check.py; new tests/rsf_reduced_quality_check.py and
.ps1; this note. No mapper/gfx presentation or snapshot implementation was edited.

## XeSS recommendation versus engine grid correction

The later coordinator XeSS-quality run `build/checks/rsf-final-xess-quality.log`
proved the remaining pending loop: the SDK recommendation was 1130x636, while
real game allocations were 1128x632. Requests now round up to the engine's
1.4.3 eight-pixel grid, distinguish queued transition from already-current plan,
and allow the latter to reach normal provider validation using actual descriptor
and camera dimensions. Unsupported nearest sizes fail explicitly. The sibling
XeSS service now retains its actually queried min/max, validates real inputs in
that range, allocates maximum private backing and passes active input extents to
execution while preserving the recommended plan. It has no engine alignment rule.

The focused CPU variant exercises the real XeSS evaluator with inert dispatch;
its synthetic test bounds are not live device evidence. The optional existing
frame log now exposes genuine range facts once for the coordinator's next run.
Full commands, exact owned hunks and the preserved pre-existing camera ABI hunks
are in RSF's docs/research/vulkan-xess-input-range-20261115.md. One final focused
batch passed the sibling archive build and two XeSS CPU contracts, plus the three
TotK complete-source compiles and relevant actual-function regressions. No worker
GPU/game/UI run or pinned edit was performed.

The live device fixture reports minimum equal to recommendation, so normalization
now rounds 1130x636 up to 1136x640. The original observed 1128x632 remains below
that lower bound. The adapter still reports real image extents and lets the
provider enforce the SDK maximum; heap overflow after alignment refuses.


## Width-derived allocation correction

The subsequent real run `build/checks/rsf-final-xess-range.log` preserves the
failed independent-axis correction: page request 1136x640, actual scene1136x632,
SR PENDING reason3 through source1800. This supersedes the earlier claim that
independent ceil8 dimensions were achievable. It is not a floating-page rounding
hypothesis or evidence of XeSS frame generation acceptance.

Read-only generated 1.4.3 source establishes the conversion. In
main_0062.c, f_7000585bd0 at main+0x585e48 stores page floats into renderer+0xcc4
and +0xcc8, but f_7000580ff0's block main+0x581a0c computes renderer+0xccc from
**width** (+0xcc4) times the dynamic tier divided by console base (+0xcc0).
The existing fixres patch at main+0x5848a8 makes the tier1 for the SR owner.
The per-view values +0x3c30 and +0x3c34 both receive that scale (main+0x5816dc).
main_0008.c f_70000d5c24 multiplies each canvas axis by its corresponding scale,
divides by allocator alignment (+0x3c38), FRINTM floors each axis, and multiplies
back. With the observed eight-pixel alignment and a1920x1080 canvas,
1136/1920*1080=639, flooring to632;1144 yields643.5, flooring to640.
Page height is stored but does not drive this scene allocation path.

res_rsf_scene now chooses the first aligned width at or above the recommendation
whose derived allocation satisfies both recommended axes, using binary32
operations matching that path. It requires a width fixed point so writing the
accepted width back cannot quantize again; existing startup heap/16-bit bounds
refuse overflow. The accepted scene for1130x636 is1144x640, not1136x640.
res_present retains game-thread mailbox ownership, res_sizes reports actual
achievable dimensions, and the adapter still gives truthful extents to the
provider's queried-range validation. Unsupported sizes explicitly fail there;
this does not introduce a vendor alignment rule or guarantee SDK acceptance.

Focused CPU verification uses tests/rsf_resolution_conversion_check.py. It
extracts the real generated width-scale and allocation-rounding blocks into an
ignored, marked build fixture, executes them with the real runtime FP helpers,
and invokes the actual resolution owner/mailbox fixture. It reproduces1136x632,
asserts1144x640 after request application and no repeated impossible request,
and covers height-driven requests, changed canvas aspect, handheld scale,
heap refusal, stale settings, register preservation and DRS ownership. This
is a source/CPU conversion contract, not complete guest FP or GPU acceptance.


Final focused commands/results (all CPU, isolated outputs):

- `python tests/rsf_resolution_conversion_check.py`: passed actual mailbox tests,
  read-only generated conversion blocks with runtime FP helpers, and complete
  `resolution.c` object compile (`-std=gnu11 -O2 -g -Wall -Werror
  -fno-strict-aliasing -march=x86-64-v3 -D_WIN32_WINNT=0x0A00 -DRT_CPU_LEVEL=3`).
- `python D:/Coding/ReScaleFrame/tests/native_sr_xess_range_check.py`: passed
  actual common validation/backend-dispatch prefix with inert capture/backend
  callbacks. It proves1144x640 reaches XeSS, backend refusal propagates, malformed
  record/camera/guide/mask/format/layout contracts refuse, and DLSS/FFX still
  require1130x636. It does not stand in for the real SDK range evaluator.
- Complete sibling `native_sr_vulkan.cpp` compiled successfully with UCRT64
  `clang++.exe -std=c++20 -O1 -Wall -Werror -Wno-unknown-pragmas
  -DRSF_HAVE_FFX_VK=1 -DRSF_RUNTIME_BUILD -DWIN32_LEAN_AND_MEAN -DNOMINMAX`,
  output `D:/Coding/ReScaleFrame/build/checks/worker-srvk-xess-range/native_sr_vulkan.o`.
  Includes: runtime/orchestrator/include, sdk/game/include, runtime/contract/include,
  runtime/backends/{fsr/include,fsr/src,dlss/include,xess/include},
  runtime/{common,graphics,shader_patch}/include, ui/overlay/include,
  references/godot/fidelityfx-vulkan/ffx-api/include,
  references/godot/engine/thirdparty/vulkan/include, and the read-only generated
  headers in build/worker-fsr4-sr-20261115/generated. Earlier fixture import and
  include-path setup failures were corrected; no compile/test failure remains.
- Scoped `git diff --check` passed in both repositories (only line-ending notices).

The coordinator subsequently measured real1144x640 in
`build/checks/rsf-xess-convergence/game.log`,1915 real presents, zero validation,
shader or skipped-draw errors, and322 generated callbacks by source1800 after
SR failed and disabled itself. This is FG-after-SR-failure evidence, not combined
XeSS SR+FG acceptance. The remaining common wrapper gate was identified before
backend invocation: its images/masks/record were all required to equal the SDK
recommendation. Authorized sibling correction now uses actual record dimensions
for XeSS only, after checking record pointer/size, retaining all common contract
checks and letting XeSS enforce its queried range. No public ABI/layout changes.
The coordinator owns the published CMake build/pin and combined hardware check;
actual min/max log and sustained SR plus FG remain to be recorded there.

This final task changes TotK runtime/resolution.c, tests/rsf_resolution_check.c,
new tests/rsf_resolution_conversion_check.py and this note; sibling changes are
runtime/orchestrator/src/native_sr_vulkan.cpp and new
tests/native_sr_xess_range_check.py. No generated file, index, GPU state, pinned
checkout or unrelated dirty work was changed by the worker.
