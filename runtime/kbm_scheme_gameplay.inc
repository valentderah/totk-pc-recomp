// kbm_scheme_gameplay.inc: the PC scheme's rows for the player's contexts. Taken unchanged from Weeping of
// the Empire (daniel-madrid-07/weeping-of-the-empire, MIT) engine/runtime/pc_scheme_gameplay.inc at v0.1.8.2;
// the row macros are kbm_scheme.c's, the scope tokens its KBM_SC_* enum, and every B_ / M_ / R_ token carries a
// K prefix: upstream's names are bit masks, input_map.h's are bit positions. See THIRD_PARTY_NOTICES.md.
// pc_scheme_gameplay.inc: the PC scheme's rows for the player's contexts. Included by pc_input.cpp after
// pc_scheme.inc, same row format (see there). Scopes: GAMEPLAY, WORLD (on foot), CLIMB, AIR, GLIDE, SWIM, AIM,
// ABILITY, HORSE, VEHICLE, CUTSCENE. Kinds beyond pc_scheme.inc's: M macro: op PCOP_NONE, arg = the macro id
// registered with pc_macro_register (pc_gameplay.h PcMacroId); out = the pad bits it presses, for the prompts'
// glyphs G the game's action bit: arg = n (0..63, PlayerVirtualKey's bit; 27..63 new splits), held while the key is
// down in the context (the hook derives trigger / hold / release per game frame) PC_NOLAYOUT(scope) ends that
// scope's fallback to the console layout for the keys it does not list.
//
// PC_VROW(scope, action, vk_mask, glyph_pad_bits, keys): the player's virtual keys, so a key sets only the bits
// that mean its action; until the VK hook has run once, its pad bits instead (stage 2a). PCVK(n) = bit n of enum
// game::player::VirtualKey (27: guard, the vk_guard).
#define PCVK(n) (1ull << (n))
// 4.23 (A): LMB is attack (MAC_ATTACK: VK 9 held, or with the bow in hand the bow's draw, whose release shoots). 4.26:
// RMB is the shield, guard + lock-on together as the pad's ZL (VK 2 | 27) on foot and in the air, VK 2 alone while
// aiming (the lock-on into the draw, the shield down); Q has no gameplay row of its own: the addendum gives it the
// holster (4.23's F job), F being the bow toggle. The action names stay "attack" / "lock_on" so a held key carries from
// WORLD into AIM (the draw) and AIR, and saved binds keep their meaning. One key per action (4.22): parry is RMB held +
// E and the dodge is RMB held + Space + a direction, the game's own ZL sequences, with no key of their own. Stage 2a:
// Npad bits and macros (no game patch). The leaves inherit only from GAMEPLAY, so each states its own verbs, and a key
// a leaf must not use has no row there: held from another context it goes dead.

// ---- every gameplay context
PC_ROW(GAMEPLAY, "move_up",       'H', PCOP_NONE, 0,             KM_U,        "W")
PC_ROW(GAMEPLAY, "move_down",     'H', PCOP_NONE, 0,             KM_D,        "S")
PC_ROW(GAMEPLAY, "move_left",     'H', PCOP_NONE, 0,             KM_L,        "A")
PC_ROW(GAMEPLAY, "move_right",    'H', PCOP_NONE, 0,             KM_R,        "D")
// Alt held: walk, W A S D's stick at 45 %. hle_hid's kWalk slot; shim_win.c keeps a lone Alt from opening the
// window menu
PC_ROW(GAMEPLAY, "walk",          'H', PCOP_NONE, 0,             KM_WALK,     "Alt")

// ---- on foot (3.1, 3.2)
PC_VROW(WORLD,    "jump",          PCVK(0) | PCVK(10) | PCVK(19), KB_X, "Space")
PC_ROW(WORLD,    "sprint",        'M', PCOP_NONE, MAC_SPRINT,    KB_B,        "Shift")   // VK 1 held; with something in hand a
                                                                // VK 5 tap first: put away, then sprint (4.25)
