# Native FSR4 SR adapter, 15 November 2026

## Scope and evidence

The question was how TotK can consume the real RSF Vulkan/DX12 FSR4 service
without submitting an unfinished producer from inside the SDK callback, losing
the continuation, or retiring borrowed images before both sections finish.
The implementation consumes headers from `external/ReScaleFrame`, pinned at
`1753d4d267e7058acd8d392e3d89a5d361458104`. The service rationale is recorded in
RSF's `docs/research/vulkan-fsr4-sr-leases-20261115.md`. No RSF implementation
or GPL runtime source was copied into TotK, and no RSF checkout was modified.

This note records source implementation, complete-source compilation and CPU
fault injection. It does not establish device acceptance, true FSR4 execution
in a game, visual quality, latency improvement or performance. The coordinator
retained exclusive GPU ownership throughout this work.

## Recording and submission

`rsf_sr.c`, `rsf_sr_open_split` binds the six FSR4 exports from the existing
runtime module: create, plan, evaluate, submitted, retire_commands and destroy.
The separate opaque FSR4 owner uses tracked split ABI 2; ordinary SR owners
retain native SR ABI 2. No external ABI layout was changed. Missing exports,
provider refusal and failed cleanup remain observable; partial owners survive
for checked cleanup retry. There is no FSR3 substitution for FSR4.

`vk_dlaa.c`, `rsf_scene` prepares a continuation slot before calling the service.
It sends the existing copied frame/camera identity, pixel jitter, RGBA16F colour,
R32F device depth and RG16F motion. Motion is previous minus current in render
pixels, so the scale remains one in both axes. The optional current-colour bias
mask remains independently supplied; the FSR4 service does not apply the DLSS
character-history hint. Reactive/composition masks remain empty. The existing
shader audit's material, deformation, translucency, particle and auxiliary-pass
gaps remain open; changing the SR provider does not repair those producers.

`vk_sync.c`, `vk_sync_sr_prepare` claims a command buffer from the existing
submission pool with the same final engine fence as its producer. A reusable
slot must already have reached its prior GPU fence. The command buffer is reset
before borrowed SDK resources are retired, outside the session recording lock.
The callback `sr_record_split` ends the current section, attaches previous_done
wait/ready signal metadata, begins a different recording buffer with its done
wait, and changes the active serial slot. It never submits its graph sections.
A consumed preparation retains both slots; an unused preparation can be released.

The recorder begins/ends the continuation according to its original ownership
mode. `rec_begin_continuation` skips a second retirement callback because that
would reenter the SR session lock from the recorder while the producer waits for
it. Retirement already happened after explicit reset in the preparation step.
Each split section retains whether the recorder owns its eventual submit,
rather than consuming the ordinary single-ended-buffer flag only once.

`vk_sync.c`, `submit_graph` uses `rsf_split.c`, `rsf_split_submit` to expand the
ordinary final submission into ordered producer and continuation submissions.
The existing prologue and initial waits remain first, and ordinary signals,
including the final engine retirement timeline, remain last. Imported timeline
waits/signals use ALL_COMMANDS. Sync2 carries values directly and preserves binary
semaphore value zero. Both sections submit to the same existing graphics queue.
The final timeline fence covers all prefix sections and their borrowed resources.
Even if the SDK consumed a producer before refusing dispatch, the continuation
and its retirement signal remain mandatory.

`vk_rec.c`, `rec_submit` deep-copies both wait and signal arrays into its packet.
An SR lease forces observation of that section's actual submit result before
`rsf_sr_commands_submitted` is called on the producer, outside the queue lock.
Failed submission preserves pending identities and owner leases. Submit failure
remains the renderer's existing fatal path; it does not free/reuse a partially
submitted graph. Unknown submission identities do not release provider leases.

`rsf_sr_record_split` returns the replacement command buffer even on SDK failure.
`rsf_scene` restores input/output layouts in that continuation and records the
SDK-independent spatial fallback there. Provider errors remain logged and are
not counted as successful temporal evaluations. History resets after fallback.
A positive SDK error after a consumed split is a failure needing fallback;
it is not confused with Vulkan NOT_READY before consumption. NULL continuation
is terminal and stops rendering rather than reusing the ended producer.

