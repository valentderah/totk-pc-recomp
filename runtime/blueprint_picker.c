// The blueprint file picker's state and logic, in C: the folder it shows, the entries it lists, the path parts a player
// can press, and the hit tests that turn a pointer position or a pad button into an action. Drawing is the only part in
// C++ (ui/blueprint_picker_draw.cpp), through a small bridge that hands the rectangles and the text to ImGui's draw list
// without using ImGui's widget or layout system at all.
//
// That split exists for a reason: the earlier picker was built from ImGui widgets (InvisibleButton, SameLine,
// SetCursorScreenPos) and ImGui's own error checks aborted the game when the layout did not match what it expected. Here
// every rectangle is computed by this file and handed back, so the picker cannot break ImGui's invariants, and the
// interaction (pointer position, hover, press) is decided here rather than by ImGui.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/blueprint_picker.h"

#include "bfres.h"
#include "blueprint_attach.h"
#include "blueprint_dir.h"
#include "blueprint_file.h"
#include "blueprint_shape.h"
#include "cfg.h"
#include "host.h"
#include "input.h"
#include "model_romfs.h"
#include "input_map.h"
#include "input_wsys.h"
#include "platform/platform.h"
#include "ui_text.h"

enum {
    PICK_MAX_ENTRIES = 512,
    PICK_PATH_MAX = PLAT_PATH_MAX,
    PICK_MAX_PARTS = 16,
    PICK_MAX_ROWS = PICK_MAX_ENTRIES + PICK_MAX_PARTS + 4,
    PICK_ROW_H = 38,        // one listed entry, in UI pixels at 1080p
    PICK_PART_H = 32,       // one breadcrumb block
    PICK_GAP = 8,
    PICK_PAD = 20,          // panel padding, roomier than the old cramped 10
    PICK_ROW_GAP = 4,       // breathing room between list rows
    PICK_HEADER_H = 62,     // the title band
    PICK_CRUMB_H = 44,      // the breadcrumb band
    PICK_FOOTER_H = 56,     // the bottom action bar
};

// One listed entry: a folder, a file, or another drive offered at the end of a drive.
typedef struct {
    char name[256];
    int dir;
    int root;   // 1 for a drive row: name is the root path itself ("C:/")
} PickEntry;

static int g_open;
static char g_dir[PICK_PATH_MAX];
static PickEntry g_entries[PICK_MAX_ENTRIES];
static int g_count;
static char g_error[128];
static BpPickDone g_done;
static void *g_ctx;
static int g_at_root;
static int g_scroll;
// ONE focus, whichever device moved it. The pointer and the pad both write this, and every action reads it, so a player
// who moves the mouse and then presses A acts on the row the mouse is on - the same row a click would take. Two
// separate "current" values (a hover row and a pad row) made the two devices disagree about what was selected, which is
// exactly the kind of mismatch a player notices.
static int g_focus_row;      // the entry index that is focused, -1 when none
static int g_focus_part;     // the path part index that is focused, -1 when none
static int g_pressed_part;   // a path part pressed this frame, -1 none
static uint64_t g_pad_prev;  // pad buttons at the previous poll, for the edge
static int g_part_count;
static char g_part[PICK_MAX_PARTS][256];
static char g_part_target[PICK_MAX_PARTS][PICK_PATH_MAX];
// Wheel notches fed by the window thread (bp_picker_wheel) and taken by the present thread (bp_picker_input). The
// right-button "go up" request is a flag for the same reason: list_folder() walks the disk and writes the picker's
// globals, so it must run on the present thread, never on the window thread that receives the click.
static int32_t g_wheel;
static int32_t g_back_pending;
// A scrollbar drag: the pointer's y within the thumb when the drag began, so the thumb does not jump under the cursor.
static int g_dragging, g_drag_grab_y;
// A camera drag inside the preview: the pointer at the previous frame, so each frame turns the view by how far the pointer
// moved. Kept apart from the scrollbar drag because the two live in different parts of the panel.
static int g_cam_dragging, g_cam_last_x, g_cam_last_y;
// 1 once the pointer has moved this session: a pointer parked on a row must not keep pulling the focus back to that row
// while the pad walks the list, and it must not swap the preview when it rests in the preview column.
static int g_pointer_moved;
static int g_pointer_x, g_pointer_y;

// ---- the preview of the file under the pointer or the pad cursor
//
// Reading a blueprint means opening the file and parsing it, so it is done once per file, not per frame: the cache is
// keyed by the file's full path and only rebuilt when the highlighted entry changes.
//
// The CAI format stores each part's placement (a rotation and a position) and nothing else - no meshes - so the preview
// is a 3D view of boxes rather than of models: every part becomes a small box at its own position, turned by its own
// rotation, and the whole thing is projected through one fixed camera. That is honest about what the file contains and
// still reads as the shape of the build, which a flat scatter could not.
// One projected face: its four corners in the view box (0..1) and how lit it is. Faces are painted far to near, which
// is the whole depth solution a draw list needs - it has no depth buffer. A triangle folds its fourth corner onto the
// third, so one quad fill paints both.
typedef struct {
    float x[4], y[4];
    float shade;
    float depth;   // mean corner depth, for the sort only
    int approx;    // 1 when the face came from a collision shape rather than the part's own model
    int color;     // the actor's palette index, as the game's own viewer colours a build (0..20)
} PickFace;

// One distinct part name and how often the blueprint uses it.
typedef struct {
    char name[96];
    int count;
} PickKind;

typedef struct {
    char path[PICK_PATH_MAX];      // the file the data below belongs to; "" when nothing is read yet
    char good[PICK_PATH_MAX];      // the last file that actually loaded: a refused file must not blank the view
    int ok;                        // 1: a real blueprint; 0: refused (note says why)
    int actors;                    // parts, when ok
    char note[96];                 // the file name when ok, else the reason it cannot be imported
    int kinds;                     // distinct part names, most used first
    PickKind kind[BPC_PICK_MAX_KINDS];
    int nfaces;
    PickFace face[BPC_PICK_MAX_FACES];   // far first
    // What the faces below were projected with. Turning the build changes the rotation stamp and the framing, and a
    // window resize changes the aspect, so the cache is only good while all three still agree and a turn re-projects the
    // same file instead of needing a new path to invalidate it.
    unsigned rot_stamp;
    float cam_yaw, cam_pitch, cam_zoom;
    float aspect;                  // the view's width/height, so a window resize also re-projects
    BpcBlueprint bp;               // the parsed file, kept across turns: a drag must not re-read it from disk
} PickPreview;
static PickPreview g_preview;

// The camera: a three-quarter view, looking down at the build from slightly above and to one side. A blueprint is built
// on a plane (its bbox is wide in x/z and thin in y), so this is the angle that shows the layout - the same reasoning a
// player would use when turning a build in the Autobuild screen. The default is 35 degrees of yaw and 28 of pitch; the
// pointer can drag it round (a picker-wide setting, kept across files so a player who turned one build keeps that view).
static struct { float yaw, pitch, zoom; } g_cam = {0.610865f, 0.488692f, 2.4f};
// The build's own rotation, accumulated by dragging, in row-major order and applied to blueprint space before the camera.
// The camera itself does not move: a drag turns the BUILD. That is what makes a drag keep working all the way round - a
// camera carried on two angles (yaw and pitch) has a pole, and past it the picture turns over, so a drag that follows the
// pointer has to reverse and then feels stuck. A rotation matrix has no pole: dragging up keeps tipping the build the
// same way for as long as the button is held.
static float g_rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
static unsigned g_rot_stamp;   // bumped on every change, so the cached face list and GPU image know they are stale
// The bounding box of the ROTATED build, recomputed each frame: the camera frames this instead of the flat layout box, so
// a build stays inside the panel at every angle instead of being clipped when it tips towards the viewer.
static float g_rot_lo[3], g_rot_hi[3];
// The orbit's leftover momentum, in radians, replayed and decayed after the button is released. three.js' OrbitControls
// calls this damping and it is what gives a drag weight: the build keeps turning for a fraction of a second and settles,
// instead of stopping dead the instant the pointer stops. Releasing fast therefore carries the view a little further,
// which is the behaviour people expect from every 3D viewer they have used.
static float g_cam_vel_yaw, g_cam_vel_pitch;
#define BP_CAM_DAMPING 0.12f   // the fraction of the leftover turn applied per step; OrbitControls' default is 0.05
// The 3D view's aspect ratio (width / height). A square view is 1; a tall view shrinks the x scale so a round part stays
// round instead of being stretched to the window. Set by layout_preview before the projection runs.
static float g_preview_aspect = 1.0f;
#define BP_CAM_DEFAULT_YAW 0.610865f
#define BP_CAM_DEFAULT_PITCH 0.488692f
// The default framing fills the panel: 0.36 * 2.4 is about 0.86 of the normalised half-extent, so a build is readable
// on open instead of needing a wheel first. The wheel zooms freely in both directions; only a floor against zero and
// negative values remains, because a zero or negative zoom would flip the projection inside out.
#define BP_CAM_DEFAULT_ZOOM 2.4f

// The preview's camera and what it shows are the picker's own state, not a frame's: a frame is a throwaway copy for the
// draw layer, so keeping the camera there meant a pad move or a drag changed data that was then discarded, and the
// renderer never learned its image was stale.
static struct {
    int shown;                 // 1 when the preview is up
    unsigned stamp;            // rises when the file or the camera changes; the renderer invalidates on it
    char path[PICK_PATH_MAX];  // the file on show
    int actors, modelled, approximated, hidden;
} g_view;

// 1 when the draw layer renders the preview on the GPU (ui/preview3d.cpp). The CPU projection and face list then have no
// consumer, and doing them anyway cost more than the whole preview.
static int g_gpu_preview;

void bp_picker_use_gpu(int on) { g_gpu_preview = on != 0; }
int bp_picker_gpu_in_use(void) { return g_gpu_preview; }
unsigned bp_picker_preview_stamp(void) { return g_view.stamp; }

