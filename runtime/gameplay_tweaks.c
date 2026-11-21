// Mouse wheel and keyboard gameplay tweaks with independent live settings.
// The game's native R + D-pad object rotation and D-pad distance controls remain authoritative.
#include "gameplay_tweaks.h"
#include "cfg.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "input_wsys.h"
#include "platform/platform.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BIT(x) (1ull << (x))
#define HOLD_FRAMES 3
#define GAP_FRAMES 1
#define MAX_QUEUED 16
enum { NONE, ROTATE, DISTANCE, ITEMS, MAP_ZOOM, WEAPONS };
enum { W_IDLE = 0, W_PRIME, W_HOLD, W_GAP, W_WAIT };
#define WEAPON_PRIME_MIN_NS  75000000ull
#define WEAPON_PRIME_MAX_NS 135000000ull
#define WEAPON_HOLD_NS       42000000ull
#define WEAPON_GAP_NS        36000000ull

static PlatMutex lock = PLAT_MUTEX_INIT;
static unsigned seen = UINT_MAX;
static struct {
    int available, rotate, c_axis, distance, wheel, keys, map_zoom, weapon_wheel;
    double rotation_speed, distance_speed, item_speed, map_zoom_speed, weapon_speed;
} options;
static struct {
    int kind, axis, direction, queued, hold, gap, primed, valid;
    double fraction;
    uint64_t frame, buttons;
    int32_t rx;
} pulse;
static struct {
    int active, phase, direction, scroll_dir, queued, polls_in_phase;
    int confirm_pending;
    double fraction;
    uint64_t phase_until_ns, prime_start_ns, last_frame;
    int32_t rx;
} weapon;
static double map_zoom_velocity;
static int held_a, held_d;
static int consumed_c, consumed_right;
static int was_e_down, consumed_e;
static uint64_t physical_buttons;

static void clear_weapon_locked(void) { memset(&weapon, 0, sizeof weapon); }
static void clear_locked(void) { memset(&pulse, 0, sizeof pulse); clear_weapon_locked(); map_zoom_velocity = 0; }

static void refresh_locked(void) {
    unsigned version = cfg_version();
    if (seen == version) return;
    options.available = cfg_bool("TOTK_GAMEPLAY_TWEAKS_ENABLED");
    options.rotate = cfg_bool("TOTK_GAMEPLAY_ULTRAHAND_ROTATE");
    options.c_axis = cfg_bool("TOTK_GAMEPLAY_ULTRAHAND_C_AXIS");
    options.distance = cfg_bool("TOTK_GAMEPLAY_ULTRAHAND_DISTANCE");
    options.wheel = cfg_bool("TOTK_GAMEPLAY_ITEM_WHEEL");
    options.keys = cfg_bool("TOTK_GAMEPLAY_ITEM_AD");
    options.map_zoom = cfg_bool("TOTK_GAMEPLAY_MAP_ZOOM");
    options.weapon_wheel = cfg_bool("TOTK_GAMEPLAY_WEAPON_WHEEL");
    options.rotation_speed = cfg_double("TOTK_GAMEPLAY_ROTATION_SENSITIVITY");
    options.distance_speed = cfg_double("TOTK_GAMEPLAY_DISTANCE_SENSITIVITY");
    options.item_speed = cfg_double("TOTK_GAMEPLAY_ITEM_SENSITIVITY");
    options.map_zoom_speed = cfg_double("TOTK_GAMEPLAY_MAP_ZOOM_SENSITIVITY");
    options.weapon_speed = cfg_double("TOTK_GAMEPLAY_WEAPON_SENSITIVITY");
    if (!isfinite(options.rotation_speed) || options.rotation_speed < .25 || options.rotation_speed > 4) options.rotation_speed = 1;
    if (!isfinite(options.distance_speed) || options.distance_speed < .25 || options.distance_speed > 4) options.distance_speed = 1;
    if (!isfinite(options.item_speed) || options.item_speed < .25 || options.item_speed > 4) options.item_speed = 1;
    if (!isfinite(options.map_zoom_speed) || options.map_zoom_speed < .25 || options.map_zoom_speed > 4) options.map_zoom_speed = 1;
    if (!isfinite(options.weapon_speed) || options.weapon_speed < .25 || options.weapon_speed > 4) options.weapon_speed = 1;
    seen = version;
    clear_locked();
}

