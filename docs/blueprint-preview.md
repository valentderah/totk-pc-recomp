# The 3D preview of a blueprint (proposed separately)

This document describes the part of the blueprint work that draws a build's parts in 3D inside the picker, before the
file is imported. **It is not part of the import change.** The import, the `.cai`/`.txt` readers, the picker and the
save-memory write are proposed on their own; the preview needs a GPL-2.0 decoder and a good deal of renderer code, so it
is kept apart so that change stays MIT and reviewable by itself.

Everything measured while building it is public, so this step does not have to be re-derived:
https://gist.github.com/kaiyao7/ad158b08a365e0e6020d0db332d96e13

## What the preview needs that the import does not

- Each part's **real model**, decoded from the player's own RomFS: `Pack/Actor/<actor>.pack.zs` (SARC, game zstd
  dictionary) → ModelInfo BYML → `FmdbName` → `Model/<project>.<FmdbName>.bfres.mc` → `MCPK` header + magicless,
  sizeless zstd + `FMSH` (MeshCodec) → BFRES `FMDL`/`FSHP`/`FVTX`/`FSKL` → vertices and indices.
- A part with no usable model falls back to its **collision body** (a convex hull from `ShapeParam` BYML).
- An offscreen Vulkan pass with a depth buffer, drawn as one textured quad inside the picker panel.

### Licensing consequence

