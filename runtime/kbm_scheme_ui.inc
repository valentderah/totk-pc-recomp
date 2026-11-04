// kbm_scheme_ui.inc: the PC scheme's menu, title, dialogue and choice rows. Taken unchanged from Weeping of
// the Empire (daniel-madrid-07/weeping-of-the-empire, MIT) engine/runtime/pc_scheme.inc at v0.1.8.2; the row
// macros are kbm_scheme.c's, the scope tokens its KBM_SC_* enum, and every B_ / M_ / R_ token carries a K
// prefix: upstream's names are bit masks, input_map.h's are bit positions. See THIRD_PARTY_NOTICES.md.
// pc_scheme.inc: the PC scheme's defaults. The single source: tools/engine/test_pc_scheme.py checks the document's
// stage tables against these rows.
//
// PC_ROW(scope, action, kind, op, arg, out, keys)
//   scope   a context (pc_input.h PcCtx, without PCX_) or a group: ROOT > GAMEPLAY (WORLD, CUTSCENE ...) and UI (TITLE,
//           DIALOGUE, CHOICE, MENUS > SAVES PAUSE POPUP PAD MAP OPTIONS SHOP MENU). A row applies to its scope's
//           contexts that have no row of their own for the action; settings.ini overrides the keys per level:
//           controls.<ctx>.<action>, controls.<group>.<action>, controls.<action> (the most specific wins).
//   action  a stable id, the same in every context (the settings key's last part)
//   kind    H held: the pad bits while the key is held in this context
//           P press: the pad bits for 70 ms, once per press O op: the menu op at the press, else as P F then: as P, and once the context has
//           changed (within 1.5 s) the op there (Esc in the world: + opens the pause menu, then tab_goto(8) walks to System, D4) E external:
//           claimed for another part (the clicks, the map's mouse), nothing here; its key is the prompt's glyph. Not rebindable
//           (pc_bind_set: PCB_FIXED): those parts read their own fixed keys K keys only: as E for the router (claimed, nothing here), but
//           the part that acts reads the action's keys through pc_bind_get, so it is rebindable (no row uses it since 4.22 removed AIM's
//           "fire") X none: the action has no key in this scope (a row that removes an inherited one) T tap: a release within 250 ms of the
//           press is a tap, played at the release (the out bits for 70 ms, and the macro arg started, the key already up, when arg names
//           one: the take-out / put-away are one-frame virtual-key taps guarded on the hands state); held 250 ms, the key becomes the NEXT
//           row's, which has the same scope, action and keys and a held kind (H / G / M / P), pressed at that moment. The pair is one
//           action: one binding moves both
//   out     hle_hid action bits (KB_A .. KB_DD: the Npad buttons; KM_L .. KM_D: the left stick's directions)
//   keys    hle_hid's key names, comma separated, first = the prompt's glyph; "Wheel Up" / "Wheel Down". A letter or
//           digit names a POSITION: the key there on a US keyboard, read through the layout in use (W A S D is Z Q S D
//           on AZERTY; settings.ini values the same)
//
// One key per action: the pointer acting on what it points at is the menu's pointer, not a binding. Stage 1 (menus,
// title, dialogue, I / M / Esc in the world). Everything a UI context does not list does nothing there; the world keeps
// the console layout (TOTK_KEYBOARD_LAYOUT) for what it does not list until stage 2 routes 3.1-3.5.

