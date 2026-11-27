# TotK Recomp

Static recompilation of *Tears of the Kingdom* (Switch, ARM64) into a native Windows x86-64 exe with a Vulkan renderer. Design: [docs/architecture.md](docs/architecture.md). Toolchain, build commands and tests: [docs/building.md](docs/building.md).

## Game content boundary

This repository is public. Game data lives outside it and every derived file stays in ignored folders.

- The user's game and key paths are in `config.local.toml` (ignored). Pass the keys file path to tools; the tools read it themselves. Key values stay out of your output, logs, commands and files.
- Everything extracted or generated goes under `build/` or `generated/`.
- Every file the code generator emits carries the marker defined as `GENERATED_MARKER` in `tools/check_content.py`.
- Run `python tools/check_content.py --staged` before proposing a commit. It is the same check CI runs.

## Reusing code

`refs/` holds the upstream checkouts (`tools/fetch_refs.sh` recreates it). Read-only.

- Copy only MIT, BSD, ISC, zlib or Apache-2.0 code, keep its header, and add a row to `THIRD_PARTY_NOTICES.md`.
- yuzu, suyu, OpenSmash-Ultimate and other GPL code: read for behaviour, write our own.
- `tools/vendor/` stays byte-identical to upstream; Windows changes go in `tools/extract.py`.

## Code

- Build C with MSYS2 UCRT64 Clang (`C:\msys64\ucrt64\bin`). Generated C uses GNU extensions, so `cl.exe` is out. Call Ninja from that directory by full path or put it first on PATH.
- Two Python environments: `.venv` (3.12) for the lifter and the build, `.venv-py314` for the extraction and shader scripts, which need `compression.zstd`.
- Generated C is never edited by hand: change the generator (`recomp/`).
- Every lifter change passes the differential tests against Unicorn ([docs/building.md](docs/building.md), "Checks"). After changing `recomp/jumptables.py`, `tests\jumptables_check.py` reports 0 disagreements on 1.2.1 and 1.4.3.
- `recomp/layout.py` alone decides module bases.
- Graphics bugs: [docs/maxwell.md](docs/maxwell.md) lists the Maxwell behaviour the Vulkan path must match and the TotK bugs other ports fixed.
- Comments say what the code does and why, in words that stand on their own. Cite our code by file plus function (a line number may follow as a hint) and upstream code by project, file and function: `refs/` moves, so its line numbers rot. Commit hashes belong to the attribution of copied code.

## Running and debugging

- Running the game, screenshots, frame dumps, timings: [docs/agents/running.md](docs/agents/running.md).
- Investigating a bug, glitch or crash: search [docs/agents/symptoms.md](docs/agents/symptoms.md) by the player's words first, then follow [docs/agents/debugging.md](docs/agents/debugging.md). Read the code before running the game.

## Pull requests

Preparing a pull request: target `dev`; the checks by changed area, the proof and the performance runs it carries are in [CONTRIBUTING.md](CONTRIBUTING.md), "Pull requests". Reviewing one: [docs/pr-review-kit/](docs/pr-review-kit/).

## Software factory (totk-factory)

`/totk-factory <n>` in Claude Code takes issue `#n` of valentderah/totk-pc-recomp through intake, build, gate and review and ends with a pull request against `dev` that a person merges; nothing in it merges. One issue at a time. The skill and its parts are in [tools/factory/](tools/factory/) (MIT, [tools/factory/LICENSE](tools/factory/LICENSE)):

- [SKILL.md](tools/factory/SKILL.md): the commands (`/totk-factory <n>`, `/totk-factory` for status, `/totk-factory <feedback>`, `/totk-factory drop`), the loop and resuming.
- [README.md](tools/factory/README.md): the design, the file layout, the lock classes and the install.
- [agents/](tools/factory/agents/): the intake, builder and reviewer instructions.
- [gate.py](tools/factory/gate.py): the gate before review (clean tree, protected paths, `check_content.py`, the `run_all` quick set green, each red test checked against the branch's merge base to tell the branch's from `dev`'s).
- [lock.py](tools/factory/lock.py): machine-wide locks so checks, builds, game runs and timed runs do not overlap. Any agent on the machine, in the factory or not, can start builds, game runs and timed runs through it.
- [dashboard.py](tools/factory/dashboard.py): one page (or JSON) with every job, what waits for a person, the locks and the worktrees.

Job state goes to `build/factory/<n>/` and worktrees to `.worktrees/<n>-<slug>` (both ignored). `.claude/` is local, so install the skill once per machine from the main checkout:

```powershell
New-Item -ItemType Directory -Force .claude\skills | Out-Null
New-Item -ItemType Junction -Path .claude\skills\totk-factory -Target (Resolve-Path tools\factory)
```

## Writing

Code, comments, docs and commit messages are in English. Commit messages are plain sentences without type prefixes.
