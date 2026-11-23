// Ported from adriadam10/mkw-recomp (MIT, Copyright (c) 2026 Adrià), runtime/input/switch2.h at eb8fe198 through dkb-pc-recomp; its HD
// rumble stream (switch2_rumble_hd) added.
//
// Switch 2 controllers (Pro Controller 2, Joy-Con 2) over Bluetooth LE, which SDL does not drive (SDL 3.4 has them over
// USB only). A transport finds and connects them (switch2_bluez.c: BlueZ over D-Bus on Linux; switch2_win.c: WinRT on
// Windows) and hands each one to switch2.c, which speaks the protocol (switch2_proto.h, from mkw-neo through
// mkw-recomp) and shows the controller to the rest of the input as a virtual SDL gamepad: selection, button layout and
// gyro then work as for any other gamepad. Rumble goes to the controller as the game's own HD values (amplitude and
// frequency of each band, both sides, 5 ms samples) through switch2_rumble_hd; a plain SDL_RumbleGamepad on the virtual
// pad still works, played at 160 / 320 Hz. TOTK_SWITCH2=1 (Settings > Controls, read at start) turns it on; it runs on
// the transport's own thread, never the game's.
#pragma once
#include <stddef.h>
#include <stdint.h>

void switch2_start(void);   // input.c's start: the transport's thread, when TOTK_SWITCH2=1
// The battery of the virtual gamepad with this SDL joystick id (SDL keeps no power state for a virtual one): its band
// (S2_BATTERY_*, switch2_proto.h; 0 not ours or none reported yet) and *mv its voltage. Any thread.
int switch2_battery(uint32_t sdl_id, int *mv);
// 1 when the SDL joystick id is one of our virtual gamepads. Any thread.
int switch2_owns(uint32_t sdl_id);
// The game's vibration for that gamepad: v[side][] = amplitude 0..1 and frequency in Hz of the low band, then of the
// high band (side 0 left, 1 right), the curve and strength already applied to the amplitudes. Called for every value
// the game sets; each is played once, in time order, at the controller's 5 ms sample rate. A pad with no value for
// 250 ms goes silent by itself. 1 if the pad is ours. Any thread.
int switch2_rumble_hd(uint32_t sdl_id, const float v[2][4]);

// ---- between a transport and switch2.c

typedef struct S2Link S2Link;   // one connected controller, the transport's
typedef struct S2Session S2Session;

typedef struct {
    int (*write_cmd)(S2Link *l, const uint8_t *d, size_t n);   // to S2_UUID_CMD_WR, without response; 0 on failure
    int (*write_vib)(S2Link *l, const uint8_t *d, size_t n);   // to the vibration characteristic, if it has one
    int (*start_input)(S2Link *l);                              // notifications of S2_UUID_INPUT on
    void (*pump)(int ms);   // the transport's events for up to ms (replies and reports reach the session from here)
    void (*log_link)(S2Link *l);   // optional: the link's own figures in the log (Windows: its connection interval)
} S2LinkOps;

uint64_t switch2_now_ns(void);
// A controller connected, its characteristics found and replies subscribed: its session (replies reach it from here
// on), then the handshake (identity, calibration, pairing when use is S2_ADV_PAIR, features) and a gamepad. 0 when
// the handshake failed: the transport closes the session and disconnects it.
S2Session *switch2_new(S2Link *l, const S2LinkOps *ops, uint16_t pid, int use, const uint8_t host[6]);
int switch2_handshake(S2Session *s);
void switch2_reply(S2Session *s, const uint8_t *d, size_t n);               // a S2_UUID_CMD_RSP notification
void switch2_report(S2Session *s, const uint8_t *d, size_t n, uint64_t at);  // a S2_UUID_INPUT notification
void switch2_tick(void);                                                     // every ~15 ms: rumble, keep-alive
void switch2_close(S2Session *s);                                            // disconnected: its gamepad goes
