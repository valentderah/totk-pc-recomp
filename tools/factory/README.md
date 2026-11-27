# totk-factory

A small issue-to-pull-request loop for Claude Code, made for this repository only. `/totk-factory <n>` takes issue
`#n` through intake, build and review and ends with a pull request to `dev` that a person merges. One issue at a time.

The design borrows ideas from ZazenCodes' "Software Factory for Claude Code" (v2): issues as the queue, a file per
step, a scripted gate before any review, verdicts on the first line of each file. No code or text is copied from it;
everything here is written for this repository and is under the MIT license in [LICENSE](LICENSE).

## The loop

```
/totk-factory <n>
  intake   (agent)   GO | ASK | SPLIT | SKIP          -> intake.md; ASK/SPLIT/SKIP become an issue comment and stop
  worktree (script)  tools/worktree.py new <n>-<slug> --base origin/dev
  build    (agent)   code, commits, gate.py until PASS, the checks intake listed, draft PR to dev -> round-K/build.md
  gate     (script)  gate.py: clean tree, protected paths, check_content, run_all quick set vs. the base -> round-K/gate.md
  review   (agent)   fresh eyes on the diff against docs/pr-review-kit/ -> round-K/review.md: PASS | CHANGES
                     CHANGES -> build round K+1 (at most 3 rounds)
  finish             PR ready for review, summary comment on the PR; a person merges
```

Three agents, one script, run in sequence by the session that runs the skill:

| Step | Who | Writes | Replaces in the original |
|---|---|---|---|
| intake | agent, opus | `intake.md` | intake and spec writer |
| build | agent, opus | `round-K/build.md`, the branch, the PR | builder; the builder runs the gate itself |
| gate | `gate.py` | `round-K/gate.md` | the gate and `checks.sh` |
| review | agent, opus | `round-K/review.md` | code, UX, UI and security reviewers |
| finish | the skill | a PR comment | the approver: nothing merges without a person |

What is left out on purpose: parallel jobs, unattended runs, labels for the queue, a job state machine, cost
tracking, per-job ports.

## Dashboard

`python tools/factory/dashboard.py` writes `build/factory/dashboard.html`: every job with its steps and verdicts, what
needs a person (ASK, SPLIT, SKIP, BLOCKED, a failed gate, CHANGES), each job's pull request, the locks and who holds
them, running games and the worktrees. `--serve [PORT]` serves it on http://127.0.0.1:8765/ and rebuilds it on every
request; the page reloads itself every 30 s; `/status.json` there, or `dashboard.py --json`, gives the same data as
JSON, which the session that runs the factory reads to answer "how is it going". It reads files, lock state,
`git worktree list` and `gh pr view`, and changes nothing. State is the files in the job folder.

## Why the gate is a script and the reviewer an agent

- The builder runs `gate.py` and may not hand over until it passes. The skill then checks that the `COMMIT:` in
  `gate.md` is the pushed head, so a gate on older code does not count. A failing build never reaches the reviewer.
- `gate.py` runs the main checkout's copy of the checks, so a branch cannot change what judges it, and it fails a
  branch that changes the factory, `tools/check_content.py` or `.github/workflows/`.
- The gate asks for a green `tests/run_all.py` quick set (the tests that need no game files). For each red test it
  says whether it also fails on the branch's merge base: if not, the branch broke it; if so, `dev` is red, and the
  builder reports BLOCKED unless fixing that test is the issue's work.
- The reviewer is a separate agent with none of the builder's context. A builder that reviews its own diff finds
  what it already thought of.

## Locks

The factory runs one job at a time, but the machine is shared: another agent session, a build, or a person playing.
`lock.py` keeps heavy work from overlapping. Wrap a command in its class:

```
python tools/factory/lock.py <cc|tests|build|game|measure> -- <command> [args...]
python tools/factory/lock.py status
```

