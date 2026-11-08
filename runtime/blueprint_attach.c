// Where a fused material hangs on the part it was fused to, and how big it is drawn.
//
// A part that went through Fuse before it was picked up and put into a blueprint is two objects in the game: the weapon or
// shield, and the material welded onto it. The CAI records both - the record's SubName names the material - but it does not
// say where the material sits: for every record whose SubName is set the Sub matrix is the identity, while the records
// without one carry a real Sub matrix (that one belongs to the actor's own collision sub-shapes).
//
// The placement is in the game's data instead, in the material's own AttachmentParam component:
//
//   Component/AttachmentParam/<Material>.game__component__AttachmentParam.bgyml
//     AttachParamSpear_ / AttachParamSword_ / AttachParamLSword_ / AttachParamShield
//       BoneName      the weapon model's socket, e.g. "Bind_W"  (usually inherited from a base template)
//       TransOffset   metres, along the weapon's own axes
//       RotateOffset  degrees about X, Y and Z
//       ExScale       the material's scale, with UseExScale saying whether to apply it (0.55 for a Lynel horn - this is
//                     NOT the weapon's fuseInfo, which scales the weapon, not the material)
//       SubModel      a material-specific model to draw instead of the material's own (a horn has one for long swords)
//     $parent         the template this record inherits from, which is another member of the same pack
//
// A derived record often carries only its own TransOffset and inherits the socket and the rotation from the template, so
// the chain is walked from the material up to the root and then read from the root back down, letting the derived record
// override only what it names.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint_attach.h"
#include "blueprint_shape.h"
#include "byml.h"

#define ATTACH_NAME 96
#define ATTACH_CHAIN 4   // the material's record, its template, and the template's template: the data uses two hops

// The key the four weapon classes use. Which one applies is decided by the weapon's actor name, because that is where the
// game's own naming puts it.
static const char *weapon_class_key(const char *weapon) {
    if (!weapon) return NULL;
    if (!strncmp(weapon, "Weapon_Spear_", 13)) return "AttachParamSpear_";
    if (!strncmp(weapon, "Weapon_Lsword_", 14)) return "AttachParamLSword_";
    if (!strncmp(weapon, "Weapon_Sword_", 13)) return "AttachParamSword_";
    if (!strncmp(weapon, "Weapon_Shield_", 14)) return "AttachParamShield";
    return NULL;
}

// The name a member or a SubModel path ends in, without its folder or extension:
// "Work/Model/.../Item_Enemy_151_Sub_Lsword.fmdb" becomes "Item_Enemy_151_Sub_Lsword", and
// "Component/AttachmentParam/General.game__component__AttachmentParam.bgyml" becomes "General".
static void member_name(const char *path, char *out, size_t cap) {
    out[0] = 0;
    if (!path || !*path) return;
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    snprintf(out, cap, "%s", base);
    for (char *p = out; *p; p++) {
        if (!strncmp(p, ".game__", 7)) { *p = 0; return; }
        if (*p == '.' && p != out && !strncmp(p, ".fmdb", 5)) { *p = 0; return; }
    }
}

// A hash entry's value as a number, or fallback when the entry is absent.
static double num_or(const BymlDoc *doc, uint32_t hash, const char *key, double fallback) {
    const uint32_t v = byml_key(doc, hash, key);
    return v ? byml_number(doc, v, fallback) : fallback;
}

typedef struct {
    const char *actor;     // the actor name being resolved
    const char *want;      // the member name this pass is looking for
    const char *class_key;
    BpAttach *rec;         // filled by the pass that finds the record
    char sub_path[256];    // the SubModel path, when the record names one
    char parent[ATTACH_NAME];  // the template the record names, "" when it names none
    int is_record;         // 1 when this pass found the wanted member
} AttachPass;

