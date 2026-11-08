# Settings reference

`totk.ini.example` describes the settings for players. This page lists the rest: diagnostics, test hooks, paths and experiments. Set one as `name = value` in `totk.ini`, or as the environment variable `TOTK_NAME`, which wins over the file. Remove the line to go back to the default.

## Safety

- **Safe**: changes only what the row says, with no effect on stability or saves.
- **Caution**: can slow the game, grow files, or change the picture, and undoes cleanly when removed.
- **Risky**: can crash or hang the game, or break the picture or game logic, while it is set.
- **Save risk**: can damage or lose saves, or point the game at other game or save files.
- **Unknown**: the code did not settle it.

Many rows below are developer tools that only act together with another setting or a command-line mode (for example a trace mode or the DLSS path). The row says so where the code does. The rows come from reading the code, and most effects have not been checked in a run.

`tests/settings_doc_check.py` fails when a name in `known[]` (`runtime/cfg.c`) is described neither here nor in `totk.ini.example`, or when a default here differs from the code.

## Separate windows

| Setting | What it does | Effect of changing it | Safety |
| --- | --- | --- | --- |
| `perf_window` (default `0`) | `1` opens a separate, non-activating performance window on Windows. | Live toggle; requests full hardware sampling and the existing GPU timestamp queries independently of `perf_overlay`. Closing the window writes this setting to 0. If an environment override keeps its effective value at 1, the window still stays closed for this process; remove the override to rearm off/on, or restart to open it again. Other platforms do not open a window or request additional sampling. | Caution |
| `input_window` (default `0`) | `1` opens a separate, non-activating controller input window on Windows. | Live toggle; shows mapped gamepad actions, physical trigger axes, stick dead zones and available sensor readings without consuming keyboard, mouse, script or recording input. Closing the window writes this setting to 0. If an environment override keeps its effective value at 1, the window still stays closed for this process; remove the override to rearm off/on, or restart to open it again. Other platforms do not open a window. | Safe |

## Diagnostics and logging

| Setting | What it does | Effect of changing it | Safety |
| --- | --- | --- | --- |
| `astc_time` | `1` times the GPU dispatches of each ASTC texture decode and logs the time per block footprint and the largest call. | Adds GPU timestamp queries around every decode; if the queue has no timestamps or no `hostQueryReset`, the log says the decodes are not timed and nothing is measured. | Caution |
| `bind_stats` | `1` counts bindings and descriptors per draw of each stage and how much of the bound state changes between draws. | Adds per-draw bookkeeping to the render thread and extra log output; frames get slower. | Caution |
| `contact_debug` | `1` draws the contact-shadow factor in grey in place of the picture. | The frame shows a grey debug image while it is set; has no effect when contact shadows are off. | Caution |
| `core_diag` | `1` records why ready guest threads wait for a virtual core and for how long, with a histogram every 60 presents. | Turns on host-call and SVC depth tracking and writes many log lines (one per present over 25 ms); frames get slower. | Caution |
| `menu_cursor_log` | `1` logs what the menu cursor code reads at every screen change. | Only adds log lines. | Safe |
| `perf_slow_ms` (default `50`) | Frames at least this many milliseconds long get a `perf: slow frame` log line with the time blocked per cause. | Changes how many frames are logged; a small value fills the log. | Safe |
| `replay_check` | `1` reads every texture read-back a second time after a full drain of the replay queue and compares the two. | Stalls the replay queue at every read-back, so frames get much slower; mismatches are only counted and logged. | Caution |
| `resolve_check` | `1` compares every cached pool lookup with the locked lookup and stops on a difference. | Slows every pool lookup; the game stops if the two ever differ. | Caution |
| `speed_probe` | `1` adds the player's position and ground speed to the frame-rate line that `TOTK_TRACE=perf` prints every 120 presents. | Only adds log text; has no effect without `TOTK_TRACE=perf`. | Safe |

## Profiling and measurement