// ---- the build's rotation ---------------------------------------------------------------------------------------------
// Row-major 3x3 matrices: out = a * b, and a vector turns as v' = m * v.
static void mat3_mul(const float a[9], const float b[9], float out[9]) {
    float t[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            t[i * 3 + j] = a[i * 3 + 0] * b[0 * 3 + j] + a[i * 3 + 1] * b[1 * 3 + j] + a[i * 3 + 2] * b[2 * 3 + j];
    memcpy(out, t, sizeof t);
}

// A rotation of `ang` radians about `axis` (a unit vector), right-handed.
static void mat3_axis_angle(const float axis[3], float ang, float out[9]) {
    const float c = cosf(ang), s = sinf(ang), t = 1.0f - c;
    const float x = axis[0], y = axis[1], z = axis[2];
    out[0] = c + x * x * t;      out[1] = x * y * t - z * s;  out[2] = x * z * t + y * s;
    out[3] = x * y * t + z * s;  out[4] = c + y * y * t;      out[5] = y * z * t - x * s;
    out[6] = x * z * t - y * s;  out[7] = y * z * t + x * s;  out[8] = c + z * z * t;
}

static void part_point(const BpcBlueprint *bp, int k, const float v[3], float out[3]);

// Composes one rotation about a world axis onto the build's own turn. The step happens in the world, so it multiplies on
// the left of whatever the build had turned to already.
static void rotate_about(const float axis[3], float ang) {
    if (ang == 0.0f) return;
    float step[9];
    mat3_axis_angle(axis, ang, step);
    mat3_mul(step, g_rot, g_rot);
    g_rot_stamp++;
    g_view.stamp++;
}

// The two world axes a drag turns about, fixed for as long as the camera is: the world's up and the camera's right.
static void drag_axes(float up[3], float right[3]) {
    up[0] = 0.0f; up[1] = 1.0f; up[2] = 0.0f;
    right[0] = cosf(g_cam.yaw); right[1] = 0.0f; right[2] = sinf(g_cam.yaw);
}

// Turns the build by one drag step. `dx` and `dy` are pointer pixels; the horizontal turn is about the world's up axis and
// the vertical one about the camera's right vector, so both follow the pointer exactly, at every orientation and with no
// pole to run into.
static void rotate_build(float dx, float dy, float per_px) {
    float up[3], right[3];
    drag_axes(up, right);
    rotate_about(up, -dx * per_px);
    rotate_about(right, -dy * per_px);
}

// The bounding box of the build as it is now turned. The camera frames this, so nothing is clipped at any angle.
static void rotated_bounds(const BpcBlueprint *bp, float lo[3], float hi[3]) {
    for (int a = 0; a < 3; a++) {
        lo[a] = 1e30f;
        hi[a] = -1e30f;
    }
    for (int c = 0; c < 8; c++) {
        const float p[3] = {(c & 1) ? bp->bbox_max[0] : bp->bbox_min[0],
                            (c & 2) ? bp->bbox_max[1] : bp->bbox_min[1],
                            (c & 4) ? bp->bbox_max[2] : bp->bbox_min[2]};
        for (int a = 0; a < 3; a++) {
            const float w = g_rot[a * 3 + 0] * p[0] + g_rot[a * 3 + 1] * p[1] + g_rot[a * 3 + 2] * p[2];
            if (w < lo[a]) lo[a] = w;
            if (w > hi[a]) hi[a] = w;
        }
    }
}

// The rotation and the rotated bounds, for the GPU renderer: it has to frame and turn the build the same way the CPU
// projection does, or the picture would jump between the two paths.
void bp_picker_preview_rot(float out[9]) { memcpy(out, g_rot, sizeof g_rot); }
void bp_picker_preview_bounds(float lo[3], float hi[3]) {
    memcpy(lo, g_rot_lo, sizeof g_rot_lo);
    memcpy(hi, g_rot_hi, sizeof g_rot_hi);
}
// The blueprint on show, for the GPU renderer (ui/preview3d.cpp). Its storage is preview_load's own static, so the
// pointer is only valid until another file is loaded.
static const BpcBlueprint *g_shown;
const BpcBlueprint *bp_picker_blueprint(void) { return g_preview.ok ? g_shown : NULL; }

// Returns the point in 0..1 of the view box (x right, y down) and its depth, larger being further from the camera.
// The point arrives already turned by the build's own rotation, and is framed by that rotation's own bounds, so the build
// fills the panel at every angle.
static void preview_project(const float p[3], float *sx, float *sy, float *depth) {
    // Centre on the box's middle and scale all three axes by ONE factor, the box's largest side. Normalising each axis by
    // its own span stretched the build: a round wheel came out oval. One factor keeps the proportions; only the overall
    // size is normalised.
    float centre[3], half = 0.0f;
    for (int i = 0; i < 3; i++) {
        centre[i] = 0.5f * (g_rot_lo[i] + g_rot_hi[i]);
        const float span = g_rot_hi[i] - g_rot_lo[i];
        if (span > half) half = span;
    }
    half *= 0.5f;
    if (half < 0.0005f) half = 0.0005f;
    // Turn the point by the build's own rotation, then frame it in the turned box.
    const float rp[3] = {
        g_rot[0] * p[0] + g_rot[1] * p[1] + g_rot[2] * p[2],
        g_rot[3] * p[0] + g_rot[4] * p[1] + g_rot[5] * p[2],
        g_rot[6] * p[0] + g_rot[7] * p[1] + g_rot[8] * p[2],
    };
    float n[3];
    for (int i = 0; i < 3; i++) n[i] = (rp[i] - centre[i]) / half;
    const float cy = cosf(g_cam.yaw), syn = sinf(g_cam.yaw);
    const float cp = cosf(g_cam.pitch), sp = sinf(g_cam.pitch);
    const float x1 = cy * n[0] + syn * n[2];
    const float z1 = -syn * n[0] + cy * n[2];
    const float y1 = cp * n[1] - sp * z1;
    const float z2 = sp * n[1] + cp * z1;
    const float d = 4.0f;                          // camera distance in normalised units
    const float k = d / (d + z2);                  // perspective: near parts grow, far ones shrink
    // The x scale is divided by the aspect ratio, so a round part stays round in a window that is not square: the view
    // box is normalised to -1..1 in x and y, but it is drawn over width x height pixels, so one unit of y covers more
    // pixels than one unit of x in a tall window.
    const float scale = 0.36f * g_cam.zoom;
    const float sxscale = scale / (g_preview_aspect > 0.01f ? g_preview_aspect : 1.0f);
    *sx = 0.5f + x1 * sxscale * k;
    *sy = 0.5f - y1 * scale * k;
    *depth = z2;
}

// A part vertex in blueprint space: fuseInfo scales the whole part, the rotation (row-major) turns it, the position moves it.
// `placement_only` leaves fuseInfo out, for the material fused onto the part (fuseInfo is the weapon's own scale).
static void part_point_ex(const BpcBlueprint *bp, int k, const float v[3], int placement_only, float out[3]) {
    // A part joined by Ultrahand glue is scaled by its fuseInfo (see part_matrix_ex in ui/preview3d.cpp); a Fuse-made part
    // and a fused material are not.
    int bonded = 0;
    if (!placement_only)
        for (int b = 0; b < bp->bonds; b++)
            if (bp->bond[b].src == k || bp->bond[b].tgt == k) { bonded = 1; break; }
    const float s = bonded ? bp->scale[k] : 1.0f;
    const float p[3] = {v[0] * s, v[1] * s, v[2] * s};
    for (int i = 0; i < 3; i++)
        out[i] = bp->pos[k][i] + bp->rot[k][i][0] * p[0] + bp->rot[k][i][1] * p[1] + bp->rot[k][i][2] * p[2];
}

static void part_point(const BpcBlueprint *bp, int k, const float v[3], float out[3]) {
    part_point_ex(bp, k, v, 0, out);
}

// One triangle, projected and emitted into the face list, with the actor's own colour and a face normal lit by the key
// light. No back-face culling: several of these models are single-sided sheets (sails, flags, nets) that must show from
// both sides, and the painter's order still hides the far side behind the near one.
static void emit_triangle(const BpcBlueprint *bp, int k, const float *a, const float *b, const float *c, int approx) {
    if (g_preview.nfaces >= BPC_PICK_MAX_FACES) return;
    float wa[3], wb[3], wc[3];
    part_point(bp, k, a, wa);
    part_point(bp, k, b, wb);
    part_point(bp, k, c, wc);
    float q[4][2];
    float depth = 0.0f;
    preview_project(wa, &q[0][0], &q[0][1], &depth);
    float d2 = 0.0f, d3 = 0.0f;
    preview_project(wb, &q[1][0], &q[1][1], &d2);
    preview_project(wc, &q[2][0], &q[2][1], &d3);
    depth = (depth + d2 + d3) / 3.0f;
    // The face's normal from the world-space cross product, lit by a fixed key light.
    float u[3] = {wb[0] - wa[0], wb[1] - wa[1], wb[2] - wa[2]};
    float v[3] = {wc[0] - wa[0], wc[1] - wa[1], wc[2] - wa[2]};
    float nrm[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    const float nl = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
    const float light[3] = {0.40f, 0.82f, 0.41f};
    float lit = nl > 1e-6f ? (nrm[0] * light[0] + nrm[1] * light[1] + nrm[2] * light[2]) / nl : 1.0f;
    if (lit < 0.0f) lit = -lit;   // two-sided: a face turned away is lit as brightly as one turned toward the light
    float shade = 0.30f + 0.70f * lit;
    if (shade < 0.12f) shade = 0.12f;
    if (shade > 1.0f) shade = 1.0f;
    PickFace *dst = &g_preview.face[g_preview.nfaces++];
    for (int i = 0; i < 3; i++) {
        dst->x[i] = q[i][0];
        dst->y[i] = q[i][1];
    }
    dst->x[3] = q[2][0];   // fold the fourth corner on the third: one quad fill paints a triangle
    dst->y[3] = q[2][1];
    dst->shade = shade;
    dst->depth = depth;
    dst->approx = approx;
    dst->color = k % 21;
}

// A fused material, drawn on the socket the game names for it. Same source of truth as the GPU path
// (runtime/blueprint_attach.c), so the two previews agree: the socket is a bone of the weapon model, and the material's
// own rotation, scale and offset go under it.
static void emit_fused_material(const BpcBlueprint *bp, int k, const BfresModel *weapon, int budget) {
    const char *sub = bp->sub_names[k];
    if (!sub || !*sub || !weapon) return;
    BpAttach at;
    if (!bp_attach_resolve(bp->names[k], sub, &at)) return;
    const BfresModel *mat = model_romfs_resolve_model(at.model);
    if (!mat) return;

    float bone[16], place[16];
    float fit = 1.0f;
    {
        const float mat_extent = model_romfs_extent(mat);
        if (at.bound_box_length > 0.0f && mat_extent > 0.0005f) {
            fit = at.bound_box_length / mat_extent;
        } else if (!at.has_own_scale && mat_extent > 0.0005f) {
            const float weapon_extent = model_romfs_extent(weapon);
            if (weapon_extent > 0.0005f) fit = weapon_extent / mat_extent;
        }
    }
    const int bi = bfres_bone_find(weapon, at.bone);
    if (bi < 0 || !bfres_bone_world(weapon, bi, bone)) {
        // No socket found: the material still gets drawn at the part's own origin, which is visibly wrong rather than
        // silently missing - a missing material is the bug this whole path exists to fix.
        memset(bone, 0, sizeof bone);
        bone[0] = bone[5] = bone[10] = bone[15] = 1.0f;
    }
    bp_attach_matrix(&at, bone, fit, place);

    const int limit = g_preview.nfaces + budget;
    for (int s = 0; s < mat->count && g_preview.nfaces < limit; s++) {
        const int ntris = bfres_shape_triangles(mat, s);
        const int nverts = bfres_shape_vertices(mat, s);
        if (ntris <= 0 || nverts <= 0) continue;
        float *verts = malloc((size_t)nverts * 3 * sizeof(float));
        uint16_t *tris = malloc((size_t)ntris * 3 * sizeof(uint16_t));
        if (!verts || !tris) { free(verts); free(tris); continue; }
        const int vc = bfres_shape_geometry(mat, s, verts, nverts, tris, ntris);
        if (vc > 0) {
            // Into the weapon's model space: the material's vertices go through the socket once, then the part's own
            // blueprint placement applies to all of them as usual. The part's fuseInfo is the WEAPON's scale, so it is
            // divided back out here: a fuse joins two objects that each keep their own size.
            const float inv_scale = 1.0f;   // parts are drawn at their own size, so nothing has to be divided out
            for (int v = 0; v < vc; v++) {
                const float p[3] = {verts[3 * v], verts[3 * v + 1], verts[3 * v + 2]};
                for (int a = 0; a < 3; a++)
                    verts[3 * v + a] = (place[a * 4 + 0] * p[0] + place[a * 4 + 1] * p[1] + place[a * 4 + 2] * p[2] +
                                        place[a * 4 + 3]) * inv_scale;
            }
            for (int t = 0; t < ntris && g_preview.nfaces < limit; t++) {
                const uint16_t *idx = tris + (size_t)t * 3;
                emit_triangle(bp, k, verts + (size_t)idx[0] * 3, verts + (size_t)idx[1] * 3, verts + (size_t)idx[2] * 3,
                              0);
            }
        }
        free(verts);
        free(tris);
    }
}

// One part's whole mesh, or its collision shapes when the pack has no model for it. The part may spend at most `budget`
// triangles, so a build of several detailed parts cannot starve the ones after it.
// A shape whose material is one of the model's `_Hide` materials: the blade of a weapon, which the game stops drawing once
// a material is fused onto it. The shape name carries its material after "__" (Weapon_Lsword_147_Blade__Mt_Blade_Hide), so
// the test is on the model's own naming and works for any weapon without a list of items.
static int shape_hidden_when_fused(const BfresModel *m, int index) {
    char name[128];
    if (!bfres_shape_name(m, index, name, sizeof name)) return 0;
    const char *mat = strstr(name, "__");
    return mat && strstr(mat, "_Hide") != NULL;
}

static void emit_part(const BpcBlueprint *bp, int k, int budget) {
    const char *nm = bp->names[k];
    if (!nm || !*nm) return;
    const int limit = g_preview.nfaces + budget;
    const int fused = bp->sub_names[k] && *bp->sub_names[k];
    // The weapon keeps its covered shapes unless the material brings a replacement model for this class (see
    // shape_hidden_when_fused and blueprint_attach.h).
    int replaces = 0;
    if (fused) {
        BpAttach probe;
        if (bp_attach_resolve(nm, bp->sub_names[k], &probe)) replaces = probe.has_sub_model;
    }

    // The runtime model source first: read the .bfres.mc from the RomFS and decode it. The BFRES reader hands back each
    // shape's vertices and indices, which are then projected like any other mesh.
    const BfresModel *model = model_romfs_resolve(nm);
    // An actor can be two models: its own and the `_Inside_Reduction_` one that fills it. Both belong to this part at the same
    // transform, exactly as the GPU path draws them.
    const BfresModel *inside = model ? model_romfs_resolve_inside(nm) : NULL;
    const BfresModel *models[2] = {model, inside};
    const int nmodel = inside ? 2 : 1;
    if (model) {
        g_view.modelled++;
        rt_log("blueprint: %s from runtime decode (%d shapes%s)", nm, model->count, inside ? " + interior" : "");
        for (int mi = 0; mi < nmodel; mi++)
        for (int s = 0; s < models[mi]->count && g_preview.nfaces < limit; s++) {
            if (replaces && shape_hidden_when_fused(models[mi], s)) continue;
            const int ntris = bfres_shape_triangles(models[mi], s);
            const int nverts = bfres_shape_vertices(models[mi], s);
            if (ntris <= 0 || nverts <= 0) continue;
            float *verts = malloc((size_t)nverts * 3 * sizeof(float));
            uint16_t *tris = malloc((size_t)ntris * 3 * sizeof(uint16_t));
            if (!verts || !tris) { free(verts); free(tris); continue; }
            const int vc = bfres_shape_geometry(models[mi], s, verts, nverts, tris, ntris);
            if (vc > 0) {
                // one diagnostic per shape: the bone transform should make a stick's Y extent the tall one
                if (!strcmp(nm, "SpObj_ControlStick_A_01")) {
                    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
                    for (int v = 0; v < vc; v++)
                        for (int a = 0; a < 3; a++) {
                            if (verts[3 * v + a] < lo[a]) lo[a] = verts[3 * v + a];
                            if (verts[3 * v + a] > hi[a]) hi[a] = verts[3 * v + a];
                        }
                    rt_log("blueprint: stick shape%d Y %.2f..%.2f X %.2f..%.2f", s, lo[1], hi[1], lo[0], hi[0]);
                }
                for (int t = 0; t < ntris && g_preview.nfaces < limit; t++) {
                    const uint16_t *idx = tris + (size_t)t * 3;
                    emit_triangle(bp, k, verts + (size_t)idx[0] * 3, verts + (size_t)idx[1] * 3,
                                  verts + (size_t)idx[2] * 3, 0);
                }
            }
            free(verts);
            free(tris);
        }
        // The material fused onto this part, drawn under the same part colour: without it a fused weapon or shield is a
        // bare one, which is the wrong object.
        emit_fused_material(bp, k, model, budget);
        return;
    }

    const BpShapeMesh *body = bp_shape_resolve(nm);
    if (body) {
        g_view.approximated++;
        for (int t = 0; t < body->ntris && g_preview.nfaces < limit && g_preview.nfaces < BPC_PICK_MAX_FACES; t++) {
            const uint16_t *idx = body->tris + (size_t)t * 3;
            emit_triangle(bp, k, body->verts + (size_t)idx[0] * 3, body->verts + (size_t)idx[1] * 3,
                          body->verts + (size_t)idx[2] * 3, 1);
        }
        return;
    }
    g_view.hidden++;
}

// ---- the folder

static int felt(const char *name, void *ctx) {
    (void)ctx;
    if (g_count >= PICK_MAX_ENTRIES) return 0;
    snprintf(g_entries[g_count].name, sizeof g_entries[g_count].name, "%s", name);
    g_entries[g_count].dir = 0;
    g_entries[g_count].root = 0;
    g_count++;
    return 1;
}

static int dled(const char *name, void *ctx) {
    (void)ctx;
    if (g_count >= PICK_MAX_ENTRIES) return 0;
    snprintf(g_entries[g_count].name, sizeof g_entries[g_count].name, "%s", name);
    g_entries[g_count].dir = 1;
    g_entries[g_count].root = 0;
    g_count++;
    return 1;
}

static int roots_added(const char *root, void *ctx) {
    (void)ctx;
    if (g_count >= PICK_MAX_ENTRIES) return 0;
    // The drive the player is already on is not offered.
    char here[8];
    snprintf(here, sizeof here, "%s", g_dir);
    size_t hl = strlen(here);
    while (hl > 1 && (here[hl - 1] == '/' || here[hl - 1] == '\\')) here[--hl] = 0;
    char there[8];
    snprintf(there, sizeof there, "%s", root);
    size_t tl = strlen(there);
    while (tl > 1 && (there[tl - 1] == '/' || there[tl - 1] == '\\')) there[--tl] = 0;
    if (!strcmp(here, there)) return 1;
    snprintf(g_entries[g_count].name, sizeof g_entries[g_count].name, "%s", root);
    g_entries[g_count].dir = 1;
    g_entries[g_count].root = 1;
    g_count++;
    return 1;
}

static int by_name(const void *a, const void *b) {
    const PickEntry *x = (const PickEntry *)a, *y = (const PickEntry *)b;
    if (x->root != y->root) return y->root - x->root;   // drives first
    if (x->dir != y->dir) return y->dir - x->dir;       // then folders
    return strcmp(x->name, y->name);
}

static int at_drive_root(const char *dir) {
    size_t n = strlen(dir);
    if (n == 0) return 0;
    while (n > 0 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) n--;
    if (n == 0) return 1;   // "/" on Linux
    if (n == 2 && dir[1] == ':') return 1;
    return 0;
}

// The folder above dir. An empty string means "This PC": the list of every drive, with no folder above it. At a drive
// root ("C:/") the parent is that empty string, and from a subfolder ("C:/Users") the parent is the drive root itself —
// written back with its slash so the picker lands on a browsable path, not the relative "C:".
static void parent_of(const char *dir, char *out, size_t n) {
    if (!out || n == 0) return;
    out[0] = 0;
    if (!dir || !*dir) return;                 // already at This PC
    char buf[PLAT_PATH_MAX];
    snprintf(buf, sizeof buf, "%s", dir);
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '/' || buf[len - 1] == '\\')) len--;
    buf[len] = 0;
    size_t cut = len;
    while (cut > 0 && buf[cut - 1] != '/' && buf[cut - 1] != '\\') cut--;
    if (cut == 0) return;                     // a drive root: its parent is This PC (the empty path)
    // Drop the separator, except for a drive root: "C:" alone is a relative path, so it must stay "C:/". A normal
    // parent ("C:/Users") is cleaner without the trailing slash and still browses the same folder.
    size_t end = cut;
    while (end > 0 && (buf[end - 1] == '/' || buf[end - 1] == '\\')) end--;
    buf[end] = 0;
    if (end == 2 && buf[1] == ':') {
        buf[2] = '/';
        buf[3] = 0;
    }
    snprintf(out, n, "%s", buf);
}

