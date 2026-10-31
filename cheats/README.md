# Cheats

Atmosphere cheat files, compiled in by `recomp/gen.py --cheats cheats` and switched on and off in the settings menu
(F1, Cheats tab) while the game runs. A file is named after the first 16 hex digits of main's build id, as Atmosphere
names it: `277178B7DBA1B6D4.txt` is TotK 1.4.3. To add a cheat, paste its `[Name]` block into the file and generate
and build again; `recomp/cheats.py` says which opcodes convert, and gen.py prints a line per cheat it skips.

The chosen cheats are kept in totk.ini as `cheats = <id>, <id>` ([cheats] section); an id is the cheat's name in lower
case with `_` between words.

## Sources

- `277178B7DBA1B6D4.txt` [Inf. Equipment Durability]: Arch9SK7/MyNXCheats,
  `The Legend Of Zelda Tears Of The Kingdom/0100F2C0115B6000/cheats/277178B7DBA1B6D4.txt`.
