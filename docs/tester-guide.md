# Tester guide: running TotK Recomp on your GPU

This guide is for testing the build on a GPU other than the developer's (AMD RX 9070). You build it from source with
your own copy of the game and your own console keys. Nothing from the game is in this repository, and nothing you build
from it may be shared.

## What you need

- Windows 10 or later, an x86-64 CPU with SSE4.2 (AVX2/FMA recommended: they build the default, faster version), about
  60 GB free.
- A GPU with Vulkan 1.3 and `VK_EXT_external_memory_host`, with a current driver.
- *Tears of the Kingdom* base game (1.0.0) and the 1.1.0, 1.2.1 or 1.4.3 update, dumped from your own Switch as NSP or XCI.
- `prod.keys` dumped from your own Switch. Never send it to anyone, and never paste it into a log or an issue.

## Build

1. Follow [building.md](building.md): the toolchain, then build steps 1-6 (Python environments, `config.local.toml`
   with your own paths, extraction, the shader translator, lifting and linking, shader preparation). The result is
   `build\<ver>\totk.exe`, with `<ver>` the game version you built (`v143` or `v121`).
2. Check the build without the game: `build\<ver>\totk.exe --selftest` must print a pass.
3. To run the build on another of your machines, package a portable folder ([building.md](building.md), "Portable
   folder"). The folder holds game code: for your own machines only.
4. The installer ([installer.md](installer.md)) is the other route to a machine without the toolchain: it builds the
   game there from your own files. No installer release is published yet.

## Run and collect the log

Every run writes its log to `logs\totk.log` next to `totk.exe`; the four runs before it are kept as `totk.1.log` to
`totk.4.log`, and Open log folder in the settings menu's Advanced tab (F1) shows them. Copy the log before you start the
game again. A file stops at 64 MiB. Started with `2> file`, the game writes its log to that file instead.

From `cmd.exe` in the repository root, with the validation layer on:

```bat
set TOTK_VK_VALIDATION=1
build\<ver>\totk.exe
```

`TOTK_VK_VALIDATION=1` turns on the Vulkan validation layer (it comes with the Vulkan SDK). It makes the game slower;
do one run with it and one without (`set TOTK_VK_VALIDATION=`).

The game keeps running behind other windows (`assume_focus = 0` in `totk.ini` pauses it instead). Shaders the preparation did not cover compile on first use, so
the first minutes of play can stutter.
Play into the open field (load a save, walk around for a minute, fight something if you can), then close the window.

## Choosing the GPU

A laptop with an iGPU and a discrete GPU, or two cards, lists every GPU at the top of the log (`vk: gpu 0: ...`).

- `set TOTK_GPU_NAME=Radeon` picks the first usable GPU whose name contains that text (case does not matter).
- `set TOTK_GPU_INDEX=1` picks GPU 1 from that list.

The line `vk: using gpu N: <name>` says which one ran.

## Mods

Mods made for emulators go into `mods/` next to `totk.exe`, one folder per mod, unpacked as they come:
`mods/<name>/romfs/...` (the Atmosphere layout `mods/<name>/atmosphere/contents/0100F2C0115B6000/romfs` works
too). The game loads them at the next start. The Mods tab of the in-game menu (F1) lists them: switch a mod off, or move
it up or down. When two mods change the same file, the higher one wins.

- RomFS mods (textures, models, text, gameplay files) work.
- Mods packaged for TKMM (`.tkcl`, "TKMM only") and mods that change the same files: merge them in TKMM and set an
  export location to an empty folder inside `mods/` (for example `mods/TKMM`). That folder is then one mod here.
- Files made for another game version (names with `.Product.<number>.` that this version does not have) are left out;
  the Mods tab counts them. Look for the mod's release for your game version.
- The resource size table (`System/Resource/ResourceSizeTable.Product.<n>...rsizetable.zs`), which tells the game how
  much memory each file needs, is rebuilt at every start with mods enabled: the tables the mods ship are merged and
  the entries of files a mod made larger or added are raised. The log line `RomFS: resource size table: ...` says
  how many. A mod's table for another game version still counts for that mod's own files.
- Mods that patch the game code (`exefs/*.ips`, `exefs/*.pchtxt`, or `atmosphere/exefs_patches/<name>/*.ips`) work
  when the patches are for this game version and change only code: the game rebuilds itself with them at the next
  start (the setup's rebuild window, a few minutes) and switches them with the mod. Patches for another version are
  counted and left out; a file that writes the game's data, or that the port cannot express, keeps the whole mod off
  and the Mods tab says why. A patch over one of the port's own patches turns that built-in patch off while the mod
  is on (the Mods tab names it).
- Mods with their own code (`exefs/subsdk*` or a replaced `exefs/main`, such as UltraCam) never load, their RomFS files
  included.
- Cheats: an Atmosphère cheat file goes into a mod's `cheats` folder (`mods/<name>/cheats/<build id>.txt`, the
  build id `277178B7DBA1B6D4` for 1.4.3, `9B4E43650501A4D4` for 1.2.1). The Cheats tab of the menu switches them.
  Cheats that write memory work at the next start; for cheats that change the game's code the game opens the setup's
  rebuild window at start and comes back after it (a few minutes). The port ships no cheat files. Details: [cheats.md](cheats.md).

The log names every mod at the start (`mods: <name>: ...`) and how many of its files it serves.

## What to send back

1. `logs\totk.log` from each run. It contains no keys and no game files. The lines that matter most start with
   `vk caps:` (GPU, driver, features and the fallbacks taken), `vk: gpu`, `validation` and `gfx_shader:`.
2. GPU model, driver version, CPU, RAM, and whether it is a laptop. Copy system info in the settings menu's Advanced
   tab (F1) puts these and your changed settings on the clipboard. Say which game update you ran.
3. What you saw: how far it got (menu, loading, field), roughly how smooth it ran, and screenshots of anything wrong
   (black or missing areas, flickering, wrong colours, missing grass). Win+Shift+S is fine.
4. If it crashed or hung: what you were doing. The log of that run ends with the crash report, so send it whole.
5. For a bug you can trigger again: a recording of your input (`set TOTK_INPUT_RECORD=input.txt`, see
   [controls.md](controls.md), "Scripted input") and the save you started from.

## Steam Deck

Build or install on Linux ([installer.md](installer.md), "Linux release"), in Desktop Mode. The game then runs in either
mode; for Game Mode, add `totk` from the install folder to Steam as a non-Steam game.

The game recognises the Deck (LCD and OLED) and uses its device profile for every setting `totk.ini` does not set:
fullscreen, the screen's own 1280x800 (16:10: the camera and interface take that shape), the game's own 3D scale,
vsync and a 40 fps limit. These are starting points, not measurements. What helps most:

1. The performance overlay (F10, or both stick clicks L3 + R3) in full mode: the frame rate, mean and longest frame
   time over a minute in Lookout Landing, in the Depths and in the Sky, with the settings named.
2. The same with `fps_limit` 30, 45 and 60 and `render_scale` 50, 67 and game, so the defaults can be picked from
   measurements. Steam's own performance overlay (the ... button) adds power draw and temperatures.
3. LCD or OLED, SteamOS version (`uname -r` in Desktop Mode's terminal), and the game update.
4. Anything that looks stretched, squeezed or cut off, with a screenshot. At 16:10 the menus keep their 16:9
   proportions in the middle of the taller screen, the HUD sits at the top and bottom edges, and full-screen
   backgrounds reach them; pre-rendered cutscenes play stretched.

`device_profile = desktop` in `totk.ini` turns the profile off; `deck` turns it on elsewhere, to try the Deck's
settings on a PC.

## Switches for the developer's questions

Only set these when asked; each forces a fallback the build picks by itself on GPUs that need it.

| Variable | Effect |
|---|---|
| `TOTK_VK_NO_FLOAT64=1` | Translate shaders without 64-bit floats |
| `TOTK_VK_SSBO_ALIGN=64` | Treat storage buffers as needing 64-byte alignment |
| `TOTK_VK_SUBGROUP_PIN=0` | Do not pin shaders to 32 lanes; tell the translator the GPU's own subgroup size |

Each switch makes the first launch translate every shader again.
