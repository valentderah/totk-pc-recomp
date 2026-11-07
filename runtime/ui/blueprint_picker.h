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

#ifdef __cplusplus
extern "C" {
#endif

#define BP_PICK_PATH_MAX 1024
#define BP_PICK_MAX_PARTS 16
#define BP_PICK_MAX_ROWS 530
#define BPC_PICK_MAX_DOTS 21   // one dot per actor of a blueprint (blueprint_cai.h BPC_MAX_ACTORS)

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
    // paints. It is a small 3D view: every part becomes a box at its own position and orientation, projected through a
    // fixed camera. The CAI format stores placement only (no meshes), so a box stands for a part rather than its model.
    int preview;               // 1 when there is a blueprint preview to draw
    int preview_x, preview_y, preview_w, preview_h;   // the box the view is drawn into
    int preview_parts;         // how many parts follow in preview_box
    struct {
        float x, y;            // the part's centre, projected into 0..1 of the box
        float s;               // its on-screen size, in the same 0..1 units
        float a;               // its shading, 0 (away) to 1 (facing the camera): the depth cue
        float tilt;            // how far the part is turned from upright, -1..1: a tilted part is drawn leaning
    } preview_box[BPC_PICK_MAX_DOTS];
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
int bp_picker_selected_row(void);
int bp_picker_scroll(void);

// Paints the panel into the overlay's draw list, in a width x height image, at the menu's pixel scale, with the pointer
// at (px, py) in the same pixels (-1, -1 when there is none). Defined in ui/blueprint_picker.cpp, the only C++ part of
// the picker. Returns 1 when it drew, so the present path keeps the overlay alive.
int bp_picker_draw(float width, float height, float scale, int px, int py);

#ifdef __cplusplus
}
#endif
