# Importing `.cai` and `.txt` Autobuild blueprints into the game

Status: **works** — a blueprint file is read, validated, and written into the game's own Autobuild history, where it
appears as the **first card of the list** (the newest draft) and builds with the native preview, rotation and Zonai
energy costs.

Upstream request: issue #680, "Implement the .cai blueprint import feature" (`valentderah/totk-pc-recomp`).
Baseline: v0.3.11 (`0ae47a5`).

Everything below was measured on a running game, on several save files, with the list wiped clean in between.

## 1. What the feature does

While the game's **Autobuild screen** is open, its bottom guide bar shows one more hint — the ⊕ (Plus) button and the
word **Import**, right of "Back":

1. Pressing Plus anywhere on that screen opens the **port's own file picker** (`runtime/blueprint_picker.c` and
   `runtime/ui/blueprint_picker.cpp`), not a Windows or desktop file dialog. The picker lists the folder's
   subdirectories and its `.cai` / `.txt` files, splits the current path into pressable parts (`C:` → `Users` →
   `Administrator`), offers the other drives at a drive's root, scrolls with the mouse wheel, and scales with the
   window. It can be driven by pointer, D-pad / A / B, or keyboard.
2. Choosing a file writes the creation into the save data the game holds in memory and clears the game's cached
   pictures, so it appears in the Autobuild history right away as the newest draft — no reload, no restart.

While the picker is open the game receives no input (`input_menu(1)`), the pointer is released to the picker without
being forced to the centre, and the settings menu's **Blueprints tab is gone**: the Autobuild screen is the only entry
point. `blueprint_hint = 0` (`TOTK_BLUEPRINT_HINT`) leaves the screen exactly as the game draws it.

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
the picker when the caller passes an empty start folder, and returns `""` when the folder no longer exists, so a deleted
folder is not a reason for the picker to fail. `bp_dir_remember` is called when a file is chosen, so the next picker
opens where the last one was left.

## 3. The CAI blob (`runtime/blueprint_cai.c`)

A blob is 6688 bytes and holds up to 21 actors. A file can be longer, and only its first 6688 bytes are the blob:

| size | what the tail is |
|---|---|
| 6688 | nothing: the blob alone, which is what the save data stores per slot |
| 6712 | two camera vectors, 12 bytes each: the position the draft was framed from and the point it was framed at. This is the form a save editor and the community sites distribute; the game does not read it back, its cameras live in the save data under their own hashes |
| 7840 | a 1152-byte block of a draft the game saved mid-build; this parser does not read it |

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

That rule is `bp_rotate_ages` (`runtime/blueprint_import.c`), kept free of save-memory access so it can be exercised
without a running game: `tests/blueprint_rotation_test.py` links the real file against stubs and checks it directly.

A save that does **not** carry the `IsFavorite` array is a special case: a zero bitmask then cannot be told apart from
"no entry is a favorite", and guessing would let an import evict a card the player starred. The rotation is skipped in
that case — the log says so, and the import falls back to replacing the newest card, which is the documented behaviour
before the rotation existed. Nothing is dropped.

Writing the rotated ages into guest memory means the open Autobuild page reorders at once, and the game writes the new
order back to `progress.sav` with its own save. The blob is written into the evicted entry in both in-memory copies, as
described above.

The writes are ordered so the blob is in place **before** the ages are rotated. The page draws its cards sorted by
`Index`, so publishing the new order first could make it render the imported card from the old blob and keep showing the
old picture. The rotation is therefore the last change, the one the game notices after the content is in place.

### Clearing the cached pictures

The game draws a picture of each draft once and caches it as `<profile>/cache/<i>/icon/Draft_NNN.png`. After a write
those files describe the old contents, so they are removed and the game draws them again. **Without this the change is
invisible**: the data in memory is correct, but the page keeps showing the picture it drew before, which looks exactly
like a failed import. Clearing them is what makes the new creation appear on the already-open page.

### What stays untouched

The array keeps its 30 entries, the item count never changes, and the save never grows. Only one entry's blob changes:
the evicted oldest free entry. The other 29 entries keep their data byte for byte, the favorites keep their age, and no
favorite flag is changed (an imported blueprint is not made a favorite).

## 6. The entry in the Autobuild screen (`runtime/blueprint_hint.cpp`)

While the Autobuild page is open, its bottom guide bar shows one more hint, the ⊕ button and the word **Import**, and
pressing ⊕ opens the port's own file picker. There is no settings-menu tab any more; this screen is the only entry point.

