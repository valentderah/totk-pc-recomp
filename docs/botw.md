# Breath of the Wild

*The Legend of Zelda: Breath of the Wild* 1.9.0 builds from this repository with the same lifter, runtime and Vulkan
renderer as *Tears of the Kingdom*. This is early work for developers: there is no installer or release for it yet.

The same rule as for *Tears of the Kingdom* holds: no game files are included, you need your own copy dumped from your
own console, and the build contains the game, so it is for your own use only.

## Status

Tested on Windows (RTX 4080 SUPER) with the 1.9.0 update (v1114112) over the base game:

- The start: the system logo, the title screen and its menu (Continue, New Game, Options, amiibo, DLC), the save list
  and a save load. Text and voices follow the system language.
- Play in the Shrine of Resurrection with the HUD, sound and voices, at the game's own 30 fps; autosaves are written
  and load again.
- Three minutes of play with no crash.

Not done or not checked yet:

- The Great Plateau and the rest of the world, prerendered movies, a gamepad (the keyboard works), long sessions.
- The game's colours are wrong for one or two seconds after a load, then right.
- The features made for *Tears of the Kingdom*: 60 fps and the other frame-rate patches, the resolution, field of
  view and shadow patches, the RomFS patches, cheats. The game runs at its own 30 fps and 1080p.
- The DLC (`aoc: no add-on content`), the installer, `tools/run_game.py` and `--seconds`: their world clock starts
  at *Tears of the Kingdom*'s player actor, which this game does not have.
- Textures that the game places 512 bytes into a 1 KiB block (NVN aligns them to 512, the NVIDIA Vulkan driver binds
  images at 1 KiB) cannot share their memory pool's memory (`vk_texture.c` bind_pool_memory): they get memory of their
  own, and a second texture object over the same storage takes their content only when it is created.

## Building

The steps of [building.md](building.md) with the version `botw190`. The extraction needs the base game and the 1.9.0
update as NSP or XCI (unpack NSZ / XCZ first):

```powershell
.venv-py314\Scripts\python tools\extract.py --base <base game.nsp> --update <1.9.0 update.nsp> --keys <prod.keys> --work build
.venv\Scripts\python tools\build_xlate.py
.venv\Scripts\python recomp\gen.py --exefs build\extracted\botw190\exefs --out generated\botw190 --patches recomp\patches-botw190.txt
.venv\Scripts\python tools\build_game.py --version botw190
build\botw190\totk.exe
```

`extract.py` tells the game by the base package's title ID and writes only `build\extracted\botw190`: the base game
alone is no build here. `make_romfs_patches.py` and `prepare_shaders.py` are *Tears of the Kingdom*'s and are skipped.
Saves go to `build\profile-botw190`.

## What differs from Tears of the Kingdom

[tools/games.py](../tools/games.py) holds each game's title IDs, its build version names and its expected-hash files
(`tools/data/expected-botw-v1.9.0.json`). `build_game.py` writes `rt_game` into `modules.c`, and the runtime asks
`RT_BOTW` (`runtime/host.h`) where the games differ:

- The program ID (`svc.c` GetInfo 18, the save data owner in `fs_host.c`, the mod folders in `mods.c`).
- The boot checkpoints and init counts (`boot.c`) and the loader's reference counts (`loader.c` `--probe-loader`).
- `sdk`, `subsdk0` and `rtld` are byte for byte *Tears of the Kingdom* 1.4.3's (SDK 20.5.22), so what depends on the
  SDK takes the 1.4.3 branch (`RT_SDK20`: the software keyboard's argument layout, the audio probe).
- The spin waits of `main` ([recomp/patches-botw190.txt](../recomp/patches-botw190.txt), from `tools/spin_waits.py`).
  Without them the game's job queue (`main+0xef62dc`) holds a virtual core while the owner of its lock waits for one,
  and the game stops after its first frame. The lines of `subsdk0` and `sdk` are those of `patches-v143.txt`.
- The depth mode (`nvn_api_core.c` nvnDeviceInitialize): the game never calls `nvnDeviceSetDepthMode` and keeps NVN's
  default, near = -w ([maxwell.md](maxwell.md) 1.5). Its final copy into the window is drawn at z = -1, which Vulkan's
  0..w clip range drops: the window stayed black while the frame was right. The runtime now starts every device in
  that mode; *Tears of the Kingdom* sets the same mode itself, so nothing changes for it.
- Command handles (`nvn_api_cmd.c` copy_touch): the game hands a recorder's 512-byte control block to the next recorder
  at once and keeps calling the first handle thousands of frames later. Hardware runs what the block holds then, the
  newer recording, and so does a released handle here. Savage Wind Recomp models the same thing differently, with
  shadow copies of command memory that a later recording over their bytes retires (`runtime/nvn/cmdbuf.c`).
- System calls *Tears of the Kingdom* does not make, answered by the runtime: the Labo VR sensor and VR mode, the gyro
  drift mode, the Joy-Con assignment and the controller applet (`ov_input.c`), `ListAllUsers`, the steady clock's time
  point and span (`ov_system.c`), `CommitSaveData` (`fs_host.c`), and the audio renderer's voice drop parameter
  (`audren.c`, commands 12 and 13).

## Other projects

- [Savage Wind Recomp](https://github.com/Revivedx/SavageWindRecomp) (MIT) is a static recompilation of this game
  alone, built on LAS Recomp like this one, and much further along: 60 fps, the DLC, PC options, a launcher. This
  repository already takes parts of it (THIRD_PARTY_NOTICES.md); its code is a reference for this game's behaviour.
- [nxnative](https://github.com/seedds/nxnative) (MIT) runs the game's own ARM code on Apple devices; its notes on this
  game (`dm/games/01007EF00011E000`) list the frame-rate sites of 1.9.0.
