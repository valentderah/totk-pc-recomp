# Game launcher

`launcher.exe` shows a card for each game this repository builds, *Breath of the Wild* on the left and *Tears of the
Kingdom* on the right (the order they came out in), and starts the one you choose. A game it finds no build of stays
grey and cannot be started: its card offers *Install*, which opens the setup window for that game
([installer.md](installer.md), "Breath of the Wild"), and *Locate the game...* for one installed elsewhere. When the
setup window closes, the launcher looks for the games again and the installed one gets *Play*. The launcher is Dear
ImGui over SDL3's 2D renderer, the toolkit of the in-game menu, with the menu's font (`launcher/main.cpp`).

Each game has its own executable, built from that game's files: `botw.exe` and `totk.exe`. The launcher only starts
them; it holds no game code.

## Building

```powershell
.venv\Scripts\python tools\build_launcher.py
build\launcher\launcher.exe
```

`build_launcher.py` compiles `launcher/` and Dear ImGui with MSYS2 UCRT64 clang and writes `build\launcher\launcher.exe`
with `SDL3.dll` and the `libiconv-2.dll` it imports next to it (SDL3 links dynamically, as for `totk.exe`: MSYS2's static
`libSDL3.a` would pull LGPL libiconv into the exe). The launcher holds no game data, so one build serves every game and
version. `tools/make_release.py` builds it with the release's llvm-mingw (`--toolchain`, `--deps`) into the release
folder's root, next to a copy of `bin/SDL3.dll`.

## Where it looks for the games

- Its own folder, each folder in it, and each folder next to it. From `build\launcher` that finds `build\v143`,
  `build\botw190` and the other build folders; in a release folder it finds the `BotW` and `TotK` folders the setup
  installs into when started from the launcher, and a `launcher.exe` copied into an install folder finds the game there.
- The executables saved in `launcher.ini`.

A folder counts when it holds `botw.exe` or `totk.exe` and `romfs-index.bin`. Which game and version it is:

1. `game.ini` next to the executable, which `tools/build_game.py` writes and the setup copies (`game = botw`,
   `version = 1.9.0`, `build = botw190`).
2. Else the folder's name, for builds made before `game.ini`: `v<digits>` is *Tears of the Kingdom* (`v143` is 1.4.3,
   `v0` 1.0.0), `botw<digits>` *Breath of the Wild*.
3. Else `totk-install.json`, which holds the game (`"game"`; none: *Tears of the Kingdom*, an install made before
   *Breath of the Wild* support) and the version.

A folder's other executable is a leftover and does not count while the game's own is there: a *Breath of the Wild*
build made before the rename keeps its `totk.exe` next to the new `botw.exe`.

When a game has several builds, the newest version comes first (an install folder before a build folder of the same
version), and the card gets a list to pick another one. The launcher remembers the pick.

## launcher.ini

Next to `launcher.exe`, written by the launcher and by the setup window it starts:

```ini
last = botw
lang = ru
botw = D:\Games\TotK-Recomp\BotW\botw.exe
totk = C:\Games\TOTK\totk.exe
```

`last` is the game started last, which is selected at the next start. `lang` is the window language picked in the
window's top right corner: English, Russian, Brazilian Portuguese or Simplified Chinese, the installer window's
languages; without it the launcher takes the system's as the installer does, and it passes its language to the setup
window it opens (`--lang`). For Chinese it merges a system font (Microsoft YaHei, SimSun or SimHei on Windows, Noto
Sans CJK on Linux) with the glyphs its texts use, as the in-game menu does. A game's line is the build it was started with,
picked in the list, chosen with *Locate the game...* or *Other folder...*, or the one the setup window installed. A file
picked there that has no game data next to it is refused. A file with game data but no `game.ini` counts as the game of
the card it was picked on.

## Installing from the launcher

*Install* (or Enter on the selected grey card) runs the setup window with `--game`, `--dest` set to a folder next to the
launcher (`BotW`, `TotK`; the window lets you change it) and `--launcher-ini`. The card says *Installing...*, and the
other card's *Install* waits until the window closes. The setup is the release folder the launcher sits in (`python/`
and `installer/` next to it); in a repository checkout it is the checkout above the launcher, run by the Python in
`TOTK_SETUP_PYTHON`, else `.venv-py314` (it needs tkinter and the packages of `tools/requirements-installer.txt`).

## The pictures

A card shows `icon.jpg` from next to the executable: the game's own icon, a 256x256 JPEG from the Control NCA of its
package. The setup writes it into the install folder; for a build, `tools/extract.py` writes
`build\extracted\<ver>\icon.jpg` after each update and `build_game.py` copies it next to the exe. For an extraction
made before that:

```powershell
.venv-py314\Scripts\python tools\game_icon.py --package <base game or update .nsp/.nsz/.xci> --keys <prod.keys> --out build\<ver>\icon.jpg
```

The Control NCA is a plain NCA in an NSZ too, so this reads NSZ packages directly. The icon is game data and stays in
`build\` or the install folder. A build without an icon shows another build's icon of the same game, else the game's
colours and name. The selected game's icon, blurred, also tints the window behind the cards.

## Use

| Input | Action |
|---|---|
| mouse | point at a card to select it, click its picture or *Play* to start it |
| Left and Right, A and D, D-pad, left stick | choose a card |
| Tab | the other card |
| Enter, Space, gamepad south button, Start | start the selected game, or install it |
| Esc, gamepad Back | quit |

*Play* starts the executable in its own folder (as a double click does) and closes the launcher. A game that cannot
start leaves the launcher open with the error on its card.

Command line: `--list` prints the builds found and exits; `--lang en|ru|pt-BR|zh-Hans` picks the language for this
run; `--select totk|botw` selects a card at start; `--shot FILE.png` draws the window, saves it and
exits; `--size WxH` sets the window size in points. The window is a GUI program, so `--list` prints only when its output
is redirected or piped.

## Not done yet

- No shortcut or uninstall entry: the launcher is a file in the release folder.
- The setup window keeps its own look (Tk); setup screens drawn in the launcher would match it.
- Linux: SDL3, its process start and file dialog are portable and `build_launcher.py` links the system's libSDL3 there,
  but the Linux build has not been tried.