That bar is the game's, not ours: `blyt/AutoBuilder_00.bflyt` holds the align pane `A_Guide_00` with four part panes of
`PaGuideKeySimple_00`, filled from `LayoutMsg/AutoBuilder_00.msbt`. Its slots are "Select", an unused one, "Confirm",
"Back". The row is drawn over the game by the present path's ImGui overlay at the bar's position, right of "Back", so
nothing of the game's archives is edited: no mod, resolution, or language can conflict, and the entry shows on every
game version and every pad layout.

* **Which screen.** `menu_cursor_screen_open()` (`runtime/menu_cursor.c`) enumerates `eui::ScreenMgr`'s open screens and
  matches the layout name `AutoBuilder_00`. The game thread reads it once per pad read (`bp_hint_poll`), which is what
  lets the port take the button before the game sees it; the present thread reads it too, freshening a short latch so
  the row does not blink during focus gaps.
* **Which control.** Being on that screen is not enough: the row belongs to its three tabs (favourites, drafts, history),
  and while the selection sits in the card list below the player is choosing a draft. `menu_cursor_focus_is_tab()` tells
  them apart by the layout each focusable button was built from: the tabs instantiate `PaAutoBuilderTabBtn_00`, the cards
  instantiate `PaAutoBuilderPhotoBtn_00`, and a button carries its template (`BTN_LAYOUT` -> `LYT_NAME`, the same field
  `menu_cursor_screen_name` reads for a whole screen). No position threshold and no pane-name offset is involved.
* **Only there.** Outside that screen, and on it while the focus is in the card list, the row is not drawn and ⊕ is
  untouched, so the game behaves exactly as before.
* **The button.** ⊕ on this screen is the game's own "Receive", which needs Zelda Notes and does nothing on this port;
  `bp_hint_poll` clears the bit from the frame's buttons so the press belongs to the import alone. Nothing else is taken.
* **The picker.** `bp_picker_open` (`runtime/blueprint_picker.c`) shows a panel drawn by `ui/blueprint_picker.cpp`
  through ImGui's draw list only: no ImGui widgets or layout, so it cannot trip ImGui's layout checks. All folder
  listing, path splitting, rectangle computation, pointer/pad hit tests and wheel scrolling are in C. The window is a
  three-band layout — title, breadcrumb, footer — around one scrolling list, designed against the Nielsen Norman Group's
  usability heuristics: it uses the settings menu's dark-green/gold palette (consistency), keeps the current folder, the
  item count and an empty/error message always visible (system status), replaces the old redundant path controls with a
  single breadcrumb (minimalism, recognition over recall), distinguishes folders/files/drives with icons, and gives
  Cancel its own labelled button. It shows a scrollbar, localised text, and scales with the window. The breadcrumb's
  back control, the pad's B, or a right mouse click go up one folder; from a drive root that reaches a real "This PC"
  state (the empty path) that lists every drive, where the breadcrumb is empty and the back control is dimmed. The
  wheel scrolls the list from the window's `WM_MOUSEWHEEL` / `SDL_EVENT_MOUSE_WHEEL` events (`bp_picker_wheel`), the
  same queue ImGui uses, rather than raw input, which stops arriving once the cursor capture is released. `input_menu(1)`
  blocks the game's input while it is open and releases the pointer to it; `menu_cursor.c` skips the game's menu cursor
  so the pointer is not forced to the centre. The raw-input wheel accumulator in `input_win.c` / `input_sdl.c` is still
  drained by `input.c` for the map zoom, so it cannot grow without bound.
* **The import.** The picker's answer is recorded and the next presented frame (`bp_hint_frame`) hands it to a thread of
  its own, because `bp_import_newest` walks the game's memory. The present path never waits.
* **Off switch.** `blueprint_hint = 0` (`TOTK_BLUEPRINT_HINT`) leaves the screen exactly as the game draws it.

### 6.1 The preview column

While a file is highlighted the picker shows a small 3D view of it on the right. Every part is drawn as **its real model**,
read from the player's own RomFS and decoded at runtime, placed by the CAI's rotation, `fuseInfo` scale and position. The
render is one offscreen Vulkan pass with a real depth buffer (`runtime/ui/preview3d.cpp`), shown as a single textured quad
inside the panel; `layout_preview` and the CPU projection in `runtime/blueprint_picker.c` remain as the fallback for a
machine where the offscreen target cannot be created.