PC_VROW(WORLD,    "crouch",        PCVK(3), KB_LS, "Ctrl")
PC_VROW(WORLD,    "interact",      PCVK(4) | PCVK(16), KB_A, "E")   // + 16: the pad's A minus 7, so RMB + E parries (4.22)
PC_ROW(WORLD,    "attack",        'M', PCOP_NONE, MAC_ATTACK,    KB_Y,        "Mouse Left")   // hold: charge; the bow in hand: the draw
PC_VROW(WORLD,    "lock_on",       PCVK(2) | PCVK(27), KB_ZL, "Mouse Right")      // 4.26: shield + target, as ZL
PC_ROW(WORLD,    "holster",       'M', PCOP_NONE, MAC_HOLSTER,   KB_B,        "Q")            // 4.26 addendum: put away / take out
// F, the bow: a tap is the bow toggle at the release; held 250 ms it is the pad's ZR, which draws in one step
// wherever the pad's does (the ground, a jump, the glider, the saddle), and its release shoots. MAC_BOW_DRAW:
// MAC_ATTACK's bow mode, so on foot / in the air the draw is AIM (its rows, its glyphs)
PC_ROW(WORLD,   "bow_toggle",    'T', PCOP_NONE, MAC_BOW_TOGGLE, 0,          "F")            // tap: the bow out / away (4.26)
PC_ROW(WORLD,   "bow_toggle",    'M', PCOP_NONE, MAC_BOW_DRAW,  KB_ZR,       "F")            // hold: the pad's ZR, the draw; release shoots
PC_VROW(WORLD,    "throw",         PCVK(12), KB_R, "G")            // hold, release throws
PC_ROW(WORLD,    "ability_wheel", 'H', PCOP_NONE, 0,             KB_L,        "Tab")          // tap: the last ability; held: the wheel (4.13)
// (Tab before 4-9: the prompts' L is Tab, the key for whichever ability is chosen, not 4 = Ultrahand only)
PC_ROW(WORLD,    "ultrahand",     'M', PCOP_NONE, MAC_ABILITY_1, KB_L,        "4")
PC_ROW(WORLD,    "fuse",          'M', PCOP_NONE, MAC_ABILITY_2, KB_L,        "5")
PC_ROW(WORLD,    "ascend",        'M', PCOP_NONE, MAC_ABILITY_3, KB_L,        "6")
PC_ROW(WORLD,    "recall",        'M', PCOP_NONE, MAC_ABILITY_4, KB_L,        "7")
PC_ROW(WORLD,    "autobuild",     'M', PCOP_NONE, MAC_ABILITY_5, KB_L,        "8")
PC_ROW(WORLD,    "camera",        'M', PCOP_NONE, MAC_ABILITY_6, KB_L,        "9")
PC_ROW(WORLD,    "weapons",       'T', PCOP_NONE, 0,             0,          "1")     // tap: nothing (4.26: 1 only chooses;
                                                                // 4.23's take-out swung while running); the open strip
PC_ROW(WORLD,    "weapons",       'M', PCOP_NONE, MAC_WEAPON_STRIP, KB_DR,    "1")     // the strip; release latches melee
PC_ROW(WORLD,    "shields",       'H', PCOP_NONE, 0,             KB_DL,       "2")
PC_ROW(WORLD,    "materials",     'H', PCOP_NONE, 0,             KB_DU,       "C")
PC_ROW(WORLD,    "bows",          'T', PCOP_NONE, 0,             0,          "3")     // tap: nothing (4.26), as 1
PC_ROW(WORLD,    "bows",          'M', PCOP_NONE, MAC_BOW_STRIP, KB_ZR | KB_DR, "3")     // the bow strip; release equips, no shot
PC_ROW(WORLD,    "weapon_next",   'M', PCOP_NONE, MAC_NEXT,      KB_DR,       "Wheel Down")
PC_ROW(WORLD,    "weapon_prev",   'M', PCOP_NONE, MAC_PREV,      KB_DR,       "Wheel Up")
// the wheel click (MMB) is the pad's R stick click, as the mouse is the right stick; V stays
PC_VROW(WORLD,    "scope",         PCVK(17), KB_RS, "V, Mouse Middle")
PC_VROW(WORLD,    "whistle",       PCVK(18) | PCVK(21), KB_DD, "X")   // the pad's D-down: 18 the whistle, 21 carrying's
                                                                // "Far / Near"
PC_NOLAYOUT(WORLD)

// ---- the bow drawn (3.3, 4.23): LMB's MAC_ATTACK keeps ZR while LMB is held (the bow was in hand at the press), its
// release shoots (the pad's model); Esc is the cancel (B lowers the bow, no shot); RMB stays lock-on
PC_ROW(AIM,      "attack",        'M', PCOP_NONE, MAC_ATTACK,    KB_ZR,       "Mouse Left")
PC_ROW(AIM,      "quit",          'P', PCOP_NONE, 0,             KB_B,        "Esc")          // lower the bow without a shot
PC_ROW(AIM,      "pause",         'X', PCOP_NONE, 0,             0,          "")             // (Esc is the cancel here)
PC_VROW(AIM,      "jump",          PCVK(0) | PCVK(10) | PCVK(19), KB_X, "Space")
PC_ROW(AIM,      "sprint",        'M', PCOP_NONE, MAC_SPRINT,    KB_B,        "Shift")   // (VK 1 only: no put-away here)
PC_VROW(AIM,      "interact",      PCVK(4), KB_A, "E")
PC_VROW(AIM,      "crouch",        PCVK(3), KB_LS, "Ctrl")  // the crouch-shot
PC_VROW(AIM,      "lock_on",       PCVK(2), KB_ZL, "Mouse Right")  // a held RMB carries into the draw as the lock-on
                                                                   // alone: the shield goes down for the bow (4.26)
