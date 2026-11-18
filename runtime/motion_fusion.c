// Adapted from MacNativeGames games/totk/runtime/motion_fusion.c @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames
// contributors).
// Six-axis sensor states (nn::hid::SixAxisSensorState) from gyro and accelerometer samples. Input per sample, in the
// Switch controller frame (x to the right, y toward the top edge, z out of the face): acceleration in G including
// gravity, as gravity reads (lying face up z = -1 G), and rotation rate in rad/s. Output per sample, as the Switch
// reports it: acceleration in G, angular velocity in revolutions per second, angle = the integrated revolutions per
// axis, direction = the controller's three axes in a reference frame whose z is up. The orientation is integrated from
// the gyro and, while the controller is not being accelerated (|a| within 15 % of 1 G), pulled toward the measured
// gravity (Mahony, gain 0.5/s: about 2 s), as the Switch's sensor fusion does; held still (|a| within 5 % of 1 G,
// turning slower than STILL_RATE) the pull is STILL_GAIN, so a down the gyro got wrong while the controller moved is
// right again before the next aim. The game splits the aim into pitch and yaw by the reported down: a wrong one turns a
// tilt up or down into a turn to the side as well. Yaw has no reference and drifts with the gyro's bias, so a bias
// measured with the controller at rest (the Calibrate gyro row, motion_calibrate_start) is subtracted from every sample
// while gyro_calibration is on, and the drift left after it is learned while the controller lies still and subtracted
// too while gyro_auto_bias is on (drift_track). The newest MOTION_RING samples are kept; reads are newest first. A
// restart (StartSixAxisSensor) drops the samples before it: reads return nothing until the first sample with a reading
// (|a| over 0.5 G: a sensor just switched on reads zero until its first report), which starts a fresh frame, levelled
// by that reading, and a fresh angle; sampling numbers go on. The feeding thread writes, guest threads read, both under
// the instance's SRW lock.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "platform/platform.h"

// A calibration skips CALIB_SETTLE samples (0.5 s at the sensor thread's 4 ms), then averages CALIB_SAMPLES (1 s).
enum { MOTION_RING = 64, CALIB_SETTLE = 125, CALIB_SAMPLES = 250 };
#define STALE_NS 200000000ull   // no sample for 200 ms: no motion
#define CALIB_TIMEOUT_NS 5000000000ull   // 5 s without a sample: the attempt gives up
// Largest per-axis standard deviation of the averaged samples (rad/s, about 2.3 deg/s) that still counts as holding
// still. The spread, not the mean, tells a moved controller from a biased one: a factory bias shifts every sample
// alike (0.14 rad/s on one Pro Controller's z axis) and passes, while the noise of a resting pad or a steady hand stays
// near 0.01-0.02 rad/s. A jolt that stays under the limit moves the mean by at most the limit times the square root of
// the share of the second it lasts.
#define CALIB_MAX_SPREAD 0.04
#define STILL_GAIN 4.0          // 1/s: a wrong down is 2 % of itself after a second held still
#define STILL_RATE 0.5          // rad/s, about 30 deg/s: a slow aim still counts as held still
// The drift tracker (drift_track), after PR #380's estimator: a window of REST_WIN samples (256 ms) is still when every
// axis of the gyro stays within REST_GYRO_PTP peak to peak and of the acceleration within REST_ACC_PTP, |a| is within
// REST_GRAVITY of 1 G, and the window's mean rate is within DRIFT_FIRST_MAX (before the estimate settled) or
// DRIFT_TRACK_MAX (after) of the estimate in use. A Pro Controller on a table reads 0.004-0.006 rad/s of noise and
// passes; held in a hand, even steadily, it reads 0.013-0.025 rad/s and mostly does not, so the tracker learns from a
// controller put down rather than from an aim held still. After REST_HOLD_S still the estimate moves toward the
// window's mean with a time constant of DRIFT_TAU_FIRST until it has settled, then DRIFT_TAU: a bias changes only
// slowly as the controller warms up, and a hand that slips past the gate shifts it little. No dead zone: a slow aim
// keeps every bit of its rate.
enum { REST_WIN = 64 };
#define REST_GYRO_PTP 0.04      // rad/s, per axis over the window
#define REST_ACC_PTP 0.03       // G, per axis over the window
#define REST_GRAVITY 0.25       // G: controllers' accelerometers read 1 G only roughly
#define REST_HOLD_S 0.5
#define DRIFT_FIRST_MAX 0.1     // rad/s, 5.7 deg/s; a larger factory bias (one Pro: 0.14 on z) needs the calibration
#define DRIFT_TRACK_MAX 0.05    // rad/s
#define DRIFT_TAU_FIRST 0.25    // s
#define DRIFT_TAU 30.0          // s
#define DRIFT_SETTLE_S 1.5      // learning this long with the estimate within DRIFT_SETTLED of the window: settled
#define DRIFT_SETTLED 0.01      // rad/s
#define DRIFT_LIMIT 0.3         // rad/s per axis

