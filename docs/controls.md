# Controls

`TOTK_KBM_SCHEME=1` switches to a second keyboard and mouse path, ported from Weeping of the Empire, whose
contexts, keys and approximations are in [kbm-port.md](kbm-port.md). Off (the default), the table below is what
the game reads.

Input comes from the first SDL3 gamepad, the keyboard and the mouse, merged into one Switch controller (`runtime/input_win.c`). Buttons are ORed; each stick takes the source with the larger deflection. Keys and mouse buttons count only while the game window is in the foreground. `TOTK_INPUT=none` disables both (automated runs). `TOTK_*` settings can also be set in `totk.ini` next to `totk.exe` (`input = none`, `motion = 0`, `rumble = 0`); the environment overrides the file. `TOTK_RUMBLE_STRENGTH` is the rumble strength in percent (0 to 300, default 130; 0 is the same as `rumble = 0`). `TOTK_RUMBLE_CURVE` is the exponent applied to the amplitude before the strength (0.2 to 1.5, default 0.6; lower makes weak pulses stronger, 1 is linear). `TOTK_RUMBLE_TRIGGERS=1` also drives the impulse triggers of an Xbox pad.

`TOTK_PAD_LAYOUT` (`pad_layout`, the *Button layout* row on the Controls tab, live) picks which face button is Switch A:

- `nintendo`: by position, as on a Switch (the table below).
- `xbox`: south A, east B, west X, north Y, as an Xbox pad labels them; on a PlayStation pad cross is A, circle B, square X, triangle Y.
- `auto` (default): `nintendo` for a pad whose east button SDL reports labelled A (Switch Pro Controller, Nintendo-layout pads), `xbox` for the others.

The log names the layout in effect (`input: gamepad layout ...`); the menu shows it under *Automatic*. Which controller's buttons the game's prompts show is `TOTK_PROMPTS` (see Xbox button prompts below). 0.1.9's `pad_layout = xbox_prompts` is read as `pad_layout = nintendo` with `prompts = xbox`, and the menu writes it that way.

| Switch | Gamepad (by position, Nintendo layout) | Keyboard and mouse | Action in the world |
|---|---|---|---|
| Left stick | left stick | W A S D | move |
| Right stick | right stick | mouse; arrow keys as a second source | camera |
| Y | west | left mouse button | attack |
| ZL | left trigger (over 50%) | right mouse button, Z | shield, lock on |
| ZR | right trigger (over 50%) | F, X | bow |
| X | north | Space | jump |
| B | south | Left Shift | run, cancel |
| A | east | E | interact, pick up |
| L | left shoulder | Q | abilities |
| R | right shoulder | R | throw weapon |
| Left stick click | left stick click | Left Ctrl | crouch |
| Right stick click | right stick click | C | first-person view |
| D-pad left, up, right, down | D-pad | 1 2 3 4 | quick-select |
| Plus | Start | Tab | menu, inventory |
| Minus | Back | M | map |

Esc is unbound in the game; it opens the settings menu and releases the mouse cursor (see Mouse camera), unless the PC
scheme runs (`TOTK_KBM_SCHEME`, docs/kbm-port.md): `Esc` is the game's pause menu there and `F1` opens this menu. Z and X duplicate ZL and ZR so menu pages turn with two neighbouring keys; the menu tabs (L, R) are Q and R. The mouse wheel is unbound. The bindings are the tables of `input_map` in `runtime/input_map.c`: the keys (platform-neutral key ids) and one gamepad table per layout (gamepad inputs by position); `runtime/input_win.c` translates Win32 virtual keys and SDL buttons.

## Button prompts

`TOTK_PROMPTS` (`prompts`, the *Button prompts* row on the Controls tab, read at start) picks the buttons the game's prompts show: `nintendo` (default) keeps the game's own, `xbox` shows an Xbox controller's, `keyboard` the PC scheme's keys as keycaps.

- L, R, ZL, ZR, Plus and Minus become LB, RB, LT, RT, Menu and View, and the d-pad prompts a cross with an arrow for each direction meant. The letters on the keys are cut out in the game's own text font.
- A, B, X and Y name the label of the button the pad layout presses, so each prompt names the button to press. `pad_layout = nintendo` plays an Xbox pad by position: the right button, labelled B, does Switch A (interact, confirm), and the bottom one, labelled A, does Switch B (run, cancel), so the prompts swap A with B and X with Y. `xbox` and `auto` put Switch A on the button labelled A, and the letters stay.

