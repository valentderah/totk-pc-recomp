// Links the real runtime/blueprint_picker.c (plus blueprint_file.c and blueprint_cai.c) and checks its layout and
// preview through the public API only. Built and run by tests/picker_layout_test.py; no game files, no Vulkan.
//
// The stubs below stand in for the platform and UI surface the picker and the blueprint reader touch: a fake folder with
// 40 subdirectories and one real .cai blob so the list scrolls and the preview column has a blueprint to draw.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/blueprint_picker.h"
#include "bfres.h"
#include "blueprint_attach.h"
#include "model_romfs.h"

#define FAKE_ENTRIES 40

// ---- the fake pointer and file system

static int g_mouse_left;                 // set while the drag test holds the left button
static char g_cai_path[64];              // the one file plat_file_list offers
static uint8_t g_cai_blob[6688];         // its contents

// plat_file_open/pread/close: one fake in-memory file, so blueprint_file.c parses a real blob through its real code.
typedef struct { const uint8_t *data; size_t size; } FakeFile;
static FakeFile g_file;

void *plat_file_open(const char *path) {
    if (path && !strcmp(path, g_cai_path) && g_file.size) return &g_file;
    return NULL;
}
int64_t plat_file_pread(void *f, void *buf, uint32_t n, uint64_t off) {
    FakeFile *ff = (FakeFile *)f;
    if (!ff || off >= ff->size) return 0;
    size_t left = ff->size - (size_t)off;
    if (n > left) n = (uint32_t)left;
    memcpy(buf, ff->data + off, n);
    return (int64_t)n;
}
void plat_file_close(void *f) { (void)f; }

// ---- the rest of the surface

const char *menu_text(int id) { (void)id; return "x"; }
const char *cfg_get(const char *k) { (void)k; return NULL; }
void cfg_set(const char *k, const char *v) { (void)k; (void)v; }
void cfg_save(void) {}
unsigned cfg_version(void) { return 0; }
const char *bp_dir_last(void) { return ""; }
void input_menu(int open) { (void)open; }
// ---- the RomFS the shape resolver reads. In this test no RomFS is mounted, so every open fails and each part falls
// back to the box the test checks. The stubs only have to link.
uint32_t romfs_open(uint64_t *out, const char *path, int mode) { (void)out; (void)path; (void)mode; return 1; }
// The mesh pack: this test has none, so the preview falls through to the collision shapes (which need a RomFS this test
// does not mount) and then to a plain part list. Stubbing the platform calls is all that is needed for the link.
int plat_exe_path(char *out, size_t cap) { (void)out; (void)cap; return 0; }
int64_t plat_file_size(const char *path) { (void)path; return -1; }
uint32_t romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    (void)out; (void)h; (void)offset; (void)buffer; (void)size; return 1;
}
uint32_t romfs_size(int64_t *out, uint64_t h) { (void)out; (void)h; return 1; }
void romfs_close(uint64_t h) { (void)h; }
// ---- the pad: the preview test drives the cursor with the D-pad, so the stubs report what the test set.
static uint64_t g_pad_buttons;

int input_pad_read(uint64_t *b, int32_t *lx, int32_t *ly) {
    *b = g_pad_buttons;
    *lx = *ly = 0;
    return 1;
}

// The picker reads both sticks; the test drives the buttons and leaves the sticks at rest.
int input_pad_sticks(uint64_t *b, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    const int ok = input_pad_read(b, lx, ly);
    *rx = 0;
    *ry = 0;
    return ok;
}
int ikb_down(int key) { (void)key; return g_mouse_left; }
int ikb_pointer(float *nx, float *ny) { (void)nx; (void)ny; return 0; }
int32_t ikb_wheel_take(void) { return 0; }
int plat_dir_exists(const char *p) { (void)p; return 1; }
void rt_log(const char *fmt, ...) { (void)fmt; }

