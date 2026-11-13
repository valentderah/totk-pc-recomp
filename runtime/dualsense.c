// DualSense (PS5) adaptive triggers (dualsense.h): effect bytes, the effects report and the choice of effect. No SDL
// and no window system here.
//
// The report is the effects part of the controller's output report as SDL lays it out (SDL,
// src/joystick/hidapi/SDL_hidapi_ps5.c, DS5EffectsState_t): byte 0 holds the bits that say which fields the controller
// takes from this report (0x04 the right trigger's effect, 0x08 the left one's), bytes 10..20 the right trigger's
// effect, bytes 21..31 the left one's. SDL's driver puts the report id, the Bluetooth header and the CRC around it
// (HIDAPI_DriverPS5_InternalSendJoystickEffect).
//
// The effect bytes follow the layouts John "Nielk1" Klein documented for the modes the firmware officially supports
// (gist.github.com/Nielk1/6d54cc2c00d2201ccb8c2720ad7538db, TriggerEffectGenerator.cs: Off, MultiplePositionFeedback,
// Vibration): a trigger's travel is ten zones; bytes 1..2 are a bit per active zone and bytes 3..6 hold three bits per
// zone, the strength or the amplitude minus one.
#include <string.h>

#include "dualsense.h"

enum {
    ENABLE_RIGHT_TRIGGER = 0x04, ENABLE_LEFT_TRIGGER = 0x08,   // report byte 0
    REPORT_RIGHT = 10, REPORT_LEFT = 21,                       // where the two effects start in the report
    // The game takes ZL and ZR as pressed from half way (input.c TRIGGER_ON): zone 5. The resistance starts before
    // that, so drawing the bow is felt from the start of the pull and not only once the press has counted.
    PULL_ZONE = 2, PRESS_ZONE = 5,
    HOLD_EXTRA = 2,    // past the press ZR holds this much harder: the drawn bow, and the let-go that shoots
    SHIELD_LESS = 2,   // ZL (shield, lock-on) is held through whole fights: lighter than ZR, and even over its travel
    // The Xbox impulse triggers TOTK_RUMBLE_TRIGGERS was made for are small motors that spin faster the harder they
    // are driven; the DualSense takes the frequency as a number, so it rises with the level the same way.
    VIBRATION_HZ_BASE = 25, VIBRATION_HZ_PER_LEVEL = 5,
};

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void ds_trigger_off(uint8_t out[DS_TRIGGER_BYTES]) {
    memset(out, 0, DS_TRIGGER_BYTES);
    out[0] = DS_MODE_OFF;
}

// A Feedback or Vibration effect with level[zone] in each zone, 0 for a zone the effect leaves alone; ds_trigger_off
// when it leaves all of them alone.
static void zones(uint8_t out[DS_TRIGGER_BYTES], int mode, const uint8_t level[DS_ZONES]) {
    uint32_t levels = 0, active = 0;
    for (int z = 0; z < DS_ZONES; z++) {
        if (!level[z]) continue;
        levels |= (uint32_t)(clamp(level[z], 1, DS_LEVEL_MAX) - 1) << (3 * z);
        active |= 1u << z;
    }
    ds_trigger_off(out);
    if (!active) return;
    out[0] = (uint8_t)mode;
    out[1] = (uint8_t)active;
    out[2] = (uint8_t)(active >> 8);
    for (int i = 0; i < 4; i++) out[3 + i] = (uint8_t)(levels >> (8 * i));
}

void ds_trigger_feedback(uint8_t out[DS_TRIGGER_BYTES], const uint8_t strength[DS_ZONES]) {
    zones(out, DS_MODE_FEEDBACK, strength);
}

void ds_trigger_vibration(uint8_t out[DS_TRIGGER_BYTES], int zone, int amplitude, int hz) {
    uint8_t level[DS_ZONES] = {0};
    if (amplitude > 0 && hz > 0)
        for (int z = clamp(zone, 0, DS_ZONES - 1); z < DS_ZONES; z++) level[z] = (uint8_t)clamp(amplitude, 1, DS_LEVEL_MAX);
    zones(out, DS_MODE_VIBRATION, level);
    if (out[0] == DS_MODE_VIBRATION) out[9] = (uint8_t)clamp(hz, 1, 255);
}

void ds_report_triggers(uint8_t out[DS_REPORT_BYTES], const uint8_t right[DS_TRIGGER_BYTES],
                        const uint8_t left[DS_TRIGGER_BYTES]) {
    memset(out, 0, DS_REPORT_BYTES);
    out[0] = ENABLE_RIGHT_TRIGGER | ENABLE_LEFT_TRIGGER;
    memcpy(out + REPORT_RIGHT, right, DS_TRIGGER_BYTES);
    memcpy(out + REPORT_LEFT, left, DS_TRIGGER_BYTES);
}

int ds_vibration_level(uint16_t motor) {
    return (int)(((uint32_t)motor * DS_LEVEL_MAX + 65534) / 65535);
}

// While the game's rumble reaches the triggers both vibrate over their whole travel, also at rest under a finger, and
// the resistance waits: a trigger has one effect at a time. Otherwise ZR resists the pull that draws the bow and holds
// harder once the press counts, and ZL resists a little less. Neither effect depends on what the game is doing, so
// the controller gets a new one only when a setting changes or a rumble starts, changes level or ends.
void ds_triggers_pick(const DsTriggerState *s, uint8_t right[DS_TRIGGER_BYTES], uint8_t left[DS_TRIGGER_BYTES]) {
    if (s->vibration > 0) {
        int level = clamp(s->vibration, 1, DS_LEVEL_MAX);
        ds_trigger_vibration(right, 0, level, VIBRATION_HZ_BASE + VIBRATION_HZ_PER_LEVEL * level);
        memcpy(left, right, DS_TRIGGER_BYTES);
        return;
    }
    uint8_t zr[DS_ZONES] = {0}, zl[DS_ZONES] = {0};
    if (s->strength > 0) {
        int pull = clamp(s->strength, 1, DS_LEVEL_MAX);
        for (int z = PULL_ZONE; z < DS_ZONES; z++) {
            zr[z] = (uint8_t)clamp(pull + (z >= PRESS_ZONE ? HOLD_EXTRA : 0), 1, DS_LEVEL_MAX);
            zl[z] = (uint8_t)clamp(pull - SHIELD_LESS, 1, DS_LEVEL_MAX);
        }
    }
    ds_trigger_feedback(right, zr);
    ds_trigger_feedback(left, zl);
}
