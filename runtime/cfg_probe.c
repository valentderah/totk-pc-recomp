// --probe-cfg: totk.ini parsing, the environment override and a missing file (cfg.c). Needs no game files.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "platform/platform.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { fails++; fprintf(stderr, "FAIL cfg: %s\n", what); } } while (0)
#define EQ(a, b) ((a) && !strcmp((a), (b)))

int probe_cfg(void) {
    char path[1024];
    if (!plat_exe_path(path, sizeof path)) return 1;
    char *cut = strrchr(path, '\\');
    if (!cut) return 1;
    strcpy(cut + 1, "cfg-probe.ini");
    static const char ini[] =
        "\xEF\xBB\xBF# comment\r\n"
        "; another comment\r\n"
        "[frame rate]\r\n"
        "  fps60 = 0  \r\n"
        "Realtime=0\r\n"
        "[Display]\r\n"
        "GPU_name = \"My GPU 1\"\r\n"
        "TOTK_Locale = de-DE\r\n"
        "lang =\r\n"
        "trace = a\r\n"
        "trace = b\r\n"
        "no_such_setting = 1\r\n"
        "no equals sign\r\n"
        "last = no newline at the end";
    _putenv_s("TOTK_LOCALE", "");   // an empty variable is how _putenv_s spells "remove" on Windows
    _putenv_s("TOTK_FPS60", "");
    CHECK(plat_file_write(path, ini, sizeof ini - 1), "write the probe ini");
    CHECK(cfg_load(path) == 1, "load the probe ini");
    CHECK(EQ(cfg_get("TOTK_FPS60"), "0"), "value read, whitespace trimmed, lower-case key");
    CHECK(EQ(cfg_get("TOTK_REALTIME"), "0"), "key without spaces, mixed case");
    CHECK(EQ(cfg_get("TOTK_GPU_NAME"), "My GPU 1"), "quoted value, section ignored");
    CHECK(EQ(cfg_get("TOTK_LOCALE"), "de-DE"), "TOTK_ prefix accepted in the file");
    CHECK(cfg_get("TOTK_LANG") == NULL, "empty value is unset");
    CHECK(EQ(cfg_get("TOTK_TRACE"), "b"), "later line wins");
    CHECK(cfg_get("TOTK_CLOTH_REALTIME") == NULL, "absent key is NULL");
    CHECK(cfg_get("TOTK_comment") == NULL, "comment is not a key");
    CHECK(EQ(cfg_get("TOTK_NO_SUCH_SETTING"), "1"), "unknown key is still readable");
    _putenv_s("TOTK_FPS60", "1");
    CHECK(EQ(cfg_get("TOTK_FPS60"), "1"), "environment overrides the file");
    _putenv_s("TOTK_FPS60", "");
    CHECK(EQ(cfg_get("TOTK_FPS60"), "0"), "file value returns when the variable is removed");
    plat_file_delete(path);
    CHECK(cfg_load(path) == 0, "missing file is not an error");
    CHECK(cfg_get("TOTK_FPS60") == NULL, "missing file leaves no keys");

    // cfg_set / cfg_save: only changed lines move, comments stay, a missing key goes under its section
    static const char before[] =
        "\xEF\xBB\xBF# my settings\r\n"
        "[frame rate]\r\n"
        "  FPS60 = 1\r\n"
        "; keep this\r\n"
        "\r\n"
        "[input]\r\n"
        "# Gamepad motion controls (0 = off) and vibration (0 = off).\r\n"
        "# motion = 1\r\n"
        "# rumble = 1\r\n"
        "rumble_curve = 0.6\r\n"
        "\r\n"
        "[files]\r\n"
        "romfs_mods = C:\\mods";
    static const char after[] =
        "\xEF\xBB\xBF# my settings\r\n"
        "[frame rate]\r\n"
        "  FPS60 = 0\r\n"
        "; keep this\r\n"
        "\r\n"
        "[input]\r\n"
        "# Gamepad motion controls (0 = off) and vibration (0 = off).\r\n"
        "# motion = 1\r\n"
        "motion = 0\r\n"
        "# rumble = 1\r\n"
        "rumble_curve = 0.6\r\n"
        "rumble_strength = 80\r\n"
        "\r\n"
        "[files]\r\n"
        "romfs_mods = C:\\mods\r\n"
        "\r\n"
        "[audio]\r\n"
        "volume = 40\r\n";
    _putenv_s("TOTK_MOTION", "");
    _putenv_s("TOTK_VOLUME", "");
    _putenv_s("TOTK_RUMBLE_STRENGTH", "");
    CHECK(plat_file_write(path, before, sizeof before - 1), "write the save ini");
    CHECK(cfg_load(path) == 1, "load the save ini");
    unsigned v0 = cfg_version();
    cfg_set("TOTK_FPS60", "0");
    cfg_set("TOTK_MOTION", "0");
    cfg_set("TOTK_RUMBLE_STRENGTH", "80");
    cfg_set("TOTK_VOLUME", "40");
    CHECK(cfg_version() == v0 + 4, "cfg_set bumps the version");
    CHECK(EQ(cfg_get("TOTK_FPS60"), "0") && EQ(cfg_get("TOTK_VOLUME"), "40"), "cfg_set values are read back at once");
    CHECK(cfg_save_to(path) == 1, "cfg_save_to writes");
    uint64_t got_n = 0;
    uint8_t *got = plat_file_read(path, &got_n);
    CHECK(got && got_n == sizeof after - 1 && !memcmp(got, after, got_n),
          "saved file: changed line replaced, comments kept, missing keys under their sections");
    if (got && (got_n != sizeof after - 1 || memcmp(got, after, got_n))) fprintf(stderr, "---- saved:\n%s\n----\n", got);
    free(got);
    CHECK(cfg_save_to(path) == 1, "a save with nothing changed");
    got = plat_file_read(path, &got_n);
    CHECK(got && got_n == sizeof after - 1 && !memcmp(got, after, got_n), "a save with nothing changed keeps the file");
    free(got);
    CHECK(cfg_load(path) == 1 && EQ(cfg_get("TOTK_FPS60"), "0") && EQ(cfg_get("TOTK_MOTION"), "0") &&
          EQ(cfg_get("TOTK_RUMBLE_STRENGTH"), "80") && EQ(cfg_get("TOTK_VOLUME"), "40") &&
          EQ(cfg_get("TOTK_ROMFS_MODS"), "C:\\mods"), "the saved file reads back");
    const char *held = cfg_get("TOTK_VOLUME");
    _putenv_s("TOTK_VOLUME", "70");
    cfg_set("TOTK_VOLUME", "90");
    CHECK(cfg_from_env("TOTK_VOLUME") && EQ(cfg_get("TOTK_VOLUME"), "70"), "environment wins over cfg_set");
    CHECK(EQ(held, "40"), "a pointer cfg_get handed out stays valid after cfg_set");
    _putenv_s("TOTK_VOLUME", "");
    CHECK(!cfg_from_env("TOTK_VOLUME") && EQ(cfg_get("TOTK_VOLUME"), "90"), "cfg_set value returns without the variable");
    plat_file_delete(path);
    CHECK(cfg_save_to(path) == 1, "cfg_save_to creates a missing file");
    got = plat_file_read(path, &got_n);
    CHECK(got && !strcmp((const char *)got, "[audio]\nvolume = 90\n"), "new file: the section and the key");
    free(got);

    // Defaults table: the menu's "default" marks and Reset
    const CfgMeta *m = cfg_meta("FPS60");
    CHECK(m && EQ(m->def, "1") && m->live, "meta: fps60 defaults to 1 and is live");
    m = cfg_meta("ASYNC_PREEMPT");
    CHECK(m && EQ(m->values, "0|1|2") && !m->live, "meta: async_preempt takes 0|1|2 and needs a restart");
    m = cfg_meta("totk_vk_present");
    CHECK(m && EQ(m->key, "VK_PRESENT") && EQ(m->values, "mailbox|fifo|immediate") && m->live,
          "meta: prefix and case are accepted");
    m = cfg_meta("RUMBLE_STRENGTH");
    CHECK(m && EQ(m->def, "130") && EQ(m->values, "0..300"), "meta: rumble strength 130 in 0..300");
    m = cfg_meta("PERF_OVERLAY");
    CHECK(m && EQ(m->def, "off") && EQ(m->values, "off|fps|full") && m->live, "meta: perf_overlay is live, off by default");
    CHECK(cfg_meta("BIND_A") == NULL && cfg_meta("FPS_COUNTER") == NULL && cfg_meta("NO_SUCH") == NULL,
          "meta: keys the menu does not show are NULL");

    // cfg_reset: the key's active lines leave totk.ini, the commented example stays
    static const char rbase[] = "[audio]\n# volume = 100\n[input]\nmotion = 0\n";
    static const char *const vol[] = {"VOLUME"};
    _putenv_s("TOTK_VOLUME", "");
    CHECK(plat_file_write(path, rbase, sizeof rbase - 1), "write the reset ini");
    CHECK(cfg_load(path) == 1, "load the reset ini");
    cfg_set("TOTK_VOLUME", "40");
    CHECK(cfg_save_to(path) == 1 && cfg_last_save_ok(), "reset: volume saved");
    got = plat_file_read(path, &got_n);
    CHECK(got && strstr((const char *)got, "\nvolume = 40\n"), "reset: the active line is in the file");
    free(got);
    unsigned v1 = cfg_version();
    CHECK(cfg_reset(vol, 1) == 1, "reset: one key reset");
    CHECK(cfg_version() > v1 && cfg_get("TOTK_VOLUME") == NULL, "reset: the value is gone in memory, the version moved");
    CHECK(cfg_reset(vol, 1) == 0, "reset: a key already reset counts 0");
    CHECK(cfg_save_to(path) == 1, "reset: save");
    got = plat_file_read(path, &got_n);
    CHECK(got && got_n == sizeof rbase - 1 && !memcmp(got, rbase, got_n) && strstr((const char *)got, "# volume = 100"),
          "reset: no active volume line, the commented example and the other keys stay");
    if (got && (got_n != sizeof rbase - 1 || memcmp(got, rbase, got_n))) fprintf(stderr, "---- saved:\n%s\n----\n", got);
    free(got);
    cfg_set("TOTK_VOLUME", "55");
    CHECK(cfg_reset(vol, 1) == 1 && cfg_get("TOTK_VOLUME") == NULL, "reset: a value set after a save resets too");
    cfg_set("TOTK_VOLUME", "60");
    CHECK(cfg_save_to(path) == 1 && EQ(cfg_get("TOTK_VOLUME"), "60"), "reset then set before the save: the set wins");
    got = plat_file_read(path, &got_n);
    CHECK(got && strstr((const char *)got, "\nvolume = 60\n"), "reset then set: the new line is written");
    free(got);

    // an active line with an empty value has no entry in memory; Reset still removes it from the file
    static const char rempty[] = "[audio]\n# volume = 100\nvolume =\n[input]\nmotion = 0\n";
    CHECK(plat_file_write(path, rempty, sizeof rempty - 1) && cfg_load(path) == 1 && cfg_get("TOTK_VOLUME") == NULL,
          "reset: load an ini with an empty value");
    CHECK(cfg_reset(vol, 1) == 0 && cfg_save_to(path) == 1, "reset: an empty-valued key saves");
    got = plat_file_read(path, &got_n);
    CHECK(got && got_n == sizeof rbase - 1 && !memcmp(got, rbase, got_n), "reset: the empty-valued line is gone");
    free(got);
    cfg_set("TOTK_VOLUME", "60");
    CHECK(cfg_save_to(path) == 1, "reset: volume 60 again for the environment case");

    // a key the environment sets is skipped and keeps its file line
    CHECK(cfg_load(path) == 1 && EQ(cfg_get("TOTK_VOLUME"), "60"), "reset: reload");
    _putenv_s("TOTK_VOLUME", "70");
    CHECK(cfg_reset(vol, 1) == 0, "reset: an environment-set key is skipped");
    _putenv_s("TOTK_VOLUME", "");
    CHECK(EQ(cfg_get("TOTK_VOLUME"), "60"), "reset: the skipped key keeps its value");
    CHECK(cfg_save_to(path) == 1, "reset: save after a skip");
    got = plat_file_read(path, &got_n);
    CHECK(got && strstr((const char *)got, "\nvolume = 60\n"), "reset: the skipped key keeps its file line");
    free(got);

    // a failed save: the call fails, cfg_last_save_ok says so, the value stays set and a later save works
    CHECK(cfg_last_save_ok() == 1, "save status: ok after a good save");
    cfg_set("TOTK_VOLUME", "20");
    char bad[1100];
    snprintf(bad, sizeof bad, "%s", path);
    strcpy(strrchr(bad, '\\') + 1, "no-such-dir\\cfg-probe.ini");
    CHECK(cfg_save_to(bad) == 0 && cfg_last_save_ok() == 0 && EQ(cfg_get("TOTK_VOLUME"), "20"),
          "save status: an unwritable path fails, the value stays");
    cfg_set("TOTK_VOLUME", "21");
    CHECK(plat_file_write(path, rbase, sizeof rbase - 1), "write the read-only ini");
    CHECK(plat_file_set_readonly(path, 1), "set the file read-only");
    CHECK(cfg_save_to(path) == 0 && cfg_last_save_ok() == 0 && EQ(cfg_get("TOTK_VOLUME"), "21"),
          "save status: a read-only file fails, cfg_get returns the new value");
    plat_file_set_readonly(path, 0);
    CHECK(cfg_save_to(path) == 1 && cfg_last_save_ok() == 1, "save status: the retry after the failure works");
    got = plat_file_read(path, &got_n);
    CHECK(got && strstr((const char *)got, "\nvolume = 21\n"), "save status: the retried save wrote the value");
    free(got);
    plat_file_delete(path);
    cfg_load(path);
    CHECK(rumble_motor(0, 0.6f, 1.3f) == 0 && rumble_motor(0.5f, 0.6f, 0) == 0, "rumble: zero amplitude or strength is silent");
    CHECK(rumble_motor(1, 0.6f, 3) == 65535 && rumble_motor(5, 1, 1) == 65535, "rumble: saturates at 65535");
    CHECK(rumble_motor(0.25f, 1, 1) > 16000 && rumble_motor(0.25f, 1, 1) < 16800, "rumble: curve 1, strength 100% is linear");
    CHECK(rumble_motor(0.1f, 0.6f, 1.3f) > rumble_motor(0.1f, 1, 1.3f), "rumble: curve lifts weak values");
    CHECK(rumble_motor(0.3f, 0.6f, 1.3f) > rumble_motor(0.3f, 0.6f, 1), "rumble: strength scales");
    // Keyboard and mouse bindings (input_map.c)
    uint8_t dn[KEY_COUNT] = {0};
    int32_t st[2];
    CHECK(input_map_buttons(dn) == 0, "keys: nothing held, no buttons");
    dn[KEY_MOUSE_L] = 1;
    CHECK(input_map_buttons(dn) == 1ull << B_Y, "keys: left mouse button is Y");
    dn[KEY_MOUSE_R] = dn[KEY_Z] = 1;
    CHECK(input_map_buttons(dn) == (1ull << B_Y | 1ull << B_ZL), "keys: right mouse button and Z both press ZL");
    memset(dn, 0, sizeof dn);
    dn[KEY_X] = dn[KEY_TAB] = dn[KEY_M] = dn[KEY_LCTRL] = dn[KEY_3] = 1;
    CHECK(input_map_buttons(dn) == (1ull << B_ZR | 1ull << B_PLUS | 1ull << B_MINUS | 1ull << B_STICK_L | 1ull << B_RIGHT),
          "keys: X ZR, Tab Plus, M Minus, Left Ctrl stick click, 3 D-pad right");
    memset(dn, 0, sizeof dn);
    dn[KEY_W] = 1;
    input_map_stick(dn, 0, st);
    CHECK(st[0] == 0 && st[1] == 32767, "keys: W is left stick up");
    dn[KEY_A] = 1;
    input_map_stick(dn, 0, st);
    CHECK(st[0] == -23170 && st[1] == 23170, "keys: W A is a diagonal on the unit circle");
    dn[KEY_W] = dn[KEY_A] = dn[KEY_S] = 1;
    dn[KEY_D] = 1;
    input_map_stick(dn, 0, st);
    CHECK(st[0] == 0 && st[1] == 0, "keys: opposite keys cancel");
    memset(dn, 0, sizeof dn);
    dn[KEY_LEFT] = 1;
    input_map_stick(dn, 1, st);
    CHECK(st[0] == -32767 && st[1] == 0, "keys: left arrow is right stick left");
    input_map_stick(dn, 0, st);
    CHECK(st[0] == 0 && st[1] == 0, "keys: arrows leave the left stick alone");
    input_mouse_stick(30, 0, 1.0 / 60, 1, st);
    CHECK(st[0] == 32767 * 1800 / 3000 && st[1] == 0, "mouse: 30 counts in 1/60 s is 60% right");
    input_mouse_stick(0, -10, 1.0 / 60, 1, st);
    CHECK(st[0] == 0 && st[1] > 0, "mouse: moving up is stick up");
    input_mouse_stick(30, 0, 1.0 / 30, 1, st);
    CHECK(st[0] == 32767 * 900 / 3000, "mouse: the same counts over twice the time are half the deflection");
    input_mouse_stick(15, 0, 1.0 / 60, 2, st);
    CHECK(st[0] == 32767 * 1800 / 3000, "mouse: doubling sensitivity halves the counts needed (15 counts at 2 = 30 at 1)");
    input_mouse_stick(1000, 1000, 1.0 / 60, 1, st);
    CHECK((int64_t)st[0] * st[0] + (int64_t)st[1] * st[1] <= 32768ll * 32768, "mouse: clamped to the unit circle");
    input_mouse_stick(30, 0, 1.0 / 60, 0, st);
    CHECK(st[0] == 0 && st[1] == 0, "mouse: sensitivity 0 is off");
    input_pad_stick(3000, -2000, 0.15f, st);
    CHECK(st[0] == 0 && st[1] == 0, "pad stick: an 11% drift inside a 15% dead zone reads centred");
    input_pad_stick(4000, -3000, 0, st);
    CHECK(st[0] == 4000 && st[1] == -3000, "pad stick: dead zone 0 passes the stick unchanged");
    input_pad_stick(32767, 0, 0.15f, st);
    CHECK(st[0] == 32767 && st[1] == 0, "pad stick: full deflection still reaches full");
    input_pad_stick(-32768, -32768, 0.15f, st);
    CHECK(st[0] < 0 && st[1] < 0 && st[0] >= -32767 && (int64_t)st[0] * st[0] + (int64_t)st[1] * st[1] <= 32768ll * 32768,
          "pad stick: a square gate's corner stays on the unit circle");
    input_pad_stick(0, 32767 / 2 + 1, 0.2f, st);
    CHECK(abs(st[1] - 32767 * 3 / 8) <= 3, "pad stick: half deflection past a 20% zone is (0.5 - 0.2) / 0.8 of full");
    _putenv_s("TOTK_STICK_DEADZONE", "12");
    _putenv_s("TOTK_LSTICK_DEADZONE", "");
    _putenv_s("TOTK_RSTICK_DEADZONE", "");
    CHECK(fabsf(input_pad_deadzone(0) - 0.12f) < 1e-6f && fabsf(input_pad_deadzone(1) - 0.12f) < 1e-6f,
          "pad dead zone: both sticks follow stick_deadzone while lstick/rstick are unset");
    _putenv_s("TOTK_RSTICK_DEADZONE", "20");
    CHECK(fabsf(input_pad_deadzone(0) - 0.12f) < 1e-6f && fabsf(input_pad_deadzone(1) - 0.20f) < 1e-6f,
          "pad dead zone: rstick_deadzone sets the right stick alone");
    _putenv_s("TOTK_LSTICK_DEADZONE", "80");
    CHECK(fabsf(input_pad_deadzone(0) - 0.5f) < 1e-6f, "pad dead zone: clamped to 50%");
    _putenv_s("TOTK_STICK_DEADZONE", "");
    _putenv_s("TOTK_LSTICK_DEADZONE", "");
    _putenv_s("TOTK_RSTICK_DEADZONE", "");
    CHECK(input_stick_percent(32767, 0) > 99.99 && input_stick_percent(32767, 0) < 100.01 &&
              input_stick_percent(0, 0) == 0 && fabs(input_stick_percent(3000, -4000) - 5000 * 100.0 / 32767) < 1e-9,
          "stick percent: an axis's end is 100%, the centre 0%, round in between");
    // Gamepad layouts (input_map.c)
    CHECK(input_pad_layout_pick("auto", 1) == PAD_LAYOUT_NINTENDO && input_pad_layout_pick(NULL, 0) == PAD_LAYOUT_XBOX,
          "pad layout: automatic is Nintendo for an east button labelled A, Xbox otherwise");
    CHECK(input_pad_layout_pick("Xbox", 1) == PAD_LAYOUT_XBOX && input_pad_layout_pick("nintendo", 0) == PAD_LAYOUT_NINTENDO,
          "pad layout: a set layout wins over the labels");
    uint8_t pd[PAD_COUNT] = {0};
    pd[PAD_SOUTH] = pd[PAD_NORTH] = 1;
    CHECK(input_map_pad_buttons(pd, PAD_LAYOUT_NINTENDO) == (1ull << B_B | 1ull << B_X) &&
          input_map_pad_buttons(pd, PAD_LAYOUT_XBOX) == (1ull << B_A | 1ull << B_Y),
          "pad layout: south and north are B and X by position, A and Y on Xbox");
    pd[PAD_SOUTH] = pd[PAD_NORTH] = 0;
    pd[PAD_START] = pd[PAD_ZR] = 1;
    CHECK(input_map_pad_buttons(pd, PAD_LAYOUT_XBOX) == (1ull << B_PLUS | 1ull << B_ZR), "pad layout: Xbox keeps the other buttons");
    CHECK(input_map_bind("A", "pad:west") && input_map_pad_buttons((uint8_t[PAD_COUNT]){[PAD_WEST] = 1}, PAD_LAYOUT_XBOX) ==
                                                 (1ull << B_A | 1ull << B_X),
          "pad layout: a bind line adds its physical button in every layout");
    CHECK(input_map_pad_buttons((uint8_t[PAD_COUNT]){[PAD_SOUTH] = 1}, PAD_LAYOUT_XBOX) == 0,
          "pad layout: a bind line replaces the layout's own button");
    input_map_reset();
    // Bind lines for stick directions and default parts; the remapping rows' edits (input_map.c)
    memset(dn, 0, sizeof dn);
    dn[KEY_I] = 1;
    CHECK(input_map_bind("LSTICK_UP", "key:i, pad:south") == 1, "bind: a stick direction takes keys, skips pad entries");
    input_map_stick(dn, 0, st);
    CHECK(st[1] == 32767, "bind: I moves forward after bind_LSTICK_UP = key:i");
    dn[KEY_I] = 0, dn[KEY_W] = 1;
    input_map_stick(dn, 0, st);
    CHECK(st[1] == 0, "bind: W no longer moves forward");
    CHECK(input_map_bind("A", "key:default, pad:north") == 1, "bind: key:default with a pad entry");
    memset(dn, 0, sizeof dn);
    dn[KEY_E] = 1;
    CHECK(input_map_buttons(dn) == 1ull << B_A &&
              input_map_pad_buttons((uint8_t[PAD_COUNT]){[PAD_NORTH] = 1}, PAD_LAYOUT_XBOX) == (1ull << B_A | 1ull << B_Y),
          "bind: key:default keeps E, pad:north adds the top button in every layout");
    CHECK(input_map_bind("B", "key:j, pad:default") == 1 &&
              input_map_pad_buttons((uint8_t[PAD_COUNT]){[PAD_EAST] = 1}, PAD_LAYOUT_XBOX) == 1ull << B_B &&
              input_map_pad_buttons((uint8_t[PAD_COUNT]){[PAD_SOUTH] = 1}, PAD_LAYOUT_NINTENDO) == 1ull << B_B,
          "bind: pad:default keeps each layout's own button");
    input_map_reset();
    InputMap mp = input_map;
    char line[256];
    input_map_line(&mp, B_ZR, line, sizeof line);
    CHECK(!line[0], "edit: an action at its defaults needs no line");
    CHECK(input_map_set_key(&mp, B_A, 0, KEY_F) == B_ZR, "edit: F taken from ZR");
    input_map_line(&mp, B_A, line, sizeof line);
    CHECK(!strcmp(line, "key:f, pad:default"), "edit: A's line is key:f, pad:default");
    input_map_line(&mp, B_ZR, line, sizeof line);
    CHECK(!strcmp(line, "key:x, pad:default"), "edit: ZR keeps X");
    CHECK(input_map_set_key(&mp, B_ZL, 1, KEY_NONE) == -1, "edit: clearing a key takes from no one");
    uint8_t ks[8];
    CHECK(input_map_keys_of(&mp, B_ZL, ks, 8) == 1 && ks[0] == KEY_MOUSE_R, "edit: ZL keeps its first key only");
    mp = input_map;
    CHECK(input_map_set_pad(&mp, B_X, PAD_LAYOUT_XBOX, PAD_SOUTH) == B_A, "edit: south swaps X with A (Xbox layout)");
    uint8_t ps[8];
    CHECK(input_map_pads_of(&mp, B_A, PAD_LAYOUT_XBOX, ps, 8) == 1 && ps[0] == PAD_WEST, "edit: A takes X's west button");
    CHECK(input_map_pads_of(&mp, B_A, PAD_LAYOUT_NINTENDO, ps, 8) == 1 && ps[0] == PAD_WEST &&
              input_map_pads_of(&mp, B_B, PAD_LAYOUT_NINTENDO, ps, 8) == 1 && ps[0] == PAD_EAST,
          "edit: the face buttons are pinned in every layout after a gamepad edit");
    input_map_line(&mp, B_X, line, sizeof line);
    CHECK(!strcmp(line, "key:default, pad:south"), "edit: X's line is key:default, pad:south");
    input_map_set_pad(&mp, B_X, PAD_LAYOUT_XBOX, PAD_WEST);   // and back
    input_map_line(&mp, B_A, line, sizeof line);
    CHECK(!line[0] && input_map_pads_of(&mp, B_A, PAD_LAYOUT_NINTENDO, ps, 8) == 1 && ps[0] == PAD_EAST,
          "edit: swapping back restores every layout's defaults");
    mp = input_map;
    input_map_set_key(&mp, B_SL_UP, 0, KEY_NONE);
    input_map_line(&mp, B_SL_UP, line, sizeof line);
    CHECK(!strcmp(line, "none"), "edit: a stick direction without keys is none");
    // defaults live in the CfgMeta table: cfg_value and the typed accessors fall back to it
    cfg_load("cfg-probe-missing.ini");
    _putenv_s("TOTK_FPS60", "");
    _putenv_s("TOTK_MOUSE_SENS", "");
    _putenv_s("TOTK_FREE_CORES", "");
    CHECK(cfg_bool("TOTK_FPS60") == 1, "default: unset FPS60 is on");
    CHECK(EQ(cfg_value("TOTK_VK_PRESENT"), "mailbox") && cfg_int("TOTK_SPLIT_DRAWS") == 64, "default: text and integer");
    _putenv_s("TOTK_MOUSE_SENS", "2.5");
    CHECK(cfg_double("TOTK_MOUSE_SENS") == 2.5, "double: the environment value");
    _putenv_s("TOTK_MOUSE_SENS", "fast");
    CHECK(cfg_double("TOTK_MOUSE_SENS") == 1.0, "double: a value that does not parse gives the default");
    _putenv_s("TOTK_MOUSE_SENS", "");
    cfg_set("TOTK_MOUSE_SENS", "3");
    CHECK(cfg_double("TOTK_MOUSE_SENS") == 3.0 && cfg_int("TOTK_MOUSE_SENS") == 3, "a cfg_set value beats the default");
    CHECK(cfg_bool("TOTK_FREE_CORES") == 0, "free_cores unset: off");
    _putenv_s("TOTK_FREE_CORES", "0");
    CHECK(cfg_bool("TOTK_FREE_CORES") == 0, "free_cores 0 gives the same as unset");
    _putenv_s("TOTK_FREE_CORES", "1");
    CHECK(cfg_bool("TOTK_FREE_CORES") == 1, "free_cores 1: on");
    _putenv_s("TOTK_FREE_CORES", "");
    cfg_load("cfg-probe-missing.ini");
    int listed = 0;
    for (int i = 0; cfg_meta_at(i); i++) {
        const CfgMeta *m = cfg_meta_at(i);
        if (!*m->def) continue;
        listed++;
        char name[96];
        snprintf(name, sizeof name, "TOTK_%s", m->key);
        _putenv_s(name, "");
        CHECK(EQ(cfg_value(name), m->def), "cfg_value returns the table default");
        int ok = 0;
        if (!m->values) ok = 1;
        else if (strstr(m->values, "..")) {
            long lo = strtol(m->values, NULL, 10), hi = strtol(strstr(m->values, "..") + 2, NULL, 10);
            double d = strtod(m->def, NULL);
            ok = d >= (double)lo && d <= (double)hi;
        } else {
            size_t dl = strlen(m->def);
            for (const char *v = m->values; *v;) {
                size_t n = strcspn(v, "|");
                ok |= n == dl && !strncmp(v, m->def, n);
                v += n + (v[n] == '|');
            }
        }
        if (!ok) fprintf(stderr, "FAIL cfg: default '%s' of %s is not one of its values '%s'\n", m->def, m->key, m->values), fails++;
    }
    CHECK(listed >= 30, "the defaults table was walked");
    printf("probe-cfg: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