// Split the folder into pressable parts: "C:\Users\Administrator" gives C:, Users, Administrator, each with the folder
// it leads to. A drive keeps its colon, so the bar never shows a bare "C" next to "C:".
static void split_path(void) {
    g_part_count = 0;
    // "This PC" (the empty path) has no folder to name, so the breadcrumb shows only the back control: no stale drive
    // letter stays up there after the player goes up from a drive root.
    if (!g_dir[0]) return;
    const size_t len = strlen(g_dir);
    size_t start = 0;
    for (size_t i = 0; i < len && g_part_count < PICK_MAX_PARTS; i++) {
        const char c = g_dir[i];
        if (c != '/' && c != '\\') continue;
        if (i == start) {
            start = i + 1;
            continue;
        }
        size_t plen = i - start;
        if (plen >= sizeof g_part[0]) plen = sizeof g_part[0] - 1;
        memcpy(g_part[g_part_count], g_dir + start, plen);
        g_part[g_part_count][plen] = 0;
        const size_t tlen = i < (size_t)PICK_PATH_MAX ? i : (size_t)PICK_PATH_MAX - 1;
        memcpy(g_part_target[g_part_count], g_dir, tlen);
        g_part_target[g_part_count][tlen] = 0;
        g_part_count++;
        start = i + 1;
    }
    if (g_part_count < PICK_MAX_PARTS && start < len) {
        size_t plen = len - start;
        if (plen >= sizeof g_part[0]) plen = sizeof g_part[0] - 1;
        memcpy(g_part[g_part_count], g_dir + start, plen);
        g_part[g_part_count][plen] = 0;
        snprintf(g_part_target[g_part_count], PICK_PATH_MAX, "%s", g_dir);
        g_part_count++;
    }
}

