# Profile-guided build

A profile-guided build compiles the generated code and the runtime at -O2 with an execution profile: a record of
which functions and branches ran while someone played. Hot paths are laid out straight, hot calls are inlined, and code
that never ran is compiled for size. On 1.4.3 a trained fight ran 12% faster per frame and an untrained fight about
6%; on 1.2.1 the trained fight ran 11% faster.

Each game version has its own profile, `pgo-v121.profdata` and `pgo-v143.profdata`, trained on that version's code.

## How a build uses a profile

- `build_game.py --pgo <file>.profdata` compiles with that profile. Every compile depends on the file, so a new
  profile rebuilds everything.
- `tools/pgo.py merge` writes the profile to `build\pgo\pgo-<ver>.profdata`, next to its record `pgo-<ver>.json`. The
  record holds the git tree of `recomp/` it was trained on and the digests of the runtime headers the generated C
  includes (`cpu.h`, `simd.h`, `simd_fp.h`).
- `make_release.py` ships every profile in `build\pgo` in the release's `data\` folder. It warns when a record names
  another `recomp/` tree than the one released, or a header digest differs. The runtime libraries of the release
  compile with the newest version's profile.
- The installer builds a player's game with `data\pgo-<ver>.profdata` when the release has one for that version, and
  at -O1 without it.

A profile only fits the code it was trained on. clang matches each function by name and control-flow hash, and a
function whose code changed loses its counts without a warning (`build_game.py` passes `-Wno-backend-plugin`, which
hides the "hash mismatch" messages). The inline helpers of `cpu.h`, `simd.h` and `simd_fp.h` become part of every
lifted function, so a change to them invalidates the profile as surely as a generator change: one change to the
floating-point helpers left a profile with a fifth of its weight and 2% of its gain.

## When to retrain

- After a change to the lifter or the generator (`recomp/`) that changes the generated C. Patch table changes
  (`recomp/patches-<ver>.txt`) touch only the patched functions and can wait for the next retrain.
- After a change to `runtime/cpu.h`, `runtime/simd.h` or `runtime/simd_fp.h`.
- Before a release whose `make_release.py` run warns about a profile.

Train both versions from the same commit, from a clean `recomp/`.

## Train

### 1. Instrumented build

MSYS2's clang has no profile runtime, so the instrumented build uses llvm-mingw (unpack it under `toolchain\`):

```powershell
$llvm = "toolchain\llvm-mingw-<date>\llvm-mingw-<date>-ucrt-x86_64"
.venv\Scripts\python tools\build_game.py --version v143 --out build\v143-pgogen --toolchain $llvm --pgo gen
```

Regenerate `generated\<ver>` first if it is older than the commit you train on. If `totk.exe` stops at start with
`0xc000007b`, copy `libiconv-2.dll` from `C:\msys64\ucrt64\bin` next to it.

`totk.exe` writes `build\v143-pgogen\pgo\default_<id>.profraw` every minute and at exit. Each run of the same exe adds
to that file.

### 2. Shaders

Copy your `totk.ini` into the build folder and prepare the shaders (`tools\prepare_shaders.py build\v143-pgogen
--version v143`). Shader preparation runs `totk.exe` too, so move `pgo\*.profraw` out of the build folder afterwards:
its counts describe shader building, not play.

Keep the default settings, with the GPU command recorder on (`vk_recorder = 1`, the default).

### 3. Training scenes

The instrumented build runs slower than a normal one. Count each scene's time by the game clock, not by the wall
clock. Spend 90 to 120 seconds on each of the first four scenes, where most frames are spent, and about 60 seconds on
each of the others, which cover code the first four do not reach.

| Place | What to do | Time |
| --- | --- | --- |
| Lookout Landing | Stand in the middle of the town and turn the camera slowly all the way round, then walk through the crowd. | 120 s |
| Kakariko Village | The same: a slow camera turn, then walk between the houses. | 100 s |
| A fight with a Battle Talus | Fight it with weapons and the bow. | 110 s |
| The Depths, near a Lightroot | Turn the camera over the lit area, throw a Brightbloom Seed. | 90 s |
| The Depths, away from any light | Walk and turn the camera in the dark. | 60 s |
| The edge of a sky island | Run off the edge, skydive, open the paraglider. | 60 s |
| Outdoors in the rain | Walk and look around while it rains. | 60 s |
| A cave with Brightbloom plants | Walk in and fight the Moblins and Bokoblins inside. | 60 s |
| Inside a shrine | Walk through the first rooms. | 60 s |
| A Moblin camp | Fight several Moblins at once. | 60 s |
| Next to Yunobo | Use his ability: roll him into something. | 60 s |
| Hyrule Field, by the standing stone pillars | Run across the field, also backwards past the pillars. | 60 s |

Keep two scenes out of the training, so that you can measure on scenes the profile has not seen: Hyrule Field in tall
grass, and a fight with Moblins and Zonai constructs at once.

With a save at a scene, `tools\run_game.py` plays it without you: `--save <backup> --script <input file> --seconds
<game time>` ([agents/running.md](agents/running.md)). Without a save, play the scenes by hand.

### 4. Free play

Then play for 15 to 25 minutes and touch every kind of thing the game does, so that no common path is left without
counts:

- Run, sprint, climb, swim and glide; ride a horse; jump from a tower and skydive.
- Fight with one-handed and two-handed weapons, a spear and the bow; shield parry, flurry rush, fused arrows.
- Use every ability: Ultrahand (build a vehicle with a fan and wheels and drive it), Fuse, Ascend, Recall, Autobuild.
- Call the sages and use their abilities: Tulin, Riju, Sidon, Yunobo, Mineru.
- Open the inventory, the map and the camera; cook at a pot; talk to people in a town.
- Travel quickly to a few places, so that areas load; enter and leave a shrine and the Depths.
- Play in daytime and at night, in clear weather and in rain or snow.

### 5. Merge

```powershell
.venv\Scripts\python tools\pgo.py merge build\v143-pgogen --version v143
```

writes `build\pgo\pgo-v143.profdata` and its record. Merge right after the training, from the checkout and commit the
instrumented build was made in: the record reads the `recomp/` tree from git and the header digests from the build's
Ninja deps log.

Repeat steps 1 to 5 with `v121` for 1.2.1.

## Measure

Build the same code three ways in three folders with llvm-mingw, as the installer does: with the new profile, with
the old one and with none. Prepare the shaders in each folder. Then run the held-out scenes and one trained scene
in the order new, old, none, none, old, new, and compare the mean frame time over the same stretch of game time. One
run of a fight can vary by up to 15% from the next, so a profile counts as better only when it wins both pairs.