| Switch | `nintendo` layout | `xbox` or `auto` layout |
|---|---|---|
| A, B, X, Y | B, A, Y, X | A, B, X, Y |
| L, R | LB, RB | LB, RB |
| ZL, ZR | LT, RT | LT, RT |
| Plus, Minus | Menu, View | Menu, View |
| D-pad | cross | cross |

With `prompts = keyboard` the game's own font carries the PC scheme's on-foot keys instead of the pad buttons (A is
E, B is shift, X is space, Y is the left mouse button, L is Tab, R is G, ZL is the right mouse button, ZR is F, Plus is
Esc, Minus is the map, the d-pad is the arrows). It needs `romfs-patches-keyboard` next to `romfs-index.bin`, written by
`tools/make_romfs_patches.py --prompts keyboard` (Kenney's CC0 "Input Prompts"). The legend is the on-foot key: a menu's
"back" prompt shows B's shift, whose key there is the right mouse button. The keycaps do not depend on `pad_layout`:
one keyboard folder serves every layout.

The action prompt (`DoCommand_00`) shows its button in a diamond whose shape gives the face buttons' positions as on a Switch, with the other three letters around it. With `nintendo` its letters are swapped with the rest and every one names the button in that place. With `xbox` and `auto` the letters are right but the diamond keeps the Switch positions: A shows on its right, while the button labelled A is at the bottom. The sticks keep the Switch icons, and so do prompts drawn as fixed images (some tutorial animations).

The prompts are part of the game's fonts, textures and messages, so they follow the layout and not the bindings. A button remapped with a `bind_` line or on the Controls tab keeps the prompt the game draws for it, which then names the wrong button. Use the Xbox prompts with the default bindings.

The game reads its UI archive and fonts once at boot, so the prompts change at the next start, also after `pad_layout` changes between `nintendo` and the others (the menu's *Button prompts* row shows *in effect* when the loaded set matches the settings and *changes after restart* while it does not); the buttons change at once. Under that note the menu offers *Restart now*: after a confirmation it writes `totk.ini`, closes the game as *Quit game* does and starts the game again with the same command line, which waits until the old process is gone before it opens the log and the saves (`runtime/gfx_present.c`, `gfx_window_closed`; `plat_restart` and `plat_restart_wait` in `runtime/platform/win32.c` and `linux.c`). Progress since the last save is lost, as with *Quit game*; while the game writes a save, the confirmation waits. With `prompts = xbox` the game loads `romfs-patches-xbox` (`pad_layout = nintendo`) or `romfs-patches-xbox-label` (`xbox`, `auto`) next to `romfs-index.bin` in place of `romfs-patches` (`runtime/romfs.c`, `overlay_init`; `runtime/input_map.c`, `input_prompts_folder`): the same 60 fps patches, with the prompts drawn from the game's own files by `tools/make_romfs_patches.py --prompts xbox` or `--prompts xbox-label` (`tools/button_icons.py`, after MacNativeGames' `make_button_icons.py`), which the setup runs next to the plain one:

- every prompt but the button guide in play is text in the button font, `scft/External.bfotf` in each `Font/Font*.Nin_NX_NVN.bfarc.zs`: glyphs U+E040 to U+E043 (A B X Y), U+E044 to U+E047 (L R ZL ZR), U+E04B and U+E04C (Plus, Minus), U+E049 and U+E060 to U+E065 (the d-pad). fontTools writes the font again, so the archives grow and the folder also carries the resource size table (`System/Resource/ResourceSizeTable.Product.<n>...rsizetable.zs`) with their entries raised by as much; without that the game stops at start.
- the button guide in play draws L and ZL from the images `Nt_KeyTexL_00` and `Nt_KeyTexZL_00` of the Common archive's `timg/__Combined.bntx`, painted with the same icons. The action prompt draws its button from `Nt_KeyTexA/B/X/Y_00`, whose image data is swapped for `nintendo`, and the diamond's letters are text the game takes from `LayoutMsg/DoCommand_00.msbt` of each language's `Mals` archive (and the layout's own text panes behind it), swapped the same way. For `keyboard`, those four images are painted with the E, Shift, Space and left-mouse keycaps, and the one-letter diamond panes become `E`, `S`, `_` and `L`: the layout's `Normal_00` font has no keycap glyphs, so the three letters stay text.

The Switch prompts stay, with a note under *Button prompts* and a `RomFS overlay: ...` line in the log, when the folder is missing (an install made by an older setup, or one where the setup could not prepare it and showed a warning; running the setup again on the install folder writes it), when it fails its check, with a game resolution of another shape than 16:9 (its own UI layout wins; 16:10 and 21:9), and when the `romfs_mods` folder replaces one of its files: `romfs-patches` then loads in its place, where two overlays replacing one file would stop the game. In a source build, `.venv-py314\Scripts\python tools\make_romfs_patches.py --version <ver> --prompts xbox` (and `--prompts xbox-label`) writes `build\<ver>\romfs-patches-xbox` (`-label`); it needs fontTools from `tools/requirements-py314.txt`. `--xbox-prompts` is the older name of `--prompts xbox`.

## Window keys

| Key | Gamepad | Action |
|---|---|---|
| F1 or Esc | Minus and Plus together | open or close the settings menu; the game gets no input while it is open |
| F10 | L3 + R3 (both stick clicks) | performance overlay: off, frame rate, full panel |
| F11 | | borderless fullscreen on or off, saved to `totk.ini` |

The Minus+Plus combo never reaches the game. In the menu the arrow keys, the D-pad or the left stick move, Enter or A selects, Q and E or L and R switch tabs, Esc or B goes back (Switch button names, as in the table above).

## Remapping

The *Controls* tab of the settings menu lists, under the button layout, every Switch button and the eight stick directions the keys drive (movement and camera), each with two key cells and a controller cell. Select a cell, press A or Enter, then press the new key, mouse button or controller button; Esc or five seconds without a press cancel, and a controller button cancels a key cell. Delete, Backspace or X clears the selected cell. A key another action has moves to the new one (the description names the action it left); a controller button swaps with the one it replaces. A remapped controller button is physical: from then on the face buttons (A B X Y) stay where they are in both layouts, until the buttons are put back as the layout in use has them or *Reset all buttons* is chosen. F1, F8, F10, F11 and Esc cannot be bound (the menu, the flight recorder and the window keep them). Punctuation keys are named by their place on a US keyboard; the menu shows the character the keyboard layout in use puts there.

The tab writes `totk.ini` lines, one per Switch button or stick direction: `bind_<button> = <entry>, <entry>, ...`. The button is `A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LCLICK RCLICK`, or a stick direction `LSTICK_LEFT LSTICK_UP LSTICK_RIGHT LSTICK_DOWN` (movement, W A S D by default) and `RSTICK_...` (camera, the arrow keys). An entry is `pad:<name>` or `key:<name>`:

- pad names: `south east west north l r zl zr start back lstick rstick dpad_left dpad_up dpad_right dpad_down lpaddle1 rpaddle1 lpaddle2 rpaddle2` (`zl`, `zr` are the triggers; the paddles are the back buttons of an Xbox Elite, DualSense Edge or Steam Controller, upper pair 1, lower pair 2, and no Switch button has them by default)
- key names: `a` to `z`, `0` to `9`, `f2` to `f7`, `f9`, `f12`, `num0` to `num9`, `space tab enter backspace lshift rshift lctrl rctrl lalt capslock insert delete home end pageup pagedown up down left right`, `num_add num_sub num_mul num_div num_decimal`, `minus equals lbracket rbracket semicolon apostrophe comma period slash backslash grave`, `mouse_l mouse_r mouse_m mouse_x1 mouse_x2`, `mouse_wheel_up mouse_wheel_down` (a notch becomes one short press, like a tap)
- `key:default` and `pad:default` keep that part's defaults (`pad:default` is each layout's own button).
- `none` leaves the button unbound.

A line replaces every default of that button, pad and keyboard, so list the keys you still want or use `key:default`. Pad names are physical positions, so a line means the same in both layouts and replaces the layout's own button for that Switch button. Stick directions take keys only: the controller's sticks and the mouse camera always work. Buttons without a line keep the table above. Names are case-insensitive. An invalid entry logs one `input: bind ...` line and is skipped; a line without a valid entry changes nothing. The `TOTK_BIND_<button>` environment variables work like any other setting. A changed line applies at once (`runtime/input_win.c` builds the tables again at its next poll).

A pad that reports its face buttons swapped (A/B and X/Y, as some GuliKit models do) is fixed with:

```
bind_A = pad:south, key:E
bind_B = pad:east, key:lshift
bind_X = pad:west, key:space
bind_Y = pad:north, key:mouse_l
```

## Mouse camera

Mouse motion (raw input) becomes right-stick deflection. Each presented frame takes the counts moved since the last frame, divides by the frame time and sets the deflection to `counts per second * TOTK_MOUSE_SENS / 3000` of full, held for that frame, so the turn rate does not depend on the frame rate. This is not mouselook: the game's stick curve and dead zone still apply, so very slow movements do nothing and a fast flick saturates. The mouse deflection competes with the gamepad and arrow keys by the larger-deflection rule.

`TOTK_MOUSE_SENS` (0 to 10, default 1; higher turns faster, 0 turns the mouse camera and the cursor capture off). `TOTK_MOUSE_AIM_SCALE` (0.1 to 3, default 1) multiplies it while ZR is held, for a calmer drawn bow. While the window is the foreground window the cursor is hidden and confined to the client area; a 4 ms keeper puts that clip back when Windows drops it. It no longer recenters the pointer: the camera reads raw motion (`WM_INPUT`'s deltas), never the cursor's position, and recentering only showed as the pointer jumping to the middle of the screen when a click briefly released the capture.


`TOTK_STICK_DEADZONE` (0 to 50, default 8) is the gamepad stick dead zone in percent of full deflection, the same for both sticks. It is round: a stick inside the circle reads centred, and the rest of its travel is scaled to start at 0 at the edge, so full deflection still reaches full. Raise it for a stick that drifts. The game's own dead zone applies after it. `TOTK_LSTICK_DEADZONE` and `TOTK_RSTICK_DEADZONE` (the *Left/Right stick dead zone* rows under *Analog sticks* on the Controls tab) set one stick each and fall back to `TOTK_STICK_DEADZONE` while unset, so a worn stick can get a larger zone than the other. With a gamepad connected each row shows its stick's deflection now, before the dead zone (*stick now*): let go of the stick and set the row a few percent above the value it rests at.

The stick direction bits (`StickLLeft` to `StickRDown`) are derived from the sticks at 50% deflection. A gamepad plugged in later is picked up within one second; with none connected the log says `input: no gamepad`.

Wheel notches are events, not held keys: each notch is bound like a key (`mouse_wheel_up`, `mouse_wheel_down`) and becomes a 70 ms press with a 70 ms rest after it, one notch at a time, so a quick scroll steps the game once per notch instead of merging the notches into one press (the notches that arrive during a press wait their turn, up to eight a direction). Keys and mouse buttons also keep their bit for two polls after a release, so a tap made between two of the game's Npad samples is not lost at high frame rates.

## Direct mouse look

`TOTK_MOUSE_DIRECT=1` (or *Direct mouse look* on the Controls tab) adds the mouse to the camera's **processed** right
stick after the game's dead zone, curve and scripted checks, so slow motion is not lost and fast flicks do not
saturate. The mouse no longer drives the right stick itself; the gamepad and the arrow keys still do. The game-side
site is the 1.4.3 PlayerCamera stick step (docs/mousedirect.md); another game version needs a new site.

`TOTK_MOUSE_DIRECT_SENS` (0.05 to 10, default 1) is the stick units per mouse count at 30 fps; 1 is about the speed of
the right-stick mouse at 60 fps. `TOTK_MOUSE_DIRECT_PITCH` (0.2 to 4, default 1.3725 = 70/51) shapes the vertical
motion as `sign(v) * sqrt(gain * |v|)`, which turns the game's quadratic pitch into the same response as yaw.

## Mouse motion for the gyro shrines

Some shrines and mechanisms read the controller's six-axis instead of the sticks. Hold `K` (`TOTK_MOUSE_MOTION_KEY`,
default `k`; `none` disables) to feed the mouse to the game's six-axis: the mouse tilts the virtual controller, and the
direct mouse look is paused while the key is held. `TOTK_MOUSE_MOTION_SPEED` (0.05 to 4, default 1) scales the tilt.
A gamepad with motion sensors keeps its own six-axis, so the key does nothing then.

## Keyboard hold, toggles and keyboard snapshot

`TOTK_KEY_MIN_HOLD_MS` (0 to 1000, default 150; 0 off) keeps a released movement key held for that long, so a quick tap
does not break walking at high frame rates. `TOTK_KBM_PULSE_MS` (0 to 200, default 70) is how long a press-triggered PC
scheme action keeps its pad bits (`0` keeps them for the frame of the press only). `TOTK_WHEEL_STEP_MS` (0 to 200,
default 70) is the press and the rest of each wheel notch (`0` keeps a notch for one poll). `TOTK_SPRINT_TOGGLE=1` taps
the run key to keep running (it ends 300 ms after movement stops or on the next tap), and `TOTK_LOCK_TOGGLE=1` taps the
shield/lock key to keep it held until the next tap; the gamepad's own buttons are never latched.
`TOTK_KEYSNAP=1` (default) reads the keyboard from the window's messages instead of polling every key every poll; `0` is
the old polling path. `TOTK_MOUSE_FLOOD` (0 to 100, default 0) adds that many mouse counts per presented frame, a
stress test for a high-report-rate mouse and the input path. `TOTK_KBM_MATERIALS_KEY` (c, z, b or n; default c) is the
PC scheme's material key, held with the throw key. All are on the Controls tab.

The strips 1, 2 and 3 are the pad's own D-pad quick selects (weapons, shields, bows). While the throw key is held, the
first wheel notch takes D-pad up and keeps it as long as that key is held -- the material page stays open only while up
is held -- and every notch after it flicks the right stick inside that hold to walk the list. Two mouse buttons belong
to that page: the left one closes it **without throwing** (the up bit goes, the throw key stays, so the material remains
in hand and aiming continues), and the page's sort is on the middle button (it is the Y the left button used to reach,
and the Scope has nothing to do inside the page). The press that closed the page is eaten while the button stays down,
so a 50 to 150 ms click cannot also fire the attack. Releasing the throw key drops both and is the game's own throw.
Without the throw key, the wheel walks the weapon strip. While the bow is drawn the game's D-pad
means bow and arrow switching instead, so put the bow away (tap `F`) before walking the strips. The scheme has `AIM`
rows for that state, but the port has no player-stance hook yet, so it keeps the on-foot rows.

