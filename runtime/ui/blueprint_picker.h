// The blueprint file picker: the port's own panel, so no Windows file dialog takes the focus away from the game and the
// pad can drive it. The logic is C (runtime/blueprint_picker.c): the folder, the entries, the path parts, the layout
// rectangles, and the pointer/pad hit tests. Drawing is the only C++ part (ui/blueprint_picker.cpp), which fills and
// labels the rectangles this file computed, through ImGui's draw list only: no ImGui widgets, no ImGui layout, so the
// picker cannot trip ImGui's layout error checks (the earlier all-widget version aborted the game there).
//
// The split keeps one rule: this file decides what happens, the draw layer only paints what it is given.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "blueprint_cai.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BP_PICK_PATH_MAX 1024
#define BP_PICK_MAX_PARTS 16
#define BP_PICK_MAX_ROWS 530
#define BPC_PICK_MAX_DOTS 21   // one dot per actor of a blueprint (blueprint_cai.h BPC_MAX_ACTORS)
// The preview's part list: (part name, how many times the blueprint uses it). A 21-actor draft has at most 21 distinct
// names, so this is the worst case.
#define BPC_PICK_MAX_KINDS 21
// The 3D view's faces: every part contributes the triangles that face the camera. A blueprint's parts are real meshes
// from the mesh pack (up to a few thousand triangles each) or their collision shapes, and a convex body shows about
// half its surface, so this covers a 21-part build; past it the view stops adding faces rather than growing.
#define BPC_PICK_MAX_FACES 24000

// done gets the chosen path (UTF-8) or NULL when the player cancelled.
typedef void (*BpPickDone)(const char *path, void *ctx);

// The icon a directory/file row carries, and the scrollbar/detail text the draw layer paints. Kept as data so the C
// logic decides every visual distinction without the draw layer re-deriving it from a string.
enum {
    BP_ICON_FOLDER,
    BP_ICON_FILE,
    BP_ICON_DRIVE,
    BP_ICON_UP,
};

// What a rectangle is, for the draw layer's colours and for the hit tests.
enum {
    BP_RECT_ROW,      // one listed folder, file or drive
    BP_RECT_PART,     // one part of the breadcrumb path
    BP_RECT_BUTTON,   // a button: the breadcrumb back arrow, or Cancel in the footer
};

// What a BP_RECT_BUTTON does when it is activated. The picker's own code decides, so a click and a pad press run the
// same action instead of each looking for its own; matching on a button's text was how the two could drift apart.
enum {
    BP_ACT_NONE,
    BP_ACT_UP,        // the breadcrumb's back arrow: up one folder
    BP_ACT_CANCEL,    // the footer's Cancel: close the picker
};

typedef struct {
    int kind;
    int action;       // BP_ACT_*, for BP_RECT_BUTTON
    int x, y, w, h;   // UI pixels, from the swapchain's top left
    int hovered, pressed, selected;
    int icon;         // BP_ICON_*, for BP_RECT_ROW
    int last;         // BP_RECT_PART: 1 for the current folder (no chevron after it)
    char text[300];
} BPickRect;

typedef struct {
    float scale;               // 1.0 at 1080p; the row height and the padding follow it
    int row_h;                 // one listed row, in pixels, already scaled
    int x, y, w, h;            // the panel
    int row_top, row_bottom;   // the scrolling list's area
    int scroll;                // first visible entry
    int nrect;
    BPickRect rect[BP_PICK_MAX_ROWS];
    // Chrome the draw layer paints without hit-testing: the panel's three bands and the list's scroll indicator.
    int header_bottom;         // y where the title band ends
    int crumb_top, crumb_bottom;  // the breadcrumb band
    int list_bottom;           // y where the list band ends
    int footer_top;            // y where the bottom action bar starts
    int scroll_track_x, scroll_track_y, scroll_track_w, scroll_track_h;  // scrollbar track (0 when not needed)
    int scroll_thumb_y, scroll_thumb_h;                                    // the thumb inside the track
    int scroll_hit_x, scroll_hit_w;  // wider grab column than the thin visual track, for a mouse or a finger
    int scroll_range;          // pixels the thumb can travel (track_h - thumb_h); 0 when no scrollbar
    int scroll_max;            // largest first visible entry (count - visible_rows)
    int scroll_dragging;       // 1 while the thumb is held, so the draw layer can highlight it
    int count;                 // number of listed entries (for the header/status line)
    int error;                 // 1 when the folder could not be listed
    int at_pc;                 // 1 at "This PC" (the empty path): no folder above, so the back control is dimmed
    char status[160];          // status line: entry count, empty-folder hint or the error text
    char title[96];            // localised window title
    char hint[192];            // localised footer key hints, one line for hosts that cannot draw key caps
    char crumb[96];            // the breadcrumb's label at "This PC" (no path parts there)

    // ---- the footer's button prompts. Each is a key cap ("A", "B", "↑") with a localised label beside it, so the pad
    // keys read the way the game's own prompts do instead of as a sentence a player has to parse.
    int nprompt;
    struct { char key[8]; char label[64]; int primary; } prompt[4];   // primary: the accept key, drawn in gold

    // ---- the preview of the file under the pointer (or the pad cursor). Filled by the C layer so the draw layer only
    // paints. A CAI file stores each part's name and placement and nothing else - no meshes, no sizes - so a preview
    // drawn from the file alone can only ever be abstract. What it can say honestly is what the build is made of: the
    // parts it uses and how many of each ("4x Wooden Wheel, 1x Cannon"), which is what a player needs to tell one draft
    // from another. The thumbnail of a draft the game has already rendered is shown separately by the page.
    int preview;               // 1 when there is a blueprint preview to draw
    int preview_x, preview_y, preview_w, preview_h;   // the 3D view's box
    int preview_kinds;         // distinct part names, most used first
    struct {
        char name[96];         // the actor name from the CAI, as the game spells it
        int count;             // how many times the build uses it
    } preview_kind[BPC_PICK_MAX_KINDS];
    int preview_faces;         // how many projected faces follow, far first
    struct {
        float x[4], y[4];      // the face's corners, in 0..1 of the view box
        float shade;           // 0 (turned away from the light) to 1 (facing it)
        int approx;            // 1 when the face came from a collision shape, not the part's model
        int color;             // the actor's palette index, as the game's own viewer colours a build (0..20)
    } preview_face[BPC_PICK_MAX_FACES];
    int preview_modelled;      // parts drawn from their own model
    int preview_approx;        // parts drawn from their collision shape
    int preview_hidden;        // parts with no geometry at all, left out of the view
    int preview_list_y, preview_list_h;   // the part list's area under the 3D view
    char preview_title[160];   // the blueprint's headline: part count
    char preview_note[96];     // a second line: the file name, or why it cannot be imported
} BPickFrame;

