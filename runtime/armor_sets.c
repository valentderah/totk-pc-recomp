// Armor sets for TotK 1.4.3 (TOTK_ARMOR_SETS, menu: Game modifications > Armor sets; off by default, switched live).
//
// In the inventory an armour piece's item window gets a third row, "Register to armor set", which turns the same window
// into Set 1..9 + Cancel; the piece is saved to the chosen set under its part (head, upper or lower). A fourth row,
// "Clear all armor sets", empties all nine after asking in the same window. While the weapon,
// shield or bow shortcut strip is held open, right stick up shows the sets instead (one tile per registered set,
// pictured by its first piece) and down goes back; releasing on a set puts the whole set on and leaves the weapon,
// shield or bow as it was. Sets are stored by item name in <profile>/armor-sets.txt, since the armour list's order
// changes with the inventory.
//
// The code sites are the patch set armor (recomp/patches-v143.txt). Off, they run the game's own instructions; the set
// follows the setting from the pad poll (armor_sets_filter, runtime/ov_input.c), which runs whatever the set's state.
// Each site is a bl to a host function on the patch page (+0xe60..+0xef0):
//   main+0xb9e5fc  the sead main loop (0xb9e5e8), once a frame between frames: all work that calls the game's data
//                  code runs here, on the game's main thread
//   main+0x21786cc/0x2178708..14  the armour branch of the item window filler (0x21782b4): four rows, Register and Clear
//   main+0x2179c28  the window's choice to the game's command handler 0x2173d18 (its switch ends at 0x1c; ours are 0x40+)
//   main+0x20c7158/0x20c71c4  the strip's tile filler 0x20c7108: in sets mode a set is drawn with the armour tile
//                  0x2094b08 on the shield path
//   main+0x2183714/0x2183774  the strip filler 0x21833f8: tile count and first cursor
//   main+0x2125db8  the name under the strip (cursor change 0x2125c3c)
//   main+0x21829d4  the strip's per-frame cursor read, which shows the Drop hint on an item tile
//   main+0x2183358  the strip closing (0x2183320) applying the selection through 0x21266a0
// The item window's ten rows come from tools/item_window_layout.py (make_romfs_patches.py).
//
// Game facts (1.4.3):
// - pouch manager [0x3a81678]: +0x188 the pouch in use, +0x480 the player's EquipmentUser, the Pouch.Armor handle at
//   +8 + 0x40 (+ 0xc0 per pouch). Member 0x7d8b094 is the equipped struct with one int each for head, upper and lower:
//   an index into the armour list (member 0x4290322e, Name 0x25efa387), -1 for nothing. GameData manager [[0x395dff0]].
// - GameData writes (0x16fdcc) are queued and land a little later; 0xc4a5f4(EquipmentUser, 9/10/11) then re-reads that
//   slot and changes the model and the armour effects at once, as the inventory does.
// - Strip screen: +0x1a8 the strip, +0x1b8 its type (0 weapon, 1 shield, 2 bow, 3 materials), byte +0x16e its state
//   (2 open), byte +0x1d8 set once it closed; the strip's cursor is [[strip+0x50]+0x8c] (tile 0 = empty, tile n = n-1).
// - The item window clamps itself into its display area (0x21134a0) only when [win+0x80] is set: after the window grows
//   to ten rows the clamp runs again a few frames later.
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "ui_text.h"
#include "utf16.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

#ifdef RT_PATCH_PAGE

#define HOST_STRIP_HINT (RT_PATCH_PAGE + 0xe60)
#define HOST_FRAME (RT_PATCH_PAGE + 0xe70)
#define HOST_ROWS (RT_PATCH_PAGE + 0xe80)
#define HOST_CMD (RT_PATCH_PAGE + 0xe90)
#define HOST_STRIP_TYPE (RT_PATCH_PAGE + 0xea0)
#define HOST_STRIP_TILE (RT_PATCH_PAGE + 0xeb0)
#define HOST_STRIP_APPLY (RT_PATCH_PAGE + 0xec0)
#define HOST_STRIP_COUNT (RT_PATCH_PAGE + 0xed0)
#define HOST_STRIP_CURSOR (RT_PATCH_PAGE + 0xee0)
#define HOST_STRIP_LABEL (RT_PATCH_PAGE + 0xef0)
#define G(a) (0x7000000000ULL + (a))   // main's base in the layout

