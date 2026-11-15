# Accepted FG metadata through delayed Present, 15 November 2026

The coordinator's clean FSR3 letterbox run reached 1801 presents with zero
validation errors, shader failures and skipped draws, but generated no frames.
The evidence remains `build/checks/rsf-combined-fsr3-fg-letterbox-fixed.log`.
The earlier `rsf-combined-fsr3-fg-letterbox-game.log` contains mapper recording,
layout and pipeline errors and an SDK crash; those were fixed separately by the
coordinator and must not be confused with the clean later run.

## Exact frame identity survives backing reuse

`gfx_present.c`, `flip_locked` constructed a new anonymous identity using only
session/frame/view, leaving resource_generation zero. RSF's `native_fg_source.h`,
`fg_present_identity_matches` requires the exact generation of the accepted
prepared record. The final blit also searched the mapper's reusable GPU pool to
recover crop metadata. Its fence-aware reuse correctly recycles a completed
source backing allocation, but that does not mean a delayed Present has consumed
that source's identity. These are independent lifetimes.

`rsf_fg_history.c` now keeps up to 64 accepted CPU metadata records, each with its
captured crop/fit map. `vk_present_fg_prepare` remembers a value copy only after
SDK prepare succeeds. No images, Vulkan objects, handles, SDK contexts or GPU
leases belong to this history. The mapper's existing fence-aware reuse and
recorder ordering are unchanged; supported providers own their private copies
of prepared GPU inputs.

After the final swapchain blit submits successfully, `fg_pending_snapshot` finds
the exact source and compares its captured mapping with that actual Present's
crop/destination rectangles. The matched record and map are copied into the
pending output image's metadata, alongside the existing acquired index/source.
`flip_locked` consumes that snapshot and passes its full session/frame/view/
resource-generation identity to RSF. Later prepares, reused GPU backing and
later history eviction cannot replace the pending snapshot. The record is never
rebuilt from a latest-frame counter or an assumed generation.

Absent, expired, invalid or ambiguous sources and changed maps pass NULL identity
to provider Present, retaining ordinary output with interpolation off. A duplicate
source with conflicting accepted identity or map is marked ambiguous rather than
selecting the newest record. HUD-less admission also compares the incoming record
with the accepted identity before mapping/tagging it; changing generation cannot
attach new colour to an old prepare. Swapchain retirement/teardown clears history
and pending metadata through `fg_forget`. No public/vendor ABI changed.

## Fullscreen surface facts after slow retirement

The coordinator's focused fullscreen DLSS run in
`build/checks/rsf-combined-dlss-fullscreen.log` reached the world and exited normally,
but recorded VUID-VkSwapchainCreateInfoKHR-pNext-07781: a 1920x1080 creation extent
was outside the then-current fixed 2560x1600 surface capabilities. Its accepted
latency sleeps/markers and still-zero FG count are separate evidence.

Source review found that `build_swapchain` cached capabilities before format
selection and slow SDK retirement. The window could enter fullscreen during that
gap. The function now retires the old owner first, then `surface_extent_refresh`
queries actual Vulkan surface capabilities. Fixed currentExtent is used directly;
only variable extent uses actual client pixels, clamped to the reported bounds.
Image count and transform also come from the refreshed capabilities. A resize
mark during retirement or before SDK creation defers the attempt, so the existing
create_swapchain retry starts from new facts. Game/output dimensions never supply
host surface dimensions.

The coordinator separately changed Win32 initial activation to synchronously
apply requested fullscreen after successful foreground raise and before publishing
window readiness. Its queued handler remains idempotent, with unfocused first
activation/background behavior preserved. That window-source edit and its compile
were performed by the coordinator, not this worker. Surface query and later driver
creation are not an atomic operation; further concurrent user/OS resizes can
still require the existing out-of-date/rebuild lifecycle. New device acceptance
must use fullscreen, background off and actual focus for DLSS, as requested.

## CPU proof

From the TotK root:

```powershell
pwsh -NoProfile -File tests/rsf_fg_history_check.ps1
python tools/check_content.py --staged
```

Both pass. The script writes only `build/checks/fg-history-worker`, compiles complete
`gfx_present.c` and `rsf_fg_history.c` with UCRT64 Clang using production-style
flags and -Wall -Werror, and runs three CPU executables:

- Pure metadata history: N and N+1 before Present N; copied metadata independent
  of backing/caller mutation; exact nonzero generation matching; conflicting
  generation ambiguity; unknown/expired sources; crop changes; invalid maps;
  bounded eviction; immutable pending snapshot and clear behavior.
- Extracted actual prepare/HUD-less/snapshot/flip functions: a single immediately
  reused fake backing slot; successful prepares only; generation mismatch before
  HUD-less tagging; N retained after N+1 reuse; a deferred snapshot stable after
  later prepare; both sources presented with exact accepted identities; unknown,
  evicted, map-changed and refused preparations use NULL identity; teardown clear.
- Extracted actual build_swapchain and surface_extent_refresh: retirement changes
  fixed capabilities from windowed 1920x1080 to fullscreen 2560x1600; the SDK create
  receives the new extent, count and transform; a resize mark prevents creation;
  variable extents clamp actual client pixels; minimized/query-failed cases refuse.

These execute repository functions with inert dispatch, not GPU work or a vendor
model. Scoped git diff --check passes. No GPU/game/UI/device tests, shared-build
writes, staging, commits or sibling edits were performed. All existing mapped
GPU fence reuse, rec_push routing and mapper drains before direct SDK recording
were preserved. The coordinator must regenerate the build graph to link the new
history module, rebuild and repeat real fullscreen FG acceptance; this CPU result
does not claim generated frames or removal of all fullscreen validation errors.

Owned files for this dispatch: runtime/gfx_present.c; runtime/rsf_fg_history.c and
runtime/rsf_fg_history.h (new); tests/rsf_fg_history_check.c,
tests/rsf_fg_history_present_check.py, tests/rsf_fg_surface_extent_check.py and
tests/rsf_fg_history_check.ps1 (new); this note (new). Other dirty work is preserved.
