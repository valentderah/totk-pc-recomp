# Breath of the Wild

*The Legend of Zelda: Breath of the Wild* 1.9.0 builds from this repository with the same lifter, runtime and Vulkan
renderer as *Tears of the Kingdom*, into its own `botw.exe`. This is early work. The setup installs it from the base
game and the 1.9.0 update (`--game botw`, or *Install* on its card in the launcher: [installer.md](installer.md),
"Breath of the Wild"; [launcher.md](launcher.md)); no release is published yet.

This work is based on [Savage Wind Recomp](https://github.com/Revivedx/SavageWindRecomp) by Revivedx and its
contributors (MIT), a static recompilation of this game alone: its notes on the game, its 1.9.0 frame-rate sites and
its fixes are the reference here, and the code taken from it is listed in
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) with its commit.

The same rule as for *Tears of the Kingdom* holds: no game files are included, you need your own copy dumped from your
own console, and the build contains the game, so it is for your own use only.

## Status

Tested on Windows (RTX 4080 SUPER) with the 1.9.0 update (v1114112) over the base game:

- The start: the system logo, the title screen and its menu (Continue, New Game, Options, amiibo, DLC), the save list
  and a save load. Text and voices follow the system language.
- Play in the Shrine of Resurrection with the HUD, sound and voices; autosaves are written and load again.
- Three minutes of play with no crash.
- The frame limit (`fps_limit`, `TOTK_FPS_LIMIT`) at 60, the default, or 30, with the game's clock right at either:
  `TOTK_TRACE=perf` with `TOTK_SPEED_PROBE=1` logs VFR's ratio (0.5 at 60 fps, 1 at 30) and its interval (1 and 2),
  and the seconds the game's own delta frames add up to keep pace with the wall clock (speed 1.000). The menus and
  the shrine hold 60 fps.