// Reads the blueprint at path into g_preview, unless it is the file already cached. Failure is not an error: a file the
// parser refuses still gets a note saying so, which is exactly what the player needs to see before pressing anything.
static void preview_load(const char *path) {
    // The parsed blueprint is kept across re-projections, so turning the build re-projects what is already in memory
    // instead of reading the file again on every frame of a drag. It is only re-read when the path changes.
    static uint8_t storage[BPF_READ_MAX];
    static BpcBlueprint bp;
    static char parsed_path[PICK_PATH_MAX];

    if (!path || !*path) {
        g_preview.path[0] = 0;
        g_preview.ok = 0;
        g_preview.nfaces = 0;
        g_preview.actors = 0;
        g_preview.note[0] = 0;
        return;
    }
    // The cache holds one file under one turn of the build and one framing: a different file needs the geometry again, and
    // the same file after it was turned needs the projection again. Only the framing work below is shared by both.
    if (!strcmp(g_preview.path, path) && g_preview.cam_zoom == g_cam.zoom && g_preview.aspect == g_preview_aspect &&
        g_preview.rot_stamp == g_rot_stamp) return;

    snprintf(g_preview.path, sizeof g_preview.path, "%s", path);
    g_preview.ok = 0;
    g_preview.nfaces = 0;
    g_preview.actors = 0;
    enum BpcResult r;
    if (!strcmp(parsed_path, path)) {
        r = BPC_OK;   // the same file, already parsed: only the projection is stale
    } else {
        r = bp_file_read(path, storage, &bp);
        // Only a file that parsed is remembered: caching the path of a refused file would make the next frame claim it
        // succeeded, and the note would then be blank over an empty view.
        if (r == BPC_OK) {
            snprintf(parsed_path, sizeof parsed_path, "%s", path);
            g_shown = &bp;
        } else {
            parsed_path[0] = 0;
            g_shown = NULL;
        }
    }
    // The framing follows the turned build, so the CPU projection and the GPU renderer both frame what is on screen.
    if (r == BPC_OK) rotated_bounds(&bp, g_rot_lo, g_rot_hi);
    g_preview.rot_stamp = g_rot_stamp;
    g_preview.cam_yaw = g_cam.yaw;
    g_preview.cam_pitch = g_cam.pitch;
    g_preview.cam_zoom = g_cam.zoom;
    g_preview.aspect = g_preview_aspect;

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (r != BPC_OK) {
        // Say why once, then put the last good file back: the column keeps its picture and its drag target instead of
        // showing a note over an empty view (which read as "the preview is broken" while the picture was still there).
        rt_log("blueprint: preview: %s refused (%s); keeping the last file that loaded", path, bpc_result_text(r));
        if (g_preview.good[0] && strcmp(g_preview.good, path)) {
            preview_load(g_preview.good);
            return;
        }
        snprintf(g_preview.note, sizeof g_preview.note, "%s", bpc_result_text(r));
        return;
    }
    g_preview.ok = 1;
    g_preview.actors = bp.actors;
    snprintf(g_preview.good, sizeof g_preview.good, "%s", path);
    snprintf(g_preview.note, sizeof g_preview.note, "%s", base);
    rt_log("blueprint: preview: %s loaded (%d parts, %d faces%s)", path, bp.actors, g_preview.nfaces,
           g_gpu_preview ? ", GPU" : "");
    g_view.actors = bp.actors;
    g_view.shown = 1;
    if (strcmp(g_view.path, path)) {
        snprintf(g_view.path, sizeof g_view.path, "%s", path);
        g_view.stamp++;   // a different build is on show
    }

    // The part list: distinct names grouped by how often the build uses them, most used first. This is what the text
    // under the view shows; it comes from the file itself and is true of any CAI, model or not.
    g_preview.kinds = 0;
    for (int k = 0; k < bp.actors; k++) {
        const char *nm = bp.names[k];
        if (!nm || !*nm) continue;
        int at = -1;
        for (int m = 0; m < g_preview.kinds; m++)
            if (!strcmp(g_preview.kind[m].name, nm)) { at = m; break; }
        if (at < 0) {
            if (g_preview.kinds >= BPC_PICK_MAX_KINDS) continue;
            at = g_preview.kinds++;
            snprintf(g_preview.kind[at].name, sizeof g_preview.kind[at].name, "%s", nm);
            g_preview.kind[at].count = 0;
        }
        g_preview.kind[at].count++;
    }
    // Most used first, stable for equal counts.
    for (int a = 1; a < g_preview.kinds; a++) {
        const PickKind k = g_preview.kind[a];
        int b = a - 1;
        while (b >= 0 && g_preview.kind[b].count < k.count) { g_preview.kind[b + 1] = g_preview.kind[b]; b--; }
        g_preview.kind[b + 1] = k;
    }

    // With the GPU preview on, nothing reads the projected faces: the renderer reads the blueprint straight from
    // bp_picker_blueprint() and rasterises with a depth buffer. The whole projection pass below is skipped.
    if (g_gpu_preview) {
        g_preview.nfaces = 0;
        g_preview.cam_yaw = g_cam.yaw;
        g_preview.cam_pitch = g_cam.pitch;
        g_preview.cam_zoom = g_cam.zoom;
        g_preview.aspect = g_preview_aspect;
        return;
    }

    // Every part draws its own model, or its collision shapes when the pack has no model for it. Each part gets an equal
    // share of the remaining budget, so a build of several detailed parts cannot starve the ones after it - the earlier
    // first-come-first-served code hid a hanging board and four wheels because three carts and a cannon spent the whole
    // 24000-face budget first. A part that needs less than its share leaves the rest for the ones that follow.
    g_preview.nfaces = 0;
    g_view.modelled = g_view.approximated = g_view.hidden = 0;
    for (int k = 0; k < bp.actors; k++) {
        const int left = bp.actors - k;
        int share = (BPC_PICK_MAX_FACES - g_preview.nfaces) / (left > 0 ? left : 1);
        if (share < 128) share = 128;   // never below a visible sliver, even near the end of a large build
        emit_part(&bp, k, share);
    }
    // Insertion sort by descending depth.
    for (int i = 1; i < g_preview.nfaces; i++) {
        const PickFace p = g_preview.face[i];
        int j = i - 1;
        while (j >= 0 && g_preview.face[j].depth < p.depth) {
            g_preview.face[j + 1] = g_preview.face[j];
            j--;
        }
        g_preview.face[j + 1] = p;
    }
    g_preview.cam_yaw = g_cam.yaw;
    g_preview.cam_pitch = g_cam.pitch;
    g_preview.cam_zoom = g_cam.zoom;
    g_preview.aspect = g_preview_aspect;
}

