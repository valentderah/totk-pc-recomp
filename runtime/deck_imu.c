// In Gaming Mode Steam Input takes the Deck's controller and hands the game a virtual gamepad without motion sensors,
// so SDL sees no gyroscope on it (steam_reveal in input.c un-hides the real device from SDL for every other
// controller, but Valve's own keeps getting its settings reset by Steam/SDL's Steam Deck driver out from under that).
// The controller (28de:1205, interface 2) still sends its state report to every reader of its hidraw node, 250 times
// a second, motion included, but only while something keeps asking it for raw accelerometer and gyroscope data
// (deck_imu_wake): otherwise Steam puts it in an idle mode that reads as a controller at rest, since Steam itself
// only asks for raw motion while its own active layout uses the gyro. Report layout, scale and the wake feature
// report cross-checked against SDL's HIDAPI Steam Deck driver (zlib) and kmicki/SteamDeckGyroDSU (MIT); an
// independent implementation. The hidraw read approach started as a port of Savage Wind Recomp's
// runtime/deck_imu.c (MIT License, Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors; Copyright
// (c) 2026 Peiikai), adapted to feed TOTK's motion_feed directly instead of a DSU packet; the wake poke is this
// project's own addition (without it, Savage Wind's approach alone reads live data for only a few samples before
// Steam idles the sensor again).
#include "deck_imu.h"
#include "host.h"
#include <string.h>
#ifdef __linux__
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/hidraw.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

