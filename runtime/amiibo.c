// Amiibo dump loader and Horizon OS NFP state emulation.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amiibo.h"
#include "host.h"
#include "platform/platform.h"

static PlatMutex g_amiibo_lock = PLAT_MUTEX_INIT;

// 540-byte raw NTAG215 binary dump
static uint8_t g_dump[AMIIBO_DUMP_SIZE];
static bool g_has_dump = false;
static AmiiboInfo g_info;

static bool g_device_present = true;
static bool g_detecting = false;
static bool g_tag_present = false;
static bool g_mounted = false;
static bool g_auto_remove = true;
static float g_auto_remove_delay_s = 2.5f;

static uint64_t g_tap_timestamp_ns = 0;
static bool g_timer_active = false;

static bool amiibo_validate_dump(const uint8_t *data, uint64_t size) {
    if (size != AMIIBO_DUMP_SIZE) {
        return false;
    }

    // NTAG215 Capability Container (CC) at page 3 (offset 0x0C).
    // Byte 0x0C must be 0xE1 (NDEF Magic / Type 2 Tag CC container byte).
    if (data[0x0C] != 0xE1) {
        return false;
    }

    // UID sanity: UID (bytes 0..2 and 4..7) cannot be all zeroes or all 0xFF.
    if ((data[0] == 0 && data[1] == 0 && data[2] == 0 && data[4] == 0 &&
         data[5] == 0 && data[6] == 0 && data[7] == 0) ||
        (data[0] == 0xFF && data[1] == 0xFF && data[2] == 0xFF && data[4] == 0xFF &&
         data[5] == 0xFF && data[6] == 0xFF && data[7] == 0xFF)) {
        return false;
    }

    // ISO 14443-3 / NTAG215 Bit Check Character (BCC) verification.
    // Page 0 byte 3 (BCC0) = 0x88 ^ UID[0] ^ UID[1] ^ UID[2]
    // Page 2 byte 0 (BCC1) = UID[3] ^ UID[4] ^ UID[5] ^ UID[6]
    uint8_t bcc0 = (uint8_t)(0x88 ^ data[0] ^ data[1] ^ data[2]);
    uint8_t bcc1 = (uint8_t)(data[4] ^ data[5] ^ data[6] ^ data[7]);
    if (data[3] != bcc0 || data[8] != bcc1) {
        return false;
    }

    return true;
}

static void parse_dump_info(const char *path) {
    memset(&g_info, 0, sizeof(g_info));
    if (path) {
        snprintf(g_info.path, sizeof(g_info.path), "%s", path);
        const char *slash = strrchr(path, '\\');
        const char *fslash = strrchr(path, '/');
        const char *name = slash ? slash + 1 : (fslash ? fslash + 1 : path);
        if (slash && fslash) {
            name = (slash > fslash ? slash : fslash) + 1;
        }
        snprintf(g_info.filename, sizeof(g_info.filename), "%s", name);
    }

    // UID: bytes 0..6 (7 bytes) or up to 9
    memcpy(g_info.uuid, g_dump, 7);
    g_info.uuid_len = 7;

    // Amiibo ID at offset 0x54 (8 bytes)
    memcpy(g_info.character_id, g_dump + 0x54, 3);
    g_info.series_id = g_dump[0x57];
    g_info.numbering_id = ((uint16_t)g_dump[0x58] << 8) | g_dump[0x59];
    g_info.nfp_type = g_dump[0x5A];
    g_info.version = g_dump[0x5B];
}

void amiibo_init(void) {
    plat_mutex_lock(&g_amiibo_lock);
    g_device_present = true;
    g_detecting = false;
    g_tag_present = false;
    g_mounted = false;
    g_has_dump = false;
    g_timer_active = false;
    memset(g_dump, 0, sizeof(g_dump));
    memset(&g_info, 0, sizeof(g_info));
    plat_mutex_unlock(&g_amiibo_lock);
}

void amiibo_set_device_present(bool present) {
    plat_mutex_lock(&g_amiibo_lock);
    g_device_present = present;
    plat_mutex_unlock(&g_amiibo_lock);
}

bool amiibo_is_device_present(void) {
    plat_mutex_lock(&g_amiibo_lock);
    bool res = g_device_present;
    plat_mutex_unlock(&g_amiibo_lock);
    return res;
}

void amiibo_start_detection(void) {
    plat_mutex_lock(&g_amiibo_lock);
    g_detecting = true;
    rt_log("amiibo: start detection");
    plat_mutex_unlock(&g_amiibo_lock);
}

void amiibo_stop_detection(void) {
    plat_mutex_lock(&g_amiibo_lock);
    g_detecting = false;
    rt_log("amiibo: stop detection");
    plat_mutex_unlock(&g_amiibo_lock);
}

bool amiibo_is_detecting(void) {
    plat_mutex_lock(&g_amiibo_lock);
    bool res = g_detecting;
    plat_mutex_unlock(&g_amiibo_lock);
    return res;
}

