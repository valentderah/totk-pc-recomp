# totk-config: the settings window

`totk-config.exe` (`totk-config` on Linux) sits next to `totk.exe` and edits `totk.ini` and named settings profiles
before the game starts. It shows the same settings as the in-game menu (F1): the menu's tabs, groups, names,
descriptions and choices, in the menu's language. Two tabs of its own list the keys the menu does not show (Expert)
and the keys of `totk.ini` this version of the game does not read (Other keys).

It is plain C: SDL3 (already beside the game) for the window and [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear)
(one MIT header, fetched at build time into `toolchain/nuklear/` and checked against a pinned hash) for the widgets. It links the runtime's own settings code instead of a
copy of it: `runtime/cfg.c` reads and writes `totk.ini` exactly as the game does, `runtime/ui_text.c` gives the menu's
texts, `runtime/input_map.c` the binding names. A setting added to the runtime and its menu shows up in the window at
the next build with nothing else to change, as long as it is written the way [Adding a setting](#adding-a-setting)
describes.

## Using it

Start `totk-config.exe` from the game folder. It edits the `totk.ini` there (the runtime reads `totk.ini` from the
folder of `totk.exe` and nowhere else); **Change** in the header picks another folder (the header shows the end of the
path, the whole path on hover), and `--dir <folder>` names one at start. A folder without the game's exe gets a
warning above the rows; `totk.ini` is written there anyway.

The left panel holds the profiles (below) and the tabs; the rows of the chosen tab, or of every tab while searching,
fill the rest; the bottom line has the status and Revert, Save and Save and launch. The colours and the font are the
in-game menu's, the icons are drawn shapes (`cfgr_icons.c`), and the window and its exe carry the game's icon.

- **Tabs.** Screen, Game, Sound, Controls, Amiibo, Menu, Advanced and Mods are the in-game menu's tabs with its rows,
  including the ones the menu shows only in some states (the DLSS rows, the gameplay tweaks). The gold number beside a
  tab counts the keys `totk.ini` sets there. **Expert** lists every other key of `runtime/cfg.c` `known[]`, grouped as
  `docs/settings.md` groups them, with its description from there or from `totk.ini.example` (in English).
  **Other keys** lists the keys of the file the runtime does not know and adds new ones.
- **Rows.** A row is a card: the name, its marks, a line with the key, its default and, for the keys the Steam Deck
  profile changes (`cfg.c` `DECK[]`), the value there, then the description; the value's control is on the right. On
  and off are a switch, up to four choices one segmented control, longer lists a drop-down, numbers a slider with the
  value beside it. A gold bar and *set* mark a key `totk.ini` sets; the reset button beside the control removes its
  line, back to the default. An orange dot marks a change not saved yet. *after restart* (orange) marks a key the game
  reads at start; a green flash marks one the in-game menu applies at once. A key a `TOTK_*` environment variable sets
  is greyed out and marked: the variable wins over the file.
- **Search** looks through every tab: names, keys and descriptions. **Only set ones** hides the keys `totk.ini` leaves
  at their defaults.
- **Save** writes the changed keys only, the way the in-game menu does (`cfg.c` `cfg_save_to`): the changed line gets
  the new value, a new key goes under its section, a reset key's line goes, and comments, unknown keys, the order and
  the line endings stay. **Save and launch** saves and starts the game. **Revert** drops the unsaved changes. Closing
  the window with unsaved changes asks first. When another program (the game's menu) changes `totk.ini` meanwhile,
  the window reads it again, or asks when it has unsaved changes of its own.
- **Language** (top right) is the menu language, `menu_lang` in `totk.ini`: the window and the game's menu follow it.

### Profiles

A profile is a named `totk.ini`: `profiles/<name>.ini` next to `totk.ini`, with `profiles/active.txt` naming the one
`totk.ini` holds now. Choosing another profile saves the window's changes, copies `totk.ini` into the active profile's
file and the chosen profile's file over `totk.ini`. **None** leaves `totk.ini` as it is and stops tracking a profile.
The profiles are listed at the top of the left panel, the active one gold; the pencil and the bin beside **New profile**
rename and delete the active one. **New profile** starts from one of these presets; the dialog lists what the chosen
one sets:

| Preset | What it sets on top of the current settings |
| --- | --- |
| Current settings | nothing: a copy of `totk.ini` |
| Defaults | every key removed: the game's and the port's defaults |
| Quality | `render_scale 100`, `dynamic_res 0`, `aniso 16`, `shadow_res 2048`, `shadow_scale 2`, `shadow_range 150`, `grass_range 150`, `contact_shadows 50` |
| Performance | `render_scale 75`, `dynamic_res 1`, `upscaler fsr`, `grass_range 100`, `contact_shadows 0`, `pcss 0`, `frame_queue 1`; `aniso`, `shadow_res`, `shadow_scale` back to their defaults |
| Potato mode | for GPUs with about 2 GB of video memory: `render_scale 67`, `fps_limit 30`, `grass_range 50`, and every extra effect and memory-hungry key back to its default |
| Steam Deck-like | the runtime's Steam Deck device profile (`cfg.c` `DECK[]`) |

The presets are in `runtime/configurator/cfgr_model.c`; every value is one `cfg.c` `metas[]` accepts.

### Updates (Windows)

The **Updates** tab, last in the left panel, shows the version in the folder and the latest release of the project
(`runtime/configurator/cfgr_update.c`):

- **Installed** is the version of the `totk.exe` that ran last (`logs/totk.log`, "version: TotK Recomp ..."), or, until
  the game runs after a setup, the release whose setup made the folder (`totk-install.json`). When the two differ, or
  `totk.exe` is newer than the setup's record, the folder holds a build of its own (*own build*), which an update
  replaces with the release's.
- **Latest release** asks GitHub only on **Check now**, or when the window opens if *Check for updates when the window
  opens* is on (kept in `%LOCALAPPDATA%\TotK Recomp\configurator.json`, not in `totk.ini`; a newer release marks the
  tab *new*). It shows the release's notes and a link to its page.
- **Update to vX.Y.Z** asks first, saves the window's changes, then downloads the release's
  `TotK-Setup-<version>-windows.zip` into `%LOCALAPPDATA%\TotK Recomp\updates\`, checks its size and SHA-256 against
  what GitHub lists for it (a mismatch deletes it), unpacks it with Windows' own `tar.exe` after refusing any entry
  outside its folder, checks that the release's `VERSION` names that release, and starts that release's setup on the
  folder (`python\pythonw.exe -m installer.gui --rebuild <folder>`, as the game's own rebuild does): saves, game files
  and `totk.ini` stay, `totk.exe` and the shaders are rebuilt, and the game starts when it is done. The window closes
  then. Never while the folder's `totk.exe` runs. Only this repository's release downloads are taken.

HTTPS goes through WinHTTP (Windows' TLS and certificate store) and SHA-256 through BCrypt, so the window needs no
library of its own for it. The releases' setups are Windows programs, so the tab is not shown on Linux.

### Command line

| Command | What it does |
| --- | --- |
| `totk-config [--dir DIR]` | the window |
| `--set KEY=VALUE` (repeatable) | sets keys and saves; an empty value removes the key; a key the runtime does not read is written with a note |
| `--profile NAME` | switches to a profile (`""`: none) |
| `--new-profile NAME [--from PRESET]` | creates a profile: `current`, `defaults`, `quality`, `performance`, `potato`, `deck` |
| `--list [TAB]` | every row: tab, key, the value in effect, `set` when `totk.ini` sets it, `env` when a variable does |
| `--check-update` | the version in the folder and the latest release (Windows) |
| `--download-update` | also downloads, checks and unpacks the release, without starting its setup (Windows) |
| `--screenshot FILE.bmp [--tab N] [--search TEXT] [--popup DIALOG]` | draws the window hidden, saves it and closes (tests, docs); `--popup` shows a dialog in it: `new`, `rename`, `delete`, `close`; `--tab 10` is Updates |

`TOTK_MENU_LANG` picks the language as in the game, and `TOTK_CONFIG_LOG=1` prints what `cfg.c` logs (unknown keys,
lines without `=`). For tests, `TOTK_CONFIG_UPDATE_API` names another GitHub API address (only `http://127.0.0.1:` or
`http://localhost:`), `TOTK_CONFIG_UPDATES_DIR` another download folder, and `TOTK_CONFIG_SHOT_CHECK=1` lets a
screenshot of the Updates tab check first.

## Building

`tools/build_game.py` builds `totk-config.exe` into its output folder after it links `totk.exe`, with the same clang
(a failure there is reported and does not stop the game build). On its own:

```
python tools/build_configurator.py [--out build/configurator]
```

The first build fetches Nuklear 4.13.4 into `toolchain/nuklear/` (its header is over the repository's 1 MiB file
limit); a build without network needs those three files put there by hand. The release builds it once (`tools/make_release.py`, `tools/make_release_linux.py`) into `bin/`, against the release's
SDL3 and, on Linux, its sysroot; the setup copies it next to `totk.exe` with the DLLs (`installer/steps.py`, compile
step). It is built for x86-64-v2, so it runs on every CPU the game runs on.

| File | What it holds |
| --- | --- |
| `runtime/configurator/cfgr_main.c` | the command line and the headless commands |
| `runtime/configurator/cfgr_model.c` | the game folder, `totk.ini` through `cfg.c`, profiles, presets, starting the game |
| `runtime/configurator/cfgr_window.c` | the window: fonts, colours, header, left panel, rows, dialogs |
| `runtime/configurator/cfgr_icons.c` | the icons, drawn with Nuklear's shapes |
| `runtime/configurator/cfgr_update.c` | the Updates tab's work: the GitHub check, the download, SHA-256, unpacking, the setup |
| `runtime/configurator/cfgr_nk.c`, `cfgr_nk.h` | Nuklear and its SDL3 renderer, compiled once |
| `runtime/configurator/cfgr_host.c` | the few runtime functions `cfg.c` and `ui_text.c` call (log, fatal error) |
| `tools/gen_configurator.py` | writes the settings table (`cfgr_rows.c` in the build folder) from the runtime's sources |
| `tools/build_configurator.py` | compiles and links the window; on Windows with `runtime/totk.rc` (the game's manifest and icon); the window's icon from `runtime/totk.ico` |
| `tests/configurator_check.py` | the table, the commands, the updates against a fake GitHub, and the window in every language |
| `runtime/ui_text.c` `UI_CFG_*` | the window's own words, in the menu's four languages |

## How the window knows the settings

`tools/gen_configurator.py` writes the table at build time. Labels, descriptions and choice names stay text ids, so the
window shows them in whatever menu language is chosen. It reads:

- `runtime/ui/menu.cpp`: the tab functions (`tab_screen` ... `tab_mods`) and the row helpers they call (`row_*`,
  `rows_*`, `mods_*`), in drawing order. That gives each menu key its tab, group, kind of row, label, description,
  slider range and step, and choice names.
- `runtime/ui/binds_page.cpp`'s rows through `runtime/input_map.c`: one binding row per action, `BIND_<ACTION>`.
- `runtime/ui/amiibo_page.cpp`: the keys it sets (folder rows).
- `runtime/cfg.c`: `known[]` (every key: the ones without a menu row go to Expert) and `metas[]`, whose defaults and
  accepted values the window reads through `cfg_meta` itself.
- `docs/settings.md` and `totk.ini.example`: the Expert descriptions; `rt_knob("TOTK_<KEY>", "<default>")` calls: the
  defaults of keys outside `metas[]`.

Three choice lists are made at run time in `cfgr_window.c` `items_of`: the game languages (`ui_text_game_langs`),
the audio devices (SDL, by the names the game's mixer uses) and the GPU index.

## Adding a setting

A setting the window shows correctly is written like this. `tests/configurator_check.py` fails when a menu row is
written another way.

1. **`runtime/cfg.c`.** The name in `known[]`, in alphabetical order. A setting the menu shows also gets a `metas[]`
   row `{"KEY", "default", "values", LIVE | RESTART}`: `values` is a choice list `"a|b|c"`, a range `"lo..hi"`, or
   `NULL` for free text; an empty default means the consumer picks per machine (the window shows Automatic). A
   `sections[]` row names the `totk.ini` section a new line goes into.
2. **The description.** A player setting in `totk.ini.example`, the rest in `docs/settings.md`
   (`tests/settings_doc_check.py`). Without a menu row, this is the text the Expert tab shows.
3. **The menu row**, if the menu shows it: a call in a tab function of `runtime/ui/menu.cpp`, or in a `row_*`,
   `rows_*` or `mods_*` function of that file a tab calls, whose first two arguments are the key as a literal
   `"TOTK_<KEY>"` and a `UI_*` label id:
   - `opt_toggle("TOTK_KEY", UI_LABEL, UI_LABEL_DESC[, true])` (`true`: the row shows *on* while the key is 0);
   - `opt_choice("TOTK_KEY", UI_LABEL, names, n, UI_LABEL_DESC)`, with `names` a `UiTextId` array initialised in
     the same function, one name per `metas[]` value, Automatic first when the default is empty;
   - `opt_choice_list("TOTK_KEY", UI_LABEL, values, names, n, UI_LABEL_DESC)`, with `values` an array of string
     literals in the same function and `names` an array of literals or `menu_text(UI_...)`, or `names[i] =
     menu_text(UI_...)` assignments; names made at run time show as their values;
   - `opt_slider("TOTK_KEY", UI_LABEL, lo, hi, UI_LABEL_DESC, step, "format"[, note, "TOTK_FALLBACK"])` with number
     literals;
   - `binds_key_row`, `row_threads` and `row_set_or_not` as the existing rows call them.

   `opt_group(UI_...)` before rows starts a group heading. A key passed through a variable rather than as a literal is
   invisible to the window: it then lands on Expert. Another call with a literal key and a label id stops the check.
4. **The texts** in `runtime/ui_text.h` and `runtime/ui_text.c`, in all four menu languages.
5. **Check:** `python tests/configurator_check.py` (it builds the window and runs it), and `python
   tests/settings_doc_check.py`.

A new kind of row (a widget the four calls above do not cover) needs the generator (`ROW_CALLS`) and the window
(`cfgr_window.c` `value_widget`) taught about it; a choice list made at run time needs a case in `items_of`.
