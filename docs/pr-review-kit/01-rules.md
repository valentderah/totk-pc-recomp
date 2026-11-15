# Rules for taking a PR

## 1. What to review first

1. Fixes before features. A PR that fixes a bug, a glitch or a crash goes first; feature PRs wait.
2. Order fixes by reach: how many players the bug hits and how badly. Windows and default settings first, rare
   hardware and unusual setups later.
3. Scale the depth of the checks to the reach. A crash many players hit and anything on the frame path get the full
   checks. A rare case (a CPU without AVX, an unusual setup) gets the minimal fix, one targeted check and a short
   note. A crash fix on rare hardware is still worth taking: cut the checks around it, not the fix.
4. Before testing, find out where the bug reproduces: OS, GPU vendor and generation, game version, settings. If the
   symptom cannot appear on the hardware at hand (an NVIDIA paging bug on an AMD machine), say so and do not run
   tests that cannot decide it.
5. Check a feature PR against the designs in `docs/milestones/`. A feature with a design (mods, cheats) goes in as
   part of that design.
6. Several PRs on one topic: compare them before taking either. Another PR may already do it right.

## 2. How to read the diff

- The author's claim is a hypothesis. Read the whole diff: races, wrong semantics, edge cases, leaks, lock order.
- Check the behaviour against evidence: how yuzu, Ryujinx, LAS Recomp, SavageWindRecomp, nxnative and MacNativeGames
  do the same thing in `refs/` (`tools/fetch_refs.sh` recreates it); the Vulkan spec; hardware docs
  ([docs/maxwell.md](../../docs/maxwell.md)); upstream issues.
- Weigh two things for every change: its cost on the hot path, and how it fails (device lost, hang, stale data, weak
  or non-AVX CPUs, other GPU vendors, Wine/Proton).
- Read the code a hypothesis depends on before running the game: the game's functions in Ghidra or
  `generated/<ver>`, our path in `runtime/`. A run confirms what the code could not rule out. It is one run, not a
  series of variants.
- Order the work so the cheapest check that could refute the PR's idea runs first. Refuted: stop, write down why,
  no build, no benches.

## 3. What a PR needs

- **Proof.** The bug reproduced before the fix, the same repro after, both results side by side: a screenshot, a log
  line, a probe, a differential test. A fix without a reproduced failure is not proof. Where a repro is impossible
  on the hardware at hand, say so; the fix is then taken only on strong static evidence in the code. Where a repro is
  risky (device lost or a TDR can hang the machine), write the recipe and let the owner decide.
- **Frame-path changes** get whole runs of the heavy scene with the switch off and on, at least two each
  ([02-performance.md](02-performance.md)). Zero new slow frames is a hard bar.
- **A switch.** A change on the hot path or in rendering sits behind an env or `totk.ini` switch, so a player can turn
  it off and we can compare.
- **Nothing changes for players outside the feature.** The DLSS PRs touched shared code; they went in once every
  path that runs without DLSS was gated on DLSS being on.
- **A fix for one setup keeps the default for everyone.** The PRs that raised the low memory block from 6 to 10 GiB for
  all players were rejected; the right shape is 6 GiB plus the extra where it is needed.
- **Parts we cannot run** (Linux-only code, an HDR display, RTX-only paths) are gated so they cannot touch anyone
  else, or wait for a report from someone with that hardware. Note them as unrun.
- **Project tests** from [03-correctness.md](03-correctness.md): lifter differential tests, probes, `--selftest`,
  `tools/check_content.py --staged`.
- **Text the player sees** goes into the catalog: `runtime/ui_text.h` ids and `runtime/ui_text.c` entries in every
  language the menu has (English, Russian, Brazilian Portuguese, Simplified Chinese), and `installer/text/*.json` for
  the installer. No hardcoded sentences, no second string table.
- **A new setting** is described in `totk.ini.example` and [docs/settings.md](../../docs/settings.md).
- **Licences.** MIT, BSD, ISC, zlib and Apache-2.0 code may be copied, with its header and a row in
  `THIRD_PARTY_NOTICES.md`. yuzu, suyu and other GPL code is read for behaviour only; we write our own.

## 4. Reasons to reject or rework

- No test or repro for a change that is not trivial.
- Slow: a state copy per draw, a global atomic counter on the hot path, synchronous file I/O or a GPU allocation on a
  game or render thread, new slow frames.
- Changes behaviour for everyone to serve one setup, or is on by default without a run in a scene where it acts.
- "Fixes" a crash by turning a feature off for a vendor. A crash that goes away with a feature off points at the bug;
  the feature gets fixed.
- Breaks another feature (a preview that loses anti-aliasing).
- Fake features: labelled FSR4 or frame generation, and not that.
- Wrong measurements: percentiles over window means, counters over the wrong set.
- Generated C edited by hand instead of the generator (`recomp/`).
- A disallowed licence or pasted GPL code.

## 5. Finishing a good PR

A good PR that misses the project's scheme (hardcoded text, conflicts, layout, comment style) is brought to it while
merging, not sent back to the author for another round. When only part of a PR is good, take that part and note what
was left out and why.

A bug found outside the PR under review is not fixed on the spot: search the open PRs on that topic, report the bug
with them, and let the owner decide.

## 6. Merging, branches, commits

- Each PR or wave of PRs gets its own branch `task/<tag>` from a fresh `dev` and its own build dir. It merges into
  `dev`.
- Once the review is done, the symptom is gone on one paired shot and the probes pass, merge into `dev` and hand the
  build to players: they test in game faster and wider than our routes. The wider checks (other routes, validation,
  fps cost) follow as a separate diff. Speed never skips the review of the diff itself.
- `main` only with the owner's explicit yes. Push only when told to.
- A commit message is a plain English sentence, no `feat:`/`fix:` prefixes.
- The version lives in the repo-root `VERSION` file; the owner decides when it moves.
- After a merge, record which PRs went in, what was reworked, what was rejected and why. The reason for a rejection
  goes into the PR as a plain English comment.
