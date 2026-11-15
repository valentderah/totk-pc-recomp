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
to the sibling ReScaleFrame checkout; no RSF headers are copied into this repository.

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
The window's native WSI calls have not yet been switched to this adapter. This
fixture does not establish generated frames, valid HUD-less inputs or presentation.

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

Build the runtime with UCRT64 Clang, exercise each available vendor's SR, FG and
latency provider, and verify unsupported selections report their actual failure.
Check HUD composition, motion, resize, provider changes, pause/load history resets,
queue synchronization and shutdown with Vulkan validation. Confirm the old NGX
implementation is removed and configuration, menu, installer and packaging all
select and deploy RSF. Provider availability alone does not prove generated frames
were presented or latency markers reached the SDK.

The in-game Graphics page now contains a ReScaleFrame group with enable, provider
and quality controls, plus disabled frame-generation and low-latency controls
marked unavailable. The DLSS model and character-mask controls have been removed
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
