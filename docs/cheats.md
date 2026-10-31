# Cheats

The port reads Atmosphère cheat files, the format Switch homebrew and the emulators use. No cheats ship with the port: bring your own file for your game update.

## Adding a cheat file

1. Find the cheat file for your update. It is named after the first 8 bytes of the game's build id:

   | Update | File |
   |---|---|
   | 1.2.1 | `9B4E43650501A4D4.txt` |
   | 1.4.3 | `277178B7DBA1B6D4.txt` |

   The Cheats tab of the menu names the file the installed game reads, and `logs\totk.log` says `cheats: no cheats\<id>.txt` when it is missing.
2. Put it in a `cheats` folder next to `totk.exe`: `cheats\277178B7DBA1B6D4.txt`.
3. Run `TotK-Setup.exe` again on the same folder. It builds the cheats that change the game's code into the game (about three minutes); saves and settings are kept. A file with only data cheats works without this step.
4. In game, open the menu (F1) and switch cheats on and off on the Cheats tab. The switches are saved under `[cheats]` in `totk.ini`.

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

Each cheat's switch is `cheat_<hash> = 1`, where the hash is FNV-1a 32 of the cheat's name. Two files that use the same names, for example one per update, share their switches.

## How cheats run

The game's code is compiled ahead of time, so a cheat cannot write new instructions into it while the game runs. The port sorts each cheat by what it writes (`tools/cheats.py` at setup, `runtime/cheats.c` in game):

| Kind | What the cheat does | How the port runs it |
|---|---|---|
| code | Opcode 0 writes into main's code | Built into the game at setup as a patch set (`recomp/patches.py`); switches at once. |
| key | Code writes inside a button condition (`8` mask, writes, `21` else, the original words, `20`) | A patch set that is on while the cheat is on and the buttons are held. |
| data | Writes data only: heap, main's data, pointer chains, button conditions | Runs in a cheat VM after dmnt's, opcodes 0 to A, every presented frame while on. |
| master | `{name}` with code writes | A patch set that is always on. |
| restore | Writes back only the game's original instructions | Not needed: switching a cheat off restores them. Greyed out. |

A cheat whose code writes call or jump into a code cave the cheat writes itself (into main's zero padding) gets the cave inlined at the call site. A code cheat added to the file or edited after the setup ran is greyed out until the setup runs again; a data cheat runs at once.

Not supported, and shown greyed out with the reason:

- the 0xC and 0xF opcode extensions;
- opcode 5 with a register offset;
- code writes mixed with other opcodes, except the button pattern above;
- code writes that change the CPU flags differently from the game's instruction, or code caves with branches or pc-relative instructions.

`cheats\<build id>.status`, written by the setup, records which cheats were built and how; the log line `cheats: ...` sums it up at start.
