# TotK Recomp

**TotK Recomp** is a static recompilation of *The Legend of Zelda: Tears of the Kingdom* (Nintendo Switch, 2023) into a native x86-64 application for Windows and Linux ([docs/architecture.md](docs/architecture.md)).

> [!IMPORTANT]
> **No game files are included.** You need your own legally obtained copy of *Tears of the Kingdom*, dumped from your own console: the base game and the 1.1.0, 1.2.1 or 1.4.3 update. Instructions for obtaining those files will not be provided.
>
> The game is built from your files on your machine, so the result contains the game. It is for your own use. Do not share it.

## Status

Early development. The game is playable on Windows and Linux; visual bugs and crashes are still expected.

Report problems as issues, following [CONTRIBUTING.md](CONTRIBUTING.md).

## Features

- Native code, not emulation
- Game updates 1.1.0, 1.2.1 and 1.4.3; 1.2.1 and 1.4.3 saves carry over between them
- 60 fps by default; up to 144 fps or the monitor's refresh rate (experimental)
- Custom resolutions (1080p, 2k...), supersampling and AMD FSR 1 upscaling
- HDR10 output with peak, game and HUD brightness settings
- 16:10 (Steam Deck, handhelds), 21:9 and 32:9 (ultrawide) game resolutions, with the camera and the interface in that shape
- Anisotropic texture filtering up to 16x, for sharp ground into the distance
- In-game settings menu (F1) and performance overlay (F10)
- Setup and in-game menu in English, Russian, Brazilian Portuguese and Simplified Chinese
- Keyboard, mouse and gamepad with remapping, rumble and gyro
- Xbox button prompts: LB, RB, LT, RT, Menu and View, and A, B, X and Y as labelled on the button to press, with either button layout
- Shaders prepared at install instead of during play
- Mods made for emulators: unpack a mod into the `mods` folder next to `totk.exe`; switch and order them in the menu. RomFS
  mods, TKMM's merged output and ExeFS code patches (`.ips`, `.pchtxt`) work; the game rebuilds itself for code patches
  ([docs/mods.md](docs/mods.md))
- Atmosphère cheats: put a cheat file into a mod and switch its cheats in the menu ([docs/cheats.md](docs/cheats.md))

## Planned

- VRR

## Installing

### Windows

Requirements: Windows 10 or later, an x86-64 CPU with SSE4.2 (AVX2/FMA machines get a faster build), a GPU with Vulkan 1.3 and `VK_EXT_external_memory_host`, about 36 GiB free during setup (20 GiB after).

1. Download `TotK-Setup-<version>-windows.zip` from [Releases](../../releases/latest) and unpack it.
2. Start `TotK-Setup.exe`. If SmartScreen warns about an unsigned app, choose *More info*, then *Run anyway*.
3. Pick your game files, the language and an install folder, and press *Start*. The setup builds the game once.
4. Play `totk.exe` from the install folder.

### Linux

Requirements: as on Windows, plus Linux 6.7 or later and glibc 2.34 or later (Ubuntu 22.04, Debian 12, Fedora 35, SteamOS 3).

1. Download `TotK-Setup-x86_64.AppImage` from [Releases](../../releases/latest), or build it ([docs/installer.md](docs/installer.md), "Linux release").
2. Make it executable (`chmod +x TotK-Setup-x86_64.AppImage`) and start it. On a Steam Deck, do this in Desktop Mode.
3. Pick your game files, the language and an install folder, and press *Start*.
4. Play `totk` from the install folder.

To update, run a newer setup on the same folder; saves and settings are kept. Settings are in the in-game menu and in `totk.ini` ([totk.ini.example](totk.ini.example) lists every key). Controls: [docs/controls.md](docs/controls.md).

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

[docs/building.md](docs/building.md) lists the toolchain and the steps. Everything the build writes stays under `build\` and `generated\`, which git ignores; run `tools/check_content.py` before a commit. Testers: [docs/tester-guide.md](docs/tester-guide.md).

Most of the code and documentation is written by AI coding agents (Claude Code) under the maintainer's direction; the maintainer plays every build and decides what is merged. [AGENTS.md](AGENTS.md) holds the rules they work under.
