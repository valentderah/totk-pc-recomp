# Cheats

The port reads Atmosphère cheat files, the format Switch homebrew and the emulators use. It ships no cheat files: a cheat file goes into a mod in the `mods` folder next to `totk.exe`, like any other mod ([tester-guide.md](tester-guide.md), "Mods"). Open the menu (F1) and switch the cheats on the Cheats tab. The switches are saved under `[cheats]` in `totk.ini`.

## Adding a cheat file

1. Find the cheat file for your update. Cheat sites and collections name it after the first 8 bytes of the game's build id:

   | Update | File |
   |---|---|
   | 1.2.1 | `9B4E43650501A4D4.txt` |
   | 1.4.3 | `277178B7DBA1B6D4.txt` |

   The Cheats tab names the file the installed game reads when no mod holds one, and `logs\totk.log` says `cheats: no enabled mod holds cheats/<id>.txt`.
2. Put it in the `cheats` folder of a mod: `mods\My cheats\cheats\277178B7DBA1B6D4.txt`. A download laid out for the Switch's SD card or an emulator works as it is: unpack it into a folder of its own under `mods`, and the port finds `atmosphere\contents\0100F2C0115B6000\cheats`, `contents\0100F2C0115B6000\cheats` or `0100F2C0115B6000\cheats` inside it. The file name's case does not matter.
3. Start the game. Cheats that write the game's memory work now.
4. Cheats that change the game's code need the game rebuilt with them. When the game starts and finds such cheats it did not build, it opens the setup's rebuild window, which builds them in (a few minutes; the shaders stay prepared) and starts the game again. Until then they are greyed out with "after the rebuild". When the rebuild fails or you cancel it, the game starts without them and does not try again for the same cheats; "Rebuild the game now" on the Cheats tab tries again. `mods_rebuild = off` under `[mods]` in `totk.ini` turns the automatic rebuild off; running `TotK-Setup.exe` on the same folder does the same job by hand. Saves and settings are kept.
5. Switch the cheats on the Cheats tab of the menu.

The Cheats tab lists the cheats grouped by mod, in the order of the Mods tab. A mod switched off on the Mods tab, or one the port cannot use, contributes no cheats; the change applies at the next start. The Mods tab says how many cheats each mod brought.

## The file format

Standard Atmosphère: a `[name]` line starts a cheat, `{name}` a master cheat that is always on, and the cheat's opcodes follow as 8-digit hex words. A name with no opcodes is shown as a heading. The file's title line (the one with `TID=`) is not shown.

```
[The Legend of Zelda: Tears of the Kingdom v1.4.3 TID=0100F2C0115B6000 BID=277178B7DBA1B6D4]

[Player]
[A code cheat]
040A0000 XXXXXXXX YYYYYYYY

[A data cheat (Hold ZL+X)]
80000104
580F0000 XXXXXXXX
640F0000 00000000 YYYYYYYY
20000000
```

The words above are placeholders: `XXXXXXXX` an offset, `YYYYYYYY` a value.

Each cheat's switch is `cheat_<hash> = 1`, where the hash is FNV-1a 32 of `<mod folder name>/<cheat name>`. A name repeated in one file gets `#2`, `#3` and so on. Renaming a mod's folder resets its cheats' switches to off.

## How cheats run

The game's code is compiled ahead of time, so a cheat cannot write new instructions into it while the game runs. At setup, `tools/cheats.py` runs every cheat of a file the way Atmosphère's dmnt would and sorts it by what it writes; in game, `runtime/cheats.c` switches the result:

| Kind | What the cheat does | How the port runs it |
|---|---|---|
| code | Writes into main's code, directly (opcode 0) or through registers (`400B0000 ... ; 640F01B0 ...`) | Built into the game at setup as a patch set (`recomp/patches.py`); switches at once. |
| key | Code writes that apply only while buttons are held, the original words otherwise | A patch set that is on while the cheat is on and the buttons are held. |
| data | Writes data only: heap, main's data, pointer chains, button conditions | Runs in a cheat VM after dmnt's every presented frame while on. |
| master | `{name}` | Always on: its patch set, its VM run. |
| built in | The file's setup code: writes only code caves (below) and loads registers for the other cheats | Nothing to switch: its code caves are built into the game. |
| restore | Writes back only the game's original instructions | Not needed: switching a cheat off restores them. Greyed out. |

Code caves: a cheat may write its own code into the zero padding after main's code and branch there from the game. The setup builds every cave the cheats branch to into the game, as functions of their own; what a cheat writes there that is not code (a value or a table a cave reads) the VM writes. A cheat that writes both, like a speed multiplier, is a code cheat whose data the VM writes while it is on. Two mods that write one cave differently conflict: the mod later by name is greyed out.

Registers: like dmnt, the VM starts each frame with the registers at 0. The setup cheats (a `{name}` master, or a file's first cheat, which in many files loads a pointer into a register for the others, often named "must be on") run first and hand their registers to every other cheat. A cheat that uses such a register also gets the value the setup cheat leaves when setup could tell it, so it works even with the setup cheat off. Packages that keep each cheat in a folder of its own work too: the setup reads the mods by name, and a file of one cheat counts as a setup cheat only when it is the first, so name the master code's folder to sort first (`#0110 ...` before `#1 ...`).

The setup builds the code cheats of every mod in the `mods` folder, switched on or off, so switching a mod on later needs only a restart. The VM runs opcodes 0 to A, C0 to C3 (register conditions, save and restore, static registers), and FF0, FF1 and FFF, which do nothing here. A code cheat added or edited after the last build waits for the rebuild; a data cheat runs at the next start, and is switched off if it turns out to write into the game's code.

Not supported, and shown greyed out with the reason:

- code writes under conditions other than buttons held;
- a branch into the middle of another function of the game, or a call to something that is not a function: the cheat was most likely made for another update;
- code writes whose address or value only a run can tell (from a register loaded from the game's memory);
- a cheat that writes a code cave differently from another cheat.

`build-cache\cheats.status` next to `totk.exe`, written by the setup, records which cheats were built and how; the log line `cheats: ...` sums them up at start. When the code cheats cannot be built, the setup warns and installs the game without them.