F1's *Keyboard & mouse* tab edits every action's key: pick *All contexts* or one context (world, aim, climb, ...), then
select a row and press the new key; Delete clears the override. The tab writes `kbm_binds` (`action=key` or
`context.action=key`), and a context entry wins over a global one.

## The game's own menus (1.4.3)

`TOTK_SCREEN_WATCH` (default 1) reads `eui::ScreenMgr` at each pad read on the game thread and gives the PC scheme the
focused screen's context: `Title_00` is `TITLE`, `Pouch_00`/`Pause*` is `PAUSE`, the save list is `SAVES`, options,
shops, dialogue and choices get their own rows, `AppMap_00` is `MAP`, Autobuild is `CONSTRUCT`, the camera and the
scope are their contexts. A held bow key (ZR) selects `AIM` once it has been held steadily for 150 ms, so the scheme's
own 70 ms pulses never flip the context; the quick-select strips never own the focus box, so they stay in the world
context on purpose. `TOTK_SCREEN_WATCH_LOG` (default 1) logs each screen and context (`screen: Pouch_00 -> pause`).
The screen read is from Wintego's PR #605 / Weeping of the Empire (MIT); the player's stance (climb, air, glide, swim,
horse, vehicle) is still not read.

`TOTK_MENU_CURSOR` (**experimental, off by default**) is Wintego's PR #605 menu pointer, ported to this tree and made
to work with the game's own cursor: eui recomputes its focus box every frame and ignores direct writes, so the pointer
moves the box the way the game does -- pressing the d-pad direction that closes the gap to the hovered item, one press
per box move, and giving up after three presses that move nothing. A left click on the hovered item is A on it, a
right click is B. It takes the pointer only in the screens named in `TOTK_MENU_CURSOR_SCREENS` (default: the title,
the pouch and the save/load lists) -- the in-world strips keep the pointer captured, because releasing it there would
also drop the wheel. `TOTK_MENU_CURSOR_LOG=1` logs each menu screen's nodes, the pointer's hit test and every d-pad
press the pointer drives. The SDL build has no pointer yet (the `ikb_pointer` stubs return 0).

