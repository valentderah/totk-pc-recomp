# Controls

Input comes from the first SDL3 gamepad, the keyboard and the mouse, merged into one Switch controller (`runtime/input_win.c`). Buttons are ORed; each stick takes the source with the larger deflection. Keys and mouse buttons count only while the game window is in the foreground. `TOTK_INPUT=none` disables both (automated runs). `TOTK_*` settings can also be set in `totk.ini` next to `totk.exe` (`input = none`, `motion = 0`, `rumble = 0`); the environment overrides the file. `TOTK_RUMBLE_STRENGTH` is the rumble strength in percent (0 to 300, default 130; 0 is the same as `rumble = 0`). `TOTK_RUMBLE_CURVE` is the exponent applied to the amplitude before the strength (0.2 to 1.5, default 0.6; lower makes weak pulses stronger, 1 is linear). `TOTK_RUMBLE_TRIGGERS=1` also drives the impulse triggers of an Xbox pad.

`TOTK_PAD_LAYOUT` (`pad_layout`, the *Button layout* row on the Controls tab, live) picks which face button is Switch A:

- `nintendo`: by position, as on a Switch (the table below).
- `xbox`: south A, east B, west X, north Y, as an Xbox pad labels them; on a PlayStation pad cross is A, circle B, square X, triangle Y.
- `auto` (default): `nintendo` for a pad whose east button SDL reports labelled A (Switch Pro Controller, Nintendo-layout pads), `xbox` for the others.

The log names the layout in effect (`input: gamepad layout ...`); the menu shows it under *Automatic*. Which controller's buttons the game's prompts show is `TOTK_PROMPTS` (see Button prompts below). 0.1.9's `pad_layout = xbox_prompts` is read as `pad_layout = nintendo` with `prompts = xbox`, and the menu writes it that way.

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

Esc is unbound in the game; it opens the settings menu and releases the mouse cursor (see Mouse camera). Z and X duplicate ZL and ZR so menu pages turn with two neighbouring keys; the menu tabs (L, R) are Q and R. T duplicates Y for the game's menus, where the left mouse button is A while the pointer is shown (sort the inventory with T). The mouse wheel zooms the Purah Pad's map and its camera, steps the game's menus and the d-pad strips (see Mouse in the game's menus), and is unbound elsewhere. The bindings are the tables of `input_map` in `runtime/input_map.c`: the keys (platform-neutral key ids) and one gamepad table per layout (gamepad inputs by position); `runtime/input_win.c` translates Win32 virtual keys and SDL buttons.

## Xbox and PlayStation button prompts

`TOTK_PROMPTS` (`prompts`, the *Button prompts* row on the Controls tab, read at start) picks the controller whose buttons the game's prompts show: `nintendo` (default) keeps the game's own, `xbox` shows an Xbox controller's, `playstation` a PlayStation controller's, `pc` the keyboard and mouse (PC button prompts, below).

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

### PlayStation

`prompts = playstation` shows a PlayStation pad's buttons: the face buttons as ○ (circle), × (cross), △ (triangle) and □ (square), L1, R1, L2 and R2 on the shoulders and triggers, Options and Share on Plus and Minus, and the d-pad as a cross. Each prompt names the button to press, so the shapes follow `pad_layout`:

| Switch | `nintendo` layout | `xbox` or `auto` layout |
|---|---|---|
| A, B, X, Y | ○, ×, △, □ | ×, ○, □, △ |
| L, R | L1, R1 | L1, R1 |
| ZL, ZR | L2, R2 | L2, R2 |
| Plus, Minus | Options, Share | Options, Share |
| D-pad | cross | cross |

The game's text fonts already have the four shapes (U+25CB, U+00D7, U+25B3 and U+25A1), so the action prompt's diamond uses them as text; the action prompt's button image and the button guide's images are drawn with the same shapes. The game loads `romfs-patches-playstation` (`pad_layout = nintendo`) or `romfs-patches-playstation-xbox` (`xbox`, `auto`) in place of `romfs-patches`; the setup writes both with `tools/make_romfs_patches.py --prompts playstation` and `--prompts playstation-xbox` (`tools/button_icons.py`, `playstation_icons`). With `xbox` and `auto` the diamond is laid out as for the Xbox prompts: × at the bottom, ○ on the right, □ on the left and △ at the top. At a game resolution of another shape the prompts follow the shape's UI layout as the Xbox ones do (below).

The prompts are part of the game's fonts, textures and messages, so they follow the layout and not the bindings. A button remapped with a `bind_` line or on the Controls tab keeps the prompt the game draws for it, which then names the wrong button. Use the Xbox or PlayStation prompts with the default bindings.

