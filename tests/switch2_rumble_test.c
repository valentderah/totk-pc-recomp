// Switch 2 controller HD rumble (runtime/switch2_proto.h, as runtime/switch2.c uses it): the coding of a game's
// vibration value, the packet layout, and the 5 ms sample schedule that plays every value once, in order. No SDL, no
// Bluetooth, no game data.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "switch2_proto.h"

static const uint64_t MS = 1000000ull;

// The 40-bit sample at o in a packet, unpacked
static s2_haptic unpack(const uint8_t *o) {
    uint64_t x = 0;
    for (int i = 0; i < 5; i++) x |= (uint64_t)o[i] << (8 * i);
    s2_haptic v = {(uint16_t)(x & 0x3FF), (uint16_t)(x >> 10 & 0x3FF), (uint16_t)(x >> 20 & 0x3FF), (uint16_t)(x >> 30 & 0x3FF)};
    return v;
}

static void codes(void) {
    // the frequency code: 96 steps an octave, 333 at 110 Hz; the round trip lands within half a step
    assert(s2_freq_code(110.f) == 333);
    assert(s2_freq_code(220.f) == 333 + 96);
    for (float hz = 40.f; hz < 1300.f; hz *= 1.07f) {
        float back = s2_freq_hz(s2_freq_code(hz));
        assert(fabsf(log2f(back / hz)) <= 0.5f / 96.f + 1e-4f);
    }
    assert(s2_freq_code(0.f) == 0 && s2_freq_code(1e9f) == 0x3FF);
    assert(s2_amp_code(0.f) == 0 && s2_amp_code(1.f) == 1023 && s2_amp_code(0.25f) == 256 && s2_amp_code(-1.f) == 0);
    // the console's cap: both bands at full add up to 0.98
    float lo = 1.f, hi = 1.f;
    s2_rumble_cap(&lo, &hi);
    assert(fabsf(lo + hi - 0.98f) < 1e-5f);
}

static void packet(void) {
    s2_haptic a[3], b[3];
    for (int i = 0; i < 3; i++) {
        a[i] = s2_haptic_from(0.1f * (float)(i + 1), 160.f, 0.2f, 320.f);
        b[i] = s2_haptic_from(0.f, 160.f, 0.3f * (float)(i + 1) / 3.f, 640.f);
    }
    uint8_t out[33];
    assert(s2_rumble_packet3(out, a, b, 0x17) == 33);
    assert(out[0] == 0x00);
    assert(out[1] == (0x40 | 3 << 4 | 0x7) && out[17] == out[1]);   // three samples, sequence in the low four bits
    for (int i = 0; i < 3; i++) {
        s2_haptic x = unpack(out + 2 + 5 * i), y = unpack(out + 18 + 5 * i);
        assert(s2_haptic_same(&x, &a[i]) && s2_haptic_same(&y, &b[i]));
    }
}

// switch2.c's rumble for one controller: resync, then three slots, each picked for this controller
static void tick(const s2_rumble_log *l, s2_rumble_cursor *c, const s2_pad_state *p, const s2_dev *dev, uint64_t now,
                 s2_haptic a[3], s2_haptic b[3]) {
    s2_rumble_resync(c, now, 20 * MS);
    for (int i = 0; i < 3; i++) {
        s2_rumble_slot(l, c, s2_rumble_motors(p, dev));
        s2_rumble_pick(p, dev, c->now, &a[i], &b[i]);
    }
}

static void schedule(void) {
    s2_pad_state pro;
    memset(&pro, 0, sizeof pro);
    pro.pro.have = 1;
    s2_rumble_log log;
    memset(&log, 0, sizeof log);
    s2_rumble_cursor cur;
    memset(&cur, 0, sizeof cur);
    // A game streaming every 5 ms from t = 100 ms: value k is amplitude k / 64 on the left low band, and on the right
    // high band at 2 * k / 64; ticks every 15 ms. Every value plays exactly once, in order; left goes to block a.
    int next_expected = 1;
    uint64_t t = 100 * MS;
    int k = 1;
    for (uint64_t now = 100 * MS; now < 500 * MS; now += 15 * MS) {
        for (; t < now && k < 60; t += 5 * MS, k++) {
            s2_haptic m[2] = {s2_haptic_from((float)k / 64.f, 160.f, 0.f, 320.f),
                              s2_haptic_from(0.f, 160.f, 2.f * (float)k / 64.f, 320.f)};
            s2_rumble_log_add(&log, t, m);
        }
        s2_haptic a[3], b[3];
        tick(&log, &cur, &pro, &pro.pro, now, a, b);
        for (int i = 0; i < 3; i++) {
            if (!a[i].lf_amp) continue;   // before the first value
            int got = (int)lroundf((float)a[i].lf_amp / 1023.f * 64.f);
            assert(b[i].hf_amp == s2_amp_code(2.f * (float)got / 64.f));   // the same value's right side
            assert(a[i].hf_amp == 0 && b[i].lf_amp == 0);
            if (got == next_expected) next_expected++;
            else assert(got == next_expected - 1);   // repeated only when no newer value was set by then
        }
    }
    assert(next_expected == 60);   // values 1..59 all played

    // A Joy-Con alone plays the stronger side of each band; one of a pair plays its own side
    s2_pad_state joy;
    memset(&joy, 0, sizeof joy);
    joy.right.have = 1;
    s2_haptic m[2] = {s2_haptic_from(0.6f, 100.f, 0.1f, 300.f), s2_haptic_from(0.2f, 150.f, 0.4f, 400.f)}, x, y;
    s2_rumble_pick(&joy, &joy.right, m, &x, &y);
    assert(x.lf_amp == m[0].lf_amp && x.lf_hz == m[0].lf_hz && x.hf_amp == m[1].hf_amp && x.hf_hz == m[1].hf_hz);
    joy.left.have = 1;
    s2_rumble_pick(&joy, &joy.right, m, &x, &y);
    assert(s2_haptic_same(&x, &m[1]) && s2_haptic_same(&y, &m[1]));
    s2_rumble_pick(&joy, &joy.left, m, &x, &y);
    assert(s2_haptic_same(&x, &m[0]));
}

int main(void) {
    codes();
    packet();
    schedule();
    puts("switch2 rumble: ok");
    return 0;
}
