// DualSense (PS5) adaptive triggers: the bytes of a trigger effect, the effects report that sets both triggers, and
// which effect each trigger gets (dualsense.c). No SDL and no window system here: input.c sends the report with
// SDL_SendGamepadEffect.
#pragma once
#include <stdint.h>

enum {
    DS_TRIGGER_BYTES = 11,   // one trigger's effect: the mode byte and ten parameter bytes
    DS_REPORT_BYTES = 47,    // the effects part of the controller's output report
    DS_ZONES = 10,           // a trigger's travel in zones: 0 at rest, 9 fully pulled
    DS_LEVEL_MAX = 8,        // the strongest resistance and the strongest vibration
    DS_MODE_OFF = 0x05, DS_MODE_FEEDBACK = 0x21, DS_MODE_VIBRATION = 0x26,   // byte 0 of an effect
};

// No effect: the trigger moves freely.
void ds_trigger_off(uint8_t out[DS_TRIGGER_BYTES]);
// The trigger resists in each zone with strength[zone], 0 (free) to 8; all zones free is ds_trigger_off.
void ds_trigger_feedback(uint8_t out[DS_TRIGGER_BYTES], const uint8_t strength[DS_ZONES]);
// The trigger vibrates from zone (0..9) to the end of its travel with amplitude 1..8 at hz (1..255); amplitude 0 or
// hz 0 is ds_trigger_off.
void ds_trigger_vibration(uint8_t out[DS_TRIGGER_BYTES], int zone, int amplitude, int hz);
// The effects report that changes the two trigger effects and nothing else (rumble, lights and audio keep their state).
void ds_report_triggers(uint8_t out[DS_REPORT_BYTES], const uint8_t right[DS_TRIGGER_BYTES],
                        const uint8_t left[DS_TRIGGER_BYTES]);

// Trigger vibration level 0..8 for a rumble motor value 0..65535, rounded up to the next eighth of full drive: 0 only
// for 0, so the weakest rumble still reaches the triggers.
int ds_vibration_level(uint16_t motor);

// What decides the two effects. strength: TOTK_DUALSENSE_TRIGGERS, 0 off, 1..8 the resistance. vibration: 0, or the
// level 1..8 the game's rumble gives the triggers (TOTK_RUMBLE_TRIGGERS).
typedef struct { int strength, vibration; } DsTriggerState;
// The effects of the right (ZR) and the left (ZL) trigger for s.
void ds_triggers_pick(const DsTriggerState *s, uint8_t right[DS_TRIGGER_BYTES], uint8_t left[DS_TRIGGER_BYTES]);