| Setting | What it does | Effect of changing it | Safety |
| --- | --- | --- | --- |
| `ab` | Splits game time into blocks of this many seconds in the order A B B A and lets settings written as `ab` take effect only in the B blocks. | Settings that accept `ab` flip on and off every few seconds, so frame times and the picture change with the block; without such a setting it does nothing. | Caution |
| `crit` | `<first>-<last>` frame range (with `TOTK_TRACE=sched`) that records every state change and wake of the guest threads, then writes a file at the last frame. | Recording adds overhead to every thread switch in the range and writes one file; `tools/crit_path.py` reads it. | Caution |
| `crit_out` (default `crit.bin`) | Path of the file that `crit` writes. | Only moves the file; nothing is written without `crit`. | Safe |
| `profile` | Path of a sampling profile file; the profiler suspends threads to unwind their stacks. | Frames get slower while it samples and the profile file grows; `tools/prof_report.py` reads it. | Caution |
| `profile_chains` | `1` keeps call chains of every thread in the profile. | Larger profile file and more work per sample; has no effect without `profile`. | Caution |
| `profile_frames` | `<lo>-<hi>` samples only while presents lo to hi-1 are pending; `w<lo>-<hi>` uses world-clock seconds. | Limits the profile to a window; has no effect without `profile`. | Safe |
| `profile_main` | `1` samples only the guest main thread. | Narrows the profile; has no effect without `profile`. | Safe |
| `profile_render` | `1` samples only the render thread and keeps its call chains. | Narrows the profile and makes it larger; has no effect without `profile`. | Caution |

## Capture and frame dumps

| Setting | What it does | Effect of changing it | Safety |
| --- | --- | --- | --- |
| `dump` | Frame number to dump: every render pass target and storage image of that frame is copied out as PNG, with `passes.txt` and `images.txt`. | Writes many PNG files and shader hashes to disk and slows the dumped frame; size is capped by the dump code. | Caution |
| `dump_buf` | `<addr>:<size>[:<off>:<stride>:<w>:<h>]` snapshots one GPU buffer range after every logged pass of a dump. | Writes `buf_<pass>.bin` files and `bufs.txt`; has no effect without `dump`. | Caution |
| `dump_dir` (default `dump`) | Folder that receives the frame dump files. | Only moves the output. | Safe |
| `dump_log` | Frame number; logs the frames from there up to the dumped frame into `passes.txt`, with PNGs only of targets up to 64K texels. | Writes a longer `passes.txt` and more PNGs; has no effect without `dump`. | Caution |
| `flight_at` | Comma-separated frame numbers that trigger a flight-recorder dump. | Writes a dump folder when each frame is reached; has no effect unless the flight recorder (`TOTK_FLIGHT=1`) is on. | Caution |
| `flight_dir` | Folder for flight-recorder dumps; the default is `flight` beside the folder of `totk.exe`. | Only moves the output. | Safe |
| `flight_frames` (default `600`) | Number of presents the flight recorder keeps. | A larger ring keeps more history and more thumbnails in memory; has no effect unless `TOTK_FLIGHT=1`. | Caution |
| `flight_thumbs` (default `1`) | `0` turns off the 320x180 thumbnail the flight recorder saves for each present. | Dumps hold only `frames.tsv`; thumbnails cost a GPU blit per present when on. | Safe |
| `rdoc` | Comma-separated frame numbers to capture with RenderDoc, which is loaded before Vulkan starts. | The game runs under RenderDoc, with its capture options set to keep `VK_EXT_external_memory_host` available and to reference all resources, and captures are written to disk. | Caution |
| `rdoc_dir` | Output folder for RenderDoc captures (the default is `build/captures`). | Only moves the output. | Safe |
| `rdoc_dll` | Path of the RenderDoc library, for when it is not found by default. | A wrong path logs that the library cannot be loaded and no capture is taken. | Safe |
| `shots` | `<lo>-<hi>:<step>` saves every step-th present in the range as `f<n>.png`, scaled to 480x270; `w<lo>-<hi>:<step>` takes them by world-clock seconds. | Writes one PNG per step and adds a readback of the window texture; the pictures clip highlights in HDR. | Caution |
| `shots_dir` (default `.`) | Folder for the `shots` pictures; it is created when needed. | Only moves the output. | Safe |
| `shots_full` | Set to any value to save `shots` at full size instead of 480x270. | Files get far larger and writing them takes longer; has no effect without `shots`. | Caution |

## Automated tests and test hooks