struct MotionFusion {
    PlatMutex lock;
    MotionState ring[MOTION_RING];
    uint64_t produced, last_ns;
    uint64_t first;   // produced at the last restart: older samples are not read
    double R[3][3];   // controller -> reference; row i = reference axis i in controller coordinates
    double angle[3];
    int restart;
    double gyro_bias[3];   // the controller's resting gyro (rad/s), subtracted from every sample while the switch is on
    int bias_valid;        // gyro_bias holds a measured or loaded estimate, so a zero is a value
    int calibrating;       // an attempt is collecting samples
    int calib_count;       // samples of the attempt so far, the skipped settle samples included
    double calib_sum[3], calib_sq[3];   // sum and sum of squares of the averaged samples, per axis
    uint64_t calib_start_ns;            // when the attempt started (the timeout)
    char calib_pad[128];   // the controller the attempt measures (input.c pad_identity), written with the bias
    int calib_result;      // MOTION_CALIB_NONE, _DONE, _FAILED or _MOVED: the outcome the row shows between attempts
    int persist;           // the gamepad's instance: a finished calibration is written to totk.ini
    // The drift tracker: the estimate (rad/s, on top of gyro_bias when gyro_calibration is on), whether it settled,
    // and the rest window it is learned from. The estimate belongs to the controller and to the bias it was learned
    // against: kept through restarts, cleared with a new controller, a new calibration or a change of
    // gyro_calibration (drift_clear).
    double drift[3];
    int drift_settled;
    int drift_base;        // gyro_calibration the estimate was learned with, -1 none
    double rest_s, learn_s;
    double win_g[REST_WIN][3], win_a[REST_WIN][3];
    int win_n, win_i;
};

MotionFusion *motion_new(void) {
    MotionFusion *m = calloc(1, sizeof *m);
    if (!m) rt_fatal("motion: out of memory");
    plat_mutex_init(&m->lock);
    m->restart = 1;
    m->drift_base = -1;
    return m;
}

static MotionFusion *global;

static void global_init(void *arg) {
    (void)arg;
    global = motion_new();
    global->persist = 1;
}

MotionFusion *motion_global(void) {
    static PlatOnce once = PLAT_ONCE_INIT;
    plat_once(&once, global_init, NULL);
    return global;
}

