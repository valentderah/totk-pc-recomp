# PR review kit

Rules and background for a tester or reviewer who takes community PRs into `dev`. One goal: no PR breaks the
game's behaviour or costs frames on weak hardware.

## The short version

1. A PR description and "works for me" prove nothing. Read the diff yourself and check it against `refs/`, the
   Vulkan spec and upstream issues.
2. Almost every change needs a test: a repro before and after, a probe, a differential test, or whole runs with the
   switch off and on.
3. The developer's machine is fast; players run weak machines. "Neutral here" says nothing about them, in either
   direction.
4. A player without the problem sees no change: the new path sits behind a switch and the old path stays as it was.
5. Nothing goes into `main` without the project owner's explicit yes. Accepted work lives in `dev` first.

## Files

| File | Covers |
|---|---|
| [01-rules.md](01-rules.md) | triage order, what a PR needs, reasons to reject, merging and commits |
| [02-performance.md](02-performance.md) | target hardware, gain ceiling, how to measure, what the hot path must not do |
| [03-correctness.md](03-correctness.md) | lifter, tests and probes, GPU portability, Maxwell behaviour, timers, threads, game content |
| [04-knowledge.md](04-knowledge.md) | invariants of recent changes, fixed bug classes, open risks, past PR decisions, our mistakes |
| [05-checklist.md](05-checklist.md) | the per-PR checklist |

The public docs this kit builds on: [docs/architecture.md](../../docs/architecture.md),
[docs/building.md](../../docs/building.md) ("Checks"), [docs/maxwell.md](../../docs/maxwell.md),
[docs/settings.md](../../docs/settings.md), [docs/tester-guide.md](../../docs/tester-guide.md), `AGENTS.md`.
Working with a coding agent: give it this folder and `AGENTS.md` before the first review.

State as of 2026-11-06. The code and `docs/` win over this text where they differ.