static int item_menu(uint64_t buttons) {
    if (buttons & BIT(B_L)) return 0;
    const uint64_t held = BIT(B_LEFT) | BIT(B_UP) | BIT(B_RIGHT) | BIT(B_DOWN);
    return input_camera_context() == INPUT_CAM_MENU_WHEEL || (buttons & held) != 0;
}

static int object_context(uint64_t buttons) {
    const uint64_t menus = BIT(B_L) | BIT(B_LEFT) | BIT(B_UP) | BIT(B_RIGHT) | BIT(B_DOWN) |
                           BIT(B_B) | BIT(B_PLUS) | BIT(B_MINUS);
    return input_camera_context() == INPUT_CAM_ULTRAHAND_OBJECT && !(buttons & menus);
}

static int map_zoom_allowed(uint64_t buttons) {
    if (!options.map_zoom) return 0;
    if (object_context(buttons) || item_menu(buttons)) return 0;
    if (buttons & (BIT(B_L) | BIT(B_ZL) | BIT(B_ZR) | BIT(B_R))) return 0;
    int ctx = input_camera_context();
    if (ctx == INPUT_CAM_AIM || ctx == INPUT_CAM_SCOPE ||
        ctx == INPUT_CAM_ULTRAHAND_OBJECT || ctx == INPUT_CAM_LOCK_ON) {
        return 0;
    }
    // Only the map menu_cursor.c finds: with menu_cursor = 0 (or on 1.2.1) the map is not read, and a fallback to
    // "allowed" would push the right stick's Y in free roam, where it tilts the camera.
    return menu_cursor_map();
}

static int weapon_wheel_allowed(uint64_t buttons, int right_down, int already_active) {
    if (!options.available || !options.weapon_wheel) return 0;
    if (right_down) return 0;
    if (!ikb_captured() || menu_cursor_map() || menu_cursor_pointer()) return 0;
    const uint64_t forbidden = BIT(B_L) | BIT(B_R) | BIT(B_ZL) | BIT(B_ZR) |
                               BIT(B_LEFT) | BIT(B_UP) | BIT(B_RIGHT) | BIT(B_DOWN) |
                               BIT(B_PLUS) | BIT(B_MINUS);
    if (buttons & forbidden) return 0;

    if (!already_active) {
        if (menu_cursor_shortcut() == 1 || menu_cursor_strip() || menu_cursor_camera() || menu_cursor_menu())
            return 0;
    }

    int ctx = input_camera_context();
    if (!already_active) {
        // Opening needs a camera the mouse_direct hooks saw this moment: an unclassified one is also a paused menu,
        // 1.2.1's menus or any screen menu_cursor.c does not read, where D-pad right and the right stick would move
        // the menu instead.
        if (ctx != INPUT_CAM_FREE_ROAM && ctx != INPUT_CAM_LOCK_ON && ctx != INPUT_CAM_VEHICLE)
            return 0;
    } else {
        if (ctx != INPUT_CAM_UNCLASSIFIED && ctx != INPUT_CAM_MENU_WHEEL &&
            ctx != INPUT_CAM_FREE_ROAM && ctx != INPUT_CAM_LOCK_ON && ctx != INPUT_CAM_VEHICLE)
            return 0;
    }
    return 1;
}

int gameplay_tweaks_available(void) {
    plat_mutex_lock(&lock);
    refresh_locked();
    int available = options.available;
    plat_mutex_unlock(&lock);
    return available;
}

int gameplay_tweaks_capture_needed(void) {
    plat_mutex_lock(&lock);
    refresh_locked();
    int active = options.available && (options.rotate || options.distance || options.wheel ||
                                      options.keys || options.map_zoom || options.weapon_wheel);
    plat_mutex_unlock(&lock);
    return active;
}

int gameplay_tweaks_map_zoom(void) {
    plat_mutex_lock(&lock);
    refresh_locked();
    int active = options.available && options.map_zoom;
    plat_mutex_unlock(&lock);
    return active;
}

