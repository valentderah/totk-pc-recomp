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

#include "blueprint_file.h"
#include "cfg.h"
#include "host.h"
#include "input.h"
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

// ---- the preview of the file under the pointer or the pad cursor
//
// Reading a blueprint means opening the file and parsing it, so it is done once per file, not per frame: the cache is
// keyed by the file's full path and only rebuilt when the highlighted entry changes.
//
// The CAI format stores each part's placement (a rotation and a position) and nothing else - no meshes - so the preview
// is a 3D view of boxes rather than of models: every part becomes a small box at its own position, turned by its own
// rotation, and the whole thing is projected through one fixed camera. That is honest about what the file contains and
// still reads as the shape of the build, which a flat scatter could not.
// One projected part: where its centre lands in the view box (0..1), how big it is there, how bright, and how far it is
// turned from upright.
typedef struct { float x, y, s, a, tilt; } PickPart;

typedef struct {
    char path[PICK_PATH_MAX];      // the file the data below belongs to; "" when nothing is read yet
    int ok;                        // 1: a real blueprint; 0: refused (note says why)
    int actors;                    // parts, when ok
    char note[96];                 // the file name when ok, else the reason it cannot be imported
    int nparts;
    PickPart part[BPC_PICK_MAX_DOTS];   // in draw order: far first, so a nearer box covers a further one
} PickPreview;
static PickPreview g_preview;

