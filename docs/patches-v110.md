# Game 1.1.0 patch coverage

The built-in table is [recomp/patches-v110.txt](../recomp/patches-v110.txt). These sites were located in the local 1.1.0 main and checked against its original instruction words, rather than applying a version-wide address shift. The generator rejects a different word or an invalid spin back-edge.

## Added coverage

| Set | Sites added | Runtime control |
| --- | ---: | --- |
| realtime | 1 | Equipment-switch cooldown: step every positive remainder, including values below FLT_EPSILON. Follows the existing real-time clock switch. |
| cursor | 3 | Scale map panning, vertical ability aim and vertical gyro aim by the 30 Hz ratio; follows `TOTK_HIGHFPS_FIX`. |
| aspect | 4 | The two camera and two viewport projections read page +56. The resolution code enables it for non-16:9 output. |
| fov | 2 | The two camera projections multiply their vertical angle by page +60. The field-of-view setting is live; 100 leaves the original path. |
| pooloff | 2 | Widen texture-pool offsets unsigned, covering 2..4 GiB without changing offsets below 2 GiB. Enabled by default; `TOTK_POOL_OFFSET_PATCH=0` retains the original path. |
| shadow | 1 | The sun shadow map takes its fixed size from page +92 before rounding. The shadow-resolution setting enables it; `shadow_res=game` leaves the game's sizing. |
| cascade | 4 | Read the four sun shadow cascade defaults from page +64..76. Shadow range 100 leaves the original constructor. |

The table has 14 sets and 53 instruction sites in total (this follow-up adds one climb site and two gears sites). Its existing 889 spin back-edges are unchanged. The new sets use the existing runtime page and settings; no new setting, allocator or renderer path is introduced.

The camera sites were cross-checked by normalized instruction-window matching from the local 1.0.0 main. The viewport routine has two matching copies: both are patched. Other sites were checked through their data flow: the equipment timer and its gate, the map clock, the ability clock, the NVN texture storage/package calls, the shadow size publication and the cascade parameter constructor. FOV's s8 is overwritten by the next original instruction; x10 is saved. The cursor sites use the same scratch registers as their 1.2.1 counterparts.

## Deliberately separate

- Direct mouse: 1.1.0 continues to use stick emulation. Porting the complete camera hook set also needs the runtime layout and load-site table; matching an individual store or one camera mode is not sufficient. No partial set is enabled here.
- Fade: the four 1.4.3 sites are not presumed to exist in 1.1.0. An equivalent zero-increment path has not been established.
- Native Zstandard: the existing host bridge calls back to hard-coded 1.4.3 decoder entry points on fallback. Adding only the BL patches to 1.1.0 would be incorrect.
- Live DLSS scene sizing and game-menu mouse input retain their existing version restrictions. They are not patch-table parity work.

## Validation

Run with Python's numpy, lz4, capstone and unicorn dependencies and a runtime-supported Clang:

```sh
# CC selects the Clang used by the differential harness.
python tests/patches_v110_check.py
python tests/patches_v110_check.py --cflags=-march=x86-64-v2
python tests/patches_v110_check.py --exefs <local ExeFS directory>
```

Without ExeFS, the test uses synthetic memory and the patch table only. It compares the complete integer/SIMD register state, SP and NZCV of the lifted C and Unicorn for all 17 additions, on and off, at 30/60/120 Hz, including pool offsets immediately below and above 2 GiB and at the end of the 32-bit range. Windows test mappings respect its 64 KiB allocation granularity. With ExeFS it additionally validates every original word and spin target. Extracted images and generated C are local build artifacts, never repository content.

These checks establish instruction and code-generation contracts. World-scene visual acceptance, texture packs allocating a pool above 2 GiB, physical gyro and comparisons of shadow quality still require gameplay checks.

## 0.4.0 follow-up: climbing and gears

This follow-up compares the final patches and runtime gating added after `3d386fe` against the fixed upstream baseline `c89b0f3ee3b131275c5c5c26426128f16469f2cd`. The target is **1.1.0 / v131072**, not 1.1.1.

| Upstream change | 1.1.0 audit | Scope of this follow-up |
| --- | --- | --- |
| climb | Local main+0x1927948 has the matching velocity compensation, r=s9 and s8=0.5 clamp, but computes the coefficients in a different order. | Port one site at +0x19279f0, using the existing TOTK_CLIMB_FIX and realtime dependency. |
| gears | main+0x213eec4 tests rotation against RotateStartDeg/30 without the update context delta. | Port two sites under TOTK_HIGHFPS_FIX, independently of realtime. |
| fixres / large scene sizes | Existing output/scene sites remain; +0xce9280 is a candidate fixed-dynamic-resolution exit. | Not ported: tiers, render scale, HeapDef and actual rendering still need acceptance checks. |
| grass | +0x100e838 and its +0x100e890 range constant are candidates. | Not ported: locating all six sites and checking textures, heaps and world scenes remain separate work. |
| optref / ability / report | The same crash root cause has not been established on v110; the report loop is not structurally equivalent on every version. | Not ported; guards must not hide an unlocated problem. |
| sync_ahead | frame_wait_lr in [nvn_api_core.c](../runtime/nvn_api_core.c) lists only 121/143 and also requires shadow storage. | Keep the version gating; GPU, readback and fence behaviour has not been accepted. |

### Shipping code and compensation semantics

The climb compensation comes from this repository's [1.2.1 climb definition](../recomp/patches-v121.txt) at the fixed baseline. Local 1.1.0 first multiplies by -0.181, doubles with fadd, then adds 1.272. This is algebraically 1.272 - 0.362(r-1), but merging the multiplication changes its floating-point rounding order. This patch extends only the final clamp and preserves the preceding v110 instructions.

