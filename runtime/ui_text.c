// Catalog of the text the host shows the player (ui_text.h). One array per catalog language, indexed by UiTextId: a new
// string gets an id in ui_text.h and an entry in every language that has one (a missing entry falls back to English).
// The software keyboard has every language; the menu has English, Russian and Simplified Chinese.
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
    [UI_MENU_LANGNAME_ZH_HANS] = "简体中文", [UI_MENU_LANGNAME_ZH_HANT] = "繁體中文", [UI_MENU_LANGNAME_KO] = "한국어"

// SWKBD: TITLE, PROMPT, DEFAULT_NAME (at most 9 UTF-16 units, no '@' '%' '\'), OK, CANCEL, PAD_HINT.
#define SWKBD(title, prompt, name, ok, cancel, hint) \
    [UI_SWKBD_TITLE] = title, [UI_SWKBD_PROMPT] = prompt, [UI_SWKBD_DEFAULT_NAME] = name, [UI_SWKBD_OK] = ok, \
    [UI_SWKBD_CANCEL] = cancel, [UI_SWKBD_PAD_HINT] = hint, LANG_NAMES

static const char *const text_en[UI_TEXT_COUNT] = {
    SWKBD("Text entry", "Enter a name", "Horse", "OK", "Cancel", "Controller: A = OK, B = Cancel"),

    [UI_MENU_TITLE] = "Settings",
    [UI_MENU_TAB_SCREEN] = "Screen", [UI_MENU_TAB_GAME] = "Game", [UI_MENU_TAB_SOUND] = "Sound",
    [UI_MENU_TAB_CONTROLS] = "Controls", [UI_MENU_TAB_MENU] = "Menu", [UI_MENU_TAB_ADVANCED] = "Advanced",
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
                                "the HUD to the screen's corners.",
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
    [UI_MENU_SHARPNESS] = "FSR sharpness",
    [UI_MENU_SHARPNESS_DESC] = "Sharpening after FSR scaling; 0 turns it off.",

    [UI_MENU_FPS_LIMIT] = "Frame rate limit",
    [UI_MENU_FPS_LIMIT_DESC] = "The most frames per second the game draws. Above 60 is experimental and needs Game speed "
                               "follows the clock.",
    [UI_MENU_FPS_30] = "30 FPS", [UI_MENU_FPS_60] = "60 FPS", [UI_MENU_FPS_90] = "90 FPS", [UI_MENU_FPS_120] = "120 FPS", [UI_MENU_FPS_144] = "144 FPS",
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
                                "Nintendo for controllers with Nintendo labels and Xbox for the others. Xbox prompts: "
                                "A is the right one as with Nintendo, and the game shows the Xbox labels (B for A, A "
                                "for B, Y for X, X for Y); the prompts change after a restart. The letters are part of the "
                                 "game's files, so a button you remap in Controls keeps the old letter on screen.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Automatic", [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A right)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A bottom)", [UI_MENU_PAD_LAYOUT_XBOX_PROMPTS] = "Xbox prompts (A right)",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "in use: %s",
    [UI_MENU_PAD_LAYOUT_PROMPTS_MISSING] = "prompt files missing: run the setup again",
    [UI_MENU_PAD_LAYOUT_PROMPTS_RESTART] = "prompts change after restart",
    [UI_MENU_STICK_DEADZONE] = "Stick dead zone",
    [UI_MENU_STICK_DEADZONE_DESC] = "How far a gamepad stick moves before the game sees it. Raise it if the character or the camera moves on its own.",
    [UI_MENU_MOUSE_SENS] = "Mouse camera sensitivity",
    [UI_MENU_MOUSE_SENS_DESC] = "How fast the mouse turns the camera.",

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
    [UI_MENU_FREE_CORES] = "Virtual core gate",
    [UI_MENU_FREE_CORES_DESC] = "Lets only one thread run per virtual core, as on the console.",
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
};

