"""Check configurable Ultrahand modifiers without game data.

Usage: python tests/gameplay_tweaks_check.py --cc clang
"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "gameplay_tweaks.h"
#include "input.h"
#include "input_map.h"
#include "platform/platform.h"

#define BIT(x) (1ull << (x))

static unsigned version = 1;
static int enabled = 1;
static int rotate_enabled = 1;
static int alternate_enabled = 1;
static int distance_enabled = 1;
static int context = INPUT_CAM_ULTRAHAND_OBJECT;
static uint64_t frame;
static uint8_t keys[KEY_COUNT];

static const char *default_action = "rotate_x";
static const char *rotation = "c";
static const char *distance = "mouse_r";

unsigned cfg_version(void) { return version; }

const char *cfg_get(const char *name) {
    (void)name;
    return NULL;
}

const char *cfg_value(const char *name) {
    if (!strcmp(name, "TOTK_GAMEPLAY_ULTRAHAND_DEFAULT_ACTION"))
        return default_action;
    if (!strcmp(name, "TOTK_GAMEPLAY_ULTRAHAND_ROTATION_KEY"))
        return rotation;
    if (!strcmp(name, "TOTK_GAMEPLAY_ULTRAHAND_DISTANCE_KEY"))
        return distance;
    return "1";
}

int cfg_bool(const char *name) {
    if (!strcmp(name, "TOTK_GAMEPLAY_TWEAKS_ENABLED"))
        return enabled;
    if (!strcmp(name, "TOTK_GAMEPLAY_ULTRAHAND_ROTATE"))
        return rotate_enabled;
    if (!strcmp(name, "TOTK_GAMEPLAY_ULTRAHAND_C_AXIS"))
        return alternate_enabled;
    if (!strcmp(name, "TOTK_GAMEPLAY_ULTRAHAND_DISTANCE"))
        return distance_enabled;
    return 1;
}

double cfg_double(const char *name) {
    (void)name;
    return 1.0;
}

void rt_log(const char *fmt, ...) { (void)fmt; }

void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }

uint64_t plat_now_ns(void) {
    return frame * 16666667ull;
}

int ikb_down(int key) {
    return key > KEY_NONE && key < KEY_COUNT && keys[key];
}

int ikb_captured(void) { return 1; }
int input_camera_context(void) { return context; }

int menu_cursor_map(void) { return 0; }
int menu_cursor_pointer(void) { return 0; }
int menu_cursor_shortcut(void) { return 0; }
int menu_cursor_strip(void) { return 0; }
int menu_cursor_camera(void) { return 0; }
int menu_cursor_menu(void) { return 0; }

static uint64_t poll(void) {
    uint8_t down[KEY_COUNT];
    memcpy(down, keys, sizeof down);

    gameplay_tweaks_prepare_keys(down, 0);

    uint64_t buttons = input_map_buttons(down);
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;

    gameplay_tweaks_apply(
        ++frame, 1, &buttons, &lx, &ly, &rx, &ry
    );

    return buttons;
}

static void configure(const char *r, const char *d, int on) {
    memset(keys, 0, sizeof keys);
    default_action = "rotate_x";
    rotation = r;
    distance = d;
    enabled = on;
    rotate_enabled = 1;
    alternate_enabled = 1;
    distance_enabled = 1;
    context = INPUT_CAM_ULTRAHAND_OBJECT;

    ++version;
    gameplay_tweaks_reset();
    poll();
}

static uint64_t scroll(double delta) {
    poll();
    gameplay_tweaks_wheel(delta);

    // Include the rotation mode's priming frame.
    uint64_t buttons = poll();
    return buttons | poll();
}

int main(void) {
    uint64_t b;

    // Original behavior without a modifier.
    configure("c", "mouse_r", 1);
    b = scroll(1);
    assert(b & BIT(B_R));
    assert(b & BIT(B_RIGHT));
    assert(!(b & BIT(B_DOWN)));
    puts("PASS: default wheel rotation");

    // Original C modifier.
    configure("c", "mouse_r", 1);
    keys[KEY_C] = 1;
    b = scroll(1);
    assert(b & BIT(B_R));
    assert(b & BIT(B_DOWN));
    assert(!(b & BIT(B_RIGHT)));
    puts("PASS: default alternate rotation");

    // Original right-mouse distance modifier.
    configure("c", "mouse_r", 1);
    keys[KEY_MOUSE_R] = 1;
    b = scroll(1);
    assert(b & BIT(B_DOWN));
    assert(!(b & BIT(B_R)));
    puts("PASS: default distance adjustment");

    // Custom right-mouse rotation modifier.
    configure("mouse_r", "r", 1);
    keys[KEY_MOUSE_R] = 1;
    b = scroll(1);
    assert(b & BIT(B_R));
    assert(b & BIT(B_DOWN));
    assert(!(b & BIT(B_ZL)));
    puts("PASS: custom rotation modifier");

    // Custom physical R distance modifier.
    configure("mouse_r", "r", 1);
    keys[KEY_R] = 1;
    b = scroll(1);
    assert(b & BIT(B_DOWN));
    assert(!(b & BIT(B_R)));
    puts("PASS: custom distance modifier");

    // C retains its ordinary binding.
    configure("mouse_r", "r", 1);
    keys[KEY_C] = 1;
    assert(poll() & BIT(B_STICK_R));
    puts("PASS: original C binding preserved");

    // Distance wins when both modifiers are held.
    configure("mouse_r", "r", 1);
    keys[KEY_MOUSE_R] = 1;
    keys[KEY_R] = 1;
    b = scroll(1);
    assert(b & BIT(B_DOWN));
    assert(!(b & BIT(B_R)));
    puts("PASS: distance modifier priority");

    // Assigning both actions to the same physical key.
    configure("r", "r", 1);
    keys[KEY_R] = 1;
    b = scroll(1);
    assert(b & BIT(B_DOWN));
    assert(!(b & BIT(B_R)));
    puts("PASS: duplicate modifier priority");

    // Case-insensitive key names.
    configure("MOUSE_R", "R", 1);
    keys[KEY_R] = 1;
    b = scroll(1);
    assert(b & BIT(B_DOWN));
    assert(!(b & BIT(B_R)));
    puts("PASS: case-insensitive key names");

    // Invalid settings fall back to the original keys.
    configure("invalid-key", "invalid-key", 1);
    keys[KEY_C] = 1;
    b = scroll(1);
    assert(b & BIT(B_R));
    assert(b & BIT(B_DOWN));
    puts("PASS: invalid rotation key fallback");

    configure("invalid-key", "invalid-key", 1);
    keys[KEY_MOUSE_R] = 1;
    b = scroll(1);
    assert(b & BIT(B_DOWN));
    assert(!(b & BIT(B_R)));
    puts("PASS: invalid distance key fallback");

    // Explicitly disabled modifiers.
    configure("none", "none", 1);
    keys[KEY_C] = 1;
    b = scroll(1);
    assert(b & BIT(B_RIGHT));
    assert(b & BIT(B_STICK_R));
    puts("PASS: disabled modifiers");

    // The original keyboard actions work outside Ultrahand.
    configure("mouse_r", "r", 1);
    context = INPUT_CAM_FREE_ROAM;
    keys[KEY_MOUSE_R] = 1;
    keys[KEY_R] = 1;
    b = poll();
    assert(b & BIT(B_ZL));
    assert(b & BIT(B_R));
    puts("PASS: normal actions outside Ultrahand");

    // Entire feature disabled.
    configure("mouse_r", "r", 0);
    keys[KEY_MOUSE_R] = 1;
    keys[KEY_R] = 1;
    b = scroll(1);
    assert(b & BIT(B_ZL));
    assert(b & BIT(B_R));
    assert(!(b & BIT(B_DOWN)));
    puts("PASS: gameplay tweaks disabled");

    // Change a binding live while the old key is held.
    configure("c", "mouse_r", 1);
    keys[KEY_C] = 1;
    assert(!(poll() & BIT(B_STICK_R)));

    rotation = "mouse_r";
    distance = "r";
    ++version;

    assert(poll() & BIT(B_STICK_R));
    puts("PASS: live modifier reconfiguration");


    // Releasing the distance modifier stops queued distance pulses.
    configure("mouse_r", "r", 1);
    keys[KEY_R] = 1;
    b = scroll(4);
    assert(b & BIT(B_DOWN));
    keys[KEY_R] = 0;
    assert(!(poll() & (BIT(B_UP) | BIT(B_DOWN))));
    puts("PASS: distance modifier release");

    // Releasing the alternate-axis modifier cancels its rotation.
    configure("mouse_r", "r", 1);
    keys[KEY_MOUSE_R] = 1;
    b = scroll(4);
    assert(b & BIT(B_DOWN));
    keys[KEY_MOUSE_R] = 0;
    assert(!(poll() & (BIT(B_R) | BIT(B_DOWN))));
    puts("PASS: rotation modifier release");

    // Leaving Ultrahand cancels any queued object movement.
    configure("mouse_r", "r", 1);
    b = scroll(4);
    assert(b & BIT(B_R));
    context = INPUT_CAM_FREE_ROAM;
    assert(!(poll() & (BIT(B_R) | BIT(B_RIGHT))));
    puts("PASS: queued rotation cancelled outside Ultrahand");

    // A released modifier can perform its ordinary action again.
    configure("mouse_r", "r", 1);
    keys[KEY_MOUSE_R] = 1;
    assert(!(poll() & BIT(B_ZL)));

    keys[KEY_MOUSE_R] = 0;
    context = INPUT_CAM_FREE_ROAM;
    poll();

    keys[KEY_MOUSE_R] = 1;
    assert(poll() & BIT(B_ZL));
    puts("PASS: ordinary action restored after release");

    // A reset discards queued gameplay input.
    configure("mouse_r", "r", 1);
    scroll(4);
    memset(keys, 0, sizeof keys);
    gameplay_tweaks_reset();
    assert(poll() == 0);
    puts("PASS: gameplay input reset");

    // Reverse rotation produces the opposite direction.
    configure("mouse_r", "r", 1);
    b = scroll(-1);
    assert(b & BIT(B_LEFT));
    assert(!(b & BIT(B_RIGHT)));
    puts("PASS: reverse rotation");

    // Reverse distance adjustment.
    configure("mouse_r", "r", 1);
    keys[KEY_R] = 1;
    b = scroll(-1);
    assert(b & BIT(B_UP));
    assert(!(b & BIT(B_DOWN)));
    puts("PASS: reverse distance");

    // Changing the distance modifier must not change weapon cancellation.
    configure("mouse_r", "c", 1);
    context = INPUT_CAM_FREE_ROAM;
    assert(scroll(1) & BIT(B_RIGHT));

    keys[KEY_C] = 1;
    assert(poll() & BIT(B_RIGHT));

    keys[KEY_MOUSE_R] = 1;
    assert(!(poll() & BIT(B_RIGHT)));
    puts("PASS: independent weapon-wheel cancellation");


    // Exercise every default action with every modifier slot.
    const char *actions[] = {"rotate_x", "rotate_y", "distance"};
    const uint64_t expected[3][3] = {
        {BIT(B_R) | BIT(B_RIGHT), BIT(B_R) | BIT(B_DOWN), BIT(B_DOWN)},
        {BIT(B_R) | BIT(B_DOWN), BIT(B_R) | BIT(B_RIGHT), BIT(B_DOWN)},
        {BIT(B_DOWN), BIT(B_R) | BIT(B_DOWN), BIT(B_R) | BIT(B_RIGHT)},
    };

    for (int action = 0; action < 3; action++) {
        for (int slot = 0; slot < 3; slot++) {
            configure("mouse_r", "r", 1);
            default_action = actions[action];
            ++version;

            if (slot == 1) keys[KEY_MOUSE_R] = 1;
            if (slot == 2) keys[KEY_R] = 1;

            b = scroll(1);

            uint64_t mask = BIT(B_R) | BIT(B_RIGHT) |
                            BIT(B_DOWN) | BIT(B_LEFT) | BIT(B_UP);

            assert((b & mask) == expected[action][slot]);

            printf("PASS: default %s, modifier slot %d\n",
                   actions[action], slot);
        }
    }

    configure("mouse_r", "r", 1);
    default_action = "invalid";
    ++version;
    b = scroll(1);
    assert(b & BIT(B_RIGHT));
    assert(b & BIT(B_R));
    puts("PASS: invalid default action fallback");

    configure("mouse_r", "r", 1);
    default_action = "distance";
    ++version;
    keys[KEY_MOUSE_R] = keys[KEY_R] = 1;
    b = scroll(1);
    assert(b & BIT(B_R));
    assert(b & BIT(B_RIGHT));
    puts("PASS: second modifier priority with distance default");

    // The losing modifier must not open a native menu.
    configure("q", "r", 1);
    keys[KEY_Q] = keys[KEY_R] = 1;
    b = scroll(1);
    assert(b & BIT(B_DOWN));
    assert(!(b & (BIT(B_L) | BIT(B_R))));
    puts("PASS: simultaneous modifiers suppress conflicting input");

    configure("c", "mouse_r", 1);
    rotate_enabled = 0;
    ++version;
    assert(scroll(1) == 0);
    puts("PASS: rotation disabled");

    configure("c", "mouse_r", 1);
    rotate_enabled = 0;
    ++version;
    keys[KEY_MOUSE_R] = 1;
    b = scroll(1);
    assert(b & BIT(B_DOWN));
    assert(!(b & BIT(B_R)));
    puts("PASS: distance works with rotation disabled");

    configure("c", "mouse_r", 1);
    alternate_enabled = 0;
    ++version;
    keys[KEY_C] = 1;
    b = scroll(1);
    assert(b & BIT(B_RIGHT));
    assert(b & BIT(B_STICK_R));
    assert(!(b & BIT(B_DOWN)));
    puts("PASS: alternate axis disabled");

    configure("c", "mouse_r", 1);
    distance_enabled = 0;
    ++version;
    keys[KEY_MOUSE_R] = 1;
    b = scroll(1);
    assert(b & BIT(B_R));
    assert(b & BIT(B_RIGHT));
    assert(b & BIT(B_ZL));
    puts("PASS: distance disabled");

    configure("c", "mouse_r", 1);
    distance_enabled = 0;
    ++version;
    keys[KEY_C] = keys[KEY_MOUSE_R] = 1;
    b = scroll(1);
    assert(b & BIT(B_R));
    assert(b & BIT(B_DOWN));
    puts("PASS: first modifier wins when second action disabled");

    configure("c", "mouse_r", 1);
    default_action = "distance";
    rotate_enabled = 0;
    ++version;
    b = scroll(1);
    assert(b & BIT(B_DOWN));
    assert(!(b & BIT(B_R)));
    puts("PASS: default distance with rotation disabled");

    puts("All 41 Ultrahand checks passed.");

    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(
        prefix="totk-gameplay-"
    ) as tmp:
        folder = Path(tmp)
        source = folder / "check.c"
        source.write_text(HARNESS, encoding="utf-8")
        exe = folder / (
            "check.exe" if os.name == "nt" else "check"
        )

        subprocess.run([
            args.cc,
            "-O2",
            "-Wall",
            "-Wextra",
            "-I", str(ROOT / "runtime"),
            str(source),
            str(ROOT / "runtime/gameplay_tweaks.c"),
            str(ROOT / "runtime/input_map.c"),
            "-lm",
            "-o", str(exe),
        ], check=True)

        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