// ---- UI: menus, title, dialogue, choices (3.8-3.10)
PC_ROW(UI,       "click",       'E', PCOP_NONE,        0, KB_A,         "Mouse Left")
PC_ROW(UI,       "confirm",     'O', PCOP_CONFIRM,     0, KB_A,         "Enter")
PC_ROW(UI,       "back",        'O', PCOP_BACK,        0, KB_B,         "Esc, Mouse Right")
PC_ROW(UI,       "focus_up",    'H', PCOP_NONE,        0, KM_U,         "W")
PC_ROW(UI,       "focus_down",  'H', PCOP_NONE,        0, KM_D,         "S")
PC_ROW(UI,       "focus_left",  'H', PCOP_NONE,        0, KM_L,         "A")
PC_ROW(UI,       "focus_right", 'H', PCOP_NONE,        0, KM_R,         "D")
PC_ROW(MENUS,    "tab_prev",    'O', PCOP_TAB_PREV,    0, KB_L,         "Q")
PC_ROW(MENUS,    "tab_next",    'O', PCOP_TAB_NEXT,    0, KB_R,         "E")
PC_ROW(MENUS,    "scroll_up",   'O', PCOP_SCROLL,     -1, KB_DU,        "Wheel Up")
PC_ROW(MENUS,    "scroll_down", 'O', PCOP_SCROLL,      1, KB_DD,        "Wheel Down")
// the arrows are the D-pad in every menu, held: the game's own repeat; on the map the D-pad's own layers (up / down)
// and Hero's Path (left / right), as the Switch
PC_ROW(UI,       "dpad_up",     'H', PCOP_NONE,        0, KB_DU,        "Up")
PC_ROW(UI,       "dpad_down",   'H', PCOP_NONE,        0, KB_DD,        "Down")
PC_ROW(UI,       "dpad_left",   'H', PCOP_NONE,        0, KB_DL,        "Left")
PC_ROW(UI,       "dpad_right",  'H', PCOP_NONE,        0, KB_DR,        "Right")
// the recipe book (H the cooking history). RecipeCard_00 over the pause menu (the TMS_RECIPE): the pad's B closes it
// (the sequence's, no close in the screen), from the history B goes back to the cards; the UI controller's Y opens
// the history in the Food tab. Esc and R are B here, not the menus' back op (it would walk the pause menu under the
// book) "close_book" first, so a B prompt draws R
PC_ROW(RECIPE,   "close_book",  'P', PCOP_NONE,        0, KB_B,         "R")
PC_ROW(RECIPE,   "back",        'P', PCOP_NONE,        0, KB_B,         "Esc, Mouse Right")
PC_ROW(RECIPE,   "history",     'P', PCOP_NONE,        0, KB_Y,         "H")
// The menu hotkeys from any menu: I the inventory (the last item tab), M the map, J the Adventure Log; within a screen
// a tab, across screens close and open (the PCOP_MENU_GOTO chain), the menu's own key closes it. Declined (no chain
// yet, a screen it does not know): the pad bits, which are what each key did before (I in the pause menu: +; M on the
// map / Pad: -; J in the Log: B), or nothing where the key was dead. Esc in a menu is UI's back, one level, closing at
// the top
PC_ROW(PAUSE,    "inventory",   'O', PCOP_MENU_GOTO, PCMG_LAST, KB_PLUS,      "I")
PC_ROW(PAUSE,    "map",         'O', PCOP_MENU_GOTO, PCMG_PAD + 2, KB_MINUS,  "M")   // (declined: the pad's -, the Purah
                                                                // Pad; and the menus' set then draws M for the map's / Pad's -)