| Setting | What it does | Effect of changing it | Safety |
| --- | --- | --- | --- |
| `vram_limit_mib` | Pretends the device-local memory budget is at most this many MiB, for the paths that react to the budget (`tex_vram_pressure`, mirror refusal); the driver knows nothing of it. | Lets a large card drive those paths; a low value drops released textures and refuses mirrors that would have fit. | Caution |
| `background_window` | `1` keeps the window behind the user's windows, never fullscreen at start, and F11 keeps its place; SDL windows ignore it. | The window does not come to the front or flash, so it is easy to miss; meant for unattended runs. | Caution |
| `gpu` | `vulkan` or `null`: the graphics back end; `null` has no GPU, completes every submit at once and returns zeros for texture reads. | `null` shows no picture; any other value stops the start with an error. | Caution |
| `input` | `none` reports no input from the keyboard, mouse or controllers. | The game gets no controller input and the menu cursor stops following the mouse; `input_script` still applies. | Caution |
| `input_script` | Path of a file with lines `<frame> <button> [hold]` or `<frame> LSTICK or RSTICK <x> <y> [hold]` that are ORed into the real input. | The game presses those buttons by itself; a file that cannot be opened is logged and ignored. | Caution |
| `menu_test` | `<frame>[:<frames per page>]` opens the menu at that frame, shows each tab for 30 frames (or the given count), saves one screenshot per tab and closes the menu. | The menu opens by itself and screenshots are written. | Caution |
| `menu_test_dir` | Folder for the `menu_test` screenshots; the default is the working directory. | Only moves the output. | Safe |
| `menu_test_set` | `<frame>:<key>=<value>[,...][;...]` changes settings live at that frame, with or without the menu open; `SAVE=1` also writes `totk.ini`. | Settings change during play without input, and `SAVE=1` rewrites the file. | Caution |
| `probe_movie` | Text that picks the movie for `totk.exe --probe-movie`: the first file whose path contains it. | Only used by the movie decoder probe; ignored in normal play. | Safe |
| `probe_movie_shot` | Path of a `.ppm` file that the movie probe writes the middle frame to. | Only used by the movie decoder probe. | Safe |
| `probe_movie_bench` | `1` makes the movie probe also time the resampler that fits a movie to a game resolution of another shape, and print the time per frame. | Only used by the movie decoder probe. | Safe |
| `swkbd_answer` | `<text>`, `<cancel>` or `<initial>`: answers the on-screen keyboard (for example a horse name) without opening a window. | The game gets that answer every time it asks for text. | Caution |
| `swkbd_test` | `ok` or `cancel` opens the keyboard window and presses OK or Cancel on the first timer tick. | The keyboard window closes by itself. | Caution |
| `test_pad` | `<lx>,<ly>,<rx>,<ry>` attaches a virtual gamepad whose sticks rest at these raw SDL axis values (-32768 to 32767). | The game sees a controller with that drift or deflection; a malformed value is logged and ignored. | Caution |
| `test_prepare_shaders` | `1` translates every recorded shader on all usable CPUs during start. | Start takes longer and fills the shader cache folder; the CPU is fully used while it runs. | Caution |

## Input

| Setting | What it does | Effect of changing it | Safety |
| --- | --- | --- | --- |
| `bind_<button>` | Remaps one Switch button or stick direction to `pad:<name>` or `key:<name>` entries; covers `bind_a`, `bind_b`, `bind_x`, `bind_y`, `bind_l`, `bind_r`, `bind_zl`, `bind_zr`, `bind_plus`, `bind_minus`, `bind_left`, `bind_up`, `bind_right`, `bind_down`, `bind_lclick`, `bind_rclick`, `bind_lstick_left`, `bind_lstick_up`, `bind_lstick_right`, `bind_lstick_down`, `bind_rstick_left`, `bind_rstick_up`, `bind_rstick_right`, `bind_rstick_down` and `bind_shake`. | A line replaces every default of that button; button prompts in the game still show the default layout. The Controls tab of the menu writes these lines; `docs/controls.md` lists the entry names. | Safe |

## Paths and files

