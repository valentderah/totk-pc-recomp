/* Game-data-free regression test. Include input.c to inspect script/recording
 * state without adding production test APIs. Unexpected merged-input calls abort.
 * Platform: Windows x86-64. Requires a runtime-supported recent Clang (the CPU
 * headers use SIMD builtins), SDL3 headers/import library/DLL, Vulkan/VMA headers,
 * and the Windows platform libraries. No game data or generated game C is needed.
 * Run without TOTK_* environment overrides. SDL physical-device backends are
 * disabled in this process; only virtual controllers are used. No ini is read or
 * saved. PASS is exit 0; a failed CHECK or unexpected path exits nonzero.
 *
 * Build/run from the repository root in PowerShell with clang on PATH. Set $deps
 * to the dependency prefix containing include/SDL3, include/vulkan, include/vma
 * and lib/libSDL3.dll.a. Put SDL3.dll and compiler runtime DLLs on PATH to run.
 * The default output is ignored build/test; an explicit temporary directory is OK.
 *
 * $deps = "runtime/deps"
 * $out = "build/test/input"
 * New-Item -ItemType Directory -Force $out | Out-Null
 * clang -std=gnu11 -O1 -g -ffunction-sections -fdata-sections -D_WIN32_WINNT=0x0A00 -I runtime -I "$deps/include" -I "$deps/include/vma" tests/input_overlay_probe.c runtime/input_map.c runtime/cfg.c runtime/platform/win32.c runtime/platform/common.c '-Wl,--gc-sections' -L "$deps/lib" -lSDL3 -lpsapi -lbcrypt -lole32 -lshell32 -luuid -lwinmm -o "$out/input_overlay_probe.exe"
 * & "$out/input_overlay_probe.exe"
 */
#include <assert.h>
#include <stdarg.h>
#include "../runtime/input.c"

void rt_log(const char *fmt, ...) {
    va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); fputc('\n',stderr); va_end(ap);
}
void rt_fatal(const char *fmt, ...) {
    va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); abort();
}
// Fail immediately if the snapshot reaches these paths instead of hiding their side effects.
int ikb_foreground(void) { abort(); }
int ikb_down(int key) { (void)key; abort(); }
int ikb_escape(void) { abort(); }
int ikb_captured(void) { abort(); }
void ikb_mouse_take(int32_t *x, int32_t *y) { (void)x; (void)y; abort(); }
float cam_speed_applied(void) { abort(); }
uint64_t frame_step_ticks(void) { abort(); }
int64_t frame_step_world_us(void) { abort(); }
int menu_is_open(void) { abort(); }
void menu_toggle(void) { abort(); }
void menu_perf_cycle(void) { abort(); }
MotionFusion *motion_global(void) { abort(); }
void motion_feed(MotionFusion *m, const double a[3], const double g[3], uint64_t ns) { (void)m; (void)a; (void)g; (void)ns; abort(); }
uint64_t kern_now_ns(void) { abort(); }
GfxStats gfx_counters;
void ikb_cursor_update(void) {}
void ikb_menu_closed(void) {}
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); exit(1); } } while (0)