PC_ROW(PAUSE,    "adventure_log", 'O', PCOP_MENU_GOTO, PCMG_PAD + 1, 0,      "J")
// F = X and T = Y in every menu: the pause menu's details / sort, the popups, the Purah Pad and the Log, the map's
// Hero's Path / Sensor, shops, Options and every other menu. Not the save list, where X / Y can copy or delete a save
// (SAVES X rows below): a stray key must not delete one. The pause menu keeps its own pair ahead of the recipe book's R
// Every pad button a menu can prompt has a key. One glyph table draws every menu (the pause menu's), so a key is the
// same button in all of them: ZL / ZR on X / V (Z / C are the inventory's Rotate, 10-30); - on M and + on I, their
// screens' own keys (the pause menu / the Pad / the map keep their gotos on M / I, these are the rest's); the stick
// clicks on Ctrl (on foot's LS, the crouch) and the wheel click (MMB, the right stick's, as in play)
PC_ROW(MENUS,    "zl",          'P', PCOP_NONE,        0, KB_ZL,        "X")
PC_ROW(MENUS,    "zr",          'P', PCOP_NONE,        0, KB_ZR,        "V")
PC_ROW(MENUS,    "map",         'P', PCOP_NONE,        0, KB_MINUS,     "M")
PC_ROW(MENUS,    "inventory",   'P', PCOP_NONE,        0, KB_PLUS,      "I")
PC_ROW(MENUS,    "l_stick_click", 'P', PCOP_NONE,      0, KB_LS,        "Ctrl")
PC_ROW(MENUS,    "r_stick_click", 'P', PCOP_NONE,      0, KB_RS,        "Mouse Middle")
PC_ROW(MENUS,    "y_prompt",    'P', PCOP_NONE,        0, KB_Y,         "T")
PC_ROW(MENUS,    "x_prompt",    'P', PCOP_NONE,        0, KB_X,         "F")
PC_ROW(SAVES,    "y_prompt",    'X', PCOP_NONE,        0, 0,           "")
PC_ROW(SAVES,    "x_prompt",    'X', PCOP_NONE,        0, 0,           "")
PC_ROW(PAUSE,    "y_prompt",    'P', PCOP_NONE,        0, KB_Y,         "T")   // sort
PC_ROW(PAUSE,    "x_prompt",    'P', PCOP_NONE,        0, KB_X,         "F")   // details
// R opens the recipe book from the Food tab, as R closes it: the pad's X there (MAC_RECIPE_OPEN, only on the Food
// tab: pause sub 5). After x_prompt, so the X prompts keep F's glyph
PC_ROW(PAUSE,    "recipe_book", 'M', PCOP_NONE, MAC_RECIPE_OPEN, KB_X,     "R")
// H: the Food tab's "View history", the pad's Y there, as H in the open book (RECIPE "history"); after y_prompt, so
// the Y prompts keep T's glyph; elsewhere nothing
PC_ROW(PAUSE,    "history",     'M', PCOP_NONE, MAC_RECIPE_HISTORY, KB_Y,  "H")
// the inventory's Rotate: the R stick (the KR_L / KR_R, rotate.patch), on Z / C, 10-30: Rotate is used far more than the
// shops' ZL / ZR, which take X / V) LMB-drag on Link is the (every menu: the shops' "Rotate" prompt is RS left / right
// too, ShopMenu_00 Pa_GuideRS_00; one glyph table draws every menu, so Z / C are the R stick's left / right in all of
// them)
PC_ROW(MENUS,    "rotate_left", 'H', PCOP_NONE,        0, KR_L,         "Z")
PC_ROW(MENUS,    "rotate_right",'H', PCOP_NONE,        0, KR_R,         "C")
PC_ROW(POPUP,    "y_prompt",    'P', PCOP_NONE,        0, KB_Y,         "T")
PC_ROW(POPUP,    "x_prompt",    'P', PCOP_NONE,        0, KB_X,         "F")
PC_ROW(PAD,      "y_prompt",    'P', PCOP_NONE,        0, KB_Y,         "T")   // (the Log too: PADLOG is the Pad's)
PC_ROW(PAD,      "x_prompt",    'P', PCOP_NONE,        0, KB_X,         "F")
PC_ROW(MAP,      "y_prompt",    'P', PCOP_NONE,        0, KB_Y,         "T")   // Sensor
PC_ROW(MAP,      "x_prompt",    'P', PCOP_NONE,        0, KB_X,         "F")   // Hero's Path
PC_ROW(PAD,      "map",         'O', PCOP_MENU_GOTO, PCMG_PAD + 2, KB_MINUS,  "M")
PC_ROW(PAD,      "inventory",   'O', PCOP_MENU_GOTO, PCMG_LAST, 0,           "I")
PC_ROW(PAD,      "adventure_log", 'O', PCOP_MENU_GOTO, PCMG_PAD + 1, 0,      "J")
PC_ROW(PAD,      "scroll_up",   'E', PCOP_NONE,        0, 0,           "Wheel Up")           // hle_hid's map zoom: the screen
PC_ROW(PAD,      "scroll_down", 'E', PCOP_NONE,        0, 0,           "Wheel Down")         // guess cannot tell the map tab
PC_ROW(PADLOG,   "adventure_log", 'O', PCOP_MENU_GOTO, PCMG_PAD + 1, KB_B,    "J")                  // J again leaves the Log
PC_ROW(MAP,      "map",         'O', PCOP_MENU_GOTO, PCMG_PAD + 2, KB_MINUS,  "M")
PC_ROW(MAP,      "inventory",   'O', PCOP_MENU_GOTO, PCMG_LAST, 0,           "I")
PC_ROW(MAP,      "adventure_log", 'O', PCOP_MENU_GOTO, PCMG_PAD + 1, 0,      "J")
// (the map's layers, sky / surface / depths, are the D-pad's up / down and the Hero's Path its left / right, held: UI's
// arrows rows, as the Switch PgUp / PgDn and, 10-30 18:10, provisional)
PC_ROW(MAP,      "click",       'E', PCOP_NONE,        0, KB_A,         "Mouse Left")         // hle_hid's map: A while held at the reticle (4.29; the mouse scrolls it)
PC_ROW(MAP,      "scroll_up",   'E', PCOP_NONE,        0, 0,           "Wheel Up")           // hle_hid's map: the zoom
PC_ROW(MAP,      "scroll_down", 'E', PCOP_NONE,        0, 0,           "Wheel Down")
PC_ROW(DIALOGUE, "click",       'X', PCOP_NONE,        0, 0,           "")                   // (no focus box: the click advances)
PC_ROW(DIALOGUE, "confirm",     'P', PCOP_NONE,        0, KB_A,         "E, Enter")           // advance / the whole line (Enter: scheme 3.14)
// Esc / RMB: the pad's B, whatever it does in a conversation, never a skip of the scene (the game has none here),
// with nothing destructive
PC_ROW(DIALOGUE, "back",        'P', PCOP_NONE,        0, KB_B,         "Esc, Mouse Right")
PC_ROW(DIALOGUE, "log",         'P', PCOP_NONE,        0, KB_Y,         "T")                  // the conversation's Log (its "Log T" hint, i2c_stable3)
PC_ROW(CHOICE,   "confirm",     'O', PCOP_CONFIRM,     0, KB_A,         "E, Enter")
PC_ROW(CHOICE,   "choice_1",    'O', PCOP_CHOICE_PICK, 1, 0,           "1")
PC_ROW(CHOICE,   "choice_2",    'O', PCOP_CHOICE_PICK, 2, 0,           "2")
PC_ROW(CHOICE,   "choice_3",    'O', PCOP_CHOICE_PICK, 3, 0,           "3")
PC_ROW(CHOICE,   "choice_4",    'O', PCOP_CHOICE_PICK, 4, 0,           "4")
PC_ROW(CHOICE,   "log",         'P', PCOP_NONE,        0, KB_Y,         "T")                  // (the Log from the answer list too)
PC_ROW(TITLE,    "confirm",     'O', PCOP_CONFIRM,     0, KB_A,         "Enter")