#define STICK_ON 22937        // 70% of full deflection: right stick up or down
#define STICK_OFF 9830        // 30%: centred again
#define NAME_LEN 64
#define LIST_LEN 150          // the armour list's size (GameData)
#define WAIT_FRAMES 60
#define SLOTS 9
#define CMD_REGISTER 0x40
#define CMD_SLOT 0x41         // + slot
#define CMD_CLEAR 0x50
#define CMD_CLEAR_YES 0x51
#define PICK_ROWS (SLOTS + 1)
#define PICK_FRAME (PICK_ROWS - 2)

static const uint32_t part_hash[3] = {0x05777012, 0xedb97ebf, 0xeecbbe92};   // head, upper, lower
// Names older files used for slots 1..4.
static const char *const old_slot_name[4] = {"up", "right", "down", "left"};

static _Atomic uint64_t seen_ms;           // the last frame host
static _Atomic int reg_item = -1;          // the item window's armour item, an index into the armour list
static _Atomic int reg_slot = -1;          // set chosen for it, for the next frame
static _Atomic int clear_req;              // all sets are to be emptied, on the next frame
static _Atomic uint64_t strip_screen;      // captured when the strip draws a tile
static _Atomic int strip_open;             // a weapon, shield or bow strip is open
static _Atomic int strip_req;              // from the pad: 1 show the sets, 2 back
static _Atomic int strip_sets;             // the strip shows sets
static _Atomic int strip_equip = -1;       // set picked on release, for the next frame
static int strip_n, strip_slot[SLOTS], strip_item[SLOTS];   // the sets shown: their slot and picture item
static _Atomic uint64_t clamp_win;         // the window that grew: its clamp runs again
static _Atomic int clamp_frames;

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

// The setting, re-read when the settings change; the patch set follows it.
static int armor_on(void) {
    static _Atomic unsigned seen = ~0u;
    static _Atomic int on = -1;
    unsigned v = cfg_version();
    if (atomic_exchange(&seen, v) != v) {
        int now = cfg_bool("TOTK_ARMOR_SETS");
        if (atomic_exchange(&on, now) != now) {
            rt_patch_switch(rt_patch_set("armor"), now);
            rt_log("armor sets: %s (TOTK_ARMOR_SETS)", now ? "on" : "off");
        }
    }
    return atomic_load(&on) == 1;
}

// Every pad poll (runtime/ov_input.c), on whichever guest thread polls. While a weapon, shield or bow strip is open the
// right stick's up and down switch between its items and the sets, and the game does not see them.
void armor_sets_filter(uint64_t *buttons, int32_t *ry) {
    static int latch;
    if (!armor_on() || !atomic_load(&strip_open) || now_ms() - atomic_load(&seen_ms) > 1000) {
        latch = 0;
        return;
    }
    int ay = *ry < 0 ? -*ry : *ry;
    if (!latch && ay >= STICK_ON) {
        atomic_store(&strip_req, *ry > 0 ? 1 : 2);   // up is +y
        latch = 1;
    } else if (ay < STICK_OFF) {
        latch = 0;
    }
    *ry = 0;
    *buttons &= ~((1ULL << 21) | (1ULL << 23));   // StickRUp, StickRDown (input.h)
    // The strip's Sort and Drop would act on the game's list behind the sets.
    if (atomic_load(&strip_sets)) *buttons &= ~((1ULL << 2) | (1ULL << 3));   // X, Y
}

// ---- the main thread's half: nothing below runs on another thread
static char sets[SLOTS][3][NAME_LEN];      // "" leaves that part alone
static int loaded;
static char sets_path[1024];
static struct { int active, frames, want[3], need[3]; } job;
static _Alignas(16) uint8_t scratch[0x80];   // guest addresses are host addresses