The only MeshCodec implementation is [dt-12345/MeshCodec](https://github.com/dt-12345/MeshCodec), **GPL-2.0**, so a
binary that links it is a GPL-2.0 work. The decoder is therefore built **outside** the tree into
`build/<version>/libmeshcodec.a` by `tools/build_meshcodec.py`, and only a small C door (`runtime/model_meshcodec.c`,
`runtime/model_meshcodec.h`) is committed. A build without the library resolves no model and the preview falls back to
collision bodies.

- A build that links the decoder is a GPL-2.0 work: redistributing that binary means honouring GPL-2.0 for it, including
  the source of the decoder.
- Nothing here re-licenses the rest of the port.
- Read for behaviour, no code copied, and all of it belongs to this half of the work: `zeux/meshoptimizer` (MIT),
  `bfrespy` (GPL-3.0), `KillzXGaming/Switch-Toolbox` (GPL-3.0), `dt-12345/totk_params` (no licence),
  Blehditor (behaviour reference), three.js `OrbitControls` (MIT).

## Files this half adds

`runtime/ui/preview3d.h/.cpp`, `runtime/model_romfs.h/.c`, `runtime/model_meshcodec.h/.c`, `runtime/bfres.h/.c`,
`runtime/byml.h/.c`, `runtime/hull.h/.c`, `runtime/blueprint_shape.h/.c`, `runtime/blueprint_attach.h/.c`,
`runtime/preview_vs_spv.h`, `runtime/preview_fs_spv.h`, `runtime/shaders/preview_vs.vert`,
`runtime/shaders/preview_fs.frag`, `tools/build_meshcodec.py`, `tools/meshcodec_bridge.cpp`, and the tests
`tests/blueprint_attach_*`, `tests/byml_*`, `tests/hull_*`.

---


### 6.1 The preview column

While a file is highlighted the picker shows a small 3D view of it on the right. Every part is drawn as **its real model**,
read from the player's own RomFS and decoded at runtime, placed by the CAI's own rotation, position and `fuseInfo`. The
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

**The decoder is not in this repository, and that is a licensing decision.** The only implementation of MeshCodec is
[dt-12345/MeshCodec](https://github.com/dt-12345/MeshCodec), which is GPL-2.0. It is compiled from `build/` into
`libmeshcodec.a` beside `totk.exe`; only the door over it is in the tree. `tools/build_game.py` links that library when it
is there and defines `TOTK_MESHCODEC`, and a build without it comes out with a preview that falls back to collision
bodies. `THIRD_PARTY_NOTICES.md` records the whole position - what a build that links it becomes, how to build without
it, and which references the format knowledge came from, each with its commit.

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


## 10. How a fused part is drawn (余料建造)

A part can be a weapon or shield that had a material fused onto it before it went into the blueprint. Drawing that part means
two objects, not one, and the second one is not stored in the CAI. This section is the rule set that produces the pair; every
line of it comes from the game's own files, and none of it is special-cased for a particular item.

### 10.1 The CAI names the material but does not place it

The record is 280 bytes, and the layout is the one McSpazzy's `totk-tools` `cai.hexpat` defines:

```
Header      "CmbAct", 2 bytes padding, u8 version, u8 actor count
2 bytes padding
Actor[count]:
    Matrix4x4 mMatrix1   16 floats: r1 r2 r3 t1, r4 r5 r6 t2, r7 r8 r9 t3, a1 a2 a3 a4
    Matrix4x4 mMatrix2   16 floats
    float     mMatrix3[6]
    char      mName[128]
OtherData[count]         u32, u32, u16, u16
float mMiscFloat[6]
u8    mMiscData[520]
```

Two fields matter for fusion:

- **`mMatrix3[4]` is `fuseInfo`** (the float at offset 144). It is 1.4-1.5 on every fused part and 1.0 on every plain one.
- **`mName` is 128 bytes wide, not 64**, and holds two null-separated names: the actor at offset 152 and the fused material
  at offset 216. `runtime/blueprint_cai.c` reads them as the part's name and `SubName`. Reading it as 64 bytes lands in the
  middle of the first name and makes the following bytes look like a third field.

For every record whose `SubName` is set the `mMatrix2` (offset 48) is the identity, while records with an empty `SubName` -
GolemHead, CookSetOnFire, SwitchWheel - carry a real matrix there. So that matrix belongs to the actor's own collision
sub-shapes and says nothing about the material. Nothing in the record places the material: the placement is entirely in the
game's AttachmentParam, below.

### 10.2 The placement is in the material's AttachmentParam, under a key per weapon class

`Component/AttachmentParam/<Material>.game__component__AttachmentParam.bgyml`, inside its actor pack, has one entry per weapon
class: `AttachParamSpear_`, `AttachParamSword_`, `AttachParamLSword_` and `AttachParamShield`. `runtime/blueprint_attach.c`
picks the key from the part's own actor name (`Weapon_Spear_*` reads the spear entry, and so on), then walks the record's
`$parent` chain - the template is another member of the same pack - reading from the root down so a derived record overrides
only the fields it names. A survey of 1356 material records finds these fields, each per class:

| field | records naming it | meaning |
|---|---|---|
| `TransOffset` | 4541 | metres; a per-class nudge, not the position - see 10.3.1 |
| `RotateOffset` | 3540 | degrees about X, Y and Z |
| `BoneName` | 1802 | the weapon model's socket, e.g. `Bind_A`, `Bind_W`, `Bind_Z` |
| `Scale` | 1504 | a per-axis scale |
| `IsValid` | 860 | whether this class accepts the material at all |
| `ExScale` (+ `UseExScale`) | 354 | a uniform scale |
| `SubModel` | 159 | a material-specific model to draw instead of the material's own |
| `BoundBoxLength` | 151 | the length the material's bounding box is fitted to |
| `ForceScaleMode` | 19 | the record's `Scale` must be used as given |

The two records that describe one attachment are on opposite sides and both are read: the weapon's names the socket, the
material's gives its own rotation and size. The material's orientation is its own: a class entry that names no rotation
leaves the material in the weapon's orientation, which is what a fused object looks like when it sits flat on the weapon.

### 10.3 The sizes

`fuseInfo` is **not** a visual scale for a fused part. Every fused part in the blueprints measured carries it above 1 (a
shield 1.40, spears 1.50) while every unfused part carries 1.0, and in game a fused weapon does not grow: the material is
attached to it. It scales the part itself, which is why `part_matrix_ex` applies it to a plain part and skips it for one that
carries a fused material.

A material's size comes from its record, per weapon class:

1. `BoundBoxLength` names a length: the material is scaled so its own largest side is that length.
2. Otherwise `Scale` and `ExScale` multiply, as `bp_attach_matrix` applies them. `ForceScaleMode` says the record's `Scale`
   must be used as given.
3. Otherwise the material is **capped** to the weapon: shrunk when it is bigger, left alone when it is not.

Step 3 is a cap and not a fit. A fit resizes every material that legitimately names no size, including ones smaller than
their weapon; a cap leaves those alone. The ratio the cap produces is the one the game's own records use where they do name a
size: the oversized ones carry `Scale` 0.22 to 0.30, and a 5.01 m statue on a 1.16 m shield comes out at 0.232.

The size is read per class because the record is per class. The Gerudo statue names `BoundBoxLength` 0.41 for swords and
nothing for a shield, so on a short sword it is 0.41 m and on a shield the cap decides.

### 10.3.1 The material is placed by the socket

`bp_attach_matrix` applies the record's `RotateOffset` and `Scale`/`ExScale`. It does not apply `TransOffset`: that is a
per-class nudge authored against one weapon, and reading it as the position puts a material off its weapon along the shaft.
Its along-the-weapon component is centimetres against a weapon measured in metres (0.029 m on a 2.08 m spear), so it is a
hand adjustment, not a placement.

The position comes from the weapon model's own skeleton. The record's `BoneName` names a socket on it, and that is where the
game hangs the material:

| weapon | socket | position | the hidden blade it anchors |
|---|---|---|---|
| `Weapon_Spear_050` | `Bind_Z` | z 1.6477 | blade z 1.642..2.227 |
| `Weapon_Spear_129` | `Bind_Z` | z 1.677 | blade base 1.674, shaft top 1.680 |
| `Weapon_Lsword_147` | `Bind_Z` | z 0.231 | blade z 0.196..1.607 |
| `Weapon_Sword_052` | `Bind_Z` | z 0.148 | blade base 0.148, body top 0.206 |
| `Weapon_Shield_103` | `Bind_C` | (0.054, -0.117, 0.003) | no `_Hide` shape, so no seam |

That table is the useful fact: **the socket sits at the base of the blade the fusion hides**, on every weapon measured. A
class that names a `SubModel` ships it with its root authored at z = 0.000 (`Item_Enemy_151_Sub_Lsword` spans z
0.000..2.615), which says the same thing from the other side: the model is authored to start at the socket. So the material
stands in for the blade it replaces, and the material's root - the low end of its model along the weapon's long axis - is
what goes on the socket. A class with no `SubModel` uses the material's own model, whose root is wherever that model authors
it (the Lynel horn's is at z -0.660); subtracting the root is what makes the two cases one rule.

`fuse_placement` in `runtime/ui/preview3d.cpp` does this once per actor, when the pair is uploaded, and caches the matrix. It
is per actor and not per part: a blueprint can hold the same weapon twice, each with its own material, and those two sit at
different coordinates.

**Still open: how far along the weapon the material should run.** The code currently lifts the material's root from the
socket to the weapon's visible end. Placed against the decoded meshes by that rule, a spear's horn spans z 1.285..2.662 on a
weapon whose visible box ends at 1.678: about two thirds of the horn lies past the tip and the rest overlaps the shaft, so
it reads as a horn passing through a spear rather than as a weapon made of one. Candidates measured the same way, all with
the root on the socket:

| rule | spear: horn span | overlap with the weapon's visible box |
|---|---|---|
| root on the socket, no lift | z 1.648..3.025 | 0.031 m - almost none of it on the weapon |
| root lifted to the visible end (current) | z 1.285..2.662 | 0.393 m |
| material box centre on the weapon's box centre | z -0.023..1.355 | 1.378 m (all of it) |
| material's far end on the weapon's tip | z 0.300..1.678 | 1.378 m (all of it) |

The horn is 1.378 m at the record's 0.55 against a 2.082 m spear, so it fits inside the weapon and every rule above is
geometrically possible - which is why this needs settling against the game rather than by argument. The `TransOffset` does
not answer it (see above). Whichever rule is chosen has to make both a spear and a long sword read as one object, and the
long sword's material is 1.569 m against a weapon whose visible part is only 0.873 m long, so it must overhang its weapon
wherever it is put.


### 10.4 What the fused part hides

A fused weapon keeps its handle and loses its blade, and the model says which shapes those are: an FSHP's name carries its
material after `__` (`Weapon_Lsword_147_Blade__Mt_Blade_Hide`), and a shape drawn with one of the model's `_Hide` materials is
the part a fusion replaces. `shape_is_hidden_blade` in `runtime/ui/preview3d.cpp` tests exactly that, so it works for any
weapon without a list of items. The material is drawn in its place.

A shield has no `_Hide` shape, so nothing is hidden on it. That is also why the placement for a shield has no seam to measure
from: its material is placed by closing the air between the two boxes on each axis.

### 10.5 Which combinations the game defines

A material's AttachmentParam names its fields per class, and an absent entry means the game defines nothing for that pairing.
Whether a class accepts a material is decided by what its `AttachParam<Class>` entry names, and nothing else. A blueprint
holding such a part is drawn by the rules above rather than refused.

### 10.6 Contact

Fuse joins two things by contact, unlike Ultrahand, which is a glued joint: the material has to lie against its weapon
rather than beside it. Section 10.3.1 is where that is decided, and its open question is the part still not settled - a
material wider than the shaft wraps it when centred, which is what a fusion looks like, but how far along the weapon it
runs is not yet right (see the table there).

The material is not scaled to fit the weapon's geometry. Fitting one box onto another is a different operation, and it
stretches the material; a material keeps the size section 10.3 gives it.
