// Adapted from MacNativeGames games/totk/runtime/motion_fusion.c @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames
// contributors).
// Six-axis sensor states (nn::hid::SixAxisSensorState) from gyro and accelerometer samples. Input per sample, in the
// Switch controller frame (x to the right, y toward the top edge, z out of the face): acceleration in G including
// gravity, as gravity reads (lying face up z = -1 G), and rotation rate in rad/s. Output per sample, as the Switch
// reports it: acceleration in G, angular velocity in revolutions per second, angle = the integrated revolutions per
// axis, direction = the controller's three axes in a reference frame whose z is up. The orientation is integrated from
// the gyro and, while the controller is not being accelerated (|a| within 15 % of 1 G), pulled toward the measured
// gravity (Mahony, gain 0.5/s: about 2 s), as the Switch's sensor fusion does; yaw has no reference and drifts with the
// gyro's bias. The newest MOTION_RING samples are kept; reads are newest first. A restart (StartSixAxisSensor) drops
// the samples before it: reads return nothing until the next sample, which starts a fresh frame and angle; sampling
// numbers go on. The feeding thread writes, guest threads read, both under the instance's SRW lock.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "platform/platform.h"

enum { MOTION_RING = 64, CALIB_SAMPLES = 250 };
#define STALE_NS 200000000ull   // no sample for 200 ms: no motion
#define CALIB_TIMEOUT_NS 5000000000ull   // 5 s: give up if sensor data stops arriving mid-calibration

struct MotionFusion {
    PlatMutex lock;
    MotionState ring[MOTION_RING];
    uint64_t produced, last_ns;
    uint64_t first;   // produced at the last restart: older samples are not read
    double R[3][3];   // controller -> reference; row i = reference axis i in controller coordinates
    double angle[3];
    int restart;
    double gyro_bias[3];   // estimated resting bias (rad/s), subtracted from the gyro
    int calibrating;       // 1: collecting resting samples for a new bias
    double calib_sum[3];   // accumulated gyro samples during calibration
    int calib_count;       // samples collected so far
    uint64_t calib_start_ns;   // when calibration started (timeout check)
    char calib_pad[128];      // controller name that was calibrated (for loading the bias on the next run)
    int calib_result;         // 0 no attempt or one in progress, 1 the last attempt finished, -1 it timed out
    int bias_valid;           // 1 when gyro_bias holds a real estimate (loaded or measured), so a zero is a value
};

MotionFusion *motion_new(void) {
    MotionFusion *m = calloc(1, sizeof *m);
    if (!m) rt_fatal("motion: out of memory");
    plat_mutex_init(&m->lock);
    m->restart = 1;
    return m;
}

static MotionFusion *global;

