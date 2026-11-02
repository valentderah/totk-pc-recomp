# Intel ANV: device lost at the title screen

On Intel GPUs with Mesa's ANV driver the game can stop rendering and exit with `VK_ERROR_DEVICE_LOST` within the first
half-minute. The kernel log names RCS0 with `ecode 12:1:8ed9eff2` and `context reset due to GPU hang`, after the 7.5 s
preemption timeout.

## Cause

One translated compute shader places its `OpControlBarrier` in the block after an early return:

```text
if (local_id != 0) return;
OpControlBarrier          ; only invocation 0 reaches it
```

Vulkan requires every invocation of a workgroup to execute a workgroup barrier, so ANV waits for lanes that have
already returned and the GPU stops making progress. The same SPIR-V is accepted by RADV and the NVIDIA driver, which is
why only Intel is affected.

## Fix

The runtime loads `<shader-cache>/override/<key>.spv` before the packed cache
([runtime/gfx_shader.c](../runtime/gfx_shader.c)). `tools/fix_divergent_barrier.py` moves the barrier before the
conditional branch in your own shader pack and writes that override file:

```sh
python3 tools/fix_divergent_barrier.py <install>/shader-cache/shader-cache.pack 530163a0f49819a0217257076c8218141767729055d863d610bfef82d7dc8dac
```

The key above is the shader that hit this on 1.2.1; the script takes any 64-hex key from your pack. It needs
`spirv-dis`, `spirv-as` and `spirv-val` (the `spirv-tools` package, or the copies next to the toolchain). A module that
does not change size is written; anything else is refused, and a malformed override falls back to the pack. Delete the
file to return to the shipped cache.

## Verified

On an Intel Iris Xe (TGL GT2), Mesa 26.2.5: before the fix, every run device-lost at submission 390–584; with the
override, a windowed run reached frame 4800 and gameplay at ~30 fps with no kernel reset, and a 156 s run passed
submission 3600.
