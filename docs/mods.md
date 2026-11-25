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
or Ctrl+Up moves it up, the left face button or Ctrl+Down moves it down. Changes apply after a restart.

The order decides every conflict, as in PC mod managers: when two mods change the same game file, the same bytes of the
game's data or the same instruction of the game's code, the higher one wins. Moving a mod and restarting is enough; the
game is not rebuilt for it. Each row lists what the mod wins and loses, by mod: "3 of its files come from X, which is
higher in the list", "It overrides 2 code patch sites of Y, which is lower in the list", and the same for game data.
`logs\totk.log` has the same as `mods: X: overrides Y at 2 code sites` and `mods: Y: 2 code sites overridden by X`, and
one line per contested code site.

The menu saves the order and the switches in `totk.ini`, under `[mods]`, by folder name: `mods_order`,
`mods_disabled` and `mods_no_code`. A folder the list does not name yet, such as a new or renamed one, goes below the
named ones.

### Mods with code the port cannot run

Some mods bring code the port cannot run: their own Switch code (a replaced main, or a subsdk plugin the rebuild
refused, as it refuses UltraCam's) or a code patch in a form the port cannot apply. Such a mod stays off as a whole, because its other files may expect
that code. Under its row the Mods tab then shows **Use <mod> without its code** (off by default). Switched on, the code
the port cannot run is left out and the rest of the mod loads after a restart: its game files, cheats and the code
patches that work. The mod's row says which part is left out and why. If the game misbehaves with it, switch it off
again. `mods_without_code = 1` under `[mods]` holds the switch on for every mod.

### Mods that change the same files

Two mods that edit the same game file do not combine: the higher one wins the whole file. Merge such mods with
[TKMM](https://github.com/TKMM-Team/Tkmm) (TotK Mod Manager) and set its export location to an empty folder inside
`mods`, for example `mods\TKMM`. The exported folder then counts as one mod here. Mods packaged for TKMM only
(`.tkcl` files) go the same way.

The port merges the resource size table on its own: at every start with mods on, it combines the tables the mods ship
and raises the entries for files a mod made larger or added, the files inside a mod's packs (`Pack\**\*.pack.zs`)
included. A separate RESTBL mod is not needed. The table tells the game how much memory each file needs: a file that
outgrew its entry crashes the game when it loads, which can be long after the start, when the game first needs it. The
log line `RomFS: resource size table: ...` says how many entries were raised and names the first files inside packs.

## Code patches (ExeFS)

A mod with `.ips` or `.pchtxt` files patches the game's code. A patch is for one game update: an `.ips` file is named
after the build id of the update's code, a `.pchtxt` file names it on its `@nsobid` line. The game's code is compiled
ahead of time, so these patches are built into `totk.exe`:

1. The first start after such a mod is added closes the game and opens the setup's rebuild window. The rebuild takes
   a few minutes (the shaders stay prepared), then the game starts again. Saves and settings are kept.
2. Until the rebuild succeeds, the whole mod stays off, its other files included.
3. After the rebuild the mod switches on and off from the Mods tab like any other, without another rebuild.

The rebuild runs from `build-kit` in the game folder, which the setup put there: the setup folder can be deleted
and the game folder moved. After a move, a game with a code plugin mod rebuilds once at its next start. A game
installed by an older setup (no `build-kit` in its folder) still rebuilds from that setup folder until a newer
setup runs once on it.
`mods_rebuild = off` under `[mods]` in `totk.ini` turns the automatic rebuild off.

A code patch over one of the port's own patches (the 60 FPS patch, the resolution patch and the other entries under
**Built into the port**) turns that built-in patch off while the mod is on. The mod's row names it.

When two mods patch the same instruction, the code of the higher one runs there; the other mod's patches run
everywhere else. The rebuild builds every mod's patch in, so a new order needs only a restart. Two patch files of one
mod that patch the same instruction: the one later by path inside the mod wins, as emulators apply a mod's files in
name order, each over the one before. A cheat that changes the same instruction runs instead of both while it is on.

A patch file may also change the game's data (constants and tables in its `.rodata` and `.data`). Those bytes are
stored into the loaded game at start, before it runs. When two mods write the same bytes, the higher one wins,
as with files.

## Code plugins (exefs subsdk)

Some mods ship their own Switch code as an exlaunch plugin: a file `exefs\subsdk<N>` (The Second Quest's MSFix, Full
Inventory Expansion's sort fixer, Quick Weapon Durability). On the Switch the plugin starts with the game and
redirects some of the game's instructions into its own code. The port does the same with the plugin's code compiled
ahead of time, after the design in [#845](https://github.com/valentderah/totk-pc-recomp/issues/845) by itaskaev2:

1. The rebuild (the same one as for code patches above) compiles the plugins with the game, then starts the game's
   code in a service mode that runs only one plugin's start-up (`totk.exe --plugin-init`), twice per plugin, and
   records which instructions it redirects. Each redirect becomes a code site of the mod, built into `totk.exe`. Each
   plugin is judged on its own: one that is refused, or whose start-up crashes or never returns, stays off with its
   reason, and the other plugins are built in.
2. At every start each plugin's start-up runs again, before the game's own start-up code, in the same setting: it
   sees the game's own instructions, as in its service run. Its redirects are compared with the record of the
   rebuild. A value it writes into the game's data that redirects nothing (a size, a counter) only has to land at
   the recorded place, since the game may hold another value there at this start, for example a memory pool that
   grows with `render_scale`. When they match, the mod's code sites go on. When they do not, the plugin stays off for
   this run, the mod's row says why, and the next start runs the rebuild, which runs the plugin's service run again.
3. The plugin keeps what its start-up made (its memory, its tables), so its code works as on the Switch. The start
   costs about 50 ms per plugin; the game runs no differently without a plugin.
4. Removing a plugin mod from the mods folder starts the rebuild at the next start, which builds the game without
   it (half a minute on 12 cores with the lift cache). A plugin mod switched off on the Mods tab only stays off.

The Mods tab order decides between a plugin and another mod's patch on the same instruction, as for code patches. A
plugin site over one of the port's own patches turns that built-in patch off while the mod is on.

The rebuild refuses a plugin when its start-up or the code its redirects reach needs what the port does not give it:
system calls other than the few its start-up needs (memory queries and maps, the heap size, the clock, debug output),
or a library call the game itself never makes and a reviewed list does not hold (memory and string functions, the heap,
text formatting, mutexes, the clock, the system language, the SD card's files), such as network sockets or the module
loader. A library call the game makes (threads, input, time, its files) runs on the path the game runs on. A call to a
function the plugin itself holds counts as its own code, even when it goes through the plugin's import table (C++
library templates such as `std::vector` built into it). A start-up
still gets only the system calls above, so it cannot start a thread or open a service; a redirect that is not one of exlaunch's hook shapes
(inline hook, replace hook, call-site rewrite, a branch into its code from inside a game function, a pointer in the
game's data swapped for one into its code); a write into the game's read-only data or into another module; two runs of
its start-up that differ in code; or a start-up that crashes the service run or does not return within two minutes.
The row then says **Its plugin ... cannot run in the port** with the reason, and the mod stays off; **Use <mod> without
its code** uses the rest of it, including its other plugins that were built in. UltraCam stays refused: it hooks the graphics API,
reads input and opens sockets.

When a start-up calls into the middle of a game function, the rebuild lifts the game again with that address as a
function start and reruns the start-up, up to four times. A plugin that reads or writes files on the SD card gets
the folder `sdcard` in the game's profile folder in its place.

`plugin_trace = 1` in `totk.ini` logs the first call into each of a plugin's hooks (`plugins: hook entry ... reached`).

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

## HD texture packs

Texture packs made for the emulators replace the game's textures with larger ones, so the heap the world's textures
stream into must grow. Such a pack grows it in its own `System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs`, sized for
the emulators' 8 GB memory option. The port reads that file at start: the texture heap and the game's memory pool grow
by the pack's amount, and the game gets the port's own heap table with that growth (the log says
`resolution: mod <name> ships a HeapDef with TextureHeap ...`). Nothing needs to be set. For a pack without such a file
that still loops the panic Blood Moon, set `tex_heap_extra_mb` under `[mods]` in `totk.ini` (0 to 4096 MiB).

Tested: **HDPacks.com Tears of the Kingdom 4K Texture Pack v1.0** (`4kTextures`, 10,407 textures, 6.5 GB) on game 1.4.3
and 1.2.1, RTX 4080 SUPER: it loads into the game from a save and runs at the same frame rate, with about 3.3 GB more of
textures in memory (4.5 GB of game memory pools against 1.2 GB without it). Other packs are not tested and come with no
guarantee. Such a pack needs a lot of video memory; on a GPU with less than 8 GB, `astc_bc = 1` under `[performance]`
keeps its textures at a quarter of the size.

## What does not work

| On the Mods tab | Why | What to do |
|---|---|---|
| This folder has no romfs, exefs or cheats files. | The archive was unpacked one level too deep (`mods\Mod\Mod\romfs`), or it holds no game files. | Move the folders up so `romfs` sits directly in the mod's folder. |
| ... files carry another game version in their names. | The files are named for another update (`.Product.<number>.`) and the game's own files have no such name. They are still served under those names, as on a console, because the game sometimes asks for one; another update's resource size table is the exception and only lends its entries. | Nothing if the mod works. If it does nothing, download its release for your game update. |
| Its code patches (exefs) are made for another game version. | The patch files name another update's build id. | Same: get the release for your update. |
| This mod brings its own game code (a subsdk plugin or a replaced main). | The mod replaces the game's main code, which the port cannot load. | The whole mod stays off. **Use <mod> without its code** under its row uses the rest of it, which may expect the missing code (see Mods with code the port cannot run). |
| Its plugin ... cannot run in the port: ... | The rebuild refused the mod's code plugin; the reason follows (see Code plugins). | The whole mod stays off. **Use <mod> without its code** under its row uses the rest of it. |
| Its plugin stays off this time: it started differently from the build. | The plugin redirected other instructions at this start than in the rebuild, for example after the mod was updated in place. | Nothing: the next start runs the rebuild. |
| This mod patches the game code (exefs) in a way the port cannot apply. | The patch changes code in a form the port cannot build, or bytes the port cannot change at start. The reason follows on the row. | The whole mod stays off. **Use <mod> without its code** under its row leaves that patch file out and uses the rest of the mod. |
| The folder name contains \|. | The mod list uses `\|` between names. | Rename the folder. |

A mod never stops the game from starting. Whatever the port cannot use is left out and shown on the Mods tab.

## When something looks wrong

1. Switch the newest mod off on the Mods tab and restart. If the problem goes away, check that the mod is made for your
   game update and does not fight another mod over the same files.
2. `logs\totk.log` next to `totk.exe` names every mod at the start (lines starting with `mods:`), the cheats
   (`cheats:`) and the resource size table (`RomFS: resource size table:`). Open log folder on the Advanced tab of
   the menu shows the logs.
3. When reporting a problem, attach `totk.log` and list your mods with the links you downloaded them from.
