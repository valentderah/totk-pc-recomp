# TotK Recomp — AMD K10 fork

> [!NOTE]
> **This fork adds support for AMD K10 processors (Phenom II X4/X6, Athlon II) on Linux.** These CPUs lack SSSE3 and
> SSE4.1/4.2, so they cannot run the builds of the [original project](https://github.com/valentderah/totk-pc-recomp).
> The setup detects a K10 and builds the game for it (`-march=amdfam10`); newer CPUs get the usual builds.
>
> - Download: [TotK-Setup-x86_64-0.3.12-k10-linux.AppImage](https://github.com/deadlynch/totk-pc-recomp-k10/releases/latest)
> - Linux only: this fork has no Windows release. On Windows, use the original project, which does not run on K10.
> - Tested on a Phenom II X6 1100T with an RX 580: about 27-31 fps standing in the opening areas of 1.4.3, at the 30
>   fps limit the K10 build sets by itself. The game is CPU-bound there; a lower render scale does not help.
> - K10 installs compile the game with an execution profile trained on that machine (about 10% more frames, shorter
>   loads). Building the game on a Phenom II takes about 2-4 hours.
> - The test machine, its BIOS, kernel and system settings: [AMD K10 reference machine](#amd-k10-reference-machine).
> - The changes are proposed upstream as pull requests.

**TotK Recomp** is a static recompilation of *The Legend of Zelda: Tears of the Kingdom* (Nintendo Switch, 2023) into a native x86-64 application ([docs/architecture.md](docs/architecture.md)). The original project builds for Windows and Linux; this fork publishes Linux releases only.

> [!IMPORTANT]
> **No game files are included.** You need your own legally obtained copy of *Tears of the Kingdom*, dumped from your own console: the base game and the 1.1.0, 1.2.1 or 1.4.3 update. Instructions for obtaining those files will not be provided.
>
> The game is built from your files on your machine, so the result contains the game. It is for your own use. Do not share it.

## Status

Early development. The game is playable on Linux; visual bugs and crashes are still expected.

Report problems as issues, following [CONTRIBUTING.md](CONTRIBUTING.md).

## Features

- Native code, not emulation
- Game updates 1.1.0, 1.2.1 and 1.4.3; 1.2.1 and 1.4.3 saves carry over between them
- Takes NSP and XCI game files, and compressed NSZ and XCZ as they are: the setup unpacks them itself
- 60 fps by default; up to 144 fps or the monitor's refresh rate (experimental)
- Custom resolutions (1080p, 2k...), supersampling and AMD FSR 1 upscaling
- HDR10 output with peak, game and HUD brightness settings
- 16:10 (Steam Deck, handhelds), 3:2 (Surface, Framework), 21:9 and 32:9 (ultrawide) game resolutions, with the camera and the interface in that shape
- Anisotropic texture filtering up to 16x, for sharp ground into the distance
- In-game settings menu (F1) and performance overlay (F10)
- Play time across runs on the Game tab of the settings menu, kept in `playtime.txt` next to the saves
- Discord Rich Presence ("Playing TotK Recomp" with the play time), off by default, own application id supported
- Setup and in-game menu in English, Russian, Brazilian Portuguese and Simplified Chinese
- Keyboard, mouse and gamepad with remapping, rumble and gyro
- Stereo or 5.1 surround sound output
- Xbox button prompts: LB, RB, LT, RT, Menu and View, and A, B, X and Y as labelled on the button to press, with either button layout
- Shaders prepared at install instead of during play
- Mods made for emulators: unpack a mod into the `mods` folder next to `totk`; switch and order them in the menu. RomFS
  mods, TKMM's merged output and ExeFS code patches (`.ips`, `.pchtxt`) work; the game rebuilds itself for code patches
  ([docs/mods.md](docs/mods.md)). HD texture packs work: the port grows the texture heap a pack asks for by itself. Tested
  with the HDPacks.com 4K Texture Pack v1.0 on 1.4.3 and 1.2.1; other packs come with no guarantee
- Atmosphère cheats: put a cheat file into a mod and switch its cheats in the menu ([docs/cheats.md](docs/cheats.md))

## Planned

- VRR

## Installing

This fork publishes a Linux setup only. For Windows, use the [original project](https://github.com/valentderah/totk-pc-recomp).

Requirements: Linux 6.7 or later and glibc 2.34 or later (Ubuntu 22.04, Debian 12, Fedora 35, SteamOS 3); an x86-64 CPU, either an AMD K10 (Phenom II, Athlon II) or one with SSE4.2 (AVX2/FMA machines get a faster build); a GPU with Vulkan 1.3 and `VK_EXT_external_memory_host`; about 36 GiB free during setup (20 GiB after). The setup detects the CPU and builds the game for it ([docs/building.md](docs/building.md), `--cpu`).

1. Download `TotK-Setup-x86_64-<version>-linux.AppImage` from [Releases](../../releases/latest), or build it ([docs/installer.md](docs/installer.md), "Linux release").
2. Make it executable (`chmod +x TotK-Setup-x86_64-*.AppImage`) and start it. On a Steam Deck, do this in Desktop Mode.
3. Pick your game files, the language and an install folder, and press *Start*. The setup builds the game once: about
   2-4 hours on a Phenom II.
4. Play `totk` from the install folder.

To update, run a newer setup on the same folder; saves and settings are kept. Settings are in the in-game menu and in `totk.ini` ([totk.ini.example](totk.ini.example) lists every key). Controls: [docs/controls.md](docs/controls.md).

## AMD K10 reference machine

Every K10 number in this fork comes from one machine. This section records what it is and everything that was set on it, from the BIOS to the kernel, so the results can be reproduced.

### Hardware and software

| Part | |
|---|---|
| CPU | AMD Phenom II X6 1100T (Thuban, 6 cores, 3.3 GHz, Turbo Core 3.7 GHz) |
| CPU-NB (L3 and memory controller clock) | 2800 MHz (stock 2000) |
| Board | ASUS M5A78L-M/USB3, BIOS 2101 |
| RAM | 28 GiB DDR3 (1 x 4 + 3 x 8 GiB), 1600 MT/s |
| GPU | AMD Radeon RX 580 2048SP (Polaris 20), RADV, Mesa 26.2.6 |
| Storage | 2 x Kingston A400 240 GB SSD, one btrfs across both (zstd:3, noatime) |
| Display | 1920x1080, 60 Hz |
| OS | Void Linux (runit), Xorg, XFCE, LightDM |
| Kernel | 7.3.3-tkg-eevdf-llvm, built for this CPU (below) |
| Game | 1.4.3, K10 build of this fork with ThinLTO and the K10 execution profile |

### BIOS

- C1E: off. With C1E, a core that halts drops its clock, and every wake-up of a game thread pays for it.
- CPU-NB at 2800 MHz. The L3 cache and the memory controller run at this clock, and the lifted code, at about 0.75
  instructions per cycle on this CPU, waits on memory a lot. 3000 MHz was considered and left for later (stability).

### Kernel

[linux-tkg](https://github.com/Frogging-Family/linux-tkg) 7.3.3, cross-built on a faster machine and installed by hand
(Void has no linux-tkg package). The kernel config is the one of the previous working kernel, so every module stays
(5,588). The `customization.cfg` values that matter:

| Option | Value | Why |
|---|---|---|
| `_version` | `7.3-latest` | 7.3.3 |
| `_distro` | `Generic` | Void; the kernel is installed by hand (see the pitfalls) |
| `_cpusched` | `eevdf` | the mainline scheduler; replaced BORE |
| `_compiler` | `llvm` | clang, with `_llvm_ias="1"` |
| `_lto_mode` | `thin` | ThinLTO |
| `_processor_opt` | `amdfam10` | `-march=amdfam10 -mtune=amdfam10` |
| `_timer_freq` | `1000` | 1000 Hz |
| `_tickless` | `2` | tickless idle, as before |
| `_default_cpu_gov` | `performance` | |
| `_configfile` | the running kernel's config | same modules and drivers as the kernel that worked |
| `_modprobeddb` | `false` | no module trimming, so no missing module later |
| `_debugdisable`, `_rustdisable` | `true` | shorter build |
| `_llvm_polly` | `0` | shorter build |
| config fragment | `CONFIG_CGROUP_DMEM=y` | for dmemcg-booster, which runs on this machine |

`_misc_adds`, `_glitched_base`, `_zenify`, `_fsync_backport`, `_fsync_legacy` and `_mglru` stay on, as in the
previous kernel. Preemption is full (`CONFIG_PREEMPT=y`).

Boot parameters (`GRUB_CMDLINE_LINUX_DEFAULT` in `/etc/default/grub`):

```
mitigations=off loglevel=4 processor.max_cstate=1 nowatchdog nmi_watchdog=0 tsc=reliable transparent_hugepage=always
```

| Parameter | Effect |
|---|---|
| `mitigations=off` | no CPU vulnerability mitigations |
| `processor.max_cstate=1` | removes C2, which took 75 µs to leave; idle cores stop in C1 at most |
| `nowatchdog nmi_watchdog=0` | no soft and NMI lockup watchdogs: fewer interrupts |
| `tsc=reliable` | no clocksource watchdog; the TSC here is constant and keeps running in idle (`constant_tsc nonstop_tsc`) |
| `transparent_hugepage=always` | 2 MiB pages for the game's memory too (the K10 TLB is small); `defrag` stays `defer+madvise`, so the game never waits for compaction |

### System settings

- **CPU governor `performance`.** With `ondemand`, half the cores sat at 800 MHz while the game ran. CoreCtrl was
  installed and re-applies its own governor within a second, so the governor is set in CoreCtrl's global profile
  (CPU → frequency governor). A runit service sets it too, for boots where CoreCtrl does not start.
- **C1 off while the game runs.** The same runit service watches for the game. While a `totk*` process is running,
  idle cores poll instead of halting, so a woken game thread starts at once; C1 comes back when the game exits. The
  CPU runs hotter while polling.
- **Autogroup off**: `kernel.sched_autogroup_enabled = 0` in `/etc/sysctl.d/90-gaming.conf`. Game threads compete
  with the desktop as equals instead of per-session groups.

The service, `/etc/sv/cpu-governor/run`:

```sh
#!/bin/sh
# acpi_cpufreq loads ~10 s into boot, after runit starts this service: wait for it
i=0
while [ ! -e /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ] && [ $i -lt 120 ]; do
    sleep 1; i=$((i + 1))
done
sleep 2
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -w "$g" ] && echo performance > "$g"
done

# C1 off while the game runs (C2 is already off through processor.max_cstate=1)
set_c1() {
    for s in /sys/devices/system/cpu/cpu*/cpuidle/state1/disable; do
        [ -w "$s" ] && echo "$1" > "$s"
    done
}
state=0
set_c1 0
while :; do
    if pgrep "^totk" >/dev/null 2>&1; then want=1; else want=0; fi
    [ "$want" != "$state" ] && { set_c1 "$want"; state=$want; }
    sleep 2
done
```

### Game settings

- `totk.ini`: `fps_limit = 30` (the K10 default), `vk_present = fifo`, 1920x1080, `render_scale = 100`. The game is
  CPU-bound here; a lower render scale does not help.
- [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk) frame generation at 2x for the `totk` process (`multiplier = 2`,
  `pacing_mode = "vsync"`), so the 30 fps the game renders are shown at 60.
- The defaults of the K10 device profile stay: the Vulkan recorder thread on (switching it off cost 11.5% of the
  frames), `rec_end = 0` on AMD GPUs (with `1` an RX 580 under RADV ran at 1 fps), `replay_async = 1`.

### Measurements

Method: `tools/input/walk.txt`, a scripted walk through an open-world save, `TOTK_FPS_LIMIT=60`, lsfg-vk off,
`--seconds 210`. The save is restored before each run, and variants are run interleaved, three runs each. Runs of the
same build differ by 2-5%.

| Setup | Frames per run |
|---|---|
| 0.3.12 K10 build, `ondemand`, C2 on, kernel 7.3.1 BORE | about 10,650 (41-48 fps walking) |
| the same with ThinLTO | 10,658 against 10,677 without it: the same within noise |
| the same with the recorder thread off | 9,401 (-11.5%) |

Under the old system settings the frame took about 23 ms, of which the GPU worked about 8 ms. No game thread filled a
core: replay 63%, guest 49%, recorder 45%, 3.6 of 6 cores in all. The frame was bound by the hand-offs between those
threads, each of which could land on a core asleep at 800 MHz or in C2. That is what the governor, C-state and kernel
changes above address. With all of them the game plays far smoother. The scripted walk has not yet been rerun to
put a number on it.

### Pitfalls met on the way

- CoreCtrl overrides the CPU governor from its profile. Setting it anywhere else lasts about a second.
- `acpi_cpufreq` loads about 10 s into boot, after runit services and `rc.local`. A governor set earlier does not
  stick, and the driver comes up with its own default.
- Void links `/lib` to `usr/lib`. Unpacking a tar with a top-level `lib/` directory as root replaces that link with a
  real directory: `/lib/firmware` disappears, amdgpu cannot load the Polaris firmware and X does not start. Install
  modules under `usr/lib/modules`, or use `tar --keep-directory-symlink`.
- With only the custom kernel kept, Void's `base-system` would reinstall the stock kernel on the next update. An
  `ignorepkg=linux` and `ignorepkg=linux-headers` line in `/etc/xbps.d/` stops it.
- The `k10temp` sensor of a Phenom II reads a relative value, often 0 °C; read the CPU temperature from the board's
  sensor or the BIOS.

## Credits

TotK Recomp builds on the projects below. What was taken from each, with its license, is listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

- [LAS Recomp](https://github.com/peiikai/LASRecomp)
- [MacNativeGames](https://github.com/ly05010419/MacNativeGames)
- [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) and [nxnative](https://github.com/seedds/nxnative)
- [SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp)
- [Ryujinx](https://github.com/Ryujinx/Ryujinx)
- [deko3d](https://github.com/devkitPro/deko3d)
- [hactool](https://github.com/SciresM/hactool)
- [SDL 3](https://libsdl.org), [Dear ImGui](https://github.com/ocornut/imgui), [AMD FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR), [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator), [libopus](https://opus-codec.org), [libvpx](https://chromium.googlesource.com/webm/libvpx), [Zstandard](https://github.com/facebook/zstd)
- [Switchbrew](https://switchbrew.org) and the Switch homebrew community

## License

The project's own code is under the [MIT License](LICENSE); third-party code keeps its own license ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). The license grants nothing for Nintendo's game.

TotK Recomp is an independent, non-commercial fan project, not affiliated with or endorsed by Nintendo. *The Legend of Zelda*, *Tears of the Kingdom* and *Nintendo Switch* are trademarks of Nintendo.

## Building from source

[docs/building.md](docs/building.md) lists the toolchain and the steps. Everything the build writes stays under `build/` and `generated/`, which git ignores; run `tools/check_content.py` before a commit. Testers: [docs/tester-guide.md](docs/tester-guide.md).

Most of the code and documentation is written by AI coding agents (Claude Code) under the maintainer's direction; the maintainer plays every build and decides what is merged. [AGENTS.md](AGENTS.md) holds the rules they work under.
