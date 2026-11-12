# Controls

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
| Y | west | left mouse button, T | attack |
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
| Right stick shaken left and right | shake the right stick | G | detach a fused part from what Ultrahand holds |

Esc is unbound in the game; it opens the settings menu and releases the mouse cursor (see Mouse camera). Z and X duplicate ZL and ZR so menu pages turn with two neighbouring keys; the menu tabs (L, R) are Q and R. T duplicates Y for the game's menus, where the left mouse button is A while the pointer is shown (sort the inventory with T). The mouse wheel zooms the Purah Pad's map and is unbound elsewhere. The bindings are the tables of `input_map` in `runtime/input_map.c`: the keys (platform-neutral key ids) and one gamepad table per layout (gamepad inputs by position); `runtime/input_win.c` translates Win32 virtual keys and SDL buttons.

## Xbox button prompts

`TOTK_PROMPTS` (`prompts`, the *Button prompts* row on the Controls tab, read at start) picks the controller whose buttons the game's prompts show: `nintendo` (default) keeps the game's own, `xbox` shows an Xbox controller's, `pc` the keyboard and mouse (PC button prompts, below).

- L, R, ZL, ZR, Plus and Minus become LB, RB, LT, RT, Menu and View, and the d-pad prompts a cross with an arrow for each direction meant. The letters on the keys are cut out in the game's own text font.
- A, B, X and Y name the label of the button the pad layout presses, so each prompt names the button to press. `pad_layout = nintendo` plays an Xbox pad by position: the right button, labelled B, does Switch A (interact, confirm), and the bottom one, labelled A, does Switch B (run, cancel), so the prompts swap A with B and X with Y. `xbox` and `auto` put Switch A on the button labelled A, and the letters stay.

| Switch | `nintendo` layout | `xbox` or `auto` layout |
|---|---|---|
| A, B, X, Y | B, A, Y, X | A, B, X, Y |
| L, R | LB, RB | LB, RB |
| ZL, ZR | LT, RT | LT, RT |
| Plus, Minus | Menu, View | Menu, View |
| D-pad | cross | cross |

The action prompt (`DoCommand_00`) shows its button in a diamond whose shape gives the face buttons' positions as on a Switch, with the other three letters around it. With `nintendo` its letters are swapped with the rest and every one names the button in that place. With `xbox` and `auto` the letters stay and the diamond is laid out as an Xbox pad: A at the bottom, B on the right, X on the left and Y at the top, the button the prompt names lit in its place, its bar reaching left from X and right from the others (`tools/make_romfs_patches.py`, `bflyt_xbox_diamond`: the panes of `blyt/DoCommand_00.bflyt` take their partner's place, A's of B and X's of Y). The sticks keep the Switch icons, and so do prompts drawn as fixed images (some tutorial animations).

The prompts are part of the game's fonts, textures and messages, so they follow the layout and not the bindings. A button remapped with a `bind_` line or on the Controls tab keeps the prompt the game draws for it, which then names the wrong button. Use the Xbox prompts with the default bindings.

The game reads its UI archive and fonts once at boot, so the prompts change at the next start, also after `pad_layout` changes between `nintendo` and the others (the menu says *prompts change after restart*); the buttons change at once. Under that note the menu offers *Restart now*: after a confirmation it writes `totk.ini`, closes the game as *Quit game* does and starts the game again with the same command line, which waits until the old process is gone before it opens the log and the saves (`runtime/gfx_present.c`, `gfx_window_closed`; `plat_restart` and `plat_restart_wait` in `runtime/platform/win32.c` and `linux.c`). Progress since the last save is lost, as with *Quit game*; while the game writes a save, the confirmation waits. With `prompts = xbox` the game loads `romfs-patches-xbox` (`pad_layout = nintendo`) or `romfs-patches-xbox-label` (`xbox`, `auto`) next to `romfs-index.bin` in place of `romfs-patches` (`runtime/romfs.c`, `overlay_init`; `runtime/input_map.c`, `input_prompts_folder`): the same 60 fps patches, with the prompts drawn from the game's own files by `tools/make_romfs_patches.py --prompts xbox` or `--prompts xbox-label` (`tools/button_icons.py`, after MacNativeGames' `make_button_icons.py`), which the setup runs next to the plain one:

- every prompt but the button guide in play is text in the button font, `scft/External.bfotf` in each `Font/Font*.Nin_NX_NVN.bfarc.zs`: glyphs U+E040 to U+E043 (A B X Y), U+E044 to U+E047 (L R ZL ZR), U+E04B and U+E04C (Plus, Minus), U+E049 and U+E060 to U+E065 (the d-pad). fontTools writes the font again, so the archives grow and the folder also carries the resource size table (`System/Resource/ResourceSizeTable.Product.<n>...rsizetable.zs`) with their entries raised by as much; without that the game stops at start.
- the button guide in play draws L and ZL from the images `Nt_KeyTexL_00` and `Nt_KeyTexZL_00` of the Common archive's `timg/__Combined.bntx`, painted with the same icons. The action prompt draws its button from `Nt_KeyTexA/B/X/Y_00`, whose image data is swapped for `nintendo`, and the diamond's letters are text the game takes from `LayoutMsg/DoCommand_00.msbt` of each language's `Mals` archive (and the layout's own text panes behind it), swapped the same way.

The Switch prompts stay, with a note under *Button prompts* and a `RomFS overlay: ...` line in the log, when the folder is missing (an install made by an older setup, or one where the setup could not prepare it and showed a warning; running the setup again on the install folder writes it), when it fails its check, with a game resolution of another shape than 16:9 that has no folder of its own (below), and when the `romfs_mods` folder replaces one of its files: `romfs-patches` then loads in its place, where two overlays replacing one file would stop the game. In a source build, `.venv-py314\Scripts\python tools\make_romfs_patches.py --version <ver> --prompts xbox` (and `--prompts xbox-label`) writes `build\<ver>\romfs-patches-xbox` (`-label`); it needs fontTools from `tools/requirements-py314.txt`. `--xbox-prompts` is the older name of `--prompts xbox`.

At a game resolution of another shape than 16:9 the game loads `romfs-patches-xbox-<w>x<h>` (or `romfs-patches-xbox-label-<w>x<h>`) in place of `romfs-patches-<w>x<h>`: the same prompts with that shape's UI layout, `make_romfs_patches.py --prompts xbox --aspect <w>x<h> --reuse <install>\romfs-patches-xbox --out <install>\romfs-patches-xbox-<w>x<h>`. The setup writes both for each of the fifteen sizes it lays the UI out for, as for the PC prompts below, so on a display of one of those shapes the game resolution follows the display with these prompts too. At another size, without that folder, the Switch prompts stay (*off at this game resolution*).

## PC button prompts

`prompts = pc` (*PC* on the *Button prompts* row) shows the key or mouse button the default keyboard bindings press each Switch button with, whatever the pad layout. The game loads `romfs-patches-pc` in place of `romfs-patches`, written by `tools/make_romfs_patches.py --prompts pc` (`button_icons.py`, `pc_icons`), which the setup runs next to the Xbox ones:

| Switch | Prompt |
|---|---|
| A, B, X, Y | E, Shift, Space, left click |
| L, R, ZL, ZR | Q, R, right click, F |
| Plus, Minus | Tab, M |
| D-pad | 1 (left), 2 (up), 3 (right), 4 (down), as a cross or the keys of the directions meant |
| Left stick, its press | W A S D (or the keys of the directions meant), Ctrl |
| Right stick, its press | the mouse, with an arrow for each direction meant, C |