static void sets_load(void) {
    if (loaded) return;
    loaded = 1;
    // The profile the run uses (portable folder, --profile), not the build's default: a moved or copied install
    // would otherwise read and write a file that is not there.
    snprintf(sets_path, sizeof sets_path, "%s/armor-sets.txt", rt_config()->profile);
    FILE *f = fopen(sets_path, "r");
    if (!f) {
        rt_log("armor sets: no sets yet (%s)", sets_path);
        return;
    }
    char slot[16], p[3][NAME_LEN];
    while (fscanf(f, "%15s %63s %63s %63s", slot, p[0], p[1], p[2]) == 4) {
        int d = atoi(slot) - 1;
        for (int k = 0; k < 4; k++)
            if (!strcmp(slot, old_slot_name[k])) d = k;
        if (d < 0 || d >= SLOTS) continue;
        for (int s = 0; s < 3; s++) snprintf(sets[d][s], NAME_LEN, "%s", strcmp(p[s], ".") ? p[s] : "");
    }
    fclose(f);
    int n = 0;
    for (int d = 0; d < SLOTS; d++) n += sets[d][0][0] || sets[d][1][0] || sets[d][2][0];
    rt_log("armor sets: %d sets read from %s", n, sets_path);
}

static void sets_store(void) {
    FILE *f = fopen(sets_path, "w");
    if (!f) {
        rt_log("armor sets: cannot write %s", sets_path);
        return;
    }
    for (int d = 0; d < SLOTS; d++) {
        char (*p)[NAME_LEN] = sets[d];
        if (p[0][0] || p[1][0] || p[2][0])
            fprintf(f, "%d %s %s %s\n", d + 1, p[0][0] ? p[0] : ".", p[1][0] ? p[1] : ".", p[2][0] ? p[2] : ".");
    }
    fclose(f);
}

typedef struct { uint64_t mgr, top, eu; } Ctx;

static int ctx_get(Cpu *c, Ctx *x) {
    uint64_t p = LD64(G(0x3a81678)), g = LD64(G(0x395dff0));
    if (!p || !g) return 0;
    x->mgr = LD64(g);
    x->eu = LD64(p + 0x480);
    uint32_t pouch = LD32(p + 0x188);
    x->top = p + 8 + (pouch < 2 ? pouch * 0xc0 : 0) + 0x40;
    if (!x->mgr || !x->eu) return 0;
    memset(scratch, 0, 16);
    return (int)(rt_call(c, G(0x16fac0), 4, x->mgr, (uint64_t)(uintptr_t)scratch, x->top, (uint64_t)0x07d8b094) & 1);
}

static int worn_get(Cpu *c, const Ctx *x, int part, int32_t *out) {
    uint64_t m = (uint64_t)(uintptr_t)scratch;
    ST32(m + 0x20, 0xffffffff);
    if (!(rt_call(c, G(0x19f9d4), 4, x->mgr, m + 0x20, m, (uint64_t)part_hash[part]) & 1)) return 0;
    *out = (int32_t)LD32(m + 0x20);
    return 1;
}

static int item_name(Cpu *c, const Ctx *x, int i, char *out) {
    uint64_t m = (uint64_t)(uintptr_t)scratch;
    memset(scratch + 0x40, 0, 0x30);
    if (!(rt_call(c, G(0x282ed8), 5, x->mgr, m + 0x40, x->top, (uint64_t)0x4290322e, (uint64_t)i) & 1)) return 0;
    if (!(rt_call(c, G(0x6e8e88), 4, x->mgr, m + 0x60, m + 0x40, (uint64_t)0x25efa387) & 1)) return 0;
    const char *s = (const char *)(uintptr_t)LD64(m + 0x60);
    if (!s || !*s) return 0;
    snprintf(out, NAME_LEN, "%s", s);
    return 1;
}

static int item_find(Cpu *c, const Ctx *x, const char *name) {
    char n[NAME_LEN];
    for (int i = 0; i < LIST_LEN; i++)
        if (item_name(c, x, i, n) && !strcmp(n, name)) return i;
    return -1;
}

