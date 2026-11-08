// Checks runtime/blueprint_attach.c against the game's own data, without the game: the RomFS calls are stubbed to read
// the pack file tests/blueprint_attach_test.py extracted, so the module's own chain walk and key choice are what is tested.
//
// Built and run by tests/blueprint_attach_test.py.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint_attach.h"

// The stub's root, set by the harness before any resolution: where Pack/Actor/*.pack.zs lives.
void harness_set_root(const char *root);

static int failures;
static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: %s <dir with Pack/Actor/*.pack.zs>\n", argv[0]);
        return 2;
    }
    harness_set_root(argv[1]);

    BpAttach a;
    // A Lynel horn fused onto a spear: the material's own record carries the offset and the 0.55 scale, and its template
    // carries the socket and the rotation, so this exercises the parent chain.
    const int got = bp_attach_resolve("Weapon_Spear_029", "Item_Enemy_151", &a);
    check(got == 1, "a spear with a Lynel horn resolves");
    if (got) {
        printf("     bone=%s trans=(%.4f %.4f %.4f) rot=(%.2f %.2f %.2f) scale=%.4f model=%s\n", a.bone, a.trans[0],
               a.trans[1], a.trans[2], a.rot_deg[0], a.rot_deg[1], a.rot_deg[2], a.scale, a.model);
        check(a.bone[0] != 0, "the socket bone comes from the template");
        check(a.bone[0] == 'B', "the socket is a Bind_* bone");
        check(a.scale > 0.5f && a.scale < 0.6f, "the material scale is the template's 0.55, not the weapon's fuseInfo");
        check(a.trans[1] > 0.5f, "the offset along the weapon is the material's own 0.517");
        check(!strcmp(a.model, "Item_Enemy_151"), "the model is the material itself when it names no SubModel");
        check(a.has_sub_model == 0, "a spear\'s horn brings no replacement model of its own");
    }

    // The same horn on a long sword: the material has a SubModel for that class, and a different template.
    BpAttach b;
    if (bp_attach_resolve("Weapon_Lsword_047", "Item_Enemy_151", &b)) {
        printf("     lsword: bone=%s scale=%.4f model=%s\n", b.bone, b.scale, b.model);
        check(!strcmp(b.model, "Item_Enemy_151_Sub_Lsword"), "a long sword draws the material's SubModel");
        check(b.has_sub_model == 1, "a long sword loses its blade: the horn brings a replacement model");
    } else {
        check(0, "a long sword with the same horn resolves");
    }

    // A part that is not a weapon has no fused material by this rule.
    BpAttach c;
    check(bp_attach_resolve("SpObj_GolemHead_A_01", "Item_Enemy_151", &c) == 0, "a non-weapon resolves nothing");

    // The Gerudo "hole cover" fused onto a shield: the whole statue (head and its six-faced base are one model). Its shield
    // record carries no rotation and no scale, so the general rule says it must come out in the weapon's own orientation at
    // its own size - a fuse joins two objects that keep their own size, and the weapon's fuseInfo is the weapon's.
    BpAttach st;
    if (bp_attach_resolve("Weapon_Shield_103", "Obj_GerudoHoleCover_A_03", &st)) {
        printf("     statue on a shield: bone='%s' trans=(%.4f %.4f %.4f) rot=(%.1f %.1f %.1f) scale=%.4f x %.4f %.4f %.4f bbl=%.3f\n",
               st.bone, st.trans[0], st.trans[1], st.trans[2], st.rot_deg[0], st.rot_deg[1], st.rot_deg[2], st.scale,
               st.scale_xyz[0], st.scale_xyz[1], st.scale_xyz[2], st.bound_box_length);
        // This material names no size for a shield, so nothing may be invented for it: it reports no scale of its own, and
        // the draw then fits it to the weapon. Borrowing the sword class's number is exactly the per-object patch this rule
        // replaces.
        check(st.has_own_scale == 0, "a material with no size for this class reports no scale of its own");
        check(st.bound_box_length == 0.0f, "the sword class's fitted length does not leak into the shield class");
        check(st.rot_deg[0] == 0.0f && st.rot_deg[1] == 0.0f && st.rot_deg[2] == 0.0f,
              "a material with no rotation for this class keeps the weapon's own orientation");
        check(st.scale == 1.0f && st.scale_xyz[0] == 1.0f && st.scale_xyz[1] == 1.0f && st.scale_xyz[2] == 1.0f,
              "a material with no scale for this class is drawn at its own size");
        // ...but its sword class names a BoundBoxLength, which is how a huge object is kept readable on a small weapon, so
        // that one must be read: the fit is what shrinks it, not a guess.
        BpAttach sw;
        if (bp_attach_resolve("Weapon_Sword_001", "Obj_GerudoHoleCover_A_03", &sw)) {
            printf("     statue on a short sword: bound_box_length=%.3f scale=%.3f\n", sw.bound_box_length, sw.scale);
            check(sw.bound_box_length > 0.3f && sw.bound_box_length < 0.5f, "the sword class's BoundBoxLength is read");
        } else {
            check(0, "the statue fused onto a long sword resolves");
        }
    } else {
        check(0, "the statue fused onto a shield resolves");
    }

    printf("blueprint attach: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}