// Catalog of the text the host shows the player (ui_text.h). One array per catalog language, indexed by UiTextId: a new
// string gets an id in ui_text.h and an entry in every language that has one (a missing entry falls back to English).
// The software keyboard has every language; the menu has English, Russian, Brazilian Portuguese and Simplified
// Chinese.
#include <stdatomic.h>
#include <string.h>

#include "cfg.h"
#include "ov.h"
#include "platform/platform.h"
#include "ui_text.h"

typedef struct { const char *lang; const char *const *text; } Row;

// The language names are the same in every row.
#define LANG_NAMES \
    [UI_MENU_LANGNAME_EN] = "English", [UI_MENU_LANGNAME_JA] = "日本語", [UI_MENU_LANGNAME_FR] = "Français", \
    [UI_MENU_LANGNAME_DE] = "Deutsch", [UI_MENU_LANGNAME_IT] = "Italiano", [UI_MENU_LANGNAME_ES] = "Español", \
    [UI_MENU_LANGNAME_NL] = "Nederlands", [UI_MENU_LANGNAME_RU] = "Русский", \
    [UI_MENU_LANGNAME_ZH_HANS] = "简体中文", [UI_MENU_LANGNAME_ZH_HANT] = "繁體中文", [UI_MENU_LANGNAME_KO] = "한국어", \
    [UI_MENU_LANGNAME_PT_BR] = "Português (Brasil)"

// SWKBD: TITLE, PROMPT, DEFAULT_NAME (at most 9 UTF-16 units, no '@' '%' '\'), OK, CANCEL, PAD_HINT.
#define SWKBD(title, prompt, name, ok, cancel, hint) \
    [UI_SWKBD_TITLE] = title, [UI_SWKBD_PROMPT] = prompt, [UI_SWKBD_DEFAULT_NAME] = name, [UI_SWKBD_OK] = ok, \
    [UI_SWKBD_CANCEL] = cancel, [UI_SWKBD_PAD_HINT] = hint, LANG_NAMES

static const char *const text_en[UI_TEXT_COUNT] = {
    SWKBD("Text entry", "Enter a name", "Horse", "OK", "Cancel", "Controller: A = OK, B = Cancel"),

    [UI_MENU_TITLE] = "Settings",
    [UI_MENU_TAB_SCREEN] = "Screen", [UI_MENU_TAB_GAME] = "Game", [UI_MENU_TAB_MODS] = "Mods", [UI_MENU_TAB_SOUND] = "Sound",
    [UI_MENU_TAB_CONTROLS] = "Controls", [UI_MENU_TAB_AMIIBO] = "Amiibo", [UI_MENU_TAB_MENU] = "Menu", [UI_MENU_TAB_ADVANCED] = "Advanced",
    [UI_MENU_CONTINUE] = "Continue", [UI_MENU_QUIT] = "Quit game",
    [UI_MENU_ON] = "On", [UI_MENU_OFF] = "Off", [UI_MENU_AUTO] = "Automatic", [UI_MENU_DEFAULT_MARK] = "default",
    [UI_MENU_AFTER_RESTART] = "after restart", [UI_MENU_SET_BY_ENV] = "set by environment variable",
    [UI_MENU_SAVE_FAILED] = "could not save settings",
    [UI_MENU_VERSION] = "TotK Recomp %s, game %s",

    [UI_MENU_FULLSCREEN] = "Fullscreen", [UI_MENU_FULLSCREEN_DESC] = "Fills the screen with the game; F11 toggles it.",
    [UI_MENU_WINDOW_SIZE] = "Window size", [UI_MENU_WINDOW_SIZE_DESC] = "Size of the game window when not fullscreen.",
    [UI_MENU_WINDOW_SIZE_CUSTOM] = "Custom",
    [UI_MENU_PRESENT] = "Presentation", [UI_MENU_PRESENT_DESC] = "How finished frames reach the screen.",
    [UI_MENU_PRESENT_MAILBOX] = "Mailbox", [UI_MENU_PRESENT_VSYNC] = "VSync", [UI_MENU_PRESENT_IMMEDIATE] = "Immediate",
    [UI_MENU_PRESENT_IN_USE] = "in use: %s",
    [UI_MENU_GPU] = "Graphics card", [UI_MENU_GPU_DESC] = "The Vulkan device that draws the game.",
    [UI_MENU_GPU_AUTO] = "Automatic",
    [UI_MENU_PERF_OVERLAY] = "Performance overlay",
    [UI_MENU_PERF_OVERLAY_DESC] = "Frame rate in the top right corner. Full adds frame times with a graph, CPU, GPU and "
                                  "memory load, temperatures and the render thread's waits. F10 cycles the modes.",
    [UI_MENU_PERF_OFF] = "Off", [UI_MENU_PERF_FPS] = "FPS", [UI_MENU_PERF_FULL] = "Full",
    // printf format of the overlay line in FPS mode: the frame rate
    [UI_PERF_LINE_FPS] = "%.0f fps",
    [UI_MENU_GRAPHICS] = "Picture",
    [UI_MENU_OUTPUT_RES] = "Game resolution",
    [UI_MENU_OUTPUT_RES_DESC] = "The size the game draws its picture and interface in. Above 1920 x 1080 the game needs "
                                "more video memory and GPU time. A wider size (21:9) widens the camera's view and moves "
                                "the HUD to the screen's corners; a taller one (16:10, Steam Deck) moves it to the top "
                                "and bottom edges.",
    [UI_MENU_RENDER_SCALE] = "3D render scale",
    [UI_MENU_RENDER_SCALE_DESC] = "Size of the 3D scene relative to the game resolution; the game's FSR scales it to "
                                  "that size. Above 100 % smooths jagged edges (supersampling) at a high GPU cost.",
    [UI_MENU_RENDER_SCALE_GAME] = "Game (83 %)",
    [UI_MENU_UPSCALER] = "Scaling to the window",
    [UI_MENU_UPSCALER_DESC] = "How the picture is scaled when the window is larger than the game resolution. FSR keeps "
                              "edges and detail sharper than bilinear.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1", [UI_MENU_UPSCALER_BILINEAR] = "Bilinear",
    [UI_MENU_ASPECT] = "Picture shape",
    [UI_MENU_ASPECT_DESC] = "When the window's shape differs from the game resolution's (a 16:9 picture on a 21:9 "
                            "screen): keep the picture's shape with black bars, or stretch it over the window.",
    [UI_MENU_ASPECT_FIT] = "Black bars", [UI_MENU_ASPECT_STRETCH] = "Stretch",
    [UI_MENU_FOV] = "Field of view",
    [UI_MENU_FOV_DESC] = "How much of the world the camera shows, in percent of the game's own. Game version 1.4.3 only.",
    [UI_MENU_SHADOW_RES] = "Shadow map size",
    [UI_MENU_SHADOW_RES_DESC] = "Size of the sun's shadow map. The game's own size changes with the view, down to about 400 "
                                "near the ground, so leaf shadows look blocky and jump; a fixed size keeps them still and "
                                "sharper. Above 1024 the shadows cost more GPU time and video memory.",
    [UI_MENU_SHADOW_RES_GAME] = "Game (changes)",
    [UI_MENU_ANISO] = "Texture filtering",
    [UI_MENU_ANISO_DESC] = "Anisotropic filtering of surface textures: ground, roads and walls seen at a shallow angle stay sharp into the distance instead of blurring. Game: the level the game asks for, as on the Switch.",
    [UI_MENU_ANISO_GAME] = "Game",
    [UI_MENU_SHARPNESS] = "FSR sharpness",
    [UI_MENU_SHARPNESS_DESC] = "Sharpening after FSR scaling; 0 turns it off.",
    [UI_MENU_COLOR] = "Colour",
    [UI_MENU_COLOR_PRESET] = "Colour profile",
    [UI_MENU_COLOR_PRESET_DESC] = "Sets the four values below. Vivid: more contrast and saturation, shadows opened a "
                                  "little (made for OLED screens). Clarity: a little more contrast, saturation and "
                                  "brightness. Moving a value makes the profile Custom. The HUD changes with the picture; "
                                  "this menu does not.",
    [UI_MENU_COLOR_DEFAULT] = "Game's own", [UI_MENU_COLOR_VIVID] = "Vivid (OLED)", [UI_MENU_COLOR_CLARITY] = "Clarity",
    [UI_MENU_COLOR_CUSTOM] = "Custom",
    [UI_MENU_COLOR_BRIGHTNESS] = "Brightness", [UI_MENU_COLOR_BRIGHTNESS_DESC] = "Added to every colour; 0 leaves it.",
    [UI_MENU_COLOR_CONTRAST] = "Contrast",
    [UI_MENU_COLOR_CONTRAST_DESC] = "Spreads colours away from mid-grey (above 100 %) or towards it; 100 % leaves them.",
    [UI_MENU_COLOR_SATURATION] = "Saturation",
    [UI_MENU_COLOR_SATURATION_DESC] = "Colour strength against grey; 100 % leaves it.",
    [UI_MENU_COLOR_GAMMA] = "Gamma",
    [UI_MENU_COLOR_GAMMA_DESC] = "Below 1 brightens dark and middle tones, above 1 darkens them; 1 leaves them.",
    [UI_MENU_HDR_GROUP] = "HDR",
    [UI_MENU_HDR] = "HDR output",
    [UI_MENU_HDR_DESC] = "Bright highlights on an HDR display. Needs HDR turned on in Windows (\"Use HDR\") or in a "
                         "Wayland compositor with colour management; without it the picture stays SDR. Applies after a "
                         "restart. The colour profile above is for SDR and is not used in HDR.",
    [UI_MENU_HDR_PEAK] = "Peak brightness",
    [UI_MENU_HDR_PEAK_DESC] = "Your display's peak in nits; the brightest highlights stop there.",
    [UI_MENU_HDR_GAME] = "Game brightness",
    [UI_MENU_HDR_GAME_DESC] = "Nits of the picture's white (paper white). 203 is the HDR reference; raise it in a "
                              "bright room.",
    [UI_MENU_HDR_UI] = "HUD and menu brightness",
    [UI_MENU_HDR_UI_DESC] = "Nits of the HUD's and this menu's white.",
    [UI_MENU_HDR_HUE] = "Highlight hue correction",
    [UI_MENU_HDR_HUE_DESC] = "Bright colours such as fire and sparks turn towards white as in the game's own SDR picture, "
                             "matching the light they cast; 0 keeps them fully saturated.",
    [UI_MENU_SHADOW_SCALE] = "Shadow resolution",
    [UI_MENU_SHADOW_SCALE_DESC] = "Draws the sun's shadow maps this many times larger on each side than the game's 960x960: "
                                  "sharper shadow edges and leaf shadows. Takes effect at the next start.",
    [UI_MENU_PCSS] = "Soft shadows",
    [UI_MENU_PCSS_DESC] = "Sun shadows sharp where an object meets the ground and softer the further the shadow falls "
                          "(PCSS). 0 keeps the game's filter. Takes effect at the next start.",
    [UI_MENU_CONTACT_SHADOWS] = "Contact shadows",
    [UI_MENU_CONTACT_SHADOWS_DESC] = "Small sun shadows the shadow maps miss: at feet, under stones, in grass and creases. "
                                     "Traced on the screen after lighting. Takes effect at the next start.",
    [UI_MENU_SHADOW_RANGE] = "Shadow cascade range",
    [UI_MENU_SHADOW_RANGE_DESC] = "Moves the steps where shadows turn softer (the game's 20, 40, 80 and 160 m) further "
                                  "away. Spreads the shadow map over more ground: raise Shadow resolution with it. "
                                  "Takes effect at the next start.",
    [UI_MENU_DLAA] = "NVIDIA DLSS / DLAA",
    [UI_MENU_DLAA_DESC] = "NVIDIA DLSS for the 3D scene (NVIDIA RTX GPUs; nvngx_dlss.dll next to totk.exe): smoother edges "
                          "and less shimmer. The HUD is not touched. The mode below sets the resolution it works from. Takes effect "
                          "after a restart.",
    [UI_MENU_DLSS] = "DLSS mode",
    [UI_MENU_DLSS_DESC] = "DLAA draws the 3D scene at full resolution. Quality, Balanced and Performance draw it at 67, 58 "
                          "and 50 % and let DLSS scale it up, for more frames per second; they replace the 3D render "
                          "scale. Takes effect after a restart.",
    [UI_MENU_DLSS_DLAA] = "DLAA (native)", [UI_MENU_DLSS_QUALITY] = "Quality", [UI_MENU_DLSS_BALANCED] = "Balanced",
    [UI_MENU_DLSS_PERFORMANCE] = "Performance",
    [UI_MENU_FXAA] = "Game anti-aliasing (FXAA)",
    [UI_MENU_FXAA_DESC] = "The game's own FXAA edge smoothing. It blurs fine detail a little; with DLAA on it is not needed.",

    [UI_MENU_FPS_LIMIT] = "Frame rate limit",
    [UI_MENU_FPS_LIMIT_DESC] = "The most frames per second the game draws. Above 60 is experimental and needs Game speed "
                               "follows the clock.",
    [UI_MENU_FPS_30] = "30 FPS", [UI_MENU_FPS_40] = "40 FPS", [UI_MENU_FPS_45] = "45 FPS",
    [UI_MENU_FPS_60] = "60 FPS", [UI_MENU_FPS_90] = "90 FPS", [UI_MENU_FPS_120] = "120 FPS", [UI_MENU_FPS_144] = "144 FPS",
    // the monitor's refresh rate; printf format with the rate in Hz when it is known
    [UI_MENU_FPS_DISPLAY] = "Monitor rate", [UI_MENU_FPS_DISPLAY_HZ] = "Monitor rate (%.0f Hz)",
    [UI_MENU_REALTIME] = "Game speed follows the clock",
    [UI_MENU_REALTIME_DESC] = "Steps the game by real time, so a slow frame does not slow the game down.",
    [UI_MENU_CLOTH_REALTIME] = "Cloth on the clock",
    [UI_MENU_CLOTH_REALTIME_DESC] = "Steps cloth and hair by real time as well.",
    [UI_MENU_HIGHFPS_FIX] = "High frame rate fixes",
    [UI_MENU_HIGHFPS_FIX_DESC] = "Corrects physics that misbehave above 30 frames per second, and objects that stay see-through or invisible above 60.",
    [UI_MENU_GAME_LANG] = "Game language", [UI_MENU_GAME_LANG_DESC] = "Language of the game's text and voices.",
    [UI_MENU_GAME_LANG_AUTO] = "Automatic",
    [UI_MENU_OPERATION_MODE] = "Console mode",
    [UI_MENU_OPERATION_MODE_DESC] = "Whether the game thinks it runs docked or handheld.",
    [UI_MENU_MODE_DOCKED] = "Docked", [UI_MENU_MODE_HANDHELD] = "Handheld",
    [UI_MENU_ASSUME_FOCUS] = "Keep running out of focus",
    [UI_MENU_ASSUME_FOCUS_DESC] = "On (the default), the game keeps running while you are in another window, and the "
                                  "keyboard stays quiet until the window is in front again. Off, it pauses when its "
                                  "window loses focus, as on the Switch when Home is pressed; a load interrupted that "
                                  "way can stop for good.",

    [UI_MENU_MODS_BUILTIN] = "Built into the port", [UI_MENU_MODS_INSTALLED] = "Installed mods",
    [UI_MENU_MODS_NONE] = "No mods yet. Unpack a mod into the mods folder, one folder per mod with its romfs folder "
                          "inside, and restart the game. TKMM can export its merged mods into a folder there too.",
    [UI_MENU_MODS_OPEN] = "Open mods folder",
    [UI_MENU_MODS_OPEN_DESC] = "Shows the folder the game loads mods from. It is created if it is missing.",
    [UI_MENU_MODS_ROW_DESC] = "Game files it replaces: %u. New files it adds: %u.",
    [UI_MENU_MODS_OVERRIDDEN] = "%u of its files come from %s, which is higher in the list.",
    [UI_MENU_MODS_MOVE_HINT] = "The top face button or Ctrl+Up moves it up, the left face button or Ctrl+Down moves it "
                               "down. When two mods change the same file, the higher one wins. Changes apply after a "
                               "restart.",
    [UI_MENU_MODS_EMPTY] = "This folder has no romfs, exefs or cheats files.",
    [UI_MENU_MODS_SUBSDK] = "This mod brings its own game code (a subsdk plugin or a replaced main). The port cannot "
                            "load code modules, so the whole mod stays off.",
    [UI_MENU_MODS_EXEFS_LATER] = "This mod patches the game code (exefs). The game builds the patches in when it "
                                 "starts next (a rebuild of a few minutes); until then the whole mod stays off.",
    [UI_MENU_MODS_CHEATS] = "Cheats: %d. Switch them on the Cheats tab.",
    [UI_MENU_MODS_CHEATS_FILE] = "Cheats for this game version are read at start from cheats\\%s in this mod.",
    [UI_MENU_MODS_BAD_NAME] = "The folder name contains |, which the mod list uses between names. Rename the folder.",
    [UI_MENU_MODS_OTHER_VERSION] = "%u of its files are made for another game version and are left out. Look for a "
                                   "version of this mod made for your game version.",
    [UI_MENU_MODS_UNUSED] = "not used",
    [UI_MENU_MODS_FPS60] = "60 FPS patch",
    [UI_MENU_MODS_FPS60_DESC] = "The port's own patch for 60 frames per second. Set it with Frame rate limit on the "
                                "Game tab.",
    [UI_MENU_MODS_RES] = "Resolution patch",
    [UI_MENU_MODS_RES_DESC] = "The port's own patch for resolutions above 1080p. Set it with Game resolution on the "
                              "Screen tab.",
    [UI_MENU_MODS_QUICKMENU] = "Quick menu fix",
    [UI_MENU_MODS_QUICKMENU_DESC] = "The port's fix for the black background of the weapon, shield and material menus "
                                    "at 60 FPS.",

    [UI_MENU_VOLUME] = "Volume", [UI_MENU_VOLUME_DESC] = "Master volume of the game.",

    [UI_MENU_MOTION] = "Motion controls", [UI_MENU_MOTION_DESC] = "Aiming with the controller's gyroscope.",
    [UI_MENU_RUMBLE_STRENGTH] = "Vibration strength",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "How strong the controller vibrates.",
    [UI_MENU_RUMBLE_CURVE] = "Vibration curve",
    [UI_MENU_RUMBLE_CURVE_DESC] = "Lower values make weak vibration stronger.",
    [UI_MENU_RUMBLE_TRIGGERS] = "Trigger vibration (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Vibrates the triggers of an Xbox controller too.",
    [UI_MENU_PAD_LAYOUT] = "Button layout",
    [UI_MENU_PAD_LAYOUT_DESC] = "Which button is A. Nintendo: the right one, as on a Switch. Xbox: the bottom one, as "
                                "labelled on an Xbox controller (cross on a PlayStation controller). Automatic picks "
                                "Nintendo for controllers with Nintendo labels and Xbox for the others. Which "
                                "controller the game's prompts show is set under Button prompts.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Automatic", [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A right)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A bottom)",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "in use: %s",
    [UI_MENU_PROMPTS] = "Button prompts",
    [UI_MENU_PROMPTS_DESC] = "Which controller's buttons the game's prompts show. Nintendo: the game's own. Xbox: LB, "
                             "RB, LT, RT, Menu, View and a d-pad cross, and A, B, X and Y as labelled on the button "
                             "the layout above presses (with Nintendo (A right) the right button is labelled B, so the "
                             "prompts for A show B). The game reads its prompts at start, so they change after a "
                             "restart. They are part of the game's files, so a button you remap in Controls keeps its "
                             "old prompt on screen.",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo", [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_MISSING] = "prompt files missing: run the setup again",
    [UI_MENU_PROMPTS_FAILED] = "prompt files damaged: run the setup again",
    [UI_MENU_PROMPTS_RESTART] = "prompts change after restart",
    [UI_MENU_PROMPTS_OFF_SHAPE] = "off at a game resolution not 16:9",
    [UI_MENU_PROMPTS_OFF_MODS] = "off: a RomFS mod replaces their files",
    [UI_MENU_PROMPTS_OFF_PATCHES] = "off while TOTK_ROMFS_PATCHES is set",
    [UI_MENU_RESTART] = "Restart now",
    [UI_MENU_RESTART_DESC] = "Closes the game and starts it again with the new prompts. Progress since your last save "
                             "is lost.",
    [UI_MENU_RESTART_CONFIRM] = "Restart the game now? Progress since your last save is lost.",
    [UI_MENU_RESTART_YES] = "Restart",
    [UI_MENU_RESTART_SAVING] = "The game is saving: Restart is possible in a moment.",
    [UI_MENU_LSTICK_DEADZONE] = "Left stick dead zone", [UI_MENU_RSTICK_DEADZONE] = "Right stick dead zone",
    [UI_MENU_STICK_DEADZONE_DESC] = "How far a gamepad stick moves before the game sees it. Raise it if the character or the camera moves on its own: let go of the stick, read \"stick now\" and set the dead zone a few percent above it.",
    [UI_MENU_STICK_DEADZONE_NOW] = "stick now: %.0f %%",
    [UI_MENU_MOUSE_SENS] = "Mouse camera sensitivity",
    [UI_MENU_MOUSE_SENS_DESC] = "How fast the mouse turns the camera.",
    [UI_MENU_WALK_KEY] = "Walk key",
    [UI_MENU_WALK_KEY_DESC] = "Hold it to tilt the keyboard's sticks part of the way, so the keyboard walks instead of "
                              "runs. The walk scale sets how far. A controller and the mouse are not affected.",
    [UI_MENU_WALK_KEY_TAKEN] = "also %s",
    [UI_MENU_WALK_SCALE] = "Walk scale",
    [UI_MENU_WALK_SCALE_DESC] = "The tilt the keyboard's sticks get while the walk key is held. Half is a walk, the "
                                "full tilt a run.",
    [UI_MENU_STICK_SENS] = "Stick camera speed",
    [UI_MENU_STICK_SENS_DESC] = "Multiplies the game's own right-stick camera speed, top speed included, at each of the "
                                "game's five camera sensitivities. The mouse keeps its own setting.",

    [UI_BINDS_HELP] = "Select a cell and press A or Enter, then the key, mouse button or controller button. Delete or "
                      "X clears a cell. A key in use moves here; a controller button swaps with the one it replaces.",
    [UI_BINDS_COL_KEY1] = "Key 1", [UI_BINDS_COL_KEY2] = "Key 2", [UI_BINDS_COL_PAD] = "Controller",
    [UI_BINDS_GROUP_BUTTONS] = "Buttons", [UI_BINDS_GROUP_MOVE] = "Movement", [UI_BINDS_GROUP_CAMERA] = "Camera",
    [UI_BINDS_PRESS_KEY] = "Press a key or mouse button for %s",
    [UI_BINDS_PRESS_PAD] = "Press a controller button for %s",
    [UI_BINDS_CANCEL_HINT] = "Esc cancels, or wait %d s",
    [UI_BINDS_TAKEN] = "%s was taken from %s", [UI_BINDS_SWAPPED] = "%s swapped with %s",
    [UI_BINDS_RESET] = "Reset all buttons",
    [UI_BINDS_RESET_DESC] = "Back to the default keys and controller buttons. The button layout above applies again: remapped controller buttons stay where you put them until this reset.",
    [UI_BIND_A] = "A", [UI_BIND_B] = "B", [UI_BIND_X] = "X", [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "Left stick click", [UI_BIND_RCLICK] = "Right stick click", [UI_BIND_L] = "L",
    [UI_BIND_R] = "R", [UI_BIND_ZL] = "ZL", [UI_BIND_ZR] = "ZR", [UI_BIND_PLUS] = "Plus", [UI_BIND_MINUS] = "Minus",
    [UI_BIND_LEFT] = "D-pad left", [UI_BIND_UP] = "D-pad up", [UI_BIND_RIGHT] = "D-pad right",
    [UI_BIND_DOWN] = "D-pad down",
    [UI_BIND_LSTICK_LEFT] = "Move left", [UI_BIND_LSTICK_UP] = "Move forward", [UI_BIND_LSTICK_RIGHT] = "Move right",
    [UI_BIND_LSTICK_DOWN] = "Move back",
    [UI_BIND_RSTICK_LEFT] = "Camera left", [UI_BIND_RSTICK_UP] = "Camera up", [UI_BIND_RSTICK_RIGHT] = "Camera right",
    [UI_BIND_RSTICK_DOWN] = "Camera down",
    [UI_BIND_DESC_A] = "Interact, pick up, talk.", [UI_BIND_DESC_B] = "Run, cancel, go back.",
    [UI_BIND_DESC_X] = "Jump.", [UI_BIND_DESC_Y] = "Attack.", [UI_BIND_DESC_LCLICK] = "Crouch.",
    [UI_BIND_DESC_RCLICK] = "First-person view.", [UI_BIND_DESC_L] = "Abilities.",
    [UI_BIND_DESC_R] = "Throw a weapon.", [UI_BIND_DESC_ZL] = "Shield, lock on.", [UI_BIND_DESC_ZR] = "Bow.",
    [UI_BIND_DESC_PLUS] = "Menu, inventory.", [UI_BIND_DESC_MINUS] = "Map.",
    [UI_BIND_DESC_DPAD] = "Quick-select.",
    [UI_BIND_DESC_MOVE] = "Moves Link like the left stick. The controller's left stick always works too.",
    [UI_BIND_DESC_CAMERA] = "Turns the camera like the right stick. The mouse and the controller's right stick always "
                            "work too.",
    [UI_KEY_SPACE] = "Space", [UI_KEY_TAB] = "Tab", [UI_KEY_LSHIFT] = "Left Shift", [UI_KEY_LCTRL] = "Left Ctrl",
    [UI_KEY_UP] = "Up arrow", [UI_KEY_DOWN] = "Down arrow", [UI_KEY_LEFT] = "Left arrow",
    [UI_KEY_RIGHT] = "Right arrow",
    [UI_KEY_MOUSE_L] = "Left mouse", [UI_KEY_MOUSE_R] = "Right mouse", [UI_KEY_MOUSE_M] = "Middle mouse",
    [UI_KEY_MOUSE_X1] = "Mouse 4", [UI_KEY_MOUSE_X2] = "Mouse 5",
    [UI_KEY_ENTER] = "Enter", [UI_KEY_BACKSPACE] = "Backspace", [UI_KEY_RSHIFT] = "Right Shift",
    [UI_KEY_RCTRL] = "Right Ctrl", [UI_KEY_LALT] = "Alt", [UI_KEY_CAPSLOCK] = "Caps Lock", [UI_KEY_INSERT] = "Insert",
    [UI_KEY_DELETE] = "Delete", [UI_KEY_HOME] = "Home", [UI_KEY_END] = "End", [UI_KEY_PAGEUP] = "Page Up",
    [UI_KEY_PAGEDOWN] = "Page Down", [UI_KEY_NUM_FMT] = "Num %s",
    [UI_PAD_SOUTH] = "Bottom button", [UI_PAD_EAST] = "Right button", [UI_PAD_WEST] = "Left button",
    [UI_PAD_NORTH] = "Top button", [UI_PAD_LSTICK] = "Left stick", [UI_PAD_RSTICK] = "Right stick",
    [UI_PAD_L] = "Left bumper", [UI_PAD_R] = "Right bumper", [UI_PAD_START] = "Start", [UI_PAD_BACK] = "Back",
    [UI_PAD_DPAD_LEFT] = "D-pad left", [UI_PAD_DPAD_UP] = "D-pad up", [UI_PAD_DPAD_RIGHT] = "D-pad right",
    [UI_PAD_DPAD_DOWN] = "D-pad down", [UI_PAD_ZL] = "Left trigger", [UI_PAD_ZR] = "Right trigger",
    [UI_PAD_LPADDLE1] = "Left back button", [UI_PAD_RPADDLE1] = "Right back button",
    [UI_PAD_LPADDLE2] = "Left back button 2", [UI_PAD_RPADDLE2] = "Right back button 2",
    [UI_PAD_CROSS] = "Cross", [UI_PAD_CIRCLE] = "Circle", [UI_PAD_SQUARE] = "Square", [UI_PAD_TRIANGLE] = "Triangle",

    [UI_MENU_AMIIBO_DIR] = "Amiibo folder",
    [UI_MENU_AMIIBO_OPEN] = "Open",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "Default folder",
    [UI_MENU_AMIIBO_REFRESH] = "Refresh",
    [UI_MENU_AMIIBO_PICK] = "Amiibo dumps (540-byte .bin or .nfc files):",
    [UI_MENU_AMIIBO_NO_DIR] = "Cannot open the folder %s",
    [UI_MENU_AMIIBO_READER] = "NFC reader",
    [UI_MENU_AMIIBO_STATE] = "Reader:",
    [UI_MENU_AMIIBO_STATE_IDLE] = "ready",
    [UI_MENU_AMIIBO_STATE_SEARCHING] = "the game is waiting for an amiibo",
    [UI_MENU_AMIIBO_STATE_FOUND] = "amiibo on the reader",
    [UI_MENU_AMIIBO_STATE_MOUNTED] = "the game is reading the amiibo",
    [UI_MENU_AMIIBO_STATE_OFF] = "off",
    [UI_MENU_AMIIBO_FILE] = "Selected file:",
    [UI_MENU_AMIIBO_NO_FILE] = "(no file selected)",
    [UI_MENU_AMIIBO_TAP] = "Tap the amiibo",
    [UI_MENU_AMIIBO_REMOVE] = "Lift the amiibo",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "Lift the amiibo by itself after a tap",
    [UI_MENU_AMIIBO_HOLD] = "Time on the reader",
    [UI_MENU_AMIIBO_SECONDS] = "%.1f s",
    [UI_MENU_AMIIBO_TAG] = "Amiibo on the reader:",
    [UI_MENU_AMIIBO_NO_TAG] = "No amiibo on the reader.",
    [UI_MENU_AMIIBO_TAG_FILE] = "File: %s",
    [UI_MENU_AMIIBO_TAG_ID] = "Character %s, series %02x, number %04x, type %02x",
    [UI_MENU_AMIIBO_TAG_UID] = "UID: %s",
    [UI_MENU_AMIIBO_READY] = "Ready",
    [UI_MENU_AMIIBO_TAPPED] = "Tapped: %s",
    [UI_MENU_AMIIBO_LOAD_FAILED] = "Not an amiibo dump (540-byte NTAG215): %s",
    [UI_MENU_AMIIBO_REMOVED] = "Amiibo lifted",
    [UI_MENU_AMIIBO_STATUS] = "Status: %s",
    [UI_MENU_MENU_LANG] = "Menu language", [UI_MENU_MENU_LANG_DESC] = "Language of this menu and the overlay.",
    [UI_MENU_MENU_LANG_AUTO] = "Automatic (from Windows)",

    [UI_MENU_ADV_WARNING] = "These settings are for testing and troubleshooting. Change them only when you know why.",
    [UI_MENU_GROUP_SCHED] = "Scheduling", [UI_MENU_GROUP_SHADERS] = "Shaders and pipelines",
    [UI_MENU_GROUP_TEXMEM] = "Textures and memory", [UI_MENU_GROUP_FIXES] = "Render fixes",
    [UI_MENU_GROUP_VULKAN] = "Vulkan", [UI_MENU_GROUP_DIAG] = "Diagnostics",
    [UI_MENU_ASYNC_PREEMPT] = "Virtual core preemption",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "Lets a game thread interrupt another one on the same virtual core.",
    [UI_MENU_PREEMPT_OFF] = "Off", [UI_MENU_PREEMPT_ON] = "On", [UI_MENU_PREEMPT_INHERIT] = "Inheritance only",
    [UI_MENU_REPLAY_ASYNC] = "GPU frame prep on its own thread",
    [UI_MENU_REPLAY_ASYNC_DESC] = "Prepares frames for the GPU on a separate thread instead of the game thread. Adds up to one frame of latency.",
    [UI_MENU_TIMER_1MS] = "1 ms Windows timer",
    [UI_MENU_TIMER_1MS_DESC] = "Raises the system timer resolution to 1 ms for steadier frame pacing.",
    [UI_MENU_PIPE_WAIT] = "Waiting draw handling",
    [UI_MENU_PIPE_WAIT_DESC] = "Default keeps the current scheduling with a reserved worker for waiting draws; Old (draw) uses the previous scheduling.",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "Default", [UI_MENU_PIPE_WAIT_DRAW] = "Old (draw)",
    [UI_MENU_PIPE_SKIP] = "Draws skipped while a pipeline builds",
    [UI_MENU_PIPE_SKIP_DESC] = "Which draws are dropped instead of waiting for their pipeline.",
    [UI_MENU_PIPE_SKIP_STRICT] = "Strict", [UI_MENU_PIPE_SKIP_ANY] = "Any",
    [UI_MENU_PIPE_SKIP_FULLSCREEN] = "Fullscreen passes",
    [UI_MENU_LIB_PREBUILD] = "Stage library prebuild",
    [UI_MENU_LIB_PREBUILD_DESC] = "Builds shader stage libraries ahead of time in the background.",
    [UI_MENU_LIB_PREBUILD_THREADS] = "Stage library prebuild threads",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "Threads used for the prebuild; Automatic picks a number from your CPU.",
    [UI_MENU_PIPE_PREBUILD] = "Pipeline prebuild",
    [UI_MENU_PIPE_PREBUILD_DESC] = "Builds known pipelines ahead of time.",
    [UI_MENU_PREWARM_XLATE] = "Translator prewarm",
    [UI_MENU_PREWARM_XLATE_DESC] = "Threads that translate known shaders at start; Automatic takes half the CPU threads, at most 16.",
    [UI_MENU_GPL] = "Graphics pipeline library",
    [UI_MENU_GPL_DESC] = "Uses the Vulkan pipeline library extension; turn off if your driver misbehaves with it.",
    [UI_MENU_ASTC] = "ASTC texture decoding", [UI_MENU_ASTC_DESC] = "Where compressed ASTC textures are decoded.",
    [UI_MENU_ASTC_AUTO] = "Automatic", [UI_MENU_ASTC_GPU] = "GPU shader", [UI_MENU_ASTC_NATIVE] = "Native",
    [UI_MENU_ASTC_CPU] = "CPU",
    [UI_MENU_MEMORY] = "GPU memory mode", [UI_MENU_MEMORY_DESC] = "How game memory is shared with the GPU.",
    [UI_MENU_MEMORY_AUTO] = "Automatic", [UI_MENU_MEMORY_IMPORT] = "Import", [UI_MENU_MEMORY_MIRROR] = "Mirror",
    [UI_MENU_MEMORY_VERIFY] = "Verify",
    [UI_MENU_TEX_ALIAS] = "Texture aliasing", [UI_MENU_TEX_ALIAS_DESC] = "A new texture created over a live texture's memory starts with that content instead of empty.",
    [UI_MENU_TEX_VRAM_PRESSURE] = "Texture memory limit", [UI_MENU_TEX_VRAM_PRESSURE_DESC] = "Percent of video memory above which textures the game released are dropped (they come back from storage when used again). 0 keeps them.",
    [UI_MENU_ASTC_BC] = "Compress ASTC to BC3", [UI_MENU_ASTC_BC_DESC] = "With the GPU decoder: re-encodes decoded ASTC textures to BC3, a quarter of the video memory. For GPUs with 4 GB or less; textures look slightly blockier.",
    [UI_MENU_LOD_GUARD] = "LOD guard", [UI_MENU_LOD_GUARD_DESC] = "Prevents objects flashing in the wrong place or a flat far field for a frame, by waiting for the game to finish rewriting level-of-detail data before the GPU reads it.",
    [UI_MENU_FIX_C0] = "Constant buffer fix",
    [UI_MENU_FIX_C0_DESC] = "Fills the driver-reserved constant bank c0 (storage sizes, dispatch size, point size, base instance) instead of reading zeros.",
    [UI_MENU_FIX_ZERO] = "Zero new GPU memory",
    [UI_MENU_FIX_ZERO_DESC] = "New GPU memory pools start zero-filled, as Switch memory does.",
    [UI_MENU_BIND_SKIP] = "Skip repeated binds",
    [UI_MENU_BIND_SKIP_DESC] = "Optimisation: skips bindings that did not change.",
    [UI_MENU_SHADER_MEMO] = "Shader memo", [UI_MENU_SHADER_MEMO_DESC] = "Optimisation: remembers shader lookups.",
    [UI_MENU_PIPE_MEMO] = "Pipeline memo", [UI_MENU_PIPE_MEMO_DESC] = "Optimisation: reuses the previous draw's pipeline when its state repeats.",
    [UI_MENU_TEX_MEMO] = "Texture binding memo", [UI_MENU_TEX_MEMO_DESC] = "Optimisation: remembers texture descriptors between draws.",
    [UI_MENU_DESC_TEMPLATE] = "Descriptor templates",
    [UI_MENU_DESC_TEMPLATE_DESC] = "Optimisation: updates descriptors in bulk.",
    [UI_MENU_PUSH_SKIP] = "Skip repeated push constants",
    [UI_MENU_PUSH_SKIP_DESC] = "Optimisation: skips push constants that did not change.",
    [UI_MENU_SPLIT_DRAWS] = "Early GPU submission",
    [UI_MENU_SPLIT_DRAWS_DESC] = "Submits GPU work in parts every N draws between passes so the GPU starts earlier; 0 turns it off.",
    [UI_MENU_VK_VALIDATION] = "Validation layer",
    [UI_MENU_VK_VALIDATION_DESC] = "Checks every Vulkan call and slows the game down a lot.",
    [UI_MENU_VK_VALIDATION_MISSING] = "The validation layer is not installed.",
    [UI_MENU_VK_DYN_VERTEX] = "Dynamic vertex state",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "Forces dynamic vertex input on or off; the default depends on the GPU.",
    [UI_MENU_VK_DYN_A2C] = "Dynamic alpha to coverage",
    [UI_MENU_VK_DYN_A2C_DESC] = "Forces dynamic alpha to coverage on or off. Default: off on AMD's driver for RDNA 2 and "
                                "older, which draws characters black with it, on elsewhere.",
    [UI_MENU_VK_SUBGROUP_PIN] = "Pin subgroup size", [UI_MENU_VK_SUBGROUP_PIN_DESC] = "Fixes the shader subgroup size.",
    [UI_MENU_TRACE] = "Performance log", [UI_MENU_TRACE_DESC] = "Writes frame timings to a file.",
    [UI_MENU_FLIGHT] = "Flight recorder",
    [UI_MENU_FLIGHT_DESC] = "Keeps a short history and saves it when a frame freezes.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "Freeze threshold (ms)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "Frame time that counts as a freeze.",
    [UI_MENU_INPUT_RECORD] = "Input recording",
    [UI_MENU_INPUT_RECORD_DESC] = "Writes your inputs to a file for bug reports.",
    [UI_MENU_OPEN_LOG] = "Open log folder", [UI_MENU_OPEN_LOG_DESC] = "Shows the folder with the log files.",
    [UI_MENU_COPY_INFO] = "Copy system info",
    [UI_MENU_COPY_INFO_DESC] = "Copies GPU, driver, CPU, RAM and changed settings to the clipboard.",
    [UI_MENU_COPY_INFO_DONE] = "Copied", [UI_MENU_ACTION_FAILED] = "did not work",
    [UI_MENU_RESET] = "Reset all settings to defaults", [UI_MENU_RESET_DESC] = "Applies to every tab.",
    [UI_MENU_RESET_CONFIRM] = "Reset every setting to its default?", [UI_MENU_RESET_YES] = "Reset",
    [UI_MENU_RESET_NO] = "Cancel",

    [UI_MENU_OV_FPS] = "%.0f FPS", [UI_MENU_OV_FRAME_MS] = "%.1f ms", [UI_MENU_OV_GAME_PCT] = "game %.0f%%",
    [UI_MENU_OV_MAX_MS] = "max %.1f ms",
    [UI_MENU_OV_GPU] = "GPU", [UI_MENU_OV_CPU] = "CPU", [UI_MENU_OV_RAM] = "RAM", [UI_MENU_OV_VRAM] = "VRAM",
    [UI_MENU_OV_TEMP] = "Temperature", [UI_MENU_OV_POWER] = "Power", [UI_MENU_OV_CLOCK] = "Clock",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "GPU frame prep", [UI_MENU_OV_GPU_TIME] = "GPU busy per frame",
    [UI_MENU_OV_PIPE_WAIT] = "Pipeline waits",
    [UI_MENU_OV_SHADER_WAIT] = "Shader waits", [UI_MENU_OV_TEX_UPLOADS] = "Texture uploads",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%", [UI_MENU_OV_FMT_TEMP] = "%.0f °C", [UI_MENU_OV_FMT_POWER] = "%.0f W",
    [UI_MENU_OV_FMT_MHZ] = "%.0f MHz", [UI_MENU_OV_FMT_GB] = "%.1f GB", [UI_MENU_OV_FMT_GB_OF] = "/ %.1f GB",
    [UI_MENU_OV_GAME_GB] = "game %.1f GB", [UI_MENU_OV_FMT_MS] = "%.2f ms", [UI_MENU_OV_PER_FRAME] = "%.1f per frame",
    [UI_MENU_OV_WINDOW] = "Window", [UI_MENU_OV_OUTPUT] = "Game image", [UI_MENU_OV_SCENE] = "3D scene",
    [UI_MENU_OV_SCALING] = "Scaling", [UI_MENU_OV_LIMIT] = "Frame limit", [UI_MENU_OV_PRESENT] = "Presentation",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u", [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, sharpness %d%%", [UI_MENU_OV_SCALE_NONE] = "none (1:1)",
    [UI_MENU_OV_SCALE_BILINEAR] = "bilinear",
    [UI_MENU_OV_SCALE_DOWN] = "bilinear (FSR only enlarges)",
    [UI_MENU_TAB_CHEATS] = "Cheats",
    [UI_MENU_CHEATS_NONE] = "No cheats for this game version. A mod's cheats are an Atmosphere cheat file in its "
                            "cheats folder, named after the game's build: mods\\<mod>\\cheats\\%s. Restart the game to "
                            "read them. For cheats that change the game's code, the game rebuilds itself at start (a "
                            "few minutes).",
    [UI_MENU_CHEATS_MOD] = "%s: %d cheats",
    [UI_MENU_CHEAT_CODE_DESC] = "Changes the game's code. Switches at once.",
    [UI_MENU_CHEAT_KEY_DESC] = "Changes the game's code while you hold %s. Switches at once.",
    [UI_MENU_CHEAT_DATA_DESC] = "Writes the game's memory every frame while it is on.",
    [UI_MENU_CHEAT_MASTER] = "always on",
    [UI_MENU_CHEAT_MASTER_DESC] = "The file's master code: it is always on.",
    [UI_MENU_CHEAT_RESTORE] = "not needed",
    [UI_MENU_CHEAT_RESTORE_DESC] = "It only writes the game's own code back. Switching a cheat off already does that.",
    [UI_MENU_CHEAT_REBUILD] = "after the rebuild",
    [UI_MENU_CHEAT_REBUILD_DESC] = "This cheat changes the game's code and is new or edited since the game was built. "
                                   "The game builds it in when it starts next (a few minutes), or use Rebuild the game "
                                   "now.",
    [UI_MENU_CHEAT_UNSUPPORTED] = "not supported",
    [UI_MENU_CHEAT_UNSUPPORTED_DESC] = "This cheat cannot be used with this version of the game:",
    [UI_MENU_CHEATS_REBUILD] = "Rebuild the game now",
    [UI_MENU_CHEATS_REBUILD_DESC] = "Some cheats change the game's code and are not built into it: they are new or "
                                    "edited, or their last rebuild failed or was cancelled. Closes the game; the setup "
                                    "builds them in (a few minutes) and starts the game again. Progress since your "
                                    "last save is lost.",
    [UI_MENU_MODS_EXEFS_UNSUPPORTED] = "This mod patches the game code (exefs) in a way the port cannot apply, so the "
                                       "whole mod stays off. %s",
    [UI_MENU_MODS_EXEFS_OTHER] = "Its code patches (exefs) are made for another game version.",
    [UI_MENU_MODS_EXEFS_ON] = "Code patches in use: %d files.",
    [UI_MENU_MODS_REPLACES_BUILTIN] = "Code patches in use: %d files. They replace the port's own patch %s, which "
                                      "stays off while this mod is on.",
};

