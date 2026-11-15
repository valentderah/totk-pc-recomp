# Narrow native-read texture snapshots

This follow-up leaves the staged shader audit untouched. It implements one
sampled 2D binding for straight-line terrain parameter paths, not general texture
history or complete grass/effect motion. No GPU/game/UI tests were run here.

## Implemented caller and ownership

`vk_snapshot_pool.h` exposes start/acquire/cancel/record/publish/consume/release,
actual submitted/reset notifications, completed-fence collection and checked
stop. The fixed 64-slot queue uses a dedicated allocator worker. Recording-side
acquire uses a try-lock and refuses unready, exhausted, expired or unsupported
requests without waiting or allocation. Measure, dedicated Vulkan allocation,
view creation and destruction run only on the worker. The native backend checks
device format/shape features and exact image requirements before reserving the
memory budget; backing plus copied payload must match those measured values.
Rejected shapes are bounded cached refusals, not repeated draw-thread retries.

The native caller caps retained backing at 64 MiB, copies at 4 MiB per source
frame and captures at eight resources per source frame. Those are mechanical
limits, not measured performance claims. No capture is amortized across native
draws. A duplicate geometry/parameter identity in a frame becomes ambiguous and
cannot supply previous history. Pool exhaustion refuses; it does not wait or
evict a pending image. Exact shapes are requested from actual reads, with no
hardcoded game texture dimensions.

`vk_motion_texture_start(enabled)` starts that worker off recording threads.
`stop()` first releases CPU history and collects completed actual engine fences,
then refuses while recorded/unsubmitted or unfinished work remains. The root's
device owner integrates start after device/VMA readiness and stop before texture
sampler destruction. Cached samplers are keyed by complete biased `samp_par`
state and live until `vk_texture_shutdown`; changing LOD bias clears slot memos
without destroying those objects. Thus the caller reuses the current native
immutable sampler only when it equals the previous recorded sampler in the same
device epoch. It does not retain a stale sampler through device teardown.

`vk_motion_texture_submitted(commands, final_engine_fence)` is called by the root
only after actual queue acceptance, outside the queue lock. Its fence is never
inferred from source frame or the SDK ready signal. `commands_reset(commands)`
is called after real reset, before that command-buffer handle begins its next
generation; these notifications must remain ordered by the command owner.
Unsubmitted command references prevent backing reuse even if CPU history is
released. Consumers retain each private view through their actual final fences,
including split continuations. Stop requires the recording/submission owner to
be quiescent; it performs no queue wait or blocking driver teardown under the
queue lock. The root also moved retained uniform-arena cleanup into real device
teardown, so these two history owners retire before VMA destruction.

## Exact native read and shader connection

Native descriptor binding records the actual VS combined image/sampler facts.
Before an eligible indexed draw, the caller requests its motion variant and
accepts exactly one marked position texture, c4 plus c15 and optional c14/c16,
and complete previous parameters. The source texture lookup is cached-only: no
native image creation, view creation, upload or slot re-resolution is allowed.
The owner supplies a monotonic image-creation identity, with exhaustion refusing
rather than wrapping. Recreated images cannot pass history by handle ABA.

Initial facts refuse custom views, nondefault swizzles/channel remapping, partial
mips, arrays, depth, multisampling, storage usage, previous target writes,
pool-bound images, known same-address aliases, feedback and unresolved slots.
The caller verifies the already-bound native view against the existing canonical
full-mip view cache while holding the texture lock. It does not assume that a
slot lookup performed after native binding still names that bound view. Source
images retain their normal current-submission ownership; only private snapshots
are stored across frames.

`vk_exec_native_suspend/resume`, declared in a new private header, use views
saved at the real native `BeginRendering`. They do not call view creation,
target resolution, contact/DLAA/dump callbacks or target-write publication.
The hooks refuse chunks, feedback, active queries/pass marks, multisampling,
stand-ins and changed pass/command/frame/attachment identity. Resume uses LOAD
for identical attachments and preserves native pipeline/descriptors/index/
vertex/dynamic bindings; transfer and barrier commands do not change those
bindings. New internal `VkExec` fields store only resolved native attachment
view handles. No shared `vk.h` edit was made by this worker.