**Why the CAI alone is not enough.** A CAI blob holds each part's *placement* — a rotation, a position and a scale — and
the actor's name, and nothing else: no mesh. The actor name is what makes a real model possible: the game ships one model
per actor, so the preview resolves the name to a model file and decodes it. This is the same information the reference
viewer (Blehditor) uses, and the same one the game itself uses when it rebuilds a draft.

**The model path** (`runtime/model_romfs.c`):

1. `Pack/Actor/<actor>.pack.zs` from the RomFS — a SARC with the game's zstd dictionary — is unpacked and its ModelInfo
   BYML walked for `FmdbName`. The ModelInfo member is *not* always named after the actor (a propeller's pack carries
   `DgnObj_SkyIsland_Propeller_A_01`), so every ModelInfo member is tried rather than guessing the file name.
2. `Model/<project>.<FmdbName>.bfres.mc` is located. The project prefix is not derivable from the actor, so an exact-name
   attempt is followed by a scan of the RomFS index for any `Model/*` path ending in `.<FmdbName>.bfres.mc`.
3. That file is decoded by `runtime/model_meshcodec.c`, the C door over the MeshCodec reference decoder, and the result is
   walked by `runtime/bfres.c`.

**The `.bfres.mc` container** (`runtime/bfres.c`, and the MeshCodec bridge). A 12-byte `MCPK` header, then a zstd stream
**without** the 4-byte zstd magic and **without** a frame content size, then an `FMSH` geometry section compressed by
Nintendo's MeshCodec. Three details cost real time to get right and are worth knowing before touching this code:

- The stream is magicless *and* sizeless, so `ZSTD_decompressDCtx` fails with "Src size is incorrect". The decode must be
  `ZSTD_decompressStream` in a loop until it returns 0, after `ZSTD_DCtx_setParameter(dctx, ZSTD_d_format,
  ZSTD_f_zstd1_magicless)`.
- MeshCodec calls zstd's **internal** block API (`ZSTD_decompressBlock`, `ZSTD_insertBlock`, `dctx->entropy.rep`), which is
  not in the public `zstd.h`. It is built from zstd source beside the game rather than against the system library.
- The byte-order field in the `FRES` header reads `0xFEFF`, not `0xFFFE`: the file stores `FF FE`, which little-endian
  reading turns into `0xFEFF`.

**Shape geometry.** `bfres_open` walks `FMDL` → `FSHP` → `FVTX`/`Mesh`. Two properties are per-shape and must be read, not
assumed:

- **The vertex stride is per model.** It is the position buffer's byte size divided by the vertex count: a wheel is 6, a
  cart 8. Reading every model at a stride of 6 scrambles a stride-8 model into a ball of spikes.
- **The rotation of a bone is either a quaternion or Euler angles**, decided by bit 12 of the bone's flags. Reading an
  Euler bone as a quaternion normalises `(0, 0, π/2, 1)` into a 115° turn instead of 90°, which tilts every part on that
  rig by the same 25° — the "all parts lean the same way" symptom.

