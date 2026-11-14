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

#include "host.h"
#include "input.h"
#include "platform/platform.h"
// ========== Gyro zero-bias fix added code starts ==========
#include <stdbool.h>

#define GYRO_WIN 16

typedef struct {
    double bias[3];
    bool have_bias;
    double acc[3];
    bool have_acc;
    double rest_ms;
    double gwin[GYRO_WIN][3];
    double awin[GYRO_WIN][3];
    int gn, gi, an, ai;
    double learn_ms;
} GyroBiasState;

static GyroBiasState g_gyro = {0};

static void push_win(double w[GYRO_WIN][3], int *n, int *idx, const double v[3]) {
    for (int i = 0; i < 3; i++) w[*idx][i] = v[i];
    *idx = (*idx + 1) % GYRO_WIN;
    if (*n < GYRO_WIN) (*n)++;
}

static void mean_win(double w[GYRO_WIN][3], int count, double out[3]) {
    for (int i = 0; i < 3; i++) {
        double s = 0;
        for (int j = 0; j < count; j++) s += w[j][i];
        out[i] = count ? s / count : 0;
    }
}

static double ptp_win(double w[GYRO_WIN][3], int count) {
    double worst = -1;
    for (int i = 0; i < 3; i++) {
        double lo = 1e18, hi = -1e18;
        for (int j = 0; j < count; j++) {
            if (w[j][i] < lo) lo = w[j][i];
            if (w[j][i] > hi) hi = w[j][i];
        }
        double d = hi - lo;
        if (d > worst) worst = d;
    }
    return worst < 0 ? 0 : worst;
}
// ========== Gyro zero-bias fix added code ends ==========

enum { MOTION_RING = 64 };
#define STALE_NS 200000000ull   // no sample for 200 ms: no motion

struct MotionFusion {
    PlatMutex lock;
    MotionState ring[MOTION_RING];
    uint64_t produced, last_ns;
    uint64_t first;   // produced at the last restart: older samples are not read
    double R[3][3];   // controller -> reference; row i = reference axis i in controller coordinates
    double angle[3];
    int restart;
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

void motion_feed(MotionFusion *m, const double acc[3], const double gyro[3], uint64_t now) {
    plat_mutex_lock(&m->lock);
    double dt = m->last_ns && now > m->last_ns ? (double)(now - m->last_ns) / 1e9 : 0.004;
    if (dt > 0.05) dt = 0.004;   // a gap (no controller, sensors were off): no integration across it
    m->last_ns = now;
    
    // Copy to apply modifications without changing the original input
    double clean_gyro[3] = {gyro[0], gyro[1], gyro[2]}; 

    if (m->restart) {
        align_to(m->R, acc);
        memset(m->angle, 0, sizeof m->angle);
        m->restart = 0;
        // Reset bias state on restart
        memset(&g_gyro, 0, sizeof(g_gyro));
    } else {
        // ========== Gyro zero-bias fix core logic starts ==========
        double dt_ms = dt * 1000.0;
        
        // Note: The source code's acc unit is G, so normal gravity is 1.0
        push_win(g_gyro.awin, &g_gyro.an, &g_gyro.ai, acc);
        g_gyro.have_acc = true;
        // Use raw gyro to calculate stability
        push_win(g_gyro.gwin, &g_gyro.gn, &g_gyro.gi, gyro); 

        double mag = sqrt(acc[0]*acc[0] + acc[1]*acc[1] + acc[2]*acc[2]);
        double gsp = ptp_win(g_gyro.gwin, g_gyro.gn);
        double asp = g_gyro.an >= GYRO_WIN ? ptp_win(g_gyro.awin, g_gyro.an) : 1e9;
        double m_mean[3];
        mean_win(g_gyro.gwin, g_gyro.gn, m_mean);

        // Acc unit is G, so the threshold is within 25% of 1.0
        int stable = (g_gyro.gn >= GYRO_WIN) && (g_gyro.an >= GYRO_WIN) && 
                     g_gyro.have_acc &&
                     (fabs(mag - 1.0) < 0.25) &&
                     (gsp < 0.040) && (asp < 0.50);

        double lim = g_gyro.have_bias ? 0.050 : 0.150;
        double dev = 0;
        for (int i = 0; i < 3; i++) {
            double d = fabs(m_mean[i] - g_gyro.bias[i]);
            if (d > dev) dev = d;
        }
        int at_rest = stable && (dev < lim);

        if (at_rest) {
            g_gyro.rest_ms += dt_ms;
            if (g_gyro.rest_ms > 400.0) {
                double tau = g_gyro.have_bias ? 30000.0  : 250.0;
                double alpha = dt_ms / tau;
                if (alpha > 0.25) alpha = 0.25;
                for (int i = 0; i < 3; i++) {
                    g_gyro.bias[i] += alpha * (m_mean[i] - g_gyro.bias[i]);
                    if (g_gyro.bias[i] > 0.3) g_gyro.bias[i] = 0.3;
                    if (g_gyro.bias[i] < -0.3) g_gyro.bias[i] = -0.3;
                }
                g_gyro.learn_ms += dt_ms;
                if (g_gyro.learn_ms > 1500.0 && dev < 0.010) g_gyro.have_bias = true;
            }
        } else {
            g_gyro.rest_ms = 0;
            if (dev > 0.10) g_gyro.learn_ms = 0;
        }

        // Subtract bias and apply deadzone
        for (int i = 0; i < 3; i++) {
            double v = gyro[i] - g_gyro.bias[i];
            if (fabs(v) < 0.006) v = 0;
            clean_gyro[i] = v;
        }
        // ========== Gyro zero-bias fix core logic ends ==========

        // Pass the clean gyro data to the original integration function
        integrate(m->R, clean_gyro, acc, dt);
    }

    MotionState *s = &m->ring[m->produced % MOTION_RING];
    *s = (MotionState){.delta_ns = (int64_t)(dt * 1e9), .sampling = (int64_t)m->produced + 1, .attributes = 1};
    for (int i = 0; i < 3; i++) {
        double rps = clean_gyro[i] / (2 * M_PI); // Use clean_gyro here as well
        m->angle[i] += rps * dt;
        s->acc[i] = (float)acc[i];
        s->gyro[i] = (float)rps;
        s->angle[i] = (float)m->angle[i];
        for (int j = 0; j < 3; j++) s->dir[i * 3 + j] = (float)m->R[j][i];
    }
    m->produced++;
    plat_mutex_unlock(&m->lock);
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
