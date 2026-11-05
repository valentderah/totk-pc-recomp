# Importing `.cai` Autobuild blueprints into the game

Status: **works** — a `.cai` blueprint file is read, validated, and written into the game's own Autobuild history, where
it appears as the **first card of the list** (the newest draft) and builds with the native preview, rotation and Zonai
energy costs.

Upstream request: issue #680, "Implement the .cai blueprint import feature" (`valentderah/totk-pc-recomp`).
Baseline: v0.3.10 (`101132a6`).

Everything below was measured on a running game, on several save files, with the list wiped clean in between.

## 1. What the feature does

The port's settings menu gets one new tab, **Blueprints**:

1. **Choose a blueprint file** opens the host's file dialog. Only two extensions are offered, `.cai` and `.txt`, and
   nothing else can be picked; the file is read and validated while the game keeps running. The dialog opens at the
   folder it was last used in, so a second import is one click away.
2. The page shows what the file holds: the part count, or why the file was rejected.
3. **Import** writes the creation into the save data the game holds in memory and clears the game's cached pictures, so
   the creation appears in the Autobuild history right away as the newest draft — no reload, no restart.

There is no slot to pick. An import behaves exactly like a creation made in the game: the new blueprint becomes the
first card of the history, every older free card moves down one place, and the oldest free card is discarded. The
history stays at 30 entries. Cards the player marked as favorites are never discarded and keep their place, as on the
console (up to 8 favorites). Successive imports therefore stack up in order: importing A, then B, then C leaves
C first, B second, A third.

## 2. The two file forms (`runtime/blueprint_file.c`)

Both forms carry the same 6688-byte CAI blob; only the wrapping differs, so one entry point decides the form from the
file's content and hands the blob to `bpc_parse` either way.

- **`.cai`** is the blob itself, at the start of the file (section 3).
- **`.txt`** is the JSON object a build site offers as a build's export (section 2.1): the blob gzip-compressed and
  Base64-encoded in one member.

A file whose name says neither form is refused without being read. Inside a `.txt`, the JSON member is the only thing
taken: it is exactly the bytes the game's own files hold, while the object's other members are the same content already
parsed into JSON, and writing those back would mean serializing them again with no guarantee of the byte layout the
game expects.

### 2.1 The `.txt` container (`runtime/blueprint_txt.c`, `runtime/blueprint_inflate.c`)

```json
{"combinedActorInfoBase64": "H4sIA...", "combinedActorInfo": {...}, "parts": [...], "unknownActorNames": [...]}
```

`combinedActorInfoBase64` is a gzip stream of the 6688-byte blob, Base64-encoded. The reader:

1. finds the member's quoted name (a bounded scan, not a JSON parser — the files are one flat object from one exporter),
2. refuses a backslash inside the value, which a Base64 run never contains,
3. Base64-decodes the run, requiring a multiple of four characters with proper `=` padding,
4. inflates it into a buffer no larger than the blob,
5. requires exactly 6688 bytes out, then lets `bpc_parse` make the final decision.

zlib does the inflating (`libz.a`, already on the link line for other work — `tools/build_game.py` names it). The bounds
are this code's own: the output goes into the caller's buffer and never past it, a stream that wants more room is
refused rather than truncated, and the gzip trailer's CRC32 and length are both checked so a cut-off download is
reported instead of half-decoded. A bare deflate stream without the gzip wrapper is accepted too, with neither check.

The three test files this was verified against are 18492 and 19748 bytes and decode to blobs byte-identical to what
Python's `gzip` gives for the same member.

### 2.2 The remembered folder (`runtime/blueprint_dir.c`)