static const char *const text_ru[UI_TEXT_COUNT] = {
    SWKBD("Ввод текста", "Введите имя", "Лошадь", "ОК", "Отмена", "Контроллер: A — ОК, B — отмена"),

    [UI_MENU_TITLE] = "Настройки",
    [UI_MENU_TAB_SCREEN] = "Экран", [UI_MENU_TAB_GAME] = "Игра", [UI_MENU_TAB_SOUND] = "Звук",
    [UI_MENU_TAB_CONTROLS] = "Управление", [UI_MENU_TAB_MENU] = "Меню", [UI_MENU_TAB_ADVANCED] = "Дополнительно",
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
                                "переносит интерфейс в углы экрана.",
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
    [UI_MENU_SHARPNESS] = "Резкость FSR",
    [UI_MENU_SHARPNESS_DESC] = "Повышение резкости после масштабирования FSR; 0 выключает его.",

    [UI_MENU_FPS_LIMIT] = "Ограничение частоты кадров",
    [UI_MENU_FPS_LIMIT_DESC] = "Наибольшее число кадров в секунду. Выше 60 — экспериментальный режим, нужна «Скорость "
                               "игры по часам».",
    [UI_MENU_FPS_30] = "30 FPS", [UI_MENU_FPS_60] = "60 FPS", [UI_MENU_FPS_90] = "90 FPS", [UI_MENU_FPS_120] = "120 FPS", [UI_MENU_FPS_144] = "144 FPS",
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
                                "Nintendo для контроллеров с подписями Nintendo, Xbox для остальных. Подсказки Xbox: "
                                "A справа, как в Nintendo, а игра показывает надписи Xbox (B вместо A, A вместо B, Y "
                                "вместо X, X вместо Y); подсказки меняются после перезапуска. Буквы записаны в файлах "
                                 "игры, поэтому у кнопки, переназначенной на вкладке управления, на экране остаётся "
                                 "прежняя буква.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Автоматически", [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A справа)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A снизу)", [UI_MENU_PAD_LAYOUT_XBOX_PROMPTS] = "Подсказки Xbox (A справа)",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "сейчас: %s",
    [UI_MENU_PAD_LAYOUT_PROMPTS_MISSING] = "нет файлов подсказок: запустите установку снова",
    [UI_MENU_PAD_LAYOUT_PROMPTS_RESTART] = "подсказки сменятся после перезапуска",
    [UI_MENU_STICK_DEADZONE] = "Мёртвая зона стиков",
    [UI_MENU_STICK_DEADZONE_DESC] = "Насколько нужно отклонить стик геймпада, чтобы игра это заметила. Увеличьте, если персонаж или камера двигаются сами.",
    [UI_MENU_MOUSE_SENS] = "Чувствительность мыши",
    [UI_MENU_MOUSE_SENS_DESC] = "Как быстро мышь поворачивает камеру.",

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
    [UI_MENU_FREE_CORES] = "Ограничение виртуальных ядер",
    [UI_MENU_FREE_CORES_DESC] = "На каждом виртуальном ядре работает один поток, как на консоли.",
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
};

