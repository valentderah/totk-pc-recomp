# Mods

TotK Recomp loads the mods players make for the Switch emulators (Ryujinx, yuzu and its forks): texture and model
packs, gameplay and text changes from GameBanana and similar sites, TKMM's merged output, code patches (`.ips`,
`.pchtxt`) and Atmosphère cheat files. No mod or cheat file ships with the port.

## Install a mod

1. Open the settings menu (F1, Esc, or Minus and Plus together on a controller), go to the Mods tab and choose
   **Open mods folder**. That is the `mods` folder next to `totk.exe`; the menu creates it when it is missing.
2. Make one folder per mod in it and unpack the mod's archive into that folder. The mod's own folders (`romfs`,
   `exefs`, `cheats`) must sit directly inside the mod's folder:

   ```
   mods\
     Better Grass\
       romfs\
         ...
     Fast Travel Anywhere\
       exefs\
         patch.pchtxt
     My cheats\
       cheats\
         277178B7DBA1B6D4.txt
   ```

   An archive laid out for a Switch SD card or an emulator works as it is. Inside the mod's folder the port also finds
   `atmosphere\contents\0100F2C0115B6000\romfs`, `contents\0100F2C0115B6000\romfs` and `0100F2C0115B6000\romfs`, the
   same places for `exefs` and `cheats`, and `atmosphere\exefs_patches\<name>\` for code patches.
3. Start the game. A mod that patches the game's code makes the game rebuild itself first (see Code patches below).
4. Open the Mods tab to check the mod. Each row says how many game files the mod replaces and adds, how many cheats it
   brings, and why a part of it is not used.

## Order and switches

The Mods tab lists every folder in `mods`. Select a mod and press A or Enter to switch it on or off. The top face button
or Ctrl+Up moves it up, the left face button or Ctrl+Down moves it down. When two mods change the same file, the
higher one wins; the lower mod's row says how many of its files the higher one covers. Changes apply after a restart.

The menu saves the order and the switches in `totk.ini`, under `[mods]`, by folder name: `mods_order` and
`mods_disabled`. A folder the list does not name yet, such as a new or renamed one, goes below the named ones.

### Mods that change the same files

Two mods that edit the same game file do not combine: the higher one wins the whole file. Merge such mods with
[TKMM](https://github.com/TKMM-Team/Tkmm) (TotK Mod Manager) and set its export location to an empty folder inside
`mods`, for example `mods\TKMM`. The exported folder then counts as one mod here. Mods packaged for TKMM only
(`.tkcl` files) go the same way.

The port merges the resource size table on its own: at every start with mods on, it combines the tables the mods ship
and raises the entries for files a mod made larger or added. A separate RESTBL mod is not needed.

## Code patches (ExeFS)

A mod with `.ips` or `.pchtxt` files patches the game's code. A patch is for one game update: an `.ips` file is named
after the build id of the update's code, a `.pchtxt` file names it on its `@nsobid` line. The game's code is compiled
ahead of time, so these patches are built into `totk.exe`:

1. The first start after such a mod is added closes the game and opens the setup's rebuild window. The rebuild takes
   a few minutes (the shaders stay prepared), then the game starts again. Saves and settings are kept.
2. Until the rebuild succeeds, the whole mod stays off, its other files included.
3. After the rebuild the mod switches on and off from the Mods tab like any other, without another rebuild.

The rebuild needs the setup folder the game was installed from. When that folder was moved or deleted, run
`TotK-Setup.exe` again on the same install folder: it does the same job. `mods_rebuild = off` under `[mods]` in
`totk.ini` turns the automatic rebuild off.

A code patch over one of the port's own patches (the 60 FPS patch, the resolution patch and the other entries under
**Built into the port**) turns that built-in patch off while the mod is on. The mod's row names it.

## Cheats

A cheat file is part of a mod: `mods\<mod>\cheats\<build id>.txt`, switched cheat by cheat on the Cheats tab. Cheats
that change the game's code need the same rebuild as code patches. [cheats.md](cheats.md) explains the file names for
each game update, the rebuild and which cheats cannot run.

## Renaming a mod folder

The port keys a mod by its folder name. A renamed folder counts as a new mod:

- it moves below the mods the list names, and comes back switched on;
- its cheats come back switched off;
- its code patches and code cheats are built again, so the next start runs the rebuild.

Rename mod folders before you set them up in the menu, not after.

## What does not work

| On the Mods tab | Why | What to do |
|---|---|---|
| This folder has no romfs, exefs or cheats files. | The archive was unpacked one level too deep (`mods\Mod\Mod\romfs`), or it holds no game files. | Move the folders up so `romfs` sits directly in the mod's folder. |
| ... files are made for another game version and are left out. | The mod's files carry another update's version in their names (`.Product.<number>.`). | Download the mod's release for your game update. |
| Its code patches (exefs) are made for another game version. | The patch files name another update's build id. | Same: get the release for your update. |
| This mod brings its own game code (a subsdk plugin or a replaced main). | Mods such as UltraCam ship compiled Switch code. The port cannot load it. | No workaround; the whole mod stays off. |
| This mod patches the game code (exefs) in a way the port cannot apply. | The patch writes the game's data, not its code, or changes code in a form the port cannot build. | No workaround; the reason follows on the row. |
| The folder name contains \|. | The mod list uses `\|` between names. | Rename the folder. |

A mod never stops the game from starting. Whatever the port cannot use is left out and shown on the Mods tab.

## When something looks wrong

1. Switch the newest mod off on the Mods tab and restart. If the problem goes away, check that the mod is made for your
   game update and does not fight another mod over the same files.
2. `logs\totk.log` next to `totk.exe` names every mod at the start (lines starting with `mods:`), the cheats
   (`cheats:`) and the resource size table (`RomFS: resource size table:`). Open log folder on the Advanced tab of
   the menu shows the logs.
3. When reporting a problem, attach `totk.log` and list your mods with the links you downloaded them from.