PC_ROW(AIM,      "holster",       'M', PCOP_NONE, MAC_HOLSTER,   KB_B,        "Q")            // the bow away (B: the cancel)
// F as on foot: a tap puts the bow away, a hold is the draw (ZR), so the aim's font prompts, on foot's set, name a
// working F for ZR
PC_ROW(AIM,      "bow_toggle",    'T', PCOP_NONE, MAC_BOW_TOGGLE, 0,         "F")            // tap: the bow away (B)
PC_ROW(AIM,      "bow_toggle",    'M', PCOP_NONE, MAC_BOW_DRAW,  KB_ZR,       "F")            // hold: the draw
PC_ROW(AIM,      "bows",          'M', PCOP_NONE, MAC_BOW_STRIP, KB_ZR | KB_DR, "3")     // (drawn: d-right only)
PC_ROW(AIM,      "materials",     'H', PCOP_NONE, 0,             KB_DU,       "C")            // D2: arrow fuse d-pad up
PC_ROW(AIM,      "arrow_next",    'M', PCOP_NONE, MAC_ARROW_NEXT, KB_DU,      "Wheel Down")   // d-up + a right-stick flick
PC_ROW(AIM,      "arrow_prev",    'M', PCOP_NONE, MAC_ARROW_PREV, KB_DU,      "Wheel Up")
PC_ROW(AIM,      "scope",         'P', PCOP_NONE, 0,             KB_RS,       "V, Mouse Middle")  // R3 while aiming: the game leaves the aim for the Scope, no bow zoom
// The pad's D-pad right/left changes bows while aiming, so the strips' 1/2 and the wheel's weapon step are not on foot's
// rows here: 1/2 do nothing, 3 opens the bow strip (ZR + d-right) and the wheel walks the arrows below.
PC_ROW(AIM,      "weapons",       'X', PCOP_NONE, 0,             0,          "")
PC_ROW(AIM,      "shields",       'X', PCOP_NONE, 0,             0,          "")
PC_ROW(AIM,      "weapon_next",   'X', PCOP_NONE, 0,             0,          "")
PC_ROW(AIM,      "weapon_prev",   'X', PCOP_NONE, 0,             0,          "")
PC_NOLAYOUT(AIM)

// ---- climbing (3.4): no sprint (B lets go); S + Space kicks off the wall (D2)
PC_VROW(CLIMB,    "jump",          PCVK(0) | PCVK(10) | PCVK(19), KB_X, "Space")   // VK 10 as AIR's: the glider opens on a
                                                                // VK 10 trigger (0x16e84f0), from WallJump 15 (CLIMB's flags) and
                                                                // in the 30 ms before a let-go's Fall reads AIR; nothing reads it
                                                                // while climbing
PC_VROW(CLIMB,    "let_go",        PCVK(5), KB_B, "Shift")     // the "Let go [B]" prompt draws B as Shift (4.11 P2, G5)
// the strips and the wheel on a wall: no Sequence module that opens them tests a climb verb, so these send the pad's
// own trigger and the game's gate decides, as for the pad. WORLD's action names, so a held key carries. No T rows: a
// tap of 1 is the pad's d-right tap (no take-out on a wall); no 3: there is no aim on a wall, and its ZR + d-right
// would be the weapon strip's d-right
PC_ROW(CLIMB,    "ability_wheel", 'H', PCOP_NONE, 0,             KB_L,        "Tab")
PC_ROW(CLIMB,    "weapons",       'M', PCOP_NONE, MAC_WEAPON_STRIP, KB_DR,    "1")
PC_ROW(CLIMB,    "shields",       'H', PCOP_NONE, 0,             KB_DL,       "2")
PC_ROW(CLIMB,    "materials",     'H', PCOP_NONE, 0,             KB_DU,       "C")
// pad parity: the pad's strips, the ability wheel and the abilities by id here too, with on foot's action
// names (a held key carries); the game's own gate decides what opens, as for the pad
PC_ROW(CLIMB,   "ultrahand",     'M', PCOP_NONE, MAC_ABILITY_1, KB_L,        "4")
PC_ROW(CLIMB,   "fuse",          'M', PCOP_NONE, MAC_ABILITY_2, KB_L,        "5")
PC_ROW(CLIMB,   "ascend",        'M', PCOP_NONE, MAC_ABILITY_3, KB_L,        "6")
PC_ROW(CLIMB,   "recall",        'M', PCOP_NONE, MAC_ABILITY_4, KB_L,        "7")
PC_ROW(CLIMB,   "autobuild",     'M', PCOP_NONE, MAC_ABILITY_5, KB_L,        "8")
PC_ROW(CLIMB,   "camera",        'M', PCOP_NONE, MAC_ABILITY_6, KB_L,        "9")
PC_VROW(CLIMB,    "dive",          PCVK(14), KB_R, "G")            // ExecutePlayerWallJump reads VK 14, the pad's R
PC_NOLAYOUT(CLIMB)

