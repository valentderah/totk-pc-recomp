# Input research tools

The Linux research tools build an isolated executable for observing input queries, controller masks, state callbacks and
actor positions. Optional action-channel experiments require an explicit build option, a verified local contract and an
input provider. Unconfigured builds preserve native input behavior.

Game-derived plans, disassembly, recordings, generated C, executables and copied profiles belong under ignored `build/`
or `generated/`. Do not publish them. Query IDs and mask bits need behavioral validation before they can be named as
semantic actions.

## Build an isolated executable

Use the installed recomp's matching SDK, including its generator, compiler, headers and runtime archive. Extract the SDK
under `build/input-logger/sdk/`. The analysis environment needs NumPy, lz4 and Capstone.

```sh
.venv/bin/python tools/build_input_logger.py \
  --install /path/to/install \
  --sdk build/input-logger/sdk \
  --out build/input-logger/run
build/input-logger/run/totk --selftest
build/input-logger/run/totk --probe-loader
```

`build_input_logger.py` regenerates affected chunks with the matching generator and requires byte-identical baseline C
before emitting wrappers. Reused objects are checked against the installer's SHA-256 manifest. Research helper sources
are snapshotted alongside the build, and their hashes appear in `probe-sites.json`. The original executable and generated
sources are not edited. Writable profiles and caches are copied; immutable game assets are referenced from the install.

## Define additional observations

`tools/input_graph.py` resolves imports, bounded ADRP address references, relocatable data references and direct caller
edges. Its reports contain game-derived disassembly and must be written under `build/`.

A verified observation contract can be included in a fingerprinted plan using these options:

- `--controller-probe`: observe a named-controller getter's returned masks.
- `--output-publisher` with repeated `--output-field`: observe scalar outputs through verified object pointers.
- `--position-path` with `--position-offset`: safely observe actor coordinates through a verified pointer chain.
- `--state-callback` with `--state-field` and `--state-size`: observe a scalar field on the original argument object
  after a callback returns.
- `--scalar-probe`: observe existing calls' original x0 argument, caller and actual x0 return, without invoking the
  function independently. Repeat for multiple verified entries.

Pass the resulting report to the builder using `--plan`. The builder verifies the game build ID, exact function boundaries
and full function hashes. The graph tool does not establish function contracts or assign gameplay meanings to fields.

An `object_state` plan site may additionally specify `returned: {"path": [0], "field": 4, "size": 4}` to copy a scalar
through a verified pointer chain starting at the actual x0 return. Paths allow at most eight aligned pointer offsets;
offsets are bounded to 65536 bytes and scalar widths to 1, 2, 4 or 8 bytes. This observes existing calls only.

## Observe player states

A local probe plan may include `player_state` alongside one fingerprinted publisher with a verified actor path. The
contract supplies bounded structure fields, a player-root global, fingerprinted evidence ranges and the relocated verb
name table with classification rules. The builder verifies the name order against the executable and generates the
numeric configuration under `build/`. An executable without that contract does not link the state reader.

The guarded reader observes the actual player's identity, action slots, movement-state bytes, target-lock state,
selected ability and Ultrahand holding state. It neither invokes guest functions nor writes guest memory. Samples are
taken after the publisher and on input polls while the publisher is inactive. Replaced/deleting players, mismatched
identities and failed reads yield absent or unknown data rather than an on-foot fallback. Individual groups have a
`known` mask: a clear bit in `flags` is meaningful only when the same bit is set in `known`.

`player_state` events include raw verbs, source (`publisher` or `poll`), identity, status (`0` unknown, `1` absent,
`2` readable player), selected ability and cumulative sample counts. Identical states are deduplicated with a periodic
record; event spacing is not the sampling interval. Use `tools/analyze_player_states.py EXPERIMENT_FOLDER` to decode
verb names from that build's verified plan. Reports remain under `build/`.
The analyzer also accepts manual recording folders containing `session.json`. Poll samples are best-effort observations
with identity rechecks, not atomic snapshots of every gameplay field.

These are state observations, not action availability or a complete input context. Menu pause remains a separate
signal; a readable actor can retain its last verb during a menu. Bow-use state can persist after the bow lowers,
the guard-keep verb does not represent every raised-shield pose, and selected ability differs from ability in use.
Equipment state, a general event-manager flag and ambiguous riding subcases are not inferred by this reader.

Focused checks: `tests/input_player_state_test.py`, `tests/player_state_plan_test.py` and
`tests/player_state_analysis_test.py`, together with the wrapper CPU-preservation tests.

## Record annotated play

