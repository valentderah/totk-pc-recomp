# ReScaleFrame integration

The requested replacement covers super resolution, frame generation and low latency
through ReScaleFrame for NVIDIA, AMD and Intel. Implementation is in progress. The
renderer's temporal evaluation now exclusively uses RSF. The direct NGX core
loader, vtable ABI, feature creation and evaluation have been removed. The legacy
DLAA/DLSS setting names remain until menu/config migration is complete.

Apply RSF's shared `game-render-analysis` integration skill, including its required
shader-input audit. Current TotK coverage and missing inputs are recorded in
[the shader audit](rsf-shader-audit.md); they remain part of this integration's
completion requirements, independent of user-reported visual symptoms.

`runtime/rsf_runtime.c` now provides opt-in module loading and validates the required
native Vulkan exports before exposing them. `runtime/vk_device.c`, `create_instance`
calls this before Vulkan initialization. This is a module lease only; SDK setup and
the rendering replacement below remain pending.

`runtime/rsf_sr.c` now binds the typed SR session API from the installed SDK and
handles creation, planning, recording, command retirement and destruction. Failed
cleanup preserves ownership for retry. It passes UCRT64 Clang syntax checks against
the actual SDK. The adapter is connected to `evaluate`, but has not yet been exercised
on the GPU. Builds consume external SDK headers through `TOTK_RSF_SOURCE`, defaulting
to the pinned `external/ReScaleFrame` submodule; no RSF headers are copied into this repository.
Initialize it with `git submodule update --init external/ReScaleFrame`.
`TOTK_RSF_SOURCE` can point to a development checkout. The gitlink pins
[RSF commit 1753d4d](https://github.com/KillerPixelCrew/ReScaleFrame/commit/1753d4d),
published on RSF's `main` branch.

The recorder and direct submission prologue now retire RSF per-command resources
after beginning a new recording of a GPU-completed buffer. Session registration,
evaluation, retirement and destruction are serialized so the recorder cannot walk
a freed owner. The compiled `tests/rsf_sr_lifetime_check.c` fault-injection test
passes with two providers, failed evaluation, refused destruction and failed
planning followed by cleanup retry. It uses stub providers and proves ownership
handling only, not SDK recording or Vulkan validation.

Run that test with UCRT64 Clang, compiling `tests/rsf_sr_lifetime_check.c` and
`runtime/rsf_sr.c` together with `-I runtime` and the three RSF SDK include
directories listed in `tools/build_game.py`. Place the executable under
`build/checks/`. The recorder, submission synchronization and adapter sources also
pass UCRT64 Clang syntax checks with those SDK and VMA include paths.

The RSF DLL previously omitted 21 native Vulkan exports because its archive link
retained only internally referenced objects. The local RSF checkout now links
`rsf_native_vulkan` with `WHOLE_ARCHIVE` into the runtime DLL. Its Release build
passes the new `native_vulkan_dll_exports` regression test, and a separate DLL
probe resolves all 22 exports required by this loader. These changes live in the
RSF checkout and must accompany the eventual package; they are not vendored here.
This proves export availability only, not ABI setup or GPU provider operation.

`runtime/rsf_vulkan.c` now initializes the shared Streamline session before instance
creation when DLSS SR is selected. Instance/device creation and proc lookups from
the renderer go through that session's resolver. Non-DLSS SR keeps normal loader
dispatch. The real `tests/rsf_vulkan_startup_check.c` probe now passes against the
isolated RSF build and corrected runtime: Streamline initializes, NVIDIA plugin
signatures verify, Vulkan instance creation succeeds, and shutdown succeeds.
The earlier shared build returned NOT_SUPPORTED because its implementation had
been compiled out without the corrected receipt.
This does not establish FG WSI ownership or a low-latency input-event producer.

The device creation path now offers RSF a writable, rebased copy of the renderer's
feature chain, merges its required extensions, and reports the successful create
info. Device destruction notifies RSF. The startup probe now exercises real device
creation too: instance and device creation succeed; latency device registration
reports NOT_SUPPORTED on the probed adapter. SDK shutdown before device destruction
passes. The earlier order (device destruction before SDK shutdown) triggered an SDK
shutdown exception. Renderer teardown now drains/releases SR owners and shuts RSF
down before destroying Vulkan owners, retaining them if checked cleanup fails.
These are device-lifecycle results, not evidence of working low-latency markers.

`runtime/rsf_present.c` now provides a checked provider-owned WSI adapter for
creation, image enumeration, acquisition, presentation, enable options, depth/
motion preparation, HUD-less recording and successful submission notifications.
It retains partial owners and drain/destroy failures for cleanup retry; a close
attempt prevents further WSI calls. Its compiled fault-injection fixture
`tests/rsf_present_lifetime_check.c` passes those cases, including submission facts
after failed recording. Build the fixture with `runtime/rsf_present.c`, the runtime
and external SDK includes, and UCRT64 Clang; run it under `build/checks/`.
The window now uses this adapter's creation, image enumeration, acquire and
presentation calls when a configured FG provider creates an owner. Its final image
layout comes from that owner. Successful host-present submissions are reported to
RSF. Resize and shutdown drain SDK work, finish GPU use, retire engine views and
close the same owner; cleanup failure prevents device destruction. Partial owners
never fall through to native WSI. An upfront unsupported/missing provider with no
retained owner may keep native presentation. HDR currently keeps native WSI.
Generation is explicitly disabled until input-frame identity, normalized FG inputs
and tonemapped HUD-less capture are connected. Menu FG controls remain unavailable.
Object compilation and the ownership fixture pass. Initial FSR 3 and DLSS provider
WSI runs now pass as described below; generated output remains unvalidated.

When a nonzero `rsf_fg` is requested, device selection prefers a graphics/compute/
presentation family with four queues. Device creation requests and retrieves four
distinct queue handles for graphics, compute, presentation and acquisition. If the
family has fewer queues, it keeps ordinary rendering with one queue and logs that
FG cannot use that family; it never fabricates four owners from one handle. WSI
provider creation and queue callbacks still need connecting, so FG remains disabled
in the menu. The device source passes UCRT64 object compilation.

The pinned corrected Streamline SDK now builds successfully under
`build/rsf-sdk-host/artifacts/rescaleframe/streamline/Release/win-x64/runtime`.
The original NVIDIA plugins are staged there from the SHA-512-pinned 2.14.1
archive in RSF's Godot dependency lock. The corrected common/interposer binaries
and their receipt remain intact. An RSF build enabling this implementation completed under `build/rsf-sdk-host/rsf-native`, isolated from the shared RSF
build directory after that build failed to open a native Vulkan object at link.

The experimental scene path uses `rsf_backend` and `rsf_provider_dir`, converts
scene colour into its own RGBA16F image, transitions colour/depth/motion to sampled
layouts and output to transfer-destination, drains the recorder, calls RSF, then
restores GENERAL layouts and invalidates draw bindings even after refused recording.
The provider's planned render size must match the actual scene; mismatches are
queued for the 1.4.3 game-thread render-size hook when the startup heap budget
permits them. Evaluation waits without skipping the native scaling draw. Older
versions or dimensions outside the budget are refused. The requested quality is
passed directly to RSF rather than guessed from an old scene's width. Native now
requests output resolution, and the live heap reserves space for that mode.
Settings-version changes invalidate queued resize requests. Camera/depth must be valid for that evaluation.
This path currently reports a renderer-local identity with NO_FG and AMBIGUOUS_ID;
it does not claim input-frame ownership or authorize generation. Provider setup
and evaluation still need game validation. DLSS early Vulkan initialization,
FSR4 split submission, negotiated scene resizing, UI and packaging remain pending.

## Integration boundaries

The camera motion dispatch now also writes an R32F device-depth image at the
render size. Each size owns that image, its view and allocation; eviction drains
GPU work before freeing them. Its contents are valid only after `mv_pass` returns
success for the current evaluation. A valid camera without history now dispatches
identity reprojection, writes current depth and zero camera motion, skips object
history replay and resets reconstruction history. A missing camera still skips
the dispatch, so those frames cannot consume this depth output.
The shader compiled and passed SPIR-V validation; the changed C files passed
UCRT64 Clang syntax checks. GPU output has not yet been checked in TotK.

`runtime/rsf_camera.c` decodes near/far, reversed/infinite depth and vertical FOV
from the recorded c4 projection block. Unsupported projections are refused. The
focused compiled test checks finite, reversed and infinite projections and refusal
without overwriting a prior valid result. The motion pass captures these values
from its current uniform snapshot for the forthcoming provider record.

Run the camera test from the repository root (output stays under ignored `build/`):

```powershell
New-Item -ItemType Directory -Force build/checks | Out-Null
C:/msys64/ucrt64/bin/clang.exe -std=gnu11 -Wall -Wextra -Werror -I runtime tests/rsf_camera_check.c runtime/rsf_camera.c -o build/checks/rsf_camera_check.exe
build/checks/rsf_camera_check.exe
```

- `runtime/vk_dlaa.c`, `evaluate`: retain scene recognition, jitter and camera/object
  motion production; replace NGX feature creation and evaluation with RSF SR sessions.
  RSF SR ABI 2 requires sampled inputs and a transfer-destination output. Its DLSS
  path requires R32F depth and transfer-source inputs. Existing D32F depth therefore
  uses the new conversion output rather than a cast of its format.
- `runtime/vk_device.c`: initialize the shared RSF Streamline session before Vulkan
  instance creation and route Vulkan dispatch through its resolver. Select SR and FG
  independently. The existing single graphics queue is insufficient for RSF's FFX
  presentation owner, which requires four distinct queues in the same family.
- `runtime/gfx_present.c`: route swapchain creation, images, acquire, submit and
  present through the selected RSF FG owner. Preserve synchronization with the
  renderer's queue submissions. An RSF chain must never reach another WSI owner.
- Frame generation needs depth/motion preparation plus a tonemapped scene capture
  before HUD composition. The SR input alone does not satisfy that contract.
  Notify RSF of successful submissions and retain inputs until GPU completion.
- Low latency needs actual input/simulation/render/present boundaries and a shared
  source-frame identity. Assigning a new frame number at present cannot establish
  these boundaries. Pacing must happen before input sampling.
- Retire recorded commands after GPU completion. Drain provider work before resize,
  backend replacement and destruction; failed destruction retains ownership for retry.

## Source reuse

The local RSF source is `D:\Coding\ReScaleFrame`. Its native Vulkan orchestrator
headers declare GPL-3.0-only. Do not copy these headers or implementation into this
repository under its source reuse policy. The game SDK is MIT but does not itself
provide the native Vulkan provider interface. Any independently written interface
must be checked against the actual runtime exports and ABI layouts.

## Completion evidence

Package discovery now checks `ReScaleFrame/ReScaleFrame.Runtime.dll` beside the
game when `rsf_runtime` is unset. Each selected backend independently resolves its
package directory (`streamline`, `fidelityfx`, `xess`), allowing mixed SR/FG vendors
without forcing one SDK directory on both. Explicit runtime/provider overrides
remain available. The compiled `rsf_package_paths_check.c` fixture passes absent
and present packages, all backend mappings and override precedence. This does not
prove that a complete distributable package or its provider DLLs are present.

The corrected FidelityFX Vulkan SDK was built under
`build/rsf-sdk-host/artifacts/rescaleframe/fsr3-sdk/Release/win-x64` using RSF's
guarded build recipe and pinned source. This supplies the native Vulkan DLL
required for FSR2/3 device evaluation; it does not establish successful evaluation.

`tools/stage_rsf.py` stages the locally built runtime, corrected Streamline and
FidelityFX Vulkan binaries, vendor libraries and licenses beside a development
game build. Supply `--runtime`, `--streamline`, `--streamline-source`,
`--fidelityfx` and `--out`; `--source` defaults to the pinned `external/ReScaleFrame` submodule.
Output must stay under `build/`. Before writing, it checks every required input,
native DLL headers, pinned SDK revisions and corrected binary receipt hashes,
and rejects unexpected stale DLLs. Its manifest records all staged SHA-256 hashes.
The local package contains 27 verified files. A mismatched FidelityFX receipt was
rejected before output creation. This tool prepares development validation only;
release packaging and provider acceptance remain outstanding.

The UCRT64 Clang v1.4.3 game build succeeds. Shader preparation translated 29,587
variants from 53,437 seed records with zero translation failures, then warmed all
29,587 variants into the driver cache with zero failures. These checks establish
translation and pipeline preparation, not correct shader inputs in gameplay.

The first full-renderer boot exposed an enumeration bypass: it called the Vulkan
loader directly while creating devices through Streamline. Streamline records
physical-device/instance ownership during enumeration, so its device startup
received a null instance dispatch table and crashed in `sl::chi::Vulkan::init`.
Physical-device enumeration and instance destruction now use the RSF resolver.
The startup fixture uses these same entry points. The real SDK/device fixture and
the game's `--boot` checkpoints pass after this correction. With the shader census
present, the kernel, Vulkan draw, texture and shader probes all pass too.

A subsequent run reached the opening scene and completed 1,657 presents with no
reported shader failures or skipped draws. DLSS creation was initially refused:
NGX rejected the plain-name project ID. Its shipped `nvsdk_ngx.h`,
`NVSDK_NGX_Init_with_ProjectID` contract requires a GUID-shaped custom-engine ID.
The adapter now uses a stable project UUID. Boot no longer logs NGX initialization
failure and scene evaluation reaches the SR recorder, which still reports
NOT_READY (-8): `native_latency_vulkan.cpp`, `rsf_slvk_invoke` cannot find a matching
CPU-created frame token. The real input/simulation identity and token path remains outstanding;
this run is not successful SR acceptance. No gameplay motion coverage is inferred
from the opening scene.

`TOTK_TRACE=present` now also logs bounded input-poll and guest-present boundaries
(first 32, then every 600), including guest thread, caller and host timestamp.
The first four input samples include a checked, bounded guest frame-pointer walk.
No input values are logged. In the opening-scene run, the connected pad was polled
by `main+0x47afa8` on a different guest thread from `main+0x2dc340` presentation.
The first two input polls precede the first present; later samples and presents
can interleave differently. Their counters cannot identify matching frames.
The poll's caller chain reaches the controller collector, input-manager update,
task calculation traversal and framework calculation (`main+0x2dad80`). Further
work must establish the framework's transfer to draw work and propagate the source
identity through queued NVN commands; reading a global "latest input" on replay
would incorrectly associate asynchronous frames.

The light-message observers in `rsf_frame.c` preserve the SDK operation and trace
bounded messages per guest-thread/queue pair under `TOTK_TRACE=present`. The draw
thread receives its work through the Sead thread loop (`main+0xacf89c`); matching
blocking sends come from the input/calculation thread through
`main+0x59a688`, called from `main+0x2db62c` under the framework pre-calculation
stage (`main+0x2dab78`). The subsequent calculation stage is called at
`main+0x2dab88`, entering `main+0x2dad80`. In the captured run, the first two sends
each preceded the corresponding next pad poll. The message is the constant 1,
not a unique frame identifier. Queue addresses are runtime owners, not portable
addresses. Any source metadata must follow publication/consumption order and the
completed calculation, rather than assigning the next input sample to received work.

The 1.4.3 patch table now brackets the framework calculation traversal, after
task setup and before its guard is released. Each hook performs its displaced
instruction. `rsf_frame.c` opens a source identity at that verified calculation
boundary, reports simulation traversal begin there, reports the first connected
controller read on its guest thread, and reports calculation completion. Input
polling and presentation cannot open identities. The input manager runs inside
calculation: RSF accepts its input sample after simulation begin, preserving the
actual order. Repeated reads are deduplicated per update. An update without a
connected read reports no invented input; the core owns any substitute. SDK calls
run without the frame-state lock or a held virtual guest core.

The compiled `tests/rsf_frame_check.c` fixture verifies distinct source IDs,
register preservation for both displaced instructions, repeated input reads,
unrelated guest threads, missing input and abandoned traversal handling. The game
build, selftest and configuration probe pass. The configuration probe now accounts
for the detected display default on a non-16:9 monitor; its previous assumption
that an empty Windows environment assignment suppresses that default was false.
The settings documentation gate also passes after documenting the new hook switch
and the existing climbing fix.

The real Streamline run reports accepted RSF CPU records with stage mask 0xf,
no failed ownership and no order violations. This establishes CPU facts only.
Renderer-local SR records use a separate session namespace until the source is
copied through the draw queue and Vulkan replay, preventing an accidental numeric
match with CPU token IDs. Draw publication/consumption, rendering and final Present
association are still outstanding, so DLSS evaluation and FG remain unverified.
The longer trace also exposes task migration: later controller polls execute on
guest worker threads rather than the framework calculation's caller. At source
1,200, RSF had accepted 1,200 updates with zero order violations and 867 explicitly
substituted input boundaries. These are not 867 missing physical polls: the current
thread-local association cannot attach those worker polls to their parent update.
The task-owner handoff must carry the source into workers before latency acceptance.
The run completed 1,549 presents without reported shader failures or skipped draws.

Worker association now follows the copied calculation context rather than the
worker thread or a global latest update. The verified pad collector and controller
manager frames identify the two worker callback variants. Their saved x19 points
to the job context; its second word points to InputManager's configuration at
object +0x480. InputManager's task link at +0xa8 leads to the framework owner at
+0x20, the same owner captured by the framework calculation hook. A unique active
traversal must match that owner. Unknown call sites and ambiguous owners refuse
association. The earlier idea of recovering the framework from saved x22 was
rejected: InputManager overwrites x22 during its calculation.

The lifecycle fixture now checks a worker context, rejection of a different caller,
register preservation and deduplication after worker association. The real run
completed 1,551 presents; at source 1,200 the core reports zero order violations
and zero input substitutes. The independent pad reads seen later in another call
tree remain unassociated. This corrects the worker-input gap above, not the still
missing source propagation into the draw queue and Vulkan replay.

The source now follows the framework's observed light-message queue lifecycle.
`rsf_handoff.c` keeps a bounded FIFO of sidecar source tags, including the initial
unassociated draw. A queue must be initialized and its first publication must be
the verified framework draw call. The framework, draw-thread object, publisher and
message must match. Foreign operations, overflow, mismatched messages and queue
finalization invalidate association until reinitialization. The native queue
operations still run. Consumed ownership is retired at the guest Present boundary
and on queue retirement.

NVN submission copies this source into the backend call, asynchronous replay item,
Vulkan recording context and recording-worker snapshots. SR records use CPU session
1 only when this copied source exists; unassociated records retain the separate
renderer namespace and ambiguity flag. FG remains prohibited pending its output
and final Present association. `tests/rsf_handoff_check.c` verifies initial source
zero, FIFO and wraparound, foreign publishers/operations, overflow, mismatch,
finalization and reinitialization. The frame fixture, game build, selftest, config
probe and Vulkan execution probe pass.

Guest Present now snapshots the draw owner's source before its blocking section
and retires that owner only after enqueueing the backend operation. The scalar
travels through both immediate replay and `RI_PRESENT`, then `PresentJob`, into
the pending swapchain image. `gfx_present.c`, `flip_locked` consumes that image's
source even when a resize or the next blit flushes it. Failed/skipped blits cannot
attach a new source to an older image, and device release clears pending ownership.
Direct presentation probes remain unassociated with source zero.

The asynchronous replay validation run (`build/checks/rsf-present-source-async.log`)
completed 1,775 presents and at least 300 successful SR evaluations with zero
validation errors, shader failures or skipped draws. Bounded host-Present traces
match draw-consumed sources 600 and 1,200. The default synchronous replay run also
matches those sources and completed 1,586 presents with zero validation errors,
shader failures and skipped draws. Selftest and the Vulkan execution probe pass.
Validation still uses GPL off and descriptor writes. This establishes copied
identity through actual presentation; FG input/output admission, present execution
events in the orchestrator and latency marker/pacing acceptance remain open.

Explicit execution events now reach the RSF core at actual queue submission and
Present boundaries. Recorder packets copy their source alongside command buffers;
split/chunk submissions retain their enclosing source. The final blit and Present
use the swapchain image's saved source. The SDK's new execution endpoint consumes
the existing game render pass interface and never guesses a latest render ID.
Its core owns marker mapping, deduplication and the end of the complete render
interval before Present. A synthetic test verifies rendering N+1 before presenting
N without changing N's marker identity.

`rsf_low_latency` is applied at the real input boundary with the virtual guest core
released. The core chooses the supported technology. A latency-only session loads
no DLSS SR/FG feature, so it works with upscaling off. The menu control is enabled
and documents the restart requirement. The frame fixture checks that configuration
occurs with the guest core released; configuration remains outside frame ownership
locks. Source zero never reports an invented execution identity.

The latency-only asynchronous replay run completed 1,655 presents; the combined
DLSS native-resolution/latency synchronous run completed 1,826 presents and at least
300 SR evaluations. Both had zero validation errors, shader failures and skipped
draws with GPL off and descriptor writes. At source 1,200 both reported Reflex
available and active, 1,200 accepted sleep calls, 7,193 markers, zero refused calls,
zero input substitutes and zero CPU ordering violations. This establishes accepted
NVIDIA SDK pacing/marker calls, not a measured latency reduction or AMD/XeLL hardware
acceptance. The full RSF gate retains its two Anomaly FG device-test failures;
focused latency contracts, Rust format and Clippy checks pass. See RSF's
`docs/research/vulkan-explicit-execution-20261115.md` for the shared contract.

DLSS now reaches successful SR evaluation with matching CPU tokens. A six-second
opening-scene run completed 1,788 presents and passed 300 SR evaluations, with zero
validation errors, shader failures and skipped draws using `TOTK_GPL=0` and
`TOTK_DESC_TEMPLATE=0`. The captured scene is visible. The default graphics-library
path is not validated: another run produced invalid push-descriptor messages and
a validation-layer crash, while the matching RSF-disabled run passed. That remains
an unresolved graphics-library/validation interaction, not a clean default-path
acceptance or a claim of complete motion quality.

The runtime's deliberate abrupt exit used `ExitProcess`, which invokes DLL detach
handlers. After the first successful DLSS run, SDK cleanup reached Vulkan after
the validation layer's state had been destroyed, despite a reported process exit
code of zero. `plat_exit` now uses self-termination after its existing explicit
flushes, matching its no-handler contract and the Linux `_exit` path. Microsoft
documents this distinction in [ExitProcess](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-exitprocess)
and [TerminateProcess](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminateprocess).
The subsequent successful validation run exited without the DLL-cleanup abort.
After source retirement at guest Present was added, the menu automation run
captured all nine settings tabs and completed 1,775 presents with zero validation
errors, shader failures and skipped draws under the same validation switches.

The corrected FidelityFX Vulkan runtime now executes FSR3 SR at native resolution
in the opening scene on the RTX 4070 Laptop GPU. That run completed 1,671 presents
without reported shader failures or skipped draws. A validation run with
`TOTK_DESC_TEMPLATE=0` completed 1,640 presents with zero validation errors.
Descriptor templates instead crash inside the validation layer at
`vk_rec.c`, `exec_packet`'s push-template call; a matching RSF-disabled run crashes
at the same call. This does not establish template-path validation acceptance.
The captured opening scene is visible, but moving-scene quality, object-motion
coverage and reduced-resolution operation remain unverified.

The SR adapter distinguishes transient NOT_READY from a terminal recording
failure. A pending frame retains its provider, native rendering and a history
reset, without consuming the provider's output or advancing temporal jitter.
Only status changes are logged. The lifetime fixture verifies a refused recording,
NOT_READY and subsequent success on the same owner, followed by retirement and
checked cleanup. The DLSS opening-scene run now completes with its token unavailable
without permanently disabling reconstruction. This recovery path does not provide
the missing CPU frame token.

XeSS initially refused SR creation with NOT_READY because its private SDK session
had never been initialized. The adapter now loads the RSF XeSS service before
instance creation, merges its required instance extensions and minimum API, and
merges device extensions/features into the writable creation chain. Context
retirement precedes checked XeSS shutdown; its SDK remains mapped through Vulkan
teardown under the RSF service contract. Only an enabled SR provider initializes
its SDK. The real device fixture passes XeSS and Streamline setup/teardown, and an
RSF-disabled fixture passes with a nonexistent provider directory.
The negotiated XeSS native-resolution opening-scene run completed 1,636 presents
with zero validation errors, shader failures and skipped draws, using descriptor
writes as above. This establishes initial XeSS evaluation on the RTX 4070 Laptop
GPU, not reduced-resolution, resize or temporal-quality acceptance.

Build the runtime with UCRT64 Clang, exercise each available vendor's SR, FG and
latency provider, and verify unsupported selections report their actual failure.
Check HUD composition, motion, resize, provider changes, pause/load history resets,
queue synchronization and shutdown with Vulkan validation. Confirm the old NGX
implementation is removed and configuration, menu, installer and packaging all
select and deploy RSF. Provider availability alone does not prove generated frames
were presented or latency markers reached the SDK.

The in-game Graphics page now contains an Upscaling & Framegen group with enable, provider
and quality controls, an enabled low-latency control and a disabled frame-generation
control marked unavailable. Visible controls and tooltips have no RSF branding. The DLSS model and character-mask controls have been removed
from that page. New menu text falls back to English in untranslated languages.
`rsf_enabled` and `rsf_quality` are the active keys; unset keys inherit legacy
`dlaa` and `dlss` values. Menu/C sources pass UCRT64 syntax checks. A game-rendered
menu and persistence still need runtime verification.

The compiled `tests/rsf_resolution_check.c` fixture exercises the real resolution
mailbox and `res_present`: producer-side requests do not call game code, the game
thread updates the patch page and invokes its size hook, guest registers survive
the call, stale settings requests are dropped, heap limits are enforced, and an
unchanged size does not rebuild targets. It passes with UCRT64 Clang. Build it with
`-std=gnu11 -O1 -I runtime` and link the MSYS2 static `libzstd.a`; place and run the
executable under `build/checks/`. Actual provider-driven resize in TotK remains unverified.

## Character history bias input

The input audit exposed a missing temporal input in the migration. The user's
skin-smearing report and F10 capture belonged to Skyrim, not TotK.
`vk_dlaa.c`, `mv_pass` still generated the R8 character current-colour bias mask,
but `rsf_scene` never supplied it. The RSF Vulkan DLSS service also lacked its
Streamline hint. The adapter now forwards the mask only when this frame's
G-buffer class input was valid, transitions it to sampled layout around evaluation
and restores GENERAL on every result. Mask allocations support transfer-source
usage for the SDK's private copy and bounded readback. Existing character-mask
strength and history behavior are preserved.

The separate optional RSF `bias_current_color` input preserves the distinction
between character history bias, material reactivity and transparency. Older ABI 2
frame sizes remain accepted. See RSF's
`docs/research/vulkan-character-history-bias-20261115.md` for the provider path.
RSF converts the R8 mask into its own R16F image before tagging it because the
pinned Streamline Vulkan format mapping omits R8_UNORM. The first R8-only run's
format errors disappeared after this conversion. The corrected opening-scene run
completed 1,767 presents and at least 300 successful evaluations with zero
validation errors, shader failures and skipped draws, using GPL off and descriptor
writes. The game self-test, configuration probe and four RSF Vulkan ABI/split
contract tests pass. The full RSF gate retains two Anomaly FG device-test failures.
This repairs a verified dropped input; deformation/unsupported-draw motion
coverage and moving-scene visual acceptance remain open.

## FG input production and initial WSI runs

Temporal depth/motion production now runs when either SR or FG is selected.
The previous reconstruction-only gates also covered object/bone history,
recording-worker serialization and CPU uniform snapshot shadows; all now admit
FG-only work. The history fixture verifies FG-only collection and clearing on
disable. With reconstruction off, the game keeps its own colour path and scene
scale, receives no injected jitter and gets no saved SR sampler bias. The
motion-only diagnostic skips its absent reconstructed-output allocation instead
of calling VMA with a null allocation.

The FSR 3 WSI run first exposed invalid image handles: the pinned SDK filled a
smaller array without reducing an oversized count. RSF now queries and bounds
that enumeration. The DLSS proxy required an exact-size fill call as well. Win32
surface creation now uses RSF's instance dispatch so Streamline records its HWND;
destruction follows that dispatch too. The host requires usable images before
acquisition and validates acquired indices before recording.

With generation still disabled, FSR 3 WSI completed 1,588 presents and DLSS WSI
completed 1,574 presents, both with zero validation errors, shader failures or
skipped draws. The latter had SR off and a saved performance quality selection;
the scene stayed at the game's native 1600x896 size. The FG-only input run completed
1,794 presents with the same clean validation counters. Its motion check uses zero
jitter; one moving-object sample improves median flow error from 6.36 to 1.58
pixels and fits better than camera-only motion on 93% of matches, with remaining
misses recorded in [the audit](rsf-shader-audit.md). All validation runs use GPL off
and descriptor writes on the RTX 4070 Laptop GPU, driver 617.42.

The separate frame-1600 dump confirms an output-sized FXAA/scaling result followed
by a fade/overlay draw and a copy into an sRGB target. HUD-less colour must retain
the final sRGB encoding; copying earlier linear scene values is insufficient.
That diagnostic dump emitted readback-layout validation warnings, so it is pass
ordering evidence, not a clean validation run. Actual SDK prepare/HUD-less
submission, generated output and other providers remain open. RSF's
`docs/research/vulkan-fg-image-enumeration-20261115.md` records the shared fixes.

## Explicit FG presentation identity

The pending output image now passes its calculation identity to RSF's new
`rsf_fgvk_present_source` entry point. The adapter requires that export at load
time and forwards null for an unknown source. RSF matches the prepared session,
frame, view and resource generation rather than selecting the newest prepared
frame. Preparing N+1 before presenting N therefore cannot replace N's inputs.
Unknown/mismatched sources retain ordinary output with interpolation off.

The provider lifetime fixture verifies the identity crosses the adapter intact.
RSF's native FSR 3 and DLSS owner fixtures verify both N and N+1 can present in
order after both were prepared; dispatcher checks cover all four providers.
The runtime builds and its DLL export checks pass. These synthetic checks do
not establish generated-image quality, bridge session restart behavior or HUD
separation. See RSF's `docs/research/vulkan-fg-present-source-20261115.md`.

The updated runtime's ordinary-output runs reached the world and exited normally:
FSR 3 WSI completed 1,578 presents and DLSS WSI completed 1,581, each with zero
validation errors, shader failures or skipped draws. Interpolation stayed off;
both used SR off, low latency on, GPL off and descriptor writes on the same RTX
4070 Laptop setup. Self-test, settings and Vulkan execution probes pass. The full
RSF gate ran 232 tests and retains the two recorded Anomaly FG device failures;
Rust formatting and Clippy pass independently.

## FG preparation and submission leases

The depth/motion producer now records SDK FG preparation with the same copied
camera record used by reconstruction. FG-only retains native colour and zero
injected jitter. Unknown calculation sources are refused. Preparation is
requested on the WSI owner, but per-output admission still requires the matching
submitted HUD-less image, so this does not enable interpolation or its menu
control yet.

With FG selected, the recorder's producer waits for each actual submit result
and reports successful command buffers, including split parts and prologues,
outside the queue lock. The recorder never takes the presentation lock, which
Present may hold while waiting for recorder ordering. This reduces CPU overlap
for now; no performance benefit is claimed. Provider notification and its
completion fence are separate facts.

RSF now refuses drain/destroy while SDK work remains recorded but unsubmitted,
including work from failed SDK operations. A failed swapchain retirement keeps
the owner alive for submission and a later retry. Native FSR 3 and DLSS fixtures
exercise this refusal; they distinguish partial recording from accepted inputs
and allow drain after submission/completion. The FSR 4 and XeSS paths compile,
but do not have equivalent device acceptance here. See RSF's
`docs/research/vulkan-fg-recorded-retirement-20261115.md`.

FG-only opening-scene runs accepted real SDK preparation, reached the world and
exited normally: FSR 3 completed 1,690 presents and DLSS 1,679, with zero
validation errors, shader failures or skipped draws. These runs use SR off,
low latency on, GPL off and descriptor writes on the RTX 4070 Laptop GPU.
Self-test, configuration and Vulkan execution probes pass. The full RSF gate
retains its two recorded Anomaly FG failures out of 232 tests; Rust formatting
and Clippy pass independently. HUD-less colour, generated output and resize/
backend-switch device acceptance remain open.

The combined native-resolution DLSS run completed 1,798 presents, reached at
least 300 successful SR evaluations and 300 accepted FG preparations, and exited
normally with zero validation errors, shader failures or skipped draws. All 300
SR evaluations report camera motion and the character history mask. This covers
the shared metadata path with SR, FG preparation and low latency requested
together, while generated output remains gated by the missing HUD-less input.

## HUD-less capture and first generated output

`vk_dlaa_fg_after_draw` now captures the recognized final FXAA/scaling draw's
output immediately after the actual draw and before subsequent overlays. The
snapshot requires an output-sized, single-layer float colour target and the
same calculation source as the accepted FG preparation. It ends rendering,
restores feedback-loop targets to GENERAL, performs the copy, then reopens
the native pass with LOAD and invalidates cached bindings. Unknown boundaries,
skipped FXAA draws and unmatched sources cannot authorize interpolation.

The float result is linear. A blit into RGBA8_SRGB supplies its final encoding,
then a size-compatible copy preserves those bytes in the RGBA8_UNORM image
required by the SDK. Both formats are actual allocations; a format label is
never substituted for a different image format. The source's blit capability
and destination's conversion capability are checked. The relevant Vulkan rules
are [sRGB blit conversion](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBlitImage.html)
and [size-compatible image copy](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyImage.html).
The SDK copies the result into its private input lease, and the existing producer
reports that command buffer only after successful actual submission. FG recording
temporarily uses serial replay until the chunk planner carries this draw boundary.
The draw hook caches its FG selection once per present.

The first FSR 3 run reached the world and completed 1,580 presents with real
generation callbacks and zero validation errors, shader failures or skipped
draws. The settled capture run completed 2,342 presents and reached 626 successful
generation callbacks before a controlled resize drained SDK readbacks. Three
source/output pairs at sources 1800-1802 show the torch/cave picture with similar
overall brightness and interpolated flame edges. Their mean absolute RGB
difference from the current HUD-less source is about 1.96-1.98 byte values; this
is descriptive, not a temporal-quality metric. Particle motion coverage is still
incomplete. The earlier sources 1500-1502 overlap the loading fade and produce
nearly black final output despite a bright scene-only input. The pinned FSR 3
inpainting shader already blends UI differences into this captured output; its
historical capture role `fg-generated-before-ui` does not establish absence of UI.

DLSS FG-only completed 1,820 presents with at least 300 accepted HUD-less inputs.
The combined native-resolution DLSS SR/FG capture completed 2,290 presents,
at least 300 successful SR evaluations, 600 accepted FG preparations and 564
HUD-less inputs before resize. Both
reached the world and exited with zero validation errors, shader failures or
skipped draws. However, the combined capture records only real presents at
sources 1800-1802, with SDK-generated index -1. DLSS generation is therefore
unverified despite its enabled status. That discrepancy remains active work.
A mailbox control retains the same combined native SR/FG/latency setup and also
records only real presents at sources 1800-1802. It exits normally with zero
validation errors, shader failures or skipped draws. FIFO alone therefore does
not explain the missing DLSS generated-present records. Its process ends before
an SDK drain, so this control establishes presenter metadata, not retired image
readbacks. The draw hook's per-present cached selection is active in this control.

All these runs used low latency on, GPL off and descriptor writes on the RTX
4070 Laptop GPU. Self-test, settings, provider lifetime and Vulkan execution
checks pass. Capture files and contact sheets remain in ignored
`build/checks/rsf-fg-hudless-*` directories. On resize the new swapchain safely
refuses inputs whose output extent differs, preserving ordinary presentation;
FG resize recovery is not complete. Ordinary gameplay HUD, reduced-resolution
SR, skipped/unknown FXAA paths, other FG providers and visual quality still need
acceptance. The FG menu control remains disabled.


## Post-rebase latency settings

The upstream rebase reintroduced the native `reflex` setting. When
`rsf_low_latency` is unset, that legacy setting now supplies off/on/boost as
RSF mode 0/1/2. An explicit RSF selection takes precedence, including off.
This preserves existing settings while RSF remains responsible for selecting
the supported latency technology. Removing the legacy device activation and
adding the boost menu choice are coordinated with the renderer changes.

An isolated CPU executable linked the complete production `runtime/cfg.c`
against fail-fast platform stubs and passed file-backed selection, environment
override and explicit-off precedence cases. `runtime/cfg_probe.c` carries the
regression cases for the normal `--probe-cfg` gate. The complete config and
probe sources compile with UCRT64 Clang, and `tests/settings_doc_check.py`
reports zero problems. This is CPU evidence, not a new device latency run.

The first full build after rebase stopped at two newly upstreamed
`runtime/vk_exec_probe.c` calls that omitted the adapter's source-frame
argument. Both now pass source 0 because the synthetic probe has no game source
identity; the complete probe source compiles. The full renderer gate must run
after the concurrent adapter changes settle.


## Verification of the published RSF pin

RSF `1753d4d` was built from the clean `external/ReScaleFrame` submodule,
separately from the sibling checkout's uncommitted Skyrim work. The complete
Release native build passed under MSVC 2026 in
`build/checks/rsf-pinned-native`; configured SDK header/runtime paths point to
the local vendor cache and corrected SDK receipts. The native Vulkan target
compiled the real Streamline, FidelityFX, FSR4 and XeSS implementations.
A failed yyjson download was replaced only after the existing local files
matched every SHA256 in the pinned `yyjson.lock.json`.

The serial CTest suite reported no failures: 204 tests ran and 14 were skipped
for unconfigured runtime or local fixture inputs. This includes the real DLL
export, split ABI, disabled/stub split, FSR4 argument and latency contracts.
With the staged runtime directories supplied explicitly, the three skipped
Vulkan device fixtures for FSR2/FSR3, DLSS and XeSS also passed. The other skips
remain unrun. Rust `cargo fmt --all -- --check` and workspace/all-target
Clippy with `--locked -- -D warnings` passed using an isolated target directory.
This is runtime and synthetic-device evidence, not TotK visual acceptance.

The runtime from this clean pin was staged into `build/v143-rsf/ReScaleFrame`.
Its SHA256 is `5aa52922320376b0b8bd6b391db5d625c20cbd73f2efffd81a4cbc701800a251`,
identical to the native build output. `tools/stage_rsf.py --source` pointed to
the sibling checkout solely for ignored vendor binaries and license inputs;
the first-party runtime DLL came from the pinned build above. All 27 staged
inputs passed the existing package preflight and receipt checks.


## Fullscreen provider and pixel verification

The focused fullscreen DLSS run with RSF `2d650e1` completed 1935 real
presents, with zero Vulkan validation errors, shader failures or skipped draws.
Dedicated capture-fence completion now flushes raw generated and real images
for sources 1800-1802 before process exit. The inspected pairs have consistent
brightness, scene framing and letterbox bars in the opening cave. This short,
mostly static sample does not establish interpolation quality during fast motion,
ordinary HUD behavior or outdoor shader coverage. Evidence remains ignored under
`build/checks/rsf-fullscreen-dlss-present-pixels-capture`.

The separate fullscreen FSR4 SR run completed 1571 real presents with zero
validation errors, shader failures or skipped draws. The optional provider query
reported effective version `0xf5a5ca1e01001002`, NVIDIA vendor `0x10de` and
INT8 compatibility enabled. Temporal scene evaluation succeeded. The query is
optional: missing headers/exports or a failed query report unknown and do not
change SR admission. The actual logger, complete adapter and lifetime fixtures
pass against the pinned and controlled pre-query SDK headers.

The first XeSS FG run refused HUD-less packets because their motion scales
were zero. The mapper now supplies scale one, matching the host-pixel guides
accepted during preparation; its actual packet fixture passes. A separate core
repair automatically binds the configured FG context to latency processing and
protects CPU operations with scoped borrowers. This is published on RSF main at
`620aee7`, pinned here. Its clean MSVC Release build passes, with 207 native tests
run, 11 skipped and no failures, including configured Vulkan SR device tests.
The TotK build passes. Actual fullscreen XeSS generation and pacing acceptance
is still pending; CPU concurrency tests are detailed in RSF's
`docs/research/vulkan-fg-latency-context-20261115.md`.


## Final latency ownership and menu implementation

The unused direct Vulkan Reflex implementation has been removed from the host:
no host latency sleep semaphore, vendor marker calls, latency swapchain chain,
submission-present ID extension or native driver timing query remains. Verified
engine source/input/render/Present facts continue through RSF. The remaining
acquired-image identity API is named `gfx_frame_begin`, `gfx_frame_current` and
`gfx_frame_present`; it serves queued native presentation and host timing only.
The old native-frame timing report is now labeled acquired image to host present,
without implying that it measures vendor latency. Legacy configuration aliases
still map onto the RSF controls.

Frame generation can now be selected and saved directly in Upscaling & Framegen
from an initially disabled session, with the existing restart requirement.
The hidden requirement to opt in through a config file before using that control
has been removed. All upscaling/FG/latency controls now have Russian, Brazilian
Portuguese and Simplified Chinese catalog entries alongside English.

The concurrent implementation batch is not yet validated. Per the requested
workflow, compilation and regression checks are deferred until the batch lands.
The last FSR4 FG run predates this batch: the provider refused creation with
NOT_SUPPORTED on this machine and ordinary presentation completed normally.
That is fallback evidence, not FSR4 FG acceptance.


The replacement menu also retains the live DLSS model choice when DLSS is
selected, together with the existing character-history colour bias consumed
by DLSS. Object/deformation motion remains independent of that colour bias. These controls were previously orphaned when the old rows were removed.
Their labels describe their actual scope; no claim is made that a colour bias
repairs missing object motion. The menu catalog boundary now includes the new
controls so normal catalog coverage and description-length validation reaches
them. Obsolete standalone NGX installation instructions were removed from the
legacy catalog descriptions.


Release build graph generation now requires RSF SDK headers only when compiling
the renderer. An installer linking `--runtime-lib` compiles game-derived modules
and the install-side sources, which do not include RSF service headers; it no
longer fails because the release folder lacks a development checkout. Full
renderer and runtime-library builds still require the pinned SDK. Build docs now
name the gitlink default rather than the obsolete sibling default.


## Disabled startup and checked device retirement

An installed package no longer activates SDK setup or device requirements when
SR, FG and low latency are all off. The adapter tracks whether it actually loaded
runtime services, independently of whether setup completed, so partial setup can
still run checked cleanup. Only a completed shutdown and destroyed Vulkan
instance permit resetting setup for another initialization.

The Vulkan device owner now retains its context when presentation, SR or SDK
shutdown refuses retirement. A later initialization first retries that teardown;
it cannot zero the retained device or create another one over it. Successful
teardown clears readiness, while deferred teardown keeps the context available
to outstanding callbacks. The CPU regression extracts the actual init/shutdown
state machine and exercises refused cleanup, partial creation failure and retry.
It passes in the combined validation batch.


Texture snapshot integration now has concrete device and submission hooks.
The pool starts off recording threads after VMA and the live device exist, when
SR or FG and object motion are enabled. Checked shutdown stops it before SDK,
allocator and device destruction. Its command notifications run after actual
queue acceptance, including asynchronous recorder submissions and split SR
sections; the recorder packet carries the owning final engine fence explicitly.
SDK intermediate signals are not treated as resource retirement. Actual reset
or successful begin of a reused command buffer also notifies the texture owner.
The producer/translator implementation and actual-function submission and
retirement regressions now pass. Runtime admission of the narrow texture subset
still needs a scene containing one of its supported draws.


The existing object-motion switch is now a restart control under Upscaling &
Framegen, using its existing `dlaa_objmv` key and default on. It controls supported
object/deformation history, including the bounded texture pool; it does not
claim complete particle or volumetric coverage. Missing history still uses the
explicit native/camera-motion path described in the shader audit.


Native memory/texture shutdown is now called by checked device teardown after
SR/FG and texture-history retirement, rather than by the backend before those
owners close. This keeps source textures and immutable cached samplers alive
through the last history reference. The memory/ASTC probes use that same teardown
order. Submission command-pool destruction also sends reset notifications after
all accepted engine fences have completed, so discarded command references do
not leave a snapshot permanently pending during shutdown.


Checked teardown now explicitly clears retained object/bone/storage snapshot
history and destroys its retired direct VMA arenas. Previously that cleanup was
reached only by the draw probe's stand-in cleanup, while ordinary backend
shutdown could destroy the allocator with those retained arenas still present.
The backend quiesces replay and completes submitted engine fences before this
step. The probe's prior cleanup remains safe because the retired arena list is
emptied by the first call.


## Optional startup change reverted

The optional Streamline startup behavior in `5fe602e` was reverted at the user's
request to keep this migration focused. The gitlink follows that revert on RSF
main; the extra native-loader fallback is not part of this integration.


## Combined validation batch

The combined game build passes. The device-retirement, settings documentation,
actual FG history/Present and post-retirement surface fixtures pass. The normal
self-test, settings and services probes pass, including all four menu catalogs.
The unsupported `--probe-ov` invocation was corrected to `--probe-services`.
Vulkan execution passes with validation enabled; its intentional device-loss
report is explicitly synthetic. The installer worker's final batch reports 134
tests with two platform skips and no failures.

Focused fullscreen Quality SR+FG runs for DLSS, FSR3 and XeSS all exit normally
with zero Vulkan validation errors, shader failures or skipped draws. DLSS and
FSR3 sustain SR evaluations and genuine generated output. Dedicated DLSS raw
capture for source 1800 contains both real and generated images with matching
scene framing, brightness and bars; the sampled view is the opening cave/torch,
so it does not prove general moving-scene interpolation quality.

XeSS now sustains prepared and HUD-less inputs through source 1800, with XeLL
sleep/marker counts progressing and no refused calls. It still produces only
real output: its 1130x636 recommended SR plan is rounded to 1128x632 by the game,
and the exact-size host gate keeps returning pending. A narrow existing-API
repair is in progress. These runs used the established `620aee7` runtime, whose
source matches reverted main `7cc5f2b`. The pending texture shader patch was not
used by the shared translator in these runs.

The subsequent build pins RSF main `2010da8`, including the actual XeSS input
range contract. The full pinned native suite passes after correcting an existing
device fixture's warning counter to distinguish the new informational range
message. Eleven tests are skipped for unavailable configurations; the targeted
device-fixture retry passes.

The current game build also passes `--probe-kernel`, `--probe-nvn-tex` (133
cases), and `--probe-shader` (32 shaders), with the latter reporting translator
identity `ab774f1cb93f41c2`. These complete the required runtime probes alongside
the existing Vulkan execution result.

Shared shader preparation now uses patch 0018 identity `ab774f1cb93f41c2`.
All 28667 normal and 14765 motion census jobs translate and validate, with 15
additional terrain previous-position outputs and no new refusals. Shared cache
preparation also completes without failures. This proves shader translation,
not runtime texture-history admission or visual quality.

The subsequent fullscreen FSR4 SR run exits normally with zero validation
errors, shader failures or skipped draws. XeSS still remains pending: rounding
the request up to 1136x640 produces a real game target of 1136x632. The engine
size conversion is being traced before another run; this is not counted as
successful XeSS SR or frame generation.

The width-derived game-size correction reaches the actual 1144x640 scene.
In that focused run, the shared RSF SR entry point still refuses the frame
before XeSS's input-range validator because it requires the exact recommended
size. SR therefore disables for that run. With SR disabled, XeSS FG succeeds:
the SDK reports genuine generated-resource callbacks for sources 1800–1802,
and the source-1800 status has 322 generated callbacks with result zero. XeLL
continues without refused calls. The run exits normally with zero Vulkan
validation errors, shader failures or skipped draws. This proves XeSS FG,
but not combined XeSS SR+FG; the outer SR admission check is being repaired.

The shared SR repair now forwards XeSS's actual matching image/camera dimensions
to its queried-range validator while preserving the exact-plan requirement for
DLSS and FFX. Its focused actual-dispatch contract and whole-source compile pass.
The engine-size regression also executes the game's generated conversion blocks
against the real resolution owner and passes. RSF main advanced concurrently to
frame-record ABI 2 with independently versioned camera ABI 1; TotK now stamps
the camera with its own ABI constant when building against that pin.

## Combined XeSS acceptance after the admission repair

RSF main `e0985ce` builds cleanly with the real providers. Ten focused native
contracts pass; the separate XeSS device fixture skips in this configuration.
The game build and both source-based size/admission regressions pass.

The focused fullscreen game run now logs the genuine XeSS recommendation
1130x636, minimum 1130x636, maximum 1920x1080 and actual input 1144x640.
It sustains 300 SR evaluations and reaches source 1800 with 327 prepared and
HUD-less frames, 317 genuine generated-output callbacks, two SDK presents and
result zero. XeLL reports 1795 sleeps, 21599 markers and zero refusals there.
The run exits normally after 1852 real presents with zero Vulkan validation
errors, shader failures or skipped draws.

Diagnostic receipts match real and generated SDK resources to sources
1800–1802. Probe mode records metadata only, so these receipts do not establish
generated-pixel quality. The normal final screenshot shows Link and Zelda on
the cave stairs; general moving-scene quality and the narrow terrain snapshot
subset's runtime admission, memory peak and performance remain unmeasured.

Fullscreen DLSS and FSR3 SR+FG were rerun against the same frame-record ABI 2
runtime pin. Both sustain 300 SR evaluations at 1280x720 and generated output.
DLSS exits after 1882 real presents and FSR3 after 1885, each with zero Vulkan
validation errors, shader failures or skipped draws. DLSS also writes matched
real/generated raw captures for source 1800. The rebuilt game's self-test passes
with zero failures. These screenshot/capture runs are functional evidence, not
performance measurements.

## Cropped SR+FG image follow-up

A report that most of the frame lay outside the visible image exposed a source
error in the retained scaling boundary: enabling FG kept the known native
scaling draw after reconstruction had already filled its output target. That
shader still addresses the source as a scene-sized image. The SR+FG path now
captures the completed target at the same boundary and skips the redundant
scaling draw. Native and FG-only scaling still run; in-place FXAA and unrelated
draws retain their existing behavior. The actual-function regression passes.
The visual comparison run was closed by the user because it blocked their PC,
so this correction has not yet received final on-screen acceptance. Further
interactive gameplay runs require the user's explicit request.
