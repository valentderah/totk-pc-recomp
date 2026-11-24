# VR

Play Tears of the Kingdom in a SteamVR headset: the world in true stereo 3D around you, head-tracked, with the game played
on the controller as usual. It is a **third-person** mode: you stand behind Link, and your head looks around the world
without moving Link or his aim (see "Why third person" below).

**VR problems go to the VR fork, not to the main project:**
<https://github.com/TakeyaSaito/totk-pc-recomp/issues/new?template=vr-bug.yml>

## What you need

- A PC that runs the flat game well. VR draws every frame twice (once per eye), so it needs roughly twice the frame rate
  of the flat game at the headset's refresh rate.
- A headset that works with **SteamVR**, and SteamVR set as the **OpenXR runtime** (SteamVR > Settings > OpenXR >
  "Set SteamVR as OpenXR runtime").
- **Linux:** the OpenXR loader from your distribution (`libopenxr_loader.so.1`: the `openxr` package on Arch,
  `libopenxr-loader1` on Debian and Ubuntu). **Windows:** the setup downloads it (Khronos's `openxr_loader.dll`) when
  "Set up VR" is ticked.
- Tested: Steam Frame (Linux AMD, Linux NVIDIA, Windows AMD), Valve Index (Linux NVIDIA). Windows NVIDIA is expected to
  work but has not been tested yet.

## Setting it up

1. Run the setup and tick **Set up VR**. It adds a **TotK VR** shortcut: in the app menu on Linux, in the Start menu and on
   the desktop on Windows.
2. Start SteamVR and put the headset on.
3. Start **TotK VR**.

You can also start `totk` (Linux) or `totk.exe` (Windows) with `--vr`, or put `vr = 1` in the `[vr]` section of
`totk.ini`. If the game starts flat on the monitor instead, the OpenXR loader or runtime wasn't found: the log says why
(`logs/totk.log`, lines starting with `vr:`).

### From Steam (Big Picture, SteamVR library)

Add the game as a non-Steam game: Steam > Add a Game > Add a Non-Steam Game > `totk` (Linux) or `totk.exe` (Windows)
from the install folder. Then in its Properties:
- **Launch options:** `--vr`
- tick **Include in VR Library**: without it, SteamVR shows the game's desktop window on a flat screen in the headset.

## Settings

`--vr` needs no configuration: its defaults are the values that worked across the headset tests. It draws each eye at a
square 2048x2048 picture (with the HUD laid out for that shape) at render scale 200, and the frame rate follows the
headset (a left and a right frame per headset frame). In game, **F1 > VR** has the comfort settings (camera height,
head aim); the `[vr]` section of `totk.ini` has them too, and `vr_render_scale` sets the render scale VR uses.

All VR settings are listed in [settings.md](settings.md), section "VR": the player's settings (HUD panel size and
distance, marker sizes, the level camera, controllers, ...) and, under "VR troubleshooting", one switch per VR fix. Leave
those alone normally. When you report a problem we may ask you to turn one off to see whether it changes anything.

### Performance

- Each eye is its own game frame, so at 90 Hz the game runs at 180 frames per second. If it can't keep up, choose a
  lower refresh rate for the headset (72-90 Hz is typical) and/or lower the render scale.
- A lower render scale (e.g. 150 instead of 200) helps most on slower GPUs; on slower CPUs a lower refresh rate helps
  more.

## Controllers

- **Steam Frame controllers** work like a gamepad: right A/B/X/Y = the face buttons, Menu = +, right bumper = R; left
  D-pad, View = -, left bumper = L; triggers = ZL/ZR; sticks and stick clicks. The right controller's motion works as the
  gyro (aiming).
- **Other VR controllers** use a Touch-style fallback layout (right A/B, left X/Y, grips = L/R, left menu = +).
- **A normal gamepad keeps working** next to the VR controllers; rumble and gyro follow whichever you used last.

## Known issues and tips

- **Blurry HUD and menus while the 3D world is sharp** (sharp only while the SteamVR dashboard is open): SteamVR and the
  headset are on different branches. Put SteamVR on the same branch as the headset (both stable or both beta); with a
  Steam Frame on SteamOS beta, use the SteamVR beta.
- **Windows: crashes or "room for device memory 0 MiB" in the log:** Windows counts video memory against the page file.
  Set a fixed page file so RAM + page file is about 64 GB (for 32 GB RAM: 32768-65536 MB), then reboot.
- **Steam Frame streaming on Windows:** if the picture stays black, set the stream width to automatic or at most 1344.
- **No picture in the headset, game on the monitor:** check that SteamVR is the OpenXR runtime and running before you
  start the game.

## Why third person

The goal is to play the whole game in VR comfortably and the way it was designed: the game's camera modes, Ultrahand,
the bow, gliding and cutscenes all keep working, and long sessions stay comfortable. First-person play with motion
controls is planned as an additional mode later. Help is welcome: open an issue or a discussion in the VR fork.

## Reporting a problem

Use the VR issue form: <https://github.com/TakeyaSaito/totk-pc-recomp/issues/new?template=vr-bug.yml>. It asks for:
- what you see and where (a **save** right before the problem helps most: zip `profile/save/<number>/slot_XX` from the
  install folder and attach it),
- your headset, SteamVR version and branch, OS, GPU and driver,
- `logs/totk.log` from a run where it happened,
- a screenshot or short video if you can.