The folder of the last chosen file is kept in `totk.ini` as `BLUEPRINT_DIR`, under `[files]`. `bp_dir_last` hands it to
the dialog when the caller passes an empty start folder, and returns `""` when the folder no longer exists, so a deleted
folder is not a reason for the dialog to fail. `bp_dir_remember` is called by both entry points (the tab's button and the
Autobuild screen's row), so they share one memory.

## 3. The CAI blob (`runtime/blueprint_cai.c`)

A blob is 6688 bytes and holds up to 21 actors. Community files are 6688 bytes or 6712 bytes (the blob plus 24 bytes:
two camera vectors). Only the first 6688 bytes are the blob.

```
 0x0000  header, 12 bytes: magic "CmbAct", u8 pad[2], u8 version (=2), u8 count, u8 count-1, u8 pad
 0x000c  actor[21], 280 bytes each:
           Matrix4x4 m1 (64), Matrix4x4 m2 (64), float m3[6] (24), char name[128]
 0x16fc  other data[21], 12 bytes each
 0x1800  float bbox[6]  (min x, y, z then max x, y, z)
 0x1818  misc (520)
```

`bpc_parse` trusts none of it: magic, version, actor count and the second copy of the count, every used actor's name
(printable, NUL-terminated inside its 128-byte field), all 38 matrix floats finite, the bounding box finite. A damaged
or foreign file is rejected with the reason the page shows, before anything is written.

## 4. Where the history lives in the save data

`AutoBuilder.Draft.Content` is **five parallel arrays of 30 entries** in `progress.sav`. They are indexed by physical
entry (0..29) and carry **no display order of their own**; the order the page draws by comes from `Index`:

| GameData hash | name | type | meaning |
|---|---|---|---|
| `0xa56722b6` | `CombinedActorInfo` | BinaryArray | the drafts: 30 items of `{u32 size = 6688; CmbAct blob}` |
| `0xd27f8651` | `Index` | IntArray | the **age** of each entry: `0` is the card drawn first, `29` the last. A permutation of 0..29 |
| `0x67f4b46b` | `IsFavorite` | BoolArray | the ★ mark, one bit per entry |
| `0xc5bf2815` | `CameraPos` | Vector3Array | the camera position the draft was framed with |
| `0xef74dca7` | `CameraAt` | Vector3Array | the camera target |

This is the fact the implementation is built on, and the one that cost the most time to establish: **the array order is
not the display order.** Writing entry 0 does not put a creation on the first card — it puts it wherever entry 0 sorts,
which differs per save file.

Measured on three save files, with the import writing entry 0:

| save file | `Index[0]` | card the import appeared on |
|---|---|---|
| slot_00 | 17 | 18 |
| slot_01 | 12 | 13 |
| slot_02..05 | 10 | 11 |

`Index[0] + 1` matched the card number exactly in every case. That is how the causal link was proven, and it is why the
final code looks up `Index` instead of writing entry 0.

### Reading array bounds from the file

The five arrays are found through the save's own declaration table, never by offset arithmetic (offsets differ per
file):

```
+0x00 u32 magic 0x01020304
+0x04 u32 format version
+0x08 u32 data_off          start of the data region
+0x0c .. 0x1f zero padding
+0x20 ..  declaration table: pairs of {u32 hash; u32 value}
          a zero hash marks a section and the next word is its number
data region: for arrays, u32 count then the items
          a Binary item is {u32 size; u8 data[size]}
```

The header bytes before each array's data are **not** the same for every type, which is easy to get wrong:

| type | bytes before the first item |
|---|---|
| BinaryArray | 4 (only the count; an item carries its own size) |
| IntArray | 4 |
| BoolArray | 8 (the count, then a u32 holding how many bits are stored — 32 for 30 entries) |
| Vector3Array | 4 |

## 5. How the import works (`runtime/blueprint_import.c`)

The game keeps the loaded save data in guest memory, and on this port guest memory is host memory
(`runtime/memory.c`), so an import is a write into it.

### Finding the arrays

The draft array is found by scanning the guest heap for the blob magic `CmbAct`, grouping the hits by stride, and
checking the shape of the group (30 entries at a 6692-byte stride, with the size fields and version bytes right). The
address is cached for the run and re-checked on each use. The scan covers the heap only — the region the save data lives
in — so it takes about 0.3 s instead of walking the whole address space.

The page copy is the same scan's other group: 60 blobs at a 6688-byte stride (two per entry, no size fields). Both
shapes are written, because the game keeps both and draws from the parsed one:

* **serialized** — what the game writes back to `progress.sav`
* **parsed** — the records the Autobuild page reads when it draws

Writing one without the other imports the data but leaves the open page showing the previous creation.

### Finding the live Index array

`Index` **is** in guest memory. The save file's copy is used only to locate it: the 30 ages form a distinctive 120-byte
run, and an exact `memchr` match over the heap (the same scan that finds the draft arrays, ~0.3 s) returns the address of
the live array. Every save slot has its own `Index` array, so each candidate file's ages are tried until one matches;
the one that matches is the save the game actually loaded. This also fixes the old bug where reading the first file
ordered an import by another save's ages and it landed on card 9.

Once found, the address is cached for the process. That matters for successive imports: after the first import rotates
the ages, the file's copy is stale, so re-matching it would fail. Reading the cached live array directly gives the
current ages every time.

### Rotating the ages (the insert)

