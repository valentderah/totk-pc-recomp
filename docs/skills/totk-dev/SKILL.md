---
name: totk-dev
description: Use when building, testing, fixing or reviewing changes in totk-pc-recomp, or when triaging its GitHub issues and pull requests. Covers branch builds side by side, headless game runs, A/B checks of rendering changes, issue triage and the pull request checklist.
---

# Working on TotK Recomp

This skill is the working loop for coding agents (and people) on this repository. It builds on
[AGENTS.md](../../../AGENTS.md), [docs/building.md](../../building.md) and [CONTRIBUTING.md](../../../CONTRIBUTING.md);
read those first. The rules there win over anything here.

To use it as a Claude Code skill, copy or link this folder to `.claude/skills/totk-dev/` (ignored by git).

## Ground rules

- Nothing game-derived enters git. `python tools/check_content.py --staged` before every commit.
- Generated C is never edited: change `recomp/`.
- Code, comments, commits, pull requests and issue comments are in English; commit messages are plain sentences.
- One topic per pull request. Say how it was tested: game version, scene, how long, which settings.

## Build a branch next to the others

Keep each branch in its own generated and build folders, so a test build never overwrites the one you play or
compare against. Use a `git worktree` for a second branch instead of switching the checkout while a build reads it.

```powershell
# lift: always pass the patch table, or the frame-rate, resolution and other patch sets are missing
# (the game then runs at 30 fps and the patch-set settings do nothing)
.venv\Scripts\python recomp\gen.py --exefs build\extracted\v143\exefs --out generated\v143-<branch> --patches recomp\patches-v143.txt
.venv\Scripts\python tools\build_game.py --version v143 --gen generated\v143-<branch> --out build\v143-<branch>
# the translator of the same branch (a branch that changes tools\xlate\patches needs its own)
.venv\Scripts\python tools\build_xlate.py --out build\xlate-<branch>
copy build\xlate-<branch>\totk_xlate.exe, build\xlate-<branch>\totk_xlate.id build\v143-<branch>\
build\v143-<branch>\totk.exe --selftest
```

Regenerate whenever `recomp/` or `recomp/patches-*.txt` changed; a runtime-only change needs only `build_game.py`
(it recompiles what changed). A new translator invalidates the shader cache of every folder that uses it: run
`totk.exe --prepare-shaders` there before judging frame times.

## Run the game headless

```powershell
python tools\run_game.py build\v143-<branch> --seconds 25 --tag <name> --shot build\<name>.png --env TOTK_<KEY>=<value>
```

- Any `totk.ini` key can be forced with `--env TOTK_<KEY>=...` (the keys are in `runtime/cfg.c`).
- The log is `build\v143-<branch>\logs\<name>.log`. Check the `FATAL` lines, the `boot: frames ... validation errors 0,
  shader failures 0` line and the `frametime: exit` summary.
- `--shot` turns asynchronous pipeline builds off, so the image is complete.
- `tools\input\load_save.txt` presses A at fixed frames. When the title screen is late (cold shader cache,
  `astc = cpu`) the presses are missed and the run idles on the title screen until it is killed. Wrap runs in a
  timeout, kill `totk.exe` after one, and rerun before blaming your change; check that a baseline run of the same
  build does it too.

## Prove a change in the game

- Run both the path you changed and the default path. A texture upload change, for example, runs with
  `astc = cpu` and with the default GPU decoder.
- For anything visual, compare screenshots of the same scene from the same build with the change on and off
  (a temporary environment switch you delete before the commit). Two runs differ slightly in time of day and
  weather; a real bug shows as missing shadows, haze, black or flashing areas.
- Put the before and after images side by side and look at them; a mean pixel difference alone does not tell
  weather from a bug.
- Do not add extra submits of the upload batch (`vk_upload_flush`) inside texture or replay code to save memory:
  an extra mid-upload flush changed the rendering in A/B runs. Split staging buffers, keep the batch.

## Triage an issue

1. Read the issue and its comments: `gh issue view <n> --repo valentderah/totk-pc-recomp --comments`.
2. Download the attached `totk.log` and read its tail. The `FATAL:` text usually names the code path:
   `git grep -n "<fatal text>" runtime/`.
3. Check whether `main` already changed that code (a setting removed or made experimental, diagnostics added).
4. Reproduce with `run_game.py` and the reporter's settings as `--env`. GPU-vendor issues (Intel UMA, AMD Polaris)
   often cannot be reproduced on another vendor; fix those only when the log names a clear code path.
5. Fix on a branch from `origin/main`, test as above, open a pull request that says `Fixes #<n>`.
6. Answer on the issue with a verdict: fixed by a pull request (ask the reporter to confirm on their hardware),
   not reproducible (what was tried, which log is missing), or working as designed (why). Mention the reporter
   so they can close it.

## Pull request checklist

- [ ] Branch from current `origin/main`; rebase before pushing if `main` moved.
- [ ] `check_content.py --staged` passes.
- [ ] `totk.exe --selftest` passes; lifter changes pass the differential tests in docs/building.md, "Checks".
- [ ] Game run on the changed path and on the default path, with the result in the description.
- [ ] Visual changes come with a same-build A/B description.
- [ ] The description says the problem, the change and the testing, in that order.