The game reads its UI archive and fonts once at boot, so the prompts change at the next start, also after `pad_layout` changes between `nintendo` and the others (the menu says *prompts change after restart*); the buttons change at once. Under that note the menu offers *Restart now*: after a confirmation it writes `totk.ini`, closes the game as *Quit game* does and starts the game again with the same command line, which waits until the old process is gone before it opens the log and the saves (`runtime/gfx_present.c`, `gfx_window_closed`; `plat_restart` and `plat_restart_wait` in `runtime/platform/win32.c` and `linux.c`). Progress since the last save is lost, as with *Quit game*; while the game writes a save, the confirmation waits. With `prompts = xbox` the game loads `romfs-patches-xbox` (`pad_layout = nintendo`) or `romfs-patches-xbox-label` (`xbox`, `auto`) next to `romfs-index.bin` in place of `romfs-patches` (`runtime/romfs.c`, `overlay_init`; `runtime/input_map.c`, `input_prompts_folder`): the same 60 fps patches, with the prompts drawn from the game's own files by `tools/make_romfs_patches.py --prompts xbox` or `--prompts xbox-label` (`tools/button_icons.py`, after MacNativeGames' `make_button_icons.py`), which the setup runs next to the plain one:

- every prompt but the button guide in play is text in the button font, `scft/External.bfotf` in each `Font/Font*.Nin_NX_NVN.bfarc.zs`: glyphs U+E040 to U+E043 (A B X Y), U+E044 to U+E047 (L R ZL ZR), U+E04B and U+E04C (Plus, Minus), U+E049 and U+E060 to U+E065 (the d-pad). fontTools writes the font again, so the archives grow and the folder also carries the resource size table (`System/Resource/ResourceSizeTable.Product.<n>...rsizetable.zs`) with their entries raised by as much; without that the game stops at start.
- the button guide in play draws L and ZL from the images `Nt_KeyTexL_00` and `Nt_KeyTexZL_00` of the Common archive's `timg/__Combined.bntx`, painted with the same icons. The action prompt draws its button from `Nt_KeyTexA/B/X/Y_00`, whose image data is swapped for `nintendo`, and the diamond's letters are text the game takes from `LayoutMsg/DoCommand_00.msbt` of each language's `Mals` archive (and the layout's own text panes behind it), swapped the same way.

With `prompts = playstation` the game loads `romfs-patches-playstation` (`pad_layout = nintendo`) or `romfs-patches-playstation-xbox` (`xbox`, `auto`) the same way, written by `tools/make_romfs_patches.py --prompts playstation` and `--prompts playstation-xbox`: the same 60 fps patches, with the face buttons' images and the diamond's letters drawn as ○ × △ □, the shoulders and triggers as L1 R1 L2 R2, Options and Share, and the button guide's L and ZL images. The game's text fonts already have the four shapes, so the diamond's panes and messages hold those characters instead of the letters.