| Setting | What it does | Effect of changing it | Safety |
| --- | --- | --- | --- |
| `amiibo_dir` | Folder the Amiibo tab starts in; the tab saves the folder you last browsed here. | Only changes which folder the tab lists first. | Safe |
| `exefs` | Folder of the game's ExeFS (with `main.npdm`); a relative path is relative to `totk.exe`'s folder. | Without it, an `exefs` folder next to `totk.exe` is used, else the path set at build time. The game code is compiled into `totk.exe`, and the loader checks the files here by SHA-256, so files from another build or game version stop the start with a message. | Caution |
| `mods_dir` (default `mods`) | Folder that holds one subfolder per player mod, beside `totk.exe`. | Mods found there replace game files; see `docs/mods.md`. | Caution |
| `mods_disabled` | Names of mods that are switched off, separated by `\|`. | Listed mods are not applied; the Mods tab writes this line. | Safe |
| `mods_order` | Mod names, top first, separated by `\|`; the top mod wins a file that two mods replace. | Changes which mod's file the game gets; unlisted mods go below the listed ones, by name. | Safe |
| `pipeline_cache` | Path of the Vulkan pipeline cache file (default `pipeline-cache.bin` beside `totk.exe`). | Moves the cache; a new path starts empty and pipelines are built again. | Caution |
| `profile_dir` | Folder of the game profile, which holds the user account id that save data is named after; the default is a `profile` folder beside `totk.exe`, else the build-time path. | Another folder means a different set of saves. | Save risk |
| `romfs` | Folder of the game's RomFS (with `romfs.bin`); a relative path is relative to `totk.exe`'s folder. | Without it, a `romfs` folder next to `totk.exe` is used, else the build-time path; a missing image stops the start with a message. | Save risk |
| `romfs_index` | Path of `romfs-index.bin`, which `tools/romfs_index.py` wrote; the default is beside `totk.exe`. | The index is checked against the image by SHA-256 and size; a mismatch stops the start. | Caution |
| `romfs_mmap` | `1` maps the RomFS image into memory instead of reading it with positional reads. | Changes how the image is read; the log reports when the mapped path is on. | Caution |
| `romfs_mods` | Folder of an overlay with a `manifest.tsv` of sizes and SHA-256 hashes; its files are served in place of the originals. | A missing file, size or hash mismatch, or a changed original stops the start; player mods win over this overlay. | Save risk |
| `romfs_patches` | Folder of an overlay with a `manifest.tsv`, served in place of the original files; the default is `romfs-patches` beside the index. | Same checks as `romfs_mods`; while it is set the Xbox and PC button prompts are off, and the menu says so. | Save risk |
| `romfs_proof_dir` | Folder for the `.romfs-verified-v1` proof file of a full verification; the default is beside the index. | Only moves one small file. | Safe |
| `romfs_verify` | `full` hashes the whole RomFS image at start; `force` hashes it again even when a proof file exists. | Start takes many minutes for a 17 GB image; a proof file is written so later starts skip the hash. | Caution |
| `shader_cache` | Folder of the translated shader cache (default `shader-cache` beside `totk.exe`). | A new folder starts empty, so shaders are translated again during play. | Caution |
| `shared_fonts` | Folder of the extracted system fonts; the default is `shared-fonts` beside `totk.exe`, else beside the RomFS. | Simplified Chinese, Traditional Chinese and Korean need these fonts: without them the game exits before its first frame in those languages. Other languages do not use them. | Caution |

## Shader translation

| Setting | What it does | Effect of changing it | Safety |
| --- | --- | --- | --- |
| `xlate` | Path of the shader translator program; the default is `totk_xlate` beside `totk.exe`, else under `build/xlate`. | Shaders are translated by that program; if it is missing the log says so and graphics shaders cannot be translated. | Risky |
| `xlate_id` | Text or 64 hex digits that identify the translator build in shader cache keys; the default comes from an `.id` file beside the translator, else a hash of the program. | A value that two different builds share lets the cache serve shaders translated by the other build. | Caution |
| `xlate_keep` (default `0`) | `1` keeps the translation job of a last-frame variant as `<key>.job` with its code. | Writes extra files to the shader cache folder; meant for running the translator by hand. | Caution |
| `xlate_procs` (default `2`) | Number of translator processes allowed during play. | More processes translate new shaders sooner and use more CPU and memory; values below 1 count as 1. | Caution |

## Graphics experiments