- The glyphs keep the width of the game's own, so the right-aligned guides do not run into their text. A key is a rounded square with its label cut out in the game's own text fonts (Shift: an arrow over its name); a mouse button is a mouse with that button filled and the others hollow.
- The button guide in play draws every button from images of the Common archive (`Nt_KeyTexA/B/X/Y/L/ZL_00`, `Nt_KeyTexLSt_00` and `_01`, the stick tilted and pressed): all of them are painted with the same icons. The letters of the action prompt's diamond are one character of the Normal font each: E, an arrow for Shift, a bar for Space, L and R for the mouse buttons.
- Like the Xbox prompts they follow the defaults and not the bindings. `--binds <totk.ini>` draws the first key of each `bind_` line of that file instead, for a folder made by hand.
- At a game resolution of another shape than 16:9 the game loads `romfs-patches-pc-<w>x<h>` in place of `romfs-patches-<w>x<h>`: the same prompts with that shape's UI layout, `make_romfs_patches.py --prompts pc --aspect <w>x<h> --reuse <install>\romfs-patches-pc --out <install>\romfs-patches-pc-<w>x<h>`. The setup writes one for each of the fifteen sizes it lays the UI out for, so on a display of one of those shapes the game resolution follows the display with these prompts too. At another size, without that folder, the Switch prompts stay (*off at this game resolution*).

## Window keys

| Key | Gamepad | Action |
|---|---|---|
| F1 or Esc | Minus and Plus together | open or close the settings menu; the game gets no input while it is open |
| F10 | L3 + R3 (both stick clicks) | performance overlay: off, frame rate, full panel |
| F11 | | borderless fullscreen on or off, saved to `totk.ini` |

The Minus+Plus combo never reaches the game. In the menu the arrow keys, the D-pad or the left stick move, Enter or A selects, Q and E or L and R switch tabs, Esc or B goes back (Switch button names, as in the table above).

## Remapping

The *Controls* tab of the settings menu lists, under the button layout, every Switch button, the eight stick directions the keys drive (movement and camera) and the shake key, each with two key cells and a controller cell. Select a cell, press A or Enter, then press the new key, mouse button or controller button; Esc or five seconds without a press cancel, and a controller button cancels a key cell. Delete, Backspace or X clears the selected cell. A key another action has moves to the new one (the description names the action it left); a controller button swaps with the one it replaces. A remapped controller button is physical: from then on the face buttons (A B X Y) stay where they are in both layouts, until the buttons are put back as the layout in use has them or *Reset all buttons* is chosen. F1, F8, F10, F11 and Esc cannot be bound (the menu, the flight recorder and the window keep them). Punctuation keys are named by their place on a US keyboard; the menu shows the character the keyboard layout in use puts there.

The tab writes `totk.ini` lines, one per Switch button or stick direction: `bind_<button> = <entry>, <entry>, ...`. The button is `A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LCLICK RCLICK`, or a stick direction `LSTICK_LEFT LSTICK_UP LSTICK_RIGHT LSTICK_DOWN` (movement, W A S D by default) and `RSTICK_...` (camera, the arrow keys), or `SHAKE` (keys only, G by default). An entry is `pad:<name>` or `key:<name>`:

- pad names: `south east west north l r zl zr start back lstick rstick dpad_left dpad_up dpad_right dpad_down lpaddle1 rpaddle1 lpaddle2 rpaddle2` (`zl`, `zr` are the triggers; the paddles are the back buttons of an Xbox Elite, DualSense Edge or Steam Controller, upper pair 1, lower pair 2, and no Switch button has them by default)
- key names: `a` to `z`, `0` to `9`, `f2` to `f7`, `f9`, `f12`, `num0` to `num9`, `space tab enter backspace lshift rshift lctrl rctrl lalt capslock insert delete home end pageup pagedown up down left right`, `num_add num_sub num_mul num_div num_decimal`, `minus equals lbracket rbracket semicolon apostrophe comma period slash backslash grave`, `mouse_l mouse_r mouse_m mouse_x1 mouse_x2`
- `key:default` and `pad:default` keep that part's defaults (`pad:default` is each layout's own button).
- `none` leaves the button unbound.

A line replaces every default of that button, pad and keyboard, so list the keys you still want or use `key:default`. Pad names are physical positions, so a line means the same in both layouts and replaces the layout's own button for that Switch button. Stick directions take keys only: the controller's sticks and the mouse camera always work. Buttons without a line keep the table above, less any key a line binds: with `bind_X = key:t`, T presses X only, not also Y. Names are case-insensitive. An invalid entry logs one `input: bind ...` line and is skipped; a line without a valid entry changes nothing. The `TOTK_BIND_<button>` environment variables work like any other setting. A changed line applies at once (`runtime/input_win.c` builds the tables again at its next poll).