int gameplay_tweaks_strip_wheel(uint64_t buttons) {
    plat_mutex_lock(&lock);
    refresh_locked();
    int active = options.available && (weapon.active || (options.wheel && item_menu(buttons)));
    plat_mutex_unlock(&lock);
    return active;
}

void gameplay_tweaks_init(void) {
    gameplay_tweaks_available();
}

void gameplay_tweaks_reset(void) {
    plat_mutex_lock(&lock);
    clear_locked();
    held_a = held_d = 0;
    physical_buttons = 0;
    was_e_down = consumed_e = 0;
    plat_mutex_unlock(&lock);
}

void gameplay_tweaks_wheel(double delta, int c_down, int right_down) {
    if (!isfinite(delta) || delta == 0) return;
    if (delta > MAX_QUEUED) delta = MAX_QUEUED;
    if (delta < -MAX_QUEUED) delta = -MAX_QUEUED;
    plat_mutex_lock(&lock);
    refresh_locked();
    if (!options.available) {
        clear_locked();
        plat_mutex_unlock(&lock);
        return;
    }
    int kind = NONE, axis = 0;
    double sensitivity = 1;
    if (object_context(physical_buttons)) {
        if (right_down && options.distance) {
            kind = DISTANCE;
            sensitivity = options.distance_speed;
        } else if (options.rotate) {
            kind = ROTATE;
            axis = options.c_axis && c_down;
            sensitivity = options.rotation_speed;
        }
    } else if ((physical_buttons & (BIT(B_LEFT) | BIT(B_UP) | BIT(B_RIGHT) | BIT(B_DOWN))) &&
               options.wheel && item_menu(physical_buttons)) {
        kind = ITEMS;
        sensitivity = options.item_speed;
    } else if (map_zoom_allowed(physical_buttons)) {
        kind = MAP_ZOOM;
        sensitivity = options.map_zoom_speed;
    } else if (weapon_wheel_allowed(physical_buttons, right_down, weapon.active)) {
        kind = WEAPONS;
    }
    if (!kind) {
        clear_locked();
        plat_mutex_unlock(&lock);
        return;
    }
    if (kind == MAP_ZOOM) {
        if (weapon.active) clear_weapon_locked();
        map_zoom_velocity -= delta * sensitivity * 16000.0;
        if (map_zoom_velocity > 65536.0) map_zoom_velocity = 65536.0;
        if (map_zoom_velocity < -65536.0) map_zoom_velocity = -65536.0;
        plat_mutex_unlock(&lock);
        return;
    }
    if (kind == WEAPONS) {
        uint64_t now = plat_now_ns();
        int new_dir = delta > 0 ? 1 : -1;
        if (weapon.scroll_dir != 0 && new_dir != weapon.scroll_dir) {
            weapon.fraction = 0;
            weapon.queued = 0;
            if (weapon.active && weapon.phase == W_HOLD) {
                weapon.phase = W_GAP;
                weapon.rx = 0;
                weapon.polls_in_phase = 0;
                weapon.phase_until_ns = now + WEAPON_GAP_NS;
            }
        }
        weapon.scroll_dir = new_dir;
        weapon.fraction += delta * options.weapon_speed;
        int steps = (int)trunc(weapon.fraction);
        if (steps != 0) {
            weapon.confirm_pending = 0;
            weapon.fraction -= steps;
            weapon.direction = steps > 0 ? 1 : -1;
            weapon.queued += (steps > 0 ? steps : -steps);
            if (weapon.queued > MAX_QUEUED) weapon.queued = MAX_QUEUED;
            if (!weapon.active) {
                weapon.active = 1;
                weapon.phase = W_PRIME;
                weapon.polls_in_phase = 0;
                weapon.prime_start_ns = now;
                weapon.phase_until_ns = now + WEAPON_PRIME_MAX_NS;
                weapon.rx = 0;
            } else if (weapon.phase == W_WAIT) {
                weapon.phase = W_HOLD;
                weapon.polls_in_phase = 0;
                weapon.phase_until_ns = now + WEAPON_HOLD_NS;
                weapon.rx = 32767 * weapon.direction;
                if (weapon.queued > 0) weapon.queued--;
            }
        }
        plat_mutex_unlock(&lock);
        return;
    }
    if (weapon.active) clear_weapon_locked();
    if (kind != pulse.kind || axis != pulse.axis ||
        (delta > 0 && pulse.direction < 0) || (delta < 0 && pulse.direction > 0)) {
        clear_locked();
    }
    pulse.kind = kind;
    pulse.axis = axis;
    pulse.fraction += delta * sensitivity;
    int steps = (int)trunc(pulse.fraction);
    if (steps) {
        pulse.fraction -= steps;
        pulse.direction = steps > 0 ? 1 : -1;
        if (!pulse.queued && !pulse.hold && !pulse.gap) pulse.valid = 0;
        pulse.queued += steps > 0 ? steps : -steps;
        if (pulse.queued > MAX_QUEUED) pulse.queued = MAX_QUEUED;
    }
    plat_mutex_unlock(&lock);
}