static void set_equip(Cpu *c, const Ctx *x, int d) {
    int change = 0;
    for (int s = 0; s < 3; s++) {
        job.need[s] = 0;
        int32_t cur;
        if (!sets[d][s][0] || !worn_get(c, x, s, &cur)) continue;
        int want = item_find(c, x, sets[d][s]);
        if (want < 0) {
            rt_log("armor sets: %s is not in the inventory; that part stays", sets[d][s]);
        } else if (want != cur) {
            rt_call(c, G(0x16fdcc), 4, x->mgr, (uint64_t)(uint32_t)want, (uint64_t)(uintptr_t)scratch,
                    (uint64_t)part_hash[s]);
            job.need[s] = 1, job.want[s] = want, change = 1;
        }
    }
    job.active = change, job.frames = 0;
    rt_log("armor sets: set %d %s", d + 1, change ? "put on" : "already worn");
    input_rumble(0, 0x5000, 90);
}

// Each queued write is applied once the game took it.
static void job_step(Cpu *c, const Ctx *x) {
    int left = 0;
    for (int s = 0; s < 3; s++) {
        int32_t cur;
        if (!job.need[s]) continue;
        if (worn_get(c, x, s, &cur) && cur == job.want[s]) {
            rt_call(c, G(0xc4a5f4), 2, x->eu, (uint64_t)(9 + s));
            job.need[s] = 0;
        } else {
            left = 1;
        }
    }
    if (left && ++job.frames > WAIT_FRAMES) {
        rt_log("armor sets: the game did not take the change in time");
        left = 0;
    }
    job.active = left;
}

// A piece goes to the part its name ends with.
static void set_register(Cpu *c, const Ctx *x, int d, int item) {
    static const char *const part[3] = {"_Head", "_Upper", "_Lower"};
    char n[NAME_LEN];
    if (item < 0 || !item_name(c, x, item, n)) return;
    size_t len = strlen(n);
    for (int s = 0; s < 3; s++) {
        size_t l = strlen(part[s]);
        if (len > l && !strcmp(n + len - l, part[s])) {
            snprintf(sets[d][s], NAME_LEN, "%s", n);
            sets_store();
            rt_log("armor sets: set %d %s = %s", d + 1, part[s] + 1, n);
            input_rumble(0x7000, 0x7000, 220);
            return;
        }
    }
    rt_log("armor sets: %s is not a head, upper or lower piece", n);
}

// The strip's Sort hint and sort box mean nothing for sets: hidden while the sets show, every frame since the strip's
// animations may show them again, and put back as they were. A pane's bit 0 of +0x58 is its visibility. The Drop hint
// follows the cursor in the game's own update: host_strip_hint.
static uint64_t hint_pane[2];
static uint8_t hint_shown[2];
static int hints_hidden;

static void hints_find(Cpu *c, uint64_t scr) {
    static const char *const name[2] = {"Pa_Guide_01", "Pa_SortWindow_00"};
    for (int i = 0; i < 2; i++) {
        hint_pane[i] = rt_call(c, G(0x406760), 2, LD64(scr + 0x28), (uint64_t)(uintptr_t)name[i]);   // pane by name
        if (hint_pane[i]) hint_shown[i] = *(uint8_t *)(uintptr_t)(hint_pane[i] + 0x58) & 1;
    }
    hints_hidden = 1;
}

static void hints_update(void) {
    if (!hints_hidden) return;
    int sets_on = atomic_load(&strip_sets);
    for (int i = 0; i < 2; i++) {
        if (!hint_pane[i]) continue;
        uint8_t *flags = (uint8_t *)(uintptr_t)(hint_pane[i] + 0x58);
        *flags = (uint8_t)((*flags & 0xfe) | (sets_on ? 0 : hint_shown[i]));
    }
    hints_hidden = sets_on;
}