static void list_folder(void) {
    g_count = 0;
    g_error[0] = 0;
    g_scroll = 0;
    g_focus_row = 0;
    g_at_root = at_drive_root(g_dir);
    // "This PC" (g_dir == "") is not a folder on disk: it is the list of every drive. Otherwise list the folder's own
    // subdirectories and blueprint files, plus the other drives when the current folder is a drive root.
    if (!g_dir[0]) {
        plat_root_list(roots_added, NULL);
    } else {
        if (!plat_dir_list(g_dir, dled, NULL)) {
            snprintf(g_error, sizeof g_error, "cannot list %s", g_dir);
            split_path();
            return;
        }
        plat_file_list(g_dir, ".cai", felt, NULL);
        plat_file_list(g_dir, ".txt", felt, NULL);
        if (g_at_root) plat_root_list(roots_added, NULL);
    }
    qsort(g_entries, (size_t)g_count, sizeof g_entries[0], by_name);
    split_path();
}

static void finish(const char *path) {
    BpPickDone done = g_done;
    void *ctx = g_ctx;
    g_open = 0;
    g_done = NULL;
    g_ctx = NULL;
    // The game gets its input back the way the settings menu gives it back: nothing reaches it until every button and
    // stick is released, so the press that closed the picker cannot act in the game.
    input_menu(0);
    // Remember the zoom for the next visit, and to totk.ini so it survives a restart.
    {
        char buf[32];
        snprintf(buf, sizeof buf, "%.4f", (double)g_cam.zoom);
        cfg_set("BLUEPRINT_ZOOM", buf);
        cfg_save();
    }
    if (done) done(path, ctx);
}

static void open_folder(const char *dir) {
    if (!dir) return;
    snprintf(g_dir, sizeof g_dir, "%s", dir);   // "" is "This PC", the list of drives
    list_folder();
}

// Go up one folder: from a subfolder to its parent, from a drive root to "This PC". At "This PC" itself there is
// nothing above it, so the press does nothing. Used by the breadcrumb button, the right mouse button and the pad's B.
static int go_up(void) {
    char up[PICK_PATH_MAX];
    parent_of(g_dir, up, sizeof up);
    if (!up[0] && !g_dir[0]) return 0;          // already at This PC: nothing above it
    if (!strcmp(up, g_dir)) return 0;            // no change (defensive)
    open_folder(up);                            // "" is This PC; "C:/" is a drive root
    return 1;
}

static void activate(int i) {
    if (i < 0 || i >= g_count) return;
    char path[PICK_PATH_MAX];
    // A drive root ("C:/") already ends in a separator, so joining it with "/" would make "C://name", which Windows
    // then cannot list. One separator is inserted only when the current folder does not already end in one.
    const char *sep = "";
    const size_t dl = strlen(g_dir);
    if (dl > 0 && g_dir[dl - 1] != '/' && g_dir[dl - 1] != '\\') sep = "/";
    if (g_entries[i].root) {
        // A drive row carries the root path itself ("C:/"), not a name below the current folder.
        snprintf(path, sizeof path, "%s", g_entries[i].name);
        open_folder(path);
    } else if (g_entries[i].dir) {
        snprintf(path, sizeof path, "%s%s%s", g_dir, sep, g_entries[i].name);
        open_folder(path);
    } else {
        snprintf(path, sizeof path, "%s%s%s", g_dir, sep, g_entries[i].name);
        finish(path);
    }
}

// ---- the API

int bp_picker_open(const char *start, BpPickDone done, void *ctx) {
    if (g_open) return 0;
    g_done = done;
    g_ctx = ctx;
    g_focus_row = g_focus_part = g_pressed_part = -1;
    g_pad_prev = 0;
    g_dragging = 0;
    __atomic_store_n(&g_wheel, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_back_pending, 0, __ATOMIC_RELEASE);
    // The folder to open at: the caller's own path, or the folder last browsed. bp_dir_last() is used, not cfg_get,
    // because it checks the folder still exists and returns "" when it does not - opening a deleted folder shows an
    // empty list with no way to tell why, and a bad path ("C://" from a hand-edited ini) fails to list at all.
    const char *from = start && *start ? start : bp_dir_last();
    snprintf(g_dir, sizeof g_dir, "%s", from && *from ? from : ".");
    list_folder();
    g_open = 1;
    input_menu(1);
    // The zoom the player left it at, from totk.ini: one zoom is enough, not one per visit.
    {
        const char *z = cfg_get("BLUEPRINT_ZOOM");
        if (z && *z) {
            const double v = atof(z);
            if (v > 0.001 && v < 100.0) g_cam.zoom = (float)v;
        }
    }
    rt_log("blueprint: picker opens at %s (%d entries)", g_dir, g_count);
    return 1;
}

int bp_picker_is_open(void) { return g_open; }

void bp_picker_cancel(void) {
    if (g_open) finish(NULL);
}

void bp_picker_wheel(int notches) {
    if (g_open && notches) __atomic_add_fetch(&g_wheel, (int32_t)notches, __ATOMIC_SEQ_CST);
}

void bp_picker_back(void) {
    if (g_open) __atomic_store_n(&g_back_pending, 1, __ATOMIC_RELEASE);
}

// The preview's camera (blueprint_picker.h): readable, settable, resettable, so a caller can restore the default view and
// a test can check that a drag moves it.
void bp_picker_camera(float *yaw, float *pitch) {
    if (yaw) *yaw = g_cam.yaw;
    if (pitch) *pitch = g_cam.pitch;
}

void bp_picker_camera_set(float yaw, float pitch) {
    g_cam.yaw = yaw;
    g_cam.pitch = pitch;   // no clamp: the view can tip past the poles and read the build from the other side
    g_view.stamp++;   // the camera changed: any rendered image is stale
}

void bp_picker_camera_reset(void) {
    g_cam.yaw = BP_CAM_DEFAULT_YAW;
    g_cam.pitch = BP_CAM_DEFAULT_PITCH;
    g_cam.zoom = BP_CAM_DEFAULT_ZOOM;
    // The build goes back to its authored orientation too: "reset the view" means the picture the player started from,
    // and after a few turns the default camera alone would not show that.
    static const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    memcpy(g_rot, ident, sizeof g_rot);
    g_rot_stamp++;
    g_cam_vel_yaw = g_cam_vel_pitch = 0.0f;
    g_view.stamp++;
}

float bp_picker_zoom(void) { return g_cam.zoom; }

void bp_picker_zoom_set(float zoom) {
    g_cam.zoom = zoom < 0.001f ? 0.001f : zoom;   // only a floor against zero/negative, no upper or lower limit
    g_view.stamp++;
}


// ---- layout: every rectangle the draw layer needs, in UI pixels

// How many rows fit in the list area. The rows are placed at (row_h + row_gap) pitch, so the wheel clamp, the pad
// cursor's window and the scrollbar must all use that pitch, not row_h alone, or the last rows scroll out of view.
static int visible_rows(const BPickFrame *f) {
    const int pitch = f->row_h + (int)(PICK_ROW_GAP * f->scale);
    return pitch > 0 ? (f->row_bottom - f->row_top) / pitch : 0;
}

// One rectangle, appended to the frame unless it is full. Every band builder goes through this so the capacity check
// exists in one place and no band can overflow the fixed array.
static BPickRect *add_rect(BPickFrame *f, int kind) {
    if (f->nrect >= PICK_MAX_ROWS) return NULL;
    BPickRect *r = &f->rect[f->nrect++];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    return r;
}

// Sizes the panel for this screen and writes the chrome's three bands. Returns the padding, which every band needs.
static int layout_panel(BPickFrame *f, int screen_w, int screen_h, float scale) {
    // The panel keeps the settings menu's proportions and its pixel scale, so it looks like part of the port on every
    // window size: roomy padding, a fixed three-band chrome (title / breadcrumb / footer) around a scrolling file list.
    f->scale = scale < 1.0f ? 1.0f : scale;
    f->row_h = (int)(PICK_ROW_H * f->scale);
    if (f->row_h < 28) f->row_h = 28;
    f->w = (int)(screen_w * 0.66f);
    if (f->w < 680) f->w = screen_w < 720 ? screen_w - 24 : 680;
    f->h = (int)(screen_h * 0.76f);
    if (f->h < 460) f->h = screen_h < 484 ? screen_h - 24 : 460;
    f->x = (screen_w - f->w) / 2;
    f->y = (screen_h - f->h) / 2;

    const float sc = f->scale;
    const int pad = (int)(PICK_PAD * sc);
    const int gap = (int)(PICK_GAP * sc);
    f->header_bottom = f->y + (int)(PICK_HEADER_H * sc);
    f->crumb_top = f->header_bottom;
    f->crumb_bottom = f->crumb_top + (int)(PICK_CRUMB_H * sc);
    f->footer_top = f->y + f->h - (int)(PICK_FOOTER_H * sc);
    f->row_top = f->crumb_bottom + gap;
    f->row_bottom = f->footer_top - gap;
    f->list_bottom = f->row_bottom;
    f->scroll = g_scroll;
    return pad;
}