**Rigid skinning** (`runtime/bfres.c`). A shape with one bone per vertex carries, per vertex, a blend index in its `_i0`
attribute. That index is a slot in the skeleton's **Node_Array** (`FSKL` +0x18, `mtx_to_bone_list`), which maps to the
real bone: `bone = Node_Array[slot]`. The vertex is then transformed by that bone's world matrix, accumulated down the
parent chain. A shape with two or more bones per vertex is a *deformation* — a pile's telescoping chain, a fan's wing —
whose rest pose is its extended state, so it keeps its raw vertices. That is also what the reference implementation
(Switch-Toolbox's BFRES reader) does, and the distinction is what finally made the fan, the wheel tread and the control
stick come out right.

**A guard worth keeping.** A ModelInfo can point at a scene group instead of a part: `HiddenKorok_SilhouettePazzle_006`
resolves to `CaveEntrance_02197d15.Group00`, a terrain sheet 126 km across, which drawn as a part puts one enormous face
over the whole preview. The decoded geometry is therefore measured, and a model far beyond any real part (40 m) is
refused so the preview falls back to the part's collision body, which is always the right size.

**The collision-shape fallback** (`runtime/blueprint_shape.c`, `runtime/byml.c`, `runtime/hull.c`). A part with no model
still gets a body: its pack's `Phive/ShapeParam` BYML is walked for box/sphere/cylinder/capsule/polytope primitives, the
points are tessellated into a mesh and a convex hull is taken. This is the same fallback the reference viewer uses.

**Camera.** Dragging inside the view orbits it, one full turn per view **height** on both axes — the convention three.js'
`OrbitControls` uses (its source divides both deltas by `clientHeight`), which is what makes a drag feel the size it looks.
A release carries the turn on and decays it, so the view settles instead of stopping dead. Pitch is free past the poles so
the build can be read from underneath, and the camera's right vector follows yaw directly rather than a fixed world up,
so there is no gimbal lock at the poles. A second click puts the camera back to 35°/28°. The wheel zooms without a limit
except against zero. Yaw is kept across files, so a player who turned one build sees the next at the same angle.

**Shading.** One grey-clay material, two-sided (several models are single-sided sheets — sails, flags, nets), lit by one
fixed key light with a little ambient, with the per-part colour coming from the game's own 21-colour palette indexed by a
part's place in the draft. The face normal is taken from the fragment stage's screen-space derivatives, so no normal
attribute is read. This is what the reference viewer does, and it is why the preview reads like the game's own.

Log lines:

```
blueprint: hint is on the Autobuild screen
blueprint: hint: opening the file picker
blueprint: picker opens at C:\Users\Administrator\Downloads (14 entries)
blueprint: hint chose <path>
blueprint: hint: <path>: <result>
model: SpObj_ControlStick_A_01 decoded at runtime (6 shapes, 94208 bytes)
model: <actor> resolves to a 126 km scene group, not a part; using its collision body
preview3d: 21 parts in 21 draws (426x828)
```

## 7. Verification

Built on the v0.3.11 baseline (`0ae47a5`), run on Windows. Cases exercised:

- **Different save files.** The import lands on **card 1** in every one of the six save slots, whose `Index` arrays all
  differ (`Index[0]` was 17, 12 and 10 in the files measured).
- **History wiped clean.** After the Autobuild history was filled with one repeated creation and then reloaded, imports
  still landed on card 1.
- **The insert rule, successively.** Importing three different blueprints in one run puts each one first and moves the
  earlier ones down: the log shows the evicted free entry changing every time while the cached live `Index` address stays
  the same and the favorite is held.
- **Live, no reload.** The creation shows up in the already-open Autobuild page right after the import; the rotated ages
  are in guest memory, so the game's own save persists the new order with the rest of the progress.
- **Nothing else disturbed.** The other 29 entries keep their blobs; only the evicted entry changes. Favorites keep their
  age, no favorite flag is touched, and the array stays at 30 entries.
- **The in-screen picker, end to end.** Opened the Autobuild page, pressed Plus, the port's own picker opened, chose a
  `.cai`, and the creation became the first card — the whole path in one session, with no Windows dialog.
- **A `.txt` file, end to end.** Three exports from HyruleWorks decoded in the reader's own harness to blobs
  byte-identical to Python's `gzip` of the same member, and a real `.txt` import in the running game landed on card 1.
- **The remembered folder.** The picker opens where the last file was chosen, and a root path offers the other drives.
- **Damaged `.txt` files.** A file cut anywhere inside the Base64 value, a member that is not Base64, a member that is
  not a gzip stream, a non-JSON file and an object without the member are all refused with their own reason and none
  reaches the save data.
- **Automated checks.** `tools/check_content.py --staged`, `tests/settings_doc_check.py` (0 problems),
  `tests/blueprint_txt_test.py` (12 tests) and `totk.exe --selftest` (0 failures) all pass.

Log lines that make this checkable:

```
blueprint: picker opens at C:\Users\Administrator\Downloads (14 entries)
blueprint: C:\Users\Administrator\Downloads/blueprint_1701794711541.cai, 6712 bytes: ok
blueprint: Index: cached live array at 0x192de3288, newest entry 19
blueprint: Index: 1 favorite(s), evicted free entry 27, 28 free entries moved down one
blueprint: <name> (<n> parts) is the first draft of the Autobuild history (entry 27)
blueprint: dropped 4 cached draft picture(s), the game draws them again
```

## 8. Files

New (across the PR's commits):

| file | what |
|---|---|
| `runtime/blueprint_cai.h/.c` | the `.cai` reader and its checks |
| `runtime/blueprint_file.h/.c` | one entry point for a blueprint file of either form: decides the form from the content, unwraps a `.txt`, then validates |
| `runtime/blueprint_txt.h/.c` | the `.txt` container: the JSON member, Base64, and the decompressed blob |
| `runtime/blueprint_inflate.h/.c` | gzip/deflate decoding bounded by the caller's buffer (zlib, already linked) |
| `runtime/blueprint_dir.h/.c` | the folder the picker last browsed, kept in `totk.ini` |
| `runtime/blueprint_import.h/.c` | finding the live arrays, choosing the entry, writing it, dropping the cached pictures |
| `runtime/blueprint_hint.h/.cpp` | the entry in the Autobuild screen's own guide bar: its button, the picker, the import, the row |
| `runtime/blueprint_picker.c` | the picker's C logic: folder listing, path parts, rectangles, pointer/pad/wheel hit tests |
| `runtime/ui/blueprint_picker.h/.cpp` | the picker's bridge and its draw layer (ImGui draw list only) |
| `runtime/ui/preview3d.h/.cpp` | the offscreen Vulkan pass: one draw per part, real depth buffer, shown as one textured quad |
| `runtime/shaders/preview_vs.vert`, `preview_fs.frag` | the preview's two shaders, and the generated `preview_*_spv.h` beside them |
| `runtime/model_romfs.h/.c` | the actor → ModelInfo → `.bfres.mc` path, the decoded-model cache, and the scene-group size guard |
| `runtime/model_meshcodec.h/.c` | the C door over the MeshCodec decoder (`libmeshcodec.a`) |
| `runtime/bfres.h/.c` | the BFRES container reader: shapes, per-model vertex stride, bone rotation type, rigid skinning |
| `runtime/blueprint_shape.h/.c` | the collision-body fallback: a part's pack → ShapeParam BYML → tessellated mesh |
| `runtime/byml.h/.c` | the BYML reader the shape fallback and the ModelInfo resolution both use |
| `runtime/hull.h/.c` | the convex hull the collision fallback takes over its point cloud |
| `tests/byml_main.c`, `tests/byml_test.py` | the BYML reader over real and truncated documents |
| `tests/hull_main.c`, `tests/hull_test.py` | the convex hull over random, deduplicated and coplanar point sets |

The settings-menu **Blueprints tab** (`runtime/ui/blueprint_page.h/.cpp`) was added in an earlier commit and is
**removed** here: the Autobuild screen is the only entry point.

Modified:

| file | change |
|---|---|
| `runtime/platform/platform.h`, `platform/win32.c`, `platform/linux.c` | `plat_dir_list` and `plat_root_list`, the picker's folder and drive enumeration |
| `runtime/ui/menu.cpp` | the hint's frame and draw calls, the picker draw call, the preview's offscreen pass, and the overlay keeping itself alive for the picker |
| `runtime/ui/menu_win.cpp`, `ui/menu_sdl.cpp` | events reach the overlay while the picker is open, not only the settings menu |
| `runtime/ui_text.h`, `ui_text.c` | the picker's and hint's text ids in English, Russian, Brazilian Portuguese and Simplified Chinese |
| `runtime/input.c`, `input.h` | the hint's pad read, `menu_cursor_screen_open` / `menu_cursor_focus_is_tab`, and leaving the wheel to the picker |
| `runtime/input_win.c` | wheel notches accumulated while the picker is open |
| `runtime/menu_cursor.c` | `menu_cursor_screen_open` (whole-screen detection), `menu_cursor_focus_is_tab` (which control has the focus), and skipping the game's cursor for the picker |
| `runtime/cfg.c` | `BLUEPRINT_HINT` and `BLUEPRINT_DIR` |
| `runtime/gfx_present.c`, `runtime/vk.h`, `runtime/vk_device.c` | `vk_queue_lock` / `vk_queue_unlock` (the preview creates its targets with no frame in flight) |
| `tools/build_game.py` | `libz.a` on the link line (the `.txt` reader) and `libmeshcodec.a` beside the executable (the model decoder) |

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
- `IsFavorite` (one bit per entry) protects ★ slots; this implementation only reads it, so an import never touches the
  player's favourites. A save that carries no `IsFavorite` array at all cannot be rotated safely (a zero mask would
  look like "nothing is a favourite"), so that save keeps the replace-the-newest-card behaviour instead.
- The camera arrays (`CameraPos`, `CameraAt`, hash `0xc5bf2815` / `0xef74dca7`) hold the framing the template was
  captured with; a `.cai` of 6712 bytes carries both and they can be written with the draft.
- The picker draws only rectangles and text through `ImDrawList`. Adding an ImGui widget or layout there brings back the
  layout-assertion crash the earlier all-widget version had; keep the C/C++ split as it is.
- The blob is written before the `Index` rotation. Reversing that order lets the page render the new card order against
  the old blob, which shows up as a stale thumbnail on the open page.