// ---- in the air, glider closed
PC_VROW(AIR,      "jump",          PCVK(0) | PCVK(10) | PCVK(19), KB_X, "Space")   // opens the glider: VK 0 alone did not
                                                                // the pad X minus 26 (horse / vehicle)
PC_ROW(AIR,      "sprint",        'M', PCOP_NONE, MAC_SPRINT,    KB_B,        "Shift")   // (the run carries from a running jump)
PC_VROW(AIR,      "interact",      PCVK(4) | PCVK(16), KB_A, "E")    // surf chord, RMB / Space / E as the pad's ZL / X / A
PC_VROW(AIR,      "lock_on",       PCVK(2) | PCVK(27), KB_ZL, "Mouse Right")  // the shield + target: held from the ground
                                                                   // it carries through the jump (doc 3.x; 2 + 16 held is the
                                                                   // surf's entry)
PC_VROW(AIR,      "dive",          PCVK(14), KB_R, "G")            // D2: R "Dive" / "Accelerate"; G is the pad's R everywhere,
                                                                  // so "Dive [G]" names the key
PC_ROW(AIR,      "attack",        'M', PCOP_NONE, MAC_ATTACK,    KB_Y,        "Mouse Left")   // the plunge / jump attack; the bow in hand: the midair draw
PC_ROW(AIR,     "bow_toggle",    'T', PCOP_NONE, MAC_BOW_OUT,  0,          "F")  // tap: the bow out, take-out only
PC_ROW(AIR,     "bow_toggle",    'M', PCOP_NONE, MAC_BOW_DRAW,  KB_ZR,       "F")            // hold: the pad's ZR, the midair draw
                                                                // (ExecutePlayerBow reads VK 13 with no air test)
// pad parity: the pad's strips, the ability wheel and the abilities by id here too, with on foot's action
// names (a held key carries); the game's own gate decides what opens, as for the pad
PC_ROW(AIR,      "ability_wheel", 'H', PCOP_NONE, 0,             KB_L,        "Tab")
PC_ROW(AIR,     "ultrahand",     'M', PCOP_NONE, MAC_ABILITY_1, KB_L,        "4")
PC_ROW(AIR,     "fuse",          'M', PCOP_NONE, MAC_ABILITY_2, KB_L,        "5")
PC_ROW(AIR,     "ascend",        'M', PCOP_NONE, MAC_ABILITY_3, KB_L,        "6")
PC_ROW(AIR,     "recall",        'M', PCOP_NONE, MAC_ABILITY_4, KB_L,        "7")
PC_ROW(AIR,     "autobuild",     'M', PCOP_NONE, MAC_ABILITY_5, KB_L,        "8")
PC_ROW(AIR,     "camera",        'M', PCOP_NONE, MAC_ABILITY_6, KB_L,        "9")
PC_ROW(AIR,     "weapons",       'T', PCOP_NONE, 0,             0,          "1")     // tap: nothing, as on foot
PC_ROW(AIR,     "weapons",       'M', PCOP_NONE, MAC_WEAPON_STRIP, KB_DR,    "1")
PC_ROW(AIR,     "shields",       'H', PCOP_NONE, 0,             KB_DL,       "2")
PC_ROW(AIR,     "materials",     'H', PCOP_NONE, 0,             KB_DU,       "C")
PC_ROW(AIR,     "bows",          'T', PCOP_NONE, 0,             0,          "3")
PC_ROW(AIR,     "bows",          'M', PCOP_NONE, MAC_BOW_STRIP, KB_ZR | KB_DR, "3")
PC_VROW(AIR,      "scope",         PCVK(17), KB_RS, "V, Mouse Middle")            // the Scope mid-air (slow motion on the pad)
PC_NOLAYOUT(AIR)
// ---- gliding: Space closes (B), G dives
PC_VROW(GLIDE,    "close_glider",  PCVK(5), KB_B, "Space, Shift")   // + Shift, the pad's B elsewhere: the glide's font B
                                                                // prompts draw on foot's Shift; a Shift held from a running
                                                                // jump stays dead until released