static void strip_switch(Cpu *c, const Ctx *x, uint64_t scr, int to_sets) {
    if (to_sets) {
        strip_n = 0;
        for (int d = 0; d < SLOTS; d++) {
            int item = -1;
            for (int s = 0; s < 3 && item < 0; s++)
                if (sets[d][s][0]) item = item_find(c, x, sets[d][s]);
            if (item >= 0) strip_slot[strip_n] = d, strip_item[strip_n] = item, strip_n++;
        }
        if (!strip_n) return;
        hints_find(c, scr);
    }
    atomic_store(&strip_sets, to_sets);
    rt_call(c, G(0x21828c8), 2, scr, (uint64_t)LD32(scr + 0x1b8));   // the strip opens again with the new tiles
}

// main+0xb9e5fc `mov x0, x19`, before the main loop's per-frame call.
static void host_frame(Cpu *c) {
    c->x[0] = c->x[19];   // the replaced instruction
    atomic_store(&seen_ms, now_ms());
    int clamp = atomic_load(&clamp_frames);
    if (clamp > 0) {
        atomic_store(&clamp_frames, clamp - 1);
        if (clamp == 1) *(uint8_t *)(uintptr_t)(atomic_load(&clamp_win) + 0x80) = 1;
    }
    uint64_t scr = atomic_load(&strip_screen);
    int open = 0;
    if (scr) {
        open = *(uint8_t *)(uintptr_t)(scr + 0x16e) == 2 && LD32(scr + 0x1b8) <= 2;
        if (*(uint8_t *)(uintptr_t)(scr + 0x1d8)) atomic_store(&strip_sets, 0);   // closed: the next one opens normally
    }
    atomic_store(&strip_open, open);
    hints_update();
    int req = atomic_exchange(&strip_req, 0), pick = atomic_exchange(&strip_equip, -1);
    int reg = atomic_exchange(&reg_slot, -1), clear = atomic_exchange(&clear_req, 0);
    if (!open || req == 2 - atomic_load(&strip_sets)) req = 0;   // 1 to the sets, 2 back: dropped when already there
    if (!req && pick < 0 && reg < 0 && !clear && !job.active) return;
    uint64_t keep[19];   // the loop's next instructions still use x8
    memcpy(keep, c->x, sizeof keep);
    Ctx x;
    if (ctx_get(c, &x)) {
        sets_load();
        if (clear) {
            memset(sets, 0, sizeof sets);
            sets_store();
            rt_log("armor sets: all sets cleared");
            input_rumble(0x7000, 0x7000, 220);
        }
        if (reg >= 0) set_register(c, &x, reg, atomic_load(&reg_item));
        if (req) strip_switch(c, &x, scr, req == 1);
        if (pick >= 0) set_equip(c, &x, pick);
        if (job.active) job_step(c, &x);
    }
    memcpy(c->x, keep, sizeof keep);
}

// ---- the item window. A row's text is a message object {char16 *text, u32 length, attributes -1}.
typedef struct { uint64_t text; uint32_t len, pad; uint64_t attr; } Msg;
static uint16_t row_text[PICK_ROWS][48];
static Msg row_msg[PICK_ROWS];

static uint64_t msg_make(uint16_t *buf, size_t cap, Msg *m, const char *utf8) {
    size_t n = utf8_to_utf16(utf8, buf, cap);
    *m = (Msg){.text = (uint64_t)(uintptr_t)buf, .len = (uint32_t)n, .attr = ~0ULL};
    return (uint64_t)(uintptr_t)m;
}

static void row_set(Cpu *c, uint64_t win, int row, int command, uint64_t text) {
    rt_call(c, G(0x2113788), 4, win, (uint64_t)row, (uint64_t)command, text);   // setButton(win, row, command, text)
    rt_call(c, G(0x2113874), 3, win, (uint64_t)row, (uint64_t)1);              // enabled
}