The game's own rule for a new creation is: favorites keep their age, every other (free) entry's age increases by one,
and the oldest free entry — the one with the largest age among the non-favorites — is evicted. The new creation takes
that entry and its age becomes 0, so it is drawn first. The result is still a permutation of 0..29; a save whose ages
do not fit that shape is refused rather than written. The `IsFavorite` bitmask (30 bits in one `u32`, with a count word
in front of it) is read from the same save file, so a favorite is never chosen as the victim.

Writing the rotated ages into guest memory means the open Autobuild page reorders at once, and the game writes the new
order back to `progress.sav` with its own save. The blob is written into the evicted entry in both in-memory copies, as
described above.

### Clearing the cached pictures

The game draws a picture of each draft once and caches it as `<profile>/cache/<i>/icon/Draft_NNN.png`. After a write
those files describe the old contents, so they are removed and the game draws them again. **Without this the change is
invisible**: the data in memory is correct, but the page keeps showing the picture it drew before, which looks exactly
like a failed import. Clearing them is what makes the new creation appear on the already-open page.

### What stays untouched

The array keeps its 30 entries, the item count never changes, and the save never grows. Only one entry's blob changes:
the evicted oldest free entry. The other 29 entries keep their data byte for byte, the favorites keep their age, and no
favorite flag is changed (an imported blueprint is not made a favorite).

## 6. The page (`runtime/ui/blueprint_page.cpp`)

One row: **Choose a blueprint file**, then **Import**. The chosen file's part count is shown, or the reason it was
rejected.

`bp_favorites` and `bp_import_newest` walk the game's memory, so both run on a worker thread and the page only collects
the results. The tab opens instantly; opening it never blocks the game. There is one lookup per tab entry, not a timer.

## 6.1 The entry in the Autobuild screen itself (`runtime/blueprint_hint.cpp`)

The tab above is reached from the settings menu, so it needs the player to know where to look. The second entry is on
the game's own screen: while the Autobuild page is open, its bottom guide bar shows one more hint, the ⊕ button and the
word **Import/D导入**, and pressing ⊕ opens the same file picker.

That bar is the game's, not ours: `blyt/AutoBuilder_00.bflyt` holds the align pane `A_Guide_00` with four part panes of
`PaGuideKeySimple_00`, filled from `LayoutMsg/AutoBuilder_00.msbt`. Its slots are "Select", an unused one, "Confirm",
"Back". The row is drawn over the game by the present path's ImGui overlay at the bar's position, right of "Back", so
nothing of the game's archives is edited: no mod, resolution, or language can conflict, and the entry shows on every
game version and every pad layout.

* **Which screen.** `runtime/menu_cursor.c` `menu_cursor_screen_name()` returns the layout name of the focused game
  screen, and `AutoBuilder_00` is that name for the Autobuild page. It is read once per pad read, on the game thread,
  through `bp_hint_poll` (`runtime/input.c` `input_poll`).
* **Only there.** Outside that screen the row is not drawn and ⊕ is untouched, so the game behaves exactly as before.
* **The button.** ⊕ on this screen is the game's own "Receive", which needs Zelda Notes and does nothing on this port;
  `bp_hint_poll` clears the bit from the frame's buttons so the press belongs to the import alone. Nothing else is taken.
* **The picker and the import.** The dialog runs on `folder_pick`'s thread and only records the path; the next presented
  frame (`bp_hint_frame`) hands it to a thread of its own, because `bp_import_newest` walks the game's memory. The
  present path never waits.
* **Off switch.** `blueprint_hint = 0` (`TOTK_BLUEPRINT_HINT`) leaves the screen exactly as the game draws it.

Log lines:

```
blueprint: hint: opening the file picker
blueprint: hint chose <path>
blueprint: hint: <path>: <result>
```

## 7. Verification

Built from v0.3.10, run on Windows. Cases exercised:

- **Different save files.** The import lands on **card 1** in every one of the six save slots, whose `Index` arrays all
  differ (`Index[0]` was 17, 12 and 10 in the files measured).
- **History wiped clean.** After the Autobuild history was filled with one repeated creation and then reloaded, imports
  still landed on card 1.
- **The insert rule, successively.** Importing three different `.txt` blueprints in one run (20, 6 and 4 parts) put
  each one first and moved the earlier ones down: the log shows the evicted free entry changing every time
  (`17`, `20`, `24`, `8`, `26`) while the cached live `Index` address stays the same and the favorite is held. Re-importing
  the same file again also lands first, so successive imports stack in order.
