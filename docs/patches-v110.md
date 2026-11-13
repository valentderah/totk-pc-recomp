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

The table has 12 sets and 50 instruction sites in total. Its existing 889 spin back-edges are unchanged. The new sets use the existing runtime page and settings; no new setting, allocator or renderer path is introduced.

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