bool amiibo_mount(void) {
    plat_mutex_lock(&g_amiibo_lock);
    if (!g_device_present || !g_tag_present) {
        plat_mutex_unlock(&g_amiibo_lock);
        return false;
    }
    g_mounted = true;
    rt_log("amiibo: mounted tag '%s'", g_info.filename[0] ? g_info.filename : "unknown");
    plat_mutex_unlock(&g_amiibo_lock);
    return true;
}

void amiibo_unmount(void) {
    plat_mutex_lock(&g_amiibo_lock);
    g_mounted = false;
    rt_log("amiibo: unmounted tag");
    plat_mutex_unlock(&g_amiibo_lock);
}

bool amiibo_is_mounted(void) {
    plat_mutex_lock(&g_amiibo_lock);
    bool res = g_mounted;
    plat_mutex_unlock(&g_amiibo_lock);
    return res;
}

AmiiboDeviceState amiibo_get_device_state(void) {
    plat_mutex_lock(&g_amiibo_lock);
    if (!g_device_present) {
        plat_mutex_unlock(&g_amiibo_lock);
        return AMIIBO_DEV_STATE_UNAVAILABLE;
    }
    if (g_mounted) {
        plat_mutex_unlock(&g_amiibo_lock);
        return AMIIBO_DEV_STATE_TAG_MOUNTED;
    }
    if (g_tag_present) {
        plat_mutex_unlock(&g_amiibo_lock);
        return AMIIBO_DEV_STATE_TAG_FOUND;
    }
    if (g_detecting) {
        plat_mutex_unlock(&g_amiibo_lock);
        return AMIIBO_DEV_STATE_SEARCHING;
    }
    plat_mutex_unlock(&g_amiibo_lock);
    return AMIIBO_DEV_STATE_INITIALIZED;
}

bool amiibo_load_file(const char *path) {
    if (!path || !*path) return false;
    uint64_t size = 0;
    uint8_t *data = plat_file_read(path, &size);
    if (!data) {
        rt_log("amiibo: failed to read file: %s", path);
        return false;
    }
    if (size != AMIIBO_DUMP_SIZE) {
        rt_log("amiibo: invalid dump size (%llu bytes, expected exactly %d): %s",
               (unsigned long long)size, AMIIBO_DUMP_SIZE, path);
        free(data);
        return false;
    }
    if (!amiibo_validate_dump(data, size)) {
        rt_log("amiibo: dump failed NTAG215 structure sanity checks: %s", path);
        free(data);
        return false;
    }

    plat_mutex_lock(&g_amiibo_lock);
    memcpy(g_dump, data, AMIIBO_DUMP_SIZE);
    g_has_dump = true;
    parse_dump_info(path);
    plat_mutex_unlock(&g_amiibo_lock);

    free(data);
    rt_log("amiibo: loaded dump '%s' (char_id: %02x%02x%02x, series: %02x, num: %04x)",
           g_info.filename, g_info.character_id[0], g_info.character_id[1], g_info.character_id[2],
           g_info.series_id, g_info.numbering_id);
    return true;
}

void amiibo_tap_tag(void) {
    plat_mutex_lock(&g_amiibo_lock);
    if (!g_has_dump) {
        plat_mutex_unlock(&g_amiibo_lock);
        return;
    }
    g_tag_present = true;
    g_tap_timestamp_ns = plat_now_ns();
    if (g_auto_remove) {
        g_timer_active = true;
    }
    rt_log("amiibo: tag tapped on reader ('%s')", g_info.filename);
    plat_mutex_unlock(&g_amiibo_lock);
}

bool amiibo_scan_and_tap(const char *path) {
    if (!amiibo_load_file(path)) return false;
    amiibo_tap_tag();
    return true;
}

void amiibo_remove_tag(void) {
    plat_mutex_lock(&g_amiibo_lock);
    if (g_tag_present || g_mounted) {
        g_tag_present = false;
        g_mounted = false;
        g_timer_active = false;
        rt_log("amiibo: tag removed from reader");
    }
    plat_mutex_unlock(&g_amiibo_lock);
}

void amiibo_tick(void) {
    plat_mutex_lock(&g_amiibo_lock);
    if (g_timer_active && g_tag_present) {
        uint64_t now = plat_now_ns();
        uint64_t diff = now > g_tap_timestamp_ns ? now - g_tap_timestamp_ns : 0;
        uint64_t limit_ns = (uint64_t)(g_auto_remove_delay_s * 1000000000.0f);
        if (diff >= limit_ns) {
            g_tag_present = false;
            g_mounted = false;
            g_timer_active = false;
            rt_log("amiibo: auto-removed tag after %.1fs timeout", (double)g_auto_remove_delay_s);
        }
    }
    plat_mutex_unlock(&g_amiibo_lock);
}

void amiibo_set_auto_remove(bool enable) {
    plat_mutex_lock(&g_amiibo_lock);
    g_auto_remove = enable;
    if (!enable) g_timer_active = false;
    plat_mutex_unlock(&g_amiibo_lock);
}

