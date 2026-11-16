# XeSS admission stop and final HUD-less fence, 15 November 2026

Question: why did rsf-fullscreen-xess-auto-latency.log stop admitting FG after its first world frames? This task used existing log evidence and source review only; no new game/GPU/UI run occurred.

## Measured distinction

The log shows first world present1477, source1500 with36 accepted preparations,36 HUD-less tags and32 generated callbacks, then native source0 and only42 preparations total by source1800. No FG preparation/HUD-less refusal messages appear in this run. The first DLAA completion is logged but no300-frame DLAA completion appears. This places the long gap at or before the renderer's admission/producer gates; it does not demonstrate an SDK generation failure.

Contrary to the initial trace concern, the log does contain periodic core status: source600 owner2 XeLL sleeps590 markers7150 refused0; source1200 sleeps1189 markers14362 refused0; source1800 sleeps1788 markers21565 refused0. Thus the generic drain pause is not latched throughout this gap. There is no need to reproduce already-established XeLL pacing solely for counters.

The earlier rsf-fullscreen-xess-framegen-history.log, before automatic latency binding/scale correction, had a different failure: HUD-less invalid argument on each attempted tag, but FG-only scene preparation continued to300 at source1778 (render1600x896). Its repeated preparation confirms the new combined SR path must be inspected separately from that resolved packet error.

## Producer coordination

The producer owner identified a concrete path: evaluate accepts fg_prepare for size e, then an SR provider-plan mismatch returns PENDING. D.last was updated only after successful SR, yet the HUD-less hook selected D.last, losing the accepted current FG producer. That owner is implementing separate D.fg_last ownership and bounded plan/size/motion/camera/SR-pending/FXAA gate diagnostics under existing present trace. This worker did not edit vk_dlaa.c or rsf_sr.c.

A source-reviewed additional gate is res_rsf_scene negotiation: rsf_scene may queue a provider-required size and return PENDING after FG preparation, including during game DRS changes. This is a distinct reason from a renderer planner failing to reach evaluation at all. The next bounded run's producer diagnostics should establish which early gate persists, without substituting a latest source or changing input dimensions to bypass checks.

## Concrete mapper lifetime correction

A mapped slot previously retained only the preparation submission's timeline fence. HUD-less copying may record later, in a different guest command submission. Successful submission notification clears command-pending flags, but does not prove the later HUD-less copy completed. Comparing only the older guide fence can then recycle its encoded colour backing before the final copy finishes.

runtime/rsf_fg_gpu.c, rsf_fg_gpu_hudless now obtains the actual vk_sync_record_fence(cb), refuses a missing live recording lease before source-view/command mutations, and extends the retained backing fence to the maximum actual recording fence. Pending command tracking remains independent and required. No device idle, queue wait, per-frame allocation or SDK policy was added. Same-submission fences remain unchanged.

The existing RT_TRACE_PRESENT flag now enables bounded mapper refusal details, first and every300 per reason:1 invalid prepare recording/input/capability contract,2 no reusable backing slot,3 mapping/allocation/source-view failure,4 HUD-less recording fence/input/view failure. Each sample records source ID, requested and completed fences, then four retained slot identities/final fences/pending command flags. Normal successful operation does not log per frame. These facts distinguish ring pressure, unknown recording ownership and invalid source contracts from an early producer that never calls the mapper.

## Regression and final check batch

The actual-function fixture tests/rsf_fg_gpu_check.c adds separate guide submission100 and HUD-less submission110. It fills the ring, completes guide fences, and proves other slots recycle while that source's backing remains intact. It then proves that even completed110 cannot retire still-unsubmitted HUD-less commands, that successful submission alone with completed109 is insufficient, and that completion110 plus actual submission permits reuse. It also checks refusal of a missing HUD-less recording fence before modifying the lease. Existing allocation, source-view reuse, recorder ordering and nonzero HUD-less scale cases remain.

Exact final validation uses UCRT64 Clang with the runtime/VMA and pinned RSF orchestrator/contract/game SDK headers, produces only build/checks/rsf-xess-admission-worker/gpu-fixture.exe, and compiles the complete changed mapper with -Wall -Wextra -Werror -fsyntax-only. No sibling source changes or shared artifacts are part of this task; runtime/input_sdl.c's unrelated cosmetic change is preserved. Root owns final combined build and serial gameplay retest. Regression and compiler results are recorded by worker completion; no pixel/visual or hardware acceptance is inferred from CPU fixtures.
