// Discord Rich Presence: "Playing TotK Recomp" with the elapsed play time in the player's Discord status, through
// the local IPC of a running Discord client (platform.h's plat_ipc_*). Nothing leaves the machine but the activity
// itself, and nothing is sent while TOTK_DISCORD_RPC is off (the default: the status is information about the
// player's session). TOTK_DISCORD_APP_ID names the application whose name and artwork Discord shows — the default
// is the app this feature was developed against, registered as "TotK Recomp"; a player who wants their own name or
// art registers an application at discord.com/developers and puts its id in totk.ini — and TOTK_DISCORD_IMAGE the
// key of its uploaded art asset.
//
// One host thread owns the connection and never holds a virtual core (kernel.c: host work runs without one). It
// re-reads the settings when cfg_version changes (a menu change reaches it within half a second), connects to the
// first of Discord's discord-ipc-0..9 pipes that answers, and writes the activity again every 60 s: that write is
// the keep-alive, and one that fails closes the connection and tries again 15 s later. Discord clears the status by
// itself when the process ends and the pipe closes, so there is no shutdown path. Every answer frame is read and
// discarded: unread, they would fill the pipe's buffer and block the client's writes.
//
// The protocol is the documented IPC one (discord-rpc's src/rpc_connection.h): a frame is <opcode u32 little
// endian><length u32><length bytes of JSON>, with Handshake 0, Frame 1, Close 2, Ping 3 and Pong 4. The handshake
// (opcode 0) carries {"v":1,"client_id":...} and is answered by a Ready frame (opcode 1); every command,
// SET_ACTIVITY here, is a frame (opcode 1) answered by one, and a Ping the client sends is echoed as a Pong. A
// client that speaks another numbering (an early arRPC did) answers the handshake with a Close. --probe-discord
// runs the same exchange once, without the game and without the setting being on.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

enum { OP_HANDSHAKE = 0, OP_FRAME = 1, OP_CLOSE = 2, OP_PING = 3, OP_PONG = 4 };

#define APP_ID_CAP 24    // a Discord snowflake: at most 20 digits
#define IMAGE_KEY_CAP 64
#define ANSWER_CAP 8192  // the Ready frame carries the client's config; a few KiB

// A settings value that fits the JSON below without escaping: the app id is digits, an asset key letters, digits,
// '_' and '-' (what Discord's developer portal accepts for an asset key).
static int valid_app_id(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 20) return 0;
    for (const char *p = s; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    return 1;
}

static int valid_image_key(const char *s) {
    size_t n = strlen(s);
    if (!n || n >= IMAGE_KEY_CAP) return 0;
    for (const char *p = s; *p; p++)
        if ((*p < 'a' || *p > 'z') && (*p < 'A' || *p > 'Z') && (*p < '0' || *p > '9') && *p != '_' && *p != '-')
            return 0;
    return 1;
}

// The setting's value, or its default when the value does not fit the protocol (logged: the player sees why their
// own app does not show). Called on the presence thread and by the probe, after cfg_version changed or once.
static void read_setting(const char *name, char *out, size_t cap, int (*valid)(const char *)) {
    const char *v = cfg_value(name);
    if (valid(v)) {
        snprintf(out, cap, "%s", v);
        return;
    }
    snprintf(out, cap, "%s", cfg_meta(name)->def);
    rt_log("discord: %s does not fit what the Rich Presence protocol takes; using %s", name + strlen("TOTK_"), out);
}

static void le32(uint8_t *at, uint32_t v) {
    at[0] = (uint8_t)v, at[1] = (uint8_t)(v >> 8), at[2] = (uint8_t)(v >> 16), at[3] = (uint8_t)(v >> 24);
}

static int write_frame(PlatIpc *p, uint32_t op, const char *json) {
    uint8_t head[8];
    le32(head, op);
    le32(head + 4, (uint32_t)strlen(json));
    return plat_ipc_write(p, head, sizeof head) && plat_ipc_write(p, json, strlen(json));
}

