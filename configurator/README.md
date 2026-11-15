# totk-config

The settings configurator of the TotK PC recomp: a window that edits `totk.ini` and settings profiles before the game
starts. What it does for players: [docs/settings.md](../docs/settings.md), "Settings configurator". How to build and
test it: [docs/building.md](../docs/building.md), "Settings configurator".

| File | What it holds |
| --- | --- |
| `settings.json` | Every setting, written by `tools/gen_configurator_settings.py` from the runtime's sources. Not edited by hand. |
| `src/ini.rs` | `totk.ini` read and written as `runtime/cfg.c` `cfg_load` and `cfg_save_to` do. |
| `src/profiles.rs` | `profiles/<name>.ini` and `profiles/active.txt` next to `totk.ini`. |
| `src/presets.rs` | The starting points of a new profile (Quality, Performance, the runtime's Steam Deck profile). |
| `src/data.rs` | `settings.json` as types; the tests that compare it with `runtime/cfg.c`. |
| `src/app.rs`, `src/theme.rs`, `src/i18n.rs` | The window, the in-game menu's colours and font, the window's own words in English and Russian. |

Licences of what the exe links: the crates are MIT, Apache-2.0, Zlib, BSD or ISC (most under a choice of them),
except `clipboard-win` and `error-code` (BSL-1.0), which eframe pulls in for the clipboard on Windows and cannot leave
out. `unicode-ident` (Unicode-3.0) only runs at build time inside proc macros. The embedded font is Roboto Medium
(Apache-2.0, `runtime/ui/fonts/`); CJK and symbol glyphs come from the system's own fonts at run time.
