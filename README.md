# TotK Recomp — Blueprint Import

**This branch adds in-game Autobuild blueprint import to [TotK Recomp](https://github.com/valentderah/totk-pc-recomp).** It is a research branch: the goal is a window inside the game that imports a community blueprint and shows it in 3D before the player builds it. Upstream is a static recompilation of *The Legend of Zelda: Tears of the Kingdom* into a native x86-64 application for Windows and Linux; this branch carries the blueprint work, tracked in PR [#757](https://github.com/valentderah/totk-pc-recomp/pull/757) and documented in [docs/blueprint-import.md](docs/blueprint-import.md).

> [!IMPORTANT]
> **No game files are included.** You need your own legally obtained copy of *Tears of the Kingdom*, dumped from your own console. The game is built from your files on your machine; the result contains the game, is for your own use and must not be shared. Instructions for obtaining those files will not be provided.

## What this branch supports

- **Import.** On the Autobuild screen, the ⊕ (Plus) button opens the port's own file picker. Choosing a local `.cai` or `.txt` blueprint writes it into the save data the game holds in memory, so it appears as the newest draft and builds with the game's own preview, rotation and Zonai-energy costs — no reload, no external save editor.
- **Container contents.** `.cai` and `.txt` are two wrappers around the same 6688-byte CAI blob. A `.cai` file is the blob itself; a `.txt` file is the blob gzip-compressed and Base64-encoded inside one JSON member (`combinedActorInfoBase64`). The blob holds up to 21 actor records (two matrices, six floats and a 128-byte name each), the 12-byte bond records and a six-float bounding box.
- **Container and media parsing.** The `.txt` reader finds the Base64 member with a bounded scan, inflates it with a hard size bound and checks the gzip trailer's CRC32 and length. The `.cai` reader validates magic, version, both actor-count copies, names, matrix floats and the bounding box before anything is written. For the 3D view, each actor name resolves to a real model in the player's own RomFS: `Pack/Actor/<actor>.pack.zs` (SARC, game zstd dictionary) → ModelInfo BYML → `FmdbName` → `Model/<project>.<FmdbName>.bfres.mc` → `MCPK` header + magicless, sizeless zstd + `FMSH` (MeshCodec) → BFRES `FMDL`/`FSHP`/`FVTX`/`FSKL` → vertices and indices. A part with no usable model falls back to its collision body (a convex hull from `ShapeParam` BYML).
- **3D preview.** While a file is highlighted, the picker renders the blueprint in one offscreen Vulkan pass with a real depth buffer, from each part's real model placed by the CAI's rotation, position and scale. Decoding MeshCodec needs [dt-12345/MeshCodec](https://github.com/dt-12345/MeshCodec) (GPL-2.0, the only implementation); it is built outside the tree into `libmeshcodec.a`, so the repository does not carry GPL code, and a build without it uses the collision-body fallback. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- **Upcoming.** A blueprint editor, and exporting a blueprint back to `.cai`/`.txt`, are the planned next work. They are **not implemented yet**.

## Terminology

Game-specific names use the official localizations, not literal translations:

| English | Japanese | Simplified Chinese |
|---|---|---|
| Autobuild | ブループリント | 蓝图 |
| Fuse (attaching a material to a weapon or shield) | スクラビルド | 余料建造 |
| Ultrahand | ウルトラハンド | 究极手 |

## Current state

Import, in-game use of imported blueprints, and the 3D preview work, and were re-verified locally against a 0.4.0 build of the port. One preview-geometry problem remains open: a material fused onto a weapon is drawn **intersecting** the weapon instead of sitting on its tip. This affects the 3D preview only — importing, saving and building are unaffected. It is accepted as an unfixed limitation of the draft so the work can be handed over.

## Read this first

- [docs/blueprint-import.md](docs/blueprint-import.md) — the format, the five save arrays, the model path, the fused-material rule set and the open placement question.
- [Public knowledge write-up](https://gist.github.com/kaiyao7/ad158b08a365e0e6020d0db332d96e13) — main write-up, references, knowledge map and the open-problem page; everything needed to pick up the last step.
- [Feature request #1081](https://github.com/valentderah/totk-pc-recomp/issues/1081) — the technical reasoning and the community references.
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) — every reference, its licence and pinned commit.

## Building

This is a fork of an upstream recompilation. Build with the upstream toolchain (MSYS2 UCRT64 Clang, Ninja, `.venv` / `.venv-py314`); see [docs/building.md](docs/building.md) in the upstream tree. `tools/build_game.py` links `libz.a` for the `.txt` reader and the optional `libmeshcodec.a` for the 3D preview; `tools/build_meshcodec.py` builds the decoder.

## Acknowledgements

**This project is built on the open-source work below.** It is a recompilation port; very little of it
exists without the tools, format research and format knowledge the community published. Every entry is
listed with its licence and with the file that carries its notice in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

### The port itself

- **[LAS Recomp](https://github.com/peiikai/LASRecomp)** by Peiikai (MIT) — the recompiler lifter, code
  generator, FP/SIMD lowering, CPU headers, differential tests and the host runtime this port is built on.
- **[MacNativeGames](https://github.com/ly05010419/MacNativeGames)** (MIT) — extraction scripts, host
  runtime, in-game menu work, frame-rate patches and the verification hashes the build is checked against.

### Blueprint format and model research

This branch's feature would not be possible without the community's work on the formats:

- **[dt-12345/MeshCodec](https://github.com/dt-12345/MeshCodec)** by dt-12345 (GPL-2.0) — the reference
  `.bfres.mc` decoder. It is the only implementation, so it is built outside this repository and linked as
  `libmeshcodec.a`; a build without it falls back to collision bodies.
- **[dt-12345/totk_params](https://github.com/dt-12345/totk_params)** by dt-12345 — the parameter files and
  the `TypedParam` / `.bgyml` parent-resolution rules behind the attachment data the preview reads.
- **[McSpazzy/cai2json](https://github.com/McSpazzy/cai2json)** and
  **[cai2json-node](https://github.com/McSpazzy/cai2json-node)** by SuperSpazzy — the CAI / JSON format
  research and the round-trip tool that established the `CmbAct` layout.
- **[McSpazzy/totk-gamedata](https://github.com/McSpazzy/totk-gamedata)** by McSpazzy — save-game structure
  research behind the five Autobuild draft arrays.
- **[KillzXGaming/Switch-Toolbox](https://github.com/KillzXGaming/Switch-Toolbox)** by KillzXGaming
  (GPL-3.0) — the BFRES rigid-skinning rule and the Euler-versus-quaternion bone flag.
- **[bfrespy](https://github.com/Neurociencias/bfrespy)** (GPL-3.0) and
  **[meshoptimizer](https://github.com/zeux/meshoptimizer)** by Arseny Kapoulkine (MIT) — BFRES field offsets
  and the index/vertex codec behaviour.
- **[three.js OrbitControls](https://github.com/mrdoob/three.js)** (MIT) — the drag convention the preview
  follows.
- **[Blehditor](https://blehditor.hyruleworks.com)** by SuperSpazzy — the community blueprint viewer this
  preview is measured against.
- **[HyruleWorks](https://www.hyruleworks.com)** — the blueprint archive whose real `.cai` files were used as
  test and preview inputs (inputs only, never committed).

### Upstream and libraries

- **[valentderah/totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp)** by valentderah and
  contributors (MIT) — the upstream project this is a fork of.
- **[Ryujinx](https://github.com/Ryujinx/Ryujinx)** (MIT) and **[deko3d](https://github.com/devkitPro/deko3d)**
  — shader translator and block-linear layout behaviour.
- **[SDL 3](https://libsdl.org)**, **[Dear ImGui](https://github.com/ocornut/imgui)**,
  **[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator)**,
  **[AMD FidelityFX FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR)**, **libopus**, **libvpx**,
  **[zstd](https://github.com/facebook/zstd)**, **[hactool](https://github.com/SciresM/hactool)** by SciresM
  (ISC) — libraries and tools the port and its installer use.
- **[Switchbrew](https://switchbrew.org)** and the Switch homebrew community — the platform documentation the
  port's reimplementation follows.

Licences: the project's own code is MIT; third-party code keeps its own licence, listed above and in
`THIRD_PARTY_NOTICES.md`. The MIT licence grants nothing for Nintendo's game. TotK Recomp is an independent,
non-commercial fan project, not affiliated with or endorsed by Nintendo. *The Legend of Zelda*, *Tears of the
Kingdom* and *Nintendo Switch* are trademarks of Nintendo.

No game files, keys, extracted data or logs containing them are committed.