// The header band's text: the title, the localised help, the "This PC" label and the live status line. Keeping the
// status here means an empty or unreadable folder always says so instead of showing a blank panel.
static void layout_header(BPickFrame *f) {
    f->count = g_count;
    f->error = g_error[0] != 0;
    f->at_pc = g_dir[0] == 0;
    snprintf(f->title, sizeof f->title, "%s", menu_text(UI_MENU_BLUEPRINT_PICK));
    snprintf(f->hint, sizeof f->hint, "%s", menu_text(UI_MENU_BLUEPRINT_PICK_HELP));
    snprintf(f->crumb, sizeof f->crumb, "%s", menu_text(UI_MENU_BLUEPRINT_THIS_PC));
    // The footer's button prompts: the pad keys the picker answers to, each as a key cap plus its word, the way the
    // game's own guide bar presents them. A is the accept key, so it is the one drawn in gold.
    //
    // Only keys that exist on a Switch pad are named. There is no "R" button to go up a folder: the pad goes up with B,
    // which also cancels, so the prompt says "B Cancel" and the breadcrumb's back arrow is the visible way up - naming a
    // button the player cannot find on the controller is worse than saying less.
    f->nprompt = 0;
    snprintf(f->prompt[f->nprompt].key, sizeof f->prompt[0].key, "A");
    snprintf(f->prompt[f->nprompt].label, sizeof f->prompt[0].label, "%s", menu_text(UI_MENU_BLUEPRINT_KEY_OPEN));
    f->prompt[f->nprompt].primary = 1;
    f->nprompt++;
    snprintf(f->prompt[f->nprompt].key, sizeof f->prompt[0].key, "B");
    snprintf(f->prompt[f->nprompt].label, sizeof f->prompt[0].label, "%s", menu_text(UI_MENU_BLUEPRINT_KEY_CANCEL));
    f->prompt[f->nprompt].primary = 0;
    f->nprompt++;
    // No scroll or page-turn prompts: the wheel scrolls the list and needs no introduction, and "turn a page" is not
    // something a file list does. The two prompts above are the only actions this screen has.
    if (g_error[0]) snprintf(f->status, sizeof f->status, "%s", g_error);
    else if (g_count == 0) snprintf(f->status, sizeof f->status, "%s", menu_text(UI_MENU_BLUEPRINT_EMPTY));
    else snprintf(f->status, sizeof f->status, menu_text(UI_MENU_BLUEPRINT_COUNT), g_count);
}

// The breadcrumb band: one compact back control, then the pressable path parts. This is the only navigation; there is
// no separate full-path box. A path too deep for the band is elided from the right.
static void layout_breadcrumb(BPickFrame *f, int pad) {
    const float sc = f->scale;
    const int gap = (int)(PICK_GAP * sc);
    const int part_h = (int)(PICK_PART_H * sc);
    const int right = f->x + f->w - pad;
    const int cy = f->crumb_top + ((f->crumb_bottom - f->crumb_top) - part_h) / 2;

    int cx = f->x + pad;
    const int upw = (int)(44 * sc);          // a compact "‹" back button
    BPickRect *r = add_rect(f, BP_RECT_BUTTON);
    if (r) {
        r->x = cx, r->y = cy, r->w = upw, r->h = part_h;
        snprintf(r->text, sizeof r->text, "%s", menu_text(UI_MENU_BLUEPRINT_UP));
    }
    cx += upw + gap;
    for (int k = 0; k < g_part_count; k++) {
        // Rough width from the label length; the draw layer measures text precisely for its chevron placement.
        const int w = (int)(strlen(g_part[k]) * 9 * sc) + (int)(24 * sc);
        if (cx + w > right && k > 0) break;
        r = add_rect(f, BP_RECT_PART);
        if (r) {
            r->x = cx, r->y = cy, r->w = w, r->h = part_h;
            r->last = k == g_part_count - 1;
            snprintf(r->text, sizeof r->text, "%s", g_part[k]);
        }
        cx += w + (int)(14 * sc);            // room for the "›" chevron
    }
}

// The scrolling list: one plate per visible entry, carrying the icon the draw layer should use. The icon is decided
// here from the entry's kind, so the draw layer never has to parse a label to tell a folder from a file.
static void layout_rows(BPickFrame *f, int pad) {
    const int row_gap = (int)(PICK_ROW_GAP * f->scale);
    const int inner = f->w - pad * 2;
    int y = f->row_top;
    for (int i = g_scroll; i < g_count && y + f->row_h <= f->row_bottom; i++) {
        BPickRect *r = add_rect(f, BP_RECT_ROW);
        if (!r) break;
        r->x = f->x + pad, r->y = y, r->w = inner, r->h = f->row_h;
        r->icon = g_entries[i].root ? BP_ICON_DRIVE : g_entries[i].dir ? BP_ICON_FOLDER : BP_ICON_FILE;
        snprintf(r->text, sizeof r->text, "%s", g_entries[i].name);
        y += f->row_h + row_gap;
    }
}

// The scrollbar's geometry, from the same state the wheel, the pad and a drag move. scroll_max is the largest first
// visible entry and scroll_range the thumb's travel in pixels; both are stored so the input pass reuses them instead of
// recomputing a second, possibly different, answer. preview_w is the width the preview column took from the rows (-1 and
// the scrollbar's own width are subtracted here), so nothing overlaps. Returns 1 when there is a scrollbar.
static int layout_scroll(BPickFrame *f, int pad, int preview_w) {
    const float sc = f->scale;
    const int gap = (int)(PICK_GAP * sc);
    const int visible = visible_rows(f);
    f->scroll_dragging = g_dragging;
    f->scroll_max = g_count > visible ? g_count - visible : 0;
    if (g_scroll > f->scroll_max) g_scroll = f->scroll_max;   // a resize can leave the old scroll past the end
    if (g_scroll < 0) g_scroll = 0;
    f->scroll = g_scroll;

    // Narrow the rows for whatever sits to their right. This happens whether or not there is a scrollbar, so the row
    // width is always one computed value rather than depending on the scrollbar being shown.
    const int right_edge = f->x + f->w - pad;
    const int track_w = (int)(6 * sc);
    for (int i = 0; i < f->nrect; i++) {
        if (f->rect[i].kind != BP_RECT_ROW) continue;
        int clip = preview_w > 0 ? preview_w + gap : 0;
        if (f->scroll_max > 0 && visible > 0) clip += track_w + gap;
        f->rect[i].w = right_edge - f->rect[i].x - clip;
        if (f->rect[i].w < 0) f->rect[i].w = 0;
    }
    if (f->scroll_max <= 0 || visible <= 0) return 0;

    // The track sits between the rows and the preview column.
    const int track_right = preview_w > 0 ? right_edge - preview_w - gap : right_edge;
    const int hit_w = (int)(22 * sc);   // a comfortable grab column around the thin drawn track
    f->scroll_track_x = track_right - track_w;
    f->scroll_track_y = f->row_top;
    f->scroll_track_w = track_w;
    f->scroll_track_h = f->row_bottom - f->row_top;
    f->scroll_hit_x = track_right - hit_w;
    f->scroll_hit_w = hit_w;
    const int thumb_h = f->scroll_track_h * visible / g_count;
    f->scroll_thumb_h = thumb_h < (int)(24 * sc) ? (int)(24 * sc) : thumb_h;
    f->scroll_range = f->scroll_track_h - f->scroll_thumb_h;
    if (f->scroll_range < 0) f->scroll_range = 0;
    // Round here and in the drag's inverse: truncating one way and the other disagrees by a row and makes the thumb
    // judder at the grab point.
    f->scroll_thumb_y = f->scroll_track_y + (f->scroll_range > 0
                            ? (int)((double)f->scroll_range * g_scroll / f->scroll_max + 0.5) : 0);
    return 1;
}

// The footer band: one clearly labelled Cancel button. The key hints are drawn beside it by the draw layer.
static void layout_footer(BPickFrame *f, int pad) {
    const int footer_h = f->footer_top > 0 ? (f->y + f->h - f->footer_top) : 0;
    BPickRect *r = add_rect(f, BP_RECT_BUTTON);
    if (r) {
        r->x = f->x + pad;
        r->y = f->footer_top + (footer_h - f->row_h) / 2;
        r->w = (int)(140 * f->scale), r->h = f->row_h;
        snprintf(r->text, sizeof r->text, "%s", menu_text(UI_MENU_BLUEPRINT_CANCEL));
    }
}

// One rectangle contains the point.
static int inside(const BPickRect *r, int px, int py) {
    return px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h;
}

// Which listed entry a point falls on, in entry indices, or -1. This reads the row rectangles the frame already holds,
// so it answers for the geometry of this frame instead of the hover flags of the previous one: the preview needs to be
// right on the very frame the pointer arrives, not one frame later.
static int row_at(const BPickFrame *f, int px, int py) {
    int rowseen = 0;
    for (int i = 0; i < f->nrect; i++) {
        const BPickRect *r = &f->rect[i];
        if (r->kind != BP_RECT_ROW) continue;
        if (inside(r, px, py)) return f->scroll + rowseen;
        rowseen++;
    }
    return -1;
}