A pad that reports its face buttons swapped (A/B and X/Y, as some GuliKit models do) is fixed with:

```
bind_A = pad:south, key:E
bind_B = pad:east, key:lshift
bind_X = pad:west, key:space
bind_Y = pad:north, key:mouse_l
```

## Mouse camera

`mouse_mode = direct` (the default) turns the camera by the raw mouse motion on games 1.4.3 and 1.2.1, through hooks in the game's camera code (`runtime/mouse_direct.c`, the `mouse_direct` sites of `recomp/patches-v143.txt` and `recomp/patches-v121.txt`). None of them changes a step of the gamepad alone. The free-orbit camera receives raw-count
angular increments at its final spherical-angle output: 0.08 degrees per count on both axes at sensitivity 1 (about
14.3 cm a turn at 800 DPI), with no time multiplier, curve or carry into later frames. The controller's persistent pitch/yaw move with
the output. Mouse input is removed from its stick path first, so the camera's later vertical signed-square curve and
velocity filters cannot process it a second time.
The aiming camera (bow, throwing), the Purah Pad camera, the scope, the ability camera (Ultrahand, Fuse, Ascend,
Recall, Autobuild), the horse camera, lock-on, the bow drawn while locked on, climbing (walls and objects), the
waterfall's turn, shield surfing, the wakeboard and the dialogue cameras take the mouse the same way, at the same
degrees per count: each turns by
the stick times its own speeds and the frame time and adds that to a base angle, so the load of the stick it turns by
gets the gamepad's part only and the load of that base angle gets the mouse's degrees (`camera_load`, two sites per
axis per camera). Their speeds, time step, slow motion (bullet time) and, for the Purah Pad camera and the abilities,
the eased curve their pitch moves along do not touch the mouse; their own limits, collision and blends apply to the
result. Past the pitch limit of the scope or the ability camera the stick moves the camera's distance; the mouse's
motion past the limit keeps doing that through the stick. Recall's cursor is the ability camera's offset mode: the
camera keeps a direction, the stick moves the cursor across the screen (ReverseRecorderCursorSpeed, within
ReverseRecorderCursorRange) and the camera follows it by up to ReverseRecorderCameraRange (10 degrees across, 7.8 up).
The mouse moves the cursor so that its aim, the camera's follow plus the cursor's angle from the screen's centre (the
offset taken as the screen's half width or height at the camera's 50 degree view and aspect ratio), turns as many
degrees a count as the camera does elsewhere. The follow range is read from the game each step (until it has been read
once, the mouse is stick input there). The stick loads that only test for input keep the mouse in the stick. A step with mouse motion that no
direct hook took is logged (`mouse: step N with mouse motion took the stick path`, the first 30, then one in 600, with
the player's state), so the cameras still on stick input show in `logs/totk.log`.
Lock-on keeps pointing the camera at the target; the stick, and so the mouse, adds an offset around that direction,
which the game limits and lets return: the mouse's degrees are the step's turn the game adds to the offset and judges
the return by.
Shield surfing and the wakeboard turn their pitch by the square of the stick's y and move a second camera parameter
with it: their stick y keeps the mouse (that parameter and the camera's state follow it as before), the pitch gets
the degrees, and the pitch turn the game made from y is cut to the gamepad's share (p |p| / t |t|). Their stick x, and
the dialogue camera's (which is also its test for input), give a tiny value in the mouse's direction when the
gamepad's axis is at rest, so the camera still sees input without a stick turn.
Two inputs stay stick input: the waterfall's vertical axis moves the camera's height, not an angle, and the horse's
lock-on without a target (HorseLockOnEmpty) turns the stick's position into an offset that returns when the stick
does, which a mouse has no position for.
Those receive input after
the radial dead zone, unit-circle clamp and stick response exponent: x is 0.0343 stick units a count over the frame
ratio (the stick turns yaw 70 x / 30 degrees a step) and y 70 / 51 times that, linear like their pitch (51 y / 30
degrees a step), so both axes turn by the same amount for the same motion. `stick_sens` (which scales the game's
stick turn rates) is divided out. While the player uses an ability and the ability camera's hooks do not take the
mouse, its part is scaled by 1 / 0.8 and 70 / (1.5 x 51), the ability camera's own stick scales.
The player's verbs are read every 50 ms from the player actor (the player root's GOT slot main+0x3969918, the
component array +0x228, its AS component +0x158 and the verbs of its five slots). Ultrahand has a separate hook after
its manipulation input's power curves; it gets 0.01 stick units a count over the frame ratio, at most three full tilts
a step.
`mouse_aim_scale` (0.1 to 3, default 1) multiplies the mouse's motion while the bow is drawn (the verb UseBow or
DrawnBow with ZR held). The game's camera inversion and gamepad response remain in effect. `mouse_sens` scales raw
counts, with the same meaning as in 0.3.10 (the free camera turns 0.08 degrees a count at 1, and now every other camera
does too); its scale differs from the stick-emulation mode.
The Controls tab switches between *Direct* and *Stick emulation* (`mouse_mode = stick`).
The orbit camera keeps its desired pitch at the final angular output, including stationary frames, so a later game
filter cannot recenter it. The persistent filter state accounts for the camera's current vertical offset instead of
accumulating corrections. A gamepad vertical input releases that mouse target to the game's own. The game's
lock-on, recenter and override (aiming) drive the camera themselves: the mouse stops holding without restoring its
earlier target, and when they end (or after the camera went 100 ms without a step) it holds the pitch the camera has
then, so the camera does not spring back to a pitch from before the aim or to the game's default after a shot.
While the ability wheel is open (L held and its screen, ShortCutMainItem_00 or ShortCut_00, has the focus), mouse
counts move its virtual cursor, which stays where it was when the mouse stops. L or the D-pad held with no wheel or
strip on screen (L soothes the horse, turns menu tabs) leaves the mouse on the camera; with the menu cursor off
(`menu_cursor = 0`) the screens are not read and the buttons alone decide.
The horizontal quick lists (D-pad up/left/right: materials, shields and weapons) use separate 32 ms right-stick steps,
with at least 32 ms of neutral between them. At sensitivity 1, 32 horizontal counts request a step; a large flick
cannot queue continued scrolling after the mouse stops. Vertical movement is ignored in these lists. Switching or
releasing a selector resets its cursor/step state without sending the last movement to the camera. Selector input is
not consumed by the direct camera or Ultrahand hooks.
To detach a fused part from what Ultrahand holds, shake the mouse left and right, or press G (`bind_SHAKE`). A press
swings the right stick left and right for 0.8 s, holding the key keeps it swinging. With `mouse_mode = direct` a shake
of the mouse while Ultrahand holds an object swings the right stick with it: once the swings (10 degrees of camera turn
or more each, 125 counts at sensitivity 1) change direction twice within 0.35 s, the stick goes full tilt the
way the mouse moves, each side held at least two frames so the game reads it, and goes neutral as soon as the mouse
rests. Only your own swings reach the stick, so nothing keeps swinging after you stop; shake until the part comes off.
The camera hooks ignore that swing (the mouse turns the camera in degrees already). Turning a held object back and
forth at an ordinary pace is slower and does not count. The arrow keys or the gamepad's right stick shaken by hand
work too.
A step of the gamepad alone, with no mouse motion and no pitch the mouse holds, reaches the camera exactly as without
the hooks.
The mapping below is still used when the camera-input hook is unavailable or the game disables camera input.
Game 1.1.0 has no hooks and keeps stick emulation whatever `mouse_mode` says. On 1.2.1 `mouse_aim_scale` has no effect (the bow-in-hand state is not read there).

