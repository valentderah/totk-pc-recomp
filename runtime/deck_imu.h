// The Steam Deck's own motion sensors, read straight from its controller's HID node (Linux), bypassing SDL's gamepad
// sensor API: Steam Input resets that controller's settings out from under SDL, so unhiding it (steam_reveal, input.c)
// is not enough for Valve's own pad specifically (it works for every other controller). No threads or blocking I/O.
// Ported (MIT, see LICENSE) from Savage Wind Recomp's runtime/deck_imu.c, which cross-checked the report layout
// against SDL's HIDAPI Steam Deck driver (zlib) and kmicki/SteamDeckGyroDSU (MIT).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DECK_IMU_PERIOD_NS 4000000ull   // one state report every 4 ms (250 Hz)

typedef struct {
    int fd;
    bool fresh, have_num, failed;
    uint32_t num;       // the last report's packet number
    uint64_t received;  // host time of the last report
    uint64_t retry;     // next time to look for the device
} DeckImu;

// One 64-byte state report -> fusion axes (acc in G, gyro in rad/s, already in nn::hid's six-axis convention), its
// packet number in *num. False when the report is not a motion state report, or reads as a controller at rest.
bool deck_imu_decode(const uint8_t *p, size_t n, double acc[3], double gyro[3], uint32_t *num);
void deck_imu_init(DeckImu *d);
void deck_imu_close(DeckImu *d);
// Callback per decoded report: acc (G), gyro (rad/s), host time estimate (ns).
typedef void (*DeckImuSampleFn)(const double acc[3], const double gyro[3], uint64_t now_ns, void *user);
// Reads the queued reports and hands each to sample.
void deck_imu_poll(DeckImu *d, uint64_t now, DeckImuSampleFn sample, void *user);
