# Retired swapchain replacement regression

`runtime/gfx_present.c`, `build_swapchain`, passed the active swapchain as `oldSwapchain` to
`vkCreateSwapchainKHR`, then restored that handle on failure. Subsequent presents acquired from it and passed it as
`oldSwapchain` again. The [Vulkan specification for VkSwapchainCreateInfoKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainCreateInfoKHR.html)
says a non-null old swapchain is retired by the create call even if creation fails; VUID
`VkSwapchainCreateInfoKHR-oldSwapchain-01933` requires a non-retired handle. This is independently wrong, but it has
not been demonstrated to cause issue #1487's V-sync symptom.

The fix clears and destroys the retired chain, its image views and rendering semaphores on replacement failure.
The next present retries with a null old swapchain. A selected presentation mode is published only after its chain
was created successfully. Failures before the create call retain the usable chain as before.

## Deterministic negative control

`vk_swapchain_fail_layer.c` is a test-only explicit layer, designed for this one-instance/one-device test process.
It lets the initial create succeed. At the first replacement it calls through to the driver, so the real old chain
is retired, destroys the successful replacement and reports a one-time `VK_ERROR_OUT_OF_DEVICE_MEMORY`. This
reproduces the application's required lifetime handling without exhausting device memory. The layer catches and
reports subsequent retired-handle acquires or reuse rather than sending invalid operations to the driver. It
reports successful creation of a fresh chain after the failure. It does not prove a driver naturally produces this
failure on the test GPU, and it is not a performance test.

Build the layer on the test desktop (no Vulkan import library needed):

```powershell
python tests/build_swapchain_fail_layer.py --cc C:/msys64/ucrt64/bin/clang.exe --include C:/msys64/ucrt64/include
```

Alternatively use the installation's bundled clang and runtime dependency include directory. Its Vulkan headers
must contain `vulkan/vk_layer.h`. The command prints the manifest path; its containing folder is `--layer` below.
No system layer registration or registry writes are needed.

Build unchanged `dev` and the fixed runtime in separate prepared game folders (the same game objects, shaders,
settings, CPU target). Check binary modification times and hashes. Run in the desktop's signed-in interactive
session, one game at a time, with copied saves:

```powershell
python tests/swapchain_retirement_test.py --game C:/path/to/unchanged/game --layer build/swapchain-fail-layer --profile C:/path/to/source/profile --exefs C:/path/to/exefs --romfs C:/path/to/romfs --out build/swapchain-before --expect invalid
python tests/swapchain_retirement_test.py --game C:/path/to/fixed/game --layer build/swapchain-fail-layer --profile C:/path/to/source/profile --exefs C:/path/to/exefs --romfs C:/path/to/romfs --out build/swapchain-after --expect recovered
```

The harness activates the explicit layer only for its child. It sets presentation changes at frames 600, 900 and
1200 using the existing menu test hook; an earlier resize may cause the first replacement sooner. The unchanged
runtime is stopped by its PID once the layer detects invalid retired-handle use. Its nonzero process exit is
intentional: the negative-control harness passes only when it observed the injected failure and invalid use. The
fixed run must recover without invalid markers, reach gameplay, finish its 20 world seconds normally and save a
gameplay screenshot. Both checks require original-profile content hashes to remain unchanged. Outputs stay in the
new result directories, and no real profile is given to the game.

Required log markers:

- Both: `swapchain-test: injected replacement failure; old swapchain is retired`.
- Unchanged: `swapchain-test: INVALID: acquire from retired swapchain` and/or retired-handle reuse.
- Fixed: `swapchain-test: retired swapchain destroyed`, then
  `swapchain-test: recovered: fresh swapchain created without retired oldSwapchain`; no `INVALID` marker.

The recovered expectation explicitly requires the destruction marker, so clearing the active handle while leaking
the retired chain does not pass.

Ordinary transitions without injected failure use `--expect ordinary` and omit `--layer`. This expectation requires
normal exit, gameplay, the screenshot and log entries for mailbox, FIFO and immediate presentation. Add
`--validation-layer <folder>` to request the existing Khronos layer for that child process; the report records its
warnings/errors. Check that the log does not say validation was unavailable. A wall timeout remains a failure even
when the game reached its screenshot; it is not treated as a clean exit.

Then run the fixed runtime without the test layer through an ordinary copied-save scene, the normal presentation
mode-change sequence, `--selftest`, `--probe-kernel`, `--probe-nvn-tex`, `--probe-vkexec`, `--probe-shader`, and
`--probe-window`. Compare validation message kinds before and after with the Khronos layer where available. Runtime
failures caused by absent source-build shader fixtures must be reported explicitly. The source-content guard and
whitespace check are also required before proposing a commit.

## Verified on Windows, 2026-11-27

Both runtimes were freshly linked from dev `3c5ae128` and this patch in separate output folders, using the same
prepared 1.4.3 game objects. Tests ran in the signed-in desktop session on a Ryzen 7 7700X, RTX 5060 Ti,
NVIDIA 621.24. Only copied profiles were passed to the game; the original profile hashes stayed unchanged.

- Injected failure: unchanged runtime attempted retired-handle reuse and was intentionally stopped by the harness.
  Fixed runtime destroyed the retired chain, created a fresh chain, reached the world for 20 seconds, captured a
  screenshot and exited 0 without invalid markers.
- Ordinary run without layers: mailbox, FIFO, immediate and FIFO changes; world and screenshot reached, exit 0.
- Khronos validation ordinary runs: both builds reported zero errors and the same 10 warnings (unused fragment
  outputs and duplicate-message suppression). Both reached gameplay and a screenshot but hung during shutdown
  and were stopped by their own PID at the wall timeout. These are failed clean-exit checks, consistent with the
  separate Windows validation shutdown problem tracked in PR #1510.
- Fixed `--selftest`, `--probe-kernel`, `--probe-nvn-tex` and `--probe-cfg`: exit 0.
- `--probe-window`: both builds passed 300 exact presents, focus, cursor, resize and approximately 60 Hz timing,
  then asserted `m_Pools.IsEmpty()` in VMA shutdown, exit 71. This check did not pass on either build.
- `--probe-vkexec` and `--probe-shader`: attempted, exit 1 because the installed-build test layout lacks
  `build/v143/shaders/jobs.json`. Source-build shader probe coverage remains unavailable.

No naturally occurring driver allocation failure, Linux, HDR, frame-generation path, other GPU vendor or long
play session was tested. This corrects swapchain lifetime handling and makes no performance or issue #1487 claim.