// One frame off the wire, its 8-byte header in as many reads as the peer made, then its payload; a payload longer
// than cap-1 fills buf with its first cap-1 bytes and the rest is still consumed. *op gets the opcode; buf is
// NUL-terminated. 1 on a whole frame, 0 when the deadline passed, -1 when the connection broke.
static int read_one_frame(PlatIpc *p, uint32_t *op, char *buf, size_t cap, uint64_t deadline) {
    uint8_t head[8];
    size_t got = 0;
    while (got < sizeof head) {
        uint32_t left = deadline > plat_now_ms() ? (uint32_t)(deadline - plat_now_ms()) : 0;
        int n = plat_ipc_read(p, head + got, sizeof head - got, left);
        if (n < 0) return -1;
        if (!n) {
            if (plat_now_ms() >= deadline) return 0;
            continue;
        }
        got += (size_t)n;
    }
    *op = head[0] | head[1] << 8 | head[2] << 16 | (uint32_t)head[3] << 24;
    uint32_t len = head[4] | head[5] << 8 | head[6] << 16 | (uint32_t)head[7] << 24;
    size_t taken = 0;
    while (taken < len) {
        size_t want = len - taken > cap - 1 ? cap - 1 : len - taken;
        uint32_t left = deadline > plat_now_ms() ? (uint32_t)(deadline - plat_now_ms()) : 0;
        int n = plat_ipc_read(p, buf, want, left);
        if (n < 0) return -1;
        if (!n) {
            if (plat_now_ms() >= deadline) return 0;
            continue;
        }
        taken += (size_t)n;
    }
    buf[taken < cap - 1 ? taken : cap - 1] = 0;
    return 1;
}

// The next frame that is not a Ping: a Ping the client sent to see we are alive is echoed as a Pong with its own
// payload, a Close (the client told us it is done) counts as a broken connection so the caller connects again
// later. Same return values as read_one_frame; the deadline covers the whole exchange.
static int read_frame(PlatIpc *p, uint32_t *op, char *buf, size_t cap, uint32_t ms) {
    uint64_t deadline = plat_now_ms() + ms;
    for (;;) {
        int r = read_one_frame(p, op, buf, cap, deadline);
        if (r != 1) return r;
        if (*op == OP_PING) {
            uint8_t head[8];
            size_t n = strlen(buf);
            le32(head, OP_PONG);
            le32(head + 4, (uint32_t)n);
            if (!plat_ipc_write(p, head, sizeof head) || !plat_ipc_write(p, buf, n)) return -1;
            continue;
        }
        if (*op == OP_CLOSE) return -1;
        return 1;
    }
}

// The first of Discord's pipes that completes a handshake. NULL when no client is running or none answered in time;
// the caller tries again later.
static PlatIpc *connect_client(const char *app_id) {
    for (int i = 0; i < 10; i++) {
        char name[32], json[160];
        snprintf(name, sizeof name, "discord-ipc-%d", i);
        PlatIpc *p = plat_ipc_connect(name);
        if (!p) continue;
        snprintf(json, sizeof json, "{\"v\":1,\"client_id\":\"%s\"}", app_id);
        if (!write_frame(p, OP_HANDSHAKE, json)) {
            plat_ipc_close(p);
            continue;
        }
        uint32_t op = 0;
        char answer[ANSWER_CAP];
        if (read_frame(p, &op, answer, sizeof answer, 2000) == 1 && op == OP_FRAME) return p;   // the Ready event
        plat_ipc_close(p);   // this pipe answered but not with a Ready frame in 2 s: the next may belong to a client
    }
    return NULL;
}

static uint32_t next_nonce(void) {
    static volatile int32_t n;
    return (uint32_t)plat_inc32(&n);
}

