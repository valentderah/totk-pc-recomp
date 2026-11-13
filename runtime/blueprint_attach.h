// Where a fused material hangs on the part it was fused to: the socket bone, the offset and rotation about the weapon's
// own axes, and the scale the material is drawn at. See runtime/blueprint_attach.c for where the game keeps this.
#pragma once
#include <stddef.h>

typedef struct {
    int found;                 // 1 when the game's data had a record for this weapon class
    char bone[96];             // the weapon model's socket bone, e.g. "Bind_W"; "" when the record names none
    float trans[3];            // offset in metres, along the weapon's own axes
    float rot_deg[3];          // rotation about X, Y and Z, in degrees
    // The material's own scale. `scale` is the uniform ExScale, `scale_xyz` the per-axis Scale vector; both default to 1
    // and both are applied. A material the data gives no scale for is drawn at its own model size.
    float scale;
    float scale_xyz[3];
    // The length the material's bounding box is scaled to when the record names one (BoundBoxLength); 0 when it does not.
    // This is the ONLY size limit the record can place on a material, and it is per weapon class: a class that names none
    // draws the material at its own model size.
    float bound_box_length;
    // 1 when the entry names a SubModel for this class, i.e. the material brings a model of its own and replaces the part of
    // the weapon it covers. Only then are the weapon's `_Hide` shapes dropped; otherwise the weapon keeps its shape and the
    // material is added to it.
    int has_sub_model;
    char model[96];            // the model to draw: a material-specific SubModel, else the material's own actor name
} BpAttach;

// Resolves the record for `material` fused onto `weapon`. Returns 1 when the game's data described the attachment, 0 when
// there is no record (an unknown weapon class, a material without one, or no RomFS). On 0, out is zeroed with scale 1.
int bp_attach_resolve(const char *weapon, const char *material, BpAttach *out);

// The placement of the material in the WEAPON MODEL's own space, ready to be multiplied by the part's blueprint matrix:
// the socket bone's world transform, then the record's rotation, scale and offset. `bone_world` is the bone matrix as
// bfres_bone_world returns it (row-major 4x4). Returns nothing; `out` is always filled (an identity when the bone could not
// be found, which still draws the material somewhere visible instead of dropping it).
// `fit_scale` multiplies the material's own scale: a record that names a BoundBoxLength is drawn fitted to that length,
// and the caller passes bound_box_length / the model's own extent. Pass 1.0 for no fitting.
void bp_attach_matrix(const BpAttach *a, const float bone_world[16], float fit_scale, float out[16]);