bool amiibo_get_auto_remove(void) {
    plat_mutex_lock(&g_amiibo_lock);
    bool res = g_auto_remove;
    plat_mutex_unlock(&g_amiibo_lock);
    return res;
}

void amiibo_set_auto_remove_delay_s(float seconds) {
    plat_mutex_lock(&g_amiibo_lock);
    if (seconds >= 0.5f && seconds <= 10.0f) g_auto_remove_delay_s = seconds;
    plat_mutex_unlock(&g_amiibo_lock);
}

float amiibo_get_auto_remove_delay_s(void) {
    plat_mutex_lock(&g_amiibo_lock);
    float res = g_auto_remove_delay_s;
    plat_mutex_unlock(&g_amiibo_lock);
    return res;
}

bool amiibo_has_tag(void) {
    plat_mutex_lock(&g_amiibo_lock);
    bool res = g_has_dump && g_tag_present;
    plat_mutex_unlock(&g_amiibo_lock);
    return res;
}

const AmiiboInfo *amiibo_get_current_info(void) {
    return &g_info;
}

const uint8_t *amiibo_get_raw_data(void) {
    return g_dump;
}

void amiibo_get_tag_info(void *out_tag_info) {
    plat_mutex_lock(&g_amiibo_lock);
    uint8_t *out = (uint8_t *)out_tag_info;
    memset(out, 0, 0x58);
    // UUID: offset 0x00, 10 bytes max
    memcpy(out + 0x00, g_dump, 7);
    out[0x0A] = 7;          // UUID length
    // Protocol at offset 0x20: TypeA = 1
    *(uint32_t *)(out + 0x20) = 1;
    // TagType at offset 0x24: Type2 / NTAG215 = 2
    *(uint32_t *)(out + 0x24) = 2;
    plat_mutex_unlock(&g_amiibo_lock);
}

void amiibo_get_model_info(void *out_model_info) {
    plat_mutex_lock(&g_amiibo_lock);
    uint8_t *out = (uint8_t *)out_model_info;
    memset(out, 0, 0x40);
    // Character ID at 0x00 (3 bytes)
    memcpy(out + 0x00, g_dump + 0x54, 3);
    // Series ID at 0x03 (1 byte)
    out[0x03] = g_dump[0x57];
    // Numbering ID at 0x04 (2 bytes, big-endian from 0x58)
    out[0x04] = g_dump[0x58];
    out[0x05] = g_dump[0x59];
    // NFP Type at 0x06 (1 byte)
    out[0x06] = g_dump[0x5A];
    plat_mutex_unlock(&g_amiibo_lock);
}

void amiibo_get_common_info(void *out_common_info) {
    plat_mutex_lock(&g_amiibo_lock);
    uint8_t *out = (uint8_t *)out_common_info;
    memset(out, 0, 0x40);
    // Last write date: 0x00..0x03
    out[0x00] = 0x07; out[0x01] = 0xE7; // 2023
    out[0x02] = 0x05; out[0x03] = 0x0C; // May 12
    // Write counter: 0x04..0x05
    out[0x04] = 0x00; out[0x05] = 0x01;
    // Version: 0x06
    out[0x06] = 0x02;
    // Application area size: 0x08..0x0B = 0xD8 (216 bytes)
    *(uint32_t *)(out + 0x08) = 0xD8;
    plat_mutex_unlock(&g_amiibo_lock);
}

void amiibo_get_register_info(void *out_register_info) {
    plat_mutex_lock(&g_amiibo_lock);
    uint8_t *out = (uint8_t *)out_register_info;
    memset(out, 0, 0x100);
    // Mii CharInfo at 0x00..0x57 (dummy valid mii)
    out[0x00] = 0x03; // version 3
    // First write date at 0x58..0x5B
    out[0x58] = 0x07; out[0x59] = 0xE7;
    out[0x5A] = 0x05; out[0x5B] = 0x0C;
    // Amiibo Name at 0x5C: copy up to 40 bytes NUL-terminated
    const char *name = g_info.filename[0] ? g_info.filename : "Amiibo";
    snprintf((char *)(out + 0x5C), 0x29, "%s", name);
    plat_mutex_unlock(&g_amiibo_lock);
}

int amiibo_get_application_area(void *out_buf, uint32_t max_size) {
    plat_mutex_lock(&g_amiibo_lock);
    if (!g_has_dump || !out_buf) {
        plat_mutex_unlock(&g_amiibo_lock);
        return 0;
    }
    uint32_t to_copy = max_size < 0xD8 ? max_size : 0xD8;
    // Decrypted/payload application area starts at offset 0xDC (or 0x0A0..0x178 in raw dump)
    if (0xDC + to_copy <= AMIIBO_DUMP_SIZE) {
        memcpy(out_buf, g_dump + 0xDC, to_copy);
    } else {
        memset(out_buf, 0, to_copy);
    }
    plat_mutex_unlock(&g_amiibo_lock);
    return (int)to_copy;
}