Close the regular game, then run `./record-controls.sh` from a terminal. Use `--run` to select a different research build.
The recorder creates a fresh session profile, executable/binding metadata, a query log and a terminal-marker log. Progress
in that session does not update the baseline research profile or the regular install.
Use `--profile-source build/path/to/profile` to record from a separate baseline while using the same executable. That
profile must be a real directory under `build/`; each session receives a new copy and checks its source hash on exit.

Load a save and enter activity labels such as `idle`, `jump`, `interact`, `inventory-confirm`, `inventory-back` or `dialogue`.
Return focus to the game and allow focus changes to settle before performing actions. Repeat actions when practical;
menu layers have no fixed repetition count. Enter `done`, then close the research game normally.

Labels describe approximate activity intervals. They are not per-frame ground truth. Run
`tools/analyze_input_queries.py SESSION_FOLDER` to produce a recording summary.

## Run bounded scripted comparisons

```sh
.venv/bin/python tools/run_control_experiment.py \
  --run build/input-logger/run \
  --script tools/input/load_save.txt \
  --seconds 6 --deadline 90
```

Each experiment receives a new profile copy, input-script snapshot, log and manifest. The runner checks the source-profile
hash on completion and kills the process group at its wall deadline. Physical input is disabled unless `--real-input` is
supplied. Startup scripts must match the current menu flow and frame-rate setting.

Useful options include:

- `--fps-limit`: record an explicit frame-rate cap.
- `--shot-interval`: capture frames on the loaded-world clock.
- `--startup-shots`: capture bounded present-clock frames, including loading screens.
- `--video-driver`: select the display backend explicitly.
- `--watch-output`: use a bounded GDB child process to trace reads of a verified publisher output.
- `--watch-value-mask` and `--watch-reads`: restrict watch arming to matching output bits and bound the trace count.

Run `tools/analyze_control_experiment.py EXPERIMENT_FOLDER` to correlate delivered input and virtual-action presses with
observed masks and position changes. `tools/compare_control_trajectories.py` compares positions on a common world-time
grid, rejects extrapolation and large gaps, and checks matching build, profile and frame-cap conditions. Missing events
can reflect deduplication or an inactive consumer; absence alone does not identify a game context.

## Optional action experiments

`--allow-mask-filter` enables an opt-in numeric clearing experiment. The runner's `--mask-clear` option requires a
publisher and mask supported by that build's verified metadata.

`--allow-action-channel` enables an opt-in single-action provider. The runner requires `--action-channel` plus a separate
`--action-script` or `--action-key`. The contract specifies a publisher, pressed/held/released fields and one bit mask.
The older `--jump-*` spellings remain aliases. `--action-phases` selects the native pressed, held and/or released fields
to replace; other fields remain untouched. Its default selects all three, so match phases to the observed action contract.
Keyboard experiments also require `--real-input`. Manual recordings accept `--jump-profile`, a local JSON profile with
matching `build_id`, numeric `channel` and keyboard `key`.

A verified context-registry plan may select an observed `context_flags.block_hash` for gating. The observer retains
clear/paused/unknown transition generations while the gameplay consumer is inactive. Unknown or paused state blocks
virtual actions; a generation change requires release before a fresh press. A conservative 100 ms consumer-gap fallback
also remains. These mechanisms require validation in each supported gameplay context.

## Recording format and checks

Events include monotonic time, input poll count and loaded-world time where available. Controller events contain masks
and cumulative call counts. Publisher records use `id` for the pointer-field offset and `result` for the original scalar;
callback records use the observed field offset and value. Position events contain copied coordinates. Context status is
encoded separately as unknown, clear or set.
`scalar_result` records contain the original x0 argument in `object`, the entry caller and the actual return in `result`;
they do not invent a query ID or establish that the return is an availability decision.
Returned-pointer observations use `return_field` for a successful read and `return_field_unavailable` for a null or
unreadable path. Unavailable data is unknown, not a valid zero. A cached target can remain stored while gameplay is
paused; consumers must also account for pause state and observation freshness.

Logs are bounded, existing recordings are not overwritten, and table/event limits are reported explicitly. Logging affects
frame timing, so these builds are unsuitable for performance benchmarks.

Focused checks live in `tests/input_query_logger_test.py`, `tests/input_graph_test.py`, `tests/input_query_analysis_test.py`,
`tests/input_action_channel_test.py`, `tests/input_context_flags_test.py`, `tests/input_mask_filter_test.py`,
`tests/control_experiment_test.py`, `tests/control_trajectory_test.py` and `tests/pause_registry_test.py`.

## Build the gameplay binding UI

`--gameplay-bindings` enables the experimental Controls-page action rows and their verified adapter. It requires a local fingerprinted plan with player-state and pause contracts plus an `action_bindings` contract containing `lockout_offset`, `ride_verb` (the verified Ride verb), `ui_pause_hash` and `menu_pause_hash` (explicit UI and pouch pause registry rows), `interaction_site` (a verified selection getter with a four-byte selected index and returned-entry contract), the three publisher `fields` in phase order, and three nine-element `masks` arrays in runtime action order. Keep that game-derived plan under ignored `build/`.