| Class | Checks | Runs beside |
|---|---|---|
| none | `tools/check_content.py`; `run_all` tests that need nothing (Python only); `settings_doc_check`; reading logs | everything |
| `cc` | `tools/xlate_smoke.py`; the `totk.exe` modes that never open a Vulkan device: `--selftest`, `--probe-loader`, `--probe-cfg`, `--probe-dpi`, `--probe-mods`, `--probe-discord`, `--probe-playtime`, `--probe-kernel`, `--probe-ipc`, `--probe-services`, `--probe-fs`, `--probe-gfx-abi`, `--probe-nvn`, `--probe-nvn-tex`, `--probe-shader` (the translator, on the CPU), `--probe-movie` | cc, tests, build |
| `tests` | the gate; `tests/run_all.py` (the quick set); a single test that builds a harness into `build/test/` (`flags_check`, `excl_mt`, the `*_check` harnesses, `loadwin_pace`, `tests/diff/simd.py`, `atomic.py`): one at a time, since every worktree shares the main checkout's `build/` | cc, build |
| `build` | the lift (`recomp/gen.py`), `tools/build_game.py`, `tools/build_xlate.py`, `tools/prepare_shaders.py` and `--prepare-shaders` / `--warm-shaders` (a Vulkan device, for pipeline builds only), `tools/shader_census.py --translate`; `tests/diff/lift.py`, `func.py`, `tests/jumptables_check.py` | cc, tests |
| `game` | anything that starts `totk.exe` on the GPU without timing it: the modes that open a Vulkan device (`--probe-window`, `--probe-vkmem`, `--probe-vkexec`, `--probe-astc`, `--probe-vkdevice`), `run_game.py` without `--quiet`, a play run, `TOTK_VK_VALIDATION=1` runs, screenshots, `mods_romfs_check`, `installer_gui_test` | nothing |
| `measure` | timed runs: `run_game.py --quiet`, `TOTK_AB` runs, before/after fps (CONTRIBUTING.md, "Performance measurements") | nothing |

Which modes open a Vulkan device was found by running each mode once under `lock.py game` and looking for the
`vk: using gpu` log line; check a new probe the same way before putting it in a row.

- `build` and `tests` are each one at a time: two quick sets in two worktrees break each other's harness builds in
  the shared `build/test/`.
- `game` and `measure` run alone: each waits until no `cc`, `tests` or `build` work holds a lock, no other `game` or
  `measure` runs, and no `totk.exe` started outside the factory runs; nothing else starts until it ends. A timed run
  that `run_game.py --quiet` starts outside the factory (it holds `build/.gpu-run.lock`) blocks everything the same
  way.
- The locks are OS file locks under `build/factory/locks/`, so a crashed holder never blocks anyone. They only bind
  work that takes them: work started by hand outside `lock.py` is seen only through `build/.gpu-run.lock` and the
  `totk.exe` process check.

## Files

```
tools/factory/SKILL.md            the skill: commands, the loop, resuming
tools/factory/agents/intake.md    the intake's instructions
tools/factory/agents/builder.md   the builder's instructions
tools/factory/agents/reviewer.md  the reviewer's instructions
tools/factory/gate.py             the gate (standard library only)
tools/factory/lock.py             the locks (standard library only)
tools/factory/dashboard.py        one page with every job, its blockers, the locks and the worktrees

build/factory/active              the issue number of the job in progress (ignored, like all of build/)
build/factory/<n>/issue.md        the issue and its comments, quoted
build/factory/<n>/intake.md
build/factory/<n>/round-<K>/build.md, gate.md, gate.log, review.md
build/factory/<n>/feedback-<K>.md the maintainer's words for another round
build/factory/<n>/done.md         how the job ended
build/factory/dashboard.html      the dashboard (dashboard.py)
.worktrees/<n>-<slug>/            the job's worktree, branch task/<n>-<slug>
CLAUDE.local.md, notes/           optional: the machine's own process (ignored; worktree.py links them into every
                                  worktree). When present, the agents read them; AGENTS.md and the factory win.
```

## Install (once per machine)

`.claude/` is local to the machine (ignored), so the skill reaches Claude Code through a link from the main checkout:

```powershell
New-Item -ItemType Directory -Force .claude\skills | Out-Null
New-Item -ItemType Junction -Path .claude\skills\totk-factory -Target (Resolve-Path tools\factory)
```

```sh
mkdir -p .claude/skills && ln -s ../../tools/factory .claude/skills/totk-factory
```

Then `/totk-factory <n>` in Claude Code from the main checkout. `gh` must be logged in with push access to
`valentderah/totk-pc-recomp`, and `origin` must point at it: the builder pushes `task/<n>-<slug>` there.

The gate runs the quick set with the checkout's `.venv` ([docs/building.md](../../docs/building.md)), which
`tools/worktree.py` links into every worktree; it refuses to run with a Python that cannot import `unicorn`. Where
the venv lives elsewhere, a junction is enough:

```powershell
New-Item -ItemType Junction -Path .venv -Target <the existing .venv>
```