static int16_t le16s(const uint8_t *p) { return (int16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

bool deck_imu_decode(const uint8_t *p, size_t n, double acc[3], double gyro[3], uint32_t *num) {
    // Header: report 1, type 9 (the Deck's state), 64 bytes. Then the packet number, buttons and pads; the
    // accelerometer at 24 and the gyroscope at 30 (x, y, z, signed 16-bit: 2 G and 2000 degrees per second at full
    // scale). The raw axes already line up with nn::hid's six-axis convention (flat on a table: acc = (0, 0, -1)),
    // so no further axis remap is applied here (unlike input_motion_axes, which remaps SDL's generic gamepad-sensor
    // convention for every other controller).
    if (n < 36 || p[0] != 1 || p[2] != 9 || p[3] != 64) return false;
    double a[3], w[3];
    for (int i = 0; i < 3; i++) a[i] = le16s(p + 24 + 2 * i) / 16384.0, w[i] = le16s(p + 30 + 2 * i);
    if (a[0] * a[0] + a[1] * a[1] + a[2] * a[2] < 0.01) return false;   // no motion in the report
    const double rad = 2000.0 / 32768 * 3.14159265358979323846 / 180;
    for (int i = 0; i < 3; i++) acc[i] = -a[i], gyro[i] = w[i] * rad;
    *num = le32(p + 4);
    return true;
}

void deck_imu_init(DeckImu *d) {
    memset(d, 0, sizeof *d);
    d->fd = -1;
}

void deck_imu_close(DeckImu *d) {
#ifdef __linux__
    if (d->fd >= 0) close(d->fd);
#endif
    d->fd = -1;
    d->fresh = d->have_num = false;
}

#ifdef __linux__
// The hidraw node of the controller's interface 2 (the one with the state reports), or -1. Also a virtual Deck
// controller (InputPlumber's deck-uhid target on other handhelds - Legion Go, ROG Ally - under Steam Input: 28de:12fe
// or 28de:1205 made through uhid, so no USB interface); the same state reports (deck_imu_decode checks them).
static int open_deck(char *name, size_t size) {
    DIR *dir = opendir("/sys/class/hidraw");
    if (!dir) return -1;
    int fd = -1;
    for (struct dirent *e; fd < 0 && (e = readdir(dir));) {
        if (strncmp(e->d_name, "hidraw", 6)) continue;
        char path[512], link[PATH_MAX], line[256];
        snprintf(path, sizeof path, "/sys/class/hidraw/%s/device/uevent", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        bool deck = false;
        while (fgets(line, sizeof line, f))
            deck |= !strncmp(line, "HID_ID=0003:000028DE:00001205", 29) || !strncmp(line, "HID_ID=0003:000028DE:000012FE", 29) ||
                    !strncmp(line, "HID_ID=0006:000028DE:00001205", 29) || !strncmp(line, "HID_ID=0006:000028DE:000012FE", 29);
        fclose(f);
        snprintf(path, sizeof path, "/sys/class/hidraw/%s/device", e->d_name);
        if (!deck || !realpath(path, link)) continue;   // the link itself is relative: "../../../0003:28DE:1205.0005"
        char *dev = strrchr(link, '/');   // ".../3-3:1.2/0003:28DE:1205.0005": the interface before it
        bool uhid = strstr(link, "/uhid/") != NULL;   // ".../virtual/misc/uhid/0003:28DE:12FE.0007"
        if (!uhid && (!dev || dev - link < 4 || memcmp(dev - 4, ":1.2", 4))) continue;
        snprintf(path, sizeof path, "/dev/%s", e->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) snprintf(name, size, "%s", path);
    }
    closedir(dir);
    return fd;
}
// Steam puts the controller in an idle IMU mode (reports read as all-zero, not even gravity) unless something keeps
// asking it for raw accelerometer and gyroscope data; Steam itself does this only while its own active layout uses
// the gyro. The feature report is SETTING_IMU_MODE (0x87), report id 3, value 48 = SEND_RAW_ACCEL | SEND_RAW_GYRO
// (cross-checked against SDL's HIDAPI Steam Controller driver, zlib).
static void deck_imu_wake(int fd) {
    unsigned char buf[65] = {0, 0x87, 3, 48, 0x18, 0x00};
    ioctl(fd, HIDIOCSFEATURE(sizeof buf), buf);
}
#endif

void deck_imu_poll(DeckImu *d, uint64_t now, DeckImuSampleFn sample, void *user) {
#ifdef __linux__
    if (d->fd < 0) {
        if (d->retry && now < d->retry) return;
        d->retry = now + 2 * DECK_IMU_PERIOD_NS * 500;   // ~4s
        char name[64];
        d->fd = open_deck(name, sizeof name);
        if (d->fd < 0) {
            if (!d->failed) rt_log("input: Steam Deck: no controller HID node to read its gyroscope from");
            d->failed = true;
            return;
        }
        rt_log("input: Steam Deck: gyroscope and accelerometer read from %s", name);
        d->failed = false;
        deck_imu_wake(d->fd);
    }
    static uint64_t next_wake;
    if (d->fresh && now - d->received >= DECK_IMU_PERIOD_NS * 50) d->fresh = d->have_num = false;
    if (!d->fresh && now >= next_wake) { next_wake = now + 2000000000ull; deck_imu_wake(d->fd); }
    static int read_n, decoded_n;
    static uint64_t next_stall_log;
    if (read_n > 50 && decoded_n == 0 && now >= next_stall_log) {
        next_stall_log = now + 2000000000ull;
        rt_log("input: Steam Deck: %d hidraw reports read, 0 decoded as a motion state report - wrong node or report "
               "format", read_n);
    }
    for (int i = 0; i < 256; i++) {
        uint8_t p[64];
        ssize_t n = read(d->fd, p, sizeof p);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) deck_imu_close(d);
            break;
        }
        read_n++;
        double acc[3], gyro[3];
        uint32_t num;
        if (!deck_imu_decode(p, (size_t)n, acc, gyro, &num)) continue;
        decoded_n++;
        if (d->have_num && num == d->num) continue;
        d->num = num, d->have_num = true;
        d->received = now, d->fresh = true;
        if (sample) sample(acc, gyro, now, user);
    }
#else
    (void)d; (void)now; (void)sample; (void)user;
#endif
}