// main+0x2178708: the armour branch's tail (x20 screen, w21 the item); the game's Equip/Remove row 0 stays.
static void host_rows(Cpu *c) {
    uint64_t keep[19];
    memcpy(keep, c->x, sizeof keep);
    uint64_t win = LD64(c->x[20] + 0x1f0);
    atomic_store(&reg_item, (int)(uint32_t)c->x[21]);
    rt_call(c, G(0x2113874), 3, win, (uint64_t)0, (uint64_t)1);
    row_set(c, win, 1, CMD_REGISTER, msg_make(row_text[0], 48, &row_msg[0], ui_text(UI_ARMOR_REGISTER)));
    row_set(c, win, 2, CMD_CLEAR, msg_make(row_text[1], 48, &row_msg[1], ui_text(UI_ARMOR_CLEAR)));
    row_set(c, win, 3, 0, 0);   // Cancel
    memcpy(c->x, keep, sizeof keep);
}

// main+0x2179c28, in place of the call to the game's command handler (x0 screen, w1 command); 1 closes the window.
static void host_cmd(Cpu *c) {
    uint64_t screen = c->x[0];
    uint32_t cmd = (uint32_t)c->x[1];
    if (cmd == CMD_REGISTER) {
        uint64_t win = LD64(screen + 0x1f0);
        rt_call(c, G(0x2113774), 2, win, (uint64_t)PICK_FRAME);   // the Pattern frame of ten rows
        for (int i = 0; i < SLOTS; i++) {
            char t[64];
            snprintf(t, sizeof t, ui_text(UI_ARMOR_SET_FMT), i + 1);
            row_set(c, win, i, CMD_SLOT + i, msg_make(row_text[i], 48, &row_msg[i], t));
        }
        row_set(c, win, SLOTS, 0, 0);
        rt_call(c, G(0xd3b420), 2, win, (uint64_t)0);
        rt_call(c, G(0x21139b4), 2, win, (uint64_t)0);   // the cursor to the first row (it stayed on Register's)
        atomic_store(&clamp_win, win);
        atomic_store(&clamp_frames, 3);
        c->x[0] = 0;
    } else if (cmd == CMD_CLEAR) {   // asks first, with the cursor on Cancel
        uint64_t win = LD64(screen + 0x1f0);
        rt_call(c, G(0x2113774), 2, win, (uint64_t)0);   // the Pattern frame of two rows
        row_set(c, win, 0, CMD_CLEAR_YES, msg_make(row_text[0], 48, &row_msg[0], ui_text(UI_ARMOR_CLEAR_YES)));
        row_set(c, win, 1, 0, 0);
        rt_call(c, G(0xd3b420), 2, win, (uint64_t)0);
        rt_call(c, G(0x21139b4), 2, win, (uint64_t)1);
        atomic_store(&clamp_win, win);
        atomic_store(&clamp_frames, 3);
        c->x[0] = 0;
    } else if (cmd == CMD_CLEAR_YES) {
        atomic_store(&clear_req, 1);
        c->x[0] = 1;
    } else if (cmd >= CMD_SLOT && cmd < CMD_SLOT + SLOTS) {
        atomic_store(&reg_slot, (int)(cmd - CMD_SLOT));
        c->x[0] = 1;
    } else {
        rt_call(c, G(0x2173d18), 2, screen, (uint64_t)cmd);
    }
}

// ---- the strip
// main+0x20c7158 `ldr w23, [x22, #0x1b8]` in the tile filler: remembers the screen; in sets mode the shield path.
static void host_strip_type(Cpu *c) {
    atomic_store(&strip_screen, c->x[22]);
    c->x[23] = atomic_load(&strip_sets) ? 1 : LD32(c->x[22] + 0x1b8);
}

// main+0x20c71c4 `bl 0x2094520` (a shield tile): in sets mode an armour tile picturing set w2 (-1: the empty tile).
static void host_strip_tile(Cpu *c) {
    uint64_t a0 = c->x[0], a1 = c->x[1], a3 = c->x[3];
    int32_t i = (int32_t)c->x[2];
    if (!atomic_load(&strip_sets)) {
        rt_call(c, G(0x2094520), 4, a0, a1, (uint64_t)(uint32_t)i, a3);
        return;
    }
    int item = i >= 0 && i < strip_n ? strip_item[i] : -1;
    *(uint8_t *)(uintptr_t)a3 = 0;
    rt_call(c, G(0x2094b08), 4, a0, a1, (uint64_t)(uint32_t)item, a3);
}