| Setting | What it does | Effect of changing it | Safety |
| --- | --- | --- | --- |
| `block_views_3d` (default `0`) | `1` gives 3D block-compressed images block views too. | On some AMD cards a GPU write past the end of the image's memory loses the Vulkan device, and the game stops. | Risky |
| `dyn_delta` (default `1`) | Records only the pipeline dynamic states (cull mode, depth and stencil tests, vertex input, blend enables, equations and write masks, ...) whose values changed since the last draw in the command buffer, also across pipeline changes; `0` records the full set at every change of pipeline state, as before; `ab` alternates with the `ab` blocks. | Fewer commands per pipeline change on the render and recorder threads; a driver that loses dynamic state across pipeline binds would draw with stale state. `TOTK_TRACE=perf` prints the calls and commands per submission. | Caution |
| `feedback` | `0` samples a target that an earlier draw of the same pass wrote with no barrier; `restart` ends and restarts the pass behind a full barrier; otherwise the feedback-loop layout is used when the GPU supports it. | `0` brings back the old behaviour that the barrier fixed, so the picture can break in passes that read a target they draw into; `restart` costs extra passes. | Risky |
| `flush_span` (default `0`) | `1` copies one span from the lowest to the highest flushed address of a pool, as an older version did. | Copies far more data to the GPU in crowded towns: an estimated 30-50 ms of GPU time per large copy over PCIe 3.0 x8. | Caution |
| `freeze_idle` (default `60`) | Number of submissions a 2 MiB region stays unwritten before it is made read-only; `0` turns the freeze off. | A larger value keeps more regions writable and costs more write tracking; a write to a frozen region costs about 60 microseconds. | Caution |
| `gpl_draw_build` (default `1`) | `0` leaves the missing shader libraries of a waiting draw to its background job instead of building them in parallel at once. | Draws whose pipeline is not ready are skipped meanwhile, until the job finishes. | Caution |
| `lib_prebuild_keep` (default `0`) | Number of recorded graphics shader library variants that stay in memory after the boot prebuild. | Each kept library costs 140-460 KB of memory; the driver cache already makes a rebuild cheap. | Caution |
| `minimal_layout` (default `1`) | `0` skips the compare against one-block tiles when a texture's memory layout is chosen. | The code calls it a negative control for fresh sessions; the comment does not say what the picture does. | Unknown |
| `mirror_gap` (default `2`) | Hot pools at most this many 2 MiB regions apart share one write-watch query. | Changes the number of queries per submission; negative values count as 0. | Caution |
| `movie_host` (default `1`) | `0` leaves every movie decoder to the game's own library. | The code comment says that library aborts at the first movie, so the game stops at the first cutscene. | Risky |
| `movie_pace` (default `1`) | `0` hands every decoded movie frame over at the next present instead of when it is due. | Movies play at the game's frame rate rather than their own, so their speed no longer matches the sound. | Caution |
| `pcss_k` (default `0.5`) | Depth-to-penumbra factor of soft shadows at 100 percent, for tuning. | Changes how wide soft shadows blur; has no effect while `pcss` is `0`. | Caution |
| `pipe_sync` | Any value other than `0` or empty stops graphics pipelines from being built in the background. | Pipelines are built on the render path, which causes stutter when new effects appear. | Caution |
| `push_stage` | `fs` pushes the descriptor set from the fragment stage instead of the vertex stage. | The log names the stage in use; the code does not say what changes in the picture. | Unknown |
| `rec_end` (default `1`) | `0` lets the submitting thread begin, end and submit a recorded part again, as older versions did. | On some AMD cards the GPU hangs or the scene turns green and then white when a submission is split. | Risky |
| `tex_vram_pressure` (default `90`) | Percent of the device-local memory budget above which released textures are dropped, 0 to 100; `0` is off. First those the game has overwritten with a later texture and left alone for 30 s, then those that can be uploaded again. | A low value drops textures sooner and re-uploads them; `0` keeps them and lets the card spill into system memory. Also a slider in the menu. | Caution |
| `txd_transpose` (default `1`) | `0` turns off the swap of gradient components for gradient texture samples. | Terrain textures turned across the screen show crawling patches again. | Caution |
| `ub_overlay` (default `1`) | `0` copies each uniform update inside a pass into its buffer at once, ending the pass with a full barrier; `ab` alternates with the `ab` blocks. | With `0` scenes with many updated blocks (the terrain, the sky islands) take longer on the GPU; `TOTK_TRACE=perf` prints how many updates stayed in their pass. | Caution |
| `ub_overlay_out` (default `1`) | `0` copies each uniform update between passes into its buffer at once, behind a full barrier of its own; `ab` alternates with the `ab` blocks. Needs `ub_overlay`. | With `0` a run of updates before a pass costs a full GPU barrier each; `TOTK_TRACE=perf` prints how many updates between passes were overlaid. | Caution |
| `ubo_snapshot` (default `1`) | `0` binds live memory for uniform blocks; `ab` alternates with the `ab` blocks. | Without snapshots late draws read another object's matrix, so objects can be drawn in the wrong place. | Risky |
| `vk_no_float64` (default `0`) | `1` clears the 64-bit float feature, so the translator lowers FP64 to FP32. | Shaders change, so the shader cache entries for it are made again; precision of FP64 shaders drops. | Caution |
| `vk_ssbo_align` (default unset) | Power of two that storage buffer offsets are treated as needing, to test the unaligned path. | The translator and the draw code round storage offsets down to it; shaders are translated again. | Caution |
| `vk_subgroup` (default `32`) | Subgroup size to pin where the driver allows it. | A value other than 32 changes what the translator is told about the GPU, and shaders are translated again. | Caution |
| `vram_headroom` (default `1`) | `0` turns off the spare half-block of video memory that a background thread keeps per memory type. | Texture allocations on the render thread can wait for a new block again. | Caution |
| `vs_outs` | `all` declares all 32 vertex shader output locations; otherwise only the union of the paired fragment inputs. | Larger vertex shaders and new pipelines; unwritten outputs read the same as before. | Caution |
| `wb_exact` (default `1`) | `0` decides write-back of GPU-written textures by page only. | A target that shares a page with data the CPU writes can go unwritten, so a name label next to the reticle can blink. | Caution |
| `wb_merge` (default `1`) | `0` copies GPU-written ranges back as plain copies instead of writing word by word the words that changed. | In mirror memory mode the GPU's copy can overwrite words the CPU wrote, which breaks effects such as falling rain and waterfall splashes. | Caution |