void gameplay_tweaks_prepare_keys(uint8_t *down, uint64_t pad_buttons) {
    plat_mutex_lock(&lock);
    refresh_locked();
    held_a = held_d = 0;
    int c = down[KEY_C], right = down[KEY_MOUSE_R];
    int e = down[KEY_E];
    int e_pressed = e && !was_e_down;
    was_e_down = e;
    if (!e) consumed_e = 0;
    if (!c) consumed_c = 0;
    if (!right) consumed_right = 0;
    if (options.available) {
        if (input_camera_context() == INPUT_CAM_ULTRAHAND_OBJECT) {
            if (options.rotate && options.c_axis && !(options.distance && right) && c) consumed_c = 1;
            if (options.distance && right) consumed_right = 1;
            if (pulse.kind == ROTATE && pulse.axis != (options.c_axis && c)) clear_locked();
        }
        if (weapon.active && e_pressed) {
            consumed_e = 1;
            weapon.confirm_pending = 1;
        }
        if (consumed_e) down[KEY_E] = 0;
        if (consumed_c) down[KEY_C] = 0;
        if (consumed_right) down[KEY_MOUSE_R] = 0;
        uint64_t buttons = pad_buttons | input_map_buttons(down);
        if (options.keys && item_menu(buttons) && !weapon.active) {
            held_a = down[KEY_A];
            held_d = down[KEY_D];
            down[KEY_A] = down[KEY_D] = 0;
        }
    } else {
        consumed_c = consumed_right = consumed_e = 0;
    }
    plat_mutex_unlock(&lock);
}

static void sample_locked(uint64_t frame) {
    if (pulse.valid && pulse.frame == frame) return;
    pulse.buttons = 0;
    pulse.rx = 0;
    if (pulse.queued || pulse.hold || pulse.gap) {
        if (pulse.kind == ROTATE) pulse.buttons = BIT(B_R);
        if (pulse.kind == ROTATE && !pulse.primed) pulse.primed = 1;
        else if (pulse.gap) pulse.gap--;
        else {
            if (!pulse.hold && pulse.queued) {
                pulse.queued--;
                pulse.hold = HOLD_FRAMES;
            }
            if (pulse.hold) {
                if (pulse.kind == ROTATE)
                    pulse.buttons |= pulse.axis ? (pulse.direction > 0 ? BIT(B_DOWN) : BIT(B_UP)) :
                                                 (pulse.direction > 0 ? BIT(B_RIGHT) : BIT(B_LEFT));
                else if (pulse.kind == DISTANCE)
                    pulse.buttons = pulse.direction < 0 ? BIT(B_UP) : BIT(B_DOWN);
                else pulse.rx = 32767 * pulse.direction;
                if (!--pulse.hold) pulse.gap = GAP_FRAMES;
            }
        }
    } else pulse.primed = 0;
    pulse.valid = 1;
    pulse.frame = frame;
}