PC_VROW(GLIDE,    "dive",          PCVK(14), KB_R, "G")            // (the same gameplay glyph set as AIR)
PC_ROW(GLIDE,    "attack",        'M', PCOP_NONE, MAC_ATTACK,    KB_ZR,       "Mouse Left")   // a bow equipped: the pad's ZR, the draw from the glider; else VK 9
// The pad's whole A, not WORLD's VK 4 | 16 (the gust's button is set by code, D2 DoCommand "Gust!"): no glide reader
// uses A otherwise
PC_ROW(GLIDE,    "interact",      'H', PCOP_NONE, 0,             KB_A,        "E")
PC_ROW(GLIDE,   "bow_toggle",    'T', PCOP_NONE, MAC_BOW_OUT,  0,          "F")            // tap: take-out only (B closes the glider)
PC_ROW(GLIDE,   "bow_toggle",    'M', PCOP_NONE, MAC_BOW_DRAW,  KB_ZR,       "F")            // hold: the pad's ZR, the draw; release shoots
// pad parity: the pad's strips, the ability wheel and the abilities by id here too, with on foot's action
// names (a held key carries); the game's own gate decides what opens, as for the pad
PC_ROW(GLIDE,    "ability_wheel", 'H', PCOP_NONE, 0,             KB_L,        "Tab")
PC_ROW(GLIDE,   "ultrahand",     'M', PCOP_NONE, MAC_ABILITY_1, KB_L,        "4")
PC_ROW(GLIDE,   "fuse",          'M', PCOP_NONE, MAC_ABILITY_2, KB_L,        "5")
PC_ROW(GLIDE,   "ascend",        'M', PCOP_NONE, MAC_ABILITY_3, KB_L,        "6")
PC_ROW(GLIDE,   "recall",        'M', PCOP_NONE, MAC_ABILITY_4, KB_L,        "7")
PC_ROW(GLIDE,   "autobuild",     'M', PCOP_NONE, MAC_ABILITY_5, KB_L,        "8")
PC_ROW(GLIDE,   "camera",        'M', PCOP_NONE, MAC_ABILITY_6, KB_L,        "9")
PC_ROW(GLIDE,   "weapons",       'T', PCOP_NONE, 0,             0,          "1")     // tap: nothing, as on foot
PC_ROW(GLIDE,   "weapons",       'M', PCOP_NONE, MAC_WEAPON_STRIP, KB_DR,    "1")
PC_ROW(GLIDE,   "shields",       'H', PCOP_NONE, 0,             KB_DL,       "2")
PC_ROW(GLIDE,   "materials",     'H', PCOP_NONE, 0,             KB_DU,       "C")
PC_ROW(GLIDE,   "bows",          'T', PCOP_NONE, 0,             0,          "3")
PC_ROW(GLIDE,   "bows",          'M', PCOP_NONE, MAC_BOW_STRIP, KB_ZR | KB_DR, "3")
PC_NOLAYOUT(GLIDE)
// ---- swimming: Space is the swim dash (X, D2 and the VK reads); Shift has no swimming meaning
PC_VROW(SWIM,     "swim_dash",     PCVK(0) | PCVK(19), KB_X, "Space")
PC_VROW(SWIM,     "interact",      PCVK(4), KB_A, "E")
// LMB: the swim attack (SelectorPlayerSwimSelector / ExecutePlayerSwimAttack read VK 9, the pad's Y). On foot's macro,
// so a held LMB carries in and out of the water as everywhere
PC_ROW(SWIM,     "attack",        'M', PCOP_NONE, MAC_ATTACK,    KB_Y,        "Mouse Left")
// RMB: the pad's ZL, which ExecutePlayerSwimMove reads held (pad parity; the lock-on / target as on foot)
PC_VROW(SWIM,     "lock_on",       PCVK(2), KB_ZL, "Mouse Right")
PC_NOLAYOUT(SWIM)