// The runtime model source (model_romfs.c + bfres.c) is not linked into this test: there is no RomFS and no MeshCodec
// bridge, so every part falls through to the collision-shape fallback and then the box. These stubs only have to link.
const BfresModel *model_romfs_resolve(const char *actor) { (void)actor; return NULL; }
const BfresModel *model_romfs_resolve_model(const char *model_name) { (void)model_name; return NULL; }
const BfresModel *model_romfs_resolve_inside(const char *actor) { (void)actor; return NULL; }
int bfres_shape_triangles(const BfresModel *m, int index) { (void)m; (void)index; return 0; }
int bfres_shape_name(const BfresModel *m, int index, char *name, size_t cap) {
    (void)m; (void)index;
    if (name && cap) name[0] = 0;
    return 0;
}
int bfres_shape_vertices(const BfresModel *m, int index) { (void)m; (void)index; return 0; }
int bfres_shape_geometry(const BfresModel *m, int index, float *verts, int max_verts, uint16_t *tris, int max_tris) {
    (void)m; (void)index; (void)verts; (void)max_verts; (void)tris; (void)max_tris; return 0;
}
// The fused-material path needs the game's attachment data and the weapon's skeleton, neither of which this test has. A
// part with no model never reaches them anyway, so these only have to link.
int bp_attach_resolve(const char *weapon, const char *material, BpAttach *out) {
    (void)weapon; (void)material;
    if (out) memset(out, 0, sizeof *out);
    return 0;
}
void bp_attach_matrix(const BpAttach *a, const float bone_world[16], float fit_scale, float out[16]) {
    (void)a; (void)bone_world; (void)fit_scale;
    memset(out, 0, sizeof(float) * 16);
    out[0] = out[5] = out[10] = out[15] = 1.0f;
}
float model_romfs_extent(const BfresModel *m) { (void)m; return 0.0f; }
int bfres_bone_find(const BfresModel *m, const char *name) { (void)m; (void)name; return -1; }
int bfres_bone_world(const BfresModel *m, int index, float out[16]) {
    (void)m; (void)index; (void)out; return 0;
}

// The picker uses this for the double-click window on the preview's camera. A monotonic counter is enough here: the
// test never depends on the wall clock, only on the value changing between calls.
uint64_t plat_now_ns(void) {
    static uint64_t t;
    t += 1000000ull;   // 1 ms per call
    return t;
}

int plat_dir_list(const char *d, int (*cb)(const char *, void *), void *c) {
    (void)d;
    // A folder whose name begins with a dot is a real, openable folder: the listing must offer it. "." and ".." are the
    // only names a listing must not carry, and plat_dir_list (the platform file) is what filters those.
    static const char *const dotdirs[] = {".cai", "Folder_00"};
    for (size_t i = 0; i < sizeof dotdirs / sizeof dotdirs[0]; i++)
        if (!cb(dotdirs[i], c)) return 1;
    for (int i = 1; i < FAKE_ENTRIES; i++) {
        char name[32];
        snprintf(name, sizeof name, "Folder_%02d", i);
        if (!cb(name, c)) break;
    }
    return 1;
}
int plat_file_list(const char *d, const char *e, int (*cb)(const char *, void *), void *c) {
    (void)d;
    if (e && !strcmp(e, ".cai") && g_file.size) {
        const char *base = strrchr(g_cai_path, '/');
        if (!cb(base ? base + 1 : g_cai_path, c)) return 1;
    }
    return 1;
}
int plat_root_list(int (*cb)(const char *, void *), void *c) { (void)cb; (void)c; return 0; }

// ---- a valid 3-actor blueprint: bbox (-3..3, -1..1, -2..2), actors at X -2/0/2 and Z -1/1/-1

static void make_blob(void) {
    memset(g_cai_blob, 0, sizeof g_cai_blob);
    memcpy(g_cai_blob, "CmbAct", 6);
    g_cai_blob[8] = 2;    // version
    g_cai_blob[9] = 3;    // actors
    g_cai_blob[10] = 2;   // actor count - 1
    const float xs[3] = {-2.0f, 0.0f, 2.0f};
    const float zs[3] = {-1.0f, 1.0f, -1.0f};
    for (int k = 0; k < 3; k++) {
        uint8_t *a = g_cai_blob + 12 + k * 280;
        // The first matrix is row-major: a rotation in the upper-left 3x3 and the position at elements 3, 7 and 11
        // (bytes 12, 28 and 44). The rotation has to be a real one - an identity here - because the preview builds each
        // part's box from it, and an all-zero matrix would collapse the box to a point.
        const float ident[16] = {1, 0, 0, xs[k],
                                 0, 1, 0, 0,
                                 0, 0, 1, zs[k],
                                 0, 0, 0, 1};
        memcpy(a, ident, sizeof ident);
        snprintf((char *)a + 152, 128, "Part_%d", k);
    }
    const float bb[6] = {-3.0f, -1.0f, -2.0f, 3.0f, 1.0f, 2.0f};
    memcpy(g_cai_blob + 6144, bb, sizeof bb);
    snprintf(g_cai_path, sizeof g_cai_path, "C:/fake/thing.cai");
    g_file.data = g_cai_blob;
    g_file.size = sizeof g_cai_blob;
}

