// The mouse as the motion of a held controller (TOTK_MOUSE_GYRO, input.c motion_main). Steam Input, for one, turns a
// controller's gyro into mouse motion with the curves and activators the player set up; fed back as six-axis states,
// the game's own gyro aiming runs on it, where the mouse camera would turn it into a right stick.
// The game aims sideways by the rotation rate and up and down by the controller's tilt. In 1.2.1 the bow camera turns
// with a yaw rate under a direction held still, and not with a direction turning sideways under no rate; and a pitch
// rate under a direction held still does not move it up or down at all (mouse_gyro_test = rate and pose, and a build
// that held the pose still). So mouse up and down tilt the controller, R = Rx(pitch) from MOUSE_GYRO_REST_PITCH,
// controller to world, in the Switch controller frame (x right, y toward the top edge, z out of the face;
// motion_fusion.c), with the pitch rate the tilt's own change; mouse left and right are a yaw rate about world up seen
// from the tilted controller (on y and z by the tilt), and the direction never turns sideways. Mouse right turns right
// (yaw down: the gyro's z is counter-clockwise positive), mouse up raises the top edge, and `dots` counts make a full
// turn. The tilt stops at +-MOUSE_GYRO_PITCH_MAX from flat: counts past it are dropped, so the tilt turns back at once.
// It rests above flat because the game takes a state with an identity direction, or no acceleration, for no sensor at
// all (main+0x2ae6dc4 marks the whole read invalid); the acceleration is gravity in the tilted pose.
// The game copies only the newest state's rotation and direction into its input snapshot, once a frame
// (main+0x20e289c), so a tick without counts in the middle of a movement reads as the aim stopping there. Each tick's
// counts are therefore spread evenly over the next ticks, as many as the game's frame takes (input.c), which keeps
// their sum: the counts between two frames all show in the second. Longer pauses inside a movement are left alone:
// Steam Input's gyro to mouse through SISR arrived in bursts up to 50 ms apart only with a touch activator, which
// drops out during fast flicks (mouse_gyro_test = stream measured it), and with a button its counts come every tick
// except at slow speeds, where spreading over the pauses would only add delay.
// The snapshot also carries the count of new samples, so the samples keep the sensor's own schedule, one per
// MOUSE_GYRO_TICK_NS (mouse_gyro_due): the feeding thread can wake late while the game keeps the CPU busy (6 to 12 ms
// instead of 5 about ten times a second), and one sample for two periods would show the game half the time passed.
#include <math.h>
#include <string.h>

#include "input.h"

#define TEST_RATE (30 * M_PI / 180)   // the diagnostic turn, radians per second
#define STEEP_PITCH (60 * M_PI / 180)   // the tilt of the steep diagnostic

void mouse_gyro_init(MouseGyro *g) {
    memset(g, 0, sizeof *g);
    g->pitch = MOUSE_GYRO_REST_PITCH;
}

int mouse_gyro_due(uint64_t *next, uint64_t now, uint64_t *first) {
    if (*next && now < *next) return 0;
    int n = 1;
    if (*next && (now - *next) / MOUSE_GYRO_TICK_NS < MOUSE_GYRO_CATCHUP) {
        n += (int)((now - *next) / MOUSE_GYRO_TICK_NS);
        *first = *next;
    } else *first = now;
    *next = *first + (uint64_t)n * MOUSE_GYRO_TICK_NS;
    return n;
}

// A pose turned by yaw about world up with the top edge pitch above flat: R = Rz(yaw) Rx(pitch), controller to world.
// Its direction (MotionState.dir: component j of controller axis i at [i * 3 + j], the columns of R) and the
// acceleration gravity gives in it (R transposed times (0, 0, -1)). The mouse source keeps the yaw at 0.
static void pose(double yaw, double pitch, double acc[3], double dir[9]) {
    double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), sp = sin(pitch);
    const double d[9] = {cy, sy, 0, -sy * cp, cy * cp, sp, sy * sp, -cy * sp, cp};
    memcpy(dir, d, sizeof d);
    acc[0] = 0;
    acc[1] = -sp;
    acc[2] = -cp;
}