void gameplay_tweaks_apply(uint64_t frame, int active, uint64_t *buttons,
                           int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    (void)lx;
    (void)ly;
    plat_mutex_lock(&lock);
    refresh_locked();
    physical_buttons = *buttons;
    if (!active || !options.available) {
        clear_locked();
        held_a = held_d = 0;
        plat_mutex_unlock(&lock);
        return;
    }
    int right = ikb_down(KEY_MOUSE_R);

    if (weapon.active) {
        if (!weapon_wheel_allowed(physical_buttons, right, 1) || (physical_buttons & BIT(B_B))) {
            clear_weapon_locked();
        }
    }

    if (weapon.active) {
        uint64_t now = plat_now_ns();
        if (weapon.last_frame != frame) {
            weapon.last_frame = frame;
            weapon.polls_in_phase++;
        }

        switch (weapon.phase) {
        case W_PRIME: {
            uint64_t elapsed = now - weapon.prime_start_ns;
            int shortcut = (menu_cursor_shortcut() == 1);
            int ready = (elapsed >= WEAPON_PRIME_MIN_NS && weapon.polls_in_phase >= 3 && shortcut) ||
                        (elapsed >= WEAPON_PRIME_MAX_NS && weapon.polls_in_phase >= 4);
            if (ready) {
                if (weapon.queued > 0) {
                    weapon.phase = W_HOLD;
                    weapon.polls_in_phase = 0;
                    weapon.phase_until_ns = now + WEAPON_HOLD_NS;
                    weapon.rx = 32767 * weapon.direction;
                    weapon.queued--;
                } else if (weapon.confirm_pending) {
                    clear_weapon_locked();
                } else {
                    weapon.phase = W_WAIT;
                    weapon.polls_in_phase = 0;
                    weapon.rx = 0;
                }
            }
            break;
        }
        case W_HOLD: {
            if (now >= weapon.phase_until_ns && weapon.polls_in_phase >= 2) {
                weapon.phase = W_GAP;
                weapon.polls_in_phase = 0;
                weapon.phase_until_ns = now + WEAPON_GAP_NS;
                weapon.rx = 0;
            }
            break;
        }
        case W_GAP: {
            if (now >= weapon.phase_until_ns && weapon.polls_in_phase >= 2) {
                if (weapon.queued > 0) {
                    weapon.phase = W_HOLD;
                    weapon.polls_in_phase = 0;
                    weapon.phase_until_ns = now + WEAPON_HOLD_NS;
                    weapon.rx = 32767 * weapon.direction;
                    weapon.queued--;
                } else if (weapon.confirm_pending) {
                    clear_weapon_locked();
                } else {
                    weapon.phase = W_WAIT;
                    weapon.polls_in_phase = 0;
                    weapon.rx = 0;
                }
            }
            break;
        }
        case W_WAIT: {
            if (weapon.confirm_pending) {
                clear_weapon_locked();
            }
            break;
        }
        default:
            clear_weapon_locked();
            break;
        }
    }

    if (weapon.active) {
        *buttons |= BIT(B_RIGHT);
        *rx = weapon.rx;
        *ry = 0;
    } else {
        int object = object_context(*buttons), items = item_menu(*buttons);
        if ((pulse.kind == ROTATE && (!object || !options.rotate || (right && options.distance))) ||
            (pulse.kind == DISTANCE && (!object || !right || !options.distance)) ||
            (pulse.kind == ITEMS && (!items || !options.wheel))) {
            clear_locked();
        }
        sample_locked(frame);
        if (!map_zoom_allowed(*buttons)) {
            map_zoom_velocity = 0;
        } else if (fabs(map_zoom_velocity) > 50.0) {
            int32_t stick_y = (int32_t)map_zoom_velocity;
            if (stick_y > 32767) stick_y = 32767;
            if (stick_y < -32767) stick_y = -32767;
            if (abs(stick_y) > abs(*ry)) *ry = stick_y;
            map_zoom_velocity *= 0.78;
            if (fabs(map_zoom_velocity) < 100.0) map_zoom_velocity = 0;
        }
        if (pulse.kind == ROTATE || pulse.kind == DISTANCE) {
            *buttons |= pulse.buttons;
            // Distance D-pad pulses do not own the look stick. R rotation does,
            // because the game's rotation mode would otherwise rotate both axes.
            if (pulse.kind == ROTATE && pulse.buttons) *rx = *ry = 0;
        } else if (pulse.kind == ITEMS && (pulse.queued || pulse.hold || pulse.gap || pulse.rx)) {
            *rx = pulse.rx;
            *ry = 0;
        } else if (items && options.keys && (held_a || held_d)) {
            *rx = ((held_d != 0) - (held_a != 0)) * 32767;
            *ry = 0;
        }
    }
    plat_mutex_unlock(&lock);
}