The Switch prompts stay, with a note under *Button prompts* and a `RomFS overlay: ...` line in the log, when the folder is missing (an install made by an older setup, or one where the setup could not prepare it and showed a warning; running the setup again on the install folder writes it; an install whose setup's tools sit next to the game makes it itself, see below), when it fails its check, with a game resolution of another shape than 16:9 that has no folder of its own (below), and when the `romfs_mods` folder replaces one of its files: `romfs-patches` then loads in its place, where two overlays replacing one file would stop the game. In a source build, `.venv-py314\Scripts\python tools\make_romfs_patches.py --version <ver> --prompts xbox` (and `--prompts xbox-label`) writes `build\<ver>\romfs-patches-xbox` (`-label`); `--prompts playstation` and `--prompts playstation-xbox` write `build\<ver>\romfs-patches-playstation` and `-playstation-xbox` the same way. It needs fontTools from `tools/requirements-py314.txt`. `--xbox-prompts` is the older name of `--prompts xbox`.

A missing prompts folder, at 16:9 or of a game resolution's shape, is made by the game itself on an install whose setup left its tools next to the game (`build-cache/rebuild.txt`): choosing those prompts or a pad layout that needs it, or starting the game with them selected, starts `make_romfs_patches.py --background` for it (`runtime/romfs.c`, `romfs_folders_prepare`; `runtime/ui/menu.cpp`, `folders_tick`, once the choice has stayed for 1.5 seconds). The note says *prompt files being made in the background; used at the next start*; when the tools did not start it says so and *Try again* starts them again. A source build has no `rebuild.txt` and keeps the notes above.

At a game resolution of another shape than 16:9 the game loads `romfs-patches-<style>-<w>x<h>` (`romfs-patches-xbox-<w>x<h>`, `-xbox-label-`, `-playstation-` or `-playstation-xbox-`) in place of `romfs-patches-<w>x<h>`: the same prompts with that shape's UI layout, `make_romfs_patches.py --prompts <style> --aspect <w>x<h> --reuse <install>\romfs-patches-<style> --out <install>\romfs-patches-<style>-<w>x<h>`. The setup writes it for the shapes of the connected displays and `output_res`, for the selected prompts only (installer Shapes step), as for the PC prompts below. At another size, without that folder, the Switch prompts stay (*off at this game resolution*) until the game has made it in the background (above).

## PC button prompts

`prompts = pc` (*PC* on the *Button prompts* row) shows the key or mouse button the default keyboard bindings press each Switch button with, whatever the pad layout. The game loads `romfs-patches-pc` in place of `romfs-patches`, written by `tools/make_romfs_patches.py --prompts pc` (`button_icons.py`, `pc_icons`), which the setup runs next to the Xbox ones:

| Switch | Prompt |
|---|---|
| A, B, X, Y | E, Shift, Space, left click (T in text prompts) |
| L, R, ZL, ZR | Q, R, right click, F |
| Plus, Minus | Tab, M |
| D-pad | 1 (left), 2 (up), 3 (right), 4 (down), as a cross or the keys of the directions meant |
| Left stick, its press | W A S D (or the keys of the directions meant), Ctrl |
| Right stick, its press | the mouse, with an arrow for each direction meant, C |

- The glyphs keep the width of the game's own, so the right-aligned guides do not run into their text. A key is a rounded square with its label cut out in the game's own text fonts (Shift: an arrow over its name); a mouse button is a mouse with that button filled and the others hollow.
- The button guide in play draws every button from images of the Common archive (`Nt_KeyTexA/B/X/Y/L/ZL_00`, `Nt_KeyTexLSt_00` and `_01`, the stick tilted and pressed): all of them are painted with the same icons. The letters of the action prompt's diamond are one character of the Normal font each: E, an arrow for Shift, a bar for Space, L and R for the mouse buttons.
- Y has two keys, the left mouse button and T. The button guide in play and the action prompt show the left button, which attacks there; the prompts in text (the menus' guides such as *Sort*, the dialogue log's) show T, because while a menu has the pointer the left button is that menu's A, and T presses Y everywhere (`PC_TEXT_KEYS` in `tools/button_icons.py`). With a `bind_y` line, the text prompts take its first key that is not a mouse button (its only key when all are mouse buttons).
- They follow the bindings of the install's `totk.ini` as the setup found them: its *Drawing the keyboard and mouse prompts* step passes the file as `--binds` (the first key of each `bind_` line), and the keyboard defaults without one. After a change on the Controls tab run the setup again: that step alone runs, since it is keyed by the `bind_` lines (`PcPrompts` in `installer/steps.py`). The Xbox prompts follow the defaults.
- At a game resolution of another shape than 16:9 the game loads `romfs-patches-pc-<w>x<h>` in place of `romfs-patches-<w>x<h>`: the same prompts with that shape's UI layout, `make_romfs_patches.py --prompts pc --aspect <w>x<h> --reuse <install>\romfs-patches-pc --out <install>\romfs-patches-pc-<w>x<h>`. The setup writes one for each of the fifteen sizes it lays the UI out for, so on a display of one of those shapes the game resolution follows the display with these prompts too. At another size, without that folder, the Switch prompts stay (*off at this game resolution*).

## Window keys

| Key | Gamepad | Action |
|---|---|---|
| F1 or Esc | Minus and Plus together | open or close the settings menu; the game gets no input while it is open |
| F10 | L3 + R3 (both stick clicks) | performance overlay: off, frame rate, full panel |
| F11 | | borderless fullscreen on or off, saved to `totk.ini` |
| F6, F7 | | with `quicksave = 1`: quick save and quick load, as in Half-Life, with no menu; `quicksave_key` and `quickload_key` choose other keys ([Quick save](#quick-save)) |

The Minus+Plus combo never reaches the game. In the menu the arrow keys, the D-pad or the left stick move, Enter or A selects, Q and E or L and R switch tabs, Esc or B goes back (Switch button names, as in the table above).

## Remapping

The *Controls* tab of the settings menu lists, under the button layout, every Switch button, the eight stick directions the keys drive (movement and camera) and the shake key, each with two key cells and a controller cell. Select a cell, press A or Enter, then press the new key, mouse button or controller button; Esc or five seconds without a press cancel, and a controller button cancels a key cell. Delete, Backspace or X clears the selected cell. A key another action has moves to the new one (the description names the action it left); a controller button swaps with the one it replaces. A remapped controller button is physical: from then on the face buttons (A B X Y) stay where they are in both layouts, until the buttons are put back as the layout in use has them or *Reset all buttons* is chosen. F1, F8, F10, F11 and Esc cannot be bound (the menu, the flight recorder and the window keep them). Punctuation keys are named by their place on a US keyboard; the menu shows the character the keyboard layout in use puts there.

The tab writes `totk.ini` lines, one per Switch button or stick direction: `bind_<button> = <entry>, <entry>, ...`. The button is `A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LCLICK RCLICK`, or a stick direction `LSTICK_LEFT LSTICK_UP LSTICK_RIGHT LSTICK_DOWN` (movement, W A S D by default) and `RSTICK_...` (camera, the arrow keys), or `SHAKE` (keys only, G by default). An entry is `pad:<name>` or `key:<name>`:

- pad names: `south east west north l r zl zr start back lstick rstick dpad_left dpad_up dpad_right dpad_down lpaddle1 rpaddle1 lpaddle2 rpaddle2 touchpad` (`zl`, `zr` are the triggers; the paddles are the back buttons of an Xbox Elite, DualSense Edge or Steam Controller, upper pair 1, lower pair 2; `touchpad` is the touchpad click of a DualSense or DualShock 4; no Switch button has the paddles or the touchpad by default)
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

## Free camera

On game 1.4.3, a chosen gamepad combo or a key steps the free camera through three stages: move, lock, and off again.
It is off by default: turn on *Free camera* on the Controls tab (`freecam_patch = 1`) and restart. Then pick the
combo or the key, neither set by default, with `freecam_pad` and `freecam_key` in `totk.ini` or on the Controls tab
(for example ZL+ZR+L3, LT+RT+L3 on an Xbox pad, and F2; `none` turns one off). An input script can press the combo as a
gamepad would; the keys bound to its buttons do not. While the free camera moves, the world goes on but the game gets
no input, so Link stands where he was.

Locked, the view stays where the flight left it and the game gets its input back. Link walks where the left stick
points on screen, as the game steers him by the camera it draws. The right stick and the mouse turn the game camera,
which still follows Link unseen.

| Gamepad | Keyboard and mouse (default bindings) | Free camera |
|---|---|---|
| Left stick | W A S D | forward and back along the view, and sideways |
| ZR / ZL (RT / LT) | X / Z | straight up / down; a trigger by how far it is pressed |
| Right stick | mouse, arrow keys | turn (yaw and pitch; pitch stops 5 degrees short of straight up or down) |
| R / L (RB / LB) | R / Q | each press doubles / halves the speed, 0.25 to 512 m/s |

The keys are the ones bound to the left and right stick and to ZR, ZL, R and L (`bind_` lines), so remapped keys follow.
The mouse turns the free camera at the game camera's rate (0.08 degrees a count at `mouse_sens = 1`) and the game's
mouse camera gets none of it meanwhile. Each switch starts at the game camera's position and view at 8 m/s; the speed
follows the clock, so it is the same at any frame rate and game speed. The horizon stays level.

How it works (`runtime/freecam.c`): the camera component update (main+0x628460) copies each component's eye, target
and up into its sead camera. The site `freecam` (`recomp/patches-v143.txt`, main+0x6286d4) runs before that copy and
writes the component's one-shot eye and target overrides (+0x180, +0x198), which the update takes in place of the pose
and then clears, so the game camera's own state is never changed and resumes the frame the free camera ends. Locked,
the site keeps writing the overrides without moving them. The up
vector is levelled through the component's up offset (+0x200), which is kept and put back. Of the three components a
frame updates, the free camera drives the two that share the player camera's eye and leaves the one at the default pose
(eye 0, 0, 10). After a load replaced the components it picks the new ones and keeps its position.

## Mouse camera

`mouse_mode = direct` (the default) turns the camera by the raw mouse motion on games 1.1.0, 1.2.1 and 1.4.3, through hooks in the game's camera code (`runtime/mouse_direct.c`, the `mouse_direct` sites of `recomp/patches-v110.txt`, `recomp/patches-v121.txt` and `recomp/patches-v143.txt`). None of them changes a step of the gamepad alone. The free-orbit camera receives raw-count
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
Some cameras run none of the direct hooks and read their turn from input the hooks never see: Autobuild's placement
camera turns by the right stick but, in direct mode, not by the mouse (issue #1300). After two
camera steps in a row that ran no direct hook (none of the free orbit, the load sites above or Ultrahand's held-object
input), the mouse acts as the right stick there, as with `mouse_mode = stick`, until a step runs a direct hook again
(`mouse: the camera runs no direct hook; the mouse acts as the right stick` and `... runs a direct hook again` in the
log, the first 6 changes; `camera_unhooked` in `runtime/mouse_direct.c`).
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
not consumed by the direct camera or Ultrahand hooks. The scroll wheel steps these lists too: each notch is one step,
away from you to the left, towards you to the right.
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
On 1.1.0 and 1.2.1 `mouse_aim_scale` has no effect (the bow-in-hand state is not read there).

In stick-emulation mode, mouse motion (raw input) becomes right-stick deflection. Each presented frame takes the counts moved since the last frame, divides by the frame time and sets the deflection to `counts per second * TOTK_MOUSE_SENS / 3000` of full, held for that frame, so the turn rate does not depend on the frame rate. This is not mouselook: the game's stick curve and dead zone still apply, so very slow movements do nothing and a fast flick saturates. The mouse deflection competes with the gamepad and arrow keys by the larger-deflection rule.

`TOTK_MOUSE_SENS` (0 to 10, default 1; higher turns faster, 0 turns the mouse camera and the cursor capture off). While the window is the foreground window the cursor is hidden and confined to the client area. It is released on focus loss, minimise, while F11 changes the window, during a size or move loop and while a text entry is open. Esc releases it by hand: the cursor shows, the mouse camera and mouse buttons stop, and the window can be moved, minimised or closed. A mouse click inside the client area captures it again; that click does not reach the game. Activating the window by a click on its title bar or border does not capture, so it can be dragged or closed right away; Alt+Tab or a click in the client area captures as before. `TOTK_INPUT=none` disables the capture too.

`TOTK_STICK_DEADZONE` (0 to 50, default 8) is the gamepad stick dead zone in percent of full deflection, the same for both sticks. It is round: a stick inside the circle reads centred, and the rest of its travel is scaled to start at 0 at the edge, so full deflection still reaches full. Raise it for a stick that drifts. The game's own dead zone applies after it. `TOTK_LSTICK_DEADZONE` and `TOTK_RSTICK_DEADZONE` (the *Left/Right stick dead zone* rows under *Analog sticks* on the Controls tab) set one stick each and fall back to `TOTK_STICK_DEADZONE` while unset, so a worn stick can get a larger zone than the other. With a gamepad connected each row shows its stick's deflection now, before the dead zone (*stick now*): let go of the stick and set the row a few percent above the value it rests at.

The stick direction bits (`StickLLeft` to `StickRDown`) are derived from the sticks at 50% deflection. A gamepad plugged in later is picked up within one second; with none connected the log says `input: no gamepad`.

## Mouse in the game's menus

On game 1.4.3, while one of the game's menus has its focus box (the frame the d-pad moves: the title, the save list, the inventory and its System tab, Options, shops, dialogue choices), the cursor is released from the mouse camera and shown in the middle of the window. A focus gap of up to 300 ms (a tab switch, a popup opening: the game drops the focus box for a frame or two) keeps the pointer where it is instead of capturing and centring it again. The item under the pointer takes the focus as if the d-pad had moved there, with the game's highlight, sound and description; a left click is A on it, a right click is B. The middle and side mouse buttons keep their bindings (bound to L and R they turn the tabs), and T presses Y (sort). The scroll wheel moves the focus up and down, one step a notch (a full-tilt push of the left stick for 70 ms with 50 ms of neutral after it, up for a turn away from you), so a long list of items scrolls without moving the pointer; `menu_cursor = 0` turns this off with the pointer. The keys and the pad keep working, and the focus moves with the pointer only when the pointer moves: after the wheel, a key or the pad moved the focus off the pointer's item, the pointer takes it back once it has moved 6 layout units (the menus' 1280 x 720: about 9 window pixels at 1080p, 18 at 4K) from where it was then (`DEAD_ZONE` in `runtime/menu_cursor.c`), so a hand resting on the mouse does not undo a scroll; a click acts at once. With `pointer_image = 1` (off by default) the game window shows a pointer of its own while the cursor is not captured, a gold arrowhead drawn at the window's DPI (`runtime/cursor_art.c`), in place of the system arrow. This happens only while the keyboard and mouse are the device used last: after a gamepad button press or a stick pushed past half way the menus are as on the console, with no pointer, until the mouse moves or a key is pressed (`input_kbm_last` in `runtime/input.c`). The ability wheel, the d-pad strips, Autobuild and the Purah Pad's camera stay gameplay: the mouse keeps the camera there. The map has no focus box; it is found in `eui::ScreenMgr`'s list of open screens (every 100 ms: the Purah Pad's frame `AppMenuOverlay_00` with `AppMap_00` and the menu background `AppMenuBG_00`), and there the mouse moves the map's cursor as the left stick does, with the cursor still captured, and the scroll wheel zooms: each notch is a full-tilt push of the right stick (the map's zoom) for 100 ms with 50 ms of neutral after it, up (in) for a turn away from you; a fast scroll's notches wait their turn (at most four), a turn the other way drops them. The Scope opens the same map screen without the menu background and keeps the mouse camera. The Purah Pad's camera (the ability wheel's camera) opens the frame and its pointer `CameraPointer_00` without `AppMap_00` (the Scope has it); there the scroll wheel zooms as the camera's guide shows: each notch is a press of the d-pad's up (in, for a turn away from you) or down for 100 ms with 50 ms of release after it (`menu_cursor_camera`). The Scope itself has no zoom; the wheel does nothing there. When the menu closes the cursor is captured again.

`runtime/menu_cursor.c` reads eui's screens at each pad read and moves the focus with the writes `eui::BoxCursorMgr::moveBoxCursor` makes, so no game code is called. `menu_cursor = 0` (`TOTK_MENU_CURSOR=0`, live; the *Mouse in the game's menus* row of the Controls tab) turns it off; `TOTK_MENU_CURSOR_LOG=1` logs every screen it reads (by default the first six), with the layout size, the node count and the hit panes it uses.

### Left click on the map

With `map_click = 1` (off by default, so each key presses one controller button as everywhere else), on game 1.4.3, while the Purah Pad's map is open (found as above; the Scope is not the map), the left mouse button presses A at the map's cursor instead of the button it is bound to (Y by default). A press is A or the binding from the moment the button goes down until it is released, so a button held as the map opens or closes keeps what it pressed (`poll_merged` in `runtime/input.c`). `map_click = 1` (`TOTK_MAP_CLICK=1`, live; the *Left click on the map* row of the Controls tab) turns it on and `0`, the default, leaves the left button on its binding there. The map is found only while `menu_cursor` is on.

### Clicks in dialogue

With `dialogue_click = 1` (off by default, so each key presses one controller button as everywhere else), on game 1.4.3, while a dialogue box is open (the layout `MessageDialog_00`: a character's lines and the game's own messages), the left mouse button presses B and the right one A, instead of the buttons they are bound to. B continues the text as A does and also shows the rest of a line that is still scrolling in; the left button's default, Y, would open the dialogue log there, and the log stays in the way until B closes it. Dialogue choices are a menu with the focus box, so the pointer above takes their clicks and a left click is A on the choice under it: clicking through a conversation fast does not pick the choice the focus starts on. A press is the dialogue's button or the binding from the moment the button goes down until it is released: the click that closes the last box does not become an attack while it is still held, and a button held when the box opens does not skip its first line. Between two lines of a conversation the game closes the box and opens it again, so both buttons stay B and A for 500 ms after the box was last open (`DIALOGUE_GAP_MS`); the first clicks after a conversation fall in that time too, as they do for a player who taps the button through the last line. `TOTK_MENU_CURSOR_LOG=1` logs how long each such gap was. The dialogue window has no focus box of its own: the scan of `eui::ScreenMgr`'s open screens (every 100 ms) finds its screen, and from then on that screen's state is read at every pad read (`dialogue_shown` in `runtime/menu_cursor.c`), so the first click after a box opens is already the dialogue's. `dialogue_click = 1` (`TOTK_DIALOGUE_CLICK=1`, live; the *Clicks in dialogue* row of the Controls tab) turns it on and `0`, the default, leaves both buttons on their bindings; the setting does not depend on `menu_cursor`. The log names the first box it sees (`menu_cursor: a dialogue box is open ...`).

## Quick save

With `quicksave = 1` (off by default; the *Quick save and load* row of the Controls tab, live), on game 1.4.3, one key saves the game at once and another loads that save: F6 and F7 by default, the keys Half-Life has for them. No menu shows on the screen (`runtime/quicksave.c`). It is for the keyboard and the mouse only (a gamepad has no quick save button yet) and for game 1.4.3 only (the addresses below are 1.4.3's; on 1.2.1 the setting does nothing).

Quick save asks the game for one of its own autosaves, the way its event graphs do: it calls the AutoSaveMgr's request with the kind the sequence node `OneShotAutoSaveMgrRequestAutoSave` uses, after the game's own check whether it would autosave now. That check also holds autosaves back for some ten minutes after the game's last one; quick save, being the player's own, skips that wait (it sets the AutoSaveMgr's timer to 0, and puts it back when the game says no for another reason), as Save in the System tab does. The game then saves as it does after a shrine or a story event, its autosave icon included, into one of its autosave slots: the oldest autosave is replaced, as by any autosave of the game. The commit of that save names the slot folder it wrote (`fs_save_commits` in `runtime/fs_host.c`), and quick save keeps a copy of it: `<profile>/quicksave/slot`, with the slot's name in `<profile>/quicksave/slot.txt`. *Game saved* shows at the top of the picture for a few seconds (`menu_toast`, `runtime/ui/menu.cpp`). The game can take the request and still hold the autosave back (seen once in a fight in the Depths); when no save commit comes within 5 s, quick save takes its request back, so the game does not save some time later on its own, and says it is not possible right now. The game's manual save is not touched.

Quick load has no request in play to start from: the game starts a load only from its own load list (System tab) or its Game Over screen, and loads the slot that screen names. So quick load goes through the load list as a player with a controller would, but out of sight. *Loading the quick save* shows at the top of the picture, and from the next frames on the window keeps that picture (`gfx_present_hold` in `runtime/gfx_present.c`, as while the window is minimized) while quick load presses Plus, R until the System tab shows, Load, A on the list's first save and Load on the confirmation. From that confirmation on, every read the game makes of a file in a slot folder (`save:/slot_NN/<file>`) opens the quick save's file instead (`fs_save_read_from` in `runtime/fs_host.c`; the log names each one, `fs: quick load: ... read from ...`), so the game loads the quick save whichever slot the list offered first, and no slot of the save is written for it. The redirect ends with the load: once the game has read the quick save's files, its loading fade has closed and the HUD has been back for half a second (`quickload: slot reads go to the save again`); at once when a menu with the focus box opens, so a Load right after it lists and loads the slots as they are. The picture comes back with the game's loading fade, about 5 s after the key. When a step finds a screen it does not expect, or none within its time, the log names the step, B is pressed until no menu has the focus box, the picture comes back and the line says *Quick load failed*. The quick save stays in the profile, so F7 loads the same state again until F6 makes a new one.

Both start only in play: a save loaded, the HUD (`PlayerStatus_00`) open, no menu with the focus box, no dialogue box and no loading fade. Anywhere else, and wherever the game itself would not autosave (a cutscene, an event, a fight), nothing happens but a line at the top of the picture (*Quick save is not possible right now*), and the log says why (`quicksave: not now: ...`). Without a quick save F7 says so. Quick save's calls into the game run on the game thread in its pad read, between two of its own calls; while quick load runs the game gets only its presses, not the player's input. While the setting is on its two keys reach no binding; with it off nothing changes and nothing is logged.

The keys are `quicksave_key` and `quickload_key` (`TOTK_QUICKSAVE_KEY`, `TOTK_QUICKLOAD_KEY`, live). The *Quick save key* and *Quick load key* rows under the switch take any key or mouse button the way a binding's cell does: select the cell, press the key (Esc cancels); Delete, Backspace or X leaves that one without a key (`none`). In `totk.ini` the value is a key name of the [Remapping](#remapping) list, in any case, or `none`. A name the list does not have is logged (`quicksave: quickload_key: unknown key '...', f7 is used`) and the default is used; the same key for both saves and leaves loading without a key. With the setting on the log names the keys in use (`quicksave: f6 saves, f7 loads`). A key a binding also uses presses only quick save or quick load while the setting is on: the row says which button it took (*also A*).

`QUICKSAVE` and `QUICKLOAD` lines in an input script ([Scripted input](#scripted-input)) start it as the keys do.

## Gameplay tweaks

With `gameplay_tweaks_enabled = 1` (`TOTK_GAMEPLAY_TWEAKS_ENABLED=1`, live; off by default in `totk.ini`), the mouse wheel and keyboard can drive common gameplay interactions directly:

- **Ultrahand wheel controls**: select the default wheel action in F1 > Controls > Gameplay tweaks, or set `gameplay_ultrahand_default_action` to `rotate_x` (default), `rotate_y` or `distance`. Two configurable modifiers select the remaining actions.
- **Ultrahand rotation**: enable rotation with `gameplay_ultrahand_rotate` and the alternate axis with `gameplay_ultrahand_c_axis`. Adjust speed using `gameplay_rotation_sensitivity`.
- **Ultrahand distance**: enable pushing and pulling objects with `gameplay_ultrahand_distance`, and adjust speed using `gameplay_distance_sensitivity`.

The first modifier (`gameplay_ultrahand_rotation_key`) defaults to C.
The second modifier (`gameplay_ultrahand_distance_key`) defaults to the right mouse button.
Both accept a keyboard key, mouse button or `none`.

| Default wheel action | First modifier + wheel | Second modifier + wheel |
| --- | --- | --- |
| Rotate X | Rotate Y | Move forward/backward |
| Rotate Y | Rotate X | Move forward/backward |
| Move forward/backward | Rotate Y | Rotate X |

The second modifier has priority when both assigned actions are enabled.
The menu labels follow the selected actions. Changes take effect live.

- **Free-roam weapon switching**: scrolling the mouse wheel in free roam steps through equipped weapons (`gameplay_weapon_wheel = 1`, sensitivity `gameplay_weapon_sensitivity = 1`). Press E to confirm the chosen weapon; right click or pressing B cancels.
- **Quick-selection menus**: scrolling the mouse wheel browses items while a quick-selection menu (weapons, bows, shields, materials) is open (`gameplay_item_wheel = 1`, sensitivity `gameplay_item_sensitivity = 1`). A and D keys step left and right through the strip (`gameplay_item_ad = 1`).
- **Smooth map zoom**: the mouse wheel performs continuous inertial zooming on the Purah Pad map (`gameplay_map_zoom = 1`, sensitivity `gameplay_map_zoom_sensitivity = 1`). It works with `menu_cursor = 0` too: the map is still found then, and the mouse moves the map's cursor there as with the menu cursor on, while the game's other menus stay without the pointer.

All sub-toggles default to 1 and sensitivities to 1 (range 0.25 to 4.0), so enabling `gameplay_tweaks_enabled` activates the full suite immediately while allowing individual features to be toggled or tuned.

## Nintendo Switch 2 controllers

Windows does not present the Switch 2 Pro Controller as a gamepad SDL can open (the log says `input: no gamepad`), so it needs a bridge program that wakes it and creates a virtual gamepad. The DS4Windows fork by hbashton ([github.com/hbashton/DS4Windows](https://github.com/hbashton/DS4Windows), tested with release VIIPERRC4.5.1, a pre-release) with the output set to an emulated DualSense gives buttons, rumble, gyro and accelerometer, and reads the sticks through the controller's calibration (they rest at 0 to 2%). The game reads its virtual DualSense directly; the log then shows `input: gamepad "DualSense Wireless Controller" (rumble, gyro and accelerometer, ...)` and `input: motion sensors on`. Its release notes also list Joy-Con 2 support, which is untested here.

Bridges that output a ViGEm DualShock 4 (ProCon2Windows, connectyourpro as tested) give buttons and rumble but no motion: their virtual DualShock 4 reports a gyro and an accelerometer that never change, so motion aiming and Ultrahand rotation do nothing. Run one bridge at a time; Steam Input can also claim the controller, so close Steam if the bridge does not find it.

## Steam Input and motion

A game Steam starts sees Steam's virtual gamepad in place of the controllers Steam Input reads (Steam lists them in
`SDL_GAMECONTROLLER_IGNORE_DEVICES`), and that virtual pad has no gyro or accelerometer. With `motion = 1` the game
clears that list before SDL starts (`runtime/input.c`, `steam_reveal`): buttons and sticks still come from the virtual
pad, so Steam's remapping applies and nothing is read twice, and when the virtual pad has no sensors the motion thread
opens the real controller behind it for its gyro and accelerometer only (`motion_source`). The log then shows
`input: motion from "<name>" (<vendor>:<product>), the controller behind Steam Input's virtual gamepad`. This covers the
Steam Controller and the Steam Deck's built-in controls, which Steam Input cannot be turned off for, and other
controllers Steam Input wraps. A controller Steam hid serves for buttons only while no other gamepad is there (Steam's
virtual pad can come up after the game starts); once the virtual pad is up the buttons move to it. With `motion = 0`
at start Steam's list stays in force.

Steam switches the controller's gyro off while the game's layout binds nothing to it: the reports keep coming with the
gyro at zero, and the game's request for it (SDL renews it every 3 s) does not win. The layout needs a gyro binding,
for example Gyro Behavior "As Mouse"; its activation only decides when Steam itself moves the mouse, the game gets the
gyro all the time. Pick an activation the game does not need (a back button), or Steam's mouse turns the camera on top
of the game's own motion. Under Steam the Steam Controller's gyro arrives already corrected, so it keeps a bias of its
own (`gyro_bias_pad` ends in "(under Steam)").

On Linux the gamepad keeps playing while the game window has no focus (#971), except in a game Steam started: the
Steam overlay and Steam's menus (the Quick Access menu and the Steam menu in SteamOS's Game Mode) take the focus while
they are open, and the presses meant for them must not reach the game too (#1462). Steam sets `SteamGameId` for its
own games and for non-Steam shortcuts; the log then says `input: the gamepad pauses while the window has no focus`.
`pad_background = 1` keeps the gamepad playing behind other windows under Steam too, `0` pauses it everywhere
(`runtime/input.c`, `start`; read at start). On Windows the game window is not SDL's, and the gamepad always plays.

## Scripted input

`TOTK_INPUT_SCRIPT=<file>` replays presses. It is ORed with real input and still applies under `TOTK_INPUT=none`. One event per line, `#` starts a comment:

```
<frame> <button> [hold_frames]            # A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LCLICK RCLICK; hold defaults to 4
<frame> LSTICK|RSTICK <x> <y> [hold_frames]   # -32767..32767, up is +y
<frame> MOUSE <dx> <dy>                  # raw mouse counts, once; down is +dy
@<ms> <button> [hold_ms]                  # timed on the world clock
@<ms> LSTICK|RSTICK <x> <y> [hold_ms]
@<ms> MOUSE <dx> <dy>
<frame> WHEEL <notches>                   # scroll wheel notches, once; + away from the player
@<ms> WHEEL <notches>
<frame> QUICKSAVE|QUICKLOAD                # quicksave = 1's save or load key, once
@<ms> QUICKSAVE|QUICKLOAD
```

`<frame>` counts 60 Hz ticks of game time since the first frame (presented frames at 60 fps), so the title menus get the presses at the same game time at any frame limit. `@<ms>` counts milliseconds of the world clock: game time stepped since the end of the first save load (the player leaves the title screen's origin), so a timed event lands at the same game time at any frame rate and load length. A later load, such as fast travel, still shifts what follows. In a script with timed lines the untimed ones stop when the world clock starts, as the load ends at another tick in each run. Each event logs once (`input: script frame N tick T: A`). `tools/input/load_save.txt` loads the first save from the title screen.

`TOTK_INPUT_RECORD=<file>` (`input_record` in `totk.ini`, or Input recording in the menu) writes the merged input in the same format, one line per button press and per stick segment: events before the world clock starts by tick, the rest timed; a button or stick held when the world clock starts is cut there and goes on as a timed event. The file replays as a script, which makes it the repro to attach to a bug report.

In direct mouse mode the recording also writes one `MOUSE` event per consumed raw displacement. These are one-shot
displacements rather than held stick segments. They replay without cursor capture, including with `input = none`;
the game must be accepting camera input when the event reaches its camera update. Where the mouse acts as the right
stick (`mouse_mode = stick`, a camera with no direct hook, the game not accepting camera input), a script's `MOUSE`
lines move the right stick as the mouse would, also without cursor capture or with `input = none`, for 100 ms after
each line (`mouse_camera` in `runtime/input.c`); spread a turn over lines a frame apart, as a real mouse moves.

`TOTK_TEST_PAD=<lx>,<ly>,<rx>,<ry>` attaches an SDL virtual gamepad at start whose sticks rest at these raw SDL axis values (-32768 to 32767, down is +y), so a drifting or held stick can be tested without a controller: the values go through the gamepad mapping and `TOTK_STICK_DEADZONE` like a real pad's, and `TOTK_INPUT_RECORD` shows what reached the game.

## RenderDoc capture (blocked)

`TOTK_RDOC=<frame>[,...]` (`runtime/rdoc_win.c`; `TOTK_RDOC_DIR`, `TOTK_RDOC_DLL`) loads RenderDoc before `vkCreateInstance`. RenderDoc hides `VK_EXT_external_memory_host`, which guest pool imports need (`runtime/vk_memory.c`); without it the back end gives CPU-visible pools shadow storage, as on any GPU without the extension (`vk_no_host_import`). Captures on that path are untested.
