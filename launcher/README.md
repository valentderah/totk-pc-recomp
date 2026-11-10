# TotK Launcher (Windows)

A launcher and installer for TotK Recomp in a single `TotKLauncher.exe`. It:

- installs itself: opened from anywhere (Downloads, for example), it copies itself to
  `%LOCALAPPDATA%\Programs\TotK Recomp\TotKLauncher.exe` and restarts from there, without admin rights;
- installs the game from your files (base game, update, `prod.keys`, optional firmware) by running `python -m installer` from a
  setup package (`TotK-Setup-<version>-windows.zip` from the releases);
- downloads the package of the latest GitHub release when this PC has none, or an older one;
- opens as a launcher (Play / Update) from the Desktop and Start menu shortcuts;
- updates the installed folder to a new release: it keeps `romfs/`, `exefs/`, `profile/` (saves) and `totk.ini` and redoes only
  what changed (the installer's own update mode);
- rebuilds for code mods: it writes `build-cache\rebuild.txt` pointing to itself (`--rebuild`), so `totk.exe` opens the launcher
  instead of the tkinter window.

The launcher builds nothing by itself: every step is the installer's, and its messages come from the package's
`installer/text/*.json`.

## Build

Needs the .NET 8 SDK (`winget install Microsoft.DotNet.SDK.8`).

```powershell
cd launcher
.\build.ps1            # writes launcher\dist\TotKLauncher\TotKLauncher.exe (one self-contained file, no .NET install needed)
```

Or through GitHub Actions: the **Launcher** workflow uploads the `TotKLauncher-win-x64` artifact.

## Use

1. Open `TotKLauncher.exe` from wherever it is. It installs itself to `%LOCALAPPDATA%\Programs\TotK Recomp` and reopens from there;
   the downloaded file can then be deleted. Opening a newer `.exe` replaces the installed copy.
2. Pick the game files, the install folder and the language, and click **Install the game**. The setup package is downloaded to
   `%LOCALAPPDATA%\TotK Recomp\setups\<tag>` (several hundred MB; older ones are removed after a successful update).
3. Afterwards, open the game from the **TotK Recomp** shortcut: it shows **Update** when a new release is out.

The shortcuts and `rebuild.txt` point to the installed copy in `%LOCALAPPDATA%\Programs\TotK Recomp`.

Command line:

```
TotKLauncher.exe                     launcher for the last installed folder, else the install page
TotKLauncher.exe --launch FOLDER     launcher for that folder
TotKLauncher.exe --update FOLDER     update that folder to the latest release
TotKLauncher.exe --rebuild FOLDER    rebuild for mods, then start the game
```

## Configuration

`%LOCALAPPDATA%\TotK Recomp\launcher.json` keeps the chosen paths. The releases come from `valentderah/totk-pc-recomp`; to use
another repository (a fork, for example), set `"Repo": "owner/repo"` in that file or the `TOTK_LAUNCHER_REPO` environment variable.

Local setup package (no download): unpack `TotK-Setup-<version>-windows.zip` into a `setup` folder next to `TotKLauncher.exe`, or
point `TOTK_SETUP_DIR` at it.

## Licenses

See [NOTICE.md](NOTICE.md).