// The preview column on the right of the list: a box for the part scatter and two lines of text saying what the file
// holds. Follows the row under the pointer, or the pad cursor's row when the pointer is over nothing: a player moving
// the mouse across the list sees each file's parts before they click, without having to move the pad first.
static int layout_preview(BPickFrame *f, int pad, int preview_w, int px, int py) {
    // The pointer chooses the file only when it is left of the preview column and actually moving. In this layout pass
    // the rows are still full width - layout_scroll narrows them after the preview is placed - so a point inside the
    // column would otherwise land on whichever row lies at that height, and the preview would jump from file to file
    // there. A pointer parked on a row while the pad walks the list must not keep pulling the preview back either.
    const int column_x = f->x + f->w - pad - preview_w;
    int idx = (px >= column_x || !g_pointer_moved) ? -1 : row_at(f, px, py);
    if (idx < 0) idx = g_focus_row;
    if (idx < 0 || idx >= g_count) return 0;

    char path[PICK_PATH_MAX];
    if (g_entries[idx].dir || g_entries[idx].root) {
        // A folder is not a blueprint, but passing over one on the way back into the column must not take the view away:
        // that left the column blank and un-draggable, and the panel claiming there was nothing to show. Keep the file that
        // is already up; the column then stays the same size and the same drag target.
        if (!g_preview.ok || !g_preview.path[0]) return 0;
        snprintf(path, sizeof path, "%s", g_preview.path);
        preview_load(path);
        /* falls through to the layout below, which draws whatever the preview holds */
        goto have_preview_path;
    }
    // Join without doubling a separator, as activate does: a drive root already ends in one.
    {
        const char *sep = "";
        const size_t dl = strlen(g_dir);
        if (dl > 0 && g_dir[dl - 1] != '/' && g_dir[dl - 1] != '\\') sep = "/";
        snprintf(path, sizeof path, "%s%s%s", g_dir, sep, g_entries[idx].name);
    }
have_preview_path: ;

    // The view's aspect ratio, so the projection can keep a round part round. It must be set BEFORE the projection runs.
    const float sc = f->scale;
    const int text_h = (int)(64 * sc);
    const int pad_in = (int)(12 * sc);
    const int vw = preview_w - pad_in * 2;
    const int vh = (f->row_bottom - f->row_top) - text_h - pad_in * 2;
    if (vw > 0 && vh > 0) g_preview_aspect = (float)vw / (float)vh;
    preview_load(path);

    const int right = f->x + f->w - pad;
    const int box_x = right - preview_w;
    const int box_y = f->row_top;

    f->preview = 1;
    // The 3D view occupies the upper part of the column; the text sits under it.
    f->preview_x = box_x + pad_in;
    f->preview_y = box_y + pad_in;
    f->preview_w = vw;
    f->preview_h = vh;
    if (f->preview_h < (int)(40 * sc)) f->preview_h = (int)(40 * sc);
    f->preview_faces = g_preview.nfaces;
    f->preview_kinds = g_preview.kinds;
    for (int k = 0; k < g_preview.kinds; k++) {
        snprintf(f->preview_kind[k].name, sizeof f->preview_kind[k].name, "%s", g_preview.kind[k].name);
        f->preview_kind[k].count = g_preview.kind[k].count;
    }
    for (int k = 0; k < g_preview.nfaces; k++) {
        for (int i = 0; i < 4; i++) {
            f->preview_face[k].x[i] = g_preview.face[k].x[i];
            f->preview_face[k].y[i] = g_preview.face[k].y[i];
        }
        f->preview_face[k].shade = g_preview.face[k].shade;
        f->preview_face[k].approx = g_preview.face[k].approx;
        f->preview_face[k].color = g_preview.face[k].color;
    }
    f->preview_modelled = g_view.modelled;
    f->preview_approx = g_view.approximated;
    f->preview_hidden = g_view.hidden;
    if (g_preview.ok) {
        snprintf(f->preview_title, sizeof f->preview_title, menu_text(UI_MENU_BLUEPRINT_PARTS), g_preview.actors);
    } else {
        snprintf(f->preview_title, sizeof f->preview_title, "%s", menu_text(UI_MENU_BLUEPRINT_INVALID));
    }
    snprintf(f->preview_note, sizeof f->preview_note, "%s", g_preview.note);
    return preview_w;
}

// Every rectangle and every derived value the draw layer and the input pass need, computed from the picker's state. The
// frame is pure derived data: nothing here is incrementally maintained between frames.
void bp_picker_layout_at(BPickFrame *f, int screen_w, int screen_h, float scale, int px, int py) {
    memset(f, 0, sizeof *f);
    const int pad = layout_panel(f, screen_w, screen_h, scale);
    // The preview column is part of the list band, on the right. It is sized from the panel and only reserved while a
    // file is highlighted, so a folder view keeps the whole width for its rows. Two fifths of the panel keeps the 3D view
    // big enough to read a build's parts while the rows still have most of the width for their names.
    const int preview_w = (int)(f->w * 0.42f);
    layout_header(f);
    layout_breadcrumb(f, pad);
    layout_rows(f, pad);
    const int used = layout_preview(f, pad, preview_w, px, py);   // 0 when nothing to preview
    layout_scroll(f, pad, used);                                  // after the rows and the preview: both narrow the rows
    layout_footer(f, pad);
}

void bp_picker_layout(BPickFrame *f, int screen_w, int screen_h, float scale) {
    bp_picker_layout_at(f, screen_w, screen_h, scale, -1, -1);
}

// ---- interaction: the pointer and the pad, decided here

