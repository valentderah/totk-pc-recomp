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

## Credits and licence

Upstream TotK Recomp is by valentderah and its contributors (MIT). The blueprint work on this branch is community research; all external references are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). No game files, keys, extracted data or logs containing them are committed.