// Opens the picker at start (a folder; "" or NULL uses the remembered folder). Returns 0 when one is already open.
int bp_picker_open(const char *start, BpPickDone done, void *ctx);
int bp_picker_is_open(void);
void bp_picker_cancel(void);

// Fills the rectangles to draw for the panel in a screen_w x screen_h image, at the menu's pixel scale (ui/menu.cpp
// menu_scale: 1.0 at 1080 lines and above, growing with the display's DPI floor). The scale is passed in, not derived
// from the height here, so the picker adapts to the window exactly the way the settings menu does.
//
// px and py are the pointer in the same pixels, or (-1, -1) when there is none. The preview follows the row under the
// pointer and falls back to the pad cursor's row, so it is decided from this frame's pointer rather than from the
// previous frame's hover flags.
void bp_picker_layout_at(BPickFrame *f, int screen_w, int screen_h, float scale, int px, int py);
// The same with no pointer: the preview follows the pad cursor only.
void bp_picker_layout(BPickFrame *f, int screen_w, int screen_h, float scale);

// Applies the pointer at (px, py) and the pad to f: marks hovered/selected, runs a press. Returns 1 when it changed the
// folder or chose a file, so the draw layer redraws from a fresh layout next frame.
int bp_picker_input(BPickFrame *f, int px, int py);

// A wheel turn from the window system's event queue (whole notches, + away from the player): scrolls the file list.
// The picker's wheel comes from the same WM_MOUSEWHEEL / SDL_EVENT_MOUSE_WHEEL events ImGui receives, not from raw
// input, so it keeps working when the cursor capture is released for the pointer. Safe from any thread.
void bp_picker_wheel(int notches);

// A right-button press goes up one folder. Called from the overlay's mouse-button event, once per press edge.
void bp_picker_back(void);

// The draw layer's colours need to know which rows and parts are under the pointer and which entry the pad is on.
int bp_picker_hover_part(void);
// Set by the draw layer once the GPU preview is ready; from then on the CPU projection and face list are skipped.
void bp_picker_use_gpu(int on);
int bp_picker_gpu_in_use(void);
// Rises when a different file is shown, so a renderer knows the image it holds is stale.
unsigned bp_picker_preview_stamp(void);
// The build's own rotation (row-major 3x3, applied to blueprint space) and the bounds of the rotated build. A drag turns
// the build rather than moving the camera, so the renderer has to apply this turn and frame these bounds, or its picture
// would disagree with the CPU projection and with what the pointer is doing.
void bp_picker_preview_rot(float out[9]);
void bp_picker_preview_bounds(float lo[3], float hi[3]);

int bp_picker_selected_row(void);
int bp_picker_scroll(void);

// The preview's camera, in radians: a drag inside the view turns it, the wheel over the view zooms it, a double click
// puts it back. These let a caller read the angle back and reset it.
void bp_picker_camera(float *yaw, float *pitch);
void bp_picker_camera_set(float yaw, float pitch);   // pitch is clamped short of the poles
void bp_picker_camera_reset(void);                   // back to the three-quarter default
float bp_picker_zoom(void);                          // 1.0 is the default distance
void bp_picker_zoom_set(float zoom);                 // clamped to the min/max range

// The blueprint on show, for the GPU renderer, or NULL when nothing is previewed. Valid until another file is loaded.
const BpcBlueprint *bp_picker_blueprint(void);

// Paints the panel into the overlay's draw list, in a width x height image, at the menu's pixel scale, with the pointer
// at (px, py) in the same pixels (-1, -1 when there is none). Defined in ui/blueprint_picker.cpp, the only C++ part of
// the picker. Returns 1 when it drew, so the present path keeps the overlay alive.
int bp_picker_draw(float width, float height, float scale, int px, int py);

#ifdef __cplusplus
}
#endif