The builder recompiles authored input, configuration and binding-page sources against the installed SDK headers, replacing members in a copied runtime archive. It never changes the original archive or generated C. This test path currently requires SDL3 development headers at `/usr/include/SDL3`; game-specific constants are generated from the verified local plan. Normal source builds include the action mapping layer but need a registered, verified backend before routing can be enabled.

For reproducible device-level comparisons, `run_control_experiment.py --action-test-script <file>` accepts a bounded fixture with `world-ms key:<name>|pad:<name> duration-ms` lines. It copies the script and records its hash and action environment in the experiment manifest. This explicit test provider runs without window focus and still uses the normal action sampler; ordinary builds and recordings do not inject input without a fixture. Use the regular `--script` only for boot/menu navigation when checking physical-action ownership. The human recorder clears inherited test-fixture settings.

`TOTK_MENU_TEST_BINDINGS=1` with the existing `TOTK_MENU_TEST` screenshot hook focuses the first gameplay binding row when the Controls tab opens, allowing capture of rows below the analog settings. A binding cell ID such as `ACTION_CANCEL#0` can be used instead of `1` to inspect a later row.

Shared Jump/Interact routing observes existing selection-getter calls. It requires a recent observation for the current player's attention component in the same pause epoch. A readable empty selection enables shared Jump; a valid selected entry enables Interact. Unreadable, stale or mismatched selection data cannot assert that no prompt exists and therefore does not enable a shared Jump. The adapter never calls guest selection code independently.


The action catalog is `runtime/input_actions.def`. Its original nine roles use the plan's virtual masks; additional host controls use the game's native button/stick paths. Common controls use one shared map. Advanced overlays only the distinct actions listed by `input_actions_binding_context`; movement/look and identical actions never acquire per-context overrides. The existing remake UI expands those specific rows inline, without a context selector. Gameplay contexts come from the guarded state reader. Menu/dialogue routing also reads the explicit controller-UI and pouch pause flags; unknown flags never imply a ready UI. These flags do not identify every in-game screen.

`TOTK_QUERY_CONTROLLERS=0` suppresses controller-mask chatter while retaining query, state, pause, input and position observations and the same event bound. The recorder and experiment runner default to this reduced stream only when the build metadata advertises the filter, and record that choice. Use `TOTK_QUERY_CONTROLLERS=1` when tracing raw controller consumers. Older executables retain their original logging behavior.

Optional `action_bindings.jump_masks` supplies contextual three-phase Jump masks by host context name. The builder accepts only known context identifiers and uint64 values, and includes all contextual bits in the owned-mask union. This keeps mounted Jump and swimming Dash separate from airborne glider qualifiers.

The gameplay-binding builder also validates `action_bindings.special_contexts`: a bounded list of `verb`, `ability` (`-1` for any selection) and host `context` selectors. Verb names must exist in the fingerprinted local state table, and overlapping selectors are rejected. Selected ability alone never activates a power context; a matching live verb is required. `input_special_plan.py` emits the selector into the ignored build. Ultrahand holding additionally requires the reader's known holding state. These native-action contexts preserve the original publisher output; they do not inherit on-foot virtual masks.

`guard_split` supplies bounded instruction replacements with complete function fingerprints. The builder applies them to a copied module before generation, leaving the source executable and SDK unchanged. The adapter mirrors the original guard source into the private guard bit even with remapping disabled. Advanced guard supplies virtual stance/guard input while native targeting retains the game's target-lock tail. Phase-specific `lockout_masks` preserve native action restrictions, and `input_off_hash` retains the original global controller cut. All numeric game contracts belong in ignored plans.

## Production build integration

Normal builds do not use this research workflow. `recomp/input_actions.py` validates the supported executable against the upstream expected-file hash, reads its action enum through relocations, and generates `input_actions_game_config.h`. It composes only the gameplay publisher and selected-interaction callbacks around ordinary function emission, with the validated private guard-key rewrite. Unknown versions or overlapping patches generate a native-input fallback. Partial refreshes of other modules preserve an existing main-module configuration.

The production backend, guarded reader and registry lookup live in `runtime/` and use the platform abstraction. `input_actions_game.c` is an install-time source in both ordinary and prebuilt-runtime builds. The installer ships the generator and compatibility source, not extracted enums or generated game code. Neither query logging, test-device providers, nor the research `--wrap=input_poll` path is linked into a normal executable. The diagnostic tools above remain a separate developer workflow requiring a matching SDK; they are not tester prerequisites.