The recorded section is EndRendering, dependencies, all-mip `CopyImage`,
copy-to-vertex-read dependencies, LOAD resume, then the matching native indexed
read. It uses the existing recorder wrappers. No upload lookup, source-slot
resolution, producer callback or command-buffer split intervenes. Upload batches
flushed before the part execute before both the snapshot and native read.
The source copy and private image writes use explicit dependencies; the snapshot
is never copied in a later, independently ordered upload command buffer.

History is associated with consecutive source frames, shader/geometry binding
facts and the selected previous c15/c16 parameter addresses. Missing, duplicate,
discontinuous or changed image/sampler/shape identities refuse. The existing
uniform-history selection remains a limitation, and dynamic vertex/storage
geometry is not newly claimed as covered by this texture repair.

Translator patch 0018 clones the actual `TextureOperation`, preserves its index,
sample type/flags/LOD and remaps previous coordinates/parameters. It accepts one
combined 2D binding, no branch/phi and only the initial parameter-bank set.
Other texture operations remain refused. Previous descriptor metadata uses the
reserved original-bank+64 convention (63/66 for the supported bound tables).
The parser preserves existing fields and maps that marker to the original
texture slot plus a private previous flag. Original current bindings remain
unchanged; previous descriptors point only at private uploaded snapshots. The
translator identity changes normally; answer-record layouts are unchanged.

## Focused implementation checks

- `tests/rsf_snapshot_pool_check.c` runs the actual queue/state/worker code with
  a real thread and synthetic allocation backend. It covers worker-only
  allocation/destruction, try-only refusal, exact-shape readiness, memory/frame
  budgets, publication before/after actual submission, pending resets, wrong
  frames, stale tokens, failed partial creation and producer/consumer retirement.
- `tests/rsf_motion_texture_check.py` compiles the actual pool, snapshot
  producer/consumer and native-pass hooks with synthetic recorder commands. It
  proves End/copy/barrier/LOAD/read ordering, old texels 11 versus current texels
  22, changed creation identity/shape/sampler refusal, duplicate-draw refusal,
  and retirement at explicit final fences 91/92 rather than source frames 1/2
  or recording values 41/42. Changed native pass identity, feedback, queries and
  chunks refuse. It is not Vulkan device execution or image-quality evidence.
- `tests/rsf_texture_variant_check.py` exercises the actual IR transform and
  resource parser. Current bindings remain unchanged, previous texture
  coordinates use previous banks, and wrong banks/branches/3D textures refuse.
  Existing descriptor metadata remains compatible. Its synthetic split-texture
  GLSL interface passes glslangValidator and spirv-val.
- The isolated NativeAOT translator builds, with vendor byte identity verified.
  Its smoke test passes. All 53 real straight-line candidate jobs translate and
  validate: 15 have the new one-binding descriptor/output; 38 remain refused.
  This counts shader capability, not actual runtime pool admission or visuals.
- Complete changed runtime sources compile to isolated objects. The full real
  census for this new identity, device validation, memory peaks and frame-time
  comparisons remain for the coordinator's later batch. The prior full census
  applied to patch 0017 and must not be relabeled as a patch 0018 result.

Outputs are isolated under `build/checks/rsf-texture-caller-worker`. The new
translator identity is `ab774f1cb93f41c2` (prefix); shared translators/caches were
not replaced. The root owns its shared build and submit/reset/device hook tests.
The outdoor capture still needs an actually admitted candidate, complete source
view/sampler facts, two-frame parameter/geometry identity and copy/pass counts.
No game-derived assets are tracked, and this worker staged or committed nothing.


## Coordinator full census

The exact full real corpus passed with identity
`ab774f1cb93f41c2369a94271c0d1e3d3dbf5936aa7d8d0aaeec904910430143`:
28667 normal jobs and 14765 motion jobs have zero translation or Vulkan1.3
SPIR-V validation failures. Previous-position outputs increase from 12994 to 13009,
with 15 startup terrain gains and no new refusals. The 146 texture metadata markers
include 131 still-refused shaders and therefore are not a coverage count.
The saved report is ignored at
`build/checks/rsf-texture-full-census-worker/report.md`. Shared translator/cache
preparation now completes with this identity; actual runtime snapshot admission
and visual quality remain unverified by this census.