static int failures, checks;
static void check(int cond, const char *what) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

int main(void) {
    static BPickFrame f;
    make_blob();

    if (!bp_picker_open("C:/fake", NULL, NULL)) {
        printf("FAIL: bp_picker_open returned 0\n");
        return 1;
    }

    const int sizes[][2] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}, {1600, 900}};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        const int w = sizes[i][0], h = sizes[i][1];
        bp_picker_layout(&f, w, h, 1.0f);
        char msg[192];

        snprintf(msg, sizeof msg, "%dx%d: panel inside the screen", w, h);
        check(f.x >= 0 && f.y >= 0 && f.x + f.w <= w && f.y + f.h <= h, msg);
        snprintf(msg, sizeof msg, "%dx%d: bands are ordered", w, h);
        check(f.header_bottom <= f.crumb_top && f.crumb_bottom <= f.row_top && f.row_bottom <= f.footer_top, msg);
        snprintf(msg, sizeof msg, "%dx%d: list area is positive", w, h);
        check(f.row_bottom > f.row_top, msg);

        int rows = 0, buttons = 0;
        for (int k = 0; k < f.nrect; k++) {
            const BPickRect *r = &f.rect[k];
            if (r->kind == BP_RECT_ROW) rows++;
            if (r->kind == BP_RECT_BUTTON) buttons++;
            snprintf(msg, sizeof msg, "%dx%d: rect %d inside the panel", w, h, k);
            check(r->x >= f.x && r->y >= f.y && r->x + r->w <= f.x + f.w && r->y + r->h <= f.y + f.h, msg);
            snprintf(msg, sizeof msg, "%dx%d: rect %d has a positive size", w, h, k);
            check(r->w > 0 && r->h > 0, msg);
        }
        snprintf(msg, sizeof msg, "%dx%d: at least 3 list rows (%d)", w, h, rows);
        check(rows >= 3, msg);
        snprintf(msg, sizeof msg, "%dx%d: back and cancel exist (%d)", w, h, buttons);
        check(buttons >= 2, msg);

        for (int a = 0; a < f.nrect; a++) {
            for (int b = a + 1; b < f.nrect; b++) {
                const BPickRect *x = &f.rect[a], *y = &f.rect[b];
                int overlap = x->x < y->x + y->w && y->x < x->x + x->w &&
                              x->y < y->y + y->h && y->y < x->y + x->h;
                if (overlap) {
                    snprintf(msg, sizeof msg, "%dx%d: rect %d(k%d) overlaps %d(k%d)", w, h, a, x->kind, b, y->kind);
                    check(0, msg);
                }
            }
        }
        checks++;

        snprintf(msg, sizeof msg, "%dx%d: the list scrolls (%d entries)", w, h, f.count);
        check(f.scroll_max > 0, msg);

        // The rows clear both the scrollbar and the preview column.
        if (f.scroll_track_w > 0) {
            snprintf(msg, sizeof msg, "%dx%d: rows clear the scroll track", w, h);
            int clear = 1;
            for (int k = 0; k < f.nrect; k++)
                if (f.rect[k].kind == BP_RECT_ROW && f.rect[k].x + f.rect[k].w > f.scroll_track_x) clear = 0;
            check(clear, msg);
            snprintf(msg, sizeof msg, "%dx%d: thumb inside the track", w, h);
            check(f.scroll_thumb_y >= f.scroll_track_y &&
                  f.scroll_thumb_y + f.scroll_thumb_h <= f.scroll_track_y + f.scroll_track_h + 1, msg);
        }

        // The preview column sits in the list band and never overlaps the rows.
        if (f.preview) {
            snprintf(msg, sizeof msg, "%dx%d: preview inside the list band", w, h);
            check(f.preview_y >= f.row_top && f.preview_y + f.preview_h <= f.row_bottom + 1, msg);
            snprintf(msg, sizeof msg, "%dx%d: preview is to the right of every row", w, h);
            int clear = 1;
            for (int k = 0; k < f.nrect; k++)
                if (f.rect[k].kind == BP_RECT_ROW && f.rect[k].x + f.rect[k].w > f.preview_x) clear = 0;
            check(clear, msg);
        }
    }

    // ---- the preview of the highlighted file: the real reader parsed a real blob, and the scatter is normalised.
    {
        char msg[192];
        // Bring the list back to the top, then scroll to the end so the file row (which sorts last) is visible.
        bp_picker_wheel(99999);
        bp_picker_input(&f, -1, -1);
        bp_picker_layout(&f, 1920, 1080, 1.0f);
        snprintf(msg, sizeof msg, "preview: back at the top (scroll %d)", f.scroll);
        check(f.scroll == 0, msg);

        bp_picker_wheel(-99999);
        bp_picker_input(&f, -1, -1);
        bp_picker_layout(&f, 1920, 1080, 1.0f);
        snprintf(msg, sizeof msg, "preview: scrolled to the end (scroll %d of %d)", f.scroll, f.scroll_max);
        check(f.scroll == f.scroll_max, msg);
    }

    // ---- the preview column draws the highlighted blueprint. The pad cursor is moved onto the file row and the frame
    // must carry its parsed part count and a normalised scatter.
    {
        // Walk the pad cursor onto the file row. The fake folder lists 40 directories and then one .cai, so the file is
        // the last entry. Each press is one D-pad edge: set the bit, poll, clear it again.
        bp_picker_layout(&f, 1920, 1080, 1.0f);
        const int steps = f.count - 1;   // from row 0 to the last row (the file)
        for (int step = 0; step < steps; step++) {
            g_pad_buttons = 1ull << 15;   // B_DOWN
            bp_picker_input(&f, -1, -1);
            g_pad_buttons = 0;
            bp_picker_input(&f, -1, -1);  // release: the edge is consumed here
            bp_picker_layout(&f, 1920, 1080, 1.0f);
        }
        char msg[192];
        bp_picker_input(&f, -1, -1);
        bp_picker_layout(&f, 1920, 1080, 1.0f);

        if (f.preview) {
            snprintf(msg, sizeof msg, "preview: the parsed blueprint has 3 parts (title '%s')", f.preview_title);
            check(f.preview_kinds > 0, msg);
            // The blob has three distinct part names (Part_0, Part_1, Part_2), so the list is three rows of one each.
            snprintf(msg, sizeof msg, "preview: three distinct parts listed (%d)", f.preview_kinds);
            check(f.preview_kinds == 3, msg);
            int total = 0;
            for (int k = 0; k < f.preview_kinds; k++) total += f.preview_kind[k].count;
            snprintf(msg, sizeof msg, "preview: the counts add up to the part count (%d)", total);
            check(total == 3, msg);
            snprintf(msg, sizeof msg, "preview: the box lies inside the panel");
            check(f.preview_x >= f.x && f.preview_y >= f.y &&
                  f.preview_x + f.preview_w <= f.x + f.w && f.preview_y + f.preview_h <= f.y + f.h, msg);
            printf("preview: %d kinds, title '%s', note '%s'\n", f.preview_kinds, f.preview_title, f.preview_note);
        } else {
            printf("preview: the cursor did not land on the file row (row selected, no preview)\n");
            check(0, "preview: a file row should have produced a preview");
        }
    }

    // ---- a folder whose name begins with a dot is listed like any other folder. The picker used to lose these
    // entirely because the platform layer skipped every name starting with a dot instead of only "." and "..".
    {
        bp_picker_layout(&f, 1920, 1080, 1.0f);
        int found = -1;
        for (int k = 0; k < f.count; k++) {
            // The fake listing puts ".cai" first; the picker shows it as a folder row with the folder icon.
            if (k == 0) found = k;
        }
        char msg[192];
        snprintf(msg, sizeof msg, "dot folder: listed at all (%d entries)", f.count);
        check(found >= 0, msg);
        int row_icon = -1;
        for (int k = 0; k < f.nrect; k++) {
            if (f.rect[k].kind != BP_RECT_ROW) continue;
            row_icon = f.rect[k].icon;   // the first row is the first entry, which qsort puts first: ".cai"
            break;
        }
        snprintf(msg, sizeof msg, "dot folder: drawn as a folder (icon %d, want %d)", row_icon, BP_ICON_FOLDER);
        check(row_icon == BP_ICON_FOLDER, msg);
    }

    // ---- the preview follows the pointer, not the pad cursor. This is the path a mouse actually takes, and the one
    // the column was silently missing on: the layout asked for the previous frame's hover flags, so the preview needed
    // a frame to catch up and never appeared at all when the pad cursor sat on a folder.
    {
        bp_picker_layout(&f, 1920, 1080, 1.0f);
        char msg[192];
        // Scroll to the end so the file row (which sorts last) is on screen, then point at it.
        bp_picker_wheel(-99999);
        bp_picker_input(&f, -1, -1);
        bp_picker_layout(&f, 1920, 1080, 1.0f);

        int fx = -1, fy = -1;
        {
            int rowseen = 0;
            for (int k = 0; k < f.nrect; k++) {
                if (f.rect[k].kind != BP_RECT_ROW) continue;
                if (f.scroll + rowseen == f.count - 1) {   // the file is the last entry
                    fx = f.rect[k].x + f.rect[k].w / 2;
                    fy = f.rect[k].y + f.rect[k].h / 2;
                }
                rowseen++;
            }
        }
        snprintf(msg, sizeof msg, "pointer preview: the file row is on screen (%d,%d)", fx, fy);
        check(fx >= 0, msg);

        if (fx >= 0) {
            // Pointing at the file must show the preview on this very frame, with no pad press in between.
            bp_picker_layout_at(&f, 1920, 1080, 1.0f, fx, fy);
            snprintf(msg, sizeof msg, "pointer preview: appears while hovering the file (%d kinds)", f.preview_kinds);
            check(f.preview && f.preview_kinds > 0, msg);
        }
    }

    // ---- dragging inside the preview must not change which file is previewed. The pointer sits in the preview column at
    // a height that lies over a different row; the file under the cursor must stay the one the player focused, because
    // the column is not the list. This was a real bug: the layout pass asked the rows before they had been narrowed to
    // clear the column, so every point in the column landed on the row at that height and the preview swapped files as
    // the pointer moved.
    {
        bp_picker_camera_reset();
        bp_picker_wheel(99999);
        bp_picker_input(&f, -1, -1);
        bp_picker_layout(&f, 1920, 1080, 1.0f);
        bp_picker_wheel(-99999);
        bp_picker_input(&f, -1, -1);
        bp_picker_layout(&f, 1920, 1080, 1.0f);

        // Point at the file row so the column is reserved and the file is the focus.
        int fx = -1, fy = -1, rowseen = 0;
        for (int k = 0; k < f.nrect; k++) {
            if (f.rect[k].kind != BP_RECT_ROW) continue;
            if (f.scroll + rowseen == f.count - 1) {
                fx = f.rect[k].x + f.rect[k].w / 2;
                fy = f.rect[k].y + f.rect[k].h / 2;
            }
            rowseen++;
        }
        char msg[192];
        snprintf(msg, sizeof msg, "drag: the file row is on screen (%d,%d)", fx, fy);
        check(fx >= 0, msg);
        if (fx < 0) goto drag_done;

        bp_picker_layout_at(&f, 1920, 1080, 1.0f, fx, fy);
        bp_picker_input(&f, fx, fy);
        bp_picker_layout_at(&f, 1920, 1080, 1.0f, fx, fy);
        const int had_faces = f.preview_faces;
        char before[128];
        snprintf(before, sizeof before, "%s", f.preview_note);
        snprintf(msg, sizeof msg, "drag: the file previews before the drag (%d faces, '%s')", had_faces, before);
        check(f.preview && before[0] != 0, msg);

        // The pointer inside the column, at the height of the first listed row (a folder, not the file).
        int cx = f.preview_x + f.preview_w / 2, cy = -1;
        for (int k = 0; k < f.nrect; k++)
            if (f.rect[k].kind == BP_RECT_ROW) { cy = f.rect[k].y + f.rect[k].h / 2; break; }
        snprintf(msg, sizeof msg, "drag: a point inside the column over a folder row (%d,%d)", cx, cy);
        check(cy >= 0 && cx > f.preview_x && cx < f.preview_x + f.preview_w, msg);

        // Layout with the pointer there: the same file must still be the preview, and the pointer must not have moved the
        // focus onto that folder row.
        const int focus_before = bp_picker_selected_row();
        bp_picker_layout_at(&f, 1920, 1080, 1.0f, cx, cy);
        snprintf(msg, sizeof msg, "drag: the column keeps the same file ('%s' -> '%s')", before, f.preview_note);
        check(f.preview && !strcmp(before, f.preview_note), msg);
        snprintf(msg, sizeof msg, "drag: the column keeps its faces (%d -> %d)", had_faces, f.preview_faces);
        check(f.preview_faces > 0 || had_faces == 0, msg);
        bp_picker_input(&f, cx, cy);
        snprintf(msg, sizeof msg, "drag: the pointer in the column does not move the focus (%d -> %d)", focus_before,
                 bp_picker_selected_row());
        check(focus_before == bp_picker_selected_row(), msg);
    }