// Body rotation of a pose at this pitch turning at these rates (rad/s): R^T dR/dt = (pitch rate, sin(pitch) * yaw
// rate, cos(pitch) * yaw rate), the yaw about world up seen from the tilted controller.
static void rate(double pitch, double yaw_rate, double pitch_rate, double gyro[3]) {
    gyro[0] = pitch_rate;
    gyro[1] = sin(pitch) * yaw_rate;
    gyro[2] = cos(pitch) * yaw_rate;
}

void mouse_gyro_step(MouseGyro *g, int32_t dx, int32_t dy, double dt, double dots, int window, double acc[3],
                     double gyro[3], double dir[9]) {
    if (!(dt > 0)) dt = 0.005;
    if (window < 1) window = 1;
    if (window > MOUSE_GYRO_WINDOW_MAX) window = MOUSE_GYRO_WINDOW_MAX;
    double k = dots > 0 ? 2 * M_PI / dots : 0, y = -dx * k / window, p = -dy * k / window;
    for (int i = 0; i < window; i++) {
        int s = (g->head + i) % MOUSE_GYRO_WINDOW_MAX;
        g->due[0][s] += y;
        g->due[1][s] += p;
    }
    double dyaw = g->due[0][g->head], to = g->pitch + g->due[1][g->head];
    g->due[0][g->head] = g->due[1][g->head] = 0;
    g->head = (g->head + 1) % MOUSE_GYRO_WINDOW_MAX;
    if (to > MOUSE_GYRO_PITCH_MAX) to = MOUSE_GYRO_PITCH_MAX;
    if (to < -MOUSE_GYRO_PITCH_MAX) to = -MOUSE_GYRO_PITCH_MAX;
    double dpitch = to - g->pitch, mid = g->pitch + dpitch / 2;
    g->yaw += dyaw;
    g->pitch = to;
    pose(0, g->pitch, acc, dir);
    rate(mid, dyaw / dt, dpitch / dt, gyro);
}

void mouse_gyro_test_step(MouseGyro *g, int mode, double dt, double acc[3], double gyro[3], double dir[9]) {
    if (!(dt > 0)) dt = 0.005;
    if (mode == MOUSE_GYRO_TEST_POSE) {
        g->yaw += TEST_RATE * dt;
        pose(g->yaw, g->pitch, acc, dir);
        gyro[0] = gyro[1] = gyro[2] = 0;
    } else {
        double tilt = mode == MOUSE_GYRO_TEST_STEEP ? STEEP_PITCH : g->pitch;
        pose(0, tilt, acc, dir);
        rate(tilt, TEST_RATE, 0, gyro);
    }
}

void mouse_gyro_flick(MouseFlick *f, double dt, double dots, int32_t *dx, int32_t *dy) {
    static const struct { double from, to, yaw, pitch; } moves[] = {   // seconds into the cycle, degrees a second
        {0.00, 0.15, 300, 0}, {0.60, 0.75, -300, 0}, {1.20, 1.30, 0, 200}, {1.75, 1.85, 0, -200}};
    double s = fmod(f->t + dt / 2, 2.4), yaw = 0, pitch = 0;   // the middle of the tick: no edge falls on it
    for (size_t i = 0; i < sizeof moves / sizeof *moves; i++)
        if (s >= moves[i].from && s < moves[i].to) yaw = moves[i].yaw, pitch = moves[i].pitch;
    f->t += dt;
    f->cx += yaw * dt * dots / 360;
    f->cy -= pitch * dt * dots / 360;   // mouse up is up
    *dx = (int32_t)f->cx;
    *dy = (int32_t)f->cy;
    f->cx -= *dx;
    f->cy -= *dy;
}