- **Live, no reload.** The creation shows up in the already-open Autobuild page right after pressing Import; the rotated
  ages are in guest memory, so the game's own save persists the new order with the rest of the progress.
- **Nothing else disturbed.** The other 29 entries keep their blobs; only the evicted entry changes. Favorites keep their
  age, no favorite flag is touched, and the array stays at 30 entries.
- **A `.txt` file, end to end.** Three exports from HyruleWorks decoded in the reader's own harness to blobs
  byte-identical to Python's `gzip` of the same member, and a real `.txt` import in the running game landed on card 1.
- **The remembered folder.** The log named the shell's default folder on the first dialog and the chosen file's folder
  before the next one, and `BLUEPRINT_DIR` was written to `totk.ini`.
- **Damaged `.txt` files.** A file cut anywhere inside the Base64 value, a member that is not Base64, a member that is
  not a gzip stream, a non-JSON file and an object without the member are all refused with their own reason and none
  reaches the save data.

Log lines that make this checkable:

```
blueprint: scan 94 blobs in 398 ms: drafts 0x174c7a4cc, page copy 0x18508f838 (30 entries)
blueprint: Index: cached live array at 0x184e41288, newest entry 17
blueprint: Index: 1 favorite(s), evicted free entry 20 (age 28 -> 0), 28 free entries moved down one
blueprint: <name> (<n> parts) is the first draft of the Autobuild history (entry 20)
blueprint: dropped 7 cached draft picture(s), the game draws them again
```

## 8. Files

New:

| file | what |
|---|---|
| `runtime/blueprint_cai.h/.c` | the `.cai` reader and its checks |
| `runtime/blueprint_file.h/.c` | one entry point for a blueprint file of either form: decides the form from the content, unwraps a `.txt`, then validates |
| `runtime/blueprint_txt.h/.c` | the `.txt` container: the JSON member, Base64, and the decompressed blob |
| `runtime/blueprint_inflate.h/.c` | gzip/deflate decoding bounded by the caller's buffer (zlib, already linked) |
| `runtime/blueprint_dir.h/.c` | the folder a blueprint dialog last browsed, kept in `totk.ini` |
| `runtime/blueprint_import.h/.c` | finding the live arrays, choosing the entry, writing it, dropping the cached pictures |
| `runtime/ui/blueprint_page.h/.cpp` | the Blueprints tab: file, checks, the import row, the worker thread |
| `runtime/blueprint_hint.h/.cpp` | the entry in the Autobuild screen's own guide bar: its button, the picker, the import, the row |

Modified:

| file | change |
|---|---|
| `runtime/folder_pick.h`, `folder_pick_win.cpp`, `folder_pick_sdl.c` | a file dialog next to the folder dialog (`file_pick`); it opens at the folder browsed last and offers only the caller's extensions |
| `runtime/ui/menu.cpp` | the tab in the enum, its name row and its page; the hint's frame and draw calls |
| `runtime/ui_text.h`, `ui_text.c` | 29 text ids in English, Russian, Brazilian Portuguese and Simplified Chinese |
| `runtime/input.c`, `input.h` | the hint's pad read, and `menu_cursor_screen_name` |
| `runtime/menu_cursor.c` | `menu_cursor_screen_name`, the focused screen's layout name |
| `runtime/cfg.c` | `BLUEPRINT_HINT` and `BLUEPRINT_DIR` |
| `tools/build_game.py` | `libz.a` on the link line, for the `.txt` reader |

## 9. Notes for whoever picks this up

- `Index` decides the display order; the array order does not. Writing entry 0 for "the newest" is the mistake that
  makes an import appear in the middle of the list.
- `Index` **is** in guest memory and its address can be found by matching the save file's 30 ages exactly. An earlier
  conclusion that it was not (and that an insert was therefore impossible) came from comparing the wrong save slot:
  the game was playing `slot_05` while the experiment read `slot_00`. Cache the address once and read the live ages from
  it for every import, or the second import will try to match stale ages from the file.
- The rotation is checked to still be a permutation of 0..29 before any write; a save with an unusual age layout is
  refused rather than corrupted.
- The cached `Draft_NNN.png` files are part of the state. An import that skips them looks broken even when the memory is
  right.
- `IsFavorite` (one bit per entry) protects ★ slots; this implementation does not write it, so an import under the
  history tab never touches the player's favourites.
- The camera arrays (`CameraPos`, `CameraAt`, hash `0xc5bf2815` / `0xef74dca7`) hold the framing the template was
  captured with; a `.cai` of 6712 bytes carries both and they can be written with the draft.