static double norm3(const double v[3]) { return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
static void cross3(const double a[3], const double b[3], double out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
// Rows, from the up row (the one the gravity correction keeps right).
static void orthonormalize(double R[3][3]) {
    double n = norm3(R[2]);
    for (int j = 0; j < 3; j++) R[2][j] /= n;
    double d = R[0][0] * R[2][0] + R[0][1] * R[2][1] + R[0][2] * R[2][2];
    for (int j = 0; j < 3; j++) R[0][j] -= d * R[2][j];
    n = norm3(R[0]);
    for (int j = 0; j < 3; j++) R[0][j] /= n;
    cross3(R[2], R[0], R[1]);
}
// A fresh orientation: z up against the measured gravity (|acc| over 0.5 G), x as close to the controller's x as that
// allows.
static void align_to(double R[3][3], const double acc[3]) {
    double n = norm3(acc), up[3], x[3] = {1, 0, 0};
    for (int j = 0; j < 3; j++) up[j] = -acc[j] / n;
    if (fabs(up[0]) > 0.9) { x[0] = 0; x[1] = 1; }
    memcpy(R[2], up, sizeof up);
    memcpy(R[0], x, sizeof x);
    orthonormalize(R);
}
// Body-frame rate: R = R * exp([w] dt), the rate plus the gravity pull.
static void integrate(double R[3][3], const double gyro[3], const double acc[3], double dt) {
    double w[3] = {gyro[0], gyro[1], gyro[2]}, n = norm3(acc);
    if (fabs(n - 1) < 0.15) {   // not being accelerated: turn the estimated down toward the measured one
        double kp = fabs(n - 1) < 0.05 && norm3(gyro) < STILL_RATE ? STILL_GAIN : 0.5;
        double measured[3] = {acc[0] / n, acc[1] / n, acc[2] / n}, estimated[3] = {-R[2][0], -R[2][1], -R[2][2]}, e[3];
        cross3(measured, estimated, e);
        for (int j = 0; j < 3; j++) w[j] += kp * e[j];
    }
    double rate = norm3(w), theta = rate * dt;
    if (theta < 1e-12) return;
    double k[3] = {w[0] / rate, w[1] / rate, w[2] / rate}, s = sin(theta), c = 1 - cos(theta);
    double K[3][3] = {{0, -k[2], k[1]}, {k[2], 0, -k[0]}, {-k[1], k[0], 0}}, E[3][3], out[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double kk = 0;
            for (int q = 0; q < 3; q++) kk += K[i][q] * K[q][j];
            E[i][j] = (i == j) + s * K[i][j] + c * kk;
        }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double v = 0;
            for (int q = 0; q < 3; q++) v += R[i][q] * E[q][j];
            out[i][j] = v;
        }
    memcpy(R, out, sizeof out);
    orthonormalize(R);
}

// The tracker's own switch (gyro_auto_bias, live, default 1). Off, nothing is learned or subtracted.
int motion_auto_bias_enabled(void) {
    static int on = -1;
    static unsigned seen;
    unsigned v = cfg_version();
    if (on < 0 || v != seen) {
        seen = v;
        on = cfg_bool("TOTK_GYRO_AUTO_BIAS");
    }
    return on;
}

// m->lock held: forget the estimate and the rest window.
static void drift_clear(MotionFusion *m) {
    memset(m->drift, 0, sizeof m->drift);
    m->drift_settled = 0;
    m->drift_base = -1;
    m->rest_s = m->learn_s = 0;
    m->win_n = m->win_i = 0;
}

// Largest per-axis peak-to-peak of a full window.
static double win_ptp(const double w[REST_WIN][3]) {
    double worst = 0;
    for (int i = 0; i < 3; i++) {
        double lo = w[0][i], hi = w[0][i];
        for (int j = 1; j < REST_WIN; j++) {
            if (w[j][i] < lo) lo = w[j][i];
            if (w[j][i] > hi) hi = w[j][i];
        }
        if (hi - lo > worst) worst = hi - lo;
    }
    return worst;
}

// m->lock held, one sample with a reading: gyro is the rate with the calibrated bias taken off when base is 1. Moves
// m->drift toward the rate of a controller lying still. Returns 1 when the estimate settled with this sample.
static int drift_track(MotionFusion *m, const double acc[3], const double gyro[3], int base, double dt) {
    if (m->drift_base != base) {   // nothing learned yet, or learned against the other baseline: start over
        drift_clear(m);
        m->drift_base = base;
    }
    memcpy(m->win_g[m->win_i], gyro, sizeof m->win_g[0]);
    memcpy(m->win_a[m->win_i], acc, sizeof m->win_a[0]);
    m->win_i = (m->win_i + 1) % REST_WIN;
    if (m->win_n < REST_WIN) m->win_n++;
    if (m->win_n < REST_WIN) return 0;
    double mean[3] = {0, 0, 0}, a[3] = {0, 0, 0}, dev = 0;
    for (int j = 0; j < REST_WIN; j++)
        for (int i = 0; i < 3; i++) mean[i] += m->win_g[j][i] / REST_WIN, a[i] += m->win_a[j][i] / REST_WIN;
    for (int i = 0; i < 3; i++)
        if (fabs(mean[i] - m->drift[i]) > dev) dev = fabs(mean[i] - m->drift[i]);
    int still = fabs(norm3(a) - 1) < REST_GRAVITY && win_ptp(m->win_g) < REST_GYRO_PTP &&
                win_ptp(m->win_a) < REST_ACC_PTP && dev < (m->drift_settled ? DRIFT_TRACK_MAX : DRIFT_FIRST_MAX);
    if (!still) {
        m->rest_s = 0;
        if (!m->drift_settled) m->learn_s = 0;
        return 0;
    }
    m->rest_s += dt;
    if (m->rest_s < REST_HOLD_S) return 0;
    double k = dt / (m->drift_settled ? DRIFT_TAU : DRIFT_TAU_FIRST);
    for (int i = 0; i < 3; i++) {
        m->drift[i] += k * (mean[i] - m->drift[i]);
        if (m->drift[i] > DRIFT_LIMIT) m->drift[i] = DRIFT_LIMIT;
        if (m->drift[i] < -DRIFT_LIMIT) m->drift[i] = -DRIFT_LIMIT;
    }
    m->learn_s += dt;
    if (m->drift_settled || m->learn_s < DRIFT_SETTLE_S || dev >= DRIFT_SETTLED) return 0;
    m->drift_settled = 1;
    return 1;
}

// Write the calibrated bias to totk.ini. Called with m->lock released: cfg_set takes cfg.c's own lock and cfg_save
// rewrites the file, and neither belongs inside the sensor thread's critical section.
static void save_bias(const double bias[3], const char *pad, double spread) {
    char buf[32];
    snprintf(buf, sizeof buf, "%.6f", bias[0]);
    cfg_set("TOTK_GYRO_BIAS_X", buf);
    snprintf(buf, sizeof buf, "%.6f", bias[1]);
    cfg_set("TOTK_GYRO_BIAS_Y", buf);
    snprintf(buf, sizeof buf, "%.6f", bias[2]);
    cfg_set("TOTK_GYRO_BIAS_Z", buf);
    cfg_set("TOTK_GYRO_BIAS_PAD", pad);
    // Saved now rather than at the menu's save tick, which may not run if the menu is closed first.
    if (cfg_save())
        rt_log("motion: gyro bias calibrated from %d samples: %.5f %.5f %.5f rad/s (spread %.4f rad/s)", CALIB_SAMPLES,
               bias[0], bias[1], bias[2], spread);
    else
        rt_log("motion: gyro bias %.5f %.5f %.5f rad/s could not be written to totk.ini; it applies to this run only",
               bias[0], bias[1], bias[2]);
}

// The one timeout decision, for the sample path and the menu's poll alike: CALIB_TIMEOUT_NS without a sample since the
// attempt started or since its last sample, so a slow sensor tick only lengthens the attempt. now may trail a sample
// the sensor thread stored meanwhile; that is no timeout. Called with m->lock held; the caller logs, with the lock
// released. A bias that is already in effect stays in effect and the row keeps reporting it; a controller with no
// bias shows the failure.
static int calib_check_timeout(MotionFusion *m, uint64_t now) {
    uint64_t since = m->last_ns > m->calib_start_ns ? m->last_ns : m->calib_start_ns;
    if (!m->calibrating || now <= since || now - since <= CALIB_TIMEOUT_NS) return 0;
    m->calibrating = 0;
    m->calib_result = m->bias_valid ? MOTION_CALIB_DONE : MOTION_CALIB_FAILED;
    return 1;
}

// The bias's own switch (gyro_calibration, live, default 1): off, the fusion takes the raw gyro, an attempt in
// progress is dropped, and the menu's calibrate entry does nothing. The saved keys stay untouched, so turning it
// back on restores the last calibration.
int motion_calibration_enabled(void) {
    static int on = -1;
    static unsigned seen;
    unsigned v = cfg_version();
    if (on < 0 || v != seen) {
        seen = v;
        on = cfg_bool("TOTK_GYRO_CALIBRATION");
    }
    return on;
}

// One calibration sample, m->lock held. The first CALIB_SETTLE samples are skipped: the button press that started the
// attempt shakes the controller. The next CALIB_SAMPLES are averaged per axis into the bias, unless their spread shows
// the controller moved (CALIB_MAX_SPREAD). Returns 1 when the attempt ended with a new bias, -1 when it was rejected,
// 0 while it goes on; *spread receives the largest per-axis standard deviation of a finished attempt.
static int calib_sample(MotionFusion *m, const double g[3], double *spread) {
    if (++m->calib_count <= CALIB_SETTLE) return 0;
    for (int i = 0; i < 3; i++) {
        m->calib_sum[i] += g[i];
        m->calib_sq[i] += g[i] * g[i];
    }
    if (m->calib_count < CALIB_SETTLE + CALIB_SAMPLES) return 0;
    m->calibrating = 0;
    double mean[3], worst = 0;
    for (int i = 0; i < 3; i++) {
        mean[i] = m->calib_sum[i] / CALIB_SAMPLES;
        double var = m->calib_sq[i] / CALIB_SAMPLES - mean[i] * mean[i];
        double sd = var > 0 ? sqrt(var) : 0;
        if (sd > worst) worst = sd;
    }
    *spread = worst;
    if (worst > CALIB_MAX_SPREAD) {
        m->calib_result = MOTION_CALIB_MOVED;   // the bias in effect, if any, stays
        return -1;
    }
    memcpy(m->gyro_bias, mean, sizeof mean);
    m->bias_valid = 1;
    m->calib_result = MOTION_CALIB_DONE;
    m->restart = 1;   // the angle and direction start over from the new bias
    drift_clear(m);   // what it learned was the old bias's remainder
    return 1;
}

void motion_feed(MotionFusion *m, const double acc[3], const double gyro_raw[3], uint64_t now) {
    // Read before the lock: a settings change takes cfg.c's lock.
    const int calib_on = motion_calibration_enabled(), auto_on = motion_auto_bias_enabled();
    double saved[3] = {0, 0, 0}, spread = 0, drift[3] = {0, 0, 0};
    char saved_pad[sizeof m->calib_pad] = "";
    int ended = 0, timed_out = 0, samples = 0, settled = 0;
    uint64_t started = 0;
    plat_mutex_lock(&m->lock);
    double dt = m->last_ns && now > m->last_ns ? (double)(now - m->last_ns) / 1e9 : 0.004;
    if (dt > 0.05) {   // a gap (no controller, sensors were off): no integration and no rest across it
        dt = 0.004;
        m->rest_s = 0;
        m->win_n = m->win_i = 0;
    }
    // Calibration: the player holds the controller still, so the raw gyro reads the bias. With the switch off an
    // attempt in progress is dropped.
    if (m->calibrating && !calib_on) {
        m->calibrating = 0;
        m->calib_result = m->bias_valid ? MOTION_CALIB_DONE : MOTION_CALIB_NONE;
    }
    if (m->calibrating) {
        if (calib_check_timeout(m, now)) {
            timed_out = 1;
            samples = m->calib_count;
            started = m->calib_start_ns;
        } else if ((ended = calib_sample(m, gyro_raw, &spread)) != 0) {
            memcpy(saved, ended > 0 ? m->gyro_bias : m->calib_sum, sizeof saved);
            if (ended < 0)
                for (int i = 0; i < 3; i++) saved[i] /= CALIB_SAMPLES;
            snprintf(saved_pad, sizeof saved_pad, "%s", m->calib_pad);
        }
    }
    m->last_ns = now;   // after the timeout check, which measures the gap before this sample
    // The bias applies only while the switch is on; with no bias measured it is zero and the gyro passes unchanged.
    double gyro[3];
    for (int i = 0; i < 3; i++) gyro[i] = gyro_raw[i] - (calib_on ? m->gyro_bias[i] : 0);
    // A sample without a reading (a sensor just switched on reads zero) neither starts a frame nor counts as rest.
    const int reading = norm3(acc) >= 0.5;
    if (!auto_on) {
        if (m->drift_base >= 0) drift_clear(m);
    } else if (reading && (settled = drift_track(m, acc, gyro, calib_on, dt)))
        memcpy(drift, m->drift, sizeof drift);
    for (int i = 0; i < 3; i++) gyro[i] -= m->drift[i];   // zero while the switch is off
    if (m->restart && !reading) goto unlock;   // no state yet; the logs below still run
    if (m->restart) {
        align_to(m->R, acc);
        memset(m->angle, 0, sizeof m->angle);
        m->restart = 0;
    } else integrate(m->R, gyro, acc, dt);
    MotionState *s = &m->ring[m->produced % MOTION_RING];
    *s = (MotionState){.delta_ns = (int64_t)(dt * 1e9), .sampling = (int64_t)m->produced + 1, .attributes = 1};
    for (int i = 0; i < 3; i++) {
        double rps = gyro[i] / (2 * M_PI);
        m->angle[i] += rps * dt;
        s->acc[i] = (float)acc[i];
        s->gyro[i] = (float)rps;
        s->angle[i] = (float)m->angle[i];
        for (int j = 0; j < 3; j++) s->dir[i * 3 + j] = (float)m->R[j][i];   // direction i = controller axis i in the reference frame
    }
    m->produced++;
unlock:
    // The logs run with the lock released: the log back end can be slow, and the menu thread holds this lock in
    // motion_calib_poll.
    plat_mutex_unlock(&m->lock);
    if (settled)
        rt_log("motion: gyro drift at rest learned: %.5f %.5f %.5f rad/s, taken off %s", drift[0], drift[1], drift[2],
               calib_on ? "on top of the calibrated bias" : "the raw gyro");
    if (timed_out)
        rt_log("motion: gyro calibration timed out after %.1f s (%d samples): sensor data not arriving",
               (now - started) / 1e9, samples);
    if (ended < 0)
        rt_log("motion: gyro calibration rejected, the controller moved: spread %.4f rad/s over the limit %.2f "
               "(mean %.5f %.5f %.5f rad/s)", spread, CALIB_MAX_SPREAD, saved[0], saved[1], saved[2]);
    if (ended > 0) {
        if (m->persist) save_bias(saved, saved_pad, spread);
        else rt_log("motion: gyro bias calibrated: %.5f %.5f %.5f rad/s (spread %.4f rad/s)", saved[0], saved[1],
                    saved[2], spread);
    }
}

int motion_read(MotionFusion *m, MotionState *out, int max, uint64_t now) {
    if (max <= 0) return 0;
    int n = 0;
    plat_mutex_lock_shared(&m->lock);
    if (m->produced > m->first && (now < m->last_ns || now - m->last_ns < STALE_NS)) {
        uint64_t have = m->produced - m->first < MOTION_RING ? m->produced - m->first : MOTION_RING;
        n = (int)(have < (uint64_t)max ? have : (uint64_t)max);
        for (int i = 0; i < n; i++) out[i] = m->ring[(m->produced - 1 - (uint64_t)i) % MOTION_RING];
    }
    plat_mutex_unlock_shared(&m->lock);
    return n;
}

void motion_restart(MotionFusion *m) {
    plat_mutex_lock(&m->lock);
    m->restart = 1;
    m->first = m->produced;
    m->last_ns = 0;   // the first sample after the restart integrates nothing across the stop
    memset(m->angle, 0, sizeof m->angle);
    plat_mutex_unlock(&m->lock);
}

void motion_calibrate_start(MotionFusion *m, const char *pad, uint64_t now) {
    plat_mutex_lock(&m->lock);
    m->calibrating = 1;
    m->calib_count = 0;
    memset(m->calib_sum, 0, sizeof m->calib_sum);
    memset(m->calib_sq, 0, sizeof m->calib_sq);
    m->calib_start_ns = now;
    snprintf(m->calib_pad, sizeof m->calib_pad, "%s", pad ? pad : "");
    plat_mutex_unlock(&m->lock);
}

// One axis of totk.ini; a value that is not a number leaves the axis at zero and is named in the log, so a
// hand-edited or damaged line does not silently read as a calibrated zero.
static double bias_value(const char *key) {
    const char *v = cfg_get(key);
    if (!v || !*v) return 0;
    char *end;
    double d = strtod(v, &end);
    if (end == v || *end || !isfinite(d)) {
        rt_log("motion: %s = \"%s\" is not a number; that axis starts uncalibrated", key, v);
        return 0;
    }
    return d;
}

void motion_bias_load(MotionFusion *m) {
    double bias[3] = {bias_value("TOTK_GYRO_BIAS_X"), bias_value("TOTK_GYRO_BIAS_Y"), bias_value("TOTK_GYRO_BIAS_Z")};
    plat_mutex_lock(&m->lock);
    memcpy(m->gyro_bias, bias, sizeof bias);
    m->bias_valid = 1;
    m->calib_result = MOTION_CALIB_DONE;
    drift_clear(m);
    plat_mutex_unlock(&m->lock);
}

void motion_bias_reset(MotionFusion *m) {
    plat_mutex_lock(&m->lock);
    memset(m->gyro_bias, 0, sizeof m->gyro_bias);
    // An attempt that was collecting belongs to the previous controller: its samples must not complete against this
    // one, and its name must not label a bias measured from another controller.
    m->calibrating = 0;
    m->calib_count = 0;
    m->calib_pad[0] = 0;
    m->calib_result = MOTION_CALIB_NONE;
    m->bias_valid = 0;
    drift_clear(m);   // the drift belonged to that controller too
    plat_mutex_unlock(&m->lock);
}

// Also ends an attempt whose samples stopped (the sensor thread no longer calls motion_feed): without this the row
// would read "Calibrating..." forever. Logs with the lock released: the menu thread calls this on every redraw.
int motion_calib_poll(MotionFusion *m, uint64_t now) {
    plat_mutex_lock(&m->lock);
    int timed_out = calib_check_timeout(m, now);
    int st = m->calibrating ? MOTION_CALIB_COLLECTING : m->calib_result;
    uint64_t start = m->calib_start_ns;
    int count = m->calib_count;
    plat_mutex_unlock(&m->lock);
    if (timed_out)
        rt_log("motion: gyro calibration timed out after %.1f s (%d samples): the sensor thread stopped feeding it",
               (now - start) / 1e9, count);
    return st;
}
