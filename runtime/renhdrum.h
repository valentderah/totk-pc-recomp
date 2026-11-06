#ifndef RENHDRUM_H
#define RENHDRUM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RENHDRUM_VERSION 0x000201

#define RENHDRUM_OK 0
#define RENHDRUM_ERR_NULL_PTR -1
#define RENHDRUM_ERR_INVALID_ARG -2
#define RENHDRUM_ERR_DECODE_FAILED -3

typedef struct RenHdcSettings {
    bool enabled;
    int32_t usb_interval_ms;
    int32_t bluetooth_level;
    bool usb_auto_slow;
    bool sync_to_input;
    bool click_pulse;
    bool sync_bluetooth;
    bool usb_no_queue;
    int32_t usb_pause_ms;
    bool envelope_5ms;
    bool frequency_in_packet;
    bool frequency_both_bands;
    bool bt_after_input;
    bool crisp_notes;
    bool bt_crisp_notes;
    bool bt_fast;
} RenHdcSettings;

typedef struct RenHdcConfig {
    bool enable_hd_rumble;
    int32_t hd_rumble_usb_interval_ms;
    int32_t hd_rumble_bluetooth_level;
    bool hd_rumble_usb_auto_slow;
    bool hd_rumble_experimental_usb_no_queue;
    bool hd_rumble_experimental_envelope_5ms;
    bool hd_rumble_experimental_frequency_in_packet;
    bool hd_rumble_experimental_frequency_both_bands;
    bool hd_rumble_experimental_bt_after_input;
    bool hd_rumble_experimental_bt_fast;
    bool hd_rumble_experimental_crisp_notes;
    bool hd_rumble_experimental_bt_crisp_notes;
    bool hd_rumble_experimental_sync_to_input;
    bool hd_rumble_experimental_click_pulse;
    bool hd_rumble_experimental_sync_bluetooth;
    bool hd_rumble_sync_auto_slow;
    float strong_rumble;
    float weak_rumble;
    bool enable_rumble;
    bool enable_adaptive_triggers;
    bool enable_dualsense_haptics;
    int32_t dualsense_haptics_volume;
    bool dualsense_crisp_notes;
    bool dualsense_bt_crisp_notes;
} RenHdcConfig;

typedef struct RenHdSideSample {
    float amp_low;
    float freq_low;
    float amp_high;
    float freq_high;
} RenHdSideSample;

typedef struct RenHdClickSide {
    int32_t kind; /* 0 = DualResonance3Pulse, 1 = SingleWaveResonance */
    int32_t res_high_code;
    int32_t res_low_code;
    int32_t pulse1_code;
    int32_t pulse2_code;
    bool res_high_off;
    bool res_low_off;
    bool pulse1_off;
    bool pulse2_off;
    int32_t amp7;
    int32_t freq7;
    bool freq_is_high;
} RenHdClickSide;

uint32_t renhdrum_version(void);

int32_t renhdrum_settings_default(RenHdcSettings *out_settings);
int32_t renhdrum_settings_resolve(const RenHdcConfig *config, RenHdcSettings *out_settings);

int32_t renhdrum_preset_apply(int32_t kind, RenHdcConfig *config);
int32_t renhdrum_preset_detect(const RenHdcConfig *config, int32_t *out_kind);

int32_t renhdrum_encode_standard_side(
    float low_freq,
    float low_amp,
    float high_freq,
    float high_amp,
    uint8_t *out_bytes4
);

int32_t renhdrum_encode_standard_packet(
    float left_lf,
    float left_la,
    float left_hf,
    float left_ha,
    float right_lf,
    float right_la,
    float right_hf,
    float right_ha,
    uint8_t *out_bytes8
);

int32_t renhdrum_amfm_game_amplitude(int32_t code, float *out_amp);
int32_t renhdrum_amfm_code(float game_amp);
int32_t renhdrum_amfm_freq7(int32_t high, float freq);
int32_t renhdrum_amfm_amp7(float game_amplitude);
int32_t renhdrum_amfm_preset_code(int32_t high, int32_t freq7);

int32_t renhdrum_amfm_pack_three(
    int32_t lo0,
    int32_t hi0,
    int32_t lo1,
    int32_t hi1,
    int32_t lo2,
    int32_t hi2,
    uint32_t *out_val
);

int32_t renhdrum_amfm_pack_three_7bit(
    int32_t high,
    int32_t is_fm,
    int32_t value7,
    int32_t lo1,
    int32_t hi1,
    int32_t lo2,
    int32_t hi2,
    uint32_t *out_val
);

int32_t renhdrum_amfm_pack_two_14bit(
    int32_t high,
    int32_t freq7,
    int32_t amp7,
    int32_t other0,
    int32_t lo1,
    int32_t hi1,
    uint32_t *out_val
);

int32_t renhdrum_amfm_pack_two_14bit_level(
    int32_t high,
    int32_t freq7,
    float game_amplitude,
    int32_t other0,
    int32_t lo1,
    int32_t hi1,
    uint32_t *out_val
);