// The activity as one SET_ACTIVITY frame: the game update as the details line, the process's start as the timestamp
// Discord counts the elapsed time from, and the application's art asset. 1 when the client answered (a slow answer
// that ran out of the 2 s read is fine: the next drain takes it), 0 when the connection broke.
static int send_activity(PlatIpc *p, const char *image) {
    char json[512];
    int v = rt_game_version;
    uint64_t start = (plat_unix_time_ns() - plat_process_uptime_ns()) / 1000000000ull;
    snprintf(json, sizeof json,
             "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%u\",\"args\":{\"pid\":%lu,\"activity\":{\"details\":\"Update "
             "%d.%d.%d\",\"timestamps\":{\"start\":%llu},\"assets\":{\"large_image\":\"%s\",\"large_text\":\"TotK "
             "Recomp %s\"}}}}",
             next_nonce(), (unsigned long)plat_process_id(), v / 100, v / 10 % 10, v % 10,
             (unsigned long long)start, image, rt_port_version);
    if (!write_frame(p, OP_FRAME, json)) return 0;
    uint32_t op = 0;
    char answer[ANSWER_CAP];
    return read_frame(p, &op, answer, sizeof answer, 2000) >= 0;
}

// ---- the presence thread

static void discord_main(void *arg) {
    (void)arg;
    unsigned seen = 0;
    char app_id[APP_ID_CAP], image[IMAGE_KEY_CAP];
    read_setting("TOTK_DISCORD_APP_ID", app_id, sizeof app_id, valid_app_id);
    read_setting("TOTK_DISCORD_IMAGE", image, sizeof image, valid_image_key);
    PlatIpc *ipc = NULL;
    uint64_t retry_at = 0, send_at = 0;
    int said_no_client = 0;
    for (;;) {
        unsigned v = cfg_version();
        if (v != seen) {   // the settings changed (the menu writes them): pick the new ones up at once
            seen = v;
            read_setting("TOTK_DISCORD_APP_ID", app_id, sizeof app_id, valid_app_id);
            read_setting("TOTK_DISCORD_IMAGE", image, sizeof image, valid_image_key);
            if (ipc) plat_ipc_close(ipc), ipc = NULL;   // the new app id needs a new handshake; the image is resent
            said_no_client = 0;
        }
        if (!cfg_bool("TOTK_DISCORD_RPC")) {
            if (ipc) plat_ipc_close(ipc), ipc = NULL;
            plat_sleep_ms(500);
            continue;
        }
        if (!ipc && plat_now_ms() >= retry_at) {
            ipc = connect_client(app_id);
            if (!ipc) {
                retry_at = plat_now_ms() + 15000;
                if (!said_no_client)
                    rt_log("discord: no client is running; trying again every 15 s while the setting is on");
                said_no_client = 1;
            } else {
                rt_log("discord: connected (app %s)", app_id);
                send_at = 0;
                said_no_client = 0;
            }
        }
        if (ipc && plat_now_ms() >= send_at) {
            if (send_activity(ipc, image)) {
                send_at = plat_now_ms() + 60000;
            } else {
                rt_log("discord: the connection broke; reconnecting");
                plat_ipc_close(ipc);
                ipc = NULL;
                retry_at = plat_now_ms() + 15000;
            }
        }
        plat_sleep_ms(500);
    }
}

void discord_init(void) {
    if (!plat_thread_spawn(discord_main, NULL, "discord", 0)) rt_log("discord: cannot start the presence thread");
}

// ---- --probe-discord: the exchange above without the game and the setting; needs no game files

int probe_discord(void) {
    static int fails;
#define CHECK(c, what) do { if (!(c)) { fails++; fprintf(stderr, "FAIL discord: %s\n", what); } } while (0)
    char app_id[APP_ID_CAP], image[IMAGE_KEY_CAP];
    read_setting("TOTK_DISCORD_APP_ID", app_id, sizeof app_id, valid_app_id);
    read_setting("TOTK_DISCORD_IMAGE", image, sizeof image, valid_image_key);
    PlatIpc *p = connect_client(app_id);
    if (!p) {
        fprintf(stderr, "discord: probe skipped: no client on discord-ipc-0..9\n");
        return 0;
    }
    fprintf(stderr, "discord: connected (app %s)\n", app_id);
    CHECK(send_activity(p, image), "SET_ACTIVITY written and answered");
    fprintf(stderr, "discord: activity set; the status shows it for ten seconds, then the connection closes\n");
    plat_sleep_ms(10000);
    plat_ipc_close(p);
    fprintf(stderr, "discord: probe %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
#undef CHECK
}