- No recorder thread (`vk_rec.c`): with it about one frame in eight came out whole-screen wrong (over-exposed, or a
  buffer's raw contents: the flicker) and the shrine averaged 55 fps. `TOTK_VK_RECORDER=1` turns it on again. Turning
  off the uniform snapshot (`TOTK_UBO_SNAPSHOT=0`) also removed the wrong frames, so the cause lies where the two
  meet and is not found yet; a frame dumped with `TOTK_DUMP` never came out wrong. The wrong colours for a second
  or two after a load were the same thing and went with it.

Not done or not checked yet:

- The Great Plateau and the rest of the world, prerendered movies, a gamepad (the keyboard works), long sessions.
- Frame rates above 60: the limit stops there for now, as Savage Wind Recomp needed more fixes above it (the
  physics solver's microsteps, keyframed bodies' turns, the shield-surf takeoff).
- The features made for *Tears of the Kingdom*: the resolution, field of view and shadow patches, the RomFS patches,
  cheats. The game renders at its own 1080p.
- The DLC (`aoc: no add-on content`), `tools/run_game.py` and `--seconds`: their world clock starts at *Tears of the
  Kingdom*'s player actor, which this game does not have.
- Shader preparation: the setup and the build leave it out (the shader list is *Tears of the Kingdom*'s), so the game
  translates its shaders and builds its pipelines when it first draws them.
- Textures that the game places 512 bytes into a 1 KiB block (NVN aligns them to 512, the NVIDIA Vulkan driver binds
  images at 1 KiB) cannot share their memory pool's memory (`vk_texture.c` bind_pool_memory): they get memory of their
  own, and a second texture object over the same storage takes their content only when it is created.

## Building

The steps of [building.md](building.md) with the version `botw190`. The extraction needs the base game and the 1.9.0
update as NSP or XCI (unpack NSZ / XCZ first, `tools/ncz.py`; the setup does that itself):

```powershell
.venv-py314\Scripts\python tools\extract.py --base <base game.nsp> --update <1.9.0 update.nsp> --keys <prod.keys> --work build
.venv\Scripts\python tools\build_xlate.py
.venv\Scripts\python recomp\gen.py --exefs build\extracted\botw190\exefs --out generated\botw190 --patches recomp\patches-botw190.txt
.venv\Scripts\python tools\build_game.py --version botw190
build\botw190\botw.exe
```

`extract.py` tells the game by the base package's title ID and writes only `build\extracted\botw190`: the base game
alone is no build here. `build\launcher\launcher.exe` ([launcher.md](launcher.md)) starts either game.
`make_romfs_patches.py` and `prepare_shaders.py` are *Tears of the Kingdom*'s and are skipped.
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
- The frame rate ([recomp/patches-botw190.txt](../recomp/patches-botw190.txt), set realtime, after Savage Wind
  Recomp's `runtime/hle/framepace.c`): the game steps its time by `ksys::VFR`'s interval ratio, and the patched
  `VFR::updateInterval` takes the ratio, the interval and its changed flag from the patch page, which
  `runtime/frame_rate.c` fills at every present from the display intervals (the same real-time step as *Tears of the
  Kingdom*'s). The menus' speed, the held buttons' repeat, the arrows' fall and the sound system's frame follow the
  same ratio. The pacer holds the presents to the limit, so the game's own 30 fps interval only feeds VFR.
  `TOTK_REALTIME=0` gives the game its own clock and 30 fps back.
- System calls *Tears of the Kingdom* does not make, answered by the runtime: the Labo VR sensor and VR mode, the gyro
  drift mode, the Joy-Con assignment and the controller applet (`ov_input.c`), `ListAllUsers`, the steady clock's time
  point and span (`ov_system.c`), `CommitSaveData` (`fs_host.c`), and the audio renderer's voice drop parameter
  (`audren.c`, commands 12 and 13).

## Porting from Savage Wind Recomp

What Savage Wind Recomp (`docs/SAVAGE-WIND.md` at `bf6146c`) fixed for this game, against this runtime. Each item is
ported on its own and measured in the open world (the Great Plateau save: one-frame glitches per 360 frames, the audio
trace's underruns, a dump of the mix).

| Savage Wind fix | Symptom here | State here |
|---|---|---|
| The game's own AudioRenderer: the `sdk`'s server and DSP command processor run in the process (CPU render device, manual execution), its DeviceSink mix goes to SDL; the HLE `audren:u` mixer stays behind a switch ("Audio") | crackle the runtime does not see: no underrun, a clean mix | to port; `sdk` is TotK 1.4.3's, so both games get it |
| Images the GPU drew kept after release, with a sum of the memory they cover, and taken back by a texture made again there (the save dialog's thumbnail, UI panels drawn once) | black save thumbnails (`caption.jpg`) | to port |
| Constant buffers copied when the commands are translated; the CPU's wait on the frame's fence one frame late, at most two frames queued; CPU-read targets (sky, exposure, eight 128x128 in turn) one frame late | frames without fog and shadows, a whole over-exposed frame now and then | partly: uniform snapshot (`vk_usnap.c`), no recorder thread, one part per submission (1 to 6 such frames in 360); see "Open-world flicker" |
| Grass draw slots split in halves read two frames apart (round 4 of the draw distance work) | grass patches flickering | to check |
| Depth clamp for the sun's shadow cascades | shadows of trees cut and moving with the camera | recorded here (`OP_DEPTH_CLAMP`); to check on the plateau |
| Sun occlusion query (`ReportCounter` `SAMPLES_PASSED`) | the sun's glare | done here for *Tears of the Kingdom* |
| A draw whose shaders are being built waits when its targets were not drawn the frame before (captures, thumbnails) | a black pause menu background | to check |
| Command lists kept until a recording writes over their memory (`cmdbuf.c` shadows) | lights not composed, wrong records | done differently here (`nvn_api_cmd.c` copy_touch) |

### Open-world flicker

On the Great Plateau about one frame in 100 loses the sun's shadows and the haze (rarely a whole frame turns
over-exposed purple). Counted as frames that differ from both neighbours while the neighbours agree, over 360 frames
captured one by one (`TOTK_SHOTS=4200-4560:1`) from the same save and input; the count varies by 2 to 4 between runs.

| Tried | Frames in 360 |
|---|---|
| defaults before the change (replay parts every 64 draws) | 10, 15 |
| one part per submission (now the default for this game) | 1 to 6 |
| the same at a 30 fps limit | 9 |
| every submission waited for on the submitting thread | 3 |
| uniform snapshot off; fence write-back limit 1 MiB; texture aliases off; mirror off (`TOTK_MEMORY=import`) | 2 to 9 |
| descriptor, texture, shader and pipeline reuse off, pipeline skips strict | 2 |
| asynchronous preemption off | 4 |
| the sun's occlusion report written at once; its counts logged (3000 to 4000, never 0) | 2 |
| no write-back dropped for CPU writes to the target's pages (logged) | 4 |
| no draw skipped for a pipeline being built (perf trace) | 2 |
| `TOTK_DUMP_LOG` over the frames (a copy after every pass) | 0 in 240 |

| the dump log without its GPU copies (`TOTK_DUMP_NOCOPY=1`) | 0 |
| the dump log without copies or binding descriptions (`TOTK_DUMP_NODESC=1`) | 3 |
| region freezing off (`TOTK_FREEZE_IDLE=0`), three stretches | 0, 2, 1 |
| a page query per texture at binding instead of the submission's snapshot | 2, 0 |

So the dump hides it through its binding descriptions, which query the texture cache (`vk_texture_by_id`,
`vk_texture_debug`: a write-watch query over each bound texture) while the replay records, not through its GPU work.
The passes of a flickering frame (`TOTK_DUMP_LOG` with both switches, compared with `difflib` against its neighbours)
match theirs: the 128x128 pass that is missing every 17th frame and the ping-pong of the exposure chain (8x8, 3x3,
1x1) and of the 1920x1080 history target are the game's own rhythm. What differs is the data a pass reads, most likely
a texture the CPU writes now and then, or a target the game makes again at the same memory, as Savage Wind found for
this game ("render targets re-created every frame").

Logged next (a texture log over the stretch): no texture the CPU writes was uploaded again in those frames, and the
slots whose resolution changed were transient targets sharing storage. One slot (an old 3x2 descriptor at the sun
shadow cascades' storage, which a 200x112 target shares now and then) changed in the one flickering frame of that
run, but two fixes aimed at it changed nothing (3, 2, 4 and 0, 3, 2 flickering frames): bindings resolved to the
slot's registration at record time (Savage Wind resolves textures when the commands are recorded), and a slot that
follows the live entries at its storage following the one the GPU wrote last. Both were taken out again.

Frame rates above 60 (the physics microsteps, keyframed bodies, the shield-surf takeoff), the resolution, field of
view, draw distance and shadow settings, and the input and HUD work follow once the game looks and sounds right.

## Other projects

- [Savage Wind Recomp](https://github.com/Revivedx/SavageWindRecomp) (MIT) is a static recompilation of this game
  alone, built on LAS Recomp like this one, and much further along: 60 fps, the DLC, PC options, a launcher. This
  repository already takes parts of it (THIRD_PARTY_NOTICES.md); its code is a reference for this game's behaviour.
- [nxnative](https://github.com/seedds/nxnative) (MIT) runs the game's own ARM code on Apple devices; its notes on this
  game (`dm/games/01007EF00011E000`) list the frame-rate sites of 1.9.0.