static void global_init(void *arg) {
    (void)arg;
    global = motion_new();
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
// A fresh orientation: z up against the measured gravity, x as close to the controller's x as that allows.
static void align_to(double R[3][3], const double acc[3]) {
    double n = norm3(acc), up[3] = {0, 0, 1}, x[3] = {1, 0, 0};
    if (n > 0.5)
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
        const double kp = 0.5;
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

// Write the calibrated bias to totk.ini. Called with m->lock released: cfg_set takes cfg.c's own lock and cfg_save
// rewrites the file, and neither belongs inside the sensor thread's critical section.
static void save_bias(const double bias[3], const char *pad, int samples) {
    char buf[32];
    snprintf(buf, sizeof buf, "%.6f", bias[0]);
    cfg_set("TOTK_GYRO_BIAS_X", buf);
    snprintf(buf, sizeof buf, "%.6f", bias[1]);
    cfg_set("TOTK_GYRO_BIAS_Y", buf);
    snprintf(buf, sizeof buf, "%.6f", bias[2]);
    cfg_set("TOTK_GYRO_BIAS_Z", buf);
    if (pad[0]) cfg_set("TOTK_GYRO_BIAS_PAD", pad);
    // Saved now rather than at the menu's save tick, which may not run if the menu is closed first.
    if (cfg_save())
        rt_log("motion: gyro bias calibrated from %d samples: %.5f %.5f %.5f rad/s", samples, bias[0], bias[1], bias[2]);
    else
        rt_log("motion: gyro bias %.5f %.5f %.5f rad/s could not be written to totk.ini; it applies to this run only",
               bias[0], bias[1], bias[2]);
}

// The one timeout decision, for the sample path and the menu's poll alike, so the two cannot drift apart. Called
// with m->lock held; the caller logs, with the lock released.
static int calib_check_timeout(MotionFusion *m, uint64_t now) {
    if (!m->calibrating || now - m->calib_start_ns <= CALIB_TIMEOUT_NS) return 0;
    m->calibrating = 0;
    // A bias that is already in effect stays in effect, and stays reported as calibrated: the row describes the bias
    // that is applied, and the failed attempt is in the log. A controller with no bias at all shows the failure.
    m->calib_result = m->bias_valid ? 1 : -1;
    return 1;
}

// Copy a name into cap bytes without splitting a UTF-8 sequence; the same as input.c's copy_name, which this file
// cannot include from.
static void copy_name(char *dst, size_t cap, const char *src) {
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;   // back off to the start of the character
    }
    memcpy(dst, src, n);
    dst[n] = 0;
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

void motion_feed(MotionFusion *m, const double acc[3], const double gyro_raw[3], uint64_t now) {
    double saved[3] = {0, 0, 0};
    char saved_pad[sizeof m->calib_pad] = "";
    int save = 0, samples = 0, first = 0, logged_timeout = 0;
    uint64_t saved_start = 0;
    plat_mutex_lock(&m->lock);
    double dt = m->last_ns && now > m->last_ns ? (double)(now - m->last_ns) / 1e9 : 0.004;
    if (dt > 0.05) dt = 0.004;   // a gap (no controller, sensors were off): no integration across it
    m->last_ns = now;
    // Calibration: collect gyro samples for CALIB_SAMPLES frames. The user holds the controller still, so the
    // average of the raw gyro (which reads the bias at rest) is the bias estimate. No motion threshold: the bias
    // itself can exceed any absolute threshold, which would block collection forever. calib_check_timeout decides
    // the deadline for both this path and the menu's poll, so the two cannot drift apart. With the calibration
    // switch off, an attempt in progress is dropped and no new one starts; the bias stops applying below.
    if (m->calibrating && !motion_calibration_enabled()) {
        m->calibrating = 0;
        m->calib_result = 0;
    }
    if (m->calibrating) {
        if (calib_check_timeout(m, now)) {
            logged_timeout = 1;
            samples = m->calib_count;
            saved_start = m->calib_start_ns;
        }
        if (m->calibrating) {
            for (int i = 0; i < 3; i++) m->calib_sum[i] += gyro_raw[i];
            m->calib_count++;
            first = m->calib_count == 1;
            if (m->calib_count >= CALIB_SAMPLES) {
                for (int i = 0; i < 3; i++) m->gyro_bias[i] = m->calib_sum[i] / m->calib_count;
                m->calibrating = 0;
                m->calib_result = 1;
                m->bias_valid = 1;
                m->restart = 1;
                // Hand the result to save_bias below, which runs with the lock released.
                memcpy(saved, m->gyro_bias, sizeof saved);
                snprintf(saved_pad, sizeof saved_pad, "%s", m->calib_pad);
                samples = m->calib_count;
                save = 1;
            }
        }
    }
    // Subtract the bias from the gyro for integration and storage.
    // The bias applies only while the switch is on: off, the fusion sees the raw gyro again.
    const int calib_on = motion_calibration_enabled();
    double gyro[3] = {gyro_raw[0] - (calib_on ? m->gyro_bias[0] : 0),
                      gyro_raw[1] - (calib_on ? m->gyro_bias[1] : 0),
                      gyro_raw[2] - (calib_on ? m->gyro_bias[2] : 0)};
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
    // The logs run with the lock released: the log back end can be slow, and the menu thread holds this lock in
    // motion_calib_poll.
    plat_mutex_unlock(&m->lock);
    if (first)
        rt_log("motion: calibration started, first raw gyro: %.5f %.5f %.5f rad/s", gyro_raw[0], gyro_raw[1],
               gyro_raw[2]);
    if (logged_timeout) {
        double secs = (now - saved_start) / 1e9;   // captured under the lock; another attempt may have begun since
        rt_log("motion: gyro calibration timed out after %.1f s (%d samples): sensor data not arriving", secs, samples);
    }
    if (save) save_bias(saved, saved_pad, samples);
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

void motion_calibrate_start(MotionFusion *m, const char *pad_name) {
    plat_mutex_lock(&m->lock);
    m->calibrating = 1;
    m->calib_result = 0;
    m->calib_count = 0;
    m->calib_sum[0] = m->calib_sum[1] = m->calib_sum[2] = 0;
    m->calib_start_ns = kern_now_ns();
    // The caller passes the same name it put into its own row label; copied again here so the fusion keeps the name
    // the bias belongs to without relying on another buffer's lifetime. Same UTF-8-safe cut as input.c's copy_name:
    // a name cut mid-sequence would reach totk.ini and the next run's comparisons.
    copy_name(m->calib_pad, sizeof m->calib_pad, pad_name);
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
    double bias[3] = {bias_value("TOTK_GYRO_BIAS_X"), bias_value("TOTK_GYRO_BIAS_Y"),
                      bias_value("TOTK_GYRO_BIAS_Z")};
    plat_mutex_lock(&m->lock);
    memcpy(m->gyro_bias, bias, sizeof bias);
    m->bias_valid = 1;
    m->calib_result = 1;   // 1 means "a bias is in effect", whether measured or loaded
    plat_mutex_unlock(&m->lock);
}

void motion_bias_reset(MotionFusion *m) {
    plat_mutex_lock(&m->lock);
    m->gyro_bias[0] = m->gyro_bias[1] = m->gyro_bias[2] = 0;
    // A calibration that was running belonged to the previous controller: its samples must not complete against this
    // one, and its name must not label a bias measured from another controller.
    m->calibrating = 0;
    m->calib_count = 0;
    m->calib_sum[0] = m->calib_sum[1] = m->calib_sum[2] = 0;
    m->calib_pad[0] = 0;
    m->calib_result = 0;
    m->bias_valid = 0;
    plat_mutex_unlock(&m->lock);
}

// The timeout, for the case motion_feed is not called at all (the sensor thread stalled): without this the row would
// read "Calibrating..." forever, which is the failure the timeout exists to end. Returns the row's state: 0 none,
// 1 collecting, 2 calibrated, 3 the last attempt timed out. Logs with the lock released: the menu thread holds this
// lock briefly on every redraw, and the sensor thread waits for it.
int motion_calib_poll(MotionFusion *m, uint64_t now) {
    plat_mutex_lock(&m->lock);
    int timed_out = calib_check_timeout(m, now);
    int st = m->calibrating ? 1 : m->calib_result > 0 ? 2 : m->calib_result < 0 ? 3 : 0;
    uint64_t start = m->calib_start_ns;
    int count = m->calib_count;
    plat_mutex_unlock(&m->lock);
    if (timed_out) {
        rt_log("motion: gyro calibration timed out after %.1f s (%d samples): the sensor thread stopped feeding it",
               (now - start) / 1e9, count);
    }
    return st;
}
