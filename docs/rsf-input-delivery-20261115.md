# Input delivery and PCL thread routing, 15 November 2026

Question: TotK previously reported INPUT_SAMPLE but no actual received input kinds or removed Windows message IDs. The Vulkan PCL bridge registers GetCurrentThreadId in VulkanStorySlBindPcl, while the game input/simulation thread has no engine message pump. This is source and CPU-fixture evidence, not game, device, latency or visual acceptance.

## Production delivery

New runtime/rsf_input.c creates versioned rsf_game_cpu_event INPUT_EVENT facts with session1 (the existing frame producer session), source0, actual monotonic counter/frequency, no render promise and no key/button/axis values. Real kinds use message_id0 and INPUT_UNFRAMED; actual queue messages use WINDOW_MESSAGE plus INPUT_UNFRAMED. The latter preserves original dequeue timestamps when native_cpu associates facts with a later FRAME_BEGIN; WINDOW_MESSAGE alone is historically retimestamped by that core. Runtime lookup never loads an SDK. Disabled settings or an unavailable runtime export produce no fact. No current/latest source is consulted, and no vendor marker or pacing policy is selected by this module.

Windows messages are reported immediately after successful GetMessageW removal in gfx_window_win.c, before Translate/Dispatch. Ordinary key and mouse queue IDs additionally report received kinds independently of message IDs. Window-procedure probes using SendMessage cannot fabricate dequeue facts. input_win.c reports raw mouse delivery only after GetRawInputData returns a full validated mouse packet; zero initialization also avoids reading uninitialized raw data after failure. Both relative and absolute raw packets are observed, while the existing relative camera path remains unchanged. WM_INPUT plus legacy mouse movement can report multiple real deliveries, which the core deduplicates by input kind per associated source.

New runtime/rsf_input_gamepad.c installs one process-lifetime SDL event watch with null userdata. It copies only event type and device ID; atomic selected-device identity is cleared before closing a pad and published after opening its replacement. Callback code neither takes the input lock nor dereferences a pad pointer, and always leaves SDL events untouched. Axis/button/touchpad deliveries from other devices, topology changes and sensors are ignored. There is no input subsystem shutdown/unload path; the callback and state therefore outlive SDL delivery. The module name deliberately avoids *_sdl.c because Windows build discovery excludes that suffix even though SDL controllers are common to both backends.

For the SDL window backend, keyboard/mouse facts are observed only after SDL_WaitEventTimeout removal and filtered to the actual game window ID. Direct synthetic handler probes are not observations. input_sdl.c itself remains unchanged.

## PCL audit and generic core correction

Pinned external/ReScaleFrame was not edited. With coordinator authorization, only sibling D:/Coding/ReScaleFrame/runtime/orchestrator/src/native_latency_vulkan.cpp and tests/native_latency_vulkan.cpp were changed, plus a new research note. Successful PCL registration tracks its actual thread and registered message ID. FRAME_BEGIN/INPUT_SAMPLE dispatch on a changed thread retries registration; failed binds are reported and cannot pump the new queue. Invalid registered IDs refuse binding. Non-input render/present calls do not redirect the input queue.

The generic core drains at most16 messages for that exact SDK-owned registered ID using HWND(-1), which excludes window messages. It checks PM_NOREMOVE first because WM_QUIT bypasses filters, preserving quit messages for the engine; a quit arriving between inspection/removal is reposted with its exit code. Unrelated IDs and same-ID window messages remain queued. Actual QPC/frequency are taken after each removal, with zero source and WINDOW_MESSAGE|INPUT_UNFRAMED. Native CPU dispatch occurs outside the SDK mutex, after dispatching the current boundary, allowing bounded association at the next boundary rather than assigning a newly removed fact an older frame timestamp. SDK mutex recursion through the sink is avoided. Token ownership and marker deduplication remain existing core behavior. Shutdown/reinit clears successful registration state; failed shutdown retains the owner for retry.

## Checks

