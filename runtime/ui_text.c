// Catalog of the text the host shows the player (ui_text.h). One array per catalog language, indexed by UiTextId: a new
// string gets an id in ui_text.h and an entry in every language that has one (a missing entry falls back to English).
// The software keyboard has every language; the menu has every game language except Portugal's Portuguese and
// Traditional Chinese (English, Russian, Brazilian Portuguese, Simplified Chinese, German, French, Spanish, Italian,
// Dutch, Japanese, Korean), and Vietnamese.
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
    [UI_MENU_LANGNAME_PT_BR] = "Português (Brasil)", [UI_MENU_LANGNAME_VI] = "Tiếng Việt", [UI_MENU_LANGNAME_EN_GB] = "English (UK)", \
    [UI_MENU_LANGNAME_FR_CA] = "Français (Canada)", [UI_MENU_LANGNAME_ES_419] = "Español (Latinoamérica)"

// SWKBD: TITLE, PROMPT, DEFAULT_NAME (at most 9 UTF-16 units, no '@' '%' '\'), OK, CANCEL, PAD_HINT.
#define SWKBD(title, prompt, name, ok, cancel, hint) \
    [UI_SWKBD_TITLE] = title, [UI_SWKBD_PROMPT] = prompt, [UI_SWKBD_DEFAULT_NAME] = name, [UI_SWKBD_OK] = ok, \
    [UI_SWKBD_CANCEL] = cancel, [UI_SWKBD_PAD_HINT] = hint, LANG_NAMES

// The DLSS model file and the executable, as the player sees them on this platform.
#ifdef _WIN32
#define DLSS_FILE "nvngx_dlss.dll"
#define EXE_FILE "totk.exe"
#else
#define DLSS_FILE "libnvidia-ngx-dlss.so"
#define EXE_FILE "totk"
#endif
// AMD's FSR 4 libraries (Windows only), as the setup downloads them next to the executable.
#define FFX_FILES "amd_fidelityfx_loader_vk.dll, amd_fidelityfx_upscaler_vk.dll"

static const char *const text_en[UI_TEXT_COUNT] = {
    SWKBD("Text entry", "Enter a name", "Horse", "OK", "Cancel", "Controller: A = OK, B = Cancel"),
    // The SDL text entry draws with SDL's ASCII debug font, so these three exist in English only (the other rows fall
    // back to it).
    [UI_SWKBD_SDL_KEYS_HINT] = "Keys: type, Enter OK, Esc cancel",
    // The controller lines: the add and delete buttons' labels, then the case switch's and the cancel button's.
    [UI_SWKBD_SDL_PAD_HINT1] = "Pad: D-pad move  %s add  %s delete",
    [UI_SWKBD_SDL_PAD_HINT2] = "%s upper/lower  Start OK  %s cancel",

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
    [UI_PERF_LINE_FPS_GEN] = "%.0f fps drawn, %.0f shown",
    [UI_MENU_GRAPHICS] = "Picture",
    [UI_MENU_OUTPUT_RES] = "Game resolution",
    [UI_MENU_OUTPUT_RES_DESC] = "The size the game draws its picture and interface in. Above 1920 x 1080 the game needs "
                                "more video memory and GPU time. A wider size (21:9) widens the camera's view and moves "
                                "the HUD to the screen's corners; a taller one (16:10, 3:2) moves it to the top "
                                "and bottom edges.",
    [UI_MENU_HUD_WIDTH] = "HUD width",
    [UI_MENU_HUD_WIDTH_DESC] = "On a screen wider than 16:9, how far apart the HUD's left and right sides are. Full puts "
                               "them at the screen's edges; 21:9 or 16:9 keeps the HUD within the middle of the screen "
                               "of that shape. A missing layout is made in the background and used after a restart.",
    [UI_MENU_HUD_WIDTH_FULL] = "Full screen",
    [UI_MENU_RENDER_SCALE] = "3D render scale",
    [UI_MENU_RENDER_SCALE_DESC] = "Size of the 3D scene relative to the game resolution; the game's FSR scales it to "
                                  "that size. Above 100 % smooths jagged edges (supersampling) at a high GPU cost.",
    [UI_MENU_RENDER_SCALE_GAME] = "Game (83 %)",
    [UI_MENU_DYNAMIC_RES] = "Dynamic resolution",
    [UI_MENU_DYNAMIC_RES_DESC] = "Lets the game lower the 3D scene to 90 or 80 %. It judges by an estimate made for the Switch that ignores your GPU, so wide scenes drop even on a fast PC. Off keeps the render scale.",
    [UI_MENU_UPSCALER] = "Scaling to the window",
    [UI_MENU_UPSCALER_DESC] = "How the picture is scaled when the window is larger than the game resolution. FSR keeps "
                              "edges and detail sharper than bilinear.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1", [UI_MENU_UPSCALER_BILINEAR] = "Bilinear",
    [UI_MENU_ASPECT] = "Picture shape",
    [UI_MENU_ASPECT_DESC] = "When the window's shape differs from the game resolution's (a 16:9 picture on a 21:9 "
                            "screen): keep the picture's shape with black bars, or stretch it over the window.",
    [UI_MENU_ASPECT_FIT] = "Black bars", [UI_MENU_ASPECT_STRETCH] = "Stretch",
    [UI_MENU_MOVIE_ASPECT] = "Movie shape",
    [UI_MENU_MOVIE_ASPECT_DESC] = "When the game resolution is not 16:9, keep the cutscene movies' own shape with black "
                                  "bars, or stretch them over the whole picture. Applies from the next movie.",
    [UI_MENU_MOVIE_ASPECT_KEEP] = "Black bars", [UI_MENU_MOVIE_ASPECT_STRETCH] = "Stretch",
    [UI_MENU_FOV] = "Field of view",
    [UI_MENU_FOV_DESC] = "How much of the world the camera shows, in percent of the game's own. Game versions 1.1.0, 1.2.1 and 1.4.3.",
    [UI_MENU_VERT_PLUS] = "Keep the 16:9 width",
    [UI_MENU_VERT_PLUS_DESC] = "On screens narrower than 16:9 (Steam Deck, 16:10, 4:3) the camera shows the same width of the world as at 16:9, and more above and below. No effect at 16:9 or wider.",
    [UI_MENU_SHADOW_RES] = "Shadow map size",
    [UI_MENU_SHADOW_RES_DESC] = "Size of the sun's shadow map. The game's own size changes with the view, down to about 400 "
                                "near the ground, so leaf shadows look blocky and jump; a fixed size keeps them still and "
                                "sharper. Above 1024 the shadows cost more GPU time and video memory; 256 is coarser than the "
                                "game's own and is for integrated GPUs short of memory bandwidth.",
    [UI_MENU_SHADOW_RES_GAME] = "Game (changes)",
    [UI_MENU_ANISO] = "Texture filtering",
    [UI_MENU_ANISO_DESC] = "Anisotropic filtering of surface textures: ground, roads and walls seen at a shallow angle stay sharp into the distance instead of blurring. Game: the level the game asks for, as on the Switch.",
    [UI_MENU_ANISO_GAME] = "Game",
    [UI_MENU_LOD_STEADY] = "Steady detail under load",
    [UI_MENU_LOD_STEADY_DESC] = "Skips the game's setup for lowering model and texture detail when the frame rate drops, as the emulator mod Disable LOD Quality Reduction does. Game versions 1.1.0, 1.2.1 and 1.4.3. Leave it off on weak PCs: there this drop keeps the frame rate up.",
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
    [UI_MENU_GRASS_RANGE] = "Grass range",
    [UI_MENU_GRASS_RANGE_DESC] = "How far around the camera grass grows (the game's: 62.5 m). Higher keeps the "
                                 "grass in meadows further out instead of growing it in just in front of you; lower "
                                 "draws less of it. Takes effect at the next start.",
    [UI_MENU_DLSS_PRESET] = "DLSS model",
    [UI_MENU_DLSS_PRESET_DESC] = "Which DLSS model runs. Automatic follows NVIDIA's advice per mode: K for DLAA, Quality "
                                 "and Balanced, M for Performance, L for Ultra Performance. J: slightly less ghosting "
                                 "behind moving characters, a little more flicker. K: the sharpest model in every mode. "
                                 "Applies at once.",
    [UI_MENU_DLSS_PRESET_DEFAULT] = "Automatic", [UI_MENU_DLSS_PRESET_K] = "K (sharpest)",
    [UI_MENU_DLSS_PRESET_J] = "J (less ghosting)",
    [UI_MENU_DLSS_CHARS] = "DLSS character trails",
    [UI_MENU_DLSS_CHARS_DESC] = "Reduces the faint trails behind moving characters, animals and what they hold: DLSS takes "
                                "less of the previous frames there. Stronger removes more of the trails and leaves those "
                                "edges a little less smooth. Applies at once.",
    [UI_MENU_DLSS_CHARS_OFF] = "Off", [UI_MENU_DLSS_CHARS_LIGHT] = "Light", [UI_MENU_DLSS_CHARS_MEDIUM] = "Medium",
    [UI_MENU_DLSS_CHARS_FULL] = "Full",
    [UI_MENU_DLSS] = "Upscaling mode",
    [UI_MENU_DLSS_DESC] = "Native anti-aliases the 3D scene at full resolution. Quality, Balanced, Performance and Ultra "
                          "Performance draw it at 67, 58, 50 and 33 % and let the upscaler (DLSS or FSR 3.1) scale it "
                          "up, which saves GPU time; they replace the 3D render scale. Takes effect at once.",
    [UI_MENU_DLSS_DLAA] = "Native", [UI_MENU_DLSS_QUALITY] = "Quality", [UI_MENU_DLSS_BALANCED] = "Balanced",
    [UI_MENU_DLSS_PERFORMANCE] = "Performance", [UI_MENU_DLSS_ULTRA] = "Ultra Performance",
    [UI_MENU_FXAA] = "Game anti-aliasing (FXAA)",
    [UI_MENU_FXAA_DESC] = "The game's own FXAA edge smoothing. It blurs fine detail a little; with DLAA on it is not needed.",
    [UI_MENU_REFLEX] = "Low Latency",
    [UI_MENU_REFLEX_DESC] = "Reduces system latency. On uses NVIDIA Reflex (VK_NV_low_latency2); On + Boost also keeps NVIDIA GPU clocks high in CPU-bound scenes. Auto uses Reflex on NVIDIA and AMD Anti-Lag 2 (VK_AMD_anti_lag) on AMD. Applies after a restart.",
    [UI_MENU_REFLEX_AUTO] = "Auto",
    [UI_MENU_REFLEX_OFF] = "Off",
    [UI_MENU_REFLEX_ON] = "On",
    [UI_MENU_REFLEX_BOOST] = "On + Boost",
    [UI_MENU_SCENE_UPSCALER] = "3D scene upscaler",
    [UI_MENU_SCENE_UPSCALER_DESC] = "A temporal upscaler for the 3D scene: smoother edges and less shimmer, and in a mode below Native it draws "
                                    "the scene smaller and scales it up, which saves GPU time. NVIDIA DLSS needs an NVIDIA "
                                    "RTX GPU and " DLSS_FILE " next to " EXE_FILE "; AMD FSR 3.1 runs on any GPU; AMD FSR 4 "
                                    "(Windows) needs a Radeon GPU AMD offers it for and " FFX_FILES " next to " EXE_FILE ". Off keeps "
                                    "the game's own scaling and FXAA. The HUD is not touched; the upscaler and sharpness "
                                    "above still fit the game image to the window. Takes effect after a restart.",
    [UI_MENU_SCENE_UPSCALER_OFF] = "Off", [UI_MENU_SCENE_UPSCALER_DLSS] = "NVIDIA DLSS",
    [UI_MENU_SCENE_UPSCALER_FSR3] = "AMD FSR 3.1", [UI_MENU_SCENE_UPSCALER_FSR4] = "AMD FSR 4",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_GPU] = "FSR 4: not offered for this GPU",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_DLL] = "FSR 4: AMD's FSR libraries are missing",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_OS] = "FSR 4: Windows only",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_GPU] = "DLSS: needs an NVIDIA RTX GPU",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_DLL] = "DLSS: " DLSS_FILE " is missing",
    [UI_MENU_FSR_SHARPNESS] = "FSR sharpness",
    [UI_MENU_FSR_SHARPNESS_DESC] = "Sharpening of the 3D scene after AMD FSR 3.1 or FSR 4; 0 turns it off.",

    [UI_MENU_FPS_LIMIT] = "Frame rate limit",
    [UI_MENU_FPS_LIMIT_DESC] = "The most frames per second the game draws. Above 60 is experimental and needs Game speed "
                               "follows the clock.",
    [UI_MENU_FPS_30] = "30 FPS", [UI_MENU_FPS_40] = "40 FPS", [UI_MENU_FPS_45] = "45 FPS",
    [UI_MENU_FPS_60] = "60 FPS", [UI_MENU_FPS_90] = "90 FPS", [UI_MENU_FPS_120] = "120 FPS", [UI_MENU_FPS_144] = "144 FPS",
    // the monitor's refresh rate; printf format with the rate in Hz when it is known
    [UI_MENU_FPS_DISPLAY] = "Monitor rate", [UI_MENU_FPS_DISPLAY_HZ] = "Monitor rate (%.0f Hz)",
    [UI_MENU_FRAME_QUEUE] = "Frame queue",
    [UI_MENU_FRAME_QUEUE_DESC] = "The game starts a frame while the previous one waits for its turn on screen: far fewer "
                                 "dropped frames at the limit, up to one frame more input delay. Applies at once.",
    [UI_MENU_FRAME_GEN] = "Frame generation", [UI_MENU_FRAME_GEN_OFF] = "Off", [UI_MENU_FRAME_GEN_FSR3] = "AMD FSR 3",
    [UI_MENU_FRAME_GEN_DESC] = "A generated picture between each two the game draws: twice the pictures on screen, the "
                               "game at half the frame limit and the controls about half a frame later. For displays "
                               "of 120 Hz and more. Not with DLSS or HDR. Applies at once.",
    [UI_MENU_REALTIME] = "Game speed follows the clock",
    [UI_MENU_REALTIME_DESC] = "Steps the game by real time, so a slow frame does not slow the game down.",
    [UI_MENU_CLOTH_REALTIME] = "Cloth on the clock",
    [UI_MENU_CLOTH_REALTIME_DESC] = "Steps cloth and hair by real time as well.",
    [UI_MENU_HIGHFPS_FIX] = "High frame rate fixes",
    [UI_MENU_HIGHFPS_FIX_DESC] = "Corrects physics that misbehave above 30 frames per second, and objects that stay see-through or invisible above 60.",
    [UI_MENU_CLIMB_FIX] = "Climbing speed fix",
    [UI_MENU_CLIMB_FIX_DESC] = "Keeps climbing walls and ladders at full speed above 60 frames per second, so Link reaches the top.",
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

    [UI_MENU_PLAY_TIME] = "Play time",
    [UI_MENU_PLAY_TIME_DESC] = "Time in the game over all runs, counted while the game window has the focus and the game "
                               "draws frames. Kept in playtime.txt in the profile folder, next to the saves.",
    [UI_MENU_PLAY_TIME_HM] = "%llu h %02u min", [UI_MENU_PLAY_TIME_SESSION] = "this run: %s",
    [UI_MENU_MODS_BUILTIN] = "Built into the port", [UI_MENU_MODS_INSTALLED] = "Installed mods",
    [UI_MENU_MODS_NONE] = "No mods yet. Unpack a mod into the mods folder, one folder per mod with its romfs folder "
                          "inside, and restart the game. TKMM can export its merged mods into a folder there too.",
    [UI_MENU_MODS_OPEN] = "Open mods folder",
    [UI_MENU_MODS_OPEN_DESC] = "Shows the folder the game loads mods from. It is created if it is missing.",
    [UI_MENU_MODS_ROW_DESC] = "Game files it replaces: %u. New files it adds: %u.",
    [UI_MENU_MODS_OVERRIDDEN] = "%u of its files come from %s, which is higher in the list.",
    [UI_MENU_MODS_MOVE_HINT] = "The top face button or Ctrl+Up moves it up, the left face button or Ctrl+Down moves it "
                               "down. When two mods change the same file, game data or code, the higher one wins. "
                               "Changes apply after a restart.",
    [UI_MENU_MODS_EMPTY] = "This folder has no romfs, exefs or cheats files.",
    [UI_MENU_MODS_SUBSDK] = "This mod brings its own game code (a subsdk plugin or a replaced main). The port cannot "
                            "load code modules, so the whole mod stays off. Its switch \"Use ... without its code\" "
                            "below uses the rest of it.",
    [UI_MENU_MODS_EXEFS_LATER] = "This mod patches the game code (exefs). The game builds the patches in when it "
                                 "starts next (a rebuild of a few minutes); until then the whole mod stays off.",
    [UI_MENU_MODS_CHEATS] = "Cheats: %d. Switch them on the Cheats tab.",
    [UI_MENU_MODS_CHEATS_FILE] = "Cheats for this game version are read at start from cheats\\%s in this mod.",
    [UI_MENU_MODS_BAD_NAME] = "The folder name contains |, which the mod list uses between names. Rename the folder.",
    [UI_MENU_MODS_OTHER_VERSION] = "%u of its files carry another game version in their names. The game reads them only "
                                   "where something asks for those names. If the mod does nothing, look for a version "
                                   "of it made for your game version.",
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
    [UI_MENU_VOLUME_MUSIC] = "Music volume", [UI_MENU_VOLUME_MUSIC_DESC] = "Volume of background music.",
    [UI_MENU_VOLUME_SFX] = "SFX volume", [UI_MENU_VOLUME_SFX_DESC] = "Volume of sound effects.",
    [UI_MENU_VOLUME_VOICE] = "Voice volume", [UI_MENU_VOLUME_VOICE_DESC] = "Volume of character voices.",
    [UI_MENU_AUDIO_CHANNELS] = "Speakers",
    [UI_MENU_AUDIO_CHANNELS_DESC] = "Stereo or 5.1 surround output. Automatic picks 5.1 when the Windows speaker setup "
                                    "has six or more channels. Applies after a restart.",
    [UI_MENU_CHANNELS_AUTO] = "Automatic", [UI_MENU_CHANNELS_STEREO] = "Stereo", [UI_MENU_CHANNELS_51] = "5.1 surround",
    [UI_MENU_AUDIO_DEVICE] = "Output device",
    [UI_MENU_AUDIO_DEVICE_DESC] = "Where the sound plays. System default follows the Windows default device. A chosen "
                                  "device that is unplugged falls back to the system default until it is back. "
                                  "Applies at once.",
    [UI_MENU_AUDIO_DEVICE_DEFAULT] = "System default",

    [UI_MENU_MOTION] = "Motion controls", [UI_MENU_MOTION_DESC] = "Aiming with the controller's gyroscope.",
    [UI_MENU_MOTION_DISABLED] = "disabled",
    [UI_MENU_MOTION_NATIVE] = "native",
    [UI_MENU_MOTION_DSU] = "dsu",
    [UI_MENU_GYRO_CALIBRATION] = "Gyro bias calibration",
    [UI_MENU_GYRO_CALIBRATION_DESC] = "Subtracts the controller's resting drift, measured with the calibrate row "
                                      "below. The measured values stay saved when this is off.",
    [UI_MENU_GYRO_CALIBRATE] = "Calibrate gyro",
    [UI_MENU_GYRO_CALIBRATE_DESC] = "Measures the gyroscope's resting drift and removes it. Put the controller down or "
                                    "hold it still for two seconds.",
    [UI_MENU_GYRO_CALIBRATE_WAIT] = "Calibrating...",
    [UI_MENU_GYRO_CALIBRATED] = "%s, calibrated",
    [UI_MENU_GYRO_CALIBRATE_FAILED] = "Calibration failed",
    [UI_MENU_GYRO_CALIBRATE_MOVED] = "Controller moved: hold it still and try again",
    [UI_MENU_GYRO_AUTO_BIAS] = "Gyro drift learning at rest",
    [UI_MENU_GYRO_AUTO_BIAS_DESC] = "Learns the drift left in the gyro whenever the controller lies still on a "
                                    "table, and subtracts it. For a bow aim that slowly slides even after "
                                    "calibration. A controller held in the hand is not learned from. Experimental.",
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
                             "prompts for A show B). PlayStation: the shapes on a PlayStation pad (○ × △ □, L1, R1, "
                             "L2, R2, Options and Share), on the button the layout above presses. PC: the keys and "
                             "mouse buttons of the keyboard's default layout (E, Shift, Space, left click...) and the "
                             "mouse for the camera. The game reads its prompts at start, so they change after a "
                             "restart. They are part of the game's files, so a button you remap in Controls keeps its "
                             "old prompt on screen.",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo", [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation", [UI_MENU_PROMPTS_PC] = "PC",
    [UI_MENU_PROMPTS_MISSING] = "prompt files missing: run the setup again",
    [UI_MENU_PROMPTS_FAILED] = "prompt files damaged: run the setup again",
    [UI_MENU_PROMPTS_RESTART] = "prompts change after restart",
    [UI_MENU_PROMPTS_OFF_SHAPE] = "off at this game resolution (no layout made for it)",
    [UI_MENU_OUTPUT_RES_NO_LAYOUT] = "no layout made for this shape: run the setup again",
    [UI_MENU_OUTPUT_RES_PREPARING] = "layout being made in the background; used at the next start",
    [UI_MENU_PROMPTS_PREPARING] = "prompt files being made in the background; used at the next start",
    [UI_MENU_PREPARE_RETRY] = "Try again",
    [UI_MENU_PREPARE_RETRY_DESC] = "Starts the setup's tools again to make the missing interface layout or button prompt files in the background. The game keeps running; it uses them from its next start.",
    [UI_MENU_SHAPE_PREPARE_FAILED] = "the setup's tools did not start: run the setup again",
    [UI_MENU_PROMPTS_OFF_MODS] = "off: a RomFS mod replaces their files",
    [UI_MENU_PROMPTS_OFF_PATCHES] = "off while TOTK_ROMFS_PATCHES is set",
    [UI_MENU_RESTART] = "Restart now",
    [UI_MENU_RESTART_DESC] = "Closes the game and starts it again with the new prompts. Progress since your last save "
                             "is lost.",
    [UI_MENU_RESTART_CONFIRM] = "Restart the game now? Progress since your last save is lost.",
    [UI_MENU_RESTART_YES] = "Restart",
    [UI_MENU_RESTART_SAVING] = "The game is saving: Restart is possible in a moment.",
    [UI_MENU_GROUP_STICKS] = "Analog sticks",
    [UI_MENU_LSTICK_DEADZONE] = "Left stick dead zone", [UI_MENU_RSTICK_DEADZONE] = "Right stick dead zone",
    [UI_MENU_STICK_DEADZONE_DESC] = "How far a gamepad stick moves before the game sees it. Raise it if the character or the camera moves on its own: let go of the stick, read \"stick now\" and set the dead zone a few percent above it.",
    [UI_MENU_STICK_DEADZONE_NOW] = "stick now: %.0f %%",
    [UI_MENU_MOUSE_MODE] = "Mouse camera",
    [UI_MENU_MOUSE_MODE_DESC] = "Direct: the camera turns by how far the mouse moves, as in a PC game, with no stick dead zone or curve. Stick emulation: the mouse acts as the right stick.",
    [UI_MENU_MOUSE_STICK] = "Stick emulation",
    [UI_MENU_MOUSE_DIRECT] = "Direct",
    [UI_MENU_MENU_CURSOR] = "Mouse in the game's menus",
    [UI_MENU_MENU_CURSOR_DESC] = "In the game's own menus (title, inventory, shops, dialogue choices) the pointer shows: the item under it is selected, left click confirms, right click goes back. Only after the mouse or a key was used last, so a gamepad sees the menus unchanged (game 1.4.3).",
    [UI_MENU_MAP_CLICK] = "Left click on the map",
    [UI_MENU_MAP_CLICK_DESC] = "On the Purah Pad's map, the left mouse button presses A at the map's cursor instead of the button it is bound to (Y by default). Needs Mouse in the game's menus (game 1.4.3).",
    [UI_MENU_DIALOGUE_CLICK] = "Clicks in dialogue",
    [UI_MENU_DIALOGUE_CLICK_DESC] = "While a dialogue box is open, left click presses B (it continues the text and skips its scroll) and right click presses A, instead of the buttons they are bound to (Y by default, which opens the dialogue log, and ZL). Dialogue choices keep the pointer: left click picks (game 1.4.3).",
    [UI_MENU_POINTER_IMAGE] = "Game pointer",
    [UI_MENU_POINTER_IMAGE_DESC] = "While the cursor is not captured (the game's menus, this menu, after Esc), the window shows a gold arrowhead of its own instead of the system arrow, sized for the screen's scale.",
    [UI_MENU_QUICKSAVE] = "Quick save and load",
    [UI_MENU_QUICKSAVE_DESC] = "One key saves the game at once and another loads that save (the two rows below: F6 and F7 by default, as in Half-Life), with no menu on the screen. Keyboard and mouse keys only, game 1.4.3 only. Quick save asks the game for one of its own autosaves, so it replaces the oldest autosave (the manual save stays), keeps a copy of it as the quick save (the quicksave folder of the profile) and shows \"Game saved\" at the top of the picture. Quick load holds the picture on \"Loading the quick save\" while it goes through the game's load list out of sight, and the game loads the quick save (about 5 s). Where the game would not make an autosave itself (a cutscene, a dialogue), nothing is saved and the line says so.",
    [UI_MENU_QUICKSAVE_KEY] = "Quick save key",
    [UI_MENU_QUICKLOAD_KEY] = "Quick load key",
    [UI_MENU_QUICKSAVE_KEY_DESC] = "The key that starts quick save or quick load; F6 and F7 by default, as in Half-Life. Select the cell and press any key or mouse button; Delete leaves that one without a key. While quick save is on, the two keys do nothing else (a note shows when a binding also uses the key). totk.ini takes any key name of the bindings (quicksave_key, quickload_key).",
    [UI_QS_SAVED] = "Game saved",
    [UI_QS_LOADING] = "Loading the quick save",
    [UI_QS_NOT_NOW] = "Quick save is not possible right now",
    [UI_QS_NONE] = "There is no quick save yet (the quick save key makes one)",
    [UI_QS_FAILED] = "Quick save failed: see totk.log",
    [UI_QS_LOAD_FAILED] = "Quick load failed: see totk.log",
    [UI_MENU_MOUSE_SENS] = "Mouse camera sensitivity",
    [UI_MENU_MOUSE_SENS_DESC] = "How fast the mouse turns the camera.",
    [UI_MENU_WALK_KEY] = "Walk key",
    [UI_MENU_WALK_KEY_DESC] = "Hold it to tilt the keyboard's sticks part of the way, so the keyboard walks instead of "
                              "runs. The walk scale sets how far. A controller and the mouse are not affected.",
    [UI_MENU_WALK_KEY_TAKEN] = "also %s",
    [UI_MENU_WALK_SCALE] = "Walk scale",
    [UI_MENU_WALK_SCALE_DESC] = "The tilt the keyboard's sticks get while the walk key is held. Half is a walk, the "
                                "full tilt a run.",
    [UI_MENU_FREECAM] = "Free camera",
    [UI_MENU_FREECAM_DESC] = "Lets a combo or a key below detach the view from Link and fly it. Off by default: "
        "with it off the game's camera code runs as shipped. Takes effect after a restart. "
        "Game version 1.4.3.",
    [UI_MENU_FREECAM_OFF_NOTE] = "Turn on Free camera above, then restart",
    [UI_MENU_FREECAM_PAD] = "Free camera combo",
    [UI_MENU_FREECAM_PAD_DESC] = "Gamepad buttons pressed together that step the view through three stages: it leaves "
        "Link and flies, it stays where it was left while Link plays on, and it comes back. While it flies the game "
        "gets no input: the left stick flies along the view and sideways, the triggers up and down, the right stick "
        "turns, and each press of L or R halves or doubles the speed. Locked, Link walks where the left stick points "
        "on screen. Game version 1.4.3.",
    [UI_MENU_FREECAM_PAD_V143] = "Game version 1.4.3 only",
    [UI_MENU_FREECAM_KEY] = "Free camera key",
    [UI_MENU_FREECAM_KEY_DESC] = "A key that steps the view through the same stages: it leaves Link and flies, it "
        "stays while Link plays on, and it comes back. While it flies: W A S D fly along the view and sideways, the "
        "keys bound to ZR and ZL (X and Z by default) up and down, the mouse and the arrow keys turn, and the keys "
        "bound to L and R (Q and R) halve or double the speed. Game version 1.4.3.",
    [UI_MENU_GROUP_GAMEPLAY_TWEAKS] = "Gameplay tweaks",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED] = "Gameplay tweaks",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED_DESC] = "Mouse wheel controls for Ultrahand rotation and distance, item and weapon switching, and smooth map zoom.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE] = "Ultrahand wheel rotation",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE_DESC] = "Turn objects held by Ultrahand directly with the mouse wheel.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS] = "Ultrahand C rotation axis",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS_DESC] = "Hold C while scrolling to turn held objects around the alternate axis.",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY] = "Ultrahand rotation sensitivity",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY_DESC] = "How fast the mouse wheel turns held objects.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE] = "Ultrahand wheel distance",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE_DESC] = "Right-click + mouse wheel pushes or pulls held objects.",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY] = "Ultrahand distance sensitivity",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY_DESC] = "How fast the mouse wheel changes object distance.",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL] = "Weapon wheel selection",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL_DESC] = "Scroll mouse wheel in free roam to switch weapons. Confirm with E.",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY] = "Weapon wheel sensitivity",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY_DESC] = "Scroll steps needed to advance to the next weapon.",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL] = "Quick menu wheel selection",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL_DESC] = "Scroll mouse wheel to browse items while a quick-selection menu is open.",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY] = "Quick menu wheel sensitivity",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY_DESC] = "Scroll steps needed to advance through items in quick-selection menus.",
    [UI_MENU_GAMEPLAY_ITEM_AD] = "Quick menu A / D navigation",
    [UI_MENU_GAMEPLAY_ITEM_AD_DESC] = "Press A / D to navigate left and right in quick-selection menus.",
    [UI_MENU_GAMEPLAY_MAP_ZOOM] = "Smooth map zoom",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_DESC] = "Smooth inertial zoom on the Purah Pad map using the mouse wheel.",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY] = "Map zoom sensitivity",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY_DESC] = "How fast the mouse wheel zooms the map.",
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
    [UI_BIND_SHAKE] = "Shake the camera stick",
    [UI_BIND_DESC_SHAKE] = "Swings the right stick left and right for the game: detaches a fused part from what "
                           "Ultrahand holds. Holding the key keeps shaking.",
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
    [UI_PAD_TOUCHPAD] = "Touchpad click",
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
    [UI_MENU_DISCORD_RPC] = "Discord Rich Presence",
    [UI_MENU_DISCORD_RPC_DESC] = "Shows \"Playing TotK Recomp\" with the play time in your Discord status while the "
                                 "game runs. discord_app_id and discord_image in totk.ini choose the application.",

    [UI_MENU_ADV_WARNING] = "These settings are for testing and troubleshooting. Change them only when you know why.",
    [UI_MENU_GROUP_SCHED] = "Scheduling", [UI_MENU_GROUP_SHADERS] = "Shaders and pipelines",
    [UI_MENU_GROUP_TEXMEM] = "Textures and memory", [UI_MENU_GROUP_FIXES] = "Render fixes",
    [UI_MENU_GROUP_VULKAN] = "Vulkan", [UI_MENU_GROUP_DIAG] = "Diagnostics",
    [UI_MENU_ASYNC_PREEMPT] = "Virtual core preemption",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "Lets a game thread interrupt another one on the same virtual core.",
    [UI_MENU_PREEMPT_OFF] = "Off", [UI_MENU_PREEMPT_ON] = "On", [UI_MENU_PREEMPT_INHERIT] = "Inheritance only",
    [UI_MENU_REPLAY_ASYNC] = "GPU frame prep on its own thread",
    [UI_MENU_REPLAY_ASYNC_DESC] = "Prepares frames for the GPU on a separate thread instead of the game thread. Adds up to one frame of latency.",
    [UI_MENU_REPLAY_WORKERS] = "GPU frame prep threads",
    [UI_MENU_REPLAY_WORKERS_DESC] = "Records large frames for the GPU on several threads at once. Helps when the CPU limits the frame rate; needs 8 or more logical processors. Off records on one thread.",
    [UI_MENU_REPLAY_WORKERS_NOW] = "now: %s",
    [UI_MENU_TIMER_1MS] = "1 ms Windows timer",
    [UI_MENU_TIMER_1MS_DESC] = "Raises the system timer resolution to 1 ms for steadier frame pacing.",
    [UI_MENU_SYNC_AHEAD] = "One frame ahead of the GPU",
    [UI_MENU_SYNC_AHEAD_DESC] = "The game builds the next frame while the graphics card still draws the previous one. More frames per second, one frame more input delay.",
    [UI_MENU_HOST_CHECKPOINT] = "Quick core hand-over",
    [UI_MENU_HOST_CHECKPOINT_DESC] = "A game thread gives its virtual core to a more important one as soon as it leaves the port's own code, not at its next system call.",
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
    [UI_MENU_ASTC_BC] = "Compress ASTC to BC3", [UI_MENU_ASTC_BC_DESC] = "With the GPU decoder: re-encodes decoded ASTC textures to BC3, a quarter of the video memory. Textures look slightly blockier. Auto: on for GPUs with 4 GB of video memory or less.",
    [UI_MENU_LOD_GUARD] = "LOD guard", [UI_MENU_LOD_GUARD_DESC] = "Prevents objects flashing in the wrong place or a flat far field for a frame, by waiting for the game to finish rewriting level-of-detail data before the GPU reads it.",
    [UI_MENU_FIX_C0] = "Constant buffer fix",
    [UI_MENU_FIX_C0_DESC] = "Fills the driver-reserved constant bank c0 (storage sizes, dispatch size, point size, base instance) instead of reading zeros.",
    [UI_MENU_FIX_ZERO] = "Zero new GPU memory",
    [UI_MENU_FIX_ZERO_DESC] = "New GPU memory pools start zero-filled, as Switch memory does.",
    [UI_MENU_DLAA_EFFECTS_AFTER] = "Particles after DLAA",
    [UI_MENU_DLAA_EFFECTS_AFTER_DESC] = "With DLAA, dust and other particles are drawn over the picture DLSS has "
                                        "already smoothed, as the game draws them without DLSS: small particles in "
                                        "fog (shrines) no longer vanish or leave trails. The fog and the clouds still "
                                        "go through DLSS, so distant edges against the sky stay clean. Off: DLSS also "
                                        "processes the particles, as before. DLAA mode only; the upscaling DLSS modes "
                                        "always process them. Applies at once.",
    [UI_MENU_DLSS_FIXED_EXPOSURE] = "Fixed DLSS exposure",
    [UI_MENU_DLSS_FIXED_EXPOSURE_DESC] = "DLSS takes the brightness of the game's picture as it is instead of "
                                         "estimating the exposure from each frame: in dark scenes (the Depths) large "
                                         "fogged areas no longer turn darker or lighter for a frame while the camera "
                                         "moves. Off: DLSS estimates the exposure itself, as before. Applies at once.",
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
                                "older, which draws characters black with it, and on Intel's Windows driver for "
                                "integrated GPUs, which draws coloured noise on foliage; on elsewhere.",
    [UI_MENU_VK_SUBGROUP_PIN] = "Pin subgroup size", [UI_MENU_VK_SUBGROUP_PIN_DESC] = "Fixes the shader subgroup size.",
    [UI_MENU_TRACE] = "Performance log", [UI_MENU_TRACE_DESC] = "Writes frame timings to a file.",
    [UI_MENU_FLIGHT] = "Flight recorder",
    [UI_MENU_FLIGHT_DESC] = "Keeps a short history and saves it when a frame freezes.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "Freeze threshold (ms)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "Frame time that counts as a freeze.",
    [UI_MENU_PASS_CRUMBS] = "GPU pass marks",
    [UI_MENU_PASS_CRUMBS_DESC] = "After a GPU crash, the log names the render pass that was running. Automatic: AMD only. On: also NVIDIA, at a small cost.",
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
    [UI_MENU_OV_FPS_LABEL] = "FPS", [UI_MENU_OV_FRAME_LABEL] = "Frame", [UI_MENU_OV_FMT_FPS] = "%.0f",
    [UI_MENU_OV_LOW1] = "1%% low %.0f", [UI_MENU_OV_LOW01] = "0.1%% low %.0f",
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
    [UI_MENU_OV_UPSCALER] = "3D upscaler",
    [UI_MENU_OV_SCALING] = "Scaling", [UI_MENU_OV_LIMIT] = "Frame limit", [UI_MENU_OV_PRESENT] = "Presentation", [UI_MENU_OV_SHOWN] = "Shown",
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
    [UI_LW_STAGE_START] = "Getting ready",
    [UI_LW_STAGE_GPU] = "Starting the graphics",
    [UI_LW_STAGE_FILES] = "Preparing the game files",
    [UI_LW_STAGE_SHADERS] = "Loading shaders",
    [UI_LW_STAGE_PIPELINES] = "Building pipelines",
    [UI_LW_STAGE_GAME] = "Starting the game",
    [UI_LW_FAILED] = "The game stopped",
    [UI_LW_FAILED_HINT] = "The log below says why. Copy it for a bug report, then close this window.",
    [UI_LW_VERSION] = "v%s · game %s",
    [UI_LW_COUNT] = "%s of %s",
    [UI_LW_LEFT_ITEMS] = "%s left",
    [UI_LW_LEFT_SEC] = "about %d s left",
    [UI_LW_LEFT_MIN] = "about %d min left",
    [UI_LW_LEFT_FEW] = "a few seconds left",
    [UI_LW_ALSO] = "Also: %s",
    [UI_LW_GAME_SEC] = "The game starts in about %d s",
    [UI_LW_GAME_MIN] = "The game starts in about %d min",
    [UI_LW_GAME_SOON] = "The game starts any moment now",
    [UI_LW_ELAPSED] = "Elapsed %d:%02d",
    [UI_LW_DETAILS] = "Details",
    [UI_LW_HIDE] = "Hide details",
    [UI_LW_COPY] = "Copy log",
    [UI_LW_COPIED] = "Copied",
    [UI_LW_CLOSE] = "Close and quit the game",
    [UI_LW_THOUSANDS] = ",",
    [UI_MENU_CHEATS_REBUILD] = "Rebuild the game now",
    [UI_MENU_CHEATS_REBUILD_DESC] = "Some cheats change the game's code and are not built into it: they are new or "
                                    "edited, or their last rebuild failed or was cancelled. Closes the game; the setup "
                                    "builds them in (a few minutes) and starts the game again. Progress since your "
                                    "last save is lost.",
    [UI_MENU_MODS_EXEFS_UNSUPPORTED] = "This mod patches the game code (exefs) in a way the port cannot apply, so the "
                                       "whole mod stays off. Its switch \"Use ... without its code\" below uses the "
                                       "rest of it. %s",
    [UI_MENU_MODS_EXEFS_OTHER] = "Its code patches (exefs) are made for another game version.",
    [UI_MENU_MODS_EXEFS_ON] = "Code patches in use: %d files.",
    [UI_MENU_MODS_REPLACES_BUILTIN] = "Code patches in use: %d files. They replace the port's own patch %s, which "
                                      "stays off while this mod is on.",
    [UI_MENU_MODS_CODE_SKIPPED] = "Its own game code (%s) does not load: the rest of the mod is used. Switch the mod "
                                  "off if the game misbehaves with it.",
    [UI_MENU_MODS_PLUGIN_ON] = "Its plugin runs: %d hooks into the game code.",
    [UI_MENU_MODS_PLUGIN_REFUSED] = "Its plugin %s cannot run in the port: %s.",
    [UI_MENU_MODS_PLUGIN_STAYS_OFF] = "The whole mod stays off. Its switch \"Use ... without its code\" below uses the "
                                      "rest of it.",
    [UI_MENU_MODS_PLUGIN_OFF] = "Its plugin stays off this time: it started differently from the build (%s). The game "
                                "rebuilds it at the next start.",
    [UI_MENU_MODS_WITHOUT_CODE] = "Use %s without its code",
    [UI_MENU_MODS_WITHOUT_CODE_DESC] = "The mod above brings code the port cannot run: its own game code (a subsdk "
                                       "plugin or a replaced main) or code patches (exefs) the port cannot apply. Off: "
                                       "the whole mod stays off. On: that code is left out and the rest of the mod is "
                                       "used: its RomFS files, cheats and the code patches that work. Its files may "
                                       "expect the missing code; switch it off again if the game misbehaves. Applies "
                                       "at the next start.",
    [UI_MENU_MODS_WITHOUT_CODE_ALL] = "On for every mod: mods_without_code = 1 in totk.ini.",
    [UI_MENU_MODS_EXEFS_SKIPPED] = "%u of its code patch files (exefs) cannot be applied and are left out: the rest of "
                                   "the mod is used. %s",
    [UI_MENU_MODS_OVERRIDES_FILES] = "%u of its files replace those of %s, which is lower in the list.",
    [UI_MENU_MODS_DATA_OVERRIDDEN] = "%u bytes of its game data changes come from %s, which is higher in the list.",
    [UI_MENU_MODS_OVERRIDES_DATA] = "%u bytes of its game data changes replace those of %s, which is lower in the "
                                    "list.",
    [UI_MENU_MODS_CODE_OVERRIDDEN] = "%u of its code patch sites are overridden by %s, which is higher in the list.",
    [UI_MENU_MODS_OVERRIDES_CODE] = "It overrides %u code patch sites of %s, which is lower in the list.",
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
    [UI_PERF_LINE_FPS_GEN] = "%.0f к/с отрисовано, %.0f показано",
    [UI_MENU_GRAPHICS] = "Изображение",
    [UI_MENU_OUTPUT_RES] = "Разрешение игры",
    [UI_MENU_OUTPUT_RES_DESC] = "Размер, в котором игра рисует картинку и интерфейс. Выше 1920 × 1080 игре нужно больше "
                                "видеопамяти и времени GPU. Более широкий размер (21:9) расширяет обзор камеры и "
                                "переносит интерфейс в углы экрана, более высокий (16:10, 3:2) переносит его к "
                                "верхнему и нижнему краям.",
    [UI_MENU_HUD_WIDTH] = "Ширина интерфейса",
    [UI_MENU_HUD_WIDTH_DESC] = "На экране шире 16:9: как далеко друг от друга левая и правая части интерфейса. "
                               "«Весь экран» ставит их у краёв экрана; 21:9 или 16:9 держит интерфейс в середине "
                               "экрана такой формы. Недостающая раскладка создаётся в фоне и будет после перезапуска.",
    [UI_MENU_HUD_WIDTH_FULL] = "Весь экран",
    [UI_MENU_RENDER_SCALE] = "Масштаб 3D-отрисовки",
    [UI_MENU_RENDER_SCALE_DESC] = "Размер 3D-сцены относительно разрешения игры; FSR игры доводит её до этого размера. "
                                  "Выше 100 % сглаживает лесенки на краях (суперсэмплинг) ценой большой нагрузки на GPU.",
    [UI_MENU_RENDER_SCALE_GAME] = "Как в игре (83 %)",
    [UI_MENU_DYNAMIC_RES] = "Динамическое разрешение",
    [UI_MENU_DYNAMIC_RES_DESC] = "Игра снижает 3D-сцену до 90 или 80 %. Она судит по оценке для Switch, не учитывающей вашу видеокарту, поэтому широкие сцены снижаются даже на быстром ПК. Выключено: остаётся масштаб отрисовки.",
    [UI_MENU_UPSCALER] = "Масштабирование под окно",
    [UI_MENU_UPSCALER_DESC] = "Как картинка растягивается, когда окно больше разрешения игры. FSR сохраняет края и "
                              "детали чётче билинейного.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1", [UI_MENU_UPSCALER_BILINEAR] = "Билинейное",
    [UI_MENU_ASPECT] = "Форма картинки",
    [UI_MENU_ASPECT_DESC] = "Когда форма окна отличается от разрешения игры (картинка 16:9 на экране 21:9): сохранить "
                            "форму картинки с чёрными полосами или растянуть её на всё окно.",
    [UI_MENU_ASPECT_FIT] = "Чёрные полосы", [UI_MENU_ASPECT_STRETCH] = "Растянуть",
    [UI_MENU_MOVIE_ASPECT] = "Форма видеороликов",
    [UI_MENU_MOVIE_ASPECT_DESC] = "Когда разрешение игры не 16:9: сохранить форму роликов с чёрными полосами или "
                                  "растянуть их на всю картинку. Действует со следующего ролика.",
    [UI_MENU_MOVIE_ASPECT_KEEP] = "Чёрные полосы", [UI_MENU_MOVIE_ASPECT_STRETCH] = "Растянуть",
    [UI_MENU_FOV] = "Поле зрения",
    [UI_MENU_FOV_DESC] = "Сколько мира показывает камера, в процентах от игрового. Для версий игры 1.1.0, 1.2.1 и 1.4.3.",
    [UI_MENU_VERT_PLUS] = "Ширина обзора как в 16:9",
    [UI_MENU_VERT_PLUS_DESC] = "На экранах уже 16:9 (Steam Deck, 16:10, 4:3) камера показывает мир той же ширины, что и в 16:9, и больше сверху и снизу. На 16:9 и шире ничего не меняет.",
    [UI_MENU_SHADOW_RES] = "Размер карты теней",
    [UI_MENU_SHADOW_RES_DESC] = "Размер карты теней от солнца. Собственный размер игры меняется вместе с видом, у земли до "
                                "примерно 400, поэтому тени листвы выглядят квадратными и дёргаются; постоянный размер делает "
                                "их ровными и чётче. Выше 1024 тени требуют больше времени GPU и видеопамяти; 256 грубее "
                                "собственного размера игры и предназначен для встроенных GPU с нехваткой пропускной способности памяти.",
    [UI_MENU_SHADOW_RES_GAME] = "Как в игре (меняется)",
    [UI_MENU_ANISO] = "Фильтрация текстур",
    [UI_MENU_ANISO_DESC] = "Анизотропная фильтрация текстур поверхностей: земля, дороги и стены под острым углом остаются чёткими вдали, а не размываются. Игра: уровень, который запрашивает игра, как на Switch.",
    [UI_MENU_ANISO_GAME] = "Игра",
    [UI_MENU_LOD_STEADY] = "Стабильная детализация под нагрузкой",
    [UI_MENU_LOD_STEADY_DESC] = "Пропускает настройку игры, снижающую детализацию моделей и текстур при падении частоты кадров, как эмуляторный мод Disable LOD Quality Reduction. Только для версий игры 1.1.0, 1.2.1 и 1.4.3. На слабых ПК не включайте: там это снижение и держит частоту кадров.",
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
    [UI_MENU_GRASS_RANGE] = "Дальность травы",
    [UI_MENU_GRASS_RANGE_DESC] = "Как далеко вокруг камеры растёт трава (в игре 62,5 м). Выше: трава на лугах видна "
                                 "дальше и не вырастает прямо перед вами; ниже: травы меньше. Действует после "
                                 "перезапуска.",
    [UI_MENU_DLSS_PRESET] = "Модель DLSS",
    [UI_MENU_DLSS_PRESET_DESC] = "Какая модель DLSS работает. «Автоматически» следует совету NVIDIA для каждого режима: K "
                                 "для DLAA, «Качества» и «Баланса», M для «Производительности», L для «Макс. "
                                 "производительности». J: чуть меньше шлейфов за движущимися персонажами, немного больше "
                                 "мерцания. K: самая чёткая модель во всех режимах. Действует сразу.",
    [UI_MENU_DLSS_PRESET_DEFAULT] = "Автоматически", [UI_MENU_DLSS_PRESET_K] = "K (самая чёткая)",
    [UI_MENU_DLSS_PRESET_J] = "J (меньше шлейфов)",
    [UI_MENU_DLSS_CHARS] = "Шлейфы персонажей в DLSS",
    [UI_MENU_DLSS_CHARS_DESC] = "Убирает слабые шлейфы за движущимися персонажами, животными и тем, что они держат: DLSS "
                                "берёт там меньше от прошлых кадров. Чем сильнее, тем меньше шлейфов и тем чуть менее "
                                "гладкие края. Действует сразу.",
    [UI_MENU_DLSS_CHARS_OFF] = "Выкл.", [UI_MENU_DLSS_CHARS_LIGHT] = "Слабо", [UI_MENU_DLSS_CHARS_MEDIUM] = "Средне",
    [UI_MENU_DLSS_CHARS_FULL] = "Полностью",
    [UI_MENU_DLSS] = "Режим апскейлера",
    [UI_MENU_DLSS_DESC] = "«Родное» сглаживает 3D-сцену в полном разрешении. «Качество», «Баланс», «Производительность» и "
                          "«Макс. производительность» рисуют её в 67, 58, 50 и 33 % и масштабируют апскейлером (DLSS "
                          "или FSR 3.1), что экономит время видеокарты; они заменяют масштаб 3D-отрисовки. Действует "
                          "сразу.",
    [UI_MENU_DLSS_DLAA] = "Родное", [UI_MENU_DLSS_QUALITY] = "Качество", [UI_MENU_DLSS_BALANCED] = "Баланс",
    [UI_MENU_DLSS_PERFORMANCE] = "Производительность", [UI_MENU_DLSS_ULTRA] = "Макс. производительность",
    [UI_MENU_FXAA] = "Сглаживание игры (FXAA)",
    [UI_MENU_FXAA_DESC] = "Собственное сглаживание FXAA. Слегка размывает мелкие детали; с DLAA оно не нужно.",
    [UI_MENU_REFLEX] = "Низкая задержка",
    [UI_MENU_REFLEX_DESC] = "Снижает задержку системы. «Вкл» включает NVIDIA Reflex (VK_NV_low_latency2); «Вкл + Ускорение» также удерживает высокие частоты GPU NVIDIA при упоре в процессор. «Авто» включает Reflex на NVIDIA и AMD Anti-Lag 2 (VK_AMD_anti_lag) на AMD. Применяется после перезапуска.",
    [UI_MENU_REFLEX_AUTO] = "Авто",
    [UI_MENU_REFLEX_OFF] = "Выкл",
    [UI_MENU_REFLEX_ON] = "Вкл",
    [UI_MENU_REFLEX_BOOST] = "Вкл + Ускорение",
    [UI_MENU_SCENE_UPSCALER] = "Апскейлер 3D-сцены",
    [UI_MENU_SCENE_UPSCALER_DESC] = "Временной апскейлер для 3D-сцены: ровнее края и меньше мерцания, а в режимах ниже «Родного» он "
                                    "рисует сцену меньше и увеличивает её, что экономит время видеокарты. NVIDIA DLSS "
                                    "нужна видеокарта NVIDIA RTX и " DLSS_FILE " рядом с " EXE_FILE "; AMD FSR 3.1 "
                                    "работает на любой видеокарте; AMD FSR 4 (Windows) нужна видеокарта Radeon, для "
                                    "которой AMD его предлагает, и " FFX_FILES " рядом с " EXE_FILE ". «Выкл» оставляет собственное масштабирование игры и "
                                    "FXAA. Интерфейс не затрагивается; масштабирование и резкость выше по-прежнему "
                                    "подгоняют изображение игры под окно. Действует после перезапуска.",
    [UI_MENU_SCENE_UPSCALER_OFF] = "Выкл", [UI_MENU_SCENE_UPSCALER_DLSS] = "NVIDIA DLSS",
    [UI_MENU_SCENE_UPSCALER_FSR3] = "AMD FSR 3.1", [UI_MENU_SCENE_UPSCALER_FSR4] = "AMD FSR 4",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_GPU] = "FSR 4: недоступен для этой видеокарты",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_DLL] = "FSR 4: нет библиотек FSR от AMD",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_OS] = "FSR 4: только Windows",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_GPU] = "DLSS: нужна видеокарта NVIDIA RTX",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_DLL] = "DLSS: нет " DLSS_FILE,
    [UI_MENU_FSR_SHARPNESS] = "Резкость FSR",
    [UI_MENU_FSR_SHARPNESS_DESC] = "Повышение резкости 3D-сцены после AMD FSR 3.1 или FSR 4; 0 выключает его.",

    [UI_MENU_FPS_LIMIT] = "Ограничение частоты кадров",
    [UI_MENU_FPS_LIMIT_DESC] = "Наибольшее число кадров в секунду. Выше 60 — экспериментальный режим, нужна «Скорость "
                               "игры по часам».",
    [UI_MENU_FPS_30] = "30 FPS", [UI_MENU_FPS_40] = "40 FPS", [UI_MENU_FPS_45] = "45 FPS",
    [UI_MENU_FPS_60] = "60 FPS", [UI_MENU_FPS_90] = "90 FPS", [UI_MENU_FPS_120] = "120 FPS", [UI_MENU_FPS_144] = "144 FPS",
    [UI_MENU_FPS_DISPLAY] = "Как у монитора", [UI_MENU_FPS_DISPLAY_HZ] = "Как у монитора (%.0f Гц)",
    [UI_MENU_FRAME_QUEUE] = "Очередь кадров",
    [UI_MENU_FRAME_QUEUE_DESC] = "Игра начинает кадр, пока предыдущий ждёт показа: намного меньше пропущенных кадров на "
                                 "лимите, задержка ввода больше не более чем на кадр. Применяется сразу.",
    [UI_MENU_FRAME_GEN] = "Генерация кадров", [UI_MENU_FRAME_GEN_OFF] = "Выкл.", [UI_MENU_FRAME_GEN_FSR3] = "AMD FSR 3",
    [UI_MENU_FRAME_GEN_DESC] = "Сгенерированный кадр между каждыми двумя, что рисует игра: на экране вдвое больше "
                               "кадров, игра идёт на половине лимита, управление отвечает примерно на полкадра позже. "
                               "Для мониторов от 120 Гц. Не работает с DLSS и HDR. Применяется сразу.",
    [UI_MENU_REALTIME] = "Скорость игры по часам",
    [UI_MENU_REALTIME_DESC] = "Игра идёт по реальному времени, медленный кадр её не замедляет.",
    [UI_MENU_CLOTH_REALTIME] = "Ткань по часам",
    [UI_MENU_CLOTH_REALTIME_DESC] = "Ткань и волосы тоже идут по реальному времени.",
    [UI_MENU_HIGHFPS_FIX] = "Исправления для высокого FPS",
    [UI_MENU_HIGHFPS_FIX_DESC] = "Исправляет физику, которая ломается при частоте выше 30 кадров в секунду, и объекты, которые выше 60 остаются полупрозрачными или невидимыми.",
    [UI_MENU_CLIMB_FIX] = "Исправление скорости лазания",
    [UI_MENU_CLIMB_FIX_DESC] = "Сохраняет нормальную скорость лазания по стенам и лестницам выше 60 кадров в секунду, чтобы Линк добирался до верха.",
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

    [UI_MENU_PLAY_TIME] = "Время в игре",
    [UI_MENU_PLAY_TIME_DESC] = "Время в игре за все запуски. Считается, пока окно игры в фокусе и игра рисует кадры. "
                               "Хранится в playtime.txt в папке профиля, рядом с сохранениями.",
    [UI_MENU_PLAY_TIME_HM] = "%llu ч %02u мин", [UI_MENU_PLAY_TIME_SESSION] = "в этом запуске: %s",
    [UI_MENU_MODS_BUILTIN] = "Встроенные в порт", [UI_MENU_MODS_INSTALLED] = "Установленные моды",
    [UI_MENU_MODS_NONE] = "Модов пока нет. Распакуйте мод в папку mods, каждый мод в свою папку с папкой romfs внутри, и "
                          "перезапустите игру. TKMM тоже может выгрузить туда объединённые моды отдельной папкой.",
    [UI_MENU_MODS_OPEN] = "Открыть папку модов",
    [UI_MENU_MODS_OPEN_DESC] = "Показывает папку, из которой игра загружает моды. Если её нет, она будет создана.",
    [UI_MENU_MODS_ROW_DESC] = "Заменяет файлов игры: %u. Добавляет новых файлов: %u.",
    [UI_MENU_MODS_OVERRIDDEN] = "%u его файлов берутся из мода %s, он выше в списке.",
    [UI_MENU_MODS_MOVE_HINT] = "Верхняя кнопка или Ctrl+Вверх поднимает мод, левая кнопка или Ctrl+Вниз опускает. Если "
                               "два мода меняют один файл, данные или код игры, побеждает верхний. Изменения действуют "
                               "после перезапуска.",
    [UI_MENU_MODS_EMPTY] = "В этой папке нет файлов romfs, exefs или cheats.",
    [UI_MENU_MODS_SUBSDK] = "Этот мод приносит свой код игры (плагин subsdk или заменённый main). Порт не загружает "
                            "модули кода, поэтому мод выключен целиком. Его переключатель «... без его кода» ниже "
                            "включает остальную часть мода.",
    [UI_MENU_MODS_EXEFS_LATER] = "Этот мод меняет код игры (exefs). Игра соберёт эти патчи при следующем запуске "
                                 "(пересборка на несколько минут); до тех пор мод выключен целиком.",
    [UI_MENU_MODS_CHEATS] = "Читов: %d. Они включаются на вкладке «Читы».",
    [UI_MENU_MODS_CHEATS_FILE] = "Читы для этой версии игры читаются при запуске из cheats\\%s в этом моде.",
    [UI_MENU_MODS_BAD_NAME] = "В имени папки есть символ |, которым список модов разделяет имена. Переименуйте папку.",
    [UI_MENU_MODS_OTHER_VERSION] = "Файлов с другой версией игры в имени: %u. Игра читает их, только когда что-то "
                                   "запрашивает эти имена. Если мод не работает, поищите его версию для вашей версии "
                                   "игры.",
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
    [UI_MENU_VOLUME_MUSIC] = "Громкость музыки", [UI_MENU_VOLUME_MUSIC_DESC] = "Громкость фоновой музыки.",
    [UI_MENU_VOLUME_SFX] = "Громкость эффектов", [UI_MENU_VOLUME_SFX_DESC] = "Громкость звуковых эффектов.",
    [UI_MENU_VOLUME_VOICE] = "Громкость голосов", [UI_MENU_VOLUME_VOICE_DESC] = "Громкость голосов персонажей.",
    [UI_MENU_AUDIO_CHANNELS] = "Динамики",
    [UI_MENU_AUDIO_CHANNELS_DESC] = "Стерео или объёмный звук 5.1. Автоматически выбирается 5.1, если в Windows "
                                    "настроено шесть каналов или больше. Применяется после перезапуска.",
    [UI_MENU_CHANNELS_AUTO] = "Автоматически", [UI_MENU_CHANNELS_STEREO] = "Стерео", [UI_MENU_CHANNELS_51] = "Объёмный 5.1",
    [UI_MENU_AUDIO_DEVICE] = "Устройство вывода",
    [UI_MENU_AUDIO_DEVICE_DESC] = "Куда выводится звук. Системное по умолчанию следует за устройством по умолчанию "
                                  "в Windows. Если выбранное устройство отключить, звук идёт на системное, пока оно "
                                  "не вернётся. Применяется сразу.",
    [UI_MENU_AUDIO_DEVICE_DEFAULT] = "Системное по умолчанию",

    [UI_MENU_MOTION] = "Управление движением", [UI_MENU_MOTION_DESC] = "Прицеливание гироскопом контроллера.",
    [UI_MENU_GYRO_CALIBRATION] = "Калибровка смещения гироскопа",
    [UI_MENU_GYRO_CALIBRATION_DESC] = "Убирает покоящееся смещение контроллера, измеренное строкой калибровки ниже. "
                                      "Измеренные значения сохраняются и при выключенном состоянии.",
    [UI_MENU_GYRO_CALIBRATE] = "Калибровка гироскопа",
    [UI_MENU_GYRO_CALIBRATE_DESC] = "Измеряет смещение гироскопа в покое и убирает его. Положите контроллер или "
                                    "держите его неподвижно две секунды.",
    [UI_MENU_GYRO_CALIBRATE_WAIT] = "Калибровка...",
    [UI_MENU_GYRO_CALIBRATED] = "%s, откалиброван",
    [UI_MENU_GYRO_CALIBRATE_FAILED] = "Калибровка не удалась",
    [UI_MENU_GYRO_CALIBRATE_MOVED] = "Контроллер сдвинулся: держите его неподвижно и повторите",
    [UI_MENU_GYRO_AUTO_BIAS] = "Учёт дрейфа гироскопа в покое",
    [UI_MENU_GYRO_AUTO_BIAS_DESC] = "Измеряет остаточный дрейф гироскопа, пока контроллер лежит на столе, и вычитает "
                                    "его. Для прицела лука, который медленно уплывает даже после калибровки. В руке "
                                    "контроллер не измеряется. Экспериментально.",
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
                             "подсказки A показывают B). PlayStation: символы геймпада PlayStation (○ × △ □, L1, R1, "
                             "L2, R2, Options и Share) на кнопке, которую нажимает раскладка выше. PC: клавиши и "
                             "кнопки мыши стандартной раскладки клавиатуры (E, Shift, пробел, левый клик...) и мышь "
                             "для камеры. Игра читает подсказки при запуске, так что они меняются после перезапуска. "
                             "Подсказки записаны в файлах игры, поэтому у кнопки, переназначенной на вкладке "
                             "управления, на экране остаётся прежняя подсказка.",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo", [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation", [UI_MENU_PROMPTS_PC] = "PC",
    [UI_MENU_PROMPTS_MISSING] = "нет файлов подсказок: запустите установку снова",
    [UI_MENU_PROMPTS_FAILED] = "файлы подсказок повреждены: запустите установку снова",
    [UI_MENU_PROMPTS_RESTART] = "подсказки сменятся после перезапуска",
    [UI_MENU_PROMPTS_OFF_SHAPE] = "выкл. при этом разрешении игры (для него нет раскладки)",
    [UI_MENU_OUTPUT_RES_NO_LAYOUT] = "для этой формы нет раскладки: запустите установщик снова",
    [UI_MENU_OUTPUT_RES_PREPARING] = "раскладка создаётся в фоне; будет при следующем запуске",
    [UI_MENU_PROMPTS_PREPARING] = "файлы подсказок создаются в фоне; будут при следующем запуске",
    [UI_MENU_PREPARE_RETRY] = "Повторить",
    [UI_MENU_PREPARE_RETRY_DESC] = "Снова запускает инструменты установщика, чтобы создать в фоне недостающую раскладку интерфейса или файлы подсказок кнопок. Игра продолжает работать; они будут со следующего запуска.",
    [UI_MENU_SHAPE_PREPARE_FAILED] = "инструменты установщика не запустились: запустите установщик снова",
    [UI_MENU_PROMPTS_OFF_MODS] = "выкл.: мод RomFS заменяет их файлы",
    [UI_MENU_PROMPTS_OFF_PATCHES] = "выкл., пока задан TOTK_ROMFS_PATCHES",
    [UI_MENU_RESTART] = "Перезапустить сейчас",
    [UI_MENU_RESTART_DESC] = "Закрывает игру и запускает её снова с новыми подсказками. Прогресс после последнего "
                             "сохранения будет потерян.",
    [UI_MENU_RESTART_CONFIRM] = "Перезапустить игру сейчас? Прогресс после последнего сохранения будет потерян.",
    [UI_MENU_RESTART_YES] = "Перезапустить",
    [UI_MENU_RESTART_SAVING] = "Игра сохраняется: перезапуск будет доступен через мгновение.",
    [UI_MENU_GROUP_STICKS] = "Стики",
    [UI_MENU_LSTICK_DEADZONE] = "Мёртвая зона левого стика", [UI_MENU_RSTICK_DEADZONE] = "Мёртвая зона правого стика",
    [UI_MENU_STICK_DEADZONE_DESC] = "Насколько нужно отклонить стик геймпада, чтобы игра это заметила. Увеличьте, если персонаж или камера двигаются сами: отпустите стик, посмотрите «стик сейчас» и поставьте мёртвую зону на несколько процентов выше.",
    [UI_MENU_STICK_DEADZONE_NOW] = "стик сейчас: %.0f %%",
    [UI_MENU_MOUSE_MODE] = "Управление мышью",
    [UI_MENU_MOUSE_MODE_DESC] = "Прямое: камера поворачивается на столько, на сколько сдвинута мышь, как в играх для ПК, без мёртвой зоны и кривой стика. Эмуляция стика: мышь работает как правый стик.",
    [UI_MENU_MOUSE_STICK] = "Эмуляция стика",
    [UI_MENU_MOUSE_DIRECT] = "Прямое",
    [UI_MENU_MENU_CURSOR] = "Мышь в меню игры",
    [UI_MENU_MENU_CURSOR_DESC] = "В меню самой игры (заставка, инвентарь, магазины, варианты ответа) появляется указатель: пункт под ним выбирается, левый щелчок подтверждает, правый возвращает назад. Только если последними использовались мышь или клавиатура, поэтому с геймпадом меню не меняются (версия игры 1.4.3).",
    [UI_MENU_MAP_CLICK] = "Левый щелчок на карте",
    [UI_MENU_MAP_CLICK_DESC] = "На карте игры левая кнопка мыши нажимает A у курсора карты, а не назначенную ей кнопку (по умолчанию Y). Нужна «Мышь в меню игры» (версия игры 1.4.3).",
    [UI_MENU_DIALOGUE_CLICK] = "Щелчки в диалоге",
    [UI_MENU_DIALOGUE_CLICK_DESC] = "Пока открыто окно диалога, левый щелчок нажимает B (продолжает текст и пропускает его прокрутку), а правый — A, вместо назначенных им кнопок (по умолчанию Y, она открывает журнал диалога, и ZL). В вариантах ответа по-прежнему работает указатель: левый щелчок выбирает (версия игры 1.4.3).",
    [UI_MENU_POINTER_IMAGE] = "Игровой курсор",
    [UI_MENU_POINTER_IMAGE_DESC] = "Пока курсор не захвачен (меню игры, это меню, после Esc), окно показывает собственный золотой наконечник вместо системной стрелки, по масштабу экрана.",
    [UI_MENU_QUICKSAVE] = "Быстрое сохранение и загрузка",
    [UI_MENU_QUICKSAVE_DESC] = "Одна клавиша сразу сохраняет игру, другая загружает это сохранение (две строки ниже: по умолчанию F6 и F7, как в Half-Life), без всяких меню на экране. Только клавиатура и мышь, только версия игры 1.4.3. Быстрое сохранение просит игру сделать одно из её собственных автосохранений, поэтому заменяет самое старое автосохранение (ручное сохранение остаётся), держит его копию как быстрое сохранение (папка quicksave профиля) и пишет «Игра сохранена» вверху экрана. Быстрая загрузка держит картинку с надписью «Загрузка быстрого сохранения», пока за кадром проходит список загрузки игры, и игра загружает быстрое сохранение (около 5 с). Где игра сама не стала бы автосохраняться (сценка, диалог), ничего не сохраняется, и надпись говорит об этом.",
    [UI_MENU_QUICKSAVE_KEY] = "Клавиша быстрого сохранения",
    [UI_MENU_QUICKLOAD_KEY] = "Клавиша быстрой загрузки",
    [UI_MENU_QUICKSAVE_KEY_DESC] = "Клавиша, которая запускает быстрое сохранение или быструю загрузку; по умолчанию F6 и F7, как в Half-Life. Выберите ячейку и нажмите любую клавишу или кнопку мыши; Delete оставляет действие без клавиши. Пока быстрое сохранение включено, эти две клавиши больше ничего не делают (если клавиша занята в раскладке, это написано под значением). В totk.ini подходит любое имя клавиши из раскладки (quicksave_key, quickload_key).",
    [UI_QS_SAVED] = "Игра сохранена",
    [UI_QS_LOADING] = "Загрузка быстрого сохранения",
    [UI_QS_NOT_NOW] = "Сейчас быстрое сохранение невозможно",
    [UI_QS_NONE] = "Быстрого сохранения ещё нет (его делает клавиша быстрого сохранения)",
    [UI_QS_FAILED] = "Быстрое сохранение не удалось: см. totk.log",
    [UI_QS_LOAD_FAILED] = "Быстрая загрузка не удалась: см. totk.log",
    [UI_MENU_MOUSE_SENS] = "Чувствительность мыши",
    [UI_MENU_MOUSE_SENS_DESC] = "Как быстро мышь поворачивает камеру.",
    [UI_MENU_WALK_KEY] = "Клавиша шага",
    [UI_MENU_WALK_KEY_DESC] = "Удерживайте: стики клавиатуры отклоняются частично, и персонаж идёт, а не бежит. "
                              "Насколько — задаёт сила шага. Контроллер и мышь не затрагиваются.",
    [UI_MENU_WALK_KEY_TAKEN] = "также %s",
    [UI_MENU_WALK_SCALE] = "Сила шага",
    [UI_MENU_WALK_SCALE_DESC] = "Отклонение стиков клавиатуры при удержании клавиши шага. Половина — шаг, полное "
                                "отклонение — бег.",
    [UI_MENU_FREECAM] = "Свободная камера",
    [UI_MENU_FREECAM_DESC] = "Позволяет сочетанию или клавише ниже отделить камеру от Линка и летать ею. По "
        "умолчанию выключена: тогда код камеры игры работает без изменений. Включается после "
        "перезапуска. Версия игры 1.4.3.",
    [UI_MENU_FREECAM_OFF_NOTE] = "Включите «Свободную камеру» выше и перезапустите игру",
    [UI_MENU_FREECAM_PAD] = "Сочетание свободной камеры",
    [UI_MENU_FREECAM_PAD_DESC] = "Кнопки геймпада, нажатые вместе, переключают камеру по трём режимам: она отделяется "
        "от Линка и летает, остаётся на месте, пока Линком снова можно управлять, и возвращается. Пока камера летает, "
        "игра не получает ввод: левый стик ведёт камеру вдоль взгляда и вбок, курки — вверх и вниз, правый стик "
        "поворачивает, а каждое нажатие L или R уменьшает или увеличивает скорость вдвое. Когда камера закреплена, "
        "Линк идёт туда, куда левый стик указывает на экране. Версия игры 1.4.3.",
    [UI_MENU_FREECAM_PAD_V143] = "Только для версии игры 1.4.3",
    [UI_MENU_FREECAM_KEY] = "Клавиша свободной камеры",
    [UI_MENU_FREECAM_KEY_DESC] = "Клавиша, которая переключает те же режимы: камера отделяется от Линка и летает, "
        "остаётся на месте, пока Линком снова можно управлять, и возвращается. Пока она летает: W A S D ведут камеру "
        "вдоль взгляда и вбок, клавиши ZR и ZL (по умолчанию X и Z) — вверх и вниз, мышь и стрелки поворачивают, а "
        "клавиши L и R (Q и R) уменьшают или увеличивают скорость вдвое. Версия игры 1.4.3.",
    [UI_MENU_GROUP_GAMEPLAY_TWEAKS] = "Геймплей",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED] = "Настройки геймплея",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED_DESC] = "Управление колесом мыши: вращение и дистанция предметов в Ультраруке, выбор оружия и предметов, плавный зум карты.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE] = "Вращение колесом (Ультрарука)",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE_DESC] = "Вращение удерживаемых Ультрарукой предметов колесом мыши без удержания R.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS] = "Ось C для вращения (Ультрарука)",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS_DESC] = "Удерживайте C при прокрутке колеса для вращения объекта вокруг третьей оси.",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY] = "Скорость вращения (Ультрарука)",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY_DESC] = "Как быстро колесо мыши поворачивает удерживаемый предмет.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE] = "Дистанция колесом (Ультрарука)",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE_DESC] = "Правая кнопка мыши + колесо приближает или отдаляет предмет.",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY] = "Скорость дистанции (Ультрарука)",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY_DESC] = "Как быстро колесо мыши меняет дистанцию до предмета.",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL] = "Выбор оружия колесом",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL_DESC] = "Прокрутите колесо в свободной игре для выбора оружия. Подтверждение клавишей E.",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY] = "Чувствительность выбора оружия",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY_DESC] = "Шаги колеса для переключения на следующее оружие.",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL] = "Выбор в быстром меню колесом",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL_DESC] = "Прокрутка колеса листает предметы в открытом меню быстрого выбора.",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY] = "Чувствительность быстрого меню",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY_DESC] = "Шаги колеса для перехода к следующему предмету в быстром меню.",
    [UI_MENU_GAMEPLAY_ITEM_AD] = "Навигация A / D в быстром меню",
    [UI_MENU_GAMEPLAY_ITEM_AD_DESC] = "Клавиши A / D для выбора влево и вправо в меню быстрого выбора.",
    [UI_MENU_GAMEPLAY_MAP_ZOOM] = "Плавный зум карты",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_DESC] = "Плавное инерционное приближение карты Планшета путеводителя колесом мыши.",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY] = "Скорость зума карты",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY_DESC] = "Как быстро колесо мыши масштабирует карту.",
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
    [UI_BIND_SHAKE] = "Встряхнуть стик камеры",
    [UI_BIND_DESC_SHAKE] = "Качает правый стик влево-вправо: отцепляет приклеенную деталь от того, что держит "
                           "Ультрарука. Пока клавиша зажата, тряска продолжается.",
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
    [UI_PAD_TOUCHPAD] = "Нажатие на тачпад",
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
    [UI_MENU_DISCORD_RPC] = "Discord Rich Presence",
    [UI_MENU_DISCORD_RPC_DESC] = "Пока игра запущена, показывает «Играю в TotK Recomp» со временем игры в статусе "
                                 "Discord. Приложение выбирают discord_app_id и discord_image в totk.ini.",

    [UI_MENU_ADV_WARNING] = "Эти настройки нужны для тестов и поиска неполадок. Меняйте их, только если знаете зачем.",
    [UI_MENU_GROUP_SCHED] = "Планирование", [UI_MENU_GROUP_SHADERS] = "Шейдеры и конвейеры",
    [UI_MENU_GROUP_TEXMEM] = "Текстуры и память", [UI_MENU_GROUP_FIXES] = "Исправления графики",
    [UI_MENU_GROUP_VULKAN] = "Vulkan", [UI_MENU_GROUP_DIAG] = "Диагностика",
    [UI_MENU_ASYNC_PREEMPT] = "Вытеснение на виртуальных ядрах",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "Позволяет потоку игры прервать другой поток на том же виртуальном ядре.",
    [UI_MENU_PREEMPT_OFF] = "Выкл.", [UI_MENU_PREEMPT_ON] = "Вкл.", [UI_MENU_PREEMPT_INHERIT] = "Только наследование",
    [UI_MENU_REPLAY_ASYNC] = "Подготовка кадра для GPU в отдельном потоке",
    [UI_MENU_REPLAY_ASYNC_DESC] = "Кадр для GPU готовится в отдельном потоке, а не в потоке игры. Добавляет до одного кадра задержки.",
    [UI_MENU_REPLAY_WORKERS] = "Потоки подготовки кадра для GPU",
    [UI_MENU_REPLAY_WORKERS_DESC] = "Большие кадры для GPU записываются сразу в нескольких потоках. Помогает, когда частоту кадров ограничивает процессор; нужно 8 логических процессоров или больше. «Выкл.» — запись в одном потоке.",
    [UI_MENU_REPLAY_WORKERS_NOW] = "сейчас: %s",
    [UI_MENU_TIMER_1MS] = "Таймер Windows 1 мс",
    [UI_MENU_TIMER_1MS_DESC] = "Повышает точность системного таймера до 1 мс для ровного темпа кадров.",
    [UI_MENU_SYNC_AHEAD] = "На кадр впереди видеокарты",
    [UI_MENU_SYNC_AHEAD_DESC] = "Игра готовит следующий кадр, пока видеокарта рисует предыдущий. Больше кадров в секунду, на один кадр больше задержка ввода.",
    [UI_MENU_HOST_CHECKPOINT] = "Быстрая передача ядра",
    [UI_MENU_HOST_CHECKPOINT_DESC] = "Поток игры отдаёт виртуальное ядро более важному потоку сразу при выходе из кода порта, а не на следующем системном вызове.",
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
    [UI_MENU_ASTC_BC] = "Сжатие ASTC в BC3", [UI_MENU_ASTC_BC_DESC] = "С декодером на GPU: декодированные текстуры ASTC пересжимаются в BC3 и занимают вчетверо меньше видеопамяти. Текстуры чуть более блочные. Авто: включено на видеокартах с 4 ГБ видеопамяти и меньше.",
    [UI_MENU_LOD_GUARD] = "Защита LOD", [UI_MENU_LOD_GUARD_DESC] = "Не даёт объектам мелькать не на своём месте, а дальнему плану — на кадр становиться плоским: ждёт, пока игра допишет данные уровней детализации, и только потом их читает GPU.",
    [UI_MENU_FIX_C0] = "Исправление константного буфера",
    [UI_MENU_FIX_C0_DESC] = "Заполняет зарезервированный драйвером банк констант c0 (размеры хранилищ, размер запуска, размер точки, базовый экземпляр) вместо чтения нулей.",
    [UI_MENU_FIX_ZERO] = "Обнулять новую память GPU",
    [UI_MENU_FIX_ZERO_DESC] = "Новые пулы памяти GPU заполняются нулями, как память Switch.",
    [UI_MENU_DLAA_EFFECTS_AFTER] = "Частицы после DLAA",
    [UI_MENU_DLAA_EFFECTS_AFTER_DESC] = "С DLAA пыль и другие частицы рисуются поверх картинки, уже сглаженной DLSS, "
                                        "как игра рисует их без DLSS: мелкие частицы в тумане (святилища) больше не "
                                        "пропадают и не оставляют шлейфов. Туман и облака по-прежнему проходят через "
                                        "DLSS, поэтому далёкие края на фоне неба остаются чистыми. Выкл.: DLSS "
                                        "обрабатывает и частицы, как раньше. Только в режиме DLAA; режимы DLSS с "
                                        "повышением разрешения обрабатывают их всегда. Применяется сразу.",
    [UI_MENU_DLSS_FIXED_EXPOSURE] = "Фиксированная экспозиция DLSS",
    [UI_MENU_DLSS_FIXED_EXPOSURE_DESC] = "DLSS берёт яркость картинки игры как есть, а не оценивает экспозицию по "
                                         "каждому кадру: в тёмных сценах (Глубины) большие участки в тумане больше не "
                                         "темнеют и не светлеют на кадр при движении камеры. Выкл.: DLSS сам "
                                         "оценивает экспозицию, как раньше. Применяется сразу.",
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
                                "выключен на драйвере AMD для RDNA 2 и старше, где с ним персонажи чёрные, и на "
                                "драйвере Intel для Windows на встроенных GPU, где с ним цветной шум на листве; "
                                "включён на остальных.",
    [UI_MENU_VK_SUBGROUP_PIN] = "Фиксировать размер подгруппы",
    [UI_MENU_VK_SUBGROUP_PIN_DESC] = "Фиксирует размер подгруппы шейдера.",
    [UI_MENU_TRACE] = "Журнал производительности", [UI_MENU_TRACE_DESC] = "Пишет время кадров в файл.",
    [UI_MENU_FLIGHT] = "Запись последних кадров (для отчёта о фризе)",
    [UI_MENU_FLIGHT_DESC] = "Хранит короткую историю и сохраняет её, когда кадр подвисает.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "Порог подвисания (мс)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "Время кадра, которое считается подвисанием.",
    [UI_MENU_PASS_CRUMBS] = "Метки проходов GPU",
    [UI_MENU_PASS_CRUMBS_DESC] = "После сбоя GPU журнал называет проход отрисовки, который выполнялся. Автоматически: только AMD. Вкл.: также NVIDIA, с небольшой нагрузкой.",
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
    [UI_MENU_OV_FPS_LABEL] = "FPS", [UI_MENU_OV_FRAME_LABEL] = "Кадр", [UI_MENU_OV_FMT_FPS] = "%.0f",
    [UI_MENU_OV_LOW1] = "1%% худших %.0f", [UI_MENU_OV_LOW01] = "0.1%% худших %.0f",
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
    [UI_MENU_OV_UPSCALER] = "3D-апскейлер",
    [UI_MENU_OV_SCALING] = "Масштабирование", [UI_MENU_OV_LIMIT] = "Лимит кадров", [UI_MENU_OV_PRESENT] = "Вывод кадров", [UI_MENU_OV_SHOWN] = "Показано",
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
    [UI_LW_STAGE_START] = "Подготовка",
    [UI_LW_STAGE_GPU] = "Запуск графики",
    [UI_LW_STAGE_FILES] = "Подготовка файлов игры",
    [UI_LW_STAGE_SHADERS] = "Загрузка шейдеров",
    [UI_LW_STAGE_PIPELINES] = "Сборка пайплайнов",
    [UI_LW_STAGE_GAME] = "Запуск игры",
    [UI_LW_FAILED] = "Игра остановилась",
    [UI_LW_FAILED_HINT] = "Причина — в логе ниже. Скопируйте его для отчёта об ошибке и закройте это окно.",
    [UI_LW_VERSION] = "v%s · игра %s",
    [UI_LW_COUNT] = "%s из %s",
    [UI_LW_LEFT_ITEMS] = "осталось %s",
    [UI_LW_LEFT_SEC] = "ещё примерно %d с",
    [UI_LW_LEFT_MIN] = "ещё примерно %d мин",
    [UI_LW_LEFT_FEW] = "ещё несколько секунд",
    [UI_LW_ALSO] = "Параллельно: %s",
    [UI_LW_GAME_SEC] = "Игра запустится примерно через %d с",
    [UI_LW_GAME_MIN] = "Игра запустится примерно через %d мин",
    [UI_LW_GAME_SOON] = "Игра вот-вот запустится",
    [UI_LW_ELAPSED] = "Прошло %d:%02d",
    [UI_LW_DETAILS] = "Подробнее",
    [UI_LW_HIDE] = "Скрыть",
    [UI_LW_COPY] = "Копировать лог",
    [UI_LW_COPIED] = "Скопировано",
    [UI_LW_CLOSE] = "Закрыть и выйти из игры",
    [UI_LW_THOUSANDS] = " ",
    [UI_MENU_CHEATS_REBUILD] = "Пересобрать игру сейчас",
    [UI_MENU_CHEATS_REBUILD_DESC] = "Некоторые читы меняют код игры и ещё не собраны в неё: они новые или изменены, "
                                    "либо прошлая пересборка не удалась или была отменена. Закрывает игру; установщик "
                                    "собирает их (несколько минут) и снова запускает игру. Прогресс после последнего "
                                    "сохранения будет потерян.",
    [UI_MENU_MODS_EXEFS_UNSUPPORTED] = "Этот мод меняет код игры (exefs) так, как порт применить не может, поэтому мод "
                                       "выключен целиком. Его переключатель «... без его кода» ниже включает остальную "
                                       "часть мода. %s",
    [UI_MENU_MODS_EXEFS_OTHER] = "Его патчи кода (exefs) сделаны для другой версии игры.",
    [UI_MENU_MODS_EXEFS_ON] = "Патчи кода в работе: файлов: %d.",
    [UI_MENU_MODS_REPLACES_BUILTIN] = "Патчи кода в работе: файлов: %d. Они заменяют собственный патч порта %s, "
                                      "который выключен, пока этот мод включён.",
    [UI_MENU_MODS_CODE_SKIPPED] = "Его собственный код игры (%s) не загружается: используется остальная часть мода. "
                                  "Выключите мод, если игра с ним ведёт себя странно.",
    [UI_MENU_MODS_PLUGIN_ON] = "Его плагин работает: перехватов в коде игры: %d.",
    [UI_MENU_MODS_PLUGIN_REFUSED] = "Его плагин %s не может работать в порте: %s.",
    [UI_MENU_MODS_PLUGIN_STAYS_OFF] = "Мод выключен целиком. Его переключатель «... без его кода» ниже включает "
                                      "остальную часть мода.",
    [UI_MENU_MODS_PLUGIN_OFF] = "Его плагин в этот раз выключен: он запустился не так, как при сборке (%s). Игра "
                                "пересоберёт его при следующем запуске.",
    [UI_MENU_MODS_WITHOUT_CODE] = "%s без его кода",
    [UI_MENU_MODS_WITHOUT_CODE_DESC] = "Мод выше приносит код, который порт не может запустить: свой код игры (плагин "
                                       "subsdk или заменённый main) или патчи кода (exefs), которые порт не может "
                                       "применить. Выкл.: мод выключен целиком. Вкл.: этот код пропускается, а "
                                       "остальная часть мода используется — его файлы RomFS, читы и работающие патчи "
                                       "кода. Его файлы могут рассчитывать на отсутствующий код; выключите снова, если "
                                       "игра ведёт себя странно. Применяется при следующем запуске.",
    [UI_MENU_MODS_WITHOUT_CODE_ALL] = "Включено для всех модов: mods_without_code = 1 в totk.ini.",
    [UI_MENU_MODS_EXEFS_SKIPPED] = "Патчи кода (exefs), которые порт не может применить, пропущены (файлов: %u): "
                                   "используется остальная часть мода. %s",
    [UI_MENU_MODS_OVERRIDES_FILES] = "%u его файлов заменяют файлы мода %s, он ниже в списке.",
    [UI_MENU_MODS_DATA_OVERRIDDEN] = "%u байт его изменений данных игры берутся из мода %s, он выше в списке.",
    [UI_MENU_MODS_OVERRIDES_DATA] = "%u байт его изменений данных игры заменяют изменения мода %s, он ниже в списке.",
    [UI_MENU_MODS_CODE_OVERRIDDEN] = "%u мест его патчей кода перекрыты модом %s, он выше в списке.",
    [UI_MENU_MODS_OVERRIDES_CODE] = "Он перекрывает %u мест патчей кода мода %s, он ниже в списке.",
};

static const char *const text_pt_br[UI_TEXT_COUNT] = {
    SWKBD("Inserir texto", "Insira um nome", "Cavalo", "OK", "Cancelar", "Controle: A = OK, B = Cancelar"),

    [UI_MENU_TITLE] = "Configurações",
    [UI_MENU_TAB_SCREEN] = "Tela", [UI_MENU_TAB_GAME] = "Jogo", [UI_MENU_TAB_SOUND] = "Som",
    [UI_MENU_TAB_CONTROLS] = "Controles", [UI_MENU_TAB_AMIIBO] = "Amiibo", [UI_MENU_TAB_MENU] = "Menu", [UI_MENU_TAB_ADVANCED] = "Avançado",
    [UI_MENU_TAB_MODS] = "Mods",
    [UI_MENU_PLAY_TIME] = "Tempo de jogo",
    [UI_MENU_PLAY_TIME_DESC] = "Tempo no jogo somando todas as sessões, contado enquanto a janela do jogo está em foco e o "
                               "jogo desenha quadros. Fica em playtime.txt na pasta do perfil, junto dos saves.",
    [UI_MENU_PLAY_TIME_HM] = "%llu h %02u min", [UI_MENU_PLAY_TIME_SESSION] = "nesta sessão: %s",
    [UI_MENU_MODS_BUILTIN] = "Embutidos no port", [UI_MENU_MODS_INSTALLED] = "Mods instalados",
    [UI_MENU_MODS_NONE] = "Nenhum mod ainda. Descompacte um mod na pasta mods, uma pasta por mod com a pasta romfs dentro, "
                          "e reinicie o jogo. O TKMM também pode exportar os mods mesclados para uma pasta ali.",
    [UI_MENU_MODS_OPEN] = "Abrir pasta de mods",
    [UI_MENU_MODS_OPEN_DESC] = "Mostra a pasta de onde o jogo carrega os mods. Ela é criada se não existir.",
    [UI_MENU_MODS_ROW_DESC] = "Arquivos do jogo que substitui: %u. Arquivos novos que adiciona: %u.",
    [UI_MENU_MODS_OVERRIDDEN] = "%u dos seus arquivos vêm de %s, que está acima na lista.",
    [UI_MENU_MODS_MOVE_HINT] = "O botão de cima ou Ctrl+Cima sobe o mod, o botão da esquerda ou Ctrl+Baixo desce. "
                               "Quando dois mods mudam o mesmo arquivo, dado ou código do jogo, vale o de cima. As "
                               "mudanças valem após reiniciar.",
    [UI_MENU_MODS_EMPTY] = "Esta pasta não tem arquivos romfs, exefs ou cheats.",
    [UI_MENU_MODS_SUBSDK] = "Este mod traz código próprio do jogo (um plugin subsdk ou um main substituído). O port "
                            "não carrega módulos de código, então o mod inteiro fica desligado. A chave \"Usar ... sem "
                            "o código dele\", abaixo, usa o resto dele.",
    [UI_MENU_MODS_EXEFS_LATER] = "Este mod altera o código do jogo (exefs). O jogo compila esses patches na próxima "
                                 "vez que iniciar (uma recompilação de alguns minutos); até lá o mod inteiro fica "
                                 "desligado.",
    [UI_MENU_MODS_CHEATS] = "Cheats: %d. Ligue-os na aba Cheats.",
    [UI_MENU_MODS_CHEATS_FILE] = "Os cheats desta versão do jogo são lidos ao iniciar de cheats\\%s neste mod.",
    [UI_MENU_MODS_BAD_NAME] = "O nome da pasta contém |, que a lista de mods usa entre os nomes. Renomeie a pasta.",
    [UI_MENU_MODS_OTHER_VERSION] = "%u dos seus arquivos trazem outra versão do jogo no nome. O jogo só os lê quando algo "
                                   "pede esses nomes. Se o mod não fizer nada, procure uma versão dele feita para a sua "
                                   "versão do jogo.",
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
    [UI_PERF_LINE_FPS_GEN] = "%.0f fps desenhados, %.0f exibidos",
    [UI_MENU_GRAPHICS] = "Imagem",
    [UI_MENU_OUTPUT_RES] = "Resolução do jogo",
    [UI_MENU_OUTPUT_RES_DESC] = "O tamanho em que o jogo desenha a imagem e a interface. Acima de 1920 x 1080 o jogo "
                                "precisa de mais memória de vídeo e tempo de GPU. Um tamanho mais largo (21:9) amplia a "
                                "visão da câmera e leva o HUD para os cantos da tela; um mais alto (16:10, 3:2) "
                                "leva-o para as bordas de cima e de baixo.",
    [UI_MENU_HUD_WIDTH] = "Largura do HUD",
    [UI_MENU_HUD_WIDTH_DESC] = "Em uma tela mais larga que 16:9: a distância entre os lados esquerdo e direito do HUD. "
                               "Tela cheia os coloca nas bordas da tela; 21:9 ou 16:9 mantém o HUD no meio da tela "
                               "desse formato. Um layout ausente é feito em segundo plano e usado depois de reiniciar o jogo.",
    [UI_MENU_HUD_WIDTH_FULL] = "Tela cheia",
    [UI_MENU_RENDER_SCALE] = "Escala da renderização 3D",
    [UI_MENU_RENDER_SCALE_DESC] = "Tamanho da cena 3D em relação à resolução do jogo; o FSR do jogo a amplia até esse "
                                  "tamanho. Acima de 100 % suaviza bordas serrilhadas (supersampling) com alto custo de "
                                  "GPU.",
    [UI_MENU_RENDER_SCALE_GAME] = "Jogo (83 %)",
    [UI_MENU_DYNAMIC_RES] = "Resolução dinâmica",
    [UI_MENU_DYNAMIC_RES_DESC] = "Deixa o jogo reduzir a cena 3D para 90 ou 80 %. Ele decide por uma estimativa feita para o Switch que ignora sua GPU, então cenas largas caem mesmo num PC rápido. Desligada, mantém a escala da renderização.",
    [UI_MENU_UPSCALER] = "Escala para a janela",
    [UI_MENU_UPSCALER_DESC] = "Como a imagem é ampliada quando a janela é maior que a resolução do jogo. O FSR mantém "
                              "bordas e detalhes mais nítidos que o bilinear.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1", [UI_MENU_UPSCALER_BILINEAR] = "Bilinear",
    [UI_MENU_ASPECT] = "Formato da imagem",
    [UI_MENU_ASPECT_DESC] = "Quando o formato da janela difere do da resolução do jogo (uma imagem 16:9 numa tela "
                            "21:9): manter o formato da imagem com barras pretas ou esticá-la sobre a janela.",
    [UI_MENU_ASPECT_FIT] = "Barras pretas", [UI_MENU_ASPECT_STRETCH] = "Esticar",
    [UI_MENU_MOVIE_ASPECT] = "Formato dos vídeos",
    [UI_MENU_MOVIE_ASPECT_DESC] = "Quando a resolução do jogo não é 16:9: manter o formato dos vídeos das cenas com "
                                  "barras pretas ou esticá-los sobre a imagem toda. Vale a partir do próximo vídeo.",
    [UI_MENU_MOVIE_ASPECT_KEEP] = "Barras pretas", [UI_MENU_MOVIE_ASPECT_STRETCH] = "Esticar",
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
    [UI_MENU_FRAME_QUEUE] = "Fila de quadros",
    [UI_MENU_FRAME_QUEUE_DESC] = "O jogo começa um quadro enquanto o anterior espera a vez de aparecer: muito menos quadros "
                                 "perdidos no limite, com até um quadro a mais de atraso na entrada. Vale na hora.",
    [UI_MENU_FRAME_GEN] = "Geração de quadros", [UI_MENU_FRAME_GEN_OFF] = "Desligado", [UI_MENU_FRAME_GEN_FSR3] = "AMD FSR 3",
    [UI_MENU_FRAME_GEN_DESC] = "Um quadro gerado entre cada dois que o jogo desenha: o dobro de quadros na tela, o "
                               "jogo na metade do limite e os controles cerca de meio quadro mais tarde. Para telas de "
                               "120 Hz ou mais. Não funciona com DLSS nem HDR. Vale na hora.",
    [UI_MENU_REALTIME] = "Velocidade do jogo pelo relógio",
    [UI_MENU_REALTIME_DESC] = "Avança o jogo pelo tempo real, para que um quadro lento não deixe o jogo mais lento.",
    [UI_MENU_CLOTH_REALTIME] = "Tecidos pelo relógio",
    [UI_MENU_CLOTH_REALTIME_DESC] = "Avança tecidos e cabelos pelo tempo real também.",
    [UI_MENU_HIGHFPS_FIX] = "Correções para alta taxa de quadros",
    [UI_MENU_HIGHFPS_FIX_DESC] = "Corrige a física que se comporta mal acima de 30 quadros por segundo e objetos que ficam transparentes ou invisíveis acima de 60.",
    [UI_MENU_CLIMB_FIX] = "Correção da velocidade de escalada",
    [UI_MENU_CLIMB_FIX_DESC] = "Mantém a escalada de paredes e escadas na velocidade normal acima de 60 quadros por segundo, para que Link chegue ao topo.",
    [UI_MENU_GAME_LANG] = "Idioma do jogo", [UI_MENU_GAME_LANG_DESC] = "Idioma dos textos e das vozes do jogo.",
    [UI_MENU_GAME_LANG_AUTO] = "Automático",
    [UI_MENU_OPERATION_MODE] = "Modo do console",
    [UI_MENU_OPERATION_MODE_DESC] = "Se o jogo acha que está no modo TV ou no modo portátil.",
    [UI_MENU_MODE_DOCKED] = "Modo TV", [UI_MENU_MODE_HANDHELD] = "Modo portátil",

    [UI_MENU_VOLUME] = "Volume", [UI_MENU_VOLUME_DESC] = "Volume geral do jogo.",
    [UI_MENU_VOLUME_MUSIC] = "Volume da música", [UI_MENU_VOLUME_MUSIC_DESC] = "Volume da música de fundo.",
    [UI_MENU_VOLUME_SFX] = "Volume dos efeitos", [UI_MENU_VOLUME_SFX_DESC] = "Volume dos efeitos sonoros.",
    [UI_MENU_VOLUME_VOICE] = "Volume das vozes", [UI_MENU_VOLUME_VOICE_DESC] = "Volume das vozes dos personagens.",
    [UI_MENU_AUDIO_CHANNELS] = "Alto-falantes",
    [UI_MENU_AUDIO_CHANNELS_DESC] = "Saída estéreo ou surround 5.1. Automático escolhe 5.1 quando a configuração de "
                                    "alto-falantes do Windows tem seis canais ou mais. Vale após reiniciar.",
    [UI_MENU_CHANNELS_AUTO] = "Automático", [UI_MENU_CHANNELS_STEREO] = "Estéreo", [UI_MENU_CHANNELS_51] = "Surround 5.1",
    [UI_MENU_AUDIO_DEVICE] = "Dispositivo de saída",
    [UI_MENU_AUDIO_DEVICE_DESC] = "Onde o som toca. Padrão do sistema segue o dispositivo padrão do Windows. Um "
                                  "dispositivo escolhido que for desconectado volta ao padrão do sistema até "
                                  "reaparecer. Vale na hora.",
    [UI_MENU_AUDIO_DEVICE_DEFAULT] = "Padrão do sistema",

    [UI_MENU_MOTION] = "Controles de movimento", [UI_MENU_MOTION_DESC] = "Mirar com o giroscópio do controle.",
    [UI_MENU_GYRO_CALIBRATION] = "Calibração do desvio do giroscópio",
    [UI_MENU_GYRO_CALIBRATION_DESC] = "Remove o desvio de repouso do controle, medido pela linha de calibração "
                                      "abaixo. Os valores medidos ficam salvos mesmo com isto desligado.",
    [UI_MENU_GYRO_CALIBRATE] = "Calibrar giroscópio",
    [UI_MENU_GYRO_CALIBRATE_DESC] = "Mede o desvio de repouso do giroscópio e o remove. Apoie o controle ou "
                                    "segure-o parado por dois segundos.",
    [UI_MENU_GYRO_CALIBRATE_WAIT] = "Calibrando...",
    [UI_MENU_GYRO_CALIBRATED] = "%s, calibrado",
    [UI_MENU_GYRO_CALIBRATE_FAILED] = "Falha na calibração",
    [UI_MENU_GYRO_CALIBRATE_MOVED] = "O controle se moveu: segure-o parado e tente de novo",
    [UI_MENU_GYRO_AUTO_BIAS] = "Aprender o desvio do giroscópio em repouso",
    [UI_MENU_GYRO_AUTO_BIAS_DESC] = "Mede o desvio que resta no giroscópio sempre que o controle fica parado sobre a "
                                    "mesa e o subtrai. Para a mira do arco que desliza devagar mesmo após a "
                                    "calibração. Na mão, o controle não é medido. Experimental.",
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
    [UI_MENU_GROUP_STICKS] = "Analógicos",
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
    [UI_BIND_SHAKE] = "Sacudir o analógico da câmera",
    [UI_BIND_DESC_SHAKE] = "Balança o analógico direito para os lados: solta uma peça fundida do que a Ultramão segura. "
                           "Segurar a tecla continua sacudindo.",
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
    [UI_PAD_TOUCHPAD] = "Clique do touchpad",
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
    [UI_MENU_DISCORD_RPC] = "Discord Rich Presence",
    [UI_MENU_DISCORD_RPC_DESC] = "Mostra \"Jogando TotK Recomp\" com o tempo de jogo no seu status do Discord "
                                 "enquanto o jogo roda. discord_app_id e discord_image no totk.ini escolhem a "
                                 "aplicação.",

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
    [UI_MENU_REPLAY_WORKERS] = "Threads de preparo de quadro da GPU",
    [UI_MENU_REPLAY_WORKERS_DESC] = "Grava quadros grandes para a GPU em várias threads ao mesmo tempo. Ajuda quando a CPU limita a taxa de quadros; precisa de 8 ou mais processadores lógicos. Desligado grava numa thread só.",
    [UI_MENU_REPLAY_WORKERS_NOW] = "agora: %s",
    [UI_MENU_TIMER_1MS] = "Timer do Windows de 1 ms",
    [UI_MENU_TIMER_1MS_DESC] = "Aumenta a resolução do timer do sistema para 1 ms, para um ritmo de quadros mais estável.",
    [UI_MENU_SYNC_AHEAD] = "Um quadro à frente da GPU",
    [UI_MENU_SYNC_AHEAD_DESC] = "O jogo prepara o próximo quadro enquanto a placa de vídeo ainda desenha o anterior. Mais quadros por segundo, um quadro a mais de atraso na entrada.",
    [UI_MENU_HOST_CHECKPOINT] = "Troca rápida de núcleo",
    [UI_MENU_HOST_CHECKPOINT_DESC] = "Uma thread do jogo cede seu núcleo virtual a uma mais importante assim que sai do código do port, e não na próxima chamada de sistema.",
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
    [UI_MENU_ASTC_BC] = "Comprimir ASTC em BC3", [UI_MENU_ASTC_BC_DESC] = "Com o decodificador da GPU: recodifica as texturas ASTC decodificadas em BC3, que ocupa um quarto da memória de vídeo. As texturas ficam um pouco mais quadriculadas. Automático: ligado em GPUs com 4 GB de memória de vídeo ou menos.",
    [UI_MENU_LOD_GUARD] = "Proteção de LOD", [UI_MENU_LOD_GUARD_DESC] = "Evita que objetos pisquem no lugar errado ou que o plano distante fique achatado por um quadro, esperando o jogo terminar de reescrever os dados de nível de detalhe antes que a GPU os leia.",
    [UI_MENU_FIX_C0] = "Correção do buffer de constantes",
    [UI_MENU_FIX_C0_DESC] = "Preenche o banco de constantes c0 reservado ao driver (tamanhos de armazenamento, tamanho do dispatch, tamanho do ponto, instância base) em vez de ler zeros.",
    [UI_MENU_FIX_ZERO] = "Zerar memória nova da GPU",
    [UI_MENU_FIX_ZERO_DESC] = "Novos pools de memória da GPU começam zerados, como a memória do Switch.",
    [UI_MENU_DLAA_EFFECTS_AFTER] = "Partículas após o DLAA",
    [UI_MENU_DLAA_EFFECTS_AFTER_DESC] = "Com DLAA, a poeira e outras partículas são desenhadas sobre a imagem já "
                                        "suavizada pelo DLSS, como o jogo as desenha sem DLSS: partículas pequenas na "
                                        "névoa (santuários) não somem mais nem deixam rastros. A névoa e as nuvens "
                                        "continuam passando pelo DLSS, então as bordas distantes contra o céu ficam "
                                        "limpas. Desligado: o DLSS também processa as partículas, como antes. Só no "
                                        "modo DLAA; os modos de DLSS que aumentam a resolução sempre as processam. "
                                        "Aplica-se na hora.",
    [UI_MENU_DLSS_FIXED_EXPOSURE] = "Exposição fixa do DLSS",
    [UI_MENU_DLSS_FIXED_EXPOSURE_DESC] = "O DLSS usa o brilho da imagem do jogo como ele é, em vez de estimar a "
                                         "exposição a cada quadro: em cenas escuras (as Profundezas), grandes áreas com "
                                         "névoa não ficam mais escuras ou claras por um quadro enquanto a câmera se "
                                         "move. Desligado: o DLSS estima a exposição sozinho, como antes. Aplica-se na "
                                         "hora.",
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
                                "para RDNA 2 e anteriores, que com ele desenha os personagens pretos, e no driver da "
                                "Intel para Windows em GPUs integradas, que desenha ruído colorido na folhagem; "
                                "ligado nos demais.",
    [UI_MENU_VK_SUBGROUP_PIN] = "Fixar tamanho do subgrupo",
    [UI_MENU_VK_SUBGROUP_PIN_DESC] = "Fixa o tamanho do subgrupo dos shaders.",
    [UI_MENU_TRACE] = "Registro de desempenho", [UI_MENU_TRACE_DESC] = "Grava os tempos de quadro num arquivo.",
    [UI_MENU_FLIGHT] = "Caixa-preta",
    [UI_MENU_FLIGHT_DESC] = "Guarda um histórico curto e o salva quando um quadro trava.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "Limite de travamento (ms)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "Tempo de quadro que conta como travamento.",
    [UI_MENU_PASS_CRUMBS] = "Marcas de passagem da GPU",
    [UI_MENU_PASS_CRUMBS_DESC] = "Após uma falha da GPU, o log indica a passagem de renderização que estava em execução. Automático: só AMD. Ligado: também NVIDIA, com um pequeno custo.",
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
    [UI_MENU_OV_FPS_LABEL] = "FPS", [UI_MENU_OV_FRAME_LABEL] = "Quadro", [UI_MENU_OV_FMT_FPS] = "%.0f",
    [UI_MENU_OV_LOW1] = "1%% piores %.0f", [UI_MENU_OV_LOW01] = "0.1%% piores %.0f",
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
    [UI_MENU_OV_UPSCALER] = "Upscaler 3D",
    [UI_MENU_OV_SCALING] = "Escala", [UI_MENU_OV_LIMIT] = "Limite de quadros", [UI_MENU_OV_PRESENT] = "Apresentação", [UI_MENU_OV_SHOWN] = "Exibidos",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u", [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, nitidez %d%%", [UI_MENU_OV_SCALE_NONE] = "nenhuma (1:1)",
    [UI_MENU_OV_SCALE_BILINEAR] = "bilinear",
    [UI_MENU_FOV] = "Campo de visão",
    [UI_MENU_FOV_DESC] = "Quanto do mundo a câmera mostra, em porcentagem do original do jogo. Nas versões 1.1.0, 1.2.1 e 1.4.3 "
                         "do jogo.",
    [UI_MENU_VERT_PLUS] = "Manter a largura de 16:9",
    [UI_MENU_VERT_PLUS_DESC] = "Em telas mais estreitas que 16:9 (Steam Deck, 16:10, 4:3), a câmera mostra a mesma largura do mundo que em 16:9, e mais acima e abaixo. Sem efeito em 16:9 ou mais largo.",
    [UI_MENU_SHADOW_RES] = "Tamanho do mapa de sombras",
    [UI_MENU_SHADOW_RES_DESC] = "Tamanho do mapa de sombras do sol. O tamanho do jogo muda com a vista, até cerca de "
                                "400 perto do chão, então as sombras das folhas ficam quadriculadas e pulam; um "
                                "tamanho fixo as deixa paradas e mais nítidas. Acima de 1024 as sombras custam mais "
                                "tempo de GPU e memória de vídeo; 256 é mais grosseiro que o do jogo e serve para GPUs "
                                "integradas com pouca largura de banda de memória.",
    [UI_MENU_SHADOW_RES_GAME] = "Do jogo (varia)",
    [UI_MENU_ANISO] = "Filtragem de texturas",
    [UI_MENU_ANISO_DESC] = "Filtragem anisotrópica das texturas de superfície: chão, estradas e paredes vistos em "
                           "ângulo raso continuam nítidos ao longe em vez de borrar. Jogo: o nível que o jogo pede, "
                           "como no Switch.",
    [UI_MENU_ANISO_GAME] = "Jogo",
    [UI_MENU_LOD_STEADY] = "Detalhe estável sob carga",
    [UI_MENU_LOD_STEADY_DESC] = "Pula a configuração do jogo que reduz o detalhe de modelos e texturas quando a taxa "
                                "de quadros cai, como faz o mod de emulador Disable LOD Quality Reduction. Só nas "
                                "versões 1.1.0, 1.2.1 e 1.4.3 do jogo. Deixe desligado em PCs fracos: lá essa redução mantém a taxa de quadros.",
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
                             "letra B, então os ícones de A mostram B). PlayStation: os símbolos de um controle "
                             "PlayStation (○ × △ □, L1, R1, L2, R2, Options e Share), no botão que o layout acima "
                             "aperta. PC: as teclas e os botões do mouse do layout padrão do teclado (E, Shift, "
                             "Espaço, clique esquerdo...) e o mouse para a câmera. O jogo lê os ícones ao iniciar, "
                             "então eles mudam depois de reiniciar. Eles fazem parte dos arquivos do jogo, então um "
                             "botão remapeado em Controles continua com o ícone antigo na tela.",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo",
    [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation",
    [UI_MENU_PROMPTS_PC] = "PC",
    [UI_MENU_MOUSE_MODE] = "Câmera com o mouse",
    [UI_MENU_MOUSE_MODE_DESC] = "Direta: a câmera gira o quanto o mouse se move, como num jogo de PC, sem zona morta nem curva do analógico. Emulação do analógico: o mouse age como o analógico direito.",
    [UI_MENU_MOUSE_STICK] = "Emulação do analógico",
    [UI_MENU_MOUSE_DIRECT] = "Direta",
    [UI_MENU_MENU_CURSOR] = "Mouse nos menus do jogo",
    [UI_MENU_MENU_CURSOR_DESC] = "Nos menus do próprio jogo (título, inventário, lojas, escolhas de diálogo) o ponteiro aparece: o item sob ele fica selecionado, o clique esquerdo confirma e o direito volta. Só depois de usar o mouse ou uma tecla por último, então com um controle os menus ficam como antes (jogo 1.4.3).",
    [UI_MENU_MAP_CLICK] = "Clique esquerdo no mapa",
    [UI_MENU_MAP_CLICK_DESC] = "No mapa do jogo, o botão esquerdo do mouse aperta A no cursor do mapa, em vez do botão ao qual está atribuído (Y por padrão). Requer Mouse nos menus do jogo (jogo 1.4.3).",
    [UI_MENU_DIALOGUE_CLICK] = "Cliques no diálogo",
    [UI_MENU_DIALOGUE_CLICK_DESC] = "Enquanto uma caixa de diálogo está aberta, o clique esquerdo aperta B (continua o texto e pula a rolagem) e o direito aperta A, em vez dos botões aos quais estão atribuídos (Y por padrão, que abre o registro do diálogo, e ZL). As escolhas de diálogo continuam com o ponteiro: o clique esquerdo escolhe (jogo 1.4.3).",
    [UI_MENU_POINTER_IMAGE] = "Ponteiro do jogo",
    [UI_MENU_POINTER_IMAGE_DESC] = "Enquanto o cursor não está capturado (os menus do jogo, este menu, depois de Esc), a janela mostra uma ponta de flecha dourada própria em vez da seta do sistema, no tamanho da escala da tela.",
    [UI_MENU_QUICKSAVE] = "Salvamento e carregamento rápidos",
    [UI_MENU_QUICKSAVE_DESC] = "Uma tecla salva o jogo na hora e outra carrega esse salvamento (as duas linhas abaixo: F6 e F7 por padrão, como no Half-Life), sem nenhum menu na tela. Só teclado e mouse, só o jogo 1.4.3. O salvamento rápido pede ao jogo um dos seus próprios salvamentos automáticos, então substitui o salvamento automático mais antigo (o salvamento manual fica), guarda uma cópia dele como salvamento rápido (a pasta quicksave do perfil) e mostra \"Jogo salvo\" no alto da imagem. O carregamento rápido segura a imagem em \"Carregando o salvamento rápido\" enquanto passa pela lista de carregamento do jogo fora de vista, e o jogo carrega o salvamento rápido (cerca de 5 s). Onde o próprio jogo não faria um salvamento automático (uma cena, um diálogo), nada é salvo e a linha avisa.",
    [UI_MENU_QUICKSAVE_KEY] = "Tecla de salvamento rápido",
    [UI_MENU_QUICKLOAD_KEY] = "Tecla de carregamento rápido",
    [UI_MENU_QUICKSAVE_KEY_DESC] = "A tecla que inicia o salvamento ou o carregamento rápido; F6 e F7 por padrão, como no Half-Life. Selecione a célula e aperte qualquer tecla ou botão do mouse; Delete deixa essa ação sem tecla. Com o salvamento rápido ligado, as duas teclas não fazem mais nada (uma nota aparece quando um mapeamento também usa a tecla). O totk.ini aceita qualquer nome de tecla dos mapeamentos (quicksave_key, quickload_key).",
    [UI_QS_SAVED] = "Jogo salvo",
    [UI_QS_LOADING] = "Carregando o salvamento rápido",
    [UI_QS_NOT_NOW] = "Salvamento rápido não é possível agora",
    [UI_QS_NONE] = "Ainda não há salvamento rápido (a tecla de salvamento rápido faz um)",
    [UI_QS_FAILED] = "O salvamento rápido falhou: veja o totk.log",
    [UI_QS_LOAD_FAILED] = "O carregamento rápido falhou: veja o totk.log",
    [UI_MENU_PROMPTS_MISSING] = "faltam os arquivos dos ícones: execute a instalação de novo",
    [UI_MENU_PROMPTS_FAILED] = "arquivos dos ícones danificados: execute a instalação de novo",
    [UI_MENU_PROMPTS_RESTART] = "os ícones mudam após reiniciar",
    [UI_MENU_PROMPTS_OFF_SHAPE] = "desligados nesta resolução de jogo (sem layout feito para ela)",
    [UI_MENU_OUTPUT_RES_NO_LAYOUT] = "sem layout feito para este formato: rode o instalador de novo",
    [UI_MENU_OUTPUT_RES_PREPARING] = "layout sendo feito em segundo plano; usado na próxima vez que o jogo abrir",
    [UI_MENU_PROMPTS_PREPARING] = "arquivos dos ícones sendo feitos em segundo plano; usados na próxima vez que o jogo abrir",
    [UI_MENU_PREPARE_RETRY] = "Tentar de novo",
    [UI_MENU_PREPARE_RETRY_DESC] = "Abre de novo as ferramentas do instalador para fazer em segundo plano o layout da interface ou os arquivos dos ícones que faltam. O jogo continua rodando; ele os usa a partir da próxima vez que abrir.",
    [UI_MENU_SHAPE_PREPARE_FAILED] = "as ferramentas do instalador não abriram: rode o instalador de novo",
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
    [UI_MENU_FREECAM] = "Câmera livre",
    [UI_MENU_FREECAM_DESC] = "Permite que uma combinação ou uma tecla abaixo solte a câmera do Link e a faça voar."
        " Desligada por padrão: assim o código de câmera do jogo roda como foi lançado. Vale "
        "depois de reiniciar. Versão 1.4.3 do jogo.",
    [UI_MENU_FREECAM_OFF_NOTE] = "Ligue a Câmera livre acima e reinicie",
    [UI_MENU_FREECAM_PAD] = "Combinação da câmera livre",
    [UI_MENU_FREECAM_PAD_DESC] = "Botões do controle pressionados juntos que passam a câmera por três etapas: ela se "
        "solta do Link e voa, fica parada onde foi deixada enquanto o Link volta a ser controlado, e volta. Enquanto "
        "ela voa o jogo não recebe comandos: o analógico esquerdo voa na direção da visão e para os lados, os gatilhos "
        "sobem e descem, o analógico direito gira, e cada toque em L ou R reduz a velocidade à metade ou a dobra. Com "
        "a câmera parada, o Link anda para onde o analógico esquerdo aponta na tela. Versão do jogo 1.4.3.",
    [UI_MENU_FREECAM_PAD_V143] = "Só na versão 1.4.3 do jogo",
    [UI_MENU_FREECAM_KEY] = "Tecla da câmera livre",
    [UI_MENU_FREECAM_KEY_DESC] = "Uma tecla que passa a câmera pelas mesmas etapas: ela se solta do Link e voa, fica "
        "parada enquanto o Link volta a ser controlado, e volta. Enquanto ela voa: W A S D voam na direção da visão e "
        "para os lados, as teclas de ZR e ZL (X e Z por padrão) sobem e descem, o mouse e as setas giram, e as teclas "
        "de L e R (Q e R) reduzem a velocidade à metade ou a dobram. Versão do jogo 1.4.3.",
    [UI_MENU_GROUP_GAMEPLAY_TWEAKS] = "Ajustes de jogabilidade",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED] = "Ajustes de jogabilidade",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED_DESC] = "Controles com a roda do mouse para rotação e distância na Ultramão, seleção de armas e itens, e zoom suave no mapa.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE] = "Rotação com roda (Ultramão)",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE_DESC] = "Gira objetos segurados pela Ultramão diretamente com a roda do mouse.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS] = "Eixo de rotação C (Ultramão)",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS_DESC] = "Segure C ao rolar a roda do mouse para girar o objeto no terceiro eixo.",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY] = "Sensibilidade de rotação (Ultramão)",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY_DESC] = "Define a velocidade de rotação do objeto com a roda do mouse.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE] = "Distância com roda (Ultramão)",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE_DESC] = "Botão direito do mouse + roda aproxima ou afasta o objeto.",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY] = "Sensibilidade de distância (Ultramão)",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY_DESC] = "Define a velocidade de ajuste de distância com a roda do mouse.",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL] = "Seleção de armas com a roda",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL_DESC] = "Role a roda do mouse no modo livre para trocar de arma. Confirme com E.",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY] = "Sensibilidade de seleção de armas",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY_DESC] = "Passos da roda necessários para avançar para a próxima arma.",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL] = "Seleção do menu rápido com a roda",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL_DESC] = "Role a roda do mouse para navegar por itens com o menu rápido aberto.",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY] = "Sensibilidade do menu rápido",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY_DESC] = "Passos da roda necessários para avançar itens nos menus rápidos.",
    [UI_MENU_GAMEPLAY_ITEM_AD] = "Navegação A / D no menu rápido",
    [UI_MENU_GAMEPLAY_ITEM_AD_DESC] = "Teclas A / D para navegar para esquerda e direita nos menus rápidos.",
    [UI_MENU_GAMEPLAY_MAP_ZOOM] = "Zoom suave no mapa",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_DESC] = "Zoom inercial suave no mapa do Purah Pad usando a roda do mouse.",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY] = "Sensibilidade do zoom no mapa",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY_DESC] = "Define a velocidade de zoom no mapa com a roda do mouse.",
    [UI_MENU_OV_SCALE_DOWN] = "bilinear (o FSR só amplia)",
    [UI_MENU_HDR_HUE] = "Correção de matiz dos realces",
    [UI_MENU_HDR_HUE_DESC] = "Cores fortes como fogo e faíscas vão para o branco como na imagem SDR do próprio jogo e "
                             "combinam com a luz que emitem; 0 as mantém totalmente saturadas.",
    [UI_MENU_SHADOW_SCALE] = "Resolução das sombras",
    [UI_MENU_SHADOW_SCALE_DESC] = "Desenha os mapas de sombra do sol essa quantidade de vezes maiores em cada lado "
                                  "que os 960x960 do jogo: bordas de sombra e sombras de folhas mais nítidas. Vale no "
                                  "próximo início.",
    [UI_MENU_SHADOW_RANGE] = "Alcance das cascatas de sombra",
    [UI_MENU_SHADOW_RANGE_DESC] = "Afasta os degraus em que as sombras ficam mais suaves (20, 40, 80 e 160 m no "
                                  "jogo). Espalha o mapa de sombras por mais terreno: aumente a Resolução das sombras "
                                  "junto. Vale no próximo início.",
    [UI_MENU_GRASS_RANGE] = "Alcance da grama",
    [UI_MENU_GRASS_RANGE_DESC] = "Até onde a grama cresce ao redor da câmera (62,5 m no jogo). Mais alto mantém a "
                                 "grama dos campos mais longe em vez de ela surgir logo à sua frente; mais baixo "
                                 "desenha menos grama. Vale no próximo início.",
    [UI_MENU_CONTACT_SHADOWS] = "Sombras de contato",
    [UI_MENU_CONTACT_SHADOWS_DESC] = "Pequenas sombras do sol que os mapas de sombra não pegam: nos pés, sob pedras, "
                                     "na grama e em dobras. Traçadas na tela depois da iluminação. Vale no próximo "
                                     "início.",
    [UI_MENU_PCSS] = "Sombras suaves",
    [UI_MENU_PCSS_DESC] = "Sombras do sol nítidas onde um objeto toca o chão e mais suaves quanto mais longe a sombra "
                          "cai (PCSS). 0 mantém o filtro do jogo. Vale no próximo início.",
    [UI_MENU_DLSS] = "Modo de upscaling",
    [UI_MENU_DLSS_DESC] = "Nativo suaviza a cena 3D na resolução cheia. Qualidade, Equilibrado, Desempenho e Ultra "
                          "Desempenho a desenham em 67, 58, 50 e 33 % e deixam o upscaler (DLSS ou FSR 3.1) ampliá-la, "
                          "o que poupa tempo da GPU; eles substituem a escala 3D. Vale na hora.",
    [UI_MENU_DLSS_DLAA] = "Nativo",
    [UI_MENU_DLSS_QUALITY] = "Qualidade",
    [UI_MENU_DLSS_BALANCED] = "Equilibrado",
    [UI_MENU_DLSS_PERFORMANCE] = "Desempenho", [UI_MENU_DLSS_ULTRA] = "Ultra Desempenho",
    [UI_MENU_DLSS_PRESET] = "Modelo do DLSS",
    [UI_MENU_DLSS_PRESET_DESC] = "Qual modelo do DLSS roda. Automático segue a recomendação da NVIDIA para cada modo: K "
                                 "para DLAA, Qualidade e Equilibrado, M para Desempenho, L para Ultra Desempenho. J: um "
                                 "pouco menos de rastro atrás de personagens em movimento, um pouco mais de cintilação. K: "
                                 "o modelo mais nítido em todos os modos. Vale na hora.",
    [UI_MENU_DLSS_PRESET_DEFAULT] = "Automático", [UI_MENU_DLSS_PRESET_K] = "K (mais nítido)",
    [UI_MENU_DLSS_PRESET_J] = "J (menos rastro)",
    [UI_MENU_DLSS_CHARS] = "Rastros de personagens no DLSS",
    [UI_MENU_DLSS_CHARS_DESC] = "Reduz os rastros fracos atrás de personagens, animais e do que eles seguram em movimento: "
                                "ali o DLSS usa menos dos quadros anteriores. Mais forte remove mais rastros e deixa essas "
                                "bordas um pouco menos suaves. Vale na hora.",
    [UI_MENU_DLSS_CHARS_OFF] = "Desligado", [UI_MENU_DLSS_CHARS_LIGHT] = "Leve", [UI_MENU_DLSS_CHARS_MEDIUM] = "Médio",
    [UI_MENU_DLSS_CHARS_FULL] = "Total",
    [UI_MENU_FXAA] = "Antisserrilhado do jogo (FXAA)",
    [UI_MENU_FXAA_DESC] = "A suavização de bordas FXAA do próprio jogo. Borra um pouco os detalhes finos; com o DLAA "
                          "ligado não é necessária.",
    [UI_MENU_REFLEX] = "Baixa latência",
    [UI_MENU_REFLEX_DESC] = "Reduz a latência do sistema. Ligado usa o NVIDIA Reflex (VK_NV_low_latency2); Ligado + Boost também mantém os clocks da GPU NVIDIA altos quando limitado pela CPU. Automático usa o Reflex na NVIDIA e o AMD Anti-Lag 2 (VK_AMD_anti_lag) na AMD. Aplica após reiniciar.",
    [UI_MENU_REFLEX_AUTO] = "Automático",
    [UI_MENU_REFLEX_OFF] = "Desligado",
    [UI_MENU_REFLEX_ON] = "Ligado",
    [UI_MENU_REFLEX_BOOST] = "Ligado + Boost",
    [UI_MENU_SCENE_UPSCALER] = "Upscaler da cena 3D",
    [UI_MENU_SCENE_UPSCALER_DESC] = "Um upscaler temporal para a cena 3D: bordas mais suaves e menos cintilação, e nos modos abaixo do "
                                    "Nativo ele desenha a cena menor e a amplia, o que poupa tempo da GPU. O NVIDIA DLSS "
                                    "precisa de uma GPU NVIDIA RTX e do " DLSS_FILE " ao lado do " EXE_FILE "; o AMD FSR "
                                    "3.1 roda em qualquer GPU; o AMD FSR 4 (Windows) precisa de uma GPU Radeon para a qual a AMD o "
                                    "oferece e de " FFX_FILES " ao lado do " EXE_FILE ". Desligado mantém a escala e o FXAA do próprio jogo. O HUD "
                                    "não é afetado; o upscaler e a nitidez acima continuam ajustando a imagem do jogo à "
                                    "janela. Vale após reiniciar.",
    [UI_MENU_SCENE_UPSCALER_OFF] = "Desligado", [UI_MENU_SCENE_UPSCALER_DLSS] = "NVIDIA DLSS",
    [UI_MENU_SCENE_UPSCALER_FSR3] = "AMD FSR 3.1", [UI_MENU_SCENE_UPSCALER_FSR4] = "AMD FSR 4",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_GPU] = "FSR 4: não oferecido para esta GPU",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_DLL] = "FSR 4: faltam as bibliotecas FSR da AMD",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_OS] = "FSR 4: só no Windows",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_GPU] = "DLSS: precisa de uma GPU NVIDIA RTX",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_DLL] = "DLSS: falta o " DLSS_FILE,
    [UI_MENU_FSR_SHARPNESS] = "Nitidez do FSR",
    [UI_MENU_FSR_SHARPNESS_DESC] = "Aumento de nitidez da cena 3D após o AMD FSR 3.1 ou FSR 4; 0 desliga.",
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
    [UI_LW_STAGE_START] = "Preparando",
    [UI_LW_STAGE_GPU] = "Iniciando os gráficos",
    [UI_LW_STAGE_FILES] = "Preparando os arquivos do jogo",
    [UI_LW_STAGE_SHADERS] = "Carregando shaders",
    [UI_LW_STAGE_PIPELINES] = "Montando pipelines",
    [UI_LW_STAGE_GAME] = "Iniciando o jogo",
    [UI_LW_FAILED] = "O jogo parou",
    [UI_LW_FAILED_HINT] = "O motivo está no log abaixo. Copie-o para um relatório de erro e feche esta janela.",
    [UI_LW_VERSION] = "v%s · jogo %s",
    [UI_LW_COUNT] = "%s de %s",
    [UI_LW_LEFT_ITEMS] = "faltam %s",
    [UI_LW_LEFT_SEC] = "cerca de %d s restantes",
    [UI_LW_LEFT_MIN] = "cerca de %d min restantes",
    [UI_LW_LEFT_FEW] = "alguns segundos restantes",
    [UI_LW_ALSO] = "Também: %s",
    [UI_LW_GAME_SEC] = "O jogo começa em cerca de %d s",
    [UI_LW_GAME_MIN] = "O jogo começa em cerca de %d min",
    [UI_LW_GAME_SOON] = "O jogo vai começar a qualquer momento",
    [UI_LW_ELAPSED] = "Decorrido %d:%02d",
    [UI_LW_DETAILS] = "Detalhes",
    [UI_LW_HIDE] = "Ocultar detalhes",
    [UI_LW_COPY] = "Copiar log",
    [UI_LW_COPIED] = "Copiado",
    [UI_LW_CLOSE] = "Fechar e sair do jogo",
    [UI_LW_THOUSANDS] = ".",
    [UI_MENU_CHEATS_REBUILD] = "Recompilar o jogo agora",
    [UI_MENU_CHEATS_REBUILD_DESC] = "Alguns cheats alteram o código do jogo e ainda não foram compilados nele: são "
                                    "novos ou editados, ou a última recompilação falhou ou foi cancelada. Fecha o "
                                    "jogo; o instalador os compila (alguns minutos) e inicia o jogo de novo. O "
                                    "progresso desde o último salvamento é perdido.",
    [UI_MENU_MODS_EXEFS_UNSUPPORTED] = "Este mod altera o código do jogo (exefs) de um jeito que o port não consegue "
                                       "aplicar, então o mod inteiro fica desligado. A chave \"Usar ... sem o código "
                                       "dele\", abaixo, usa o resto dele. %s",
    [UI_MENU_MODS_EXEFS_OTHER] = "Os patches de código dele (exefs) são para outra versão do jogo.",
    [UI_MENU_MODS_EXEFS_ON] = "Patches de código em uso: %d arquivos.",
    [UI_MENU_MODS_REPLACES_BUILTIN] = "Patches de código em uso: %d arquivos. Eles substituem o patch do próprio port "
                                      "%s, que fica desligado enquanto este mod está ligado.",
    [UI_MENU_MODS_CODE_SKIPPED] = "O código próprio do jogo dele (%s) não é carregado: o resto do mod é usado. "
                                  "Desligue o mod se o jogo se comportar mal com ele.",
    [UI_MENU_MODS_PLUGIN_ON] = "O plugin dele está ativo: %d ganchos no código do jogo.",
    [UI_MENU_MODS_PLUGIN_REFUSED] = "O plugin %s dele não pode rodar no port: %s.",
    [UI_MENU_MODS_PLUGIN_STAYS_OFF] = "O mod inteiro fica desligado. A chave \"Usar ... sem o código dele\", abaixo, "
                                      "usa o resto dele.",
    [UI_MENU_MODS_PLUGIN_OFF] = "O plugin dele fica desligado desta vez: ele iniciou de forma diferente da compilação "
                                "(%s). O jogo o recompila na próxima inicialização.",
    [UI_MENU_MODS_WITHOUT_CODE] = "Usar %s sem o código dele",
    [UI_MENU_MODS_WITHOUT_CODE_DESC] = "O mod acima traz código que o port não consegue rodar: código próprio do jogo "
                                       "(um plugin subsdk ou um main substituído) ou patches de código (exefs) que o "
                                       "port não consegue aplicar. Desligado: o mod inteiro fica desligado. Ligado: "
                                       "esse código fica de fora e o resto do mod é usado: os arquivos RomFS, cheats e "
                                       "os patches de código que funcionam. Os arquivos dele podem depender do código "
                                       "que falta; desligue de novo se o jogo se comportar mal. Vale na próxima "
                                       "inicialização.",
    [UI_MENU_MODS_WITHOUT_CODE_ALL] = "Ligado para todos os mods: mods_without_code = 1 no totk.ini.",
    [UI_MENU_MODS_EXEFS_SKIPPED] = "%u arquivos de patch de código (exefs) dele não podem ser aplicados e ficam de "
                                   "fora: o resto do mod é usado. %s",
    [UI_MENU_MODS_OVERRIDES_FILES] = "%u dos seus arquivos substituem os de %s, que está abaixo na lista.",
    [UI_MENU_MODS_DATA_OVERRIDDEN] = "%u bytes das suas mudanças nos dados do jogo vêm de %s, que está acima na lista.",
    [UI_MENU_MODS_OVERRIDES_DATA] = "%u bytes das suas mudanças nos dados do jogo substituem as de %s, que está abaixo "
                                    "na lista.",
    [UI_MENU_MODS_CODE_OVERRIDDEN] = "%u pontos dos seus patches de código são sobrepostos por %s, que está acima na "
                                     "lista.",
    [UI_MENU_MODS_OVERRIDES_CODE] = "Ele sobrepõe %u pontos dos patches de código de %s, que está abaixo na lista.",
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
    [UI_MENU_PLAY_TIME] = "游戏时间",
    [UI_MENU_PLAY_TIME_DESC] = "所有运行累计的游戏时间，仅在游戏窗口处于焦点且游戏正在绘制画面时计时。"
                               "保存在配置文件夹的 playtime.txt 中，与存档放在一起。",
    [UI_MENU_PLAY_TIME_HM] = "%llu 小时 %02u 分钟", [UI_MENU_PLAY_TIME_SESSION] = "本次运行：%s",
    [UI_MENU_MODS_BUILTIN] = "移植版内置", [UI_MENU_MODS_INSTALLED] = "已安装的模组",
    [UI_MENU_MODS_NONE] = "还没有模组。把模组解压到 mods 文件夹，每个模组一个文件夹，里面放 romfs 文件夹，然后重启游戏。"
                          "TKMM 也可以把合并后的模组导出到那里的一个文件夹。",
    [UI_MENU_MODS_OPEN] = "打开模组文件夹",
    [UI_MENU_MODS_OPEN_DESC] = "显示游戏加载模组的文件夹。不存在时会创建它。",
    [UI_MENU_MODS_ROW_DESC] = "替换的游戏文件：%u。新增的文件：%u。",
    [UI_MENU_MODS_OVERRIDDEN] = "它的 %u 个文件改用列表中更靠上的 %s 的文件。",
    [UI_MENU_MODS_MOVE_HINT] = "上方按钮或 Ctrl+上 上移，左侧按钮或 Ctrl+下 下移。两个模组修改同一个文件、游戏数据或代码时，以靠上的为准。更改在重启后生效。",
    [UI_MENU_MODS_EMPTY] = "此文件夹中没有 romfs、exefs 或 cheats 文件。",
    [UI_MENU_MODS_SUBSDK] = "此模组带有自己的游戏代码（subsdk 插件或替换的 main）。移植版无法加载代码模块，因此整个模组保持关闭。打开下方它的“不带代码使用 ...”开关即可使用其余部分。",
    [UI_MENU_MODS_EXEFS_LATER] = "此模组修改游戏代码（exefs）。游戏会在下次启动时把这些补丁构建进去（重新构建需要几分钟）；在此之前整个模组保持关闭。",
    [UI_MENU_MODS_CHEATS] = "金手指：%d 个。在“金手指”标签页中开关。",
    [UI_MENU_MODS_CHEATS_FILE] = "此游戏版本的金手指在启动时从此模组的 cheats\\%s 读取。",
    [UI_MENU_MODS_BAD_NAME] = "文件夹名称含有 |，模组列表用它分隔名称。请重命名该文件夹。",
    [UI_MENU_MODS_OTHER_VERSION] = "它有 %u 个文件的文件名带有其他游戏版本号。只有在有内容请求这些文件名时，游戏才会读取它们。"
                                   "如果模组不起作用，请寻找适用于你的游戏版本的此模组。",
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
    [UI_PERF_LINE_FPS_GEN] = "绘制 %.0f fps，显示 %.0f",

    [UI_MENU_GRAPHICS] = "画面",
    [UI_MENU_OUTPUT_RES] = "游戏分辨率",
    [UI_MENU_OUTPUT_RES_DESC] = "游戏绘制画面和界面的尺寸。"
                                "高于 1920 x 1080 会占用更多显存和 GPU 时间。"
                                "更宽的尺寸（21:9）会扩大视野，并把界面移到屏幕四角；"
                                "更高的尺寸（16:10、3:2）则移到上下边缘。",
    [UI_MENU_HUD_WIDTH] = "界面宽度",
    [UI_MENU_HUD_WIDTH_DESC] = "在比 16:9 更宽的屏幕上，界面左右两侧相距多远。"
                               "“整个屏幕”把它们放在屏幕边缘；21:9 或 16:9 则把界面保持在屏幕中间该比例的区域内。"
                               "缺少的布局会在后台制作，重启游戏后生效。",
    [UI_MENU_HUD_WIDTH_FULL] = "整个屏幕",
    [UI_MENU_RENDER_SCALE] = "3D 渲染比例",
    [UI_MENU_RENDER_SCALE_DESC] = "3D 场景相对于游戏分辨率的尺寸；"
                                  "游戏的 FSR 会将其缩放到该尺寸。"
                                  "高于 100% 会平滑锯齿边缘（超采样），但 GPU 开销很高。",
    [UI_MENU_RENDER_SCALE_GAME] = "游戏（83%）",
    [UI_MENU_DYNAMIC_RES] = "动态分辨率",
    [UI_MENU_DYNAMIC_RES_DESC] = "让游戏把 3D 场景降到 90 % 或 80 %。它依据为 Switch 制定的估算，不考虑你的显卡，因此即使在高性能 PC 上宽场景也会降低。关闭时保持渲染比例。",
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
    [UI_MENU_MOVIE_ASPECT] = "视频画面比例",
    [UI_MENU_MOVIE_ASPECT_DESC] = "当游戏分辨率不是 16:9 时：用黑边保持过场视频的原有比例，或拉伸铺满整个画面。"
                                  "从下一段视频起生效。",
    [UI_MENU_MOVIE_ASPECT_KEEP] = "黑边",
    [UI_MENU_MOVIE_ASPECT_STRETCH] = "拉伸",
    [UI_MENU_FOV] = "视野",
    [UI_MENU_FOV_DESC] = "镜头能看到多大范围，以游戏自身视野的百分比表示。"
                          "支持游戏版本 1.1.0、1.2.1 和 1.4.3。",
    [UI_MENU_VERT_PLUS] = "保持 16:9 宽度",
    [UI_MENU_VERT_PLUS_DESC] = "在比 16:9 更窄的屏幕上（Steam Deck、16:10、4:3），镜头显示与 16:9 相同宽度的世界，上下显示更多。16:9 或更宽的屏幕没有变化。",
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
    [UI_MENU_FRAME_QUEUE] = "帧队列",
    [UI_MENU_FRAME_QUEUE_DESC] = "前一帧等待显示时游戏就开始绘制下一帧：在帧率上限下掉帧大幅减少，输入延迟最多增加一帧。立即生效。",
    [UI_MENU_FRAME_GEN] = "帧生成", [UI_MENU_FRAME_GEN_OFF] = "关", [UI_MENU_FRAME_GEN_FSR3] = "AMD FSR 3",
    [UI_MENU_FRAME_GEN_DESC] = "在游戏绘制的每两帧之间插入一帧生成画面：屏幕上的帧数翻倍，游戏以帧率上限的一半运行，操作响应约晚半帧。适合 120 Hz 及以上的显示器。不能与 DLSS 或 HDR 同时使用。立即生效。",
    [UI_MENU_REALTIME] = "游戏速度跟随时钟",
    [UI_MENU_REALTIME_DESC] = "按真实时间推进游戏，帧率低时游戏速度也不会变慢。",
    [UI_MENU_CLOTH_REALTIME] = "布料跟随时钟",
    [UI_MENU_CLOTH_REALTIME_DESC] = "布料和头发也按真实时间推进。",
    [UI_MENU_HIGHFPS_FIX] = "高帧率修正",
    [UI_MENU_HIGHFPS_FIX_DESC] = "修正帧率高于 30 时出现异常的物理效果，"
                                 "以及帧率高于 60 时一直半透明或不可见的物体。",
    [UI_MENU_CLIMB_FIX] = "攀爬速度修正",
    [UI_MENU_CLIMB_FIX_DESC] = "帧率高于 60 时让攀爬墙壁和梯子保持正常速度，林克能爬到顶。",
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
    [UI_MENU_VOLUME_MUSIC] = "音乐音量",
    [UI_MENU_VOLUME_MUSIC_DESC] = "背景音乐的音量。",
    [UI_MENU_VOLUME_SFX] = "音效音量",
    [UI_MENU_VOLUME_SFX_DESC] = "声音特效的音量。",
    [UI_MENU_VOLUME_VOICE] = "语音音量",
    [UI_MENU_VOLUME_VOICE_DESC] = "角色语音的音量。",
    [UI_MENU_AUDIO_CHANNELS] = "扬声器",
    [UI_MENU_AUDIO_CHANNELS_DESC] = "立体声或 5.1 环绕声输出。选择自动时，若 Windows 扬声器设置有六个或更多声道则使用 5.1。重启后生效。",
    [UI_MENU_CHANNELS_AUTO] = "自动", [UI_MENU_CHANNELS_STEREO] = "立体声", [UI_MENU_CHANNELS_51] = "5.1 环绕声",
    [UI_MENU_AUDIO_DEVICE] = "输出设备",
    [UI_MENU_AUDIO_DEVICE_DESC] = "声音从哪个设备播放。系统默认跟随 Windows 的默认设备。所选设备被拔出时改用系统默认，"
                                  "设备重新连接后恢复。立即生效。",
    [UI_MENU_AUDIO_DEVICE_DEFAULT] = "系统默认",
    [UI_MENU_MOTION] = "体感操作",
    [UI_MENU_MOTION_DESC] = "用手柄的陀螺仪瞄准。",
    [UI_MENU_GYRO_CALIBRATION] = "陀螺仪偏置校准",
    [UI_MENU_GYRO_CALIBRATION_DESC] = "减去下面校准行测得的手柄静止偏移。关闭时测量值仍然保留。",
    [UI_MENU_GYRO_CALIBRATE] = "校准陀螺仪",
    [UI_MENU_GYRO_CALIBRATE_DESC] = "测量并消除陀螺仪的静止偏移。请放下手柄或保持静止两秒。",
    [UI_MENU_GYRO_CALIBRATE_WAIT] = "校准中...",
    [UI_MENU_GYRO_CALIBRATED] = "%s，已校准",
    [UI_MENU_GYRO_CALIBRATE_FAILED] = "校准失败",
    [UI_MENU_GYRO_CALIBRATE_MOVED] = "手柄移动了：请保持静止后重试",
    [UI_MENU_GYRO_AUTO_BIAS] = "静止时学习陀螺仪漂移",
    [UI_MENU_GYRO_AUTO_BIAS_DESC] = "手柄平放在桌上不动时测量陀螺仪的剩余漂移并将其减去。适用于校准后弓箭准星仍缓慢漂"
                                    "移的情况。手持时不会测量。实验性功能。",
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
    [UI_MENU_GROUP_STICKS] = "摇杆",
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
    [UI_BIND_SHAKE] = "晃动视角摇杆",
    [UI_BIND_DESC_SHAKE] = "让右摇杆左右晃动：把究极手抓着的物体上粘着的零件拆下。按住该键会一直晃动。",
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
    [UI_PAD_TOUCHPAD] = "触摸板按下",
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
    [UI_MENU_DISCORD_RPC] = "Discord Rich Presence",
    [UI_MENU_DISCORD_RPC_DESC] = "游戏运行时在 Discord 状态中显示“正在玩 TotK Recomp”及游戏时长。totk.ini 中的 "
                                 "discord_app_id 和 discord_image 选择应用。",
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
    [UI_MENU_REPLAY_WORKERS] = "GPU 帧准备线程数",
    [UI_MENU_REPLAY_WORKERS_DESC] = "同时在多个线程上为 GPU 录制大帧。CPU 限制帧率时有帮助；需要 8 个或更多逻辑处理器。"
                                    "关：在单个线程上录制。",
    [UI_MENU_REPLAY_WORKERS_NOW] = "当前：%s",
    [UI_MENU_TIMER_1MS] = "1 ms Windows 计时器",
    [UI_MENU_TIMER_1MS_DESC] = "将系统计时器精度提高到 1 ms，使帧间隔更平稳。",
    [UI_MENU_SYNC_AHEAD] = "领先 GPU 一帧",
    [UI_MENU_SYNC_AHEAD_DESC] = "游戏在显卡仍在绘制上一帧时就开始准备下一帧。帧率更高，输入延迟多一帧。",
    [UI_MENU_HOST_CHECKPOINT] = "快速交出核心",
    [UI_MENU_HOST_CHECKPOINT_DESC] = "游戏线程一离开移植层代码就把虚拟核心让给更重要的线程，而不是等到下一次系统调用。",
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
                             "显存占用只有四分之一。纹理会出现轻微色块。"
                             "自动：显存 4 GB 及以下的显卡上开启。",
    [UI_MENU_LOD_GUARD] = "LOD 保护",
    [UI_MENU_LOD_GUARD_DESC] = "等待游戏改写完细节层次数据后再让 GPU 读取，"
                               "避免物体错位闪现，或远景有一帧变平。",
    [UI_MENU_FIX_C0] = "常量缓冲区修正",
    [UI_MENU_FIX_C0_DESC] = "填充驱动保留的常量组 c0（存储缓冲区大小、派发大小、点大小、"
                            "基础实例），而不是读取零值。",
    [UI_MENU_FIX_ZERO] = "清零新的 GPU 内存",
    [UI_MENU_FIX_ZERO_DESC] = "新的 GPU 内存池初始内容为零，与 Switch 内存一致。",
    [UI_MENU_DLAA_EFFECTS_AFTER] = "DLAA 之后绘制粒子",
    [UI_MENU_DLAA_EFFECTS_AFTER_DESC] = "开启 DLAA 时，灰尘和其他粒子绘制在 DLSS 已平滑的画面之上，与游戏不用 DLSS "
                                        "时的绘制方式相同：雾中的细小粒子（神庙中）不再消失，也不再留下拖影。雾和云仍由 DLSS 处理，因此远处与天空相接的边缘保持干净。关闭：DLSS "
                                        "也处理这些粒子，与以前相同。仅用于 DLAA 模式；提升分辨率的 DLSS 模式始终处理它们。立即生效。",
    [UI_MENU_DLSS_FIXED_EXPOSURE] = "固定 DLSS 曝光",
    [UI_MENU_DLSS_FIXED_EXPOSURE_DESC] = "DLSS 直接使用游戏画面的亮度，而不是逐帧自行估算曝光：在暗场景（地底）中，"
                                         "镜头移动时大片雾中区域不再忽暗忽亮一帧。关闭：DLSS 自行估算曝光，与以前相同。立即生效。",
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
                                "开启会导致人物变黑；Intel 集成显卡的 Windows 驱动上也为关闭，开启会使植被出现彩色噪点；"
                                "其他显卡上为开启。",
    [UI_MENU_VK_SUBGROUP_PIN] = "固定子群大小",
    [UI_MENU_VK_SUBGROUP_PIN_DESC] = "固定着色器的子群大小。",
    [UI_MENU_TRACE] = "性能日志",
    [UI_MENU_TRACE_DESC] = "把每帧耗时写入文件。",
    [UI_MENU_FLIGHT] = "飞行记录仪",
    [UI_MENU_FLIGHT_DESC] = "保留一小段历史记录，在画面卡住时保存。",
    [UI_MENU_FLIGHT_SPIKE_MS] = "卡顿阈值（ms）",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "算作卡顿的帧时间。",
    [UI_MENU_PASS_CRUMBS] = "GPU 渲染通道标记",
    [UI_MENU_PASS_CRUMBS_DESC] = "GPU 崩溃后，日志会写出当时正在运行的渲染通道。自动：仅 AMD。开：NVIDIA 也启用，有少量开销。",
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
    [UI_MENU_OV_FPS_LABEL] = "FPS", [UI_MENU_OV_FRAME_LABEL] = "帧时间", [UI_MENU_OV_FMT_FPS] = "%.0f",
    [UI_MENU_OV_LOW1] = "1%% 低 %.0f", [UI_MENU_OV_LOW01] = "0.1%% 低 %.0f",
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
    [UI_MENU_OV_UPSCALER] = "3D 超分",
    [UI_MENU_OV_SCALING] = "缩放",
    [UI_MENU_OV_LIMIT] = "帧率上限",
    [UI_MENU_OV_PRESENT] = "呈现模式",
    [UI_MENU_OV_SHOWN] = "显示帧率",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u",
    [UI_MENU_OV_FMT_SCENE] = "%ux%u，%.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1，锐度 %d%%",
    [UI_MENU_OV_SCALE_NONE] = "无（1:1）",
    [UI_MENU_OV_SCALE_BILINEAR] = "双线性",
    [UI_MENU_SHADOW_RES] = "阴影贴图尺寸",
    [UI_MENU_SHADOW_RES_DESC] = "太阳阴影贴图的尺寸。游戏自身的尺寸随视角变化，靠近地面时降到约 400，因此树叶阴影显得块状并且跳动；固定尺寸能让阴影稳定、更清晰。高于 1024 时阴影会占用更多 "
                                "GPU 时间和显存；256 比游戏自身的尺寸更粗糙，适合显存带宽不足的集成显卡。",
    [UI_MENU_SHADOW_RES_GAME] = "游戏默认（会变化）",
    [UI_MENU_ANISO] = "纹理过滤",
    [UI_MENU_ANISO_DESC] = "表面纹理的各向异性过滤：以小角度看到的地面、道路和墙壁在远处依然清晰，而不会模糊。游戏：使用游戏请求的级别，与 Switch 上相同。",
    [UI_MENU_ANISO_GAME] = "游戏",
    [UI_MENU_LOD_STEADY] = "负载下保持细节",
    [UI_MENU_LOD_STEADY_DESC] = "跳过游戏在帧率下降时降低模型和纹理细节的设置，与模拟器模组 Disable LOD Quality "
                                "Reduction 的作用相同。仅适用于游戏版本 1.1.0、1.2.1 和 1.4.3。低配电脑请勿开启：在那里正是这种降低维持了帧率。",
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
                             "B）。PlayStation：PlayStation 手柄上的符号（○ × △ □、L1、R1、L2、R2、Options 和 Share），"
                             "显示在上方布局所按下的按键上。PC：键盘默认布局的按键和鼠标按键（E、Shift、空格、左键……），镜头用鼠标。"
                             "游戏在启动时读取按键提示，因此重启后才会改变。提示属于游戏文件，在“操作”中重新映射的按键在屏幕上仍显示原来的提示。",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo",
    [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation",
    [UI_MENU_PROMPTS_PC] = "PC",
    [UI_MENU_MOUSE_MODE] = "鼠标视角",
    [UI_MENU_MOUSE_MODE_DESC] = "直接：镜头按鼠标移动的距离转动，与电脑游戏相同，没有摇杆死区和响应曲线。摇杆模拟：鼠标相当于右摇杆。",
    [UI_MENU_MOUSE_STICK] = "摇杆模拟",
    [UI_MENU_MOUSE_DIRECT] = "直接",
    [UI_MENU_MENU_CURSOR] = "在游戏菜单中使用鼠标",
    [UI_MENU_MENU_CURSOR_DESC] = "在游戏自身的菜单中（标题、背包、商店、对话选项）显示指针：指针下的项目被选中，左键确认，右键返回。仅在最后使用的是鼠标或键盘时生效，因此使用手柄时菜单保持不变（游戏 1.4.3）。",
    [UI_MENU_MAP_CLICK] = "在地图上左键单击",
    [UI_MENU_MAP_CLICK_DESC] = "在游戏地图上，鼠标左键在地图光标处按下 A 键，而不是按下它所绑定的按键（默认为 Y）。需要开启“在游戏菜单中使用鼠标”（游戏 1.4.3）。",
    [UI_MENU_DIALOGUE_CLICK] = "对话中的鼠标点击",
    [UI_MENU_DIALOGUE_CLICK_DESC] = "对话框打开时，鼠标左键按下 B 键（继续对话并跳过文字滚动），右键按下 A 键，而不是它们所绑定的按键（默认为 Y，会打开对话记录；以及 ZL）。对话选项仍使用指针：左键选择（游戏 1.4.3）。",
    [UI_MENU_POINTER_IMAGE] = "游戏指针",
    [UI_MENU_POINTER_IMAGE_DESC] = "光标未被捕获时（游戏菜单、本菜单、按 Esc 之后），窗口显示自带的金色箭头，而不是系统箭头，大小随屏幕缩放。",
    [UI_MENU_QUICKSAVE] = "快速存档与读档",
    [UI_MENU_QUICKSAVE_DESC] = "一个键立即保存游戏，另一个键读取该存档（见下方两行：默认 F6 和 F7，与 Half-Life 相同），屏幕上不会出现任何菜单。仅限键盘和鼠标按键，仅限游戏 1.4.3。快速存档会让游戏自己进行一次自动存档，因此会替换最旧的自动存档（手动存档保留），并将其副本保存为快速存档（配置文件中的 quicksave 文件夹），画面顶部显示“游戏已保存”。快速读档时画面停留在“正在读取快速存档”，同时在后台走完游戏的读档列表，游戏随即读取快速存档（约 5 秒）。在游戏本身不会自动存档的地方（过场动画、对话），不会保存，并会显示提示。",
    [UI_MENU_QUICKSAVE_KEY] = "快速存档键",
    [UI_MENU_QUICKLOAD_KEY] = "快速读档键",
    [UI_MENU_QUICKSAVE_KEY_DESC] = "启动快速存档或快速读档的按键；默认 F6 和 F7，与 Half-Life 相同。选中该格后按下任意键盘按键或鼠标按键；按 Delete 则该操作没有按键。快速存档开启时，这两个键不再有其他作用（若按键映射也使用该键，值下方会有提示）。totk.ini 接受按键映射中的任意按键名（quicksave_key、quickload_key）。",
    [UI_QS_SAVED] = "游戏已保存",
    [UI_QS_LOADING] = "正在读取快速存档",
    [UI_QS_NOT_NOW] = "现在无法快速存档",
    [UI_QS_NONE] = "还没有快速存档（按快速存档键创建）",
    [UI_QS_FAILED] = "快速存档失败：请查看 totk.log",
    [UI_QS_LOAD_FAILED] = "快速读档失败：请查看 totk.log",
    [UI_MENU_PROMPTS_MISSING] = "缺少按键提示文件：请重新运行安装程序",
    [UI_MENU_PROMPTS_FAILED] = "按键提示文件已损坏：请重新运行安装程序",
    [UI_MENU_PROMPTS_RESTART] = "按键提示重启后生效",
    [UI_MENU_PROMPTS_OFF_SHAPE] = "此游戏分辨率下关闭（未为其制作布局）",
    [UI_MENU_OUTPUT_RES_NO_LAYOUT] = "未为此比例制作布局：请重新运行安装程序",
    [UI_MENU_OUTPUT_RES_PREPARING] = "布局正在后台制作；下次启动时使用",
    [UI_MENU_PROMPTS_PREPARING] = "按键提示文件正在后台制作；下次启动时使用",
    [UI_MENU_PREPARE_RETRY] = "重试",
    [UI_MENU_PREPARE_RETRY_DESC] = "再次启动安装程序的工具，在后台制作缺少的界面布局或按键提示文件。游戏继续运行；下次启动时生效。",
    [UI_MENU_SHAPE_PREPARE_FAILED] = "安装程序的工具未能启动：请重新运行安装程序",
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
    [UI_MENU_FREECAM] = "自由镜头",
    [UI_MENU_FREECAM_DESC] = "允许用下方的组合键或按键让镜头脱离林克自由飞行。默认关闭：关闭时游戏的镜头代码保持原样运行。重启后生效。仅限游戏版本 1.4.3。",
    [UI_MENU_FREECAM_OFF_NOTE] = "请先打开上方的“自由镜头”，然后重启",
    [UI_MENU_FREECAM_PAD] = "自由镜头组合键",
    [UI_MENU_FREECAM_PAD_DESC] = "同时按下的手柄按键，让镜头依次切换三个阶段：脱离林克自由飞行、停在原处而林克恢复操作、回到林克。飞行期间游戏不接收输入：左摇杆沿视线方向和左右移动，扳机键上升和下降，右摇杆转动视角，每按一次 L 或 R 速度减半或加倍。镜头固定时，林克朝左摇杆在屏幕上指向的方向移动。游戏版本 1.4.3。",
    [UI_MENU_FREECAM_PAD_V143] = "仅限游戏版本 1.4.3",
    [UI_MENU_FREECAM_KEY] = "自由镜头键",
    [UI_MENU_FREECAM_KEY_DESC] = "让镜头依次切换相同阶段的按键：脱离林克自由飞行、停在原处而林克恢复操作、回到林克。飞行期间：W A S D 沿视线方向和左右移动，ZR 和 ZL 对应的按键（默认 X 和 Z）上升和下降，鼠标和方向键转动视角，L 和 R 对应的按键（默认 Q 和 R）让速度减半或加倍。游戏版本 1.4.3。",
    [UI_MENU_GROUP_GAMEPLAY_TWEAKS] = "游戏操作微调",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED] = "启用操作微调",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED_DESC] = "使用鼠标滚轮控制究极手物体旋转与距离、武器与道具切换以及地图平滑缩放。",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE] = "究极手滚轮旋转",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE_DESC] = "究极手抓取物体时直接使用鼠标滚轮旋转物体，无需按住 R 键。",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS] = "究极手 C 轴旋转",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS_DESC] = "滚动滚轮时按住 C 键可绕第三轴旋转物体。",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY] = "究极手旋转灵敏度",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY_DESC] = "鼠标滚轮旋转物体的灵敏度倍率。",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE] = "究极手滚轮推拉距离",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE_DESC] = "按住鼠标右键滚动滚轮可推远或拉近物体。",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY] = "究极手距离灵敏度",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY_DESC] = "鼠标滚轮推拉物体距离的灵敏度倍率。",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL] = "自由状态滚轮切换武器",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL_DESC] = "在自由行动时滚动鼠标滚轮切换武器，按 E 键确认。",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY] = "武器切换滚轮灵敏度",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY_DESC] = "滚轮切换下一个武器所需的滚动步数灵敏度。",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL] = "快捷菜单滚轮选择",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL_DESC] = "快捷菜单打开时通过鼠标滚轮快速切换选择物品。",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY] = "快捷菜单滚轮灵敏度",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY_DESC] = "在快捷菜单中切换下一个物品所需的滚轮灵敏度。",
    [UI_MENU_GAMEPLAY_ITEM_AD] = "快捷菜单 A / D 键切换",
    [UI_MENU_GAMEPLAY_ITEM_AD_DESC] = "快捷菜单打开时按 A / D 键向左或向右切换选择物品。",
    [UI_MENU_GAMEPLAY_MAP_ZOOM] = "地图平滑缩放",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_DESC] = "在普尔亚平板地图上使用鼠标滚轮进行平滑惯性缩放。",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY] = "地图缩放灵敏度",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY_DESC] = "鼠标滚轮缩放地图的灵敏度倍率。",
    [UI_MENU_OV_SCALE_DOWN] = "双线性（FSR 只能放大）",
    [UI_MENU_HDR_HUE] = "高光色调校正",
    [UI_MENU_HDR_HUE_DESC] = "火焰、火花等明亮色彩像游戏自身的 SDR 画面一样趋向白色，与它们发出的光一致；0 保持完全饱和。",
    [UI_MENU_SHADOW_SCALE] = "阴影分辨率",
    [UI_MENU_SHADOW_SCALE_DESC] = "将太阳阴影贴图的每条边绘制为游戏 960x960 的若干倍：阴影边缘和树叶阴影更清晰。下次启动时生效。",
    [UI_MENU_SHADOW_RANGE] = "阴影级联范围",
    [UI_MENU_SHADOW_RANGE_DESC] = "把阴影变柔和的分界（游戏中为 20、40、80 和 160 米）推得更远。阴影贴图会覆盖更大的地面，请同时提高阴影分辨率。下次启动时生效。",
    [UI_MENU_GRASS_RANGE] = "草地范围",
    [UI_MENU_GRASS_RANGE_DESC] = "镜头周围长草的距离（游戏中为 62.5 米）。调高后草地延伸得更远，不会在你面前才长出来；调低则少画一些草。下次启动时生效。",
    [UI_MENU_CONTACT_SHADOWS] = "接触阴影",
    [UI_MENU_CONTACT_SHADOWS_DESC] = "阴影贴图遗漏的细小太阳阴影：脚下、石头下、草丛和褶皱中。在光照之后于屏幕空间追踪。下次启动时生效。",
    [UI_MENU_PCSS] = "柔和阴影",
    [UI_MENU_PCSS_DESC] = "物体接触地面处的太阳阴影锐利，阴影投得越远越柔和（PCSS）。0 保留游戏自身的过滤。下次启动时生效。",
    [UI_MENU_DLSS] = "超分模式",
    [UI_MENU_DLSS_DESC] = "原生以全分辨率对 3D 场景做抗锯齿。质量、平衡、性能和超级性能分别以 67%、58%、50% 和 33% 绘制，再由超分"
                          "（DLSS 或 FSR 3.1）放大，以节省 GPU 时间；它们会取代 3D 渲染比例。立即生效。",
    [UI_MENU_DLSS_DLAA] = "原生",
    [UI_MENU_DLSS_QUALITY] = "质量",
    [UI_MENU_DLSS_BALANCED] = "平衡",
    [UI_MENU_DLSS_PERFORMANCE] = "性能",
    [UI_MENU_DLSS_ULTRA] = "超级性能",
    [UI_MENU_DLSS_PRESET] = "DLSS 模型",
    [UI_MENU_DLSS_PRESET_DESC] = "运行哪个 DLSS 模型。自动按 NVIDIA 对各模式的建议：DLAA、质量和平衡用 K，性能用 M，超级性能用 L。"
                                 "J：移动角色身后的拖影略少，闪烁略多。K：所有模式都用最清晰的模型。立即生效。",
    [UI_MENU_DLSS_PRESET_DEFAULT] = "自动", [UI_MENU_DLSS_PRESET_K] = "K（最清晰）",
    [UI_MENU_DLSS_PRESET_J] = "J（拖影更少）",
    [UI_MENU_DLSS_CHARS] = "DLSS 角色拖影",
    [UI_MENU_DLSS_CHARS_DESC] = "减少移动中的角色、动物及其手持物品身后的淡淡拖影：DLSS 在这些位置少用之前的帧。越强拖影越少，"
                                "这些边缘也略欠平滑。立即生效。",
    [UI_MENU_DLSS_CHARS_OFF] = "关", [UI_MENU_DLSS_CHARS_LIGHT] = "弱", [UI_MENU_DLSS_CHARS_MEDIUM] = "中",
    [UI_MENU_DLSS_CHARS_FULL] = "强",
    [UI_MENU_FXAA] = "游戏抗锯齿（FXAA）",
    [UI_MENU_FXAA_DESC] = "游戏自身的 FXAA 边缘平滑。会让细节略微模糊；开启 DLAA 时不需要。",
    [UI_MENU_REFLEX] = "低延迟",
    [UI_MENU_REFLEX_DESC] = "降低系统延迟。开使用 NVIDIA Reflex (VK_NV_low_latency2);开 + 增强 (On + Boost) 还会在 CPU 瓶颈时保持 NVIDIA 显卡高频率。自动在 NVIDIA 显卡上使用 Reflex,在 AMD 显卡上使用 AMD Anti-Lag 2 (VK_AMD_anti_lag)。重启后生效。",
    [UI_MENU_REFLEX_AUTO] = "自动",
    [UI_MENU_REFLEX_OFF] = "关",
    [UI_MENU_REFLEX_ON] = "开",
    [UI_MENU_REFLEX_BOOST] = "开 + 增强",
    [UI_MENU_SCENE_UPSCALER] = "3D 场景超分",
    [UI_MENU_SCENE_UPSCALER_DESC] = "用于 3D 场景的时间性超分：边缘更平滑，闪烁更少；低于“原生”的模式会以更小的尺寸绘制场景再放大，以节省 GPU "
                                    "时间。NVIDIA DLSS 需要 NVIDIA RTX 显卡，并将 " DLSS_FILE " 放在 " EXE_FILE " 旁；AMD "
                                    "FSR 3.1 可在任何显卡上运行；AMD FSR 4（Windows）需要 AMD 为其提供 FSR 4 的 Radeon 显卡，并将 " FFX_FILES
                                    " 放在 " EXE_FILE " 旁。“关”保留游戏自己的缩放和 FXAA。HUD 不受影响；上方的缩放"
                                    "方式和锐度仍负责把游戏画面适配到窗口。重启后生效。",
    [UI_MENU_SCENE_UPSCALER_OFF] = "关", [UI_MENU_SCENE_UPSCALER_DLSS] = "NVIDIA DLSS",
    [UI_MENU_SCENE_UPSCALER_FSR3] = "AMD FSR 3.1", [UI_MENU_SCENE_UPSCALER_FSR4] = "AMD FSR 4",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_GPU] = "FSR 4：此显卡不提供",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_DLL] = "FSR 4：缺少 AMD 的 FSR 库",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_OS] = "FSR 4：仅限 Windows",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_GPU] = "DLSS：需要 NVIDIA RTX 显卡",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_DLL] = "DLSS：缺少 " DLSS_FILE,
    [UI_MENU_FSR_SHARPNESS] = "FSR 锐度",
    [UI_MENU_FSR_SHARPNESS_DESC] = "AMD FSR 3.1 或 FSR 4 之后 3D 场景的锐化强度；0 为关闭。",
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
    [UI_LW_STAGE_START] = "准备中",
    [UI_LW_STAGE_GPU] = "启动图形",
    [UI_LW_STAGE_FILES] = "准备游戏文件",
    [UI_LW_STAGE_SHADERS] = "加载着色器",
    [UI_LW_STAGE_PIPELINES] = "构建管线",
    [UI_LW_STAGE_GAME] = "启动游戏",
    [UI_LW_FAILED] = "游戏已停止",
    [UI_LW_FAILED_HINT] = "原因见下方日志。复制日志用于错误报告，然后关闭此窗口。",
    [UI_LW_VERSION] = "v%s · 游戏 %s",
    [UI_LW_COUNT] = "%s / %s",
    [UI_LW_LEFT_ITEMS] = "剩余 %s",
    [UI_LW_LEFT_SEC] = "还需约 %d 秒",
    [UI_LW_LEFT_MIN] = "还需约 %d 分钟",
    [UI_LW_LEFT_FEW] = "还需几秒",
    [UI_LW_ALSO] = "同时：%s",
    [UI_LW_GAME_SEC] = "约 %d 秒后启动游戏",
    [UI_LW_GAME_MIN] = "约 %d 分钟后启动游戏",
    [UI_LW_GAME_SOON] = "游戏即将启动",
    [UI_LW_ELAPSED] = "已用时 %d:%02d",
    [UI_LW_DETAILS] = "详细信息",
    [UI_LW_HIDE] = "隐藏详细信息",
    [UI_LW_COPY] = "复制日志",
    [UI_LW_COPIED] = "已复制",
    [UI_LW_CLOSE] = "关闭并退出游戏",
    [UI_LW_THOUSANDS] = ",",
    [UI_MENU_CHEATS_REBUILD] = "立即重新构建游戏",
    [UI_MENU_CHEATS_REBUILD_DESC] = "有些金手指会修改游戏代码，但尚未构建进游戏：它们是新增或修改的，或上次重新构建失败或被取消。将关闭游戏；安装程序会把它们构建进去（几分钟）并重新启动游戏。上次存档之后的进度会丢失。",
    [UI_MENU_MODS_EXEFS_UNSUPPORTED] = "此模组修改游戏代码（exefs）的方式本移植无法应用，因此整个模组保持关闭。打开下方它的“不带代码使用 ...”开关即可使用其余部分。%s",
    [UI_MENU_MODS_EXEFS_OTHER] = "它的代码补丁（exefs）是为其他游戏版本制作的。",
    [UI_MENU_MODS_EXEFS_ON] = "正在使用的代码补丁：%d 个文件。",
    [UI_MENU_MODS_REPLACES_BUILTIN] = "正在使用的代码补丁：%d 个文件。它们取代本移植自带的补丁 %s，此模组开启时该补丁保持关闭。",
    [UI_MENU_MODS_CODE_SKIPPED] = "它自带的游戏代码（%s）不会加载：模组的其余部分照常使用。如果游戏因此表现异常，请关闭此模组。",
    [UI_MENU_MODS_PLUGIN_ON] = "它的插件正在运行：游戏代码中有 %d 处挂钩。",
    [UI_MENU_MODS_PLUGIN_REFUSED] = "它的插件 %s 无法在移植版中运行：%s。",
    [UI_MENU_MODS_PLUGIN_STAYS_OFF] = "整个模组保持关闭。打开下方它的“不带代码使用 ...”开关即可使用其余部分。",
    [UI_MENU_MODS_PLUGIN_OFF] = "它的插件本次保持关闭：启动情况与构建时不同（%s）。游戏会在下次启动时重新构建它。",
    [UI_MENU_MODS_WITHOUT_CODE] = "不带代码使用 %s",
    [UI_MENU_MODS_WITHOUT_CODE_DESC] = "上方的模组带有移植版无法运行的代码：它自己的游戏代码（subsdk 插件或替换的 main），或移植版无法应用的代码补丁（exefs）。关闭：整个模组保持关闭。开启：跳过这些代码，使用模组的其余部分：其 RomFS 文件、金手指和可用的代码补丁。它的文件可能依赖缺失的代码；如果游戏表现异常，请再次关闭。下次启动时生效。",
    [UI_MENU_MODS_WITHOUT_CODE_ALL] = "已对所有模组开启：totk.ini 中 mods_without_code = 1。",
    [UI_MENU_MODS_EXEFS_SKIPPED] = "它有 %u 个代码补丁文件（exefs）无法应用，已跳过：模组的其余部分照常使用。%s",
    [UI_MENU_MODS_OVERRIDES_FILES] = "它的 %u 个文件取代了列表中更靠下的 %s 的文件。",
    [UI_MENU_MODS_DATA_OVERRIDDEN] = "它对游戏数据的修改中有 %u 字节改用列表中更靠上的 %s 的修改。",
    [UI_MENU_MODS_OVERRIDES_DATA] = "它对游戏数据的修改中有 %u 字节取代了列表中更靠下的 %s 的修改。",
    [UI_MENU_MODS_CODE_OVERRIDDEN] = "它有 %u 处代码补丁位置被列表中更靠上的 %s 覆盖。",
    [UI_MENU_MODS_OVERRIDES_CODE] = "它覆盖了 %u 处代码补丁位置，这些位置原属于列表中更靠下的 %s。",
};

// German
static const char *const text_de[UI_TEXT_COUNT] = {
    SWKBD("Texteingabe", "Gib einen Namen ein", "Pferd", "OK", "Abbrechen", "Controller: A = OK, B = Abbrechen"),

    [UI_MENU_TITLE] = "Einstellungen",
    [UI_MENU_TAB_SCREEN] = "Bild",
    [UI_MENU_TAB_GAME] = "Spiel",
    [UI_MENU_TAB_MODS] = "Mods",
    [UI_MENU_TAB_SOUND] = "Ton",
    [UI_MENU_TAB_CONTROLS] = "Steuerung",
    [UI_MENU_TAB_AMIIBO] = "Amiibo",
    [UI_MENU_TAB_MENU] = "Menü",
    [UI_MENU_TAB_ADVANCED] = "Erweitert",
    [UI_MENU_CONTINUE] = "Weiter",
    [UI_MENU_QUIT] = "Spiel beenden",
    [UI_MENU_ON] = "Ein",
    [UI_MENU_OFF] = "Aus",
    [UI_MENU_AUTO] = "Automatisch",
    [UI_MENU_DEFAULT_MARK] = "Standard",
    [UI_MENU_AFTER_RESTART] = "nach Neustart",
    [UI_MENU_SET_BY_ENV] = "durch Umgebungsvariable gesetzt",
    [UI_MENU_SAVE_FAILED] = "Einstellungen konnten nicht gespeichert werden",
    [UI_MENU_VERSION] = "TotK Recomp %s, Spiel %s",
    [UI_MENU_FULLSCREEN] = "Vollbild",
    [UI_MENU_FULLSCREEN_DESC] = "Das Spiel füllt den Bildschirm; F11 schaltet um.",
    [UI_MENU_WINDOW_SIZE] = "Fenstergröße",
    [UI_MENU_WINDOW_SIZE_DESC] = "Größe des Spielfensters außerhalb des Vollbilds.",
    [UI_MENU_WINDOW_SIZE_CUSTOM] = "Eigene",
    [UI_MENU_PRESENT] = "Bildausgabe",
    [UI_MENU_PRESENT_DESC] = "Wie fertige Bilder auf den Bildschirm gelangen.",
    [UI_MENU_PRESENT_MAILBOX] = "Mailbox",
    [UI_MENU_PRESENT_VSYNC] = "VSync",
    [UI_MENU_PRESENT_IMMEDIATE] = "Sofort",
    [UI_MENU_PRESENT_IN_USE] = "aktiv: %s",
    [UI_MENU_GPU] = "Grafikkarte",
    [UI_MENU_GPU_DESC] = "Das Vulkan-Gerät, das das Spiel zeichnet.",
    [UI_MENU_GPU_AUTO] = "Automatisch",
    [UI_MENU_PERF_OVERLAY] = "Leistungsanzeige",
    [UI_MENU_PERF_OVERLAY_DESC] = "Bildrate oben rechts. Voll zeigt zusätzlich Bildzeiten mit Grafik, CPU-, GPU- und "
                                  "Speicherlast, Temperaturen und die Wartezeiten des Render-Threads. F10 wechselt "
                                  "die Modi.",
    [UI_MENU_PERF_OFF] = "Aus",
    [UI_MENU_PERF_FPS] = "FPS",
    [UI_MENU_PERF_FULL] = "Voll",
    [UI_PERF_LINE_FPS] = "%.0f FPS",
    [UI_PERF_LINE_FPS_GEN] = "%.0f FPS gezeichnet, %.0f angezeigt",
    [UI_MENU_GRAPHICS] = "Bild",
    [UI_MENU_OUTPUT_RES] = "Spielauflösung",
    [UI_MENU_OUTPUT_RES_DESC] = "Die Größe, in der das Spiel Bild und Oberfläche zeichnet. Über 1920 x 1080 braucht "
                                "das Spiel mehr Videospeicher und GPU-Zeit. Ein breiteres Format (21:9) erweitert das "
                                "Sichtfeld der Kamera und rückt das HUD in die Bildschirmecken; ein höheres (16:10, "
                                "Steam Deck) rückt es an den oberen und unteren Rand.",
    [UI_MENU_HUD_WIDTH] = "HUD-Breite",
    [UI_MENU_HUD_WIDTH_DESC] = "Auf einem Bildschirm breiter als 16:9: wie weit linke und rechte Seite des HUD "
                               "auseinanderliegen. Volle Breite rückt sie an die Bildschirmränder; 21:9 oder 16:9 "
                               "hält das HUD in der Mitte des Bildschirms in diesem Format. Ein fehlendes Layout wird "
                               "im Hintergrund erstellt und nach einem Neustart verwendet.",
    [UI_MENU_HUD_WIDTH_FULL] = "Volle Breite",
    [UI_MENU_RENDER_SCALE] = "3D-Renderskalierung",
    [UI_MENU_RENDER_SCALE_DESC] = "Größe der 3D-Szene im Verhältnis zur Spielauflösung; das FSR des Spiels skaliert "
                                  "sie auf diese Größe. Über 100 % glättet Kanten (Supersampling), kostet aber viel "
                                  "GPU-Leistung.",
    [UI_MENU_RENDER_SCALE_GAME] = "Spiel (83 %)",
    [UI_MENU_DYNAMIC_RES] = "Dynamische Auflösung",
    [UI_MENU_DYNAMIC_RES_DESC] = "Lässt das Spiel die 3D-Szene verkleinern (auf 90 oder 80 %). Es entscheidet nach "
                                 "einer für die Switch gemachten Schätzung, die deine GPU nicht berücksichtigt, daher "
                                 "sinkt sie bei weiten Szenen selbst auf einem schnellen PC. Aus behält die "
                                 "Renderskalierung.",
    [UI_MENU_UPSCALER] = "Skalierung auf das Fenster",
    [UI_MENU_UPSCALER_DESC] = "Wie das Bild skaliert wird, wenn das Fenster größer als die Spielauflösung ist. FSR "
                              "hält Kanten und Details schärfer als bilinear.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1",
    [UI_MENU_UPSCALER_BILINEAR] = "Bilinear",
    [UI_MENU_ASPECT] = "Bildformat",
    [UI_MENU_ASPECT_DESC] = "Wenn das Fensterformat vom Format der Spielauflösung abweicht (ein 16:9-Bild auf einem "
                            "21:9-Bildschirm): Format mit schwarzen Balken beibehalten oder über das Fenster strecken.",
    [UI_MENU_ASPECT_FIT] = "Schwarze Balken",
    [UI_MENU_ASPECT_STRETCH] = "Strecken",
    [UI_MENU_MOVIE_ASPECT] = "Videoformat",
    [UI_MENU_MOVIE_ASPECT_DESC] = "Wenn die Spielauflösung nicht 16:9 ist: das eigene Format der "
                                  "Zwischensequenz-Videos mit schwarzen Balken beibehalten oder sie über das ganze "
                                  "Bild strecken. Wirkt ab dem nächsten Video.",
    [UI_MENU_MOVIE_ASPECT_KEEP] = "Schwarze Balken",
    [UI_MENU_MOVIE_ASPECT_STRETCH] = "Strecken",
    [UI_MENU_FOV] = "Sichtfeld",
    [UI_MENU_FOV_DESC] = "Wie viel von der Welt die Kamera zeigt, in Prozent des Spielwerts. Nur Spielversion 1.4.3.",
    [UI_MENU_VERT_PLUS] = "16:9-Breite behalten",
    [UI_MENU_VERT_PLUS_DESC] = "Auf Bildschirmen, die schmaler als 16:9 sind (Steam Deck, 16:10, 4:3), zeigt die Kamera dieselbe Breite der Welt wie bei 16:9 und mehr darüber und darunter. Bei 16:9 oder breiter ohne Wirkung.",
    [UI_MENU_SHADOW_RES] = "Größe der Schattenkarte",
    [UI_MENU_SHADOW_RES_DESC] = "Größe der Schattenkarte der Sonne. Die Größe des Spiels ändert sich mit der Ansicht, "
                                "bis etwa 400 nahe am Boden, daher wirken Blätterschatten klotzig und springen; eine "
                                "feste Größe hält sie ruhig und schärfer. Über 1024 kosten die Schatten mehr GPU-Zeit "
                                "und Videospeicher.",
    [UI_MENU_SHADOW_RES_GAME] = "Spiel (wechselnd)",
    [UI_MENU_ANISO] = "Texturfilterung",
    [UI_MENU_ANISO_DESC] = "Anisotrope Filterung der Oberflächentexturen: Boden, Wege und Wände bleiben im flachen "
                           "Winkel bis in die Ferne scharf statt zu verschwimmen. Spiel: die Stufe, die das Spiel "
                           "anfordert, wie auf der Switch.",
    [UI_MENU_ANISO_GAME] = "Spiel",
    [UI_MENU_LOD_STEADY] = "Konstante Details unter Last",
    [UI_MENU_LOD_STEADY_DESC] = "Überspringt die Vorkehrung des Spiels, Modell- und Texturdetails zu senken, wenn die "
                                "Bildrate fällt, wie es der Emulator-Mod Disable LOD Quality Reduction tut. "
                                "Spielversionen 1.1.0, 1.2.1 und 1.4.3. Auf schwachen PCs ausgeschaltet lassen: Dort hält "
                                "diese Absenkung die Bildrate oben.",
    [UI_MENU_SHARPNESS] = "FSR-Schärfe",
    [UI_MENU_SHARPNESS_DESC] = "Nachschärfen nach der FSR-Skalierung; 0 schaltet es aus.",
    [UI_MENU_COLOR] = "Farbe",
    [UI_MENU_COLOR_PRESET] = "Farbprofil",
    [UI_MENU_COLOR_PRESET_DESC] = "Setzt die vier Werte darunter. Lebendig: mehr Kontrast und Sättigung, Schatten "
                                  "etwas aufgehellt (für OLED-Bildschirme). Klarheit: etwas mehr Kontrast, Sättigung "
                                  "und Helligkeit. Wird ein Wert verändert, wird das Profil zu Eigenes. Das HUD "
                                  "ändert sich mit dem Bild, dieses Menü nicht.",
    [UI_MENU_COLOR_DEFAULT] = "Original",
    [UI_MENU_COLOR_VIVID] = "Lebendig (OLED)",
    [UI_MENU_COLOR_CLARITY] = "Klarheit",
    [UI_MENU_COLOR_CUSTOM] = "Eigenes",
    [UI_MENU_COLOR_BRIGHTNESS] = "Helligkeit",
    [UI_MENU_COLOR_BRIGHTNESS_DESC] = "Wird zu jeder Farbe addiert; 0 lässt sie unverändert.",
    [UI_MENU_COLOR_CONTRAST] = "Kontrast",
    [UI_MENU_COLOR_CONTRAST_DESC] = "Rückt Farben vom mittleren Grau weg (über 100 %) oder zu ihm hin; 100 % lässt "
                                    "sie unverändert.",
    [UI_MENU_COLOR_SATURATION] = "Sättigung",
    [UI_MENU_COLOR_SATURATION_DESC] = "Farbstärke gegenüber Grau; 100 % lässt sie unverändert.",
    [UI_MENU_COLOR_GAMMA] = "Gamma",
    [UI_MENU_COLOR_GAMMA_DESC] = "Unter 1 hellt dunkle und mittlere Töne auf, über 1 dunkelt sie ab; 1 lässt sie "
                                 "unverändert.",
    [UI_MENU_HDR_GROUP] = "HDR",
    [UI_MENU_HDR] = "HDR-Ausgabe",
    [UI_MENU_HDR_DESC] = "Helle Lichter auf einem HDR-Bildschirm. Erfordert eingeschaltetes HDR in Windows („HDR "
                         "verwenden“) oder einen Wayland-Compositor mit Farbverwaltung; ohne bleibt das Bild SDR. "
                         "Wirkt nach einem Neustart. Das Farbprofil oben gilt für SDR und wird bei HDR nicht "
                         "verwendet.",
    [UI_MENU_HDR_PEAK] = "Spitzenhelligkeit",
    [UI_MENU_HDR_PEAK_DESC] = "Die Spitzenhelligkeit deines Bildschirms in Nits; die hellsten Lichter enden dort.",
    [UI_MENU_HDR_GAME] = "Spielhelligkeit",
    [UI_MENU_HDR_GAME_DESC] = "Nits des Bildweiß (Papierweiß). 203 ist der HDR-Referenzwert; in einem hellen Raum "
                              "höher stellen.",
    [UI_MENU_HDR_UI] = "Helligkeit von HUD und Menü",
    [UI_MENU_HDR_UI_DESC] = "Nits des Weiß von HUD und diesem Menü.",
    [UI_MENU_HDR_HUE] = "Farbtonkorrektur für Spitzlichter",
    [UI_MENU_HDR_HUE_DESC] = "Helle Farben wie Feuer und Funken gehen wie im eigenen SDR-Bild des Spiels ins Weiße "
                             "über, passend zum Licht, das sie werfen; 0 lässt sie voll gesättigt.",
    [UI_MENU_SHADOW_SCALE] = "Schattenauflösung",
    [UI_MENU_SHADOW_SCALE_DESC] = "Zeichnet die Schattenkarten der Sonne je Seite so viel größer als die 960x960 des "
                                  "Spiels: schärfere Schattenkanten und Blätterschatten. Wirkt beim nächsten Start.",
    [UI_MENU_PCSS] = "Weiche Schatten",
    [UI_MENU_PCSS_DESC] = "Sonnenschatten scharf, wo ein Objekt den Boden berührt, und weicher, je weiter der "
                          "Schatten fällt (PCSS). 0 behält den Filter des Spiels. Wirkt beim nächsten Start.",
    [UI_MENU_CONTACT_SHADOWS] = "Kontaktschatten",
    [UI_MENU_CONTACT_SHADOWS_DESC] = "Kleine Sonnenschatten, die den Schattenkarten entgehen: an Füßen, unter "
                                     "Steinen, im Gras und in Falten. Nach der Beleuchtung im Bildraum berechnet. "
                                     "Wirkt beim nächsten Start.",
    [UI_MENU_SHADOW_RANGE] = "Reichweite der Schattenkaskaden",
    [UI_MENU_SHADOW_RANGE_DESC] = "Verschiebt die Stufen, ab denen Schatten weicher werden (im Spiel 20, 40, 80 und "
                                  "160 m), weiter weg. Verteilt die Schattenkarte auf mehr Boden: Schattenauflösung "
                                  "mit erhöhen. Wirkt beim nächsten Start.",
    [UI_MENU_GRASS_RANGE] = "Grasreichweite",
    [UI_MENU_GRASS_RANGE_DESC] = "Wie weit um die Kamera herum Gras wächst (im Spiel 62,5 m). Höher hält das Gras auf "
                                 "Wiesen weiter draußen, statt es erst direkt vor dir wachsen zu lassen; niedriger "
                                 "zeichnet weniger davon. Wirkt beim nächsten Start.",
    [UI_MENU_DLSS_PRESET] = "DLSS-Modell",
    [UI_MENU_DLSS_PRESET_DESC] = "Welches DLSS-Modell läuft. Automatisch folgt der Empfehlung von NVIDIA je Modus: K "
                                 "für DLAA, Qualität und Ausgewogen, M für Leistung, L für Ultra-Leistung. J: etwas "
                                 "weniger Ghosting hinter bewegten Figuren, etwas mehr Flimmern. K: das schärfste "
                                 "Modell in jedem Modus. Wirkt sofort.",
    [UI_MENU_DLSS_PRESET_DEFAULT] = "Automatisch",
    [UI_MENU_DLSS_PRESET_K] = "K (schärfstes)",
    [UI_MENU_DLSS_PRESET_J] = "J (weniger Ghosting)",
    [UI_MENU_DLSS_CHARS] = "DLSS-Spuren an Figuren",
    [UI_MENU_DLSS_CHARS_DESC] = "Verringert die schwachen Spuren hinter bewegten Figuren, Tieren und dem, was sie "
                                "halten: DLSS nimmt dort weniger aus den vorigen Bildern. Eine stärkere Stufe "
                                "entfernt mehr von den Spuren und lässt diese Kanten etwas weniger glatt. Wirkt "
                                "sofort.",
    [UI_MENU_DLSS_CHARS_OFF] = "Aus",
    [UI_MENU_DLSS_CHARS_LIGHT] = "Leicht",
    [UI_MENU_DLSS_CHARS_MEDIUM] = "Mittel",
    [UI_MENU_DLSS_CHARS_FULL] = "Voll",
    [UI_MENU_DLSS] = "Skalierungsmodus",
    [UI_MENU_DLSS_DESC] = "Nativ glättet die Kanten der 3D-Szene in voller Auflösung. Qualität, Ausgewogen, Leistung "
                          "und Ultra-Leistung zeichnen sie mit 67, 58, 50 bzw. 33 %, und der Upscaler (DLSS oder FSR "
                          "3.1) skaliert sie hoch, was GPU-Zeit spart; sie ersetzen die 3D-Renderskalierung. Wirkt "
                          "sofort.",
    [UI_MENU_DLSS_DLAA] = "Nativ",
    [UI_MENU_DLSS_QUALITY] = "Qualität",
    [UI_MENU_DLSS_BALANCED] = "Ausgewogen",
    [UI_MENU_DLSS_PERFORMANCE] = "Leistung",
    [UI_MENU_DLSS_ULTRA] = "Ultra-Leistung",
    [UI_MENU_FXAA] = "Kantenglättung des Spiels (FXAA)",
    [UI_MENU_FXAA_DESC] = "Die FXAA-Kantenglättung des Spiels. Sie verwischt feine Details etwas; mit DLAA wird sie "
                          "nicht gebraucht.",
    [UI_MENU_REFLEX] = "Niedrige Latenz",
    [UI_MENU_REFLEX_DESC] = "Senkt die Systemlatenz. Ein nutzt NVIDIA Reflex (VK_NV_low_latency2); Ein + Boost hält zusätzlich die NVIDIA-GPU-Takte in CPU-begrenzten Szenen hoch. Automatisch nutzt Reflex auf NVIDIA und AMD Anti-Lag 2 (VK_AMD_anti_lag) auf AMD. Gilt nach einem Neustart.",
    [UI_MENU_REFLEX_AUTO] = "Automatisch",
    [UI_MENU_REFLEX_OFF] = "Aus",
    [UI_MENU_REFLEX_ON] = "Ein",
    [UI_MENU_REFLEX_BOOST] = "Ein + Boost",
    [UI_MENU_SCENE_UPSCALER] = "3D-Szenen-Upscaler",
    [UI_MENU_SCENE_UPSCALER_DESC] = "Ein temporaler Upscaler für die 3D-Szene: glattere Kanten und weniger Flimmern, "
                                    "und in einem Modus unter Nativ zeichnet er die Szene kleiner und skaliert sie "
                                    "hoch, was GPU-Zeit spart. NVIDIA DLSS braucht eine NVIDIA-RTX-GPU und " DLSS_FILE
                                    " neben " EXE_FILE "; AMD FSR 3.1 läuft auf jeder GPU. Aus behält die eigene "
                                    "Skalierung des Spiels und FXAA. Das HUD bleibt unberührt; die Skalierung auf das "
                                    "Fenster und die Schärfe oben passen das Spielbild weiterhin an das Fenster an. "
                                    "Wirkt nach einem Neustart.",
    [UI_MENU_SCENE_UPSCALER_OFF] = "Aus",
    [UI_MENU_SCENE_UPSCALER_DLSS] = "NVIDIA DLSS",
    [UI_MENU_SCENE_UPSCALER_FSR3] = "AMD FSR 3.1",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_GPU] = "DLSS: braucht eine NVIDIA-RTX-GPU",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_DLL] = "DLSS: " DLSS_FILE " fehlt",
    [UI_MENU_FSR_SHARPNESS] = "FSR-Schärfe",
    [UI_MENU_FSR_SHARPNESS_DESC] = "Nachschärfen der 3D-Szene nach AMD FSR 3.1 oder FSR 4; 0 schaltet es aus.",
    [UI_MENU_SCENE_UPSCALER_FSR4] = "AMD FSR 4",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_GPU] = "FSR 4: für diese GPU nicht angeboten",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_DLL] = "FSR 4: Die FSR-Bibliotheken von AMD fehlen",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_OS] = "FSR 4: nur unter Windows",
    [UI_MENU_FPS_LIMIT] = "Bildratenbegrenzung",
    [UI_MENU_FPS_LIMIT_DESC] = "Die höchste Bildrate, die das Spiel zeichnet. Über 60 ist experimentell und erfordert "
                               "„Spieltempo folgt der Uhr“.",
    [UI_MENU_FPS_30] = "30 FPS",
    [UI_MENU_FPS_40] = "40 FPS",
    [UI_MENU_FPS_45] = "45 FPS",
    [UI_MENU_FPS_60] = "60 FPS",
    [UI_MENU_FPS_90] = "90 FPS",
    [UI_MENU_FPS_120] = "120 FPS",
    [UI_MENU_FPS_144] = "144 FPS",
    [UI_MENU_FPS_DISPLAY] = "Monitorfrequenz",
    [UI_MENU_FPS_DISPLAY_HZ] = "Monitorfrequenz (%.0f Hz)",
    [UI_MENU_FRAME_QUEUE] = "Bildwarteschlange",
    [UI_MENU_FRAME_QUEUE_DESC] = "Das Spiel beginnt ein Bild, während das vorige noch darauf wartet, angezeigt zu "
                                 "werden: viel weniger ausgelassene Bilder an der Bildratenbegrenzung, bis zu ein "
                                 "Bild mehr Eingabeverzögerung. Wirkt sofort.",
    [UI_MENU_FRAME_GEN] = "Bildgenerierung",
    [UI_MENU_FRAME_GEN_OFF] = "Aus",
    [UI_MENU_FRAME_GEN_FSR3] = "AMD FSR 3",
    [UI_MENU_FRAME_GEN_DESC] = "Ein erzeugtes Bild zwischen je zwei, die das Spiel zeichnet: doppelt so viele Bilder "
                               "auf dem Bildschirm, das Spiel bei der halben Bildratenbegrenzung und die Steuerung "
                               "etwa ein halbes Bild später. Für Bildschirme mit 120 Hz und mehr. Nicht mit DLSS oder "
                               "HDR. Wirkt sofort.",
    [UI_MENU_REALTIME] = "Spieltempo folgt der Uhr",
    [UI_MENU_REALTIME_DESC] = "Lässt das Spiel nach Echtzeit laufen, sodass ein langsames Bild das Spiel nicht "
                              "verlangsamt.",
    [UI_MENU_CLOTH_REALTIME] = "Stoff nach der Uhr",
    [UI_MENU_CLOTH_REALTIME_DESC] = "Lässt auch Stoff und Haare nach Echtzeit laufen.",
    [UI_MENU_HIGHFPS_FIX] = "Korrekturen für hohe Bildraten",
    [UI_MENU_HIGHFPS_FIX_DESC] = "Korrigiert Physik, die über 30 Bildern pro Sekunde falsch läuft, und Objekte, die "
                                 "über 60 durchsichtig oder unsichtbar bleiben.",
    [UI_MENU_CLIMB_FIX] = "Klettertempo-Korrektur",
    [UI_MENU_CLIMB_FIX_DESC] = "Hält das Klettern an Wänden und Leitern über 60 Bildern pro Sekunde auf voller "
                               "Geschwindigkeit, sodass Link oben ankommt.",
    [UI_MENU_GAME_LANG] = "Spielsprache",
    [UI_MENU_GAME_LANG_DESC] = "Sprache der Texte und Stimmen des Spiels.",
    [UI_MENU_GAME_LANG_AUTO] = "Automatisch",
    [UI_MENU_OPERATION_MODE] = "Konsolenmodus",
    [UI_MENU_OPERATION_MODE_DESC] = "Ob das Spiel glaubt, im TV- oder Handheld-Modus zu laufen.",
    [UI_MENU_MODE_DOCKED] = "TV-Modus",
    [UI_MENU_MODE_HANDHELD] = "Handheld",
    [UI_MENU_ASSUME_FOCUS] = "Im Hintergrund weiterlaufen",
    [UI_MENU_ASSUME_FOCUS_DESC] = "Ein (Standard): Das Spiel läuft weiter, während du in einem anderen Fenster bist, "
                                  "und die Tastatur bleibt still, bis das Fenster wieder vorne ist. Aus: Es pausiert, "
                                  "wenn sein Fenster den Fokus verliert, wie auf der Switch beim Drücken von Home; "
                                  "ein so unterbrochener Ladevorgang kann dauerhaft hängen bleiben.",
    [UI_MENU_PLAY_TIME] = "Spielzeit",
    [UI_MENU_PLAY_TIME_DESC] = "Zeit im Spiel über alle Sitzungen, gezählt, solange das Spielfenster den Fokus hat "
                               "und das Spiel Bilder zeichnet. Gespeichert in playtime.txt im Profilordner, neben den "
                               "Spielständen.",
    [UI_MENU_PLAY_TIME_HM] = "%llu h %02u min",
    [UI_MENU_PLAY_TIME_SESSION] = "diesmal: %s",
    [UI_MENU_MODS_BUILTIN] = "Im Port enthalten",
    [UI_MENU_MODS_INSTALLED] = "Installierte Mods",
    [UI_MENU_MODS_NONE] = "Noch keine Mods. Entpacke einen Mod in den Ordner mods, ein Ordner pro Mod mit seinem "
                          "romfs-Ordner darin, und starte das Spiel neu. TKMM kann seine zusammengeführten Mods "
                          "ebenfalls in einen Ordner dort exportieren.",
    [UI_MENU_MODS_OPEN] = "Mods-Ordner öffnen",
    [UI_MENU_MODS_OPEN_DESC] = "Zeigt den Ordner, aus dem das Spiel Mods lädt. Er wird angelegt, falls er fehlt.",
    [UI_MENU_MODS_ROW_DESC] = "Ersetzte Spieldateien: %u. Neu hinzugefügte Dateien: %u.",
    [UI_MENU_MODS_OVERRIDDEN] = "%u seiner Dateien kommen aus %s, das weiter oben in der Liste steht.",
    [UI_MENU_MODS_MOVE_HINT] = "Die obere Aktionstaste oder Strg+Hoch schiebt ihn nach oben, die linke Aktionstaste "
                               "oder Strg+Runter nach unten. Ändern zwei Mods dieselbe Datei, gewinnt der obere. "
                               "Änderungen wirken nach einem Neustart.",
    [UI_MENU_MODS_EMPTY] = "Dieser Ordner enthält keine romfs-, exefs- oder cheats-Dateien.",
    [UI_MENU_MODS_SUBSDK] = "Dieser Mod bringt eigenen Spielcode mit (ein subsdk-Plugin oder ein ersetztes main). Der "
                            "Port kann keine Codemodule laden, daher bleibt der ganze Mod aus.",
    [UI_MENU_MODS_EXEFS_LATER] = "Dieser Mod patcht den Spielcode (exefs). Das Spiel baut die Patches beim nächsten "
                                 "Start ein (ein Neubau von einigen Minuten); bis dahin bleibt der ganze Mod aus.",
    [UI_MENU_MODS_CHEATS] = "Cheats: %d. Schalte sie im Tab Cheats.",
    [UI_MENU_MODS_CHEATS_FILE] = "Cheats für diese Spielversion werden beim Start aus cheats\\%s in diesem Mod "
                                 "gelesen.",
    [UI_MENU_MODS_BAD_NAME] = "Der Ordnername enthält |, das die Mod-Liste als Trennzeichen verwendet. Benenne den "
                              "Ordner um.",
    [UI_MENU_MODS_OTHER_VERSION] = "%u seiner Dateien sind für eine andere Spielversion gemacht und werden "
                                   "ausgelassen. Suche eine Version dieses Mods für deine Spielversion.",
    [UI_MENU_MODS_UNUSED] = "nicht verwendet",
    [UI_MENU_MODS_FPS60] = "60-FPS-Patch",
    [UI_MENU_MODS_FPS60_DESC] = "Der eigene Patch des Ports für 60 Bilder pro Sekunde. Einstellbar über die "
                                "Bildratenbegrenzung im Tab Spiel.",
    [UI_MENU_MODS_RES] = "Auflösungspatch",
    [UI_MENU_MODS_RES_DESC] = "Der eigene Patch des Ports für Auflösungen über 1080p. Einstellbar über die "
                              "Spielauflösung im Tab Bild.",
    [UI_MENU_MODS_QUICKMENU] = "Schnellmenü-Korrektur",
    [UI_MENU_MODS_QUICKMENU_DESC] = "Die Korrektur des Ports für den schwarzen Hintergrund der Waffen-, Schild- und "
                                    "Materialmenüs bei 60 FPS.",
    [UI_MENU_VOLUME] = "Lautstärke",
    [UI_MENU_VOLUME_DESC] = "Gesamtlautstärke des Spiels.",
    [UI_MENU_VOLUME_MUSIC] = "Musiklautstärke",
    [UI_MENU_VOLUME_MUSIC_DESC] = "Lautstärke der Hintergrundmusik.",
    [UI_MENU_VOLUME_SFX] = "Effektlautstärke",
    [UI_MENU_VOLUME_SFX_DESC] = "Lautstärke der Soundeffekte.",
    [UI_MENU_VOLUME_VOICE] = "Sprachlautstärke",
    [UI_MENU_VOLUME_VOICE_DESC] = "Lautstärke der Stimmen der Figuren.",
    [UI_MENU_AUDIO_CHANNELS] = "Lautsprecher",
    [UI_MENU_AUDIO_CHANNELS_DESC] = "Stereo- oder 5.1-Surround-Ausgabe. Automatisch wählt 5.1, wenn die "
                                    "Lautsprecherkonfiguration von Windows sechs oder mehr Kanäle hat. Wirkt nach "
                                    "einem Neustart.",
    [UI_MENU_CHANNELS_AUTO] = "Automatisch",
    [UI_MENU_CHANNELS_STEREO] = "Stereo",
    [UI_MENU_CHANNELS_51] = "5.1-Surround",
    [UI_MENU_AUDIO_DEVICE] = "Ausgabegerät",
    [UI_MENU_AUDIO_DEVICE_DESC] = "Wo der Ton abgespielt wird. Systemstandard folgt dem Standardgerät von Windows. "
                                  "Ein gewähltes Gerät, das getrennt wird, fällt auf den Systemstandard zurück, bis "
                                  "es wieder da ist. Wirkt sofort.",
    [UI_MENU_AUDIO_DEVICE_DEFAULT] = "Systemstandard",
    [UI_MENU_MOTION] = "Bewegungssteuerung",
    [UI_MENU_MOTION_DESC] = "Zielen mit dem Gyroskop des Controllers.",
    [UI_MENU_GYRO_CALIBRATION] = "Gyro-Nullpunktkorrektur",
    [UI_MENU_GYRO_CALIBRATION_DESC] = "Zieht die Ruhedrift des Controllers ab, gemessen mit der Kalibrieren-Zeile "
                                      "darunter. Die Messwerte bleiben gespeichert, wenn dies aus ist.",
    [UI_MENU_GYRO_CALIBRATE] = "Gyro kalibrieren",
    [UI_MENU_GYRO_CALIBRATE_DESC] = "Misst die Ruhedrift des Gyroskops und entfernt sie. Lege den Controller ab oder "
                                    "halte ihn zwei Sekunden lang still.",
    [UI_MENU_GYRO_CALIBRATE_WAIT] = "Kalibrierung...",
    [UI_MENU_GYRO_CALIBRATED] = "%s, kalibriert",
    [UI_MENU_GYRO_CALIBRATE_FAILED] = "Kalibrierung misslungen",
    [UI_MENU_GYRO_CALIBRATE_MOVED] = "Controller bewegt: halte ihn still und versuche es erneut",
    [UI_MENU_GYRO_AUTO_BIAS] = "Gyro-Drift in Ruhe lernen",
    [UI_MENU_GYRO_AUTO_BIAS_DESC] = "Lernt die im Gyroskop verbleibende Drift, wann immer der Controller still auf "
                                    "einem Tisch liegt, und zieht sie ab. Für Bogenzielen, das auch nach der "
                                    "Kalibrierung langsam wegdriftet. Von einem Controller in der Hand wird nicht "
                                    "gelernt. Experimentell.",
    [UI_MENU_RUMBLE_STRENGTH] = "Vibrationsstärke",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "Wie stark der Controller vibriert.",
    [UI_MENU_RUMBLE_CURVE] = "Vibrationskurve",
    [UI_MENU_RUMBLE_CURVE_DESC] = "Niedrigere Werte verstärken schwache Vibration.",
    [UI_MENU_RUMBLE_TRIGGERS] = "Trigger-Vibration (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Lässt auch die Trigger eines Xbox-Controllers vibrieren.",
    [UI_MENU_PAD_LAYOUT] = "Tastenbelegung",
    [UI_MENU_PAD_LAYOUT_DESC] = "Welche Taste A ist. Nintendo: die rechte, wie auf der Switch. Xbox: die untere, wie "
                                "auf einem Xbox-Controller beschriftet (Kreuz auf einem PlayStation-Controller). "
                                "Automatisch wählt Nintendo für Controller mit Nintendo-Beschriftung und Xbox für die "
                                "übrigen. Welche Controller-Tasten das Spiel anzeigt, wird unter Tastensymbole "
                                "eingestellt.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Automatisch",
    [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A rechts)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A unten)",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "aktiv: %s",
    [UI_MENU_PROMPTS] = "Tastensymbole",
    [UI_MENU_PROMPTS_DESC] = "Welche Controller-Tasten die Tastensymbole des Spiels zeigen. Nintendo: die eigenen des "
                             "Spiels. Xbox: LB, RB, LT, RT, Menü, Ansicht und ein Steuerkreuz, dazu A, B, X und Y so, "
                             "wie die Taste beschriftet ist, die die Tastenbelegung oben drückt (mit Nintendo (A "
                             "rechts) ist die rechte Taste mit B beschriftet, daher zeigen die Symbole für A ein B). "
                             "PlayStation: die Formen auf einem PlayStation-Controller (○ × △ □, L1, R1, L2, R2, "
                             "Options und Share) auf der Taste, die die Tastenbelegung oben drückt. PC: die Tasten "
                             "und Maustasten der Standardbelegung der Tastatur (E, Umschalt, Leertaste, "
                             "Linksklick...) und die Maus für die Kamera. Das Spiel liest seine Symbole beim Start, "
                             "daher ändern sie sich nach einem Neustart. Sie sind Teil der Spieldateien, daher behält "
                             "eine Taste, die du unter Steuerung umbelegst, ihr altes Symbol auf dem Bildschirm.",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo",
    [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation",
    [UI_MENU_PROMPTS_PC] = "PC",
    [UI_MENU_PROMPTS_MISSING] = "Symboldateien fehlen: Setup erneut ausführen",
    [UI_MENU_PROMPTS_FAILED] = "Symboldateien beschädigt: Setup erneut ausführen",
    [UI_MENU_PROMPTS_RESTART] = "Symbole ändern sich nach Neustart",
    [UI_MENU_PROMPTS_OFF_SHAPE] = "aus bei einer Spielauflösung, die nicht 16:9 ist",
    [UI_MENU_OUTPUT_RES_NO_LAYOUT] = "kein Layout für dieses Format erstellt: Setup erneut ausführen",
    [UI_MENU_OUTPUT_RES_PREPARING] = "Layout wird im Hintergrund erstellt; ab dem nächsten Start verwendet",
    [UI_MENU_PROMPTS_PREPARING] = "Symboldateien werden im Hintergrund erstellt; ab dem nächsten Start verwendet",
    [UI_MENU_PREPARE_RETRY] = "Erneut versuchen",
    [UI_MENU_PREPARE_RETRY_DESC] = "Startet die Werkzeuge des Setups erneut, um das fehlende Oberflächen-Layout oder "
                                   "die Symboldateien der Tastensymbole im Hintergrund zu erstellen. Das Spiel läuft "
                                   "weiter; es verwendet sie ab seinem nächsten Start.",
    [UI_MENU_SHAPE_PREPARE_FAILED] = "die Werkzeuge des Setups starteten nicht: Setup erneut ausführen",
    [UI_MENU_PROMPTS_OFF_MODS] = "aus: ein RomFS-Mod ersetzt ihre Dateien",
    [UI_MENU_PROMPTS_OFF_PATCHES] = "aus, solange TOTK_ROMFS_PATCHES gesetzt ist",
    [UI_MENU_RESTART] = "Jetzt neu starten",
    [UI_MENU_RESTART_DESC] = "Schließt das Spiel und startet es mit den neuen Symbolen erneut. Fortschritt seit dem "
                             "letzten Speichern geht verloren.",
    [UI_MENU_RESTART_CONFIRM] = "Spiel jetzt neu starten? Fortschritt seit dem letzten Speichern geht verloren.",
    [UI_MENU_RESTART_YES] = "Neu starten",
    [UI_MENU_RESTART_SAVING] = "Das Spiel speichert: Neustart ist gleich möglich.",
    [UI_MENU_GROUP_STICKS] = "Analogsticks",
    [UI_MENU_LSTICK_DEADZONE] = "Totzone linker Stick",
    [UI_MENU_RSTICK_DEADZONE] = "Totzone rechter Stick",
    [UI_MENU_STICK_DEADZONE_DESC] = "Wie weit sich ein Stick bewegen muss, bevor das Spiel es bemerkt. Erhöhe sie, "
                                    "wenn sich Figur oder Kamera von selbst bewegen: Stick loslassen, „Stick jetzt“ "
                                    "ablesen und die Totzone einige Prozent darüber setzen.",
    [UI_MENU_STICK_DEADZONE_NOW] = "Stick jetzt: %.0f %%",
    [UI_MENU_MOUSE_MODE] = "Mauskamera",
    [UI_MENU_MOUSE_MODE_DESC] = "Direkt: Die Kamera dreht sich so weit, wie sich die Maus bewegt, wie in einem "
                                "PC-Spiel, ohne Totzone oder Kurve des Sticks. Stick-Emulation: Die Maus wirkt wie der rechte Stick.",
    [UI_MENU_MOUSE_STICK] = "Stick-Emulation",
    [UI_MENU_MOUSE_DIRECT] = "Direkt",
    [UI_MENU_MENU_CURSOR] = "Maus in den Spielmenüs",
    [UI_MENU_MENU_CURSOR_DESC] = "In den eigenen Menüs des Spiels (Titel, Inventar, Läden, Antwortauswahl in "
                                 "Dialogen) erscheint der Zeiger: Das Element darunter wird ausgewählt, Linksklick "
                                 "bestätigt, Rechtsklick geht zurück. Nur wenn zuletzt die Maus oder eine Taste "
                                 "benutzt wurde, sodass die Menüs für einen Controller unverändert bleiben (Spiel "
                                 "1.4.3).",
    [UI_MENU_MAP_CLICK] = "Linksklick auf der Karte",
    [UI_MENU_MAP_CLICK_DESC] = "Auf der Karte des Purah-Pads drückt die linke Maustaste A am Kartencursor statt der "
                               "Taste, auf die sie gelegt ist (standardmäßig Y). Erfordert „Maus in den Spielmenüs“ "
                               "(Spiel 1.4.3).",
    [UI_MENU_DIALOGUE_CLICK] = "Klicks im Dialog",
    [UI_MENU_DIALOGUE_CLICK_DESC] = "Solange ein Dialogfenster offen ist, drückt Linksklick B (setzt den Text fort "
                                    "und überspringt sein schrittweises Einblenden) und Rechtsklick A, statt der "
                                    "Tasten, auf die sie gelegt sind (standardmäßig Y, das das Dialogprotokoll "
                                    "öffnet, und ZL). Bei der Antwortauswahl bleibt der Zeiger: Linksklick wählt "
                                    "(Spiel 1.4.3).",
    [UI_MENU_POINTER_IMAGE] = "Spiel-Mauszeiger",
    [UI_MENU_POINTER_IMAGE_DESC] = "Solange der Cursor nicht gefangen ist (Spielmenüs, dieses Menü, nach Esc), zeigt "
                                   "das Fenster statt des Systempfeils eine eigene goldene Pfeilspitze, passend zur "
                                   "Skalierung des Bildschirms.",
    [UI_MENU_QUICKSAVE] = "Schnellspeichern/-laden",
    [UI_MENU_QUICKSAVE_DESC] = "Eine Taste speichert das Spiel sofort, eine andere lädt diesen Spielstand (die zwei "
                               "Zeilen darunter: standardmäßig F6 und F7, wie in Half-Life), ohne Menü auf dem "
                               "Bildschirm. Nur Tastatur- und Maustasten, nur Spiel 1.4.3. Schnellspeichern fordert "
                               "vom Spiel eine seiner eigenen automatischen Speicherungen an, ersetzt also die "
                               "älteste automatische Speicherung (der manuelle Spielstand bleibt), behält eine Kopie "
                               "davon als Schnellspeicherstand (der Ordner quicksave im Profil) und zeigt \"Spiel "
                               "gespeichert\" oben im Bild. Schnellladen hält das Bild auf \"Schnellspeicherstand "
                               "wird geladen\", während es unsichtbar die Ladeliste des Spiels durchgeht, und das "
                               "Spiel lädt den Schnellspeicherstand (etwa 5 s). Wo das Spiel selbst keine "
                               "automatische Speicherung anlegen würde (Zwischensequenz, Dialog), wird nichts "
                               "gespeichert und die Zeile sagt das.",
    [UI_MENU_QUICKSAVE_KEY] = "Schnellspeichertaste",
    [UI_MENU_QUICKLOAD_KEY] = "Schnellladetaste",
    [UI_MENU_QUICKSAVE_KEY_DESC] = "Die Taste, die Schnellspeichern oder Schnellladen auslöst; standardmäßig F6 und "
                                   "F7, wie in Half-Life. Wähle die Zelle und drücke eine beliebige Taste oder "
                                   "Maustaste; Entf lässt sie ohne Taste. Solange Schnellspeichern an ist, tun die "
                                   "beiden Tasten nichts anderes (ein Hinweis erscheint, wenn eine Belegung die Taste "
                                   "ebenfalls verwendet). totk.ini nimmt jeden Tastennamen der Belegungen "
                                   "(quicksave_key, quickload_key).",
    [UI_QS_SAVED] = "Spiel gespeichert",
    [UI_QS_LOADING] = "Schnellspeicherstand wird geladen",
    [UI_QS_NOT_NOW] = "Schnellspeichern ist gerade nicht möglich",
    [UI_QS_NONE] = "Noch kein Schnellspeicherstand vorhanden (die Schnellspeichertaste legt einen an)",
    [UI_QS_FAILED] = "Schnellspeichern fehlgeschlagen: siehe totk.log",
    [UI_QS_LOAD_FAILED] = "Schnellladen fehlgeschlagen: siehe totk.log",
    [UI_MENU_MOUSE_SENS] = "Mausempfindlichkeit der Kamera",
    [UI_MENU_MOUSE_SENS_DESC] = "Wie schnell die Maus die Kamera dreht.",
    [UI_MENU_WALK_KEY] = "Gehen-Taste",
    [UI_MENU_WALK_KEY_DESC] = "Gedrückt halten, um die Tastatur-Sticks nur teilweise zu neigen, sodass die Tastatur "
                              "geht statt rennt. Der Geh-Anteil legt fest, wie weit. Controller und Maus sind nicht "
                              "betroffen.",
    [UI_MENU_WALK_KEY_TAKEN] = "auch %s",
    [UI_MENU_WALK_SCALE] = "Geh-Anteil",
    [UI_MENU_WALK_SCALE_DESC] = "Die Neigung der Tastatur-Sticks, solange die Gehen-Taste gehalten wird. Die Hälfte "
                                "ist Gehen, volle Neigung Rennen.",
    [UI_MENU_FREECAM] = "Freie Kamera",
    [UI_MENU_FREECAM_DESC] = "Lässt eine Tastenkombination oder eine Taste darunter die Ansicht von Link lösen und "
                             "frei fliegen. Standardmäßig aus: Ist sie aus, läuft der Kameracode des Spiels "
                             "unverändert. Wirkt nach einem Neustart. Spielversion 1.4.3.",
    [UI_MENU_FREECAM_OFF_NOTE] = "Schalte oben Freie Kamera ein und starte dann neu",
    [UI_MENU_FREECAM_PAD] = "Freie-Kamera-Kombi",
    [UI_MENU_FREECAM_PAD_DESC] = "Gleichzeitig gedrückte Controller-Tasten, die die Ansicht durch drei Stufen "
                                 "schalten: Sie verlässt Link und fliegt, sie bleibt, wo sie gelassen wurde, während "
                                 "Link weiterspielt, und sie kehrt zurück. Während sie fliegt, erhält das Spiel keine "
                                 "Eingaben: Der linke Stick fliegt entlang der Blickrichtung und seitwärts, die "
                                 "Trigger nach oben und unten, der rechte Stick dreht, und jeder Druck auf L oder R "
                                 "halbiert oder verdoppelt das Tempo. Bei festgestellter Ansicht geht Link dorthin, "
                                 "wohin der linke Stick auf dem Bildschirm zeigt. Spielversion 1.4.3.",
    [UI_MENU_FREECAM_PAD_V143] = "Nur Spielversion 1.4.3",
    [UI_MENU_FREECAM_KEY] = "Freie-Kamera-Taste",
    [UI_MENU_FREECAM_KEY_DESC] = "Eine Taste, die die Ansicht durch dieselben Stufen schaltet: Sie verlässt Link und "
                                 "fliegt, sie bleibt, während Link weiterspielt, und sie kehrt zurück. Während sie "
                                 "fliegt: W A S D fliegen entlang der Blickrichtung und seitwärts, die auf ZR und ZL "
                                 "gelegten Tasten (standardmäßig X und Z) nach oben und unten, Maus und Pfeiltasten "
                                 "drehen, und die auf L und R gelegten Tasten (Q und R) halbieren oder verdoppeln das "
                                 "Tempo. Spielversion 1.4.3.",
    [UI_MENU_GROUP_GAMEPLAY_TWEAKS] = "Gameplay-Anpassungen",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED] = "Gameplay-Anpassungen",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED_DESC] = "Mausrad-Steuerung für Drehung und Abstand der Ultrahand, Wechsel von "
                                             "Gegenständen und Waffen und sanften Karten-Zoom.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE] = "Ultrahand-Drehung per Mausrad",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE_DESC] = "Mit der Ultrahand gehaltene Objekte direkt mit dem Mausrad drehen.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS] = "Ultrahand-Drehachse mit C",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS_DESC] = "C beim Scrollen halten, um gehaltene Objekte um die andere Achse zu "
                                               "drehen.",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY] = "Ultrahand-Drehempfindlichkeit",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY_DESC] = "Wie schnell das Mausrad gehaltene Objekte dreht.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE] = "Ultrahand-Abstand per Mausrad",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE_DESC] = "Rechtsklick + Mausrad schiebt gehaltene Objekte weg oder zieht sie "
                                                 "heran.",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY] = "Ultrahand-Abstandsempfindlichkeit",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY_DESC] = "Wie schnell das Mausrad den Objektabstand ändert.",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL] = "Waffenwahl per Mausrad",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL_DESC] = "Beim freien Erkunden das Mausrad drehen, um Waffen zu wechseln. Mit E "
                                           "bestätigen.",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY] = "Empfindlichkeit der Waffenwahl",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY_DESC] = "Nötige Mausrad-Schritte bis zur nächsten Waffe.",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL] = "Schnellmenü-Auswahl per Mausrad",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL_DESC] = "Das Mausrad drehen, um bei offenem Schnellauswahlmenü durch die Gegenstände "
                                         "zu blättern.",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY] = "Mausrad-Empfindlichkeit im Schnellmenü",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY_DESC] = "Nötige Mausrad-Schritte, um in Schnellauswahlmenüs zum nächsten "
                                               "Gegenstand zu gelangen.",
    [UI_MENU_GAMEPLAY_ITEM_AD] = "Schnellmenü-Navigation mit A / D",
    [UI_MENU_GAMEPLAY_ITEM_AD_DESC] = "A / D drücken, um in Schnellauswahlmenüs nach links und rechts zu wechseln.",
    [UI_MENU_GAMEPLAY_MAP_ZOOM] = "Sanfter Karten-Zoom",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_DESC] = "Sanfter Zoom mit Nachlauf auf der Karte des Purah-Pads per Mausrad.",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY] = "Karten-Zoomempfindlichkeit",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY_DESC] = "Wie schnell das Mausrad die Karte zoomt.",
    [UI_MENU_STICK_SENS] = "Kameratempo des Sticks",
    [UI_MENU_STICK_SENS_DESC] = "Vervielfacht das Kameratempo des rechten Sticks im Spiel, Höchsttempo "
                                "eingeschlossen, bei jeder der fünf Kameraempfindlichkeiten des Spiels. Die Maus "
                                "behält ihre eigene Einstellung.",
    [UI_BINDS_HELP] = "Wähle eine Zelle und drücke A oder Enter, dann die Taste, Maustaste oder Controller-Taste. "
                      "Entf oder X leert eine Zelle. Eine schon belegte Taste wandert hierher; eine Controller-Taste "
                      "tauscht mit der, die sie ersetzt.",
    [UI_BINDS_COL_KEY1] = "Taste 1",
    [UI_BINDS_COL_KEY2] = "Taste 2",
    [UI_BINDS_COL_PAD] = "Controller",
    [UI_BINDS_GROUP_BUTTONS] = "Tasten",
    [UI_BINDS_GROUP_MOVE] = "Bewegung",
    [UI_BINDS_GROUP_CAMERA] = "Kamera",
    [UI_BINDS_PRESS_KEY] = "Drücke eine Taste oder Maustaste für %s",
    [UI_BINDS_PRESS_PAD] = "Drücke eine Controller-Taste für %s",
    [UI_BINDS_CANCEL_HINT] = "Esc bricht ab, oder %d s warten",
    [UI_BINDS_TAKEN] = "%s wurde von %s entfernt",
    [UI_BINDS_SWAPPED] = "%s mit %s getauscht",
    [UI_BINDS_RESET] = "Alle Tasten zurücksetzen",
    [UI_BINDS_RESET_DESC] = "Zurück zu den Standardtasten und Controller-Tasten. Die Tastenbelegung oben gilt wieder: "
                            "umbelegte Controller-Tasten bleiben bis zu diesem Zurücksetzen, wo du sie hingelegt hast.",
    [UI_BIND_A] = "A",
    [UI_BIND_B] = "B",
    [UI_BIND_X] = "X",
    [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "Linker Stick drücken",
    [UI_BIND_RCLICK] = "Rechter Stick drücken",
    [UI_BIND_L] = "L",
    [UI_BIND_R] = "R",
    [UI_BIND_ZL] = "ZL",
    [UI_BIND_ZR] = "ZR",
    [UI_BIND_PLUS] = "Plus",
    [UI_BIND_MINUS] = "Minus",
    [UI_BIND_LEFT] = "Steuerkreuz links",
    [UI_BIND_UP] = "Steuerkreuz oben",
    [UI_BIND_RIGHT] = "Steuerkreuz rechts",
    [UI_BIND_DOWN] = "Steuerkreuz unten",
    [UI_BIND_LSTICK_LEFT] = "Nach links",
    [UI_BIND_LSTICK_UP] = "Vorwärts",
    [UI_BIND_LSTICK_RIGHT] = "Nach rechts",
    [UI_BIND_LSTICK_DOWN] = "Rückwärts",
    [UI_BIND_RSTICK_LEFT] = "Kamera links",
    [UI_BIND_RSTICK_UP] = "Kamera hoch",
    [UI_BIND_RSTICK_RIGHT] = "Kamera rechts",
    [UI_BIND_RSTICK_DOWN] = "Kamera runter",
    [UI_BIND_DESC_A] = "Interagieren, aufheben, sprechen.",
    [UI_BIND_DESC_B] = "Rennen, abbrechen, zurück.",
    [UI_BIND_DESC_X] = "Springen.",
    [UI_BIND_DESC_Y] = "Angreifen.",
    [UI_BIND_DESC_LCLICK] = "Schleichen.",
    [UI_BIND_DESC_RCLICK] = "Ego-Ansicht.",
    [UI_BIND_DESC_L] = "Fähigkeiten.",
    [UI_BIND_DESC_R] = "Waffe werfen.",
    [UI_BIND_DESC_ZL] = "Schild, Anvisieren.",
    [UI_BIND_DESC_ZR] = "Bogen.",
    [UI_BIND_DESC_PLUS] = "Menü, Inventar.",
    [UI_BIND_DESC_MINUS] = "Karte.",
    [UI_BIND_DESC_DPAD] = "Schnellauswahl.",
    [UI_BIND_DESC_MOVE] = "Bewegt Link wie der linke Stick. Der linke Stick des Controllers funktioniert immer auch.",
    [UI_BIND_DESC_CAMERA] = "Dreht die Kamera wie der rechte Stick. Maus und rechter Stick des Controllers "
                            "funktionieren immer auch.",
    [UI_BIND_SHAKE] = "Kamerastick schütteln",
    [UI_BIND_DESC_SHAKE] = "Bewegt den rechten Stick für das Spiel hin und her: löst ein verbundenes Teil von dem, "
                           "was die Ultrahand hält. Gedrückt halten schüttelt weiter.",
    [UI_KEY_SPACE] = "Leertaste",
    [UI_KEY_TAB] = "Tab",
    [UI_KEY_LSHIFT] = "Umschalt links",
    [UI_KEY_LCTRL] = "Strg links",
    [UI_KEY_UP] = "Pfeil hoch",
    [UI_KEY_DOWN] = "Pfeil runter",
    [UI_KEY_LEFT] = "Pfeil links",
    [UI_KEY_RIGHT] = "Pfeil rechts",
    [UI_KEY_MOUSE_L] = "Linke Maustaste",
    [UI_KEY_MOUSE_R] = "Rechte Maustaste",
    [UI_KEY_MOUSE_M] = "Mittlere Maustaste",
    [UI_KEY_MOUSE_X1] = "Maustaste 4",
    [UI_KEY_MOUSE_X2] = "Maustaste 5",
    [UI_KEY_ENTER] = "Enter",
    [UI_KEY_BACKSPACE] = "Rücktaste",
    [UI_KEY_RSHIFT] = "Umschalt rechts",
    [UI_KEY_RCTRL] = "Strg rechts",
    [UI_KEY_LALT] = "Alt",
    [UI_KEY_CAPSLOCK] = "Feststell",
    [UI_KEY_INSERT] = "Einfg",
    [UI_KEY_DELETE] = "Entf",
    [UI_KEY_HOME] = "Pos1",
    [UI_KEY_END] = "Ende",
    [UI_KEY_PAGEUP] = "Bild hoch",
    [UI_KEY_PAGEDOWN] = "Bild runter",
    [UI_KEY_NUM_FMT] = "Num %s",
    [UI_PAD_SOUTH] = "Untere Taste",
    [UI_PAD_EAST] = "Rechte Taste",
    [UI_PAD_WEST] = "Linke Taste",
    [UI_PAD_NORTH] = "Obere Taste",
    [UI_PAD_LSTICK] = "Linker Stick",
    [UI_PAD_RSTICK] = "Rechter Stick",
    [UI_PAD_L] = "Linke Schultertaste",
    [UI_PAD_R] = "Rechte Schultertaste",
    [UI_PAD_START] = "Start",
    [UI_PAD_BACK] = "Zurück",
    [UI_PAD_DPAD_LEFT] = "Steuerkreuz links",
    [UI_PAD_DPAD_UP] = "Steuerkreuz oben",
    [UI_PAD_DPAD_RIGHT] = "Steuerkreuz rechts",
    [UI_PAD_DPAD_DOWN] = "Steuerkreuz unten",
    [UI_PAD_ZL] = "Linker Trigger",
    [UI_PAD_ZR] = "Rechter Trigger",
    [UI_PAD_LPADDLE1] = "Linke Rücktaste",
    [UI_PAD_RPADDLE1] = "Rechte Rücktaste",
    [UI_PAD_LPADDLE2] = "Linke Rücktaste 2",
    [UI_PAD_RPADDLE2] = "Rechte Rücktaste 2",
    [UI_PAD_TOUCHPAD] = "Touchpad-Klick",
    [UI_PAD_CROSS] = "Kreuz",
    [UI_PAD_CIRCLE] = "Kreis",
    [UI_PAD_SQUARE] = "Quadrat",
    [UI_PAD_TRIANGLE] = "Dreieck",
    [UI_MENU_AMIIBO_DIR] = "Amiibo-Ordner",
    [UI_MENU_AMIIBO_OPEN] = "Öffnen",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "Standardordner",
    [UI_MENU_AMIIBO_REFRESH] = "Aktualisieren",
    [UI_MENU_AMIIBO_PICK] = "Amiibo-Dumps (.bin- oder .nfc-Dateien mit 540 Byte):",
    [UI_MENU_AMIIBO_NO_DIR] = "Der Ordner %s kann nicht geöffnet werden",
    [UI_MENU_AMIIBO_READER] = "NFC-Leser",
    [UI_MENU_AMIIBO_STATE] = "Leser:",
    [UI_MENU_AMIIBO_STATE_IDLE] = "bereit",
    [UI_MENU_AMIIBO_STATE_SEARCHING] = "das Spiel wartet auf ein Amiibo",
    [UI_MENU_AMIIBO_STATE_FOUND] = "Amiibo auf dem Leser",
    [UI_MENU_AMIIBO_STATE_MOUNTED] = "das Spiel liest das Amiibo",
    [UI_MENU_AMIIBO_STATE_OFF] = "aus",
    [UI_MENU_AMIIBO_FILE] = "Gewählte Datei:",
    [UI_MENU_AMIIBO_NO_FILE] = "(keine Datei gewählt)",
    [UI_MENU_AMIIBO_TAP] = "Amiibo auflegen",
    [UI_MENU_AMIIBO_REMOVE] = "Amiibo abnehmen",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "Amiibo nach dem Auflegen von selbst abnehmen",
    [UI_MENU_AMIIBO_HOLD] = "Zeit auf dem Leser",
    [UI_MENU_AMIIBO_SECONDS] = "%.1f s",
    [UI_MENU_AMIIBO_TAG] = "Amiibo auf dem Leser:",
    [UI_MENU_AMIIBO_NO_TAG] = "Kein Amiibo auf dem Leser.",
    [UI_MENU_AMIIBO_TAG_FILE] = "Datei: %s",
    [UI_MENU_AMIIBO_TAG_ID] = "Figur %s, Serie %02x, Nummer %04x, Typ %02x",
    [UI_MENU_AMIIBO_TAG_UID] = "UID: %s",
    [UI_MENU_AMIIBO_READY] = "Bereit",
    [UI_MENU_AMIIBO_TAPPED] = "Aufgelegt: %s",
    [UI_MENU_AMIIBO_LOAD_FAILED] = "Kein Amiibo-Dump (540-Byte-NTAG215): %s",
    [UI_MENU_AMIIBO_REMOVED] = "Amiibo abgenommen",
    [UI_MENU_AMIIBO_STATUS] = "Status: %s",
    [UI_MENU_MENU_LANG] = "Menüsprache",
    [UI_MENU_MENU_LANG_DESC] = "Sprache dieses Menüs und der Leistungsanzeige.",
    [UI_MENU_MENU_LANG_AUTO] = "Automatisch (aus Windows)",
    [UI_MENU_DISCORD_RPC] = "Discord Rich Presence",
    [UI_MENU_DISCORD_RPC_DESC] = "Zeigt \"Spielt TotK Recomp\" mit der Spielzeit in deinem Discord-Status, solange "
                                 "das Spiel läuft. discord_app_id und discord_image in totk.ini wählen die Anwendung.",
    [UI_MENU_ADV_WARNING] = "Diese Einstellungen dienen Tests und der Fehlersuche. Ändere sie nur, wenn du weißt, "
                            "warum.",
    [UI_MENU_GROUP_SCHED] = "Zeitplanung",
    [UI_MENU_GROUP_SHADERS] = "Shader und Pipelines",
    [UI_MENU_GROUP_TEXMEM] = "Texturen und Speicher",
    [UI_MENU_GROUP_FIXES] = "Render-Korrekturen",
    [UI_MENU_GROUP_VULKAN] = "Vulkan",
    [UI_MENU_GROUP_DIAG] = "Diagnose",
    [UI_MENU_ASYNC_PREEMPT] = "Unterbrechung auf virtuellen Kernen",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "Erlaubt einem Spiel-Thread, einen anderen auf demselben virtuellen Kern zu "
                                   "unterbrechen.",
    [UI_MENU_PREEMPT_OFF] = "Aus",
    [UI_MENU_PREEMPT_ON] = "Ein",
    [UI_MENU_PREEMPT_INHERIT] = "Nur Vererbung",
    [UI_MENU_REPLAY_ASYNC] = "GPU-Bildvorbereitung im eigenen Thread",
    [UI_MENU_REPLAY_ASYNC_DESC] = "Bereitet Bilder für die GPU in einem eigenen Thread statt im Spiel-Thread vor. "
                                  "Fügt bis zu ein Bild Latenz hinzu.",
    [UI_MENU_REPLAY_WORKERS] = "GPU-Vorbereitungsthreads",
    [UI_MENU_REPLAY_WORKERS_DESC] = "Bereitet große Bilder auf mehreren Threads gleichzeitig für die GPU vor. Hilft, "
                                    "wenn die CPU die Bildrate begrenzt; braucht 8 oder mehr logische Prozessoren. "
                                    "Aus bereitet auf einem Thread vor.",
    [UI_MENU_REPLAY_WORKERS_NOW] = "jetzt: %s",
    [UI_MENU_TIMER_1MS] = "Windows-Timer mit 1 ms",
    [UI_MENU_TIMER_1MS_DESC] = "Erhöht die Auflösung des Systemtimers auf 1 ms für gleichmäßigere Bildzeiten.",
    [UI_MENU_SYNC_AHEAD] = "Ein Bild vor der GPU",
    [UI_MENU_SYNC_AHEAD_DESC] = "Das Spiel baut das nächste Bild, während die Grafikkarte noch das vorige zeichnet. Mehr Bilder pro Sekunde, ein Bild mehr Eingabeverzögerung.",
    [UI_MENU_HOST_CHECKPOINT] = "Schnelle Kernübergabe",
    [UI_MENU_HOST_CHECKPOINT_DESC] = "Ein Spiel-Thread gibt seinen virtuellen Kern an einen wichtigeren ab, sobald er den Code des Ports verlässt, nicht erst beim nächsten Systemaufruf.",
    [UI_MENU_PIPE_WAIT] = "Umgang mit wartenden Draws",
    [UI_MENU_PIPE_WAIT_DESC] = "Standard behält die aktuelle Zeitplanung mit einem reservierten Worker für wartende "
                               "Draws; Alt (Draw) verwendet die vorherige Zeitplanung.",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "Standard",
    [UI_MENU_PIPE_WAIT_DRAW] = "Alt (Draw)",
    [UI_MENU_PIPE_SKIP] = "Übersprungene Draws beim Pipeline-Bau",
    [UI_MENU_PIPE_SKIP_DESC] = "Welche Draws verworfen werden, statt auf ihre Pipeline zu warten.",
    [UI_MENU_PIPE_SKIP_STRICT] = "Streng",
    [UI_MENU_PIPE_SKIP_ANY] = "Alle",
    [UI_MENU_PIPE_SKIP_FULLSCREEN] = "Vollbild-Durchgänge",
    [UI_MENU_LIB_PREBUILD] = "Vorbau der Stufenbibliotheken",
    [UI_MENU_LIB_PREBUILD_DESC] = "Baut Shader-Stufenbibliotheken vorab im Hintergrund.",
    [UI_MENU_LIB_PREBUILD_THREADS] = "Threads für den Vorbau der Stufenbibliotheken",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "Threads für den Vorbau; Automatisch wählt eine Zahl passend zur CPU.",
    [UI_MENU_PIPE_PREBUILD] = "Pipeline-Vorbau",
    [UI_MENU_PIPE_PREBUILD_DESC] = "Baut bekannte Pipelines vorab.",
    [UI_MENU_PREWARM_XLATE] = "Übersetzer vorwärmen",
    [UI_MENU_PREWARM_XLATE_DESC] = "Threads, die bekannte Shader beim Start übersetzen; Automatisch nimmt die Hälfte "
                                   "der CPU-Threads, höchstens 16.",
    [UI_MENU_GPL] = "Grafik-Pipeline-Bibliothek",
    [UI_MENU_GPL_DESC] = "Verwendet die Vulkan-Erweiterung für Pipeline-Bibliotheken; ausschalten, wenn dein Treiber "
                         "damit Probleme macht.",
    [UI_MENU_ASTC] = "ASTC-Texturdekodierung",
    [UI_MENU_ASTC_DESC] = "Wo komprimierte ASTC-Texturen dekodiert werden.",
    [UI_MENU_ASTC_AUTO] = "Automatisch",
    [UI_MENU_ASTC_GPU] = "GPU-Shader",
    [UI_MENU_ASTC_NATIVE] = "Nativ",
    [UI_MENU_ASTC_CPU] = "CPU",
    [UI_MENU_MEMORY] = "GPU-Speichermodus",
    [UI_MENU_MEMORY_DESC] = "Wie der Spielspeicher mit der GPU geteilt wird.",
    [UI_MENU_MEMORY_AUTO] = "Automatisch",
    [UI_MENU_MEMORY_IMPORT] = "Import",
    [UI_MENU_MEMORY_MIRROR] = "Spiegel",
    [UI_MENU_MEMORY_VERIFY] = "Prüfen",
    [UI_MENU_TEX_ALIAS] = "Textur-Aliasing",
    [UI_MENU_TEX_ALIAS_DESC] = "Eine neue Textur im Speicher einer lebenden Textur beginnt mit deren Inhalt statt "
                               "leer.",
    [UI_MENU_TEX_VRAM_PRESSURE] = "Texturspeicher-Grenze",
    [UI_MENU_TEX_VRAM_PRESSURE_DESC] = "Prozent des Videospeichers, ab dem vom Spiel freigegebene Texturen verworfen "
                                       "werden (sie kommen bei Bedarf aus dem Speicher zurück). 0 behält sie.",
    [UI_MENU_ASTC_BC] = "ASTC zu BC3 komprimieren",
    [UI_MENU_ASTC_BC_DESC] = "Mit dem GPU-Dekoder: kodiert dekodierte ASTC-Texturen zu BC3 um, ein Viertel des "
                             "Videospeichers. Für GPUs mit 4 GB oder weniger; Texturen wirken etwas klotziger.",
    [UI_MENU_LOD_GUARD] = "LOD-Schutz",
    [UI_MENU_LOD_GUARD_DESC] = "Verhindert, dass Objekte für ein Bild an der falschen Stelle aufblitzen oder die "
                               "Ferne flach wirkt, indem gewartet wird, bis das Spiel die Detailstufen-Daten fertig "
                               "geschrieben hat, bevor die GPU sie liest.",
    [UI_MENU_FIX_C0] = "Konstantpuffer-Korrektur",
    [UI_MENU_FIX_C0_DESC] = "Füllt die vom Treiber reservierte Konstantbank c0 (Speichergrößen, Dispatch-Größe, "
                            "Punktgröße, Basisinstanz), statt Nullen zu lesen.",
    [UI_MENU_FIX_ZERO] = "Neuen GPU-Speicher nullen",
    [UI_MENU_FIX_ZERO_DESC] = "Neue GPU-Speicherpools beginnen mit Nullen gefüllt, wie Switch-Speicher.",
    [UI_MENU_DLAA_EFFECTS_AFTER] = "Partikel nach DLAA",
    [UI_MENU_DLAA_EFFECTS_AFTER_DESC] = "Mit DLAA werden Staub und andere Partikel über das bereits von DLSS "
                                        "geglättete Bild gezeichnet, so wie das Spiel sie ohne DLSS zeichnet: Kleine "
                                        "Partikel im Nebel (Schreine) verschwinden nicht mehr und ziehen keine Spuren "
                                        "mehr. Nebel und Wolken gehen weiterhin durch DLSS, sodass ferne Kanten vor "
                                        "dem Himmel sauber bleiben. Aus: DLSS verarbeitet auch die Partikel, wie "
                                        "bisher. Nur im DLAA-Modus; die hochskalierenden DLSS-Modi verarbeiten sie "
                                        "immer. Wirkt sofort.",
    [UI_MENU_DLSS_FIXED_EXPOSURE] = "Feste DLSS-Belichtung",
    [UI_MENU_DLSS_FIXED_EXPOSURE_DESC] = "DLSS nimmt die Helligkeit des Spielbilds, wie sie ist, statt die Belichtung "
                                         "aus jedem Bild zu schätzen: In dunklen Szenen (Untergrund) werden große "
                                         "neblige Flächen nicht mehr für ein Bild dunkler oder heller, während sich "
                                         "die Kamera bewegt. Aus: DLSS schätzt die Belichtung selbst, wie bisher. "
                                         "Wirkt sofort.",
    [UI_MENU_BIND_SKIP] = "Wiederholte Bindungen überspringen",
    [UI_MENU_BIND_SKIP_DESC] = "Optimierung: überspringt unveränderte Bindungen.",
    [UI_MENU_SHADER_MEMO] = "Shader-Merker",
    [UI_MENU_SHADER_MEMO_DESC] = "Optimierung: merkt sich Shader-Abfragen.",
    [UI_MENU_PIPE_MEMO] = "Pipeline-Merker",
    [UI_MENU_PIPE_MEMO_DESC] = "Optimierung: verwendet die Pipeline des vorigen Draws erneut, wenn sich sein Zustand "
                               "wiederholt.",
    [UI_MENU_TEX_MEMO] = "Texturbindungs-Merker",
    [UI_MENU_TEX_MEMO_DESC] = "Optimierung: merkt sich Texturdeskriptoren zwischen Draws.",
    [UI_MENU_DESC_TEMPLATE] = "Deskriptor-Vorlagen",
    [UI_MENU_DESC_TEMPLATE_DESC] = "Optimierung: aktualisiert Deskriptoren gebündelt.",
    [UI_MENU_PUSH_SKIP] = "Wiederholte Push-Konstanten überspringen",
    [UI_MENU_PUSH_SKIP_DESC] = "Optimierung: überspringt unveränderte Push-Konstanten.",
    [UI_MENU_SPLIT_DRAWS] = "Frühe GPU-Übergabe",
    [UI_MENU_SPLIT_DRAWS_DESC] = "Übergibt GPU-Arbeit zwischen Durchgängen alle N Draws in Teilen, damit die GPU "
                                 "früher beginnt; 0 schaltet es aus.",
    [UI_MENU_VK_VALIDATION] = "Validierungsschicht",
    [UI_MENU_VK_VALIDATION_DESC] = "Prüft jeden Vulkan-Aufruf und verlangsamt das Spiel stark.",
    [UI_MENU_VK_VALIDATION_MISSING] = "Die Validierungsschicht ist nicht installiert.",
    [UI_MENU_VK_DYN_VERTEX] = "Dynamischer Vertex-Zustand",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "Erzwingt dynamische Vertex-Eingabe an oder aus; der Standard hängt von der GPU ab.",
    [UI_MENU_VK_DYN_A2C] = "Dynamisches Alpha-to-Coverage",
    [UI_MENU_VK_DYN_A2C_DESC] = "Erzwingt dynamisches Alpha-to-Coverage an oder aus. Standard: aus beim AMD-Treiber "
                                "für RDNA 2 und älter, der Figuren damit schwarz zeichnet, sonst an.",
    [UI_MENU_VK_SUBGROUP_PIN] = "Subgroup-Größe festlegen",
    [UI_MENU_VK_SUBGROUP_PIN_DESC] = "Legt die Subgroup-Größe der Shader fest.",
    [UI_MENU_TRACE] = "Leistungsprotokoll",
    [UI_MENU_TRACE_DESC] = "Schreibt Bildzeiten in eine Datei.",
    [UI_MENU_FLIGHT] = "Flugschreiber",
    [UI_MENU_FLIGHT_DESC] = "Hält einen kurzen Verlauf und speichert ihn, wenn ein Bild hängt.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "Hänger-Schwelle (ms)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "Bildzeit, die als Hänger zählt.",
    [UI_MENU_PASS_CRUMBS] = "GPU-Durchgangsmarken",
    [UI_MENU_PASS_CRUMBS_DESC] = "Nach einem GPU-Absturz nennt das Protokoll den Render-Durchgang, der gerade lief. "
                                 "Automatisch: nur AMD. Ein: auch NVIDIA, mit geringen Leistungskosten.",
    [UI_MENU_INPUT_RECORD] = "Eingabeaufzeichnung",
    [UI_MENU_INPUT_RECORD_DESC] = "Schreibt deine Eingaben für Fehlerberichte in eine Datei.",
    [UI_MENU_OPEN_LOG] = "Protokollordner öffnen",
    [UI_MENU_OPEN_LOG_DESC] = "Zeigt den Ordner mit den Protokolldateien.",
    [UI_MENU_COPY_INFO] = "Systeminfo kopieren",
    [UI_MENU_COPY_INFO_DESC] = "Kopiert GPU, Treiber, CPU, RAM und geänderte Einstellungen in die Zwischenablage.",
    [UI_MENU_COPY_INFO_DONE] = "Kopiert",
    [UI_MENU_ACTION_FAILED] = "hat nicht funktioniert",
    [UI_MENU_RESET] = "Alle Einstellungen zurücksetzen",
    [UI_MENU_RESET_DESC] = "Gilt für alle Tabs.",
    [UI_MENU_RESET_CONFIRM] = "Alle Einstellungen auf Standard zurücksetzen?",
    [UI_MENU_RESET_YES] = "Zurücksetzen",
    [UI_MENU_RESET_NO] = "Abbrechen",
    [UI_MENU_OV_FPS] = "%.0f FPS",
    [UI_MENU_OV_FRAME_MS] = "%.1f ms",
    [UI_MENU_OV_GAME_PCT] = "Spiel %.0f%%",
    [UI_MENU_OV_MAX_MS] = "max. %.1f ms",
    [UI_MENU_OV_FPS_LABEL] = "FPS",
    [UI_MENU_OV_FRAME_LABEL] = "Bild",
    [UI_MENU_OV_FMT_FPS] = "%.0f",
    [UI_MENU_OV_LOW1] = "1%% Low %.0f",
    [UI_MENU_OV_LOW01] = "0,1%% Low %.0f",
    [UI_MENU_OV_GPU] = "GPU",
    [UI_MENU_OV_CPU] = "CPU",
    [UI_MENU_OV_RAM] = "RAM",
    [UI_MENU_OV_VRAM] = "VRAM",
    [UI_MENU_OV_TEMP] = "Temperatur",
    [UI_MENU_OV_POWER] = "Leistung",
    [UI_MENU_OV_CLOCK] = "Takt",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "GPU-Bildvorbereitung",
    [UI_MENU_OV_GPU_TIME] = "GPU-Last pro Bild",
    [UI_MENU_OV_PIPE_WAIT] = "Pipeline-Wartezeiten",
    [UI_MENU_OV_SHADER_WAIT] = "Shader-Wartezeiten",
    [UI_MENU_OV_TEX_UPLOADS] = "Textur-Uploads",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%",
    [UI_MENU_OV_FMT_TEMP] = "%.0f °C",
    [UI_MENU_OV_FMT_POWER] = "%.0f W",
    [UI_MENU_OV_FMT_MHZ] = "%.0f MHz",
    [UI_MENU_OV_FMT_GB] = "%.1f GB",
    [UI_MENU_OV_FMT_GB_OF] = "/ %.1f GB",
    [UI_MENU_OV_GAME_GB] = "Spiel %.1f GB",
    [UI_MENU_OV_FMT_MS] = "%.2f ms",
    [UI_MENU_OV_PER_FRAME] = "%.1f pro Bild",
    [UI_MENU_OV_WINDOW] = "Fenster",
    [UI_MENU_OV_OUTPUT] = "Spielbild",
    [UI_MENU_OV_SCENE] = "3D-Szene",
    [UI_MENU_OV_UPSCALER] = "3D-Upscaler",
    [UI_MENU_OV_SCALING] = "Skalierung",
    [UI_MENU_OV_LIMIT] = "Bildgrenze",
    [UI_MENU_OV_PRESENT] = "Bildausgabe",
    [UI_MENU_OV_SHOWN] = "Gezeigt",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u",
    [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, Schärfe %d%%",
    [UI_MENU_OV_SCALE_NONE] = "keine (1:1)",
    [UI_MENU_OV_SCALE_BILINEAR] = "bilinear",
    [UI_MENU_OV_SCALE_DOWN] = "bilinear (FSR vergrößert nur)",
    [UI_MENU_TAB_CHEATS] = "Cheats",
    [UI_MENU_CHEATS_NONE] = "Keine Cheats für diese Spielversion. Die Cheats eines Mods sind eine "
                            "Atmosphère-Cheatdatei in seinem cheats-Ordner, benannt nach dem Build des Spiels: "
                            "mods\\<mod>\\cheats\\%s. Starte das Spiel neu, um sie zu lesen. Für Cheats, die den "
                            "Spielcode ändern, baut sich das Spiel beim Start neu (einige Minuten).",
    [UI_MENU_CHEATS_MOD] = "%s: %d Cheats",
    [UI_MENU_CHEAT_CODE_DESC] = "Ändert den Spielcode. Schaltet sofort.",
    [UI_MENU_CHEAT_KEY_DESC] = "Ändert den Spielcode, solange du %s hältst. Schaltet sofort.",
    [UI_MENU_CHEAT_DATA_DESC] = "Schreibt jedes Bild in den Spielspeicher, solange er an ist.",
    [UI_MENU_CHEAT_MASTER] = "immer an",
    [UI_MENU_CHEAT_MASTER_DESC] = "Der Master-Code der Datei: er ist immer an.",
    [UI_MENU_CHEAT_RESTORE] = "nicht nötig",
    [UI_MENU_CHEAT_RESTORE_DESC] = "Er schreibt nur den Originalcode des Spiels zurück. Das Ausschalten eines Cheats "
                                   "tut das bereits.",
    [UI_MENU_CHEAT_REBUILD] = "nach dem Neubau",
    [UI_MENU_CHEAT_REBUILD_DESC] = "Dieser Cheat ändert den Spielcode und ist neu oder geändert, seit das Spiel "
                                   "gebaut wurde. Das Spiel baut ihn beim nächsten Start ein (einige Minuten), oder "
                                   "verwende Spiel jetzt neu bauen.",
    [UI_MENU_CHEAT_UNSUPPORTED] = "nicht unterstützt",
    [UI_MENU_CHEAT_UNSUPPORTED_DESC] = "Dieser Cheat kann mit dieser Spielversion nicht verwendet werden:",
    [UI_MENU_CHEATS_REBUILD] = "Spiel jetzt neu bauen",
    [UI_MENU_CHEATS_REBUILD_DESC] = "Einige Cheats ändern den Spielcode und sind nicht eingebaut: Sie sind neu oder "
                                    "geändert, oder ihr letzter Neubau ist fehlgeschlagen oder wurde abgebrochen. "
                                    "Schließt das Spiel; das Setup baut sie ein (einige Minuten) und startet das "
                                    "Spiel erneut. Fortschritt seit dem letzten Speichern geht verloren.",
    [UI_MENU_MODS_EXEFS_UNSUPPORTED] = "Dieser Mod patcht den Spielcode (exefs) auf eine Weise, die der Port nicht "
                                       "anwenden kann, daher bleibt der ganze Mod aus. %s",
    [UI_MENU_MODS_EXEFS_OTHER] = "Seine Codepatches (exefs) sind für eine andere Spielversion gemacht.",
    [UI_MENU_MODS_EXEFS_ON] = "Aktive Codepatches: %d Dateien.",
    [UI_MENU_MODS_REPLACES_BUILTIN] = "Aktive Codepatches: %d Dateien. Sie ersetzen den eigenen Patch %s des Ports, "
                                      "der aus bleibt, solange dieser Mod an ist.",
    [UI_MENU_MODS_CODE_SKIPPED] = "Sein eigener Spielcode (%s) wird nicht geladen: Der Rest des Mods wird verwendet. "
                                  "Schalte den Mod aus, wenn sich das Spiel damit fehlerhaft verhält.",
    [UI_MENU_MODS_PLUGIN_ON] = "Sein Plugin läuft: %d Hooks in den Spielcode.",
    [UI_MENU_MODS_PLUGIN_REFUSED] = "Sein Plugin %s kann im Port nicht laufen: %s.",
    [UI_MENU_MODS_PLUGIN_STAYS_OFF] = "Der ganze Mod bleibt aus. Sein Schalter \"... ohne seinen Code verwenden\" "
                                      "darunter verwendet den Rest davon.",
    [UI_MENU_MODS_PLUGIN_OFF] = "Sein Plugin bleibt diesmal aus: Es startete anders als beim Bau (%s). Das Spiel baut "
                                "es beim nächsten Start neu.",
    [UI_MENU_MODS_WITHOUT_CODE] = "%s ohne seinen Code verwenden",
    [UI_MENU_MODS_WITHOUT_CODE_DESC] = "Der Mod darüber bringt Code mit, den der Port nicht ausführen kann: eigenen "
                                       "Spielcode (ein subsdk-Plugin oder ein ersetztes main) oder Codepatches "
                                       "(exefs), die der Port nicht anwenden kann. Aus: Der ganze Mod bleibt aus. "
                                       "Ein: Dieser Code wird ausgelassen und der Rest des Mods verwendet: seine "
                                       "RomFS-Dateien, Cheats und die funktionierenden Codepatches. Seine Dateien "
                                       "erwarten womöglich den fehlenden Code; schalte ihn wieder aus, wenn sich das "
                                       "Spiel fehlerhaft verhält. Wirkt beim nächsten Start.",
    [UI_MENU_MODS_WITHOUT_CODE_ALL] = "Für jeden Mod an: mods_without_code = 1 in totk.ini.",
    [UI_MENU_MODS_EXEFS_SKIPPED] = "%u seiner Codepatch-Dateien (exefs) können nicht angewendet werden und werden "
                                   "ausgelassen: Der Rest des Mods wird verwendet. %s",
    [UI_MENU_MODS_OVERRIDES_FILES] = "%u seiner Dateien ersetzen die von %s, das weiter unten in der Liste steht.",
    [UI_MENU_MODS_DATA_OVERRIDDEN] = "%u Byte seiner Spieldatenänderungen kommen aus %s, das weiter oben in der Liste "
                                     "steht.",
    [UI_MENU_MODS_OVERRIDES_DATA] = "%u Byte seiner Spieldatenänderungen ersetzen die von %s, das weiter unten in der "
                                    "Liste steht.",
    [UI_MENU_MODS_CODE_OVERRIDDEN] = "%u seiner Codepatch-Stellen werden von %s überschrieben, das weiter oben in der "
                                     "Liste steht.",
    [UI_MENU_MODS_OVERRIDES_CODE] = "Er überschreibt %u Codepatch-Stellen von %s, das weiter unten in der Liste steht.",
};

// French
static const char *const text_fr[UI_TEXT_COUNT] = {
    SWKBD("Saisie de texte", "Entrez un nom", "Cheval", "OK", "Annuler", "Manette : A = OK, B = Annuler"),

    [UI_MENU_TITLE] = "Paramètres",
    [UI_MENU_TAB_SCREEN] = "Écran",
    [UI_MENU_TAB_GAME] = "Jeu",
    [UI_MENU_TAB_MODS] = "Mods",
    [UI_MENU_TAB_SOUND] = "Son",
    [UI_MENU_TAB_CONTROLS] = "Commandes",
    [UI_MENU_TAB_AMIIBO] = "Amiibo",
    [UI_MENU_TAB_MENU] = "Menu",
    [UI_MENU_TAB_ADVANCED] = "Avancé",
    [UI_MENU_CONTINUE] = "Continuer",
    [UI_MENU_QUIT] = "Quitter le jeu",
    [UI_MENU_ON] = "Activé",
    [UI_MENU_OFF] = "Désactivé",
    [UI_MENU_AUTO] = "Automatique",
    [UI_MENU_DEFAULT_MARK] = "par défaut",
    [UI_MENU_AFTER_RESTART] = "après redémarrage",
    [UI_MENU_SET_BY_ENV] = "fixé par une variable d'environnement",
    [UI_MENU_SAVE_FAILED] = "impossible d'enregistrer les paramètres",
    [UI_MENU_VERSION] = "TotK Recomp %s, jeu %s",
    [UI_MENU_FULLSCREEN] = "Plein écran",
    [UI_MENU_FULLSCREEN_DESC] = "Le jeu remplit l'écran ; F11 bascule.",
    [UI_MENU_WINDOW_SIZE] = "Taille de la fenêtre",
    [UI_MENU_WINDOW_SIZE_DESC] = "Taille de la fenêtre du jeu hors plein écran.",
    [UI_MENU_WINDOW_SIZE_CUSTOM] = "Personnalisée",
    [UI_MENU_PRESENT] = "Présentation",
    [UI_MENU_PRESENT_DESC] = "Comment les images terminées arrivent à l'écran.",
    [UI_MENU_PRESENT_MAILBOX] = "Mailbox",
    [UI_MENU_PRESENT_VSYNC] = "VSync",
    [UI_MENU_PRESENT_IMMEDIATE] = "Immédiat",
    [UI_MENU_PRESENT_IN_USE] = "utilisé : %s",
    [UI_MENU_GPU] = "Carte graphique",
    [UI_MENU_GPU_DESC] = "Le périphérique Vulkan qui dessine le jeu.",
    [UI_MENU_GPU_AUTO] = "Automatique",
    [UI_MENU_PERF_OVERLAY] = "Affichage des performances",
    [UI_MENU_PERF_OVERLAY_DESC] = "Images par seconde en haut à droite. Complet ajoute les temps d'image avec un "
                                  "graphique, la charge du CPU, du GPU et de la mémoire, les températures et les "
                                  "attentes du thread de rendu. F10 fait défiler les modes.",
    [UI_MENU_PERF_OFF] = "Désactivé",
    [UI_MENU_PERF_FPS] = "IPS",
    [UI_MENU_PERF_FULL] = "Complet",
    [UI_PERF_LINE_FPS] = "%.0f IPS",
    [UI_PERF_LINE_FPS_GEN] = "%.0f IPS dessinées, %.0f affichées",
    [UI_MENU_GRAPHICS] = "Image",
    [UI_MENU_OUTPUT_RES] = "Résolution du jeu",
    [UI_MENU_OUTPUT_RES_DESC] = "La taille dans laquelle le jeu dessine son image et son interface. Au-delà de 1920 x "
                                "1080, le jeu demande plus de mémoire vidéo et de temps GPU. Un format plus large "
                                "(21:9) élargit le champ de la caméra et place le HUD dans les coins de l'écran ; un "
                                "format plus haut (16:10, Steam Deck) le place sur les bords haut et bas.",
    [UI_MENU_HUD_WIDTH] = "Largeur du HUD",
    [UI_MENU_HUD_WIDTH_DESC] = "Sur un écran plus large que 16:9, l'écart entre les côtés gauche et droit du HUD. "
                               "Plein écran les place aux bords de l'écran ; 21:9 ou 16:9 garde le HUD au milieu de "
                               "l'écran, dans une zone de cette forme. Une disposition manquante est créée en "
                               "arrière-plan et utilisée après un redémarrage.",
    [UI_MENU_HUD_WIDTH_FULL] = "Plein écran",
    [UI_MENU_RENDER_SCALE] = "Échelle du rendu 3D",
    [UI_MENU_RENDER_SCALE_DESC] = "Taille de la scène 3D par rapport à la résolution du jeu ; le FSR du jeu la met à "
                                  "cette taille. Au-delà de 100 %, les bords crénelés sont lissés "
                                  "(suréchantillonnage) au prix d'un coût GPU élevé.",
    [UI_MENU_RENDER_SCALE_GAME] = "Jeu (83 %)",
    [UI_MENU_DYNAMIC_RES] = "Résolution dynamique",
    [UI_MENU_DYNAMIC_RES_DESC] = "Permet au jeu de réduire la scène 3D à 90 ou 80 %. Il en juge d'après une "
                                 "estimation faite pour la Switch qui ignore votre GPU, si bien que les scènes "
                                 "étendues baissent même sur un PC rapide. Désactivé garde l'échelle du rendu 3D.",
    [UI_MENU_UPSCALER] = "Mise à l'échelle vers la fenêtre",
    [UI_MENU_UPSCALER_DESC] = "Comment l'image est agrandie quand la fenêtre est plus grande que la résolution du "
                              "jeu. FSR garde les bords et les détails plus nets que le bilinéaire.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1",
    [UI_MENU_UPSCALER_BILINEAR] = "Bilinéaire",
    [UI_MENU_ASPECT] = "Forme de l'image",
    [UI_MENU_ASPECT_DESC] = "Quand la forme de la fenêtre diffère de celle de la résolution du jeu (une image 16:9 "
                            "sur un écran 21:9) : garder la forme de l'image avec des bandes noires, ou l'étirer sur "
                            "la fenêtre.",
    [UI_MENU_ASPECT_FIT] = "Bandes noires",
    [UI_MENU_ASPECT_STRETCH] = "Étirer",
    [UI_MENU_MOVIE_ASPECT] = "Forme des vidéos",
    [UI_MENU_MOVIE_ASPECT_DESC] = "Quand la résolution du jeu n'est pas en 16:9 : garder la forme propre des vidéos "
                                  "des cinématiques avec des bandes noires, ou les étirer sur toute l'image. "
                                  "S'applique à partir de la vidéo suivante.",
    [UI_MENU_MOVIE_ASPECT_KEEP] = "Bandes noires",
    [UI_MENU_MOVIE_ASPECT_STRETCH] = "Étirer",
    [UI_MENU_FOV] = "Champ de vision",
    [UI_MENU_FOV_DESC] = "La part du monde que montre la caméra, en pourcentage de celle du jeu. Version 1.4.3 du jeu "
                         "uniquement.",
    [UI_MENU_VERT_PLUS] = "Garder la largeur 16:9",
    [UI_MENU_VERT_PLUS_DESC] = "Sur les écrans plus étroits que 16:9 (Steam Deck, 16:10, 4:3), la caméra montre la même largeur du monde qu'en 16:9, et davantage au-dessus et en dessous. Sans effet en 16:9 ou plus large.",
    [UI_MENU_SHADOW_RES] = "Taille de la carte d'ombres",
    [UI_MENU_SHADOW_RES_DESC] = "Taille de la carte d'ombres du soleil. La taille du jeu change avec la vue, jusqu'à "
                                "environ 400 près du sol, si bien que les ombres des feuilles paraissent pixelisées "
                                "et sautent ; une taille fixe les garde stables et plus nettes. Au-delà de 1024, les "
                                "ombres coûtent plus de temps GPU et de mémoire vidéo.",
    [UI_MENU_SHADOW_RES_GAME] = "Jeu (variable)",
    [UI_MENU_ANISO] = "Filtrage des textures",
    [UI_MENU_ANISO_DESC] = "Filtrage anisotrope des textures : le sol, les routes et les murs vus sous un angle "
                           "rasant restent nets au loin au lieu de devenir flous. Jeu : le niveau que demande le jeu, "
                           "comme sur la Switch.",
    [UI_MENU_ANISO_GAME] = "Jeu",
    [UI_MENU_LOD_STEADY] = "Détails stables sous charge",
    [UI_MENU_LOD_STEADY_DESC] = "Ignore le mécanisme du jeu qui réduit les détails des modèles et des textures quand "
                                "la fréquence d'images baisse, comme le fait le mod d'émulateur Disable LOD Quality "
                                "Reduction. Versions 1.1.0, 1.2.1 et 1.4.3 du jeu. Laissez-le désactivé sur les PC peu "
                                "puissants : cette baisse y maintient la fréquence d'images.",
    [UI_MENU_SHARPNESS] = "Netteté FSR",
    [UI_MENU_SHARPNESS_DESC] = "Accentuation après la mise à l'échelle FSR ; 0 la désactive.",
    [UI_MENU_COLOR] = "Couleur",
    [UI_MENU_COLOR_PRESET] = "Profil de couleur",
    [UI_MENU_COLOR_PRESET_DESC] = "Règle les quatre valeurs ci-dessous. Vif : plus de contraste et de saturation, "
                                  "ombres un peu éclaircies (pour les écrans OLED). Clarté : un peu plus de "
                                  "contraste, de saturation et de luminosité. Modifier une valeur passe le profil en "
                                  "Personnalisé. Le HUD change avec l'image ; ce menu, non.",
    [UI_MENU_COLOR_DEFAULT] = "Celui du jeu",
    [UI_MENU_COLOR_VIVID] = "Vif (OLED)",
    [UI_MENU_COLOR_CLARITY] = "Clarté",
    [UI_MENU_COLOR_CUSTOM] = "Personnalisé",
    [UI_MENU_COLOR_BRIGHTNESS] = "Luminosité",
    [UI_MENU_COLOR_BRIGHTNESS_DESC] = "Ajoutée à chaque couleur ; 0 la laisse inchangée.",
    [UI_MENU_COLOR_CONTRAST] = "Contraste",
    [UI_MENU_COLOR_CONTRAST_DESC] = "Éloigne les couleurs du gris moyen (au-delà de 100 %) ou les en rapproche ; 100 "
                                    "% les laisse inchangées.",
    [UI_MENU_COLOR_SATURATION] = "Saturation",
    [UI_MENU_COLOR_SATURATION_DESC] = "Intensité des couleurs par rapport au gris ; 100 % la laisse inchangée.",
    [UI_MENU_COLOR_GAMMA] = "Gamma",
    [UI_MENU_COLOR_GAMMA_DESC] = "En dessous de 1, éclaircit les tons sombres et moyens, au-dessus de 1, les "
                                 "assombrit ; 1 les laisse inchangés.",
    [UI_MENU_HDR_GROUP] = "HDR",
    [UI_MENU_HDR] = "Sortie HDR",
    [UI_MENU_HDR_DESC] = "Hautes lumières éclatantes sur un écran HDR. Demande le HDR activé dans Windows (« Utiliser "
                         "le HDR ») ou un compositeur Wayland avec gestion des couleurs ; sinon l'image reste en SDR. "
                         "S'applique après un redémarrage. Le profil de couleur ci-dessus est pour le SDR et n'est "
                         "pas utilisé en HDR.",
    [UI_MENU_HDR_PEAK] = "Luminosité maximale",
    [UI_MENU_HDR_PEAK_DESC] = "Le pic de votre écran en nits ; les hautes lumières les plus vives s'arrêtent là.",
    [UI_MENU_HDR_GAME] = "Luminosité du jeu",
    [UI_MENU_HDR_GAME_DESC] = "Nits du blanc de l'image (blanc papier). 203 est la référence HDR ; augmentez-la dans "
                              "une pièce lumineuse.",
    [UI_MENU_HDR_UI] = "Luminosité du HUD et du menu",
    [UI_MENU_HDR_UI_DESC] = "Nits du blanc du HUD et de ce menu.",
    [UI_MENU_HDR_HUE] = "Teinte des hautes lumières",
    [UI_MENU_HDR_HUE_DESC] = "Les couleurs vives comme le feu et les étincelles tirent vers le blanc comme dans "
                             "l'image SDR du jeu, en accord avec la lumière qu'elles projettent ; 0 les garde "
                             "entièrement saturées.",
    [UI_MENU_SHADOW_SCALE] = "Résolution des ombres",
    [UI_MENU_SHADOW_SCALE_DESC] = "Dessine les cartes d'ombres du soleil ce nombre de fois plus grandes de chaque "
                                  "côté que les 960x960 du jeu : bords d'ombres et ombres des feuilles plus nets. "
                                  "Prend effet au prochain démarrage.",
    [UI_MENU_PCSS] = "Ombres douces",
    [UI_MENU_PCSS_DESC] = "Ombres du soleil nettes là où un objet touche le sol et plus douces à mesure que l'ombre "
                          "s'éloigne (PCSS). 0 garde le filtre du jeu. Prend effet au prochain démarrage.",
    [UI_MENU_CONTACT_SHADOWS] = "Ombres de contact",
    [UI_MENU_CONTACT_SHADOWS_DESC] = "Petites ombres du soleil que les cartes d'ombres manquent : aux pieds, sous les "
                                     "pierres, dans l'herbe et les plis. Calculées à l'écran après l'éclairage. Prend "
                                     "effet au prochain démarrage.",
    [UI_MENU_SHADOW_RANGE] = "Portée des cascades d'ombres",
    [UI_MENU_SHADOW_RANGE_DESC] = "Éloigne les paliers où les ombres deviennent plus douces (20, 40, 80 et 160 m dans "
                                  "le jeu). Étale la carte d'ombres sur plus de terrain : augmentez aussi la "
                                  "résolution des ombres. Prend effet au prochain démarrage.",
    [UI_MENU_GRASS_RANGE] = "Portée de l'herbe",
    [UI_MENU_GRASS_RANGE_DESC] = "Jusqu'où l'herbe pousse autour de la caméra (dans le jeu : 62,5 m). Plus haut garde "
                                 "l'herbe des prairies plus loin au lieu de la faire pousser juste devant vous ; plus "
                                 "bas en dessine moins. Prend effet au prochain démarrage.",
    [UI_MENU_DLSS_PRESET] = "Modèle DLSS",
    [UI_MENU_DLSS_PRESET_DESC] = "Le modèle DLSS utilisé. Automatique suit les conseils de NVIDIA pour chaque mode : "
                                 "K pour DLAA, Qualité et Équilibré, M pour Performance, L pour Ultra Performance. J "
                                 ": un peu moins de traînées derrière les personnages en mouvement, un peu plus de "
                                 "scintillement. K : le modèle le plus net dans tous les modes. S'applique "
                                 "immédiatement.",
    [UI_MENU_DLSS_PRESET_DEFAULT] = "Automatique",
    [UI_MENU_DLSS_PRESET_K] = "K (le plus net)",
    [UI_MENU_DLSS_PRESET_J] = "J (moins de traînées)",
    [UI_MENU_DLSS_CHARS] = "Traînées DLSS des personnages",
    [UI_MENU_DLSS_CHARS_DESC] = "Réduit les légères traînées derrière les personnages et les animaux en mouvement et "
                                "ce qu'ils tiennent : DLSS y reprend moins des images précédentes. Un niveau plus "
                                "fort retire davantage de traînées et laisse ces bords un peu moins lisses. "
                                "S'applique immédiatement.",
    [UI_MENU_DLSS_CHARS_OFF] = "Désactivé",
    [UI_MENU_DLSS_CHARS_LIGHT] = "Léger",
    [UI_MENU_DLSS_CHARS_MEDIUM] = "Moyen",
    [UI_MENU_DLSS_CHARS_FULL] = "Complet",
    [UI_MENU_DLSS] = "Mode de l'upscaler",
    [UI_MENU_DLSS_DESC] = "Natif lisse les bords de la scène 3D en pleine résolution. Qualité, Équilibré, Performance "
                          "et Ultra Performance la dessinent à 67, 58, 50 et 33 % avant de laisser l'upscaler (DLSS "
                          "ou FSR 3.1) l'agrandir, ce qui économise du temps GPU ; ils remplacent l'échelle du rendu "
                          "3D. S'applique immédiatement.",
    [UI_MENU_DLSS_DLAA] = "Natif",
    [UI_MENU_DLSS_QUALITY] = "Qualité",
    [UI_MENU_DLSS_BALANCED] = "Équilibré",
    [UI_MENU_DLSS_PERFORMANCE] = "Performance",
    [UI_MENU_DLSS_ULTRA] = "Ultra Performance",
    [UI_MENU_FXAA] = "Anticrénelage du jeu (FXAA)",
    [UI_MENU_FXAA_DESC] = "Le lissage des bords FXAA du jeu. Il floute un peu les détails fins ; avec DLAA, il n'est "
                          "pas nécessaire.",
    [UI_MENU_REFLEX] = "Faible latence",
    [UI_MENU_REFLEX_DESC] = "Réduit la latence du système. Activé utilise NVIDIA Reflex (VK_NV_low_latency2) ; Activé + Boost maintient aussi les fréquences du GPU NVIDIA élevées dans les scènes limitées par le CPU. Automatique utilise Reflex sur NVIDIA et AMD Anti-Lag 2 (VK_AMD_anti_lag) sur AMD. S'applique après un redémarrage.",
    [UI_MENU_REFLEX_AUTO] = "Automatique",
    [UI_MENU_REFLEX_OFF] = "Désactivé",
    [UI_MENU_REFLEX_ON] = "Activé",
    [UI_MENU_REFLEX_BOOST] = "Activé + Boost",
    [UI_MENU_SCENE_UPSCALER] = "Upscaler de la scène 3D",
    [UI_MENU_SCENE_UPSCALER_DESC] = "Un upscaler temporel pour la scène 3D : des bords plus lisses et moins de "
                                    "scintillement, et dans un mode en dessous de Natif, il dessine la scène plus "
                                    "petite puis l'agrandit, ce qui économise du temps GPU. NVIDIA DLSS demande un "
                                    "GPU NVIDIA RTX et " DLSS_FILE " à côté de " EXE_FILE " ; AMD FSR 3.1 fonctionne "
                                    "sur tout GPU. Désactivé garde la mise à l'échelle du jeu et son FXAA. Le HUD "
                                    "n'est pas touché ; la mise à l'échelle vers la fenêtre et la netteté ci-dessus "
                                    "adaptent toujours l'image du jeu à la fenêtre. S'applique après un redémarrage.",
    [UI_MENU_SCENE_UPSCALER_OFF] = "Désactivé",
    [UI_MENU_SCENE_UPSCALER_DLSS] = "NVIDIA DLSS",
    [UI_MENU_SCENE_UPSCALER_FSR3] = "AMD FSR 3.1",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_GPU] = "DLSS : demande un GPU NVIDIA RTX",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_DLL] = "DLSS : " DLSS_FILE " manquant",
    [UI_MENU_FSR_SHARPNESS] = "Netteté FSR",
    [UI_MENU_FSR_SHARPNESS_DESC] = "Netteté de la scène 3D après AMD FSR 3.1 ou FSR 4 ; 0 la désactive.",
    [UI_MENU_SCENE_UPSCALER_FSR4] = "AMD FSR 4",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_GPU] = "FSR 4 : non proposé pour ce GPU",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_DLL] = "FSR 4 : les bibliothèques FSR d'AMD sont absentes",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_OS] = "FSR 4 : Windows uniquement",
    [UI_MENU_FPS_LIMIT] = "Limite d'images par seconde",
    [UI_MENU_FPS_LIMIT_DESC] = "Le nombre maximal d'images par seconde que dessine le jeu. Au-delà de 60, c'est "
                               "expérimental et demande « Vitesse du jeu selon l'horloge ».",
    [UI_MENU_FPS_30] = "30 IPS",
    [UI_MENU_FPS_40] = "40 IPS",
    [UI_MENU_FPS_45] = "45 IPS",
    [UI_MENU_FPS_60] = "60 IPS",
    [UI_MENU_FPS_90] = "90 IPS",
    [UI_MENU_FPS_120] = "120 IPS",
    [UI_MENU_FPS_144] = "144 IPS",
    [UI_MENU_FPS_DISPLAY] = "Fréquence de l'écran",
    [UI_MENU_FPS_DISPLAY_HZ] = "Fréquence de l'écran (%.0f Hz)",
    [UI_MENU_FRAME_QUEUE] = "File d'images",
    [UI_MENU_FRAME_QUEUE_DESC] = "Le jeu commence une image pendant que la précédente attend son tour à l'écran : "
                                 "bien moins d'images perdues à la limite, jusqu'à une image de latence en plus. "
                                 "S'applique immédiatement.",
    [UI_MENU_FRAME_GEN] = "Génération d'images",
    [UI_MENU_FRAME_GEN_OFF] = "Désactivé",
    [UI_MENU_FRAME_GEN_FSR3] = "AMD FSR 3",
    [UI_MENU_FRAME_GEN_DESC] = "Une image générée entre chaque paire d'images que dessine le jeu : deux fois plus "
                               "d'images à l'écran, le jeu à la moitié de la limite d'images et les commandes environ "
                               "une demi-image plus tard. Pour les écrans de 120 Hz et plus. Pas avec DLSS ni HDR. "
                               "S'applique immédiatement.",
    [UI_MENU_REALTIME] = "Vitesse du jeu selon l'horloge",
    [UI_MENU_REALTIME_DESC] = "Fait avancer le jeu en temps réel, pour qu'une image lente ne ralentisse pas le jeu.",
    [UI_MENU_CLOTH_REALTIME] = "Tissus selon l'horloge",
    [UI_MENU_CLOTH_REALTIME_DESC] = "Fait aussi avancer les tissus et les cheveux en temps réel.",
    [UI_MENU_HIGHFPS_FIX] = "Corrections pour hautes fréquences",
    [UI_MENU_HIGHFPS_FIX_DESC] = "Corrige la physique qui se dérègle au-delà de 30 images par seconde, et les objets "
                                 "qui restent transparents ou invisibles au-delà de 60.",
    [UI_MENU_CLIMB_FIX] = "Correctif d'escalade",
    [UI_MENU_CLIMB_FIX_DESC] = "Garde l'escalade des parois et des échelles à pleine vitesse au-delà de 60 images par "
                               "seconde, pour que Link atteigne le sommet.",
    [UI_MENU_GAME_LANG] = "Langue du jeu",
    [UI_MENU_GAME_LANG_DESC] = "Langue des textes et des voix du jeu.",
    [UI_MENU_GAME_LANG_AUTO] = "Automatique",
    [UI_MENU_OPERATION_MODE] = "Mode de la console",
    [UI_MENU_OPERATION_MODE_DESC] = "Si le jeu se croit en mode TV ou en mode portable.",
    [UI_MENU_MODE_DOCKED] = "Mode TV",
    [UI_MENU_MODE_HANDHELD] = "Mode portable",
    [UI_MENU_ASSUME_FOCUS] = "Continuer en arrière-plan",
    [UI_MENU_ASSUME_FOCUS_DESC] = "Activé (par défaut), le jeu continue pendant que vous êtes dans une autre fenêtre, "
                                  "et le clavier reste muet jusqu'au retour de la fenêtre au premier plan. Désactivé, "
                                  "il se met en pause quand sa fenêtre perd le focus, comme sur la Switch quand on "
                                  "appuie sur Home ; un chargement interrompu ainsi peut se bloquer définitivement.",
    [UI_MENU_PLAY_TIME] = "Temps de jeu",
    [UI_MENU_PLAY_TIME_DESC] = "Le temps passé dans le jeu sur toutes les sessions, compté tant que la fenêtre du jeu "
                               "a le focus et que le jeu dessine des images. Conservé dans playtime.txt dans le "
                               "dossier du profil, à côté des sauvegardes.",
    [UI_MENU_PLAY_TIME_HM] = "%llu h %02u min",
    [UI_MENU_PLAY_TIME_SESSION] = "cette session : %s",
    [UI_MENU_MODS_BUILTIN] = "Intégrés au portage",
    [UI_MENU_MODS_INSTALLED] = "Mods installés",
    [UI_MENU_MODS_NONE] = "Aucun mod pour l'instant. Décompressez un mod dans le dossier mods, un dossier par mod "
                          "avec son dossier romfs à l'intérieur, puis redémarrez le jeu. TKMM peut aussi y exporter "
                          "ses mods fusionnés dans un dossier.",
    [UI_MENU_MODS_OPEN] = "Ouvrir le dossier des mods",
    [UI_MENU_MODS_OPEN_DESC] = "Affiche le dossier où le jeu charge les mods. Il est créé s'il manque.",
    [UI_MENU_MODS_ROW_DESC] = "Fichiers du jeu remplacés : %u. Nouveaux fichiers ajoutés : %u.",
    [UI_MENU_MODS_OVERRIDDEN] = "%u de ses fichiers viennent de %s, placé plus haut dans la liste.",
    [UI_MENU_MODS_MOVE_HINT] = "Le bouton du haut ou Ctrl+Haut le monte, le bouton de gauche ou Ctrl+Bas le descend. "
                               "Quand deux mods modifient le même fichier, celui du dessus l'emporte. Les changements "
                               "s'appliquent après un redémarrage.",
    [UI_MENU_MODS_EMPTY] = "Ce dossier ne contient aucun fichier romfs, exefs ou cheats.",
    [UI_MENU_MODS_SUBSDK] = "Ce mod apporte son propre code de jeu (un plugin subsdk ou un main remplacé). Le portage "
                            "ne peut pas charger de modules de code, donc tout le mod reste désactivé.",
    [UI_MENU_MODS_EXEFS_LATER] = "Ce mod modifie le code du jeu (exefs). Le jeu intègre les correctifs à son prochain "
                                 "démarrage (une reconstruction de quelques minutes) ; d'ici là, tout le mod reste "
                                 "désactivé.",
    [UI_MENU_MODS_CHEATS] = "Codes de triche : %d. Activez-les dans l'onglet Triches.",
    [UI_MENU_MODS_CHEATS_FILE] = "Les codes de triche de cette version du jeu sont lus au démarrage depuis cheats\\%s "
                                 "dans ce mod.",
    [UI_MENU_MODS_BAD_NAME] = "Le nom du dossier contient |, que la liste des mods utilise entre les noms. Renommez "
                              "le dossier.",
    [UI_MENU_MODS_OTHER_VERSION] = "%u de ses fichiers sont faits pour une autre version du jeu et sont ignorés. "
                                   "Cherchez une version de ce mod faite pour votre version du jeu.",
    [UI_MENU_MODS_UNUSED] = "non utilisé",
    [UI_MENU_MODS_FPS60] = "Correctif 60 IPS",
    [UI_MENU_MODS_FPS60_DESC] = "Le correctif du portage pour 60 images par seconde. Réglez-le avec la limite "
                                "d'images par seconde dans l'onglet Jeu.",
    [UI_MENU_MODS_RES] = "Correctif de résolution",
    [UI_MENU_MODS_RES_DESC] = "Le correctif du portage pour les résolutions au-delà de 1080p. Réglez-le avec la "
                              "résolution du jeu dans l'onglet Écran.",
    [UI_MENU_MODS_QUICKMENU] = "Correction du menu rapide",
    [UI_MENU_MODS_QUICKMENU_DESC] = "La correction du portage pour le fond noir des menus d'armes, de boucliers et de "
                                    "matériaux à 60 IPS.",
    [UI_MENU_VOLUME] = "Volume",
    [UI_MENU_VOLUME_DESC] = "Volume général du jeu.",
    [UI_MENU_VOLUME_MUSIC] = "Volume de la musique",
    [UI_MENU_VOLUME_MUSIC_DESC] = "Volume de la musique de fond.",
    [UI_MENU_VOLUME_SFX] = "Volume des effets",
    [UI_MENU_VOLUME_SFX_DESC] = "Volume des effets sonores.",
    [UI_MENU_VOLUME_VOICE] = "Volume des voix",
    [UI_MENU_VOLUME_VOICE_DESC] = "Volume des voix des personnages.",
    [UI_MENU_AUDIO_CHANNELS] = "Haut-parleurs",
    [UI_MENU_AUDIO_CHANNELS_DESC] = "Sortie stéréo ou surround 5.1. Automatique choisit 5.1 quand la configuration "
                                    "des haut-parleurs de Windows a six canaux ou plus. S'applique après un "
                                    "redémarrage.",
    [UI_MENU_CHANNELS_AUTO] = "Automatique",
    [UI_MENU_CHANNELS_STEREO] = "Stéréo",
    [UI_MENU_CHANNELS_51] = "Surround 5.1",
    [UI_MENU_AUDIO_DEVICE] = "Périphérique de sortie",
    [UI_MENU_AUDIO_DEVICE_DESC] = "Où le son est joué. Par défaut du système suit le périphérique par défaut de "
                                  "Windows. Un périphérique choisi qui est débranché cède la place à celui par défaut "
                                  "du système jusqu'à son retour. S'applique immédiatement.",
    [UI_MENU_AUDIO_DEVICE_DEFAULT] = "Par défaut du système",
    [UI_MENU_MOTION] = "Commandes par mouvement",
    [UI_MENU_MOTION_DESC] = "Visée avec le gyroscope de la manette.",
    [UI_MENU_GYRO_CALIBRATION] = "Calibrage du gyroscope",
    [UI_MENU_GYRO_CALIBRATION_DESC] = "Soustrait la dérive au repos de la manette, mesurée avec la ligne de calibrage "
                                      "ci-dessous. Les valeurs mesurées restent enregistrées quand ce réglage est "
                                      "désactivé.",
    [UI_MENU_GYRO_CALIBRATE] = "Calibrer le gyroscope",
    [UI_MENU_GYRO_CALIBRATE_DESC] = "Mesure la dérive au repos du gyroscope et la retire. Posez la manette ou "
                                    "tenez-la immobile pendant deux secondes.",
    [UI_MENU_GYRO_CALIBRATE_WAIT] = "Calibrage...",
    [UI_MENU_GYRO_CALIBRATED] = "%s, calibré",
    [UI_MENU_GYRO_CALIBRATE_FAILED] = "Échec du calibrage",
    [UI_MENU_GYRO_CALIBRATE_MOVED] = "La manette a bougé : tenez-la immobile et réessayez",
    [UI_MENU_GYRO_AUTO_BIAS] = "Apprentissage de la dérive au repos",
    [UI_MENU_GYRO_AUTO_BIAS_DESC] = "Apprend la dérive restante du gyroscope chaque fois que la manette repose "
                                    "immobile sur une table, et la soustrait. Pour une visée à l'arc qui glisse "
                                    "lentement même après le calibrage. Une manette tenue en main n'est pas prise en "
                                    "compte. Expérimental.",
    [UI_MENU_RUMBLE_STRENGTH] = "Intensité des vibrations",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "La force des vibrations de la manette.",
    [UI_MENU_RUMBLE_CURVE] = "Courbe des vibrations",
    [UI_MENU_RUMBLE_CURVE_DESC] = "Des valeurs plus basses renforcent les vibrations faibles.",
    [UI_MENU_RUMBLE_TRIGGERS] = "Vibration des gâchettes (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Fait aussi vibrer les gâchettes d'une manette Xbox.",
    [UI_MENU_PAD_LAYOUT] = "Disposition des boutons",
    [UI_MENU_PAD_LAYOUT_DESC] = "Quel bouton est A. Nintendo : celui de droite, comme sur une Switch. Xbox : celui du "
                                "bas, comme indiqué sur une manette Xbox (croix sur une manette PlayStation). "
                                "Automatique choisit Nintendo pour les manettes aux inscriptions Nintendo et Xbox "
                                "pour les autres. La manette affichée dans les indications du jeu se règle dans "
                                "Icônes des boutons.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Automatique",
    [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A à droite)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A en bas)",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "utilisé : %s",
    [UI_MENU_PROMPTS] = "Icônes des boutons",
    [UI_MENU_PROMPTS_DESC] = "La manette dont les indications du jeu montrent les boutons. Nintendo : celles du jeu. "
                             "Xbox : LB, RB, LT, RT, Menu, Affichage et une croix directionnelle, et A, B, X et Y "
                             "tels qu'inscrits sur le bouton qu'actionne la disposition ci-dessus (avec Nintendo (A à "
                             "droite), le bouton de droite porte l'inscription B, donc les indications pour A "
                             "montrent B). PlayStation : les symboles d'une manette PlayStation (○ × △ □, L1, R1, L2, "
                             "R2, Options et Share), sur le bouton qu'actionne la disposition ci-dessus. PC : les "
                             "touches et boutons de souris de la disposition du clavier par défaut (E, Maj, Espace, "
                             "clic gauche...) et la souris pour la caméra. Le jeu lit ses indications au démarrage, "
                             "elles changent donc après un redémarrage. Elles font partie des fichiers du jeu, donc "
                             "un bouton réattribué dans Commandes garde son ancienne indication à l'écran.",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo",
    [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation",
    [UI_MENU_PROMPTS_PC] = "PC",
    [UI_MENU_PROMPTS_MISSING] = "fichiers d'icônes manquants : relancez l'installation",
    [UI_MENU_PROMPTS_FAILED] = "fichiers d'icônes endommagés : relancez l'installation",
    [UI_MENU_PROMPTS_RESTART] = "les icônes changent après redémarrage",
    [UI_MENU_PROMPTS_OFF_SHAPE] = "désactivé si la résolution du jeu n'est pas en 16:9",
    [UI_MENU_OUTPUT_RES_NO_LAYOUT] = "aucune disposition pour cette forme : relancez l'installation",
    [UI_MENU_OUTPUT_RES_PREPARING] = "disposition en cours de création en arrière-plan ; utilisée au prochain "
                                     "démarrage",
    [UI_MENU_PROMPTS_PREPARING] = "fichiers d'icônes en cours de création en arrière-plan ; utilisés au prochain "
                                  "démarrage",
    [UI_MENU_PREPARE_RETRY] = "Réessayer",
    [UI_MENU_PREPARE_RETRY_DESC] = "Relance les outils de l'installation pour créer en arrière-plan la disposition "
                                   "d'interface ou les fichiers d'icônes des boutons manquants. Le jeu continue de "
                                   "tourner ; il les utilise dès son prochain démarrage.",
    [UI_MENU_SHAPE_PREPARE_FAILED] = "les outils de l'installation n'ont pas démarré : relancez l'installation",
    [UI_MENU_PROMPTS_OFF_MODS] = "désactivé : un mod RomFS remplace leurs fichiers",
    [UI_MENU_PROMPTS_OFF_PATCHES] = "désactivé tant que TOTK_ROMFS_PATCHES est défini",
    [UI_MENU_RESTART] = "Redémarrer maintenant",
    [UI_MENU_RESTART_DESC] = "Ferme le jeu et le relance avec les nouvelles icônes. La progression depuis votre "
                             "dernière sauvegarde est perdue.",
    [UI_MENU_RESTART_CONFIRM] = "Redémarrer le jeu maintenant ? La progression depuis votre dernière sauvegarde est "
                                "perdue.",
    [UI_MENU_RESTART_YES] = "Redémarrer",
    [UI_MENU_RESTART_SAVING] = "Le jeu sauvegarde : le redémarrage sera possible dans un instant.",
    [UI_MENU_GROUP_STICKS] = "Sticks analogiques",
    [UI_MENU_LSTICK_DEADZONE] = "Zone morte du stick gauche",
    [UI_MENU_RSTICK_DEADZONE] = "Zone morte du stick droit",
    [UI_MENU_STICK_DEADZONE_DESC] = "Jusqu'où un stick doit bouger avant que le jeu le remarque. Augmentez-la si le "
                                    "personnage ou la caméra bouge tout seul : lâchez le stick, lisez « stick actuel "
                                    "» et réglez la zone morte quelques pour cent au-dessus.",
    [UI_MENU_STICK_DEADZONE_NOW] = "stick actuel : %.0f %%",
    [UI_MENU_MOUSE_MODE] = "Caméra à la souris",
    [UI_MENU_MOUSE_MODE_DESC] = "Direct : la caméra tourne selon le déplacement de la souris, comme dans un jeu PC, "
                                "sans zone morte ni courbe de stick. Émulation du stick : la souris agit comme le stick droit.",
    [UI_MENU_MOUSE_STICK] = "Émulation du stick",
    [UI_MENU_MOUSE_DIRECT] = "Direct",
    [UI_MENU_MENU_CURSOR] = "Souris dans les menus du jeu",
    [UI_MENU_MENU_CURSOR_DESC] = "Dans les menus du jeu lui-même (écran titre, inventaire, boutiques, choix de "
                                 "dialogue), le pointeur s'affiche : l'élément en dessous est sélectionné, le clic "
                                 "gauche confirme, le clic droit revient en arrière. Seulement si la souris ou une "
                                 "touche a servi en dernier, si bien qu'une manette voit les menus inchangés (jeu "
                                 "1.4.3).",
    [UI_MENU_MAP_CLICK] = "Clic gauche sur la carte",
    [UI_MENU_MAP_CLICK_DESC] = "Sur la carte de la tablette de Purah, le bouton gauche de la souris appuie sur A au "
                               "curseur de la carte au lieu du bouton qui lui est attribué (Y par défaut). Demande "
                               "Souris dans les menus du jeu (jeu 1.4.3).",
    [UI_MENU_DIALOGUE_CLICK] = "Clics dans les dialogues",
    [UI_MENU_DIALOGUE_CLICK_DESC] = "Quand une boîte de dialogue est ouverte, le clic gauche appuie sur B (il fait "
                                    "avancer le texte et saute son défilement) et le clic droit sur A, au lieu des "
                                    "boutons qui leur sont attribués (Y par défaut, qui ouvre l'historique des "
                                    "dialogues, et ZL). Les choix de dialogue gardent le pointeur : le clic gauche "
                                    "choisit (jeu 1.4.3).",
    [UI_MENU_POINTER_IMAGE] = "Pointeur du jeu",
    [UI_MENU_POINTER_IMAGE_DESC] = "Quand le curseur n'est pas capturé (menus du jeu, ce menu, après Échap), la "
                                   "fenêtre affiche sa propre pointe de flèche dorée au lieu de la flèche du système, "
                                   "à la taille de l'échelle de l'écran.",
    [UI_MENU_QUICKSAVE] = "Sauvegarde/chargement rapide",
    [UI_MENU_QUICKSAVE_DESC] = "Une touche sauvegarde la partie immédiatement et une autre charge cette sauvegarde "
                               "(les deux lignes ci-dessous : F6 et F7 par défaut, comme dans Half-Life), sans aucun "
                               "menu à l'écran. Touches du clavier et de la souris uniquement, jeu 1.4.3 uniquement. "
                               "La sauvegarde rapide demande au jeu l'une de ses propres sauvegardes automatiques, "
                               "elle remplace donc la plus ancienne sauvegarde automatique (la sauvegarde manuelle "
                               "reste), en garde une copie comme sauvegarde rapide (le dossier quicksave du profil) "
                               "et affiche \"Partie sauvegardée\" en haut de l'image. Le chargement rapide fige "
                               "l'image sur \"Chargement de la sauvegarde rapide\" pendant qu'il parcourt hors de vue "
                               "la liste de chargement du jeu, et le jeu charge la sauvegarde rapide (environ 5 s). "
                               "Là où le jeu ne ferait pas lui-même de sauvegarde automatique (une cinématique, un "
                               "dialogue), rien n'est sauvegardé et la ligne l'indique.",
    [UI_MENU_QUICKSAVE_KEY] = "Sauvegarde rapide",
    [UI_MENU_QUICKLOAD_KEY] = "Chargement rapide",
    [UI_MENU_QUICKSAVE_KEY_DESC] = "La touche qui lance la sauvegarde rapide ou le chargement rapide ; F6 et F7 par "
                                   "défaut, comme dans Half-Life. Sélectionnez la case et appuyez sur une touche ou "
                                   "un bouton de souris ; Suppr laisse celle-ci sans touche. Tant que la sauvegarde "
                                   "rapide est activée, les deux touches ne font rien d'autre (une note s'affiche "
                                   "quand une attribution utilise aussi la touche). totk.ini accepte tout nom de "
                                   "touche des attributions (quicksave_key, quickload_key).",
    [UI_QS_SAVED] = "Partie sauvegardée",
    [UI_QS_LOADING] = "Chargement de la sauvegarde rapide",
    [UI_QS_NOT_NOW] = "Sauvegarde rapide impossible pour le moment",
    [UI_QS_NONE] = "Aucune sauvegarde rapide pour l'instant (la touche de sauvegarde rapide en crée une)",
    [UI_QS_FAILED] = "Échec de la sauvegarde rapide : voir totk.log",
    [UI_QS_LOAD_FAILED] = "Échec du chargement rapide : voir totk.log",
    [UI_MENU_MOUSE_SENS] = "Sensibilité de la caméra à la souris",
    [UI_MENU_MOUSE_SENS_DESC] = "La vitesse à laquelle la souris tourne la caméra.",
    [UI_MENU_WALK_KEY] = "Touche de marche",
    [UI_MENU_WALK_KEY_DESC] = "Maintenez-la pour n'incliner qu'en partie les sticks du clavier, afin que le clavier "
                              "marche au lieu de courir. Le facteur de marche fixe de combien. La manette et la "
                              "souris ne sont pas concernées.",
    [UI_MENU_WALK_KEY_TAKEN] = "aussi %s",
    [UI_MENU_WALK_SCALE] = "Facteur de marche",
    [UI_MENU_WALK_SCALE_DESC] = "L'inclinaison des sticks du clavier pendant que la touche de marche est maintenue. "
                                "La moitié est une marche, l'inclinaison complète une course.",
    [UI_MENU_FREECAM] = "Caméra libre",
    [UI_MENU_FREECAM_DESC] = "Permet à une combinaison ou à une touche ci-dessous de détacher la vue de Link et de la "
                             "faire voler. Désactivée par défaut : ainsi, le code de caméra du jeu s'exécute tel "
                             "quel. S'applique après un redémarrage. Version 1.4.3 du jeu.",
    [UI_MENU_FREECAM_OFF_NOTE] = "Activez Caméra libre ci-dessus, puis redémarrez",
    [UI_MENU_FREECAM_PAD] = "Combinaison caméra libre",
    [UI_MENU_FREECAM_PAD_DESC] = "Boutons de manette pressés ensemble qui font passer la vue par trois étapes : elle "
                                 "quitte Link et vole, elle reste là où elle a été laissée pendant que Link continue "
                                 "de jouer, puis elle revient. Pendant le vol, le jeu ne reçoit aucune entrée : le "
                                 "stick gauche fait voler le long de la vue et sur les côtés, les gâchettes vers le "
                                 "haut et le bas, le stick droit tourne, et chaque appui sur L ou R divise par deux "
                                 "ou double la vitesse. Vue fixée, Link marche là où pointe le stick gauche à "
                                 "l'écran. Version 1.4.3 du jeu.",
    [UI_MENU_FREECAM_PAD_V143] = "Version 1.4.3 du jeu uniquement",
    [UI_MENU_FREECAM_KEY] = "Touche de caméra libre",
    [UI_MENU_FREECAM_KEY_DESC] = "Une touche qui fait passer la vue par les mêmes étapes : elle quitte Link et vole, "
                                 "elle reste pendant que Link continue de jouer, puis elle revient. Pendant le vol : "
                                 "W A S D font voler le long de la vue et sur les côtés, les touches attribuées à ZR "
                                 "et ZL (X et Z par défaut) vers le haut et le bas, la souris et les flèches "
                                 "tournent, et les touches attribuées à L et R (Q et R) divisent par deux ou doublent "
                                 "la vitesse. Version 1.4.3 du jeu.",
    [UI_MENU_GROUP_GAMEPLAY_TWEAKS] = "Ajustements de jeu",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED] = "Ajustements de jeu",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED_DESC] = "Commandes à la molette de la souris pour la rotation et la distance de "
                                             "l'Emprise, le changement d'objets et d'armes, et un zoom fluide sur la "
                                             "carte.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE] = "Rotation de l'Emprise à la molette",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE_DESC] = "Tourne directement les objets tenus par l'Emprise avec la molette de "
                                               "la souris.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS] = "Axe de rotation C de l'Emprise",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS_DESC] = "Maintenez C en faisant défiler pour tourner les objets tenus autour "
                                               "de l'autre axe.",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY] = "Sensibilité de rotation de l'Emprise",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY_DESC] = "Vitesse à laquelle la molette de la souris tourne les objets "
                                                   "tenus.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE] = "Distance de l'Emprise à la molette",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE_DESC] = "Clic droit + molette de la souris pousse ou tire les objets tenus.",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY] = "Sensibilité de distance de l'Emprise",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY_DESC] = "Vitesse à laquelle la molette de la souris change la distance des "
                                                   "objets.",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL] = "Choix d'arme à la molette",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL_DESC] = "Faites défiler la molette de la souris en exploration libre pour changer "
                                           "d'arme. Confirmez avec E.",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY] = "Sensibilité de la molette des armes",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY_DESC] = "Crans de molette nécessaires pour passer à l'arme suivante.",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL] = "Molette dans le menu rapide",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL_DESC] = "Faites défiler la molette de la souris pour parcourir les objets quand un "
                                         "menu de sélection rapide est ouvert.",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY] = "Sensibilité de la molette du menu rapide",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY_DESC] = "Crans de molette nécessaires pour avancer d'un objet dans les menus "
                                               "de sélection rapide.",
    [UI_MENU_GAMEPLAY_ITEM_AD] = "Menu rapide : navigation A / D",
    [UI_MENU_GAMEPLAY_ITEM_AD_DESC] = "Appuyez sur A / D pour aller à gauche et à droite dans les menus de sélection "
                                      "rapide.",
    [UI_MENU_GAMEPLAY_MAP_ZOOM] = "Zoom fluide de la carte",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_DESC] = "Zoom fluide avec inertie sur la carte de la tablette de Purah, à la molette "
                                       "de la souris.",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY] = "Sensibilité du zoom de la carte",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY_DESC] = "Vitesse à laquelle la molette de la souris zoome sur la carte.",
    [UI_MENU_STICK_SENS] = "Vitesse de la caméra au stick",
    [UI_MENU_STICK_SENS_DESC] = "Multiplie la vitesse de caméra du stick droit du jeu, vitesse maximale comprise, "
                                "pour chacune des cinq sensibilités de caméra du jeu. La souris garde son propre "
                                "réglage.",
    [UI_BINDS_HELP] = "Sélectionnez une case et appuyez sur A ou Entrée, puis sur la touche, le bouton de souris ou "
                      "le bouton de manette. Suppr ou X vide une case. Une touche déjà utilisée est déplacée ici ; un "
                      "bouton de manette s'échange avec celui qu'il remplace.",
    [UI_BINDS_COL_KEY1] = "Touche 1",
    [UI_BINDS_COL_KEY2] = "Touche 2",
    [UI_BINDS_COL_PAD] = "Manette",
    [UI_BINDS_GROUP_BUTTONS] = "Boutons",
    [UI_BINDS_GROUP_MOVE] = "Déplacement",
    [UI_BINDS_GROUP_CAMERA] = "Caméra",
    [UI_BINDS_PRESS_KEY] = "Appuyez sur une touche ou un bouton de souris pour %s",
    [UI_BINDS_PRESS_PAD] = "Appuyez sur un bouton de manette pour %s",
    [UI_BINDS_CANCEL_HINT] = "Échap annule, ou attendez %d s",
    [UI_BINDS_TAKEN] = "%s a été retiré de %s",
    [UI_BINDS_SWAPPED] = "%s échangé avec %s",
    [UI_BINDS_RESET] = "Réinitialiser tous les boutons",
    [UI_BINDS_RESET_DESC] = "Retour aux touches et boutons de manette par défaut. La disposition des boutons "
                            "ci-dessus s'applique de nouveau : les boutons de manette réattribués restent à leur "
                            "place jusqu'à cette réinitialisation.",
    [UI_BIND_A] = "A",
    [UI_BIND_B] = "B",
    [UI_BIND_X] = "X",
    [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "Clic du stick gauche",
    [UI_BIND_RCLICK] = "Clic du stick droit",
    [UI_BIND_L] = "L",
    [UI_BIND_R] = "R",
    [UI_BIND_ZL] = "ZL",
    [UI_BIND_ZR] = "ZR",
    [UI_BIND_PLUS] = "Plus",
    [UI_BIND_MINUS] = "Moins",
    [UI_BIND_LEFT] = "Croix gauche",
    [UI_BIND_UP] = "Croix haut",
    [UI_BIND_RIGHT] = "Croix droite",
    [UI_BIND_DOWN] = "Croix bas",
    [UI_BIND_LSTICK_LEFT] = "Aller à gauche",
    [UI_BIND_LSTICK_UP] = "Avancer",
    [UI_BIND_LSTICK_RIGHT] = "Aller à droite",
    [UI_BIND_LSTICK_DOWN] = "Reculer",
    [UI_BIND_RSTICK_LEFT] = "Caméra à gauche",
    [UI_BIND_RSTICK_UP] = "Caméra en haut",
    [UI_BIND_RSTICK_RIGHT] = "Caméra à droite",
    [UI_BIND_RSTICK_DOWN] = "Caméra en bas",
    [UI_BIND_DESC_A] = "Interagir, ramasser, parler.",
    [UI_BIND_DESC_B] = "Courir, annuler, revenir.",
    [UI_BIND_DESC_X] = "Sauter.",
    [UI_BIND_DESC_Y] = "Attaquer.",
    [UI_BIND_DESC_LCLICK] = "S'accroupir.",
    [UI_BIND_DESC_RCLICK] = "Vue à la première personne.",
    [UI_BIND_DESC_L] = "Pouvoirs.",
    [UI_BIND_DESC_R] = "Lancer une arme.",
    [UI_BIND_DESC_ZL] = "Bouclier, cibler.",
    [UI_BIND_DESC_ZR] = "Arc.",
    [UI_BIND_DESC_PLUS] = "Menu, inventaire.",
    [UI_BIND_DESC_MINUS] = "Carte.",
    [UI_BIND_DESC_DPAD] = "Sélection rapide.",
    [UI_BIND_DESC_MOVE] = "Déplace Link comme le stick gauche. Le stick gauche de la manette fonctionne toujours "
                          "aussi.",
    [UI_BIND_DESC_CAMERA] = "Tourne la caméra comme le stick droit. La souris et le stick droit de la manette "
                            "fonctionnent toujours aussi.",
    [UI_BIND_SHAKE] = "Secouer le stick de caméra",
    [UI_BIND_DESC_SHAKE] = "Agite le stick droit à gauche et à droite pour le jeu : détache une pièce assemblée de ce "
                           "que tient l'Emprise. Maintenir la touche continue de secouer.",
    [UI_KEY_SPACE] = "Espace",
    [UI_KEY_TAB] = "Tab",
    [UI_KEY_LSHIFT] = "Maj gauche",
    [UI_KEY_LCTRL] = "Ctrl gauche",
    [UI_KEY_UP] = "Flèche haut",
    [UI_KEY_DOWN] = "Flèche bas",
    [UI_KEY_LEFT] = "Flèche gauche",
    [UI_KEY_RIGHT] = "Flèche droite",
    [UI_KEY_MOUSE_L] = "Clic gauche",
    [UI_KEY_MOUSE_R] = "Clic droit",
    [UI_KEY_MOUSE_M] = "Clic milieu",
    [UI_KEY_MOUSE_X1] = "Souris 4",
    [UI_KEY_MOUSE_X2] = "Souris 5",
    [UI_KEY_ENTER] = "Entrée",
    [UI_KEY_BACKSPACE] = "Retour arrière",
    [UI_KEY_RSHIFT] = "Maj droite",
    [UI_KEY_RCTRL] = "Ctrl droite",
    [UI_KEY_LALT] = "Alt",
    [UI_KEY_CAPSLOCK] = "Verr. Maj",
    [UI_KEY_INSERT] = "Inser",
    [UI_KEY_DELETE] = "Suppr",
    [UI_KEY_HOME] = "Début",
    [UI_KEY_END] = "Fin",
    [UI_KEY_PAGEUP] = "Page préc.",
    [UI_KEY_PAGEDOWN] = "Page suiv.",
    [UI_KEY_NUM_FMT] = "Pavé num. %s",
    [UI_PAD_SOUTH] = "Bouton du bas",
    [UI_PAD_EAST] = "Bouton de droite",
    [UI_PAD_WEST] = "Bouton de gauche",
    [UI_PAD_NORTH] = "Bouton du haut",
    [UI_PAD_LSTICK] = "Stick gauche",
    [UI_PAD_RSTICK] = "Stick droit",
    [UI_PAD_L] = "Bouton de tranche gauche",
    [UI_PAD_R] = "Bouton de tranche droit",
    [UI_PAD_START] = "Start",
    [UI_PAD_BACK] = "Retour",
    [UI_PAD_DPAD_LEFT] = "Croix gauche",
    [UI_PAD_DPAD_UP] = "Croix haut",
    [UI_PAD_DPAD_RIGHT] = "Croix droite",
    [UI_PAD_DPAD_DOWN] = "Croix bas",
    [UI_PAD_ZL] = "Gâchette gauche",
    [UI_PAD_ZR] = "Gâchette droite",
    [UI_PAD_LPADDLE1] = "Palette arrière gauche",
    [UI_PAD_RPADDLE1] = "Palette arrière droite",
    [UI_PAD_LPADDLE2] = "Palette arrière gauche 2",
    [UI_PAD_RPADDLE2] = "Palette arrière droite 2",
    [UI_PAD_TOUCHPAD] = "Clic du pavé tactile",
    [UI_PAD_CROSS] = "Croix",
    [UI_PAD_CIRCLE] = "Rond",
    [UI_PAD_SQUARE] = "Carré",
    [UI_PAD_TRIANGLE] = "Triangle",
    [UI_MENU_AMIIBO_DIR] = "Dossier des amiibo",
    [UI_MENU_AMIIBO_OPEN] = "Ouvrir",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "Dossier par défaut",
    [UI_MENU_AMIIBO_REFRESH] = "Actualiser",
    [UI_MENU_AMIIBO_PICK] = "Dumps d'amiibo (fichiers .bin ou .nfc de 540 octets) :",
    [UI_MENU_AMIIBO_NO_DIR] = "Impossible d'ouvrir le dossier %s",
    [UI_MENU_AMIIBO_READER] = "Lecteur NFC",
    [UI_MENU_AMIIBO_STATE] = "Lecteur :",
    [UI_MENU_AMIIBO_STATE_IDLE] = "prêt",
    [UI_MENU_AMIIBO_STATE_SEARCHING] = "le jeu attend un amiibo",
    [UI_MENU_AMIIBO_STATE_FOUND] = "amiibo sur le lecteur",
    [UI_MENU_AMIIBO_STATE_MOUNTED] = "le jeu lit l'amiibo",
    [UI_MENU_AMIIBO_STATE_OFF] = "désactivé",
    [UI_MENU_AMIIBO_FILE] = "Fichier choisi :",
    [UI_MENU_AMIIBO_NO_FILE] = "(aucun fichier choisi)",
    [UI_MENU_AMIIBO_TAP] = "Poser l'amiibo",
    [UI_MENU_AMIIBO_REMOVE] = "Retirer l'amiibo",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "Retirer l'amiibo tout seul après l'avoir posé",
    [UI_MENU_AMIIBO_HOLD] = "Temps sur le lecteur",
    [UI_MENU_AMIIBO_SECONDS] = "%.1f s",
    [UI_MENU_AMIIBO_TAG] = "Amiibo sur le lecteur :",
    [UI_MENU_AMIIBO_NO_TAG] = "Aucun amiibo sur le lecteur.",
    [UI_MENU_AMIIBO_TAG_FILE] = "Fichier : %s",
    [UI_MENU_AMIIBO_TAG_ID] = "Personnage %s, série %02x, numéro %04x, type %02x",
    [UI_MENU_AMIIBO_TAG_UID] = "UID : %s",
    [UI_MENU_AMIIBO_READY] = "Prêt",
    [UI_MENU_AMIIBO_TAPPED] = "Posé : %s",
    [UI_MENU_AMIIBO_LOAD_FAILED] = "Pas un dump d'amiibo (NTAG215 de 540 octets) : %s",
    [UI_MENU_AMIIBO_REMOVED] = "Amiibo retiré",
    [UI_MENU_AMIIBO_STATUS] = "État : %s",
    [UI_MENU_MENU_LANG] = "Langue du menu",
    [UI_MENU_MENU_LANG_DESC] = "Langue de ce menu et de l'affichage des performances.",
    [UI_MENU_MENU_LANG_AUTO] = "Automatique (selon Windows)",
    [UI_MENU_DISCORD_RPC] = "Discord Rich Presence",
    [UI_MENU_DISCORD_RPC_DESC] = "Affiche \"Joue à TotK Recomp\" avec le temps de jeu dans votre statut Discord "
                                 "pendant que le jeu tourne. discord_app_id et discord_image dans totk.ini "
                                 "choisissent l'application.",
    [UI_MENU_ADV_WARNING] = "Ces paramètres servent aux tests et au dépannage. Ne les modifiez que si vous savez "
                            "pourquoi.",
    [UI_MENU_GROUP_SCHED] = "Ordonnancement",
    [UI_MENU_GROUP_SHADERS] = "Shaders et pipelines",
    [UI_MENU_GROUP_TEXMEM] = "Textures et mémoire",
    [UI_MENU_GROUP_FIXES] = "Corrections du rendu",
    [UI_MENU_GROUP_VULKAN] = "Vulkan",
    [UI_MENU_GROUP_DIAG] = "Diagnostic",
    [UI_MENU_ASYNC_PREEMPT] = "Préemption des cœurs virtuels",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "Permet à un thread du jeu d'en interrompre un autre sur le même cœur virtuel.",
    [UI_MENU_PREEMPT_OFF] = "Désactivée",
    [UI_MENU_PREEMPT_ON] = "Activée",
    [UI_MENU_PREEMPT_INHERIT] = "Héritage seulement",
    [UI_MENU_REPLAY_ASYNC] = "Préparation GPU des images sur son propre thread",
    [UI_MENU_REPLAY_ASYNC_DESC] = "Prépare les images pour le GPU sur un thread séparé au lieu du thread du jeu. "
                                  "Ajoute jusqu'à une image de latence.",
    [UI_MENU_REPLAY_WORKERS] = "Threads de préparation GPU",
    [UI_MENU_REPLAY_WORKERS_DESC] = "Enregistre les grandes images pour le GPU sur plusieurs threads à la fois. Utile "
                                    "quand le CPU limite la fréquence d'images ; demande au moins 8 processeurs "
                                    "logiques. Désactivé enregistre sur un seul thread.",
    [UI_MENU_REPLAY_WORKERS_NOW] = "actuel : %s",
    [UI_MENU_TIMER_1MS] = "Minuteur Windows de 1 ms",
    [UI_MENU_TIMER_1MS_DESC] = "Porte la résolution du minuteur système à 1 ms pour un rythme d'images plus régulier.",
    [UI_MENU_SYNC_AHEAD] = "Une image d'avance sur le GPU",
    [UI_MENU_SYNC_AHEAD_DESC] = "Le jeu prépare l'image suivante pendant que la carte graphique dessine encore la précédente. Plus d'images par seconde, une image de latence d'entrée en plus.",
    [UI_MENU_HOST_CHECKPOINT] = "Cession rapide du cœur",
    [UI_MENU_HOST_CHECKPOINT_DESC] = "Un thread du jeu cède son cœur virtuel à un thread plus important dès qu'il quitte le code du portage, et non à son prochain appel système.",
    [UI_MENU_PIPE_WAIT] = "Gestion des draws en attente",
    [UI_MENU_PIPE_WAIT_DESC] = "Par défaut garde l'ordonnancement actuel avec un worker réservé aux draws en attente "
                               "; Ancien (draw) utilise l'ordonnancement précédent.",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "Par défaut",
    [UI_MENU_PIPE_WAIT_DRAW] = "Ancien (draw)",
    [UI_MENU_PIPE_SKIP] = "Draws ignorés pendant la création d'un pipeline",
    [UI_MENU_PIPE_SKIP_DESC] = "Quels draws sont abandonnés au lieu d'attendre leur pipeline.",
    [UI_MENU_PIPE_SKIP_STRICT] = "Strict",
    [UI_MENU_PIPE_SKIP_ANY] = "Tous",
    [UI_MENU_PIPE_SKIP_FULLSCREEN] = "Passes plein écran",
    [UI_MENU_LIB_PREBUILD] = "Préconstruction des bibliothèques d'étapes",
    [UI_MENU_LIB_PREBUILD_DESC] = "Construit à l'avance les bibliothèques d'étapes de shaders en arrière-plan.",
    [UI_MENU_LIB_PREBUILD_THREADS] = "Threads de préconstruction des bibliothèques",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "Threads utilisés pour la préconstruction ; Automatique choisit un nombre "
                                          "selon votre CPU.",
    [UI_MENU_PIPE_PREBUILD] = "Préconstruction des pipelines",
    [UI_MENU_PIPE_PREBUILD_DESC] = "Construit à l'avance les pipelines connus.",
    [UI_MENU_PREWARM_XLATE] = "Préchauffage du traducteur",
    [UI_MENU_PREWARM_XLATE_DESC] = "Threads qui traduisent les shaders connus au démarrage ; Automatique prend la "
                                   "moitié des threads du CPU, 16 au plus.",
    [UI_MENU_GPL] = "Bibliothèque de pipelines graphiques",
    [UI_MENU_GPL_DESC] = "Utilise l'extension Vulkan de bibliothèque de pipelines ; désactivez-la si votre pilote "
                         "fonctionne mal avec.",
    [UI_MENU_ASTC] = "Décodage des textures ASTC",
    [UI_MENU_ASTC_DESC] = "Où les textures compressées ASTC sont décodées.",
    [UI_MENU_ASTC_AUTO] = "Automatique",
    [UI_MENU_ASTC_GPU] = "Shader GPU",
    [UI_MENU_ASTC_NATIVE] = "Natif",
    [UI_MENU_ASTC_CPU] = "CPU",
    [UI_MENU_MEMORY] = "Mode mémoire GPU",
    [UI_MENU_MEMORY_DESC] = "Comment la mémoire du jeu est partagée avec le GPU.",
    [UI_MENU_MEMORY_AUTO] = "Automatique",
    [UI_MENU_MEMORY_IMPORT] = "Importation",
    [UI_MENU_MEMORY_MIRROR] = "Miroir",
    [UI_MENU_MEMORY_VERIFY] = "Vérification",
    [UI_MENU_TEX_ALIAS] = "Alias de textures",
    [UI_MENU_TEX_ALIAS_DESC] = "Une nouvelle texture créée sur la mémoire d'une texture active commence avec son "
                               "contenu au lieu d'être vide.",
    [UI_MENU_TEX_VRAM_PRESSURE] = "Limite de mémoire des textures",
    [UI_MENU_TEX_VRAM_PRESSURE_DESC] = "Pourcentage de mémoire vidéo au-delà duquel les textures libérées par le jeu "
                                       "sont abandonnées (elles reviennent du stockage quand elles resservent). 0 les "
                                       "garde.",
    [UI_MENU_ASTC_BC] = "Compresser l'ASTC en BC3",
    [UI_MENU_ASTC_BC_DESC] = "Avec le décodeur GPU : réencode les textures ASTC décodées en BC3, un quart de la "
                             "mémoire vidéo. Pour les GPU de 4 Go ou moins ; les textures paraissent un peu plus "
                             "pixelisées.",
    [UI_MENU_LOD_GUARD] = "Protection LOD",
    [UI_MENU_LOD_GUARD_DESC] = "Évite que des objets clignotent au mauvais endroit ou que le lointain paraisse plat "
                               "pendant une image, en attendant que le jeu ait fini de réécrire les données de niveau "
                               "de détail avant que le GPU les lise.",
    [UI_MENU_FIX_C0] = "Correction du tampon de constantes",
    [UI_MENU_FIX_C0_DESC] = "Remplit la banque de constantes c0 réservée au pilote (tailles de stockage, taille de "
                            "dispatch, taille de point, instance de base) au lieu de lire des zéros.",
    [UI_MENU_FIX_ZERO] = "Remettre à zéro la nouvelle mémoire GPU",
    [UI_MENU_FIX_ZERO_DESC] = "Les nouveaux pools de mémoire GPU commencent remplis de zéros, comme la mémoire de la "
                              "Switch.",
    [UI_MENU_DLAA_EFFECTS_AFTER] = "Particules après DLAA",
    [UI_MENU_DLAA_EFFECTS_AFTER_DESC] = "Avec DLAA, la poussière et les autres particules sont dessinées par-dessus "
                                        "l'image déjà lissée par DLSS, comme le jeu les dessine sans DLSS : les "
                                        "petites particules dans le brouillard (sanctuaires) ne disparaissent plus et "
                                        "ne laissent plus de traînées. Le brouillard et les nuages passent toujours "
                                        "par DLSS, donc les bords lointains sur le ciel restent nets. Désactivé : "
                                        "DLSS traite aussi les particules, comme avant. Mode DLAA uniquement ; les "
                                        "modes DLSS avec mise à l'échelle les traitent toujours. S'applique "
                                        "immédiatement.",
    [UI_MENU_DLSS_FIXED_EXPOSURE] = "Exposition DLSS fixe",
    [UI_MENU_DLSS_FIXED_EXPOSURE_DESC] = "DLSS prend la luminosité de l'image du jeu telle quelle au lieu d'estimer "
                                         "l'exposition à chaque image : dans les scènes sombres (les Profondeurs), "
                                         "les grandes zones de brouillard ne s'assombrissent ni ne s'éclaircissent "
                                         "plus pendant une image quand la caméra bouge. Désactivé : DLSS estime "
                                         "lui-même l'exposition, comme avant. S'applique immédiatement.",
    [UI_MENU_BIND_SKIP] = "Ignorer les liaisons répétées",
    [UI_MENU_BIND_SKIP_DESC] = "Optimisation : ignore les liaisons qui n'ont pas changé.",
    [UI_MENU_SHADER_MEMO] = "Mémo des shaders",
    [UI_MENU_SHADER_MEMO_DESC] = "Optimisation : mémorise les recherches de shaders.",
    [UI_MENU_PIPE_MEMO] = "Mémo des pipelines",
    [UI_MENU_PIPE_MEMO_DESC] = "Optimisation : réutilise le pipeline du draw précédent quand son état se répète.",
    [UI_MENU_TEX_MEMO] = "Mémo des liaisons de textures",
    [UI_MENU_TEX_MEMO_DESC] = "Optimisation : mémorise les descripteurs de textures entre les draws.",
    [UI_MENU_DESC_TEMPLATE] = "Modèles de descripteurs",
    [UI_MENU_DESC_TEMPLATE_DESC] = "Optimisation : met à jour les descripteurs en bloc.",
    [UI_MENU_PUSH_SKIP] = "Ignorer les push constants répétées",
    [UI_MENU_PUSH_SKIP_DESC] = "Optimisation : ignore les push constants qui n'ont pas changé.",
    [UI_MENU_SPLIT_DRAWS] = "Envoi anticipé au GPU",
    [UI_MENU_SPLIT_DRAWS_DESC] = "Envoie le travail GPU par parties tous les N draws entre les passes pour que le GPU "
                                 "commence plus tôt ; 0 le désactive.",
    [UI_MENU_VK_VALIDATION] = "Couche de validation",
    [UI_MENU_VK_VALIDATION_DESC] = "Vérifie chaque appel Vulkan et ralentit beaucoup le jeu.",
    [UI_MENU_VK_VALIDATION_MISSING] = "La couche de validation n'est pas installée.",
    [UI_MENU_VK_DYN_VERTEX] = "État de sommets dynamique",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "Force l'entrée de sommets dynamique en activé ou désactivé ; la valeur par défaut "
                                   "dépend du GPU.",
    [UI_MENU_VK_DYN_A2C] = "Alpha to coverage dynamique",
    [UI_MENU_VK_DYN_A2C_DESC] = "Force l'alpha to coverage dynamique en activé ou désactivé. Par défaut : désactivé "
                                "avec le pilote AMD pour RDNA 2 et antérieurs, qui dessine alors les personnages en "
                                "noir, activé ailleurs.",
    [UI_MENU_VK_SUBGROUP_PIN] = "Fixer la taille des sous-groupes",
    [UI_MENU_VK_SUBGROUP_PIN_DESC] = "Fixe la taille des sous-groupes des shaders.",
    [UI_MENU_TRACE] = "Journal des performances",
    [UI_MENU_TRACE_DESC] = "Écrit les temps d'image dans un fichier.",
    [UI_MENU_FLIGHT] = "Enregistreur de vol",
    [UI_MENU_FLIGHT_DESC] = "Garde un court historique et l'enregistre quand une image se fige.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "Seuil de blocage (ms)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "Temps d'image compté comme un blocage.",
    [UI_MENU_PASS_CRUMBS] = "Marques de passes GPU",
    [UI_MENU_PASS_CRUMBS_DESC] = "Après un plantage du GPU, le journal nomme la passe de rendu en cours. Automatique "
                                 ": AMD uniquement. Activé : aussi NVIDIA, pour un faible coût.",
    [UI_MENU_INPUT_RECORD] = "Enregistrement des entrées",
    [UI_MENU_INPUT_RECORD_DESC] = "Écrit vos entrées dans un fichier pour les rapports de bug.",
    [UI_MENU_OPEN_LOG] = "Ouvrir le dossier des journaux",
    [UI_MENU_OPEN_LOG_DESC] = "Affiche le dossier contenant les fichiers journaux.",
    [UI_MENU_COPY_INFO] = "Copier les infos système",
    [UI_MENU_COPY_INFO_DESC] = "Copie le GPU, le pilote, le CPU, la RAM et les paramètres modifiés dans le "
                               "presse-papiers.",
    [UI_MENU_COPY_INFO_DONE] = "Copié",
    [UI_MENU_ACTION_FAILED] = "n'a pas fonctionné",
    [UI_MENU_RESET] = "Rétablir tous les paramètres par défaut",
    [UI_MENU_RESET_DESC] = "S'applique à tous les onglets.",
    [UI_MENU_RESET_CONFIRM] = "Rétablir chaque paramètre à sa valeur par défaut ?",
    [UI_MENU_RESET_YES] = "Rétablir",
    [UI_MENU_RESET_NO] = "Annuler",
    [UI_MENU_OV_FPS] = "%.0f IPS",
    [UI_MENU_OV_FRAME_MS] = "%.1f ms",
    [UI_MENU_OV_GAME_PCT] = "jeu %.0f%%",
    [UI_MENU_OV_MAX_MS] = "max %.1f ms",
    [UI_MENU_OV_FPS_LABEL] = "IPS",
    [UI_MENU_OV_FRAME_LABEL] = "Image",
    [UI_MENU_OV_FMT_FPS] = "%.0f",
    [UI_MENU_OV_LOW1] = "1%% min %.0f",
    [UI_MENU_OV_LOW01] = "0,1%% min %.0f",
    [UI_MENU_OV_GPU] = "GPU",
    [UI_MENU_OV_CPU] = "CPU",
    [UI_MENU_OV_RAM] = "RAM",
    [UI_MENU_OV_VRAM] = "VRAM",
    [UI_MENU_OV_TEMP] = "Température",
    [UI_MENU_OV_POWER] = "Puissance",
    [UI_MENU_OV_CLOCK] = "Fréquence",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "Préparation GPU des images",
    [UI_MENU_OV_GPU_TIME] = "GPU occupé par image",
    [UI_MENU_OV_PIPE_WAIT] = "Attentes de pipelines",
    [UI_MENU_OV_SHADER_WAIT] = "Attentes de shaders",
    [UI_MENU_OV_TEX_UPLOADS] = "Envois de textures",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%",
    [UI_MENU_OV_FMT_TEMP] = "%.0f °C",
    [UI_MENU_OV_FMT_POWER] = "%.0f W",
    [UI_MENU_OV_FMT_MHZ] = "%.0f MHz",
    [UI_MENU_OV_FMT_GB] = "%.1f Go",
    [UI_MENU_OV_FMT_GB_OF] = "/ %.1f Go",
    [UI_MENU_OV_GAME_GB] = "jeu %.1f Go",
    [UI_MENU_OV_FMT_MS] = "%.2f ms",
    [UI_MENU_OV_PER_FRAME] = "%.1f par image",
    [UI_MENU_OV_WINDOW] = "Fenêtre",
    [UI_MENU_OV_OUTPUT] = "Image du jeu",
    [UI_MENU_OV_SCENE] = "Scène 3D",
    [UI_MENU_OV_UPSCALER] = "Upscaler 3D",
    [UI_MENU_OV_SCALING] = "Mise à l'échelle",
    [UI_MENU_OV_LIMIT] = "Limite d'images",
    [UI_MENU_OV_PRESENT] = "Présentation",
    [UI_MENU_OV_SHOWN] = "Affichées",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u",
    [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, netteté %d%%",
    [UI_MENU_OV_SCALE_NONE] = "aucune (1:1)",
    [UI_MENU_OV_SCALE_BILINEAR] = "bilinéaire",
    [UI_MENU_OV_SCALE_DOWN] = "bilinéaire (FSR ne fait qu'agrandir)",
    [UI_MENU_TAB_CHEATS] = "Triches",
    [UI_MENU_CHEATS_NONE] = "Aucun code de triche pour cette version du jeu. Les codes d'un mod sont un fichier de "
                            "triche Atmosphère dans son dossier cheats, nommé d'après le build du jeu : "
                            "mods\\<mod>\\cheats\\%s. Redémarrez le jeu pour les lire. Pour les codes qui modifient "
                            "le code du jeu, le jeu se reconstruit au démarrage (quelques minutes).",
    [UI_MENU_CHEATS_MOD] = "%s : %d codes",
    [UI_MENU_CHEAT_CODE_DESC] = "Modifie le code du jeu. Bascule immédiatement.",
    [UI_MENU_CHEAT_KEY_DESC] = "Modifie le code du jeu tant que vous maintenez %s. Bascule immédiatement.",
    [UI_MENU_CHEAT_DATA_DESC] = "Écrit dans la mémoire du jeu à chaque image tant qu'il est activé.",
    [UI_MENU_CHEAT_MASTER] = "toujours activé",
    [UI_MENU_CHEAT_MASTER_DESC] = "Le code maître du fichier : il est toujours activé.",
    [UI_MENU_CHEAT_RESTORE] = "inutile",
    [UI_MENU_CHEAT_RESTORE_DESC] = "Il ne fait que réécrire le code d'origine du jeu. Désactiver un code le fait déjà.",
    [UI_MENU_CHEAT_REBUILD] = "après la reconstruction",
    [UI_MENU_CHEAT_REBUILD_DESC] = "Ce code modifie le code du jeu et est nouveau ou modifié depuis la construction "
                                   "du jeu. Le jeu l'intègre à son prochain démarrage (quelques minutes), ou utilisez "
                                   "Reconstruire le jeu maintenant.",
    [UI_MENU_CHEAT_UNSUPPORTED] = "non pris en charge",
    [UI_MENU_CHEAT_UNSUPPORTED_DESC] = "Ce code ne peut pas être utilisé avec cette version du jeu :",
    [UI_MENU_CHEATS_REBUILD] = "Reconstruire le jeu maintenant",
    [UI_MENU_CHEATS_REBUILD_DESC] = "Certains codes modifient le code du jeu et n'y sont pas intégrés : ils sont "
                                    "nouveaux ou modifiés, ou leur dernière reconstruction a échoué ou a été annulée. "
                                    "Ferme le jeu ; l'installation les intègre (quelques minutes) et relance le jeu. "
                                    "La progression depuis votre dernière sauvegarde est perdue.",
    [UI_MENU_MODS_EXEFS_UNSUPPORTED] = "Ce mod modifie le code du jeu (exefs) d'une façon que le portage ne peut pas "
                                       "appliquer, donc tout le mod reste désactivé. %s",
    [UI_MENU_MODS_EXEFS_OTHER] = "Ses correctifs de code (exefs) sont faits pour une autre version du jeu.",
    [UI_MENU_MODS_EXEFS_ON] = "Correctifs de code utilisés : %d fichiers.",
    [UI_MENU_MODS_REPLACES_BUILTIN] = "Correctifs de code utilisés : %d fichiers. Ils remplacent le correctif %s du "
                                      "portage, qui reste désactivé tant que ce mod est activé.",
    [UI_MENU_MODS_CODE_SKIPPED] = "Son propre code de jeu (%s) n'est pas chargé : le reste du mod est utilisé. "
                                  "Désactivez le mod si le jeu se comporte mal avec.",
    [UI_MENU_MODS_PLUGIN_ON] = "Son plugin est actif : %d hooks dans le code du jeu.",
    [UI_MENU_MODS_PLUGIN_REFUSED] = "Son plugin %s ne peut pas fonctionner dans le portage : %s.",
    [UI_MENU_MODS_PLUGIN_STAYS_OFF] = "Tout le mod reste désactivé. Son interrupteur \"Utiliser ... sans son code\" "
                                      "ci-dessous en utilise le reste.",
    [UI_MENU_MODS_PLUGIN_OFF] = "Son plugin reste désactivé cette fois : il a démarré autrement que lors de la "
                                "construction (%s). Le jeu le reconstruit au prochain démarrage.",
    [UI_MENU_MODS_WITHOUT_CODE] = "Utiliser %s sans son code",
    [UI_MENU_MODS_WITHOUT_CODE_DESC] = "Le mod ci-dessus apporte du code que le portage ne peut pas exécuter : son "
                                       "propre code de jeu (un plugin subsdk ou un main remplacé) ou des correctifs "
                                       "de code (exefs) que le portage ne peut pas appliquer. Désactivé : tout le mod "
                                       "reste désactivé. Activé : ce code est laissé de côté et le reste du mod est "
                                       "utilisé : ses fichiers RomFS, ses codes de triche et les correctifs de code "
                                       "qui fonctionnent. Ses fichiers peuvent avoir besoin du code manquant ; "
                                       "désactivez-le de nouveau si le jeu se comporte mal. S'applique au prochain "
                                       "démarrage.",
    [UI_MENU_MODS_WITHOUT_CODE_ALL] = "Activé pour tous les mods : mods_without_code = 1 dans totk.ini.",
    [UI_MENU_MODS_EXEFS_SKIPPED] = "%u de ses fichiers de correctifs de code (exefs) ne peuvent pas être appliqués et "
                                   "sont laissés de côté : le reste du mod est utilisé. %s",
    [UI_MENU_MODS_OVERRIDES_FILES] = "%u de ses fichiers remplacent ceux de %s, placé plus bas dans la liste.",
    [UI_MENU_MODS_DATA_OVERRIDDEN] = "%u octets de ses modifications des données du jeu viennent de %s, placé plus "
                                     "haut dans la liste.",
    [UI_MENU_MODS_OVERRIDES_DATA] = "%u octets de ses modifications des données du jeu remplacent ceux de %s, placé "
                                    "plus bas dans la liste.",
    [UI_MENU_MODS_CODE_OVERRIDDEN] = "%u de ses emplacements de correctifs de code sont remplacés par ceux de %s, "
                                     "placé plus haut dans la liste.",
    [UI_MENU_MODS_OVERRIDES_CODE] = "Il remplace %u emplacements de correctifs de code de %s, placé plus bas dans la "
                                    "liste.",
};

// Spanish
static const char *const text_es[UI_TEXT_COUNT] = {
    SWKBD("Introducir texto", "Introduce un nombre", "Caballo", "Aceptar", "Cancelar", "Mando: A = Aceptar, B = Cancelar"),

    [UI_MENU_TITLE] = "Ajustes",
    [UI_MENU_TAB_SCREEN] = "Pantalla",
    [UI_MENU_TAB_GAME] = "Juego",
    [UI_MENU_TAB_MODS] = "Mods",
    [UI_MENU_TAB_SOUND] = "Sonido",
    [UI_MENU_TAB_CONTROLS] = "Controles",
    [UI_MENU_TAB_AMIIBO] = "Amiibo",
    [UI_MENU_TAB_MENU] = "Menú",
    [UI_MENU_TAB_ADVANCED] = "Avanzado",
    [UI_MENU_CONTINUE] = "Continuar",
    [UI_MENU_QUIT] = "Salir del juego",
    [UI_MENU_ON] = "Sí",
    [UI_MENU_OFF] = "No",
    [UI_MENU_AUTO] = "Automático",
    [UI_MENU_DEFAULT_MARK] = "predeterminado",
    [UI_MENU_AFTER_RESTART] = "tras reiniciar",
    [UI_MENU_SET_BY_ENV] = "fijado por una variable de entorno",
    [UI_MENU_SAVE_FAILED] = "no se pudieron guardar los ajustes",
    [UI_MENU_VERSION] = "TotK Recomp %s, juego %s",
    [UI_MENU_FULLSCREEN] = "Pantalla completa",
    [UI_MENU_FULLSCREEN_DESC] = "El juego ocupa toda la pantalla; F11 la alterna.",
    [UI_MENU_WINDOW_SIZE] = "Tamaño de la ventana",
    [UI_MENU_WINDOW_SIZE_DESC] = "Tamaño de la ventana del juego fuera de la pantalla completa.",
    [UI_MENU_WINDOW_SIZE_CUSTOM] = "Personalizado",
    [UI_MENU_PRESENT] = "Presentación",
    [UI_MENU_PRESENT_DESC] = "Cómo llegan a la pantalla los fotogramas terminados.",
    [UI_MENU_PRESENT_MAILBOX] = "Mailbox",
    [UI_MENU_PRESENT_VSYNC] = "VSync",
    [UI_MENU_PRESENT_IMMEDIATE] = "Inmediato",
    [UI_MENU_PRESENT_IN_USE] = "en uso: %s",
    [UI_MENU_GPU] = "Tarjeta gráfica",
    [UI_MENU_GPU_DESC] = "El dispositivo Vulkan que dibuja el juego.",
    [UI_MENU_GPU_AUTO] = "Automática",
    [UI_MENU_PERF_OVERLAY] = "Indicador de rendimiento",
    [UI_MENU_PERF_OVERLAY_DESC] = "Fotogramas por segundo en la esquina superior derecha. Completo añade tiempos de "
                                  "fotograma con un gráfico, carga de CPU, GPU y memoria, temperaturas y las esperas "
                                  "del hilo de renderizado. F10 cambia de modo.",
    [UI_MENU_PERF_OFF] = "No",
    [UI_MENU_PERF_FPS] = "FPS",
    [UI_MENU_PERF_FULL] = "Completo",
    [UI_PERF_LINE_FPS] = "%.0f fps",
    [UI_PERF_LINE_FPS_GEN] = "%.0f fps dibujados, %.0f mostrados",
    [UI_MENU_GRAPHICS] = "Imagen",
    [UI_MENU_OUTPUT_RES] = "Resolución del juego",
    [UI_MENU_OUTPUT_RES_DESC] = "El tamaño en el que el juego dibuja su imagen e interfaz. Por encima de 1920 x 1080 "
                                "el juego necesita más memoria de vídeo y tiempo de GPU. Un formato más ancho (21:9) "
                                "amplía la vista de la cámara y lleva el HUD a las esquinas de la pantalla; uno más "
                                "alto (16:10, Steam Deck) lo lleva a los bordes superior e inferior.",
    [UI_MENU_HUD_WIDTH] = "Ancho del HUD",
    [UI_MENU_HUD_WIDTH_DESC] = "En una pantalla más ancha que 16:9, cuánto se separan los lados izquierdo y derecho "
                               "del HUD. Pantalla completa los lleva a los bordes de la pantalla; 21:9 o 16:9 "
                               "mantiene el HUD en el centro de la pantalla con esa forma. Una distribución que falte "
                               "se crea en segundo plano y se usa tras reiniciar.",
    [UI_MENU_HUD_WIDTH_FULL] = "Pantalla completa",
    [UI_MENU_RENDER_SCALE] = "Escala de renderizado 3D",
    [UI_MENU_RENDER_SCALE_DESC] = "Tamaño de la escena 3D respecto a la resolución del juego; el FSR del juego la "
                                  "escala a ese tamaño. Por encima del 100 % suaviza los bordes dentados "
                                  "(supermuestreo) con un alto coste de GPU.",
    [UI_MENU_RENDER_SCALE_GAME] = "Juego (83 %)",
    [UI_MENU_DYNAMIC_RES] = "Resolución dinámica",
    [UI_MENU_DYNAMIC_RES_DESC] = "Permite que el juego baje la escena 3D al 90 o al 80 %. Decide según una estimación "
                                 "hecha para Switch que no tiene en cuenta tu GPU, así que las escenas amplias bajan "
                                 "incluso en un PC rápido. No: mantiene la escala de renderizado.",
    [UI_MENU_UPSCALER] = "Escalado a la ventana",
    [UI_MENU_UPSCALER_DESC] = "Cómo se escala la imagen cuando la ventana es mayor que la resolución del juego. FSR "
                              "mantiene los bordes y los detalles más nítidos que el bilineal.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1",
    [UI_MENU_UPSCALER_BILINEAR] = "Bilineal",
    [UI_MENU_ASPECT] = "Forma de la imagen",
    [UI_MENU_ASPECT_DESC] = "Cuando la forma de la ventana difiere de la de la resolución del juego (una imagen 16:9 "
                            "en una pantalla 21:9): mantener la forma con franjas negras o estirarla sobre la ventana.",
    [UI_MENU_ASPECT_FIT] = "Franjas negras",
    [UI_MENU_ASPECT_STRETCH] = "Estirar",
    [UI_MENU_MOVIE_ASPECT] = "Forma de vídeos",
    [UI_MENU_MOVIE_ASPECT_DESC] = "Cuando la resolución del juego no es 16:9: mantener la forma propia de los vídeos "
                                  "de las cinemáticas con franjas negras o estirarlos sobre toda la imagen. Se aplica "
                                  "desde el siguiente vídeo.",
    [UI_MENU_MOVIE_ASPECT_KEEP] = "Franjas negras",
    [UI_MENU_MOVIE_ASPECT_STRETCH] = "Estirar",
    [UI_MENU_FOV] = "Campo de visión",
    [UI_MENU_FOV_DESC] = "Cuánto del mundo muestra la cámara, en porcentaje del propio del juego. Solo la versión "
                         "1.4.3 del juego.",
    [UI_MENU_VERT_PLUS] = "Mantener el ancho de 16:9",
    [UI_MENU_VERT_PLUS_DESC] = "En pantallas más estrechas que 16:9 (Steam Deck, 16:10, 4:3), la cámara muestra el mismo ancho del mundo que en 16:9, y más por arriba y por abajo. Sin efecto en 16:9 o más ancho.",
    [UI_MENU_SHADOW_RES] = "Tamaño del mapa de sombras",
    [UI_MENU_SHADOW_RES_DESC] = "Tamaño del mapa de sombras del sol. El tamaño del juego cambia con la vista, hasta "
                                "unos 400 cerca del suelo, por lo que las sombras de las hojas se ven pixeladas y "
                                "saltan; un tamaño fijo las mantiene quietas y más nítidas. Por encima de 1024 las "
                                "sombras cuestan más tiempo de GPU y memoria de vídeo.",
    [UI_MENU_SHADOW_RES_GAME] = "Juego (variable)",
    [UI_MENU_ANISO] = "Filtrado de texturas",
    [UI_MENU_ANISO_DESC] = "Filtrado anisótropo de las texturas: el suelo, los caminos y las paredes vistos en ángulo "
                           "rasante siguen nítidos en la distancia en lugar de difuminarse. Juego: el nivel que pide "
                           "el juego, como en Switch.",
    [UI_MENU_ANISO_GAME] = "Juego",
    [UI_MENU_LOD_STEADY] = "Detalle estable con carga",
    [UI_MENU_LOD_STEADY_DESC] = "Omite el ajuste del juego que reduce el detalle de modelos y texturas cuando bajan "
                                "los fotogramas por segundo, como hace el mod de emulador Disable LOD Quality "
                                "Reduction. Versiones 1.1.0, 1.2.1 y 1.4.3 del juego. Déjalo desactivado en PC poco "
                                "potentes: ahí esa reducción mantiene los fotogramas por segundo.",
    [UI_MENU_SHARPNESS] = "Nitidez de FSR",
    [UI_MENU_SHARPNESS_DESC] = "Enfoque tras el escalado FSR; 0 lo desactiva.",
    [UI_MENU_COLOR] = "Color",
    [UI_MENU_COLOR_PRESET] = "Perfil de color",
    [UI_MENU_COLOR_PRESET_DESC] = "Fija los cuatro valores de abajo. Vivo: más contraste y saturación, sombras algo "
                                  "más abiertas (pensado para pantallas OLED). Claridad: algo más de contraste, "
                                  "saturación y brillo. Cambiar un valor pasa el perfil a Personalizado. El HUD "
                                  "cambia con la imagen; este menú no.",
    [UI_MENU_COLOR_DEFAULT] = "El del juego",
    [UI_MENU_COLOR_VIVID] = "Vivo (OLED)",
    [UI_MENU_COLOR_CLARITY] = "Claridad",
    [UI_MENU_COLOR_CUSTOM] = "Personalizado",
    [UI_MENU_COLOR_BRIGHTNESS] = "Brillo",
    [UI_MENU_COLOR_BRIGHTNESS_DESC] = "Se suma a cada color; 0 lo deja igual.",
    [UI_MENU_COLOR_CONTRAST] = "Contraste",
    [UI_MENU_COLOR_CONTRAST_DESC] = "Aleja los colores del gris medio (por encima del 100 %) o los acerca; 100 % los "
                                    "deja igual.",
    [UI_MENU_COLOR_SATURATION] = "Saturación",
    [UI_MENU_COLOR_SATURATION_DESC] = "Intensidad del color frente al gris; 100 % la deja igual.",
    [UI_MENU_COLOR_GAMMA] = "Gamma",
    [UI_MENU_COLOR_GAMMA_DESC] = "Por debajo de 1 aclara los tonos oscuros y medios, por encima de 1 los oscurece; 1 "
                                 "los deja igual.",
    [UI_MENU_HDR_GROUP] = "HDR",
    [UI_MENU_HDR] = "Salida HDR",
    [UI_MENU_HDR_DESC] = "Luces brillantes en una pantalla HDR. Requiere el HDR activado en Windows («Usar HDR») o un "
                         "compositor Wayland con gestión del color; sin él la imagen sigue en SDR. Se aplica tras "
                         "reiniciar. El perfil de color de arriba es para SDR y no se usa en HDR.",
    [UI_MENU_HDR_PEAK] = "Brillo máximo",
    [UI_MENU_HDR_PEAK_DESC] = "El pico de tu pantalla en nits; las luces más brillantes se detienen ahí.",
    [UI_MENU_HDR_GAME] = "Brillo del juego",
    [UI_MENU_HDR_GAME_DESC] = "Nits del blanco de la imagen (blanco papel). 203 es la referencia HDR; súbelo en una "
                              "habitación luminosa.",
    [UI_MENU_HDR_UI] = "Brillo del HUD y del menú",
    [UI_MENU_HDR_UI_DESC] = "Nits del blanco del HUD y de este menú.",
    [UI_MENU_HDR_HUE] = "Corrección del tono de luces",
    [UI_MENU_HDR_HUE_DESC] = "Los colores brillantes, como el fuego y las chispas, tienden al blanco como en la "
                             "propia imagen SDR del juego, coincidiendo con la luz que emiten; 0 los mantiene "
                             "totalmente saturados.",
    [UI_MENU_SHADOW_SCALE] = "Resolución de sombras",
    [UI_MENU_SHADOW_SCALE_DESC] = "Dibuja los mapas de sombras del sol tantas veces más grandes por lado que los "
                                  "960x960 del juego: bordes de sombra y sombras de hojas más nítidos. Se aplica en "
                                  "el próximo inicio.",
    [UI_MENU_PCSS] = "Sombras suaves",
    [UI_MENU_PCSS_DESC] = "Sombras del sol nítidas donde un objeto toca el suelo y más suaves cuanto más lejos cae la "
                          "sombra (PCSS). 0 mantiene el filtro del juego. Se aplica en el próximo inicio.",
    [UI_MENU_CONTACT_SHADOWS] = "Sombras de contacto",
    [UI_MENU_CONTACT_SHADOWS_DESC] = "Pequeñas sombras del sol que los mapas de sombras no captan: en los pies, bajo "
                                     "las piedras, en la hierba y en los pliegues. Calculadas en pantalla tras la "
                                     "iluminación. Se aplica en el próximo inicio.",
    [UI_MENU_SHADOW_RANGE] = "Alcance de las cascadas de sombras",
    [UI_MENU_SHADOW_RANGE_DESC] = "Aleja los escalones donde las sombras se vuelven más suaves (20, 40, 80 y 160 m en "
                                  "el juego). Reparte el mapa de sombras sobre más terreno: sube también la "
                                  "resolución de sombras. Se aplica en el próximo inicio.",
    [UI_MENU_GRASS_RANGE] = "Alcance de hierba",
    [UI_MENU_GRASS_RANGE_DESC] = "Hasta qué distancia alrededor de la cámara crece la hierba (en el juego: 62,5 m). "
                                 "Más mantiene visible la hierba de los prados más lejos en lugar de hacerla crecer "
                                 "justo delante de ti; menos dibuja menos hierba. Se aplica en el próximo inicio.",
    [UI_MENU_DLSS_PRESET] = "Modelo DLSS",
    [UI_MENU_DLSS_PRESET_DESC] = "Qué modelo de DLSS se usa. Automático sigue el consejo de NVIDIA para cada modo: K "
                                 "para DLAA, Calidad y Equilibrado, M para Rendimiento, L para Ultrarrendimiento. J: "
                                 "algo menos de estelas tras los personajes en movimiento y algo más de parpadeo. K: "
                                 "el modelo más nítido en todos los modos. Se aplica al momento.",
    [UI_MENU_DLSS_PRESET_DEFAULT] = "Automático",
    [UI_MENU_DLSS_PRESET_K] = "K (más nítido)",
    [UI_MENU_DLSS_PRESET_J] = "J (menos estelas)",
    [UI_MENU_DLSS_CHARS] = "Estelas de personajes en DLSS",
    [UI_MENU_DLSS_CHARS_DESC] = "Reduce las leves estelas tras los personajes y animales en movimiento y lo que "
                                "llevan: ahí DLSS toma menos de los fotogramas anteriores. Más fuerte elimina más "
                                "estelas y deja esos bordes algo menos suaves. Se aplica al momento.",
    [UI_MENU_DLSS_CHARS_OFF] = "No",
    [UI_MENU_DLSS_CHARS_LIGHT] = "Leve",
    [UI_MENU_DLSS_CHARS_MEDIUM] = "Medio",
    [UI_MENU_DLSS_CHARS_FULL] = "Completo",
    [UI_MENU_DLSS] = "Modo de escalado",
    [UI_MENU_DLSS_DESC] = "Nativo suaviza los bordes de la escena 3D a resolución completa. Calidad, Equilibrado, "
                          "Rendimiento y Ultrarrendimiento la dibujan al 67, 58, 50 y 33 % y dejan que el escalador "
                          "(DLSS o FSR 3.1) la amplíe, lo que ahorra tiempo de GPU; sustituyen a la escala de "
                          "renderizado 3D. Se aplica al momento.",
    [UI_MENU_DLSS_DLAA] = "Nativo",
    [UI_MENU_DLSS_QUALITY] = "Calidad",
    [UI_MENU_DLSS_BALANCED] = "Equilibrado",
    [UI_MENU_DLSS_PERFORMANCE] = "Rendimiento",
    [UI_MENU_DLSS_ULTRA] = "Ultrarrendimiento",
    [UI_MENU_FXAA] = "Antialiasing del juego (FXAA)",
    [UI_MENU_FXAA_DESC] = "El suavizado de bordes FXAA del propio juego. Difumina un poco los detalles finos; con "
                          "DLAA no hace falta.",
    [UI_MENU_REFLEX] = "Baja latencia",
    [UI_MENU_REFLEX_DESC] = "Reduce la latencia del sistema. Sí usa NVIDIA Reflex (VK_NV_low_latency2); Sí + Boost mantiene además altas las frecuencias de la GPU NVIDIA en escenas limitadas por la CPU. Automático usa Reflex en NVIDIA y AMD Anti-Lag 2 (VK_AMD_anti_lag) en AMD. Se aplica tras reiniciar.",
    [UI_MENU_REFLEX_AUTO] = "Automático",
    [UI_MENU_REFLEX_OFF] = "No",
    [UI_MENU_REFLEX_ON] = "Sí",
    [UI_MENU_REFLEX_BOOST] = "Sí + Boost",
    [UI_MENU_SCENE_UPSCALER] = "Escalador de escena 3D",
    [UI_MENU_SCENE_UPSCALER_DESC] = "Un escalador temporal para la escena 3D: bordes más suaves y menos centelleo, y "
                                    "en un modo por debajo de Nativo dibuja la escena más pequeña y la amplía, lo que "
                                    "ahorra tiempo de GPU. NVIDIA DLSS necesita una GPU NVIDIA RTX y " DLSS_FILE " "
                                    "junto a " EXE_FILE "; AMD FSR 3.1 funciona en cualquier GPU. No: mantiene el "
                                    "escalado y el FXAA propios del juego. El HUD no se toca; el escalado y la "
                                    "nitidez de arriba siguen ajustando la imagen del juego a la ventana. Se aplica "
                                    "tras reiniciar.",
    [UI_MENU_SCENE_UPSCALER_OFF] = "No",
    [UI_MENU_SCENE_UPSCALER_DLSS] = "NVIDIA DLSS",
    [UI_MENU_SCENE_UPSCALER_FSR3] = "AMD FSR 3.1",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_GPU] = "DLSS: necesita una GPU NVIDIA RTX",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_DLL] = "DLSS: falta " DLSS_FILE,
    [UI_MENU_FSR_SHARPNESS] = "Nitidez de FSR",
    [UI_MENU_FSR_SHARPNESS_DESC] = "Nitidez de la escena 3D tras AMD FSR 3.1 o FSR 4; 0 la desactiva.",
    [UI_MENU_SCENE_UPSCALER_FSR4] = "AMD FSR 4",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_GPU] = "FSR 4: no disponible para esta GPU",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_DLL] = "FSR 4: faltan las bibliotecas FSR de AMD",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_OS] = "FSR 4: solo en Windows",
    [UI_MENU_FPS_LIMIT] = "Límite de fotogramas",
    [UI_MENU_FPS_LIMIT_DESC] = "El máximo de fotogramas por segundo que dibuja el juego. Por encima de 60 es "
                               "experimental y requiere «Velocidad del juego según el reloj».",
    [UI_MENU_FPS_30] = "30 FPS",
    [UI_MENU_FPS_40] = "40 FPS",
    [UI_MENU_FPS_45] = "45 FPS",
    [UI_MENU_FPS_60] = "60 FPS",
    [UI_MENU_FPS_90] = "90 FPS",
    [UI_MENU_FPS_120] = "120 FPS",
    [UI_MENU_FPS_144] = "144 FPS",
    [UI_MENU_FPS_DISPLAY] = "Frecuencia del monitor",
    [UI_MENU_FPS_DISPLAY_HZ] = "Frecuencia del monitor (%.0f Hz)",
    [UI_MENU_FRAME_QUEUE] = "Cola de fotogramas",
    [UI_MENU_FRAME_QUEUE_DESC] = "El juego empieza un fotograma mientras el anterior espera su turno en pantalla: "
                                 "muchos menos fotogramas perdidos en el límite, hasta un fotograma más de latencia "
                                 "de los controles. Se aplica al momento.",
    [UI_MENU_FRAME_GEN] = "Generación de fotogramas",
    [UI_MENU_FRAME_GEN_OFF] = "No",
    [UI_MENU_FRAME_GEN_FSR3] = "AMD FSR 3",
    [UI_MENU_FRAME_GEN_DESC] = "Una imagen generada entre cada dos que dibuja el juego: el doble de imágenes en "
                               "pantalla, el juego a la mitad del límite de fotogramas y los controles, alrededor de "
                               "medio fotograma más tarde. Para pantallas de 120 Hz o más. No funciona con DLSS ni "
                               "HDR. Se aplica al momento.",
    [UI_MENU_REALTIME] = "Velocidad del juego según el reloj",
    [UI_MENU_REALTIME_DESC] = "Avanza el juego según el tiempo real, para que un fotograma lento no ralentice el "
                              "juego.",
    [UI_MENU_CLOTH_REALTIME] = "Telas según el reloj",
    [UI_MENU_CLOTH_REALTIME_DESC] = "Avanza también las telas y el pelo según el tiempo real.",
    [UI_MENU_HIGHFPS_FIX] = "Correcciones para muchos FPS",
    [UI_MENU_HIGHFPS_FIX_DESC] = "Corrige la física que falla por encima de 30 fotogramas por segundo y los objetos "
                                 "que se quedan transparentes o invisibles por encima de 60.",
    [UI_MENU_CLIMB_FIX] = "Corrección de escalada",
    [UI_MENU_CLIMB_FIX_DESC] = "Mantiene la escalada por paredes y escaleras a velocidad completa por encima de 60 "
                               "fotogramas por segundo, para que Link llegue arriba.",
    [UI_MENU_GAME_LANG] = "Idioma del juego",
    [UI_MENU_GAME_LANG_DESC] = "Idioma de los textos y las voces del juego.",
    [UI_MENU_GAME_LANG_AUTO] = "Automático",
    [UI_MENU_OPERATION_MODE] = "Modo de la consola",
    [UI_MENU_OPERATION_MODE_DESC] = "Si el juego cree que funciona en modo TV o en modo portátil.",
    [UI_MENU_MODE_DOCKED] = "Modo TV",
    [UI_MENU_MODE_HANDHELD] = "Modo portátil",
    [UI_MENU_ASSUME_FOCUS] = "Seguir en segundo plano",
    [UI_MENU_ASSUME_FOCUS_DESC] = "Sí (predeterminado): el juego sigue mientras estás en otra ventana, y el teclado "
                                  "no responde hasta que la ventana vuelve al frente. No: se pausa cuando su ventana "
                                  "pierde el foco, como en Switch al pulsar Home; una carga interrumpida así puede "
                                  "quedarse bloqueada.",
    [UI_MENU_PLAY_TIME] = "Tiempo de juego",
    [UI_MENU_PLAY_TIME_DESC] = "Tiempo en el juego en todas las sesiones, contado mientras la ventana del juego tiene "
                               "el foco y el juego dibuja fotogramas. Se guarda en playtime.txt en la carpeta del "
                               "perfil, junto a las partidas guardadas.",
    [UI_MENU_PLAY_TIME_HM] = "%llu h %02u min",
    [UI_MENU_PLAY_TIME_SESSION] = "esta sesión: %s",
    [UI_MENU_MODS_BUILTIN] = "Incluidos en el port",
    [UI_MENU_MODS_INSTALLED] = "Mods instalados",
    [UI_MENU_MODS_NONE] = "Aún no hay mods. Descomprime un mod en la carpeta mods, una carpeta por mod con su carpeta "
                          "romfs dentro, y reinicia el juego. TKMM también puede exportar ahí sus mods combinados en "
                          "una carpeta.",
    [UI_MENU_MODS_OPEN] = "Abrir carpeta de mods",
    [UI_MENU_MODS_OPEN_DESC] = "Muestra la carpeta de la que el juego carga los mods. Se crea si falta.",
    [UI_MENU_MODS_ROW_DESC] = "Archivos del juego que sustituye: %u. Archivos nuevos que añade: %u.",
    [UI_MENU_MODS_OVERRIDDEN] = "%u de sus archivos vienen de %s, que está más arriba en la lista.",
    [UI_MENU_MODS_MOVE_HINT] = "El botón superior o Ctrl+Arriba lo sube, el botón izquierdo o Ctrl+Abajo lo baja. "
                               "Cuando dos mods cambian el mismo archivo, gana el de más arriba. Los cambios se "
                               "aplican tras reiniciar.",
    [UI_MENU_MODS_EMPTY] = "Esta carpeta no tiene archivos romfs, exefs ni cheats.",
    [UI_MENU_MODS_SUBSDK] = "Este mod trae su propio código de juego (un plugin subsdk o un main sustituido). El port "
                            "no puede cargar módulos de código, así que todo el mod queda desactivado.",
    [UI_MENU_MODS_EXEFS_LATER] = "Este mod parchea el código del juego (exefs). El juego incorpora los parches en el "
                                 "próximo inicio (una reconstrucción de unos minutos); hasta entonces todo el mod "
                                 "queda desactivado.",
    [UI_MENU_MODS_CHEATS] = "Trucos: %d. Actívalos en la pestaña Trucos.",
    [UI_MENU_MODS_CHEATS_FILE] = "Los trucos para esta versión del juego se leen al iniciar desde cheats\\%s en este "
                                 "mod.",
    [UI_MENU_MODS_BAD_NAME] = "El nombre de la carpeta contiene |, que la lista de mods usa entre nombres. Cambia el "
                              "nombre de la carpeta.",
    [UI_MENU_MODS_OTHER_VERSION] = "%u de sus archivos son para otra versión del juego y se omiten. Busca una versión "
                                   "de este mod para tu versión del juego.",
    [UI_MENU_MODS_UNUSED] = "sin usar",
    [UI_MENU_MODS_FPS60] = "Parche de 60 FPS",
    [UI_MENU_MODS_FPS60_DESC] = "El parche propio del port para 60 fotogramas por segundo. Se ajusta con el límite de "
                                "fotogramas en la pestaña Juego.",
    [UI_MENU_MODS_RES] = "Parche de resolución",
    [UI_MENU_MODS_RES_DESC] = "El parche propio del port para resoluciones por encima de 1080p. Se ajusta con la "
                              "resolución del juego en la pestaña Pantalla.",
    [UI_MENU_MODS_QUICKMENU] = "Corrección del menú rápido",
    [UI_MENU_MODS_QUICKMENU_DESC] = "La corrección del port para el fondo negro de los menús de armas, escudos y "
                                    "materiales a 60 FPS.",
    [UI_MENU_VOLUME] = "Volumen",
    [UI_MENU_VOLUME_DESC] = "Volumen general del juego.",
    [UI_MENU_VOLUME_MUSIC] = "Volumen de música",
    [UI_MENU_VOLUME_MUSIC_DESC] = "Volumen de la música de fondo.",
    [UI_MENU_VOLUME_SFX] = "Volumen de efectos",
    [UI_MENU_VOLUME_SFX_DESC] = "Volumen de los efectos de sonido.",
    [UI_MENU_VOLUME_VOICE] = "Volumen de voces",
    [UI_MENU_VOLUME_VOICE_DESC] = "Volumen de las voces de los personajes.",
    [UI_MENU_AUDIO_CHANNELS] = "Altavoces",
    [UI_MENU_AUDIO_CHANNELS_DESC] = "Salida estéreo o envolvente 5.1. Automático elige 5.1 cuando la configuración de "
                                    "altavoces de Windows tiene seis canales o más. Se aplica tras reiniciar.",
    [UI_MENU_CHANNELS_AUTO] = "Automático",
    [UI_MENU_CHANNELS_STEREO] = "Estéreo",
    [UI_MENU_CHANNELS_51] = "Envolvente 5.1",
    [UI_MENU_AUDIO_DEVICE] = "Dispositivo de salida",
    [UI_MENU_AUDIO_DEVICE_DESC] = "Dónde suena el sonido. Predeterminado del sistema sigue al dispositivo "
                                  "predeterminado de Windows. Si el dispositivo elegido se desconecta, se usa el "
                                  "predeterminado del sistema hasta que vuelva. Se aplica al momento.",
    [UI_MENU_AUDIO_DEVICE_DEFAULT] = "Predeterminado del sistema",
    [UI_MENU_MOTION] = "Control por movimiento",
    [UI_MENU_MOTION_DESC] = "Apuntar con el giroscopio del mando.",
    [UI_MENU_GYRO_CALIBRATION] = "Calibración del giroscopio",
    [UI_MENU_GYRO_CALIBRATION_DESC] = "Resta la deriva del mando en reposo, medida con la fila de calibración de "
                                      "abajo. Los valores medidos siguen guardados cuando esto está desactivado.",
    [UI_MENU_GYRO_CALIBRATE] = "Calibrar giroscopio",
    [UI_MENU_GYRO_CALIBRATE_DESC] = "Mide la deriva del giroscopio en reposo y la elimina. Deja el mando apoyado o "
                                    "mantenlo quieto durante dos segundos.",
    [UI_MENU_GYRO_CALIBRATE_WAIT] = "Calibrando...",
    [UI_MENU_GYRO_CALIBRATED] = "%s, calibrado",
    [UI_MENU_GYRO_CALIBRATE_FAILED] = "Calibración fallida",
    [UI_MENU_GYRO_CALIBRATE_MOVED] = "El mando se movió: mantenlo quieto y vuelve a intentarlo",
    [UI_MENU_GYRO_AUTO_BIAS] = "Aprender la deriva en reposo",
    [UI_MENU_GYRO_AUTO_BIAS_DESC] = "Aprende la deriva que queda en el giroscopio cada vez que el mando está quieto "
                                    "sobre una mesa, y la resta. Para una mira del arco que se desliza lentamente "
                                    "incluso tras calibrar. No aprende de un mando sostenido en la mano. Experimental.",
    [UI_MENU_RUMBLE_STRENGTH] = "Intensidad de la vibración",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "Con qué fuerza vibra el mando.",
    [UI_MENU_RUMBLE_CURVE] = "Curva de vibración",
    [UI_MENU_RUMBLE_CURVE_DESC] = "Los valores bajos refuerzan la vibración débil.",
    [UI_MENU_RUMBLE_TRIGGERS] = "Vibración de gatillos (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Hace vibrar también los gatillos de un mando Xbox.",
    [UI_MENU_PAD_LAYOUT] = "Distribución de botones",
    [UI_MENU_PAD_LAYOUT_DESC] = "Qué botón es A. Nintendo: el derecho, como en Switch. Xbox: el inferior, como está "
                                "rotulado en un mando Xbox (cruz en un mando PlayStation). Automático elige Nintendo "
                                "para mandos con rótulos de Nintendo y Xbox para los demás. El mando que muestran los "
                                "avisos del juego se elige en Iconos de botones.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Automático",
    [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A a la derecha)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A abajo)",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "en uso: %s",
    [UI_MENU_PROMPTS] = "Iconos de botones",
    [UI_MENU_PROMPTS_DESC] = "De qué mando son los botones que muestran los avisos del juego. Nintendo: los del "
                             "propio juego. Xbox: LB, RB, LT, RT, Menu, View y una cruceta, y A, B, X e Y según el "
                             "rótulo del botón que pulsa la distribución de arriba (con Nintendo (A a la derecha) el "
                             "botón derecho está rotulado B, así que los avisos de A muestran B). PlayStation: las "
                             "formas de un mando PlayStation (○ × △ □, L1, R1, L2, R2, Options y Share), en el botón "
                             "que pulsa la distribución de arriba. PC: las teclas y botones del ratón de la "
                             "distribución predeterminada del teclado (E, Mayús, Espacio, clic izquierdo...) y el "
                             "ratón para la cámara. El juego lee sus avisos al iniciar, así que cambian tras "
                             "reiniciar. Forman parte de los archivos del juego, así que un botón que reasignes en "
                             "Controles mantiene su aviso anterior en pantalla.",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo",
    [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation",
    [UI_MENU_PROMPTS_PC] = "PC",
    [UI_MENU_PROMPTS_MISSING] = "faltan archivos de iconos: vuelve a ejecutar el instalador",
    [UI_MENU_PROMPTS_FAILED] = "archivos de iconos dañados: vuelve a ejecutar el instalador",
    [UI_MENU_PROMPTS_RESTART] = "los iconos cambian tras reiniciar",
    [UI_MENU_PROMPTS_OFF_SHAPE] = "desactivado con una resolución del juego que no es 16:9",
    [UI_MENU_OUTPUT_RES_NO_LAYOUT] = "no hay distribución para esta forma: vuelve a ejecutar el instalador",
    [UI_MENU_OUTPUT_RES_PREPARING] = "distribución creándose en segundo plano; se usará en el próximo inicio",
    [UI_MENU_PROMPTS_PREPARING] = "archivos de iconos creándose en segundo plano; se usarán en el próximo inicio",
    [UI_MENU_PREPARE_RETRY] = "Reintentar",
    [UI_MENU_PREPARE_RETRY_DESC] = "Vuelve a iniciar las herramientas del instalador para crear en segundo plano la "
                                   "distribución de la interfaz o los archivos de iconos de botones que faltan. El "
                                   "juego sigue funcionando; los usará desde su próximo inicio.",
    [UI_MENU_SHAPE_PREPARE_FAILED] = "las herramientas del instalador no se iniciaron: vuelve a ejecutar el instalador",
    [UI_MENU_PROMPTS_OFF_MODS] = "desactivado: un mod RomFS sustituye sus archivos",
    [UI_MENU_PROMPTS_OFF_PATCHES] = "desactivado mientras TOTK_ROMFS_PATCHES esté definido",
    [UI_MENU_RESTART] = "Reiniciar ahora",
    [UI_MENU_RESTART_DESC] = "Cierra el juego y lo vuelve a iniciar con los nuevos iconos. Se pierde el progreso "
                             "desde tu último guardado.",
    [UI_MENU_RESTART_CONFIRM] = "¿Reiniciar el juego ahora? Se pierde el progreso desde tu último guardado.",
    [UI_MENU_RESTART_YES] = "Reiniciar",
    [UI_MENU_RESTART_SAVING] = "El juego está guardando: podrás reiniciar en un momento.",
    [UI_MENU_GROUP_STICKS] = "Sticks analógicos",
    [UI_MENU_LSTICK_DEADZONE] = "Zona muerta del stick izquierdo",
    [UI_MENU_RSTICK_DEADZONE] = "Zona muerta del stick derecho",
    [UI_MENU_STICK_DEADZONE_DESC] = "Cuánto debe moverse un stick antes de que el juego lo note. Súbela si el "
                                    "personaje o la cámara se mueven solos: suelta el stick, lee «stick ahora» y pon "
                                    "la zona muerta unos puntos por encima.",
    [UI_MENU_STICK_DEADZONE_NOW] = "stick ahora: %.0f %%",
    [UI_MENU_MOUSE_MODE] = "Cámara con ratón",
    [UI_MENU_MOUSE_MODE_DESC] = "Directo: la cámara gira según cuánto se mueve el ratón, como en un juego de PC, sin "
                                "zona muerta ni curva de stick. Emulación de stick: el ratón actúa como el stick derecho.",
    [UI_MENU_MOUSE_STICK] = "Emulación de stick",
    [UI_MENU_MOUSE_DIRECT] = "Directo",
    [UI_MENU_MENU_CURSOR] = "Ratón en los menús del juego",
    [UI_MENU_MENU_CURSOR_DESC] = "En los menús propios del juego (pantalla de título, inventario, tiendas, opciones "
                                 "de diálogo) aparece el puntero: se selecciona el elemento bajo él, el clic "
                                 "izquierdo confirma y el derecho vuelve atrás. Solo si lo último que se usó fue el "
                                 "ratón o una tecla, así que con un mando los menús no cambian (versión 1.4.3 del "
                                 "juego).",
    [UI_MENU_MAP_CLICK] = "Clic izquierdo en el mapa",
    [UI_MENU_MAP_CLICK_DESC] = "En el mapa de la tableta Sheikah, el botón izquierdo del ratón pulsa A en el cursor "
                               "del mapa en lugar del botón al que está asignado (predeterminado: Y). Requiere «Ratón "
                               "en los menús del juego» (versión 1.4.3 del juego).",
    [UI_MENU_DIALOGUE_CLICK] = "Clics en diálogos",
    [UI_MENU_DIALOGUE_CLICK_DESC] = "Mientras hay un cuadro de diálogo abierto, el clic izquierdo pulsa B (continúa "
                                    "el texto y salta su animación) y el clic derecho pulsa A, en lugar de los "
                                    "botones a los que están asignados (predeterminados: Y, que abre el registro de "
                                    "diálogos, y ZL). Las opciones de diálogo mantienen el puntero: el clic izquierdo "
                                    "elige (versión 1.4.3 del juego).",
    [UI_MENU_POINTER_IMAGE] = "Puntero del juego",
    [UI_MENU_POINTER_IMAGE_DESC] = "Mientras el cursor no está capturado (menús del juego, este menú, tras Esc), la "
                                   "ventana muestra su propia punta de flecha dorada en lugar de la flecha del "
                                   "sistema, con un tamaño según la escala de la pantalla.",
    [UI_MENU_QUICKSAVE] = "Guardado y carga rápidos",
    [UI_MENU_QUICKSAVE_DESC] = "Una tecla guarda la partida al momento y otra carga ese guardado (las dos filas de "
                               "abajo: F6 y F7 de forma predeterminada, como en Half-Life), sin ningún menú en "
                               "pantalla. Solo teclas del teclado y del ratón, solo la versión 1.4.3 del juego. El "
                               "guardado rápido pide al juego uno de sus propios autoguardados, así que sustituye al "
                               "autoguardado más antiguo (el guardado manual se conserva), guarda una copia como "
                               "guardado rápido (la carpeta quicksave del perfil) y muestra \"Partida guardada\" en "
                               "la parte superior de la imagen. La carga rápida mantiene la imagen en \"Cargando el "
                               "guardado rápido\" mientras recorre fuera de la vista la lista de carga del juego, y "
                               "el juego carga el guardado rápido (unos 5 s). Donde el juego no haría un autoguardado "
                               "por sí mismo (una cinemática, un diálogo), no se guarda nada y la línea lo indica.",
    [UI_MENU_QUICKSAVE_KEY] = "Tecla de guardado rápido",
    [UI_MENU_QUICKLOAD_KEY] = "Tecla de carga rápida",
    [UI_MENU_QUICKSAVE_KEY_DESC] = "La tecla que inicia el guardado rápido o la carga rápida; F6 y F7 de forma "
                                   "predeterminada, como en Half-Life. Selecciona la celda y pulsa cualquier tecla o "
                                   "botón del ratón; Supr deja esa acción sin tecla. Mientras el guardado rápido está "
                                   "activado, las dos teclas no hacen nada más (aparece una nota cuando una "
                                   "asignación también usa la tecla). totk.ini acepta cualquier nombre de tecla de "
                                   "las asignaciones (quicksave_key, quickload_key).",
    [UI_QS_SAVED] = "Partida guardada",
    [UI_QS_LOADING] = "Cargando el guardado rápido",
    [UI_QS_NOT_NOW] = "El guardado rápido no es posible ahora",
    [UI_QS_NONE] = "Aún no hay guardado rápido (la tecla de guardado rápido crea uno)",
    [UI_QS_FAILED] = "El guardado rápido falló: consulta totk.log",
    [UI_QS_LOAD_FAILED] = "La carga rápida falló: consulta totk.log",
    [UI_MENU_MOUSE_SENS] = "Sensibilidad de cámara con ratón",
    [UI_MENU_MOUSE_SENS_DESC] = "Lo rápido que el ratón gira la cámara.",
    [UI_MENU_WALK_KEY] = "Tecla de caminar",
    [UI_MENU_WALK_KEY_DESC] = "Mantenla pulsada para inclinar los sticks del teclado solo en parte, de modo que el "
                              "teclado camine en lugar de correr. La escala de caminar fija cuánto. El mando y el "
                              "ratón no se ven afectados.",
    [UI_MENU_WALK_KEY_TAKEN] = "también %s",
    [UI_MENU_WALK_SCALE] = "Escala de caminar",
    [UI_MENU_WALK_SCALE_DESC] = "La inclinación de los sticks del teclado mientras se mantiene la tecla de caminar. "
                                "La mitad es caminar, la inclinación completa correr.",
    [UI_MENU_FREECAM] = "Cámara libre",
    [UI_MENU_FREECAM_DESC] = "Permite que una combinación o una tecla de abajo separe la vista de Link y la haga "
                             "volar. Desactivada de forma predeterminada: así el código de cámara del juego funciona "
                             "tal como se publicó. Se aplica tras reiniciar. Versión 1.4.3 del juego.",
    [UI_MENU_FREECAM_OFF_NOTE] = "Activa «Cámara libre» arriba y reinicia",
    [UI_MENU_FREECAM_PAD] = "Botones de cámara libre",
    [UI_MENU_FREECAM_PAD_DESC] = "Botones del mando pulsados a la vez que pasan la vista por tres fases: se separa de "
                                 "Link y vuela, se queda donde se dejó mientras sigues jugando con Link, y vuelve. "
                                 "Mientras vuela, el juego no recibe entradas: el stick izquierdo vuela en la "
                                 "dirección de la vista y de lado, los gatillos arriba y abajo, el stick derecho "
                                 "gira, y cada pulsación de L o R divide a la mitad o duplica la velocidad. Con la "
                                 "vista fija, Link camina hacia donde apunta el stick izquierdo en la pantalla. "
                                 "Versión 1.4.3 del juego.",
    [UI_MENU_FREECAM_PAD_V143] = "Solo la versión 1.4.3 del juego",
    [UI_MENU_FREECAM_KEY] = "Tecla de cámara libre",
    [UI_MENU_FREECAM_KEY_DESC] = "Una tecla que pasa la vista por las mismas fases: se separa de Link y vuela, se "
                                 "queda quieta mientras sigues jugando con Link, y vuelve. Mientras vuela: W A S D "
                                 "vuelan en la dirección de la vista y de lado, las teclas asignadas a ZR y ZL (X y Z "
                                 "de forma predeterminada) arriba y abajo, el ratón y las flechas giran, y las teclas "
                                 "asignadas a L y R (Q y R) dividen a la mitad o duplican la velocidad. Versión 1.4.3 "
                                 "del juego.",
    [UI_MENU_GROUP_GAMEPLAY_TWEAKS] = "Jugabilidad",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED] = "Ajustes de jugabilidad",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED_DESC] = "Controles con la rueda del ratón para el giro y la distancia de la "
                                             "Ultramano, el cambio de objetos y armas, y un zoom suave del mapa.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE] = "Giro de Ultramano con rueda",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE_DESC] = "Gira directamente con la rueda del ratón los objetos sujetos con la "
                                               "Ultramano.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS] = "Ultramano: eje de giro con C",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS_DESC] = "Mantén C mientras usas la rueda para girar los objetos sujetos "
                                               "alrededor del eje alternativo.",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY] = "Sensibilidad de giro de Ultramano",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY_DESC] = "Lo rápido que la rueda del ratón gira los objetos sujetos.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE] = "Distancia de Ultramano con rueda",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE_DESC] = "Clic derecho + rueda del ratón acerca o aleja los objetos sujetos.",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY] = "Sensibilidad de distancia de Ultramano",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY_DESC] = "Lo rápido que la rueda del ratón cambia la distancia del objeto.",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL] = "Elegir arma con la rueda",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL_DESC] = "Usa la rueda del ratón mientras exploras libremente para cambiar de arma. "
                                           "Confirma con E.",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY] = "Sensibilidad de rueda de armas",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY_DESC] = "Pasos de la rueda necesarios para pasar a la siguiente arma.",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL] = "Rueda en el menú rápido",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL_DESC] = "Usa la rueda del ratón para recorrer los objetos mientras un menú de "
                                         "selección rápida está abierto.",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY] = "Sensibilidad de rueda del menú rápido",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY_DESC] = "Pasos de la rueda necesarios para avanzar entre los objetos de los "
                                               "menús de selección rápida.",
    [UI_MENU_GAMEPLAY_ITEM_AD] = "Navegación A / D en menú rápido",
    [UI_MENU_GAMEPLAY_ITEM_AD_DESC] = "Pulsa A / D para moverte a la izquierda y a la derecha en los menús de "
                                      "selección rápida.",
    [UI_MENU_GAMEPLAY_MAP_ZOOM] = "Zoom suave del mapa",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_DESC] = "Zoom suave con inercia en el mapa de la tableta Sheikah usando la rueda del "
                                       "ratón.",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY] = "Sensibilidad de zoom del mapa",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY_DESC] = "Lo rápido que la rueda del ratón amplía o reduce el mapa.",
    [UI_MENU_STICK_SENS] = "Velocidad de cámara con stick",
    [UI_MENU_STICK_SENS_DESC] = "Multiplica la velocidad de cámara del stick derecho del juego, velocidad máxima "
                                "incluida, en cada una de las cinco sensibilidades de cámara del juego. El ratón "
                                "conserva su propio ajuste.",
    [UI_BINDS_HELP] = "Selecciona una celda y pulsa A o Intro, y luego la tecla, el botón del ratón o el botón del "
                      "mando. Supr o X vacía una celda. Una tecla ya en uso se mueve aquí; un botón del mando se "
                      "intercambia con el que sustituye.",
    [UI_BINDS_COL_KEY1] = "Tecla 1",
    [UI_BINDS_COL_KEY2] = "Tecla 2",
    [UI_BINDS_COL_PAD] = "Mando",
    [UI_BINDS_GROUP_BUTTONS] = "Botones",
    [UI_BINDS_GROUP_MOVE] = "Movimiento",
    [UI_BINDS_GROUP_CAMERA] = "Cámara",
    [UI_BINDS_PRESS_KEY] = "Pulsa una tecla o un botón del ratón para %s",
    [UI_BINDS_PRESS_PAD] = "Pulsa un botón del mando para %s",
    [UI_BINDS_CANCEL_HINT] = "Esc cancela, o espera %d s",
    [UI_BINDS_TAKEN] = "%s se quitó de %s",
    [UI_BINDS_SWAPPED] = "%s intercambiado con %s",
    [UI_BINDS_RESET] = "Restablecer todos los botones",
    [UI_BINDS_RESET_DESC] = "Vuelve a las teclas y botones del mando predeterminados. La distribución de botones de "
                            "arriba vuelve a aplicarse: los botones del mando reasignados se quedan donde los pusiste "
                            "hasta este restablecimiento.",
    [UI_BIND_A] = "A",
    [UI_BIND_B] = "B",
    [UI_BIND_X] = "X",
    [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "Pulsar stick izquierdo",
    [UI_BIND_RCLICK] = "Pulsar stick derecho",
    [UI_BIND_L] = "L",
    [UI_BIND_R] = "R",
    [UI_BIND_ZL] = "ZL",
    [UI_BIND_ZR] = "ZR",
    [UI_BIND_PLUS] = "Más",
    [UI_BIND_MINUS] = "Menos",
    [UI_BIND_LEFT] = "Cruceta izquierda",
    [UI_BIND_UP] = "Cruceta arriba",
    [UI_BIND_RIGHT] = "Cruceta derecha",
    [UI_BIND_DOWN] = "Cruceta abajo",
    [UI_BIND_LSTICK_LEFT] = "Moverse a la izquierda",
    [UI_BIND_LSTICK_UP] = "Avanzar",
    [UI_BIND_LSTICK_RIGHT] = "Moverse a la derecha",
    [UI_BIND_LSTICK_DOWN] = "Retroceder",
    [UI_BIND_RSTICK_LEFT] = "Cámara a la izquierda",
    [UI_BIND_RSTICK_UP] = "Cámara arriba",
    [UI_BIND_RSTICK_RIGHT] = "Cámara a la derecha",
    [UI_BIND_RSTICK_DOWN] = "Cámara abajo",
    [UI_BIND_DESC_A] = "Interactuar, recoger, hablar.",
    [UI_BIND_DESC_B] = "Correr, cancelar, volver.",
    [UI_BIND_DESC_X] = "Saltar.",
    [UI_BIND_DESC_Y] = "Atacar.",
    [UI_BIND_DESC_LCLICK] = "Agacharse.",
    [UI_BIND_DESC_RCLICK] = "Vista en primera persona.",
    [UI_BIND_DESC_L] = "Habilidades.",
    [UI_BIND_DESC_R] = "Lanzar un arma.",
    [UI_BIND_DESC_ZL] = "Escudo, fijar objetivo.",
    [UI_BIND_DESC_ZR] = "Arco.",
    [UI_BIND_DESC_PLUS] = "Menú, inventario.",
    [UI_BIND_DESC_MINUS] = "Mapa.",
    [UI_BIND_DESC_DPAD] = "Selección rápida.",
    [UI_BIND_DESC_MOVE] = "Mueve a Link como el stick izquierdo. El stick izquierdo del mando también funciona "
                          "siempre.",
    [UI_BIND_DESC_CAMERA] = "Gira la cámara como el stick derecho. El ratón y el stick derecho del mando también "
                            "funcionan siempre.",
    [UI_BIND_SHAKE] = "Agitar el stick de cámara",
    [UI_BIND_DESC_SHAKE] = "Mueve el stick derecho a izquierda y derecha para el juego: separa una pieza unida de lo "
                           "que sujeta la Ultramano. Mantener pulsada la tecla sigue agitando.",
    [UI_KEY_SPACE] = "Espacio",
    [UI_KEY_TAB] = "Tab",
    [UI_KEY_LSHIFT] = "Mayús izq.",
    [UI_KEY_LCTRL] = "Ctrl izq.",
    [UI_KEY_UP] = "Flecha arriba",
    [UI_KEY_DOWN] = "Flecha abajo",
    [UI_KEY_LEFT] = "Flecha izquierda",
    [UI_KEY_RIGHT] = "Flecha derecha",
    [UI_KEY_MOUSE_L] = "Botón izquierdo",
    [UI_KEY_MOUSE_R] = "Botón derecho",
    [UI_KEY_MOUSE_M] = "Botón central",
    [UI_KEY_MOUSE_X1] = "Ratón 4",
    [UI_KEY_MOUSE_X2] = "Ratón 5",
    [UI_KEY_ENTER] = "Intro",
    [UI_KEY_BACKSPACE] = "Retroceso",
    [UI_KEY_RSHIFT] = "Mayús der.",
    [UI_KEY_RCTRL] = "Ctrl der.",
    [UI_KEY_LALT] = "Alt",
    [UI_KEY_CAPSLOCK] = "Bloq Mayús",
    [UI_KEY_INSERT] = "Insert",
    [UI_KEY_DELETE] = "Supr",
    [UI_KEY_HOME] = "Inicio",
    [UI_KEY_END] = "Fin",
    [UI_KEY_PAGEUP] = "Re Pág",
    [UI_KEY_PAGEDOWN] = "Av Pág",
    [UI_KEY_NUM_FMT] = "Num %s",
    [UI_PAD_SOUTH] = "Botón inferior",
    [UI_PAD_EAST] = "Botón derecho",
    [UI_PAD_WEST] = "Botón izquierdo",
    [UI_PAD_NORTH] = "Botón superior",
    [UI_PAD_LSTICK] = "Stick izquierdo",
    [UI_PAD_RSTICK] = "Stick derecho",
    [UI_PAD_L] = "Botón superior izquierdo",
    [UI_PAD_R] = "Botón superior derecho",
    [UI_PAD_START] = "Start",
    [UI_PAD_BACK] = "Atrás",
    [UI_PAD_DPAD_LEFT] = "Cruceta izquierda",
    [UI_PAD_DPAD_UP] = "Cruceta arriba",
    [UI_PAD_DPAD_RIGHT] = "Cruceta derecha",
    [UI_PAD_DPAD_DOWN] = "Cruceta abajo",
    [UI_PAD_ZL] = "Gatillo izquierdo",
    [UI_PAD_ZR] = "Gatillo derecho",
    [UI_PAD_LPADDLE1] = "Botón trasero izquierdo",
    [UI_PAD_RPADDLE1] = "Botón trasero derecho",
    [UI_PAD_LPADDLE2] = "Botón trasero izquierdo 2",
    [UI_PAD_RPADDLE2] = "Botón trasero derecho 2",
    [UI_PAD_TOUCHPAD] = "Pulsar panel táctil",
    [UI_PAD_CROSS] = "Cruz",
    [UI_PAD_CIRCLE] = "Círculo",
    [UI_PAD_SQUARE] = "Cuadrado",
    [UI_PAD_TRIANGLE] = "Triángulo",
    [UI_MENU_AMIIBO_DIR] = "Carpeta de amiibo",
    [UI_MENU_AMIIBO_OPEN] = "Abrir",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "Carpeta predeterminada",
    [UI_MENU_AMIIBO_REFRESH] = "Actualizar",
    [UI_MENU_AMIIBO_PICK] = "Volcados de amiibo (archivos .bin o .nfc de 540 bytes):",
    [UI_MENU_AMIIBO_NO_DIR] = "No se puede abrir la carpeta %s",
    [UI_MENU_AMIIBO_READER] = "Lector NFC",
    [UI_MENU_AMIIBO_STATE] = "Lector:",
    [UI_MENU_AMIIBO_STATE_IDLE] = "listo",
    [UI_MENU_AMIIBO_STATE_SEARCHING] = "el juego espera un amiibo",
    [UI_MENU_AMIIBO_STATE_FOUND] = "amiibo en el lector",
    [UI_MENU_AMIIBO_STATE_MOUNTED] = "el juego está leyendo el amiibo",
    [UI_MENU_AMIIBO_STATE_OFF] = "desactivado",
    [UI_MENU_AMIIBO_FILE] = "Archivo elegido:",
    [UI_MENU_AMIIBO_NO_FILE] = "(ningún archivo elegido)",
    [UI_MENU_AMIIBO_TAP] = "Acercar el amiibo",
    [UI_MENU_AMIIBO_REMOVE] = "Retirar el amiibo",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "Retirar el amiibo solo tras acercarlo",
    [UI_MENU_AMIIBO_HOLD] = "Tiempo en el lector",
    [UI_MENU_AMIIBO_SECONDS] = "%.1f s",
    [UI_MENU_AMIIBO_TAG] = "Amiibo en el lector:",
    [UI_MENU_AMIIBO_NO_TAG] = "No hay ningún amiibo en el lector.",
    [UI_MENU_AMIIBO_TAG_FILE] = "Archivo: %s",
    [UI_MENU_AMIIBO_TAG_ID] = "Personaje %s, serie %02x, número %04x, tipo %02x",
    [UI_MENU_AMIIBO_TAG_UID] = "UID: %s",
    [UI_MENU_AMIIBO_READY] = "Listo",
    [UI_MENU_AMIIBO_TAPPED] = "Acercado: %s",
    [UI_MENU_AMIIBO_LOAD_FAILED] = "No es un volcado de amiibo (NTAG215 de 540 bytes): %s",
    [UI_MENU_AMIIBO_REMOVED] = "Amiibo retirado",
    [UI_MENU_AMIIBO_STATUS] = "Estado: %s",
    [UI_MENU_MENU_LANG] = "Idioma del menú",
    [UI_MENU_MENU_LANG_DESC] = "Idioma de este menú y del indicador de rendimiento.",
    [UI_MENU_MENU_LANG_AUTO] = "Automático (según Windows)",
    [UI_MENU_DISCORD_RPC] = "Discord Rich Presence",
    [UI_MENU_DISCORD_RPC_DESC] = "Muestra \"Jugando a TotK Recomp\" con el tiempo de juego en tu estado de Discord "
                                 "mientras el juego está abierto. discord_app_id y discord_image en totk.ini eligen "
                                 "la aplicación.",
    [UI_MENU_ADV_WARNING] = "Estos ajustes son para pruebas y resolución de problemas. Cámbialos solo si sabes por "
                            "qué.",
    [UI_MENU_GROUP_SCHED] = "Planificación",
    [UI_MENU_GROUP_SHADERS] = "Shaders y pipelines",
    [UI_MENU_GROUP_TEXMEM] = "Texturas y memoria",
    [UI_MENU_GROUP_FIXES] = "Correcciones de renderizado",
    [UI_MENU_GROUP_VULKAN] = "Vulkan",
    [UI_MENU_GROUP_DIAG] = "Diagnóstico",
    [UI_MENU_ASYNC_PREEMPT] = "Apropiación de núcleos virtuales",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "Permite que un hilo del juego interrumpa a otro en el mismo núcleo virtual.",
    [UI_MENU_PREEMPT_OFF] = "No",
    [UI_MENU_PREEMPT_ON] = "Sí",
    [UI_MENU_PREEMPT_INHERIT] = "Solo herencia",
    [UI_MENU_REPLAY_ASYNC] = "Preparación de fotogramas GPU en su propio hilo",
    [UI_MENU_REPLAY_ASYNC_DESC] = "Prepara los fotogramas para la GPU en un hilo aparte en lugar del hilo del juego. "
                                  "Añade hasta un fotograma de latencia.",
    [UI_MENU_REPLAY_WORKERS] = "Hilos de preparación GPU",
    [UI_MENU_REPLAY_WORKERS_DESC] = "Registra los fotogramas grandes para la GPU en varios hilos a la vez. Ayuda "
                                    "cuando la CPU limita los fotogramas por segundo; requiere 8 procesadores lógicos "
                                    "o más. No: registra en un solo hilo.",
    [UI_MENU_REPLAY_WORKERS_NOW] = "ahora: %s",
    [UI_MENU_TIMER_1MS] = "Temporizador de Windows de 1 ms",
    [UI_MENU_TIMER_1MS_DESC] = "Sube la resolución del temporizador del sistema a 1 ms para un ritmo de fotogramas "
                               "más estable.",
    [UI_MENU_SYNC_AHEAD] = "Un fotograma por delante de la GPU",
    [UI_MENU_SYNC_AHEAD_DESC] = "El juego prepara el siguiente fotograma mientras la tarjeta gráfica aún dibuja el anterior. Más fotogramas por segundo, un fotograma más de retraso de entrada.",
    [UI_MENU_HOST_CHECKPOINT] = "Cesión rápida del núcleo",
    [UI_MENU_HOST_CHECKPOINT_DESC] = "Un hilo del juego cede su núcleo virtual a uno más importante en cuanto sale del código del port, no en su siguiente llamada al sistema.",
    [UI_MENU_PIPE_WAIT] = "Gestión de draws en espera",
    [UI_MENU_PIPE_WAIT_DESC] = "Predeterminado mantiene la planificación actual con un worker reservado para draws en "
                               "espera; Antiguo (draw) usa la planificación anterior.",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "Predeterminado",
    [UI_MENU_PIPE_WAIT_DRAW] = "Antiguo (draw)",
    [UI_MENU_PIPE_SKIP] = "Draws omitidos mientras se crea un pipeline",
    [UI_MENU_PIPE_SKIP_DESC] = "Qué draws se descartan en lugar de esperar a su pipeline.",
    [UI_MENU_PIPE_SKIP_STRICT] = "Estricto",
    [UI_MENU_PIPE_SKIP_ANY] = "Cualquiera",
    [UI_MENU_PIPE_SKIP_FULLSCREEN] = "Pasadas a pantalla completa",
    [UI_MENU_LIB_PREBUILD] = "Precompilación de bibliotecas de etapas",
    [UI_MENU_LIB_PREBUILD_DESC] = "Compila de antemano en segundo plano las bibliotecas de etapas de shaders.",
    [UI_MENU_LIB_PREBUILD_THREADS] = "Hilos de precompilación de bibliotecas",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "Hilos usados para la precompilación; Automático elige un número según tu "
                                          "CPU.",
    [UI_MENU_PIPE_PREBUILD] = "Precompilación de pipelines",
    [UI_MENU_PIPE_PREBUILD_DESC] = "Compila de antemano los pipelines conocidos.",
    [UI_MENU_PREWARM_XLATE] = "Precalentamiento del traductor",
    [UI_MENU_PREWARM_XLATE_DESC] = "Hilos que traducen los shaders conocidos al iniciar; Automático usa la mitad de "
                                   "los hilos de la CPU, 16 como máximo.",
    [UI_MENU_GPL] = "Biblioteca de pipelines gráficos",
    [UI_MENU_GPL_DESC] = "Usa la extensión de bibliotecas de pipelines de Vulkan; desactívala si tu controlador falla "
                         "con ella.",
    [UI_MENU_ASTC] = "Descodificación de texturas ASTC",
    [UI_MENU_ASTC_DESC] = "Dónde se descodifican las texturas comprimidas ASTC.",
    [UI_MENU_ASTC_AUTO] = "Automático",
    [UI_MENU_ASTC_GPU] = "Shader de GPU",
    [UI_MENU_ASTC_NATIVE] = "Nativo",
    [UI_MENU_ASTC_CPU] = "CPU",
    [UI_MENU_MEMORY] = "Modo de memoria GPU",
    [UI_MENU_MEMORY_DESC] = "Cómo se comparte la memoria del juego con la GPU.",
    [UI_MENU_MEMORY_AUTO] = "Automático",
    [UI_MENU_MEMORY_IMPORT] = "Importar",
    [UI_MENU_MEMORY_MIRROR] = "Espejo",
    [UI_MENU_MEMORY_VERIFY] = "Verificar",
    [UI_MENU_TEX_ALIAS] = "Alias de texturas",
    [UI_MENU_TEX_ALIAS_DESC] = "Una textura nueva creada sobre la memoria de una textura viva empieza con ese "
                               "contenido en lugar de vacía.",
    [UI_MENU_TEX_VRAM_PRESSURE] = "Límite de memoria de texturas",
    [UI_MENU_TEX_VRAM_PRESSURE_DESC] = "Porcentaje de memoria de vídeo por encima del cual se descartan las texturas "
                                       "que el juego liberó (vuelven del almacenamiento cuando se usan de nuevo). 0 "
                                       "las conserva.",
    [UI_MENU_ASTC_BC] = "Comprimir ASTC a BC3",
    [UI_MENU_ASTC_BC_DESC] = "Con el descodificador de GPU: recodifica las texturas ASTC descodificadas a BC3, una "
                             "cuarta parte de la memoria de vídeo. Para GPU de 4 GB o menos; las texturas se ven algo "
                             "más pixeladas.",
    [UI_MENU_LOD_GUARD] = "Protección de LOD",
    [UI_MENU_LOD_GUARD_DESC] = "Evita que los objetos parpadeen en el lugar equivocado o que la lejanía se vea plana "
                               "durante un fotograma, esperando a que el juego termine de reescribir los datos de "
                               "nivel de detalle antes de que la GPU los lea.",
    [UI_MENU_FIX_C0] = "Corrección del búfer de constantes",
    [UI_MENU_FIX_C0_DESC] = "Rellena el banco de constantes c0 reservado al controlador (tamaños de almacenamiento, "
                            "tamaño de dispatch, tamaño de punto, instancia base) en lugar de leer ceros.",
    [UI_MENU_FIX_ZERO] = "Poner a cero la memoria GPU nueva",
    [UI_MENU_FIX_ZERO_DESC] = "Los nuevos bloques de memoria GPU empiezan llenos de ceros, como la memoria de Switch.",
    [UI_MENU_DLAA_EFFECTS_AFTER] = "Partículas tras DLAA",
    [UI_MENU_DLAA_EFFECTS_AFTER_DESC] = "Con DLAA, el polvo y otras partículas se dibujan sobre la imagen que DLSS ya "
                                        "ha suavizado, como las dibuja el juego sin DLSS: las partículas pequeñas en "
                                        "la niebla (santuarios) ya no desaparecen ni dejan estelas. La niebla y las "
                                        "nubes siguen pasando por DLSS, así que los bordes lejanos contra el cielo "
                                        "quedan limpios. No: DLSS también procesa las partículas, como antes. Solo en "
                                        "el modo DLAA; los modos DLSS con escalado siempre los procesan. Se aplica al "
                                        "momento.",
    [UI_MENU_DLSS_FIXED_EXPOSURE] = "Exposición fija de DLSS",
    [UI_MENU_DLSS_FIXED_EXPOSURE_DESC] = "DLSS toma el brillo de la imagen del juego tal cual en lugar de estimar la "
                                         "exposición en cada fotograma: en escenas oscuras (el Subsuelo) las grandes "
                                         "zonas con niebla ya no se oscurecen ni se aclaran durante un fotograma "
                                         "mientras se mueve la cámara. No: DLSS estima la exposición por sí mismo, "
                                         "como antes. Se aplica al momento.",
    [UI_MENU_BIND_SKIP] = "Omitir enlaces repetidos",
    [UI_MENU_BIND_SKIP_DESC] = "Optimización: omite los enlaces que no cambiaron.",
    [UI_MENU_SHADER_MEMO] = "Memoria de shaders",
    [UI_MENU_SHADER_MEMO_DESC] = "Optimización: recuerda las búsquedas de shaders.",
    [UI_MENU_PIPE_MEMO] = "Memoria de pipelines",
    [UI_MENU_PIPE_MEMO_DESC] = "Optimización: reutiliza el pipeline del draw anterior cuando su estado se repite.",
    [UI_MENU_TEX_MEMO] = "Memoria de enlaces de texturas",
    [UI_MENU_TEX_MEMO_DESC] = "Optimización: recuerda los descriptores de texturas entre draws.",
    [UI_MENU_DESC_TEMPLATE] = "Plantillas de descriptores",
    [UI_MENU_DESC_TEMPLATE_DESC] = "Optimización: actualiza los descriptores en bloque.",
    [UI_MENU_PUSH_SKIP] = "Omitir push constants repetidas",
    [UI_MENU_PUSH_SKIP_DESC] = "Optimización: omite las push constants que no cambiaron.",
    [UI_MENU_SPLIT_DRAWS] = "Envío anticipado a la GPU",
    [UI_MENU_SPLIT_DRAWS_DESC] = "Envía el trabajo de GPU por partes cada N draws entre pasadas para que la GPU "
                                 "empiece antes; 0 lo desactiva.",
    [UI_MENU_VK_VALIDATION] = "Capa de validación",
    [UI_MENU_VK_VALIDATION_DESC] = "Comprueba cada llamada a Vulkan y ralentiza mucho el juego.",
    [UI_MENU_VK_VALIDATION_MISSING] = "La capa de validación no está instalada.",
    [UI_MENU_VK_DYN_VERTEX] = "Estado de vértices dinámico",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "Fuerza la entrada de vértices dinámica activada o desactivada; el valor "
                                   "predeterminado depende de la GPU.",
    [UI_MENU_VK_DYN_A2C] = "Alpha to coverage dinámico",
    [UI_MENU_VK_DYN_A2C_DESC] = "Fuerza el alpha to coverage dinámico activado o desactivado. Predeterminado: "
                                "desactivado con el controlador de AMD para RDNA 2 y anteriores, que con él dibuja a "
                                "los personajes en negro; activado en los demás.",
    [UI_MENU_VK_SUBGROUP_PIN] = "Fijar tamaño de subgrupo",
    [UI_MENU_VK_SUBGROUP_PIN_DESC] = "Fija el tamaño de subgrupo de los shaders.",
    [UI_MENU_TRACE] = "Registro de rendimiento",
    [UI_MENU_TRACE_DESC] = "Escribe los tiempos de fotograma en un archivo.",
    [UI_MENU_FLIGHT] = "Caja negra",
    [UI_MENU_FLIGHT_DESC] = "Guarda un historial corto y lo escribe cuando un fotograma se congela.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "Umbral de congelación (ms)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "Tiempo de fotograma que cuenta como congelación.",
    [UI_MENU_PASS_CRUMBS] = "Marcas de pasadas GPU",
    [UI_MENU_PASS_CRUMBS_DESC] = "Tras un fallo de la GPU, el registro indica la pasada de renderizado que se estaba "
                                 "ejecutando. Automático: solo AMD. Sí: también NVIDIA, con un pequeño coste.",
    [UI_MENU_INPUT_RECORD] = "Grabación de entradas",
    [UI_MENU_INPUT_RECORD_DESC] = "Escribe tus entradas en un archivo para informes de errores.",
    [UI_MENU_OPEN_LOG] = "Abrir carpeta de registros",
    [UI_MENU_OPEN_LOG_DESC] = "Muestra la carpeta con los archivos de registro.",
    [UI_MENU_COPY_INFO] = "Copiar información del sistema",
    [UI_MENU_COPY_INFO_DESC] = "Copia la GPU, el controlador, la CPU, la RAM y los ajustes cambiados al portapapeles.",
    [UI_MENU_COPY_INFO_DONE] = "Copiado",
    [UI_MENU_ACTION_FAILED] = "no funcionó",
    [UI_MENU_RESET] = "Restablecer todos los ajustes",
    [UI_MENU_RESET_DESC] = "Se aplica a todas las pestañas.",
    [UI_MENU_RESET_CONFIRM] = "¿Restablecer todos los ajustes a sus valores predeterminados?",
    [UI_MENU_RESET_YES] = "Restablecer",
    [UI_MENU_RESET_NO] = "Cancelar",
    [UI_MENU_OV_FPS] = "%.0f FPS",
    [UI_MENU_OV_FRAME_MS] = "%.1f ms",
    [UI_MENU_OV_GAME_PCT] = "juego %.0f%%",
    [UI_MENU_OV_MAX_MS] = "máx. %.1f ms",
    [UI_MENU_OV_FPS_LABEL] = "FPS",
    [UI_MENU_OV_FRAME_LABEL] = "Fotograma",
    [UI_MENU_OV_FMT_FPS] = "%.0f",
    [UI_MENU_OV_LOW1] = "1%% mín. %.0f",
    [UI_MENU_OV_LOW01] = "0.1%% mín. %.0f",
    [UI_MENU_OV_GPU] = "GPU",
    [UI_MENU_OV_CPU] = "CPU",
    [UI_MENU_OV_RAM] = "RAM",
    [UI_MENU_OV_VRAM] = "VRAM",
    [UI_MENU_OV_TEMP] = "Temperatura",
    [UI_MENU_OV_POWER] = "Consumo",
    [UI_MENU_OV_CLOCK] = "Frecuencia",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "Preparación de fotogramas GPU",
    [UI_MENU_OV_GPU_TIME] = "GPU ocupada por fotograma",
    [UI_MENU_OV_PIPE_WAIT] = "Esperas de pipelines",
    [UI_MENU_OV_SHADER_WAIT] = "Esperas de shaders",
    [UI_MENU_OV_TEX_UPLOADS] = "Subidas de texturas",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%",
    [UI_MENU_OV_FMT_TEMP] = "%.0f °C",
    [UI_MENU_OV_FMT_POWER] = "%.0f W",
    [UI_MENU_OV_FMT_MHZ] = "%.0f MHz",
    [UI_MENU_OV_FMT_GB] = "%.1f GB",
    [UI_MENU_OV_FMT_GB_OF] = "/ %.1f GB",
    [UI_MENU_OV_GAME_GB] = "juego %.1f GB",
    [UI_MENU_OV_FMT_MS] = "%.2f ms",
    [UI_MENU_OV_PER_FRAME] = "%.1f por fotograma",
    [UI_MENU_OV_WINDOW] = "Ventana",
    [UI_MENU_OV_OUTPUT] = "Imagen del juego",
    [UI_MENU_OV_SCENE] = "Escena 3D",
    [UI_MENU_OV_UPSCALER] = "Escalador 3D",
    [UI_MENU_OV_SCALING] = "Escalado",
    [UI_MENU_OV_LIMIT] = "Límite de fotogramas",
    [UI_MENU_OV_PRESENT] = "Presentación",
    [UI_MENU_OV_SHOWN] = "Mostrados",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u",
    [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, nitidez %d%%",
    [UI_MENU_OV_SCALE_NONE] = "ninguno (1:1)",
    [UI_MENU_OV_SCALE_BILINEAR] = "bilineal",
    [UI_MENU_OV_SCALE_DOWN] = "bilineal (FSR solo amplía)",
    [UI_MENU_TAB_CHEATS] = "Trucos",
    [UI_MENU_CHEATS_NONE] = "No hay trucos para esta versión del juego. Los trucos de un mod son un archivo de trucos "
                            "de Atmosphère en su carpeta cheats, con el nombre de la build del juego: "
                            "mods\\<mod>\\cheats\\%s. Reinicia el juego para leerlos. Para los trucos que cambian el "
                            "código del juego, el juego se reconstruye al iniciar (unos minutos).",
    [UI_MENU_CHEATS_MOD] = "%s: %d trucos",
    [UI_MENU_CHEAT_CODE_DESC] = "Cambia el código del juego. Se activa al momento.",
    [UI_MENU_CHEAT_KEY_DESC] = "Cambia el código del juego mientras mantienes %s. Se activa al momento.",
    [UI_MENU_CHEAT_DATA_DESC] = "Escribe en la memoria del juego en cada fotograma mientras está activado.",
    [UI_MENU_CHEAT_MASTER] = "siempre activo",
    [UI_MENU_CHEAT_MASTER_DESC] = "El código maestro del archivo: siempre está activo.",
    [UI_MENU_CHEAT_RESTORE] = "no hace falta",
    [UI_MENU_CHEAT_RESTORE_DESC] = "Solo vuelve a escribir el código original del juego. Desactivar un truco ya lo "
                                   "hace.",
    [UI_MENU_CHEAT_REBUILD] = "tras la reconstrucción",
    [UI_MENU_CHEAT_REBUILD_DESC] = "Este truco cambia el código del juego y es nuevo o se editó desde que se "
                                   "construyó el juego. El juego lo incorpora en el próximo inicio (unos minutos), o "
                                   "usa Reconstruir el juego ahora.",
    [UI_MENU_CHEAT_UNSUPPORTED] = "no compatible",
    [UI_MENU_CHEAT_UNSUPPORTED_DESC] = "Este truco no se puede usar con esta versión del juego:",
    [UI_MENU_CHEATS_REBUILD] = "Reconstruir el juego ahora",
    [UI_MENU_CHEATS_REBUILD_DESC] = "Algunos trucos cambian el código del juego y no están incorporados: son nuevos o "
                                    "se editaron, o su última reconstrucción falló o se canceló. Cierra el juego; el "
                                    "instalador los incorpora (unos minutos) y vuelve a iniciar el juego. Se pierde "
                                    "el progreso desde tu último guardado.",
    [UI_MENU_MODS_EXEFS_UNSUPPORTED] = "Este mod parchea el código del juego (exefs) de una forma que el port no "
                                       "puede aplicar, así que todo el mod queda desactivado. %s",
    [UI_MENU_MODS_EXEFS_OTHER] = "Sus parches de código (exefs) son para otra versión del juego.",
    [UI_MENU_MODS_EXEFS_ON] = "Parches de código en uso: %d archivos.",
    [UI_MENU_MODS_REPLACES_BUILTIN] = "Parches de código en uso: %d archivos. Sustituyen al parche propio del port "
                                      "%s, que queda desactivado mientras este mod esté activo.",
    [UI_MENU_MODS_CODE_SKIPPED] = "Su propio código de juego (%s) no se carga: se usa el resto del mod. Desactiva el "
                                  "mod si el juego se comporta mal con él.",
    [UI_MENU_MODS_PLUGIN_ON] = "Su plugin funciona: %d enganches en el código del juego.",
    [UI_MENU_MODS_PLUGIN_REFUSED] = "Su plugin %s no puede funcionar en el port: %s.",
    [UI_MENU_MODS_PLUGIN_STAYS_OFF] = "Todo el mod queda desactivado. Su interruptor \"Usar ... sin su código\" de "
                                      "abajo usa el resto.",
    [UI_MENU_MODS_PLUGIN_OFF] = "Su plugin queda desactivado esta vez: se inició de forma distinta que en la "
                                "reconstrucción (%s). El juego lo reconstruye en el próximo inicio.",
    [UI_MENU_MODS_WITHOUT_CODE] = "Usar %s sin su código",
    [UI_MENU_MODS_WITHOUT_CODE_DESC] = "El mod de arriba trae código que el port no puede ejecutar: su propio código "
                                       "de juego (un plugin subsdk o un main sustituido) o parches de código (exefs) "
                                       "que el port no puede aplicar. No: todo el mod queda desactivado. Sí: ese "
                                       "código se omite y se usa el resto del mod: sus archivos RomFS, sus trucos y "
                                       "los parches de código que funcionan. Sus archivos pueden contar con el código "
                                       "que falta; vuelve a desactivarlo si el juego se comporta mal. Se aplica en el "
                                       "próximo inicio.",
    [UI_MENU_MODS_WITHOUT_CODE_ALL] = "Activado para todos los mods: mods_without_code = 1 en totk.ini.",
    [UI_MENU_MODS_EXEFS_SKIPPED] = "%u de sus archivos de parches de código (exefs) no se pueden aplicar y se omiten: "
                                   "se usa el resto del mod. %s",
    [UI_MENU_MODS_OVERRIDES_FILES] = "%u de sus archivos sustituyen a los de %s, que está más abajo en la lista.",
    [UI_MENU_MODS_DATA_OVERRIDDEN] = "%u bytes de sus cambios en los datos del juego vienen de %s, que está más "
                                     "arriba en la lista.",
    [UI_MENU_MODS_OVERRIDES_DATA] = "%u bytes de sus cambios en los datos del juego sustituyen a los de %s, que está "
                                    "más abajo en la lista.",
    [UI_MENU_MODS_CODE_OVERRIDDEN] = "%u de sus puntos de parche de código quedan anulados por %s, que está más "
                                     "arriba en la lista.",
    [UI_MENU_MODS_OVERRIDES_CODE] = "Anula %u puntos de parche de código de %s, que está más abajo en la lista.",
};

// Italian
static const char *const text_it[UI_TEXT_COUNT] = {
    SWKBD("Inserimento testo", "Inserisci un nome", "Cavallo", "OK", "Annulla", "Controller: A = OK, B = Annulla"),

    [UI_MENU_TITLE] = "Impostazioni",
    [UI_MENU_TAB_SCREEN] = "Schermo",
    [UI_MENU_TAB_GAME] = "Gioco",
    [UI_MENU_TAB_MODS] = "Mod",
    [UI_MENU_TAB_SOUND] = "Audio",
    [UI_MENU_TAB_CONTROLS] = "Comandi",
    [UI_MENU_TAB_AMIIBO] = "Amiibo",
    [UI_MENU_TAB_MENU] = "Menu",
    [UI_MENU_TAB_ADVANCED] = "Avanzate",
    [UI_MENU_CONTINUE] = "Continua",
    [UI_MENU_QUIT] = "Esci dal gioco",
    [UI_MENU_ON] = "Sì",
    [UI_MENU_OFF] = "No",
    [UI_MENU_AUTO] = "Automatico",
    [UI_MENU_DEFAULT_MARK] = "predefinito",
    [UI_MENU_AFTER_RESTART] = "dopo il riavvio",
    [UI_MENU_SET_BY_ENV] = "impostato da una variabile d'ambiente",
    [UI_MENU_SAVE_FAILED] = "impossibile salvare le impostazioni",
    [UI_MENU_VERSION] = "TotK Recomp %s, gioco %s",
    [UI_MENU_FULLSCREEN] = "Schermo intero",
    [UI_MENU_FULLSCREEN_DESC] = "Il gioco riempie lo schermo; F11 lo attiva e disattiva.",
    [UI_MENU_WINDOW_SIZE] = "Dimensione finestra",
    [UI_MENU_WINDOW_SIZE_DESC] = "Dimensione della finestra del gioco quando non è a schermo intero.",
    [UI_MENU_WINDOW_SIZE_CUSTOM] = "Personalizzata",
    [UI_MENU_PRESENT] = "Presentazione",
    [UI_MENU_PRESENT_DESC] = "Come i fotogrammi completati arrivano sullo schermo.",
    [UI_MENU_PRESENT_MAILBOX] = "Mailbox",
    [UI_MENU_PRESENT_VSYNC] = "VSync",
    [UI_MENU_PRESENT_IMMEDIATE] = "Immediata",
    [UI_MENU_PRESENT_IN_USE] = "in uso: %s",
    [UI_MENU_GPU] = "Scheda grafica",
    [UI_MENU_GPU_DESC] = "Il dispositivo Vulkan che disegna il gioco.",
    [UI_MENU_GPU_AUTO] = "Automatica",
    [UI_MENU_PERF_OVERLAY] = "Indicatore prestazioni",
    [UI_MENU_PERF_OVERLAY_DESC] = "Fotogrammi al secondo nell'angolo in alto a destra. Completo aggiunge i tempi dei "
                                  "fotogrammi con un grafico, il carico di CPU, GPU e memoria, le temperature e le "
                                  "attese del thread di rendering. F10 cambia modalità.",
    [UI_MENU_PERF_OFF] = "No",
    [UI_MENU_PERF_FPS] = "FPS",
    [UI_MENU_PERF_FULL] = "Completo",
    [UI_PERF_LINE_FPS] = "%.0f fps",
    [UI_PERF_LINE_FPS_GEN] = "%.0f fps disegnati, %.0f mostrati",
    [UI_MENU_GRAPHICS] = "Immagine",
    [UI_MENU_OUTPUT_RES] = "Risoluzione del gioco",
    [UI_MENU_OUTPUT_RES_DESC] = "La dimensione in cui il gioco disegna immagine e interfaccia. Sopra 1920 x 1080 il "
                                "gioco richiede più memoria video e tempo GPU. Un formato più largo (21:9) allarga la "
                                "visuale della telecamera e sposta l'HUD negli angoli dello schermo; uno più alto "
                                "(16:10, Steam Deck) lo sposta sui bordi superiore e inferiore.",
    [UI_MENU_HUD_WIDTH] = "Larghezza HUD",
    [UI_MENU_HUD_WIDTH_DESC] = "Su uno schermo più largo di 16:9, quanto distano i lati sinistro e destro dell'HUD. "
                               "Schermo intero li mette ai bordi dello schermo; 21:9 o 16:9 mantiene l'HUD al centro "
                               "dello schermo, in un'area di quel formato. Un layout mancante viene creato in "
                               "background e usato dopo il riavvio.",
    [UI_MENU_HUD_WIDTH_FULL] = "Schermo intero",
    [UI_MENU_RENDER_SCALE] = "Scala di rendering 3D",
    [UI_MENU_RENDER_SCALE_DESC] = "Dimensione della scena 3D rispetto alla risoluzione del gioco; l'FSR del gioco la "
                                  "porta a quella dimensione. Sopra il 100 % smussa i bordi seghettati "
                                  "(supersampling) con un alto costo per la GPU.",
    [UI_MENU_RENDER_SCALE_GAME] = "Gioco (83 %)",
    [UI_MENU_DYNAMIC_RES] = "Risoluzione dinamica",
    [UI_MENU_DYNAMIC_RES_DESC] = "Permette al gioco di ridurre la scena 3D al 90 o all'80 %. Il gioco decide in base "
                                 "a una stima fatta per Switch che ignora la tua GPU, quindi le scene ampie scendono "
                                 "anche su un PC veloce. No mantiene la scala di rendering.",
    [UI_MENU_UPSCALER] = "Ridimensionamento alla finestra",
    [UI_MENU_UPSCALER_DESC] = "Come viene ridimensionata l'immagine quando la finestra è più grande della risoluzione "
                              "del gioco. FSR mantiene bordi e dettagli più nitidi del bilineare.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1",
    [UI_MENU_UPSCALER_BILINEAR] = "Bilineare",
    [UI_MENU_ASPECT] = "Forma dell'immagine",
    [UI_MENU_ASPECT_DESC] = "Quando la forma della finestra differisce da quella della risoluzione del gioco "
                            "(un'immagine 16:9 su uno schermo 21:9): mantenere la forma con bande nere o allungarla "
                            "sulla finestra.",
    [UI_MENU_ASPECT_FIT] = "Bande nere",
    [UI_MENU_ASPECT_STRETCH] = "Allunga",
    [UI_MENU_MOVIE_ASPECT] = "Forma dei filmati",
    [UI_MENU_MOVIE_ASPECT_DESC] = "Quando la risoluzione del gioco non è 16:9, mantenere la forma originale dei "
                                  "filmati con bande nere o allungarli sull'intera immagine. Si applica dal filmato "
                                  "successivo.",
    [UI_MENU_MOVIE_ASPECT_KEEP] = "Bande nere",
    [UI_MENU_MOVIE_ASPECT_STRETCH] = "Allunga",
    [UI_MENU_FOV] = "Campo visivo",
    [UI_MENU_FOV_DESC] = "Quanta parte del mondo mostra la telecamera, in percentuale di quella del gioco. Solo "
                         "versione 1.4.3 del gioco.",
    [UI_MENU_VERT_PLUS] = "Mantieni la larghezza 16:9",
    [UI_MENU_VERT_PLUS_DESC] = "Sugli schermi più stretti di 16:9 (Steam Deck, 16:10, 4:3) la telecamera mostra la stessa larghezza del mondo che in 16:9, e di più sopra e sotto. Nessun effetto a 16:9 o più largo.",
    [UI_MENU_SHADOW_RES] = "Dimensione della mappa delle ombre",
    [UI_MENU_SHADOW_RES_DESC] = "Dimensione della mappa delle ombre del sole. Quella del gioco cambia con la visuale, "
                                "fino a circa 400 vicino al suolo, quindi le ombre delle foglie sembrano squadrate e "
                                "saltano; una dimensione fissa le mantiene ferme e più nitide. Sopra 1024 le ombre "
                                "costano più tempo GPU e memoria video.",
    [UI_MENU_SHADOW_RES_GAME] = "Gioco (variabile)",
    [UI_MENU_ANISO] = "Filtro delle texture",
    [UI_MENU_ANISO_DESC] = "Filtro anisotropico delle texture: terreno, strade e muri visti di sbieco restano nitidi "
                           "in lontananza invece di sfocarsi. Gioco: il livello che chiede il gioco, come su Switch.",
    [UI_MENU_ANISO_GAME] = "Gioco",
    [UI_MENU_LOD_STEADY] = "Dettaglio stabile sotto carico",
    [UI_MENU_LOD_STEADY_DESC] = "Salta l'impostazione del gioco che riduce il dettaglio di modelli e texture quando i "
                                "fotogrammi al secondo calano, come fa la mod per emulatore Disable LOD Quality "
                                "Reduction. Versioni del gioco 1.1.0, 1.2.1 e 1.4.3. Lasciala disattivata sui PC poco "
                                "potenti: lì questa riduzione mantiene alti i fotogrammi al secondo.",
    [UI_MENU_SHARPNESS] = "Nitidezza FSR",
    [UI_MENU_SHARPNESS_DESC] = "Nitidezza dopo il ridimensionamento FSR; 0 la disattiva.",
    [UI_MENU_COLOR] = "Colore",
    [UI_MENU_COLOR_PRESET] = "Profilo colore",
    [UI_MENU_COLOR_PRESET_DESC] = "Imposta i quattro valori qui sotto. Vivido: più contrasto e saturazione, ombre un "
                                  "po' più aperte (pensato per schermi OLED). Nitidezza: un po' più di contrasto, "
                                  "saturazione e luminosità. Cambiare un valore porta il profilo a Personalizzato. "
                                  "L'HUD cambia con l'immagine; questo menu no.",
    [UI_MENU_COLOR_DEFAULT] = "Quello del gioco",
    [UI_MENU_COLOR_VIVID] = "Vivido (OLED)",
    [UI_MENU_COLOR_CLARITY] = "Chiarezza",
    [UI_MENU_COLOR_CUSTOM] = "Personalizzato",
    [UI_MENU_COLOR_BRIGHTNESS] = "Luminosità",
    [UI_MENU_COLOR_BRIGHTNESS_DESC] = "Aggiunta a ogni colore; 0 la lascia invariata.",
    [UI_MENU_COLOR_CONTRAST] = "Contrasto",
    [UI_MENU_COLOR_CONTRAST_DESC] = "Allontana i colori dal grigio medio (sopra il 100 %) o li avvicina; 100 % li "
                                    "lascia invariati.",
    [UI_MENU_COLOR_SATURATION] = "Saturazione",
    [UI_MENU_COLOR_SATURATION_DESC] = "Intensità del colore rispetto al grigio; 100 % la lascia invariata.",
    [UI_MENU_COLOR_GAMMA] = "Gamma",
    [UI_MENU_COLOR_GAMMA_DESC] = "Sotto 1 schiarisce i toni scuri e medi, sopra 1 li scurisce; 1 li lascia invariati.",
    [UI_MENU_HDR_GROUP] = "HDR",
    [UI_MENU_HDR] = "Uscita HDR",
    [UI_MENU_HDR_DESC] = "Luci brillanti su uno schermo HDR. Richiede l'HDR attivo in Windows («Usa HDR») o un "
                         "compositor Wayland con gestione del colore; senza, l'immagine resta SDR. Si applica dopo il "
                         "riavvio. Il profilo colore sopra è per l'SDR e non si usa in HDR.",
    [UI_MENU_HDR_PEAK] = "Luminosità di picco",
    [UI_MENU_HDR_PEAK_DESC] = "Il picco del tuo schermo in nit; le luci più brillanti si fermano lì.",
    [UI_MENU_HDR_GAME] = "Luminosità del gioco",
    [UI_MENU_HDR_GAME_DESC] = "Nit del bianco dell'immagine (bianco carta). 203 è il riferimento HDR; aumentalo in "
                              "una stanza luminosa.",
    [UI_MENU_HDR_UI] = "Luminosità di HUD e menu",
    [UI_MENU_HDR_UI_DESC] = "Nit del bianco dell'HUD e di questo menu.",
    [UI_MENU_HDR_HUE] = "Correzione tinta delle luci",
    [UI_MENU_HDR_HUE_DESC] = "I colori brillanti come fuoco e scintille tendono al bianco come nell'immagine SDR del "
                             "gioco, in accordo con la luce che emettono; 0 li mantiene completamente saturi.",
    [UI_MENU_SHADOW_SCALE] = "Risoluzione delle ombre",
    [UI_MENU_SHADOW_SCALE_DESC] = "Disegna le mappe delle ombre del sole tante volte più grandi per lato rispetto ai "
                                  "960x960 del gioco: bordi delle ombre e ombre delle foglie più nitidi. Ha effetto "
                                  "al prossimo avvio.",
    [UI_MENU_PCSS] = "Ombre morbide",
    [UI_MENU_PCSS_DESC] = "Ombre del sole nitide dove un oggetto tocca il suolo e più morbide quanto più lontano cade "
                          "l'ombra (PCSS). 0 mantiene il filtro del gioco. Ha effetto al prossimo avvio.",
    [UI_MENU_CONTACT_SHADOWS] = "Ombre di contatto",
    [UI_MENU_CONTACT_SHADOWS_DESC] = "Piccole ombre del sole che le mappe delle ombre non colgono: ai piedi, sotto le "
                                     "pietre, nell'erba e nelle pieghe. Calcolate sullo schermo dopo l'illuminazione. "
                                     "Ha effetto al prossimo avvio.",
    [UI_MENU_SHADOW_RANGE] = "Portata delle cascate di ombre",
    [UI_MENU_SHADOW_RANGE_DESC] = "Allontana i gradini in cui le ombre diventano più morbide (20, 40, 80 e 160 m nel "
                                  "gioco). Distribuisce la mappa delle ombre su più terreno: aumenta anche la "
                                  "risoluzione delle ombre. Ha effetto al prossimo avvio.",
    [UI_MENU_GRASS_RANGE] = "Distanza dell'erba",
    [UI_MENU_GRASS_RANGE_DESC] = "Fin dove cresce l'erba intorno alla telecamera (nel gioco: 62,5 m). Più alta "
                                 "mantiene visibile l'erba dei prati più lontano invece di farla spuntare appena "
                                 "davanti a te; più bassa ne disegna meno. Ha effetto al prossimo avvio.",
    [UI_MENU_DLSS_PRESET] = "Modello DLSS",
    [UI_MENU_DLSS_PRESET_DESC] = "Quale modello DLSS viene usato. Automatico segue il consiglio di NVIDIA per ogni "
                                 "modalità: K per DLAA, Qualità e Bilanciata, M per Prestazioni, L per Ultra "
                                 "prestazioni. J: un po' meno scie dietro i personaggi in movimento, un po' più di "
                                 "sfarfallio. K: il modello più nitido in ogni modalità. Si applica subito.",
    [UI_MENU_DLSS_PRESET_DEFAULT] = "Automatico",
    [UI_MENU_DLSS_PRESET_K] = "K (più nitido)",
    [UI_MENU_DLSS_PRESET_J] = "J (meno scie)",
    [UI_MENU_DLSS_CHARS] = "Scie dei personaggi (DLSS)",
    [UI_MENU_DLSS_CHARS_DESC] = "Riduce le deboli scie dietro personaggi e animali in movimento e ciò che tengono in "
                                "mano: lì DLSS prende meno dai fotogrammi precedenti. Più forte rimuove più scie e "
                                "lascia quei bordi un po' meno levigati. Si applica subito.",
    [UI_MENU_DLSS_CHARS_OFF] = "No",
    [UI_MENU_DLSS_CHARS_LIGHT] = "Leggera",
    [UI_MENU_DLSS_CHARS_MEDIUM] = "Media",
    [UI_MENU_DLSS_CHARS_FULL] = "Completa",
    [UI_MENU_DLSS] = "Modalità di upscaling",
    [UI_MENU_DLSS_DESC] = "Nativa applica l'antialiasing alla scena 3D a piena risoluzione. Qualità, Bilanciata, "
                          "Prestazioni e Ultra prestazioni la disegnano al 67, 58, 50 e 33 %, e lasciano che "
                          "l'upscaler (DLSS o FSR 3.1) la ingrandisca, risparmiando tempo GPU; sostituiscono la scala "
                          "di rendering 3D. Ha effetto subito.",
    [UI_MENU_DLSS_DLAA] = "Nativa",
    [UI_MENU_DLSS_QUALITY] = "Qualità",
    [UI_MENU_DLSS_BALANCED] = "Bilanciata",
    [UI_MENU_DLSS_PERFORMANCE] = "Prestazioni",
    [UI_MENU_DLSS_ULTRA] = "Ultra prestazioni",
    [UI_MENU_FXAA] = "Antialiasing del gioco (FXAA)",
    [UI_MENU_FXAA_DESC] = "Lo smussamento dei bordi FXAA del gioco. Sfoca un po' i dettagli fini; con DLAA attivo non "
                          "serve.",
    [UI_MENU_REFLEX] = "Bassa latenza",
    [UI_MENU_REFLEX_DESC] = "Riduce la latenza del sistema. Sì usa NVIDIA Reflex (VK_NV_low_latency2); Sì + Boost mantiene inoltre alte le frequenze della GPU NVIDIA nelle scene limitate dalla CPU. Automatico usa Reflex su NVIDIA e AMD Anti-Lag 2 (VK_AMD_anti_lag) su AMD. Si applica dopo un riavvio.",
    [UI_MENU_REFLEX_AUTO] = "Automatico",
    [UI_MENU_REFLEX_OFF] = "No",
    [UI_MENU_REFLEX_ON] = "Sì",
    [UI_MENU_REFLEX_BOOST] = "Sì + Boost",
    [UI_MENU_SCENE_UPSCALER] = "Upscaler della scena 3D",
    [UI_MENU_SCENE_UPSCALER_DESC] = "Un upscaler temporale per la scena 3D: bordi più lisci e meno sfarfallio, e in "
                                    "una modalità sotto Nativa disegna la scena più piccola e la ingrandisce, "
                                    "risparmiando tempo GPU. NVIDIA DLSS richiede una GPU NVIDIA RTX e " DLSS_FILE " "
                                    "accanto a " EXE_FILE "; AMD FSR 3.1 funziona su qualsiasi GPU. No mantiene il "
                                    "ridimensionamento e l'FXAA del gioco. L'HUD non viene toccato; il "
                                    "ridimensionamento e la nitidezza sopra adattano ancora l'immagine del gioco alla "
                                    "finestra. Ha effetto dopo il riavvio.",
    [UI_MENU_SCENE_UPSCALER_OFF] = "No",
    [UI_MENU_SCENE_UPSCALER_DLSS] = "NVIDIA DLSS",
    [UI_MENU_SCENE_UPSCALER_FSR3] = "AMD FSR 3.1",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_GPU] = "DLSS: richiede una GPU NVIDIA RTX",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_DLL] = "DLSS: " DLSS_FILE " mancante",
    [UI_MENU_FSR_SHARPNESS] = "Nitidezza FSR",
    [UI_MENU_FSR_SHARPNESS_DESC] = "Nitidezza della scena 3D dopo AMD FSR 3.1 o FSR 4; 0 la disattiva.",
    [UI_MENU_SCENE_UPSCALER_FSR4] = "AMD FSR 4",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_GPU] = "FSR 4: non disponibile per questa GPU",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_DLL] = "FSR 4: mancano le librerie FSR di AMD",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_OS] = "FSR 4: solo su Windows",
    [UI_MENU_FPS_LIMIT] = "Limite fotogrammi",
    [UI_MENU_FPS_LIMIT_DESC] = "Il massimo di fotogrammi al secondo che il gioco disegna. Sopra 60 è sperimentale e "
                               "richiede «Velocità del gioco secondo l'orologio».",
    [UI_MENU_FPS_30] = "30 FPS",
    [UI_MENU_FPS_40] = "40 FPS",
    [UI_MENU_FPS_45] = "45 FPS",
    [UI_MENU_FPS_60] = "60 FPS",
    [UI_MENU_FPS_90] = "90 FPS",
    [UI_MENU_FPS_120] = "120 FPS",
    [UI_MENU_FPS_144] = "144 FPS",
    [UI_MENU_FPS_DISPLAY] = "Frequenza del monitor",
    [UI_MENU_FPS_DISPLAY_HZ] = "Frequenza del monitor (%.0f Hz)",
    [UI_MENU_FRAME_QUEUE] = "Coda fotogrammi",
    [UI_MENU_FRAME_QUEUE_DESC] = "Il gioco inizia un fotogramma mentre il precedente attende il suo turno sullo "
                                 "schermo: molti meno fotogrammi persi al limite, fino a un fotogramma in più di "
                                 "ritardo dei comandi. Si applica subito.",
    [UI_MENU_FRAME_GEN] = "Generazione fotogrammi",
    [UI_MENU_FRAME_GEN_OFF] = "No",
    [UI_MENU_FRAME_GEN_FSR3] = "AMD FSR 3",
    [UI_MENU_FRAME_GEN_DESC] = "Un'immagine generata tra ogni due che il gioco disegna: il doppio delle immagini "
                               "sullo schermo, il gioco a metà del limite fotogrammi e i comandi circa mezzo "
                               "fotogramma più tardi. Per schermi da 120 Hz in su. Non con DLSS o HDR. Si applica "
                               "subito.",
    [UI_MENU_REALTIME] = "Velocità del gioco secondo l'orologio",
    [UI_MENU_REALTIME_DESC] = "Fa avanzare il gioco in tempo reale, così un fotogramma lento non rallenta il gioco.",
    [UI_MENU_CLOTH_REALTIME] = "Tessuti secondo l'orologio",
    [UI_MENU_CLOTH_REALTIME_DESC] = "Fa avanzare in tempo reale anche tessuti e capelli.",
    [UI_MENU_HIGHFPS_FIX] = "Correzioni per alti FPS",
    [UI_MENU_HIGHFPS_FIX_DESC] = "Corregge la fisica che sbaglia sopra i 30 fotogrammi al secondo e gli oggetti che "
                                 "restano trasparenti o invisibili sopra i 60.",
    [UI_MENU_CLIMB_FIX] = "Correzione arrampicata",
    [UI_MENU_CLIMB_FIX_DESC] = "Mantiene a piena velocità l'arrampicata su pareti e scale sopra i 60 fotogrammi al "
                               "secondo, così Link arriva in cima.",
    [UI_MENU_GAME_LANG] = "Lingua del gioco",
    [UI_MENU_GAME_LANG_DESC] = "Lingua dei testi e delle voci del gioco.",
    [UI_MENU_GAME_LANG_AUTO] = "Automatica",
    [UI_MENU_OPERATION_MODE] = "Modalità console",
    [UI_MENU_OPERATION_MODE_DESC] = "Se il gioco crede di essere in modalità TV o portatile.",
    [UI_MENU_MODE_DOCKED] = "Modalità TV",
    [UI_MENU_MODE_HANDHELD] = "Modalità portatile",
    [UI_MENU_ASSUME_FOCUS] = "Continua in background",
    [UI_MENU_ASSUME_FOCUS_DESC] = "Sì (predefinito): il gioco continua mentre sei in un'altra finestra, e la tastiera "
                                  "resta inattiva finché la finestra non torna in primo piano. No: si mette in pausa "
                                  "quando la finestra perde il focus, come su Switch premendo Home; un caricamento "
                                  "interrotto così può bloccarsi per sempre.",
    [UI_MENU_PLAY_TIME] = "Tempo di gioco",
    [UI_MENU_PLAY_TIME_DESC] = "Tempo passato nel gioco in tutte le sessioni, contato mentre la finestra del gioco ha "
                               "il focus e il gioco disegna fotogrammi. Salvato in playtime.txt nella cartella del "
                               "profilo, accanto ai salvataggi.",
    [UI_MENU_PLAY_TIME_HM] = "%llu h %02u min",
    [UI_MENU_PLAY_TIME_SESSION] = "questa sessione: %s",
    [UI_MENU_MODS_BUILTIN] = "Integrate nel port",
    [UI_MENU_MODS_INSTALLED] = "Mod installate",
    [UI_MENU_MODS_NONE] = "Ancora nessuna mod. Estrai una mod nella cartella mods, una cartella per mod con la sua "
                          "cartella romfs dentro, e riavvia il gioco. Anche TKMM può esportare lì le sue mod unite in "
                          "una cartella.",
    [UI_MENU_MODS_OPEN] = "Apri cartella mod",
    [UI_MENU_MODS_OPEN_DESC] = "Mostra la cartella da cui il gioco carica le mod. Viene creata se manca.",
    [UI_MENU_MODS_ROW_DESC] = "File del gioco sostituiti: %u. Nuovi file aggiunti: %u.",
    [UI_MENU_MODS_OVERRIDDEN] = "%u dei suoi file provengono da %s, che è più in alto nell'elenco.",
    [UI_MENU_MODS_MOVE_HINT] = "Il pulsante superiore o Ctrl+Su la sposta in alto, il pulsante sinistro o Ctrl+Giù in "
                               "basso. Quando due mod cambiano lo stesso file, vince quella più in alto. Le modifiche "
                               "si applicano dopo il riavvio.",
    [UI_MENU_MODS_EMPTY] = "Questa cartella non contiene file romfs, exefs o cheats.",
    [UI_MENU_MODS_SUBSDK] = "Questa mod porta il proprio codice di gioco (un plugin subsdk o un main sostituito). Il "
                            "port non può caricare moduli di codice, quindi l'intera mod resta disattivata.",
    [UI_MENU_MODS_EXEFS_LATER] = "Questa mod modifica il codice del gioco (exefs). Il gioco integra le patch al "
                                 "prossimo avvio (una ricostruzione di qualche minuto); fino ad allora l'intera mod "
                                 "resta disattivata.",
    [UI_MENU_MODS_CHEATS] = "Trucchi: %d. Attivali nella scheda Trucchi.",
    [UI_MENU_MODS_CHEATS_FILE] = "I trucchi per questa versione del gioco vengono letti all'avvio da cheats\\%s in "
                                 "questa mod.",
    [UI_MENU_MODS_BAD_NAME] = "Il nome della cartella contiene |, che l'elenco delle mod usa tra i nomi. Rinomina la "
                              "cartella.",
    [UI_MENU_MODS_OTHER_VERSION] = "%u dei suoi file sono fatti per un'altra versione del gioco e vengono esclusi. "
                                   "Cerca una versione di questa mod fatta per la tua versione del gioco.",
    [UI_MENU_MODS_UNUSED] = "non usato",
    [UI_MENU_MODS_FPS60] = "Patch 60 FPS",
    [UI_MENU_MODS_FPS60_DESC] = "La patch del port per 60 fotogrammi al secondo. Si imposta con il limite fotogrammi "
                                "nella scheda Gioco.",
    [UI_MENU_MODS_RES] = "Patch risoluzione",
    [UI_MENU_MODS_RES_DESC] = "La patch del port per risoluzioni sopra 1080p. Si imposta con la risoluzione del gioco "
                              "nella scheda Schermo.",
    [UI_MENU_MODS_QUICKMENU] = "Correzione menu rapido",
    [UI_MENU_MODS_QUICKMENU_DESC] = "La correzione del port per lo sfondo nero dei menu di armi, scudi e materiali a "
                                    "60 FPS.",
    [UI_MENU_VOLUME] = "Volume",
    [UI_MENU_VOLUME_DESC] = "Volume generale del gioco.",
    [UI_MENU_VOLUME_MUSIC] = "Volume musica",
    [UI_MENU_VOLUME_MUSIC_DESC] = "Volume della musica di sottofondo.",
    [UI_MENU_VOLUME_SFX] = "Volume effetti",
    [UI_MENU_VOLUME_SFX_DESC] = "Volume degli effetti sonori.",
    [UI_MENU_VOLUME_VOICE] = "Volume voci",
    [UI_MENU_VOLUME_VOICE_DESC] = "Volume delle voci dei personaggi.",
    [UI_MENU_AUDIO_CHANNELS] = "Altoparlanti",
    [UI_MENU_AUDIO_CHANNELS_DESC] = "Uscita stereo o surround 5.1. Automatico sceglie 5.1 quando la configurazione "
                                    "degli altoparlanti di Windows ha sei o più canali. Si applica dopo il riavvio.",
    [UI_MENU_CHANNELS_AUTO] = "Automatico",
    [UI_MENU_CHANNELS_STEREO] = "Stereo",
    [UI_MENU_CHANNELS_51] = "Surround 5.1",
    [UI_MENU_AUDIO_DEVICE] = "Dispositivo di uscita",
    [UI_MENU_AUDIO_DEVICE_DESC] = "Dove viene riprodotto l'audio. Predefinito di sistema segue il dispositivo "
                                  "predefinito di Windows. Se il dispositivo scelto viene scollegato, l'audio passa "
                                  "al predefinito di sistema finché non viene ricollegato. Si applica subito.",
    [UI_MENU_AUDIO_DEVICE_DEFAULT] = "Predefinito di sistema",
    [UI_MENU_MOTION] = "Comandi di movimento",
    [UI_MENU_MOTION_DESC] = "Mira con il giroscopio del controller.",
    [UI_MENU_GYRO_CALIBRATION] = "Correzione deriva giroscopio",
    [UI_MENU_GYRO_CALIBRATION_DESC] = "Sottrae la deriva a riposo del controller, misurata con la riga di "
                                      "calibrazione qui sotto. I valori misurati restano salvati anche quando è "
                                      "disattivata.",
    [UI_MENU_GYRO_CALIBRATE] = "Calibra giroscopio",
    [UI_MENU_GYRO_CALIBRATE_DESC] = "Misura la deriva a riposo del giroscopio e la rimuove. Appoggia il controller o "
                                    "tienilo fermo per due secondi.",
    [UI_MENU_GYRO_CALIBRATE_WAIT] = "Calibrazione...",
    [UI_MENU_GYRO_CALIBRATED] = "%s, calibrato",
    [UI_MENU_GYRO_CALIBRATE_FAILED] = "Calibrazione non riuscita",
    [UI_MENU_GYRO_CALIBRATE_MOVED] = "Il controller si è mosso: tienilo fermo e riprova",
    [UI_MENU_GYRO_AUTO_BIAS] = "Apprendimento deriva a riposo",
    [UI_MENU_GYRO_AUTO_BIAS_DESC] = "Apprende la deriva rimasta nel giroscopio ogni volta che il controller è fermo "
                                    "su un tavolo, e la sottrae. Per una mira con l'arco che scivola lentamente anche "
                                    "dopo la calibrazione. Un controller tenuto in mano non viene usato per "
                                    "l'apprendimento. Sperimentale.",
    [UI_MENU_RUMBLE_STRENGTH] = "Intensità della vibrazione",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "Quanto forte vibra il controller.",
    [UI_MENU_RUMBLE_CURVE] = "Curva della vibrazione",
    [UI_MENU_RUMBLE_CURVE_DESC] = "Valori più bassi rendono più forte la vibrazione debole.",
    [UI_MENU_RUMBLE_TRIGGERS] = "Vibrazione dei grilletti (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Fa vibrare anche i grilletti di un controller Xbox.",
    [UI_MENU_PAD_LAYOUT] = "Disposizione dei pulsanti",
    [UI_MENU_PAD_LAYOUT_DESC] = "Quale pulsante è A. Nintendo: quello a destra, come su Switch. Xbox: quello in "
                                "basso, come indicato su un controller Xbox (croce su un controller PlayStation). "
                                "Automatico sceglie Nintendo per i controller con scritte Nintendo e Xbox per gli "
                                "altri. Il controller mostrato nei suggerimenti del gioco si sceglie in Icone dei "
                                "pulsanti.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Automatico",
    [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A a destra)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A in basso)",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "in uso: %s",
    [UI_MENU_PROMPTS] = "Icone dei pulsanti",
    [UI_MENU_PROMPTS_DESC] = "Di quale controller sono i pulsanti mostrati nei suggerimenti del gioco. Nintendo: "
                             "quelli del gioco. Xbox: LB, RB, LT, RT, Menu, View e una croce, e A, B, X e Y come "
                             "indicati sul pulsante che preme la disposizione sopra (con Nintendo (A a destra) il "
                             "pulsante destro è indicato come B, quindi i suggerimenti per A mostrano B). "
                             "PlayStation: i simboli di un controller PlayStation (○ × △ □, L1, R1, L2, R2, Options e "
                             "Share), sul pulsante che preme la disposizione sopra. PC: i tasti e i pulsanti del "
                             "mouse della disposizione predefinita della tastiera (E, Maiusc, Spazio, clic "
                             "sinistro...) e il mouse per la telecamera. Il gioco legge i suggerimenti all'avvio, "
                             "quindi cambiano dopo il riavvio. Fanno parte dei file del gioco, quindi un pulsante che "
                             "riassegni in Comandi mantiene sullo schermo il suo vecchio suggerimento.",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo",
    [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation",
    [UI_MENU_PROMPTS_PC] = "PC",
    [UI_MENU_PROMPTS_MISSING] = "file delle icone mancanti: esegui di nuovo l'installazione",
    [UI_MENU_PROMPTS_FAILED] = "file delle icone danneggiati: esegui di nuovo l'installazione",
    [UI_MENU_PROMPTS_RESTART] = "le icone cambiano dopo il riavvio",
    [UI_MENU_PROMPTS_OFF_SHAPE] = "disattivato con una risoluzione del gioco non 16:9",
    [UI_MENU_OUTPUT_RES_NO_LAYOUT] = "nessun layout per questo formato: esegui di nuovo l'installazione",
    [UI_MENU_OUTPUT_RES_PREPARING] = "layout in preparazione in background; usato al prossimo avvio",
    [UI_MENU_PROMPTS_PREPARING] = "file delle icone in preparazione in background; usati al prossimo avvio",
    [UI_MENU_PREPARE_RETRY] = "Riprova",
    [UI_MENU_PREPARE_RETRY_DESC] = "Riavvia gli strumenti dell'installazione per creare in background il layout "
                                   "dell'interfaccia o i file delle icone dei pulsanti mancanti. Il gioco continua; "
                                   "li usa dal prossimo avvio.",
    [UI_MENU_SHAPE_PREPARE_FAILED] = "gli strumenti dell'installazione non sono partiti: esegui di nuovo "
                                     "l'installazione",
    [UI_MENU_PROMPTS_OFF_MODS] = "disattivato: una mod RomFS sostituisce i loro file",
    [UI_MENU_PROMPTS_OFF_PATCHES] = "disattivato finché TOTK_ROMFS_PATCHES è impostato",
    [UI_MENU_RESTART] = "Riavvia ora",
    [UI_MENU_RESTART_DESC] = "Chiude il gioco e lo riavvia con le nuove icone. I progressi dall'ultimo salvataggio "
                             "vanno persi.",
    [UI_MENU_RESTART_CONFIRM] = "Riavviare il gioco ora? I progressi dall'ultimo salvataggio vanno persi.",
    [UI_MENU_RESTART_YES] = "Riavvia",
    [UI_MENU_RESTART_SAVING] = "Il gioco sta salvando: il riavvio sarà possibile tra un momento.",
    [UI_MENU_GROUP_STICKS] = "Levette analogiche",
    [UI_MENU_LSTICK_DEADZONE] = "Zona morta levetta sinistra",
    [UI_MENU_RSTICK_DEADZONE] = "Zona morta levetta destra",
    [UI_MENU_STICK_DEADZONE_DESC] = "Quanto deve muoversi una levetta prima che il gioco se ne accorga. Aumentala se "
                                    "il personaggio o la telecamera si muovono da soli: lascia la levetta, leggi "
                                    "«levetta ora» e imposta la zona morta qualche punto sopra.",
    [UI_MENU_STICK_DEADZONE_NOW] = "levetta ora: %.0f %%",
    [UI_MENU_MOUSE_MODE] = "Telecamera con mouse",
    [UI_MENU_MOUSE_MODE_DESC] = "Diretta: la telecamera gira di quanto si sposta il mouse, come in un gioco per PC, "
                                "senza zona morta né curva della levetta. Emulazione levetta: il mouse fa da levetta destra.",
    [UI_MENU_MOUSE_STICK] = "Emulazione levetta",
    [UI_MENU_MOUSE_DIRECT] = "Diretta",
    [UI_MENU_MENU_CURSOR] = "Mouse nei menu del gioco",
    [UI_MENU_MENU_CURSOR_DESC] = "Nei menu del gioco stesso (schermata del titolo, inventario, negozi, scelte nei "
                                 "dialoghi) appare il puntatore: l'elemento sotto di esso viene selezionato, il clic "
                                 "sinistro conferma, il clic destro torna indietro. Solo se per ultimi sono stati "
                                 "usati il mouse o un tasto, così con un controller i menu restano invariati (gioco "
                                 "1.4.3).",
    [UI_MENU_MAP_CLICK] = "Clic sinistro sulla mappa",
    [UI_MENU_MAP_CLICK_DESC] = "Sulla mappa del Purah Pad, il tasto sinistro del mouse preme A sul cursore della "
                               "mappa invece del pulsante a cui è assegnato (Y per impostazione predefinita). "
                               "Richiede «Mouse nei menu del gioco» (gioco 1.4.3).",
    [UI_MENU_DIALOGUE_CLICK] = "Clic nei dialoghi",
    [UI_MENU_DIALOGUE_CLICK_DESC] = "Mentre è aperto un riquadro di dialogo, il clic sinistro preme B (prosegue il "
                                    "testo e ne salta lo scorrimento) e il clic destro preme A, invece dei pulsanti a "
                                    "cui sono assegnati (Y per impostazione predefinita, che apre il registro dei "
                                    "dialoghi, e ZL). Le scelte nei dialoghi mantengono il puntatore: il clic "
                                    "sinistro sceglie (gioco 1.4.3).",
    [UI_MENU_POINTER_IMAGE] = "Puntatore del gioco",
    [UI_MENU_POINTER_IMAGE_DESC] = "Mentre il cursore non è catturato (menu del gioco, questo menu, dopo Esc), la "
                                   "finestra mostra una propria punta di freccia dorata invece della freccia di "
                                   "sistema, dimensionata secondo la scala dello schermo.",
    [UI_MENU_QUICKSAVE] = "Salvataggio/caricamento rapido",
    [UI_MENU_QUICKSAVE_DESC] = "Un tasto salva subito la partita e un altro carica quel salvataggio (le due righe qui "
                               "sotto: F6 e F7 per impostazione predefinita, come in Half-Life), senza nessun menu "
                               "sullo schermo. Solo tasti della tastiera e del mouse, solo gioco 1.4.3. Il "
                               "salvataggio rapido chiede al gioco uno dei suoi salvataggi automatici, quindi "
                               "sostituisce il salvataggio automatico più vecchio (il salvataggio manuale resta), ne "
                               "conserva una copia come salvataggio rapido (la cartella quicksave del profilo) e "
                               "mostra \"Partita salvata\" in alto nell'immagine. Il caricamento rapido mantiene "
                               "l'immagine su \"Caricamento del salvataggio rapido\" mentre scorre di nascosto "
                               "l'elenco dei caricamenti del gioco, e il gioco carica il salvataggio rapido (circa 5 "
                               "s). Dove il gioco non farebbe da sé un salvataggio automatico (una scena "
                               "d'intermezzo, un dialogo), non viene salvato nulla e la riga lo indica.",
    [UI_MENU_QUICKSAVE_KEY] = "Tasto salvataggio rapido",
    [UI_MENU_QUICKLOAD_KEY] = "Tasto caricamento rapido",
    [UI_MENU_QUICKSAVE_KEY_DESC] = "Il tasto che avvia il salvataggio o il caricamento rapido; F6 e F7 per "
                                   "impostazione predefinita, come in Half-Life. Seleziona la cella e premi un tasto "
                                   "o un pulsante del mouse; Canc lascia quell'azione senza tasto. Mentre il "
                                   "salvataggio rapido è attivo, i due tasti non fanno nient'altro (una nota appare "
                                   "quando anche un'assegnazione usa il tasto). totk.ini accetta qualsiasi nome di "
                                   "tasto delle assegnazioni (quicksave_key, quickload_key).",
    [UI_QS_SAVED] = "Partita salvata",
    [UI_QS_LOADING] = "Caricamento del salvataggio rapido",
    [UI_QS_NOT_NOW] = "Il salvataggio rapido non è possibile ora",
    [UI_QS_NONE] = "Non c'è ancora un salvataggio rapido (lo crea il tasto di salvataggio rapido)",
    [UI_QS_FAILED] = "Salvataggio rapido non riuscito: vedi totk.log",
    [UI_QS_LOAD_FAILED] = "Caricamento rapido non riuscito: vedi totk.log",
    [UI_MENU_MOUSE_SENS] = "Sensibilità telecamera con mouse",
    [UI_MENU_MOUSE_SENS_DESC] = "Quanto velocemente il mouse gira la telecamera.",
    [UI_MENU_WALK_KEY] = "Tasto camminata",
    [UI_MENU_WALK_KEY_DESC] = "Tienilo premuto per inclinare solo in parte le levette della tastiera, così la "
                              "tastiera cammina invece di correre. La scala di camminata stabilisce quanto. "
                              "Controller e mouse non sono coinvolti.",
    [UI_MENU_WALK_KEY_TAKEN] = "anche %s",
    [UI_MENU_WALK_SCALE] = "Scala di camminata",
    [UI_MENU_WALK_SCALE_DESC] = "L'inclinazione delle levette della tastiera mentre il tasto camminata è premuto. "
                                "Metà è camminare, l'inclinazione completa correre.",
    [UI_MENU_FREECAM] = "Telecamera libera",
    [UI_MENU_FREECAM_DESC] = "Permette a una combinazione o a un tasto qui sotto di staccare la visuale da Link e "
                             "farla volare. Disattivata per impostazione predefinita: così il codice della telecamera "
                             "del gioco funziona come in origine. Ha effetto dopo il riavvio. Versione del gioco "
                             "1.4.3.",
    [UI_MENU_FREECAM_OFF_NOTE] = "Attiva «Telecamera libera» qui sopra, poi riavvia",
    [UI_MENU_FREECAM_PAD] = "Combo telecamera libera",
    [UI_MENU_FREECAM_PAD_DESC] = "Pulsanti del controller premuti insieme che portano la visuale attraverso tre fasi: "
                                 "lascia Link e vola, resta dove è stata lasciata mentre Link continua a muoversi, e "
                                 "torna indietro. Mentre vola il gioco non riceve input: la levetta sinistra la fa "
                                 "volare lungo la visuale e di lato, i grilletti in su e in giù, la levetta destra la "
                                 "gira, e ogni pressione di L o R dimezza o raddoppia la velocità. Quando è ferma, "
                                 "Link cammina dove punta la levetta sinistra sullo schermo. Versione del gioco 1.4.3.",
    [UI_MENU_FREECAM_PAD_V143] = "Solo versione 1.4.3 del gioco",
    [UI_MENU_FREECAM_KEY] = "Tasto telecamera libera",
    [UI_MENU_FREECAM_KEY_DESC] = "Un tasto che porta la visuale attraverso le stesse fasi: lascia Link e vola, resta "
                                 "ferma mentre Link continua a muoversi, e torna indietro. Mentre vola: W A S D la "
                                 "fanno volare lungo la visuale e di lato, i tasti assegnati a ZR e ZL (X e Z per "
                                 "impostazione predefinita) in su e in giù, il mouse e i tasti freccia la girano, e i "
                                 "tasti assegnati a L e R (Q e R) dimezzano o raddoppiano la velocità. Versione del "
                                 "gioco 1.4.3.",
    [UI_MENU_GROUP_GAMEPLAY_TWEAKS] = "Ritocchi di gioco",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED] = "Ritocchi di gioco",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED_DESC] = "Comandi con la rotellina del mouse per rotazione e distanza "
                                             "dell'Ultramano, cambio di oggetti e armi, e zoom fluido della mappa.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE] = "Rotazione Ultramano con rotellina",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE_DESC] = "Ruota direttamente con la rotellina del mouse gli oggetti tenuti con "
                                               "l'Ultramano.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS] = "Asse C di rotazione Ultramano",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS_DESC] = "Tieni premuto C mentre scorri per ruotare gli oggetti tenuti attorno "
                                               "all'asse alternativo.",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY] = "Sensibilità rotazione Ultramano",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY_DESC] = "Quanto velocemente la rotellina del mouse ruota gli oggetti "
                                                   "tenuti.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE] = "Distanza Ultramano con rotellina",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE_DESC] = "Clic destro + rotellina del mouse allontana o avvicina gli oggetti "
                                                 "tenuti.",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY] = "Sensibilità distanza Ultramano",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY_DESC] = "Quanto velocemente la rotellina del mouse cambia la distanza "
                                                   "degli oggetti.",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL] = "Selezione armi con rotellina",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL_DESC] = "Scorri la rotellina del mouse durante l'esplorazione libera per cambiare "
                                           "arma. Conferma con E.",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY] = "Sensibilità rotellina armi",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY_DESC] = "Scatti di rotellina necessari per passare all'arma successiva.",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL] = "Menu rapido con rotellina",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL_DESC] = "Scorri la rotellina del mouse per sfogliare gli oggetti mentre è aperto un "
                                         "menu di selezione rapida.",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY] = "Sensibilità rotellina menu rapido",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY_DESC] = "Scatti di rotellina necessari per passare da un oggetto all'altro nei "
                                               "menu di selezione rapida.",
    [UI_MENU_GAMEPLAY_ITEM_AD] = "Menu rapido con A / D",
    [UI_MENU_GAMEPLAY_ITEM_AD_DESC] = "Premi A / D per spostarti a sinistra e a destra nei menu di selezione rapida.",
    [UI_MENU_GAMEPLAY_MAP_ZOOM] = "Zoom fluido della mappa",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_DESC] = "Zoom fluido con inerzia sulla mappa del Purah Pad usando la rotellina del "
                                       "mouse.",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY] = "Sensibilità zoom mappa",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY_DESC] = "Quanto velocemente la rotellina del mouse ingrandisce la mappa.",
    [UI_MENU_STICK_SENS] = "Velocità telecamera con levetta",
    [UI_MENU_STICK_SENS_DESC] = "Moltiplica la velocità della telecamera della levetta destra del gioco, velocità "
                                "massima inclusa, per ognuna delle cinque sensibilità della telecamera del gioco. Il "
                                "mouse mantiene la propria impostazione.",
    [UI_BINDS_HELP] = "Seleziona una cella e premi A o Invio, poi il tasto, il pulsante del mouse o del controller. "
                      "Canc o X svuota una cella. Un tasto già usato viene spostato qui; un pulsante del controller "
                      "si scambia con quello che sostituisce.",
    [UI_BINDS_COL_KEY1] = "Tasto 1",
    [UI_BINDS_COL_KEY2] = "Tasto 2",
    [UI_BINDS_COL_PAD] = "Controller",
    [UI_BINDS_GROUP_BUTTONS] = "Pulsanti",
    [UI_BINDS_GROUP_MOVE] = "Movimento",
    [UI_BINDS_GROUP_CAMERA] = "Telecamera",
    [UI_BINDS_PRESS_KEY] = "Premi un tasto o un pulsante del mouse per %s",
    [UI_BINDS_PRESS_PAD] = "Premi un pulsante del controller per %s",
    [UI_BINDS_CANCEL_HINT] = "Esc annulla, oppure attendi %d s",
    [UI_BINDS_TAKEN] = "%s è stato tolto da %s",
    [UI_BINDS_SWAPPED] = "%s scambiato con %s",
    [UI_BINDS_RESET] = "Ripristina tutti i pulsanti",
    [UI_BINDS_RESET_DESC] = "Torna ai tasti e ai pulsanti del controller predefiniti. La disposizione dei pulsanti "
                            "sopra torna a valere: i pulsanti del controller riassegnati restano dove li hai messi "
                            "fino a questo ripristino.",
    [UI_BIND_A] = "A",
    [UI_BIND_B] = "B",
    [UI_BIND_X] = "X",
    [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "Pressione levetta sinistra",
    [UI_BIND_RCLICK] = "Pressione levetta destra",
    [UI_BIND_L] = "L",
    [UI_BIND_R] = "R",
    [UI_BIND_ZL] = "ZL",
    [UI_BIND_ZR] = "ZR",
    [UI_BIND_PLUS] = "Più",
    [UI_BIND_MINUS] = "Meno",
    [UI_BIND_LEFT] = "Croce sinistra",
    [UI_BIND_UP] = "Croce su",
    [UI_BIND_RIGHT] = "Croce destra",
    [UI_BIND_DOWN] = "Croce giù",
    [UI_BIND_LSTICK_LEFT] = "Vai a sinistra",
    [UI_BIND_LSTICK_UP] = "Avanti",
    [UI_BIND_LSTICK_RIGHT] = "Vai a destra",
    [UI_BIND_LSTICK_DOWN] = "Indietro",
    [UI_BIND_RSTICK_LEFT] = "Telecamera a sinistra",
    [UI_BIND_RSTICK_UP] = "Telecamera in alto",
    [UI_BIND_RSTICK_RIGHT] = "Telecamera a destra",
    [UI_BIND_RSTICK_DOWN] = "Telecamera in basso",
    [UI_BIND_DESC_A] = "Interagire, raccogliere, parlare.",
    [UI_BIND_DESC_B] = "Correre, annullare, tornare indietro.",
    [UI_BIND_DESC_X] = "Saltare.",
    [UI_BIND_DESC_Y] = "Attaccare.",
    [UI_BIND_DESC_LCLICK] = "Accovacciarsi.",
    [UI_BIND_DESC_RCLICK] = "Visuale in prima persona.",
    [UI_BIND_DESC_L] = "Abilità.",
    [UI_BIND_DESC_R] = "Lanciare un'arma.",
    [UI_BIND_DESC_ZL] = "Scudo, mirare un bersaglio.",
    [UI_BIND_DESC_ZR] = "Arco.",
    [UI_BIND_DESC_PLUS] = "Menu, inventario.",
    [UI_BIND_DESC_MINUS] = "Mappa.",
    [UI_BIND_DESC_DPAD] = "Selezione rapida.",
    [UI_BIND_DESC_MOVE] = "Muove Link come la levetta sinistra. Anche la levetta sinistra del controller funziona "
                          "sempre.",
    [UI_BIND_DESC_CAMERA] = "Gira la telecamera come la levetta destra. Anche il mouse e la levetta destra del "
                            "controller funzionano sempre.",
    [UI_BIND_SHAKE] = "Scuoti levetta telecamera",
    [UI_BIND_DESC_SHAKE] = "Muove per il gioco la levetta destra a sinistra e a destra: stacca una parte attaccata da "
                           "ciò che tiene l'Ultramano. Tenendo premuto il tasto continua a scuotere.",
    [UI_KEY_SPACE] = "Spazio",
    [UI_KEY_TAB] = "Tab",
    [UI_KEY_LSHIFT] = "Maiusc sinistro",
    [UI_KEY_LCTRL] = "Ctrl sinistro",
    [UI_KEY_UP] = "Freccia su",
    [UI_KEY_DOWN] = "Freccia giù",
    [UI_KEY_LEFT] = "Freccia sinistra",
    [UI_KEY_RIGHT] = "Freccia destra",
    [UI_KEY_MOUSE_L] = "Tasto sinistro",
    [UI_KEY_MOUSE_R] = "Tasto destro",
    [UI_KEY_MOUSE_M] = "Tasto centrale",
    [UI_KEY_MOUSE_X1] = "Mouse 4",
    [UI_KEY_MOUSE_X2] = "Mouse 5",
    [UI_KEY_ENTER] = "Invio",
    [UI_KEY_BACKSPACE] = "Backspace",
    [UI_KEY_RSHIFT] = "Maiusc destro",
    [UI_KEY_RCTRL] = "Ctrl destro",
    [UI_KEY_LALT] = "Alt",
    [UI_KEY_CAPSLOCK] = "Bloc Maiusc",
    [UI_KEY_INSERT] = "Ins",
    [UI_KEY_DELETE] = "Canc",
    [UI_KEY_HOME] = "Home",
    [UI_KEY_END] = "Fine",
    [UI_KEY_PAGEUP] = "Pag su",
    [UI_KEY_PAGEDOWN] = "Pag giù",
    [UI_KEY_NUM_FMT] = "Num %s",
    [UI_PAD_SOUTH] = "Pulsante in basso",
    [UI_PAD_EAST] = "Pulsante a destra",
    [UI_PAD_WEST] = "Pulsante a sinistra",
    [UI_PAD_NORTH] = "Pulsante in alto",
    [UI_PAD_LSTICK] = "Levetta sinistra",
    [UI_PAD_RSTICK] = "Levetta destra",
    [UI_PAD_L] = "Dorsale sinistro",
    [UI_PAD_R] = "Dorsale destro",
    [UI_PAD_START] = "Start",
    [UI_PAD_BACK] = "Indietro",
    [UI_PAD_DPAD_LEFT] = "Croce sinistra",
    [UI_PAD_DPAD_UP] = "Croce su",
    [UI_PAD_DPAD_RIGHT] = "Croce destra",
    [UI_PAD_DPAD_DOWN] = "Croce giù",
    [UI_PAD_ZL] = "Grilletto sinistro",
    [UI_PAD_ZR] = "Grilletto destro",
    [UI_PAD_LPADDLE1] = "Pulsante posteriore sinistro",
    [UI_PAD_RPADDLE1] = "Pulsante posteriore destro",
    [UI_PAD_LPADDLE2] = "Pulsante posteriore sinistro 2",
    [UI_PAD_RPADDLE2] = "Pulsante posteriore destro 2",
    [UI_PAD_TOUCHPAD] = "Pressione touchpad",
    [UI_PAD_CROSS] = "Croce",
    [UI_PAD_CIRCLE] = "Cerchio",
    [UI_PAD_SQUARE] = "Quadrato",
    [UI_PAD_TRIANGLE] = "Triangolo",
    [UI_MENU_AMIIBO_DIR] = "Cartella amiibo",
    [UI_MENU_AMIIBO_OPEN] = "Apri",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "Cartella predefinita",
    [UI_MENU_AMIIBO_REFRESH] = "Aggiorna",
    [UI_MENU_AMIIBO_PICK] = "Dump di amiibo (file .bin o .nfc da 540 byte):",
    [UI_MENU_AMIIBO_NO_DIR] = "Impossibile aprire la cartella %s",
    [UI_MENU_AMIIBO_READER] = "Lettore NFC",
    [UI_MENU_AMIIBO_STATE] = "Lettore:",
    [UI_MENU_AMIIBO_STATE_IDLE] = "pronto",
    [UI_MENU_AMIIBO_STATE_SEARCHING] = "il gioco attende un amiibo",
    [UI_MENU_AMIIBO_STATE_FOUND] = "amiibo sul lettore",
    [UI_MENU_AMIIBO_STATE_MOUNTED] = "il gioco sta leggendo l'amiibo",
    [UI_MENU_AMIIBO_STATE_OFF] = "disattivato",
    [UI_MENU_AMIIBO_FILE] = "File scelto:",
    [UI_MENU_AMIIBO_NO_FILE] = "(nessun file scelto)",
    [UI_MENU_AMIIBO_TAP] = "Avvicina l'amiibo",
    [UI_MENU_AMIIBO_REMOVE] = "Allontana l'amiibo",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "Allontana l'amiibo da solo dopo averlo avvicinato",
    [UI_MENU_AMIIBO_HOLD] = "Tempo sul lettore",
    [UI_MENU_AMIIBO_SECONDS] = "%.1f s",
    [UI_MENU_AMIIBO_TAG] = "Amiibo sul lettore:",
    [UI_MENU_AMIIBO_NO_TAG] = "Nessun amiibo sul lettore.",
    [UI_MENU_AMIIBO_TAG_FILE] = "File: %s",
    [UI_MENU_AMIIBO_TAG_ID] = "Personaggio %s, serie %02x, numero %04x, tipo %02x",
    [UI_MENU_AMIIBO_TAG_UID] = "UID: %s",
    [UI_MENU_AMIIBO_READY] = "Pronto",
    [UI_MENU_AMIIBO_TAPPED] = "Avvicinato: %s",
    [UI_MENU_AMIIBO_LOAD_FAILED] = "Non è un dump di amiibo (NTAG215 da 540 byte): %s",
    [UI_MENU_AMIIBO_REMOVED] = "Amiibo allontanato",
    [UI_MENU_AMIIBO_STATUS] = "Stato: %s",
    [UI_MENU_MENU_LANG] = "Lingua del menu",
    [UI_MENU_MENU_LANG_DESC] = "Lingua di questo menu e dell'indicatore prestazioni.",
    [UI_MENU_MENU_LANG_AUTO] = "Automatica (da Windows)",
    [UI_MENU_DISCORD_RPC] = "Discord Rich Presence",
    [UI_MENU_DISCORD_RPC_DESC] = "Mostra \"Sta giocando a TotK Recomp\" con il tempo di gioco nel tuo stato Discord "
                                 "mentre il gioco è in esecuzione. discord_app_id e discord_image in totk.ini "
                                 "scelgono l'applicazione.",
    [UI_MENU_ADV_WARNING] = "Queste impostazioni servono per test e risoluzione dei problemi. Cambiale solo se sai "
                            "perché.",
    [UI_MENU_GROUP_SCHED] = "Pianificazione",
    [UI_MENU_GROUP_SHADERS] = "Shader e pipeline",
    [UI_MENU_GROUP_TEXMEM] = "Texture e memoria",
    [UI_MENU_GROUP_FIXES] = "Correzioni di rendering",
    [UI_MENU_GROUP_VULKAN] = "Vulkan",
    [UI_MENU_GROUP_DIAG] = "Diagnostica",
    [UI_MENU_ASYNC_PREEMPT] = "Prelazione dei core virtuali",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "Permette a un thread del gioco di interromperne un altro sullo stesso core "
                                   "virtuale.",
    [UI_MENU_PREEMPT_OFF] = "No",
    [UI_MENU_PREEMPT_ON] = "Sì",
    [UI_MENU_PREEMPT_INHERIT] = "Solo ereditarietà",
    [UI_MENU_REPLAY_ASYNC] = "Preparazione fotogrammi GPU su thread proprio",
    [UI_MENU_REPLAY_ASYNC_DESC] = "Prepara i fotogrammi per la GPU su un thread separato invece che sul thread del "
                                  "gioco. Aggiunge fino a un fotogramma di latenza.",
    [UI_MENU_REPLAY_WORKERS] = "Thread di preparazione GPU",
    [UI_MENU_REPLAY_WORKERS_DESC] = "Registra i fotogrammi grandi per la GPU su più thread contemporaneamente. Aiuta "
                                    "quando la CPU limita i fotogrammi al secondo; richiede 8 o più processori "
                                    "logici. No registra su un solo thread.",
    [UI_MENU_REPLAY_WORKERS_NOW] = "ora: %s",
    [UI_MENU_TIMER_1MS] = "Timer di Windows a 1 ms",
    [UI_MENU_TIMER_1MS_DESC] = "Porta la risoluzione del timer di sistema a 1 ms per un ritmo dei fotogrammi più "
                               "regolare.",
    [UI_MENU_SYNC_AHEAD] = "Un fotogramma avanti alla GPU",
    [UI_MENU_SYNC_AHEAD_DESC] = "Il gioco prepara il fotogramma successivo mentre la scheda video disegna ancora il precedente. Più fotogrammi al secondo, un fotogramma in più di ritardo dell'input.",
    [UI_MENU_HOST_CHECKPOINT] = "Cessione rapida del core",
    [UI_MENU_HOST_CHECKPOINT_DESC] = "Un thread del gioco cede il suo core virtuale a uno più importante appena esce dal codice del port, non alla sua prossima chiamata di sistema.",
    [UI_MENU_PIPE_WAIT] = "Gestione dei draw in attesa",
    [UI_MENU_PIPE_WAIT_DESC] = "Predefinito mantiene la pianificazione attuale con un worker riservato ai draw in "
                               "attesa; Vecchio (draw) usa la pianificazione precedente.",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "Predefinito",
    [UI_MENU_PIPE_WAIT_DRAW] = "Vecchio (draw)",
    [UI_MENU_PIPE_SKIP] = "Draw saltati durante la creazione di una pipeline",
    [UI_MENU_PIPE_SKIP_DESC] = "Quali draw vengono scartati invece di attendere la loro pipeline.",
    [UI_MENU_PIPE_SKIP_STRICT] = "Rigoroso",
    [UI_MENU_PIPE_SKIP_ANY] = "Qualsiasi",
    [UI_MENU_PIPE_SKIP_FULLSCREEN] = "Passate a schermo intero",
    [UI_MENU_LIB_PREBUILD] = "Precompilazione librerie di stadi",
    [UI_MENU_LIB_PREBUILD_DESC] = "Compila in anticipo in background le librerie di stadi degli shader.",
    [UI_MENU_LIB_PREBUILD_THREADS] = "Thread di precompilazione librerie",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "Thread usati per la precompilazione; Automatico sceglie un numero in base "
                                          "alla CPU.",
    [UI_MENU_PIPE_PREBUILD] = "Precompilazione pipeline",
    [UI_MENU_PIPE_PREBUILD_DESC] = "Compila in anticipo le pipeline note.",
    [UI_MENU_PREWARM_XLATE] = "Preriscaldamento del traduttore",
    [UI_MENU_PREWARM_XLATE_DESC] = "Thread che traducono gli shader noti all'avvio; Automatico usa metà dei thread "
                                   "della CPU, al massimo 16.",
    [UI_MENU_GPL] = "Libreria di pipeline grafiche",
    [UI_MENU_GPL_DESC] = "Usa l'estensione Vulkan per le librerie di pipeline; disattivala se il driver non funziona "
                         "bene con essa.",
    [UI_MENU_ASTC] = "Decodifica texture ASTC",
    [UI_MENU_ASTC_DESC] = "Dove vengono decodificate le texture compresse ASTC.",
    [UI_MENU_ASTC_AUTO] = "Automatica",
    [UI_MENU_ASTC_GPU] = "Shader GPU",
    [UI_MENU_ASTC_NATIVE] = "Nativa",
    [UI_MENU_ASTC_CPU] = "CPU",
    [UI_MENU_MEMORY] = "Modalità memoria GPU",
    [UI_MENU_MEMORY_DESC] = "Come la memoria del gioco viene condivisa con la GPU.",
    [UI_MENU_MEMORY_AUTO] = "Automatica",
    [UI_MENU_MEMORY_IMPORT] = "Importa",
    [UI_MENU_MEMORY_MIRROR] = "Specchio",
    [UI_MENU_MEMORY_VERIFY] = "Verifica",
    [UI_MENU_TEX_ALIAS] = "Alias delle texture",
    [UI_MENU_TEX_ALIAS_DESC] = "Una nuova texture creata sulla memoria di una texture attiva parte con quel contenuto "
                               "invece che vuota.",
    [UI_MENU_TEX_VRAM_PRESSURE] = "Limite memoria texture",
    [UI_MENU_TEX_VRAM_PRESSURE_DESC] = "Percentuale di memoria video oltre la quale le texture rilasciate dal gioco "
                                       "vengono scartate (tornano dall'archivio quando servono di nuovo). 0 le "
                                       "mantiene.",
    [UI_MENU_ASTC_BC] = "Comprimi ASTC in BC3",
    [UI_MENU_ASTC_BC_DESC] = "Con il decodificatore GPU: ricodifica le texture ASTC decodificate in BC3, un quarto "
                             "della memoria video. Per GPU con 4 GB o meno; le texture appaiono un po' più squadrate.",
    [UI_MENU_LOD_GUARD] = "Protezione LOD",
    [UI_MENU_LOD_GUARD_DESC] = "Evita che gli oggetti lampeggino nel posto sbagliato o che lo sfondo lontano appaia "
                               "piatto per un fotogramma, attendendo che il gioco finisca di riscrivere i dati del "
                               "livello di dettaglio prima che la GPU li legga.",
    [UI_MENU_FIX_C0] = "Correzione del buffer delle costanti",
    [UI_MENU_FIX_C0_DESC] = "Riempie il banco di costanti c0 riservato al driver (dimensioni di storage, dimensione "
                            "del dispatch, dimensione dei punti, istanza base) invece di leggere zeri.",
    [UI_MENU_FIX_ZERO] = "Azzera la nuova memoria GPU",
    [UI_MENU_FIX_ZERO_DESC] = "I nuovi pool di memoria GPU partono riempiti di zeri, come la memoria di Switch.",
    [UI_MENU_DLAA_EFFECTS_AFTER] = "Particelle dopo DLAA",
    [UI_MENU_DLAA_EFFECTS_AFTER_DESC] = "Con DLAA, polvere e altre particelle vengono disegnate sopra l'immagine già "
                                        "levigata da DLSS, come le disegna il gioco senza DLSS: le piccole particelle "
                                        "nella nebbia (sacrari) non spariscono più né lasciano scie. Nebbia e nuvole "
                                        "passano ancora per DLSS, così i bordi lontani contro il cielo restano "
                                        "puliti. No: DLSS elabora anche le particelle, come prima. Solo in modalità "
                                        "DLAA; le modalità DLSS con upscaling le elaborano sempre. Si applica subito.",
    [UI_MENU_DLSS_FIXED_EXPOSURE] = "Esposizione DLSS fissa",
    [UI_MENU_DLSS_FIXED_EXPOSURE_DESC] = "DLSS prende la luminosità dell'immagine del gioco così com'è invece di "
                                         "stimare l'esposizione da ogni fotogramma: nelle scene buie (il Sottosuolo) "
                                         "le ampie zone nella nebbia non diventano più scure o chiare per un "
                                         "fotogramma mentre la telecamera si muove. No: DLSS stima l'esposizione da "
                                         "sé, come prima. Si applica subito.",
    [UI_MENU_BIND_SKIP] = "Salta i binding ripetuti",
    [UI_MENU_BIND_SKIP_DESC] = "Ottimizzazione: salta i binding che non sono cambiati.",
    [UI_MENU_SHADER_MEMO] = "Memo degli shader",
    [UI_MENU_SHADER_MEMO_DESC] = "Ottimizzazione: ricorda le ricerche degli shader.",
    [UI_MENU_PIPE_MEMO] = "Memo delle pipeline",
    [UI_MENU_PIPE_MEMO_DESC] = "Ottimizzazione: riusa la pipeline del draw precedente quando il suo stato si ripete.",
    [UI_MENU_TEX_MEMO] = "Memo dei binding delle texture",
    [UI_MENU_TEX_MEMO_DESC] = "Ottimizzazione: ricorda i descrittori delle texture tra i draw.",
    [UI_MENU_DESC_TEMPLATE] = "Modelli di descrittori",
    [UI_MENU_DESC_TEMPLATE_DESC] = "Ottimizzazione: aggiorna i descrittori in blocco.",
    [UI_MENU_PUSH_SKIP] = "Salta le push constant ripetute",
    [UI_MENU_PUSH_SKIP_DESC] = "Ottimizzazione: salta le push constant che non sono cambiate.",
    [UI_MENU_SPLIT_DRAWS] = "Invio anticipato alla GPU",
    [UI_MENU_SPLIT_DRAWS_DESC] = "Invia il lavoro GPU a parti ogni N draw tra le passate perché la GPU inizi prima; 0 "
                                 "lo disattiva.",
    [UI_MENU_VK_VALIDATION] = "Livello di convalida",
    [UI_MENU_VK_VALIDATION_DESC] = "Controlla ogni chiamata Vulkan e rallenta molto il gioco.",
    [UI_MENU_VK_VALIDATION_MISSING] = "Il livello di convalida non è installato.",
    [UI_MENU_VK_DYN_VERTEX] = "Stato dei vertici dinamico",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "Forza l'input dei vertici dinamico attivo o disattivo; il valore predefinito "
                                   "dipende dalla GPU.",
    [UI_MENU_VK_DYN_A2C] = "Alpha to coverage dinamico",
    [UI_MENU_VK_DYN_A2C_DESC] = "Forza l'alpha to coverage dinamico attivo o disattivo. Predefinito: disattivo con il "
                                "driver AMD per RDNA 2 e precedenti, che con esso disegna i personaggi neri, attivo "
                                "altrove.",
    [UI_MENU_VK_SUBGROUP_PIN] = "Fissa la dimensione dei subgroup",
    [UI_MENU_VK_SUBGROUP_PIN_DESC] = "Fissa la dimensione dei subgroup degli shader.",
    [UI_MENU_TRACE] = "Registro prestazioni",
    [UI_MENU_TRACE_DESC] = "Scrive i tempi dei fotogrammi in un file.",
    [UI_MENU_FLIGHT] = "Scatola nera",
    [UI_MENU_FLIGHT_DESC] = "Mantiene una breve cronologia e la salva quando un fotogramma si blocca.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "Soglia di blocco (ms)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "Tempo del fotogramma che conta come blocco.",
    [UI_MENU_PASS_CRUMBS] = "Marcatori passate GPU",
    [UI_MENU_PASS_CRUMBS_DESC] = "Dopo un crash della GPU, il log indica la passata di rendering in corso. "
                                 "Automatico: solo AMD. Sì: anche NVIDIA, con un piccolo costo.",
    [UI_MENU_INPUT_RECORD] = "Registrazione degli input",
    [UI_MENU_INPUT_RECORD_DESC] = "Scrive i tuoi input in un file per le segnalazioni di bug.",
    [UI_MENU_OPEN_LOG] = "Apri cartella dei log",
    [UI_MENU_OPEN_LOG_DESC] = "Mostra la cartella con i file di log.",
    [UI_MENU_COPY_INFO] = "Copia info di sistema",
    [UI_MENU_COPY_INFO_DESC] = "Copia negli appunti GPU, driver, CPU, RAM e impostazioni modificate.",
    [UI_MENU_COPY_INFO_DONE] = "Copiato",
    [UI_MENU_ACTION_FAILED] = "non ha funzionato",
    [UI_MENU_RESET] = "Ripristina tutte le impostazioni",
    [UI_MENU_RESET_DESC] = "Vale per tutte le schede.",
    [UI_MENU_RESET_CONFIRM] = "Ripristinare ogni impostazione al valore predefinito?",
    [UI_MENU_RESET_YES] = "Ripristina",
    [UI_MENU_RESET_NO] = "Annulla",
    [UI_MENU_OV_FPS] = "%.0f FPS",
    [UI_MENU_OV_FRAME_MS] = "%.1f ms",
    [UI_MENU_OV_GAME_PCT] = "gioco %.0f%%",
    [UI_MENU_OV_MAX_MS] = "max %.1f ms",
    [UI_MENU_OV_FPS_LABEL] = "FPS",
    [UI_MENU_OV_FRAME_LABEL] = "Fotogramma",
    [UI_MENU_OV_FMT_FPS] = "%.0f",
    [UI_MENU_OV_LOW1] = "1%% min %.0f",
    [UI_MENU_OV_LOW01] = "0,1%% min %.0f",
    [UI_MENU_OV_GPU] = "GPU",
    [UI_MENU_OV_CPU] = "CPU",
    [UI_MENU_OV_RAM] = "RAM",
    [UI_MENU_OV_VRAM] = "VRAM",
    [UI_MENU_OV_TEMP] = "Temperatura",
    [UI_MENU_OV_POWER] = "Potenza",
    [UI_MENU_OV_CLOCK] = "Frequenza",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "Preparazione fotogrammi GPU",
    [UI_MENU_OV_GPU_TIME] = "GPU occupata per fotogramma",
    [UI_MENU_OV_PIPE_WAIT] = "Attese pipeline",
    [UI_MENU_OV_SHADER_WAIT] = "Attese shader",
    [UI_MENU_OV_TEX_UPLOADS] = "Caricamenti texture",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%",
    [UI_MENU_OV_FMT_TEMP] = "%.0f °C",
    [UI_MENU_OV_FMT_POWER] = "%.0f W",
    [UI_MENU_OV_FMT_MHZ] = "%.0f MHz",
    [UI_MENU_OV_FMT_GB] = "%.1f GB",
    [UI_MENU_OV_FMT_GB_OF] = "/ %.1f GB",
    [UI_MENU_OV_GAME_GB] = "gioco %.1f GB",
    [UI_MENU_OV_FMT_MS] = "%.2f ms",
    [UI_MENU_OV_PER_FRAME] = "%.1f per fotogramma",
    [UI_MENU_OV_WINDOW] = "Finestra",
    [UI_MENU_OV_OUTPUT] = "Immagine del gioco",
    [UI_MENU_OV_SCENE] = "Scena 3D",
    [UI_MENU_OV_UPSCALER] = "Upscaler 3D",
    [UI_MENU_OV_SCALING] = "Ridimensionamento",
    [UI_MENU_OV_LIMIT] = "Limite fotogrammi",
    [UI_MENU_OV_PRESENT] = "Presentazione",
    [UI_MENU_OV_SHOWN] = "Mostrati",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u",
    [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, nitidezza %d%%",
    [UI_MENU_OV_SCALE_NONE] = "nessuno (1:1)",
    [UI_MENU_OV_SCALE_BILINEAR] = "bilineare",
    [UI_MENU_OV_SCALE_DOWN] = "bilineare (FSR solo ingrandisce)",
    [UI_MENU_TAB_CHEATS] = "Trucchi",
    [UI_MENU_CHEATS_NONE] = "Nessun trucco per questa versione del gioco. I trucchi di una mod sono un file di "
                            "trucchi Atmosphère nella sua cartella cheats, con il nome della build del gioco: "
                            "mods\\<mod>\\cheats\\%s. Riavvia il gioco per leggerli. Per i trucchi che cambiano il "
                            "codice del gioco, il gioco si ricostruisce all'avvio (qualche minuto).",
    [UI_MENU_CHEATS_MOD] = "%s: %d trucchi",
    [UI_MENU_CHEAT_CODE_DESC] = "Cambia il codice del gioco. Si attiva subito.",
    [UI_MENU_CHEAT_KEY_DESC] = "Cambia il codice del gioco mentre tieni premuto %s. Si attiva subito.",
    [UI_MENU_CHEAT_DATA_DESC] = "Scrive nella memoria del gioco a ogni fotogramma finché è attivo.",
    [UI_MENU_CHEAT_MASTER] = "sempre attivo",
    [UI_MENU_CHEAT_MASTER_DESC] = "Il codice master del file: è sempre attivo.",
    [UI_MENU_CHEAT_RESTORE] = "non serve",
    [UI_MENU_CHEAT_RESTORE_DESC] = "Riscrive solo il codice originale del gioco. Disattivare un trucco lo fa già.",
    [UI_MENU_CHEAT_REBUILD] = "dopo la ricostruzione",
    [UI_MENU_CHEAT_REBUILD_DESC] = "Questo trucco cambia il codice del gioco ed è nuovo o modificato da quando il "
                                   "gioco è stato costruito. Il gioco lo integra al prossimo avvio (qualche minuto), "
                                   "oppure usa Ricostruisci il gioco ora.",
    [UI_MENU_CHEAT_UNSUPPORTED] = "non supportato",
    [UI_MENU_CHEAT_UNSUPPORTED_DESC] = "Questo trucco non può essere usato con questa versione del gioco:",
    [UI_MENU_CHEATS_REBUILD] = "Ricostruisci il gioco ora",
    [UI_MENU_CHEATS_REBUILD_DESC] = "Alcuni trucchi cambiano il codice del gioco e non vi sono integrati: sono nuovi "
                                    "o modificati, oppure la loro ultima ricostruzione è fallita o è stata annullata. "
                                    "Chiude il gioco; l'installazione li integra (qualche minuto) e riavvia il gioco. "
                                    "I progressi dall'ultimo salvataggio vanno persi.",
    [UI_MENU_MODS_EXEFS_UNSUPPORTED] = "Questa mod modifica il codice del gioco (exefs) in un modo che il port non "
                                       "può applicare, quindi l'intera mod resta disattivata. %s",
    [UI_MENU_MODS_EXEFS_OTHER] = "Le sue patch del codice (exefs) sono fatte per un'altra versione del gioco.",
    [UI_MENU_MODS_EXEFS_ON] = "Patch del codice in uso: %d file.",
    [UI_MENU_MODS_REPLACES_BUILTIN] = "Patch del codice in uso: %d file. Sostituiscono la patch %s del port, che "
                                      "resta disattivata finché questa mod è attiva.",
    [UI_MENU_MODS_CODE_SKIPPED] = "Il suo codice di gioco (%s) non viene caricato: il resto della mod viene usato. "
                                  "Disattiva la mod se il gioco si comporta in modo strano.",
    [UI_MENU_MODS_PLUGIN_ON] = "Il suo plugin è attivo: %d hook nel codice del gioco.",
    [UI_MENU_MODS_PLUGIN_REFUSED] = "Il suo plugin %s non può funzionare nel port: %s.",
    [UI_MENU_MODS_PLUGIN_STAYS_OFF] = "L'intera mod resta disattivata. Il suo interruttore \"Usa ... senza il suo "
                                      "codice\" qui sotto ne usa il resto.",
    [UI_MENU_MODS_PLUGIN_OFF] = "Il suo plugin questa volta resta disattivato: si è avviato in modo diverso dalla "
                                "build (%s). Il gioco lo ricostruisce al prossimo avvio.",
    [UI_MENU_MODS_WITHOUT_CODE] = "Usa %s senza il suo codice",
    [UI_MENU_MODS_WITHOUT_CODE_DESC] = "La mod qui sopra porta codice che il port non può eseguire: il proprio codice "
                                       "di gioco (un plugin subsdk o un main sostituito) o patch del codice (exefs) "
                                       "che il port non può applicare. No: l'intera mod resta disattivata. Sì: quel "
                                       "codice viene escluso e il resto della mod viene usato: i suoi file RomFS, i "
                                       "trucchi e le patch del codice che funzionano. I suoi file potrebbero "
                                       "aspettarsi il codice mancante; disattivala di nuovo se il gioco si comporta "
                                       "in modo strano. Si applica al prossimo avvio.",
    [UI_MENU_MODS_WITHOUT_CODE_ALL] = "Attivo per ogni mod: mods_without_code = 1 in totk.ini.",
    [UI_MENU_MODS_EXEFS_SKIPPED] = "%u dei suoi file di patch del codice (exefs) non possono essere applicati e "
                                   "vengono esclusi: il resto della mod viene usato. %s",
    [UI_MENU_MODS_OVERRIDES_FILES] = "%u dei suoi file sostituiscono quelli di %s, che è più in basso nell'elenco.",
    [UI_MENU_MODS_DATA_OVERRIDDEN] = "%u byte delle sue modifiche ai dati del gioco provengono da %s, che è più in "
                                     "alto nell'elenco.",
    [UI_MENU_MODS_OVERRIDES_DATA] = "%u byte delle sue modifiche ai dati del gioco sostituiscono quelle di %s, che è "
                                    "più in basso nell'elenco.",
    [UI_MENU_MODS_CODE_OVERRIDDEN] = "%u dei suoi punti di patch del codice sono sostituiti da %s, che è più in alto "
                                     "nell'elenco.",
    [UI_MENU_MODS_OVERRIDES_CODE] = "Sostituisce %u punti di patch del codice di %s, che è più in basso nell'elenco.",
};

// Dutch
static const char *const text_nl[UI_TEXT_COUNT] = {
    SWKBD("Tekstinvoer", "Voer een naam in", "Paard", "OK", "Annuleren", "Controller: A = OK, B = Annuleren"),

    [UI_MENU_TITLE] = "Instellingen",
    [UI_MENU_TAB_SCREEN] = "Scherm",
    [UI_MENU_TAB_GAME] = "Spel",
    [UI_MENU_TAB_MODS] = "Mods",
    [UI_MENU_TAB_SOUND] = "Geluid",
    [UI_MENU_TAB_CONTROLS] = "Besturing",
    [UI_MENU_TAB_AMIIBO] = "Amiibo",
    [UI_MENU_TAB_MENU] = "Menu",
    [UI_MENU_TAB_ADVANCED] = "Geavanceerd",
    [UI_MENU_CONTINUE] = "Doorgaan",
    [UI_MENU_QUIT] = "Spel afsluiten",
    [UI_MENU_ON] = "Aan",
    [UI_MENU_OFF] = "Uit",
    [UI_MENU_AUTO] = "Automatisch",
    [UI_MENU_DEFAULT_MARK] = "standaard",
    [UI_MENU_AFTER_RESTART] = "na herstart",
    [UI_MENU_SET_BY_ENV] = "ingesteld door omgevingsvariabele",
    [UI_MENU_SAVE_FAILED] = "instellingen konden niet worden opgeslagen",
    [UI_MENU_VERSION] = "TotK Recomp %s, spel %s",
    [UI_MENU_FULLSCREEN] = "Volledig scherm",
    [UI_MENU_FULLSCREEN_DESC] = "Het spel vult het scherm; F11 schakelt het om.",
    [UI_MENU_WINDOW_SIZE] = "Venstergrootte",
    [UI_MENU_WINDOW_SIZE_DESC] = "Grootte van het spelvenster buiten volledig scherm.",
    [UI_MENU_WINDOW_SIZE_CUSTOM] = "Aangepast",
    [UI_MENU_PRESENT] = "Weergave",
    [UI_MENU_PRESENT_DESC] = "Hoe voltooide beelden het scherm bereiken.",
    [UI_MENU_PRESENT_MAILBOX] = "Mailbox",
    [UI_MENU_PRESENT_VSYNC] = "VSync",
    [UI_MENU_PRESENT_IMMEDIATE] = "Direct",
    [UI_MENU_PRESENT_IN_USE] = "in gebruik: %s",
    [UI_MENU_GPU] = "Videokaart",
    [UI_MENU_GPU_DESC] = "Het Vulkan-apparaat dat het spel tekent.",
    [UI_MENU_GPU_AUTO] = "Automatisch",
    [UI_MENU_PERF_OVERLAY] = "Prestatieweergave",
    [UI_MENU_PERF_OVERLAY_DESC] = "Beelden per seconde rechtsboven. Volledig voegt beeldtijden met een grafiek toe, "
                                  "CPU-, GPU- en geheugenbelasting, temperaturen en de wachttijden van de "
                                  "renderthread. F10 wisselt de modi.",
    [UI_MENU_PERF_OFF] = "Uit",
    [UI_MENU_PERF_FPS] = "FPS",
    [UI_MENU_PERF_FULL] = "Volledig",
    [UI_PERF_LINE_FPS] = "%.0f fps",
    [UI_PERF_LINE_FPS_GEN] = "%.0f fps getekend, %.0f getoond",
    [UI_MENU_GRAPHICS] = "Beeld",
    [UI_MENU_OUTPUT_RES] = "Spelresolutie",
    [UI_MENU_OUTPUT_RES_DESC] = "De grootte waarin het spel zijn beeld en interface tekent. Boven 1920 x 1080 heeft "
                                "het spel meer videogeheugen en GPU-tijd nodig. Een breder formaat (21:9) verbreedt "
                                "het zicht van de camera en zet de HUD in de hoeken van het scherm; een hoger formaat "
                                "(16:10, Steam Deck) zet hem aan de boven- en onderrand.",
    [UI_MENU_HUD_WIDTH] = "HUD-breedte",
    [UI_MENU_HUD_WIDTH_DESC] = "Op een scherm breder dan 16:9: hoe ver de linker- en rechterkant van de HUD uit "
                               "elkaar staan. Volledig scherm zet ze aan de randen van het scherm; 21:9 of 16:9 houdt "
                               "de HUD binnen het midden van een scherm met die vorm. Een ontbrekende indeling wordt "
                               "op de achtergrond gemaakt en na een herstart gebruikt.",
    [UI_MENU_HUD_WIDTH_FULL] = "Volledig scherm",
    [UI_MENU_RENDER_SCALE] = "3D-renderschaal",
    [UI_MENU_RENDER_SCALE_DESC] = "Grootte van de 3D-scène ten opzichte van de spelresolutie; de FSR van het spel "
                                  "schaalt hem naar die grootte. Boven 100 % worden kartelranden gladder "
                                  "(supersampling) tegen hoge GPU-kosten.",
    [UI_MENU_RENDER_SCALE_GAME] = "Spel (83 %)",
    [UI_MENU_DYNAMIC_RES] = "Dynamische resolutie",
    [UI_MENU_DYNAMIC_RES_DESC] = "Laat het spel de 3D-scène verlagen naar 90 of 80 %. Het oordeelt op basis van een "
                                 "schatting voor de Switch die je GPU negeert, dus brede scènes zakken zelfs op een "
                                 "snelle pc. Uit houdt de renderschaal.",
    [UI_MENU_UPSCALER] = "Schalen naar het venster",
    [UI_MENU_UPSCALER_DESC] = "Hoe het beeld wordt geschaald als het venster groter is dan de spelresolutie. FSR "
                              "houdt randen en details scherper dan bilineair.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1",
    [UI_MENU_UPSCALER_BILINEAR] = "Bilineair",
    [UI_MENU_ASPECT] = "Beeldverhouding",
    [UI_MENU_ASPECT_DESC] = "Als de vorm van het venster afwijkt van die van de spelresolutie (een 16:9-beeld op een "
                            "21:9-scherm): de vorm behouden met zwarte balken, of het beeld over het venster "
                            "uitrekken.",
    [UI_MENU_ASPECT_FIT] = "Zwarte balken",
    [UI_MENU_ASPECT_STRETCH] = "Uitrekken",
    [UI_MENU_MOVIE_ASPECT] = "Filmvorm",
    [UI_MENU_MOVIE_ASPECT_DESC] = "Als de spelresolutie niet 16:9 is: de eigen vorm van de tussenfilmpjes behouden "
                                  "met zwarte balken, of ze over het hele beeld uitrekken. Werkt vanaf het volgende "
                                  "filmpje.",
    [UI_MENU_MOVIE_ASPECT_KEEP] = "Zwarte balken",
    [UI_MENU_MOVIE_ASPECT_STRETCH] = "Uitrekken",
    [UI_MENU_FOV] = "Gezichtsveld",
    [UI_MENU_FOV_DESC] = "Hoeveel van de wereld de camera toont, in procenten van dat van het spel. Alleen spelversie "
                         "1.4.3.",
    [UI_MENU_VERT_PLUS] = "16:9-breedte behouden",
    [UI_MENU_VERT_PLUS_DESC] = "Op schermen die smaller zijn dan 16:9 (Steam Deck, 16:10, 4:3) toont de camera dezelfde breedte van de wereld als bij 16:9, en meer erboven en eronder. Geen effect bij 16:9 of breder.",
    [UI_MENU_SHADOW_RES] = "Grootte van de schaduwkaart",
    [UI_MENU_SHADOW_RES_DESC] = "Grootte van de schaduwkaart van de zon. De grootte van het spel verandert met het "
                                "zicht, tot ongeveer 400 dicht bij de grond, waardoor bladschaduwen blokkerig ogen en "
                                "verspringen; een vaste grootte houdt ze stil en scherper. Boven 1024 kosten de "
                                "schaduwen meer GPU-tijd en videogeheugen.",
    [UI_MENU_SHADOW_RES_GAME] = "Spel (wisselend)",
    [UI_MENU_ANISO] = "Textuurfiltering",
    [UI_MENU_ANISO_DESC] = "Anisotrope filtering van oppervlaktetexturen: grond, wegen en muren onder een schuine "
                           "hoek blijven tot in de verte scherp in plaats van te vervagen. Spel: het niveau dat het "
                           "spel vraagt, zoals op de Switch.",
    [UI_MENU_ANISO_GAME] = "Spel",
    [UI_MENU_LOD_STEADY] = "Stabiel detail bij belasting",
    [UI_MENU_LOD_STEADY_DESC] = "Slaat de regeling van het spel over die model- en textuurdetail verlaagt als de "
                                "beeldsnelheid daalt, zoals de emulatormod Disable LOD Quality Reduction doet. "
                                "Spelversies 1.1.0, 1.2.1 en 1.4.3. Laat het uit op zwakke pc's: daar houdt deze verlaging "
                                "de beeldsnelheid op peil.",
    [UI_MENU_SHARPNESS] = "FSR-scherpte",
    [UI_MENU_SHARPNESS_DESC] = "Verscherpen na FSR-schaling; 0 zet het uit.",
    [UI_MENU_COLOR] = "Kleur",
    [UI_MENU_COLOR_PRESET] = "Kleurprofiel",
    [UI_MENU_COLOR_PRESET_DESC] = "Stelt de vier waarden hieronder in. Levendig: meer contrast en verzadiging, "
                                  "schaduwen iets lichter (voor OLED-schermen). Helderheid: iets meer contrast, "
                                  "verzadiging en helderheid. Een waarde wijzigen maakt het profiel Aangepast. De HUD "
                                  "verandert mee met het beeld; dit menu niet.",
    [UI_MENU_COLOR_DEFAULT] = "Van het spel",
    [UI_MENU_COLOR_VIVID] = "Levendig (OLED)",
    [UI_MENU_COLOR_CLARITY] = "Helder",
    [UI_MENU_COLOR_CUSTOM] = "Aangepast",
    [UI_MENU_COLOR_BRIGHTNESS] = "Helderheid",
    [UI_MENU_COLOR_BRIGHTNESS_DESC] = "Wordt bij elke kleur opgeteld; 0 laat haar ongewijzigd.",
    [UI_MENU_COLOR_CONTRAST] = "Contrast",
    [UI_MENU_COLOR_CONTRAST_DESC] = "Duwt kleuren weg van middengrijs (boven 100 %) of ernaartoe; 100 % laat ze "
                                    "ongewijzigd.",
    [UI_MENU_COLOR_SATURATION] = "Verzadiging",
    [UI_MENU_COLOR_SATURATION_DESC] = "Kleursterkte ten opzichte van grijs; 100 % laat haar ongewijzigd.",
    [UI_MENU_COLOR_GAMMA] = "Gamma",
    [UI_MENU_COLOR_GAMMA_DESC] = "Onder 1 worden donkere en middentonen lichter, boven 1 donkerder; 1 laat ze "
                                 "ongewijzigd.",
    [UI_MENU_HDR_GROUP] = "HDR",
    [UI_MENU_HDR] = "HDR-uitvoer",
    [UI_MENU_HDR_DESC] = "Felle hooglichten op een HDR-scherm. Vereist HDR aan in Windows (\"HDR gebruiken\") of een "
                         "Wayland-compositor met kleurbeheer; anders blijft het beeld SDR. Werkt na een herstart. Het "
                         "kleurprofiel hierboven is voor SDR en wordt in HDR niet gebruikt.",
    [UI_MENU_HDR_PEAK] = "Piekhelderheid",
    [UI_MENU_HDR_PEAK_DESC] = "De piek van je scherm in nits; de felste hooglichten stoppen daar.",
    [UI_MENU_HDR_GAME] = "Spelhelderheid",
    [UI_MENU_HDR_GAME_DESC] = "Nits van het wit van het beeld (papierwit). 203 is de HDR-referentie; verhoog het in "
                              "een lichte kamer.",
    [UI_MENU_HDR_UI] = "Helderheid van HUD en menu",
    [UI_MENU_HDR_UI_DESC] = "Nits van het wit van de HUD en dit menu.",
    [UI_MENU_HDR_HUE] = "Tintcorrectie hooglichten",
    [UI_MENU_HDR_HUE_DESC] = "Felle kleuren zoals vuur en vonken neigen naar wit, zoals in het eigen SDR-beeld van "
                             "het spel, passend bij het licht dat ze verspreiden; 0 houdt ze volledig verzadigd.",
    [UI_MENU_SHADOW_SCALE] = "Schaduwresolutie",
    [UI_MENU_SHADOW_SCALE_DESC] = "Tekent de schaduwkaarten van de zon per zijde zoveel keer groter dan de 960x960 "
                                  "van het spel: scherpere schaduwranden en bladschaduwen. Werkt bij de volgende "
                                  "start.",
    [UI_MENU_PCSS] = "Zachte schaduwen",
    [UI_MENU_PCSS_DESC] = "Zonneschaduwen scherp waar een voorwerp de grond raakt en zachter naarmate de schaduw "
                          "verder valt (PCSS). 0 houdt het filter van het spel. Werkt bij de volgende start.",
    [UI_MENU_CONTACT_SHADOWS] = "Contactschaduwen",
    [UI_MENU_CONTACT_SHADOWS_DESC] = "Kleine zonneschaduwen die de schaduwkaarten missen: bij voeten, onder stenen, "
                                     "in gras en plooien. Na de belichting op het scherm berekend. Werkt bij de "
                                     "volgende start.",
    [UI_MENU_SHADOW_RANGE] = "Bereik van schaduwcascades",
    [UI_MENU_SHADOW_RANGE_DESC] = "Verlegt de stappen waar schaduwen zachter worden (in het spel 20, 40, 80 en 160 m) "
                                  "verder weg. Spreidt de schaduwkaart over meer grond: verhoog de schaduwresolutie "
                                  "mee. Werkt bij de volgende start.",
    [UI_MENU_GRASS_RANGE] = "Grasbereik",
    [UI_MENU_GRASS_RANGE_DESC] = "Hoe ver rond de camera gras groeit (in het spel: 62,5 m). Hoger houdt het gras in "
                                 "weiden verder weg zichtbaar in plaats van het pas vlak voor je te laten opgroeien; "
                                 "lager tekent er minder van. Werkt bij de volgende start.",
    [UI_MENU_DLSS_PRESET] = "DLSS-model",
    [UI_MENU_DLSS_PRESET_DESC] = "Welk DLSS-model draait. Automatisch volgt het advies van NVIDIA per modus: K voor "
                                 "DLAA, Kwaliteit en Gebalanceerd, M voor Prestaties, L voor Ultraprestaties. J: iets "
                                 "minder ghosting achter bewegende personages, iets meer flikkering. K: het scherpste "
                                 "model in elke modus. Werkt meteen.",
    [UI_MENU_DLSS_PRESET_DEFAULT] = "Automatisch",
    [UI_MENU_DLSS_PRESET_K] = "K (scherpst)",
    [UI_MENU_DLSS_PRESET_J] = "J (minder ghosting)",
    [UI_MENU_DLSS_CHARS] = "DLSS-sporen van personages",
    [UI_MENU_DLSS_CHARS_DESC] = "Vermindert de vage sporen achter bewegende personages, dieren en wat ze vasthouden: "
                                "DLSS neemt daar minder over van de vorige beelden. Een sterkere stand verwijdert "
                                "meer van de sporen en laat die randen iets minder glad. Werkt meteen.",
    [UI_MENU_DLSS_CHARS_OFF] = "Uit",
    [UI_MENU_DLSS_CHARS_LIGHT] = "Licht",
    [UI_MENU_DLSS_CHARS_MEDIUM] = "Gemiddeld",
    [UI_MENU_DLSS_CHARS_FULL] = "Volledig",
    [UI_MENU_DLSS] = "Upscalingmodus",
    [UI_MENU_DLSS_DESC] = "Native verzacht de randen van de 3D-scène op volledige resolutie. Kwaliteit, Gebalanceerd, "
                          "Prestaties en Ultraprestaties tekenen hem op 67, 58, 50 en 33 % van die resolutie en laten "
                          "de upscaler (DLSS of FSR 3.1) hem opschalen, wat GPU-tijd bespaart; ze vervangen de "
                          "3D-renderschaal. Werkt meteen.",
    [UI_MENU_DLSS_DLAA] = "Native",
    [UI_MENU_DLSS_QUALITY] = "Kwaliteit",
    [UI_MENU_DLSS_BALANCED] = "Gebalanceerd",
    [UI_MENU_DLSS_PERFORMANCE] = "Prestaties",
    [UI_MENU_DLSS_ULTRA] = "Ultraprestaties",
    [UI_MENU_FXAA] = "Anti-aliasing van het spel (FXAA)",
    [UI_MENU_FXAA_DESC] = "De eigen FXAA-randverzachting van het spel. Ze maakt fijne details iets vager; met DLAA is "
                          "ze niet nodig.",
    [UI_MENU_REFLEX] = "Lage latentie",
    [UI_MENU_REFLEX_DESC] = "Verlaagt de systeemlatentie. Aan gebruikt NVIDIA Reflex (VK_NV_low_latency2); Aan + Boost houdt bovendien de klokfrequenties van de NVIDIA-GPU hoog in scènes die door de CPU beperkt zijn. Automatisch gebruikt Reflex op NVIDIA en AMD Anti-Lag 2 (VK_AMD_anti_lag) op AMD. Geldt na een herstart.",
    [UI_MENU_REFLEX_AUTO] = "Automatisch",
    [UI_MENU_REFLEX_OFF] = "Uit",
    [UI_MENU_REFLEX_ON] = "Aan",
    [UI_MENU_REFLEX_BOOST] = "Aan + Boost",
    [UI_MENU_SCENE_UPSCALER] = "Upscaler 3D-scène",
    [UI_MENU_SCENE_UPSCALER_DESC] = "Een temporele upscaler voor de 3D-scène: gladdere randen en minder flikkering, "
                                    "en in een modus onder Native tekent hij de scène kleiner en schaalt hem op, wat "
                                    "GPU-tijd bespaart. NVIDIA DLSS vereist een NVIDIA RTX-GPU en " DLSS_FILE " naast "
                                    EXE_FILE "; AMD FSR 3.1 draait op elke GPU. Uit houdt de eigen schaling en FXAA "
                                    "van het spel. De HUD blijft onaangeroerd; de schaling en scherpte hierboven "
                                    "passen het spelbeeld nog steeds aan het venster aan. Werkt na een herstart.",
    [UI_MENU_SCENE_UPSCALER_OFF] = "Uit",
    [UI_MENU_SCENE_UPSCALER_DLSS] = "NVIDIA DLSS",
    [UI_MENU_SCENE_UPSCALER_FSR3] = "AMD FSR 3.1",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_GPU] = "DLSS: vereist een NVIDIA RTX-GPU",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_DLL] = "DLSS: " DLSS_FILE " ontbreekt",
    [UI_MENU_FSR_SHARPNESS] = "FSR-scherpte",
    [UI_MENU_FSR_SHARPNESS_DESC] = "Verscherping van de 3D-scène na AMD FSR 3.1 of FSR 4; 0 schakelt het uit.",
    [UI_MENU_SCENE_UPSCALER_FSR4] = "AMD FSR 4",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_GPU] = "FSR 4: niet beschikbaar voor deze GPU",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_DLL] = "FSR 4: de FSR-bibliotheken van AMD ontbreken",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_OS] = "FSR 4: alleen op Windows",
    [UI_MENU_FPS_LIMIT] = "Beeldsnelheidslimiet",
    [UI_MENU_FPS_LIMIT_DESC] = "Het hoogste aantal beelden per seconde dat het spel tekent. Boven 60 is experimenteel "
                               "en vereist \"Spelsnelheid volgt de klok\".",
    [UI_MENU_FPS_30] = "30 FPS",
    [UI_MENU_FPS_40] = "40 FPS",
    [UI_MENU_FPS_45] = "45 FPS",
    [UI_MENU_FPS_60] = "60 FPS",
    [UI_MENU_FPS_90] = "90 FPS",
    [UI_MENU_FPS_120] = "120 FPS",
    [UI_MENU_FPS_144] = "144 FPS",
    [UI_MENU_FPS_DISPLAY] = "Monitorfrequentie",
    [UI_MENU_FPS_DISPLAY_HZ] = "Monitorfrequentie (%.0f Hz)",
    [UI_MENU_FRAME_QUEUE] = "Beeldwachtrij",
    [UI_MENU_FRAME_QUEUE_DESC] = "Het spel begint aan een beeld terwijl het vorige op zijn beurt op het scherm wacht: "
                                 "veel minder overgeslagen beelden bij de limiet, tot één beeld meer "
                                 "invoervertraging. Werkt meteen.",
    [UI_MENU_FRAME_GEN] = "Beeldgeneratie",
    [UI_MENU_FRAME_GEN_OFF] = "Uit",
    [UI_MENU_FRAME_GEN_FSR3] = "AMD FSR 3",
    [UI_MENU_FRAME_GEN_DESC] = "Een gegenereerd beeld tussen elke twee die het spel tekent: twee keer zoveel beelden "
                               "op het scherm, het spel op de helft van de beeldlimiet en de besturing ongeveer een "
                               "half beeld later. Voor schermen van 120 Hz en meer. Niet met DLSS of HDR. Werkt "
                               "meteen.",
    [UI_MENU_REALTIME] = "Spelsnelheid volgt de klok",
    [UI_MENU_REALTIME_DESC] = "Laat het spel in echte tijd lopen, zodat een traag beeld het spel niet vertraagt.",
    [UI_MENU_CLOTH_REALTIME] = "Stof op de klok",
    [UI_MENU_CLOTH_REALTIME_DESC] = "Laat ook stof en haar in echte tijd lopen.",
    [UI_MENU_HIGHFPS_FIX] = "Correcties voor hoge beeldsnelheid",
    [UI_MENU_HIGHFPS_FIX_DESC] = "Corrigeert natuurkunde die boven 30 beelden per seconde misgaat, en voorwerpen die "
                                 "boven 60 doorzichtig of onzichtbaar blijven.",
    [UI_MENU_CLIMB_FIX] = "Correctie klimsnelheid",
    [UI_MENU_CLIMB_FIX_DESC] = "Houdt klimmen tegen muren en ladders op volle snelheid boven 60 beelden per seconde, "
                               "zodat Link de top bereikt.",
    [UI_MENU_GAME_LANG] = "Taal van het spel",
    [UI_MENU_GAME_LANG_DESC] = "Taal van de teksten en stemmen van het spel.",
    [UI_MENU_GAME_LANG_AUTO] = "Automatisch",
    [UI_MENU_OPERATION_MODE] = "Consolemodus",
    [UI_MENU_OPERATION_MODE_DESC] = "Of het spel denkt dat het in tv- of handheldmodus draait.",
    [UI_MENU_MODE_DOCKED] = "Tv-modus",
    [UI_MENU_MODE_HANDHELD] = "Handheld",
    [UI_MENU_ASSUME_FOCUS] = "Op de achtergrond doorlopen",
    [UI_MENU_ASSUME_FOCUS_DESC] = "Aan (standaard): het spel loopt door terwijl je in een ander venster bent, en het "
                                  "toetsenbord blijft stil tot het venster weer voorop staat. Uit: het pauzeert als "
                                  "zijn venster de focus verliest, zoals op de Switch bij het indrukken van Home; een "
                                  "zo onderbroken laadactie kan voorgoed vastlopen.",
    [UI_MENU_PLAY_TIME] = "Speeltijd",
    [UI_MENU_PLAY_TIME_DESC] = "Tijd in het spel over alle sessies, geteld zolang het spelvenster de focus heeft en "
                               "het spel beelden tekent. Bewaard in playtime.txt in de profielmap, naast de "
                               "opslagbestanden.",
    [UI_MENU_PLAY_TIME_HM] = "%llu u %02u min",
    [UI_MENU_PLAY_TIME_SESSION] = "deze sessie: %s",
    [UI_MENU_MODS_BUILTIN] = "Ingebouwd in de port",
    [UI_MENU_MODS_INSTALLED] = "Geïnstalleerde mods",
    [UI_MENU_MODS_NONE] = "Nog geen mods. Pak een mod uit in de map mods, één map per mod met de romfs-map erin, en "
                          "start het spel opnieuw. TKMM kan zijn samengevoegde mods daar ook naar een map exporteren.",
    [UI_MENU_MODS_OPEN] = "Modsmap openen",
    [UI_MENU_MODS_OPEN_DESC] = "Toont de map waaruit het spel mods laadt. Hij wordt aangemaakt als hij ontbreekt.",
    [UI_MENU_MODS_ROW_DESC] = "Vervangen spelbestanden: %u. Nieuwe bestanden: %u.",
    [UI_MENU_MODS_OVERRIDDEN] = "%u van zijn bestanden komen van %s, dat hoger in de lijst staat.",
    [UI_MENU_MODS_MOVE_HINT] = "De bovenste knop of Ctrl+Omhoog verplaatst hem omhoog, de linkerknop of Ctrl+Omlaag "
                               "omlaag. Als twee mods hetzelfde bestand wijzigen, wint de bovenste. Wijzigingen "
                               "werken na een herstart.",
    [UI_MENU_MODS_EMPTY] = "Deze map bevat geen romfs-, exefs- of cheats-bestanden.",
    [UI_MENU_MODS_SUBSDK] = "Deze mod brengt eigen spelcode mee (een subsdk-plug-in of een vervangen main). De port "
                            "kan geen codemodules laden, dus de hele mod blijft uit.",
    [UI_MENU_MODS_EXEFS_LATER] = "Deze mod past de spelcode aan (exefs). Het spel bouwt de patches in bij de volgende "
                                 "start (een herbouw van enkele minuten); tot dan blijft de hele mod uit.",
    [UI_MENU_MODS_CHEATS] = "Cheats: %d. Zet ze aan op het tabblad Cheats.",
    [UI_MENU_MODS_CHEATS_FILE] = "Cheats voor deze spelversie worden bij de start gelezen uit cheats\\%s in deze mod.",
    [UI_MENU_MODS_BAD_NAME] = "De mapnaam bevat |, dat de modlijst tussen namen gebruikt. Hernoem de map.",
    [UI_MENU_MODS_OTHER_VERSION] = "%u van zijn bestanden zijn gemaakt voor een andere spelversie en worden "
                                   "weggelaten. Zoek een versie van deze mod voor jouw spelversie.",
    [UI_MENU_MODS_UNUSED] = "niet gebruikt",
    [UI_MENU_MODS_FPS60] = "60 FPS-patch",
    [UI_MENU_MODS_FPS60_DESC] = "De eigen patch van de port voor 60 beelden per seconde. Stel hem in met de "
                                "beeldsnelheidslimiet op het tabblad Spel.",
    [UI_MENU_MODS_RES] = "Resolutiepatch",
    [UI_MENU_MODS_RES_DESC] = "De eigen patch van de port voor resoluties boven 1080p. Stel hem in met de "
                              "spelresolutie op het tabblad Scherm.",
    [UI_MENU_MODS_QUICKMENU] = "Correctie snelmenu",
    [UI_MENU_MODS_QUICKMENU_DESC] = "De correctie van de port voor de zwarte achtergrond van de wapen-, schild- en "
                                    "materiaalmenu's bij 60 FPS.",
    [UI_MENU_VOLUME] = "Volume",
    [UI_MENU_VOLUME_DESC] = "Hoofdvolume van het spel.",
    [UI_MENU_VOLUME_MUSIC] = "Muziekvolume",
    [UI_MENU_VOLUME_MUSIC_DESC] = "Volume van de achtergrondmuziek.",
    [UI_MENU_VOLUME_SFX] = "Effectenvolume",
    [UI_MENU_VOLUME_SFX_DESC] = "Volume van de geluidseffecten.",
    [UI_MENU_VOLUME_VOICE] = "Stemvolume",
    [UI_MENU_VOLUME_VOICE_DESC] = "Volume van de stemmen van personages.",
    [UI_MENU_AUDIO_CHANNELS] = "Luidsprekers",
    [UI_MENU_AUDIO_CHANNELS_DESC] = "Stereo- of 5.1-surrounduitvoer. Automatisch kiest 5.1 als de "
                                    "luidsprekerinstelling van Windows zes of meer kanalen heeft. Werkt na een "
                                    "herstart.",
    [UI_MENU_CHANNELS_AUTO] = "Automatisch",
    [UI_MENU_CHANNELS_STEREO] = "Stereo",
    [UI_MENU_CHANNELS_51] = "5.1 surround",
    [UI_MENU_AUDIO_DEVICE] = "Uitvoerapparaat",
    [UI_MENU_AUDIO_DEVICE_DESC] = "Waar het geluid speelt. Systeemstandaard volgt het standaardapparaat van Windows. "
                                  "Een gekozen apparaat dat is losgekoppeld, valt terug op de systeemstandaard tot "
                                  "het er weer is. Werkt meteen.",
    [UI_MENU_AUDIO_DEVICE_DEFAULT] = "Systeemstandaard",
    [UI_MENU_MOTION] = "Bewegingsbesturing",
    [UI_MENU_MOTION_DESC] = "Richten met de gyroscoop van de controller.",
    [UI_MENU_GYRO_CALIBRATION] = "Kalibratie gyroafwijking",
    [UI_MENU_GYRO_CALIBRATION_DESC] = "Trekt de rustdrift van de controller af, gemeten met de kalibratierij "
                                      "hieronder. De gemeten waarden blijven bewaard als dit uit staat.",
    [UI_MENU_GYRO_CALIBRATE] = "Gyro kalibreren",
    [UI_MENU_GYRO_CALIBRATE_DESC] = "Meet de rustdrift van de gyroscoop en verwijdert die. Leg de controller neer of "
                                    "houd hem twee seconden stil.",
    [UI_MENU_GYRO_CALIBRATE_WAIT] = "Kalibreren...",
    [UI_MENU_GYRO_CALIBRATED] = "%s, gekalibreerd",
    [UI_MENU_GYRO_CALIBRATE_FAILED] = "Kalibratie mislukt",
    [UI_MENU_GYRO_CALIBRATE_MOVED] = "Controller bewogen: houd hem stil en probeer opnieuw",
    [UI_MENU_GYRO_AUTO_BIAS] = "Gyrodrift leren in rust",
    [UI_MENU_GYRO_AUTO_BIAS_DESC] = "Leert de drift die in de gyro overblijft wanneer de controller stil op een tafel "
                                    "ligt, en trekt die af. Voor een boogrichting die zelfs na kalibratie langzaam "
                                    "wegglijdt. Van een controller in de hand wordt niet geleerd. Experimenteel.",
    [UI_MENU_RUMBLE_STRENGTH] = "Trilsterkte",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "Hoe sterk de controller trilt.",
    [UI_MENU_RUMBLE_CURVE] = "Trilcurve",
    [UI_MENU_RUMBLE_CURVE_DESC] = "Lagere waarden maken zwak trillen sterker.",
    [UI_MENU_RUMBLE_TRIGGERS] = "Triggertrilling (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Laat ook de triggers van een Xbox-controller trillen.",
    [UI_MENU_PAD_LAYOUT] = "Knopindeling",
    [UI_MENU_PAD_LAYOUT_DESC] = "Welke knop A is. Nintendo: de rechter, zoals op een Switch. Xbox: de onderste, zoals "
                                "gelabeld op een Xbox-controller (kruis op een PlayStation-controller). Automatisch "
                                "kiest Nintendo voor controllers met Nintendo-labels en Xbox voor de rest. Welke "
                                "controller de aanwijzingen van het spel tonen, stel je in bij Knopsymbolen.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Automatisch",
    [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A rechts)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A onder)",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "in gebruik: %s",
    [UI_MENU_PROMPTS] = "Knopsymbolen",
    [UI_MENU_PROMPTS_DESC] = "Van welke controller de aanwijzingen van het spel de knoppen tonen. Nintendo: die van "
                             "het spel zelf. Xbox: LB, RB, LT, RT, Menu, View en een richtingskruis, en A, B, X en Y "
                             "zoals gelabeld op de knop die de knopindeling hierboven indrukt (met Nintendo (A "
                             "rechts) heet de rechterknop B, dus de aanwijzingen voor A tonen B). PlayStation: de "
                             "symbolen op een PlayStation-controller (○ × △ □, L1, R1, L2, R2, Options en Share), op "
                             "de knop die de knopindeling hierboven indrukt. PC: de toetsen en muisknoppen van de "
                             "standaardindeling van het toetsenbord (E, Shift, Spatie, linkerklik...) en de muis voor "
                             "de camera. Het spel leest zijn aanwijzingen bij de start, dus ze veranderen na een "
                             "herstart. Ze horen bij de spelbestanden, dus een knop die je bij Besturing omzet, houdt "
                             "zijn oude aanwijzing op het scherm.",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo",
    [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation",
    [UI_MENU_PROMPTS_PC] = "PC",
    [UI_MENU_PROMPTS_MISSING] = "symboolbestanden ontbreken: voer de installatie opnieuw uit",
    [UI_MENU_PROMPTS_FAILED] = "symboolbestanden beschadigd: voer de installatie opnieuw uit",
    [UI_MENU_PROMPTS_RESTART] = "symbolen veranderen na herstart",
    [UI_MENU_PROMPTS_OFF_SHAPE] = "uit bij een spelresolutie die niet 16:9 is",
    [UI_MENU_OUTPUT_RES_NO_LAYOUT] = "geen indeling voor deze vorm: voer de installatie opnieuw uit",
    [UI_MENU_OUTPUT_RES_PREPARING] = "indeling wordt op de achtergrond gemaakt; gebruikt bij de volgende start",
    [UI_MENU_PROMPTS_PREPARING] = "symboolbestanden worden op de achtergrond gemaakt; gebruikt bij de volgende start",
    [UI_MENU_PREPARE_RETRY] = "Opnieuw proberen",
    [UI_MENU_PREPARE_RETRY_DESC] = "Start de hulpmiddelen van de installatie opnieuw om de ontbrekende "
                                   "interface-indeling of knopsymboolbestanden op de achtergrond te maken. Het spel "
                                   "blijft lopen; het gebruikt ze vanaf de volgende start.",
    [UI_MENU_SHAPE_PREPARE_FAILED] = "de hulpmiddelen van de installatie zijn niet gestart: voer de installatie "
                                     "opnieuw uit",
    [UI_MENU_PROMPTS_OFF_MODS] = "uit: een RomFS-mod vervangt hun bestanden",
    [UI_MENU_PROMPTS_OFF_PATCHES] = "uit zolang TOTK_ROMFS_PATCHES is ingesteld",
    [UI_MENU_RESTART] = "Nu herstarten",
    [UI_MENU_RESTART_DESC] = "Sluit het spel en start het opnieuw met de nieuwe symbolen. Voortgang sinds je laatste "
                             "opslag gaat verloren.",
    [UI_MENU_RESTART_CONFIRM] = "Het spel nu herstarten? Voortgang sinds je laatste opslag gaat verloren.",
    [UI_MENU_RESTART_YES] = "Herstarten",
    [UI_MENU_RESTART_SAVING] = "Het spel slaat op: herstarten kan zo meteen.",
    [UI_MENU_GROUP_STICKS] = "Analoge sticks",
    [UI_MENU_LSTICK_DEADZONE] = "Dode zone linkerstick",
    [UI_MENU_RSTICK_DEADZONE] = "Dode zone rechterstick",
    [UI_MENU_STICK_DEADZONE_DESC] = "Hoe ver een stick moet bewegen voordat het spel het merkt. Verhoog hem als het "
                                    "personage of de camera vanzelf beweegt: laat de stick los, lees \"stick nu\" af "
                                    "en zet de dode zone een paar procent hoger.",
    [UI_MENU_STICK_DEADZONE_NOW] = "stick nu: %.0f %%",
    [UI_MENU_MOUSE_MODE] = "Muiscamera",
    [UI_MENU_MOUSE_MODE_DESC] = "Direct: de camera draait zover als de muis beweegt, zoals in een pc-game, zonder "
                                "dode zone of curve van de stick. Stickemulatie: de muis werkt als de rechterstick.",
    [UI_MENU_MOUSE_STICK] = "Stickemulatie",
    [UI_MENU_MOUSE_DIRECT] = "Direct",
    [UI_MENU_MENU_CURSOR] = "Muis in de spelmenu's",
    [UI_MENU_MENU_CURSOR_DESC] = "In de eigen menu's van het spel (titelscherm, inventaris, winkels, dialoogkeuzes) "
                                 "verschijnt de aanwijzer: het item eronder wordt geselecteerd, linkerklik bevestigt, "
                                 "rechterklik gaat terug. Alleen als de muis of een toets het laatst is gebruikt, "
                                 "zodat een controller de menu's ongewijzigd ziet (spel 1.4.3).",
    [UI_MENU_MAP_CLICK] = "Linkerklik op de kaart",
    [UI_MENU_MAP_CLICK_DESC] = "Op de kaart van de Purah Pad drukt de linkermuisknop A in bij de cursor van de kaart, "
                               "in plaats van de knop waaraan hij is gekoppeld (standaard Y). Vereist Muis in de "
                               "spelmenu's (spel 1.4.3).",
    [UI_MENU_DIALOGUE_CLICK] = "Klikken in dialogen",
    [UI_MENU_DIALOGUE_CLICK_DESC] = "Zolang een dialoogvenster open is, drukt linkerklik op B (de tekst gaat verder "
                                    "en het doorrollen wordt overgeslagen) en rechterklik op A, in plaats van de "
                                    "knoppen waaraan ze zijn gekoppeld (standaard Y, dat het dialooglogboek opent, en "
                                    "ZL). Dialoogkeuzes houden de aanwijzer: linkerklik kiest (spel 1.4.3).",
    [UI_MENU_POINTER_IMAGE] = "Spelaanwijzer",
    [UI_MENU_POINTER_IMAGE_DESC] = "Zolang de cursor niet is vastgezet (de spelmenu's, dit menu, na Esc), toont het "
                                   "venster een eigen gouden pijlpunt in plaats van de systeempijl, op maat van de "
                                   "schaal van het scherm.",
    [UI_MENU_QUICKSAVE] = "Snel opslaan en laden",
    [UI_MENU_QUICKSAVE_DESC] = "Eén toets slaat het spel meteen op en een andere laadt die opslag (de twee rijen "
                               "hieronder: standaard F6 en F7, zoals in Half-Life), zonder menu op het scherm. Alleen "
                               "toetsenbord- en muisknoppen, alleen spel 1.4.3. Snel opslaan vraagt het spel om een "
                               "van zijn eigen automatische opslagen, dus het vervangt de oudste automatische opslag "
                               "(de handmatige opslag blijft), bewaart er een kopie van als snelle opslag (de map "
                               "quicksave van het profiel) en toont \"Spel opgeslagen\" boven in het beeld. Snel "
                               "laden houdt het beeld op \"Snelle opslag wordt geladen\" terwijl het buiten zicht "
                               "door de laadlijst van het spel gaat, en het spel laadt de snelle opslag (ongeveer 5 "
                               "s). Waar het spel zelf geen automatische opslag zou maken (een tussenfilmpje, een "
                               "dialoog), wordt niets opgeslagen en meldt de regel dat.",
    [UI_MENU_QUICKSAVE_KEY] = "Toets snel opslaan",
    [UI_MENU_QUICKLOAD_KEY] = "Toets snel laden",
    [UI_MENU_QUICKSAVE_KEY_DESC] = "De toets die snel opslaan of snel laden start; standaard F6 en F7, zoals in "
                                   "Half-Life. Kies de cel en druk op een toets of muisknop; Delete laat die zonder "
                                   "toets. Zolang snel opslaan aan staat, doen de twee toetsen niets anders (er "
                                   "verschijnt een melding als een toewijzing de toets ook gebruikt). totk.ini "
                                   "accepteert elke toetsnaam uit de toewijzingen (quicksave_key, quickload_key).",
    [UI_QS_SAVED] = "Spel opgeslagen",
    [UI_QS_LOADING] = "Snelle opslag wordt geladen",
    [UI_QS_NOT_NOW] = "Snel opslaan kan nu niet",
    [UI_QS_NONE] = "Er is nog geen snelle opslag (de toets voor snel opslaan maakt er een)",
    [UI_QS_FAILED] = "Snel opslaan mislukt: zie totk.log",
    [UI_QS_LOAD_FAILED] = "Snel laden mislukt: zie totk.log",
    [UI_MENU_MOUSE_SENS] = "Muisgevoeligheid camera",
    [UI_MENU_MOUSE_SENS_DESC] = "Hoe snel de muis de camera draait.",
    [UI_MENU_WALK_KEY] = "Looptoets",
    [UI_MENU_WALK_KEY_DESC] = "Houd hem ingedrukt om de sticks van het toetsenbord maar gedeeltelijk te kantelen, "
                              "zodat het toetsenbord loopt in plaats van rent. De loopschaal bepaalt hoeveel. "
                              "Controller en muis worden niet beïnvloed.",
    [UI_MENU_WALK_KEY_TAKEN] = "ook %s",
    [UI_MENU_WALK_SCALE] = "Loopschaal",
    [UI_MENU_WALK_SCALE_DESC] = "De kanteling van de toetsenbordsticks zolang de looptoets wordt ingedrukt. De helft "
                                "is lopen, volledig kantelen rennen.",
    [UI_MENU_FREECAM] = "Vrije camera",
    [UI_MENU_FREECAM_DESC] = "Laat een combinatie of een toets hieronder het beeld van Link losmaken en ermee "
                             "vliegen. Standaard uit: zolang het uit staat, draait de cameracode van het spel zoals "
                             "uitgebracht. Werkt na een herstart. Spelversie 1.4.3.",
    [UI_MENU_FREECAM_OFF_NOTE] = "Zet Vrije camera hierboven aan en herstart",
    [UI_MENU_FREECAM_PAD] = "Combinatie vrije camera",
    [UI_MENU_FREECAM_PAD_DESC] = "Controllerknoppen die samen ingedrukt het beeld door drie fasen leiden: het verlaat "
                                 "Link en vliegt, het blijft staan waar het werd achtergelaten terwijl Link verder "
                                 "speelt, en het komt terug. Zolang het vliegt, krijgt het spel geen invoer: de "
                                 "linkerstick vliegt langs de kijkrichting en zijwaarts, de triggers omhoog en "
                                 "omlaag, de rechterstick draait, en elke druk op L of R halveert of verdubbelt de "
                                 "snelheid. Vastgezet loopt Link waar de linkerstick op het scherm heen wijst. "
                                 "Spelversie 1.4.3.",
    [UI_MENU_FREECAM_PAD_V143] = "Alleen spelversie 1.4.3",
    [UI_MENU_FREECAM_KEY] = "Toets vrije camera",
    [UI_MENU_FREECAM_KEY_DESC] = "Een toets die het beeld door dezelfde fasen leidt: het verlaat Link en vliegt, het "
                                 "blijft staan terwijl Link verder speelt, en het komt terug. Zolang het vliegt: W A "
                                 "S D vliegen langs de kijkrichting en zijwaarts, de toetsen gekoppeld aan ZR en ZL "
                                 "(standaard X en Z) omhoog en omlaag, de muis en de pijltoetsen draaien, en de "
                                 "toetsen gekoppeld aan L en R (Q en R) halveren of verdubbelen de snelheid. "
                                 "Spelversie 1.4.3.",
    [UI_MENU_GROUP_GAMEPLAY_TWEAKS] = "Spelaanpassingen",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED] = "Spelaanpassingen",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED_DESC] = "Muiswielbesturing voor draaien en afstand bij Ultrahand, wisselen van "
                                             "voorwerpen en wapens, en vloeiend zoomen op de kaart.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE] = "Ultrahand draaien met wiel",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE_DESC] = "Draai voorwerpen die Ultrahand vasthoudt direct met het muiswiel.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS] = "Ultrahand C-draaias",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS_DESC] = "Houd C ingedrukt tijdens het scrollen om vastgehouden voorwerpen om "
                                               "de andere as te draaien.",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY] = "Draaigevoeligheid Ultrahand",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY_DESC] = "Hoe snel het muiswiel vastgehouden voorwerpen draait.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE] = "Ultrahand-afstand met wiel",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE_DESC] = "Rechterklik + muiswiel duwt vastgehouden voorwerpen weg of trekt ze "
                                                 "dichterbij.",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY] = "Afstandsgevoeligheid Ultrahand",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY_DESC] = "Hoe snel het muiswiel de afstand van voorwerpen verandert.",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL] = "Wapenkeuze met wiel",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL_DESC] = "Scroll met het muiswiel tijdens vrij rondlopen om van wapen te wisselen. "
                                           "Bevestig met E.",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY] = "Gevoeligheid wapenwiel",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY_DESC] = "Aantal scrollstappen om naar het volgende wapen te gaan.",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL] = "Snelmenukeuze met wiel",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL_DESC] = "Scroll met het muiswiel door voorwerpen zolang een snelmenu open is.",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY] = "Gevoeligheid snelmenuwiel",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY_DESC] = "Aantal scrollstappen om in snelmenu's naar het volgende voorwerp te "
                                               "gaan.",
    [UI_MENU_GAMEPLAY_ITEM_AD] = "A / D in snelmenu's",
    [UI_MENU_GAMEPLAY_ITEM_AD_DESC] = "Druk op A / D om in snelmenu's naar links en rechts te gaan.",
    [UI_MENU_GAMEPLAY_MAP_ZOOM] = "Vloeiend kaartzoomen",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_DESC] = "Vloeiend zoomen met naloop op de kaart van de Purah Pad met het muiswiel.",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY] = "Gevoeligheid kaartzoom",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY_DESC] = "Hoe snel het muiswiel op de kaart zoomt.",
    [UI_MENU_STICK_SENS] = "Camerasnelheid stick",
    [UI_MENU_STICK_SENS_DESC] = "Vermenigvuldigt de camerasnelheid van de rechterstick in het spel, topsnelheid "
                                "inbegrepen, bij elk van de vijf cameragevoeligheden van het spel. De muis houdt zijn "
                                "eigen instelling.",
    [UI_BINDS_HELP] = "Kies een cel en druk op A of Enter, daarna op de toets, muisknop of controllerknop. Delete of "
                      "X wist een cel. Een toets die al in gebruik is verhuist hierheen; een controllerknop ruilt met "
                      "de knop die hij vervangt.",
    [UI_BINDS_COL_KEY1] = "Toets 1",
    [UI_BINDS_COL_KEY2] = "Toets 2",
    [UI_BINDS_COL_PAD] = "Controller",
    [UI_BINDS_GROUP_BUTTONS] = "Knoppen",
    [UI_BINDS_GROUP_MOVE] = "Beweging",
    [UI_BINDS_GROUP_CAMERA] = "Camera",
    [UI_BINDS_PRESS_KEY] = "Druk op een toets of muisknop voor %s",
    [UI_BINDS_PRESS_PAD] = "Druk op een controllerknop voor %s",
    [UI_BINDS_CANCEL_HINT] = "Esc annuleert, of wacht %d s",
    [UI_BINDS_TAKEN] = "%s is weggehaald bij %s",
    [UI_BINDS_SWAPPED] = "%s geruild met %s",
    [UI_BINDS_RESET] = "Alle knoppen herstellen",
    [UI_BINDS_RESET_DESC] = "Terug naar de standaardtoetsen en -controllerknoppen. De knopindeling hierboven geldt "
                            "weer: omgezette controllerknoppen blijven tot dit herstel waar je ze hebt gezet.",
    [UI_BIND_A] = "A",
    [UI_BIND_B] = "B",
    [UI_BIND_X] = "X",
    [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "Linkerstick indrukken",
    [UI_BIND_RCLICK] = "Rechterstick indrukken",
    [UI_BIND_L] = "L",
    [UI_BIND_R] = "R",
    [UI_BIND_ZL] = "ZL",
    [UI_BIND_ZR] = "ZR",
    [UI_BIND_PLUS] = "Plus",
    [UI_BIND_MINUS] = "Min",
    [UI_BIND_LEFT] = "Richtingskruis links",
    [UI_BIND_UP] = "Richtingskruis omhoog",
    [UI_BIND_RIGHT] = "Richtingskruis rechts",
    [UI_BIND_DOWN] = "Richtingskruis omlaag",
    [UI_BIND_LSTICK_LEFT] = "Naar links",
    [UI_BIND_LSTICK_UP] = "Vooruit",
    [UI_BIND_LSTICK_RIGHT] = "Naar rechts",
    [UI_BIND_LSTICK_DOWN] = "Achteruit",
    [UI_BIND_RSTICK_LEFT] = "Camera links",
    [UI_BIND_RSTICK_UP] = "Camera omhoog",
    [UI_BIND_RSTICK_RIGHT] = "Camera rechts",
    [UI_BIND_RSTICK_DOWN] = "Camera omlaag",
    [UI_BIND_DESC_A] = "Gebruiken, oppakken, praten.",
    [UI_BIND_DESC_B] = "Rennen, annuleren, terug.",
    [UI_BIND_DESC_X] = "Springen.",
    [UI_BIND_DESC_Y] = "Aanvallen.",
    [UI_BIND_DESC_LCLICK] = "Hurken.",
    [UI_BIND_DESC_RCLICK] = "Eerstepersoonsweergave.",
    [UI_BIND_DESC_L] = "Vaardigheden.",
    [UI_BIND_DESC_R] = "Wapen gooien.",
    [UI_BIND_DESC_ZL] = "Schild, vastzetten.",
    [UI_BIND_DESC_ZR] = "Boog.",
    [UI_BIND_DESC_PLUS] = "Menu, inventaris.",
    [UI_BIND_DESC_MINUS] = "Kaart.",
    [UI_BIND_DESC_DPAD] = "Snelkeuze.",
    [UI_BIND_DESC_MOVE] = "Beweegt Link zoals de linkerstick. De linkerstick van de controller werkt ook altijd.",
    [UI_BIND_DESC_CAMERA] = "Draait de camera zoals de rechterstick. De muis en de rechterstick van de controller "
                            "werken ook altijd.",
    [UI_BIND_SHAKE] = "Camerastick schudden",
    [UI_BIND_DESC_SHAKE] = "Zwaait de rechterstick voor het spel naar links en rechts: maakt een vastgemaakt "
                           "onderdeel los van wat Ultrahand vasthoudt. Ingedrukt houden blijft schudden.",
    [UI_KEY_SPACE] = "Spatie",
    [UI_KEY_TAB] = "Tab",
    [UI_KEY_LSHIFT] = "Linker Shift",
    [UI_KEY_LCTRL] = "Linker Ctrl",
    [UI_KEY_UP] = "Pijl omhoog",
    [UI_KEY_DOWN] = "Pijl omlaag",
    [UI_KEY_LEFT] = "Pijl links",
    [UI_KEY_RIGHT] = "Pijl rechts",
    [UI_KEY_MOUSE_L] = "Linkermuisknop",
    [UI_KEY_MOUSE_R] = "Rechtermuisknop",
    [UI_KEY_MOUSE_M] = "Middelste muisknop",
    [UI_KEY_MOUSE_X1] = "Muis 4",
    [UI_KEY_MOUSE_X2] = "Muis 5",
    [UI_KEY_ENTER] = "Enter",
    [UI_KEY_BACKSPACE] = "Backspace",
    [UI_KEY_RSHIFT] = "Rechter Shift",
    [UI_KEY_RCTRL] = "Rechter Ctrl",
    [UI_KEY_LALT] = "Alt",
    [UI_KEY_CAPSLOCK] = "Caps Lock",
    [UI_KEY_INSERT] = "Insert",
    [UI_KEY_DELETE] = "Delete",
    [UI_KEY_HOME] = "Home",
    [UI_KEY_END] = "End",
    [UI_KEY_PAGEUP] = "Page Up",
    [UI_KEY_PAGEDOWN] = "Page Down",
    [UI_KEY_NUM_FMT] = "Num %s",
    [UI_PAD_SOUTH] = "Onderste knop",
    [UI_PAD_EAST] = "Rechterknop",
    [UI_PAD_WEST] = "Linkerknop",
    [UI_PAD_NORTH] = "Bovenste knop",
    [UI_PAD_LSTICK] = "Linkerstick",
    [UI_PAD_RSTICK] = "Rechterstick",
    [UI_PAD_L] = "Linker schouderknop",
    [UI_PAD_R] = "Rechter schouderknop",
    [UI_PAD_START] = "Start",
    [UI_PAD_BACK] = "Terug",
    [UI_PAD_DPAD_LEFT] = "Richtingskruis links",
    [UI_PAD_DPAD_UP] = "Richtingskruis omhoog",
    [UI_PAD_DPAD_RIGHT] = "Richtingskruis rechts",
    [UI_PAD_DPAD_DOWN] = "Richtingskruis omlaag",
    [UI_PAD_ZL] = "Linkertrigger",
    [UI_PAD_ZR] = "Rechtertrigger",
    [UI_PAD_LPADDLE1] = "Linker achterknop",
    [UI_PAD_RPADDLE1] = "Rechter achterknop",
    [UI_PAD_LPADDLE2] = "Linker achterknop 2",
    [UI_PAD_RPADDLE2] = "Rechter achterknop 2",
    [UI_PAD_TOUCHPAD] = "Touchpadklik",
    [UI_PAD_CROSS] = "Kruis",
    [UI_PAD_CIRCLE] = "Cirkel",
    [UI_PAD_SQUARE] = "Vierkant",
    [UI_PAD_TRIANGLE] = "Driehoek",
    [UI_MENU_AMIIBO_DIR] = "Amiibomap",
    [UI_MENU_AMIIBO_OPEN] = "Openen",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "Standaardmap",
    [UI_MENU_AMIIBO_REFRESH] = "Vernieuwen",
    [UI_MENU_AMIIBO_PICK] = "Amiibo-dumps (.bin- of .nfc-bestanden van 540 bytes):",
    [UI_MENU_AMIIBO_NO_DIR] = "Kan de map %s niet openen",
    [UI_MENU_AMIIBO_READER] = "NFC-lezer",
    [UI_MENU_AMIIBO_STATE] = "Lezer:",
    [UI_MENU_AMIIBO_STATE_IDLE] = "gereed",
    [UI_MENU_AMIIBO_STATE_SEARCHING] = "het spel wacht op een amiibo",
    [UI_MENU_AMIIBO_STATE_FOUND] = "amiibo op de lezer",
    [UI_MENU_AMIIBO_STATE_MOUNTED] = "het spel leest de amiibo",
    [UI_MENU_AMIIBO_STATE_OFF] = "uit",
    [UI_MENU_AMIIBO_FILE] = "Gekozen bestand:",
    [UI_MENU_AMIIBO_NO_FILE] = "(geen bestand gekozen)",
    [UI_MENU_AMIIBO_TAP] = "Amiibo aanhouden",
    [UI_MENU_AMIIBO_REMOVE] = "Amiibo weghalen",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "Amiibo na het aanhouden vanzelf weghalen",
    [UI_MENU_AMIIBO_HOLD] = "Tijd op de lezer",
    [UI_MENU_AMIIBO_SECONDS] = "%.1f s",
    [UI_MENU_AMIIBO_TAG] = "Amiibo op de lezer:",
    [UI_MENU_AMIIBO_NO_TAG] = "Geen amiibo op de lezer.",
    [UI_MENU_AMIIBO_TAG_FILE] = "Bestand: %s",
    [UI_MENU_AMIIBO_TAG_ID] = "Personage %s, serie %02x, nummer %04x, type %02x",
    [UI_MENU_AMIIBO_TAG_UID] = "UID: %s",
    [UI_MENU_AMIIBO_READY] = "Gereed",
    [UI_MENU_AMIIBO_TAPPED] = "Aangehouden: %s",
    [UI_MENU_AMIIBO_LOAD_FAILED] = "Geen amiibo-dump (NTAG215 van 540 bytes): %s",
    [UI_MENU_AMIIBO_REMOVED] = "Amiibo weggehaald",
    [UI_MENU_AMIIBO_STATUS] = "Status: %s",
    [UI_MENU_MENU_LANG] = "Menutaal",
    [UI_MENU_MENU_LANG_DESC] = "Taal van dit menu en de prestatieweergave.",
    [UI_MENU_MENU_LANG_AUTO] = "Automatisch (uit Windows)",
    [UI_MENU_DISCORD_RPC] = "Discord Rich Presence",
    [UI_MENU_DISCORD_RPC_DESC] = "Toont \"Speelt TotK Recomp\" met de speeltijd in je Discord-status terwijl het spel "
                                 "draait. discord_app_id en discord_image in totk.ini kiezen de applicatie.",
    [UI_MENU_ADV_WARNING] = "Deze instellingen zijn voor testen en probleemoplossing. Wijzig ze alleen als je weet "
                            "waarom.",
    [UI_MENU_GROUP_SCHED] = "Planning",
    [UI_MENU_GROUP_SHADERS] = "Shaders en pipelines",
    [UI_MENU_GROUP_TEXMEM] = "Texturen en geheugen",
    [UI_MENU_GROUP_FIXES] = "Rendercorrecties",
    [UI_MENU_GROUP_VULKAN] = "Vulkan",
    [UI_MENU_GROUP_DIAG] = "Diagnose",
    [UI_MENU_ASYNC_PREEMPT] = "Onderbreking op virtuele kernen",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "Laat een spelthread een andere op dezelfde virtuele kern onderbreken.",
    [UI_MENU_PREEMPT_OFF] = "Uit",
    [UI_MENU_PREEMPT_ON] = "Aan",
    [UI_MENU_PREEMPT_INHERIT] = "Alleen overerving",
    [UI_MENU_REPLAY_ASYNC] = "GPU-beeldvoorbereiding op eigen thread",
    [UI_MENU_REPLAY_ASYNC_DESC] = "Bereidt beelden voor de GPU voor op een aparte thread in plaats van de spelthread. "
                                  "Voegt tot één beeld vertraging toe.",
    [UI_MENU_REPLAY_WORKERS] = "Threads GPU-beeldvoorbereiding",
    [UI_MENU_REPLAY_WORKERS_DESC] = "Stelt grote beelden voor de GPU samen op meerdere threads tegelijk. Helpt als de "
                                    "CPU de beeldsnelheid beperkt; vereist 8 of meer logische processors. Uit stelt "
                                    "ze samen op één thread.",
    [UI_MENU_REPLAY_WORKERS_NOW] = "nu: %s",
    [UI_MENU_TIMER_1MS] = "Windows-timer van 1 ms",
    [UI_MENU_TIMER_1MS_DESC] = "Verhoogt de resolutie van de systeemtimer naar 1 ms voor een gelijkmatiger beeldritme.",
    [UI_MENU_SYNC_AHEAD] = "Eén beeld voor op de GPU",
    [UI_MENU_SYNC_AHEAD_DESC] = "Het spel bouwt het volgende beeld terwijl de videokaart het vorige nog tekent. Meer beelden per seconde, één beeld meer invoervertraging.",
    [UI_MENU_HOST_CHECKPOINT] = "Snelle kernoverdracht",
    [UI_MENU_HOST_CHECKPOINT_DESC] = "Een spelthread geeft zijn virtuele kern aan een belangrijkere thread zodra hij de code van de port verlaat, niet pas bij zijn volgende systeemaanroep.",
    [UI_MENU_PIPE_WAIT] = "Afhandeling van wachtende draws",
    [UI_MENU_PIPE_WAIT_DESC] = "Standaard houdt de huidige planning met een gereserveerde worker voor wachtende "
                               "draws; Oud (draw) gebruikt de vorige planning.",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "Standaard",
    [UI_MENU_PIPE_WAIT_DRAW] = "Oud (draw)",
    [UI_MENU_PIPE_SKIP] = "Overgeslagen draws tijdens pipelinebouw",
    [UI_MENU_PIPE_SKIP_DESC] = "Welke draws worden weggelaten in plaats van op hun pipeline te wachten.",
    [UI_MENU_PIPE_SKIP_STRICT] = "Strikt",
    [UI_MENU_PIPE_SKIP_ANY] = "Alle",
    [UI_MENU_PIPE_SKIP_FULLSCREEN] = "Volledigschermpassen",
    [UI_MENU_LIB_PREBUILD] = "Stagebibliotheken vooraf bouwen",
    [UI_MENU_LIB_PREBUILD_DESC] = "Bouwt shader-stagebibliotheken vooraf op de achtergrond.",
    [UI_MENU_LIB_PREBUILD_THREADS] = "Threads voor stagebibliotheken",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "Threads voor het vooraf bouwen; Automatisch kiest een aantal op basis van "
                                          "je CPU.",
    [UI_MENU_PIPE_PREBUILD] = "Pipelines vooraf bouwen",
    [UI_MENU_PIPE_PREBUILD_DESC] = "Bouwt bekende pipelines vooraf.",
    [UI_MENU_PREWARM_XLATE] = "Vertaler voorverwarmen",
    [UI_MENU_PREWARM_XLATE_DESC] = "Threads die bekende shaders bij de start vertalen; Automatisch neemt de helft van "
                                   "de CPU-threads, maximaal 16.",
    [UI_MENU_GPL] = "Graphics-pipelinebibliotheek",
    [UI_MENU_GPL_DESC] = "Gebruikt de Vulkan-extensie voor pipelinebibliotheken; zet hem uit als je stuurprogramma er "
                         "problemen mee heeft.",
    [UI_MENU_ASTC] = "ASTC-textuurdecodering",
    [UI_MENU_ASTC_DESC] = "Waar gecomprimeerde ASTC-texturen worden gedecodeerd.",
    [UI_MENU_ASTC_AUTO] = "Automatisch",
    [UI_MENU_ASTC_GPU] = "GPU-shader",
    [UI_MENU_ASTC_NATIVE] = "Native",
    [UI_MENU_ASTC_CPU] = "CPU",
    [UI_MENU_MEMORY] = "GPU-geheugenmodus",
    [UI_MENU_MEMORY_DESC] = "Hoe het spelgeheugen met de GPU wordt gedeeld.",
    [UI_MENU_MEMORY_AUTO] = "Automatisch",
    [UI_MENU_MEMORY_IMPORT] = "Importeren",
    [UI_MENU_MEMORY_MIRROR] = "Spiegelen",
    [UI_MENU_MEMORY_VERIFY] = "Controleren",
    [UI_MENU_TEX_ALIAS] = "Textuur-aliasing",
    [UI_MENU_TEX_ALIAS_DESC] = "Een nieuwe textuur in het geheugen van een levende textuur begint met die inhoud in "
                               "plaats van leeg.",
    [UI_MENU_TEX_VRAM_PRESSURE] = "Limiet textuurgeheugen",
    [UI_MENU_TEX_VRAM_PRESSURE_DESC] = "Percentage videogeheugen waarboven texturen die het spel heeft vrijgegeven "
                                       "worden weggegooid (ze komen terug uit de opslag als ze weer nodig zijn). 0 "
                                       "bewaart ze.",
    [UI_MENU_ASTC_BC] = "ASTC comprimeren naar BC3",
    [UI_MENU_ASTC_BC_DESC] = "Met de GPU-decoder: codeert gedecodeerde ASTC-texturen opnieuw naar BC3, een kwart van "
                             "het videogeheugen. Voor GPU's met 4 GB of minder; texturen ogen iets blokkeriger.",
    [UI_MENU_LOD_GUARD] = "LOD-bewaking",
    [UI_MENU_LOD_GUARD_DESC] = "Voorkomt dat voorwerpen een beeld lang op de verkeerde plek oplichten of de verte "
                               "plat oogt, door te wachten tot het spel klaar is met het herschrijven van de "
                               "detailniveaugegevens voordat de GPU ze leest.",
    [UI_MENU_FIX_C0] = "Correctie constantenbuffer",
    [UI_MENU_FIX_C0_DESC] = "Vult de door het stuurprogramma gereserveerde constantenbank c0 (opslaggroottes, "
                            "dispatchgrootte, puntgrootte, basisinstantie) in plaats van nullen te lezen.",
    [UI_MENU_FIX_ZERO] = "Nieuw GPU-geheugen op nul zetten",
    [UI_MENU_FIX_ZERO_DESC] = "Nieuwe GPU-geheugenpools beginnen gevuld met nullen, zoals Switch-geheugen.",
    [UI_MENU_DLAA_EFFECTS_AFTER] = "Deeltjes na DLAA",
    [UI_MENU_DLAA_EFFECTS_AFTER_DESC] = "Met DLAA worden stof en andere deeltjes over het beeld getekend dat DLSS al "
                                        "heeft gladgestreken, zoals het spel ze zonder DLSS tekent: kleine deeltjes "
                                        "in mist (heiligdommen) verdwijnen niet meer en laten geen sporen meer "
                                        "achter. Mist en wolken gaan nog steeds door DLSS, zodat verre randen tegen "
                                        "de lucht schoon blijven. Uit: DLSS verwerkt ook de deeltjes, zoals voorheen. "
                                        "Alleen in DLAA-modus; de upscalingmodi van DLSS verwerken ze altijd. Werkt "
                                        "meteen.",
    [UI_MENU_DLSS_FIXED_EXPOSURE] = "Vaste DLSS-belichting",
    [UI_MENU_DLSS_FIXED_EXPOSURE_DESC] = "DLSS neemt de helderheid van het spelbeeld zoals ze is, in plaats van de "
                                         "belichting per beeld te schatten: in donkere scènes (de Diepten) worden "
                                         "grote mistige vlakken niet meer een beeld lang donkerder of lichter terwijl "
                                         "de camera beweegt. Uit: DLSS schat de belichting zelf, zoals voorheen. "
                                         "Werkt meteen.",
    [UI_MENU_BIND_SKIP] = "Herhaalde bindingen overslaan",
    [UI_MENU_BIND_SKIP_DESC] = "Optimalisatie: slaat ongewijzigde bindingen over.",
    [UI_MENU_SHADER_MEMO] = "Shadergeheugen",
    [UI_MENU_SHADER_MEMO_DESC] = "Optimalisatie: onthoudt shaderopzoekingen.",
    [UI_MENU_PIPE_MEMO] = "Pipelinegeheugen",
    [UI_MENU_PIPE_MEMO_DESC] = "Optimalisatie: hergebruikt de pipeline van de vorige draw als de toestand zich "
                               "herhaalt.",
    [UI_MENU_TEX_MEMO] = "Geheugen textuurbindingen",
    [UI_MENU_TEX_MEMO_DESC] = "Optimalisatie: onthoudt textuurdescriptors tussen draws.",
    [UI_MENU_DESC_TEMPLATE] = "Descriptorsjablonen",
    [UI_MENU_DESC_TEMPLATE_DESC] = "Optimalisatie: werkt descriptors in bulk bij.",
    [UI_MENU_PUSH_SKIP] = "Herhaalde push constants overslaan",
    [UI_MENU_PUSH_SKIP_DESC] = "Optimalisatie: slaat ongewijzigde push constants over.",
    [UI_MENU_SPLIT_DRAWS] = "Vroege GPU-indiening",
    [UI_MENU_SPLIT_DRAWS_DESC] = "Dient GPU-werk tussen passen in delen in, elke N draws, zodat de GPU eerder begint; "
                                 "0 zet het uit.",
    [UI_MENU_VK_VALIDATION] = "Validatielaag",
    [UI_MENU_VK_VALIDATION_DESC] = "Controleert elke Vulkan-aanroep en vertraagt het spel sterk.",
    [UI_MENU_VK_VALIDATION_MISSING] = "De validatielaag is niet geïnstalleerd.",
    [UI_MENU_VK_DYN_VERTEX] = "Dynamische vertextoestand",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "Dwingt dynamische vertexinvoer aan of uit; de standaard hangt af van de GPU.",
    [UI_MENU_VK_DYN_A2C] = "Dynamische alpha-to-coverage",
    [UI_MENU_VK_DYN_A2C_DESC] = "Dwingt dynamische alpha-to-coverage aan of uit. Standaard: uit bij het "
                                "AMD-stuurprogramma voor RDNA 2 en ouder, dat personages er zwart mee tekent, elders "
                                "aan.",
    [UI_MENU_VK_SUBGROUP_PIN] = "Subgroepgrootte vastzetten",
    [UI_MENU_VK_SUBGROUP_PIN_DESC] = "Zet de subgroepgrootte van de shaders vast.",
    [UI_MENU_TRACE] = "Prestatielogboek",
    [UI_MENU_TRACE_DESC] = "Schrijft beeldtijden naar een bestand.",
    [UI_MENU_FLIGHT] = "Zwarte doos",
    [UI_MENU_FLIGHT_DESC] = "Houdt een korte geschiedenis bij en slaat die op als een beeld vastloopt.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "Drempel voor vastlopers (ms)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "Beeldtijd die als vastloper telt.",
    [UI_MENU_PASS_CRUMBS] = "GPU-pasmarkeringen",
    [UI_MENU_PASS_CRUMBS_DESC] = "Na een GPU-crash noemt het logboek de renderpas die bezig was. Automatisch: alleen "
                                 "AMD. Aan: ook NVIDIA, wat iets prestaties kost.",
    [UI_MENU_INPUT_RECORD] = "Invoer opnemen",
    [UI_MENU_INPUT_RECORD_DESC] = "Schrijft je invoer naar een bestand voor foutrapporten.",
    [UI_MENU_OPEN_LOG] = "Logmap openen",
    [UI_MENU_OPEN_LOG_DESC] = "Toont de map met de logbestanden.",
    [UI_MENU_COPY_INFO] = "Systeeminfo kopiëren",
    [UI_MENU_COPY_INFO_DESC] = "Kopieert GPU, stuurprogramma, CPU, RAM en gewijzigde instellingen naar het klembord.",
    [UI_MENU_COPY_INFO_DONE] = "Gekopieerd",
    [UI_MENU_ACTION_FAILED] = "werkte niet",
    [UI_MENU_RESET] = "Alle instellingen herstellen",
    [UI_MENU_RESET_DESC] = "Geldt voor alle tabbladen.",
    [UI_MENU_RESET_CONFIRM] = "Alle instellingen terugzetten naar standaard?",
    [UI_MENU_RESET_YES] = "Herstellen",
    [UI_MENU_RESET_NO] = "Annuleren",
    [UI_MENU_OV_FPS] = "%.0f FPS",
    [UI_MENU_OV_FRAME_MS] = "%.1f ms",
    [UI_MENU_OV_GAME_PCT] = "spel %.0f%%",
    [UI_MENU_OV_MAX_MS] = "max %.1f ms",
    [UI_MENU_OV_FPS_LABEL] = "FPS",
    [UI_MENU_OV_FRAME_LABEL] = "Beeld",
    [UI_MENU_OV_FMT_FPS] = "%.0f",
    [UI_MENU_OV_LOW1] = "1%% laagst %.0f",
    [UI_MENU_OV_LOW01] = "0,1%% laagst %.0f",
    [UI_MENU_OV_GPU] = "GPU",
    [UI_MENU_OV_CPU] = "CPU",
    [UI_MENU_OV_RAM] = "RAM",
    [UI_MENU_OV_VRAM] = "VRAM",
    [UI_MENU_OV_TEMP] = "Temperatuur",
    [UI_MENU_OV_POWER] = "Vermogen",
    [UI_MENU_OV_CLOCK] = "Kloksnelheid",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "GPU-beeldvoorbereiding",
    [UI_MENU_OV_GPU_TIME] = "GPU bezig per beeld",
    [UI_MENU_OV_PIPE_WAIT] = "Pipelinewachttijden",
    [UI_MENU_OV_SHADER_WAIT] = "Shaderwachttijden",
    [UI_MENU_OV_TEX_UPLOADS] = "Textuuruploads",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%",
    [UI_MENU_OV_FMT_TEMP] = "%.0f °C",
    [UI_MENU_OV_FMT_POWER] = "%.0f W",
    [UI_MENU_OV_FMT_MHZ] = "%.0f MHz",
    [UI_MENU_OV_FMT_GB] = "%.1f GB",
    [UI_MENU_OV_FMT_GB_OF] = "/ %.1f GB",
    [UI_MENU_OV_GAME_GB] = "spel %.1f GB",
    [UI_MENU_OV_FMT_MS] = "%.2f ms",
    [UI_MENU_OV_PER_FRAME] = "%.1f per beeld",
    [UI_MENU_OV_WINDOW] = "Venster",
    [UI_MENU_OV_OUTPUT] = "Spelbeeld",
    [UI_MENU_OV_SCENE] = "3D-scène",
    [UI_MENU_OV_UPSCALER] = "3D-upscaler",
    [UI_MENU_OV_SCALING] = "Schaling",
    [UI_MENU_OV_LIMIT] = "Beeldlimiet",
    [UI_MENU_OV_PRESENT] = "Weergave",
    [UI_MENU_OV_SHOWN] = "Getoond",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u",
    [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, scherpte %d%%",
    [UI_MENU_OV_SCALE_NONE] = "geen (1:1)",
    [UI_MENU_OV_SCALE_BILINEAR] = "bilineair",
    [UI_MENU_OV_SCALE_DOWN] = "bilineair (FSR vergroot alleen)",
    [UI_MENU_TAB_CHEATS] = "Cheats",
    [UI_MENU_CHEATS_NONE] = "Geen cheats voor deze spelversie. De cheats van een mod zijn een Atmosphère-cheatbestand "
                            "in zijn cheats-map, genoemd naar de build van het spel: mods\\<mod>\\cheats\\%s. Start "
                            "het spel opnieuw om ze te lezen. Voor cheats die de spelcode wijzigen, bouwt het spel "
                            "zichzelf bij de start opnieuw (enkele minuten).",
    [UI_MENU_CHEATS_MOD] = "%s: %d cheats",
    [UI_MENU_CHEAT_CODE_DESC] = "Wijzigt de spelcode. Schakelt meteen.",
    [UI_MENU_CHEAT_KEY_DESC] = "Wijzigt de spelcode zolang je %s ingedrukt houdt. Schakelt meteen.",
    [UI_MENU_CHEAT_DATA_DESC] = "Schrijft elk beeld naar het spelgeheugen zolang hij aan staat.",
    [UI_MENU_CHEAT_MASTER] = "altijd aan",
    [UI_MENU_CHEAT_MASTER_DESC] = "De mastercode van het bestand: hij staat altijd aan.",
    [UI_MENU_CHEAT_RESTORE] = "niet nodig",
    [UI_MENU_CHEAT_RESTORE_DESC] = "Hij schrijft alleen de oorspronkelijke spelcode terug. Een cheat uitzetten doet "
                                   "dat al.",
    [UI_MENU_CHEAT_REBUILD] = "na de herbouw",
    [UI_MENU_CHEAT_REBUILD_DESC] = "Deze cheat wijzigt de spelcode en is nieuw of bewerkt sinds het spel werd "
                                   "gebouwd. Het spel bouwt hem in bij de volgende start (enkele minuten), of gebruik "
                                   "Spel nu herbouwen.",
    [UI_MENU_CHEAT_UNSUPPORTED] = "niet ondersteund",
    [UI_MENU_CHEAT_UNSUPPORTED_DESC] = "Deze cheat kan niet worden gebruikt met deze spelversie:",
    [UI_MENU_CHEATS_REBUILD] = "Spel nu herbouwen",
    [UI_MENU_CHEATS_REBUILD_DESC] = "Sommige cheats wijzigen de spelcode en zijn niet ingebouwd: ze zijn nieuw of "
                                    "bewerkt, of hun laatste herbouw is mislukt of geannuleerd. Sluit het spel; de "
                                    "installatie bouwt ze in (enkele minuten) en start het spel opnieuw. Voortgang "
                                    "sinds je laatste opslag gaat verloren.",
    [UI_MENU_MODS_EXEFS_UNSUPPORTED] = "Deze mod past de spelcode aan (exefs) op een manier die de port niet kan "
                                       "toepassen, dus de hele mod blijft uit. %s",
    [UI_MENU_MODS_EXEFS_OTHER] = "Zijn codepatches (exefs) zijn gemaakt voor een andere spelversie.",
    [UI_MENU_MODS_EXEFS_ON] = "Codepatches in gebruik: %d bestanden.",
    [UI_MENU_MODS_REPLACES_BUILTIN] = "Codepatches in gebruik: %d bestanden. Ze vervangen de eigen patch %s van de "
                                      "port, die uit blijft zolang deze mod aan staat.",
    [UI_MENU_MODS_CODE_SKIPPED] = "Zijn eigen spelcode (%s) wordt niet geladen: de rest van de mod wordt gebruikt. "
                                  "Zet de mod uit als het spel zich ermee misdraagt.",
    [UI_MENU_MODS_PLUGIN_ON] = "Zijn plug-in draait: %d hooks in de spelcode.",
    [UI_MENU_MODS_PLUGIN_REFUSED] = "Zijn plug-in %s kan niet draaien in de port: %s.",
    [UI_MENU_MODS_PLUGIN_STAYS_OFF] = "De hele mod blijft uit. Zijn schakelaar \"... gebruiken zonder zijn code\" "
                                      "hieronder gebruikt de rest ervan.",
    [UI_MENU_MODS_PLUGIN_OFF] = "Zijn plug-in blijft deze keer uit: hij startte anders dan bij het bouwen (%s). Het "
                                "spel bouwt hem opnieuw bij de volgende start.",
    [UI_MENU_MODS_WITHOUT_CODE] = "%s gebruiken zonder zijn code",
    [UI_MENU_MODS_WITHOUT_CODE_DESC] = "De mod hierboven brengt code mee die de port niet kan uitvoeren: eigen "
                                       "spelcode (een subsdk-plug-in of een vervangen main) of codepatches (exefs) "
                                       "die de port niet kan toepassen. Uit: de hele mod blijft uit. Aan: die code "
                                       "wordt weggelaten en de rest van de mod wordt gebruikt: zijn RomFS-bestanden, "
                                       "cheats en de codepatches die werken. Zijn bestanden kunnen de ontbrekende "
                                       "code verwachten; zet het weer uit als het spel zich misdraagt. Werkt bij de "
                                       "volgende start.",
    [UI_MENU_MODS_WITHOUT_CODE_ALL] = "Aan voor elke mod: mods_without_code = 1 in totk.ini.",
    [UI_MENU_MODS_EXEFS_SKIPPED] = "%u van zijn codepatchbestanden (exefs) kunnen niet worden toegepast en worden "
                                   "weggelaten: de rest van de mod wordt gebruikt. %s",
    [UI_MENU_MODS_OVERRIDES_FILES] = "%u van zijn bestanden vervangen die van %s, dat lager in de lijst staat.",
    [UI_MENU_MODS_DATA_OVERRIDDEN] = "%u bytes van zijn wijzigingen in de spelgegevens komen van %s, dat hoger in de "
                                     "lijst staat.",
    [UI_MENU_MODS_OVERRIDES_DATA] = "%u bytes van zijn wijzigingen in de spelgegevens vervangen die van %s, dat lager "
                                    "in de lijst staat.",
    [UI_MENU_MODS_CODE_OVERRIDDEN] = "%u van zijn codepatchplaatsen worden overschreven door %s, dat hoger in de "
                                     "lijst staat.",
    [UI_MENU_MODS_OVERRIDES_CODE] = "Hij overschrijft %u codepatchplaatsen van %s, dat lager in de lijst staat.",
};

// Japanese
static const char *const text_ja[UI_TEXT_COUNT] = {
    SWKBD("文字入力", "名前を入力してください", "ウマ", "OK", "キャンセル", "コントローラー: A = OK、B = キャンセル"),

    [UI_MENU_TITLE] = "設定",
    [UI_MENU_TAB_SCREEN] = "画面",
    [UI_MENU_TAB_GAME] = "ゲーム",
    [UI_MENU_TAB_MODS] = "Mod",
    [UI_MENU_TAB_SOUND] = "サウンド",
    [UI_MENU_TAB_CONTROLS] = "操作",
    [UI_MENU_TAB_AMIIBO] = "amiibo",
    [UI_MENU_TAB_MENU] = "メニュー",
    [UI_MENU_TAB_ADVANCED] = "詳細",
    [UI_MENU_CONTINUE] = "続ける",
    [UI_MENU_QUIT] = "ゲームを終了",
    [UI_MENU_ON] = "オン",
    [UI_MENU_OFF] = "オフ",
    [UI_MENU_AUTO] = "自動",
    [UI_MENU_DEFAULT_MARK] = "既定",
    [UI_MENU_AFTER_RESTART] = "再起動後",
    [UI_MENU_SET_BY_ENV] = "環境変数で固定",
    [UI_MENU_SAVE_FAILED] = "設定を保存できませんでした",
    [UI_MENU_VERSION] = "TotK Recomp %s、ゲーム %s",
    [UI_MENU_FULLSCREEN] = "フルスクリーン",
    [UI_MENU_FULLSCREEN_DESC] = "ゲームで画面全体を使います。F11 で切り替えます。",
    [UI_MENU_WINDOW_SIZE] = "ウィンドウサイズ",
    [UI_MENU_WINDOW_SIZE_DESC] = "フルスクリーンでないときのゲームウィンドウの大きさです。",
    [UI_MENU_WINDOW_SIZE_CUSTOM] = "カスタム",
    [UI_MENU_PRESENT] = "表示方式",
    [UI_MENU_PRESENT_DESC] = "完成したフレームを画面に出す方法です。",
    [UI_MENU_PRESENT_MAILBOX] = "メールボックス",
    [UI_MENU_PRESENT_VSYNC] = "垂直同期",
    [UI_MENU_PRESENT_IMMEDIATE] = "即時",
    [UI_MENU_PRESENT_IN_USE] = "使用中: %s",
    [UI_MENU_GPU] = "グラフィックカード",
    [UI_MENU_GPU_DESC] = "ゲームを描画する Vulkan デバイスです。",
    [UI_MENU_GPU_AUTO] = "自動",
    [UI_MENU_PERF_OVERLAY] = "パフォーマンス表示",
    [UI_MENU_PERF_OVERLAY_DESC] = "右上にフレームレートを表示します。「詳細」では、グラフ付きのフレーム時間、CPU・GPU・メモリの負荷、温度、描画スレッドの待ち時間も表示します。F10 "
                                  "でモードを切り替えます。",
    [UI_MENU_PERF_OFF] = "オフ",
    [UI_MENU_PERF_FPS] = "FPS",
    [UI_MENU_PERF_FULL] = "詳細",
    [UI_PERF_LINE_FPS] = "%.0f fps",
    [UI_PERF_LINE_FPS_GEN] = "描画 %.0f fps、表示 %.0f",
    [UI_MENU_GRAPHICS] = "画質",
    [UI_MENU_OUTPUT_RES] = "ゲーム解像度",
    [UI_MENU_OUTPUT_RES_DESC] = "ゲームが画面とインターフェースを描く大きさです。1920 x 1080 を超えると、ビデオメモリと GPU 時間がより多く必要になります。横長の比率 (21:9) "
                                "ではカメラの視界が広がり HUD が画面の隅に移り、縦長の比率 (16:10、Steam Deck) では上下の端に移ります。",
    [UI_MENU_HUD_WIDTH] = "HUD の幅",
    [UI_MENU_HUD_WIDTH_DESC] = "16:9 より横長の画面で、HUD の左右をどれだけ離すかです。「全画面」では画面の端に置きます。21:9 または 16:9 では、その形の画面の中央部分に HUD "
                               "を収めます。足りないレイアウトはバックグラウンドで作成され、再起動後に使われます。",
    [UI_MENU_HUD_WIDTH_FULL] = "全画面",
    [UI_MENU_RENDER_SCALE] = "3D 描画スケール",
    [UI_MENU_RENDER_SCALE_DESC] = "ゲーム解像度に対する 3D シーンの大きさです。ゲームの FSR がその大きさに拡大します。100 % を超えるとギザギザが滑らかになります "
                                  "(スーパーサンプリング) が、GPU の負荷が大きく増えます。",
    [UI_MENU_RENDER_SCALE_GAME] = "ゲーム (83 %)",
    [UI_MENU_DYNAMIC_RES] = "動的解像度",
    [UI_MENU_DYNAMIC_RES_DESC] = "ゲームが 3D シーンを 90 % または 80 % に下げられるようにします。判断には Switch 向けの見積もりが使われ、お使いの GPU "
                                 "は考慮されないため、広い景色では高速な PC でも解像度が下がります。オフでは描画スケールを保ちます。",
    [UI_MENU_UPSCALER] = "ウィンドウへの拡大",
    [UI_MENU_UPSCALER_DESC] = "ウィンドウがゲーム解像度より大きいときの拡大方法です。FSR はバイリニアより輪郭と細部をくっきり保ちます。",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1",
    [UI_MENU_UPSCALER_BILINEAR] = "バイリニア",
    [UI_MENU_ASPECT] = "画面の形",
    [UI_MENU_ASPECT_DESC] = "ウィンドウの形がゲーム解像度の形と違うとき (21:9 の画面に 16:9 の映像など): 黒帯を付けて形を保つか、ウィンドウいっぱいに引き伸ばすかを選びます。",
    [UI_MENU_ASPECT_FIT] = "黒帯",
    [UI_MENU_ASPECT_STRETCH] = "引き伸ばし",
    [UI_MENU_MOVIE_ASPECT] = "ムービーの形",
    [UI_MENU_MOVIE_ASPECT_DESC] = "ゲーム解像度が 16:9 でないとき、ムービーを黒帯付きで本来の形のまま表示するか、映像全体に引き伸ばすかを選びます。次のムービーから反映されます。",
    [UI_MENU_MOVIE_ASPECT_KEEP] = "黒帯",
    [UI_MENU_MOVIE_ASPECT_STRETCH] = "引き伸ばし",
    [UI_MENU_FOV] = "視野角",
    [UI_MENU_FOV_DESC] = "カメラが映す範囲を、ゲーム本来の視野に対するパーセントで指定します。ゲームのバージョン 1.4.3 のみ。",
    [UI_MENU_VERT_PLUS] = "16:9 の横幅を保つ",
    [UI_MENU_VERT_PLUS_DESC] = "16:9 より横幅の狭い画面（Steam Deck、16:10、4:3）で、カメラが 16:9 と同じ横幅の世界を映し、上下はより広く映します。16:9 以上の横長の画面では変わりません。",
    [UI_MENU_SHADOW_RES] = "シャドウマップのサイズ",
    [UI_MENU_SHADOW_RES_DESC] = "太陽のシャドウマップの大きさです。ゲーム本来のサイズは視点によって変わり、地面近くでは約 400 まで下がるため、葉の影がブロック状になって跳ねます。固定サイ"
                                "ズにすると影が安定し、くっきりします。1024 を超えると GPU 時間とビデオメモリが増えます。",
    [UI_MENU_SHADOW_RES_GAME] = "ゲーム (可変)",
    [UI_MENU_ANISO] = "テクスチャフィルタリング",
    [UI_MENU_ANISO_DESC] = "表面テクスチャの異方性フィルタリングです。浅い角度で見た地面、道、壁がぼやけずに遠くまでくっきりします。「ゲーム」は Switch と同じく、ゲームが求めるレベルです。",
    [UI_MENU_ANISO_GAME] = "ゲーム",
    [UI_MENU_LOD_STEADY] = "負荷時も詳細度を維持",
    [UI_MENU_LOD_STEADY_DESC] = "フレームレートが下がったときにモデルとテクスチャの詳細度を下げるゲームの仕組みを飛ばします。エミュレーター用 Mod の Disable LOD Quality "
                                "Reduction と同じ動作です。ゲームのバージョン 1.1.0、1.2.1、1.4.3。性能の低い PC ではオフのままにしてください: "
                                "そこではこの低下がフレームレートを保っています。",
    [UI_MENU_SHARPNESS] = "FSR シャープネス",
    [UI_MENU_SHARPNESS_DESC] = "FSR で拡大した後のシャープ化です。0 でオフになります。",
    [UI_MENU_COLOR] = "色",
    [UI_MENU_COLOR_PRESET] = "カラープロファイル",
    [UI_MENU_COLOR_PRESET_DESC] = "下の 4 つの値をまとめて設定します。ビビッド: コントラストと彩度を上げ、暗部を少し明るくします (OLED 画面向け)。クリア: "
                                  "コントラスト、彩度、明るさを少し上げます。値を動かすとプロファイルは「カスタム」になります。HUD は映像と一緒に変わりますが、このメニューは変わりません。",
    [UI_MENU_COLOR_DEFAULT] = "ゲーム標準",
    [UI_MENU_COLOR_VIVID] = "ビビッド (OLED)",
    [UI_MENU_COLOR_CLARITY] = "クリア",
    [UI_MENU_COLOR_CUSTOM] = "カスタム",
    [UI_MENU_COLOR_BRIGHTNESS] = "明るさ",
    [UI_MENU_COLOR_BRIGHTNESS_DESC] = "すべての色に加算します。0 で変更なし。",
    [UI_MENU_COLOR_CONTRAST] = "コントラスト",
    [UI_MENU_COLOR_CONTRAST_DESC] = "色を中間のグレーから遠ざけ (100 % 超)、または近づけます。100 % で変更なし。",
    [UI_MENU_COLOR_SATURATION] = "彩度",
    [UI_MENU_COLOR_SATURATION_DESC] = "グレーに対する色の強さです。100 % で変更なし。",
    [UI_MENU_COLOR_GAMMA] = "ガンマ",
    [UI_MENU_COLOR_GAMMA_DESC] = "1 未満で暗部と中間調を明るく、1 を超えると暗くします。1 で変更なし。",
    [UI_MENU_HDR_GROUP] = "HDR",
    [UI_MENU_HDR] = "HDR 出力",
    [UI_MENU_HDR_DESC] = "HDR ディスプレイで明るいハイライトを表示します。Windows で HDR をオン (「HDR を使用する」) にするか、カラーマネジメント対応の Wayland "
                         "コンポジターが必要です。ない場合は SDR のままです。再起動後に反映されます。上のカラープロファイルは SDR 用で、HDR では使われません。",
    [UI_MENU_HDR_PEAK] = "ピーク輝度",
    [UI_MENU_HDR_PEAK_DESC] = "ディスプレイのピーク輝度 (nit) です。最も明るいハイライトはここで止まります。",
    [UI_MENU_HDR_GAME] = "ゲームの明るさ",
    [UI_MENU_HDR_GAME_DESC] = "映像の白 (ペーパーホワイト) の nit 値です。203 が HDR の基準です。明るい部屋では上げてください。",
    [UI_MENU_HDR_UI] = "HUD とメニューの明るさ",
    [UI_MENU_HDR_UI_DESC] = "HUD とこのメニューの白の nit 値です。",
    [UI_MENU_HDR_HUE] = "ハイライトの色相補正",
    [UI_MENU_HDR_HUE_DESC] = "炎や火花などの明るい色を、ゲーム本来の SDR 映像と同じく白に近づけ、それらが放つ光と合わせます。0 では彩度を完全に保ちます。",
    [UI_MENU_SHADOW_SCALE] = "影の解像度",
    [UI_MENU_SHADOW_SCALE_DESC] = "太陽のシャドウマップを、ゲーム標準の 960x960 より各辺でこの倍率だけ大きく描きます。影の輪郭と葉の影がくっきりします。次回の起動時に反映されます。",
    [UI_MENU_PCSS] = "ソフトシャドウ",
    [UI_MENU_PCSS_DESC] = "物体が地面に接する部分では太陽の影をくっきりと、影が遠くに落ちるほど柔らかくします (PCSS)。0 でゲームのフィルターのままです。次回の起動時に反映されます。",
    [UI_MENU_CONTACT_SHADOWS] = "接触影",
    [UI_MENU_CONTACT_SHADOWS_DESC] = "シャドウマップが拾えない小さな太陽の影です。足元、石の下、草むら、くぼみなど。ライティングの後に画面上で計算します。次回の起動時に反映されます。",
    [UI_MENU_SHADOW_RANGE] = "影のカスケード範囲",
    [UI_MENU_SHADOW_RANGE_DESC] = "影が柔らかくなる段階 (ゲームでは 20、40、80、160 m) をより遠くに移します。シャドウマップがより広い範囲に広がるの"
                                  "で、影の解像度も一緒に上げてください。次回の起動時に反映されます。",
    [UI_MENU_GRASS_RANGE] = "草の描画範囲",
    [UI_MENU_GRASS_RANGE_DESC] = "カメラの周りのどこまで草が生えるかです (ゲームでは 62.5 m)。高くすると草原の草がより遠くまで残り、目の前で生えてく"
                                 "ることがなくなります。低くすると描かれる草が減ります。次回の起動時に反映されます。",
    [UI_MENU_DLSS_PRESET] = "DLSS モデル",
    [UI_MENU_DLSS_PRESET_DESC] = "どの DLSS モデルを使うかです。「自動」はモードごとの NVIDIA の推奨に従います: DLAA、クオリティ、バランスでは K、パフォーマンスでは "
                                 "M、ウルトラパフォーマンスでは L。J: 動くキャラクターの後ろの残像がやや少なく、ちらつきがやや多くなります。K: "
                                 "どのモードでも最もシャープなモデルです。すぐに反映されます。",
    [UI_MENU_DLSS_PRESET_DEFAULT] = "自動",
    [UI_MENU_DLSS_PRESET_K] = "K (最もシャープ)",
    [UI_MENU_DLSS_PRESET_J] = "J (残像が少ない)",
    [UI_MENU_DLSS_CHARS] = "DLSS のキャラクター残像",
    [UI_MENU_DLSS_CHARS_DESC] = "動くキャラクター、動物、それらが持つ物の後ろに出るかすかな残像を減らします: その部分では DLSS が前のフレームをあまり使いません。強くするほど残像が消えます"
                                "が、その輪郭は少し滑らかさを失います。すぐに反映されます。",
    [UI_MENU_DLSS_CHARS_OFF] = "オフ",
    [UI_MENU_DLSS_CHARS_LIGHT] = "弱",
    [UI_MENU_DLSS_CHARS_MEDIUM] = "中",
    [UI_MENU_DLSS_CHARS_FULL] = "最大",
    [UI_MENU_DLSS] = "アップスケーリングモード",
    [UI_MENU_DLSS_DESC] = "ネイティブは 3D シーンをフル解像度でアンチエイリアス処理します。クオリティ、バランス、パフォーマンス、ウルトラパフォーマンスはそれぞれ 67、58、50、33 % "
                          "で描画し、アップスケーラー (DLSS または FSR 3.1) で拡大することで GPU 時間を節約します。これらは 3D 描画スケールの代わりになります。すぐに反映されます。",
    [UI_MENU_DLSS_DLAA] = "ネイティブ",
    [UI_MENU_DLSS_QUALITY] = "クオリティ",
    [UI_MENU_DLSS_BALANCED] = "バランス",
    [UI_MENU_DLSS_PERFORMANCE] = "パフォーマンス",
    [UI_MENU_DLSS_ULTRA] = "ウルトラパフォーマンス",
    [UI_MENU_FXAA] = "ゲームのアンチエイリアス (FXAA)",
    [UI_MENU_FXAA_DESC] = "ゲーム標準の FXAA による輪郭の平滑化です。細部が少しぼやけます。DLAA がオンなら不要です。",
    [UI_MENU_REFLEX] = "低遅延",
    [UI_MENU_REFLEX_DESC] = "システムの遅延を減らします。オンは NVIDIA Reflex (VK_NV_low_latency2) を使い、オン + ブースト は CPU がボトルネックの場面で NVIDIA GPU のクロックも高く保ちます。自動は NVIDIA では Reflex、AMD では AMD Anti-Lag 2 (VK_AMD_anti_lag) を使います。再起動後に反映されます。",
    [UI_MENU_REFLEX_AUTO] = "自動",
    [UI_MENU_REFLEX_OFF] = "オフ",
    [UI_MENU_REFLEX_ON] = "オン",
    [UI_MENU_REFLEX_BOOST] = "オン + ブースト",
    [UI_MENU_SCENE_UPSCALER] = "3D シーンのアップスケーラー",
    [UI_MENU_SCENE_UPSCALER_DESC] = "3D シーン用のテンポラルアップスケーラーです: 輪郭が滑らかになり、ちらつきが減ります。ネイティブより下のモードではシーンを小さく描いて拡大し、GPU "
                                    "時間を節約します。NVIDIA DLSS には NVIDIA RTX GPU と、" DLSS_FILE " (" EXE_FILE " "
                                    "と同じフォルダーに置く) が必要です。AMD FSR 3.1 はどの GPU でも動作します。オフではゲーム本来のスケーリングと FXAA のままです。HUD "
                                    "には影響しません。上の「ウィンドウへの拡大」とシャープネスは、引き続きゲーム画像をウィンドウに合わせます。再起動後に反映されます。",
    [UI_MENU_SCENE_UPSCALER_OFF] = "オフ",
    [UI_MENU_SCENE_UPSCALER_DLSS] = "NVIDIA DLSS",
    [UI_MENU_SCENE_UPSCALER_FSR3] = "AMD FSR 3.1",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_GPU] = "DLSS: NVIDIA RTX GPU が必要です",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_DLL] = "DLSS: " DLSS_FILE " がありません",
    [UI_MENU_FSR_SHARPNESS] = "FSR シャープネス",
    [UI_MENU_FSR_SHARPNESS_DESC] = "AMD FSR 3.1 または FSR 4 の後の 3D シーンのシャープ化です。0 でオフになります。",
    [UI_MENU_SCENE_UPSCALER_FSR4] = "AMD FSR 4",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_GPU] = "FSR 4: この GPU では利用できません",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_DLL] = "FSR 4: AMD の FSR ライブラリがありません",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_OS] = "FSR 4: Windows のみ",
    [UI_MENU_FPS_LIMIT] = "フレームレート上限",
    [UI_MENU_FPS_LIMIT_DESC] = "ゲームが描く 1 秒あたりの最大フレーム数です。60 を超える設定は実験的で、「ゲーム速度を時計に合わせる」が必要です。",
    [UI_MENU_FPS_30] = "30 FPS",
    [UI_MENU_FPS_40] = "40 FPS",
    [UI_MENU_FPS_45] = "45 FPS",
    [UI_MENU_FPS_60] = "60 FPS",
    [UI_MENU_FPS_90] = "90 FPS",
    [UI_MENU_FPS_120] = "120 FPS",
    [UI_MENU_FPS_144] = "144 FPS",
    [UI_MENU_FPS_DISPLAY] = "モニターのリフレッシュレート",
    [UI_MENU_FPS_DISPLAY_HZ] = "モニターのリフレッシュレート (%.0f Hz)",
    [UI_MENU_FRAME_QUEUE] = "フレームキュー",
    [UI_MENU_FRAME_QUEUE_DESC] = "前のフレームが画面に出る順番を待つ間に、ゲームが次のフレームを始めます: 上限でのフレーム落ちが大幅に減り、入力遅延が最大 1 フレーム増えます。すぐに反映されます。",
    [UI_MENU_FRAME_GEN] = "フレーム生成",
    [UI_MENU_FRAME_GEN_OFF] = "オフ",
    [UI_MENU_FRAME_GEN_FSR3] = "AMD FSR 3",
    [UI_MENU_FRAME_GEN_DESC] = "ゲームが描く 2 枚の画像の間に、生成した画像を 1 枚入れます: 画面上の画像は 2 倍になり、ゲームはフレーム上限の半分で動き、操作は約半フレーム遅れます。120 "
                               "Hz 以上のディスプレイ向けです。DLSS や HDR とは併用できません。すぐに反映されます。",
    [UI_MENU_REALTIME] = "ゲーム速度を時計に合わせる",
    [UI_MENU_REALTIME_DESC] = "ゲームを実時間で進め、遅いフレームがあってもゲームが遅くならないようにします。",
    [UI_MENU_CLOTH_REALTIME] = "布を時計に合わせる",
    [UI_MENU_CLOTH_REALTIME_DESC] = "布や髪も実時間で進めます。",
    [UI_MENU_HIGHFPS_FIX] = "高フレームレート用の修正",
    [UI_MENU_HIGHFPS_FIX_DESC] = "30 fps を超えるとおかしくなる物理と、60 を超えると透けたり見えなくなったりする物体を修正します。",
    [UI_MENU_CLIMB_FIX] = "登る速さの修正",
    [UI_MENU_CLIMB_FIX_DESC] = "60 fps を超えても壁やはしごを登る速さを保ち、リンクが上まで登れるようにします。",
    [UI_MENU_GAME_LANG] = "ゲームの言語",
    [UI_MENU_GAME_LANG_DESC] = "ゲームのテキストと音声の言語です。",
    [UI_MENU_GAME_LANG_AUTO] = "自動",
    [UI_MENU_OPERATION_MODE] = "本体のモード",
    [UI_MENU_OPERATION_MODE_DESC] = "ゲームが TV モードと携帯モードのどちらで動いていると見なすかです。",
    [UI_MENU_MODE_DOCKED] = "TV モード",
    [UI_MENU_MODE_HANDHELD] = "携帯モード",
    [UI_MENU_ASSUME_FOCUS] = "フォーカスがなくても動かす",
    [UI_MENU_ASSUME_FOCUS_DESC] = "オン (既定) では、別のウィンドウにいる間もゲームは動き続け、ウィンドウが前面に戻るまでキーボードは反応しません。オフでは、Switch で HOME "
                                  "ボタンを押したときのように、ウィンドウがフォーカスを失うと一時停止します。この方法で中断された読み込みは止まったままになることがあります。",
    [UI_MENU_PLAY_TIME] = "プレイ時間",
    [UI_MENU_PLAY_TIME_DESC] = "すべての起動を合計したゲーム内の時間です。ゲームウィンドウにフォーカスがあり、ゲームがフレームを描いている間だけ数えます。プロファイルフォルダーの "
                               "playtime.txt (セーブデータと同じ場所) に保存されます。",
    [UI_MENU_PLAY_TIME_HM] = "%llu 時間 %02u 分",
    [UI_MENU_PLAY_TIME_SESSION] = "今回: %s",
    [UI_MENU_MODS_BUILTIN] = "移植版に組み込み",
    [UI_MENU_MODS_INSTALLED] = "インストール済みの Mod",
    [UI_MENU_MODS_NONE] = "Mod はまだありません。Mod を mods フォルダーに展開し (Mod ごとに 1 フォルダー、その中に romfs フォルダー)、ゲームを再起動してください。TKMM "
                          "も、統合した Mod をそこにフォルダーとして書き出せます。",
    [UI_MENU_MODS_OPEN] = "mods フォルダーを開く",
    [UI_MENU_MODS_OPEN_DESC] = "ゲームが Mod を読み込むフォルダーを表示します。ない場合は作成されます。",
    [UI_MENU_MODS_ROW_DESC] = "置き換えるゲームファイル: %u。追加する新しいファイル: %u。",
    [UI_MENU_MODS_OVERRIDDEN] = "%u 個のファイルは、リストでより上にある %s のものが使われます。",
    [UI_MENU_MODS_MOVE_HINT] = "上のボタンか Ctrl+↑ で上へ、左のボタンか Ctrl+↓ で下へ移動します。2 つの Mod が同じファイルを変更する場合は、上にある方が優先されます。変"
                               "更は再起動後に反映されます。",
    [UI_MENU_MODS_EMPTY] = "このフォルダーには romfs、exefs、cheats のファイルがありません。",
    [UI_MENU_MODS_SUBSDK] = "この Mod は独自のゲームコード (subsdk プラグインや置き換えた main) を含みます。移植版はコードモジュールを読み込めないため、Mod 全体がオフのままです。",
    [UI_MENU_MODS_EXEFS_LATER] = "この Mod はゲームコードを書き換えます (exefs)。次回の起動時にパッチを組み込みます (数分の再ビルド)。それまでは Mod 全体がオフのままです。",
    [UI_MENU_MODS_CHEATS] = "チート: %d 個。「チート」タブで切り替えます。",
    [UI_MENU_MODS_CHEATS_FILE] = "このゲームバージョン用のチートは、起動時にこの Mod の cheats\\%s から読み込まれます。",
    [UI_MENU_MODS_BAD_NAME] = "フォルダー名に | が含まれています。Mod リストは名前の区切りに | を使います。フォルダー名を変えてください。",
    [UI_MENU_MODS_OTHER_VERSION] = "%u 個のファイルは別のゲームバージョン用のため除外されます。お使いのゲームバージョン用のこの Mod を探してください。",
    [UI_MENU_MODS_UNUSED] = "未使用",
    [UI_MENU_MODS_FPS60] = "60 FPS パッチ",
    [UI_MENU_MODS_FPS60_DESC] = "60 fps 用の移植版独自のパッチです。「ゲーム」タブのフレームレート上限で設定します。",
    [UI_MENU_MODS_RES] = "解像度パッチ",
    [UI_MENU_MODS_RES_DESC] = "1080p を超える解像度用の移植版独自のパッチです。「画面」タブのゲーム解像度で設定します。",
    [UI_MENU_MODS_QUICKMENU] = "クイックメニューの修正",
    [UI_MENU_MODS_QUICKMENU_DESC] = "60 FPS で武器、盾、素材のメニューの背景が黒くなる問題に対する移植版の修正です。",
    [UI_MENU_VOLUME] = "音量",
    [UI_MENU_VOLUME_DESC] = "ゲーム全体の音量です。",
    [UI_MENU_VOLUME_MUSIC] = "音楽の音量",
    [UI_MENU_VOLUME_MUSIC_DESC] = "BGM の音量です。",
    [UI_MENU_VOLUME_SFX] = "効果音の音量",
    [UI_MENU_VOLUME_SFX_DESC] = "効果音の音量です。",
    [UI_MENU_VOLUME_VOICE] = "ボイスの音量",
    [UI_MENU_VOLUME_VOICE_DESC] = "キャラクターのボイスの音量です。",
    [UI_MENU_AUDIO_CHANNELS] = "スピーカー",
    [UI_MENU_AUDIO_CHANNELS_DESC] = "ステレオまたは 5.1 サラウンドで出力します。「自動」は、Windows のスピーカー設定が 6 チャンネル以上なら 5.1 "
                                    "を選びます。再起動後に反映されます。",
    [UI_MENU_CHANNELS_AUTO] = "自動",
    [UI_MENU_CHANNELS_STEREO] = "ステレオ",
    [UI_MENU_CHANNELS_51] = "5.1 サラウンド",
    [UI_MENU_AUDIO_DEVICE] = "出力デバイス",
    [UI_MENU_AUDIO_DEVICE_DESC] = "音を再生する場所です。「システムの既定」は Windows の既定のデバイスに従います。選んだデバイスが外れている間は、"
                                  "戻るまでシステムの既定を使います。すぐに反映されます。",
    [UI_MENU_AUDIO_DEVICE_DEFAULT] = "システムの既定",
    [UI_MENU_MOTION] = "モーション操作",
    [UI_MENU_MOTION_DESC] = "コントローラーのジャイロで狙いを付けます。",
    [UI_MENU_GYRO_CALIBRATION] = "ジャイロのバイアス補正",
    [UI_MENU_GYRO_CALIBRATION_DESC] = "下のキャリブレーションの行で測った、静止時のコントローラーのドリフトを差し引きます。オフにしても測定値は保存されたままです。",
    [UI_MENU_GYRO_CALIBRATE] = "ジャイロのキャリブレーション",
    [UI_MENU_GYRO_CALIBRATE_DESC] = "静止時のジャイロのドリフトを測り、取り除きます。コントローラーを置くか、2 秒間動かさずに持ってください。",
    [UI_MENU_GYRO_CALIBRATE_WAIT] = "キャリブレーション中...",
    [UI_MENU_GYRO_CALIBRATED] = "%s、キャリブレーション済み",
    [UI_MENU_GYRO_CALIBRATE_FAILED] = "キャリブレーションに失敗しました",
    [UI_MENU_GYRO_CALIBRATE_MOVED] = "コントローラーが動きました: 動かさずに持って、もう一度お試しください",
    [UI_MENU_GYRO_AUTO_BIAS] = "静止時のジャイロドリフト学習",
    [UI_MENU_GYRO_AUTO_BIAS_DESC] = "コントローラーが机の上に静かに置かれているたびに、ジャイロに残ったドリフトを学習して差し引きます。キャリブレーション後も"
                                    "弓の狙いがゆっくりずれる場合に使います。手に持っているコントローラーからは学習しません。実験的な機能です。",
    [UI_MENU_RUMBLE_STRENGTH] = "振動の強さ",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "コントローラーの振動の強さです。",
    [UI_MENU_RUMBLE_CURVE] = "振動カーブ",
    [UI_MENU_RUMBLE_CURVE_DESC] = "値を下げると弱い振動が強くなります。",
    [UI_MENU_RUMBLE_TRIGGERS] = "トリガーの振動 (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Xbox コントローラーのトリガーも振動させます。",
    [UI_MENU_PAD_LAYOUT] = "ボタン配置",
    [UI_MENU_PAD_LAYOUT_DESC] = "どのボタンを A にするかです。Nintendo: Switch と同じく右のボタン。Xbox: Xbox コントローラーの表記どおり下のボタン "
                                "(PlayStation コントローラーでは ×)。自動は、Nintendo 表記のコントローラーには Nintendo を、それ以外には Xbox "
                                "を選びます。ゲームのボタン表示にどのコントローラーを使うかは「ボタン表示」で設定します。",
    [UI_MENU_PAD_LAYOUT_AUTO] = "自動",
    [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A が右)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A が下)",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "使用中: %s",
    [UI_MENU_PROMPTS] = "ボタン表示",
    [UI_MENU_PROMPTS_DESC] = "ゲームのボタン表示にどのコントローラーのボタンを使うかです。Nintendo: ゲーム本来の表示。Xbox: LB、RB、LT、RT、Menu、View、十字キー、そして"
                             "上のボタン配置で押されるボタンの表記どおりの A、B、X、Y (Nintendo (A が右) では右のボタンの表記は B なので、A の表示は B "
                             "になります)。PlayStation: PlayStation コントローラーの記号 (○ × △ □、L1、R1、L2、R2、Options、Share) "
                             "を、上のボタン配置で押されるボタンに合わせて表示します。PC: キーボードの既定の配置のキーとマウスボタン (E、Shift、スペース、左クリック...) "
                             "と、カメラ用のマウス。ゲームは起動時にボタン表示を読み込むため、変更は再起動後に反映されます。ボタン表示はゲームのファイ"
                             "ルの一部なので、「操作」で割り当てを変えたボタンは、画面上では元の表示のままです。",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo",
    [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation",
    [UI_MENU_PROMPTS_PC] = "PC",
    [UI_MENU_PROMPTS_MISSING] = "ボタン表示のファイルがありません: セットアップを再実行してください",
    [UI_MENU_PROMPTS_FAILED] = "ボタン表示のファイルが壊れています: セットアップを再実行してください",
    [UI_MENU_PROMPTS_RESTART] = "表示は再起動後に変わります",
    [UI_MENU_PROMPTS_OFF_SHAPE] = "ゲーム解像度が 16:9 でないためオフ",
    [UI_MENU_OUTPUT_RES_NO_LAYOUT] = "この形のレイアウトがありません: セットアップを再実行してください",
    [UI_MENU_OUTPUT_RES_PREPARING] = "レイアウトをバックグラウンドで作成中です。次回の起動時に使われます",
    [UI_MENU_PROMPTS_PREPARING] = "ボタン表示のファイルをバックグラウンドで作成中です。次回の起動時に使われます",
    [UI_MENU_PREPARE_RETRY] = "再試行",
    [UI_MENU_PREPARE_RETRY_DESC] = "セットアップのツールを再び起動し、足りないインターフェースのレイアウトやボタン表示のファイルをバックグラウンドで作成しま"
                                   "す。ゲームは動き続け、次回の起動時からそれらを使います。",
    [UI_MENU_SHAPE_PREPARE_FAILED] = "セットアップのツールが起動しませんでした: セットアップを再実行してください",
    [UI_MENU_PROMPTS_OFF_MODS] = "オフ: RomFS の Mod がファイルを置き換えています",
    [UI_MENU_PROMPTS_OFF_PATCHES] = "TOTK_ROMFS_PATCHES が設定されている間はオフ",
    [UI_MENU_RESTART] = "今すぐ再起動",
    [UI_MENU_RESTART_DESC] = "ゲームを閉じ、新しいボタン表示で起動し直します。最後のセーブ以降の進行状況は失われます。",
    [UI_MENU_RESTART_CONFIRM] = "今すぐゲームを再起動しますか？最後のセーブ以降の進行状況は失われます。",
    [UI_MENU_RESTART_YES] = "再起動",
    [UI_MENU_RESTART_SAVING] = "ゲームがセーブ中です。少し待つと再起動できます。",
    [UI_MENU_GROUP_STICKS] = "アナログスティック",
    [UI_MENU_LSTICK_DEADZONE] = "左スティックのデッドゾーン",
    [UI_MENU_RSTICK_DEADZONE] = "右スティックのデッドゾーン",
    [UI_MENU_STICK_DEADZONE_DESC] = "スティックをどれだけ動かすとゲームが反応するかです。キャラクターやカメラが勝手に動く場合は上げてください。スティックから"
                                    "手を離し、「現在のスティック」の値を読んで、それより数パーセント上に設定します。",
    [UI_MENU_STICK_DEADZONE_NOW] = "現在のスティック: %.0f %%",
    [UI_MENU_MOUSE_MODE] = "マウスのカメラ操作",
    [UI_MENU_MOUSE_MODE_DESC] = "ダイレクト: PC ゲームのように、マウスを動かした量だけカメラが回ります。スティックのデッドゾーンやカーブはありません。スティックエミュレーション: マウスが右スティックとして動作します。",
    [UI_MENU_MOUSE_STICK] = "スティックエミュレーション",
    [UI_MENU_MOUSE_DIRECT] = "ダイレクト",
    [UI_MENU_MENU_CURSOR] = "ゲームのメニューでマウスを使う",
    [UI_MENU_MENU_CURSOR_DESC] = "ゲーム本来のメニュー (タイトル、ポーチ、ショップ、会話の選択肢) でポインターを表示します: ポインターの下の項目が選ばれ、左クリックで決定、右クリックで"
                                 "戻ります。最後に使ったのがマウスかキーのときだけなので、ゲームパッドではメニューは変わりません (ゲーム 1.4.3)。",
    [UI_MENU_MAP_CLICK] = "マップで左クリック",
    [UI_MENU_MAP_CLICK_DESC] = "プルアパッドのマップで、マウスの左ボタンが、割り当てられたボタン (既定では Y) の代わりに、マップのカーソル位置で A "
                               "を押します。「ゲームのメニューでマウスを使う」が必要です (ゲーム 1.4.3)。",
    [UI_MENU_DIALOGUE_CLICK] = "会話中のクリック",
    [UI_MENU_DIALOGUE_CLICK_DESC] = "会話ウィンドウが開いている間、左クリックで B (テキストを進め、文字送りを飛ばします)、右クリックで A を押します。割り当てられたボタン "
                                    "(既定では会話ログを開く Y と、ZL) の代わりです。会話の選択肢ではポインターのままです: 左クリックで選びます (ゲーム 1.4.3)。",
    [UI_MENU_POINTER_IMAGE] = "ゲームのポインター",
    [UI_MENU_POINTER_IMAGE_DESC] = "カーソルがキャプチャされていない間 (ゲームのメニュー、このメニュー、Esc の後)、ウィンドウにはシステムの矢印の代わりに独自の金色の矢"
                                   "じりが、画面の拡大率に合わせた大きさで表示されます。",
    [UI_MENU_QUICKSAVE] = "クイックセーブとロード",
    [UI_MENU_QUICKSAVE_DESC] = "1 つのキーでゲームをすぐにセーブし、もう 1 つのキーでそのセーブを読み込みます (下の 2 行: 既定では Half-Life と同じく F6 と "
                               "F7)。画面にメニューは出ません。キーボードとマウスのキーのみ、ゲーム 1.4.3 のみです。クイックセーブはゲーム自身にオートセーブを 1 "
                               "つ作らせるため、最も古いオートセーブが置き換わり (手動セーブはそのまま)、そのコピーをクイックセーブとして保存し (プロファイルの quicksave "
                               "フォルダー)、画面上部に \"セーブしました\" と表示します。クイックロードは、画面に \"クイックセーブを読み込み中\" "
                               "を表示したまま、見えないところでゲームのロード一覧をたどり、ゲームがクイックセーブを読み込みます (約 5 秒)。ゲーム自身がオートセーブを作らない場面 "
                               "(ムービー、会話) では何もセーブされず、その旨が表示されます。",
    [UI_MENU_QUICKSAVE_KEY] = "クイックセーブのキー",
    [UI_MENU_QUICKLOAD_KEY] = "クイックロードのキー",
    [UI_MENU_QUICKSAVE_KEY_DESC] = "クイックセーブまたはクイックロードを行うキーです。既定では Half-Life と同じく F6 と F7。セルを選んで任意のキーかマウスボタンを押します。Del"
                                   "ete でそのキーを空にします。クイックセーブがオンの間、この 2 つのキーは他の動作をしません (割り当てでも同じキーを使っている場合は注記が出ます)。to"
                                   "tk.ini では、割り当てで使える任意のキー名を指定できます (quicksave_key、quickload_key)。",
    [UI_QS_SAVED] = "セーブしました",
    [UI_QS_LOADING] = "クイックセーブを読み込み中",
    [UI_QS_NOT_NOW] = "今はクイックセーブできません",
    [UI_QS_NONE] = "クイックセーブはまだありません (クイックセーブのキーで作成できます)",
    [UI_QS_FAILED] = "クイックセーブに失敗しました: totk.log を確認してください",
    [UI_QS_LOAD_FAILED] = "クイックロードに失敗しました: totk.log を確認してください",
    [UI_MENU_MOUSE_SENS] = "マウスのカメラ感度",
    [UI_MENU_MOUSE_SENS_DESC] = "マウスでカメラが回る速さです。",
    [UI_MENU_WALK_KEY] = "歩きキー",
    [UI_MENU_WALK_KEY_DESC] = "押している間、キーボードのスティック入力を途中までだけ傾け、走らずに歩くようにします。どれだけ傾けるかは歩きの度合いで決めます。コントローラーとマウスには影響しません。",
    [UI_MENU_WALK_KEY_TAKEN] = "%s にも割り当て",
    [UI_MENU_WALK_SCALE] = "歩きの度合い",
    [UI_MENU_WALK_SCALE_DESC] = "歩きキーを押している間のキーボードのスティックの傾きです。半分で歩き、いっぱいで走りです。",
    [UI_MENU_FREECAM] = "フリーカメラ",
    [UI_MENU_FREECAM_DESC] = "下のボタンの組み合わせかキーで、視点をリンクから切り離して自由に飛ばせるようにします。既定ではオフ: オフの間は、ゲームのカメラコードは元のまま動きます。再起動後"
                             "に反映されます。ゲームのバージョン 1.4.3。",
    [UI_MENU_FREECAM_OFF_NOTE] = "上のフリーカメラをオンにして再起動してください",
    [UI_MENU_FREECAM_PAD] = "フリーカメラのボタン",
    [UI_MENU_FREECAM_PAD_DESC] = "同時に押すゲームパッドのボタンで、視点を 3 段階で切り替えます: リンクから離れて飛ぶ、リンクがプレイを続ける間その場にとどま"
                                 "る、元に戻る。飛んでいる間、ゲームには入力が届きません: 左スティックで視線の方向と横に飛び、トリガーで上下、右スティックで向きを変え、L か R "
                                 "を押すたびに速度が半分または 2 倍になります。固定中は、リンクは左スティックが画面上で指す方向に歩きます。ゲームのバージョン 1.4.3。",
    [UI_MENU_FREECAM_PAD_V143] = "ゲームのバージョン 1.4.3 のみ",
    [UI_MENU_FREECAM_KEY] = "フリーカメラのキー",
    [UI_MENU_FREECAM_KEY_DESC] = "視点を同じ段階で切り替えるキーです: リンクから離れて飛ぶ、リンクがプレイを続ける間とどまる、元に戻る。飛んでいる間: W A S D で視線の方向と横に飛び、ZR "
                                 "と ZL に割り当てたキー (既定では X と Z) で上下、マウスと矢印キーで向きを変え、L と R に割り当てたキー (Q と R) で速度を半分または 2 "
                                 "倍にします。ゲームのバージョン 1.4.3。",
    [UI_MENU_GROUP_GAMEPLAY_TWEAKS] = "ゲームプレイの調整",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED] = "ゲームプレイの調整",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED_DESC] = "マウスホイールによる、ウルトラハンドの回転と距離、アイテムと武器の切り替え、マップの滑らかなズームの操作です。",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE] = "ホイールでウルトラハンド回転",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE_DESC] = "ウルトラハンドで持っている物を、マウスホイールで直接回します。",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS] = "ウルトラハンドの C 回転軸",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS_DESC] = "C を押しながらスクロールすると、持っている物を別の軸で回します。",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY] = "ウルトラハンドの回転感度",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY_DESC] = "マウスホイールで持っている物が回る速さです。",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE] = "ホイールでウルトラハンドの距離",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE_DESC] = "右クリック + マウスホイールで、持っている物を押し出したり引き寄せたりします。",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY] = "ウルトラハンドの距離感度",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY_DESC] = "マウスホイールで物の距離が変わる速さです。",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL] = "ホイールで武器を選択",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL_DESC] = "フィールドでマウスホイールをスクロールして武器を切り替えます。E で決定します。",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY] = "武器ホイールの感度",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY_DESC] = "次の武器に進むのに必要なスクロールの段数です。",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL] = "ホイールでクイックメニューを選択",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL_DESC] = "クイック選択メニューが開いている間、マウスホイールをスクロールしてアイテムを切り替えます。",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY] = "クイックメニューのホイール感度",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY_DESC] = "クイック選択メニューで次のアイテムに進むのに必要なスクロールの段数です。",
    [UI_MENU_GAMEPLAY_ITEM_AD] = "クイックメニューの A / D 操作",
    [UI_MENU_GAMEPLAY_ITEM_AD_DESC] = "クイック選択メニューで A / D を押して左右に移動します。",
    [UI_MENU_GAMEPLAY_MAP_ZOOM] = "マップの滑らかなズーム",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_DESC] = "マウスホイールで、プルアパッドのマップを慣性付きで滑らかにズームします。",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY] = "マップのズーム感度",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY_DESC] = "マウスホイールでマップがズームする速さです。",
    [UI_MENU_STICK_SENS] = "スティックのカメラ速度",
    [UI_MENU_STICK_SENS_DESC] = "ゲームの 5 段階のカメラ感度それぞれで、右スティックのカメラ速度 (最高速度を含む) を何倍にするかです。マウスは独自の設定を使います。",
    [UI_BINDS_HELP] = "セルを選んで A か Enter を押し、続けてキー、マウスボタン、コントローラーのボタンを押します。Delete か X でセルを空にします。使用中のキーはここに移ります。コントロー"
                      "ラーのボタンは置き換えるボタンと入れ替わります。",
    [UI_BINDS_COL_KEY1] = "キー 1",
    [UI_BINDS_COL_KEY2] = "キー 2",
    [UI_BINDS_COL_PAD] = "コントローラー",
    [UI_BINDS_GROUP_BUTTONS] = "ボタン",
    [UI_BINDS_GROUP_MOVE] = "移動",
    [UI_BINDS_GROUP_CAMERA] = "カメラ",
    [UI_BINDS_PRESS_KEY] = "%s に割り当てるキーかマウスボタンを押してください",
    [UI_BINDS_PRESS_PAD] = "%s に割り当てるコントローラーのボタンを押してください",
    [UI_BINDS_CANCEL_HINT] = "Esc でキャンセル、または %d 秒待ってください",
    [UI_BINDS_TAKEN] = "%s を %s から外しました",
    [UI_BINDS_SWAPPED] = "%s を %s と入れ替えました",
    [UI_BINDS_RESET] = "すべてのボタンを元に戻す",
    [UI_BINDS_RESET_DESC] = "既定のキーとコントローラーのボタンに戻します。上のボタン配置が再び適用されます。割り当てを変えたコントローラーのボタンは、このリセットまで変えた位置のままです。",
    [UI_BIND_A] = "Aボタン",
    [UI_BIND_B] = "Bボタン",
    [UI_BIND_X] = "Xボタン",
    [UI_BIND_Y] = "Yボタン",
    [UI_BIND_LCLICK] = "左スティック押し込み",
    [UI_BIND_RCLICK] = "右スティック押し込み",
    [UI_BIND_L] = "Lボタン",
    [UI_BIND_R] = "Rボタン",
    [UI_BIND_ZL] = "ZLボタン",
    [UI_BIND_ZR] = "ZRボタン",
    [UI_BIND_PLUS] = "+ボタン",
    [UI_BIND_MINUS] = "-ボタン",
    [UI_BIND_LEFT] = "十字キー左",
    [UI_BIND_UP] = "十字キー上",
    [UI_BIND_RIGHT] = "十字キー右",
    [UI_BIND_DOWN] = "十字キー下",
    [UI_BIND_LSTICK_LEFT] = "左へ移動",
    [UI_BIND_LSTICK_UP] = "前へ移動",
    [UI_BIND_LSTICK_RIGHT] = "右へ移動",
    [UI_BIND_LSTICK_DOWN] = "後ろへ移動",
    [UI_BIND_RSTICK_LEFT] = "カメラ左",
    [UI_BIND_RSTICK_UP] = "カメラ上",
    [UI_BIND_RSTICK_RIGHT] = "カメラ右",
    [UI_BIND_RSTICK_DOWN] = "カメラ下",
    [UI_BIND_DESC_A] = "調べる、拾う、話す。",
    [UI_BIND_DESC_B] = "ダッシュ、キャンセル、戻る。",
    [UI_BIND_DESC_X] = "ジャンプ。",
    [UI_BIND_DESC_Y] = "攻撃。",
    [UI_BIND_DESC_LCLICK] = "しゃがむ。",
    [UI_BIND_DESC_RCLICK] = "一人称視点。",
    [UI_BIND_DESC_L] = "能力。",
    [UI_BIND_DESC_R] = "武器を投げる。",
    [UI_BIND_DESC_ZL] = "盾、注目。",
    [UI_BIND_DESC_ZR] = "弓。",
    [UI_BIND_DESC_PLUS] = "メニュー、ポーチ。",
    [UI_BIND_DESC_MINUS] = "マップ。",
    [UI_BIND_DESC_DPAD] = "クイック選択。",
    [UI_BIND_DESC_MOVE] = "左スティックと同じようにリンクを動かします。コントローラーの左スティックもいつでも使えます。",
    [UI_BIND_DESC_CAMERA] = "右スティックと同じようにカメラを回します。マウスとコントローラーの右スティックもいつでも使えます。",
    [UI_BIND_SHAKE] = "右スティックを振る",
    [UI_BIND_DESC_SHAKE] = "ゲームに対して右スティックを左右に振ります: ウルトラハンドで持っている物から、くっつけた部品を外します。キーを押し続けると振り続けます。",
    [UI_KEY_SPACE] = "スペース",
    [UI_KEY_TAB] = "Tab",
    [UI_KEY_LSHIFT] = "左 Shift",
    [UI_KEY_LCTRL] = "左 Ctrl",
    [UI_KEY_UP] = "↑ キー",
    [UI_KEY_DOWN] = "↓ キー",
    [UI_KEY_LEFT] = "← キー",
    [UI_KEY_RIGHT] = "→ キー",
    [UI_KEY_MOUSE_L] = "マウス左",
    [UI_KEY_MOUSE_R] = "マウス右",
    [UI_KEY_MOUSE_M] = "マウス中",
    [UI_KEY_MOUSE_X1] = "マウス 4",
    [UI_KEY_MOUSE_X2] = "マウス 5",
    [UI_KEY_ENTER] = "Enter",
    [UI_KEY_BACKSPACE] = "Backspace",
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
    [UI_KEY_NUM_FMT] = "テンキー %s",
    [UI_PAD_SOUTH] = "下のボタン",
    [UI_PAD_EAST] = "右のボタン",
    [UI_PAD_WEST] = "左のボタン",
    [UI_PAD_NORTH] = "上のボタン",
    [UI_PAD_LSTICK] = "左スティック",
    [UI_PAD_RSTICK] = "右スティック",
    [UI_PAD_L] = "左バンパー",
    [UI_PAD_R] = "右バンパー",
    [UI_PAD_START] = "スタート",
    [UI_PAD_BACK] = "バック",
    [UI_PAD_DPAD_LEFT] = "十字キー左",
    [UI_PAD_DPAD_UP] = "十字キー上",
    [UI_PAD_DPAD_RIGHT] = "十字キー右",
    [UI_PAD_DPAD_DOWN] = "十字キー下",
    [UI_PAD_ZL] = "左トリガー",
    [UI_PAD_ZR] = "右トリガー",
    [UI_PAD_LPADDLE1] = "左背面ボタン",
    [UI_PAD_RPADDLE1] = "右背面ボタン",
    [UI_PAD_LPADDLE2] = "左背面ボタン 2",
    [UI_PAD_RPADDLE2] = "右背面ボタン 2",
    [UI_PAD_TOUCHPAD] = "タッチパッド押し込み",
    [UI_PAD_CROSS] = "×ボタン",
    [UI_PAD_CIRCLE] = "○ボタン",
    [UI_PAD_SQUARE] = "□ボタン",
    [UI_PAD_TRIANGLE] = "△ボタン",
    [UI_MENU_AMIIBO_DIR] = "amiibo フォルダー",
    [UI_MENU_AMIIBO_OPEN] = "開く",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "既定のフォルダー",
    [UI_MENU_AMIIBO_REFRESH] = "更新",
    [UI_MENU_AMIIBO_PICK] = "amiibo のダンプ (540 バイトの .bin または .nfc ファイル):",
    [UI_MENU_AMIIBO_NO_DIR] = "フォルダー %s を開けません",
    [UI_MENU_AMIIBO_READER] = "NFC リーダー",
    [UI_MENU_AMIIBO_STATE] = "リーダー:",
    [UI_MENU_AMIIBO_STATE_IDLE] = "準備完了",
    [UI_MENU_AMIIBO_STATE_SEARCHING] = "ゲームが amiibo を待っています",
    [UI_MENU_AMIIBO_STATE_FOUND] = "amiibo がリーダーの上にあります",
    [UI_MENU_AMIIBO_STATE_MOUNTED] = "ゲームが amiibo を読み込んでいます",
    [UI_MENU_AMIIBO_STATE_OFF] = "オフ",
    [UI_MENU_AMIIBO_FILE] = "選んだファイル:",
    [UI_MENU_AMIIBO_NO_FILE] = "(ファイルが選ばれていません)",
    [UI_MENU_AMIIBO_TAP] = "amiibo をタッチ",
    [UI_MENU_AMIIBO_REMOVE] = "amiibo を離す",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "タッチ後に amiibo を自動で離す",
    [UI_MENU_AMIIBO_HOLD] = "リーダー上の時間",
    [UI_MENU_AMIIBO_SECONDS] = "%.1f 秒",
    [UI_MENU_AMIIBO_TAG] = "リーダー上の amiibo:",
    [UI_MENU_AMIIBO_NO_TAG] = "リーダーに amiibo がありません。",
    [UI_MENU_AMIIBO_TAG_FILE] = "ファイル: %s",
    [UI_MENU_AMIIBO_TAG_ID] = "キャラクター %s、シリーズ %02x、番号 %04x、タイプ %02x",
    [UI_MENU_AMIIBO_TAG_UID] = "UID: %s",
    [UI_MENU_AMIIBO_READY] = "準備完了",
    [UI_MENU_AMIIBO_TAPPED] = "タッチ済み: %s",
    [UI_MENU_AMIIBO_LOAD_FAILED] = "amiibo のダンプ (540 バイトの NTAG215) ではありません: %s",
    [UI_MENU_AMIIBO_REMOVED] = "amiibo を離しました",
    [UI_MENU_AMIIBO_STATUS] = "状態: %s",
    [UI_MENU_MENU_LANG] = "メニューの言語",
    [UI_MENU_MENU_LANG_DESC] = "このメニューとパフォーマンス表示の言語です。",
    [UI_MENU_MENU_LANG_AUTO] = "自動 (Windows に合わせる)",
    [UI_MENU_DISCORD_RPC] = "Discord Rich Presence",
    [UI_MENU_DISCORD_RPC_DESC] = "ゲームの実行中、Discord のステータスにプレイ時間付きで \"TotK Recomp をプレイ中\" と表示します。totk.ini の "
                                 "discord_app_id と discord_image でアプリケーションを選びます。",
    [UI_MENU_ADV_WARNING] = "これらはテストとトラブルシューティング用の設定です。理由がわかっているときだけ変更してください。",
    [UI_MENU_GROUP_SCHED] = "スケジューリング",
    [UI_MENU_GROUP_SHADERS] = "シェーダーとパイプライン",
    [UI_MENU_GROUP_TEXMEM] = "テクスチャとメモリ",
    [UI_MENU_GROUP_FIXES] = "描画の修正",
    [UI_MENU_GROUP_VULKAN] = "Vulkan",
    [UI_MENU_GROUP_DIAG] = "診断",
    [UI_MENU_ASYNC_PREEMPT] = "仮想コアのプリエンプション",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "同じ仮想コア上で、ゲームのスレッドが別のスレッドに割り込めるようにします。",
    [UI_MENU_PREEMPT_OFF] = "オフ",
    [UI_MENU_PREEMPT_ON] = "オン",
    [UI_MENU_PREEMPT_INHERIT] = "継承のみ",
    [UI_MENU_REPLAY_ASYNC] = "GPU のフレーム準備を別スレッドで",
    [UI_MENU_REPLAY_ASYNC_DESC] = "GPU 用のフレームを、ゲームのスレッドではなく別のスレッドで準備します。遅延が最大 1 フレーム増えます。",
    [UI_MENU_REPLAY_WORKERS] = "GPU フレーム準備のスレッド",
    [UI_MENU_REPLAY_WORKERS_DESC] = "大きなフレームを、複数のスレッドで同時に GPU 用に記録します。CPU がフレームレートを制限している場合に効果があります。論理プロセッサーが 8 "
                                    "個以上必要です。オフでは 1 つのスレッドで記録します。",
    [UI_MENU_REPLAY_WORKERS_NOW] = "現在: %s",
    [UI_MENU_TIMER_1MS] = "1 ms の Windows タイマー",
    [UI_MENU_TIMER_1MS_DESC] = "システムタイマーの分解能を 1 ms に上げ、フレーム間隔を安定させます。",
    [UI_MENU_SYNC_AHEAD] = "GPU より 1 フレーム先行",
    [UI_MENU_SYNC_AHEAD_DESC] = "グラフィックカードが前のフレームを描いている間に、ゲームが次のフレームを組み立てます。フレームレートが上がり、入力遅延が 1 フレーム増えます。",
    [UI_MENU_HOST_CHECKPOINT] = "コアの素早い受け渡し",
    [UI_MENU_HOST_CHECKPOINT_DESC] = "ゲームのスレッドは、移植側のコードを抜けた時点で、より重要なスレッドに仮想コアを譲ります。次のシステムコールまで待ちません。",
    [UI_MENU_PIPE_WAIT] = "待機中の描画の扱い",
    [UI_MENU_PIPE_WAIT_DESC] = "「既定」は待機中の描画用にワーカーを確保した現在のスケジューリングを使い、「旧方式 (draw)」は以前のスケジューリングを使います。",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "既定",
    [UI_MENU_PIPE_WAIT_DRAW] = "旧方式 (draw)",
    [UI_MENU_PIPE_SKIP] = "パイプライン作成中に飛ばす描画",
    [UI_MENU_PIPE_SKIP_DESC] = "パイプラインを待たずに捨てる描画の種類です。",
    [UI_MENU_PIPE_SKIP_STRICT] = "厳密",
    [UI_MENU_PIPE_SKIP_ANY] = "すべて",
    [UI_MENU_PIPE_SKIP_FULLSCREEN] = "全画面パス",
    [UI_MENU_LIB_PREBUILD] = "ステージライブラリの事前ビルド",
    [UI_MENU_LIB_PREBUILD_DESC] = "シェーダーのステージライブラリを、バックグラウンドで先にビルドします。",
    [UI_MENU_LIB_PREBUILD_THREADS] = "ステージライブラリ事前ビルドのスレッド数",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "事前ビルドに使うスレッド数です。自動では CPU に合わせて決めます。",
    [UI_MENU_PIPE_PREBUILD] = "パイプラインの事前ビルド",
    [UI_MENU_PIPE_PREBUILD_DESC] = "既知のパイプラインを先にビルドします。",
    [UI_MENU_PREWARM_XLATE] = "トランスレーターの事前準備",
    [UI_MENU_PREWARM_XLATE_DESC] = "起動時に既知のシェーダーを変換するスレッド数です。自動では CPU スレッドの半分 (最大 16) を使います。",
    [UI_MENU_GPL] = "グラフィックスパイプラインライブラリ",
    [UI_MENU_GPL_DESC] = "Vulkan のパイプラインライブラリ拡張を使います。ドライバーとの相性が悪い場合はオフにしてください。",
    [UI_MENU_ASTC] = "ASTC テクスチャのデコード",
    [UI_MENU_ASTC_DESC] = "圧縮された ASTC テクスチャをどこでデコードするかです。",
    [UI_MENU_ASTC_AUTO] = "自動",
    [UI_MENU_ASTC_GPU] = "GPU シェーダー",
    [UI_MENU_ASTC_NATIVE] = "ネイティブ",
    [UI_MENU_ASTC_CPU] = "CPU",
    [UI_MENU_MEMORY] = "GPU メモリモード",
    [UI_MENU_MEMORY_DESC] = "ゲームのメモリを GPU とどう共有するかです。",
    [UI_MENU_MEMORY_AUTO] = "自動",
    [UI_MENU_MEMORY_IMPORT] = "インポート",
    [UI_MENU_MEMORY_MIRROR] = "ミラー",
    [UI_MENU_MEMORY_VERIFY] = "検証",
    [UI_MENU_TEX_ALIAS] = "テクスチャのエイリアス",
    [UI_MENU_TEX_ALIAS_DESC] = "使用中のテクスチャのメモリ上に作られた新しいテクスチャを、空ではなくその内容から始めます。",
    [UI_MENU_TEX_VRAM_PRESSURE] = "テクスチャメモリの上限",
    [UI_MENU_TEX_VRAM_PRESSURE_DESC] = "ビデオメモリの使用率がこのパーセントを超えると、ゲームが解放したテクスチャを破棄します (再び使うときはストレージから戻します)。0 で保持します。",
    [UI_MENU_ASTC_BC] = "ASTC を BC3 に圧縮",
    [UI_MENU_ASTC_BC_DESC] = "GPU デコーダー使用時、デコードした ASTC テクスチャを BC3 に再エンコードし、ビデオメモリを 4 分の 1 にします。4 GB 以下の GPU "
                             "向けです。テクスチャが少しブロック状に見えます。",
    [UI_MENU_LOD_GUARD] = "LOD ガード",
    [UI_MENU_LOD_GUARD_DESC] = "GPU が詳細度データを読む前に、ゲームがその書き換えを終えるのを待つことで、物体が 1 フレームだけ違う場所に表示されたり、遠景が平らに見えたりするのを防ぎます。",
    [UI_MENU_FIX_C0] = "定数バッファの修正",
    [UI_MENU_FIX_C0_DESC] = "ドライバー予約の定数バンク c0 (ストレージサイズ、ディスパッチサイズ、ポイントサイズ、ベースインスタンス) を、ゼロを読む代わりに埋めます。",
    [UI_MENU_FIX_ZERO] = "新しい GPU メモリをゼロで埋める",
    [UI_MENU_FIX_ZERO_DESC] = "Switch のメモリと同じく、新しい GPU メモリプールをゼロで埋めた状態から始めます。",
    [UI_MENU_DLAA_EFFECTS_AFTER] = "DLAA の後にパーティクル",
    [UI_MENU_DLAA_EFFECTS_AFTER_DESC] = "DLAA 使用時、ちりやその他のパーティクルを、DLSS が滑らかにした後の映像の上に描きます。DLSS なしでゲームが描くのと同じです: "
                                        "霧の中の小さなパーティクル (祠) が消えたり残像を残したりしなくなります。霧と雲は引き続き DLSS "
                                        "を通るため、空を背にした遠くの輪郭はきれいなままです。オフ: これまでどおり DLSS がパーティクルも処理します。DLAA "
                                        "モードのみ。アップスケーリングする DLSS モードでは常に処理されます。すぐに反映されます。",
    [UI_MENU_DLSS_FIXED_EXPOSURE] = "DLSS の露出を固定",
    [UI_MENU_DLSS_FIXED_EXPOSURE_DESC] = "DLSS が各フレームから露出を推定せず、ゲームの映像の明るさをそのまま使います: 暗い場面 (地底) "
                                         "で、カメラを動かしている間に霧のかかった広い範囲が 1 フレームだけ暗くなったり明るくなったりしなくなります。オフ: これまでどおり DLSS "
                                         "が自分で露出を推定します。すぐに反映されます。",
    [UI_MENU_BIND_SKIP] = "同じバインドを省略",
    [UI_MENU_BIND_SKIP_DESC] = "最適化: 変わっていないバインドを省略します。",
    [UI_MENU_SHADER_MEMO] = "シェーダーのメモ化",
    [UI_MENU_SHADER_MEMO_DESC] = "最適化: シェーダーの検索結果を覚えておきます。",
    [UI_MENU_PIPE_MEMO] = "パイプラインのメモ化",
    [UI_MENU_PIPE_MEMO_DESC] = "最適化: 状態が同じなら、直前の描画のパイプラインを再利用します。",
    [UI_MENU_TEX_MEMO] = "テクスチャバインドのメモ化",
    [UI_MENU_TEX_MEMO_DESC] = "最適化: 描画間でテクスチャディスクリプターを覚えておきます。",
    [UI_MENU_DESC_TEMPLATE] = "ディスクリプターテンプレート",
    [UI_MENU_DESC_TEMPLATE_DESC] = "最適化: ディスクリプターをまとめて更新します。",
    [UI_MENU_PUSH_SKIP] = "同じプッシュ定数を省略",
    [UI_MENU_PUSH_SKIP_DESC] = "最適化: 変わっていないプッシュ定数を省略します。",
    [UI_MENU_SPLIT_DRAWS] = "GPU への早期送信",
    [UI_MENU_SPLIT_DRAWS_DESC] = "パスの間で N 回の描画ごとに GPU の作業を分けて送り、GPU が早く始められるようにします。0 でオフ。",
    [UI_MENU_VK_VALIDATION] = "検証レイヤー",
    [UI_MENU_VK_VALIDATION_DESC] = "すべての Vulkan 呼び出しを検査し、ゲームがとても遅くなります。",
    [UI_MENU_VK_VALIDATION_MISSING] = "検証レイヤーがインストールされていません。",
    [UI_MENU_VK_DYN_VERTEX] = "動的頂点ステート",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "動的頂点入力を強制的にオンまたはオフにします。既定値は GPU によって決まります。",
    [UI_MENU_VK_DYN_A2C] = "動的 Alpha to Coverage",
    [UI_MENU_VK_DYN_A2C_DESC] = "動的 Alpha to Coverage を強制的にオンまたはオフにします。既定: RDNA 2 以前向けの AMD ドライバーではオフ "
                                "(オンだとキャラクターが黒く描かれるため)、それ以外ではオン。",
    [UI_MENU_VK_SUBGROUP_PIN] = "サブグループサイズを固定",
    [UI_MENU_VK_SUBGROUP_PIN_DESC] = "シェーダーのサブグループサイズを固定します。",
    [UI_MENU_TRACE] = "パフォーマンスログ",
    [UI_MENU_TRACE_DESC] = "フレーム時間をファイルに書き出します。",
    [UI_MENU_FLIGHT] = "フライトレコーダー",
    [UI_MENU_FLIGHT_DESC] = "短い履歴を保持し、フレームが固まったときに保存します。",
    [UI_MENU_FLIGHT_SPIKE_MS] = "フリーズのしきい値 (ms)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "フリーズとみなすフレーム時間です。",
    [UI_MENU_PASS_CRUMBS] = "GPU パスの記録",
    [UI_MENU_PASS_CRUMBS_DESC] = "GPU がクラッシュした後、実行中だった描画パスをログに記録します。自動: AMD のみ。オン: NVIDIA でも記録します (わずかな負荷があります)。",
    [UI_MENU_INPUT_RECORD] = "入力の記録",
    [UI_MENU_INPUT_RECORD_DESC] = "不具合報告用に、入力をファイルに書き出します。",
    [UI_MENU_OPEN_LOG] = "ログフォルダーを開く",
    [UI_MENU_OPEN_LOG_DESC] = "ログファイルのあるフォルダーを表示します。",
    [UI_MENU_COPY_INFO] = "システム情報をコピー",
    [UI_MENU_COPY_INFO_DESC] = "GPU、ドライバー、CPU、RAM、変更した設定をクリップボードにコピーします。",
    [UI_MENU_COPY_INFO_DONE] = "コピーしました",
    [UI_MENU_ACTION_FAILED] = "失敗しました",
    [UI_MENU_RESET] = "すべての設定を既定に戻す",
    [UI_MENU_RESET_DESC] = "すべてのタブに適用されます。",
    [UI_MENU_RESET_CONFIRM] = "すべての設定を既定値に戻しますか？",
    [UI_MENU_RESET_YES] = "戻す",
    [UI_MENU_RESET_NO] = "キャンセル",
    [UI_MENU_OV_FPS] = "%.0f FPS",
    [UI_MENU_OV_FRAME_MS] = "%.1f ms",
    [UI_MENU_OV_GAME_PCT] = "ゲーム %.0f%%",
    [UI_MENU_OV_MAX_MS] = "最大 %.1f ms",
    [UI_MENU_OV_FPS_LABEL] = "FPS",
    [UI_MENU_OV_FRAME_LABEL] = "フレーム",
    [UI_MENU_OV_FMT_FPS] = "%.0f",
    [UI_MENU_OV_LOW1] = "下位 1%% %.0f",
    [UI_MENU_OV_LOW01] = "下位 0.1%% %.0f",
    [UI_MENU_OV_GPU] = "GPU",
    [UI_MENU_OV_CPU] = "CPU",
    [UI_MENU_OV_RAM] = "RAM",
    [UI_MENU_OV_VRAM] = "VRAM",
    [UI_MENU_OV_TEMP] = "温度",
    [UI_MENU_OV_POWER] = "電力",
    [UI_MENU_OV_CLOCK] = "クロック",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "GPU のフレーム準備",
    [UI_MENU_OV_GPU_TIME] = "1 フレームの GPU 使用時間",
    [UI_MENU_OV_PIPE_WAIT] = "パイプライン待ち",
    [UI_MENU_OV_SHADER_WAIT] = "シェーダー待ち",
    [UI_MENU_OV_TEX_UPLOADS] = "テクスチャ転送",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%",
    [UI_MENU_OV_FMT_TEMP] = "%.0f °C",
    [UI_MENU_OV_FMT_POWER] = "%.0f W",
    [UI_MENU_OV_FMT_MHZ] = "%.0f MHz",
    [UI_MENU_OV_FMT_GB] = "%.1f GB",
    [UI_MENU_OV_FMT_GB_OF] = "/ %.1f GB",
    [UI_MENU_OV_GAME_GB] = "ゲーム %.1f GB",
    [UI_MENU_OV_FMT_MS] = "%.2f ms",
    [UI_MENU_OV_PER_FRAME] = "1 フレームあたり %.1f",
    [UI_MENU_OV_WINDOW] = "ウィンドウ",
    [UI_MENU_OV_OUTPUT] = "ゲーム画像",
    [UI_MENU_OV_SCENE] = "3D シーン",
    [UI_MENU_OV_UPSCALER] = "3D アップスケーラー",
    [UI_MENU_OV_SCALING] = "拡大方法",
    [UI_MENU_OV_LIMIT] = "フレーム上限",
    [UI_MENU_OV_PRESENT] = "表示方式",
    [UI_MENU_OV_SHOWN] = "表示",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u",
    [UI_MENU_OV_FMT_SCENE] = "%ux%u、%.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1、シャープネス %d%%",
    [UI_MENU_OV_SCALE_NONE] = "なし (1:1)",
    [UI_MENU_OV_SCALE_BILINEAR] = "バイリニア",
    [UI_MENU_OV_SCALE_DOWN] = "バイリニア (FSR は拡大のみ)",
    [UI_MENU_TAB_CHEATS] = "チート",
    [UI_MENU_CHEATS_NONE] = "このゲームバージョン用のチートはありません。Mod のチートは、その cheats フォルダーにある、ゲームのビルド名を付けた Atmosphère のチートファイルです: "
                            "mods\\<mod>\\cheats\\%s。読み込むにはゲームを再起動してください。ゲームコードを変えるチートの場合、ゲームは起動時に自分を再ビルドします (数分)。",
    [UI_MENU_CHEATS_MOD] = "%s: チート %d 個",
    [UI_MENU_CHEAT_CODE_DESC] = "ゲームコードを変えます。すぐに切り替わります。",
    [UI_MENU_CHEAT_KEY_DESC] = "%s を押している間、ゲームコードを変えます。すぐに切り替わります。",
    [UI_MENU_CHEAT_DATA_DESC] = "オンの間、毎フレームゲームのメモリに書き込みます。",
    [UI_MENU_CHEAT_MASTER] = "常にオン",
    [UI_MENU_CHEAT_MASTER_DESC] = "ファイルのマスターコードです。常にオンです。",
    [UI_MENU_CHEAT_RESTORE] = "不要",
    [UI_MENU_CHEAT_RESTORE_DESC] = "ゲーム本来のコードを書き戻すだけです。チートをオフにすれば同じことが行われます。",
    [UI_MENU_CHEAT_REBUILD] = "再ビルド後",
    [UI_MENU_CHEAT_REBUILD_DESC] = "このチートはゲームコードを変えるもので、ゲームのビルド後に追加または編集されています。次回の起動時に組み込まれます "
                                   "(数分)。または「今すぐゲームを再ビルド」を使ってください。",
    [UI_MENU_CHEAT_UNSUPPORTED] = "非対応",
    [UI_MENU_CHEAT_UNSUPPORTED_DESC] = "このチートはこのバージョンのゲームでは使えません:",
    [UI_MENU_CHEATS_REBUILD] = "今すぐゲームを再ビルド",
    [UI_MENU_CHEATS_REBUILD_DESC] = "ゲームコードを変えるチートの一部がまだ組み込まれていません。新規または編集済みか、前回の再ビルドが失敗またはキャンセルさ"
                                    "れました。ゲームを閉じ、セットアップがそれらを組み込んで (数分) ゲームを起動し直します。最後のセーブ以降の進行状況は失われます。",
    [UI_MENU_MODS_EXEFS_UNSUPPORTED] = "この Mod は移植版が適用できない方法でゲームコード (exefs) を書き換えるため、Mod 全体がオフのままです。%s",
    [UI_MENU_MODS_EXEFS_OTHER] = "コードパッチ (exefs) は別のゲームバージョン用です。",
    [UI_MENU_MODS_EXEFS_ON] = "使用中のコードパッチ: %d ファイル。",
    [UI_MENU_MODS_REPLACES_BUILTIN] = "使用中のコードパッチ: %d ファイル。移植版独自のパッチ %s を置き換え、この Mod がオンの間はそのパッチはオフになります。",
    [UI_MENU_MODS_CODE_SKIPPED] = "独自のゲームコード (%s) は読み込まれませんが、Mod の残りの部分は使われます。ゲームの動作がおかしい場合は Mod をオフにしてください。",
    [UI_MENU_MODS_PLUGIN_ON] = "プラグインが動作中: ゲームコードへのフック %d 個。",
    [UI_MENU_MODS_PLUGIN_REFUSED] = "プラグイン %s は移植版では動作できません: %s。",
    [UI_MENU_MODS_PLUGIN_STAYS_OFF] = "Mod 全体がオフのままです。下の \"... をコードなしで使う\" スイッチで、残りの部分を使えます。",
    [UI_MENU_MODS_PLUGIN_OFF] = "今回はプラグインがオフのままです: ビルド時と異なる状態で起動しました (%s)。ゲームは次回の起動時にプラグインを再ビルドします。",
    [UI_MENU_MODS_WITHOUT_CODE] = "%s をコードなしで使う",
    [UI_MENU_MODS_WITHOUT_CODE_DESC] = "上の Mod には移植版が実行できないコードが含まれています: 独自のゲームコード (subsdk プラグインや置き換えた main) "
                                       "か、移植版が適用できないコードパッチ (exefs) です。オフ: Mod 全体がオフのままです。オン: そのコードを除き、Mod の残りの部分 "
                                       "(RomFS のファイル、チート、適用できるコードパッチ) を使います。Mod のファイルが除いたコードを前提にしていることがあります。ゲー"
                                       "ムの動作がおかしい場合は再びオフにしてください。次回の起動時に反映されます。",
    [UI_MENU_MODS_WITHOUT_CODE_ALL] = "すべての Mod でオン: totk.ini の mods_without_code = 1。",
    [UI_MENU_MODS_EXEFS_SKIPPED] = "コードパッチファイル (exefs) のうち %u 個は適用できないため除外され、Mod の残りの部分が使われます。%s",
    [UI_MENU_MODS_OVERRIDES_FILES] = "%u 個のファイルが、リストでより下にある %s のものを置き換えます。",
    [UI_MENU_MODS_DATA_OVERRIDDEN] = "ゲームデータの変更のうち %u バイトは、リストでより上にある %s のものが使われます。",
    [UI_MENU_MODS_OVERRIDES_DATA] = "ゲームデータの変更のうち %u バイトが、リストでより下にある %s のものを置き換えます。",
    [UI_MENU_MODS_CODE_OVERRIDDEN] = "コードパッチ箇所のうち %u 個は、リストでより上にある %s のものが優先されます。",
    [UI_MENU_MODS_OVERRIDES_CODE] = "%u 個のコードパッチ箇所で、リストでより下にある %s より優先されます。",
};

// Korean
static const char *const text_ko[UI_TEXT_COUNT] = {
    SWKBD("문자 입력", "이름을 입력하세요", "말", "확인", "취소", "컨트롤러: A 확인, B 취소"),

    [UI_MENU_TITLE] = "설정",
    [UI_MENU_TAB_SCREEN] = "화면",
    [UI_MENU_TAB_GAME] = "게임",
    [UI_MENU_TAB_MODS] = "모드",
    [UI_MENU_TAB_SOUND] = "사운드",
    [UI_MENU_TAB_CONTROLS] = "조작",
    [UI_MENU_TAB_AMIIBO] = "amiibo",
    [UI_MENU_TAB_MENU] = "메뉴",
    [UI_MENU_TAB_ADVANCED] = "고급",
    [UI_MENU_CONTINUE] = "계속하기",
    [UI_MENU_QUIT] = "게임 종료",
    [UI_MENU_ON] = "켜기",
    [UI_MENU_OFF] = "끄기",
    [UI_MENU_AUTO] = "자동",
    [UI_MENU_DEFAULT_MARK] = "기본값",
    [UI_MENU_AFTER_RESTART] = "재시작 후",
    [UI_MENU_SET_BY_ENV] = "환경 변수로 고정됨",
    [UI_MENU_SAVE_FAILED] = "설정을 저장할 수 없습니다",
    [UI_MENU_VERSION] = "TotK Recomp %s, 게임 %s",
    [UI_MENU_FULLSCREEN] = "전체 화면",
    [UI_MENU_FULLSCREEN_DESC] = "게임이 화면 전체를 사용합니다. F11로 전환합니다.",
    [UI_MENU_WINDOW_SIZE] = "창 크기",
    [UI_MENU_WINDOW_SIZE_DESC] = "전체 화면이 아닐 때 게임 창의 크기입니다.",
    [UI_MENU_WINDOW_SIZE_CUSTOM] = "사용자 지정",
    [UI_MENU_PRESENT] = "표시 방식",
    [UI_MENU_PRESENT_DESC] = "완성된 프레임을 화면에 내보내는 방식입니다.",
    [UI_MENU_PRESENT_MAILBOX] = "메일박스",
    [UI_MENU_PRESENT_VSYNC] = "수직 동기화",
    [UI_MENU_PRESENT_IMMEDIATE] = "즉시",
    [UI_MENU_PRESENT_IN_USE] = "사용 중: %s",
    [UI_MENU_GPU] = "그래픽 카드",
    [UI_MENU_GPU_DESC] = "게임을 그리는 Vulkan 장치입니다.",
    [UI_MENU_GPU_AUTO] = "자동",
    [UI_MENU_PERF_OVERLAY] = "성능 표시",
    [UI_MENU_PERF_OVERLAY_DESC] = "오른쪽 위에 프레임 속도를 표시합니다. '상세'는 그래프가 있는 프레임 시간, CPU·GPU·메모리 부하, 온도, 렌더링 스레드 대기 시간도 보여 "
                                  "줍니다. F10으로 모드를 전환합니다.",
    [UI_MENU_PERF_OFF] = "끄기",
    [UI_MENU_PERF_FPS] = "FPS",
    [UI_MENU_PERF_FULL] = "상세",
    [UI_PERF_LINE_FPS] = "%.0f fps",
    [UI_PERF_LINE_FPS_GEN] = "%.0f fps 렌더링, %.0f 표시",
    [UI_MENU_GRAPHICS] = "그래픽",
    [UI_MENU_OUTPUT_RES] = "게임 해상도",
    [UI_MENU_OUTPUT_RES_DESC] = "게임이 화면과 인터페이스를 그리는 크기입니다. 1920 x 1080을 넘으면 비디오 메모리와 GPU 시간이 더 필요합니다. 더 넓은 비율(21:9)은 "
                                "카메라 시야를 넓히고 HUD를 화면 모서리로 옮기며, 더 높은 비율(16:10, Steam Deck)은 위아래 가장자리로 옮깁니다.",
    [UI_MENU_HUD_WIDTH] = "HUD 너비",
    [UI_MENU_HUD_WIDTH_DESC] = "16:9보다 넓은 화면에서 HUD의 왼쪽과 오른쪽이 얼마나 떨어질지입니다. '전체 화면'은 화면 가장자리에 두고, 21:9나 16:9는 HUD를 그 비율 "
                               "화면의 가운데 안에 둡니다. 없는 레이아웃은 백그라운드에서 만들어 재시작 후 사용합니다.",
    [UI_MENU_HUD_WIDTH_FULL] = "전체 너비",
    [UI_MENU_RENDER_SCALE] = "3D 렌더링 배율",
    [UI_MENU_RENDER_SCALE_DESC] = "게임 해상도에 대한 3D 장면의 크기입니다. 게임의 FSR이 그 크기로 확대합니다. 100 %를 넘으면 계단 현상이 부드러워지지만(슈퍼샘플링) "
                                  "GPU 부하가 크게 늘어납니다.",
    [UI_MENU_RENDER_SCALE_GAME] = "게임 (83 %)",
    [UI_MENU_DYNAMIC_RES] = "동적 해상도",
    [UI_MENU_DYNAMIC_RES_DESC] = "게임이 3D 장면을 90 또는 80 %까지 낮출 수 있게 합니다. GPU를 고려하지 않는 Switch용 추정치로 판단하므로 빠른 PC에서도 넓은 "
                                 "장면에서는 낮아집니다. 끄면 렌더링 배율을 유지합니다.",
    [UI_MENU_UPSCALER] = "창에 맞춰 확대",
    [UI_MENU_UPSCALER_DESC] = "창이 게임 해상도보다 클 때의 확대 방식입니다. FSR은 바이리니어보다 윤곽과 세부를 더 선명하게 유지합니다.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1",
    [UI_MENU_UPSCALER_BILINEAR] = "바이리니어",
    [UI_MENU_ASPECT] = "화면 비율",
    [UI_MENU_ASPECT_DESC] = "창의 비율이 게임 해상도의 비율과 다를 때(21:9 화면에 16:9 영상 등): 검은 띠를 넣어 비율을 유지하거나 창에 맞게 늘립니다.",
    [UI_MENU_ASPECT_FIT] = "검은 띠",
    [UI_MENU_ASPECT_STRETCH] = "늘리기",
    [UI_MENU_MOVIE_ASPECT] = "동영상 비율",
    [UI_MENU_MOVIE_ASPECT_DESC] = "게임 해상도가 16:9가 아닐 때 컷신 동영상의 원래 비율을 검은 띠로 유지하거나 영상 전체로 늘립니다. 다음 동영상부터 적용됩니다.",
    [UI_MENU_MOVIE_ASPECT_KEEP] = "검은 띠",
    [UI_MENU_MOVIE_ASPECT_STRETCH] = "늘리기",
    [UI_MENU_FOV] = "시야각",
    [UI_MENU_FOV_DESC] = "카메라가 보여 주는 범위를 게임 원래 시야에 대한 퍼센트로 지정합니다. 게임 버전 1.4.3 전용입니다.",
    [UI_MENU_VERT_PLUS] = "16:9 너비 유지",
    [UI_MENU_VERT_PLUS_DESC] = "16:9보다 좁은 화면(Steam Deck, 16:10, 4:3)에서 카메라가 16:9와 같은 너비의 세계를 보여 주고 위아래는 더 많이 보여 줍니다. 16:9 이상 넓은 화면에서는 효과가 없습니다.",
    [UI_MENU_SHADOW_RES] = "그림자 맵 크기",
    [UI_MENU_SHADOW_RES_DESC] = "태양 그림자 맵의 크기입니다. 게임 원래 크기는 시점에 따라 바뀌며 지면 가까이에서는 약 400까지 내려가서 나뭇잎 그림자가 각지고 튀게 됩니다. 고정 "
                                "크기로 하면 그림자가 안정되고 선명해집니다. 1024를 넘으면 GPU 시간과 비디오 메모리가 늘어납니다.",
    [UI_MENU_SHADOW_RES_GAME] = "게임 (가변)",
    [UI_MENU_ANISO] = "텍스처 필터링",
    [UI_MENU_ANISO_DESC] = "표면 텍스처의 비등방성 필터링입니다. 비스듬히 보이는 땅, 길, 벽이 흐려지지 않고 먼 곳까지 선명하게 유지됩니다. '게임'은 Switch처럼 게임이 요청하는 "
                           "수준입니다.",
    [UI_MENU_ANISO_GAME] = "게임",
    [UI_MENU_LOD_STEADY] = "부하 시에도 세부 유지",
    [UI_MENU_LOD_STEADY_DESC] = "프레임 속도가 떨어질 때 모델과 텍스처의 세부를 낮추는 게임 설정을 건너뜁니다. 에뮬레이터 모드 Disable LOD Quality Reduction과 "
                                "같은 효과입니다. 게임 버전 1.1.0, 1.2.1, 1.4.3 전용입니다. 성능이 낮은 PC에서는 끄세요. 그런 PC에서는 이 저하가 프레임 속도를 유지해 줍니다.",
    [UI_MENU_SHARPNESS] = "FSR 선명도",
    [UI_MENU_SHARPNESS_DESC] = "FSR로 확대한 뒤의 선명화입니다. 0이면 꺼집니다.",
    [UI_MENU_COLOR] = "색상",
    [UI_MENU_COLOR_PRESET] = "색상 프로필",
    [UI_MENU_COLOR_PRESET_DESC] = "아래 네 값을 한 번에 설정합니다. 비비드: 대비와 채도를 높이고 어두운 부분을 조금 밝게 합니다(OLED 화면용). 클리어: 대비, 채도, 밝기를 "
                                  "조금 높입니다. 값을 움직이면 프로필이 '사용자 지정'으로 바뀝니다. HUD는 영상과 함께 바뀌지만 이 메뉴는 바뀌지 않습니다.",
    [UI_MENU_COLOR_DEFAULT] = "게임 기본",
    [UI_MENU_COLOR_VIVID] = "비비드 (OLED)",
    [UI_MENU_COLOR_CLARITY] = "클리어",
    [UI_MENU_COLOR_CUSTOM] = "사용자 지정",
    [UI_MENU_COLOR_BRIGHTNESS] = "밝기",
    [UI_MENU_COLOR_BRIGHTNESS_DESC] = "모든 색에 더해집니다. 0이면 변화 없음.",
    [UI_MENU_COLOR_CONTRAST] = "대비",
    [UI_MENU_COLOR_CONTRAST_DESC] = "색을 중간 회색에서 멀어지게(100 % 초과) 하거나 가까워지게 합니다. 100 %면 변화 없음.",
    [UI_MENU_COLOR_SATURATION] = "채도",
    [UI_MENU_COLOR_SATURATION_DESC] = "회색 대비 색의 강도입니다. 100 %면 변화 없음.",
    [UI_MENU_COLOR_GAMMA] = "감마",
    [UI_MENU_COLOR_GAMMA_DESC] = "1 미만이면 어두운 부분과 중간 톤이 밝아지고, 1 초과면 어두워집니다. 1이면 변화 없음.",
    [UI_MENU_HDR_GROUP] = "HDR",
    [UI_MENU_HDR] = "HDR 출력",
    [UI_MENU_HDR_DESC] = "HDR 디스플레이에서 밝은 하이라이트를 표시합니다. Windows에서 HDR을 켜거나('HDR 사용') 색 관리를 지원하는 Wayland 컴포지터가 필요하며, "
                         "없으면 SDR로 유지됩니다. 재시작 후 적용됩니다. 위의 색상 프로필은 SDR용이며 HDR에서는 사용되지 않습니다.",
    [UI_MENU_HDR_PEAK] = "최대 밝기",
    [UI_MENU_HDR_PEAK_DESC] = "디스플레이의 최대 밝기(니트)입니다. 가장 밝은 하이라이트는 여기서 멈춥니다.",
    [UI_MENU_HDR_GAME] = "게임 밝기",
    [UI_MENU_HDR_GAME_DESC] = "영상의 흰색(페이퍼 화이트) 니트 값입니다. 203이 HDR 기준입니다. 밝은 방에서는 올리세요.",
    [UI_MENU_HDR_UI] = "HUD 및 메뉴 밝기",
    [UI_MENU_HDR_UI_DESC] = "HUD와 이 메뉴의 흰색 니트 값입니다.",
    [UI_MENU_HDR_HUE] = "하이라이트 색조 보정",
    [UI_MENU_HDR_HUE_DESC] = "불이나 불똥 같은 밝은 색을 게임 원래 SDR 영상처럼 흰색 쪽으로 바꿔, 그 색이 비추는 빛과 맞춥니다. 0이면 채도를 그대로 유지합니다.",
    [UI_MENU_SHADOW_SCALE] = "그림자 해상도",
    [UI_MENU_SHADOW_SCALE_DESC] = "태양 그림자 맵을 게임 기본 960x960보다 각 변에서 이 배율만큼 크게 그립니다. 그림자 윤곽과 나뭇잎 그림자가 선명해집니다. 다음 실행 시 "
                                  "적용됩니다.",
    [UI_MENU_PCSS] = "부드러운 그림자",
    [UI_MENU_PCSS_DESC] = "물체가 땅에 닿는 곳에서는 태양 그림자를 선명하게, 그림자가 멀리 드리울수록 부드럽게 합니다(PCSS). 0이면 게임의 필터를 유지합니다. 다음 실행 시 "
                          "적용됩니다.",
    [UI_MENU_CONTACT_SHADOWS] = "접촉 그림자",
    [UI_MENU_CONTACT_SHADOWS_DESC] = "그림자 맵이 놓치는 작은 태양 그림자입니다. 발밑, 돌 아래, 풀숲, 움푹한 곳 등. 조명 후 화면 공간에서 계산합니다. 다음 실행 시 "
                                     "적용됩니다.",
    [UI_MENU_SHADOW_RANGE] = "그림자 단계 범위",
    [UI_MENU_SHADOW_RANGE_DESC] = "그림자가 부드러워지는 단계(게임에서는 20, 40, 80, 160 m)를 더 멀리 옮깁니다. 그림자 맵이 더 넓은 범위에 퍼지므로 그림자 해상도도 "
                                  "함께 올리세요. 다음 실행 시 적용됩니다.",
    [UI_MENU_GRASS_RANGE] = "풀 표시 거리",
    [UI_MENU_GRASS_RANGE_DESC] = "카메라 주변 어디까지 풀이 자라는지입니다(게임: 62.5 m). 높이면 초원의 풀이 바로 앞에서 자라나지 않고 더 멀리까지 보이며, 낮추면 풀을 덜 "
                                 "그립니다. 다음 실행 시 적용됩니다.",
    [UI_MENU_DLSS_PRESET] = "DLSS 모델",
    [UI_MENU_DLSS_PRESET_DESC] = "어느 DLSS 모델을 실행할지입니다. '자동'은 모드별 NVIDIA 권장을 따릅니다: DLAA, 품질, 균형은 K, 성능은 M, 울트라 성능은 L. "
                                 "J: 움직이는 캐릭터 뒤의 잔상이 약간 적고 깜박임이 조금 많습니다. K: 모든 모드에서 가장 선명한 모델입니다. 즉시 적용됩니다.",
    [UI_MENU_DLSS_PRESET_DEFAULT] = "자동",
    [UI_MENU_DLSS_PRESET_K] = "K (가장 선명)",
    [UI_MENU_DLSS_PRESET_J] = "J (잔상 적음)",
    [UI_MENU_DLSS_CHARS] = "DLSS 캐릭터 잔상",
    [UI_MENU_DLSS_CHARS_DESC] = "움직이는 캐릭터와 동물, 그들이 든 물건 뒤의 희미한 잔상을 줄입니다. 그 부분에서는 DLSS가 이전 프레임을 덜 사용합니다. 강할수록 잔상이 더 "
                                "사라지고 그 윤곽이 조금 덜 부드러워집니다. 즉시 적용됩니다.",
    [UI_MENU_DLSS_CHARS_OFF] = "끄기",
    [UI_MENU_DLSS_CHARS_LIGHT] = "약하게",
    [UI_MENU_DLSS_CHARS_MEDIUM] = "보통",
    [UI_MENU_DLSS_CHARS_FULL] = "최대",
    [UI_MENU_DLSS] = "업스케일링 모드",
    [UI_MENU_DLSS_DESC] = "'네이티브'는 3D 장면을 전체 해상도에서 안티에일리어싱합니다. '품질', '균형', '성능', '울트라 성능'은 장면을 67, 58, 50, 33 %로 그리고 "
                          "업스케일러(DLSS 또는 FSR 3.1)로 확대해 GPU 시간을 절약하며, 3D 렌더링 배율을 대신합니다. 즉시 적용됩니다.",
    [UI_MENU_DLSS_DLAA] = "네이티브",
    [UI_MENU_DLSS_QUALITY] = "품질",
    [UI_MENU_DLSS_BALANCED] = "균형",
    [UI_MENU_DLSS_PERFORMANCE] = "성능",
    [UI_MENU_DLSS_ULTRA] = "울트라 성능",
    [UI_MENU_FXAA] = "게임 안티에일리어싱 (FXAA)",
    [UI_MENU_FXAA_DESC] = "게임 기본 FXAA 윤곽 부드럽게 하기입니다. 세부가 약간 흐려집니다. DLAA가 켜져 있으면 필요 없습니다.",
    [UI_MENU_REFLEX] = "낮은 지연",
    [UI_MENU_REFLEX_DESC] = "시스템 지연을 줄입니다. 켜기는 NVIDIA Reflex(VK_NV_low_latency2)를 사용하고, 켜기 + 부스트는 CPU 병목 장면에서 NVIDIA GPU 클럭도 높게 유지합니다. 자동은 NVIDIA에서는 Reflex, AMD에서는 AMD Anti-Lag 2(VK_AMD_anti_lag)를 사용합니다. 다시 시작한 뒤 적용됩니다.",
    [UI_MENU_REFLEX_AUTO] = "자동",
    [UI_MENU_REFLEX_OFF] = "끄기",
    [UI_MENU_REFLEX_ON] = "켜기",
    [UI_MENU_REFLEX_BOOST] = "켜기 + 부스트",
    [UI_MENU_SCENE_UPSCALER] = "3D 장면 업스케일러",
    [UI_MENU_SCENE_UPSCALER_DESC] = "3D 장면용 시간적 업스케일러입니다. 윤곽이 더 매끄럽고 깜박임이 줄며, '네이티브'보다 낮은 모드에서는 장면을 더 작게 그린 뒤 확대해 GPU "
                                    "시간을 절약합니다. NVIDIA DLSS를 쓰려면 NVIDIA RTX GPU가 필요하고 " DLSS_FILE " 파일이 " EXE_FILE " "
                                    "옆에 있어야 합니다. AMD FSR 3.1은 모든 GPU에서 작동합니다. '끄기'는 게임 자체의 확대와 FXAA를 유지합니다. HUD는 건드리지 "
                                    "않으며, 위의 '창에 맞춰 확대'와 선명도가 여전히 게임 이미지를 창에 맞춥니다. 재시작 후 적용됩니다.",
    [UI_MENU_SCENE_UPSCALER_OFF] = "끄기",
    [UI_MENU_SCENE_UPSCALER_DLSS] = "NVIDIA DLSS",
    [UI_MENU_SCENE_UPSCALER_FSR3] = "AMD FSR 3.1",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_GPU] = "DLSS: NVIDIA RTX GPU 필요",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_DLL] = "DLSS: " DLSS_FILE " 없음",
    [UI_MENU_FSR_SHARPNESS] = "FSR 선명도",
    [UI_MENU_FSR_SHARPNESS_DESC] = "AMD FSR 3.1 또는 FSR 4 이후 3D 장면의 선명도를 높입니다. 0이면 꺼집니다.",
    [UI_MENU_SCENE_UPSCALER_FSR4] = "AMD FSR 4",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_GPU] = "FSR 4: 이 GPU에서는 제공되지 않음",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_DLL] = "FSR 4: AMD FSR 라이브러리가 없음",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_OS] = "FSR 4: Windows 전용",
    [UI_MENU_FPS_LIMIT] = "프레임 속도 제한",
    [UI_MENU_FPS_LIMIT_DESC] = "게임이 초당 그리는 최대 프레임 수입니다. 60을 넘는 설정은 실험적이며 '게임 속도를 시계에 맞추기'가 필요합니다.",
    [UI_MENU_FPS_30] = "30 FPS",
    [UI_MENU_FPS_40] = "40 FPS",
    [UI_MENU_FPS_45] = "45 FPS",
    [UI_MENU_FPS_60] = "60 FPS",
    [UI_MENU_FPS_90] = "90 FPS",
    [UI_MENU_FPS_120] = "120 FPS",
    [UI_MENU_FPS_144] = "144 FPS",
    [UI_MENU_FPS_DISPLAY] = "모니터 주사율",
    [UI_MENU_FPS_DISPLAY_HZ] = "모니터 주사율 (%.0f Hz)",
    [UI_MENU_FRAME_QUEUE] = "프레임 대기열",
    [UI_MENU_FRAME_QUEUE_DESC] = "이전 프레임이 화면에 표시될 차례를 기다리는 동안 게임이 다음 프레임을 시작합니다. 제한 속도에서 빠지는 프레임이 훨씬 줄고, 입력 지연은 최대 "
                                 "1프레임 늘어납니다. 즉시 적용됩니다.",
    [UI_MENU_FRAME_GEN] = "프레임 생성",
    [UI_MENU_FRAME_GEN_OFF] = "끄기",
    [UI_MENU_FRAME_GEN_FSR3] = "AMD FSR 3",
    [UI_MENU_FRAME_GEN_DESC] = "게임이 그리는 두 프레임 사이마다 생성된 프레임을 넣습니다. 화면의 프레임은 두 배가 되고, 게임은 프레임 제한의 절반으로 돌며 조작은 약 반 프레임 "
                               "늦어집니다. 120 Hz 이상 디스플레이용입니다. DLSS나 HDR과는 함께 쓸 수 없습니다. 즉시 적용됩니다.",
    [UI_MENU_REALTIME] = "게임 속도를 시계에 맞추기",
    [UI_MENU_REALTIME_DESC] = "게임을 실제 시간에 맞춰 진행해 느린 프레임이 있어도 게임이 느려지지 않게 합니다.",
    [UI_MENU_CLOTH_REALTIME] = "천을 시계에 맞추기",
    [UI_MENU_CLOTH_REALTIME_DESC] = "천과 머리카락도 실제 시간에 맞춰 진행합니다.",
    [UI_MENU_HIGHFPS_FIX] = "높은 프레임 속도 수정",
    [UI_MENU_HIGHFPS_FIX_DESC] = "30 fps를 넘으면 이상해지는 물리와 60을 넘으면 비치거나 사라지는 물체를 수정합니다.",
    [UI_MENU_CLIMB_FIX] = "등반 속도 수정",
    [UI_MENU_CLIMB_FIX_DESC] = "초당 60프레임을 넘어도 벽과 사다리를 오르는 속도를 제대로 유지해 링크가 꼭대기까지 오르게 합니다.",
    [UI_MENU_GAME_LANG] = "게임 언어",
    [UI_MENU_GAME_LANG_DESC] = "게임 텍스트와 음성의 언어입니다.",
    [UI_MENU_GAME_LANG_AUTO] = "자동",
    [UI_MENU_OPERATION_MODE] = "본체 모드",
    [UI_MENU_OPERATION_MODE_DESC] = "게임이 TV 모드와 휴대 모드 중 어느 쪽에서 실행 중이라고 생각할지입니다.",
    [UI_MENU_MODE_DOCKED] = "TV 모드",
    [UI_MENU_MODE_HANDHELD] = "휴대 모드",
    [UI_MENU_ASSUME_FOCUS] = "포커스 없이도 실행",
    [UI_MENU_ASSUME_FOCUS_DESC] = "켜면(기본값) 다른 창에 있는 동안에도 게임이 계속 실행되며, 창이 다시 앞으로 올 때까지 키보드는 반응하지 않습니다. 끄면 Switch에서 "
                                  "HOME 버튼을 누른 것처럼 창이 포커스를 잃을 때 일시 정지합니다. 이렇게 중단된 로딩은 멈춘 채로 남을 수 있습니다.",
    [UI_MENU_PLAY_TIME] = "플레이 시간",
    [UI_MENU_PLAY_TIME_DESC] = "모든 실행에 걸친 게임 시간으로, 게임 창에 포커스가 있고 게임이 프레임을 그리는 동안 셉니다. 프로필 폴더의 저장 데이터 옆 playtime.txt에 "
                               "보관됩니다.",
    [UI_MENU_PLAY_TIME_HM] = "%llu시간 %02u분",
    [UI_MENU_PLAY_TIME_SESSION] = "이번 실행: %s",
    [UI_MENU_MODS_BUILTIN] = "포트에 내장",
    [UI_MENU_MODS_INSTALLED] = "설치된 모드",
    [UI_MENU_MODS_NONE] = "아직 모드가 없습니다. 모드를 mods 폴더에 압축 해제하고(모드마다 폴더 하나, 그 안에 romfs 폴더) 게임을 재시작하세요. TKMM도 병합한 모드를 그곳에 "
                          "폴더로 내보낼 수 있습니다.",
    [UI_MENU_MODS_OPEN] = "mods 폴더 열기",
    [UI_MENU_MODS_OPEN_DESC] = "게임이 모드를 읽는 폴더를 보여 줍니다. 없으면 만듭니다.",
    [UI_MENU_MODS_ROW_DESC] = "교체하는 게임 파일: %u. 추가하는 새 파일: %u.",
    [UI_MENU_MODS_OVERRIDDEN] = "파일 %u개는 목록에서 더 위에 있는 %s의 것을 사용합니다.",
    [UI_MENU_MODS_MOVE_HINT] = "위 버튼이나 Ctrl+↑로 위로, 왼쪽 버튼이나 Ctrl+↓로 아래로 옮깁니다. 두 모드가 같은 파일을 바꾸면 위에 있는 모드가 우선합니다. 변경은 "
                               "재시작 후 적용됩니다.",
    [UI_MENU_MODS_EMPTY] = "이 폴더에는 romfs, exefs, cheats 파일이 없습니다.",
    [UI_MENU_MODS_SUBSDK] = "이 모드는 자체 게임 코드(subsdk 플러그인이나 교체된 main)를 포함합니다. 포트는 코드 모듈을 불러올 수 없으므로 모드 전체가 꺼진 상태로 유지됩니다.",
    [UI_MENU_MODS_EXEFS_LATER] = "이 모드는 게임 코드를 수정합니다(exefs). 다음 실행 시 패치를 빌드해 넣습니다(몇 분간 재빌드). 그때까지 모드 전체가 꺼진 상태로 유지됩니다.",
    [UI_MENU_MODS_CHEATS] = "치트: %d개. '치트' 탭에서 전환합니다.",
    [UI_MENU_MODS_CHEATS_FILE] = "이 게임 버전용 치트는 시작 시 이 모드의 cheats\\%s에서 읽습니다.",
    [UI_MENU_MODS_BAD_NAME] = "폴더 이름에 |가 있습니다. 모드 목록은 이름 구분에 |를 사용합니다. 폴더 이름을 바꾸세요.",
    [UI_MENU_MODS_OTHER_VERSION] = "파일 %u개는 다른 게임 버전용이라 제외됩니다. 사용 중인 게임 버전용 이 모드를 찾으세요.",
    [UI_MENU_MODS_UNUSED] = "사용 안 함",
    [UI_MENU_MODS_FPS60] = "60 FPS 패치",
    [UI_MENU_MODS_FPS60_DESC] = "60 fps용 포트 자체 패치입니다. '게임' 탭의 프레임 속도 제한으로 설정합니다.",
    [UI_MENU_MODS_RES] = "해상도 패치",
    [UI_MENU_MODS_RES_DESC] = "1080p를 넘는 해상도용 포트 자체 패치입니다. '화면' 탭의 게임 해상도로 설정합니다.",
    [UI_MENU_MODS_QUICKMENU] = "빠른 메뉴 수정",
    [UI_MENU_MODS_QUICKMENU_DESC] = "60 FPS에서 무기, 방패, 소재 메뉴의 배경이 검게 되는 문제에 대한 포트의 수정입니다.",
    [UI_MENU_VOLUME] = "음량",
    [UI_MENU_VOLUME_DESC] = "게임 전체 음량입니다.",
    [UI_MENU_VOLUME_MUSIC] = "음악 음량",
    [UI_MENU_VOLUME_MUSIC_DESC] = "배경 음악의 음량입니다.",
    [UI_MENU_VOLUME_SFX] = "효과음 음량",
    [UI_MENU_VOLUME_SFX_DESC] = "효과음의 음량입니다.",
    [UI_MENU_VOLUME_VOICE] = "음성 음량",
    [UI_MENU_VOLUME_VOICE_DESC] = "캐릭터 음성의 음량입니다.",
    [UI_MENU_AUDIO_CHANNELS] = "스피커",
    [UI_MENU_AUDIO_CHANNELS_DESC] = "스테레오 또는 5.1 서라운드 출력입니다. '자동'은 Windows 스피커 설정이 6채널 이상이면 5.1을 고릅니다. 재시작 후 적용됩니다.",
    [UI_MENU_CHANNELS_AUTO] = "자동",
    [UI_MENU_CHANNELS_STEREO] = "스테레오",
    [UI_MENU_CHANNELS_51] = "5.1 서라운드",
    [UI_MENU_AUDIO_DEVICE] = "출력 장치",
    [UI_MENU_AUDIO_DEVICE_DESC] = "소리가 나오는 곳입니다. '시스템 기본값'은 Windows 기본 장치를 따릅니다. 선택한 장치가 분리되면 다시 연결될 때까지 시스템 기본값으로 "
                                  "출력합니다. 즉시 적용됩니다.",
    [UI_MENU_AUDIO_DEVICE_DEFAULT] = "시스템 기본값",
    [UI_MENU_MOTION] = "모션 조작",
    [UI_MENU_MOTION_DESC] = "컨트롤러의 자이로로 조준합니다.",
    [UI_MENU_GYRO_CALIBRATION] = "자이로 오프셋 보정",
    [UI_MENU_GYRO_CALIBRATION_DESC] = "아래 '자이로 보정'으로 측정한, 정지 상태에서의 컨트롤러 드리프트를 뺍니다. 이 설정을 꺼도 측정값은 저장된 채로 남습니다.",
    [UI_MENU_GYRO_CALIBRATE] = "자이로 보정",
    [UI_MENU_GYRO_CALIBRATE_DESC] = "정지 상태에서의 자이로스코프 드리프트를 측정해 제거합니다. 컨트롤러를 내려놓거나 2초 동안 가만히 들고 있으세요.",
    [UI_MENU_GYRO_CALIBRATE_WAIT] = "보정 중...",
    [UI_MENU_GYRO_CALIBRATED] = "%s, 보정됨",
    [UI_MENU_GYRO_CALIBRATE_FAILED] = "보정 실패",
    [UI_MENU_GYRO_CALIBRATE_MOVED] = "컨트롤러가 움직였습니다: 가만히 들고 다시 시도하세요",
    [UI_MENU_GYRO_AUTO_BIAS] = "정지 시 자이로 드리프트 학습",
    [UI_MENU_GYRO_AUTO_BIAS_DESC] = "컨트롤러가 책상 위에 가만히 놓여 있을 때마다 자이로에 남은 드리프트를 학습해 뺍니다. 보정 후에도 활 조준이 천천히 흘러갈 때 쓰세요. 손에 "
                                    "든 컨트롤러로는 학습하지 않습니다. 실험적 기능입니다.",
    [UI_MENU_RUMBLE_STRENGTH] = "진동 세기",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "컨트롤러 진동의 세기입니다.",
    [UI_MENU_RUMBLE_CURVE] = "진동 곡선",
    [UI_MENU_RUMBLE_CURVE_DESC] = "값을 낮추면 약한 진동이 강해집니다.",
    [UI_MENU_RUMBLE_TRIGGERS] = "트리거 진동 (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Xbox 컨트롤러의 트리거도 진동시킵니다.",
    [UI_MENU_PAD_LAYOUT] = "버튼 배치",
    [UI_MENU_PAD_LAYOUT_DESC] = "어느 버튼을 A로 할지입니다. Nintendo: Switch처럼 오른쪽 버튼. Xbox: Xbox 컨트롤러 표기대로 아래 버튼(PlayStation "
                                "컨트롤러에서는 ×). 자동은 Nintendo 표기 컨트롤러에는 Nintendo를, 그 외에는 Xbox를 고릅니다. 게임의 버튼 표시에 어느 컨트롤러를 "
                                "쓸지는 '버튼 표시'에서 설정합니다.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "자동",
    [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A가 오른쪽)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A가 아래)",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "사용 중: %s",
    [UI_MENU_PROMPTS] = "버튼 표시",
    [UI_MENU_PROMPTS_DESC] = "게임의 버튼 표시가 어느 컨트롤러의 버튼을 보여 줄지입니다. Nintendo: 게임 원래 표시. Xbox: LB, RB, LT, RT, Menu, View, "
                             "방향 패드 십자, 그리고 A, B, X, Y는 위의 버튼 배치가 누르는 버튼의 표기대로 표시합니다(Nintendo (A가 오른쪽)에서는 오른쪽 버튼이 B로 "
                             "표기되므로 A의 표시가 B가 됩니다). PlayStation: PlayStation 컨트롤러의 기호(○ × △ □, L1, R1, L2, R2, "
                             "Options, Share)를 위의 버튼 배치가 누르는 버튼에 표시합니다. PC: 키보드 기본 배치의 키와 마우스 버튼(E, Shift, 스페이스, 왼쪽 "
                             "클릭...)과 카메라용 마우스. 게임은 시작할 때 버튼 표시를 읽으므로 재시작 후 바뀝니다. 버튼 표시는 게임 파일의 일부라서 '조작' 탭에서 다시 지정한 "
                             "버튼에는 화면에 예전 표시가 남습니다.",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo",
    [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation",
    [UI_MENU_PROMPTS_PC] = "PC",
    [UI_MENU_PROMPTS_MISSING] = "버튼 표시 파일이 없습니다: 설치를 다시 실행하세요",
    [UI_MENU_PROMPTS_FAILED] = "버튼 표시 파일이 손상되었습니다: 설치를 다시 실행하세요",
    [UI_MENU_PROMPTS_RESTART] = "표시는 재시작 후 바뀝니다",
    [UI_MENU_PROMPTS_OFF_SHAPE] = "게임 해상도가 16:9가 아니라 꺼짐",
    [UI_MENU_OUTPUT_RES_NO_LAYOUT] = "이 비율용 레이아웃이 없습니다: 설치를 다시 실행하세요",
    [UI_MENU_OUTPUT_RES_PREPARING] = "백그라운드에서 레이아웃을 만드는 중입니다. 다음 실행 시 사용됩니다",
    [UI_MENU_PROMPTS_PREPARING] = "백그라운드에서 버튼 표시 파일을 만드는 중입니다. 다음 실행 시 사용됩니다",
    [UI_MENU_PREPARE_RETRY] = "다시 시도",
    [UI_MENU_PREPARE_RETRY_DESC] = "설치 프로그램의 도구를 다시 시작해 없는 인터페이스 레이아웃이나 버튼 표시 파일을 백그라운드에서 만듭니다. 게임은 계속 실행되며, 다음 실행부터 "
                                   "그 파일을 사용합니다.",
    [UI_MENU_SHAPE_PREPARE_FAILED] = "설치 프로그램의 도구가 시작되지 않았습니다: 설치를 다시 실행하세요",
    [UI_MENU_PROMPTS_OFF_MODS] = "꺼짐: RomFS 모드가 파일을 교체함",
    [UI_MENU_PROMPTS_OFF_PATCHES] = "TOTK_ROMFS_PATCHES가 설정된 동안 꺼짐",
    [UI_MENU_RESTART] = "지금 재시작",
    [UI_MENU_RESTART_DESC] = "게임을 닫고 새 버튼 표시로 다시 시작합니다. 마지막 저장 이후의 진행은 사라집니다.",
    [UI_MENU_RESTART_CONFIRM] = "지금 게임을 재시작할까요? 마지막 저장 이후의 진행은 사라집니다.",
    [UI_MENU_RESTART_YES] = "재시작",
    [UI_MENU_RESTART_SAVING] = "게임이 저장 중입니다. 잠시 후 재시작할 수 있습니다.",
    [UI_MENU_GROUP_STICKS] = "아날로그 스틱",
    [UI_MENU_LSTICK_DEADZONE] = "왼쪽 스틱 데드존",
    [UI_MENU_RSTICK_DEADZONE] = "오른쪽 스틱 데드존",
    [UI_MENU_STICK_DEADZONE_DESC] = "스틱을 얼마나 움직여야 게임이 반응할지입니다. 캐릭터나 카메라가 저절로 움직이면 올리세요. 스틱에서 손을 떼고 '현재 스틱' 값을 읽은 뒤 "
                                    "그보다 몇 퍼센트 높게 설정하세요.",
    [UI_MENU_STICK_DEADZONE_NOW] = "현재 스틱: %.0f %%",
    [UI_MENU_MOUSE_MODE] = "마우스 카메라",
    [UI_MENU_MOUSE_MODE_DESC] = "직접: PC 게임처럼 마우스를 움직인 만큼 카메라가 돌며, 스틱 데드존이나 곡선이 없습니다. 스틱 에뮬레이션: 마우스가 오른쪽 스틱처럼 작동합니다.",
    [UI_MENU_MOUSE_STICK] = "스틱 에뮬레이션",
    [UI_MENU_MOUSE_DIRECT] = "직접",
    [UI_MENU_MENU_CURSOR] = "게임 메뉴에서 마우스",
    [UI_MENU_MENU_CURSOR_DESC] = "게임 자체 메뉴(타이틀, 인벤토리, 상점, 대화 선택지)에 포인터가 나타납니다. 포인터 아래 항목이 선택되고, 왼쪽 클릭으로 결정, 오른쪽 클릭으로 "
                                 "뒤로 갑니다. 마지막으로 마우스나 키를 사용했을 때만 나타나므로 컨트롤러로는 메뉴가 그대로입니다(게임 1.4.3).",
    [UI_MENU_MAP_CLICK] = "지도에서 왼쪽 클릭",
    [UI_MENU_MAP_CLICK_DESC] = "푸르아 패드의 지도에서 마우스 왼쪽 버튼이 지정된 버튼(기본값 Y) 대신 지도 커서 위치에서 A를 누릅니다. '게임 메뉴에서 마우스'가 필요합니다(게임 "
                               "1.4.3).",
    [UI_MENU_DIALOGUE_CLICK] = "대화 중 클릭",
    [UI_MENU_DIALOGUE_CLICK_DESC] = "대화창이 열려 있는 동안 왼쪽 클릭은 B(텍스트를 진행하고 스크롤을 건너뜀)를, 오른쪽 클릭은 A를 누릅니다. 원래 지정된 버튼(기본값: 대화 "
                                    "기록을 여는 Y와 ZL) 대신입니다. 대화 선택지에서는 포인터가 그대로이며 왼쪽 클릭으로 고릅니다(게임 1.4.3).",
    [UI_MENU_POINTER_IMAGE] = "게임 포인터",
    [UI_MENU_POINTER_IMAGE_DESC] = "커서가 잡혀 있지 않은 동안(게임 메뉴, 이 메뉴, Esc 이후) 창에 시스템 화살표 대신 화면 배율에 맞춘 자체 금색 화살촉이 표시됩니다.",
    [UI_MENU_QUICKSAVE] = "빠른 저장 및 불러오기",
    [UI_MENU_QUICKSAVE_DESC] = "키 하나로 즉시 게임을 저장하고, 다른 키로 그 저장을 불러옵니다(아래 두 줄: 기본값은 Half-Life처럼 F6, F7). 화면에 메뉴가 뜨지 "
                               "않습니다. 키보드와 마우스 키 전용이며 게임 1.4.3 전용입니다. 빠른 저장은 게임에 자체 자동 저장 하나를 요청하므로 가장 오래된 자동 저장을 "
                               "대체하고(수동 저장은 남음), 그 사본을 빠른 저장으로 보관하며(프로필의 quicksave 폴더) 화면 위쪽에 \"게임을 저장했습니다\"를 표시합니다. "
                               "빠른 불러오기는 보이지 않는 곳에서 게임의 불러오기 과정을 거치는 동안 화면을 \"빠른 저장 불러오는 중\"에 멈춰 두고, 게임이 빠른 저장을 "
                               "불러옵니다(약 5초). 게임이 스스로 자동 저장하지 않을 곳(컷신, 대화)에서는 아무것도 저장되지 않으며 그렇다고 표시합니다.",
    [UI_MENU_QUICKSAVE_KEY] = "빠른 저장 키",
    [UI_MENU_QUICKLOAD_KEY] = "빠른 불러오기 키",
    [UI_MENU_QUICKSAVE_KEY_DESC] = "빠른 저장 또는 빠른 불러오기를 시작하는 키입니다. 기본값은 Half-Life처럼 F6, F7입니다. 칸을 선택하고 아무 키나 마우스 버튼을 "
                                   "누르세요. Delete로 그 동작의 키를 비웁니다. 빠른 저장이 켜져 있는 동안 두 키는 다른 동작을 하지 않습니다(다른 지정에서도 그 키를 쓰면 "
                                   "안내가 표시됩니다). totk.ini에는 키 지정에서 쓰는 어떤 키 이름이든 쓸 수 있습니다(quicksave_key, quickload_key).",
    [UI_QS_SAVED] = "게임을 저장했습니다",
    [UI_QS_LOADING] = "빠른 저장 불러오는 중",
    [UI_QS_NOT_NOW] = "지금은 빠른 저장을 할 수 없습니다",
    [UI_QS_NONE] = "아직 빠른 저장이 없습니다(빠른 저장 키로 만듭니다)",
    [UI_QS_FAILED] = "빠른 저장 실패: totk.log를 확인하세요",
    [UI_QS_LOAD_FAILED] = "빠른 불러오기 실패: totk.log를 확인하세요",
    [UI_MENU_MOUSE_SENS] = "마우스 카메라 감도",
    [UI_MENU_MOUSE_SENS_DESC] = "마우스로 카메라가 도는 속도입니다.",
    [UI_MENU_WALK_KEY] = "걷기 키",
    [UI_MENU_WALK_KEY_DESC] = "누르고 있는 동안 키보드 스틱 입력을 일부만 기울여 뛰지 않고 걷게 합니다. 얼마나 기울일지는 걷기 정도로 정합니다. 컨트롤러와 마우스에는 영향이 없습니다.",
    [UI_MENU_WALK_KEY_TAKEN] = "%s에도 지정됨",
    [UI_MENU_WALK_SCALE] = "걷기 정도",
    [UI_MENU_WALK_SCALE_DESC] = "걷기 키를 누르고 있는 동안 키보드 스틱의 기울기입니다. 절반이면 걷기, 끝까지면 달리기입니다.",
    [UI_MENU_FREECAM] = "자유 카메라",
    [UI_MENU_FREECAM_DESC] = "아래 조합이나 키로 시점을 링크에게서 떼어 내 날아다니게 합니다. 기본값은 끔이며, 끄면 게임의 카메라 코드가 원래대로 실행됩니다. 재시작 후 적용됩니다. "
                             "게임 버전 1.4.3.",
    [UI_MENU_FREECAM_OFF_NOTE] = "위의 '자유 카메라'를 켜고 재시작하세요",
    [UI_MENU_FREECAM_PAD] = "자유 카메라 조합",
    [UI_MENU_FREECAM_PAD_DESC] = "함께 누르면 시점을 세 단계로 바꾸는 컨트롤러 버튼입니다: 링크에게서 떨어져 날아다니고, 링크가 계속 플레이하는 동안 남겨진 자리에 머물고, 다시 "
                                 "돌아옵니다. 날아다니는 동안에는 게임에 입력이 가지 않습니다. 왼쪽 스틱은 시선 방향과 옆으로, 트리거는 위아래로 움직이고, 오른쪽 스틱은 회전하며, "
                                 "L이나 R을 누를 때마다 속도가 절반 또는 두 배가 됩니다. 고정된 동안에는 링크가 화면에서 왼쪽 스틱이 가리키는 쪽으로 걷습니다. 게임 버전 "
                                 "1.4.3.",
    [UI_MENU_FREECAM_PAD_V143] = "게임 버전 1.4.3 전용",
    [UI_MENU_FREECAM_KEY] = "자유 카메라 키",
    [UI_MENU_FREECAM_KEY_DESC] = "시점을 같은 단계로 바꾸는 키입니다: 링크에게서 떨어져 날아다니고, 링크가 계속 플레이하는 동안 머물고, 다시 돌아옵니다. 날아다니는 동안: W A "
                                 "S D는 시선 방향과 옆으로, ZR과 ZL에 지정된 키(기본값 X와 Z)는 위아래로 움직이고, 마우스와 화살표 키는 회전하며, L과 R에 지정된 "
                                 "키(Q와 R)는 속도를 절반 또는 두 배로 합니다. 게임 버전 1.4.3.",
    [UI_MENU_GROUP_GAMEPLAY_TWEAKS] = "게임플레이 조정",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED] = "게임플레이 조정",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED_DESC] = "울트라핸드 회전과 거리, 아이템과 무기 전환, 부드러운 지도 확대를 마우스 휠로 조작합니다.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE] = "울트라핸드 휠 회전",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE_DESC] = "울트라핸드로 잡은 물체를 마우스 휠로 바로 회전합니다.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS] = "울트라핸드 C 회전축",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS_DESC] = "C를 누른 채 휠을 굴리면 잡은 물체를 다른 축으로 회전합니다.",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY] = "울트라핸드 회전 감도",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY_DESC] = "마우스 휠이 잡은 물체를 회전하는 속도입니다.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE] = "울트라핸드 휠 거리",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE_DESC] = "오른쪽 클릭 + 마우스 휠로 잡은 물체를 밀거나 당깁니다.",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY] = "울트라핸드 거리 감도",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY_DESC] = "마우스 휠이 물체 거리를 바꾸는 속도입니다.",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL] = "휠로 무기 선택",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL_DESC] = "필드에서 마우스 휠을 굴려 무기를 바꿉니다. E로 확정합니다.",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY] = "무기 휠 감도",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY_DESC] = "다음 무기로 넘어가는 데 필요한 휠 단계 수입니다.",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL] = "휠로 빠른 메뉴 선택",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL_DESC] = "빠른 선택 메뉴가 열려 있는 동안 마우스 휠로 아이템을 넘겨 봅니다.",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY] = "빠른 메뉴 휠 감도",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY_DESC] = "빠른 선택 메뉴에서 다음 아이템으로 넘어가는 데 필요한 휠 단계 수입니다.",
    [UI_MENU_GAMEPLAY_ITEM_AD] = "빠른 메뉴 A / D 이동",
    [UI_MENU_GAMEPLAY_ITEM_AD_DESC] = "빠른 선택 메뉴에서 A / D를 눌러 왼쪽과 오른쪽으로 이동합니다.",
    [UI_MENU_GAMEPLAY_MAP_ZOOM] = "부드러운 지도 확대",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_DESC] = "마우스 휠로 푸르아 패드 지도를 관성 있게 부드럽게 확대합니다.",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY] = "지도 확대 감도",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY_DESC] = "마우스 휠이 지도를 확대·축소하는 속도입니다.",
    [UI_MENU_STICK_SENS] = "스틱 카메라 속도",
    [UI_MENU_STICK_SENS_DESC] = "게임의 5단계 카메라 감도 각각에서 오른쪽 스틱 카메라 속도(최고 속도 포함)를 몇 배로 할지입니다. 마우스는 자체 설정을 사용합니다.",
    [UI_BINDS_HELP] = "칸을 선택하고 A나 Enter를 누른 다음 키, 마우스 버튼 또는 컨트롤러 버튼을 누르세요. Delete나 X로 칸을 비웁니다. 이미 쓰인 키는 이곳으로 옮겨지고, "
                      "컨트롤러 버튼은 교체되는 버튼과 서로 바뀝니다.",
    [UI_BINDS_COL_KEY1] = "키 1",
    [UI_BINDS_COL_KEY2] = "키 2",
    [UI_BINDS_COL_PAD] = "컨트롤러",
    [UI_BINDS_GROUP_BUTTONS] = "버튼",
    [UI_BINDS_GROUP_MOVE] = "이동",
    [UI_BINDS_GROUP_CAMERA] = "카메라",
    [UI_BINDS_PRESS_KEY] = "%s에 지정할 키나 마우스 버튼을 누르세요",
    [UI_BINDS_PRESS_PAD] = "%s에 지정할 컨트롤러 버튼을 누르세요",
    [UI_BINDS_CANCEL_HINT] = "Esc로 취소하거나 %d초 기다리세요",
    [UI_BINDS_TAKEN] = "%s을(를) %s에서 해제했습니다",
    [UI_BINDS_SWAPPED] = "%s을(를) %s와(과) 바꿨습니다",
    [UI_BINDS_RESET] = "모든 버튼 초기화",
    [UI_BINDS_RESET_DESC] = "기본 키와 컨트롤러 버튼으로 되돌립니다. 위의 버튼 배치가 다시 적용됩니다. 다시 지정한 컨트롤러 버튼은 이 초기화 전까지 지정한 위치를 유지합니다.",
    [UI_BIND_A] = "A",
    [UI_BIND_B] = "B",
    [UI_BIND_X] = "X",
    [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "왼쪽 스틱 누르기",
    [UI_BIND_RCLICK] = "오른쪽 스틱 누르기",
    [UI_BIND_L] = "L",
    [UI_BIND_R] = "R",
    [UI_BIND_ZL] = "ZL",
    [UI_BIND_ZR] = "ZR",
    [UI_BIND_PLUS] = "+ 버튼",
    [UI_BIND_MINUS] = "- 버튼",
    [UI_BIND_LEFT] = "방향 패드 왼쪽",
    [UI_BIND_UP] = "방향 패드 위",
    [UI_BIND_RIGHT] = "방향 패드 오른쪽",
    [UI_BIND_DOWN] = "방향 패드 아래",
    [UI_BIND_LSTICK_LEFT] = "왼쪽으로 이동",
    [UI_BIND_LSTICK_UP] = "앞으로 이동",
    [UI_BIND_LSTICK_RIGHT] = "오른쪽으로 이동",
    [UI_BIND_LSTICK_DOWN] = "뒤로 이동",
    [UI_BIND_RSTICK_LEFT] = "카메라 왼쪽",
    [UI_BIND_RSTICK_UP] = "카메라 위",
    [UI_BIND_RSTICK_RIGHT] = "카메라 오른쪽",
    [UI_BIND_RSTICK_DOWN] = "카메라 아래",
    [UI_BIND_DESC_A] = "조사, 줍기, 말하기.",
    [UI_BIND_DESC_B] = "달리기, 취소, 뒤로.",
    [UI_BIND_DESC_X] = "점프.",
    [UI_BIND_DESC_Y] = "공격.",
    [UI_BIND_DESC_LCLICK] = "웅크리기.",
    [UI_BIND_DESC_RCLICK] = "1인칭 시점.",
    [UI_BIND_DESC_L] = "능력.",
    [UI_BIND_DESC_R] = "무기 던지기.",
    [UI_BIND_DESC_ZL] = "방패, 주목.",
    [UI_BIND_DESC_ZR] = "활.",
    [UI_BIND_DESC_PLUS] = "메뉴, 파우치.",
    [UI_BIND_DESC_MINUS] = "지도.",
    [UI_BIND_DESC_DPAD] = "빠른 선택.",
    [UI_BIND_DESC_MOVE] = "왼쪽 스틱처럼 링크를 움직입니다. 컨트롤러의 왼쪽 스틱도 언제나 작동합니다.",
    [UI_BIND_DESC_CAMERA] = "오른쪽 스틱처럼 카메라를 돌립니다. 마우스와 컨트롤러의 오른쪽 스틱도 언제나 작동합니다.",
    [UI_BIND_SHAKE] = "카메라 스틱 흔들기",
    [UI_BIND_DESC_SHAKE] = "게임에 오른쪽 스틱을 좌우로 흔드는 입력을 보냅니다. 울트라핸드로 잡은 것에서 붙인 부품을 떼어 냅니다. 키를 누르고 있으면 계속 흔듭니다.",
    [UI_KEY_SPACE] = "스페이스",
    [UI_KEY_TAB] = "Tab",
    [UI_KEY_LSHIFT] = "왼쪽 Shift",
    [UI_KEY_LCTRL] = "왼쪽 Ctrl",
    [UI_KEY_UP] = "↑ 키",
    [UI_KEY_DOWN] = "↓ 키",
    [UI_KEY_LEFT] = "← 키",
    [UI_KEY_RIGHT] = "→ 키",
    [UI_KEY_MOUSE_L] = "마우스 왼쪽",
    [UI_KEY_MOUSE_R] = "마우스 오른쪽",
    [UI_KEY_MOUSE_M] = "마우스 가운데",
    [UI_KEY_MOUSE_X1] = "마우스 4",
    [UI_KEY_MOUSE_X2] = "마우스 5",
    [UI_KEY_ENTER] = "Enter",
    [UI_KEY_BACKSPACE] = "Backspace",
    [UI_KEY_RSHIFT] = "오른쪽 Shift",
    [UI_KEY_RCTRL] = "오른쪽 Ctrl",
    [UI_KEY_LALT] = "Alt",
    [UI_KEY_CAPSLOCK] = "Caps Lock",
    [UI_KEY_INSERT] = "Insert",
    [UI_KEY_DELETE] = "Delete",
    [UI_KEY_HOME] = "Home",
    [UI_KEY_END] = "End",
    [UI_KEY_PAGEUP] = "Page Up",
    [UI_KEY_PAGEDOWN] = "Page Down",
    [UI_KEY_NUM_FMT] = "숫자 패드 %s",
    [UI_PAD_SOUTH] = "아래 버튼",
    [UI_PAD_EAST] = "오른쪽 버튼",
    [UI_PAD_WEST] = "왼쪽 버튼",
    [UI_PAD_NORTH] = "위 버튼",
    [UI_PAD_LSTICK] = "왼쪽 스틱",
    [UI_PAD_RSTICK] = "오른쪽 스틱",
    [UI_PAD_L] = "왼쪽 범퍼",
    [UI_PAD_R] = "오른쪽 범퍼",
    [UI_PAD_START] = "Start",
    [UI_PAD_BACK] = "Back",
    [UI_PAD_DPAD_LEFT] = "방향 패드 왼쪽",
    [UI_PAD_DPAD_UP] = "방향 패드 위",
    [UI_PAD_DPAD_RIGHT] = "방향 패드 오른쪽",
    [UI_PAD_DPAD_DOWN] = "방향 패드 아래",
    [UI_PAD_ZL] = "왼쪽 트리거",
    [UI_PAD_ZR] = "오른쪽 트리거",
    [UI_PAD_LPADDLE1] = "왼쪽 후면 버튼",
    [UI_PAD_RPADDLE1] = "오른쪽 후면 버튼",
    [UI_PAD_LPADDLE2] = "왼쪽 후면 버튼 2",
    [UI_PAD_RPADDLE2] = "오른쪽 후면 버튼 2",
    [UI_PAD_TOUCHPAD] = "터치패드 클릭",
    [UI_PAD_CROSS] = "× 버튼",
    [UI_PAD_CIRCLE] = "○ 버튼",
    [UI_PAD_SQUARE] = "□ 버튼",
    [UI_PAD_TRIANGLE] = "△ 버튼",
    [UI_MENU_AMIIBO_DIR] = "amiibo 폴더",
    [UI_MENU_AMIIBO_OPEN] = "열기",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "기본 폴더",
    [UI_MENU_AMIIBO_REFRESH] = "새로 고침",
    [UI_MENU_AMIIBO_PICK] = "amiibo 덤프 (540바이트 .bin 또는 .nfc 파일):",
    [UI_MENU_AMIIBO_NO_DIR] = "폴더 %s을(를) 열 수 없습니다",
    [UI_MENU_AMIIBO_READER] = "NFC 리더",
    [UI_MENU_AMIIBO_STATE] = "리더:",
    [UI_MENU_AMIIBO_STATE_IDLE] = "준비됨",
    [UI_MENU_AMIIBO_STATE_SEARCHING] = "게임이 amiibo를 기다리는 중",
    [UI_MENU_AMIIBO_STATE_FOUND] = "amiibo가 리더 위에 있음",
    [UI_MENU_AMIIBO_STATE_MOUNTED] = "게임이 amiibo를 읽는 중",
    [UI_MENU_AMIIBO_STATE_OFF] = "꺼짐",
    [UI_MENU_AMIIBO_FILE] = "선택한 파일:",
    [UI_MENU_AMIIBO_NO_FILE] = "(선택한 파일 없음)",
    [UI_MENU_AMIIBO_TAP] = "amiibo 터치",
    [UI_MENU_AMIIBO_REMOVE] = "amiibo 떼기",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "터치 후 amiibo 자동으로 떼기",
    [UI_MENU_AMIIBO_HOLD] = "리더 위 시간",
    [UI_MENU_AMIIBO_SECONDS] = "%.1f초",
    [UI_MENU_AMIIBO_TAG] = "리더 위의 amiibo:",
    [UI_MENU_AMIIBO_NO_TAG] = "리더에 amiibo가 없습니다.",
    [UI_MENU_AMIIBO_TAG_FILE] = "파일: %s",
    [UI_MENU_AMIIBO_TAG_ID] = "캐릭터 %s, 시리즈 %02x, 번호 %04x, 유형 %02x",
    [UI_MENU_AMIIBO_TAG_UID] = "UID: %s",
    [UI_MENU_AMIIBO_READY] = "준비됨",
    [UI_MENU_AMIIBO_TAPPED] = "터치함: %s",
    [UI_MENU_AMIIBO_LOAD_FAILED] = "amiibo 덤프(540바이트 NTAG215)가 아닙니다: %s",
    [UI_MENU_AMIIBO_REMOVED] = "amiibo를 뗐습니다",
    [UI_MENU_AMIIBO_STATUS] = "상태: %s",
    [UI_MENU_MENU_LANG] = "메뉴 언어",
    [UI_MENU_MENU_LANG_DESC] = "이 메뉴와 성능 표시의 언어입니다.",
    [UI_MENU_MENU_LANG_AUTO] = "자동 (Windows에 맞춤)",
    [UI_MENU_DISCORD_RPC] = "Discord Rich Presence",
    [UI_MENU_DISCORD_RPC_DESC] = "게임이 실행되는 동안 Discord 상태에 플레이 시간과 함께 \"TotK Recomp 플레이 중\"을 표시합니다. totk.ini의 "
                                 "discord_app_id와 discord_image로 애플리케이션을 고릅니다.",
    [UI_MENU_ADV_WARNING] = "테스트와 문제 해결용 설정입니다. 이유를 알 때만 바꾸세요.",
    [UI_MENU_GROUP_SCHED] = "스케줄링",
    [UI_MENU_GROUP_SHADERS] = "셰이더와 파이프라인",
    [UI_MENU_GROUP_TEXMEM] = "텍스처와 메모리",
    [UI_MENU_GROUP_FIXES] = "렌더링 수정",
    [UI_MENU_GROUP_VULKAN] = "Vulkan",
    [UI_MENU_GROUP_DIAG] = "진단",
    [UI_MENU_ASYNC_PREEMPT] = "가상 코어 선점",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "같은 가상 코어에서 게임 스레드가 다른 스레드를 중단시킬 수 있게 합니다.",
    [UI_MENU_PREEMPT_OFF] = "끄기",
    [UI_MENU_PREEMPT_ON] = "켜기",
    [UI_MENU_PREEMPT_INHERIT] = "상속만",
    [UI_MENU_REPLAY_ASYNC] = "별도 스레드에서 GPU 프레임 준비",
    [UI_MENU_REPLAY_ASYNC_DESC] = "GPU용 프레임을 게임 스레드가 아닌 별도 스레드에서 준비합니다. 지연이 최대 1프레임 늘어납니다.",
    [UI_MENU_REPLAY_WORKERS] = "GPU 프레임 준비 스레드",
    [UI_MENU_REPLAY_WORKERS_DESC] = "큰 프레임을 여러 스레드에서 동시에 GPU용으로 기록합니다. CPU가 프레임 속도를 제한할 때 도움이 되며, 논리 프로세서가 8개 이상 "
                                    "필요합니다. 끄면 스레드 하나에서 기록합니다.",
    [UI_MENU_REPLAY_WORKERS_NOW] = "현재: %s",
    [UI_MENU_TIMER_1MS] = "1 ms Windows 타이머",
    [UI_MENU_TIMER_1MS_DESC] = "시스템 타이머 해상도를 1 ms로 올려 프레임 간격을 고르게 합니다.",
    [UI_MENU_SYNC_AHEAD] = "GPU보다 한 프레임 앞서기",
    [UI_MENU_SYNC_AHEAD_DESC] = "그래픽 카드가 이전 프레임을 그리는 동안 게임이 다음 프레임을 만듭니다. 프레임이 늘고 입력 지연이 한 프레임 늘어납니다.",
    [UI_MENU_HOST_CHECKPOINT] = "빠른 코어 넘기기",
    [UI_MENU_HOST_CHECKPOINT_DESC] = "게임 스레드가 포트 코드를 벗어나는 즉시 더 중요한 스레드에 가상 코어를 넘깁니다. 다음 시스템 호출까지 기다리지 않습니다.",
    [UI_MENU_PIPE_WAIT] = "대기 중인 그리기 처리",
    [UI_MENU_PIPE_WAIT_DESC] = "'기본값'은 대기 중인 그리기용 워커를 둔 현재 스케줄링을, '이전 방식(draw)'은 예전 스케줄링을 사용합니다.",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "기본값",
    [UI_MENU_PIPE_WAIT_DRAW] = "이전 방식 (draw)",
    [UI_MENU_PIPE_SKIP] = "파이프라인 빌드 중 건너뛸 그리기",
    [UI_MENU_PIPE_SKIP_DESC] = "파이프라인을 기다리지 않고 버리는 그리기의 종류입니다.",
    [UI_MENU_PIPE_SKIP_STRICT] = "엄격",
    [UI_MENU_PIPE_SKIP_ANY] = "전부",
    [UI_MENU_PIPE_SKIP_FULLSCREEN] = "전체 화면 패스",
    [UI_MENU_LIB_PREBUILD] = "스테이지 라이브러리 미리 빌드",
    [UI_MENU_LIB_PREBUILD_DESC] = "셰이더 스테이지 라이브러리를 백그라운드에서 미리 빌드합니다.",
    [UI_MENU_LIB_PREBUILD_THREADS] = "스테이지 라이브러리 미리 빌드 스레드",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "미리 빌드에 쓰는 스레드 수입니다. 자동은 CPU에 맞춰 정합니다.",
    [UI_MENU_PIPE_PREBUILD] = "파이프라인 미리 빌드",
    [UI_MENU_PIPE_PREBUILD_DESC] = "알려진 파이프라인을 미리 빌드합니다.",
    [UI_MENU_PREWARM_XLATE] = "변환기 예열",
    [UI_MENU_PREWARM_XLATE_DESC] = "시작 시 알려진 셰이더를 변환하는 스레드 수입니다. 자동은 CPU 스레드의 절반(최대 16)을 씁니다.",
    [UI_MENU_GPL] = "그래픽 파이프라인 라이브러리",
    [UI_MENU_GPL_DESC] = "Vulkan 파이프라인 라이브러리 확장을 사용합니다. 드라이버와 맞지 않으면 끄세요.",
    [UI_MENU_ASTC] = "ASTC 텍스처 디코딩",
    [UI_MENU_ASTC_DESC] = "압축된 ASTC 텍스처를 어디서 디코딩할지입니다.",
    [UI_MENU_ASTC_AUTO] = "자동",
    [UI_MENU_ASTC_GPU] = "GPU 셰이더",
    [UI_MENU_ASTC_NATIVE] = "네이티브",
    [UI_MENU_ASTC_CPU] = "CPU",
    [UI_MENU_MEMORY] = "GPU 메모리 모드",
    [UI_MENU_MEMORY_DESC] = "게임 메모리를 GPU와 어떻게 공유할지입니다.",
    [UI_MENU_MEMORY_AUTO] = "자동",
    [UI_MENU_MEMORY_IMPORT] = "가져오기",
    [UI_MENU_MEMORY_MIRROR] = "미러",
    [UI_MENU_MEMORY_VERIFY] = "검증",
    [UI_MENU_TEX_ALIAS] = "텍스처 별칭",
    [UI_MENU_TEX_ALIAS_DESC] = "사용 중인 텍스처의 메모리 위에 만든 새 텍스처를 빈 상태가 아니라 그 내용에서 시작합니다.",
    [UI_MENU_TEX_VRAM_PRESSURE] = "텍스처 메모리 한도",
    [UI_MENU_TEX_VRAM_PRESSURE_DESC] = "비디오 메모리 사용률이 이 퍼센트를 넘으면 게임이 해제한 텍스처를 버립니다(다시 쓰면 저장소에서 되돌립니다). 0이면 유지합니다.",
    [UI_MENU_ASTC_BC] = "ASTC를 BC3로 압축",
    [UI_MENU_ASTC_BC_DESC] = "GPU 디코더 사용 시 디코딩한 ASTC 텍스처를 BC3로 다시 인코딩해 비디오 메모리를 4분의 1로 줄입니다. 4 GB 이하 GPU용입니다. 텍스처가 약간 "
                             "각져 보입니다.",
    [UI_MENU_LOD_GUARD] = "LOD 보호",
    [UI_MENU_LOD_GUARD_DESC] = "GPU가 세부 수준 데이터를 읽기 전에 게임이 그 수정을 끝내기를 기다려, 물체가 한 프레임 동안 엉뚱한 곳에 보이거나 먼 지형이 평평해 보이는 것을 "
                               "막습니다.",
    [UI_MENU_FIX_C0] = "상수 버퍼 수정",
    [UI_MENU_FIX_C0_DESC] = "드라이버 예약 상수 뱅크 c0(저장소 크기, 디스패치 크기, 포인트 크기, 기본 인스턴스)을 0을 읽는 대신 채웁니다.",
    [UI_MENU_FIX_ZERO] = "새 GPU 메모리를 0으로 채우기",
    [UI_MENU_FIX_ZERO_DESC] = "Switch 메모리처럼 새 GPU 메모리 풀을 0으로 채운 상태에서 시작합니다.",
    [UI_MENU_DLAA_EFFECTS_AFTER] = "DLAA 후 파티클",
    [UI_MENU_DLAA_EFFECTS_AFTER_DESC] = "DLAA 사용 시 먼지 등 파티클을 DLSS가 이미 부드럽게 한 영상 위에 그립니다. DLSS 없이 게임이 그리는 방식과 같습니다. 안개 "
                                        "속의 작은 파티클(사당)이 더 이상 사라지거나 잔상을 남기지 않습니다. 안개와 구름은 계속 DLSS를 거치므로 하늘을 배경으로 한 먼 "
                                        "윤곽이 깨끗하게 유지됩니다. 끄기: 예전처럼 DLSS가 파티클도 처리합니다. DLAA 모드 전용이며, 업스케일링 DLSS 모드는 항상 이 "
                                        "효과를 처리합니다. 즉시 적용됩니다.",
    [UI_MENU_DLSS_FIXED_EXPOSURE] = "DLSS 노출 고정",
    [UI_MENU_DLSS_FIXED_EXPOSURE_DESC] = "DLSS가 프레임마다 노출을 추정하지 않고 게임 영상의 밝기를 그대로 사용합니다. 어두운 장면(지저)에서 안개 낀 넓은 영역이 카메라가 "
                                         "움직일 때 한 프레임 동안 어두워지거나 밝아지지 않습니다. 끄기: 예전처럼 DLSS가 노출을 직접 추정합니다. 즉시 적용됩니다.",
    [UI_MENU_BIND_SKIP] = "같은 바인딩 건너뛰기",
    [UI_MENU_BIND_SKIP_DESC] = "최적화: 바뀌지 않은 바인딩을 건너뜁니다.",
    [UI_MENU_SHADER_MEMO] = "셰이더 메모이제이션",
    [UI_MENU_SHADER_MEMO_DESC] = "최적화: 셰이더 조회 결과를 기억합니다.",
    [UI_MENU_PIPE_MEMO] = "파이프라인 메모이제이션",
    [UI_MENU_PIPE_MEMO_DESC] = "최적화: 상태가 같으면 직전 그리기의 파이프라인을 재사용합니다.",
    [UI_MENU_TEX_MEMO] = "텍스처 바인딩 메모이제이션",
    [UI_MENU_TEX_MEMO_DESC] = "최적화: 그리기 사이에 텍스처 디스크립터를 기억합니다.",
    [UI_MENU_DESC_TEMPLATE] = "디스크립터 템플릿",
    [UI_MENU_DESC_TEMPLATE_DESC] = "최적화: 디스크립터를 한 번에 갱신합니다.",
    [UI_MENU_PUSH_SKIP] = "같은 푸시 상수 건너뛰기",
    [UI_MENU_PUSH_SKIP_DESC] = "최적화: 바뀌지 않은 푸시 상수를 건너뜁니다.",
    [UI_MENU_SPLIT_DRAWS] = "GPU에 일찍 제출",
    [UI_MENU_SPLIT_DRAWS_DESC] = "패스 사이에서 그리기 N번마다 GPU 작업을 나눠 보내 GPU가 더 일찍 시작하게 합니다. 0이면 끕니다.",
    [UI_MENU_VK_VALIDATION] = "검증 레이어",
    [UI_MENU_VK_VALIDATION_DESC] = "모든 Vulkan 호출을 검사하며 게임이 매우 느려집니다.",
    [UI_MENU_VK_VALIDATION_MISSING] = "검증 레이어가 설치되어 있지 않습니다.",
    [UI_MENU_VK_DYN_VERTEX] = "동적 정점 상태",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "동적 정점 입력을 강제로 켜거나 끕니다. 기본값은 GPU에 따라 정해집니다.",
    [UI_MENU_VK_DYN_A2C] = "동적 Alpha to Coverage",
    [UI_MENU_VK_DYN_A2C_DESC] = "동적 Alpha to Coverage를 강제로 켜거나 끕니다. 기본값: RDNA 2 이전용 AMD 드라이버에서는 끔(켜면 캐릭터가 검게 그려짐), 그 "
                                "외에는 켬.",
    [UI_MENU_VK_SUBGROUP_PIN] = "서브그룹 크기 고정",
    [UI_MENU_VK_SUBGROUP_PIN_DESC] = "셰이더 서브그룹 크기를 고정합니다.",
    [UI_MENU_TRACE] = "성능 로그",
    [UI_MENU_TRACE_DESC] = "프레임 시간을 파일에 기록합니다.",
    [UI_MENU_FLIGHT] = "플라이트 레코더",
    [UI_MENU_FLIGHT_DESC] = "짧은 기록을 유지하다가 프레임이 멈추면 저장합니다.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "멈춤 기준 (ms)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "멈춤으로 볼 프레임 시간입니다.",
    [UI_MENU_PASS_CRUMBS] = "GPU 패스 표시",
    [UI_MENU_PASS_CRUMBS_DESC] = "GPU 충돌 후 로그에 실행 중이던 렌더 패스 이름을 남깁니다. 자동: AMD만. 켜기: NVIDIA도 포함하며 부하가 약간 있습니다.",
    [UI_MENU_INPUT_RECORD] = "입력 기록",
    [UI_MENU_INPUT_RECORD_DESC] = "버그 보고용으로 입력을 파일에 기록합니다.",
    [UI_MENU_OPEN_LOG] = "로그 폴더 열기",
    [UI_MENU_OPEN_LOG_DESC] = "로그 파일이 있는 폴더를 보여 줍니다.",
    [UI_MENU_COPY_INFO] = "시스템 정보 복사",
    [UI_MENU_COPY_INFO_DESC] = "GPU, 드라이버, CPU, RAM, 변경한 설정을 클립보드에 복사합니다.",
    [UI_MENU_COPY_INFO_DONE] = "복사됨",
    [UI_MENU_ACTION_FAILED] = "실패함",
    [UI_MENU_RESET] = "모든 설정 초기화",
    [UI_MENU_RESET_DESC] = "모든 탭에 적용됩니다.",
    [UI_MENU_RESET_CONFIRM] = "모든 설정을 기본값으로 되돌릴까요?",
    [UI_MENU_RESET_YES] = "초기화",
    [UI_MENU_RESET_NO] = "취소",
    [UI_MENU_OV_FPS] = "%.0f FPS",
    [UI_MENU_OV_FRAME_MS] = "%.1f ms",
    [UI_MENU_OV_GAME_PCT] = "게임 %.0f%%",
    [UI_MENU_OV_MAX_MS] = "최대 %.1f ms",
    [UI_MENU_OV_FPS_LABEL] = "FPS",
    [UI_MENU_OV_FRAME_LABEL] = "프레임",
    [UI_MENU_OV_FMT_FPS] = "%.0f",
    [UI_MENU_OV_LOW1] = "하위 1%% %.0f",
    [UI_MENU_OV_LOW01] = "하위 0.1%% %.0f",
    [UI_MENU_OV_GPU] = "GPU",
    [UI_MENU_OV_CPU] = "CPU",
    [UI_MENU_OV_RAM] = "RAM",
    [UI_MENU_OV_VRAM] = "VRAM",
    [UI_MENU_OV_TEMP] = "온도",
    [UI_MENU_OV_POWER] = "전력",
    [UI_MENU_OV_CLOCK] = "클럭",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "GPU 프레임 준비",
    [UI_MENU_OV_GPU_TIME] = "프레임당 GPU 사용 시간",
    [UI_MENU_OV_PIPE_WAIT] = "파이프라인 대기",
    [UI_MENU_OV_SHADER_WAIT] = "셰이더 대기",
    [UI_MENU_OV_TEX_UPLOADS] = "텍스처 업로드",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%",
    [UI_MENU_OV_FMT_TEMP] = "%.0f °C",
    [UI_MENU_OV_FMT_POWER] = "%.0f W",
    [UI_MENU_OV_FMT_MHZ] = "%.0f MHz",
    [UI_MENU_OV_FMT_GB] = "%.1f GB",
    [UI_MENU_OV_FMT_GB_OF] = "/ %.1f GB",
    [UI_MENU_OV_GAME_GB] = "게임 %.1f GB",
    [UI_MENU_OV_FMT_MS] = "%.2f ms",
    [UI_MENU_OV_PER_FRAME] = "프레임당 %.1f",
    [UI_MENU_OV_WINDOW] = "창",
    [UI_MENU_OV_OUTPUT] = "게임 이미지",
    [UI_MENU_OV_SCENE] = "3D 장면",
    [UI_MENU_OV_UPSCALER] = "3D 업스케일러",
    [UI_MENU_OV_SCALING] = "확대 방식",
    [UI_MENU_OV_LIMIT] = "프레임 제한",
    [UI_MENU_OV_PRESENT] = "표시 방식",
    [UI_MENU_OV_SHOWN] = "표시",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u",
    [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, 선명도 %d%%",
    [UI_MENU_OV_SCALE_NONE] = "없음 (1:1)",
    [UI_MENU_OV_SCALE_BILINEAR] = "바이리니어",
    [UI_MENU_OV_SCALE_DOWN] = "바이리니어 (FSR은 확대 전용)",
    [UI_MENU_TAB_CHEATS] = "치트",
    [UI_MENU_CHEATS_NONE] = "이 게임 버전용 치트가 없습니다. 모드의 치트는 그 cheats 폴더에 있는, 게임 빌드 이름을 붙인 Atmosphère 치트 파일입니다: "
                            "mods\\<mod>\\cheats\\%s. 불러오려면 게임을 재시작하세요. 게임 코드를 바꾸는 치트라면 게임이 시작 시 스스로를 다시 빌드합니다(몇 분).",
    [UI_MENU_CHEATS_MOD] = "%s: 치트 %d개",
    [UI_MENU_CHEAT_CODE_DESC] = "게임 코드를 바꿉니다. 즉시 전환됩니다.",
    [UI_MENU_CHEAT_KEY_DESC] = "%s을(를) 누르고 있는 동안 게임 코드를 바꿉니다. 즉시 전환됩니다.",
    [UI_MENU_CHEAT_DATA_DESC] = "켜져 있는 동안 매 프레임 게임 메모리에 씁니다.",
    [UI_MENU_CHEAT_MASTER] = "항상 켜짐",
    [UI_MENU_CHEAT_MASTER_DESC] = "파일의 마스터 코드입니다. 항상 켜져 있습니다.",
    [UI_MENU_CHEAT_RESTORE] = "필요 없음",
    [UI_MENU_CHEAT_RESTORE_DESC] = "게임 원래 코드를 되돌려 쓸 뿐입니다. 치트를 끄면 같은 일이 일어납니다.",
    [UI_MENU_CHEAT_REBUILD] = "재빌드 후",
    [UI_MENU_CHEAT_REBUILD_DESC] = "이 치트는 게임 코드를 바꾸며 게임이 빌드된 뒤에 추가되거나 수정되었습니다. 다음 실행 시 빌드해 넣습니다(몇 분). 또는 '지금 게임 재빌드'를 "
                                   "사용하세요.",
    [UI_MENU_CHEAT_UNSUPPORTED] = "지원 안 함",
    [UI_MENU_CHEAT_UNSUPPORTED_DESC] = "이 치트는 이 버전의 게임에서 작동하지 않습니다:",
    [UI_MENU_CHEATS_REBUILD] = "지금 게임 재빌드",
    [UI_MENU_CHEATS_REBUILD_DESC] = "게임 코드를 바꾸는 일부 치트가 아직 빌드되지 않았습니다. 새로 추가·수정되었거나 이전 재빌드가 실패 또는 취소되었습니다. 게임을 닫고 설치 "
                                    "프로그램이 이를 빌드해 넣은 뒤(몇 분) 게임을 다시 시작합니다. 마지막 저장 이후의 진행은 사라집니다.",
    [UI_MENU_MODS_EXEFS_UNSUPPORTED] = "이 모드는 포트가 적용할 수 없는 방식으로 게임 코드(exefs)를 수정하므로 모드 전체가 꺼진 상태로 유지됩니다. %s",
    [UI_MENU_MODS_EXEFS_OTHER] = "코드 패치(exefs)가 다른 게임 버전용입니다.",
    [UI_MENU_MODS_EXEFS_ON] = "사용 중인 코드 패치: 파일 %d개.",
    [UI_MENU_MODS_REPLACES_BUILTIN] = "사용 중인 코드 패치: 파일 %d개. 포트 자체 패치 %s을(를) 대체하며, 이 모드가 켜져 있는 동안 그 패치는 꺼집니다.",
    [UI_MENU_MODS_CODE_SKIPPED] = "자체 게임 코드(%s)는 불러오지 않으며, 모드의 나머지는 사용됩니다. 게임이 이상하게 동작하면 모드를 끄세요.",
    [UI_MENU_MODS_PLUGIN_ON] = "플러그인 작동 중: 게임 코드 후크 %d개.",
    [UI_MENU_MODS_PLUGIN_REFUSED] = "플러그인 %s은(는) 포트에서 실행할 수 없습니다: %s.",
    [UI_MENU_MODS_PLUGIN_STAYS_OFF] = "모드 전체가 꺼진 상태로 유지됩니다. 아래의 \"...을(를) 코드 없이 사용\"을 켜면 모드의 나머지를 사용합니다.",
    [UI_MENU_MODS_PLUGIN_OFF] = "이번에는 플러그인이 꺼진 상태입니다: 빌드 때와 다르게 시작되었습니다(%s). 다음 실행 시 게임이 다시 빌드합니다.",
    [UI_MENU_MODS_WITHOUT_CODE] = "%s을(를) 코드 없이 사용",
    [UI_MENU_MODS_WITHOUT_CODE_DESC] = "위 모드는 포트가 실행할 수 없는 코드를 포함합니다: 자체 게임 코드(subsdk 플러그인이나 교체된 main) 또는 포트가 적용할 수 "
                                       "없는 코드 패치(exefs). 끄기: 모드 전체가 꺼진 상태로 유지됩니다. 켜기: 그 코드는 제외하고 모드의 나머지(RomFS 파일, "
                                       "치트, 작동하는 코드 패치)를 사용합니다. 모드의 파일이 빠진 코드를 전제로 할 수 있으니 게임이 이상하게 동작하면 다시 끄세요. 다음 "
                                       "실행 시 적용됩니다.",
    [UI_MENU_MODS_WITHOUT_CODE_ALL] = "모든 모드에 켜짐: totk.ini의 mods_without_code = 1.",
    [UI_MENU_MODS_EXEFS_SKIPPED] = "코드 패치 파일(exefs) %u개는 적용할 수 없어 제외되며, 모드의 나머지는 사용됩니다. %s",
    [UI_MENU_MODS_OVERRIDES_FILES] = "이 모드의 파일 %u개가 목록에서 더 아래에 있는 %s의 파일을 대체합니다.",
    [UI_MENU_MODS_DATA_OVERRIDDEN] = "게임 데이터 변경 중 %u바이트는 목록에서 더 위에 있는 %s의 것을 사용합니다.",
    [UI_MENU_MODS_OVERRIDES_DATA] = "게임 데이터 변경 %u바이트가 목록에서 더 아래에 있는 %s의 변경을 대체합니다.",
    [UI_MENU_MODS_CODE_OVERRIDDEN] = "코드 패치 위치 %u곳은 목록에서 더 위에 있는 %s의 패치가 우선합니다.",
    [UI_MENU_MODS_OVERRIDES_CODE] = "코드 패치 위치 %u곳에서 목록에서 더 아래에 있는 %s의 패치보다 우선합니다.",
};

// Vietnamese: the software keyboard and the whole menu, so the menu and the overlay read Vietnamese when the menu
// language is vi (ui_text.h menu_text). An id without a Vietnamese entry falls back to English.
static const char *const text_vi[UI_TEXT_COUNT] = {
    SWKBD("Nhập văn bản", "Nhập một cái tên", "Ngựa", "OK", "Hủy", "Tay cầm: A = OK, B = Hủy"),

    [UI_MENU_TITLE] = "Cài đặt",
    [UI_MENU_TAB_SCREEN] = "Màn hình", [UI_MENU_TAB_GAME] = "Trò chơi", [UI_MENU_TAB_MODS] = "Mod", [UI_MENU_TAB_SOUND] = "Âm thanh",
    [UI_MENU_TAB_CONTROLS] = "Điều khiển", [UI_MENU_TAB_AMIIBO] = "Amiibo", [UI_MENU_TAB_MENU] = "Menu", [UI_MENU_TAB_ADVANCED] = "Nâng cao",
    [UI_MENU_CONTINUE] = "Tiếp tục", [UI_MENU_QUIT] = "Thoát game",
    [UI_MENU_ON] = "Bật", [UI_MENU_OFF] = "Tắt", [UI_MENU_AUTO] = "Tự động", [UI_MENU_DEFAULT_MARK] = "mặc định",
    [UI_MENU_AFTER_RESTART] = "sau khi khởi động lại", [UI_MENU_SET_BY_ENV] = "đặt bởi biến môi trường",
    [UI_MENU_SAVE_FAILED] = "không lưu được cài đặt",
    [UI_MENU_VERSION] = "TotK Recomp %s, game %s",

    [UI_MENU_FULLSCREEN] = "Toàn màn hình", [UI_MENU_FULLSCREEN_DESC] = "Game chiếm toàn màn hình; F11 để bật hoặc tắt.",
    [UI_MENU_WINDOW_SIZE] = "Kích thước cửa sổ", [UI_MENU_WINDOW_SIZE_DESC] = "Kích thước cửa sổ game khi không ở toàn màn hình.",
    [UI_MENU_WINDOW_SIZE_CUSTOM] = "Tùy chỉnh",
    [UI_MENU_PRESENT] = "Chế độ hiển thị", [UI_MENU_PRESENT_DESC] = "Cách các khung hình hoàn chỉnh được đưa lên màn hình.",
    [UI_MENU_PRESENT_MAILBOX] = "Mailbox", [UI_MENU_PRESENT_VSYNC] = "VSync", [UI_MENU_PRESENT_IMMEDIATE] = "Tức thì",
    [UI_MENU_PRESENT_IN_USE] = "đang dùng: %s",
    [UI_MENU_GPU] = "Card đồ họa", [UI_MENU_GPU_DESC] = "Thiết bị Vulkan dùng để vẽ game.",
    [UI_MENU_GPU_AUTO] = "Tự động",
    [UI_MENU_PERF_OVERLAY] = "Giám sát hiệu năng",
    [UI_MENU_PERF_OVERLAY_DESC] = "Hiện tốc độ khung hình ở góc trên bên phải. Chế độ Đầy đủ thêm thời gian mỗi khung hình kèm "
                                  "biểu đồ, tải CPU, GPU và bộ nhớ, nhiệt độ và thời gian chờ của luồng kết xuất. F10 đổi "
                                  "chế độ.",
    [UI_MENU_PERF_OFF] = "Tắt", [UI_MENU_PERF_FPS] = "FPS", [UI_MENU_PERF_FULL] = "Đầy đủ",
    [UI_PERF_LINE_FPS] = "%.0f fps",
    [UI_PERF_LINE_FPS_GEN] = "%.0f fps drawn, %.0f shown",
    [UI_MENU_GRAPHICS] = "Hình ảnh",
    [UI_MENU_OUTPUT_RES] = "Độ phân giải game",
    [UI_MENU_OUTPUT_RES_DESC] = "Kích thước game dùng để vẽ hình ảnh và giao diện. Trên 1920 x 1080 game cần nhiều bộ nhớ "
                                "video và thời gian GPU hơn. Kích thước rộng hơn (21:9) mở rộng tầm nhìn của camera và đưa "
                                "HUD ra các góc màn hình; kích thước cao hơn (16:10, 3:2) đưa HUD lên mép trên và mép dưới.",
    [UI_MENU_HUD_WIDTH] = "Chiều rộng HUD",
    [UI_MENU_HUD_WIDTH_DESC] = "Trên màn hình rộng hơn 16:9, khoảng cách giữa bên trái và bên phải của HUD. Toàn màn hình "
                               "đặt chúng sát mép màn hình; 21:9 hoặc 16:9 giữ HUD trong phần giữa màn hình có tỷ lệ đó. "
                               "Bố cục còn thiếu sẽ được tạo ở nền và dùng sau khi khởi động lại.",
    [UI_MENU_HUD_WIDTH_FULL] = "Toàn màn hình",
    [UI_MENU_RENDER_SCALE] = "Tỷ lệ kết xuất 3D",
    [UI_MENU_RENDER_SCALE_DESC] = "Kích thước cảnh 3D so với độ phân giải game; FSR của game phóng nó lên kích thước đó. "
                                  "Trên 100 % sẽ làm mịn răng cưa (supersampling) nhưng tốn nhiều GPU.",
    [UI_MENU_RENDER_SCALE_GAME] = "Theo game (83 %)",
    [UI_MENU_DYNAMIC_RES] = "Độ phân giải động",
    [UI_MENU_DYNAMIC_RES_DESC] = "Cho phép game hạ cảnh 3D xuống 90 hoặc 80 %. Game đánh giá theo ước tính dành cho Switch và bỏ qua GPU của bạn, nên cảnh rộng vẫn bị hạ ngay cả trên PC mạnh. Tắt thì giữ nguyên tỷ lệ kết xuất.",
    [UI_MENU_UPSCALER] = "Phóng to theo cửa sổ",
    [UI_MENU_UPSCALER_DESC] = "Cách phóng hình ảnh khi cửa sổ lớn hơn độ phân giải game. FSR giữ cạnh và chi tiết sắc nét "
                              "hơn bilinear.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1", [UI_MENU_UPSCALER_BILINEAR] = "Bilinear",
    [UI_MENU_ASPECT] = "Tỷ lệ hình ảnh",
    [UI_MENU_ASPECT_DESC] = "Khi tỷ lệ cửa sổ khác tỷ lệ độ phân giải game (hình 16:9 trên màn 21:9): giữ nguyên tỷ lệ hình "
                            "với các dải đen, hoặc kéo giãn ra cả cửa sổ.",
    [UI_MENU_ASPECT_FIT] = "Dải đen", [UI_MENU_ASPECT_STRETCH] = "Kéo giãn",
    [UI_MENU_MOVIE_ASPECT] = "Tỷ lệ video",
    [UI_MENU_MOVIE_ASPECT_DESC] = "Khi độ phân giải game không phải 16:9: giữ tỷ lệ gốc của các video cắt cảnh với các dải "
                                  "đen, hoặc kéo giãn ra toàn bộ hình ảnh. Áp dụng từ video kế tiếp.",
    [UI_MENU_MOVIE_ASPECT_KEEP] = "Dải đen", [UI_MENU_MOVIE_ASPECT_STRETCH] = "Kéo giãn",
    [UI_MENU_FOV] = "Góc nhìn",
    [UI_MENU_FOV_DESC] = "Camera cho thấy bao nhiêu thế giới, tính theo phần trăm của game gốc. Game phiên bản 1.1.0, 1.2.1 và 1.4.3.",
    [UI_MENU_VERT_PLUS] = "Giữ bề ngang 16:9",
    [UI_MENU_VERT_PLUS_DESC] = "Trên màn hình hẹp hơn 16:9 (Steam Deck, 16:10, 4:3), camera cho thấy thế giới rộng ngang như ở 16:9, và nhiều hơn ở phía trên và dưới. Không có tác dụng ở 16:9 hoặc rộng hơn.",
    [UI_MENU_SHADOW_RES] = "Kích thước bản đồ bóng",
    [UI_MENU_SHADOW_RES_DESC] = "Kích thước bản đồ bóng của mặt trời. Kích thước của game đổi theo tầm nhìn, xuống còn khoảng "
                                "400 khi gần mặt đất, nên bóng lá trông bị vỡ ô và nhảy; kích thước cố định giữ chúng đứng "
                                "yên và sắc nét hơn. Trên 1024 bóng tốn thêm thời gian GPU và bộ nhớ video; 256 thô hơn của "
                                "game và dành cho GPU tích hợp thiếu băng thông bộ nhớ.",
    [UI_MENU_SHADOW_RES_GAME] = "Theo game (thay đổi)",
    [UI_MENU_ANISO] = "Lọc họa tiết",
    [UI_MENU_ANISO_DESC] = "Lọc dị hướng cho họa tiết bề mặt: nền đất, đường và tường nhìn ở góc thấp vẫn sắc nét ở xa thay vì bị nhòe. Theo game: mức game yêu cầu, như trên Switch.",
    [UI_MENU_ANISO_GAME] = "Theo game",
    [UI_MENU_LOD_STEADY] = "Giữ chi tiết ổn định khi tải nặng",
    [UI_MENU_LOD_STEADY_DESC] = "Bỏ qua cơ chế của game hạ chi tiết mô hình và họa tiết khi tốc độ khung hình tụt, giống mod giả lập Disable LOD Quality Reduction. Game phiên bản 1.1.0, 1.2.1 và 1.4.3. Nên tắt trên PC yếu: ở đó việc hạ này giữ tốc độ khung hình.",
    [UI_MENU_SHARPNESS] = "Độ sắc nét FSR",
    [UI_MENU_SHARPNESS_DESC] = "Làm nét sau khi FSR phóng to; 0 là tắt.",
    [UI_MENU_COLOR] = "Màu sắc",
    [UI_MENU_COLOR_PRESET] = "Hồ sơ màu",
    [UI_MENU_COLOR_PRESET_DESC] = "Đặt bốn giá trị bên dưới. Sống động: tương phản và độ bão hòa cao hơn, vùng tối được mở "
                                  "nhẹ (dành cho màn hình OLED). Rõ nét: tăng nhẹ tương phản, độ bão hòa và độ sáng. Kéo "
                                  "một giá trị sẽ chuyển hồ sơ sang Tùy chỉnh. HUD đổi theo hình ảnh; menu này thì không.",
    [UI_MENU_COLOR_DEFAULT] = "Của game", [UI_MENU_COLOR_VIVID] = "Sống động (OLED)", [UI_MENU_COLOR_CLARITY] = "Rõ nét",
    [UI_MENU_COLOR_CUSTOM] = "Tùy chỉnh",
    [UI_MENU_COLOR_BRIGHTNESS] = "Độ sáng", [UI_MENU_COLOR_BRIGHTNESS_DESC] = "Cộng vào mọi màu; 0 là giữ nguyên.",
    [UI_MENU_COLOR_CONTRAST] = "Độ tương phản",
    [UI_MENU_COLOR_CONTRAST_DESC] = "Đẩy màu ra xa xám trung bình (trên 100 %) hoặc kéo lại gần; 100 % là giữ nguyên.",
    [UI_MENU_COLOR_SATURATION] = "Độ bão hòa",
    [UI_MENU_COLOR_SATURATION_DESC] = "Độ đậm của màu so với xám; 100 % là giữ nguyên.",
    [UI_MENU_COLOR_GAMMA] = "Gamma",
    [UI_MENU_COLOR_GAMMA_DESC] = "Dưới 1 làm sáng vùng tối và vùng giữa, trên 1 làm tối chúng; 1 là giữ nguyên.",
    [UI_MENU_HDR_GROUP] = "HDR",
    [UI_MENU_HDR] = "Xuất HDR",
    [UI_MENU_HDR_DESC] = "Vùng sáng chói trên màn hình HDR. Cần bật HDR trong Windows (\"Use HDR\") hoặc trong compositor "
                         "Wayland có quản lý màu; nếu không hình ảnh vẫn là SDR. Áp dụng sau khi khởi động lại. Hồ sơ màu "
                         "ở trên dành cho SDR và không được dùng trong HDR.",
    [UI_MENU_HDR_PEAK] = "Độ sáng đỉnh",
    [UI_MENU_HDR_PEAK_DESC] = "Độ sáng đỉnh của màn hình bạn tính bằng nit; các vùng sáng nhất dừng ở đó.",
    [UI_MENU_HDR_GAME] = "Độ sáng game",
    [UI_MENU_HDR_GAME_DESC] = "Nit của màu trắng trong hình (paper white). 203 là chuẩn tham chiếu HDR; hãy tăng lên trong "
                              "phòng sáng.",
    [UI_MENU_HDR_UI] = "Độ sáng HUD và menu",
    [UI_MENU_HDR_UI_DESC] = "Nit của màu trắng trong HUD và menu này.",
    [UI_MENU_HDR_HUE] = "Hiệu chỉnh sắc độ vùng sáng",
    [UI_MENU_HDR_HUE_DESC] = "Màu sáng như lửa và tia lửa chuyển dần sang trắng như trong hình SDR của game, khớp với ánh sáng "
                             "chúng phát ra; 0 giữ chúng bão hòa hoàn toàn.",
    [UI_MENU_SHADOW_SCALE] = "Độ phân giải bóng",
    [UI_MENU_SHADOW_SCALE_DESC] = "Vẽ bản đồ bóng mặt trời lớn hơn mức 960x960 của game theo số lần này ở mỗi cạnh: cạnh bóng "
                                  "và bóng lá sắc nét hơn. Có hiệu lực từ lần khởi động kế tiếp.",
    [UI_MENU_PCSS] = "Bóng mềm",
    [UI_MENU_PCSS_DESC] = "Bóng mặt trời sắc nét ở chỗ vật thể chạm đất và mềm dần theo độ xa của bóng (PCSS). 0 giữ bộ lọc "
                          "của game. Có hiệu lực từ lần khởi động kế tiếp.",
    [UI_MENU_CONTACT_SHADOWS] = "Bóng tiếp xúc",
    [UI_MENU_CONTACT_SHADOWS_DESC] = "Các bóng mặt trời nhỏ mà bản đồ bóng bỏ sót: ở chân, dưới đá, trong cỏ và các khe. "
                                     "Được dò trên màn hình sau khi chiếu sáng. Có hiệu lực từ lần khởi động kế tiếp.",
    [UI_MENU_SHADOW_RANGE] = "Tầm cascade bóng",
    [UI_MENU_SHADOW_RANGE_DESC] = "Dời các bậc mà bóng chuyển sang mềm hơn (20, 40, 80 và 160 m của game) ra xa hơn. Bản đồ "
                                  "bóng trải trên vùng đất rộng hơn: hãy tăng Độ phân giải bóng cùng lúc. Có hiệu lực từ "
                                  "lần khởi động kế tiếp.",
    [UI_MENU_GRASS_RANGE] = "Tầm cỏ",
    [UI_MENU_GRASS_RANGE_DESC] = "Cỏ mọc xa đến đâu quanh camera (của game: 62,5 m). Cao hơn thì cỏ ở đồng cỏ xa vẫn còn thay "
                                 "vì chỉ mọc ngay trước mặt bạn; thấp hơn thì vẽ ít cỏ hơn. Có hiệu lực từ lần khởi động kế "
                                 "tiếp.",
    [UI_MENU_DLSS_PRESET] = "DLSS model",
    [UI_MENU_DLSS_PRESET_DESC] = "Model DLSS nào được chạy. Tự động theo khuyến nghị của NVIDIA cho từng chế độ: K cho DLAA, "
                                 "Chất lượng và Cân bằng, M cho Hiệu năng, L cho Siêu hiệu năng. J: bớt bóng mờ phía sau "
                                 "nhân vật di chuyển, nhấp nháy nhiều hơn một chút. K: model sắc nét nhất ở mọi chế độ. Áp "
                                 "dụng ngay.",
    [UI_MENU_DLSS_PRESET_DEFAULT] = "Automatic", [UI_MENU_DLSS_PRESET_K] = "K (sharpest)",
    [UI_MENU_DLSS_PRESET_J] = "J (less ghosting)",
    [UI_MENU_DLSS_CHARS] = "Vệt mờ nhân vật DLSS",
    [UI_MENU_DLSS_CHARS_DESC] = "Giảm các vệt mờ nhạt phía sau nhân vật, động vật đang di chuyển và vật chúng cầm: DLSS lấy "
                                "ít hơn từ các khung hình trước ở đó. Mạnh hơn xóa được nhiều vệt hơn nhưng làm các cạnh đó "
                                "kém mịn hơn một chút. Áp dụng ngay.",
    [UI_MENU_DLSS_CHARS_OFF] = "Tắt", [UI_MENU_DLSS_CHARS_LIGHT] = "Nhẹ", [UI_MENU_DLSS_CHARS_MEDIUM] = "Vừa",
    [UI_MENU_DLSS_CHARS_FULL] = "Tối đa",
    [UI_MENU_DLSS] = "Chế độ phóng to",
    [UI_MENU_DLSS_DESC] = "Gốc: khử răng cưa cảnh 3D ở độ phân giải đầy đủ. Chất lượng, Cân bằng, Hiệu năng và Siêu hiệu năng "
                          "vẽ cảnh ở 67, 58, 50 và 33 % rồi để bộ phóng to (DLSS hoặc FSR 3.1) phóng lên, giúp tiết kiệm "
                          "thời gian GPU; chúng thay thế Tỷ lệ kết xuất 3D. Có hiệu lực ngay.",
    [UI_MENU_DLSS_DLAA] = "Gốc", [UI_MENU_DLSS_QUALITY] = "Chất lượng", [UI_MENU_DLSS_BALANCED] = "Cân bằng",
    [UI_MENU_DLSS_PERFORMANCE] = "Hiệu năng", [UI_MENU_DLSS_ULTRA] = "Siêu hiệu năng",
    [UI_MENU_FXAA] = "Khử răng cưa của game (FXAA)",
    [UI_MENU_FXAA_DESC] = "FXAA làm mịn cạnh của chính game. Nó làm nhòe chi tiết nhỏ một chút; khi bật DLAA thì không cần.",
    [UI_MENU_REFLEX] = "Độ trễ thấp",
    [UI_MENU_REFLEX_DESC] = "Giảm độ trễ hệ thống. Bật dùng NVIDIA Reflex (VK_NV_low_latency2); Bật + Boost còn giữ xung nhịp GPU NVIDIA ở mức cao trong các cảnh bị giới hạn bởi CPU. Tự động dùng Reflex trên NVIDIA và AMD Anti-Lag 2 (VK_AMD_anti_lag) trên AMD. Áp dụng sau khi khởi động lại.",
    [UI_MENU_REFLEX_AUTO] = "Tự động",
    [UI_MENU_REFLEX_OFF] = "Tắt",
    [UI_MENU_REFLEX_ON] = "Bật",
    [UI_MENU_REFLEX_BOOST] = "Bật + Boost",
    [UI_MENU_SCENE_UPSCALER] = "Bộ phóng to cảnh 3D",
    [UI_MENU_SCENE_UPSCALER_DESC] = "Bộ phóng to theo thời gian cho cảnh 3D: cạnh mịn hơn và ít nhấp nháy hơn, và ở chế độ "
                                    "thấp hơn Gốc nó vẽ cảnh nhỏ hơn rồi phóng lên, giúp tiết kiệm thời gian GPU. NVIDIA "
                                    "DLSS cần GPU NVIDIA RTX và " DLSS_FILE " cạnh " EXE_FILE "; AMD FSR 3.1 chạy trên mọi "
                                    "GPU. Tắt thì giữ cách phóng to và FXAA của chính game. HUD không bị ảnh hưởng; bộ phóng "
                                    "to và độ sắc nét ở trên vẫn đưa hình game vừa với cửa sổ. Có hiệu lực sau khi khởi động "
                                    "lại.",
    [UI_MENU_SCENE_UPSCALER_OFF] = "Tắt", [UI_MENU_SCENE_UPSCALER_DLSS] = "NVIDIA DLSS",
    [UI_MENU_SCENE_UPSCALER_FSR3] = "AMD FSR 3.1",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_GPU] = "DLSS: cần GPU NVIDIA RTX",
    [UI_MENU_SCENE_UPSCALER_NO_DLSS_DLL] = "DLSS: thiếu " DLSS_FILE,
    [UI_MENU_FSR_SHARPNESS] = "Độ sắc nét FSR",
    [UI_MENU_FSR_SHARPNESS_DESC] = "Làm nét cảnh 3D sau AMD FSR 3.1 hoặc FSR 4; 0 là tắt.",
    [UI_MENU_SCENE_UPSCALER_FSR4] = "AMD FSR 4",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_GPU] = "FSR 4: không hỗ trợ GPU này",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_DLL] = "FSR 4: thiếu thư viện FSR của AMD",
    [UI_MENU_SCENE_UPSCALER_NO_FSR4_OS] = "FSR 4: chỉ trên Windows",

    [UI_MENU_FPS_LIMIT] = "Giới hạn tốc độ khung hình",
    [UI_MENU_FPS_LIMIT_DESC] = "Số khung hình tối đa mỗi giây game vẽ. Trên 60 là thử nghiệm và cần bật Tốc độ game theo "
                               "đồng hồ.",
    [UI_MENU_FPS_30] = "30 FPS", [UI_MENU_FPS_40] = "40 FPS", [UI_MENU_FPS_45] = "45 FPS",
    [UI_MENU_FPS_60] = "60 FPS", [UI_MENU_FPS_90] = "90 FPS", [UI_MENU_FPS_120] = "120 FPS", [UI_MENU_FPS_144] = "144 FPS",
    [UI_MENU_FPS_DISPLAY] = "Theo màn hình", [UI_MENU_FPS_DISPLAY_HZ] = "Theo màn hình (%.0f Hz)",
    [UI_MENU_FRAME_QUEUE] = "Hàng đợi khung hình",
    [UI_MENU_FRAME_QUEUE_DESC] = "Game bắt đầu một khung hình trong lúc khung trước đang chờ đến lượt lên màn hình: ít rớt "
                                 "khung hơn hẳn ở mức giới hạn, đổi lại trễ điều khiển thêm tối đa một khung hình. Áp dụng ngay.",
    [UI_MENU_FRAME_GEN] = "Tạo khung hình", [UI_MENU_FRAME_GEN_OFF] = "Tắt", [UI_MENU_FRAME_GEN_FSR3] = "AMD FSR 3",
    [UI_MENU_FRAME_GEN_DESC] = "Một hình được tạo ra giữa mỗi hai hình game vẽ: số hình trên màn hình gấp đôi, game chạy ở "
                               "một nửa giới hạn khung hình và điều khiển trễ thêm khoảng nửa khung hình. Dành cho màn hình "
                               "từ 120 Hz trở lên. Không dùng cùng DLSS hoặc HDR. Áp dụng ngay.",
    [UI_MENU_REALTIME] = "Tốc độ game theo đồng hồ",
    [UI_MENU_REALTIME_DESC] = "Chạy game theo thời gian thực, để một khung hình chậm không làm game chậm theo.",
    [UI_MENU_CLOTH_REALTIME] = "Vải theo đồng hồ",
    [UI_MENU_CLOTH_REALTIME_DESC] = "Chạy cả vải và tóc theo thời gian thực.",
    [UI_MENU_HIGHFPS_FIX] = "Sửa lỗi tốc độ khung hình cao",
    [UI_MENU_HIGHFPS_FIX_DESC] = "Sửa vật lý hoạt động sai trên 30 khung hình mỗi giây, và các vật thể vẫn trong suốt hoặc vô hình trên 60.",
    [UI_MENU_CLIMB_FIX] = "Sửa tốc độ leo trèo",
    [UI_MENU_CLIMB_FIX_DESC] = "Giữ tốc độ leo tường và thang đầy đủ trên 60 khung hình mỗi giây, để Link lên được tới đỉnh.",
    [UI_MENU_GAME_LANG] = "Ngôn ngữ game", [UI_MENU_GAME_LANG_DESC] = "Ngôn ngữ của văn bản và giọng nói trong game.",
    [UI_MENU_GAME_LANG_AUTO] = "Tự động",
    [UI_MENU_OPERATION_MODE] = "Chế độ máy",
    [UI_MENU_OPERATION_MODE_DESC] = "Game nghĩ nó đang chạy ở chế độ cắm đế hay cầm tay.",
    [UI_MENU_MODE_DOCKED] = "Cắm đế", [UI_MENU_MODE_HANDHELD] = "Cầm tay",
    [UI_MENU_ASSUME_FOCUS] = "Tiếp tục chạy khi mất focus",
    [UI_MENU_ASSUME_FOCUS_DESC] = "Bật (mặc định): game vẫn chạy khi bạn ở cửa sổ khác, và bàn phím im lặng cho đến khi cửa "
                                  "sổ game lên trước lại. Tắt: game tạm dừng khi cửa sổ mất focus, như trên Switch khi nhấn "
                                  "Home; lần tải bị ngắt theo cách đó có thể dừng hẳn.",

    [UI_MENU_PLAY_TIME] = "Thời gian chơi",
    [UI_MENU_PLAY_TIME_DESC] = "Thời gian trong game qua mọi lần chạy, chỉ tính khi cửa sổ game có focus và game đang vẽ "
                               "khung hình. Lưu trong playtime.txt ở thư mục hồ sơ, cạnh các tệp save.",
    [UI_MENU_PLAY_TIME_HM] = "%llu giờ %02u phút", [UI_MENU_PLAY_TIME_SESSION] = "lần chạy này: %s",
    [UI_MENU_MODS_BUILTIN] = "Tích hợp sẵn trong bản port", [UI_MENU_MODS_INSTALLED] = "Mod đã cài",
    [UI_MENU_MODS_NONE] = "Chưa có mod nào. Giải nén mod vào thư mục mods, mỗi mod một thư mục chứa thư mục romfs bên trong, "
                          "rồi khởi động lại game. TKMM cũng có thể xuất các mod đã gộp vào một thư mục ở đó.",
    [UI_MENU_MODS_OPEN] = "Mở thư mục mod",
    [UI_MENU_MODS_OPEN_DESC] = "Hiện thư mục game tải mod từ đó. Nếu chưa có, thư mục sẽ được tạo.",
    [UI_MENU_MODS_ROW_DESC] = "Tệp game nó thay thế: %u. Tệp mới nó thêm vào: %u.",
    [UI_MENU_MODS_OVERRIDDEN] = "%u tệp của nó lấy từ %s, mod cao hơn trong danh sách.",
    [UI_MENU_MODS_MOVE_HINT] = "Nút mặt trên hoặc Ctrl+Lên đẩy nó lên, nút mặt trái hoặc Ctrl+Xuống đẩy nó xuống. Khi hai mod "
                               "đổi cùng một tệp, dữ liệu game hay mã, mod cao hơn thắng. Thay đổi áp dụng sau khi khởi "
                               "động lại.",
    [UI_MENU_MODS_EMPTY] = "Thư mục này không có tệp romfs, exefs hoặc cheats.",
    [UI_MENU_MODS_SUBSDK] = "Mod này mang theo mã game riêng (plugin subsdk hoặc main bị thay). Bản port không tải được "
                            "module mã, nên cả mod vẫn tắt. Công tắc \"Dùng ... không kèm mã\" bên dưới sẽ dùng phần còn lại.",
    [UI_MENU_MODS_EXEFS_LATER] = "Mod này vá mã game (exefs). Game sẽ tích hợp các bản vá khi khởi động lần sau (dựng lại vài "
                                 "phút); đến lúc đó cả mod vẫn tắt.",
    [UI_MENU_MODS_CHEATS] = "Cheat: %d. Bật chúng ở tab Cheat.",
    [UI_MENU_MODS_CHEATS_FILE] = "Cheat cho phiên bản game này được đọc khi khởi động từ cheats\\%s trong mod này.",
    [UI_MENU_MODS_BAD_NAME] = "Tên thư mục chứa ký tự |, mà danh sách mod dùng để ngăn cách các tên. Hãy đổi tên thư mục.",
    [UI_MENU_MODS_OTHER_VERSION] = "%u tệp của nó mang phiên bản game khác trong tên. Game chỉ đọc chúng khi có gì đó yêu cầu "
                                   "đúng các tên ấy. Nếu mod không có tác dụng, hãy tìm bản của nó dành cho phiên bản game "
                                   "của bạn.",
    [UI_MENU_MODS_UNUSED] = "không dùng",
    [UI_MENU_MODS_FPS60] = "Bản vá 60 FPS",
    [UI_MENU_MODS_FPS60_DESC] = "Bản vá riêng của bản port cho 60 khung hình mỗi giây. Đặt cùng Giới hạn tốc độ khung hình ở "
                                "tab Trò chơi.",
    [UI_MENU_MODS_RES] = "Bản vá độ phân giải",
    [UI_MENU_MODS_RES_DESC] = "Bản vá riêng của bản port cho độ phân giải trên 1080p. Đặt cùng Độ phân giải game ở tab Màn hình.",
    [UI_MENU_MODS_QUICKMENU] = "Sửa menu nhanh",
    [UI_MENU_MODS_QUICKMENU_DESC] = "Bản sửa của bản port cho nền đen của các menu vũ khí, khiên và nguyên liệu ở 60 FPS.",

    [UI_MENU_VOLUME] = "Âm lượng", [UI_MENU_VOLUME_DESC] = "Âm lượng tổng của game.",
    [UI_MENU_VOLUME_MUSIC] = "Âm lượng nhạc", [UI_MENU_VOLUME_MUSIC_DESC] = "Âm lượng nhạc nền.",
    [UI_MENU_VOLUME_SFX] = "Âm lượng hiệu ứng", [UI_MENU_VOLUME_SFX_DESC] = "Âm lượng hiệu ứng âm thanh.",
    [UI_MENU_VOLUME_VOICE] = "Âm lượng giọng nói", [UI_MENU_VOLUME_VOICE_DESC] = "Âm lượng giọng nói nhân vật.",
    [UI_MENU_AUDIO_CHANNELS] = "Loa",
    [UI_MENU_AUDIO_CHANNELS_DESC] = "Đầu ra stereo hoặc surround 5.1. Tự động chọn 5.1 khi cấu hình loa của Windows có từ sáu "
                                    "kênh trở lên. Áp dụng sau khi khởi động lại.",
    [UI_MENU_CHANNELS_AUTO] = "Tự động", [UI_MENU_CHANNELS_STEREO] = "Stereo", [UI_MENU_CHANNELS_51] = "Surround 5.1",
    [UI_MENU_AUDIO_DEVICE] = "Thiết bị đầu ra",
    [UI_MENU_AUDIO_DEVICE_DESC] = "Nơi phát âm thanh. Mặc định hệ thống theo thiết bị mặc định của Windows. Thiết bị đã chọn "
                                  "mà bị rút ra sẽ quay về mặc định hệ thống cho đến khi nó trở lại. Áp dụng ngay.",
    [UI_MENU_AUDIO_DEVICE_DEFAULT] = "Mặc định hệ thống",

    [UI_MENU_MOTION] = "Điều khiển chuyển động", [UI_MENU_MOTION_DESC] = "Ngắm bằng con quay hồi chuyển của tay cầm.",
    [UI_MENU_GYRO_CALIBRATION] = "Hiệu chỉnh độ lệch con quay",
    [UI_MENU_GYRO_CALIBRATION_DESC] = "Trừ độ trôi lúc nghỉ của tay cầm, đo bằng dòng hiệu chỉnh bên dưới. Các giá trị đã đo "
                                      "vẫn được lưu khi tắt mục này.",
    [UI_MENU_GYRO_CALIBRATE] = "Hiệu chỉnh con quay",
    [UI_MENU_GYRO_CALIBRATE_DESC] = "Đo độ trôi lúc nghỉ của con quay và loại bỏ nó. Đặt tay cầm xuống hoặc giữ yên trong hai "
                                    "giây.",
    [UI_MENU_GYRO_CALIBRATE_WAIT] = "Đang hiệu chỉnh...",
    [UI_MENU_GYRO_CALIBRATED] = "%s, đã hiệu chỉnh",
    [UI_MENU_GYRO_CALIBRATE_FAILED] = "Hiệu chỉnh thất bại",
    [UI_MENU_GYRO_CALIBRATE_MOVED] = "Tay cầm đã di chuyển: hãy giữ yên và thử lại",
    [UI_MENU_GYRO_AUTO_BIAS] = "Tự học độ trôi con quay khi nghỉ",
    [UI_MENU_GYRO_AUTO_BIAS_DESC] = "Học độ trôi còn lại trong con quay mỗi khi tay cầm nằm yên trên bàn, rồi trừ nó đi. Dành "
                                    "cho cú ngắm cung từ từ trượt dù đã hiệu chỉnh. Tay cầm đang cầm trên tay thì không được "
                                    "học. Thử nghiệm.",
    [UI_MENU_RUMBLE_STRENGTH] = "Cường độ rung",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "Tay cầm rung mạnh đến mức nào.",
    [UI_MENU_RUMBLE_CURVE] = "Đường cong rung",
    [UI_MENU_RUMBLE_CURVE_DESC] = "Giá trị thấp hơn làm rung yếu mạnh lên.",
    [UI_MENU_RUMBLE_TRIGGERS] = "Rung cò (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Rung cả các cò của tay cầm Xbox.",
    [UI_MENU_PAD_LAYOUT] = "Bố cục nút",
    [UI_MENU_PAD_LAYOUT_DESC] = "Nút nào là A. Nintendo: nút bên phải, như trên Switch. Xbox: nút phía dưới, như ghi trên tay "
                                "cầm Xbox (dấu X trên tay cầm PlayStation). Tự động chọn Nintendo cho tay cầm có nhãn "
                                "Nintendo và Xbox cho các tay cầm khác. Biểu tượng tay cầm nào hiện trong game được đặt ở "
                                "Biểu tượng nút.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Tự động", [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A bên phải)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A phía dưới)",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "đang dùng: %s",
    [UI_MENU_PROMPTS] = "Biểu tượng nút",
    [UI_MENU_PROMPTS_DESC] = "Biểu tượng nút của tay cầm nào hiện trong game. Nintendo: của chính game. Xbox: LB, RB, LT, RT, "
                             "Menu, View và chữ thập, cùng A, B, X, Y như ghi trên nút mà bố cục ở trên nhấn (với Nintendo "
                             "(A bên phải), nút bên phải mang nhãn B, nên biểu tượng cho A hiện B). PlayStation: các hình "
                             "trên tay cầm PlayStation (○ × △ □, L1, R1, L2, R2, Options và Share), trên nút mà bố cục ở "
                             "trên nhấn. PC: các phím và nút chuột của bố cục bàn phím mặc định (E, Shift, Space, nhấp "
                             "trái...) và chuột cho camera. Game đọc biểu tượng khi khởi động, nên chúng đổi sau khi khởi "
                             "động lại. Chúng nằm trong tệp của game, nên nút bạn gán lại trong Điều khiển vẫn giữ biểu "
                             "tượng cũ trên màn hình.",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo", [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation", [UI_MENU_PROMPTS_PC] = "PC",
    [UI_MENU_PROMPTS_MISSING] = "thiếu tệp biểu tượng: hãy chạy lại trình cài đặt",
    [UI_MENU_PROMPTS_FAILED] = "tệp biểu tượng bị hỏng: hãy chạy lại trình cài đặt",
    [UI_MENU_PROMPTS_RESTART] = "biểu tượng đổi sau khi khởi động lại",
    [UI_MENU_PROMPTS_OFF_SHAPE] = "tắt ở độ phân giải game này (chưa có bố cục cho nó)",
    [UI_MENU_OUTPUT_RES_NO_LAYOUT] = "chưa có bố cục cho tỷ lệ này: hãy chạy lại trình cài đặt",
    [UI_MENU_OUTPUT_RES_PREPARING] = "bố cục đang được tạo ở nền; dùng từ lần khởi động kế tiếp",
    [UI_MENU_PROMPTS_PREPARING] = "tệp biểu tượng đang được tạo ở nền; dùng từ lần khởi động kế tiếp",
    [UI_MENU_PREPARE_RETRY] = "Thử lại",
    [UI_MENU_PREPARE_RETRY_DESC] = "Chạy lại các công cụ của trình cài đặt để tạo ở nền các tệp bố cục giao diện hoặc biểu tượng nút còn thiếu. Game vẫn chạy; nó dùng chúng từ lần khởi động kế tiếp.",
    [UI_MENU_SHAPE_PREPARE_FAILED] = "các công cụ của trình cài đặt không khởi chạy: hãy chạy lại trình cài đặt",
    [UI_MENU_PROMPTS_OFF_MODS] = "tắt: một mod RomFS thay các tệp của chúng",
    [UI_MENU_PROMPTS_OFF_PATCHES] = "tắt khi TOTK_ROMFS_PATCHES được đặt",
    [UI_MENU_RESTART] = "Khởi động lại ngay",
    [UI_MENU_RESTART_DESC] = "Đóng game và chạy lại với biểu tượng mới. Tiến trình từ lần lưu gần nhất sẽ mất.",
    [UI_MENU_RESTART_CONFIRM] = "Khởi động lại game ngay? Tiến trình từ lần lưu gần nhất sẽ mất.",
    [UI_MENU_RESTART_YES] = "Khởi động lại",
    [UI_MENU_RESTART_SAVING] = "Game đang lưu: sẽ khởi động lại được sau giây lát.",
    [UI_MENU_GROUP_STICKS] = "Cần analog",
    [UI_MENU_LSTICK_DEADZONE] = "Vùng chết cần trái", [UI_MENU_RSTICK_DEADZONE] = "Vùng chết cần phải",
    [UI_MENU_STICK_DEADZONE_DESC] = "Cần của tay cầm phải nghiêng bao xa thì game mới nhận. Hãy tăng nếu nhân vật hoặc camera tự di chuyển: thả cần ra, đọc \"cần lúc này\" và đặt vùng chết cao hơn vài phần trăm.",
    [UI_MENU_STICK_DEADZONE_NOW] = "cần lúc này: %.0f %%",
    [UI_MENU_MOUSE_MODE] = "Camera chuột",
    [UI_MENU_MOUSE_MODE_DESC] = "Trực tiếp: camera xoay theo quãng chuột di chuyển, như game PC, không có vùng chết hay đường cong của cần. Giả lập cần: chuột đóng vai cần phải.",
    [UI_MENU_MOUSE_STICK] = "Giả lập cần",
    [UI_MENU_MOUSE_DIRECT] = "Trực tiếp",
    [UI_MENU_MENU_CURSOR] = "Chuột trong menu của game",
    [UI_MENU_MENU_CURSOR_DESC] = "Trong menu riêng của game (tiêu đề, túi đồ, cửa hàng, lựa chọn hội thoại) con trỏ hiện ra: mục dưới nó được chọn, nhấp trái để xác nhận, nhấp phải để quay lại. Chỉ khi chuột hoặc phím được dùng gần nhất, nên tay cầm thấy menu không đổi (game 1.4.3).",
    [UI_MENU_MAP_CLICK] = "Nhấp trái trên bản đồ",
    [UI_MENU_MAP_CLICK_DESC] = "Trên bản đồ của Purah Pad, nút chuột trái nhấn A tại con trỏ bản đồ thay cho nút nó được gán (mặc định là Y). Cần bật Chuột trong menu của game (game 1.4.3).",
    [UI_MENU_DIALOGUE_CLICK] = "Nhấp chuột trong hội thoại",
    [UI_MENU_DIALOGUE_CLICK_DESC] = "Khi hộp hội thoại đang mở, nhấp trái nhấn B (tiếp tục văn bản và bỏ qua cuộn chữ) và nhấp phải nhấn A, thay cho các nút chúng được gán (mặc định là Y, mở nhật ký hội thoại, và ZL). Lựa chọn hội thoại giữ con trỏ: nhấp trái để chọn (game 1.4.3).",
    [UI_MENU_POINTER_IMAGE] = "Con trỏ game",
    [UI_MENU_POINTER_IMAGE_DESC] = "Khi con trỏ không bị bắt (menu của game, menu này, sau khi nhấn Esc), cửa sổ hiện một mũi tên vàng riêng thay cho mũi tên hệ thống, cỡ theo tỷ lệ màn hình.",
    [UI_MENU_QUICKSAVE] = "Lưu nhanh và tải nhanh",
    [UI_MENU_QUICKSAVE_DESC] = "Một phím lưu game ngay và một phím khác tải bản lưu đó (hai dòng bên dưới: mặc định F6 và F7, như Half-Life), không hiện menu trên màn hình. Chỉ phím bàn phím và nút chuột, chỉ game 1.4.3. Lưu nhanh nhờ game tạo một bản lưu tự động của chính nó, nên nó thay bản lưu tự động cũ nhất (bản lưu thủ công vẫn giữ), giữ một bản sao làm bản lưu nhanh (thư mục quicksave của hồ sơ) và hiện \"Đã lưu game\" ở đầu hình. Tải nhanh giữ hình ở dòng \"Đang tải bản lưu nhanh\" trong lúc nó đi qua danh sách tải của game khuất tầm nhìn, rồi game tải bản lưu nhanh (khoảng 5 giây). Ở chỗ game không tự tạo bản lưu tự động (cảnh cắt, hội thoại), sẽ không lưu gì và dòng chữ cho biết điều đó.",
    [UI_MENU_QUICKSAVE_KEY] = "Phím lưu nhanh",
    [UI_MENU_QUICKLOAD_KEY] = "Phím tải nhanh",
    [UI_MENU_QUICKSAVE_KEY_DESC] = "Phím bắt đầu lưu nhanh hoặc tải nhanh; mặc định F6 và F7, như Half-Life. Chọn ô rồi nhấn phím hoặc nút chuột bất kỳ; Delete để ô đó không có phím. Khi lưu nhanh đang bật, hai phím này không làm gì khác (có ghi chú khi một phím gán cũng dùng phím đó). totk.ini nhận mọi tên phím của phần gán phím (quicksave_key, quickload_key).",
    [UI_QS_SAVED] = "Đã lưu game",
    [UI_QS_LOADING] = "Đang tải bản lưu nhanh",
    [UI_QS_NOT_NOW] = "Hiện chưa thể lưu nhanh",
    [UI_QS_NONE] = "Chưa có bản lưu nhanh (phím lưu nhanh sẽ tạo một bản)",
    [UI_QS_FAILED] = "Lưu nhanh thất bại: xem totk.log",
    [UI_QS_LOAD_FAILED] = "Tải nhanh thất bại: xem totk.log",
    [UI_MENU_MOUSE_SENS] = "Độ nhạy camera chuột",
    [UI_MENU_MOUSE_SENS_DESC] = "Chuột xoay camera nhanh đến đâu.",
    [UI_MENU_WALK_KEY] = "Phím đi bộ",
    [UI_MENU_WALK_KEY_DESC] = "Giữ nó để nghiêng các cần của bàn phím một phần, để bàn phím đi bộ thay vì chạy. Tỷ lệ đi bộ "
                              "đặt độ nghiêng. Tay cầm và chuột không bị ảnh hưởng.",
    [UI_MENU_WALK_KEY_TAKEN] = "cũng là %s",
    [UI_MENU_WALK_SCALE] = "Tỷ lệ đi bộ",
    [UI_MENU_WALK_SCALE_DESC] = "Độ nghiêng của các cần bàn phím khi giữ phím đi bộ. Một nửa là đi bộ, nghiêng hết là chạy.",
    [UI_MENU_FREECAM] = "Camera tự do",
    [UI_MENU_FREECAM_DESC] = "Cho phép một tổ hợp hoặc một phím bên dưới tách góc nhìn khỏi Link và bay đi. Mặc định tắt: khi "
        "tắt, mã camera của game chạy nguyên bản. Có hiệu lực sau khi khởi động lại. Game phiên bản 1.4.3.",
    [UI_MENU_FREECAM_OFF_NOTE] = "Hãy bật Camera tự do ở trên rồi khởi động lại",
    [UI_MENU_FREECAM_PAD] = "Tổ hợp camera tự do",
    [UI_MENU_FREECAM_PAD_DESC] = "Các nút tay cầm nhấn cùng lúc để chuyển góc nhìn qua ba giai đoạn: rời Link và bay, đứng yên "
        "tại chỗ vừa để trong khi Link chơi tiếp, và quay lại. Khi đang bay game không nhận điều khiển: cần trái bay theo "
        "hướng nhìn và sang ngang, các cò lên và xuống, cần phải xoay, và mỗi lần nhấn L hoặc R giảm một nửa hoặc tăng gấp "
        "đôi tốc độ. Khi khóa, Link đi theo hướng cần trái chỉ trên màn hình. Game phiên bản 1.4.3.",
    [UI_MENU_FREECAM_PAD_V143] = "Chỉ game phiên bản 1.4.3",
    [UI_MENU_FREECAM_KEY] = "Phím camera tự do",
    [UI_MENU_FREECAM_KEY_DESC] = "Một phím chuyển góc nhìn qua cùng các giai đoạn: rời Link và bay, đứng yên trong khi Link "
        "chơi tiếp, và quay lại. Khi đang bay: W A S D bay theo hướng nhìn và sang ngang, các phím gán cho ZR và ZL (mặc định "
        "X và Z) lên và xuống, chuột và phím mũi tên xoay, và các phím gán cho L và R (Q và R) giảm một nửa hoặc tăng gấp đôi "
        "tốc độ. Game phiên bản 1.4.3.",
    [UI_MENU_GROUP_GAMEPLAY_TWEAKS] = "Tinh chỉnh lối chơi",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED] = "Tinh chỉnh lối chơi",
    [UI_MENU_GAMEPLAY_TWEAKS_ENABLED_DESC] = "Điều khiển bằng con lăn chuột cho xoay và khoảng cách của Ultrahand, đổi vật phẩm và vũ khí, và thu phóng bản đồ mượt.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE] = "Xoay Ultrahand bằng con lăn",
    [UI_MENU_GAMEPLAY_ULTRAHAND_ROTATE_DESC] = "Xoay vật đang giữ bằng Ultrahand trực tiếp bằng con lăn chuột.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS] = "Trục xoay C của Ultrahand",
    [UI_MENU_GAMEPLAY_ULTRAHAND_C_AXIS_DESC] = "Giữ C khi cuộn để xoay vật đang giữ quanh trục còn lại.",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY] = "Độ nhạy xoay Ultrahand",
    [UI_MENU_GAMEPLAY_ROTATION_SENSITIVITY_DESC] = "Con lăn chuột xoay vật đang giữ nhanh đến đâu.",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE] = "Khoảng cách Ultrahand bằng con lăn",
    [UI_MENU_GAMEPLAY_ULTRAHAND_DISTANCE_DESC] = "Nhấp phải + con lăn chuột đẩy ra hoặc kéo vào vật đang giữ.",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY] = "Độ nhạy khoảng cách Ultrahand",
    [UI_MENU_GAMEPLAY_DISTANCE_SENSITIVITY_DESC] = "Con lăn chuột đổi khoảng cách vật nhanh đến đâu.",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL] = "Chọn vũ khí bằng con lăn",
    [UI_MENU_GAMEPLAY_WEAPON_WHEEL_DESC] = "Cuộn con lăn chuột khi di chuyển tự do để đổi vũ khí. Xác nhận bằng E.",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY] = "Độ nhạy con lăn vũ khí",
    [UI_MENU_GAMEPLAY_WEAPON_SENSITIVITY_DESC] = "Số nấc cuộn cần để chuyển sang vũ khí kế tiếp.",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL] = "Chọn menu nhanh bằng con lăn",
    [UI_MENU_GAMEPLAY_ITEM_WHEEL_DESC] = "Cuộn con lăn chuột để duyệt vật phẩm khi menu chọn nhanh đang mở.",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY] = "Độ nhạy con lăn menu nhanh",
    [UI_MENU_GAMEPLAY_ITEM_SENSITIVITY_DESC] = "Số nấc cuộn cần để chuyển qua các vật phẩm trong menu chọn nhanh.",
    [UI_MENU_GAMEPLAY_ITEM_AD] = "Điều hướng menu nhanh bằng A / D",
    [UI_MENU_GAMEPLAY_ITEM_AD_DESC] = "Nhấn A / D để di chuyển trái và phải trong menu chọn nhanh.",
    [UI_MENU_GAMEPLAY_MAP_ZOOM] = "Thu phóng bản đồ mượt",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_DESC] = "Thu phóng có quán tính mượt trên bản đồ Purah Pad bằng con lăn chuột.",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY] = "Độ nhạy thu phóng bản đồ",
    [UI_MENU_GAMEPLAY_MAP_ZOOM_SENSITIVITY_DESC] = "Con lăn chuột thu phóng bản đồ nhanh đến đâu.",
    [UI_MENU_STICK_SENS] = "Tốc độ camera bằng cần",
    [UI_MENU_STICK_SENS_DESC] = "Nhân tốc độ camera của cần phải trong game, kể cả tốc độ tối đa, ở mỗi mức trong năm mức độ "
                                "nhạy camera của game. Chuột giữ cài đặt riêng của nó.",

    [UI_BINDS_HELP] = "Chọn một ô và nhấn A hoặc Enter, rồi nhấn phím, nút chuột hoặc nút tay cầm. Delete hoặc X xóa một ô. "
                      "Phím đang được dùng sẽ chuyển sang đây; nút tay cầm đổi chỗ với nút mà nó thay.",
    [UI_BINDS_COL_KEY1] = "Phím 1", [UI_BINDS_COL_KEY2] = "Phím 2", [UI_BINDS_COL_PAD] = "Tay cầm",
    [UI_BINDS_GROUP_BUTTONS] = "Nút", [UI_BINDS_GROUP_MOVE] = "Di chuyển", [UI_BINDS_GROUP_CAMERA] = "Camera",
    [UI_BINDS_PRESS_KEY] = "Nhấn một phím hoặc nút chuột cho %s",
    [UI_BINDS_PRESS_PAD] = "Nhấn một nút tay cầm cho %s",
    [UI_BINDS_CANCEL_HINT] = "Esc để hủy, hoặc chờ %d giây",
    [UI_BINDS_TAKEN] = "%s đã bị lấy từ %s", [UI_BINDS_SWAPPED] = "%s đã đổi chỗ với %s",
    [UI_BINDS_RESET] = "Đặt lại mọi nút",
    [UI_BINDS_RESET_DESC] = "Về phím và nút tay cầm mặc định. Bố cục nút ở trên áp dụng lại: các nút tay cầm đã gán lại vẫn ở nơi bạn đặt cho đến lần đặt lại này.",
    [UI_BIND_A] = "A", [UI_BIND_B] = "B", [UI_BIND_X] = "X", [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "Nhấn cần trái", [UI_BIND_RCLICK] = "Nhấn cần phải", [UI_BIND_L] = "L",
    [UI_BIND_R] = "R", [UI_BIND_ZL] = "ZL", [UI_BIND_ZR] = "ZR", [UI_BIND_PLUS] = "Cộng", [UI_BIND_MINUS] = "Trừ",
    [UI_BIND_LEFT] = "D-pad trái", [UI_BIND_UP] = "D-pad lên", [UI_BIND_RIGHT] = "D-pad phải",
    [UI_BIND_DOWN] = "D-pad xuống",
    [UI_BIND_LSTICK_LEFT] = "Đi sang trái", [UI_BIND_LSTICK_UP] = "Đi tới", [UI_BIND_LSTICK_RIGHT] = "Đi sang phải",
    [UI_BIND_LSTICK_DOWN] = "Đi lùi",
    [UI_BIND_RSTICK_LEFT] = "Camera trái", [UI_BIND_RSTICK_UP] = "Camera lên", [UI_BIND_RSTICK_RIGHT] = "Camera phải",
    [UI_BIND_RSTICK_DOWN] = "Camera xuống",
    [UI_BIND_DESC_A] = "Tương tác, nhặt, nói chuyện.", [UI_BIND_DESC_B] = "Chạy, hủy, quay lại.",
    [UI_BIND_DESC_X] = "Nhảy.", [UI_BIND_DESC_Y] = "Tấn công.", [UI_BIND_DESC_LCLICK] = "Cúi người.",
    [UI_BIND_DESC_RCLICK] = "Góc nhìn thứ nhất.", [UI_BIND_DESC_L] = "Khả năng.",
    [UI_BIND_DESC_R] = "Ném vũ khí.", [UI_BIND_DESC_ZL] = "Khiên, khóa mục tiêu.", [UI_BIND_DESC_ZR] = "Cung.",
    [UI_BIND_DESC_PLUS] = "Menu, túi đồ.", [UI_BIND_DESC_MINUS] = "Bản đồ.",
    [UI_BIND_DESC_DPAD] = "Chọn nhanh.",
    [UI_BIND_DESC_MOVE] = "Di chuyển Link như cần trái. Cần trái của tay cầm luôn hoạt động.",
    [UI_BIND_DESC_CAMERA] = "Xoay camera như cần phải. Chuột và cần phải của tay cầm luôn hoạt động.",
    [UI_BIND_SHAKE] = "Lắc cần camera",
    [UI_BIND_DESC_SHAKE] = "Gạt cần phải qua trái và phải thay cho game: tách bộ phận đã dính ra khỏi vật Ultrahand đang giữ. "
                           "Giữ phím thì tiếp tục lắc.",
    [UI_KEY_SPACE] = "Space", [UI_KEY_TAB] = "Tab", [UI_KEY_LSHIFT] = "Shift trái", [UI_KEY_LCTRL] = "Ctrl trái",
    [UI_KEY_UP] = "Mũi tên lên", [UI_KEY_DOWN] = "Mũi tên xuống", [UI_KEY_LEFT] = "Mũi tên trái",
    [UI_KEY_RIGHT] = "Mũi tên phải",
    [UI_KEY_MOUSE_L] = "Chuột trái", [UI_KEY_MOUSE_R] = "Chuột phải", [UI_KEY_MOUSE_M] = "Chuột giữa",
    [UI_KEY_MOUSE_X1] = "Chuột 4", [UI_KEY_MOUSE_X2] = "Chuột 5",
    [UI_KEY_ENTER] = "Enter", [UI_KEY_BACKSPACE] = "Backspace", [UI_KEY_RSHIFT] = "Shift phải",
    [UI_KEY_RCTRL] = "Ctrl phải", [UI_KEY_LALT] = "Alt", [UI_KEY_CAPSLOCK] = "Caps Lock", [UI_KEY_INSERT] = "Insert",
    [UI_KEY_DELETE] = "Delete", [UI_KEY_HOME] = "Home", [UI_KEY_END] = "End", [UI_KEY_PAGEUP] = "Page Up",
    [UI_KEY_PAGEDOWN] = "Page Down", [UI_KEY_NUM_FMT] = "Num %s",
    [UI_PAD_SOUTH] = "Nút dưới", [UI_PAD_EAST] = "Nút phải", [UI_PAD_WEST] = "Nút trái",
    [UI_PAD_NORTH] = "Nút trên", [UI_PAD_LSTICK] = "Cần trái", [UI_PAD_RSTICK] = "Cần phải",
    [UI_PAD_L] = "Nút vai trái", [UI_PAD_R] = "Nút vai phải", [UI_PAD_START] = "Start", [UI_PAD_BACK] = "Back",
    [UI_PAD_DPAD_LEFT] = "D-pad trái", [UI_PAD_DPAD_UP] = "D-pad lên", [UI_PAD_DPAD_RIGHT] = "D-pad phải",
    [UI_PAD_DPAD_DOWN] = "D-pad xuống", [UI_PAD_ZL] = "Cò trái", [UI_PAD_ZR] = "Cò phải",
    [UI_PAD_LPADDLE1] = "Nút lưng trái", [UI_PAD_RPADDLE1] = "Nút lưng phải",
    [UI_PAD_LPADDLE2] = "Nút lưng trái 2", [UI_PAD_RPADDLE2] = "Nút lưng phải 2",
    [UI_PAD_TOUCHPAD] = "Nhấn touchpad",
    [UI_PAD_CROSS] = "Chữ X", [UI_PAD_CIRCLE] = "Tròn", [UI_PAD_SQUARE] = "Vuông", [UI_PAD_TRIANGLE] = "Tam giác",

    [UI_MENU_AMIIBO_DIR] = "Thư mục amiibo",
    [UI_MENU_AMIIBO_OPEN] = "Mở",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "Thư mục mặc định",
    [UI_MENU_AMIIBO_REFRESH] = "Làm mới",
    [UI_MENU_AMIIBO_PICK] = "Tệp dump amiibo (.bin hoặc .nfc 540 byte):",
    [UI_MENU_AMIIBO_NO_DIR] = "Không mở được thư mục %s",
    [UI_MENU_AMIIBO_READER] = "Đầu đọc NFC",
    [UI_MENU_AMIIBO_STATE] = "Đầu đọc:",
    [UI_MENU_AMIIBO_STATE_IDLE] = "sẵn sàng",
    [UI_MENU_AMIIBO_STATE_SEARCHING] = "game đang chờ một amiibo",
    [UI_MENU_AMIIBO_STATE_FOUND] = "amiibo trên đầu đọc",
    [UI_MENU_AMIIBO_STATE_MOUNTED] = "game đang đọc amiibo",
    [UI_MENU_AMIIBO_STATE_OFF] = "tắt",
    [UI_MENU_AMIIBO_FILE] = "Tệp đã chọn:",
    [UI_MENU_AMIIBO_NO_FILE] = "(chưa chọn tệp)",
    [UI_MENU_AMIIBO_TAP] = "Chạm amiibo",
    [UI_MENU_AMIIBO_REMOVE] = "Nhấc amiibo ra",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "Tự nhấc amiibo ra sau khi chạm",
    [UI_MENU_AMIIBO_HOLD] = "Thời gian trên đầu đọc",
    [UI_MENU_AMIIBO_SECONDS] = "%.1f giây",
    [UI_MENU_AMIIBO_TAG] = "Amiibo trên đầu đọc:",
    [UI_MENU_AMIIBO_NO_TAG] = "Không có amiibo trên đầu đọc.",
    [UI_MENU_AMIIBO_TAG_FILE] = "Tệp: %s",
    [UI_MENU_AMIIBO_TAG_ID] = "Nhân vật %s, series %02x, số %04x, loại %02x",
    [UI_MENU_AMIIBO_TAG_UID] = "UID: %s",
    [UI_MENU_AMIIBO_READY] = "Sẵn sàng",
    [UI_MENU_AMIIBO_TAPPED] = "Đã chạm: %s",
    [UI_MENU_AMIIBO_LOAD_FAILED] = "Không phải tệp dump amiibo (NTAG215 540 byte): %s",
    [UI_MENU_AMIIBO_REMOVED] = "Đã nhấc amiibo ra",
    [UI_MENU_AMIIBO_STATUS] = "Trạng thái: %s",
    [UI_MENU_MENU_LANG] = "Ngôn ngữ menu", [UI_MENU_MENU_LANG_DESC] = "Ngôn ngữ của menu này và lớp phủ hiệu năng.",
    [UI_MENU_MENU_LANG_AUTO] = "Tự động (theo Windows)",
    [UI_MENU_DISCORD_RPC] = "Trạng thái Discord (Rich Presence)",
    [UI_MENU_DISCORD_RPC_DESC] = "Hiện \"Playing TotK Recomp\" cùng thời gian chơi trong trạng thái Discord của bạn khi game "
                                 "chạy. discord_app_id và discord_image trong totk.ini chọn ứng dụng.",

    [UI_MENU_ADV_WARNING] = "Các cài đặt này dành cho kiểm thử và khắc phục sự cố. Chỉ đổi khi bạn biết lý do.",
    [UI_MENU_GROUP_SCHED] = "Lập lịch", [UI_MENU_GROUP_SHADERS] = "Bộ đổ bóng và đường ống đồ họa",
    [UI_MENU_GROUP_TEXMEM] = "Họa tiết và bộ nhớ", [UI_MENU_GROUP_FIXES] = "Sửa lỗi kết xuất",
    [UI_MENU_GROUP_VULKAN] = "Vulkan", [UI_MENU_GROUP_DIAG] = "Chẩn đoán",
    [UI_MENU_ASYNC_PREEMPT] = "Chiếm quyền trên lõi ảo",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "Cho phép một luồng game chiếm quyền của luồng khác trên cùng lõi ảo.",
    [UI_MENU_PREEMPT_OFF] = "Tắt", [UI_MENU_PREEMPT_ON] = "Bật", [UI_MENU_PREEMPT_INHERIT] = "Chỉ kế thừa",
    [UI_MENU_REPLAY_ASYNC] = "Chuẩn bị khung GPU ở luồng riêng",
    [UI_MENU_REPLAY_ASYNC_DESC] = "Chuẩn bị khung hình cho GPU ở một luồng riêng thay vì luồng game. Thêm tối đa một khung hình độ trễ.",
    [UI_MENU_REPLAY_WORKERS] = "Số luồng chuẩn bị khung GPU",
    [UI_MENU_REPLAY_WORKERS_DESC] = "Ghi các khung hình lớn cho GPU trên nhiều luồng cùng lúc. Hữu ích khi CPU giới hạn tốc độ khung hình; cần từ 8 bộ xử lý logic trở lên. Tắt thì ghi trên một luồng.",
    [UI_MENU_REPLAY_WORKERS_NOW] = "hiện tại: %s",
    [UI_MENU_TIMER_1MS] = "Bộ hẹn giờ Windows 1 ms",
    [UI_MENU_TIMER_1MS_DESC] = "Nâng độ phân giải bộ hẹn giờ hệ thống lên 1 ms để nhịp khung hình đều hơn.",
    [UI_MENU_SYNC_AHEAD] = "Đi trước GPU một khung hình",
    [UI_MENU_SYNC_AHEAD_DESC] = "Game dựng khung hình tiếp theo trong khi card đồ họa vẫn vẽ khung hình trước. Nhiều khung hình mỗi giây hơn, thêm một khung hình độ trễ điều khiển.",
    [UI_MENU_HOST_CHECKPOINT] = "Nhường lõi nhanh",
    [UI_MENU_HOST_CHECKPOINT_DESC] = "Một luồng game nhường lõi ảo cho luồng quan trọng hơn ngay khi rời khỏi mã của bản port, không đợi đến lời gọi hệ thống tiếp theo.",
    [UI_MENU_PIPE_WAIT] = "Xử lý lệnh vẽ đang chờ",
    [UI_MENU_PIPE_WAIT_DESC] = "Mặc định giữ cách lập lịch hiện tại, có một luồng xử lý dành riêng cho các lệnh vẽ đang chờ; Cũ (lệnh vẽ) dùng cách lập lịch trước đó.",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "Mặc định", [UI_MENU_PIPE_WAIT_DRAW] = "Cũ (lệnh vẽ)",
    [UI_MENU_PIPE_SKIP] = "Lệnh vẽ bị bỏ qua khi đường ống đang dựng",
    [UI_MENU_PIPE_SKIP_DESC] = "Những lệnh vẽ nào bị bỏ thay vì chờ đường ống của chúng.",
    [UI_MENU_PIPE_SKIP_STRICT] = "Nghiêm ngặt", [UI_MENU_PIPE_SKIP_ANY] = "Bất kỳ",
    [UI_MENU_PIPE_SKIP_FULLSCREEN] = "Lượt vẽ toàn màn hình",
    [UI_MENU_LIB_PREBUILD] = "Dựng trước thư viện công đoạn",
    [UI_MENU_LIB_PREBUILD_DESC] = "Dựng trước các thư viện công đoạn của bộ đổ bóng ở nền.",
    [UI_MENU_LIB_PREBUILD_THREADS] = "Số luồng dựng trước thư viện công đoạn",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "Số luồng dùng để dựng trước; Tự động chọn con số theo CPU của bạn.",
    [UI_MENU_PIPE_PREBUILD] = "Dựng trước đường ống",
    [UI_MENU_PIPE_PREBUILD_DESC] = "Dựng trước các đường ống đã biết.",
    [UI_MENU_PREWARM_XLATE] = "Làm nóng bộ dịch",
    [UI_MENU_PREWARM_XLATE_DESC] = "Số luồng dịch các bộ đổ bóng đã biết khi khởi động; Tự động lấy một nửa số luồng CPU, tối đa 16.",
    [UI_MENU_GPL] = "Thư viện đường ống đồ họa",
    [UI_MENU_GPL_DESC] = "Dùng phần mở rộng thư viện đường ống của Vulkan; hãy tắt nếu trình điều khiển của bạn hoạt động sai với nó.",
    [UI_MENU_ASTC] = "Giải mã họa tiết ASTC", [UI_MENU_ASTC_DESC] = "Nơi giải mã họa tiết ASTC đã nén.",
    [UI_MENU_ASTC_AUTO] = "Tự động", [UI_MENU_ASTC_GPU] = "Bộ đổ bóng GPU", [UI_MENU_ASTC_NATIVE] = "Gốc",
    [UI_MENU_ASTC_CPU] = "CPU",
    [UI_MENU_MEMORY] = "Chế độ bộ nhớ GPU", [UI_MENU_MEMORY_DESC] = "Cách bộ nhớ game được chia sẻ với GPU.",
    [UI_MENU_MEMORY_AUTO] = "Tự động", [UI_MENU_MEMORY_IMPORT] = "Nhập", [UI_MENU_MEMORY_MIRROR] = "Phản chiếu",
    [UI_MENU_MEMORY_VERIFY] = "Kiểm tra",
    [UI_MENU_TEX_ALIAS] = "Chồng họa tiết", [UI_MENU_TEX_ALIAS_DESC] = "Một họa tiết mới tạo đè lên bộ nhớ của họa tiết đang dùng sẽ bắt đầu với nội dung đó thay vì trống.",
    [UI_MENU_TEX_VRAM_PRESSURE] = "Giới hạn bộ nhớ họa tiết", [UI_MENU_TEX_VRAM_PRESSURE_DESC] = "Phần trăm bộ nhớ video mà trên mức đó các họa tiết game đã giải phóng bị bỏ (chúng được nạp lại từ ổ lưu trữ khi dùng lần nữa). 0 là giữ lại.",
    [UI_MENU_ASTC_BC] = "Nén ASTC sang BC3", [UI_MENU_ASTC_BC_DESC] = "Với bộ giải mã GPU: mã hóa lại họa tiết ASTC đã giải mã sang BC3, chỉ tốn một phần tư bộ nhớ video. Họa tiết trông hơi vỡ khối hơn. Tự động: bật cho GPU có 4 GB bộ nhớ video trở xuống.",
    [UI_MENU_LOD_GUARD] = "Bảo vệ LOD", [UI_MENU_LOD_GUARD_DESC] = "Ngăn vật thể nhấp nháy sai chỗ hoặc vùng xa bị phẳng trong một khung hình, bằng cách chờ game ghi xong dữ liệu mức chi tiết trước khi GPU đọc.",
    [UI_MENU_FIX_C0] = "Sửa bộ đệm hằng số",
    [UI_MENU_FIX_C0_DESC] = "Điền bảng hằng số c0 mà trình điều khiển dành riêng (kích thước vùng lưu trữ, kích thước điều phối, kích thước điểm, thực thể gốc) thay vì đọc ra số không.",
    [UI_MENU_FIX_ZERO] = "Xóa sạch bộ nhớ GPU mới",
    [UI_MENU_FIX_ZERO_DESC] = "Các vùng nhớ GPU mới bắt đầu với toàn số không, như bộ nhớ Switch.",
    [UI_MENU_DLAA_EFFECTS_AFTER] = "Hạt sau DLAA",
    [UI_MENU_DLAA_EFFECTS_AFTER_DESC] = "Với DLAA, bụi và các hạt khác được vẽ đè lên hình DLSS đã làm mịn, như game "
                                        "vẽ chúng khi không có DLSS: các hạt nhỏ trong sương mù (đền thờ) không còn "
                                        "biến mất hay để lại vệt. Sương mù và mây vẫn đi qua DLSS, nên các cạnh ở xa "
                                        "trên nền trời vẫn sạch. Tắt: DLSS cũng xử lý các hạt này, như trước. Chỉ ở "
                                        "chế độ DLAA; các chế độ phóng to DLSS luôn xử lý chúng. Áp dụng ngay.",
    [UI_MENU_DLSS_FIXED_EXPOSURE] = "Phơi sáng DLSS cố định",
    [UI_MENU_DLSS_FIXED_EXPOSURE_DESC] = "DLSS lấy độ sáng của hình game như nó vốn có thay vì ước lượng phơi sáng từ từng "
                                         "khung hình: trong cảnh tối (Depths) các vùng sương lớn không còn tối hoặc sáng đi "
                                         "một khung hình khi camera di chuyển. Tắt: DLSS tự ước lượng phơi sáng, như trước. "
                                         "Áp dụng ngay.",
    [UI_MENU_BIND_SKIP] = "Bỏ qua liên kết lặp lại",
    [UI_MENU_BIND_SKIP_DESC] = "Tối ưu hóa: bỏ qua các liên kết không đổi.",
    [UI_MENU_SHADER_MEMO] = "Ghi nhớ bộ đổ bóng", [UI_MENU_SHADER_MEMO_DESC] = "Tối ưu hóa: ghi nhớ các lần tra cứu bộ đổ bóng.",
    [UI_MENU_PIPE_MEMO] = "Ghi nhớ đường ống", [UI_MENU_PIPE_MEMO_DESC] = "Tối ưu hóa: dùng lại đường ống của lệnh vẽ trước khi trạng thái lặp lại.",
    [UI_MENU_TEX_MEMO] = "Ghi nhớ liên kết họa tiết", [UI_MENU_TEX_MEMO_DESC] = "Tối ưu hóa: ghi nhớ bộ mô tả họa tiết giữa các lệnh vẽ.",
    [UI_MENU_DESC_TEMPLATE] = "Mẫu bộ mô tả",
    [UI_MENU_DESC_TEMPLATE_DESC] = "Tối ưu hóa: cập nhật bộ mô tả hàng loạt.",
    [UI_MENU_PUSH_SKIP] = "Bỏ qua hằng số đẩy lặp lại",
    [UI_MENU_PUSH_SKIP_DESC] = "Tối ưu hóa: bỏ qua các hằng số đẩy không đổi.",
    [UI_MENU_SPLIT_DRAWS] = "Gửi GPU sớm",
    [UI_MENU_SPLIT_DRAWS_DESC] = "Gửi công việc GPU theo từng phần sau mỗi N lệnh vẽ giữa các lượt vẽ để GPU bắt đầu sớm hơn; 0 là tắt.",
    [UI_MENU_VK_VALIDATION] = "Lớp kiểm tra",
    [UI_MENU_VK_VALIDATION_DESC] = "Kiểm tra mọi lệnh gọi Vulkan và làm game chậm đi rất nhiều.",
    [UI_MENU_VK_VALIDATION_MISSING] = "Lớp kiểm tra chưa được cài.",
    [UI_MENU_VK_DYN_VERTEX] = "Trạng thái đỉnh động",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "Buộc bật hoặc tắt đầu vào đỉnh động; mặc định phụ thuộc GPU.",
    [UI_MENU_VK_DYN_A2C] = "Alpha sang độ phủ động",
    [UI_MENU_VK_DYN_A2C_DESC] = "Buộc bật hoặc tắt alpha sang độ phủ động. Mặc định: tắt trên trình điều khiển của AMD cho RDNA 2 trở xuống, nơi nhân vật bị vẽ đen khi bật, và trên trình điều khiển Windows của Intel cho GPU tích hợp, nơi tán lá bị nhiễu màu; bật ở các nơi khác.",
    [UI_MENU_VK_SUBGROUP_PIN] = "Ghim kích thước nhóm con", [UI_MENU_VK_SUBGROUP_PIN_DESC] = "Cố định kích thước nhóm con của bộ đổ bóng.",
    [UI_MENU_TRACE] = "Nhật ký hiệu năng", [UI_MENU_TRACE_DESC] = "Ghi thời gian khung hình ra tệp.",
    [UI_MENU_FLIGHT] = "Hộp đen",
    [UI_MENU_FLIGHT_DESC] = "Giữ một lịch sử ngắn và lưu lại khi một khung hình bị đứng.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "Ngưỡng đứng hình (ms)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "Thời gian khung hình được tính là đứng hình.",
    [UI_MENU_PASS_CRUMBS] = "Dấu lượt vẽ GPU",
    [UI_MENU_PASS_CRUMBS_DESC] = "Sau khi GPU gặp sự cố, nhật ký ghi tên lượt kết xuất đang chạy. Tự động: chỉ AMD. Bật: cả NVIDIA, tốn thêm một chút.",
    [UI_MENU_INPUT_RECORD] = "Ghi đầu vào",
    [UI_MENU_INPUT_RECORD_DESC] = "Ghi các thao tác nhập của bạn ra tệp để báo lỗi.",
    [UI_MENU_OPEN_LOG] = "Mở thư mục nhật ký", [UI_MENU_OPEN_LOG_DESC] = "Hiện thư mục chứa các tệp nhật ký.",
    [UI_MENU_COPY_INFO] = "Sao chép thông tin hệ thống",
    [UI_MENU_COPY_INFO_DESC] = "Sao chép GPU, trình điều khiển, CPU, RAM và các cài đặt đã đổi vào bộ nhớ tạm.",
    [UI_MENU_COPY_INFO_DONE] = "Đã sao chép", [UI_MENU_ACTION_FAILED] = "không thành công",
    [UI_MENU_RESET] = "Đặt lại mọi cài đặt về mặc định", [UI_MENU_RESET_DESC] = "Áp dụng cho mọi tab.",
    [UI_MENU_RESET_CONFIRM] = "Đặt lại mọi cài đặt về mặc định?", [UI_MENU_RESET_YES] = "Đặt lại",
    [UI_MENU_RESET_NO] = "Hủy",

    [UI_MENU_OV_FPS] = "%.0f FPS", [UI_MENU_OV_FRAME_MS] = "%.1f ms", [UI_MENU_OV_GAME_PCT] = "game %.0f%%",
    [UI_MENU_OV_MAX_MS] = "max %.1f ms",
    [UI_MENU_OV_FPS_LABEL] = "FPS", [UI_MENU_OV_FRAME_LABEL] = "Frame", [UI_MENU_OV_FMT_FPS] = "%.0f",
    [UI_MENU_OV_LOW1] = "1%% low %.0f", [UI_MENU_OV_LOW01] = "0.1%% low %.0f",
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
    [UI_MENU_OV_UPSCALER] = "3D upscaler",
    [UI_MENU_OV_SCALING] = "Scaling", [UI_MENU_OV_LIMIT] = "Frame limit", [UI_MENU_OV_PRESENT] = "Presentation", [UI_MENU_OV_SHOWN] = "Shown",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u", [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, sharpness %d%%", [UI_MENU_OV_SCALE_NONE] = "none (1:1)",
    [UI_MENU_OV_SCALE_BILINEAR] = "bilinear",
    [UI_MENU_OV_SCALE_DOWN] = "bilinear (FSR only enlarges)",
    [UI_MENU_TAB_CHEATS] = "Cheat",
    [UI_MENU_CHEATS_NONE] = "Không có cheat cho phiên bản game này. Cheat của mod là tệp cheat Atmosphere trong thư mục "
                            "cheats của nó, đặt theo bản dựng của game: mods\\<mod>\\cheats\\%s. Hãy khởi động lại game để "
                            "đọc chúng. Với cheat thay đổi mã game, game tự dựng lại khi khởi động (vài phút).",
    [UI_MENU_CHEATS_MOD] = "%s: %d cheat",
    [UI_MENU_CHEAT_CODE_DESC] = "Thay đổi mã của game. Bật tắt ngay.",
    [UI_MENU_CHEAT_KEY_DESC] = "Thay đổi mã của game trong lúc bạn giữ %s. Bật tắt ngay.",
    [UI_MENU_CHEAT_DATA_DESC] = "Ghi vào bộ nhớ game mỗi khung hình khi đang bật.",
    [UI_MENU_CHEAT_MASTER] = "luôn bật",
    [UI_MENU_CHEAT_MASTER_DESC] = "Mã chủ của tệp: luôn bật.",
    [UI_MENU_CHEAT_RESTORE] = "không cần",
    [UI_MENU_CHEAT_RESTORE_DESC] = "Nó chỉ ghi lại mã gốc của game. Tắt một cheat đã làm việc đó rồi.",
    [UI_MENU_CHEAT_REBUILD] = "sau khi dựng lại",
    [UI_MENU_CHEAT_REBUILD_DESC] = "Cheat này thay đổi mã game và là mới hoặc đã sửa kể từ khi game được dựng. Game tích hợp "
                                   "nó khi khởi động lần sau (vài phút), hoặc dùng Dựng lại game ngay.",
    [UI_MENU_CHEAT_UNSUPPORTED] = "không hỗ trợ",
    [UI_MENU_CHEAT_UNSUPPORTED_DESC] = "Cheat này không dùng được với phiên bản game này:",
    [UI_MENU_CHEATS_REBUILD] = "Dựng lại game ngay",
    [UI_MENU_CHEATS_REBUILD_DESC] = "Một số cheat thay đổi mã game và chưa được tích hợp vào nó: chúng mới hoặc đã sửa, hoặc "
                                    "lần dựng lại gần nhất thất bại hay bị hủy. Đóng game; trình cài đặt tích hợp chúng (vài "
                                    "phút) rồi chạy lại game. Tiến trình từ lần lưu gần nhất sẽ mất.",
    [UI_MENU_MODS_EXEFS_UNSUPPORTED] = "Mod này vá mã game (exefs) theo cách bản port không áp dụng được, nên cả mod vẫn "
                                       "tắt. Công tắc \"Dùng ... không kèm mã\" bên dưới sẽ dùng phần còn lại. %s",
    [UI_MENU_MODS_EXEFS_OTHER] = "Các bản vá mã (exefs) của nó được làm cho phiên bản game khác.",
    [UI_MENU_MODS_EXEFS_ON] = "Bản vá mã đang dùng: %d tệp.",
    [UI_MENU_MODS_REPLACES_BUILTIN] = "Bản vá mã đang dùng: %d tệp. Chúng thay bản vá riêng %s của bản port, bản vá đó vẫn "
                                      "tắt khi mod này bật.",
    [UI_MENU_MODS_CODE_SKIPPED] = "Mã game riêng của nó (%s) không được tải: phần còn lại của mod vẫn dùng. Hãy tắt mod nếu "
                                  "game hoạt động sai với nó.",
    [UI_MENU_MODS_PLUGIN_ON] = "Plugin của nó đang chạy: %d hook vào mã game.",
    [UI_MENU_MODS_PLUGIN_REFUSED] = "Plugin %s của nó không chạy được trong bản port: %s.",
    [UI_MENU_MODS_PLUGIN_STAYS_OFF] = "Cả mod vẫn tắt. Công tắc \"Dùng ... không kèm mã\" bên dưới sẽ dùng phần còn lại.",
    [UI_MENU_MODS_PLUGIN_OFF] = "Plugin của nó lần này vẫn tắt: nó khởi động khác với bản dựng (%s). Game sẽ dựng lại ở lần "
                                "khởi động kế tiếp.",
    [UI_MENU_MODS_WITHOUT_CODE] = "Dùng %s không kèm mã",
    [UI_MENU_MODS_WITHOUT_CODE_DESC] = "Mod ở trên mang theo mã mà bản port không chạy được: mã game riêng (plugin subsdk hoặc "
                                       "main bị thay) hoặc bản vá mã (exefs) bản port không áp dụng được. Tắt: cả mod vẫn "
                                       "tắt. Bật: phần mã đó bị bỏ và dùng phần còn lại của mod: tệp RomFS, cheat và các "
                                       "bản vá mã chạy được. Tệp của nó có thể cần phần mã bị thiếu; hãy tắt lại nếu game "
                                       "hoạt động sai. Áp dụng từ lần khởi động kế tiếp.",
    [UI_MENU_MODS_WITHOUT_CODE_ALL] = "Bật cho mọi mod: mods_without_code = 1 trong totk.ini.",
    [UI_MENU_MODS_EXEFS_SKIPPED] = "%u tệp bản vá mã (exefs) của nó không áp dụng được và bị bỏ: phần còn lại của mod vẫn "
                                   "dùng. %s",
    [UI_MENU_MODS_OVERRIDES_FILES] = "%u tệp của nó thay tệp của %s, mod thấp hơn trong danh sách.",
    [UI_MENU_MODS_DATA_OVERRIDDEN] = "%u byte thay đổi dữ liệu game của nó lấy từ %s, mod cao hơn trong danh sách.",
    [UI_MENU_MODS_OVERRIDES_DATA] = "%u byte thay đổi dữ liệu game của nó thay của %s, mod thấp hơn trong danh sách.",
    [UI_MENU_MODS_CODE_OVERRIDDEN] = "%u vị trí vá mã của nó bị %s ghi đè, mod cao hơn trong danh sách.",
    [UI_MENU_MODS_OVERRIDES_CODE] = "Nó ghi đè %u vị trí vá mã của %s, mod thấp hơn trong danh sách.",
};

#define SWKBD_ONLY(name, ...) static const char *const name[UI_TEXT_COUNT] = {SWKBD(__VA_ARGS__)}
SWKBD_ONLY(text_pt, "Inserir texto", "Introduz um nome", "Cavalo", "OK", "Cancelar", "Comando: A = OK, B = Cancelar");
SWKBD_ONLY(text_zh_hant, "文字輸入", "請輸入名字", "馬兒", "確定", "取消", "手把：A 確定，B 取消");

static const Row rows[] = {
    {"en", text_en}, {"ja", text_ja}, {"fr", text_fr}, {"de", text_de}, {"it", text_it}, {"es", text_es},
    {"nl", text_nl}, {"pt", text_pt}, {"ru", text_ru}, {"zh-Hans", text_zh_hans}, {"zh-Hant", text_zh_hant},
    {"ko", text_ko}, {"pt-BR", text_pt_br}, {"vi", text_vi},
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

// Every game language: the Mals files EUen/USen, USfr/EUfr, USes/EUes ship in both builds; USpt only from 1.4.3.
int ui_text_game_langs(int game_version, UiGameLang *out, int max) {
    static const struct { const char *code; UiTextId name; int since; } all[] = {
        {"en-US", UI_MENU_LANGNAME_EN, 0}, {"en-GB", UI_MENU_LANGNAME_EN_GB, 0}, {"ja", UI_MENU_LANGNAME_JA, 0},
        {"fr", UI_MENU_LANGNAME_FR, 0}, {"fr-CA", UI_MENU_LANGNAME_FR_CA, 0}, {"de", UI_MENU_LANGNAME_DE, 0},
        {"it", UI_MENU_LANGNAME_IT, 0}, {"es", UI_MENU_LANGNAME_ES, 0}, {"es-419", UI_MENU_LANGNAME_ES_419, 0},
        {"pt-BR", UI_MENU_LANGNAME_PT_BR, 143}, {"nl", UI_MENU_LANGNAME_NL, 0}, {"ru", UI_MENU_LANGNAME_RU, 0},
        {"zh-Hans", UI_MENU_LANGNAME_ZH_HANS, 0}, {"zh-Hant", UI_MENU_LANGNAME_ZH_HANT, 0},
        {"ko", UI_MENU_LANGNAME_KO, 0},
    };
    int n = 0;
    for (size_t i = 0; i < sizeof all / sizeof *all && n < max; i++)
        if (game_version >= all[i].since) out[n++] = (UiGameLang){all[i].code, all[i].name};
    return n;
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
    // Traditional Chinese has no menu text: a Chinese reader gets the Simplified Chinese menu, not the English one.
    if (!strncmp(lang, "zh", 2)) return "zh-Hans";
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