// One SARC member: if it is the wanted attachment record, note its template and read its class entry; otherwise nothing.
static int attach_member(const char *name, const uint8_t *data, size_t size, void *vctx) {
    AttachPass *c = vctx;
    char base[ATTACH_NAME];
    member_name(name, base, sizeof base);
    if (strcmp(base, c->want)) return 1;

    BymlDoc doc;
    if (!byml_open(&doc, data, size)) return 1;
    const uint32_t root = byml_root(&doc);
    if (!root) return 1;
    c->is_record = 1;
    const uint32_t parent = byml_key(&doc, root, "$parent");
    if (parent) {
        const char *pv = byml_string(&doc, parent);
        member_name(pv, c->parent, sizeof c->parent);
    }
    const uint32_t at = byml_key(&doc, root, c->class_key);
    if (at) {
        BpAttach *r = c->rec;
        const uint32_t bone = byml_key(&doc, at, "BoneName");
        if (bone) {
            const char *s = byml_string(&doc, bone);
            if (s && *s) snprintf(r->bone, sizeof r->bone, "%s", s);
        }
        const uint32_t trans = byml_key(&doc, at, "TransOffset");
        if (trans) {
            r->trans[0] = (float)num_or(&doc, trans, "X", 0);
            r->trans[1] = (float)num_or(&doc, trans, "Y", 0);
            r->trans[2] = (float)num_or(&doc, trans, "Z", 0);
        }
        const uint32_t rot = byml_key(&doc, at, "RotateOffset");
        if (rot) {
            r->rot_deg[0] = (float)num_or(&doc, rot, "X", 0);
            r->rot_deg[1] = (float)num_or(&doc, rot, "Y", 0);
            r->rot_deg[2] = (float)num_or(&doc, rot, "Z", 0);
        }
        const uint32_t ex = byml_key(&doc, at, "ExScale");
        if (ex) {
            const float v = (float)byml_number(&doc, ex, 1.0);
            r->scale = v > 0.0f ? v : 1.0f;   // a zero or negative scale would collapse or mirror the material
        }
        // A per-axis Scale is a separate field from ExScale, and a lot of items use only this one. It multiplies.
        const uint32_t sc = byml_key(&doc, at, "Scale");
        if (sc) {
            static const char *const axis[3] = {"X", "Y", "Z"};
            for (int i = 0; i < 3; i++) {
                const float v = (float)num_or(&doc, sc, axis[i], 1.0);
                r->scale_xyz[i] = v > 0.0f ? v : 1.0f;
            }
        }
        // The length a fused material's bounding box is scaled to. This is the game's own answer to "a rock on a shield must
        // not look absurd": the material is fitted to a box of this size instead of keeping its world size.
        const uint32_t bbl = byml_key(&doc, at, "BoundBoxLength");
        if (bbl) {
            const float v = (float)byml_number(&doc, bbl, 0.0);
            r->bound_box_length = v > 0.0f ? v : 0.0f;
        }
        // Any of the three is the data taking charge of the size; none of them means the material is fitted to its weapon.
        if (r->bound_box_length > 0.0f || r->scale != 1.0f || r->scale_xyz[0] != 1.0f || r->scale_xyz[1] != 1.0f ||
            r->scale_xyz[2] != 1.0f)
            r->has_own_scale = 1;
        const uint32_t sub = byml_key(&doc, at, "SubModel");
        if (sub) {
            const char *s = byml_string(&doc, sub);
            if (s && *s) {
                snprintf(c->sub_path, sizeof c->sub_path, "%s", s);
                r->has_sub_model = 1;   // the material brings its own model: the weapon's covered shapes make way for it
            }
        }
    }
    return 1;
}

// Runs one pass over the pack for member `want`. Returns 1 when the member was there, and fills `parent` with the template
// it named.
static int read_member(const uint8_t *pack, size_t pack_n, const char *want, const char *class_key, BpAttach *rec,
                       char *sub_path, size_t sub_cap, char *parent, size_t parent_cap) {
    AttachPass c;
    memset(&c, 0, sizeof c);
    c.want = want;
    c.class_key = class_key;
    c.rec = rec;
    sarc_each_matching(pack, pack_n, "Component/AttachmentParam/", "", attach_member, &c);
    if (sub_path && c.sub_path[0]) snprintf(sub_path, sub_cap, "%s", c.sub_path);
    snprintf(parent, parent_cap, "%s", c.parent);
    return c.is_record;
}

// Reads one actor's record for a weapon class, walking its $parent chain inside its own pack: a template supplies the
// defaults and the actor's own record overrides only the fields it names.
static int read_actor_record(const char *actor, const char *class_key, BpAttach *rec, char *sub_path, size_t sub_cap) {
    size_t pack_n = 0;
    uint8_t *pack = bp_actor_pack(actor, &pack_n);
    if (!pack) return 0;

    char chain[ATTACH_CHAIN][ATTACH_NAME];
    char here[ATTACH_NAME];
    snprintf(here, sizeof here, "%s", actor);
    int hops = 0;
    int found = 0;
    while (hops < ATTACH_CHAIN) {
        char parent[ATTACH_NAME] = "";
        if (!read_member(pack, pack_n, here, class_key, rec, sub_path, sub_cap, parent, sizeof parent)) break;
        found = 1;
        if (!parent[0] || !strcmp(parent, here)) break;   // no template, or one that names itself
        snprintf(chain[hops], sizeof chain[hops], "%s", here);
        snprintf(here, sizeof here, "%s", parent);
        hops++;
    }
    // `here` is the root now. Read it first, then each record down towards the actor, so the deepest override wins.
    if (found) {
        char none[ATTACH_NAME] = "";
        read_member(pack, pack_n, here, class_key, rec, sub_path, sub_cap, none, sizeof none);
        for (int i = hops - 1; i >= 0; i--)
            read_member(pack, pack_n, chain[i], class_key, rec, sub_path, sub_cap, none, sizeof none);
    }
    free(pack);
    return found;
}