## Nintendo Switch 2 controllers

Windows does not present the Switch 2 Pro Controller as a gamepad SDL can open (the log says `input: no gamepad`), so it needs a bridge program that wakes it and creates a virtual gamepad. The DS4Windows fork by hbashton ([github.com/hbashton/DS4Windows](https://github.com/hbashton/DS4Windows), tested with release VIIPERRC4.5.1, a pre-release) with the output set to an emulated DualSense gives buttons, rumble, gyro and accelerometer, and reads the sticks through the controller's calibration (they rest at 0 to 2%). The game reads its virtual DualSense directly; the log then shows `input: gamepad "DualSense Wireless Controller" (rumble, gyro and accelerometer, ...)` and `input: motion sensors on`. Its release notes also list Joy-Con 2 support, which is untested here.

Bridges that output a ViGEm DualShock 4 (ProCon2Windows, connectyourpro as tested) give buttons and rumble but no motion: their virtual DualShock 4 reports a gyro and an accelerometer that never change, so motion aiming and Ultrahand rotation do nothing. Run one bridge at a time; Steam Input can also claim the controller, so close Steam if the bridge does not find it.

## Scripted input

`TOTK_INPUT_SCRIPT=<file>` replays presses. It is ORed with real input and still applies under `TOTK_INPUT=none`. One event per line, `#` starts a comment:

```
<frame> <button> [hold_frames]            # A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LCLICK RCLICK; hold defaults to 4
<frame> LSTICK|RSTICK <x> <y> [hold_frames]   # -32767..32767, up is +y
@<ms> <button> [hold_ms]                  # timed on the world clock
@<ms> LSTICK|RSTICK <x> <y> [hold_ms]
```

`<frame>` counts 60 Hz ticks of game time since the first frame (presented frames at 60 fps), so the title menus get the presses at the same game time at any frame limit. `@<ms>` counts milliseconds of the world clock: game time stepped since the end of the first save load (the player leaves the title screen's origin), so a timed event lands at the same game time at any frame rate and load length. A later load, such as fast travel, still shifts what follows. In a script with timed lines the untimed ones stop when the world clock starts, as the load ends at another tick in each run. Each event logs once (`input: script frame N tick T: A`). `tools/input/load_save.txt` loads the first save from the title screen.

`TOTK_INPUT_RECORD=<file>` (`input_record` in `totk.ini`, or Input recording in the menu) writes the merged input in the same format, one line per button press and per stick segment: events before the world clock starts by tick, the rest timed; a button or stick held when the world clock starts is cut there and goes on as a timed event. The file replays as a script, which makes it the repro to attach to a bug report.

`TOTK_TEST_PAD=<lx>,<ly>,<rx>,<ry>` attaches an SDL virtual gamepad at start whose sticks rest at these raw SDL axis values (-32768 to 32767, down is +y), so a drifting or held stick can be tested without a controller: the values go through the gamepad mapping and `TOTK_STICK_DEADZONE` like a real pad's, and `TOTK_INPUT_RECORD` shows what reached the game.

## RenderDoc capture (blocked)

`TOTK_RDOC=<frame>[,...]` (`runtime/rdoc_win.c`; `TOTK_RDOC_DIR`, `TOTK_RDOC_DLL`) loads RenderDoc before `vkCreateInstance`. RenderDoc hides `VK_EXT_external_memory_host`, which the back end requires for guest pool imports (`runtime/vk_memory.c`), so device creation fails under it. A capture needs a copy-based pool fallback first.