int32_t renhdrum_amfm_pack_two_14bit_reach(
    int32_t high,
    int32_t freq7,
    float game_amplitude,
    int32_t reach_code,
    float other_amplitude,
    int32_t lo1,
    int32_t hi1,
    uint32_t *out_val
);

int32_t renhdrum_amfm_is_three_sample_side(const uint8_t *in_bytes4);

int32_t renhdrum_click_amp4(float amplitude);
int32_t renhdrum_click_amp7(float amplitude);
int32_t renhdrum_click_build_packet(float amp_l, float amp_r, uint8_t *out_packet10);

int32_t renhdrum_click_encode_side(const RenHdClickSide *side, uint8_t *out_bytes4);
int32_t renhdrum_click_decode_side(const uint8_t *in_bytes4, RenHdClickSide *out_side);
int32_t renhdrum_click_is_dual_wave_side(const uint8_t *in_bytes4);
int32_t renhdrum_click_is_click_packet(const uint8_t *in_bytes10);

float renhdrum_crisp_slot_gain(int32_t slots_since_onset);
int32_t renhdrum_crisp_apply(
    const RenHdSideSample *s,
    float gain,
    RenHdSideSample *out_sample,
    int32_t *out_capped
);
int32_t renhdrum_crisp_shape_slot(
    int32_t *slots_since_onset,
    const RenHdSideSample *s,
    RenHdSideSample *out_sample,
    int32_t *out_onset,
    int32_t *out_capped
);

/* Opaque pointer types for stateful engines */
typedef struct RenHdcEngine RenHdcEngine;
typedef struct RenHdcDualSenseDsp RenHdcDualSenseDsp;
typedef struct RenHdcXboxRumble RenHdcXboxRumble;

RenHdcEngine *renhdrum_engine_create(bool is_bluetooth, bool is_pro_controller);
void renhdrum_engine_destroy(RenHdcEngine *engine);
int32_t renhdrum_engine_apply_settings(RenHdcEngine *engine, const RenHdcSettings *settings);
int32_t renhdrum_engine_push_sample(
    RenHdcEngine *engine,
    int64_t now_us,
    float left_al,
    float left_fl,
    float left_ah,
    float left_fh,
    float right_al,
    float right_fl,
    float right_ah,
    float right_fh
);
int32_t renhdrum_engine_push_half_sample(
    RenHdcEngine *engine,
    int64_t now_us,
    bool is_right,
    float al,
    float fl,
    float ah,
    float fh
);
int32_t renhdrum_engine_tick(RenHdcEngine *engine, int64_t now_us, uint8_t *out_packet10);

RenHdcDualSenseDsp *renhdrum_dualsense_dsp_create(int32_t volume_percent, bool crisp_notes);
void renhdrum_dualsense_dsp_destroy(RenHdcDualSenseDsp *dsp);
int32_t renhdrum_dualsense_dsp_set_target(
    RenHdcDualSenseDsp *dsp,
    float left_al,
    float left_fl,
    float left_ah,
    float left_fh,
    float right_al,
    float right_fl,
    float right_ah,
    float right_fh
);
int32_t renhdrum_dualsense_dsp_set_half_target(
    RenHdcDualSenseDsp *dsp,
    bool is_right,
    float al,
    float fl,
    float ah,
    float fh
);
int32_t renhdrum_dualsense_dsp_fill_f32(
    RenHdcDualSenseDsp *dsp,
    float *out_interleaved_4ch,
    size_t frame_count
);
/* DualSense over Bluetooth: one HID output report 0x32 every RENHDRUM_DS5_BT_PERIOD_NS.
   Report layout from SAxense by egormanga (https://apps.sdore.me/SAxense). */
#define RENHDRUM_DS5_BT_REPORT_SIZE 142
#define RENHDRUM_DS5_BT_PERIOD_NS 10666666ULL
#define RENHDRUM_DS5_BT_BUFFER_LENGTH_DEFAULT 0xFF
int32_t renhdrum_dualsense_dsp_build_bt_report(
    RenHdcDualSenseDsp *dsp,
    uint8_t buffer_length,
    uint8_t *out_report142
);
int32_t renhdrum_dualsense_evaluate_triggers(
    uint64_t title_id,
    bool zl_held,
    bool zr_held,
    bool rumble_on,
    int64_t rumble_on_ms,
    int64_t pulse_age_ms,
    uint8_t *out_ds5_effects47
);

RenHdcXboxRumble *renhdrum_xbox_rumble_create(bool is_bluetooth);
void renhdrum_xbox_rumble_destroy(RenHdcXboxRumble *rumble);
int32_t renhdrum_xbox_rumble_push_half_sample(
    RenHdcXboxRumble *rumble,
    int64_t now_us,
    bool is_right,
    float low_amp,
    float low_freq,
    float high_amp,
    float high_freq
);
/* out_motors4: left grip, right grip, left trigger, right trigger. Returns 1 when they must be sent now. */
int32_t renhdrum_xbox_rumble_tick(RenHdcXboxRumble *rumble, int64_t now_us, uint16_t *out_motors4);
int32_t renhdrum_xbox_rumble_resend(RenHdcXboxRumble *rumble);

#ifdef __cplusplus
}
#endif

#endif /* RENHDRUM_H */