`vk_exec.c`, `chunked_on` disables worker chunking only for active FSR4 SR, alongside
the existing FG serial guard. Native rendering and other SR modes retain their
chunk admission. The first integration deliberately owns the split through the
serial submission pool; worker-pool continuation support is not implemented.
This may cost CPU overlap and has not been benchmarked.

## Device and lifecycle

`vk_device.c`, `create_device` enables Win32 external-memory and external-semaphore
imports when both are available, allowing live SR selection. Missing support
refuses FSR4 honestly. The renderer already requires/enables timeline semaphores.
The RSF service checks per-format importability, shared D3D12 fence support,
matching adapter LUID and effective 4.x provider identity. It owns the signed
provider, guarded INT8 compatibility path and DX12 resources.

The rebase's direct TOTK_REFLEX extension/has_nv_reflex activation was removed as
requested by the coordinator. RSF exclusively owns vendor latency; the independent
RSF requirements path and its queried extension facts are preserved. Native engine
sleep/markers/Present pNext are no longer activated through that legacy flag.

Existing checked size/context destruction is preserved. GPU users must be drained
before images are freed; failed provider destruction retains the owner. The
coordinator's presentation/resize work supplies the corresponding WSI lifecycle.
`vk_sync_record_fence` exposes the actual final fence only for the active serial
recording's current command buffer, without acquiring a lock. After a split the
continuation matches and the ended producer does not. Outside recording it returns
zero, so the crop mapper cannot infer a lifetime from issued+1.

At the coordinator's request, `vk_dlaa_fg_after_draw` now allocates and captures
HUD-less colour at the game composition extent e->gw/e->gh, including both ends
of its encoding blit, the copy extent and the submitted image descriptor. The
frame record keeps the host output e->fw/e->fh and the host-size recheck remains.
Presentation wrappers own the single crop/fit/bars transform. Their API and source
files were not edited by this worker.

## Validation

From the TotK root:

```powershell
pwsh -NoProfile -File tests/rsf_split_check.ps1
python tools/check_content.py --staged
```

Both commands exit zero. The adapter script compiles seven complete source files
with MSYS2 UCRT64 Clang, production-style flags and -Wall -Werror:
rsf_sr.c, rsf_split.c, vk_sync.c, vk_rec.c, vk_device.c, vk_dlaa.c and vk_exec.c.
It uses pinned external SDK headers; the existing generated/v143 and build/v143-rsf
headers are read only. All objects, fixtures and executables are written under
`build/checks/fsr4-adapter-worker`, never the shared game or RSF build outputs.

Six CPU executables pass:

- Extended actual rsf_sr.c lifetime fixture: ordinary SR behavior, all six split
  exports, creation cleanup retry, pending before split, continuation on SDK
  failure (including positive error 1), actual-submit failure retention, separate
  producer/continuation release and command retirement.
- Actual rsf_split.c submit expander: original prologue, binary acquire waits,
  external timeline dependencies, final engine fence, unchanged caller structures,
  full preflight refusal and stopping after the first failed submit.
- Extracted actual claim_slot/handoff/submit_graph functions: exact recording
  identity, different continuation, final-fence retention, reset before retirement,
  consumed versus unused cancellation, ordered graph emission and fence accessor.
- Extracted actual recorder submission functions: synchronous observation for
  leased sections, failed-submit notification suppression, thread ownership
  handoff and deep packet copies before asynchronous consumer execution.
- Extracted actual chunk admission function: active FSR4 serial recording,
  unchanged native/other SR admission and preserved FG/dump guards.
- Extracted actual rsf_scene function: continuation adoption, subsequent layout
  work on the new buffer, spatial fallback on that buffer, successful dispatch,
  unavailable recording lease and honest missing-interop refusal.

The generated narrow fixtures execute repository functions with inert dispatch,
not a separately reimplemented rendering model. They do not call a Vulkan device.
Initial fixture declaration/order and expected-transition-count errors were fixed;
all final builds and tests pass. A scoped git diff --check also passes.
An optional whole-worktree content check reports the pre-existing untracked
.local/Firmware.23.5.0.zip size violation; that unrelated archive was not touched.
The staged content check is clean, and no files were staged or committed.