// ---- an ability in use (3.5); the macros branch on pc_ctx_ability() where the abilities differ. Esc (leave) and the
// pause's 'X' are pc_scheme.inc's, in both gameplay modes
PC_ROW(ABILITY,  "use",           'M', PCOP_NONE, MAC_PRIMARY,   KB_A,        "Mouse Left")   // grab / pick / build / photo; Fuse: weapon (Y)
PC_ROW(ABILITY,  "secondary",     'M', PCOP_NONE, MAC_SECONDARY, KB_A | KB_L,  "Mouse Right")  // Ultrahand release, Recall stop, Fuse: shield (ZL)
PC_ROW(ABILITY,  "pull",          'M', PCOP_NONE, MAC_PULL,      KB_DU,       "Wheel Up")
PC_ROW(ABILITY,  "push",          'M', PCOP_NONE, MAC_PUSH,      KB_DD,       "Wheel Down")
PC_ROW(ABILITY,  "rotate",        'M', PCOP_NONE, MAC_ROTATE,    KB_R | KB_DL | KB_DR, "R")   // R + the mouse: the d-pad (D2 Rotate: Left / Right "Horizontal rotation" names R; Up / Down stay the wheel's)
PC_VROW(ABILITY,  "reset_rotation",PCVK(2), KB_ZL, "Q")            // D2: ZL "Reset"
PC_ROW(ABILITY,  "raise",         'M', PCOP_NONE, MAC_RAISE,     0,          "Space")
PC_ROW(ABILITY,  "lower",         'M', PCOP_NONE, MAC_LOWER,     0,          "Ctrl")
PC_ROW(ABILITY,  "unstick",       'M', PCOP_NONE, MAC_UNSTICK,   0,          "X")  // X
                                                                // Backspace was out of its reach)
PC_NOLAYOUT(ABILITY)

// ---- horse (3.6)
PC_VROW(HORSE,    "spur",          PCVK(4), KB_A, "Shift")
PC_VROW(HORSE,    "jump",          PCVK(0) | PCVK(26), KB_X, "Space")   // + 26: SelectorPlayerSchedulerRideJumpAttack
                                                                // reads it (the jump attack from the saddle; the pad's X sets
                                                                // 0 / 10 / 19 / 26)
PC_VROW(HORSE,    "dismount",      PCVK(5), KB_B, "E")
PC_VROW(HORSE,    "soothe",        PCVK(24), KB_L, "R")
PC_VROW(HORSE,    "reset_camera",  PCVK(2), KB_ZL, "Mouse Right")  // the horse's ZL: the lock-on key
PC_ROW(HORSE,    "attack",        'M', PCOP_NONE, MAC_ATTACK,    KB_Y,        "Mouse Left")
PC_ROW(HORSE,    "whistle",       'P', PCOP_NONE, 0,             KB_DD,       "X")
// the Scope from the saddle (pad parity: VK 17 is read by the arms layer, SelectorPlayerSubSchedulerNone, in any
// state); V and the wheel click as on foot
PC_VROW(HORSE,    "scope",         PCVK(17), KB_RS, "V, Mouse Middle")
// F: the bow out in the saddle. Take-out only, as in the air: ExecutePlayerRide reads VK 5 itself (the get-off), so
// the put-away tap could dismount. LMB then draws (MAC_ATTACK, the bow in hand)
PC_ROW(HORSE,   "bow_toggle",    'T', PCOP_NONE, MAC_BOW_OUT,  0,          "F")            // tap: take-out only (VK 5 is the get-off)
PC_ROW(HORSE,   "bow_toggle",    'M', PCOP_NONE, MAC_BOW_DRAW,  KB_ZR,       "F")            // hold: the pad's ZR, the draw; release shoots
// pad parity (scheme §2): the strips in the saddle (no Tab / 4-9: the pad's L is the soothe on a horse)
PC_ROW(HORSE,   "weapons",       'T', PCOP_NONE, 0,             0,          "1")     // tap: nothing, as on foot
PC_ROW(HORSE,   "weapons",       'M', PCOP_NONE, MAC_WEAPON_STRIP, KB_DR,    "1")
PC_ROW(HORSE,   "shields",       'H', PCOP_NONE, 0,             KB_DL,       "2")
PC_ROW(HORSE,   "materials",     'H', PCOP_NONE, 0,             KB_DU,       "C")
PC_ROW(HORSE,   "bows",          'T', PCOP_NONE, 0,             0,          "3")
PC_ROW(HORSE,   "bows",          'M', PCOP_NONE, MAC_BOW_STRIP, KB_ZR | KB_DR, "3")
PC_NOLAYOUT(HORSE)

