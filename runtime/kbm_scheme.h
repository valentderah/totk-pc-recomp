// kbm_scheme.h: the context-aware keyboard and mouse scheme, ported from Weeping of the Empire
// (daniel-madrid-07/weeping-of-the-empire, MIT; engine/runtime/pc_input.h and pc_scheme*.inc, v0.1.8.2).
//
// The scheme is a table of rows: (context, action) -> the pad bits the game is given and the keys that ask for
// them. A row applies to the contexts of its scope that have no row of their own for the action, so the world's
// and the menus' rows never see each other's keys, and a key held across a context change goes dead where the new
// context has no row for it. kbm_scheme.c holds the engine; kbm_scheme_ui.inc and kbm_scheme_gameplay.inc are the
// rows, unchanged from upstream but for a K on the pad-bit names.
//
// What this port does today, and what waits for a game hook: the upstream port ends every action on the game's own
// PlayerVirtualKey word and runs macros for the actions no single button means (the ability wheel's picks, the
// weapon strip, the bow's take-out). Neither needs a patch here: a macro row holds its pad bits (upstream's stage
// 2a) and a tap-only row pulses them for KBM_PULSE_MS, which is the closest the pad layer gets. The contexts past
// the menus (WORLD, AIM, CLIMB...) need the game's player state; kbm_scheme_frame takes whatever the caller pushes
// and KBM_SC_WORLD is the fallback.
//
// Esc is the game's key here: the scheme gives it an id (KEY_ESC) and the host menu keeps F1 while the scheme runs
// (input_kbm_scheme). The table's "pause" opens the game's own pause menu, its "back" leaves a menu, its "quit"
// cancels an aim, a camera or an ability. See docs/kbm-port.md.
#pragma once
#include <stdint.h>

#include "input_map.h"

// The pads and sticks a row asks for (input_map.h's action bits), and the walk modifier, which is not a pad bit:
// with it the left stick's rows are scaled to KBM_WALK_SCALE (upstream: Alt, "W A S D's stick at 45 %").
#define KBM_OUT_WALK (1u << 24)
#define KBM_WALK_SCALE 0.45f

enum { KBM_PULSE_MS = 70, KBM_TAP_MS = 250 };

// The scopes: the two groups (GAMEPLAY, UI with MENUS under it) and the contexts the rows are written for. The
// numbers are private; use the KBM_SC_ names.
enum {
    KBM_SC_ROOT, KBM_SC_GAMEPLAY, KBM_SC_UI, KBM_SC_MENUS,
    KBM_SC_WORLD, KBM_SC_OVERLAY, KBM_SC_TITLE, KBM_SC_SAVES, KBM_SC_PAUSE, KBM_SC_POPUP, KBM_SC_PAD, KBM_SC_MAP,
    KBM_SC_OPTIONS, KBM_SC_SHOP, KBM_SC_MENU, KBM_SC_DIALOGUE, KBM_SC_CHOICE, KBM_SC_CUTSCENE, KBM_SC_TRANSITION,
    KBM_SC_CLIMB, KBM_SC_AIR, KBM_SC_GLIDE, KBM_SC_SWIM, KBM_SC_AIM, KBM_SC_ABILITY, KBM_SC_HORSE, KBM_SC_VEHICLE,
    KBM_SC_LOADING, KBM_SC_QUICKMENU, KBM_SC_PADLOG, KBM_SC_CAMERA, KBM_SC_SCOPE, KBM_SC_CONSTRUCT, KBM_SC_RECIPE,
    KBM_SC_COUNT
};

// Parses the rows and builds the per-context tables; call once before a frame. Any thread, but before the first
// kbm_scheme_frame (the runtime does both from its input init).
void kbm_scheme_init(void);
// TOTK_KBM_BINDS overrides, applied before the table is parsed: "action=key,key|action=key", or "context.action=key"
// for one context (the names of kbm_scheme_scope_name). A context entry wins over a global one for its rows.
void kbm_scheme_set_binds(const char *spec);
// The F1 "Keyboard & mouse" tab: every action, and the actions a context writes rows for directly, in table order.
int kbm_scheme_action_count(void);
const char *kbm_scheme_action_name(int index);
int kbm_scheme_action_keys(const char *action, int *keys, int max);   // the first row's keys, any context
int kbm_scheme_action_default_keys(const char *action, int *keys, int max);   // the table's own keys, no override
int kbm_scheme_scope_action_count(int scope);
const char *kbm_scheme_scope_action(int scope, int index);
// 0 no override, 1 a global one, 2 a context one: the tab shows it and Delete clears that entry.
int kbm_scheme_bind_kind(int scope, const char *action);
const char *kbm_scheme_bind(int scope, const char *action);   // the override's keys, or NULL
// The context the caller pushed (KBM_SC_*); KBM_SC_WORLD until it pushes another.
void kbm_scheme_set_ctx(int scope);
int kbm_scheme_ctx(void);
// The context for a frame comes from runtime/screen_watch.c (screen_watch_ctx): the game's own screen, PAUSE while the
// F1 menu is open, WORLD otherwise.
const char *kbm_scheme_scope_name(int scope);

// One poll: down[key] != 0 for the keys held, now_ms a monotonic millisecond clock (the same one every poll).
// *buttons gets the pad bits (0..15) the rows ask for; left/right the left and right sticks (up +y, -32767..32767)
// their direction rows make, the walk row scaled. Clears *buttons, left and right first. The stick directions
// (16..23) are the caller's to set from the sticks it ends up with (input.c's stick_bits), not this function's.
void kbm_scheme_frame(const uint8_t down[KEY_COUNT], uint64_t now_ms, uint64_t *buttons, int32_t left[2],
                      int32_t right[2]);

// The keys of (scope, action), first first, at most max; returns how many, 0 for an action the scope does not
// have. For diagnostics and the prompt glyphs the port does not draw yet.
int kbm_scheme_keys(int scope, const char *action, int *keys, int max);
// How many rows the table holds (a diagnostic: the upstream table is 255).
int kbm_scheme_row_count(void);
// Names in the table's keys that the key map does not know (a diagnostic: 0 when the port is complete).
int kbm_scheme_unknown_key_count(void);
// The length of one presented frame, in ms (0: 16, 60 fps). The wheel's strip step counts frames, as upstream's macro
// does, so the host feeds the measured interval here and the step plays the same at 30, 60 or 120 fps.
void kbm_scheme_set_frame_ms(unsigned ms);