The shipping factor's lower 32 bits stay unchanged at normal 30 Hz and at slower steps with r>=1. The 60 Hz change is intentional: at normal r=0.5, compensation is about 1.8665 instead of the shipping 1.453. Above 60 Hz it uses upstream g(u), with u=max(page+0,r), so reducing the time scale alone does not trigger the higher-frame-rate g branch. It reads existing page+0 without adding fields.

climb uses a 96-byte, 16-byte-aligned stack frame to preserve x8 and full q5/q6/q7/q16/q17. Only s0/s1 are declared outputs; scalar writes clear their vector upper bits. This avoids relying on 1.2.1 dead-register assumptions. TOTK_CLIMB_FIX=0 executes only the original fmov; existing [frame_step_init](../runtime/frame_rate.c) disables the set when realtime=0.

The gear function is reached through vtable 0x347e8e8 slot +0x110. AI dispatchers +0x1205b90 and +0x2ae29ac copy context+8 into the passed structure's +8; AI root update +0x1085c88 calls the owner's virtual +0x30 and publishes its returned frame delta there. The first site, +0x213eee4, follows the original str d12 save. This function does not write s12/d12/q12 between that site and the threshold site. Across calls it relies only on AAPCS64 preserving d12's low 64 bits: the delta occupies the low 32 bits and does not depend on the vector upper half surviving. The second site produces a fresh scalar s12, and the original epilogue restores d12; q12's upper half is not callee-saved.

The threshold becomes (RotateStartDeg/30)*ctx.delta, shared by the three original fcmp/b.le pairs at +0x213f10c, +0x213f174 and +0x213f1b0. The second site also preserves full q4 and restores SP before continuing. gears does not read the patch page, remains active with realtime=0, and restores both shipping sites with TOTK_HIGHFPS_FIX=0.

### Unique matches and original-word guards

Each of these original windows occurs exactly once in the local 1.1.0 main. These are addresses and hashes, not game-byte dumps:

| Window | Length | SHA-256 |
| --- | ---: | --- |
| +0x19279b0 | 108 bytes | 8262a11c180c734f2692a74e9563140f26d743f0f1f95d31732f975b46c27331 |
| +0x213eee4 | 32 bytes | 17487c0fe195655e9a1bcaf49835208bd1ebca86eeef9a2ca0059752f503e378 |
| +0x213f0d0 | 28 bytes | d07279a2abea53cffbd8861e95034cd1a5c85edf90f6094c5261ba55da203577 |

The generator still checks each original instruction. Optional local tests also check the shipping constants, d12 save, vtable entry, all three compares and their b.le targets. No version-wide fixed address shift was used.

### Added validation

The [high-frame-rate patch test](../tests/patches_v110_highfps_check.py) reuses shared CC selection, platform suffixes and mappings, with pathlib paths. The [patch parser](../recomp/patches.py) explicitly reads UTF-8, including the Unicode encoding fixture, independently of the Windows default code page.

```sh
env CC=clang python tests/patches_v110_highfps_check.py
python tests/patches_v110_highfps_check.py --cflags=-march=x86-64-v2
python tests/patches_v110_highfps_check.py --exefs <local-ExeFS> --work-dir <local-artifact-directory>
```

- Windows v3/v2, each with synthetic and local ExeFS input: 3240 native/Unicorn cases per run passed, covering on/off, zero/tiny deltas, adjacent ULPs around 0.5, 30/60/120/240 Hz, slower steps and all 16 NZCV values.
- All 31 GPRs, 32 full 128-bit SIMD registers, SP, NZCV and the complete scratch-memory CRC are compared. Every undeclared output must match its input bit for bit. Nonzero upper-64-bit sentinels in q4/q8/q9 are explicit preconditions.
- Exact save-slot contents, 16-byte stack alignment, restored SP, slot gaps, surrounding canaries and all other scratch memory are checked. No whole memory region or broad SIMD allowlist is excluded.
- Nine default/explicit setting combinations compile the real [patches.c](../runtime/patches.c), frame_step_init and step_store and pass. The existing [#1002 regression](../tests/patches_v110_check.py) retains its SIMD allowlist fix; all four runs pass 408 cases each.
- All five in-memory negative mutations are rejected: corrupt x8, corrupt q17, write outside the save area, restore only d4, and restore only s4. No mutation was applied to the patch file. Combined cases also clobber q12's upper 64 bits to check that calls need preserve only low d12.
- Native WSL Ubuntu 22.04 compilation and execution pass all nine gating combinations. The complete SIMD native replay fails to compile with the installed Clang 14 because the shared [simd_fp.h](../runtime/simd_fp.h) requires newer elementwise builtins. **There is no claim that 3240 Linux replay cases passed.** Synthetic inputs/outputs remain local for a newer-toolchain replay and are not committed. Linux Python differential dependencies were not installed, and the distribution environment was not modified.

These establish instruction, numeric and configuration contracts, not acceptance of real climbing, shrine gears, slow motion, severe stalls or GPU scenes. This branch did not launch or deploy the game; maintainers perform full builds, installation updates and gameplay acceptance separately.

### Regeneration requirements

Re-lift the generated chunks containing main+0x1927948 and main+0x213eec4 and regenerate patch metadata. A normal complete generation is recommended; never hand-edit generated C. RT_PATCH_COUNT changes from 12 to 14, retaining the original 12 indices and appending climb/gears. The patch page address and layout stay unchanged: climb reuses +0 and gears adds no page fields. Do not reuse stale generated objects or lift hashes; rebuild runtime libraries against the regenerated patch macros.