In stick-emulation mode, mouse motion (raw input) becomes right-stick deflection. Each presented frame takes the counts moved since the last frame, divides by the frame time and sets the deflection to `counts per second * TOTK_MOUSE_SENS / 3000` of full, held for that frame, so the turn rate does not depend on the frame rate. This is not mouselook: the game's stick curve and dead zone still apply, so very slow movements do nothing and a fast flick saturates. The mouse deflection competes with the gamepad and arrow keys by the larger-deflection rule.

`TOTK_MOUSE_SENS` (0 to 10, default 1; higher turns faster, 0 turns the mouse camera and the cursor capture off). While the window is the foreground window the cursor is hidden and confined to the client area. It is released on focus loss, minimise, while F11 changes the window, during a size or move loop and while a text entry is open. Esc releases it by hand: the cursor shows, the mouse camera and mouse buttons stop, and the window can be moved, minimised or closed. A mouse click inside the client area captures it again; that click does not reach the game. Activating the window by a click on its title bar or border does not capture, so it can be dragged or closed right away; Alt+Tab or a click in the client area captures as before. `TOTK_INPUT=none` disables the capture too.

`TOTK_STICK_DEADZONE` (0 to 50, default 8) is the gamepad stick dead zone in percent of full deflection, the same for both sticks. It is round: a stick inside the circle reads centred, and the rest of its travel is scaled to start at 0 at the edge, so full deflection still reaches full. Raise it for a stick that drifts. The game's own dead zone applies after it. `TOTK_LSTICK_DEADZONE` and `TOTK_RSTICK_DEADZONE` (the *Left/Right stick dead zone* rows under *Analog sticks* on the Controls tab) set one stick each and fall back to `TOTK_STICK_DEADZONE` while unset, so a worn stick can get a larger zone than the other. With a gamepad connected each row shows its stick's deflection now, before the dead zone (*stick now*): let go of the stick and set the row a few percent above the value it rests at.

