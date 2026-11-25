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
| grass | 6 | Near grass map extent, step, texture sizes and half-extent read page +128..144. Grass range 100 leaves the original path. |
| climb | 1 | Climbing speed above 60 fps: the 1.2.1 climb factor in the 1.1.0 climb controller. Follows `TOTK_CLIMB_FIX` and needs the real-time step. |
| gears | 2 | The gear rotation check scales its one-frame threshold by the update's frame delta, so shrine gears read as turning above 30 fps. Follows `TOTK_HIGHFPS_FIX`. |

The table has 17 sets and 60 instruction sites in total. Its 889 spin back-edges are unchanged. The sets use the existing runtime page and settings; no new setting, allocator or renderer path is introduced.

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

## Climbing and gears

Both sets port their 1.2.1 counterparts in [recomp/patches-v121.txt](../recomp/patches-v121.txt); the comments there explain the formulas.

**climb**, one site at main+0x19279f0 (1.2.1: main+0x19182cc) in the climb controller's velocity step at main+0x1927948. The site replaces `fmov s1, #1.5` after the shipping factor 1.272 - 0.362 (r - 1), r in s9, s8 = 0.5. The body between prologue and epilogue is word for word the 1.2.1 one: it reads u = max(page +0, r) and gives s0 = max(s0, h), s1 = max(1.5, h). At r >= 1 h stays at or below the shipping factor, so 30 fps and slower keep it bit for bit; at 60 fps the factor becomes 1.4.3's, about 1.8665 instead of 1.453; above 60 fps g(u) takes over. Unlike 1.2.1, the site does not rely on dead registers: a 96-byte stack frame saves x8 and full q5, q6, q7, q16 and q17 and restores them before the original code continues, so only s0 and s1 change. `TOTK_CLIMB_FIX=0` runs the original `fmov`; [frame_step_init](../runtime/frame_rate.c) switches the set off without the real-time step, where the page is not written.

**gears**, two sites in the ActorCheckRotateModule update at main+0x213eec4 (vtable 0x347e8e8 slot +0x110; 1.2.1: main+0x21350c0). The first, main+0x213eee4, loads the update context's frame delta (x1 + 8) into s12 before the function overwrites x1; the prologue saved d12 at main+0x213eec8, nothing writes s12 before the second site, and callees preserve the low 64 bits of v12. The second, main+0x213f0d4, turns `fdiv s12, s0, s4` into s12 = (s0 / s4) * delta, the RotateStartDeg / 30 threshold scaled by the delta, which the three `fcmp s8, s12` at main+0x213f10c, +0x213f174 and +0x213f1b0 read. It saves full q4 in a 16-byte stack slot and restores it and SP. The set does not read the patch page and works with or without the real-time step; `TOTK_HIGHFPS_FIX=0` keeps both original sites.

Each original window below occurs once in the 1.1.0 main (addresses and hashes, not game bytes):

| Window | Length | SHA-256 |
| --- | ---: | --- |
| +0x19279b0 | 108 bytes | 8262a11c180c734f2692a74e9563140f26d743f0f1f95d31732f975b46c27331 |
| +0x213eee4 | 32 bytes | 17487c0fe195655e9a1bcaf49835208bd1ebca86eeef9a2ca0059752f503e378 |
| +0x213f0d0 | 28 bytes | d07279a2abea53cffbd8861e95034cd1a5c85edf90f6094c5261ba55da203577 |

```sh
python tests/patches_v110_highfps_check.py
python tests/patches_v110_highfps_check.py --cflags=-march=x86-64-v2
python tests/patches_v110_highfps_check.py --exefs <local ExeFS directory>
```

The test lifts both sets and runs them against Unicorn on and off, at 30, 60, 120 and 240 Hz and slower steps, with zero and tiny deltas, adjacent ULPs around 0.5 and at each compare threshold, and all 16 NZCV values (3240 cases). It compares all general and full 128-bit SIMD registers, SP, NZCV and the whole scratch memory: only s0/s1 (climb), x1/s12 (gears) and the exact save slots may change. It also compiles [patches.c](../runtime/patches.c) with frame_step_init for the nine combinations of the three switches. With `--exefs` it checks every original word, the shipping constants, the d12 save, the vtable entry and the three compares with their branch targets. These are instruction and switch contracts; climbing and the shrine gears still need checks in the game.