// The camera: a fixed three-quarter view, looking down at the build from slightly above and to one side. A blueprint is
// built on a plane (its bbox is wide in x/z and thin in y), so this is the angle that shows the layout - the same
// reasoning a player would use when turning a build in the Autobuild screen. Returns the point in 0..1 of the view box
// (x right, y down) and its depth, larger being further from the camera.
static void preview_project(const float p[3], const float bbox_min[3], const float bbox_max[3],
                            float *sx, float *sy, float *depth) {
    // Normalise the part's position into -1..1 across the blueprint's own box, so any build fills the view the same way.
    float n[3];
    for (int i = 0; i < 3; i++) {
        const float span = bbox_max[i] - bbox_min[i];
        n[i] = span > 0.001f ? ((p[i] - bbox_min[i]) / span) * 2.0f - 1.0f : 0.0f;
    }
    const float cy = 0.819152f, syn = 0.573576f;   // cos / sin of a 35 degree yaw
    const float cp = 0.882948f, sp = 0.469472f;    // cos / sin of a 28 degree pitch
    const float x1 = cy * n[0] + syn * n[2];
    const float z1 = -syn * n[0] + cy * n[2];
    const float y1 = cp * n[1] - sp * z1;
    const float z2 = sp * n[1] + cp * z1;
    const float d = 4.0f;                          // camera distance in normalised units
    const float k = d / (d + z2);                  // perspective: near parts grow, far ones shrink
    *sx = 0.5f + x1 * 0.36f * k;
    *sy = 0.5f - y1 * 0.36f * k;
    *depth = z2;
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
    if (!path || !*path) {
        g_preview.path[0] = 0;
        g_preview.ok = 0;
        g_preview.nparts = 0;
        g_preview.actors = 0;
        g_preview.note[0] = 0;
        return;
    }
    if (!strcmp(g_preview.path, path)) return;   // already this file

    snprintf(g_preview.path, sizeof g_preview.path, "%s", path);
    g_preview.ok = 0;
    g_preview.nparts = 0;
    g_preview.actors = 0;
    // A blueprint is at most 262144 bytes and parsing it is a few dozen float reads, so reading it here (on the present
    // path, once per highlighted file) is nothing next to the folder walk that already happens when a folder opens.
    static uint8_t storage[BPF_READ_MAX];
    BpcBlueprint bp;
    enum BpcResult r = bp_file_read(path, storage, &bp);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (r != BPC_OK) {
        snprintf(g_preview.note, sizeof g_preview.note, "%s", bpc_result_text(r));
        return;
    }
    g_preview.ok = 1;
    g_preview.actors = bp.actors;
    snprintf(g_preview.note, sizeof g_preview.note, "%s", base);

    // Project every part, then sort far-to-near so the painter's algorithm is enough (the overlay has no depth buffer).
    const int n = bp.actors < BPC_PICK_MAX_DOTS ? bp.actors : BPC_PICK_MAX_DOTS;
    float depth[BPC_PICK_MAX_DOTS];
    const float per = 1.0f / (float)(n > 0 ? n : 1);
    for (int k = 0; k < n; k++) {
        float vx, vy, vz;
        preview_project(bp.pos[k], bp.bbox_min, bp.bbox_max, &vx, &vy, &vz);
        depth[k] = vz;
        g_preview.part[k].x = vx;
        g_preview.part[k].y = vy;
        // The perspective already shrank the centre; the box shrinks with it too, and a busier build draws smaller
        // parts so a 21-part draft does not become a solid block of colour.
        const float k2 = 4.0f / (4.0f + vz);
        g_preview.part[k].s = (0.10f + 0.35f * per) * k2;
        // Shade by depth: the further a part is, the dimmer, which is the only depth cue a flat fill can give.
        float a = 1.0f - (vz + 1.6f) / 3.2f;
        if (a < 0.2f) a = 0.2f;
        if (a > 1.0f) a = 1.0f;
        g_preview.part[k].a = a;
        // How far the part leans away from vertical, from its own rotation. rot's middle row is the part's local +y
        // expressed in blueprint space, so its y component is 1 when the part stands upright, -1 when it hangs upside
        // down, and 0 when it lies flat: the magnitude is what "leans", and the sign only says which way is up.
        float lean = 1.0f - fabsf(bp.rot[k][1][1]);
        if (lean < 0) lean = 0;
        if (lean > 1.0f) lean = 1.0f;
        g_preview.part[k].tilt = lean;
    }
    // Insertion sort by descending depth: at most 21 items, so this is both simpler and faster than a general sort.
    for (int i = 1; i < n; i++) {
        const PickPart p = g_preview.part[i];
        const float d = depth[i];
        int j = i - 1;
        while (j >= 0 && depth[j] < d) {
            g_preview.part[j + 1] = g_preview.part[j];
            depth[j + 1] = depth[j];
            j--;
        }
        g_preview.part[j + 1] = p;
        depth[j + 1] = d;
    }
    g_preview.nparts = n;
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
    if (g_entries[i].root) {
        // A drive row carries the root path itself ("C:/"), not a name below the current folder.
        snprintf(path, sizeof path, "%s", g_entries[i].name);
        open_folder(path);
    } else if (g_entries[i].dir) {
        snprintf(path, sizeof path, "%s/%s", g_dir, g_entries[i].name);
        open_folder(path);
    } else {
        snprintf(path, sizeof path, "%s/%s", g_dir, g_entries[i].name);
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
    const char *from = start && *start ? start : cfg_get("TOTK_BLUEPRINT_DIR");
    snprintf(g_dir, sizeof g_dir, "%s", from && *from ? from : ".");
    list_folder();
    g_open = 1;
    input_menu(1);
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
    f->w = (int)(screen_w * 0.60f);
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
    if (g_count > visible_rows(f)) {
        snprintf(f->prompt[f->nprompt].key, sizeof f->prompt[0].key, "\xe2\x87\x95");
        snprintf(f->prompt[f->nprompt].label, sizeof f->prompt[0].label, "%s",
                 menu_text(UI_MENU_BLUEPRINT_KEY_SCROLL));
        f->prompt[f->nprompt].primary = 0;
        f->nprompt++;
        if (f->nprompt < 4) {
            snprintf(f->prompt[f->nprompt].key, sizeof f->prompt[0].key, "L/R");
            snprintf(f->prompt[f->nprompt].label, sizeof f->prompt[0].label, "%s",
                     menu_text(UI_MENU_BLUEPRINT_KEY_PAGE));
            f->prompt[f->nprompt].primary = 0;
            f->nprompt++;
        }
    }
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
    int idx = row_at(f, px, py);
    if (idx < 0) idx = g_focus_row;
    if (idx < 0 || idx >= g_count) return 0;
    if (g_entries[idx].dir || g_entries[idx].root) return 0;   // a folder has no blueprint to preview

    char path[PICK_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", g_dir, g_entries[idx].name);
    preview_load(path);

    const float sc = f->scale;
    const int right = f->x + f->w - pad;
    const int box_w = preview_w;
    const int box_x = right - box_w;
    const int box_y = f->row_top;
    const int box_h = f->row_bottom - f->row_top;

    (void)box_w;
    f->preview = 1;
    // The 3D view occupies the upper part of the column; the text sits under it.
    const int text_h = (int)(64 * sc);
    const int pad_in = (int)(12 * sc);
    f->preview_x = box_x + pad_in;
    f->preview_y = box_y + pad_in;
    f->preview_w = preview_w - pad_in * 2;
    f->preview_h = box_h - text_h - pad_in * 2;
    if (f->preview_h < (int)(40 * sc)) f->preview_h = (int)(40 * sc);
    f->preview_parts = g_preview.nparts;
    for (int k = 0; k < g_preview.nparts; k++) {
        f->preview_box[k].x = g_preview.part[k].x;
        f->preview_box[k].y = g_preview.part[k].y;
        f->preview_box[k].s = g_preview.part[k].s;
        f->preview_box[k].a = g_preview.part[k].a;
        f->preview_box[k].tilt = g_preview.part[k].tilt;
    }
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
    // file is highlighted, so a folder view keeps the whole width for its rows.
    const int preview_w = (int)(f->w * 0.30f);
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
    // The focus follows the pointer only while the pointer is on something. When it drifts off the rows (over the
    // preview, the title, outside the panel) the focus stays where it was, so the pad can carry on from there instead of
    // having its selection silently cleared by a pointer that is not pointing at anything.
    const int under_row = row_at(f, px, py) >= 0;
    if (under_row) {
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

    // The pointer's left button. Its edge and its held state drive both a click and a scrollbar drag; the drag has
    // priority so that pressing on the thumb never activates the row that happens to lie behind it.
    static int held_l;
    const int l = ikb_down(KEY_MOUSE_L);
    const int click = l && !held_l;
    held_l = l;

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
        g_scroll -= wheel;   // away from the player scrolls down
        if (g_scroll > f->scroll_max) g_scroll = f->scroll_max;
        if (g_scroll < 0) g_scroll = 0;
        f->scroll = g_scroll;
        acted = 1;
    }

    // ---- the pad. It drives the SAME focus the pointer does, so the two never disagree: a mouse that leaves a row
    // focused and a D-pad press continue from there, and A acts on whatever the pointer last pointed at.
    uint64_t pad = 0;
    int32_t lx, ly;
    if (input_pad_read(&pad, &lx, &ly)) {
        const uint64_t down = pad & ~g_pad_prev;
        g_pad_prev = pad;
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
