# Profile-guided optimisation

The game code is compiled from generated C: millions of branches whose taken/not-taken mix depends on the scene. Without
a profile the compiler guesses (loops taken, `if` not taken, every function equally likely), lays the code out for that
guess and inlines by size. With a profile it knows which of the generated functions carry the frame, how each branch
actually goes, and where the cold paths are — the same source, faster code, no behaviour change. Expect a few percent on
the game's threads; the win shows up as fewer instruction-cache misses and better block layout, so it is largest exactly
where the port is weakest (crowded scenes, the render thread's hot loops).

Two builds are involved: a **training** build that counts, and the **optimised** build that reads the counts.

## The loop

```powershell
# 1. Instrumented build: build/v143-pgo-gen/totk.exe
python tools/build_game.py --version v143 --pgo gen

# 2. Play it. Every run adds counters; keep them apart with LLVM_PROFILE_FILE
$env:LLVM_PROFILE_FILE = "J:\path\to\game\pgo-raw\run1-%p.profraw"
.\totk.exe

# 3. Merge the runs into one profile: pgo/totk-v143.profdata
python tools/pgo.py merge --version v143

# 4. Optimised build: build/v143-pgo/totk.exe
python tools/build_game.py --version v143 --pgo use
```

Step 4 takes `pgo/totk-v143.profdata` by default; `--pgo-data FILE` names another. Both builds default to their own output
directory (`build/<version>-pgo-gen`, `build/<version>-pgo`), so the play build in `build/<version>` is untouched and the
instrumented and optimised objects never mix.

`python tools/pgo.py show --version v143` prints what the profile holds: the number of functions, the total count and the
hottest functions. A useful profile has the game's functions at the top; a profile trained on the title screen alone does
not.

## What to play during training

Train on what should be fast, in one or a few runs: an hour of ordinary play through a town, a field, a fight, the Depths
and a shrine covers most of the hot code. Menus, cutscenes and the title screen barely matter. Run at the frame rate and
resolution you care about — the counters are branch and function counts, so a session at 60 fps in a busy town says more
than twice as long at the title screen.

Close the game normally when a run is done: the counters are written as the process exits, so a crash loses that run (the
next run starts a new file). Several runs merge into one profile; every run counts the same.

## Toolchain: only the training build is special

The instrumented build links clang's profiling runtime (`libclang_rt.profile`). MSYS2's **UCRT64** clang does not ship
it, so `--pgo gen` stops before compiling and says so — train with `--toolchain <llvm-mingw>` (the toolchain the release
build uses, which bundles the runtime). `tools/build_game.py` tests the link with a trivial program before it starts, so
another clang that has the runtime is accepted without being listed here.

The optimised build needs nothing extra: `-fprofile-instr-use` is a compile-time input, with no runtime library, no
slowdown and nothing to ship beside `totk.exe`.

## When a profile stops matching

LLVM keeps a function's counters under its name and a hash of its control flow. A function whose body changed since the
training is compiled without profile data — clang says nothing about it, the build simply gets slower there. So a profile
belongs to:

- the game version it was trained on (1.2.1 and 1.4.3 are different code);
- the generator that emitted the C (`recomp/`) and the runtime it was trained with — re-merge after changing either;
- the same `--cpu` level is not required, but train the way you ship.

`tools/build_game.py` passes `-mllvm -static-func-full-module-prefix=false` in both builds: a static function's profile
name is then its file name, not its absolute path, so a profile trained in one checkout still matches a build made in
another directory (a player's install, a release build, a worktree).

## Shipping a profile

The `.profdata` is derived from the user's own copy of the game, like everything else the build makes: it is not in this
repository (`.gitignore` has `pgo/`) and must not be committed. It can be published as a release asset next to the
installer, which then builds with it:

```powershell
python tools/build_game.py --version v143 --pgo use --pgo-data <downloaded>\totk-v143.profdata
```

That is the point of the whole exercise for players: they get the optimised build without running an instrumented one. The
installer does not pass the flag yet (`installer/steps.py`).

## Cost

The instrumented build is roughly twice as large and 1.5–3× slower at run time — it is for training, not play. Compile
time is close to a normal build. The optimised build compiles at the normal speed and its code is usually a little
smaller.

## Under the hood

`--pgo gen` adds `-fprofile-instr-generate` to the generated and the runtime compile rules and to the link line (that is
what links the profiling runtime); `--pgo use` adds `-fprofile-instr-use=<file>`. `tools/pgo.py merge` is
`llvm-profdata merge` with the failure mode that skips a run killed while it was writing its file, plus the summary.
