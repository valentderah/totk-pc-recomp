# ReScaleFrame integration

The requested replacement covers super resolution, frame generation and low latency
through ReScaleFrame for NVIDIA, AMD and Intel. Implementation is pending. The current
renderer still uses NGX directly in `runtime/vk_dlaa.c`.

`runtime/rsf_runtime.c` now provides opt-in module loading and validates the required
native Vulkan exports before exposing them. `runtime/vk_device.c`, `create_instance`
calls this before Vulkan initialization. This is a module lease only; SDK setup and
the rendering replacement below remain pending.

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
success for the current evaluation. Missing camera/history currently skips that
dispatch, so an RSF caller must arrange depth production on those frames too.
The shader compiled and passed SPIR-V validation; the changed C files passed
UCRT64 Clang syntax checks. GPU output has not yet been checked in TotK.

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
