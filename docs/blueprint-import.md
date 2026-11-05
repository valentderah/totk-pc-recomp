# Importing `.cai` Autobuild blueprints into the game

Status: **works** — a `.cai` blueprint file is read, validated, and written into the game's own Autobuild history, where
it appears as the **first card of the list** (the newest draft) and builds with the native preview, rotation and Zonai
energy costs.

Upstream request: issue #680, "Implement the .cai blueprint import feature" (`valentderah/totk-pc-recomp`).
Baseline: v0.3.10 (`101132a6`).

Everything below was measured on a running game, on several save files, with the list wiped clean in between.

## 1. What the feature does

The port's settings menu gets one new tab, **Blueprints**:

1. **Choose a blueprint file** opens the host's file dialog (`.cai`, `.totkab`, `.txt`). The file is read and validated
   while the game keeps running.
2. The page shows what the file holds: the part count, or why the file was rejected.
3. **Import** writes the creation into the save data the game holds in memory and clears the game's cached pictures, so
   the creation appears in the Autobuild history right away as the newest draft — no reload, no restart.

There is no slot to pick. A creation made in the game always takes the newest place of the history, and so does an
import; the entry it displaces is the one that was newest before, which slides down one position. The other 29 drafts
keep their data.

## 2. The `.cai` file (`runtime/blueprint_cai.c`)

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

## 3. Where the history lives in the save data

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

## 4. How the import works (`runtime/blueprint_import.c`)

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

### Finding the entry to replace

`Index` is **not** in guest memory while the menu runs, so it is read from the save file. That read is where the
subtlety is: every save slot has its own `Index` array, and only one of them belongs to the file the game has loaded.
Reading whichever file came first ordered the import by another save's ages — which is exactly how an import landed on
card 9 instead of card 1 during development.

The fix is to check each candidate against guest memory: the array the game is drawing by must be present there. The
first file whose `Index` values are found in the heap is the loaded save, and `Index == 0` in it names the entry drawn
first. The import writes that entry.

### Clearing the cached pictures

The game draws a picture of each draft once and caches it as `<profile>/cache/<i>/icon/Draft_NNN.png`. After a write
those files describe the old contents, so they are removed and the game draws them again. **Without this the change is
invisible**: the data in memory is correct, but the page keeps showing the picture it drew before, which looks exactly
like a failed import. Clearing them is what makes the new creation appear on the already-open page.

### What stays untouched

The array keeps its 30 entries, the item count never changes, and the save never grows. Only one entry's blob changes;
the entry that was drawn first moves one place down the list because `Index` no longer marks it as the newest, and the
other 29 entries keep their data byte for byte.

## 5. The page (`runtime/ui/blueprint_page.cpp`)

One row: **Choose a blueprint file**, then **Import**. The chosen file's part count is shown, or the reason it was
rejected.

`bp_favorites` and `bp_import_newest` walk the game's memory, so both run on a worker thread and the page only collects
the results. The tab opens instantly; opening it never blocks the game. There is one lookup per tab entry, not a timer.

## 6. Verification

Built from v0.3.10, run on Windows. Cases exercised:

- **Different save files.** The import lands on **card 1** in every one of the six save slots, whose `Index` arrays all
  differ (`Index[0]` was 17, 12 and 10 in the files measured).
- **History wiped clean.** After the Autobuild history was filled with one repeated creation and then reloaded, imports
  still landed on card 1.
- **Different files.** A 21-part creation and a 20-part creation (`AsbObj_RopewayArm`, `Item_Fruit_A`) both imported and
  both appeared first.
- **Live, no reload.** The creation shows up in the already-open Autobuild page right after pressing Import; the game's
  own save then persists it with the rest of the progress.
- **Nothing else disturbed.** The other 29 entries keep their blobs; only the entry that is replaced changes.

Log lines that make this checkable:

```
blueprint: scan 94 blobs in 280 ms: drafts 0x174c793c8, page copy 0x185011838 (30 entries)
blueprint: <name> (<n> parts) is now the first draft of the Autobuild history (entry <k>, the one the save draws first)
blueprint: dropped 30 cached draft picture(s), the game draws them again
```

## 7. Files

New:

| file | what |
|---|---|
| `runtime/blueprint_cai.h/.c` | the `.cai` reader and its checks |
| `runtime/blueprint_import.h/.c` | finding the live arrays, choosing the entry, writing it, dropping the cached pictures |
| `runtime/ui/blueprint_page.h/.cpp` | the Blueprints tab: file, checks, the import row, the worker thread |

Modified:

| file | change |
|---|---|
| `runtime/folder_pick.h`, `folder_pick_win.cpp`, `folder_pick_sdl.c` | a file dialog next to the folder dialog (`file_pick`) |
| `runtime/ui/menu.cpp` | the tab in the enum, its name row and its page |
| `runtime/ui_text.h`, `ui_text.c` | 28 text ids in English, Russian, Brazilian Portuguese and Simplified Chinese |

## 8. Notes for whoever picks this up

- `Index` decides the display order; the array order does not. Writing entry 0 for "the newest" is the mistake that
  makes an import appear in the middle of the list.
- The cached `Draft_NNN.png` files are part of the state. An import that skips them looks broken even when the memory is
  right.
- `IsFavorite` (one bit per entry) protects ★ slots; this implementation does not write it, so an import under the
  history tab never touches the player's favourites.
- The camera arrays (`CameraPos`, `CameraAt`, hash `0xc5bf2815` / `0xef74dca7`) hold the framing the template was
  captured with; a `.cai` of 6712 bytes carries both and they can be written with the draft.