All executables are isolated under build/checks/rsf-input-worker. No shared build outputs, game, GPU or UI automation were used. The core CPU fixture uses real Win32 thread queues and one invisible message-only STATIC window solely to verify same-ID window messages are not removed.

- Clang compilation of tests/rsf_input_delivery_check.c with runtime/rsf_input.c and runtime/rsf_input_gamepad.c, followed by delivery.exe: pass. Stubs verify disabled/unloaded behavior, event size/version/source/session/timestamps/flags, message_id0 for kinds, selected-device callback delivery, disconnect/unselected/sensor refusal, one watch registration and SDL window filtering.
- Clang++ C++20 compilation of sibling tests/native_latency_vulkan.cpp with native_latency_vulkan.cpp, latency_amd_vulkan.cpp and native_cpu.cpp, RSF_RUNTIME_BUILD and RSF_HAVE_STREAMLINE_VK=1, -luser32, followed by core-latency.exe: pass. Existing marker/ownership tests remain, plus first registration, actual next-boundary ping, thread changes, failed/successful rebinding, sixteen-message flood bound, no duplicate markers, unrelated same-range IDs, same-ID window messages, WM_QUIT and shutdown/reinit.
- Whole-source Clang -fsyntax-only for rsf_input.c, rsf_input_gamepad.c, gfx_window_win.c, input_win.c, input.c, input_sdl.c and gfx_window_sdl.c: pass. New input modules also pass -Wall -Wextra -Werror.
- Sibling changed core/test sources pass Clang++ -std=c++20 -Wall -Wextra -Werror -Wno-missing-field-initializers -fsyntax-only.

TotK includes: -I runtime, -I C:/msys64/ucrt64/include/vma, and -I external/ReScaleFrame/{runtime/orchestrator/include,runtime/contract/include,sdk/game/include}. Sibling core includes use those three directories under D:/Coding/ReScaleFrame instead. Exact source lists are above; output filenames are delivery.exe and core-latency.exe. Content and diff checks were also run without staging.

Remaining acceptance: coordinator review/build/pin of the narrow sibling changes, full TotK link, real input and PCL delivery counters in gameplay, focus/pause/resume and input-thread changes, and shutdown with outstanding OS messages. No claim is made that a successful synthetic marker is measured end-to-end latency, that queued pings at process/runtime teardown have a proven cross-reinitialization generation, or that absence of a timing tool generates a ping. Messages still queued on an old, no-longer-selected thread cannot be stolen by another thread; the owning engine/thread lifecycle retains that queue.


## Coordinator review and published runtime

The generic routing correction is published directly on RSF `main` at
`7b92ad601d2c0e65ffe7795dd5cb0f12116d4929`, now pinned by this repository.
Review extended the initial implementation in two ways: accepted input
boundaries register their actual thread before pacing or markers, and every
successful registration posts a private exact-ID FIFO epoch fence. Old queued
pings before that fence are discarded without becoming new-source markers;
synthetic fences are never reported as received tool pings. Actual dequeue QPC
is preserved. The extended CPU fixture proves same-thread shutdown/reinit and
rebind-away/back isolation, fresh ping admission and pre-marker registration.
This supersedes the earlier same-thread queue-generation caveat above.

The clean pinned tree passes its complete MSVC Release native build and serial
CTest suite with real Vulkan runtime directories supplied: 207 tests run,
11 skipped for other unconfigured local fixtures, no failures. The existing
Rust checks remain applicable because this commit changes no Rust sources.
The rebuilt DLL was staged with verified vendor receipts. A bounded fullscreen
DLSS SR/FG/low-latency run reached the world and exited normally; runtime
counters showed input-boundary sleeps and markers with zero refused calls.
That run exposed a separate startup swapchain extent validation error and did
not establish frame-generation acceptance or measured end-to-end latency.

The Win32 startup now synchronously applies a requested fullscreen mode if the
new window actually became foreground, before publishing `g_wnd_ready` to Vulkan.
The queued first-activation handler remains idempotent, while unfocused and
background startup retain their prior behavior. Complete source compilation
passes; fullscreen runtime revalidation follows the presentation worker's
post-retirement surface extent refresh.