// Turns this frame's input into hover state and into an action, and fills f's hovered/pressed flags. Returns 1 when the
// picker handled a press this frame (the draw layer then refreshes what it needs).
int bp_picker_input(BPickFrame *f, int px, int py) {
    int acted = 0;

    // The orbit's leftover momentum from the previous drag step. It decays geometrically and stops once it is too small
    // to see, so a fast release glides to a halt and a slow one barely moves - the damping behaviour of the orbit
    // controls every 3D viewer ships. While the button is still down the drag path below overwrites it, so this only
    // takes effect after a release. It replays the same two turns the drag made, about the same two axes, so the glide
    // continues the motion the hand started.
    if (!g_cam_dragging && (g_cam_vel_yaw != 0.0f || g_cam_vel_pitch != 0.0f)) {
        float up[3], right[3];
        drag_axes(up, right);
        rotate_about(up, g_cam_vel_yaw * BP_CAM_DAMPING);
        rotate_about(right, g_cam_vel_pitch * BP_CAM_DAMPING);
        g_cam_vel_yaw *= 1.0f - BP_CAM_DAMPING;
        g_cam_vel_pitch *= 1.0f - BP_CAM_DAMPING;
        // Below a thousandth of a radian per step the glide is invisible: stop it, so the view settles and the renderer
        // can go idle instead of redrawing for a turn nobody can see.
        if (fabsf(g_cam_vel_yaw) < 1e-5f) g_cam_vel_yaw = 0.0f;
        if (fabsf(g_cam_vel_pitch) < 1e-5f) g_cam_vel_pitch = 0.0f;
        acted = 1;
    }

    // A right click (bp_picker_back) is only flagged by the window thread; the folder walk happens here, on the present
    // thread that owns the picker's state, and the caller re-lays out from the new folder.
    if (__atomic_exchange_n(&g_back_pending, 0, __ATOMIC_SEQ_CST)) {
        if (go_up()) return 1;
    }

    // Which rectangles the pointer is over.
    for (int i = 0; i < f->nrect; i++) {
        BPickRect *r = &f->rect[i];
        if (!inside(r, px, py)) continue;
        r->hovered = 1;
    }
    // The pointer only takes the focus on a frame where it actually moved: a mouse parked on a row does not fight the
    // pad's own cursor. Once the pad steps, the pointer stays parked, so this stays 0 and the focus remains the pad's.
    g_pointer_moved = (px != g_pointer_x || py != g_pointer_y);
    g_pointer_x = px;
    g_pointer_y = py;
    // The focus follows the pointer only while the pointer is on something. When it drifts off the rows (over the
    // preview, the title, outside the panel) the focus stays where it was, so the pad can carry on from there instead of
    // having its selection silently cleared by a pointer that is not pointing at anything.
    const int under_row = row_at(f, px, py) >= 0;
    if (under_row && g_pointer_moved) {
        g_focus_row = row_at(f, px, py);
        g_focus_part = -1;
    } else {
        int partseen = 0, hit = -1;
        for (int i = 0; i < f->nrect; i++) {
            const BPickRect *r = &f->rect[i];
            if (r->kind != BP_RECT_PART) continue;
            if (r->hovered) hit = partseen;
            partseen++;
        }
        if (hit >= 0) {
            g_focus_part = hit;
        } else {
        }
    }

    // The pointer's left button. Its edge and its held state drive a click, a scrollbar drag and a camera drag; each drag
    // has priority over the click so pressing on a thumb or in the view never also activates what lies behind it.
    static int held_l;
    const int l = ikb_down(KEY_MOUSE_L);
    const int click = l && !held_l;
    held_l = l;

    // ---- camera drag: turning the preview. Checked before the scrollbar because a press inside the view must not be
    // read as a scrollbar grab if the two ever overlap, and because the view is the thing the player is aiming at.
    const int in_preview = f->preview && px >= f->preview_x && px < f->preview_x + f->preview_w &&
                           py >= f->preview_y && py < f->preview_y + f->preview_h;
    if (click && in_preview) {
        // A second click soon after the first puts the view back: the way out of a camera the player has lost track of,
        // without a button for it in the footer.
        static uint64_t last_click;
        const uint64_t now = plat_now_ns();
        if (now - last_click < 400ull * 1000000ull) {
            bp_picker_camera_reset();
            last_click = 0;
        } else {
            last_click = now;
        }
        g_cam_dragging = 1;
        g_cam_last_x = px;
        g_cam_last_y = py;
    }
    if (!l) g_cam_dragging = 0;
    if (g_cam_dragging) {
        // The build turns by the pointer's movement, one full circle per view height. Dividing by the height instead of the
        // width is what fixes the "dizzy" feel here - this panel is tall and narrow, so a width divisor made the same pixel
        // of movement turn the build nearly twice as far as it should.
        const float dx = (float)(px - g_cam_last_x), dy = (float)(py - g_cam_last_y);
        g_cam_last_x = px;
        g_cam_last_y = py;
        const int span = f->preview_h > 0 ? f->preview_h : (f->preview_w > 0 ? f->preview_w : 1);
        const float per_px = 6.28318531f / (float)span;
        rotate_build(dx, dy, per_px);   // the build turns, not the camera: no pole to run into, so a drag never locks
        // Remember this step as momentum, so a release carries the turn a little further instead of stopping dead.
        g_cam_vel_yaw = -dx * per_px;
        g_cam_vel_pitch = -dy * per_px;
        acted = 1;   // the projection changed, so the caller re-lays out and the faces are rebuilt
        return acted;
    }

    // ---- scrollbar drag: clicking the grab column starts it, moving the pointer while held maps y to the first visible
    // entry through the same scroll_max the wheel uses. g_drag_grab_y keeps the thumb from jumping to the cursor.
    const int in_column = f->scroll_track_w > 0 && px >= f->scroll_hit_x &&
                          px < f->scroll_hit_x + f->scroll_hit_w &&
                          py >= f->scroll_track_y && py < f->scroll_track_y + f->scroll_track_h;
    if (click && in_column) {
        g_dragging = 1;
        g_drag_grab_y = (py >= f->scroll_thumb_y && py < f->scroll_thumb_y + f->scroll_thumb_h)
                            ? py - f->scroll_thumb_y                       // grab the thumb where it was clicked
                            : f->scroll_thumb_h / 2;                       // click the track: centre on the cursor
    }
    if (!l) g_dragging = 0;
    if (g_dragging && f->scroll_range > 0 && f->scroll_max > 0) {
        const int thumb_top = py - g_drag_grab_y - f->scroll_track_y;
        int next = (int)((double)thumb_top * f->scroll_max / f->scroll_range + 0.5);
        if (next < 0) next = 0;
        if (next > f->scroll_max) next = f->scroll_max;
        if (next != g_scroll) {
            g_scroll = next;
            f->scroll = g_scroll;
            acted = 1;
        }
    }
    if (g_dragging) {
        // A pressed thumb is not a click on anything else. Consume the press edge and skip the row/button handling.
        if (click) return acted;
    } else if (click) {
        if (g_focus_part >= 0) g_pressed_part = g_focus_part;
        else if (g_focus_row >= 0) {
            activate(g_focus_row);
            return 1;
        }
        // The buttons are found by their text-free position: up is the first BP_RECT_BUTTON, cancel the last.
        for (int i = 0; i < f->nrect; i++) {
            BPickRect *r = &f->rect[i];
            if (r->kind != BP_RECT_BUTTON || !r->hovered) continue;
            if (!strcmp(r->text, menu_text(UI_MENU_BLUEPRINT_UP))) {
                if (go_up()) return 1;
            } else if (!strcmp(r->text, menu_text(UI_MENU_BLUEPRINT_CANCEL))) {
                bp_picker_cancel();
                return 1;
            }
        }
    }
    if (g_pressed_part >= 0) {
        if (g_pressed_part < g_part_count && g_part_target[g_pressed_part][0] &&
            plat_dir_exists(g_part_target[g_pressed_part])) {
            open_folder(g_part_target[g_pressed_part]);
            g_pressed_part = -1;
            return 1;
        }
        g_pressed_part = -1;
    }

    // The mouse wheel scrolls the list. The notches come from the window system's WM_MOUSEWHEEL / SDL_EVENT_MOUSE_WHEEL
    // events (bp_picker_wheel), the same ones the overlay receives, not from raw input: raw wheel messages stop arriving
    // once the cursor capture is released for the picker's pointer. Whole notches are taken since the last frame and the
    // visible row count decides how far it may go so the list never scrolls past its end.
    int32_t wheel = __atomic_exchange_n(&g_wheel, 0, __ATOMIC_SEQ_CST);
    if (wheel) {
        // Over the 3D view the wheel zooms the camera, the way the reference viewer's orbit controls do; over the list
        // it scrolls, so neither meaning is lost.
        if (in_preview) {
            bp_picker_zoom_set(g_cam.zoom * (wheel > 0 ? 1.15f : 1.0f / 1.15f));
        } else {
            g_scroll -= wheel;   // away from the player scrolls down
            if (g_scroll > f->scroll_max) g_scroll = f->scroll_max;
            if (g_scroll < 0) g_scroll = 0;
            f->scroll = g_scroll;
        }
        acted = 1;
    }

    // ---- the pad. It drives the SAME focus the pointer does, so the two never disagree: a mouse that leaves a row
    // focused and a D-pad press continue from there, and A acts on whatever the pointer last pointed at.
    uint64_t pad = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    if (input_pad_sticks(&pad, &lx, &ly, &rx, &ry)) {
        const uint64_t down = pad & ~g_pad_prev;
        g_pad_prev = pad;
        // ---- the two sticks.
        // The left one walks the list exactly as the D-pad does, from a threshold with a repeat delay so a held stick steps
        // at a readable rate instead of flying through the files. The right one turns the preview the way a drag does: a
        // rate applied every frame while it is held, so the build keeps turning for as long as the stick stays over.
        {
            static uint64_t stick_repeat_ns;
            const int32_t kStickOn = 16000;      // ~half deflection: far enough that a resting stick does nothing
            const uint64_t kRepeat = 90000000u;   // one step per 90 ms while the stick is held
            if (g_count > 0 && (ly > kStickOn || ly < -kStickOn)) {
                const uint64_t now = plat_now_ns();
                if (stick_repeat_ns == 0 || now - stick_repeat_ns > kRepeat) {
                    stick_repeat_ns = now;
                    const int from = g_focus_row < 0 ? 0 : g_focus_row;
                    g_focus_row = ly < 0 ? (from + 1) % g_count : (from + g_count - 1) % g_count;
                    const int visible = visible_rows(f);
                    if (visible > 0) {
                        if (g_focus_row < g_scroll) g_scroll = g_focus_row;
                        if (g_focus_row >= g_scroll + visible) g_scroll = g_focus_row - visible + 1;
                        if (g_scroll > f->scroll_max) g_scroll = f->scroll_max;
                        if (g_scroll < 0) g_scroll = 0;
                        f->scroll = g_scroll;
                    }
                    acted = 1;
                }
            } else {
                stick_repeat_ns = 0;
            }
            // L2 / R2 zoom the preview, as a rate while held: the wheel's own field, with its floor against zero intact.
            if (pad & ((1ull << B_ZL) | (1ull << B_ZR))) {
                const float step = 0.02f;
                if (pad & (1ull << B_ZR)) g_cam.zoom *= 1.0f + step;
                if (pad & (1ull << B_ZL)) g_cam.zoom /= 1.0f + step;
                if (g_cam.zoom < 0.001f) g_cam.zoom = 0.001f;
                g_view.stamp++;   // the zoom changed: any rendered image is stale
                acted = 1;
            }
            // The folder walk is a pair of shoulders, not the stick: a stick nudged while the player is lining up a row fired
            // the step again and again. A shoulder is one press, so one press is one action. L1 up a folder, R1 into the row's
            // folder (R2 belongs to the zoom).
            if (down & (1ull << B_L)) {
                if (go_up()) return 1;
                acted = 1;
            }
            if (down & (1ull << B_R)) {
                if (g_focus_row >= 0 && g_focus_row < g_count &&
                    (g_entries[g_focus_row].dir || g_entries[g_focus_row].root)) {
                    activate(g_focus_row);
                    return 1;
                }
                acted = 1;
            }
            if (f->preview && (rx > kStickOn || rx < -kStickOn || ry > kStickOn || ry < -kStickOn)) {
                // A rate, in the same units the drag uses: a full deflection turns the build about 130 degrees a second.
                const int span = f->preview_h > 0 ? f->preview_h : 1;
                const float per_frame = 6.28318531f / (float)span * 2.2f;
                rotate_build((float)rx / 32767.0f, -(float)ry / 32767.0f, per_frame);
                acted = 1;
            }
        }
        // Once the pad moves, it owns the focus: the pointer is no longer what the player is driving.
        const int moved = (down & ((1ull << B_DOWN) | (1ull << B_UP))) != 0;
        if (g_count > 0 && moved) {
            // Step from wherever the focus is, which may be where the mouse left it. Wrapping keeps a long list usable
            // with one direction held.
            const int from = g_focus_row < 0 ? 0 : g_focus_row;
            g_focus_row = (down & (1ull << B_DOWN)) ? (from + 1) % g_count : (from + g_count - 1) % g_count;
            // Only a move scrolls: without this guard every frame re-snapped the list to the cursor, which undid a
            // wheel scroll the moment the pad was read.
            const int visible = visible_rows(f);
            if (visible > 0) {
                if (g_focus_row < g_scroll) g_scroll = g_focus_row;
                if (g_focus_row >= g_scroll + visible) g_scroll = g_focus_row - visible + 1;
                if (g_scroll > f->scroll_max) g_scroll = f->scroll_max;
                if (g_scroll < 0) g_scroll = 0;
                f->scroll = g_scroll;
            }
        }
        if (down & (1ull << B_A)) {
            // A on the breadcrumb goes to that folder, on a row opens it: the same rule a click follows, so the two
            // devices cannot mean different things by "confirm".
            if (g_focus_part >= 0) g_pressed_part = g_focus_part;
            else if (g_focus_row >= 0 && g_count > 0) {
                activate(g_focus_row);
                return 1;
            }
        }
        if (down & (1ull << B_B)) {
            // B does exactly what a right click does: up one folder, and cancelled once there is no folder above.
            // Doing both in that order everywhere would mean B could never cancel from inside a folder, which is the
            // opposite of what the mouse can do from the same screen - so the two stay in step.
            if (g_dir[0] && go_up()) return 1;
            bp_picker_cancel();
            return 1;
        }
        // L / R scroll a page, the pad's counterpart of the mouse wheel: without them the only way to reach a row far
        // down a long folder was to hold the D-pad, which is a much slower way to do what the wheel does in a flick.
        if (down & (1ull << B_L)) {
            const int page = visible_rows(f) > 1 ? visible_rows(f) - 1 : 1;
            g_scroll -= page;
            if (g_scroll < 0) g_scroll = 0;
            f->scroll = g_scroll;
            acted = 1;
        }
        if (down & (1ull << B_R)) {
            const int page = visible_rows(f) > 1 ? visible_rows(f) - 1 : 1;
            g_scroll += page;
            if (g_scroll > f->scroll_max) g_scroll = f->scroll_max;
            f->scroll = g_scroll;
            acted = 1;
        }
    }
    return acted;
}

// The results of the last input pass, for the draw layer's colours.
int bp_picker_hover_part(void) { return g_focus_part; }
int bp_picker_selected_row(void) { return g_focus_row; }
int bp_picker_scroll(void) { return g_scroll; }