// ---- an event: talk or a cutscene with no focused menu (3.8's Event rows; the context comes from the player push's
// event flag, the producer): advance, the game's skip prompt, every other key dead the loading screen: its "A Next
// tip"; every other key stays dead there
PC_ROW(LOADING,  "next_tip",    'P', PCOP_NONE,        0, KB_A,         "E, Enter, Mouse Left")
PC_ROW(CUTSCENE, "confirm",     'P', PCOP_NONE,        0, KB_A,         "E, Enter")
PC_ROW(CUTSCENE, "skip",        'P', PCOP_NONE,        0, KB_PLUS,      "Esc")
PC_ROW(CUTSCENE, "skip_x",      'P', PCOP_NONE,        0, KB_X,         "Space")              // a short scene's "Skip X" (its hint "Skip SPACE", i2c_dye3)
PC_NOLAYOUT(CUTSCENE)

// ---- a quickmenu: the L ability wheel, the D-pad strips (4.13; on foot's rows otherwise, the mouse as the R stick:
// pc_route)

// ---- the world (3.1's stage-1 rows) The menu hotkeys from the world (4.25 3c): the chain opens the screen (+ /
// -) and walks to the page. Declined (the cursor off): + / - alone, the screen where it was left
PC_ROW(GAMEPLAY, "pause",       'O', PCOP_MENU_GOTO, PCMG_LAST, KB_PLUS,      "Esc")
PC_ROW(GAMEPLAY, "inventory",   'O', PCOP_MENU_GOTO, PCMG_LAST, KB_PLUS,      "I")  // the last page shown, System too
PC_ROW(GAMEPLAY, "map",         'O', PCOP_MENU_GOTO, PCMG_PAD + 2, KB_MINUS,  "M")
PC_ROW(GAMEPLAY, "adventure_log", 'O', PCOP_MENU_GOTO, PCMG_PAD + 1, KB_MINUS, "J")   // the Log page

// ---- Esc backs out of the camera and of an ability in both gameplay modes: B, and the world's pause chain dead
// there; the camera's capture too (A). The modes' other keys come from pc_scheme_gameplay.inc (gameplay = pc) or the
// console layout (gameplay = console).
PC_ROW(CAMERA,   "quit",        'P', PCOP_NONE,        0, KB_B,         "Esc")
PC_ROW(CAMERA,   "capture",     'P', PCOP_NONE,        0, KB_A,         "Mouse Left")     // (LMB captures in both modes)
PC_ROW(CAMERA,   "pause",       'X', PCOP_NONE,        0, 0,           "")
// an ability chosen with Tab is left with Tab: B. Tab's press is dead in the world it goes back to (a P row's key), so
// the same press does not open the wheel. Its own action, not "quit": ROOT's "quit" entry (Esc: the camera, the Scope,
// the quick menu, the bow) would otherwise move it too. Esc backs out of an ability as well: B, the pause row dead.
// "leave" stays first, so the B glyph ("Quit" / "Cancel") is Tab
PC_ROW(ABILITY,  "leave",       'P', PCOP_NONE,        0, KB_B,         "Tab")
PC_ROW(ABILITY,  "quit",        'P', PCOP_NONE,        0, KB_B,         "Esc")
PC_ROW(ABILITY,  "pause",       'X', PCOP_NONE,        0, 0,           "")
// the Scope (doc 4.18): the same, LMB places the pin (A, the prompt's button). No map row reaches it (gameplay class);
// the gameplay file adds V = quit and the zoom
PC_ROW(SCOPE,    "quit",        'P', PCOP_NONE,        0, KB_B,         "Esc")
PC_ROW(SCOPE,    "place_pin",   'P', PCOP_NONE,        0, KB_A,         "Mouse Left")
PC_ROW(SCOPE,    "pause",       'X', PCOP_NONE,        0, 0,           "")
