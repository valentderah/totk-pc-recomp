# ReScaleFrame integration

The requested replacement covers super resolution, frame generation and low latency
through ReScaleFrame for NVIDIA, AMD and Intel. Implementation is pending. The current
renderer still uses NGX directly in `runtime/vk_dlaa.c`.

`runtime/rsf_runtime.c` now provides opt-in module loading and validates the required
native Vulkan exports before exposing them. `runtime/vk_device.c`, `create_instance`
calls this before Vulkan initialization. This is a module lease only; SDK setup and
the rendering replacement below remain pending.

`runtime/rsf_sr.c` now binds the typed SR session API from the installed SDK and
handles creation, planning, recording, command retirement and destruction. Failed
cleanup preserves ownership for retry. It passes UCRT64 Clang syntax checks against
the actual SDK. The adapter has not yet been connected to `evaluate` or exercised
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