## Remaining acceptance

The coordinator must regenerate the game build graph so the new rsf_split.c is
linked, then complete its full integration gate and GPU checks. In particular:
true effective FSR4 provider, Vulkan synchronization validation, reduced/native
resolution, SDK-failure fallback, final-fence retirement, live backend/quality
switches, resize/teardown and moving-game input/visual acceptance remain untested.
The two existing Anomaly RSF gate failures were neither run nor altered here.
No RSF native_sr.cpp shadow-alias fix was needed for these TotK-only checks.

Owned files are runtime/rsf_sr.c, runtime/rsf_sr.h, runtime/rsf_split.c (new),
runtime/rsf_split.h (new), runtime/vk.h, runtime/vk_device.c, runtime/vk_dlaa.c,
runtime/vk_exec.c (narrow coordinator-authorized guard), runtime/vk_rec.c,
runtime/vk_rec.h, runtime/vk_sync.c, tests/rsf_sr_lifetime_check.c,
tests/rsf_split_submit_check.c (new), tests/rsf_split_record_check.py (new),
tests/rsf_split_check.ps1 (new) and this note (new). Other dirty work belongs to
other workers and was preserved.

## First bounded run and allocation-generation correction

The coordinator's first real FSR4-native run used runtime 1753d4d and executable
SHA-256 7d539d2dc348046544e937de88a1967cb4b53e8c3a99b87ef01a356093addc08.
Its evidence is `build/checks/rsf-combined-fsr4-native-game.log`: it reached the
world at present 1492, exited zero after 1590 presents, and reported zero validation
errors, shader failures and skipped draws. It also reported FSR4 evaluation
refused (-1), followed by spatial fallback. Those facts do not establish a
successful temporal dispatch. Ten depth-only unused-fragment-output warnings
remain coordinator evidence pending baseline comparison.

The initial hypothesis that the output was described as GENERAL was disproven
by the current source. `rsf_scene` already transitions RGBA16F output from GENERAL
to TRANSFER_DST_OPTIMAL, supplies that layout and the output extent to the service,
and restores GENERAL after evaluation/fallback. Those transitions and ordinary SR
layout semantics were left unchanged.

A concrete earlier admission violation was found in `scene_record`: it omitted
resource_generation entirely, leaving zero. The pinned RSF service's
`rsf_fsr4vk_evaluate` explicitly rejects zero resource_generation before recording
or calling the split callback. Other working SR paths do not make that same
admission check. A source review of setup/handoff, image formats/layouts, extents
and the DX12 bridge therefore did not require changing shared output handling or
instrumenting the sibling service.

The correction assigns a nonzero identity when `size_for` successfully allocates
a new work-image set, preserves it while that allocation is reused, and copies
it into every scene record. A replacement allocation gets a distinct generation,
even at the same dimensions. This follows actual resource lifetime rather than
using the changing source frame as a generation. Counter exhaustion refuses
rather than returning the reserved zero identity. No external ABI or sibling
checkout was modified.

The actual-scene fixture now checks the precise output image identity, RGBA16F
format, output width/height, TRANSFER_DST descriptor and real transition state.
It checks success, SDK failure after a split, validation failure without a split,
spatial fallback and GENERAL restoration on the returned current/continuation
buffer. The ordinary-provider branch is also checked unchanged. A new CPU
executable compiles the actual allocation and scene-record producers together:
a reused set retains its generation, another size gets a distinct one, a freed
and recreated size gets a fresh one, and copied frame records remain immutable.
This would fail with the previous unstamped record producer.

`pwsh -NoProfile -File tests/rsf_split_check.ps1` now passes seven complete-source
compiles and seven CPU executables using the isolated worker output directory.
This follow-up changes only runtime/vk_dlaa.c, tests/rsf_split_record_check.py,
tests/rsf_split_check.ps1 and this note. The coordinator still needs a rebuilt,
bounded FSR4 run to establish dispatch acceptance after the identity fix; there
were no worker GPU/game/UI tests and no staging or commits.