## Motion vectors and DLSS

| Setting | What it does | Effect of changing it | Safety |
| --- | --- | --- | --- |
| `dlaa_jitter` (default `1`) | `0` turns off the camera jitter fed to DLSS. | DLSS gets no jitter between frames; a test setting. | Caution |
| `dlaa_maskdebug` (default `0`) | `1` shows the reactive mask in place of the picture. | The frame shows a debug image while it is set. | Caution |
| `dlaa_mv` (default `1`) | `0` sends zero motion vectors to DLSS. | DLSS sees no motion, so moving content can smear; a test setting. | Caution |
| `dlaa_mvdebug` (default `0`) | `1` shows the motion vectors in place of the picture, x in red and y in green. | The frame shows a debug image while it is set. | Caution |
| `dlaa_survey` (default `0`) | `1` logs what is drawn into the light target that DLSS reads. | Adds log output and per-draw work. | Caution |
| `dlaa_times` (default `0`) | `1` times the DLSS passes on the GPU. | Adds GPU timestamps and log lines. | Caution |
| `dlss_reactive` (default `0`) | `1` builds a reactive mask for DLSS where it ran. | Changes how DLSS blends moving areas; a test setting. | Caution |
| `mv_check` | Frame interval; every that many DLSS frames copies the light target and motion images to host memory and logs how well the vectors explain the picture. | Costs a GPU-to-host copy and a comparison on the completion thread at each interval. | Caution |
| `mv_survey` (default `0`) | `1` notes the uniform blocks of each scene draw and compares them with those of earlier frames. | Adds per-draw work and log output. | Caution |
| `mvobj_all` (default `0`) | `1` also lists draws for object motion that read no object blocks (terrain and grass). | More draws are checked and listed, which costs time. | Caution |
| `mvobj_banks` (default `0`) | `1` logs which words of the other uniform banks changed for each scene draw. | Adds log output and per-draw work. | Caution |
| `mvobj_equal` (default `0`) | `1` uses an EQUAL depth compare for the object motion pass, for comparison. | Changes which pixels get object vectors; the code comment names the default as the one that hides objects behind nearer geometry. | Caution |
| `mvobj_exact` | Hex bank mask; only draws whose position uses exactly these banks get object motion. | Fewer draws get object vectors, so their motion falls back to the camera's. | Caution |
| `mvobj_exec` (default `1`) | `0` lists object motion draws but never records them. | Objects get camera motion only. | Caution |
| `mvobj_noc4` (default `0`) | `1` draws again positions that have no scene camera bank. | Those draws get this frame's view, so they can show wrong motion. | Caution |
| `mvobj_only` | Hex bank mask; only draws whose position reads no bank outside the mask get object motion. | Fewer draws get object vectors. | Caution |
| `mvobj_times` (default `0`) | `1` adds timings to the object-motion summary. | The clock reads themselves cost frame time. | Caution |
| `mvprev_check` (default `0`) | `1` verifies the stored first bytes of each previous-frame table entry. | The code comment measures 3.6 ms per frame. | Caution |
| `mvprev_grow` (default `1`) | `0` keeps only three banks in the previous-frame tables. | Object motion that depends on the other banks is no longer tracked. | Caution |
| `mvprev_skip` | Hex bank mask of banks left out of the previous-frame copy, replacing the default. | Different banks are tracked, which changes object motion. | Caution |
| `mvprev_trust` | Hex bank mask of banks whose found previous-frame copy is taken without the check. | A copy that does not match is used anyway, which can give wrong motion. | Caution |

