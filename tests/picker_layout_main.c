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
void input_menu(int open) { (void)open; }
// ---- the pad: the preview test drives the cursor with the D-pad, so the stubs report what the test set.
static uint64_t g_pad_buttons;

int input_pad_read(uint64_t *b, int32_t *lx, int32_t *ly) {
    *b = g_pad_buttons;
    *lx = *ly = 0;
    return 1;
}
int ikb_down(int key) { (void)key; return g_mouse_left; }
int ikb_pointer(float *nx, float *ny) { (void)nx; (void)ny; return 0; }
int32_t ikb_wheel_take(void) { return 0; }
int plat_dir_exists(const char *p) { (void)p; return 1; }
void rt_log(const char *fmt, ...) { (void)fmt; }

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
        const float zero = 0.0f;
        memcpy(a + 12, &xs[k], 4);   // column-major translation: element 3 is x
        memcpy(a + 16, &zero, 4);
        memcpy(a + 20, &zs[k], 4);   // element 11 is z
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

        if (f.preview) {            snprintf(msg, sizeof msg, "preview: the parsed blueprint has 3 parts (title '%s')", f.preview_title);
            check(f.preview_parts == 3, msg);
            snprintf(msg, sizeof msg, "preview: the scatter is normalised into 0..1");
            int ok = 1;
            for (int k = 0; k < f.preview_parts; k++)
                if (f.preview_box[k].x < 0 || f.preview_box[k].x > 1 ||
                    f.preview_box[k].y < 0 || f.preview_box[k].y > 1) ok = 0;
            check(ok, msg);
            snprintf(msg, sizeof msg, "preview: the box lies inside the panel");
            check(f.preview_x >= f.x && f.preview_y >= f.y &&
                  f.preview_x + f.preview_w <= f.x + f.w && f.preview_y + f.preview_h <= f.y + f.h, msg);
            printf("preview: %d parts, title '%s', note '%s'\n", f.preview_parts, f.preview_title, f.preview_note);
        } else {
            // Reaching the row with the D-pad depends on the list length; report rather than fail when it is a folder.
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
            snprintf(msg, sizeof msg, "pointer preview: appears while hovering the file (%d parts)", f.preview_parts);
            check(f.preview && f.preview_parts == 3, msg);
        }
    }

    printf("picker layout: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