// ---- Zonai vehicles (3.7)
PC_ROW(VEHICLE,  "attack",        'M', PCOP_NONE, MAC_ATTACK,    KB_Y,        "Mouse Left")   // hit a device on / off
// Shift lets go, as on a wall (the rides' B prompts draw Shift)
PC_VROW(VEHICLE,  "interact",      PCVK(4) | PCVK(16), KB_A, "E")
PC_ROW(VEHICLE,  "let_go",        'M', PCOP_NONE, MAC_VEHICLE_LEAVE, KB_B,  "Shift")
// F, the bow: hold = the pad's ZR where the game allows a draw, tap = the take- out request (the vehicle gate refuses it:
// nothing)
PC_ROW(VEHICLE,  "bow_toggle",    'T', PCOP_NONE, MAC_BOW_OUT,   0,          "F")
PC_ROW(VEHICLE,  "bow_toggle",    'M', PCOP_NONE, MAC_BOW_DRAW,  KB_ZR,       "F")
PC_VROW(VEHICLE,  "jump",          PCVK(0) | PCVK(26), KB_X, "Space")   // stick: jump off; construct: it jumps (26);
                                                                          // telescope: leaves (26); sled: nothing
// (Mineru's construct, RideRobot 25: no other VEHICLE executor (Manipulate, RideSled, RideTelescopeBase)
// reads VK 2 / 6 / 12)
PC_NOLAYOUT(VEHICLE)

// ---- Mineru's construct (RideRobot 25: "the controls overlap", G attached and attacked at once, no key to get off, G
// / Tab odd). The game's guide (D2 GolemNormalMode): ZL Guard, L Left Hand, R Right Hand, Y Back Part, X Attach;
// Golem*Mode / GolemPreGetOff: B Cancel. One action per key: the mouse buttons are the arms (the right one the main
// hand), F the back part, Shift the guard, E the attach (its window: a part key only picks, MAC_CONSTRUCT_ATTACH),
// Space gets off (MAC_CONSTRUCT_LEAVE), Esc the inventory as on foot (the attach's cancel in its window)
PC_VROW(CONSTRUCT, "right_arm",    PCVK(12), KB_R, "Mouse Left")    // the right arm's device (its punch): VK 12, held
PC_VROW(CONSTRUCT, "left_arm",     PCVK(6), KB_L, "Mouse Right")    // the left arm's device: VK 6
PC_VROW(CONSTRUCT, "back_part",    PCVK(9), KB_Y, "Q")  // the back part's device: VK 9
// F, the bow: held, the pad's ZR (ExecutePlayerRideRobot reads VK 13: "you can fire arrows while riding the construct"),
// release shoots; a tap is the take-out request, which the vehicle gate refuses
PC_ROW(CONSTRUCT, "bow_toggle",   'T', PCOP_NONE, MAC_BOW_OUT,   0,          "F")
PC_ROW(CONSTRUCT, "bow_toggle",   'M', PCOP_NONE, MAC_BOW_DRAW,  KB_ZR,       "F")
PC_VROW(CONSTRUCT, "guard",        PCVK(2), KB_ZL, "Shift")         // VK 2 held
PC_ROW(CONSTRUCT, "attach",        'M', PCOP_NONE, MAC_CONSTRUCT_ATTACH, KB_X | KB_A, "E")  // X near a device, else A
PC_ROW(CONSTRUCT, "get_off",       'M', PCOP_NONE, MAC_CONSTRUCT_LEAVE, KB_B, "Space")   // B:
                                                                // the guide's "B Cancel" (attach, PreGetOff) draws Space
PC_NOLAYOUT(CONSTRUCT)