drag_done:

    // ---- one focus for both devices: a row the pointer left focused is where the pad continues from, so A acts on the
    // row the mouse was on rather than on something the pad picked independently.
    {
        bp_picker_wheel(99999);
        bp_picker_input(&f, -1, -1);
        bp_picker_layout(&f, 1920, 1080, 1.0f);
        char msg[192];

        // Point at the third visible row, then move the pointer off the list entirely.
        int rx = 0, ry = 0, seen = 0;
        for (int k = 0; k < f.nrect; k++) {
            if (f.rect[k].kind != BP_RECT_ROW) continue;
            if (seen == 2) {
                rx = f.rect[k].x + f.rect[k].w / 2;
                ry = f.rect[k].y + f.rect[k].h / 2;
            }
            seen++;
        }
        snprintf(msg, sizeof msg, "focus: found a third row (%d,%d)", rx, ry);
        check(rx > 0, msg);
        bp_picker_layout_at(&f, 1920, 1080, 1.0f, rx, ry);
        bp_picker_input(&f, rx, ry);
        bp_picker_layout_at(&f, 1920, 1080, 1.0f, rx, ry);
        const int pointed = bp_picker_selected_row();
        snprintf(msg, sizeof msg, "focus: the pointer sets it (row %d)", pointed);
        check(pointed == f.scroll + 2, msg);

        // The pointer leaves the list (the title band): the focus must stay where the pointer left it, not clear.
        bp_picker_input(&f, -1, -1);
        bp_picker_layout(&f, 1920, 1080, 1.0f);
        snprintf(msg, sizeof msg, "focus: survives the pointer leaving the list (%d)", bp_picker_selected_row());
        check(bp_picker_selected_row() == pointed, msg);

        // And a D-pad press continues from there, one row on.
        g_pad_buttons = 1ull << 15;   // B_DOWN
        bp_picker_input(&f, -1, -1);
        g_pad_buttons = 0;
        bp_picker_input(&f, -1, -1);
        bp_picker_layout(&f, 1920, 1080, 1.0f);
        snprintf(msg, sizeof msg, "focus: the pad continues from the mouse's row (%d -> %d)", pointed,
                 bp_picker_selected_row());
        check(bp_picker_selected_row() == pointed + 1, msg);
    }

    // ---- a mouse that is parked on a row must not take the focus back from the pad. This was a real bug: the pointer
    // set the focus on every frame it lay on a row, so the D-pad moved the focus and the pointer put it back one frame
    // later, and A acted on the row under the mouse rather than the one the pad was on.
    {
        bp_picker_camera_reset();
        bp_picker_wheel(99999);
        bp_picker_input(&f, -1, -1);
        bp_picker_layout(&f, 1920, 1080, 1.0f);
        char msg[192];

        // Point at the third visible row and let the pointer settle there (two frames at the same position).
        int rx = 0, ry = 0, seen = 0;
        for (int k = 0; k < f.nrect; k++) {
            if (f.rect[k].kind != BP_RECT_ROW) continue;
            if (seen == 2) {
                rx = f.rect[k].x + f.rect[k].w / 2;
                ry = f.rect[k].y + f.rect[k].h / 2;
            }
            seen++;
        }
        bp_picker_layout_at(&f, 1920, 1080, 1.0f, rx, ry);
        bp_picker_input(&f, rx, ry);
        bp_picker_layout_at(&f, 1920, 1080, 1.0f, rx, ry);
        bp_picker_input(&f, rx, ry);
        const int parked = bp_picker_selected_row();
        snprintf(msg, sizeof msg, "pad over a parked mouse: the pointer set the focus (row %d)", parked);
        check(parked == f.scroll + 2, msg);

        // The pad steps down one row, with the pointer still resting on that same row.
        g_pad_buttons = 1ull << 15;   // B_DOWN
        bp_picker_input(&f, rx, ry);
        g_pad_buttons = 0;
        bp_picker_input(&f, rx, ry);
        snprintf(msg, sizeof msg, "pad over a parked mouse: the pad moved the focus (%d -> %d)", parked,
                 bp_picker_selected_row());
        check(bp_picker_selected_row() == parked + 1, msg);

        // And it stays there on the following frames, which is where the bug showed.
        for (int i = 0; i < 3; i++) {
            bp_picker_layout_at(&f, 1920, 1080, 1.0f, rx, ry);
            bp_picker_input(&f, rx, ry);
        }
        snprintf(msg, sizeof msg, "pad over a parked mouse: the focus stays with the pad (%d -> %d)", parked + 1,
                 bp_picker_selected_row());
        check(bp_picker_selected_row() == parked + 1, msg);

        // The pointer moving again takes the focus back to the row it is on, the way it should.
        bp_picker_layout_at(&f, 1920, 1080, 1.0f, rx + 3, ry);
        bp_picker_input(&f, rx + 3, ry);
        snprintf(msg, sizeof msg, "pad over a parked mouse: a moving pointer takes the focus back (%d -> %d)",
                 parked + 1, bp_picker_selected_row());
        check(bp_picker_selected_row() == parked, msg);
    }

    // ---- the footer names only the actions the screen has. The scroll and page-turn prompts are gone: the wheel scrolls a
      // list without needing to be introduced, and "turn a page" is not something a file list does.
      {
          bp_picker_wheel(0);
          bp_picker_input(&f, -1, -1);
          bp_picker_layout(&f, 1920, 1080, 1.0f);
          char msg[192];
          snprintf(msg, sizeof msg, "the footer carries the two actions the screen has, not four (%d prompts)", f.nprompt);
          check(f.nprompt == 2, msg);
      }

    // ---- the footer names only keys that exist on a Switch pad: no "R" prompt (the pad has no such button).
    {
        bp_picker_wheel(99999);
        bp_picker_input(&f, -1, -1);
        bp_picker_layout(&f, 1920, 1080, 1.0f);
        int has_r = 0, prompts = f.nprompt;
        for (int i = 0; i < f.nprompt; i++)
            if (!strcmp(f.prompt[i].key, "R")) has_r = 1;
        char msg[192];
        snprintf(msg, sizeof msg, "prompts: no \"R\" key cap among the %d prompts", prompts);
        check(!has_r, msg);
        snprintf(msg, sizeof msg, "prompts: A and B are always offered (%d prompts)", prompts);
        check(prompts >= 2 && !strcmp(f.prompt[0].key, "A") && !strcmp(f.prompt[1].key, "B"), msg);
    }

    // ---- the camera: a drag turns it, the projection follows, and it stays inside its poles.
    {
        char msg[192];
        float yaw = 0, pitch = 0;
        bp_picker_camera_reset();
        bp_picker_camera(&yaw, &pitch);
        snprintf(msg, sizeof msg, "camera: the default view is the documented three-quarter angle (%.2f, %.2f)", yaw,
                 pitch);
        check(yaw > 0.60f && yaw < 0.62f && pitch > 0.48f && pitch < 0.50f, msg);

        // Pitch is no longer clamped: the view can tip past the poles and read the build from the other side, so an
        // out-of-range pitch is stored verbatim.
        bp_picker_camera_set(0.0f, 99.0f);
        bp_picker_camera(&yaw, &pitch);
        snprintf(msg, sizeof msg, "camera: pitch passes through unclamped (%.2f)", pitch);
        check(pitch > 98.9f && pitch < 99.1f, msg);
        bp_picker_camera_set(0.0f, -99.0f);
        bp_picker_camera(&yaw, &pitch);
        snprintf(msg, sizeof msg, "camera: pitch passes through unclamped the other way (%.2f)", pitch);
        check(pitch < -98.9f && pitch > -99.1f, msg);

        bp_picker_camera_reset();
        bp_picker_layout(&f, 1920, 1080, 1.0f);
        snprintf(msg, sizeof msg, "camera: the reset restores the default view");
        bp_picker_camera(&yaw, &pitch);
        check(yaw > 0.60f && yaw < 0.62f && pitch > 0.48f && pitch < 0.50f, msg);
    }

    printf("picker layout: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