// main+0x21829d4 `bl 0x2128848`: the strip's update reads the cursor each frame and shows the Drop hint on an item
// tile. In sets mode it reads the empty tile, so the game hides the hint itself.
static void host_strip_hint(Cpu *c) {
    c->x[0] = atomic_load(&strip_sets) ? 0 : LD32(LD64(c->x[0] + 0x50) + 0x8c);
}

// main+0x2183358 `bl 0x21266a0` (the strip closing applies its selection): in sets mode the set under the cursor goes
// on by the next frame instead, and the weapon, shield or bow stays.
static void host_strip_apply(Cpu *c) {
    uint64_t strip = c->x[0];
    if (!atomic_exchange(&strip_sets, 0)) {
        rt_call(c, G(0x21266a0), 2, strip, (uint64_t)(uint32_t)c->x[1]);
        return;
    }
    uint32_t tile = LD32(LD64(strip + 0x50) + 0x8c);
    if (tile >= 1 && (int)tile <= strip_n) atomic_store(&strip_equip, strip_slot[tile - 1]);
}

// main+0x2125db8 `bl 0xba1730` in the strip's cursor change (x19 the strip): the name under the strip.
static uint16_t label_text[64];
static Msg label_msg;
static void host_strip_label(Cpu *c) {
    uint64_t scr = atomic_load(&strip_screen);
    int tile = -1;
    if (atomic_load(&strip_sets) && scr && c->x[19] == LD64(scr + 0x1a8)) {
        static const char none[1];
        tile = (int)(int32_t)LD32(LD64(c->x[19] + 0x50) + 0x8c);   // w20 is reused on the item's path
        // The line under the name (Pa_GripEssence_00) comes from the game's item behind the tile; empty hides it.
        ST64(c->x[29] - 0x20, (uint64_t)(uintptr_t)none);
    }
    if (tile >= 1 && tile <= strip_n) {
        char t[64];
        snprintf(t, sizeof t, ui_text(UI_ARMOR_STRIP_FMT), strip_slot[tile - 1] + 1);
        c->x[2] = msg_make(label_text, 64, &label_msg, t);
    }
    rt_call(c, G(0xba1730), 5, c->x[0], c->x[1], c->x[2], c->x[3], c->x[4]);
}

// main+0x2183714 `bl 0x21287d8` and main+0x2183774 `bl 0x212882c` in the strip filler: tile count and first cursor.
static void host_strip_count(Cpu *c) {
    uint64_t n = atomic_load(&strip_sets) ? (uint64_t)strip_n + 1 : (uint32_t)c->x[1];
    rt_call(c, G(0x21287d8), 2, c->x[0], n);
}
static void host_strip_cursor(Cpu *c) {
    uint64_t t = atomic_load(&strip_sets) ? 1 : (uint32_t)c->x[1];
    rt_call(c, G(0x212882c), 2, c->x[0], t);
}

__attribute__((constructor)) static void armor_sets_register(void) {
    rt_register_host(HOST_STRIP_HINT, host_strip_hint);
    rt_register_host(HOST_FRAME, host_frame);
    rt_register_host(HOST_ROWS, host_rows);
    rt_register_host(HOST_CMD, host_cmd);
    rt_register_host(HOST_STRIP_TYPE, host_strip_type);
    rt_register_host(HOST_STRIP_TILE, host_strip_tile);
    rt_register_host(HOST_STRIP_APPLY, host_strip_apply);
    rt_register_host(HOST_STRIP_COUNT, host_strip_count);
    rt_register_host(HOST_STRIP_CURSOR, host_strip_cursor);
    rt_register_host(HOST_STRIP_LABEL, host_strip_label);
}

#else
void armor_sets_filter(uint64_t *buttons, int32_t *ry) { (void)buttons, (void)ry; }
#endif