// ---- the ability wheel and the d-pad strips open (4.13: UIShortCutMainItemScreen / UIShortCutScreen, the gameplay
// class; its parent is on foot, so Tab / 1 / 2 / C and the rest keep their WORLD rows and the opening key's hold
// carries). The context feeds the mouse and the wheel to the R stick; LMB confirms, Esc cancels.
PC_ROW(QUICKMENU, "confirm",       'P', PCOP_NONE, 0,             KB_A,        "Mouse Left")
PC_ROW(QUICKMENU, "quit",          'P', PCOP_NONE, 0,             KB_B,        "Esc")
PC_ROW(QUICKMENU, "pause",         'X', PCOP_NONE, 0,             0,          "")
PC_ROW(QUICKMENU, "attack",        'X', PCOP_NONE, 0,             0,          "")
PC_ROW(QUICKMENU, "lock_on",       'X', PCOP_NONE, 0,             0,          "")             // (LMB confirms here)
PC_ROW(QUICKMENU, "holster",       'X', PCOP_NONE, 0,             0,          "")
PC_ROW(QUICKMENU, "bow_toggle",    'X', PCOP_NONE, 0,             0,          "")             // (F's VK 5 / take-out: the same)
PC_ROW(QUICKMENU, "sort",          'P', PCOP_NONE, 0,             KB_Y,        "T")  // the strip's Sort [Y]
// the strip's "X Drop" (drop a material, e.g. into a fire to roast it): dead before, Space's on-foot jump row gave
// the strip a VK it never reads. On the key its prompt drew (Space), the pad's X
PC_ROW(QUICKMENU, "drop",          'P', PCOP_NONE, 0,             KB_X,        "Space")
// the arrows: the D-pad (the Autobuild list's "D-pad Up / Down: Select" and its Left / Right view; the strips
// take the D-pad as the pad does), as in the menus
PC_ROW(QUICKMENU, "dpad_up",       'H', PCOP_NONE, 0,             KB_DU,       "Up")
PC_ROW(QUICKMENU, "dpad_down",     'H', PCOP_NONE, 0,             KB_DD,       "Down")
PC_ROW(QUICKMENU, "dpad_left",     'H', PCOP_NONE, 0,             KB_DL,       "Left")
PC_ROW(QUICKMENU, "dpad_right",    'H', PCOP_NONE, 0,             KB_DR,       "Right")
PC_ROW(QUICKMENU, "jump",          'X', PCOP_NONE, 0,             0,          "")
PC_ROW(QUICKMENU, "crouch",        'X', PCOP_NONE, 0,             0,          "")             // (on foot's VK rows: a strip reads the pad)
PC_ROW(QUICKMENU, "interact",      'X', PCOP_NONE, 0,             0,          "")
PC_ROW(QUICKMENU, "throw",         'X', PCOP_NONE, 0,             0,          "")
PC_ROW(QUICKMENU, "scope",         'X', PCOP_NONE, 0,             0,          "")
PC_ROW(QUICKMENU, "whistle",       'X', PCOP_NONE, 0,             0,          "")
PC_ROW(QUICKMENU, "weapon_next",   'E', PCOP_NONE, 0,             KB_DR,       "Wheel Down")   // the notch steps the open strip
PC_ROW(QUICKMENU, "weapon_prev",   'E', PCOP_NONE, 0,             KB_DR,       "Wheel Up")  // (the feed), not MAC_NEXT

// ---- the Purah Pad camera: the mouse aims, the prompts' own keys (Esc quit and LMB / E capture are pc_scheme.inc's,
// both gameplay modes). The zoom and the album are not routed yet (no button for them in the game's data, D2: the
// zoom is in code; to boot-check first)
#ifdef PC_HAS_CAMERA
PC_ROW(CAMERA,   "self_portrait", 'P', PCOP_NONE, 0,             KB_X,        "Space")
// RMB held: the self-portrait's second set of poses (ExecutePlayerSelfie reads VK 2 held, the pad's ZL: with the
// stick, other poses. On foot's lock_on name (RMB = the pad's ZL everywhere), so one bind moves both. No prompt names
// it. L's release there is the ability's cancel, Esc's
PC_VROW(CAMERA,   "lock_on",       PCVK(2), KB_ZL, "Mouse Right")
// the bar's Zoom and Album (AppMenuOverlay 1009 / 1011, buttons set in code, the Switch: the D-pad zooms, -
// opens the Album): the wheel is the zoom (6 frames of D-pad up / down a notch, as Ultrahand's pull / push),
// and M, the - everywhere else, is the Album here instead of leaving for the map
PC_ROW(CAMERA,   "zoom_in",       'M', PCOP_NONE, MAC_PULL,      KB_DU,       "Wheel Up")
PC_ROW(CAMERA,   "zoom_out",      'M', PCOP_NONE, MAC_PUSH,      KB_DD,       "Wheel Down")
PC_ROW(CAMERA,   "map",           'P', PCOP_NONE, 0,             KB_MINUS,    "M")            // the Album
// (the bar's slots: B Quit, A Capture, X Self-Portrait / Landscape Shot (the same slot), - Album, D-pad Up / Down
// Zoom: every one has its key above Full Screen is the Album / Compendium viewer's Y, T in the Purah Pad)
PC_NOLAYOUT(CAMERA)
#endif

// ---- the Scope: Esc quit and LMB place-pin are pc_scheme.inc's (both modes). V, which opened it, closes it (the
// "Quit" prompt's B). The zoom and PC_NOLAYOUT(SCOPE) wait for the zoom probe: no source names the Scope's zoom
// input, and NOLAYOUT before it would leave it dead
#ifdef PC_HAS_SCOPE
PC_ROW(SCOPE,    "scope",         'P', PCOP_NONE, 0,             KB_B,        "V, Mouse Middle")   // (the key that opened it)
// (without it the console layout answered here, RMB closing the Scope into the bow, 1 / 3 swapped; the Scope
// has no zoom input, so nothing waits on the probe)
PC_NOLAYOUT(SCOPE)
#endif
#undef PCVK