The stick direction bits (`StickLLeft` to `StickRDown`) are derived from the sticks at 50% deflection. A gamepad plugged in later is picked up within one second; with none connected the log says `input: no gamepad`.

## Mouse in the game's menus

On game 1.4.3, while one of the game's menus has its focus box (the frame the d-pad moves: the title, the save list, the inventory and its System tab, Options, shops, dialogue choices), the cursor is released from the mouse camera and shown in the middle of the window. A focus gap of up to 300 ms (a tab switch, a popup opening: the game drops the focus box for a frame or two) keeps the pointer where it is instead of capturing and centring it again. The item under the pointer takes the focus as if the d-pad had moved there, with the game's highlight, sound and description; a left click is A on it, a right click is B. The middle and side mouse buttons keep their bindings (bound to L and R they turn the tabs), and T presses Y (sort). The keys and the pad keep working, and the focus moves only when the pointer moves. This happens only while the keyboard and mouse are the device used last: after a gamepad button press or a stick pushed past half way the menus are as on the console, with no pointer, until the mouse moves or a key is pressed (`input_kbm_last` in `runtime/input.c`). The ability wheel, the d-pad strips, Autobuild and the Purah Pad's camera stay gameplay: the mouse keeps the camera there. The map has no focus box; it is found in `eui::ScreenMgr`'s list of open screens (every 100 ms: the Purah Pad's frame `AppMenuOverlay_00` with `AppMap_00` and the menu background `AppMenuBG_00`), and there the mouse moves the map's cursor as the left stick does, with the cursor still captured, and the scroll wheel zooms: each notch is a full-tilt push of the right stick (the map's zoom) for 100 ms with 50 ms of neutral after it, up (in) for a turn away from you; a fast scroll's notches wait their turn (at most four), a turn the other way drops them. The Scope opens the same map screen without the menu background and keeps the mouse camera. When the menu closes the cursor is captured again.

`runtime/menu_cursor.c` reads eui's screens at each pad read and moves the focus with the writes `eui::BoxCursorMgr::moveBoxCursor` makes, so no game code is called. `menu_cursor = 0` (`TOTK_MENU_CURSOR=0`, live; the *Mouse in the game's menus* row of the Controls tab) turns it off; `TOTK_MENU_CURSOR_LOG=1` logs every screen it reads (by default the first six), with the layout size, the node count and the hit panes it uses.

## Nintendo Switch 2 controllers

Windows does not present the Switch 2 Pro Controller as a gamepad SDL can open (the log says `input: no gamepad`), so it needs a bridge program that wakes it and creates a virtual gamepad. The DS4Windows fork by hbashton ([github.com/hbashton/DS4Windows](https://github.com/hbashton/DS4Windows), tested with release VIIPERRC4.5.1, a pre-release) with the output set to an emulated DualSense gives buttons, rumble, gyro and accelerometer, and reads the sticks through the controller's calibration (they rest at 0 to 2%). The game reads its virtual DualSense directly; the log then shows `input: gamepad "DualSense Wireless Controller" (rumble, gyro and accelerometer, ...)` and `input: motion sensors on`. Its release notes also list Joy-Con 2 support, which is untested here.

Bridges that output a ViGEm DualShock 4 (ProCon2Windows, connectyourpro as tested) give buttons and rumble but no motion: their virtual DualShock 4 reports a gyro and an accelerometer that never change, so motion aiming and Ultrahand rotation do nothing. Run one bridge at a time; Steam Input can also claim the controller, so close Steam if the bridge does not find it.

## Scripted input

`TOTK_INPUT_SCRIPT=<file>` replays presses. It is ORed with real input and still applies under `TOTK_INPUT=none`. One event per line, `#` starts a comment:

```
<frame> <button> [hold_frames]            # A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LCLICK RCLICK; hold defaults to 4
<frame> LSTICK|RSTICK <x> <y> [hold_frames]   # -32767..32767, up is +y
<frame> MOUSE <dx> <dy>                  # direct-mode raw counts, once; down is +dy
@<ms> <button> [hold_ms]                  # timed on the world clock
@<ms> LSTICK|RSTICK <x> <y> [hold_ms]
@<ms> MOUSE <dx> <dy>
```

`<frame>` counts 60 Hz ticks of game time since the first frame (presented frames at 60 fps), so the title menus get the presses at the same game time at any frame limit. `@<ms>` counts milliseconds of the world clock: game time stepped since the end of the first save load (the player leaves the title screen's origin), so a timed event lands at the same game time at any frame rate and load length. A later load, such as fast travel, still shifts what follows. In a script with timed lines the untimed ones stop when the world clock starts, as the load ends at another tick in each run. Each event logs once (`input: script frame N tick T: A`). `tools/input/load_save.txt` loads the first save from the title screen.

`TOTK_INPUT_RECORD=<file>` (`input_record` in `totk.ini`, or Input recording in the menu) writes the merged input in the same format, one line per button press and per stick segment: events before the world clock starts by tick, the rest timed; a button or stick held when the world clock starts is cut there and goes on as a timed event. The file replays as a script, which makes it the repro to attach to a bug report.

In direct mouse mode the recording also writes one `MOUSE` event per consumed raw displacement. These are one-shot
displacements rather than held stick segments. They replay without cursor capture, including with `input = none`;
the game must be accepting camera input when the event reaches its camera update.

`TOTK_TEST_PAD=<lx>,<ly>,<rx>,<ry>` attaches an SDL virtual gamepad at start whose sticks rest at these raw SDL axis values (-32768 to 32767, down is +y), so a drifting or held stick can be tested without a controller: the values go through the gamepad mapping and `TOTK_STICK_DEADZONE` like a real pad's, and `TOTK_INPUT_RECORD` shows what reached the game.

## RenderDoc capture (blocked)

`TOTK_RDOC=<frame>[,...]` (`runtime/rdoc_win.c`; `TOTK_RDOC_DIR`, `TOTK_RDOC_DLL`) loads RenderDoc before `vkCreateInstance`. RenderDoc hides `VK_EXT_external_memory_host`, which the back end requires for guest pool imports (`runtime/vk_memory.c`), so device creation fails under it. A capture needs a copy-based pool fallback first.
