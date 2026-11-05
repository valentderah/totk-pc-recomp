// Host input sampling (runtime/input.c, with input_win.c or input_sdl.c): SDL3 gamepad and the keyboard as one Switch npad.
#pragma once
#include <stdint.h>

// Bits of libnx HidNpadButton (A=0 B=1 X=2 Y=3 StickL=4 StickR=5 L=6 R=7 ZL=8 ZR=9 Plus=10 Minus=11 Left=12 Up=13 Right=14
// Down=15) and the stick directions (StickLLeft..Down 16-19, StickRLeft..Down 20-23, derived from the sticks).
// Sticks are -32767..32767, up is +y. TOTK_INPUT=none reports no input. Callable from any thread.
void input_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
// 1 when keyboard and mouse are the device the player used last, 0 when the gamepad is: what is shown for pointing
// (menu_cursor.c shows the pointer in the game's menus only for the keyboard and mouse)
int input_kbm_last(void);
// Direct mouse input is consumed by the game's camera-input update, after its stick response. While that update
// accepts camera input, the HID right stick contains only the controller and keyboard. Selectors route mouse input
// separately: a persistent radial cursor for the ability wheel, discrete steps for horizontal quick lists.
int input_direct_mouse_active(void);
int input_direct_mouse_enabled(void);
// A radial selector uses a persistent virtual cursor while its opening button is held.
void input_direct_mouse_radial(int32_t dx, int32_t dy, float sensitivity, float cursor[2], int32_t out[2]);
// Horizontal quick selectors receive short, separated steps instead of a latched radial-cursor direction.
typedef struct { float distance; uint64_t pulse_until, neutral_until; int direction; } InputMouseRow;
void input_direct_mouse_row(int32_t dx, float sensitivity, uint64_t now, InputMouseRow *row, int32_t out[2]);
int input_direct_mouse_selector_active(void);
// Consume direct mouse counts, including one-shot MOUSE events from an input script. enabled=0 discards scripted
// counts while leaving the real mouse for menu input. Live counts require cursor capture and foreground focus.
void input_direct_mouse_sample(int enabled, int32_t *dx, int32_t *dy);
// The buttons (B_* bits) of the last poll, keys and gamepad merged; any thread (mouse_direct.c: ZR for the bow's scale)
uint64_t input_buttons_last(void);
// mouse_direct.c: the right stick's side a mouse shake gives while Ultrahand holds an object (-1 / +1), 0 while the
// mouse does not shake; input.c puts it on the stick
int input_direct_mouse_shake(void);
// menu_cursor.c: the L ability wheel or a D-pad strip has the focus: 1 / 0, -1 with the menu cursor off (not read)
int menu_cursor_shortcut(void);
// menu_cursor.c: the pointer in the game's menus (1.4.3). poll: once per pad read, adds a click's A or B; pointer: a
// menu has the pointer, so the mouse camera's capture is released
void menu_cursor_poll(uint64_t *buttons);
int menu_cursor_pointer(void);
int menu_cursor_map(void);   // the Purah Pad's map is open: the mouse's stick is the left one (its cursor)
// The same for host UI (the text entry): never blocked. input_block(1) while a text entry is open: input_poll then
// reports nothing; input_block(0) when it closes: nothing until all buttons and stick directions are released.
void input_poll_ui(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
void input_block(int on);
// The in-game menu (ui/menu.h): input_menu(1) when it opens (input_poll reports nothing, the cursor is released), (0) when
// it closes (nothing until every button and stick direction is released). Independent of input_block.
void input_menu(int open);
// The gamepad alone, for the menu: Switch button bits and the left stick (up +y). 0 without a connected gamepad.
// Minus+Plus combo and the neutral pad while the menu is open (input_map.c, window-system free): *held is the caller's memory
// of the combo, sticks are lx, ly, rx, ry. Returns 1 in the poll where the combo goes down (toggle the menu once).
int input_menu_gate(int *held, int menu_open, uint64_t *buttons, int32_t sticks[4]);
int input_overlay_gate(int *held, uint64_t *buttons);   // L3+R3: 1 in the poll both go down
int input_pad_read(uint64_t *buttons, int32_t *lx, int32_t *ly);
// The gamepad layout in effect (PAD_LAYOUT_*, input_map.h; TOTK_PAD_LAYOUT, live); -1 without a gamepad. Any thread.
int input_pad_layout(void);
// How far the open gamepad's left (pct[0]) and right (pct[1]) sticks are from the centre now, in percent of full
// deflection, before the dead zone: the menu shows it on the dead zone rows, so a drifting stick's resting value can be
// read off. 0 (and both 0) without a connected gamepad. Any thread.
int input_pad_stick_percent(double pct[2]);
// The label on a face button of the open gamepad (input_map.h PAD_SOUTH..PAD_NORTH, as SDL reports it); PAD_LABEL_NONE for another
// button, an unknown label or no gamepad. Any thread.
enum { PAD_LABEL_NONE, PAD_LABEL_A, PAD_LABEL_B, PAD_LABEL_X, PAD_LABEL_Y, PAD_LABEL_CROSS, PAD_LABEL_CIRCLE,
       PAD_LABEL_SQUARE, PAD_LABEL_TRIANGLE };
int input_pad_label(int pad);
// The menu's remapping rows (Controls tab). input_map_snapshot copies the tables in effect (after any changed TOTK_BIND_* line).
// input_capture polls the keys, the mouse buttons and the gamepad for a press: reset 1 takes what is held now as the
// start (a held key counts once released and pressed again) and returns NONE; then it returns KEY or PAD with *id the
// key or pad id (input_map.h) pressed since the last call, or CANCEL for Esc. Keys count only while the game window is in front.
enum { INPUT_CAPTURE_NONE, INPUT_CAPTURE_KEY, INPUT_CAPTURE_PAD, INPUT_CAPTURE_CANCEL };
struct InputMap;   // input_map.h (not included here: swkbd_win.c has button names of its own)
void input_map_snapshot(struct InputMap *m);
int input_capture(int reset, int *id);
// The character on a punctuation key (KEY_MINUS..KEY_GRAVE) in the keyboard layout in use, 0 for another key.
uint32_t input_key_char(int key);
// F8, the flight recorder's key (no KEY_ id), held while the game window is in front.
int input_flight_key(void);

// Mouse camera and cursor (input_win.c, Win32 window thread). The window procedure calls input_window_msg first for raw
// mouse input, cursor hiding and confinement; it returns 1 with *res set when the message is handled. input_cursor_suspend(1)
// releases the cursor around an F11 window change, (0) captures it again once the client area has its new size.
void input_cursor_suspend(int on);
int input_cursor_released(void);   // Esc or a non-client activation released the cursor (probe)

// Rumble: SDL_RumbleGamepad on the open gamepad, low (heavy, left) and high (light, right) motor 0..65535 for ms
// milliseconds. 1 if a gamepad with rumble took it. Callable from any thread.
int input_rumble(uint16_t low, uint16_t high, uint32_t ms);
// Motor value for a normalised amplitude: pow(a, curve) * strength, saturated to 0..65535. Pure.
uint16_t rumble_motor(float a, float curve, float strength);
// TOTK_RUMBLE_TRIGGERS=1: input_rumble also drives the impulse triggers (Xbox) with the high motor's value.
int input_rumble_triggers_enabled(void);

// Motion (TOTK_MOTION=0 turns it off, live). input_motion_start starts the sensor thread once: every 4 ms it reads the
// gamepad's gyro and accelerometer (when it has them) and feeds motion_global(); with TOTK_MOUSE_GYRO=1 it feeds the
// mouse's motion instead, every 5 ms (motion_mouse.c). Callable from any thread.
void input_motion_start(void);
// TOTK_MOTION is not "0" (a live setting: read again when cfg_version changes). Off: the sensor reads at rest.
int input_motion_enabled(void);
// TOTK_MOUSE_GYRO is "1" (live): the mouse drives the six-axis sensor and no longer the right stick.
int input_mouse_gyro_enabled(void);
// The game read the six-axis sensor (ov_input.c six_fill, live states): the mouse source spreads its motion over at
// least the game's read interval. Any thread.
void input_motion_read_note(uint64_t now_ns);
// SDL sensor values (accelerometer m/s^2, gyro rad/s, SDL gamepad axes) to the Switch controller frame motion_feed
// takes (acceleration in G as gravity reads, rotation rad/s). The mapping is documented in input_win.c.
void input_motion_axes(const float sdl_acc[3], const float sdl_gyro[3], double acc_g[3], double gyro[3]);

// motion_fusion.c: nn::hid::SixAxisSensorState and the sensor fusion that produces it.
typedef struct {
    int64_t delta_ns, sampling;
    float acc[3], gyro[3], angle[3], dir[9];   // G, revolutions/s, revolutions, controller axes in the reference frame
    uint32_t attributes, reserved;             // bit 0 connected
} MotionState;
_Static_assert(sizeof(MotionState) == 0x60, "SixAxisSensorState ABI");
typedef struct MotionFusion MotionFusion;
MotionFusion *motion_new(void);
MotionFusion *motion_global(void);   // the controller's, fed by input.c (the gamepad's sensors or the mouse)
void motion_feed(MotionFusion *m, const double acc_g[3], const double gyro_rad_s[3], uint64_t now_ns);
// A state whose direction (MotionState.dir layout, unfused) the caller knows, the angle integrated over dt_s. Dropped,
// returning 0, when a motion_restart came after the caller read `restarts` (motion_restarts): its pose starts over first.
int motion_feed_pose(MotionFusion *m, const double acc_g[3], const double gyro_rad_s[3], const double dir[9], double dt_s,
                     uint64_t now_ns, unsigned restarts);
unsigned motion_restarts(MotionFusion *m);   // motion_restart calls so far
int motion_read(MotionFusion *m, MotionState *out, int max, uint64_t now_ns);   // newest first; 0 = none in 200 ms
void motion_restart(MotionFusion *m);   // angle and direction start over at the next sample

// motion_mouse.c: the mouse as a held controller's motion. Pure: the caller owns the struct and the timing.
#define MOUSE_GYRO_TICK_NS 5000000ull   // a sample every 5 ms, the Switch's six-axis rate
enum {
    MOUSE_GYRO_WINDOW_MAX = 11,   // ticks a count can be spread over
    MOUSE_GYRO_CATCHUP = 10,      // samples a late wake makes up for; a longer stall starts the schedule over
};
// The samples due at `now` on the fixed MOUSE_GYRO_TICK_NS schedule *next (0: none yet): how many, the first one's time
// in *first, and *next moved past them. A wake late by up to MOUSE_GYRO_CATCHUP ticks makes up the samples it missed,
// so the game's count of new samples follows the time that passed; the first wake, or one after a longer stall, starts
// the schedule at now with one sample; an early wake has none due.
int mouse_gyro_due(uint64_t *next, uint64_t now, uint64_t *first);
typedef struct {
    double yaw, pitch;                      // radians: turned since the last init (rate only); the tilt above flat
    double due[2][MOUSE_GYRO_WINDOW_MAX];   // yaw, pitch still to turn at each coming tick (ring at head)
    int head;
} MouseGyro;
#define MOUSE_GYRO_REST_PITCH 0.2617993877991494   // 15 degrees: the tilt a held controller rests at
#define MOUSE_GYRO_PITCH_MAX 1.3962634015954636    // 80 degrees up or down from flat at most
void mouse_gyro_init(MouseGyro *g);   // at rest: nothing turned, the rest tilt, nothing due
// One tick of dt_s seconds: dx, dy raw mouse counts since the last tick (right and down positive), dots counts per
// full turn, each tick's counts spread evenly over the next `window` ticks (the game's frame; 1 to
// MOUSE_GYRO_WINDOW_MAX). Up and down tilt the controller, left and right are a yaw rate with no sideways turn of its
// direction. Writes the state in the Switch controller frame: acceleration in G as gravity reads, rotation rad/s, dir
// as MotionState.dir.
void mouse_gyro_step(MouseGyro *g, int32_t dx, int32_t dy, double dt_s, double dots, int window, double acc_g[3],
                     double gyro_rad_s[3], double dir[9]);
// Diagnostic sources (TOTK_MOUSE_GYRO_TEST): MOUSE_GYRO_TEST_RATE a 30 degree per second turn in the rotation rate
// alone, the pose held at rest; MOUSE_GYRO_TEST_POSE the pose turning at that rate with no rotation reported;
// MOUSE_GYRO_TEST_STEEP the RATE turn with the controller tilted 60 degrees instead (whether the game takes the turn
// about world up or about the controller's z); MOUSE_GYRO_TEST_FLICK mouse counts making even flicks
// (mouse_gyro_flick) through the mouse's own path, traced like TRACE. The others only log (input.c): CALIBRATE the
// turn of each movement, STREAM how the mouse counts arrive, TRACE every tick to a file.
enum { MOUSE_GYRO_TEST_OFF, MOUSE_GYRO_TEST_RATE, MOUSE_GYRO_TEST_POSE, MOUSE_GYRO_TEST_CALIBRATE,
       MOUSE_GYRO_TEST_STREAM, MOUSE_GYRO_TEST_TRACE, MOUSE_GYRO_TEST_STEEP, MOUSE_GYRO_TEST_FLICK };
void mouse_gyro_test_step(MouseGyro *g, int mode, double dt_s, double acc_g[3], double gyro_rad_s[3], double dir[9]);
// The counts of a mouse making even flicks, one tick of dt_s at `dots` counts per turn, from a zeroed MouseFlick, in a
// 2.4 s cycle: 45 degrees right in 0.15 s and back left, 20 degrees up in 0.1 s and back down, each move followed by
// a pause; whole counts, the fractions carried.
typedef struct { double t, cx, cy; } MouseFlick;
void mouse_gyro_flick(MouseFlick *f, double dt_s, double dots, int32_t *dx, int32_t *dy);
