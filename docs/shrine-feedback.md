# Shrine floor feedback snapshots

Related: [issue #530](https://github.com/valentderah/totk-pc-recomp/issues/530) and
[PR #690](https://github.com/valentderah/totk-pc-recomp/pull/690).

Small black rectangles can flicker over shrine floors even with the feedback-layout barriers or a full pass restart.
The material samples R32F linear depth while writing it as a colour attachment. Giving fragment-stage sampling a
separate depth snapshot removes the artifact in the tested views while retaining the material's blend.

## Materials covered

The first workaround recognized fragment termination in SPIR-V: `OpKill`, `OpTerminateInvocation`, or
`OpDemoteToHelperInvocation`. That fixed the tested Nachoyah Shrine entrance on 0.3.4 but missed a second shrine
tested on 0.3.7. Its affected draws had no termination instruction and no fixed-function blending. Diagnostic
output showed sampled attachment mask `0x8a`, write mask `0x8a` or `0x8b`, and `blend_en = 0`: they read and write
colour attachments 1/3 and linear depth 7. The shader itself mixes its result with the existing G-buffer.

The default path now accepts any of:

- A fragment-terminating shader.
- Fixed-function blending into another written, non-feedback colour attachment.
- Sampling a non-R32F colour attachment that the draw also writes.

The copy itself remains limited to writable, sampled, single-level, single-layer, single-sample R32F 2D attachments
with transfer-source usage. Other attachments keep the existing feedback handling. These conditions are a local
workaround supported by the reported captures and A/B tests, not a complete model of every Maxwell feedback case.

## Snapshot reuse and its limits

Copying before every eligible draw repeatedly ends and restarts rendering. The optimized path keeps the most recent
snapshot, keyed by frame counter, attachment-image signature, source image and snapshot generation. It reuses that
image while those values match; a changed frame, attachment set or snapshot generation requires another copy.
Snapshot-induced rendering restarts do not invalidate it. Cached copies are reserved before allocating copies for
other attachments of the draw, so a later lookup cannot overwrite an image already selected for sampling.

This is an approximation, rather than a version of the attachments' contents. A snapshot contains depth at the
first eligible draw, not necessarily at the start of a Vulkan render pass. Writes or clears to unchanged attachments
do not change the signature. Overlapping materials, repeated uses of the same targets within a frame, and related
view changes need validation. The cache has one most-recent entry, not one permanent entry for every pass/image.

`feedback_r32_copy` in `totk.ini`, or `TOTK_FEEDBACK_R32_COPY` in the environment, selects:

| Value | Behaviour |
| --- | --- |
| `0` | No depth snapshots. |
| `1` (default) | The material conditions above, with snapshot reuse. |
| `2` | Copy all eligible R32F attachments before each draw, without the reuse heuristic. |

These settings require restarting the game. Mode 2 is useful for comparison, but it is not a complete reference for
all rendering semantics: it snapshots only linear depth and can be much slower.

Submissions recorded in parallel chunks keep the renderer's existing feedback barriers and layouts. The depth
snapshots apply to sequential recording, the default with `replay_workers = 0`.

`feedback_debug = 1` / `TOTK_FEEDBACK_DEBUG=1` logs the material decision and masks for up to 48 signatures, plus
periodic draw, eligibility and copy counters. The printed SPIR-V identifier hashes instruction words after the
header; it is not the raw-byte hash used by the frame dumper. Diagnostics are off by default.

## Validation and attribution

The supplied DeepSeek-assisted investigation reports testing on TotK Recomp 0.3.7 (`0f65b4ee`), game 1.4.3,
Windows 11, RTX 3070 Laptop GPU, NVIDIA driver 616.56, at 1920x1080 output / 1600x896 scene with FSR:

| Build | Black rectangles | Performance evidence |
| --- | --- | --- |
| Original cutout-only #690 | Present in the second shrine | About 58.6 fps mean. |
| Expanded conditions with per-draw copying | Absent | 55.2 fps mean, 1% low 24.1 fps; 160 frames above 25 ms. |
| Expanded conditions with reuse | Absent in the player's test | Player reported normal performance; no matched timing measurement supplied. |

The report also records a passing executable selftest and installer validation. Its captures place the artifact in
scene colour before tonemapping/upscaling; per-pixel depth correlation was inconclusive. The A/B result supports
the snapshot workaround, but does not independently rule out every other contributing rendering issue.

The first integration merged upstream 0.3.10 at `101132a6`, retaining both upstream `FLUSH_SPAN` and the feedback
configuration. The complete Windows runtime compiled successfully (113 objects) through the supported llvm-mingw
build path. Local checks of the actual snapshot functions, with mocked Vulkan calls, cover eligibility, reuse,
frame/generation invalidation, disabled and per-draw modes, and multiple/duplicate attachments. These checks do not
validate GPU synchronization or visuals. The game has not been run in gameplay for the merged upstream code; other
GPUs, Linux, other shrines and overlapping-material cases remain untested.

This branch now also merges upstream 0.3.11 at `0ae47a5`. The configuration conflict retains upstream
`FRAME_QUEUE` and `FLUSH_SPAN` alongside `FEEDBACK_R32_COPY` and `FEEDBACK_DEBUG`; both feedback settings are
documented in `docs/settings.md`, whose runtime-setting check passes. A full 0.3.11 runtime build and gameplay run
have not yet been performed.

This branch now also merges upstream 0.3.13 at `ae30a61`. The `runtime/cfg.c` conflict keeps both sides' settings,
`DESWIZZLE` and `DYN_DELTA` from upstream and `FEEDBACK_R32_COPY` and `FEEDBACK_DEBUG` from this branch, and
upstream's newer `probe_movie_bench` row replaces this branch's. `python tests/settings_doc_check.py` reports one
problem on the merged tree, `DESWIZZLE`: upstream lists that setting in `known[]` without a description in
`totk.ini.example` or `docs/settings.md`. The same check reports the same problem on 0.3.13 without this branch. A
0.3.13 runtime build and gameplay run have not been performed.

The original cutout workaround and this integration were prepared with Codex. The expanded material conditions,
reuse heuristic and diagnostics were supplied by the user's DeepSeek-assisted update. Only source and
documentation are included here; no game data, translated game code, shader dumps, saves or local installers.