static const char *const text_ru[UI_TEXT_COUNT] = {
    SWKBD("Ввод текста", "Введите имя", "Лошадь", "ОК", "Отмена", "Контроллер: A — ОК, B — отмена"),

    [UI_MENU_TITLE] = "Настройки",
    [UI_MENU_TAB_SCREEN] = "Экран", [UI_MENU_TAB_GAME] = "Игра", [UI_MENU_TAB_MODS] = "Моды", [UI_MENU_TAB_SOUND] = "Звук",
    [UI_MENU_TAB_CONTROLS] = "Управление", [UI_MENU_TAB_AMIIBO] = "Amiibo", [UI_MENU_TAB_MENU] = "Меню", [UI_MENU_TAB_ADVANCED] = "Дополнительно",
    [UI_MENU_CONTINUE] = "Продолжить", [UI_MENU_QUIT] = "Выйти из игры",
    [UI_MENU_ON] = "Вкл.", [UI_MENU_OFF] = "Выкл.", [UI_MENU_AUTO] = "Автоматически",
    [UI_MENU_DEFAULT_MARK] = "по умолчанию",
    [UI_MENU_AFTER_RESTART] = "после перезапуска", [UI_MENU_SET_BY_ENV] = "задано переменной окружения",
    [UI_MENU_SAVE_FAILED] = "не удалось сохранить настройки",
    [UI_MENU_VERSION] = "TotK Recomp %s, игра %s",

    [UI_MENU_FULLSCREEN] = "Полный экран", [UI_MENU_FULLSCREEN_DESC] = "Игра на весь экран; F11 переключает режим.",
    [UI_MENU_WINDOW_SIZE] = "Размер окна", [UI_MENU_WINDOW_SIZE_DESC] = "Размер окна игры вне полноэкранного режима.",
    [UI_MENU_WINDOW_SIZE_CUSTOM] = "Другой",
    [UI_MENU_PRESENT] = "Вывод кадров", [UI_MENU_PRESENT_DESC] = "Как готовые кадры попадают на экран.",
    [UI_MENU_PRESENT_MAILBOX] = "Mailbox", [UI_MENU_PRESENT_VSYNC] = "Вертикальная синхронизация",
    [UI_MENU_PRESENT_IMMEDIATE] = "Без ожидания",
    [UI_MENU_PRESENT_IN_USE] = "сейчас: %s",
    [UI_MENU_GPU] = "Видеокарта", [UI_MENU_GPU_DESC] = "Устройство Vulkan, которое рисует игру.",
    [UI_MENU_GPU_AUTO] = "Автоматически",
    [UI_MENU_PERF_OVERLAY] = "Индикатор производительности",
    [UI_MENU_PERF_OVERLAY_DESC] = "Частота кадров в правом верхнем углу. «Полный» добавляет время кадров с графиком, "
                                  "загрузку ЦП, GPU и памяти, температуры и ожидания потока отрисовки. F10 переключает "
                                  "режимы.",
    [UI_MENU_PERF_OFF] = "Выкл.", [UI_MENU_PERF_FPS] = "FPS", [UI_MENU_PERF_FULL] = "Полный",
    [UI_PERF_LINE_FPS] = "%.0f к/с",
    [UI_MENU_GRAPHICS] = "Изображение",
    [UI_MENU_OUTPUT_RES] = "Разрешение игры",
    [UI_MENU_OUTPUT_RES_DESC] = "Размер, в котором игра рисует картинку и интерфейс. Выше 1920 × 1080 игре нужно больше "
                                "видеопамяти и времени GPU. Более широкий размер (21:9) расширяет обзор камеры и "
                                "переносит интерфейс в углы экрана, более высокий (16:10, Steam Deck) переносит его к "
                                "верхнему и нижнему краям.",
    [UI_MENU_RENDER_SCALE] = "Масштаб 3D-отрисовки",
    [UI_MENU_RENDER_SCALE_DESC] = "Размер 3D-сцены относительно разрешения игры; FSR игры доводит её до этого размера. "
                                  "Выше 100 % сглаживает лесенки на краях (суперсэмплинг) ценой большой нагрузки на GPU.",
    [UI_MENU_RENDER_SCALE_GAME] = "Как в игре (83 %)",
    [UI_MENU_UPSCALER] = "Масштабирование под окно",
    [UI_MENU_UPSCALER_DESC] = "Как картинка растягивается, когда окно больше разрешения игры. FSR сохраняет края и "
                              "детали чётче билинейного.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1", [UI_MENU_UPSCALER_BILINEAR] = "Билинейное",
    [UI_MENU_ASPECT] = "Форма картинки",
    [UI_MENU_ASPECT_DESC] = "Когда форма окна отличается от разрешения игры (картинка 16:9 на экране 21:9): сохранить "
                            "форму картинки с чёрными полосами или растянуть её на всё окно.",
    [UI_MENU_ASPECT_FIT] = "Чёрные полосы", [UI_MENU_ASPECT_STRETCH] = "Растянуть",
    [UI_MENU_FOV] = "Поле зрения",
    [UI_MENU_FOV_DESC] = "Сколько мира показывает камера, в процентах от игрового. Только для версии игры 1.4.3.",
    [UI_MENU_SHADOW_RES] = "Размер карты теней",
    [UI_MENU_SHADOW_RES_DESC] = "Размер карты теней от солнца. Собственный размер игры меняется вместе с видом, у земли до "
                                "примерно 400, поэтому тени листвы выглядят квадратными и дёргаются; постоянный размер делает "
                                "их ровными и чётче. Выше 1024 тени требуют больше времени GPU и видеопамяти.",
    [UI_MENU_SHADOW_RES_GAME] = "Как в игре (меняется)",
    [UI_MENU_ANISO] = "Фильтрация текстур",
    [UI_MENU_ANISO_DESC] = "Анизотропная фильтрация текстур поверхностей: земля, дороги и стены под острым углом остаются чёткими вдали, а не размываются. Игра: уровень, который запрашивает игра, как на Switch.",
    [UI_MENU_ANISO_GAME] = "Игра",
    [UI_MENU_SHARPNESS] = "Резкость FSR",
    [UI_MENU_SHARPNESS_DESC] = "Повышение резкости после масштабирования FSR; 0 выключает его.",
    [UI_MENU_COLOR] = "Цвет",
    [UI_MENU_COLOR_PRESET] = "Цветовой профиль",
    [UI_MENU_COLOR_PRESET_DESC] = "Задаёт четыре значения ниже. «Яркий»: больше контраста и насыщенности, тени немного "
                                  "светлее (для OLED-экранов). «Чёткость»: немного больше контраста, насыщенности и "
                                  "яркости. Изменение любого значения делает профиль «Своим». Интерфейс игры меняется "
                                  "вместе с картинкой, это меню — нет.",
    [UI_MENU_COLOR_DEFAULT] = "Как в игре", [UI_MENU_COLOR_VIVID] = "Яркий (OLED)", [UI_MENU_COLOR_CLARITY] = "Чёткость",
    [UI_MENU_COLOR_CUSTOM] = "Свой",
    [UI_MENU_COLOR_BRIGHTNESS] = "Яркость", [UI_MENU_COLOR_BRIGHTNESS_DESC] = "Прибавляется к каждому цвету; 0 — без изменений.",
    [UI_MENU_COLOR_CONTRAST] = "Контраст",
    [UI_MENU_COLOR_CONTRAST_DESC] = "Отдаляет цвета от среднего серого (выше 100 %) или приближает к нему; 100 % — без "
                                    "изменений.",
    [UI_MENU_COLOR_SATURATION] = "Насыщенность",
    [UI_MENU_COLOR_SATURATION_DESC] = "Сила цвета относительно серого; 100 % — без изменений.",
    [UI_MENU_COLOR_GAMMA] = "Гамма",
    [UI_MENU_COLOR_GAMMA_DESC] = "Ниже 1 осветляет тёмные и средние тона, выше 1 затемняет их; 1 — без изменений.",
    [UI_MENU_HDR_GROUP] = "HDR",
    [UI_MENU_HDR] = "Вывод HDR",
    [UI_MENU_HDR_DESC] = "Яркие блики на HDR-дисплее. Нужен включённый HDR в Windows («Использовать HDR») или "
                         "Wayland-композитор с управлением цветом; без него картинка остаётся SDR. Применяется после "
                         "перезапуска. Цветовой профиль выше — для SDR и в HDR не используется.",
    [UI_MENU_HDR_PEAK] = "Пиковая яркость",
    [UI_MENU_HDR_PEAK_DESC] = "Пик вашего дисплея в нитах; самые яркие блики останавливаются на нём.",
    [UI_MENU_HDR_GAME] = "Яркость игры",
    [UI_MENU_HDR_GAME_DESC] = "Ниты белого цвета картинки (paper white). 203 — эталон HDR; в светлой комнате "
                              "поднимите.",
    [UI_MENU_HDR_UI] = "Яркость интерфейса и меню",
    [UI_MENU_HDR_UI_DESC] = "Ниты белого цвета интерфейса игры и этого меню.",
    [UI_MENU_HDR_HUE] = "Коррекция оттенка бликов",
    [UI_MENU_HDR_HUE_DESC] = "Яркие цвета (огонь, искры) уходят к белому, как в SDR-картинке игры, и совпадают со светом, "
                             "который они дают; 0 оставляет их полностью насыщенными.",
    [UI_MENU_SHADOW_SCALE] = "Разрешение теней",
    [UI_MENU_SHADOW_SCALE_DESC] = "Карты теней от солнца рисуются во столько раз больше по каждой стороне, чем 960x960 "
                                  "самой игры: края теней и тени листвы чётче. Действует после перезапуска.",
    [UI_MENU_PCSS] = "Мягкие тени",
    [UI_MENU_PCSS_DESC] = "Тени от солнца резкие там, где предмет касается земли, и мягче, чем дальше падает тень "
                          "(PCSS). 0 оставляет фильтр игры. Действует после перезапуска.",
    [UI_MENU_CONTACT_SHADOWS] = "Контактные тени",
    [UI_MENU_CONTACT_SHADOWS_DESC] = "Мелкие тени от солнца, которых нет на картах теней: у ног, под камнями, в траве и "
                                     "складках. Считаются по экрану после освещения. Действует после перезапуска.",
    [UI_MENU_SHADOW_RANGE] = "Дальность каскадов теней",
    [UI_MENU_SHADOW_RANGE_DESC] = "Отодвигает границы, за которыми тени становятся мягче (в игре 20, 40, 80 и 160 м). "
                                  "Карта теней растягивается на большую площадь, поэтому вместе с этим поднимайте "
                                  "разрешение теней. Действует после перезапуска.",
    [UI_MENU_DLAA] = "NVIDIA DLSS / DLAA",
    [UI_MENU_DLAA_DESC] = "NVIDIA DLSS для 3D-сцены (видеокарты NVIDIA RTX; nvngx_dlss.dll рядом с totk.exe): ровнее края и "
                          "меньше мерцания. Интерфейс не затрагивается. Режим ниже задаёт разрешение, с которым он работает. "
                          "Действует после перезапуска.",
    [UI_MENU_DLSS] = "Режим DLSS",
    [UI_MENU_DLSS_DESC] = "DLAA рисует 3D-сцену в полном разрешении. «Качество», «Баланс» и «Производительность» рисуют её "
                          "в 67, 58 и 50 % и масштабируют через DLSS ради большего числа кадров; они заменяют масштаб "
                          "3D-отрисовки. Действует после перезапуска.",
    [UI_MENU_DLSS_DLAA] = "DLAA (родное)", [UI_MENU_DLSS_QUALITY] = "Качество", [UI_MENU_DLSS_BALANCED] = "Баланс",
    [UI_MENU_DLSS_PERFORMANCE] = "Производительность",
    [UI_MENU_FXAA] = "Сглаживание игры (FXAA)",
    [UI_MENU_FXAA_DESC] = "Собственное сглаживание FXAA. Слегка размывает мелкие детали; с DLAA оно не нужно.",

    [UI_MENU_FPS_LIMIT] = "Ограничение частоты кадров",
    [UI_MENU_FPS_LIMIT_DESC] = "Наибольшее число кадров в секунду. Выше 60 — экспериментальный режим, нужна «Скорость "
                               "игры по часам».",
    [UI_MENU_FPS_30] = "30 FPS", [UI_MENU_FPS_40] = "40 FPS", [UI_MENU_FPS_45] = "45 FPS",
    [UI_MENU_FPS_60] = "60 FPS", [UI_MENU_FPS_90] = "90 FPS", [UI_MENU_FPS_120] = "120 FPS", [UI_MENU_FPS_144] = "144 FPS",
    [UI_MENU_FPS_DISPLAY] = "Как у монитора", [UI_MENU_FPS_DISPLAY_HZ] = "Как у монитора (%.0f Гц)",
    [UI_MENU_REALTIME] = "Скорость игры по часам",
    [UI_MENU_REALTIME_DESC] = "Игра идёт по реальному времени, медленный кадр её не замедляет.",
    [UI_MENU_CLOTH_REALTIME] = "Ткань по часам",
    [UI_MENU_CLOTH_REALTIME_DESC] = "Ткань и волосы тоже идут по реальному времени.",
    [UI_MENU_HIGHFPS_FIX] = "Исправления для высокого FPS",
    [UI_MENU_HIGHFPS_FIX_DESC] = "Исправляет физику, которая ломается при частоте выше 30 кадров в секунду, и объекты, которые выше 60 остаются полупрозрачными или невидимыми.",
    [UI_MENU_GAME_LANG] = "Язык игры", [UI_MENU_GAME_LANG_DESC] = "Язык текста и озвучки игры.",
    [UI_MENU_GAME_LANG_AUTO] = "Автоматически",
    [UI_MENU_OPERATION_MODE] = "Режим консоли",
    [UI_MENU_OPERATION_MODE_DESC] = "Игра считает, что консоль в док-станции или в портативном режиме.",
    [UI_MENU_MODE_DOCKED] = "В док-станции", [UI_MENU_MODE_HANDHELD] = "Портативный",
    [UI_MENU_ASSUME_FOCUS] = "Работать без фокуса",
    [UI_MENU_ASSUME_FOCUS_DESC] = "Включено (по умолчанию): игра идёт, пока вы в другом окне, а клавиатура не "
                                  "действует, пока окно не вернётся на передний план. Выключено: игра встаёт на паузу, "
                                  "когда окно теряет фокус, как на Switch при нажатии Home; прерванная так загрузка "
                                  "может остановиться насовсем.",

    [UI_MENU_MODS_BUILTIN] = "Встроенные в порт", [UI_MENU_MODS_INSTALLED] = "Установленные моды",
    [UI_MENU_MODS_NONE] = "Модов пока нет. Распакуйте мод в папку mods, каждый мод в свою папку с папкой romfs внутри, и "
                          "перезапустите игру. TKMM тоже может выгрузить туда объединённые моды отдельной папкой.",
    [UI_MENU_MODS_OPEN] = "Открыть папку модов",
    [UI_MENU_MODS_OPEN_DESC] = "Показывает папку, из которой игра загружает моды. Если её нет, она будет создана.",
    [UI_MENU_MODS_ROW_DESC] = "Заменяет файлов игры: %u. Добавляет новых файлов: %u.",
    [UI_MENU_MODS_OVERRIDDEN] = "%u его файлов берутся из мода %s, он выше в списке.",
    [UI_MENU_MODS_MOVE_HINT] = "Верхняя кнопка или Ctrl+Вверх поднимает мод, левая кнопка или Ctrl+Вниз опускает. Если "
                               "два мода меняют один файл, побеждает верхний. Изменения действуют после перезапуска.",
    [UI_MENU_MODS_EMPTY] = "В этой папке нет файлов romfs, exefs или cheats.",
    [UI_MENU_MODS_SUBSDK] = "Этот мод приносит свой код игры (плагин subsdk или заменённый main). Порт не загружает "
                            "модули кода, поэтому мод выключен целиком.",
    [UI_MENU_MODS_EXEFS_LATER] = "Этот мод меняет код игры (exefs). Игра соберёт эти патчи при следующем запуске "
                                 "(пересборка на несколько минут); до тех пор мод выключен целиком.",
    [UI_MENU_MODS_CHEATS] = "Читов: %d. Они включаются на вкладке «Читы».",
    [UI_MENU_MODS_CHEATS_FILE] = "Читы для этой версии игры читаются при запуске из cheats\\%s в этом моде.",
    [UI_MENU_MODS_BAD_NAME] = "В имени папки есть символ |, которым список модов разделяет имена. Переименуйте папку.",
    [UI_MENU_MODS_OTHER_VERSION] = "Файлов для другой версии игры: %u, они пропущены. Поищите версию этого мода для "
                                   "вашей версии игры.",
    [UI_MENU_MODS_UNUSED] = "не используется",
    [UI_MENU_MODS_FPS60] = "Патч 60 FPS",
    [UI_MENU_MODS_FPS60_DESC] = "Собственный патч порта для 60 кадров в секунду. Настраивается «Ограничением частоты "
                                "кадров» на вкладке «Игра».",
    [UI_MENU_MODS_RES] = "Патч разрешения",
    [UI_MENU_MODS_RES_DESC] = "Собственный патч порта для разрешений выше 1080p. Настраивается «Разрешением игры» на "
                              "вкладке «Экран».",
    [UI_MENU_MODS_QUICKMENU] = "Исправление быстрого меню",
    [UI_MENU_MODS_QUICKMENU_DESC] = "Исправление порта: чёрный фон меню оружия, щитов и материалов при 60 FPS.",

    [UI_MENU_VOLUME] = "Громкость", [UI_MENU_VOLUME_DESC] = "Общая громкость игры.",

    [UI_MENU_MOTION] = "Управление движением", [UI_MENU_MOTION_DESC] = "Прицеливание гироскопом контроллера.",
    [UI_MENU_RUMBLE_STRENGTH] = "Сила вибрации",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "Насколько сильно вибрирует контроллер.",
    [UI_MENU_RUMBLE_CURVE] = "Кривая вибрации",
    [UI_MENU_RUMBLE_CURVE_DESC] = "Чем ниже значение, тем сильнее слабая вибрация.",
    [UI_MENU_RUMBLE_TRIGGERS] = "Вибрация триггеров (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Вибрация и в триггерах контроллера Xbox.",
    [UI_MENU_PAD_LAYOUT] = "Раскладка кнопок",
    [UI_MENU_PAD_LAYOUT_DESC] = "Какая кнопка работает как A. Nintendo: правая, как на Switch. Xbox: нижняя, как "
                                "подписано на контроллере Xbox (крест на контроллере PlayStation). Автоматически: "
                                "Nintendo для контроллеров с подписями Nintendo, Xbox для остальных. Кнопки какого "
                                "контроллера показывают подсказки игры, задаётся в строке «Подсказки кнопок».",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Автоматически", [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A справа)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A снизу)",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "сейчас: %s",
    [UI_MENU_PROMPTS] = "Подсказки кнопок",
    [UI_MENU_PROMPTS_DESC] = "Кнопки какого контроллера показывают подсказки игры. Nintendo: как в самой игре. Xbox: "
                             "LB, RB, LT, RT, Menu, View и крестовина, а A, B, X и Y подписаны так же, как кнопка, "
                             "которую нажимает раскладка выше (в Nintendo (A справа) правая кнопка подписана B, и "
                             "подсказки A показывают B). Игра читает подсказки при запуске, так что они меняются после "
                             "перезапуска. Подсказки записаны в файлах игры, поэтому у кнопки, переназначенной на "
                             "вкладке управления, на экране остаётся прежняя подсказка.",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo", [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_MISSING] = "нет файлов подсказок: запустите установку снова",
    [UI_MENU_PROMPTS_FAILED] = "файлы подсказок повреждены: запустите установку снова",
    [UI_MENU_PROMPTS_RESTART] = "подсказки сменятся после перезапуска",
    [UI_MENU_PROMPTS_OFF_SHAPE] = "выкл. при разрешении игры не 16:9",
    [UI_MENU_PROMPTS_OFF_MODS] = "выкл.: мод RomFS заменяет их файлы",
    [UI_MENU_PROMPTS_OFF_PATCHES] = "выкл., пока задан TOTK_ROMFS_PATCHES",
    [UI_MENU_RESTART] = "Перезапустить сейчас",
    [UI_MENU_RESTART_DESC] = "Закрывает игру и запускает её снова с новыми подсказками. Прогресс после последнего "
                             "сохранения будет потерян.",
    [UI_MENU_RESTART_CONFIRM] = "Перезапустить игру сейчас? Прогресс после последнего сохранения будет потерян.",
    [UI_MENU_RESTART_YES] = "Перезапустить",
    [UI_MENU_RESTART_SAVING] = "Игра сохраняется: перезапуск будет доступен через мгновение.",
    [UI_MENU_LSTICK_DEADZONE] = "Мёртвая зона левого стика", [UI_MENU_RSTICK_DEADZONE] = "Мёртвая зона правого стика",
    [UI_MENU_STICK_DEADZONE_DESC] = "Насколько нужно отклонить стик геймпада, чтобы игра это заметила. Увеличьте, если персонаж или камера двигаются сами: отпустите стик, посмотрите «стик сейчас» и поставьте мёртвую зону на несколько процентов выше.",
    [UI_MENU_STICK_DEADZONE_NOW] = "стик сейчас: %.0f %%",
    [UI_MENU_MOUSE_SENS] = "Чувствительность мыши",
    [UI_MENU_MOUSE_SENS_DESC] = "Как быстро мышь поворачивает камеру.",
    [UI_MENU_WALK_KEY] = "Клавиша шага",
    [UI_MENU_WALK_KEY_DESC] = "Удерживайте: стики клавиатуры отклоняются частично, и персонаж идёт, а не бежит. "
                              "Насколько — задаёт сила шага. Контроллер и мышь не затрагиваются.",
    [UI_MENU_WALK_KEY_TAKEN] = "также %s",
    [UI_MENU_WALK_SCALE] = "Сила шага",
    [UI_MENU_WALK_SCALE_DESC] = "Отклонение стиков клавиатуры при удержании клавиши шага. Половина — шаг, полное "
                                "отклонение — бег.",
    [UI_MENU_STICK_SENS] = "Скорость камеры (стик)",
    [UI_MENU_STICK_SENS_DESC] = "Умножает скорость поворота камеры правым стиком, включая наибольшую, при каждой из пяти "
                                "чувствительностей камеры в игре. На мышь не влияет.",

    [UI_BINDS_HELP] = "Выберите ячейку и нажмите A или Enter, затем клавишу, кнопку мыши или кнопку контроллера. "
                      "Delete или X очищает ячейку. Занятая клавиша переходит сюда; кнопка контроллера меняется "
                      "местами с той, которую заменяет.",
    [UI_BINDS_COL_KEY1] = "Клавиша 1", [UI_BINDS_COL_KEY2] = "Клавиша 2", [UI_BINDS_COL_PAD] = "Контроллер",
    [UI_BINDS_GROUP_BUTTONS] = "Кнопки", [UI_BINDS_GROUP_MOVE] = "Движение", [UI_BINDS_GROUP_CAMERA] = "Камера",
    [UI_BINDS_PRESS_KEY] = "Нажмите клавишу или кнопку мыши для «%s»",
    [UI_BINDS_PRESS_PAD] = "Нажмите кнопку контроллера для «%s»",
    [UI_BINDS_CANCEL_HINT] = "Esc отменяет, или подождите %d с",
    [UI_BINDS_TAKEN] = "%s: убрано с «%s»", [UI_BINDS_SWAPPED] = "«%s» и «%s» поменялись местами",
    [UI_BINDS_RESET] = "Сбросить все кнопки",
    [UI_BINDS_RESET_DESC] = "Вернуть клавиши и кнопки контроллера по умолчанию. Снова действует раскладка кнопок выше: "
                            "переназначенные кнопки контроллера остаются на своих местах до "
                            "этого сброса.",
    [UI_BIND_A] = "A", [UI_BIND_B] = "B", [UI_BIND_X] = "X", [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "Нажатие левого стика", [UI_BIND_RCLICK] = "Нажатие правого стика", [UI_BIND_L] = "L",
    [UI_BIND_R] = "R", [UI_BIND_ZL] = "ZL", [UI_BIND_ZR] = "ZR", [UI_BIND_PLUS] = "Плюс", [UI_BIND_MINUS] = "Минус",
    [UI_BIND_LEFT] = "Крестовина влево", [UI_BIND_UP] = "Крестовина вверх", [UI_BIND_RIGHT] = "Крестовина вправо",
    [UI_BIND_DOWN] = "Крестовина вниз",
    [UI_BIND_LSTICK_LEFT] = "Влево", [UI_BIND_LSTICK_UP] = "Вперёд", [UI_BIND_LSTICK_RIGHT] = "Вправо",
    [UI_BIND_LSTICK_DOWN] = "Назад",
    [UI_BIND_RSTICK_LEFT] = "Камера влево", [UI_BIND_RSTICK_UP] = "Камера вверх",
    [UI_BIND_RSTICK_RIGHT] = "Камера вправо", [UI_BIND_RSTICK_DOWN] = "Камера вниз",
    [UI_BIND_DESC_A] = "Взаимодействие, подобрать, поговорить.", [UI_BIND_DESC_B] = "Бег, отмена, назад.",
    [UI_BIND_DESC_X] = "Прыжок.", [UI_BIND_DESC_Y] = "Атака.", [UI_BIND_DESC_LCLICK] = "Присесть.",
    [UI_BIND_DESC_RCLICK] = "Вид от первого лица.", [UI_BIND_DESC_L] = "Способности.",
    [UI_BIND_DESC_R] = "Бросок оружия.", [UI_BIND_DESC_ZL] = "Щит, захват цели.", [UI_BIND_DESC_ZR] = "Лук.",
    [UI_BIND_DESC_PLUS] = "Меню, инвентарь.", [UI_BIND_DESC_MINUS] = "Карта.",
    [UI_BIND_DESC_DPAD] = "Быстрый выбор.",
    [UI_BIND_DESC_MOVE] = "Двигает Линка, как левый стик. Левый стик контроллера тоже всегда работает.",
    [UI_BIND_DESC_CAMERA] = "Поворачивает камеру, как правый стик. Мышь и правый стик контроллера тоже всегда "
                            "работают.",
    [UI_KEY_SPACE] = "Пробел", [UI_KEY_TAB] = "Tab", [UI_KEY_LSHIFT] = "Левый Shift", [UI_KEY_LCTRL] = "Левый Ctrl",
    [UI_KEY_UP] = "Стрелка вверх", [UI_KEY_DOWN] = "Стрелка вниз", [UI_KEY_LEFT] = "Стрелка влево",
    [UI_KEY_RIGHT] = "Стрелка вправо",
    [UI_KEY_MOUSE_L] = "ЛКМ", [UI_KEY_MOUSE_R] = "ПКМ", [UI_KEY_MOUSE_M] = "СКМ", [UI_KEY_MOUSE_X1] = "Мышь 4",
    [UI_KEY_MOUSE_X2] = "Мышь 5",
    [UI_KEY_ENTER] = "Enter", [UI_KEY_BACKSPACE] = "Backspace", [UI_KEY_RSHIFT] = "Правый Shift",
    [UI_KEY_RCTRL] = "Правый Ctrl", [UI_KEY_LALT] = "Alt", [UI_KEY_CAPSLOCK] = "Caps Lock", [UI_KEY_INSERT] = "Insert",
    [UI_KEY_DELETE] = "Delete", [UI_KEY_HOME] = "Home", [UI_KEY_END] = "End", [UI_KEY_PAGEUP] = "Page Up",
    [UI_KEY_PAGEDOWN] = "Page Down", [UI_KEY_NUM_FMT] = "Num %s",
    [UI_PAD_SOUTH] = "Нижняя кнопка", [UI_PAD_EAST] = "Правая кнопка", [UI_PAD_WEST] = "Левая кнопка",
    [UI_PAD_NORTH] = "Верхняя кнопка", [UI_PAD_LSTICK] = "Левый стик", [UI_PAD_RSTICK] = "Правый стик",
    [UI_PAD_L] = "Левый бампер", [UI_PAD_R] = "Правый бампер", [UI_PAD_START] = "Start", [UI_PAD_BACK] = "Back",
    [UI_PAD_DPAD_LEFT] = "Крестовина влево", [UI_PAD_DPAD_UP] = "Крестовина вверх",
    [UI_PAD_DPAD_RIGHT] = "Крестовина вправо", [UI_PAD_DPAD_DOWN] = "Крестовина вниз", [UI_PAD_ZL] = "Левый курок",
    [UI_PAD_ZR] = "Правый курок",
    [UI_PAD_LPADDLE1] = "Левая задняя кнопка", [UI_PAD_RPADDLE1] = "Правая задняя кнопка",
    [UI_PAD_LPADDLE2] = "Левая задняя кнопка 2", [UI_PAD_RPADDLE2] = "Правая задняя кнопка 2",
    [UI_PAD_CROSS] = "Крест", [UI_PAD_CIRCLE] = "Круг", [UI_PAD_SQUARE] = "Квадрат", [UI_PAD_TRIANGLE] = "Треугольник",

    [UI_MENU_AMIIBO_DIR] = "Папка amiibo",
    [UI_MENU_AMIIBO_OPEN] = "Открыть",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "Папка по умолчанию",
    [UI_MENU_AMIIBO_REFRESH] = "Обновить",
    [UI_MENU_AMIIBO_PICK] = "Дампы amiibo (файлы .bin или .nfc по 540 байт):",
    [UI_MENU_AMIIBO_NO_DIR] = "Не удаётся открыть папку %s",
    [UI_MENU_AMIIBO_READER] = "NFC-считыватель",
    [UI_MENU_AMIIBO_STATE] = "Считыватель:",
    [UI_MENU_AMIIBO_STATE_IDLE] = "готов",
    [UI_MENU_AMIIBO_STATE_SEARCHING] = "игра ждёт amiibo",
    [UI_MENU_AMIIBO_STATE_FOUND] = "amiibo на считывателе",
    [UI_MENU_AMIIBO_STATE_MOUNTED] = "игра читает amiibo",
    [UI_MENU_AMIIBO_STATE_OFF] = "выключен",
    [UI_MENU_AMIIBO_FILE] = "Выбранный файл:",
    [UI_MENU_AMIIBO_NO_FILE] = "(файл не выбран)",
    [UI_MENU_AMIIBO_TAP] = "Приложить amiibo",
    [UI_MENU_AMIIBO_REMOVE] = "Убрать amiibo",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "Убирать amiibo само после касания",
    [UI_MENU_AMIIBO_HOLD] = "Время на считывателе",
    [UI_MENU_AMIIBO_SECONDS] = "%.1f с",
    [UI_MENU_AMIIBO_TAG] = "Amiibo на считывателе:",
    [UI_MENU_AMIIBO_NO_TAG] = "На считывателе нет amiibo.",
    [UI_MENU_AMIIBO_TAG_FILE] = "Файл: %s",
    [UI_MENU_AMIIBO_TAG_ID] = "Персонаж %s, серия %02x, номер %04x, тип %02x",
    [UI_MENU_AMIIBO_TAG_UID] = "UID: %s",
    [UI_MENU_AMIIBO_READY] = "Готово",
    [UI_MENU_AMIIBO_TAPPED] = "Приложен: %s",
    [UI_MENU_AMIIBO_LOAD_FAILED] = "Не дамп amiibo (NTAG215, 540 байт): %s",
    [UI_MENU_AMIIBO_REMOVED] = "Amiibo убран",
    [UI_MENU_AMIIBO_STATUS] = "Состояние: %s",
    [UI_MENU_MENU_LANG] = "Язык меню", [UI_MENU_MENU_LANG_DESC] = "Язык этого меню и индикатора.",
    [UI_MENU_MENU_LANG_AUTO] = "Автоматически (из Windows)",

    [UI_MENU_ADV_WARNING] = "Эти настройки нужны для тестов и поиска неполадок. Меняйте их, только если знаете зачем.",
    [UI_MENU_GROUP_SCHED] = "Планирование", [UI_MENU_GROUP_SHADERS] = "Шейдеры и конвейеры",
    [UI_MENU_GROUP_TEXMEM] = "Текстуры и память", [UI_MENU_GROUP_FIXES] = "Исправления графики",
    [UI_MENU_GROUP_VULKAN] = "Vulkan", [UI_MENU_GROUP_DIAG] = "Диагностика",
    [UI_MENU_ASYNC_PREEMPT] = "Вытеснение на виртуальных ядрах",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "Позволяет потоку игры прервать другой поток на том же виртуальном ядре.",
    [UI_MENU_PREEMPT_OFF] = "Выкл.", [UI_MENU_PREEMPT_ON] = "Вкл.", [UI_MENU_PREEMPT_INHERIT] = "Только наследование",
    [UI_MENU_REPLAY_ASYNC] = "Подготовка кадра для GPU в отдельном потоке",
    [UI_MENU_REPLAY_ASYNC_DESC] = "Кадр для GPU готовится в отдельном потоке, а не в потоке игры. Добавляет до одного кадра задержки.",
    [UI_MENU_TIMER_1MS] = "Таймер Windows 1 мс",
    [UI_MENU_TIMER_1MS_DESC] = "Повышает точность системного таймера до 1 мс для ровного темпа кадров.",
    [UI_MENU_PIPE_WAIT] = "Обработка ожидающих отрисовок",
    [UI_MENU_PIPE_WAIT_DESC] = "«По умолчанию» — текущее планирование с отдельным потоком для ожидающих отрисовок; «Старый способ» — прежнее планирование.",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "По умолчанию", [UI_MENU_PIPE_WAIT_DRAW] = "Старый способ",
    [UI_MENU_PIPE_SKIP] = "Пропуск отрисовок при сборке конвейера",
    [UI_MENU_PIPE_SKIP_DESC] = "Какие отрисовки отбрасываются, а не ждут свой конвейер.",
    [UI_MENU_PIPE_SKIP_STRICT] = "Строго", [UI_MENU_PIPE_SKIP_ANY] = "Любые",
    [UI_MENU_PIPE_SKIP_FULLSCREEN] = "Полноэкранные проходы",
    [UI_MENU_LIB_PREBUILD] = "Предсборка библиотек стадий",
    [UI_MENU_LIB_PREBUILD_DESC] = "Заранее собирает библиотеки стадий шейдеров в фоне.",
    [UI_MENU_LIB_PREBUILD_THREADS] = "Потоки предсборки библиотек",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "Сколько потоков занято предсборкой; «Автоматически» — по числу ядер процессора.",
    [UI_MENU_PIPE_PREBUILD] = "Предсборка конвейеров",
    [UI_MENU_PIPE_PREBUILD_DESC] = "Заранее собирает известные конвейеры.",
    [UI_MENU_PREWARM_XLATE] = "Прогрев транслятора",
    [UI_MENU_PREWARM_XLATE_DESC] = "Потоки, которые при запуске переводят известные шейдеры; «Автоматически» — половина потоков процессора, не больше 16.",
    [UI_MENU_GPL] = "Библиотека графических конвейеров",
    [UI_MENU_GPL_DESC] = "Использует расширение Vulkan для библиотек конвейеров; выключите, если драйвер с ним сбоит.",
    [UI_MENU_ASTC] = "Распаковка текстур ASTC", [UI_MENU_ASTC_DESC] = "Где распаковываются сжатые текстуры ASTC.",
    [UI_MENU_ASTC_AUTO] = "Автоматически", [UI_MENU_ASTC_GPU] = "Шейдер GPU", [UI_MENU_ASTC_NATIVE] = "Аппаратно",
    [UI_MENU_ASTC_CPU] = "Процессор",
    [UI_MENU_MEMORY] = "Режим памяти GPU", [UI_MENU_MEMORY_DESC] = "Как память игры передаётся видеокарте.",
    [UI_MENU_MEMORY_AUTO] = "Автоматически", [UI_MENU_MEMORY_IMPORT] = "Импорт", [UI_MENU_MEMORY_MIRROR] = "Зеркало",
    [UI_MENU_MEMORY_VERIFY] = "Проверка",
    [UI_MENU_TEX_ALIAS] = "Наследование содержимого текстур", [UI_MENU_TEX_ALIAS_DESC] = "Новая текстура поверх памяти живой текстуры получает её содержимое, а не пустую.",
    [UI_MENU_TEX_VRAM_PRESSURE] = "Предел памяти текстур", [UI_MENU_TEX_VRAM_PRESSURE_DESC] = "Процент видеопамяти, выше которого освобождённые игрой текстуры удаляются (при повторном использовании загружаются снова). 0 — не удалять.",
    [UI_MENU_ASTC_BC] = "Сжатие ASTC в BC3", [UI_MENU_ASTC_BC_DESC] = "С декодером на GPU: декодированные текстуры ASTC пересжимаются в BC3 и занимают вчетверо меньше видеопамяти. Для видеокарт с 4 ГБ и меньше; текстуры чуть более блочные.",
    [UI_MENU_LOD_GUARD] = "Защита LOD", [UI_MENU_LOD_GUARD_DESC] = "Не даёт объектам мелькать не на своём месте, а дальнему плану — на кадр становиться плоским: ждёт, пока игра допишет данные уровней детализации, и только потом их читает GPU.",
    [UI_MENU_FIX_C0] = "Исправление константного буфера",
    [UI_MENU_FIX_C0_DESC] = "Заполняет зарезервированный драйвером банк констант c0 (размеры хранилищ, размер запуска, размер точки, базовый экземпляр) вместо чтения нулей.",
    [UI_MENU_FIX_ZERO] = "Обнулять новую память GPU",
    [UI_MENU_FIX_ZERO_DESC] = "Новые пулы памяти GPU заполняются нулями, как память Switch.",
    [UI_MENU_BIND_SKIP] = "Пропуск повторных привязок",
    [UI_MENU_BIND_SKIP_DESC] = "Оптимизация: пропускает неизменившиеся привязки.",
    [UI_MENU_SHADER_MEMO] = "Память шейдеров", [UI_MENU_SHADER_MEMO_DESC] = "Оптимизация: запоминает поиск шейдеров.",
    [UI_MENU_PIPE_MEMO] = "Память конвейера", [UI_MENU_PIPE_MEMO_DESC] = "Оптимизация: повторяет конвейер прошлой отрисовки, если её состояние не изменилось.",
    [UI_MENU_TEX_MEMO] = "Память привязок текстур", [UI_MENU_TEX_MEMO_DESC] = "Оптимизация: запоминает дескрипторы текстур между отрисовками.",
    [UI_MENU_DESC_TEMPLATE] = "Шаблоны дескрипторов",
    [UI_MENU_DESC_TEMPLATE_DESC] = "Оптимизация: обновляет дескрипторы пачками.",
    [UI_MENU_PUSH_SKIP] = "Пропуск повторных push-констант",
    [UI_MENU_PUSH_SKIP_DESC] = "Оптимизация: пропускает неизменившиеся push-константы.",
    [UI_MENU_SPLIT_DRAWS] = "Ранняя отправка работы GPU",
    [UI_MENU_SPLIT_DRAWS_DESC] = "Отправляет работу GPU частями через каждые N отрисовок между проходами, чтобы GPU начинал раньше; 0 — выключено.",
    [UI_MENU_VK_VALIDATION] = "Слой проверки",
    [UI_MENU_VK_VALIDATION_DESC] = "Проверяет каждый вызов Vulkan и сильно замедляет игру.",
    [UI_MENU_VK_VALIDATION_MISSING] = "Слой проверки не установлен.",
    [UI_MENU_VK_DYN_VERTEX] = "Динамическое состояние вершин",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "Принудительно включает или выключает динамический ввод вершин; по умолчанию зависит от GPU.",
    [UI_MENU_VK_DYN_A2C] = "Динамический alpha to coverage",
    [UI_MENU_VK_DYN_A2C_DESC] = "Принудительно включает или выключает динамический alpha to coverage. По умолчанию "
                                "выключен на драйвере AMD для RDNA 2 и старше, где с ним персонажи чёрные, и включён "
                                "на остальных.",
    [UI_MENU_VK_SUBGROUP_PIN] = "Фиксировать размер подгруппы",
    [UI_MENU_VK_SUBGROUP_PIN_DESC] = "Фиксирует размер подгруппы шейдера.",
    [UI_MENU_TRACE] = "Журнал производительности", [UI_MENU_TRACE_DESC] = "Пишет время кадров в файл.",
    [UI_MENU_FLIGHT] = "Запись последних кадров (для отчёта о фризе)",
    [UI_MENU_FLIGHT_DESC] = "Хранит короткую историю и сохраняет её, когда кадр подвисает.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "Порог подвисания (мс)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "Время кадра, которое считается подвисанием.",
    [UI_MENU_INPUT_RECORD] = "Запись ввода",
    [UI_MENU_INPUT_RECORD_DESC] = "Пишет ваш ввод в файл для отчёта об ошибке.",
    [UI_MENU_OPEN_LOG] = "Открыть папку журналов", [UI_MENU_OPEN_LOG_DESC] = "Показывает папку с файлами журнала.",
    [UI_MENU_COPY_INFO] = "Копировать сведения о системе",
    [UI_MENU_COPY_INFO_DESC] = "Копирует в буфер обмена видеокарту, драйвер, процессор, память и изменённые настройки.",
    [UI_MENU_COPY_INFO_DONE] = "Скопировано", [UI_MENU_ACTION_FAILED] = "не получилось",
    [UI_MENU_RESET] = "Сбросить все настройки", [UI_MENU_RESET_DESC] = "Действует на все вкладки.",
    [UI_MENU_RESET_CONFIRM] = "Сбросить все настройки к значениям по умолчанию?", [UI_MENU_RESET_YES] = "Сбросить",
    [UI_MENU_RESET_NO] = "Отмена",

    [UI_MENU_OV_FPS] = "%.0f FPS", [UI_MENU_OV_FRAME_MS] = "%.1f мс", [UI_MENU_OV_GAME_PCT] = "игра %.0f%%",
    [UI_MENU_OV_MAX_MS] = "макс. %.1f мс",
    [UI_MENU_OV_GPU] = "GPU", [UI_MENU_OV_CPU] = "ЦП", [UI_MENU_OV_RAM] = "ОЗУ", [UI_MENU_OV_VRAM] = "Видеопамять",
    [UI_MENU_OV_TEMP] = "Температура", [UI_MENU_OV_POWER] = "Мощность", [UI_MENU_OV_CLOCK] = "Частота",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "Подготовка кадра для GPU", [UI_MENU_OV_GPU_TIME] = "Занятость GPU за кадр",
    [UI_MENU_OV_PIPE_WAIT] = "Ожидания конвейеров",
    [UI_MENU_OV_SHADER_WAIT] = "Ожидания шейдеров", [UI_MENU_OV_TEX_UPLOADS] = "Загрузки текстур",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%", [UI_MENU_OV_FMT_TEMP] = "%.0f °C", [UI_MENU_OV_FMT_POWER] = "%.0f Вт",
    [UI_MENU_OV_FMT_MHZ] = "%.0f МГц", [UI_MENU_OV_FMT_GB] = "%.1f ГБ", [UI_MENU_OV_FMT_GB_OF] = "/ %.1f ГБ",
    [UI_MENU_OV_GAME_GB] = "игра %.1f ГБ", [UI_MENU_OV_FMT_MS] = "%.2f мс", [UI_MENU_OV_PER_FRAME] = "%.1f за кадр",
    [UI_MENU_OV_WINDOW] = "Окно", [UI_MENU_OV_OUTPUT] = "Изображение игры", [UI_MENU_OV_SCENE] = "3D-сцена",
    [UI_MENU_OV_SCALING] = "Масштабирование", [UI_MENU_OV_LIMIT] = "Лимит кадров", [UI_MENU_OV_PRESENT] = "Вывод кадров",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u", [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, резкость %d%%", [UI_MENU_OV_SCALE_NONE] = "нет (1:1)",
    [UI_MENU_OV_SCALE_BILINEAR] = "билинейное",
    [UI_MENU_OV_SCALE_DOWN] = "билинейное (FSR только увеличивает)",
    [UI_MENU_TAB_CHEATS] = "Читы",
    [UI_MENU_CHEATS_NONE] = "Для этой версии игры читов нет. Читы мода — это файл читов Atmosphere в его папке cheats "
                            "с именем по сборке игры: mods\\<мод>\\cheats\\%s. Перезапустите игру, чтобы они "
                            "прочитались. Для читов, меняющих код игры, игра пересоберётся сама при запуске (несколько "
                            "минут).",
    [UI_MENU_CHEATS_MOD] = "%s — читов: %d",
    [UI_MENU_CHEAT_CODE_DESC] = "Меняет код игры. Включается и выключается сразу.",
    [UI_MENU_CHEAT_KEY_DESC] = "Меняет код игры, пока вы держите %s. Включается и выключается сразу.",
    [UI_MENU_CHEAT_DATA_DESC] = "Пока включён, записывает память игры каждый кадр.",
    [UI_MENU_CHEAT_MASTER] = "всегда вкл.",
    [UI_MENU_CHEAT_MASTER_DESC] = "Главный код файла: включён всегда.",
    [UI_MENU_CHEAT_RESTORE] = "не нужен",
    [UI_MENU_CHEAT_RESTORE_DESC] = "Он только возвращает исходный код игры. Выключение чита уже делает это.",
    [UI_MENU_CHEAT_REBUILD] = "после пересборки",
    [UI_MENU_CHEAT_REBUILD_DESC] = "Этот чит меняет код игры и появился или изменился после сборки игры. Игра соберёт "
                                   "его при следующем запуске (несколько минут), или нажмите «Пересобрать игру сейчас».",
    [UI_MENU_CHEAT_UNSUPPORTED] = "не поддерживается",
    [UI_MENU_CHEAT_UNSUPPORTED_DESC] = "Этот чит нельзя использовать с этой версией игры:",
    [UI_MENU_CHEATS_REBUILD] = "Пересобрать игру сейчас",
    [UI_MENU_CHEATS_REBUILD_DESC] = "Некоторые читы меняют код игры и ещё не собраны в неё: они новые или изменены, "
                                    "либо прошлая пересборка не удалась или была отменена. Закрывает игру; установщик "
                                    "собирает их (несколько минут) и снова запускает игру. Прогресс после последнего "
                                    "сохранения будет потерян.",
    [UI_MENU_MODS_EXEFS_UNSUPPORTED] = "Этот мод меняет код игры (exefs) так, как порт применить не может, поэтому мод "
                                       "выключен целиком. %s",
    [UI_MENU_MODS_EXEFS_OTHER] = "Его патчи кода (exefs) сделаны для другой версии игры.",
    [UI_MENU_MODS_EXEFS_ON] = "Патчи кода в работе: файлов: %d.",
    [UI_MENU_MODS_REPLACES_BUILTIN] = "Патчи кода в работе: файлов: %d. Они заменяют собственный патч порта %s, "
                                      "который выключен, пока этот мод включён.",
};

static const char *const text_pt_br[UI_TEXT_COUNT] = {
    SWKBD("Inserir texto", "Insira um nome", "Cavalo", "OK", "Cancelar", "Controle: A = OK, B = Cancelar"),

    [UI_MENU_TITLE] = "Configurações",
    [UI_MENU_TAB_SCREEN] = "Tela", [UI_MENU_TAB_GAME] = "Jogo", [UI_MENU_TAB_SOUND] = "Som",
    [UI_MENU_TAB_CONTROLS] = "Controles", [UI_MENU_TAB_AMIIBO] = "Amiibo", [UI_MENU_TAB_MENU] = "Menu", [UI_MENU_TAB_ADVANCED] = "Avançado",
    [UI_MENU_TAB_MODS] = "Mods",
    [UI_MENU_MODS_BUILTIN] = "Embutidos no port", [UI_MENU_MODS_INSTALLED] = "Mods instalados",
    [UI_MENU_MODS_NONE] = "Nenhum mod ainda. Descompacte um mod na pasta mods, uma pasta por mod com a pasta romfs dentro, "
                          "e reinicie o jogo. O TKMM também pode exportar os mods mesclados para uma pasta ali.",
    [UI_MENU_MODS_OPEN] = "Abrir pasta de mods",
    [UI_MENU_MODS_OPEN_DESC] = "Mostra a pasta de onde o jogo carrega os mods. Ela é criada se não existir.",
    [UI_MENU_MODS_ROW_DESC] = "Arquivos do jogo que substitui: %u. Arquivos novos que adiciona: %u.",
    [UI_MENU_MODS_OVERRIDDEN] = "%u dos seus arquivos vêm de %s, que está acima na lista.",
    [UI_MENU_MODS_MOVE_HINT] = "O botão de cima ou Ctrl+Cima sobe o mod, o botão da esquerda ou Ctrl+Baixo desce. "
                               "Quando dois mods mudam o mesmo arquivo, vale o de cima. As mudanças valem após reiniciar.",
    [UI_MENU_MODS_EMPTY] = "Esta pasta não tem arquivos romfs, exefs ou cheats.",
    [UI_MENU_MODS_SUBSDK] = "Este mod traz código próprio do jogo (um plugin subsdk ou um main substituído). O port não "
                            "carrega módulos de código, então o mod inteiro fica desligado.",
    [UI_MENU_MODS_EXEFS_LATER] = "Este mod altera o código do jogo (exefs). O jogo compila esses patches na próxima "
                                 "vez que iniciar (uma recompilação de alguns minutos); até lá o mod inteiro fica "
                                 "desligado.",
    [UI_MENU_MODS_CHEATS] = "Cheats: %d. Ligue-os na aba Cheats.",
    [UI_MENU_MODS_CHEATS_FILE] = "Os cheats desta versão do jogo são lidos ao iniciar de cheats\\%s neste mod.",
    [UI_MENU_MODS_BAD_NAME] = "O nome da pasta contém |, que a lista de mods usa entre os nomes. Renomeie a pasta.",
    [UI_MENU_MODS_OTHER_VERSION] = "%u dos seus arquivos são de outra versão do jogo e ficam de fora. Procure uma versão "
                                   "deste mod feita para a sua versão do jogo.",
    [UI_MENU_MODS_UNUSED] = "não usado",
    [UI_MENU_MODS_FPS60] = "Patch de 60 FPS",
    [UI_MENU_MODS_FPS60_DESC] = "O patch do próprio port para 60 quadros por segundo. Ajuste em Limite de quadros na aba "
                                "Jogo.",
    [UI_MENU_MODS_RES] = "Patch de resolução",
    [UI_MENU_MODS_RES_DESC] = "O patch do próprio port para resoluções acima de 1080p. Ajuste em Resolução do jogo na aba "
                              "Tela.",
    [UI_MENU_MODS_QUICKMENU] = "Correção do menu rápido",
    [UI_MENU_MODS_QUICKMENU_DESC] = "A correção do port para o fundo preto dos menus de armas, escudos e materiais a 60 "
                                    "FPS.",
    [UI_MENU_CONTINUE] = "Continuar", [UI_MENU_QUIT] = "Sair do jogo",
    [UI_MENU_ON] = "Ligado", [UI_MENU_OFF] = "Desligado", [UI_MENU_AUTO] = "Automático",
    [UI_MENU_DEFAULT_MARK] = "padrão",
    [UI_MENU_AFTER_RESTART] = "após reiniciar", [UI_MENU_SET_BY_ENV] = "definido por variável de ambiente",
    [UI_MENU_SAVE_FAILED] = "não foi possível salvar as configurações",
    [UI_MENU_VERSION] = "TotK Recomp %s, jogo %s",

    [UI_MENU_FULLSCREEN] = "Tela cheia", [UI_MENU_FULLSCREEN_DESC] = "O jogo ocupa a tela inteira; F11 alterna.",
    [UI_MENU_WINDOW_SIZE] = "Tamanho da janela",
    [UI_MENU_WINDOW_SIZE_DESC] = "Tamanho da janela do jogo fora da tela cheia.",
    [UI_MENU_WINDOW_SIZE_CUSTOM] = "Personalizado",
    [UI_MENU_PRESENT] = "Apresentação", [UI_MENU_PRESENT_DESC] = "Como os quadros prontos chegam à tela.",
    [UI_MENU_PRESENT_MAILBOX] = "Mailbox", [UI_MENU_PRESENT_VSYNC] = "VSync", [UI_MENU_PRESENT_IMMEDIATE] = "Imediato",
    [UI_MENU_PRESENT_IN_USE] = "em uso: %s",
    [UI_MENU_GPU] = "Placa de vídeo", [UI_MENU_GPU_DESC] = "O dispositivo Vulkan que desenha o jogo.",
    [UI_MENU_GPU_AUTO] = "Automática",
    [UI_MENU_PERF_OVERLAY] = "Indicador de desempenho",
    [UI_MENU_PERF_OVERLAY_DESC] = "Taxa de quadros no canto superior direito. Completo adiciona os tempos de quadro com "
                                  "gráfico, o uso de CPU, GPU e memória, as temperaturas e as esperas da thread de "
                                  "renderização. F10 alterna os modos.",
    [UI_MENU_PERF_OFF] = "Desligado", [UI_MENU_PERF_FPS] = "FPS", [UI_MENU_PERF_FULL] = "Completo",
    [UI_PERF_LINE_FPS] = "%.0f fps",
    [UI_MENU_GRAPHICS] = "Imagem",
    [UI_MENU_OUTPUT_RES] = "Resolução do jogo",
    [UI_MENU_OUTPUT_RES_DESC] = "O tamanho em que o jogo desenha a imagem e a interface. Acima de 1920 x 1080 o jogo "
                                "precisa de mais memória de vídeo e tempo de GPU. Um tamanho mais largo (21:9) amplia a "
                                "visão da câmera e leva o HUD para os cantos da tela; um mais alto (16:10, Steam Deck) "
                                "leva-o para as bordas de cima e de baixo.",
    [UI_MENU_RENDER_SCALE] = "Escala da renderização 3D",
    [UI_MENU_RENDER_SCALE_DESC] = "Tamanho da cena 3D em relação à resolução do jogo; o FSR do jogo a amplia até esse "
                                  "tamanho. Acima de 100 % suaviza bordas serrilhadas (supersampling) com alto custo de "
                                  "GPU.",
    [UI_MENU_RENDER_SCALE_GAME] = "Jogo (83 %)",
    [UI_MENU_UPSCALER] = "Escala para a janela",
    [UI_MENU_UPSCALER_DESC] = "Como a imagem é ampliada quando a janela é maior que a resolução do jogo. O FSR mantém "
                              "bordas e detalhes mais nítidos que o bilinear.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1", [UI_MENU_UPSCALER_BILINEAR] = "Bilinear",
    [UI_MENU_ASPECT] = "Formato da imagem",
    [UI_MENU_ASPECT_DESC] = "Quando o formato da janela difere do da resolução do jogo (uma imagem 16:9 numa tela "
                            "21:9): manter o formato da imagem com barras pretas ou esticá-la sobre a janela.",
    [UI_MENU_ASPECT_FIT] = "Barras pretas", [UI_MENU_ASPECT_STRETCH] = "Esticar",
    [UI_MENU_SHARPNESS] = "Nitidez do FSR",
    [UI_MENU_SHARPNESS_DESC] = "Aumento de nitidez após a ampliação do FSR; 0 desliga.",

    [UI_MENU_FPS_LIMIT] = "Limite de quadros",
    [UI_MENU_FPS_LIMIT_DESC] = "O máximo de quadros por segundo que o jogo desenha. Acima de 60 é experimental e "
                               "precisa de Velocidade do jogo pelo relógio.",
    [UI_MENU_FPS_30] = "30 FPS", [UI_MENU_FPS_40] = "40 FPS", [UI_MENU_FPS_45] = "45 FPS",
    [UI_MENU_FPS_60] = "60 FPS", [UI_MENU_FPS_90] = "90 FPS", [UI_MENU_FPS_120] = "120 FPS", [UI_MENU_FPS_144] = "144 FPS",
    [UI_MENU_ASSUME_FOCUS] = "Continuar rodando fora de foco",
    [UI_MENU_ASSUME_FOCUS_DESC] = "Ligado (o padrão), o jogo continua rodando enquanto você está em outra janela, e o "
                                  "teclado fica sem efeito até a janela voltar para a frente. Desligado, ele pausa quando "
                                  "a janela perde o foco, como no Switch ao apertar Home; um carregamento interrompido "
                                  "assim pode parar de vez.",
    [UI_MENU_FPS_DISPLAY] = "Taxa do monitor", [UI_MENU_FPS_DISPLAY_HZ] = "Taxa do monitor (%.0f Hz)",
    [UI_MENU_REALTIME] = "Velocidade do jogo pelo relógio",
    [UI_MENU_REALTIME_DESC] = "Avança o jogo pelo tempo real, para que um quadro lento não deixe o jogo mais lento.",
    [UI_MENU_CLOTH_REALTIME] = "Tecidos pelo relógio",
    [UI_MENU_CLOTH_REALTIME_DESC] = "Avança tecidos e cabelos pelo tempo real também.",
    [UI_MENU_HIGHFPS_FIX] = "Correções para alta taxa de quadros",
    [UI_MENU_HIGHFPS_FIX_DESC] = "Corrige a física que se comporta mal acima de 30 quadros por segundo e objetos que ficam transparentes ou invisíveis acima de 60.",
    [UI_MENU_GAME_LANG] = "Idioma do jogo", [UI_MENU_GAME_LANG_DESC] = "Idioma dos textos e das vozes do jogo.",
    [UI_MENU_GAME_LANG_AUTO] = "Automático",
    [UI_MENU_OPERATION_MODE] = "Modo do console",
    [UI_MENU_OPERATION_MODE_DESC] = "Se o jogo acha que está no modo TV ou no modo portátil.",
    [UI_MENU_MODE_DOCKED] = "Modo TV", [UI_MENU_MODE_HANDHELD] = "Modo portátil",

    [UI_MENU_VOLUME] = "Volume", [UI_MENU_VOLUME_DESC] = "Volume geral do jogo.",

    [UI_MENU_MOTION] = "Controles de movimento", [UI_MENU_MOTION_DESC] = "Mirar com o giroscópio do controle.",
    [UI_MENU_RUMBLE_STRENGTH] = "Intensidade da vibração",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "Quão forte o controle vibra.",
    [UI_MENU_RUMBLE_CURVE] = "Curva da vibração",
    [UI_MENU_RUMBLE_CURVE_DESC] = "Valores menores deixam a vibração fraca mais forte.",
    [UI_MENU_RUMBLE_TRIGGERS] = "Vibração dos gatilhos (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Também vibra os gatilhos de um controle de Xbox.",
    [UI_MENU_PAD_LAYOUT] = "Layout dos botões",
    [UI_MENU_PAD_LAYOUT_DESC] = "Qual botão é o A. Nintendo: o da direita, como no Switch. Xbox: o de baixo, como "
                                "indicado no controle de Xbox (cruz no controle de PlayStation). Automático escolhe "
                                "Nintendo para controles com os rótulos da Nintendo e Xbox para os demais. Ícones de "
                                "Xbox: o A é o da direita, como no Nintendo, e o jogo mostra os rótulos do Xbox (B no "
                                "lugar de A, A no lugar de B, Y no lugar de X, X no lugar de Y); os ícones mudam após "
                                "reiniciar. As letras fazem parte dos arquivos do jogo, então um botão remapeado em "
                                "Controles continua com a letra antiga na tela.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Automático", [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A à direita)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A embaixo)",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "em uso: %s",
    [UI_MENU_LSTICK_DEADZONE] = "Zona morta do analógico esquerdo",
    [UI_MENU_RSTICK_DEADZONE] = "Zona morta do analógico direito",
    [UI_MENU_STICK_DEADZONE_DESC] = "Quanto um analógico do controle precisa se mover antes que o jogo perceba. Aumente "
                                    "se o personagem ou a câmera se moverem sozinhos: solte o analógico, leia \"analógico "
                                    "agora\" e coloque a zona morta alguns por cento acima.",
    [UI_MENU_STICK_DEADZONE_NOW] = "analógico agora: %.0f %%",
    [UI_MENU_MOUSE_SENS] = "Sensibilidade da câmera no mouse",
    [UI_MENU_MOUSE_SENS_DESC] = "Quão rápido o mouse gira a câmera.",

    [UI_BINDS_HELP] = "Selecione uma célula e aperte A ou Enter, depois a tecla, o botão do mouse ou o botão do "
                      "controle. Delete ou X limpa a célula. Uma tecla já em uso vem para cá; um botão do controle "
                      "troca de lugar com o que ele substitui.",
    [UI_BINDS_COL_KEY1] = "Tecla 1", [UI_BINDS_COL_KEY2] = "Tecla 2", [UI_BINDS_COL_PAD] = "Controle",
    [UI_BINDS_GROUP_BUTTONS] = "Botões", [UI_BINDS_GROUP_MOVE] = "Movimento", [UI_BINDS_GROUP_CAMERA] = "Câmera",
    [UI_BINDS_PRESS_KEY] = "Aperte uma tecla ou um botão do mouse para %s",
    [UI_BINDS_PRESS_PAD] = "Aperte um botão do controle para %s",
    [UI_BINDS_CANCEL_HINT] = "Esc cancela, ou aguarde %d s",
    [UI_BINDS_TAKEN] = "%s saiu de %s", [UI_BINDS_SWAPPED] = "%s trocou de lugar com %s",
    [UI_BINDS_RESET] = "Restaurar todos os botões",
    [UI_BINDS_RESET_DESC] = "Volta às teclas e aos botões do controle padrão. O layout dos botões acima volta a valer: "
                            "os botões do controle remapeados ficam onde você os colocou até esta restauração.",
    [UI_BIND_A] = "A", [UI_BIND_B] = "B", [UI_BIND_X] = "X", [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "Clique do analógico esquerdo", [UI_BIND_RCLICK] = "Clique do analógico direito",
    [UI_BIND_L] = "L", [UI_BIND_R] = "R", [UI_BIND_ZL] = "ZL", [UI_BIND_ZR] = "ZR", [UI_BIND_PLUS] = "Mais",
    [UI_BIND_MINUS] = "Menos",
    [UI_BIND_LEFT] = "Direcional para a esquerda", [UI_BIND_UP] = "Direcional para cima",
    [UI_BIND_RIGHT] = "Direcional para a direita", [UI_BIND_DOWN] = "Direcional para baixo",
    [UI_BIND_LSTICK_LEFT] = "Mover para a esquerda", [UI_BIND_LSTICK_UP] = "Mover para a frente",
    [UI_BIND_LSTICK_RIGHT] = "Mover para a direita", [UI_BIND_LSTICK_DOWN] = "Mover para trás",
    [UI_BIND_RSTICK_LEFT] = "Câmera para a esquerda", [UI_BIND_RSTICK_UP] = "Câmera para cima",
    [UI_BIND_RSTICK_RIGHT] = "Câmera para a direita", [UI_BIND_RSTICK_DOWN] = "Câmera para baixo",
    [UI_BIND_DESC_A] = "Interagir, pegar, conversar.", [UI_BIND_DESC_B] = "Correr, cancelar, voltar.",
    [UI_BIND_DESC_X] = "Pular.", [UI_BIND_DESC_Y] = "Atacar.", [UI_BIND_DESC_LCLICK] = "Agachar.",
    [UI_BIND_DESC_RCLICK] = "Visão em primeira pessoa.", [UI_BIND_DESC_L] = "Habilidades.",
    [UI_BIND_DESC_R] = "Arremessar a arma.", [UI_BIND_DESC_ZL] = "Escudo, travar a mira.", [UI_BIND_DESC_ZR] = "Arco.",
    [UI_BIND_DESC_PLUS] = "Menu, inventário.", [UI_BIND_DESC_MINUS] = "Mapa.",
    [UI_BIND_DESC_DPAD] = "Seleção rápida.",
    [UI_BIND_DESC_MOVE] = "Move o Link como o analógico esquerdo. O analógico esquerdo do controle também sempre "
                          "funciona.",
    [UI_BIND_DESC_CAMERA] = "Gira a câmera como o analógico direito. O mouse e o analógico direito do controle também "
                            "sempre funcionam.",
    [UI_KEY_SPACE] = "Espaço", [UI_KEY_TAB] = "Tab", [UI_KEY_LSHIFT] = "Shift esquerdo",
    [UI_KEY_LCTRL] = "Ctrl esquerdo",
    [UI_KEY_UP] = "Seta para cima", [UI_KEY_DOWN] = "Seta para baixo", [UI_KEY_LEFT] = "Seta para a esquerda",
    [UI_KEY_RIGHT] = "Seta para a direita",
    [UI_KEY_MOUSE_L] = "Mouse esquerdo", [UI_KEY_MOUSE_R] = "Mouse direito", [UI_KEY_MOUSE_M] = "Mouse do meio",
    [UI_KEY_MOUSE_X1] = "Mouse 4", [UI_KEY_MOUSE_X2] = "Mouse 5",
    [UI_KEY_ENTER] = "Enter", [UI_KEY_BACKSPACE] = "Backspace", [UI_KEY_RSHIFT] = "Shift direito",
    [UI_KEY_RCTRL] = "Ctrl direito", [UI_KEY_LALT] = "Alt", [UI_KEY_CAPSLOCK] = "Caps Lock", [UI_KEY_INSERT] = "Insert",
    [UI_KEY_DELETE] = "Delete", [UI_KEY_HOME] = "Home", [UI_KEY_END] = "End", [UI_KEY_PAGEUP] = "Page Up",
    [UI_KEY_PAGEDOWN] = "Page Down", [UI_KEY_NUM_FMT] = "Num %s",
    [UI_PAD_SOUTH] = "Botão de baixo", [UI_PAD_EAST] = "Botão da direita", [UI_PAD_WEST] = "Botão da esquerda",
    [UI_PAD_NORTH] = "Botão de cima", [UI_PAD_LSTICK] = "Analógico esquerdo", [UI_PAD_RSTICK] = "Analógico direito",
    [UI_PAD_L] = "Bumper esquerdo", [UI_PAD_R] = "Bumper direito", [UI_PAD_START] = "Start", [UI_PAD_BACK] = "Back",
    [UI_PAD_DPAD_LEFT] = "Direcional para a esquerda", [UI_PAD_DPAD_UP] = "Direcional para cima",
    [UI_PAD_DPAD_RIGHT] = "Direcional para a direita", [UI_PAD_DPAD_DOWN] = "Direcional para baixo",
    [UI_PAD_ZL] = "Gatilho esquerdo", [UI_PAD_ZR] = "Gatilho direito",
    [UI_PAD_LPADDLE1] = "Botão traseiro esquerdo", [UI_PAD_RPADDLE1] = "Botão traseiro direito",
    [UI_PAD_LPADDLE2] = "Botão traseiro esquerdo 2", [UI_PAD_RPADDLE2] = "Botão traseiro direito 2",
    [UI_PAD_CROSS] = "Cruz", [UI_PAD_CIRCLE] = "Círculo", [UI_PAD_SQUARE] = "Quadrado", [UI_PAD_TRIANGLE] = "Triângulo",

    [UI_MENU_AMIIBO_DIR] = "Pasta de amiibo",
    [UI_MENU_AMIIBO_OPEN] = "Abrir",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "Pasta padrão",
    [UI_MENU_AMIIBO_REFRESH] = "Atualizar",
    [UI_MENU_AMIIBO_PICK] = "Dumps de amiibo (arquivos .bin ou .nfc de 540 bytes):",
    [UI_MENU_AMIIBO_NO_DIR] = "Não foi possível abrir a pasta %s",
    [UI_MENU_AMIIBO_READER] = "Leitor NFC",
    [UI_MENU_AMIIBO_STATE] = "Leitor:",
    [UI_MENU_AMIIBO_STATE_IDLE] = "pronto",
    [UI_MENU_AMIIBO_STATE_SEARCHING] = "o jogo está esperando um amiibo",
    [UI_MENU_AMIIBO_STATE_FOUND] = "amiibo no leitor",
    [UI_MENU_AMIIBO_STATE_MOUNTED] = "o jogo está lendo o amiibo",
    [UI_MENU_AMIIBO_STATE_OFF] = "desligado",
    [UI_MENU_AMIIBO_FILE] = "Arquivo selecionado:",
    [UI_MENU_AMIIBO_NO_FILE] = "(nenhum arquivo selecionado)",
    [UI_MENU_AMIIBO_TAP] = "Encostar o amiibo",
    [UI_MENU_AMIIBO_REMOVE] = "Afastar o amiibo",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "Afastar o amiibo sozinho depois de encostar",
    [UI_MENU_AMIIBO_HOLD] = "Tempo no leitor",
    [UI_MENU_AMIIBO_SECONDS] = "%.1f s",
    [UI_MENU_AMIIBO_TAG] = "Amiibo no leitor:",
    [UI_MENU_AMIIBO_NO_TAG] = "Nenhum amiibo no leitor.",
    [UI_MENU_AMIIBO_TAG_FILE] = "Arquivo: %s",
    [UI_MENU_AMIIBO_TAG_ID] = "Personagem %s, série %02x, número %04x, tipo %02x",
    [UI_MENU_AMIIBO_TAG_UID] = "UID: %s",
    [UI_MENU_AMIIBO_READY] = "Pronto",
    [UI_MENU_AMIIBO_TAPPED] = "Encostado: %s",
    [UI_MENU_AMIIBO_LOAD_FAILED] = "Não é um dump de amiibo (NTAG215, 540 bytes): %s",
    [UI_MENU_AMIIBO_REMOVED] = "Amiibo afastado",
    [UI_MENU_AMIIBO_STATUS] = "Estado: %s",
    [UI_MENU_MENU_LANG] = "Idioma do menu", [UI_MENU_MENU_LANG_DESC] = "Idioma deste menu e do indicador.",
    [UI_MENU_MENU_LANG_AUTO] = "Automático (do Windows)",

    [UI_MENU_ADV_WARNING] = "Estas configurações são para testes e solução de problemas. Altere-as só quando souber "
                            "por quê.",
    [UI_MENU_GROUP_SCHED] = "Agendamento", [UI_MENU_GROUP_SHADERS] = "Shaders e pipelines",
    [UI_MENU_GROUP_TEXMEM] = "Texturas e memória", [UI_MENU_GROUP_FIXES] = "Correções de renderização",
    [UI_MENU_GROUP_VULKAN] = "Vulkan", [UI_MENU_GROUP_DIAG] = "Diagnóstico",
    [UI_MENU_ASYNC_PREEMPT] = "Preempção de núcleo virtual",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "Deixa uma thread do jogo interromper outra no mesmo núcleo virtual.",
    [UI_MENU_PREEMPT_OFF] = "Desligado", [UI_MENU_PREEMPT_ON] = "Ligado", [UI_MENU_PREEMPT_INHERIT] = "Só herança",
    [UI_MENU_REPLAY_ASYNC] = "Preparo de quadro da GPU em thread própria",
    [UI_MENU_REPLAY_ASYNC_DESC] = "Prepara os quadros para a GPU numa thread separada em vez da thread do jogo. Adiciona até um quadro de latência.",
    [UI_MENU_TIMER_1MS] = "Timer do Windows de 1 ms",
    [UI_MENU_TIMER_1MS_DESC] = "Aumenta a resolução do timer do sistema para 1 ms, para um ritmo de quadros mais estável.",
    [UI_MENU_PIPE_WAIT] = "Tratamento de desenhos em espera",
    [UI_MENU_PIPE_WAIT_DESC] = "Padrão mantém o agendamento atual, com um worker reservado para desenhos em espera; Antigo (draw) usa o agendamento anterior.",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "Padrão", [UI_MENU_PIPE_WAIT_DRAW] = "Antigo (draw)",
    [UI_MENU_PIPE_SKIP] = "Desenhos pulados ao montar pipeline",
    [UI_MENU_PIPE_SKIP_DESC] = "Quais desenhos são descartados em vez de esperar pelo seu pipeline.",
    [UI_MENU_PIPE_SKIP_STRICT] = "Estrito", [UI_MENU_PIPE_SKIP_ANY] = "Qualquer",
    [UI_MENU_PIPE_SKIP_FULLSCREEN] = "Passes de tela cheia",
    [UI_MENU_LIB_PREBUILD] = "Pré-montagem das bibliotecas de estágio",
    [UI_MENU_LIB_PREBUILD_DESC] = "Monta antes, em segundo plano, as bibliotecas de estágio dos shaders.",
    [UI_MENU_LIB_PREBUILD_THREADS] = "Threads da pré-montagem das bibliotecas",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "Threads usadas na pré-montagem; Automático escolhe um número pela sua CPU.",
    [UI_MENU_PIPE_PREBUILD] = "Pré-montagem de pipelines",
    [UI_MENU_PIPE_PREBUILD_DESC] = "Monta antes os pipelines conhecidos.",
    [UI_MENU_PREWARM_XLATE] = "Pré-aquecimento do tradutor",
    [UI_MENU_PREWARM_XLATE_DESC] = "Threads que traduzem os shaders conhecidos ao iniciar; Automático usa metade das threads da CPU, no máximo 16.",
    [UI_MENU_GPL] = "Biblioteca de pipelines gráficos",
    [UI_MENU_GPL_DESC] = "Usa a extensão de biblioteca de pipelines do Vulkan; desligue se o seu driver tiver problemas com ela.",
    [UI_MENU_ASTC] = "Decodificação de texturas ASTC",
    [UI_MENU_ASTC_DESC] = "Onde as texturas comprimidas em ASTC são decodificadas.",
    [UI_MENU_ASTC_AUTO] = "Automático", [UI_MENU_ASTC_GPU] = "Shader da GPU", [UI_MENU_ASTC_NATIVE] = "Nativo",
    [UI_MENU_ASTC_CPU] = "CPU",
    [UI_MENU_MEMORY] = "Modo de memória da GPU",
    [UI_MENU_MEMORY_DESC] = "Como a memória do jogo é compartilhada com a GPU.",
    [UI_MENU_MEMORY_AUTO] = "Automático", [UI_MENU_MEMORY_IMPORT] = "Importar", [UI_MENU_MEMORY_MIRROR] = "Espelhar",
    [UI_MENU_MEMORY_VERIFY] = "Verificar",
    [UI_MENU_TEX_ALIAS] = "Aliasing de texturas", [UI_MENU_TEX_ALIAS_DESC] = "Uma textura nova criada sobre a memória de uma textura viva começa com esse conteúdo em vez de vazia.",
    [UI_MENU_TEX_VRAM_PRESSURE] = "Limite de memória de texturas", [UI_MENU_TEX_VRAM_PRESSURE_DESC] = "Porcentagem da memória de vídeo acima da qual as texturas liberadas pelo jogo são descartadas (voltam do armazenamento quando usadas de novo). 0 mantém todas.",
    [UI_MENU_ASTC_BC] = "Comprimir ASTC em BC3", [UI_MENU_ASTC_BC_DESC] = "Com o decodificador da GPU: recodifica as texturas ASTC decodificadas em BC3, que ocupa um quarto da memória de vídeo. Para GPUs com 4 GB ou menos; as texturas ficam um pouco mais quadriculadas.",
    [UI_MENU_LOD_GUARD] = "Proteção de LOD", [UI_MENU_LOD_GUARD_DESC] = "Evita que objetos pisquem no lugar errado ou que o plano distante fique achatado por um quadro, esperando o jogo terminar de reescrever os dados de nível de detalhe antes que a GPU os leia.",
    [UI_MENU_FIX_C0] = "Correção do buffer de constantes",
    [UI_MENU_FIX_C0_DESC] = "Preenche o banco de constantes c0 reservado ao driver (tamanhos de armazenamento, tamanho do dispatch, tamanho do ponto, instância base) em vez de ler zeros.",
    [UI_MENU_FIX_ZERO] = "Zerar memória nova da GPU",
    [UI_MENU_FIX_ZERO_DESC] = "Novos pools de memória da GPU começam zerados, como a memória do Switch.",
    [UI_MENU_BIND_SKIP] = "Pular bindings repetidos",
    [UI_MENU_BIND_SKIP_DESC] = "Otimização: pula bindings que não mudaram.",
    [UI_MENU_SHADER_MEMO] = "Memória de shaders", [UI_MENU_SHADER_MEMO_DESC] = "Otimização: lembra as buscas de shaders.",
    [UI_MENU_PIPE_MEMO] = "Memória de pipeline", [UI_MENU_PIPE_MEMO_DESC] = "Otimização: reaproveita o pipeline do desenho anterior quando o estado se repete.",
    [UI_MENU_TEX_MEMO] = "Memória de bindings de textura", [UI_MENU_TEX_MEMO_DESC] = "Otimização: lembra os descritores de textura entre desenhos.",
    [UI_MENU_DESC_TEMPLATE] = "Templates de descritores",
    [UI_MENU_DESC_TEMPLATE_DESC] = "Otimização: atualiza os descritores em lote.",
    [UI_MENU_PUSH_SKIP] = "Pular push constants repetidas",
    [UI_MENU_PUSH_SKIP_DESC] = "Otimização: pula push constants que não mudaram.",
    [UI_MENU_SPLIT_DRAWS] = "Envio antecipado à GPU",
    [UI_MENU_SPLIT_DRAWS_DESC] = "Envia o trabalho da GPU em partes a cada N desenhos entre passes, para a GPU começar mais cedo; 0 desliga.",
    [UI_MENU_VK_VALIDATION] = "Camada de validação",
    [UI_MENU_VK_VALIDATION_DESC] = "Verifica cada chamada Vulkan e deixa o jogo muito mais lento.",
    [UI_MENU_VK_VALIDATION_MISSING] = "A camada de validação não está instalada.",
    [UI_MENU_VK_DYN_VERTEX] = "Estado dinâmico de vértices",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "Força a entrada dinâmica de vértices ligada ou desligada; o padrão depende da GPU.",
    [UI_MENU_VK_DYN_A2C] = "Alpha to coverage dinâmico",
    [UI_MENU_VK_DYN_A2C_DESC] = "Força o alpha to coverage dinâmico ligado ou desligado. Padrão: desligado no driver da AMD "
                                "para RDNA 2 e anteriores, que com ele desenha os personagens pretos, ligado nos demais.",
    [UI_MENU_VK_SUBGROUP_PIN] = "Fixar tamanho do subgrupo",
    [UI_MENU_VK_SUBGROUP_PIN_DESC] = "Fixa o tamanho do subgrupo dos shaders.",
    [UI_MENU_TRACE] = "Registro de desempenho", [UI_MENU_TRACE_DESC] = "Grava os tempos de quadro num arquivo.",
    [UI_MENU_FLIGHT] = "Caixa-preta",
    [UI_MENU_FLIGHT_DESC] = "Guarda um histórico curto e o salva quando um quadro trava.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "Limite de travamento (ms)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "Tempo de quadro que conta como travamento.",
    [UI_MENU_INPUT_RECORD] = "Gravação de comandos",
    [UI_MENU_INPUT_RECORD_DESC] = "Grava os seus comandos num arquivo para relatórios de bug.",
    [UI_MENU_OPEN_LOG] = "Abrir pasta de logs", [UI_MENU_OPEN_LOG_DESC] = "Mostra a pasta com os arquivos de log.",
    [UI_MENU_COPY_INFO] = "Copiar informações do sistema",
    [UI_MENU_COPY_INFO_DESC] = "Copia GPU, driver, CPU, RAM e as configurações alteradas para a área de transferência.",
    [UI_MENU_COPY_INFO_DONE] = "Copiado", [UI_MENU_ACTION_FAILED] = "não funcionou",
    [UI_MENU_RESET] = "Restaurar todas as configurações", [UI_MENU_RESET_DESC] = "Vale para todas as abas.",
    [UI_MENU_RESET_CONFIRM] = "Restaurar todas as configurações para o padrão?", [UI_MENU_RESET_YES] = "Restaurar",
    [UI_MENU_RESET_NO] = "Cancelar",

    [UI_MENU_OV_FPS] = "%.0f FPS", [UI_MENU_OV_FRAME_MS] = "%.1f ms", [UI_MENU_OV_GAME_PCT] = "jogo %.0f%%",
    [UI_MENU_OV_MAX_MS] = "máx. %.1f ms",
    [UI_MENU_OV_GPU] = "GPU", [UI_MENU_OV_CPU] = "CPU", [UI_MENU_OV_RAM] = "RAM", [UI_MENU_OV_VRAM] = "VRAM",
    [UI_MENU_OV_TEMP] = "Temperatura", [UI_MENU_OV_POWER] = "Consumo", [UI_MENU_OV_CLOCK] = "Frequência",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "Preparo de quadro da GPU", [UI_MENU_OV_GPU_TIME] = "GPU ocupada por quadro",
    [UI_MENU_OV_PIPE_WAIT] = "Esperas de pipeline",
    [UI_MENU_OV_SHADER_WAIT] = "Esperas de shader", [UI_MENU_OV_TEX_UPLOADS] = "Envios de textura",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%", [UI_MENU_OV_FMT_TEMP] = "%.0f °C", [UI_MENU_OV_FMT_POWER] = "%.0f W",
    [UI_MENU_OV_FMT_MHZ] = "%.0f MHz", [UI_MENU_OV_FMT_GB] = "%.1f GB", [UI_MENU_OV_FMT_GB_OF] = "/ %.1f GB",
    [UI_MENU_OV_GAME_GB] = "jogo %.1f GB", [UI_MENU_OV_FMT_MS] = "%.2f ms", [UI_MENU_OV_PER_FRAME] = "%.1f por quadro",
    [UI_MENU_OV_WINDOW] = "Janela", [UI_MENU_OV_OUTPUT] = "Imagem do jogo", [UI_MENU_OV_SCENE] = "Cena 3D",
    [UI_MENU_OV_SCALING] = "Escala", [UI_MENU_OV_LIMIT] = "Limite de quadros", [UI_MENU_OV_PRESENT] = "Apresentação",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u", [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, nitidez %d%%", [UI_MENU_OV_SCALE_NONE] = "nenhuma (1:1)",
    [UI_MENU_OV_SCALE_BILINEAR] = "bilinear",
    [UI_MENU_FOV] = "Campo de visão",
    [UI_MENU_FOV_DESC] = "Quanto do mundo a câmera mostra, em porcentagem do original do jogo. Só na versão 1.4.3 do "
                         "jogo.",
    [UI_MENU_SHADOW_RES] = "Tamanho do mapa de sombras",
    [UI_MENU_SHADOW_RES_DESC] = "Tamanho do mapa de sombras do sol. O tamanho do jogo muda com a vista, até cerca de "
                                "400 perto do chão, então as sombras das folhas ficam quadriculadas e pulam; um "
                                "tamanho fixo as deixa paradas e mais nítidas. Acima de 1024 as sombras custam mais "
                                "tempo de GPU e memória de vídeo.",
    [UI_MENU_SHADOW_RES_GAME] = "Do jogo (varia)",
    [UI_MENU_ANISO] = "Filtragem de texturas",
    [UI_MENU_ANISO_DESC] = "Filtragem anisotrópica das texturas de superfície: chão, estradas e paredes vistos em "
                           "ângulo raso continuam nítidos ao longe em vez de borrar. Jogo: o nível que o jogo pede, "
                           "como no Switch.",
    [UI_MENU_ANISO_GAME] = "Jogo",
    [UI_MENU_COLOR] = "Cor",
    [UI_MENU_COLOR_PRESET] = "Perfil de cor",
    [UI_MENU_COLOR_PRESET_DESC] = "Define os quatro valores abaixo. Vívido: mais contraste e saturação, sombras um "
                                  "pouco mais abertas (feito para telas OLED). Clareza: um pouco mais de contraste, "
                                  "saturação e brilho. Mexer em um valor torna o perfil Personalizado. O HUD muda com "
                                  "a imagem; este menu não.",
    [UI_MENU_COLOR_DEFAULT] = "Do jogo",
    [UI_MENU_COLOR_VIVID] = "Vívido (OLED)",
    [UI_MENU_COLOR_CLARITY] = "Clareza",
    [UI_MENU_COLOR_CUSTOM] = "Personalizado",
    [UI_MENU_COLOR_BRIGHTNESS] = "Brilho",
    [UI_MENU_COLOR_BRIGHTNESS_DESC] = "Somado a todas as cores; 0 não muda nada.",
    [UI_MENU_COLOR_CONTRAST] = "Contraste",
    [UI_MENU_COLOR_CONTRAST_DESC] = "Afasta as cores do cinza médio (acima de 100 %) ou as aproxima dele; 100 % não "
                                    "muda nada.",
    [UI_MENU_COLOR_SATURATION] = "Saturação",
    [UI_MENU_COLOR_SATURATION_DESC] = "Força da cor em relação ao cinza; 100 % não muda nada.",
    [UI_MENU_COLOR_GAMMA] = "Gama",
    [UI_MENU_COLOR_GAMMA_DESC] = "Abaixo de 1 clareia os tons escuros e médios, acima de 1 os escurece; 1 não muda "
                                 "nada.",
    [UI_MENU_HDR_GROUP] = "HDR",
    [UI_MENU_HDR] = "Saída HDR",
    [UI_MENU_HDR_DESC] = "Realces brilhantes em uma tela HDR. Precisa do HDR ligado no Windows (\"Usar HDR\") ou num "
                         "compositor Wayland com gerenciamento de cor; sem isso a imagem continua SDR. Vale após "
                         "reiniciar. O perfil de cor acima é para SDR e não é usado em HDR.",
    [UI_MENU_HDR_PEAK] = "Brilho máximo",
    [UI_MENU_HDR_PEAK_DESC] = "O pico da sua tela em nits; os realces mais claros param aí.",
    [UI_MENU_HDR_GAME] = "Brilho do jogo",
    [UI_MENU_HDR_GAME_DESC] = "Nits do branco da imagem (branco de papel). 203 é a referência de HDR; aumente num "
                              "ambiente claro.",
    [UI_MENU_HDR_UI] = "Brilho do HUD e do menu",
    [UI_MENU_HDR_UI_DESC] = "Nits do branco do HUD e deste menu.",
    [UI_MENU_PROMPTS] = "Ícones de botões",
    [UI_MENU_PROMPTS_DESC] = "Os botões de qual controle os ícones do jogo mostram. Nintendo: os do próprio jogo. "
                             "Xbox: LB, RB, LT, RT, Menu, Exibir e uma cruz direcional, e A, B, X e Y com a letra do "
                             "botão que o layout acima aperta (com Nintendo (A à direita) o botão da direita tem a "
                             "letra B, então os ícones de A mostram B). O jogo lê os ícones ao iniciar, então eles "
                             "mudam depois de reiniciar. Eles fazem parte dos arquivos do jogo, então um botão "
                             "remapeado em Controles continua com o ícone antigo na tela.",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo",
    [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_MISSING] = "faltam os arquivos dos ícones: execute a instalação de novo",
    [UI_MENU_PROMPTS_FAILED] = "arquivos dos ícones danificados: execute a instalação de novo",
    [UI_MENU_PROMPTS_RESTART] = "os ícones mudam após reiniciar",
    [UI_MENU_PROMPTS_OFF_SHAPE] = "desligados numa resolução de jogo que não é 16:9",
    [UI_MENU_PROMPTS_OFF_MODS] = "desligados: um mod de RomFS substitui os arquivos deles",
    [UI_MENU_PROMPTS_OFF_PATCHES] = "desligados enquanto TOTK_ROMFS_PATCHES está definido",
    [UI_MENU_RESTART] = "Reiniciar agora",
    [UI_MENU_RESTART_DESC] = "Fecha o jogo e o abre de novo com os novos ícones. O progresso desde o último "
                             "salvamento é perdido.",
    [UI_MENU_RESTART_CONFIRM] = "Reiniciar o jogo agora? O progresso desde o último salvamento é perdido.",
    [UI_MENU_RESTART_YES] = "Reiniciar",
    [UI_MENU_RESTART_SAVING] = "O jogo está salvando: dá para reiniciar em um instante.",
    [UI_MENU_STICK_SENS] = "Velocidade da câmera (analógico)",
    [UI_MENU_STICK_SENS_DESC] = "Multiplica a velocidade da câmera do analógico direito do jogo, incluindo a máxima, "
                                "em cada uma das cinco sensibilidades de câmera do jogo. O mouse mantém a própria "
                                "configuração.",
    [UI_MENU_WALK_KEY] = "Tecla de andar",
    [UI_MENU_WALK_KEY_DESC] = "Segure para inclinar os analógicos do teclado só em parte, assim o teclado anda em vez "
                              "de correr. A intensidade do andar define quanto. Controle e mouse não são afetados.",
    [UI_MENU_WALK_KEY_TAKEN] = "também %s",
    [UI_MENU_WALK_SCALE] = "Intensidade do andar",
    [UI_MENU_WALK_SCALE_DESC] = "A inclinação dos analógicos do teclado enquanto a tecla de andar está segura. Metade "
                                "é andar, a inclinação total é correr.",
    [UI_MENU_OV_SCALE_DOWN] = "bilinear (o FSR só amplia)",
    [UI_MENU_SHADOW_SCALE] = "Resolução das sombras",
    [UI_MENU_SHADOW_SCALE_DESC] = "Desenha os mapas de sombra do sol essa quantidade de vezes maiores em cada lado "
                                  "que os 960x960 do jogo: bordas de sombra e sombras de folhas mais nítidas. Vale no "
                                  "próximo início.",
    [UI_MENU_SHADOW_RANGE] = "Alcance das cascatas de sombra",
    [UI_MENU_SHADOW_RANGE_DESC] = "Afasta os degraus em que as sombras ficam mais suaves (20, 40, 80 e 160 m no "
                                  "jogo). Espalha o mapa de sombras por mais terreno: aumente a Resolução das sombras "
                                  "junto. Vale no próximo início.",
    [UI_MENU_CONTACT_SHADOWS] = "Sombras de contato",
    [UI_MENU_CONTACT_SHADOWS_DESC] = "Pequenas sombras do sol que os mapas de sombra não pegam: nos pés, sob pedras, "
                                     "na grama e em dobras. Traçadas na tela depois da iluminação. Vale no próximo "
                                     "início.",
    [UI_MENU_PCSS] = "Sombras suaves",
    [UI_MENU_PCSS_DESC] = "Sombras do sol nítidas onde um objeto toca o chão e mais suaves quanto mais longe a sombra "
                          "cai (PCSS). 0 mantém o filtro do jogo. Vale no próximo início.",
    [UI_MENU_DLAA] = "NVIDIA DLSS / DLAA",
    [UI_MENU_DLAA_DESC] = "NVIDIA DLSS para a cena 3D (placas NVIDIA RTX; nvngx_dlss.dll ao lado do totk.exe): bordas "
                          "mais suaves e menos cintilação. O HUD não é afetado. O modo abaixo define a resolução de "
                          "onde ele parte. Vale após reiniciar.",
    [UI_MENU_DLSS] = "Modo do DLSS",
    [UI_MENU_DLSS_DESC] = "DLAA desenha a cena 3D na resolução cheia. Qualidade, Equilibrado e Desempenho a desenham "
                          "em 67, 58 e 50 % e deixam o DLSS ampliá-la, para mais quadros por segundo; eles substituem "
                          "a escala 3D. Vale após reiniciar.",
    [UI_MENU_DLSS_DLAA] = "DLAA (nativo)",
    [UI_MENU_DLSS_QUALITY] = "Qualidade",
    [UI_MENU_DLSS_BALANCED] = "Equilibrado",
    [UI_MENU_DLSS_PERFORMANCE] = "Desempenho",
    [UI_MENU_FXAA] = "Antisserrilhado do jogo (FXAA)",
    [UI_MENU_FXAA_DESC] = "A suavização de bordas FXAA do próprio jogo. Borra um pouco os detalhes finos; com o DLAA "
                          "ligado não é necessária.",
    [UI_MENU_TAB_CHEATS] = "Cheats",
    [UI_MENU_CHEATS_NONE] = "Nenhum cheat para esta versão do jogo. Os cheats de um mod são um arquivo de cheats do "
                            "Atmosphere na pasta cheats dele, com o nome da build do jogo: mods\\<mod>\\cheats\\%s. "
                            "Reinicie o jogo para lê-los. Para cheats que alteram o código do jogo, o jogo se recompila "
                            "sozinho ao iniciar (alguns minutos).",
    [UI_MENU_CHEATS_MOD] = "%s: %d cheats",
    [UI_MENU_CHEAT_CODE_DESC] = "Altera o código do jogo. Liga e desliga na hora.",
    [UI_MENU_CHEAT_KEY_DESC] = "Altera o código do jogo enquanto você segura %s. Liga e desliga na hora.",
    [UI_MENU_CHEAT_DATA_DESC] = "Escreve na memória do jogo a cada quadro enquanto está ligado.",
    [UI_MENU_CHEAT_MASTER] = "sempre ligado",
    [UI_MENU_CHEAT_MASTER_DESC] = "O código mestre do arquivo: está sempre ligado.",
    [UI_MENU_CHEAT_RESTORE] = "desnecessário",
    [UI_MENU_CHEAT_RESTORE_DESC] = "Ele só escreve de volta o código original do jogo. Desligar um cheat já faz isso.",
    [UI_MENU_CHEAT_REBUILD] = "após recompilar",
    [UI_MENU_CHEAT_REBUILD_DESC] = "Este cheat altera o código do jogo e é novo ou foi editado depois que o jogo foi "
                                   "compilado. O jogo o compila na próxima vez que iniciar (alguns minutos), ou use "
                                   "Recompilar o jogo agora.",
    [UI_MENU_CHEAT_UNSUPPORTED] = "não suportado",
    [UI_MENU_CHEAT_UNSUPPORTED_DESC] = "Este cheat não pode ser usado com esta versão do jogo:",
    [UI_MENU_CHEATS_REBUILD] = "Recompilar o jogo agora",
    [UI_MENU_CHEATS_REBUILD_DESC] = "Alguns cheats alteram o código do jogo e ainda não foram compilados nele: são "
                                    "novos ou editados, ou a última recompilação falhou ou foi cancelada. Fecha o "
                                    "jogo; o instalador os compila (alguns minutos) e inicia o jogo de novo. O "
                                    "progresso desde o último salvamento é perdido.",
    [UI_MENU_MODS_EXEFS_UNSUPPORTED] = "Este mod altera o código do jogo (exefs) de um jeito que o port não consegue "
                                       "aplicar, então o mod inteiro fica desligado. %s",
    [UI_MENU_MODS_EXEFS_OTHER] = "Os patches de código dele (exefs) são para outra versão do jogo.",
    [UI_MENU_MODS_EXEFS_ON] = "Patches de código em uso: %d arquivos.",
    [UI_MENU_MODS_REPLACES_BUILTIN] = "Patches de código em uso: %d arquivos. Eles substituem o patch do próprio port "
                                      "%s, que fica desligado enquanto este mod está ligado.",
};

// Simplified Chinese: this row covers the software keyboard and the whole menu, so the menu and the overlay read Chinese
// when the menu language is zh-Hans (ui_text.h menu_text). The rows are fixed width, so the strings stay close to the
// English length; an id without a Chinese entry falls back to English.
static const char *const text_zh_hans[UI_TEXT_COUNT] = {
    SWKBD("文字输入", "请输入名字", "马儿", "确定", "取消", "手柄：A 确定，B 取消"),

    [UI_MENU_TITLE] = "设置",
    [UI_MENU_TAB_SCREEN] = "显示",
    [UI_MENU_TAB_GAME] = "游戏",
    [UI_MENU_TAB_SOUND] = "声音",
    [UI_MENU_TAB_CONTROLS] = "操作",
    [UI_MENU_TAB_AMIIBO] = "amiibo",
    [UI_MENU_TAB_MENU] = "菜单",
    [UI_MENU_TAB_ADVANCED] = "高级",
    [UI_MENU_TAB_MODS] = "模组",
    [UI_MENU_MODS_BUILTIN] = "移植版内置", [UI_MENU_MODS_INSTALLED] = "已安装的模组",
    [UI_MENU_MODS_NONE] = "还没有模组。把模组解压到 mods 文件夹，每个模组一个文件夹，里面放 romfs 文件夹，然后重启游戏。"
                          "TKMM 也可以把合并后的模组导出到那里的一个文件夹。",
    [UI_MENU_MODS_OPEN] = "打开模组文件夹",
    [UI_MENU_MODS_OPEN_DESC] = "显示游戏加载模组的文件夹。不存在时会创建它。",
    [UI_MENU_MODS_ROW_DESC] = "替换的游戏文件：%u。新增的文件：%u。",
    [UI_MENU_MODS_OVERRIDDEN] = "它的 %u 个文件改用列表中更靠上的 %s 的文件。",
    [UI_MENU_MODS_MOVE_HINT] = "上方按钮或 Ctrl+上 上移，左侧按钮或 Ctrl+下 下移。两个模组修改同一个文件时，以靠上的为准。"
                               "更改在重启后生效。",
    [UI_MENU_MODS_EMPTY] = "此文件夹中没有 romfs、exefs 或 cheats 文件。",
    [UI_MENU_MODS_SUBSDK] = "此模组带有自己的游戏代码（subsdk 插件或替换的 main）。移植版无法加载代码模块，因此整个模组保持关闭。",
    [UI_MENU_MODS_EXEFS_LATER] = "此模组修改游戏代码（exefs）。游戏会在下次启动时把这些补丁构建进去（重新构建需要几分钟）；在此之前整个模组保持关闭。",
    [UI_MENU_MODS_CHEATS] = "金手指：%d 个。在“金手指”标签页中开关。",
    [UI_MENU_MODS_CHEATS_FILE] = "此游戏版本的金手指在启动时从此模组的 cheats\\%s 读取。",
    [UI_MENU_MODS_BAD_NAME] = "文件夹名称含有 |，模组列表用它分隔名称。请重命名该文件夹。",
    [UI_MENU_MODS_OTHER_VERSION] = "它有 %u 个文件是为其他游戏版本制作的，已被跳过。请寻找适用于你的游戏版本的此模组。",
    [UI_MENU_MODS_UNUSED] = "未使用",
    [UI_MENU_MODS_FPS60] = "60 FPS 补丁",
    [UI_MENU_MODS_FPS60_DESC] = "移植版自带的 60 帧补丁。在“游戏”页的帧率上限中设置。",
    [UI_MENU_MODS_RES] = "分辨率补丁",
    [UI_MENU_MODS_RES_DESC] = "移植版自带的 1080p 以上分辨率补丁。在“显示”页的游戏分辨率中设置。",
    [UI_MENU_MODS_QUICKMENU] = "快捷菜单修复",
    [UI_MENU_MODS_QUICKMENU_DESC] = "移植版对 60 FPS 下武器、盾牌和材料快捷菜单黑色背景的修复。",
    [UI_MENU_CONTINUE] = "继续",
    [UI_MENU_QUIT] = "退出游戏",
    [UI_MENU_ON] = "开",
    [UI_MENU_OFF] = "关",
    [UI_MENU_AUTO] = "自动",
    [UI_MENU_DEFAULT_MARK] = "默认",
    [UI_MENU_AFTER_RESTART] = "重启后生效",
    [UI_MENU_SET_BY_ENV] = "由环境变量设定",
    [UI_MENU_SAVE_FAILED] = "无法保存设置",
    [UI_MENU_VERSION] = "TotK Recomp %s，游戏 %s",
    [UI_MENU_FULLSCREEN] = "全屏",
    [UI_MENU_FULLSCREEN_DESC] = "游戏铺满屏幕；按 F11 切换。",
    [UI_MENU_WINDOW_SIZE] = "窗口大小",
    [UI_MENU_WINDOW_SIZE_DESC] = "非全屏时游戏窗口的大小。",
    [UI_MENU_WINDOW_SIZE_CUSTOM] = "自定义",
    [UI_MENU_PRESENT] = "呈现模式",
    [UI_MENU_PRESENT_DESC] = "渲染完成的帧如何显示到屏幕上。",
    [UI_MENU_PRESENT_MAILBOX] = "Mailbox",
    [UI_MENU_PRESENT_VSYNC] = "垂直同步",
    [UI_MENU_PRESENT_IMMEDIATE] = "立即",
    [UI_MENU_PRESENT_IN_USE] = "使用中：%s",
    [UI_MENU_GPU] = "显卡",
    [UI_MENU_GPU_DESC] = "用于渲染游戏的 Vulkan 设备。",
    [UI_MENU_GPU_AUTO] = "自动",
    [UI_MENU_PERF_OVERLAY] = "性能浮层",
    [UI_MENU_PERF_OVERLAY_DESC] = "在右上角显示帧率。“完整”还会显示带图表的帧时间、CPU、"
                                  "GPU 和内存占用、温度，以及渲染线程的等待时间。"
                                  "按 F10 循环切换模式。",
    [UI_MENU_PERF_OFF] = "关",
    [UI_MENU_PERF_FPS] = "FPS",
    [UI_MENU_PERF_FULL] = "完整",

    [UI_PERF_LINE_FPS] = "%.0f fps",

    [UI_MENU_GRAPHICS] = "画面",
    [UI_MENU_OUTPUT_RES] = "游戏分辨率",
    [UI_MENU_OUTPUT_RES_DESC] = "游戏绘制画面和界面的尺寸。"
                                "高于 1920 x 1080 会占用更多显存和 GPU 时间。"
                                "更宽的尺寸（21:9）会扩大视野，并把界面移到屏幕四角；"
                                "更高的尺寸（16:10、Steam Deck）则移到上下边缘。",
    [UI_MENU_RENDER_SCALE] = "3D 渲染比例",
    [UI_MENU_RENDER_SCALE_DESC] = "3D 场景相对于游戏分辨率的尺寸；"
                                  "游戏的 FSR 会将其缩放到该尺寸。"
                                  "高于 100% 会平滑锯齿边缘（超采样），但 GPU 开销很高。",
    [UI_MENU_RENDER_SCALE_GAME] = "游戏（83%）",
    [UI_MENU_UPSCALER] = "放大到窗口",
    [UI_MENU_UPSCALER_DESC] = "窗口大于游戏分辨率时画面的放大方式。"
                              "FSR 比双线性更能保留边缘和细节。",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1",
    [UI_MENU_UPSCALER_BILINEAR] = "双线性",
    [UI_MENU_ASPECT] = "画面宽高比",
    [UI_MENU_ASPECT_DESC] = "当窗口宽高比和游戏分辨率不同时（21:9 屏幕上显示 16:9 画面）："
                            "用黑边保持画面比例，或拉伸铺满窗口。",
    [UI_MENU_ASPECT_FIT] = "黑边",
    [UI_MENU_ASPECT_STRETCH] = "拉伸",
    [UI_MENU_FOV] = "视野",
    [UI_MENU_FOV_DESC] = "镜头能看到多大范围，以游戏自身视野的百分比表示。"
                          "仅限游戏版本 1.4.3。",
    [UI_MENU_SHARPNESS] = "FSR 锐度",
    [UI_MENU_SHARPNESS_DESC] = "FSR 缩放后的锐化强度；0 为关闭。",
    [UI_MENU_COLOR] = "色彩",
    [UI_MENU_COLOR_PRESET] = "色彩配置",
    [UI_MENU_COLOR_PRESET_DESC] = "同时设置下面四项。鲜艳：对比度和饱和度更高，暗部略微提亮（为 OLED 屏幕调校）。"
                                  "清晰：对比度、饱和度和亮度都略高。改动任一项后，配置变为自定义。"
                                  "HUD 随画面一起变化，本菜单不随之变化。",
    [UI_MENU_COLOR_DEFAULT] = "游戏原样", [UI_MENU_COLOR_VIVID] = "鲜艳（OLED）",
    [UI_MENU_COLOR_CLARITY] = "清晰", [UI_MENU_COLOR_CUSTOM] = "自定义",
    [UI_MENU_COLOR_BRIGHTNESS] = "亮度",
    [UI_MENU_COLOR_BRIGHTNESS_DESC] = "叠加到每种颜色上；0 表示不调整。",
    [UI_MENU_COLOR_CONTRAST] = "对比度",
    [UI_MENU_COLOR_CONTRAST_DESC] = "把颜色从中灰向外拉开（高于 100%）或向中灰收拢；"
                                     "100% 表示不调整。",
    [UI_MENU_COLOR_SATURATION] = "饱和度",
    [UI_MENU_COLOR_SATURATION_DESC] = "颜色相对灰色的强度；100% 表示不调整。",
    [UI_MENU_COLOR_GAMMA] = "伽马",
    [UI_MENU_COLOR_GAMMA_DESC] = "小于 1 提亮暗部与中间调，大于 1 压暗；1 表示不调整。",
    [UI_MENU_FPS_LIMIT] = "帧率上限",
    [UI_MENU_FPS_LIMIT_DESC] = "游戏每秒绘制的最高帧数。高于 60 帧为实验性功能，"
                               "需要开启“游戏速度跟随时钟”。",
    [UI_MENU_FPS_30] = "30 FPS",
    [UI_MENU_FPS_40] = "40 FPS",
    [UI_MENU_FPS_45] = "45 FPS",
    [UI_MENU_FPS_60] = "60 FPS",
    [UI_MENU_FPS_90] = "90 FPS",
    [UI_MENU_FPS_120] = "120 FPS",
    [UI_MENU_FPS_144] = "144 FPS",
    [UI_MENU_FPS_DISPLAY] = "显示器刷新率",
    [UI_MENU_FPS_DISPLAY_HZ] = "显示器刷新率（%.0f Hz）",
    [UI_MENU_REALTIME] = "游戏速度跟随时钟",
    [UI_MENU_REALTIME_DESC] = "按真实时间推进游戏，帧率低时游戏速度也不会变慢。",
    [UI_MENU_CLOTH_REALTIME] = "布料跟随时钟",
    [UI_MENU_CLOTH_REALTIME_DESC] = "布料和头发也按真实时间推进。",
    [UI_MENU_HIGHFPS_FIX] = "高帧率修正",
    [UI_MENU_HIGHFPS_FIX_DESC] = "修正帧率高于 30 时出现异常的物理效果，"
                                 "以及帧率高于 60 时一直半透明或不可见的物体。",
    [UI_MENU_GAME_LANG] = "游戏语言",
    [UI_MENU_GAME_LANG_DESC] = "游戏文本和语音的语言。",
    [UI_MENU_GAME_LANG_AUTO] = "自动",
    [UI_MENU_OPERATION_MODE] = "主机模式",
    [UI_MENU_OPERATION_MODE_DESC] = "游戏认为自己处于底座模式还是掌机模式。",
    [UI_MENU_MODE_DOCKED] = "底座模式",
    [UI_MENU_MODE_HANDHELD] = "掌机模式",
    [UI_MENU_ASSUME_FOCUS] = "后台保持运行",
    [UI_MENU_ASSUME_FOCUS_DESC] = "开启（默认）时，切到其他窗口游戏仍会继续运行，"
                                  "但键盘要等窗口回到前台才会响应。关闭时，"
                                  "窗口失去焦点即暂停，如同在 Switch 上按 Home 键；"
                                  "这样被打断的读取可能会永久卡住。",
    [UI_MENU_VOLUME] = "音量",
    [UI_MENU_VOLUME_DESC] = "游戏的主音量。",
    [UI_MENU_MOTION] = "体感操作",
    [UI_MENU_MOTION_DESC] = "用手柄的陀螺仪瞄准。",
    [UI_MENU_RUMBLE_STRENGTH] = "振动强度",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "手柄振动的强弱。",
    [UI_MENU_RUMBLE_CURVE] = "振动曲线",
    [UI_MENU_RUMBLE_CURVE_DESC] = "数值越低，弱振动越强。",
    [UI_MENU_RUMBLE_TRIGGERS] = "扳机振动（Xbox）",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Xbox 手柄的扳机也会振动。",
    [UI_MENU_PAD_LAYOUT] = "按键布局",
    [UI_MENU_PAD_LAYOUT_DESC] = "哪个按键是 A。Nintendo：右侧按键，与 Switch 相同。Xbox：下方按键，"
                                "与 Xbox 手柄上的标注一致（PlayStation 手柄上为叉键）。自动："
                                "带 Nintendo 标注的手柄使用 Nintendo 布局，其他手柄使用 Xbox 布局。"
                                "Xbox 按键提示：A 仍是右侧键，但游戏显示 Xbox 标注（A 显示为 B、"
                                "B 显示为 A、X 显示为 Y、Y 显示为 X），重启后生效。"
                                "这些字母来自游戏文件，因此在“操作”中重映射过的按键，"
                                "屏幕上仍显示原来的字母。",
    [UI_MENU_PAD_LAYOUT_AUTO] = "自动",
    [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo（A 在右）",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox（A 在下）",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "使用中：%s",
    [UI_MENU_LSTICK_DEADZONE] = "左摇杆死区", [UI_MENU_RSTICK_DEADZONE] = "右摇杆死区",
    [UI_MENU_STICK_DEADZONE_DESC] = "手柄摇杆要推动多远游戏才会响应。如果角色或视角会自己移动，"
                                    "请调高此值：松开摇杆，查看“当前摇杆”，把死区设得比它高几个百分点。",
    [UI_MENU_STICK_DEADZONE_NOW] = "当前摇杆：%.0f %%",
    [UI_MENU_MOUSE_SENS] = "鼠标视角灵敏度",
    [UI_MENU_MOUSE_SENS_DESC] = "鼠标转动视角的速度。",

    [UI_BINDS_HELP] = "选中一个格子后按 A 或 Enter，再按键、鼠标键或手柄按键。"
                      "Delete 或 X 清除格子。已占用的按键会移到这里；"
                      "手柄按键与它替换的那个对调。",
    [UI_BINDS_COL_KEY1] = "按键 1",
    [UI_BINDS_COL_KEY2] = "按键 2",
    [UI_BINDS_COL_PAD] = "手柄",
    [UI_BINDS_GROUP_BUTTONS] = "按键",
    [UI_BINDS_GROUP_MOVE] = "移动",
    [UI_BINDS_GROUP_CAMERA] = "视角",
    [UI_BINDS_PRESS_KEY] = "请为“%s”按下一个键或鼠标键",
    [UI_BINDS_PRESS_PAD] = "请为“%s”按下一个手柄按键",
    [UI_BINDS_CANCEL_HINT] = "Esc 取消，或等待 %d 秒",
    [UI_BINDS_TAKEN] = "%s 已从 %s 移走",
    [UI_BINDS_SWAPPED] = "%s 已与 %s 对调",
    [UI_BINDS_RESET] = "重置所有按键",
    [UI_BINDS_RESET_DESC] = "恢复默认的键盘和手柄按键。上方的按键布局会再次生效："
                            "重映射过的手柄按键会一直保留到此次重置。",
    [UI_BIND_A] = "A",
    [UI_BIND_B] = "B",
    [UI_BIND_X] = "X",
    [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "左摇杆按下",
    [UI_BIND_RCLICK] = "右摇杆按下",
    [UI_BIND_L] = "L",
    [UI_BIND_R] = "R",
    [UI_BIND_ZL] = "ZL",
    [UI_BIND_ZR] = "ZR",
    [UI_BIND_PLUS] = "+ 键",
    [UI_BIND_MINUS] = "- 键",
    [UI_BIND_LEFT] = "十字键左",
    [UI_BIND_UP] = "十字键上",
    [UI_BIND_RIGHT] = "十字键右",
    [UI_BIND_DOWN] = "十字键下",
    [UI_BIND_LSTICK_LEFT] = "向左移动",
    [UI_BIND_LSTICK_UP] = "向前移动",
    [UI_BIND_LSTICK_RIGHT] = "向右移动",
    [UI_BIND_LSTICK_DOWN] = "向后移动",
    [UI_BIND_RSTICK_LEFT] = "视角左转",
    [UI_BIND_RSTICK_UP] = "视角上抬",
    [UI_BIND_RSTICK_RIGHT] = "视角右转",
    [UI_BIND_RSTICK_DOWN] = "视角下压",
    [UI_BIND_DESC_A] = "互动、拾取、对话。",
    [UI_BIND_DESC_B] = "奔跑、取消、返回。",
    [UI_BIND_DESC_X] = "跳跃。",
    [UI_BIND_DESC_Y] = "攻击。",
    [UI_BIND_DESC_LCLICK] = "蹲下。",
    [UI_BIND_DESC_RCLICK] = "第一人称视角。",
    [UI_BIND_DESC_L] = "能力。",
    [UI_BIND_DESC_R] = "投掷武器。",
    [UI_BIND_DESC_ZL] = "举盾、锁定。",
    [UI_BIND_DESC_ZR] = "弓箭。",
    [UI_BIND_DESC_PLUS] = "菜单、背包。",
    [UI_BIND_DESC_MINUS] = "地图。",
    [UI_BIND_DESC_DPAD] = "快捷选择。",
    [UI_BIND_DESC_MOVE] = "像左摇杆一样移动林克。手柄的左摇杆始终可用。",
    [UI_BIND_DESC_CAMERA] = "像右摇杆一样转动视角。鼠标和手柄的右摇杆始终可用。",
    [UI_KEY_SPACE] = "空格",
    [UI_KEY_TAB] = "Tab",
    [UI_KEY_LSHIFT] = "左 Shift",
    [UI_KEY_LCTRL] = "左 Ctrl",
    [UI_KEY_UP] = "上方向键",
    [UI_KEY_DOWN] = "下方向键",
    [UI_KEY_LEFT] = "左方向键",
    [UI_KEY_RIGHT] = "右方向键",
    [UI_KEY_MOUSE_L] = "鼠标左键",
    [UI_KEY_MOUSE_R] = "鼠标右键",
    [UI_KEY_MOUSE_M] = "鼠标中键",
    [UI_KEY_MOUSE_X1] = "鼠标侧键 4",
    [UI_KEY_MOUSE_X2] = "鼠标侧键 5",
    [UI_KEY_ENTER] = "回车",
    [UI_KEY_BACKSPACE] = "退格",
    [UI_KEY_RSHIFT] = "右 Shift",
    [UI_KEY_RCTRL] = "右 Ctrl",
    [UI_KEY_LALT] = "Alt",
    [UI_KEY_CAPSLOCK] = "Caps Lock",
    [UI_KEY_INSERT] = "Insert",
    [UI_KEY_DELETE] = "Delete",
    [UI_KEY_HOME] = "Home",
    [UI_KEY_END] = "End",
    [UI_KEY_PAGEUP] = "Page Up",
    [UI_KEY_PAGEDOWN] = "Page Down",
    [UI_KEY_NUM_FMT] = "小键盘 %s",
    [UI_PAD_SOUTH] = "下方按键",
    [UI_PAD_EAST] = "右侧按键",
    [UI_PAD_WEST] = "左侧按键",
    [UI_PAD_NORTH] = "上方按键",
    [UI_PAD_LSTICK] = "左摇杆",
    [UI_PAD_RSTICK] = "右摇杆",
    [UI_PAD_L] = "左肩键",
    [UI_PAD_R] = "右肩键",
    [UI_PAD_START] = "Start",
    [UI_PAD_BACK] = "Back",
    [UI_PAD_DPAD_LEFT] = "十字键左",
    [UI_PAD_DPAD_UP] = "十字键上",
    [UI_PAD_DPAD_RIGHT] = "十字键右",
    [UI_PAD_DPAD_DOWN] = "十字键下",
    [UI_PAD_ZL] = "左扳机",
    [UI_PAD_ZR] = "右扳机",
    [UI_PAD_LPADDLE1] = "左背键",
    [UI_PAD_RPADDLE1] = "右背键",
    [UI_PAD_LPADDLE2] = "左背键 2",
    [UI_PAD_RPADDLE2] = "右背键 2",
    [UI_PAD_CROSS] = "叉键",
    [UI_PAD_CIRCLE] = "圆圈键",
    [UI_PAD_SQUARE] = "方块键",
    [UI_PAD_TRIANGLE] = "三角键",

    [UI_MENU_AMIIBO_DIR] = "amiibo 文件夹",
    [UI_MENU_AMIIBO_OPEN] = "打开",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "默认文件夹",
    [UI_MENU_AMIIBO_REFRESH] = "刷新",
    [UI_MENU_AMIIBO_PICK] = "amiibo 转储（540 字节的 .bin 或 .nfc 文件）：",
    [UI_MENU_AMIIBO_NO_DIR] = "无法打开文件夹 %s",
    [UI_MENU_AMIIBO_READER] = "NFC 读取器",
    [UI_MENU_AMIIBO_STATE] = "读取器：",
    [UI_MENU_AMIIBO_STATE_IDLE] = "就绪",
    [UI_MENU_AMIIBO_STATE_SEARCHING] = "游戏正在等待 amiibo",
    [UI_MENU_AMIIBO_STATE_FOUND] = "amiibo 已放在读取器上",
    [UI_MENU_AMIIBO_STATE_MOUNTED] = "游戏正在读取 amiibo",
    [UI_MENU_AMIIBO_STATE_OFF] = "已关闭",
    [UI_MENU_AMIIBO_FILE] = "已选文件：",
    [UI_MENU_AMIIBO_NO_FILE] = "（未选择文件）",
    [UI_MENU_AMIIBO_TAP] = "放上 amiibo",
    [UI_MENU_AMIIBO_REMOVE] = "拿开 amiibo",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "放上后自动拿开 amiibo",
    [UI_MENU_AMIIBO_HOLD] = "停留时间",
    [UI_MENU_AMIIBO_SECONDS] = "%.1f 秒",
    [UI_MENU_AMIIBO_TAG] = "读取器上的 amiibo：",
    [UI_MENU_AMIIBO_NO_TAG] = "读取器上没有 amiibo。",
    [UI_MENU_AMIIBO_TAG_FILE] = "文件：%s",
    [UI_MENU_AMIIBO_TAG_ID] = "角色 %s，系列 %02x，编号 %04x，类型 %02x",
    [UI_MENU_AMIIBO_TAG_UID] = "UID：%s",
    [UI_MENU_AMIIBO_READY] = "就绪",
    [UI_MENU_AMIIBO_TAPPED] = "已放上：%s",
    [UI_MENU_AMIIBO_LOAD_FAILED] = "不是 amiibo 转储（NTAG215，540 字节）：%s",
    [UI_MENU_AMIIBO_REMOVED] = "已拿开 amiibo",
    [UI_MENU_AMIIBO_STATUS] = "状态：%s",
    [UI_MENU_MENU_LANG] = "菜单语言",
    [UI_MENU_MENU_LANG_DESC] = "本菜单和性能浮层的语言。",
    [UI_MENU_MENU_LANG_AUTO] = "自动（跟随 Windows）",
    [UI_MENU_ADV_WARNING] = "这些设置用于测试和排查问题，请在了解原因后再修改。",
    [UI_MENU_GROUP_SCHED] = "调度",
    [UI_MENU_GROUP_SHADERS] = "着色器和管线",
    [UI_MENU_GROUP_TEXMEM] = "纹理和内存",
    [UI_MENU_GROUP_FIXES] = "渲染修正",
    [UI_MENU_GROUP_VULKAN] = "Vulkan",
    [UI_MENU_GROUP_DIAG] = "诊断",
    [UI_MENU_ASYNC_PREEMPT] = "虚拟核心抢占",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "允许同一虚拟核心上的一个游戏线程打断另一个。",
    [UI_MENU_PREEMPT_OFF] = "关",
    [UI_MENU_PREEMPT_ON] = "开",
    [UI_MENU_PREEMPT_INHERIT] = "仅继承",
    [UI_MENU_REPLAY_ASYNC] = "GPU 帧准备独立线程",
    [UI_MENU_REPLAY_ASYNC_DESC] = "在单独的线程而不是游戏线程上为 GPU 准备帧。"
                                  "最多增加一帧延迟。",
    [UI_MENU_TIMER_1MS] = "1 ms Windows 计时器",
    [UI_MENU_TIMER_1MS_DESC] = "将系统计时器精度提高到 1 ms，使帧间隔更平稳。",
    [UI_MENU_PIPE_WAIT] = "等待中绘制的处理",
    [UI_MENU_PIPE_WAIT_DESC] = "默认保留当前调度，并为等待中的绘制预留一个工作线程；"
                               "旧版（绘制）沿用先前的调度。",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "默认",
    [UI_MENU_PIPE_WAIT_DRAW] = "旧版（绘制）",
    [UI_MENU_PIPE_SKIP] = "构建管线时跳过的绘制",
    [UI_MENU_PIPE_SKIP_DESC] = "哪些绘制会被直接丢弃，而不是等待其管线构建完成。",
    [UI_MENU_PIPE_SKIP_STRICT] = "严格",
    [UI_MENU_PIPE_SKIP_ANY] = "任意",
    [UI_MENU_PIPE_SKIP_FULLSCREEN] = "全屏通道",
    [UI_MENU_LIB_PREBUILD] = "阶段库预构建",
    [UI_MENU_LIB_PREBUILD_DESC] = "在后台提前构建着色器阶段库。",
    [UI_MENU_LIB_PREBUILD_THREADS] = "阶段库预构建线程数",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "预构建使用的线程数；自动会根据你的 CPU 选择。",
    [UI_MENU_PIPE_PREBUILD] = "管线预构建",
    [UI_MENU_PIPE_PREBUILD_DESC] = "提前构建已知的管线。",
    [UI_MENU_PREWARM_XLATE] = "着色器转译预热",
    [UI_MENU_PREWARM_XLATE_DESC] = "启动时转译已知着色器的线程数；自动取 CPU 线程数的一半，"
                                   "最多 16。",
    [UI_MENU_GPL] = "图形管线库",
    [UI_MENU_GPL_DESC] = "使用 Vulkan 管线库扩展；驱动对此有问题时请关闭。",
    [UI_MENU_ASTC] = "ASTC 纹理解码",
    [UI_MENU_ASTC_DESC] = "压缩的 ASTC 纹理在哪里解码。",
    [UI_MENU_ASTC_AUTO] = "自动",
    [UI_MENU_ASTC_GPU] = "GPU 着色器",
    [UI_MENU_ASTC_NATIVE] = "原生",
    [UI_MENU_ASTC_CPU] = "CPU",
    [UI_MENU_MEMORY] = "GPU 内存模式",
    [UI_MENU_MEMORY_DESC] = "游戏内存如何与 GPU 共享。",
    [UI_MENU_MEMORY_AUTO] = "自动",
    [UI_MENU_MEMORY_IMPORT] = "导入",
    [UI_MENU_MEMORY_MIRROR] = "镜像",
    [UI_MENU_MEMORY_VERIFY] = "校验",
    [UI_MENU_TEX_ALIAS] = "纹理别名",
    [UI_MENU_TEX_ALIAS_DESC] = "在仍在使用的纹理内存上新建纹理时，新纹理沿用原有内容，"
                               "而不是从空白开始。",
    [UI_MENU_TEX_VRAM_PRESSURE] = "纹理显存上限",
    [UI_MENU_TEX_VRAM_PRESSURE_DESC] = "显存占用超过这个百分比后，游戏已释放的纹理会被丢弃，"
                                       "再次用到时从存储重新加载。0 表示不丢弃。",
    [UI_MENU_ASTC_BC] = "ASTC 压缩为 BC3",
    [UI_MENU_ASTC_BC_DESC] = "配合 GPU 解码器使用：把解码后的 ASTC 纹理重新编码为 BC3，"
                             "显存占用只有四分之一。适用于 4 GB 及以下的显卡；"
                             "纹理会出现轻微色块。",
    [UI_MENU_LOD_GUARD] = "LOD 保护",
    [UI_MENU_LOD_GUARD_DESC] = "等待游戏改写完细节层次数据后再让 GPU 读取，"
                               "避免物体错位闪现，或远景有一帧变平。",
    [UI_MENU_FIX_C0] = "常量缓冲区修正",
    [UI_MENU_FIX_C0_DESC] = "填充驱动保留的常量组 c0（存储缓冲区大小、派发大小、点大小、"
                            "基础实例），而不是读取零值。",
    [UI_MENU_FIX_ZERO] = "清零新的 GPU 内存",
    [UI_MENU_FIX_ZERO_DESC] = "新的 GPU 内存池初始内容为零，与 Switch 内存一致。",
    [UI_MENU_BIND_SKIP] = "跳过重复绑定",
    [UI_MENU_BIND_SKIP_DESC] = "优化：跳过没有变化的绑定。",
    [UI_MENU_SHADER_MEMO] = "着色器查找缓存",
    [UI_MENU_SHADER_MEMO_DESC] = "优化：记住着色器的查找结果。",
    [UI_MENU_PIPE_MEMO] = "管线缓存",
    [UI_MENU_PIPE_MEMO_DESC] = "优化：状态重复时复用上一次绘制的管线。",
    [UI_MENU_TEX_MEMO] = "纹理绑定缓存",
    [UI_MENU_TEX_MEMO_DESC] = "优化：在多次绘制之间记住纹理描述符。",
    [UI_MENU_DESC_TEMPLATE] = "描述符模板",
    [UI_MENU_DESC_TEMPLATE_DESC] = "优化：批量更新描述符。",
    [UI_MENU_PUSH_SKIP] = "跳过重复推送常量",
    [UI_MENU_PUSH_SKIP_DESC] = "优化：跳过没有变化的推送常量。",
    [UI_MENU_SPLIT_DRAWS] = "提前提交 GPU 工作",
    [UI_MENU_SPLIT_DRAWS_DESC] = "在渲染通道之间每隔 N 次绘制分批提交 GPU 工作，让 GPU 更早开始；"
                                 "0 为关闭。",
    [UI_MENU_VK_VALIDATION] = "验证层",
    [UI_MENU_VK_VALIDATION_DESC] = "检查每一次 Vulkan 调用，会大幅拖慢游戏。",
    [UI_MENU_VK_VALIDATION_MISSING] = "未安装验证层。",
    [UI_MENU_VK_DYN_VERTEX] = "动态顶点状态",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "强制开关动态顶点输入；默认值取决于显卡。",
    [UI_MENU_VK_DYN_A2C] = "动态 Alpha 覆盖",
    [UI_MENU_VK_DYN_A2C_DESC] = "强制开关动态 Alpha 覆盖。默认：AMD RDNA 2 及更早的驱动上为关闭，"
                                "开启会导致人物变黑；其他显卡上为开启。",
    [UI_MENU_VK_SUBGROUP_PIN] = "固定子群大小",
    [UI_MENU_VK_SUBGROUP_PIN_DESC] = "固定着色器的子群大小。",
    [UI_MENU_TRACE] = "性能日志",
    [UI_MENU_TRACE_DESC] = "把每帧耗时写入文件。",
    [UI_MENU_FLIGHT] = "飞行记录仪",
    [UI_MENU_FLIGHT_DESC] = "保留一小段历史记录，在画面卡住时保存。",
    [UI_MENU_FLIGHT_SPIKE_MS] = "卡顿阈值（ms）",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "算作卡顿的帧时间。",
    [UI_MENU_INPUT_RECORD] = "输入录制",
    [UI_MENU_INPUT_RECORD_DESC] = "把你的输入写入文件，用于反馈问题。",
    [UI_MENU_OPEN_LOG] = "打开日志文件夹",
    [UI_MENU_OPEN_LOG_DESC] = "显示存放日志文件的文件夹。",
    [UI_MENU_COPY_INFO] = "复制系统信息",
    [UI_MENU_COPY_INFO_DESC] = "把显卡、驱动、CPU、内存和改动过的设置复制到剪贴板。",
    [UI_MENU_COPY_INFO_DONE] = "已复制",
    [UI_MENU_ACTION_FAILED] = "操作失败",
    [UI_MENU_RESET] = "将所有设置恢复为默认值",
    [UI_MENU_RESET_DESC] = "对所有标签页生效。",
    [UI_MENU_RESET_CONFIRM] = "要将所有设置恢复为默认值吗？",
    [UI_MENU_RESET_YES] = "重置",
    [UI_MENU_RESET_NO] = "取消",
    [UI_MENU_OV_FPS] = "%.0f FPS",
    [UI_MENU_OV_FRAME_MS] = "%.1f ms",
    [UI_MENU_OV_GAME_PCT] = "游戏 %.0f%%",
    [UI_MENU_OV_MAX_MS] = "最长 %.1f ms",
    [UI_MENU_OV_GPU] = "GPU",
    [UI_MENU_OV_CPU] = "CPU",
    [UI_MENU_OV_RAM] = "内存",
    [UI_MENU_OV_VRAM] = "显存",
    [UI_MENU_OV_TEMP] = "温度",
    [UI_MENU_OV_POWER] = "功耗",
    [UI_MENU_OV_CLOCK] = "频率",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "GPU 帧准备",
    [UI_MENU_OV_GPU_TIME] = "每帧 GPU 忙碌时间",
    [UI_MENU_OV_PIPE_WAIT] = "管线等待",
    [UI_MENU_OV_SHADER_WAIT] = "着色器等待",
    [UI_MENU_OV_TEX_UPLOADS] = "纹理上传",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%",
    [UI_MENU_OV_FMT_TEMP] = "%.0f °C",
    [UI_MENU_OV_FMT_POWER] = "%.0f W",
    [UI_MENU_OV_FMT_MHZ] = "%.0f MHz",
    [UI_MENU_OV_FMT_GB] = "%.1f GB",
    [UI_MENU_OV_FMT_GB_OF] = "/ %.1f GB",
    [UI_MENU_OV_GAME_GB] = "游戏 %.1f GB",
    [UI_MENU_OV_FMT_MS] = "%.2f ms",
    [UI_MENU_OV_PER_FRAME] = "每帧 %.1f",
    [UI_MENU_OV_WINDOW] = "窗口",
    [UI_MENU_OV_OUTPUT] = "游戏画面",
    [UI_MENU_OV_SCENE] = "3D 场景",
    [UI_MENU_OV_SCALING] = "缩放",
    [UI_MENU_OV_LIMIT] = "帧率上限",
    [UI_MENU_OV_PRESENT] = "呈现模式",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u",
    [UI_MENU_OV_FMT_SCENE] = "%ux%u，%.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1，锐度 %d%%",
    [UI_MENU_OV_SCALE_NONE] = "无（1:1）",
    [UI_MENU_OV_SCALE_BILINEAR] = "双线性",
    [UI_MENU_SHADOW_RES] = "阴影贴图尺寸",
    [UI_MENU_SHADOW_RES_DESC] = "太阳阴影贴图的尺寸。游戏自身的尺寸随视角变化，靠近地面时降到约 400，因此树叶阴影显得块状并且跳动；固定尺寸能让阴影稳定、更清晰。高于 1024 时阴影会占用更多 "
                                "GPU 时间和显存。",
    [UI_MENU_SHADOW_RES_GAME] = "游戏默认（会变化）",
    [UI_MENU_ANISO] = "纹理过滤",
    [UI_MENU_ANISO_DESC] = "表面纹理的各向异性过滤：以小角度看到的地面、道路和墙壁在远处依然清晰，而不会模糊。游戏：使用游戏请求的级别，与 Switch 上相同。",
    [UI_MENU_ANISO_GAME] = "游戏",
    [UI_MENU_HDR_GROUP] = "HDR",
    [UI_MENU_HDR] = "HDR 输出",
    [UI_MENU_HDR_DESC] = "在 HDR 显示器上呈现明亮的高光。需要在 Windows 中开启 HDR（“使用 HDR”），或使用带色彩管理的 Wayland 合成器；否则画面保持 "
                         "SDR。重启后生效。上方的色彩配置用于 SDR，在 HDR 下不使用。",
    [UI_MENU_HDR_PEAK] = "峰值亮度",
    [UI_MENU_HDR_PEAK_DESC] = "显示器的峰值亮度（尼特）；最亮的高光止于此。",
    [UI_MENU_HDR_GAME] = "游戏亮度",
    [UI_MENU_HDR_GAME_DESC] = "画面白色（纸白）的尼特值。203 为 HDR 参考值；在明亮的房间里可调高。",
    [UI_MENU_HDR_UI] = "HUD 和菜单亮度",
    [UI_MENU_HDR_UI_DESC] = "HUD 和本菜单白色的尼特值。",
    [UI_MENU_PROMPTS] = "按键提示",
    [UI_MENU_PROMPTS_DESC] = "游戏的按键提示显示哪种手柄的按键。Nintendo：游戏原有的提示。Xbox：LB、RB、LT、RT、Menu、View 和十字键，以及上方布局所按下的按键上标注的 "
                             "A、B、X、Y（使用 Nintendo（A 在右）时右侧按键标为 B，因此 A 的提示显示 "
                             "B）。游戏在启动时读取按键提示，因此重启后才会改变。提示属于游戏文件，在“操作”中重新映射的按键在屏幕上仍显示原来的提示。",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo",
    [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_MISSING] = "缺少按键提示文件：请重新运行安装程序",
    [UI_MENU_PROMPTS_FAILED] = "按键提示文件已损坏：请重新运行安装程序",
    [UI_MENU_PROMPTS_RESTART] = "按键提示重启后生效",
    [UI_MENU_PROMPTS_OFF_SHAPE] = "游戏分辨率不是 16:9 时关闭",
    [UI_MENU_PROMPTS_OFF_MODS] = "已关闭：RomFS 模组替换了它们的文件",
    [UI_MENU_PROMPTS_OFF_PATCHES] = "设置了 TOTK_ROMFS_PATCHES 时关闭",
    [UI_MENU_RESTART] = "立即重启",
    [UI_MENU_RESTART_DESC] = "关闭游戏并以新的按键提示重新启动。上次存档之后的进度将会丢失。",
    [UI_MENU_RESTART_CONFIRM] = "现在重启游戏？上次存档之后的进度将会丢失。",
    [UI_MENU_RESTART_YES] = "重启",
    [UI_MENU_RESTART_SAVING] = "游戏正在存档：稍后即可重启。",
    [UI_MENU_STICK_SENS] = "摇杆镜头速度",
    [UI_MENU_STICK_SENS_DESC] = "在游戏的五档镜头灵敏度下，按倍数调整右摇杆的镜头转动速度（包括最高速度）。鼠标使用自己的设置。",
    [UI_MENU_WALK_KEY] = "步行键",
    [UI_MENU_WALK_KEY_DESC] = "按住时键盘模拟的摇杆只推到一部分，使键盘操作走路而不是奔跑。推动幅度由步行幅度决定。手柄和鼠标不受影响。",
    [UI_MENU_WALK_KEY_TAKEN] = "同时为 %s",
    [UI_MENU_WALK_SCALE] = "步行幅度",
    [UI_MENU_WALK_SCALE_DESC] = "按住步行键时键盘摇杆的推动幅度。一半为走路，推满为奔跑。",
    [UI_MENU_OV_SCALE_DOWN] = "双线性（FSR 只能放大）",
    [UI_MENU_SHADOW_SCALE] = "阴影分辨率",
    [UI_MENU_SHADOW_SCALE_DESC] = "将太阳阴影贴图的每条边绘制为游戏 960x960 的若干倍：阴影边缘和树叶阴影更清晰。下次启动时生效。",
    [UI_MENU_SHADOW_RANGE] = "阴影级联范围",
    [UI_MENU_SHADOW_RANGE_DESC] = "把阴影变柔和的分界（游戏中为 20、40、80 和 160 米）推得更远。阴影贴图会覆盖更大的地面，请同时提高阴影分辨率。下次启动时生效。",
    [UI_MENU_CONTACT_SHADOWS] = "接触阴影",
    [UI_MENU_CONTACT_SHADOWS_DESC] = "阴影贴图遗漏的细小太阳阴影：脚下、石头下、草丛和褶皱中。在光照之后于屏幕空间追踪。下次启动时生效。",
    [UI_MENU_PCSS] = "柔和阴影",
    [UI_MENU_PCSS_DESC] = "物体接触地面处的太阳阴影锐利，阴影投得越远越柔和（PCSS）。0 保留游戏自身的过滤。下次启动时生效。",
    [UI_MENU_DLAA] = "NVIDIA DLSS / DLAA",
    [UI_MENU_DLAA_DESC] = "用于 3D 场景的 NVIDIA DLSS（NVIDIA RTX 显卡；nvngx_dlss.dll 放在 totk.exe 旁）：边缘更平滑，闪烁更少。HUD "
                          "不受影响。下方的模式决定它的工作分辨率。重启后生效。",
    [UI_MENU_DLSS] = "DLSS 模式",
    [UI_MENU_DLSS_DESC] = "DLAA 以全分辨率绘制 3D 场景。质量、平衡和性能分别以 67%、58% 和 50% 绘制，再由 DLSS 放大，以获得更高帧率；它们会取代 3D 渲染比例。重启后生效。",
    [UI_MENU_DLSS_DLAA] = "DLAA（原生）",
    [UI_MENU_DLSS_QUALITY] = "质量",
    [UI_MENU_DLSS_BALANCED] = "平衡",
    [UI_MENU_DLSS_PERFORMANCE] = "性能",
    [UI_MENU_FXAA] = "游戏抗锯齿（FXAA）",
    [UI_MENU_FXAA_DESC] = "游戏自身的 FXAA 边缘平滑。会让细节略微模糊；开启 DLAA 时不需要。",
    [UI_MENU_TAB_CHEATS] = "金手指",
    [UI_MENU_CHEATS_NONE] = "此游戏版本没有金手指。模组的金手指是它的 cheats 文件夹中以游戏构建命名的 Atmosphere "
                            "金手指文件：mods\\<模组>\\cheats\\%s。重启游戏后才会读取。对于修改游戏代码的金手指，"
                            "游戏会在启动时自行重新构建（几分钟）。",
    [UI_MENU_CHEATS_MOD] = "%s：%d 个金手指",
    [UI_MENU_CHEAT_CODE_DESC] = "修改游戏代码。立即开关。",
    [UI_MENU_CHEAT_KEY_DESC] = "按住 %s 时修改游戏代码。立即开关。",
    [UI_MENU_CHEAT_DATA_DESC] = "开启时每帧写入游戏内存。",
    [UI_MENU_CHEAT_MASTER] = "始终开启",
    [UI_MENU_CHEAT_MASTER_DESC] = "文件的主代码：始终开启。",
    [UI_MENU_CHEAT_RESTORE] = "无需使用",
    [UI_MENU_CHEAT_RESTORE_DESC] = "它只会写回游戏原本的代码。关闭金手指时已经会这样做。",
    [UI_MENU_CHEAT_REBUILD] = "重新构建后",
    [UI_MENU_CHEAT_REBUILD_DESC] = "此金手指修改游戏代码，并且是在游戏构建后新增或修改的。游戏会在下次启动时把它构建进去（几分钟），也可以使用“立即重新构建游戏”。",
    [UI_MENU_CHEAT_UNSUPPORTED] = "不支持",
    [UI_MENU_CHEAT_UNSUPPORTED_DESC] = "此金手指不能用于此游戏版本：",
    [UI_MENU_CHEATS_REBUILD] = "立即重新构建游戏",
    [UI_MENU_CHEATS_REBUILD_DESC] = "有些金手指会修改游戏代码，但尚未构建进游戏：它们是新增或修改的，或上次重新构建失败或被取消。将关闭游戏；安装程序会把它们构建进去（几分钟）并重新启动游戏。上次存档之后的进度会丢失。",
    [UI_MENU_MODS_EXEFS_UNSUPPORTED] = "此模组修改游戏代码（exefs）的方式本移植无法应用，因此整个模组保持关闭。%s",
    [UI_MENU_MODS_EXEFS_OTHER] = "它的代码补丁（exefs）是为其他游戏版本制作的。",
    [UI_MENU_MODS_EXEFS_ON] = "正在使用的代码补丁：%d 个文件。",
    [UI_MENU_MODS_REPLACES_BUILTIN] = "正在使用的代码补丁：%d 个文件。它们取代本移植自带的补丁 %s，此模组开启时该补丁保持关闭。",
};

#define SWKBD_ONLY(name, ...) static const char *const name[UI_TEXT_COUNT] = {SWKBD(__VA_ARGS__)}
SWKBD_ONLY(text_ja, "文字入力", "名前を入力してください", "ウマ", "OK", "キャンセル", "コントローラー: A = OK、B = キャンセル");
SWKBD_ONLY(text_fr, "Saisie de texte", "Entrez un nom", "Cheval", "OK", "Annuler", "Manette : A = OK, B = Annuler");
SWKBD_ONLY(text_de, "Texteingabe", "Gib einen Namen ein", "Pferd", "OK", "Abbrechen", "Controller: A = OK, B = Abbrechen");
SWKBD_ONLY(text_it, "Inserimento testo", "Inserisci un nome", "Cavallo", "OK", "Annulla", "Controller: A = OK, B = Annulla");
SWKBD_ONLY(text_es, "Introducir texto", "Introduce un nombre", "Caballo", "Aceptar", "Cancelar",
           "Mando: A = Aceptar, B = Cancelar");
SWKBD_ONLY(text_nl, "Tekstinvoer", "Voer een naam in", "Paard", "OK", "Annuleren", "Controller: A = OK, B = Annuleren");
SWKBD_ONLY(text_pt, "Inserir texto", "Introduz um nome", "Cavalo", "OK", "Cancelar", "Comando: A = OK, B = Cancelar");
SWKBD_ONLY(text_zh_hant, "文字輸入", "請輸入名字", "馬兒", "確定", "取消", "手把：A 確定，B 取消");
SWKBD_ONLY(text_ko, "문자 입력", "이름을 입력하세요", "말", "확인", "취소", "컨트롤러: A 확인, B 취소");

static const Row rows[] = {
    {"en", text_en}, {"ja", text_ja}, {"fr", text_fr}, {"de", text_de}, {"it", text_it}, {"es", text_es},
    {"nl", text_nl}, {"pt", text_pt}, {"ru", text_ru}, {"zh-Hans", text_zh_hans}, {"zh-Hant", text_zh_hant},
    {"ko", text_ko}, {"pt-BR", text_pt_br},
};

const char *ui_text_lang(const char *code) {
    static const struct { const char *code, *lang; } map[] = {
        {"en-US", "en"}, {"en-GB", "en"}, {"fr-CA", "fr"}, {"es-419", "es"},
        {"zh-CN", "zh-Hans"}, {"zh-TW", "zh-Hant"},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(code, map[i].code)) return map[i].lang;
    return code;
}

static const Row *find_row(const char *lang) {
    for (size_t i = 0; i < sizeof rows / sizeof *rows; i++)
        if (!strcmp(rows[i].lang, lang)) return &rows[i];
    return NULL;
}

int ui_text_defined(const char *lang, UiTextId id) {
    const Row *r = find_row(lang);
    return (unsigned)id < UI_TEXT_COUNT && r && r->text[id] && *r->text[id];
}

const char *ui_text_in(const char *lang, UiTextId id) {
    if ((unsigned)id >= UI_TEXT_COUNT) return "";
    return ui_text_defined(lang, id) ? find_row(lang)->text[id] : rows[0].text[id];
}

const char *ui_text(UiTextId id) { return ui_text_in(ui_text_lang(ov_language()), id); }

// A language's own strings, for the font that draws them: the merged system font must cover the whole menu of the
// language, not only its name (ui/menu.cpp fonts_up).
void ui_text_visit(const char *lang, void (*visit)(void *ctx, const char *text), void *ctx) {
    const Row *r = find_row(lang);
    if (!r) return;
    for (int id = 0; id < (int)UI_TEXT_COUNT; id++)
        if (r->text[id] && *r->text[id]) visit(ctx, r->text[id]);
}

// The menu language, re-read when cfg_version changes. TOTK_MENU_LANG names a catalog language ("en", "ru", "pt-BR",
// "zh-Hans"); unset, it follows the system UI language, through the same code map the game language uses ("zh-CN" ->
// "zh-Hans"). Windows names a region with it ("ru-RU"), so a tag without menu text tries its language alone ("ru"). A
// language whose row has no menu text (only the software keyboard does) shows English, so other Windows locales keep the
// English menu.
static const char *resolve_menu_lang(void) {
    const char *v = cfg_value("TOTK_MENU_LANG");
    const char *sys = plat_ui_language();
    const char *lang = v && *v ? ui_text_lang(v) : sys && *sys ? ui_text_lang(sys) : "en";
    if (ui_text_defined(lang, UI_MENU_TITLE)) return lang;
    static char base[16];   // menu_text calls this on one thread at a time, on a cfg_version change
    size_t n = strcspn(lang, "-");
    if (n < sizeof base && lang[n]) {
        memcpy(base, lang, n), base[n] = 0;
        if (ui_text_defined(base, UI_MENU_TITLE)) return find_row(base)->lang;
    }
    return "en";
}

const char *menu_text(UiTextId id) {
    static _Atomic(const char *) lang = "en";
    static _Atomic unsigned seen = ~0u;
    unsigned v = cfg_version();
    if (v != atomic_load_explicit(&seen, memory_order_relaxed)) {
        atomic_store_explicit(&lang, resolve_menu_lang(), memory_order_relaxed);
        atomic_store_explicit(&seen, v, memory_order_relaxed);
    }
    return ui_text_in(atomic_load_explicit(&lang, memory_order_relaxed), id);
}