## Scheduling and host

| Setting | What it does | Effect of changing it | Safety |
| --- | --- | --- | --- |
| `app_pool` | Size in bytes of the guest application memory pool; accepts decimal or `0x` hex. | The default depends on the resolution settings; `0` stops the start with a message, and too small a pool leaves the game without memory. | Risky |
| `audio_off` | Any value mixes audio without opening an output device. | The game runs in silence and reports two channels; same as `audio = none`. | Caution |
| `one_per_core` (default `0`) | `1` keeps the process on the first logical processor of each physical core; `ab` switches it with the `ab` blocks. | Busy threads stop sharing a core with a sibling thread, which can help or hurt on the CPU; it only applies when the CPU has hyperthreads. | Caution |
| `reexec` | Counter of address-space retries that the runtime sets itself when it starts the process again. | The counter is cleared when the window is recreated; a value of 20 or more stops the start with a fatal error. | Risky |
| `restart_wait` | Set by the runtime when it restarts itself; names the old process by id and creation time so the new process waits for it to exit. | Meant for the runtime's own use; the code does not describe a manual value. | Unknown |
| `spin` (default `1`) | `0` makes a guest thread that polls memory keep its virtual core. | Polling threads can starve the thread whose item they wait for, so frames can stall. | Caution |
| `spin_handoff_us` (default `50`) | Microseconds a polling wait lasts before it gives its virtual core to a waiting thread. | A shorter value hands cores over sooner; a longer one keeps them longer. | Caution |

## Generated-code patches

These names are code patch sets built into the generated game code (`recomp/patches-*.txt`). `rt_patches_init` reads the environment name at start, then the resolution and mouse code switches some sets itself.

| Setting | What it does | Effect of changing it | Safety |
| --- | --- | --- | --- |
| `aspect_patch` (default `0`) | Patch set that feeds the camera the output's aspect ratio when it is not 16:9. | The resolution code switches this set on or off itself at start, so a value here is replaced; the code does not settle what a manual value does. | Unknown |
| `fov_patch` (default `0`) | Patch set that scales the camera's vertical view angle by the field-of-view setting. | The resolution code switches this set itself whenever the field of view is not 100 percent; a manual value is replaced. | Unknown |
| `mouse_direct_patch` (default `1`) | Patch set of the hooks in the game's camera code behind `mouse_mode = direct` on game 1.4.3. | `0` stops `mouse_mode = direct` from taking effect; other mouse modes are unaffected. | Safe |
| `patch_output` (default `0`) | Patch set that makes the game use the configured output size for its swapchain and display configuration. | The resolution code switches this set whenever the output differs from the base size; a manual value is replaced. | Unknown |
| `patch_scene` (default `0`) | Patch set that makes the game use the configured scene render size. | The resolution code switches this set from the render size setting; a manual value is replaced. | Unknown |