static const char *const text_zh_hans[UI_TEXT_COUNT] = {
    SWKBD("文字输入", "请输入名字", "马儿", "确定", "取消", "手柄：A 确定，B 取消"),

    [UI_MENU_TITLE] = "设置", [UI_MENU_TAB_SCREEN] = "显示", [UI_MENU_TAB_GAME] = "游戏", [UI_MENU_TAB_SOUND] = "声音",
    [UI_MENU_TAB_CONTROLS] = "操作", [UI_MENU_TAB_MENU] = "菜单", [UI_MENU_TAB_ADVANCED] = "高级",
    [UI_MENU_CONTINUE] = "继续", [UI_MENU_QUIT] = "退出游戏", [UI_MENU_ON] = "开", [UI_MENU_OFF] = "关",
    [UI_MENU_AUTO] = "自动", [UI_MENU_DEFAULT_MARK] = "默认", [UI_MENU_AFTER_RESTART] = "重启后生效",
    [UI_MENU_SET_BY_ENV] = "由环境变量设定", [UI_MENU_SAVE_FAILED] = "无法保存设置",
    [UI_MENU_VERSION] = "TotK Recomp %s，游戏 %s",

    [UI_MENU_FULLSCREEN] = "全屏", [UI_MENU_FULLSCREEN_DESC] = "让游戏画面铺满屏幕；按 F11 切换。",
    [UI_MENU_WINDOW_SIZE] = "窗口大小", [UI_MENU_WINDOW_SIZE_DESC] = "非全屏时游戏窗口的大小。",
    [UI_MENU_WINDOW_SIZE_CUSTOM] = "自定义", [UI_MENU_PRESENT] = "呈现方式",
    [UI_MENU_PRESENT_DESC] = "渲染完成的帧如何显示到屏幕上。", [UI_MENU_PRESENT_MAILBOX] = "Mailbox",
    [UI_MENU_PRESENT_VSYNC] = "垂直同步", [UI_MENU_PRESENT_IMMEDIATE] = "立即",
    [UI_MENU_PRESENT_IN_USE] = "实际使用：%s", [UI_MENU_GPU] = "显卡",
    [UI_MENU_GPU_DESC] = "用于渲染游戏的 Vulkan 设备。", [UI_MENU_GPU_AUTO] = "自动",
    [UI_MENU_PERF_OVERLAY] = "性能叠加层",
    [UI_MENU_PERF_OVERLAY_DESC] = "在右上角显示帧率。“完整”模式还会显示帧时间曲线、CPU、GPU 和内存占用、温度，"
                                  "以及渲染线程的等待时间。按 F10 循环切换模式。",
    [UI_MENU_PERF_OFF] = "关", [UI_MENU_PERF_FPS] = "帧率", [UI_MENU_PERF_FULL] = "完整",
    [UI_PERF_LINE_FPS] = "%.0f FPS", [UI_MENU_GRAPHICS] = "画面", [UI_MENU_OUTPUT_RES] = "游戏分辨率",
    [UI_MENU_OUTPUT_RES_DESC] = "游戏绘制画面和界面所用的尺寸。高于 1920 x 1080 时，游戏需要更多显存和 GPU 时间。"
                                "更宽的尺寸（21:9）会拓宽镜头视野，并把 HUD 移到屏幕四角。",
    [UI_MENU_RENDER_SCALE] = "3D 渲染比例",
    [UI_MENU_RENDER_SCALE_DESC] = "3D 场景相对于游戏分辨率的大小；游戏自带的 FSR 会把它缩放到游戏分辨率。高于 100 % "
                                  "时可平滑锯齿边缘（超级采样），但 GPU 开销很大。",
    [UI_MENU_RENDER_SCALE_GAME] = "游戏默认（83 %）", [UI_MENU_UPSCALER] = "缩放到窗口",
    [UI_MENU_UPSCALER_DESC] = "窗口大于游戏分辨率时画面的缩放方式。FSR 比双线性保留更锐利的边缘和细节。",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1", [UI_MENU_UPSCALER_BILINEAR] = "双线性", [UI_MENU_ASPECT] = "画面比例",
    [UI_MENU_ASPECT_DESC] = "窗口的比例与游戏分辨率不同时（例如 21:9 屏幕上的 16:9 画面）：保持画面比例并加黑边，"
                            "或拉伸铺满窗口。",
    [UI_MENU_ASPECT_FIT] = "黑边", [UI_MENU_ASPECT_STRETCH] = "拉伸", [UI_MENU_SHARPNESS] = "FSR 锐度",
    [UI_MENU_SHARPNESS_DESC] = "FSR 缩放后的锐化强度；0 为关闭。",

    [UI_MENU_FPS_LIMIT] = "帧率上限",
    [UI_MENU_FPS_LIMIT_DESC] = "游戏每秒最多绘制的帧数。高于 60 为实验性功能，且需要开启“游戏速度跟随时钟”。",
    [UI_MENU_FPS_30] = "30 FPS", [UI_MENU_FPS_60] = "60 FPS", [UI_MENU_FPS_90] = "90 FPS",
    [UI_MENU_FPS_120] = "120 FPS", [UI_MENU_FPS_144] = "144 FPS", [UI_MENU_FPS_DISPLAY] = "显示器刷新率",
    [UI_MENU_FPS_DISPLAY_HZ] = "显示器刷新率（%.0f Hz）", [UI_MENU_REALTIME] = "游戏速度跟随时钟",
    [UI_MENU_REALTIME_DESC] = "按真实时间推进游戏，帧变慢时游戏不会随之变慢。",
    [UI_MENU_CLOTH_REALTIME] = "布料跟随时钟", [UI_MENU_CLOTH_REALTIME_DESC] = "布料和头发也按真实时间推进。",
    [UI_MENU_HIGHFPS_FIX] = "高帧率修正",
    [UI_MENU_HIGHFPS_FIX_DESC] = "修正帧率高于 30 时出现异常的物理效果，以及帧率高于 60 时一直半透明或不可见的物体。",
    [UI_MENU_GAME_LANG] = "游戏语言", [UI_MENU_GAME_LANG_DESC] = "游戏文本和语音的语言。",
    [UI_MENU_GAME_LANG_AUTO] = "自动", [UI_MENU_OPERATION_MODE] = "运行模式",
    [UI_MENU_OPERATION_MODE_DESC] = "让游戏认为自己处于 TV 模式（插在底座上）还是掌机模式。",
    [UI_MENU_MODE_DOCKED] = "TV 模式", [UI_MENU_MODE_HANDHELD] = "掌机模式",

    [UI_MENU_VOLUME] = "音量", [UI_MENU_VOLUME_DESC] = "游戏的主音量。",

    [UI_MENU_MOTION] = "体感操作", [UI_MENU_MOTION_DESC] = "使用手柄的陀螺仪瞄准。",
    [UI_MENU_RUMBLE_STRENGTH] = "振动强度", [UI_MENU_RUMBLE_STRENGTH_DESC] = "手柄振动的强弱。",
    [UI_MENU_RUMBLE_CURVE] = "振动曲线", [UI_MENU_RUMBLE_CURVE_DESC] = "数值越低，弱振动越明显。",
    [UI_MENU_RUMBLE_TRIGGERS] = "扳机振动（Xbox）", [UI_MENU_RUMBLE_TRIGGERS_DESC] = "同时让 Xbox 手柄的扳机键振动。",
    [UI_MENU_PAD_LAYOUT] = "按键布局",
    [UI_MENU_PAD_LAYOUT_DESC] = "哪个按键作为 A。Nintendo：右侧按键，与 Switch 相同。Xbox：下方按键，即 Xbox "
                                "手柄上标着 A 的键（PlayStation 手柄上为叉键）。自动：带 Nintendo 标注的手柄用 "
                                "Nintendo，其他手柄用 Xbox。Xbox 按键提示：A 与 Nintendo 布局一样在右侧，游戏显示 "
                                "Xbox 的按键标注（A 显示为 B，B 显示为 A，X 显示为 Y，Y 显示为 X）；"
                                "按键提示重启后更改。这些字母属于游戏文件，所以在“操作”中重新映射过的按键，"
                                "屏幕上仍显示原来的字母。",
    [UI_MENU_PAD_LAYOUT_AUTO] = "自动", [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo（A 在右）",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox（A 在下）", [UI_MENU_PAD_LAYOUT_XBOX_PROMPTS] = "Xbox 按键提示（A 在右）",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "实际使用：%s",
    [UI_MENU_PAD_LAYOUT_PROMPTS_MISSING] = "缺少按键提示文件：请重新运行安装程序",
    [UI_MENU_PAD_LAYOUT_PROMPTS_RESTART] = "按键提示重启后更改", [UI_MENU_STICK_DEADZONE] = "摇杆死区",
    [UI_MENU_STICK_DEADZONE_DESC] = "手柄摇杆推动多远后游戏才会识别。如果角色或视角会自行移动，请调高此值。",
    [UI_MENU_MOUSE_SENS] = "鼠标视角灵敏度", [UI_MENU_MOUSE_SENS_DESC] = "鼠标转动视角的速度。",

    [UI_BINDS_HELP] = "选中一个格子并按 A 或 Enter，然后按下键盘按键、鼠标按键或手柄按键。按 Delete 或 X 清空格子。"
                      "已被占用的键盘按键会移到这里；手柄按键则与被替换的按键互换。",
    [UI_BINDS_COL_KEY1] = "按键 1", [UI_BINDS_COL_KEY2] = "按键 2", [UI_BINDS_COL_PAD] = "手柄",
    [UI_BINDS_GROUP_BUTTONS] = "按钮", [UI_BINDS_GROUP_MOVE] = "移动", [UI_BINDS_GROUP_CAMERA] = "视角",
    [UI_BINDS_PRESS_KEY] = "请为“%s”按下键盘按键或鼠标按键", [UI_BINDS_PRESS_PAD] = "请为“%s”按下手柄按键",
    [UI_BINDS_CANCEL_HINT] = "按 Esc 取消，或等待 %d 秒", [UI_BINDS_TAKEN] = "%s 已从“%s”上移走",
    [UI_BINDS_SWAPPED] = "“%s”与“%s”已互换", [UI_BINDS_RESET] = "重置所有按键",
    [UI_BINDS_RESET_DESC] = "恢复默认的键盘按键和手柄按键。上方的按键布局将重新生效：在执行此重置之前，"
                            "重新映射过的手柄按键会一直保持在你设定的位置。",
    [UI_BIND_A] = "A", [UI_BIND_B] = "B", [UI_BIND_X] = "X", [UI_BIND_Y] = "Y", [UI_BIND_LCLICK] = "按下左摇杆",
    [UI_BIND_RCLICK] = "按下右摇杆", [UI_BIND_L] = "L", [UI_BIND_R] = "R", [UI_BIND_ZL] = "ZL", [UI_BIND_ZR] = "ZR",
    [UI_BIND_PLUS] = "+ 键", [UI_BIND_MINUS] = "- 键", [UI_BIND_LEFT] = "十字键左", [UI_BIND_UP] = "十字键上",
    [UI_BIND_RIGHT] = "十字键右", [UI_BIND_DOWN] = "十字键下", [UI_BIND_LSTICK_LEFT] = "向左移动",
    [UI_BIND_LSTICK_UP] = "向前移动", [UI_BIND_LSTICK_RIGHT] = "向右移动", [UI_BIND_LSTICK_DOWN] = "向后移动",
    [UI_BIND_RSTICK_LEFT] = "视角向左", [UI_BIND_RSTICK_UP] = "视角向上", [UI_BIND_RSTICK_RIGHT] = "视角向右",
    [UI_BIND_RSTICK_DOWN] = "视角向下", [UI_BIND_DESC_A] = "调查、拾取、对话。",
    [UI_BIND_DESC_B] = "奔跑、取消、返回。", [UI_BIND_DESC_X] = "跳跃。", [UI_BIND_DESC_Y] = "攻击。",
    [UI_BIND_DESC_LCLICK] = "蹲下。", [UI_BIND_DESC_RCLICK] = "第一人称视角。", [UI_BIND_DESC_L] = "能力。",
    [UI_BIND_DESC_R] = "投掷武器。", [UI_BIND_DESC_ZL] = "举盾、锁定。", [UI_BIND_DESC_ZR] = "弓。",
    [UI_BIND_DESC_PLUS] = "菜单、道具。", [UI_BIND_DESC_MINUS] = "地图。", [UI_BIND_DESC_DPAD] = "快捷选择。",
    [UI_BIND_DESC_MOVE] = "像左摇杆一样移动林克。手柄的左摇杆也始终可用。",
    [UI_BIND_DESC_CAMERA] = "像右摇杆一样转动视角。鼠标和手柄的右摇杆也始终可用。", [UI_KEY_SPACE] = "空格",
    [UI_KEY_TAB] = "Tab", [UI_KEY_LSHIFT] = "左 Shift", [UI_KEY_LCTRL] = "左 Ctrl", [UI_KEY_UP] = "上方向键",
    [UI_KEY_DOWN] = "下方向键", [UI_KEY_LEFT] = "左方向键", [UI_KEY_RIGHT] = "右方向键", [UI_KEY_MOUSE_L] = "鼠标左键",
    [UI_KEY_MOUSE_R] = "鼠标右键", [UI_KEY_MOUSE_M] = "鼠标中键", [UI_KEY_MOUSE_X1] = "鼠标侧键 4",
    [UI_KEY_MOUSE_X2] = "鼠标侧键 5", [UI_KEY_ENTER] = "Enter", [UI_KEY_BACKSPACE] = "Backspace",
    [UI_KEY_RSHIFT] = "右 Shift", [UI_KEY_RCTRL] = "右 Ctrl", [UI_KEY_LALT] = "Alt", [UI_KEY_CAPSLOCK] = "Caps Lock",
    [UI_KEY_INSERT] = "Insert", [UI_KEY_DELETE] = "Delete", [UI_KEY_HOME] = "Home", [UI_KEY_END] = "End",
    [UI_KEY_PAGEUP] = "Page Up", [UI_KEY_PAGEDOWN] = "Page Down", [UI_KEY_NUM_FMT] = "小键盘 %s",
    [UI_PAD_SOUTH] = "下方按键", [UI_PAD_EAST] = "右侧按键", [UI_PAD_WEST] = "左侧按键", [UI_PAD_NORTH] = "上方按键",
    [UI_PAD_LSTICK] = "左摇杆", [UI_PAD_RSTICK] = "右摇杆", [UI_PAD_L] = "左肩键", [UI_PAD_R] = "右肩键",
    [UI_PAD_START] = "Start 键", [UI_PAD_BACK] = "Back 键", [UI_PAD_DPAD_LEFT] = "十字键左",
    [UI_PAD_DPAD_UP] = "十字键上", [UI_PAD_DPAD_RIGHT] = "十字键右", [UI_PAD_DPAD_DOWN] = "十字键下",
    [UI_PAD_ZL] = "左扳机", [UI_PAD_ZR] = "右扳机", [UI_PAD_LPADDLE1] = "左背键", [UI_PAD_RPADDLE1] = "右背键",
    [UI_PAD_LPADDLE2] = "左背键 2", [UI_PAD_RPADDLE2] = "右背键 2", [UI_PAD_CROSS] = "叉键", [UI_PAD_CIRCLE] = "圆圈键",
    [UI_PAD_SQUARE] = "方块键", [UI_PAD_TRIANGLE] = "三角键",

    [UI_MENU_MENU_LANG] = "菜单语言", [UI_MENU_MENU_LANG_DESC] = "本菜单和性能叠加层的语言。",
    [UI_MENU_MENU_LANG_AUTO] = "自动（跟随 Windows）",

    [UI_MENU_ADV_WARNING] = "这些设置用于测试和排查问题。除非清楚修改的原因，否则请勿改动。",
    [UI_MENU_GROUP_SCHED] = "调度", [UI_MENU_GROUP_SHADERS] = "着色器与管线", [UI_MENU_GROUP_TEXMEM] = "纹理与内存",
    [UI_MENU_GROUP_FIXES] = "渲染修正", [UI_MENU_GROUP_VULKAN] = "Vulkan", [UI_MENU_GROUP_DIAG] = "诊断",
    [UI_MENU_ASYNC_PREEMPT] = "虚拟核心抢占",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "允许一个游戏线程打断同一虚拟核心上的另一个线程。", [UI_MENU_PREEMPT_OFF] = "关",
    [UI_MENU_PREEMPT_ON] = "开", [UI_MENU_PREEMPT_INHERIT] = "仅继承",
    [UI_MENU_REPLAY_ASYNC] = "在独立线程上准备 GPU 帧",
    [UI_MENU_REPLAY_ASYNC_DESC] = "在独立线程而不是游戏线程上为 GPU 准备帧。最多增加一帧延迟。",
    [UI_MENU_FREE_CORES] = "虚拟核心闸门", [UI_MENU_FREE_CORES_DESC] = "每个虚拟核心只允许运行一个线程，与主机上相同。",
    [UI_MENU_TIMER_1MS] = "1 ms Windows 计时器",
    [UI_MENU_TIMER_1MS_DESC] = "将系统计时器精度提高到 1 ms，使帧间隔更平稳。",
    [UI_MENU_PIPE_WAIT] = "等待中绘制的处理",
    [UI_MENU_PIPE_WAIT_DESC] = "“默认”保持当前的调度方式，并为等待中的绘制保留一个工作线程；“旧版（draw）”"
                               "使用之前的调度方式。",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "默认", [UI_MENU_PIPE_WAIT_DRAW] = "旧版（draw）",
    [UI_MENU_PIPE_SKIP] = "管线构建期间跳过的绘制",
    [UI_MENU_PIPE_SKIP_DESC] = "哪些绘制会被直接丢弃，而不是等待其管线构建完成。", [UI_MENU_PIPE_SKIP_STRICT] = "严格",
    [UI_MENU_PIPE_SKIP_ANY] = "任意", [UI_MENU_PIPE_SKIP_FULLSCREEN] = "全屏通道",
    [UI_MENU_LIB_PREBUILD] = "阶段库预构建", [UI_MENU_LIB_PREBUILD_DESC] = "在后台提前构建着色器阶段库。",
    [UI_MENU_LIB_PREBUILD_THREADS] = "阶段库预构建线程数",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "预构建使用的线程数；“自动”会根据你的 CPU 选择。",
    [UI_MENU_PIPE_PREBUILD] = "管线预构建", [UI_MENU_PIPE_PREBUILD_DESC] = "提前构建已知的管线。",
    [UI_MENU_PREWARM_XLATE] = "着色器转译预热",
    [UI_MENU_PREWARM_XLATE_DESC] = "启动时转译已知着色器的线程数；“自动”使用 CPU 线程数的一半，最多 16 个。",
    [UI_MENU_GPL] = "图形管线库",
    [UI_MENU_GPL_DESC] = "使用 Vulkan 管线库扩展；如果你的驱动程序在此模式下出现异常，请关闭。",
    [UI_MENU_ASTC] = "ASTC 纹理解码", [UI_MENU_ASTC_DESC] = "压缩的 ASTC 纹理在何处解码。",
    [UI_MENU_ASTC_AUTO] = "自动", [UI_MENU_ASTC_GPU] = "GPU 着色器", [UI_MENU_ASTC_NATIVE] = "原生",
    [UI_MENU_ASTC_CPU] = "CPU", [UI_MENU_MEMORY] = "GPU 内存模式",
    [UI_MENU_MEMORY_DESC] = "游戏内存与 GPU 的共享方式。", [UI_MENU_MEMORY_AUTO] = "自动",
    [UI_MENU_MEMORY_IMPORT] = "导入", [UI_MENU_MEMORY_MIRROR] = "镜像", [UI_MENU_MEMORY_VERIFY] = "校验",
    [UI_MENU_TEX_ALIAS] = "纹理别名",
    [UI_MENU_TEX_ALIAS_DESC] = "在仍在使用的纹理内存上新建纹理时，新纹理沿用原有内容，而不是从空白开始。",
    [UI_MENU_TEX_VRAM_PRESSURE] = "纹理显存上限",
    [UI_MENU_TEX_VRAM_PRESSURE_DESC] = "显存占用超过这个百分比时，丢弃游戏已释放的纹理（再次用到时会从存储重新载入）。"
                                       "0 表示一律保留。",
    [UI_MENU_ASTC_BC] = "ASTC 压缩为 BC3",
    [UI_MENU_ASTC_BC_DESC] = "配合 GPU 解码器使用：把解码后的 ASTC 纹理重新编码为 BC3，显存占用只有四分之一。适合显存 "
                             "4 GB 及以下的显卡；纹理会略显块状。",
    [UI_MENU_LOD_GUARD] = "LOD 保护",
    [UI_MENU_LOD_GUARD_DESC] = "等游戏改写完细节层次（LOD）数据后再让 GPU 读取，防止物体在错误位置闪现，"
                               "或远景在某一帧变平。",
    [UI_MENU_FIX_C0] = "常量缓冲区修正",
    [UI_MENU_FIX_C0_DESC] = "填充驱动保留的常量库 c0（存储大小、调度大小、点大小、基础实例），而不是读到零。",
    [UI_MENU_FIX_ZERO] = "新 GPU 内存清零",
    [UI_MENU_FIX_ZERO_DESC] = "新的 GPU 内存池初始内容为零，与 Switch 内存一致。", [UI_MENU_BIND_SKIP] = "跳过重复绑定",
    [UI_MENU_BIND_SKIP_DESC] = "优化：跳过没有变化的绑定。", [UI_MENU_SHADER_MEMO] = "着色器查找缓存",
    [UI_MENU_SHADER_MEMO_DESC] = "优化：记住着色器的查找结果。", [UI_MENU_PIPE_MEMO] = "管线缓存",
    [UI_MENU_PIPE_MEMO_DESC] = "优化：状态重复时复用上一次绘制的管线。", [UI_MENU_TEX_MEMO] = "纹理绑定缓存",
    [UI_MENU_TEX_MEMO_DESC] = "优化：在多次绘制之间记住纹理描述符。", [UI_MENU_DESC_TEMPLATE] = "描述符模板",
    [UI_MENU_DESC_TEMPLATE_DESC] = "优化：批量更新描述符。", [UI_MENU_PUSH_SKIP] = "跳过重复的推送常量",
    [UI_MENU_PUSH_SKIP_DESC] = "优化：跳过没有变化的推送常量。", [UI_MENU_SPLIT_DRAWS] = "提前提交 GPU 工作",
    [UI_MENU_SPLIT_DRAWS_DESC] = "在渲染通道之间每隔 N 次绘制分批提交 GPU 工作，让 GPU 更早开始；0 为关闭。",
    [UI_MENU_VK_VALIDATION] = "验证层", [UI_MENU_VK_VALIDATION_DESC] = "检查每一次 Vulkan 调用，会让游戏明显变慢。",
    [UI_MENU_VK_VALIDATION_MISSING] = "未安装验证层。", [UI_MENU_VK_DYN_VERTEX] = "动态顶点状态",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "强制开启或关闭动态顶点输入；默认值取决于 GPU。",
    [UI_MENU_VK_SUBGROUP_PIN] = "固定子组大小", [UI_MENU_VK_SUBGROUP_PIN_DESC] = "固定着色器的子组大小。",
    [UI_MENU_TRACE] = "性能日志", [UI_MENU_TRACE_DESC] = "将每帧耗时写入文件。", [UI_MENU_FLIGHT] = "飞行记录器",
    [UI_MENU_FLIGHT_DESC] = "保留一小段近期记录，在画面卡住时存盘。", [UI_MENU_FLIGHT_SPIKE_MS] = "卡顿阈值（毫秒）",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "帧时间超过多少算作卡顿。", [UI_MENU_INPUT_RECORD] = "输入录制",
    [UI_MENU_INPUT_RECORD_DESC] = "将你的输入写入文件，用于报告问题。", [UI_MENU_OPEN_LOG] = "打开日志文件夹",
    [UI_MENU_OPEN_LOG_DESC] = "打开存放日志文件的文件夹。", [UI_MENU_COPY_INFO] = "复制系统信息",
    [UI_MENU_COPY_INFO_DESC] = "将 GPU、驱动程序、CPU、内存和已修改的设置复制到剪贴板。",
    [UI_MENU_COPY_INFO_DONE] = "已复制", [UI_MENU_ACTION_FAILED] = "操作失败",
    [UI_MENU_RESET] = "将所有设置恢复为默认值", [UI_MENU_RESET_DESC] = "对所有标签页生效。",
    [UI_MENU_RESET_CONFIRM] = "要将所有设置恢复为默认值吗？", [UI_MENU_RESET_YES] = "重置", [UI_MENU_RESET_NO] = "取消",

    [UI_MENU_OV_FPS] = "%.0f FPS", [UI_MENU_OV_FRAME_MS] = "%.1f ms", [UI_MENU_OV_GAME_PCT] = "游戏 %.0f%%",
    [UI_MENU_OV_MAX_MS] = "最大 %.1f ms", [UI_MENU_OV_GPU] = "GPU", [UI_MENU_OV_CPU] = "CPU", [UI_MENU_OV_RAM] = "内存",
    [UI_MENU_OV_VRAM] = "显存", [UI_MENU_OV_TEMP] = "温度", [UI_MENU_OV_POWER] = "功耗", [UI_MENU_OV_CLOCK] = "频率",
    [UI_MENU_OV_NONE] = "-", [UI_MENU_OV_REPLAY] = "GPU 帧准备", [UI_MENU_OV_GPU_TIME] = "每帧 GPU 忙碌时间",
    [UI_MENU_OV_PIPE_WAIT] = "管线等待", [UI_MENU_OV_SHADER_WAIT] = "着色器等待", [UI_MENU_OV_TEX_UPLOADS] = "纹理上传",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%", [UI_MENU_OV_FMT_TEMP] = "%.0f °C", [UI_MENU_OV_FMT_POWER] = "%.0f W",
    [UI_MENU_OV_FMT_MHZ] = "%.0f MHz", [UI_MENU_OV_FMT_GB] = "%.1f GB", [UI_MENU_OV_FMT_GB_OF] = "/ %.1f GB",
    [UI_MENU_OV_GAME_GB] = "游戏 %.1f GB", [UI_MENU_OV_FMT_MS] = "%.2f ms", [UI_MENU_OV_PER_FRAME] = "每帧 %.1f",
    [UI_MENU_OV_WINDOW] = "窗口", [UI_MENU_OV_OUTPUT] = "游戏画面", [UI_MENU_OV_SCENE] = "3D 场景",
    [UI_MENU_OV_SCALING] = "缩放", [UI_MENU_OV_LIMIT] = "帧率上限", [UI_MENU_OV_PRESENT] = "呈现方式",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u", [UI_MENU_OV_FMT_SCENE] = "%ux%u，%.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1，锐度 %d%%", [UI_MENU_OV_SCALE_NONE] = "无（1:1）",
    [UI_MENU_OV_SCALE_BILINEAR] = "双线性",
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
    {"ko", text_ko},
};

const char *ui_text_lang(const char *code) {
    static const struct { const char *code, *lang; } map[] = {
        {"en-US", "en"}, {"en-GB", "en"}, {"fr-CA", "fr"}, {"es-419", "es"}, {"pt-BR", "pt"},
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

// Matches "<lang>" and "<lang>-..." (a BCP 47 tag with that primary subtag or prefix).
static int tag_is(const char *tag, const char *lang) {
    size_t n = strlen(lang);
    return !strncmp(tag, lang, n) && (tag[n] == 0 || tag[n] == '-');
}

const char *menu_lang_for_system(const char *tag) {
    if (!tag) return "en";
    if (tag_is(tag, "ru")) return "ru";
    // Simplified Chinese: the script subtag when Windows gives one, else the regions that write it (China, Singapore).
    // zh-TW, zh-HK and zh-MO write Traditional Chinese, which the menu does not have: English.
    if (tag_is(tag, "zh-Hans") || tag_is(tag, "zh-CN") || tag_is(tag, "zh-SG")) return "zh-Hans";
    return "en";
}

// The menu language, re-read when cfg_version changes. TOTK_MENU_LANG "en", "ru" or "zh-Hans"; anything else (unset
// included) follows the system UI language (menu_lang_for_system).
static const char *resolve_menu_lang(void) {
    static const char *const langs[] = {"en", "ru", "zh-Hans"};
    const char *v = cfg_value("TOTK_MENU_LANG");
    for (size_t i = 0; v && i < sizeof langs / sizeof *langs; i++)
        if (!strcmp(v, langs[i])) return langs[i];
    return menu_lang_for_system(plat_ui_language());
}

const char *menu_language(void) {
    static _Atomic(const char *) lang = "en";
    static _Atomic unsigned seen = ~0u;
    unsigned v = cfg_version();
    if (v != atomic_load_explicit(&seen, memory_order_relaxed)) {
        atomic_store_explicit(&lang, resolve_menu_lang(), memory_order_relaxed);
        atomic_store_explicit(&seen, v, memory_order_relaxed);
    }
    return atomic_load_explicit(&lang, memory_order_relaxed);
}

const char *menu_text(UiTextId id) { return ui_text_in(menu_language(), id); }
