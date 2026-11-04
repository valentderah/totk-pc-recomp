# Direct mouse look (experimental, TotK 1.4.3)

The mouse is added to the PlayerCamera's **processed** right stick after the game's dead zone, curve and scripted /
stick-off checks, so slow motion is not lost and fast flicks do not saturate. The game's own inversion, idle flags
and camera turn follow unchanged. This is an optional second input path; with `mouse_direct = 0` the runtime is the
unmodified right-stick mouse.

## Prerequisites

- This repo at commit `7f38eb6` (PC controls + F1 rows) or a tree with the same mouse-camera layout.
- TotK **1.4.3** ExeFS (`main`). The game-side site is hardcoded; another version needs a new site.
- The runtime patch page after main (the build already reserves it).

## Apply and build

```
git apply mousedirect-1.4.3.patch
.venv/Scripts/python recomp/gen.py --exefs <exefs dir> --out generated/v143 --patches recomp/patches-v143.txt
.venv/Scripts/python tools/build_game.py --version v143 --exefs <exefs dir> --romfs <romfs dir>
```

`gen.py` checks the site's original word; a mismatch stops the build. `build_game.py` writes the usual `totk.exe`;
the direct path is off until `mouse_direct = 1`.

## Settings (`totk.ini` or F1 → Controls)

| key | default | meaning |
|---|---|---|
| `mouse_direct` | 0 | 1 = direct look on; switches the patch set live |
| `mouse_direct_sens` | 1 | stick units per count at 30 fps; 1 ≈ the right-stick mouse at 60 fps |
| `mouse_direct_pitch` | 1.3725 | `sign(v)*sqrt(gain*|v|)`, 70/51 matches the game's quadratic pitch to yaw; 0.2..4 |

This patch does not include the mouse gyro. The patch zeroes the mouse in scripted camera sources and where the game
ignores the stick (the sub-object's `+0x148` bit 1 and the flags' bit 0).

## Where it hooks

`main+0x874c1c` is the 1.4.3 PlayerCamera stick step (found from the 1.0.0 function at `0xd052ac`; the sub-object
moved `+0x428` → `+0x490`). The site `main+0x874cb4` (`str s9, [x20, #0x420]`) is the merge point that stores the
processed x; the next instruction stores y. The replacement reads running mouse totals and the last applied totals
from the patch page (`+0x100/+0x104` total, `+0x108/+0x10c` applied, `+0x110` stick units per count, `+0x114` pitch
gain), updates them, and stores the shaped values. No word branches and no word writes the flags.

## Limitations

- Only the 1.4.3 site; the offsets above are not portable by distance.
- The runtime drops motion the game did not apply for 8 presents (menus, loading, cutscenes) instead of saving it up.
- Context refinement (AIM/CLIMB/... via the game's ScreenMgr), virtual player keys, menu goto and game-side macros are
  not part of this patch.
- The camera still uses the game's dead zone/curve for gamepad and keyboard stick input; only the mouse skips them.

## Attribution

The site semantics and the pitch-shaping formula come from **Weeping of the Empire**
(https://github.com/daniel-madrid-07/weeping-of-the-empire @ `df0586cdc5702ca19bbebfa59697e0b6980f6237`, v0.1.8.2,
MIT, Copyright (c) 2026 daniel-madrid-07), which documents the 1.0.0 `pc_scheme` mouse veneer. This patch contains no
Weeping source code; the encoding and runtime integration are written for this runtime.
