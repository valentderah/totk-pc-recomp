# Importing `.cai` Autobuild blueprints into the game

Status: **prototype works** — a `.cai` blueprint file is read, validated, and written into the game's own Autobuild
blueprint list, and the game then shows it in its native blueprint page and saves it with the rest of the progress.

Upstream request: issue #680, "Implement the .cai blueprint import feature" (`valentderah/totk-pc-recomp`).
Baseline: v0.3.10 (`101132a6`).

## 1. What the feature does

The port's settings menu gets one new tab, **Blueprints**:

1. **Choose a blueprint file** opens the host's own file dialog (`.cai`, `.totkab`, `.txt`), and the file is read and
   checked while the game keeps running.
2. The page shows what the file holds: the part count, or why the file was rejected.
3. The page lists the game's **30 Autobuild slots** with the part count of what each holds now, and the player picks the
   one to replace. Replacing is the only operation: the list length and the save layout never change, so the other
   entries are untouched.
4. **Import into slot N** writes the blueprint into the save data the game holds in memory, drops the game's cached
   picture of that slot, and the game's own save then persists it.

The point of writing into the save data (instead of the port drawing a second, host-side list) is that everything
downstream is the game's own code: the native preview, rotation, the Zonai energy cost, the "not enough parts" rule and
the save it writes are unchanged, because the game cannot tell an imported creation from one the player built.

## 2. How a `.cai` file is read (`runtime/blueprint_cai.c`)

A blob is 6688 bytes and holds up to 21 actors. Community files are 6688 bytes or 6712 bytes (the blob plus 24 bytes of
unknown meaning); the game's own assets are longer. Only the first 6688 bytes are the blob, and the rest is ignored.

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

## 3. How the import works (`runtime/blueprint_import.c`)

The Autobuild list is one `BinaryArray` flag in `progress.sav` (GameData hash `0xa56722b6`): 30 items, each
`{u32 size = 6688, blob}`. The game keeps the loaded save data in guest memory, and because guest memory is host memory
on this port (`runtime/memory.c`), the import is a write into that memory:

- The list is found by scanning the guest heap for the blob magic `CmbAct`, grouping the hits by their stride, and
  checking the group's shape (30 items at 6692 bytes with the right size fields). The address is cached for the run and
  re-checked on each use; a failed scan is retried at most once a second, so a save that is still loading does not stall
  the page.
- The game keeps the list in two shapes and **both** are written: the serialized array (the bytes the game writes back
  to `progress.sav`) and the parsed copies the blueprint page draws (two 6688-byte records per slot). Writing only the
  serialized copy persists the import but leaves the open page showing the old creation.
- `runtime/fs`'s read of `progress.sav` is not needed for this; the scan finds the buffer whatever its address.
- The game caches a picture of each slot as `<profile>/cache/<i>/icon/Draft_NNN.png`, drawn once per creation. After a
  write those are removed and the game draws them again, so a replaced slot does not keep the old picture.

One write replaces exactly one slot: the item count, the sizes and the position of every other entry are untouched.

## 4. The host side (`runtime/ui/blueprint_page.cpp`)

The page must never stall the present path, and the import walks a few gigabytes of guest memory. Both the lookup and
the write run on a worker thread of their own (as the file dialog already does, `runtime/folder_pick.h`); the page only
queues one request and collects the result while it draws. The tab therefore opens instantly, says "looking for the
favourite slots" while that runs, and needs no retry timer: one attempt is queued per tab entry, plus a row to retry.

## 5. Verification

Built from the v0.3.10 baseline with the feature applied. Log line confirms the baseline:
`version: TotK Recomp 0.3.10, game 1.4.3`.

1. Settings -> Blueprints opens immediately; no slow frame in the log; the slot grid shows the game's own 30 entries
   with their part counts.
2. A 19-part community blueprint was written into the list while the game was running (the same three writes
   `bp_import` performs), the cached pictures were dropped, and the reference screenshot
   `build/blueprint/shots-clean/ACCEPTED-v0310-history-woodSail.png` shows the imported creation in the game's native
   **↺ history** list, drawn from the new data (the pictures regenerate at ~93 KB instead of the old ~12 KB).
3. The game then saved the modified list by itself: `slot_01/progress.sav` changed hash and its slot 1 now holds the 19
   actors of the imported creation. Reloading that save keeps it.

### A replacement touches only its own slot

`build/blueprint/verify_single_slot.py` reads all 30 blobs of the live list before the write, writes one slot the way
`bp_import` does (serialized item plus both parsed blobs), and reads all 30 back:

```
before: slot 1 = 4 actors, AsbObj_WoodStick_A_LL_01
serialized slots changed: [1]
parsed slots changed:     [1]
after:  slot 1 = 19 actors, AsbObj_WoodSail_A_01
RESULT: only the chosen slot changed, the other 29 are byte-identical
```

The item count, every other entry's bytes and the save layout are unchanged, so an import cannot damage the blueprints
the player already has. (An earlier experiment that filled all 30 slots was a way to make the page show *something*
before the lists were located; it is not what the feature does and `bp_import` never writes more than one slot.)

Not yet verified end to end: pressing the page's Import button on the v0.3.10 build (the test wrote the same memory from
outside the game to isolate the data path). The page's slot list is by index, while the history list draws its cards in
the save's own order (see 6), so the two do not line up one to one; the player picks the slot whose contents they want
to replace.

## 6. Known limitations

- **One list only.** The 30-slot array is the **↺ history** list. The ★ mark in the game's list is a separate save flag
  (a `BoolArray`, one bit per slot) that the import does not set, so an imported creation shows up under history, not
  under ★ favourites. Setting that bit for the imported slot is future work.
- **Display order is not slot order.** The order the history list shows its entries in is its own save flag (an
  `IntArray` of 30 slot indices; its first value is the card at the top). In the test save it is
  `[12, 11, 10, 9, 8, 0, …]`, so slot 1 is the sixth card and slot 13 the first — which is why replacing slot 1 left the
  top card alone. The page lists slots in index order; the player picks the entry they want to replace.
- `.totkab` (the `TOTKAuto` wrapper around a CAI) is recognised and refused with a reason; decoding it is separate work.
- The importer writes into the game's live save buffer, so an import must not race the game's own save. In the port the
  menu blocks game input while it is open, and the worker performs the write between two frames of the game's own save.

## 7. Files

New:

| file | lines | what |
|---|---|---|
| `runtime/blueprint_cai.h/.c` | 55 / 105 | the `.cai` reader and its checks |
| `runtime/blueprint_import.h/.c` | 45 / 290 | finding the live list, writing one slot, dropping the cached pictures |
| `runtime/ui/blueprint_page.h/.cpp` | 8 / 415 | the Blueprints tab: file, checks, slot grid, import row, worker thread |

Modified:

| file | change |
|---|---|
| `runtime/folder_pick.h`, `folder_pick_win.cpp`, `folder_pick_sdl.c` | a file dialog next to the folder dialog (`file_pick`) |
| `runtime/ui/menu.cpp` | the tab in the enum, its name row and its page |
| `runtime/ui_text.h`, `ui_text.c` | 33 text ids in English, Russian, Brazilian Portuguese and Simplified Chinese |
