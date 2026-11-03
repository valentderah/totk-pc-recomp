# Game launcher

`launcher.exe` shows a card for each game this repository builds, *Tears of the Kingdom* and *Breath of the Wild*, and
starts the one you choose. A game it finds no build of stays grey and cannot be started; its card offers to locate the
game's `totk.exe` instead. The launcher is Dear ImGui over SDL3's 2D renderer, the toolkit of the in-game menu, with the
menu's font (`launcher/main.cpp`).

It is for developers so far: the setup does not install it and the release does not ship it.

## Building

```powershell
.venv\Scripts\python tools\build_launcher.py
build\launcher\launcher.exe
```

`build_launcher.py` compiles `launcher/` and Dear ImGui with MSYS2 UCRT64 clang and writes `build\launcher\launcher.exe`
with `SDL3.dll` and the `libiconv-2.dll` it imports next to it (SDL3 links dynamically, as for `totk.exe`: MSYS2's static
`libSDL3.a` would pull LGPL libiconv into the exe). The launcher holds no game data, so one build serves every game and
version.

## Where it looks for the games

- Its own folder, each folder in it, and each folder next to it. From `build\launcher` that finds `build\v143`,
  `build\botw190` and the other build folders; a `launcher.exe` copied into an install folder finds the `totk.exe` there.
- The executables saved in `launcher.ini`.

A folder counts when it holds `totk.exe` and `romfs-index.bin`. Which game and version it is:

1. `game.ini` next to `totk.exe`, which `tools/build_game.py` writes (`game = botw`, `version = 1.9.0`, `build = botw190`).
2. Else the folder's name, for builds made before `game.ini`: `v<digits>` is *Tears of the Kingdom* (`v143` is 1.4.3,
   `v0` 1.0.0), `botw<digits>` *Breath of the Wild*.
3. Else `totk-install.json`: the setup installs *Tears of the Kingdom*, and the file holds its version.

When a game has several builds, the newest version comes first (an install folder before a build folder of the same
version), and the card gets a list to pick another one. The launcher remembers the pick.

## launcher.ini

Next to `launcher.exe`, written by the launcher:

```ini
last = botw
totk = C:\Games\TOTK\totk.exe
botw = C:\src\totk-recomp\build\botw190\totk.exe
```

`last` is the game started last, which is selected at the next start. A game's line is the build it was started with,
picked in the list, or chosen with *Locate the game...* or *Other folder...*. A file picked there that has no game data
next to it is refused. A file with game data but no `game.ini` counts as the game of the card it was picked on.

## The pictures

A card shows `icon.jpg` from next to `totk.exe`: the game's own icon, a 256x256 JPEG from the Control NCA of its
package. `tools/extract.py` writes `build\extracted\<ver>\icon.jpg` after each update, and `build_game.py` copies it next
to the exe. For an extraction made before that:

```powershell
.venv-py314\Scripts\python tools\game_icon.py --package <base game or update .nsp/.nsz/.xci> --keys <prod.keys> --out build\<ver>\icon.jpg
```

The Control NCA is a plain NCA in an NSZ too, so this reads NSZ packages directly. The icon is game data and stays in
`build\`. A build without an icon shows another build's icon of the same game, else the game's colours and name. The
selected game's icon, blurred, also tints the window behind the cards.

## Use

| Input | Action |
|---|---|
| mouse | point at a card to select it, click its picture or *Play* to start it |
| Left and Right, A and D, D-pad, left stick | choose a card |
| Tab | the other card |
| Enter, Space, gamepad south button, Start | start the selected game |
| Esc, gamepad Back | quit |

*Play* starts `totk.exe` in its own folder (as a double click does) and closes the launcher. A game that cannot start
leaves the launcher open with the error on its card.

Command line: `--list` prints the builds found and exits; `--lang en|ru` picks the language (default: the system's,
Russian or English); `--select totk|botw` selects a card at start; `--shot FILE.png` draws the window, saves it and
exits; `--size WxH` sets the window size in points. The window is a GUI program, so `--list` prints only when its output
is redirected or piped.

## Not done yet

- The setup and the release: no install, shortcut or uninstall entry for the launcher.
- Languages other than English and Russian (they get English): the installer's Brazilian Portuguese and Simplified
  Chinese need the catalog and, for Chinese, a system font as the in-game menu merges one.
- Linux: SDL3, its process start and file dialog are portable and `build_launcher.py` links the system's libSDL3 there,
  but the Linux build has not been tried.