int bp_attach_resolve(const char *weapon, const char *material, BpAttach *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    out->scale = 1.0f;
    out->scale_xyz[0] = out->scale_xyz[1] = out->scale_xyz[2] = 1.0f;
    if (!weapon || !material || !*material) return 0;
    const char *class_key = weapon_class_key(weapon);
    if (!class_key) return 0;   // not a weapon or a shield: this rule has nothing to say about it

    // The material's own record: its socket name comes from a template in the same pack, and its own fields (offset,
    // scale, SubModel) override that template's.
    char sub_path[256] = "";
    if (!read_actor_record(material, class_key, out, sub_path, sizeof sub_path)) return 0;
    out->found = 1;
    if (sub_path[0]) member_name(sub_path, out->model, sizeof out->model);
    else snprintf(out->model, sizeof out->model, "%s", material);

    // The weapon's record names the socket this class of material hangs on. A material that names none of its own still
    // hangs there, which is what makes it sit on the weapon rather than at its origin; but the weapon's ROTATION is not
    // taken, because a fused material keeps its own orientation. (A record that does carry a rotation is unaffected: its
    // own value is already in out.)
    if (!out->bone[0]) {
        BpAttach socket;
        memset(&socket, 0, sizeof socket);
        socket.scale = 1.0f;
        socket.scale_xyz[0] = socket.scale_xyz[1] = socket.scale_xyz[2] = 1.0f;
        if (read_actor_record(weapon, class_key, &socket, NULL, 0) && socket.bone[0])
            snprintf(out->bone, sizeof out->bone, "%s", socket.bone);
    }
    return 1;
}

// ---- the placement matrix --------------------------------------------------------------------------------------------

// Row-major 4x4, out = a * b. The same convention bone_world uses, so these compose directly.
static void mat4_mul(const float a[16], const float b[16], float out[16]) {
    float t[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
            t[r * 4 + c] = s;
        }
    memcpy(out, t, sizeof t);
}

// A rotation of `deg` degrees about the three axes, applied X then Y then Z, row-major.
static void mat4_rot_deg(const float deg[3], float out[16]) {
    const float rx = deg[0] * 0.01745329252f, ry = deg[1] * 0.01745329252f, rz = deg[2] * 0.01745329252f;
    const float cx = cosf(rx), sx = sinf(rx), cy = cosf(ry), sy = sinf(ry), cz = cosf(rz), sz = sinf(rz);
    // R = Rz * Ry * Rx, written out so this stays a rotation composition rather than three matrix multiplies.
    out[0] = cz * cy;
    out[1] = cz * sy * sx - sz * cx;
    out[2] = cz * sy * cx + sz * sx;
    out[3] = 0;
    out[4] = sz * cy;
    out[5] = sz * sy * sx + cz * cx;
    out[6] = sz * sy * cx - cz * sx;
    out[7] = 0;
    out[8] = -sy;
    out[9] = cy * sx;
    out[10] = cy * cx;
    out[11] = 0;
    out[12] = out[13] = out[14] = 0;
    out[15] = 1;
}

void bp_attach_matrix(const BpAttach *a, const float bone_world[16], float fit_scale, float out[16]) {
    static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float rot[16], scale[16], trans[16];
    memset(out, 0, sizeof(float) * 16);
    out[0] = out[5] = out[10] = out[15] = 1.0f;
    if (!a || !a->found) return;

    mat4_rot_deg(a->rot_deg, rot);
    memcpy(scale, ident, sizeof scale);
    const float fit = fit_scale > 0.0f ? fit_scale : 1.0f;
    const float s = (a->scale > 0.0f ? a->scale : 1.0f) * fit;
    scale[0] = s * (a->scale_xyz[0] > 0.0f ? a->scale_xyz[0] : 1.0f);
    scale[5] = s * (a->scale_xyz[1] > 0.0f ? a->scale_xyz[1] : 1.0f);
    scale[10] = s * (a->scale_xyz[2] > 0.0f ? a->scale_xyz[2] : 1.0f);
    memcpy(trans, ident, sizeof trans);
    trans[3] = a->trans[0];
    trans[7] = a->trans[1];
    trans[11] = a->trans[2];

    // The material's own transform about the socket is T * R * S; the socket puts that whole assembly on the weapon.
    float local[16], tmp[16];
    mat4_mul(rot, scale, tmp);
    mat4_mul(trans, tmp, local);
    mat4_mul(bone_world ? bone_world : ident, local, out);
}