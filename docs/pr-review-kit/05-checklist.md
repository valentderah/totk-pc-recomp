# Per-PR checklist

## Before reading

- [ ] Fix or feature? Fixes first, ordered by how many players they reach.
- [ ] Where does the bug reproduce: OS, GPU vendor and generation, game version, settings? Can the hardware at hand
      show it?
- [ ] Is the symptom in [04-knowledge.md](04-knowledge.md) or `docs/maxwell.md`?
- [ ] Another open PR on the same topic? Compare them.
- [ ] Feature: is there a design in `docs/milestones/`?

## Reading the diff

- [ ] The whole diff read, not the description: races, edge cases, lock order, leaks.
- [ ] Behaviour checked against `refs/` (Ryujinx, yuzu, LAS, SavageWind, nxnative, MacNativeGames), the Vulkan spec,
      `docs/maxwell.md`.
- [ ] Hot-path cost: no file I/O, GPU allocations, waits, per-draw copies or global atomics on game or render threads.
- [ ] No memory kept per frame or per object without a free.
- [ ] Failure modes: device lost, hang, stale data, CPUs without AVX, NVIDIA/Intel/old AMD, Wine/Proton.
- [ ] New Vulkan code queries the feature and has a fallback; nothing hardcoded for one GPU.
- [ ] Invariants kept: queue order, GPU-written memory never frozen, VRAM pressure path, pins freed.
- [ ] Players without the feature see no change. The default for everyone stays for one setup's sake.
- [ ] New behaviour behind a switch. A new setting described in `totk.ini.example` and `docs/settings.md`.
- [ ] Player-visible text in the catalogs, in every menu language (`runtime/ui_text.c`, `installer/text/*.json`).
- [ ] Copied code: allowed licence, header kept, row in `THIRD_PARTY_NOTICES.md`. No GPL code.
- [ ] Generated C not edited by hand.
- [ ] Parts we cannot run (Linux, HDR, RTX-only) gated away from everyone else and noted as unrun.

## Proof

- [ ] Repro before the fix and the same repro after, side by side.
- [ ] Lifter or CPU headers: differential tests against Unicorn; `jumptables_check.py` 0 disagreements.
- [ ] Runtime: `--selftest` and the four probes pass.
- [ ] Settings: `--probe-cfg` and `tests\settings_doc_check.py`. Prompts: `romfs_prompts_test.py`, overlay generated
      for a game check.
- [ ] Frame path: whole heavy-scene runs off/on, two each at least, by game time, quiet machine, warm cache prepared
      in that build dir. No fps loss, zero new slow frames.
- [ ] Rendering: shots or a frame dump in a scene where the change acts; no black or flat frames; no new validation
      errors.
- [ ] Perf PR: gain ceiling computed for weak hardware.

## Decision

- [ ] Taken, partly taken or rejected, with the reason written down and posted to the PR.
- [ ] What missed the project's scheme brought to it while merging.
- [ ] Bugs found outside the PR reported, not fixed on the spot.
- [ ] Merged into `dev` from `task/<tag>`. `main` only with the owner's yes.
- [ ] `python tools/check_content.py --staged` before the commit.
- [ ] Commit message: a plain English sentence, no `feat:`/`fix:`.