static SDL_JoystickID attach(SDL_Joystick **joy) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.name = "TotK overlay regression";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    CHECK(id);
    *joy = SDL_OpenJoystick(id); CHECK(*joy);
    CHECK(SDL_SetJoystickVirtualAxis(*joy,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-32768));
    CHECK(SDL_SetJoystickVirtualAxis(*joy,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768));
    SDL_UpdateGamepads();
    return id;
}
static void sample(InputOverlayState *s) { CHECK(input_overlay_state(s)); }
static void worker(void *arg) {
    (void)arg;
    for (int i=0;i<500;i++) {
        InputOverlayState s; sample(&s);
        CHECK(s.connected && s.raw[1] == 32768);
    }
}
int main(void) {
    // Disable physical device backends and create only this test's SDL virtual gamepad.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI,"0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT,"0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT,"0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED,"0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI,"0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ROG_CHAKRAM,"0");
    CHECK(SDL_Init(SDL_INIT_GAMEPAD));
    cfg_set("TOTK_PAD_LAYOUT","nintendo");
    cfg_set("TOTK_STICK_DEADZONE","20");
    cfg_set("TOTK_INPUT_SCRIPT","must-not-open-overlay-script.txt");
    cfg_set("TOTK_INPUT_RECORD","must-not-create-overlay-record.txt");
    SDL_Joystick *joy;
    SDL_JoystickID id = attach(&joy);
    InputOverlayState s;
    sample(&s);
    CHECK(s.layout == PAD_LAYOUT_NINTENDO && s.buttons == 0);
    CHECK(!s.motion_available && !s.motion_known);
    CHECK(s.deadzone[0] == 20 && s.deadzone[1] == 20);
    CHECK(s.trigger[0] == 0 && s.trigger[1] == 0);
    CHECK(SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_SOUTH,true));
    sample(&s); CHECK(s.buttons == (1ull << B_B));
    CHECK(s.labels[0] == input_pad_label(PAD_SOUTH));
    int label_south = s.labels[0];
    cfg_set("TOTK_PAD_LAYOUT","xbox");
    sample(&s); CHECK(s.layout == PAD_LAYOUT_XBOX && s.buttons == (1ull << B_A));
    CHECK(s.labels[0] == label_south); // Changing the layout does not rewrite hardware labels.
    cfg_set("TOTK_BIND_A","pad:west");
    cfg_set("TOTK_BIND_Y","pad:south");
    sample(&s); CHECK(s.buttons == (1ull << B_Y));
    CHECK(SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_SOUTH,false));
    CHECK(SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_WEST,true));
    sample(&s); CHECK((s.buttons & (1ull << B_A)) != 0);
    CHECK(SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_WEST,false));
    const struct { SDL_GamepadButton button; int bit; } buttons[] = {
        {SDL_GAMEPAD_BUTTON_BACK,B_MINUS},{SDL_GAMEPAD_BUTTON_START,B_PLUS},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,B_L},{SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,B_R},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT,B_LEFT},{SDL_GAMEPAD_BUTTON_DPAD_UP,B_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,B_RIGHT},{SDL_GAMEPAD_BUTTON_DPAD_DOWN,B_DOWN},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK,B_STICK_L},{SDL_GAMEPAD_BUTTON_RIGHT_STICK,B_STICK_R}
    };
    for (unsigned i=0;i<sizeof buttons/sizeof *buttons;i++) {
        CHECK(SDL_SetJoystickVirtualButton(joy,buttons[i].button,true));
        sample(&s); CHECK(s.buttons == (1ull << buttons[i].bit));
        CHECK(SDL_SetJoystickVirtualButton(joy,buttons[i].button,false));
    }
    CHECK(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_LEFTX,3000));
    sample(&s); CHECK(s.raw[0] == 3000 && s.out[0] == 0 && s.out[1] == 0);
    CHECK(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_LEFTX,16384));
    sample(&s); CHECK(s.out[0] > 12000 && s.out[0] < 12500);
    CHECK(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_LEFTX,0));
    CHECK(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_LEFTY,-32768));
    CHECK(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_RIGHTX,-32768));
    CHECK(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_RIGHTY,32767));
    sample(&s); CHECK(s.raw[1] == 32768 && s.out[1] == 32767);
    CHECK(s.raw[2] == -32768 && s.raw[3] == -32767);
    CHECK(s.out[2] >= -32767 && s.out[3] >= -32767);
    for (int v=-32768;v<=32767;v+=8191) {
        CHECK(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,(Sint16)v));
        sample(&s);
        CHECK(s.trigger[0] >= 0 && s.trigger[0] <= 32767);
        CHECK(!!(s.buttons & (1ull << B_ZL)) == (s.trigger[0] > TRIGGER_ON));
    }
    CHECK(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-16384));
    CHECK(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767));
    sample(&s); CHECK(s.trigger[0] > 0 && s.trigger[0] < 16384 && s.trigger[1] == 32767);
    CHECK(!(s.buttons & (1ull << B_ZL)) && (s.buttons & (1ull << B_ZR)));
    // Face buttons can drive ZL/ZR while the physical triggers drive other actions.
    cfg_set("TOTK_BIND_ZL","pad:west"); cfg_set("TOTK_BIND_ZR","pad:north");
    cfg_set("TOTK_BIND_A","pad:zl"); cfg_set("TOTK_BIND_B","pad:zr");
    CHECK(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-32768));
    CHECK(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768));
    const SDL_GamepadButton sources[] = {SDL_GAMEPAD_BUTTON_WEST,SDL_GAMEPAD_BUTTON_NORTH};
    for (int i=0;i<2;i++) {
        CHECK(SDL_SetJoystickVirtualButton(joy,sources[i],true)); sample(&s);
        CHECK(s.buttons & (1ull << (i ? B_ZR : B_ZL)));
        CHECK(!(s.buttons & (1ull << B_A | 1ull << B_B)));
        CHECK(s.trigger[0] == 0 && s.trigger[1] == 0);
        CHECK(SDL_SetJoystickVirtualButton(joy,sources[i],false));
    }
    const int levels[] = {-16384,16384};
    for (unsigned i=0;i<sizeof levels/sizeof *levels;i++) {
        CHECK(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,(Sint16)levels[i]));
        CHECK(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,(Sint16)levels[i]));
        sample(&s);
        CHECK(!(s.buttons & (1ull << B_ZL | 1ull << B_ZR)));
        CHECK((s.trigger[0] > TRIGGER_ON) == (i != 0));
        CHECK((s.trigger[1] > TRIGGER_ON) == (i != 0));
        CHECK(!!(s.buttons & (1ull << B_A)) == (s.trigger[0] > TRIGGER_ON));
        CHECK(!!(s.buttons & (1ull << B_B)) == (s.trigger[1] > TRIGGER_ON));
    }
    cfg_set("TOTK_BIND_ZL","pad:default"); cfg_set("TOTK_BIND_ZR","pad:default");
    cfg_set("TOTK_BIND_A","pad:default"); cfg_set("TOTK_BIND_B","pad:default");
    puts("PASS: remapped trigger actions, physical axes and trigger-to-other-action thresholds are independent");
    // The overlay neither applies menu blocking nor consumes chords; simultaneous presses remain visible.
    input_menu(1); input_block(1);
    CHECK(SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_BACK,true));
    CHECK(SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_START,true));
    CHECK(SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_LEFT_STICK,true));
    CHECK(SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_RIGHT_STICK,true));
    sample(&s);
    uint64_t combo = (1ull<<B_MINUS)|(1ull<<B_PLUS)|(1ull<<B_STICK_L)|(1ull<<B_STICK_R);
    CHECK((s.buttons & combo) == combo);
    PlatThread *a = plat_thread_new(worker,NULL,"overlay-test-a"), *b = plat_thread_new(worker,NULL,"overlay-test-b");
    CHECK(a && b); plat_thread_join(a); plat_thread_join(b);
    CHECK(script_state == 0 && script_ev == NULL && rec_state == 0 && rec == NULL);
    SDL_CloseJoystick(joy); CHECK(SDL_DetachVirtualJoystick(id));
    CHECK(!input_overlay_state(&s));
    CHECK(!s.connected && !s.buttons && s.layout == -1 && !s.motion_known);
    CHECK(s.raw[0] == 0 && s.trigger[1] == 0 && s.labels[0] == PAD_LABEL_NONE);
    id = attach(&joy);
    SDL_Delay(1100); sample(&s); CHECK(s.connected);
    CHECK(script_state == 0 && rec_state == 0);
    plat_mutex_lock(&lock);
    if (pad) { SDL_CloseGamepad(pad); pad = NULL; }
    plat_mutex_unlock(&lock);
    SDL_CloseJoystick(joy); CHECK(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    puts("PASS: mappings, button bits, labels, dead zones, Y endpoints, triggers, unknown gyro, menu independence, two threads, no script/recording side effects, hotplug");
    return 0;
}
