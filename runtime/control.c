// Control socket: a small local text protocol a companion app on the same PC (a progress map, a script) uses to read
// live game state, such as where Link is. Off unless TOTK_CONTROL=<port> is set, in the environment or as
// `control = <port>` in totk.ini (the environment wins). The Menu tab of the settings menu switches it while the game
// runs: control_frame sees the setting change and the socket thread opens or closes the listener, closing every
// connection when it goes off or to another port. The same protocol as Savage Wind's control
// endpoint (its docs/CONTROL.md), with only commands that read: nothing here writes guest memory or files, moves
// anything or ends the game.
//
// The socket listens on 127.0.0.1 only, never another address. One request per line, one JSON reply per line:
// {"ok":true,"data":{...}} or {"ok":false,"error":"..."}. A web page in a browser on the same PC can send a request
// to 127.0.0.1 too; a connection whose first line looks like HTTP is closed before anything runs, so a page cannot read
// the game through it. At most MAX_CLIENTS connections at once, MAX_PER_CLIENT requests waiting per connection (a
// client that sends more is not read until the game has answered); a line longer than MAX_LINE closes the connection.
//
// Threads: one host thread (it never holds a virtual core) owns every socket: it listens, accepts, reads lines and
// queues them, and sends the replies. It is started the first time the setting asks for a port and then stays, idle
// while the setting is off. The commands run in control_frame, once per presented frame on the thread that presents
// (nvn_api_obj.c, after the frame step), so they read the game between frames. With nothing queued that costs two
// atomic loads (the settings version and the queue count); no socket call ever runs on that thread.
//
// Commands (help lists them): help, pos, state. pos reads the Player actor as frame_rate.c's walk finds it
// (frame_step_player_actor): +0x2b4 its world position, then +0x2c0 its 3x3 rotation, a turn about y while he stands
// (row 0 is (cos a, 0, sin a)). The yaw a is measured as Savage Wind measures it: 0 looks along +z, 90 along +x. It was
// found on 1.4.3 by logging the actor's floats while a script walked Link right, back, left and forward: a matched the
// direction of each leg within 3 degrees. heading is the same direction as a map bearing, clockwise from north (-z).
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

#define MAX_CLIENTS 4
#define MAX_PER_CLIENT 8
#define MAX_QUEUE (MAX_CLIENTS * MAX_PER_CLIENT)
#define MAX_LINE 512
#define MAX_REPLY 2048
#define WAIT_MS 10   // how long the socket thread waits for input before it looks for replies to send

typedef struct {
    PlatTcp *sock;
    unsigned gen;         // bumped when the slot gets a new connection: a reply for an old one is dropped
    char line[MAX_LINE + 1];
    size_t used;
    int checked;          // the first line was looked at (not HTTP)
    int waiting;          // requests of this connection in the queue
} Client;

enum { FREE, PENDING, DONE };
typedef struct {
    int state, slot;
    unsigned gen;
    char line[MAX_LINE + 1];
    char reply[MAX_REPLY];
} Request;

static Client clients[MAX_CLIENTS];   // the socket thread's
static Request queue[MAX_QUEUE];      // under lock
static PlatMutex lock = PLAT_MUTEX_INIT;
static atomic_int pending;            // PENDING entries: control_frame takes the lock only when there are some
static PlatTcp *listener;            // the socket thread's
static int port;                     // the socket thread's: the port it last tried, 0 none
static atomic_int want;              // the port the setting asks for, 0 off: control_frame writes, the socket thread acts
static atomic_int status;            // the socket thread's answer to want: the port it listens on, -port when it could
                                     // not listen there, 0 off (control_status, for the menu)
static int running;                  // the socket thread was started (control_init, control_frame)
static unsigned cfg_seen;            // cfg_version at control_frame's last look

// ---- replies

typedef struct { char *s; size_t n, cap; int fields, failed; } Reply;

static void put(Reply *r, const char *fmt, ...) {
    if (r->n >= r->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(r->s + r->n, r->cap - r->n, fmt, ap);
    va_end(ap);
    r->n = k < 0 ? r->cap : r->n + (size_t)k > r->cap ? r->cap : r->n + (size_t)k;
}

// s as a JSON string: quotes, backslashes and control characters escaped (a command name echoes what a client sent)
static void put_str(Reply *r, const char *s) {
    put(r, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') put(r, "\\%c", *p);
        else if (*p < 0x20) put(r, "\\u%04x", *p);
        else put(r, "%c", *p);
    }
    put(r, "\"");
}

static void key(Reply *r, const char *k) {
    put(r, r->fields++ ? "," : "");
    put_str(r, k);
    put(r, ":");
}
static void field_str(Reply *r, const char *k, const char *v) { key(r, k), put_str(r, v); }
static void field_int(Reply *r, const char *k, long long v) { key(r, k), put(r, "%lld", v); }
static void field_bool(Reply *r, const char *k, int v) { key(r, k), put(r, v ? "true" : "false"); }
static void field_num(Reply *r, const char *k, double v, int decimals) {
    key(r, k);
    if (isfinite(v)) put(r, "%.*f", decimals, v);   // the runtime never sets a locale: the C locale's '.' point
    else put(r, "null");
}

// Turns the reply into {"ok":false,"error":...}: what the command had added is dropped.
static void fail(Reply *r, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    r->n = 0;
    r->failed = 1;
    put(r, "{\"ok\":false,\"error\":");
    put_str(r, msg);
    put(r, "}");
}

// ---- the game's state

// Link's position, and his yaw in degrees (NAN when the rotation is not a turn about y): 1 while he is in the world.
// Before a save loads the title screen keeps the Player at the origin (frame_rate.c world_clock).
static int player(float p[3], float *yaw) {
    uint64_t actor = frame_step_player_actor();
    float f[12];   // +0x2b4: x y z, then the rotation's rows
    *yaw = NAN;
    if (!actor || !plat_mem_read(actor + 0x2b4, f, sizeof f)) return 0;
    memcpy(p, f, 12);
    if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]) || fabsf(p[0]) + fabsf(p[2]) < 1.0f) return 0;
    float c = f[3], s = f[5], len = c * c + s * s;
    if (isfinite(len) && len > 0.5f && len < 1.5f) *yaw = atan2f(s, c) * (180.0f / 3.14159265f);
    return 1;
}

static uint64_t frames;                 // presents since the start (control_frame)
static uint64_t fps_t0, fps_frames;     // the last whole second's count
static double fps;

// ---- commands

typedef void (*CmdFn)(Reply *r);
typedef struct { const char *name, *help; CmdFn fn; } Cmd;
static void cmd_help(Reply *r);

static void cmd_pos(Reply *r) {
    float p[3], yaw;
    if (!player(p, &yaw)) {
        fail(r, "No save loaded");
        return;
    }
    field_num(r, "x", p[0], 2);
    field_num(r, "y", p[1], 2);
    field_num(r, "z", p[2], 2);
    if (isfinite(yaw)) {
        float h = 180.0f - yaw;
        field_num(r, "yaw", yaw, 1);
        field_num(r, "heading", h >= 360.0f ? h - 360.0f : h < 0.0f ? h + 360.0f : h, 1);
    }
    char place[160];
    int n = snprintf(place, sizeof place, "totk:1 x=%.2f y=%.2f z=%.2f", p[0], p[1], p[2]);
    if (isfinite(yaw) && n > 0 && (size_t)n < sizeof place) snprintf(place + n, sizeof place - (size_t)n, " yaw=%.1f", yaw);
    field_str(r, "place", place);
}

static void cmd_state(Reply *r) {
    float p[3], yaw;
    int v = rt_game_version;
    char game[16];
    snprintf(game, sizeof game, "%d.%d.%d", v / 100, v / 10 % 10, v % 10);
    field_str(r, "game", game);
    field_str(r, "recomp", rt_port_version);
    field_int(r, "frame", (long long)frames);
    field_num(r, "fps", fps, 1);
    field_num(r, "uptime_s", plat_process_uptime_ns() / 1e9, 1);
    int64_t w = frame_step_world_us();
    if (w >= 0) field_num(r, "world_s", w / 1e6, 1);   // game time since the save loaded
    field_bool(r, "in_world", player(p, &yaw));
}

static const Cmd cmds[] = {
    {"help", "the commands", cmd_help},
    {"pos", "Link's position (x y z), yaw (0 along +z, 90 along +x) and map heading (clockwise from north); an error "
            "before a save is loaded", cmd_pos},
    {"state", "game and recomp version, frame, fps, uptime, game time since the save loaded", cmd_state},
};

static void cmd_help(Reply *r) {
    key(r, "commands");
    put(r, "[");
    for (size_t i = 0; i < sizeof cmds / sizeof *cmds; i++) {
        put(r, i ? ",{\"name\":" : "{\"name\":");
        put_str(r, cmds[i].name);
        put(r, ",\"args\":\"\",\"help\":");
        put_str(r, cmds[i].help);
        put(r, "}");
    }
    put(r, "]");
}

// One request line into out (a whole JSON line with its '\n'). Commands take no arguments.
static void run(const char *line, char *out, size_t cap) {
    char name[64];
    size_t n = 0;
    while (*line == ' ' || *line == '\t') line++;
    while (line[n] && line[n] != ' ' && line[n] != '\t' && n < sizeof name - 1) name[n] = line[n], n++;
    name[n] = 0;
    const char *rest = line + n;
    while (*rest == ' ' || *rest == '\t') rest++;
    Reply r = {out, 0, cap - 2, 0, 0};   // room for the '\n' and the NUL
    put(&r, "{\"ok\":true,\"data\":{");
    const Cmd *c = NULL;
    for (size_t i = 0; i < sizeof cmds / sizeof *cmds; i++)
        if (!strcmp(cmds[i].name, name)) c = &cmds[i];
    if (!*name) fail(&r, "Empty line (try help)");
    else if (!c) fail(&r, "Unknown command: %s (try help)", name);
    else if (*rest) fail(&r, "Usage: %s", name);
    else c->fn(&r);
    if (!r.failed) put(&r, "}}");
    if (r.n >= r.cap) {   // cut: never send half a line
        r.n = 0, r.failed = 0;
        fail(&r, "Reply too long");
    }
    out[r.n] = '\n';
    out[r.n + 1] = 0;
}

static void apply(int p);

// TOTK_CONTROL as a port: 1 to 65535, 0 for off (unset, empty, 0), -1 for a value that is not a port.
static int port_setting(void) {
    const char *v = cfg_get("TOTK_CONTROL");
    if (!v || !*v || !strcmp(v, "0")) return 0;
    char *end;
    long p = strtol(v, &end, 10);
    return *end || p < 1 || p > 65535 ? -1 : (int)p;
}

void control_frame(uint64_t display_ns) {
    unsigned v = cfg_version();
    if (v != cfg_seen) cfg_seen = v, apply(port_setting());   // the menu changed a setting: maybe this one
    if (!running) return;
    frames++;
    if (!fps_t0) fps_t0 = display_ns;
    fps_frames++;
    if (display_ns - fps_t0 >= 1000000000ull) {
        fps = fps_frames * 1e9 / (double)(display_ns - fps_t0);
        fps_t0 = display_ns, fps_frames = 0;
    }
    if (!atomic_load_explicit(&pending, memory_order_acquire)) return;
    for (int i = 0; i < MAX_QUEUE; i++) {
        plat_mutex_lock(&lock);
        int mine = queue[i].state == PENDING;
        char line[MAX_LINE + 1];
        if (mine) memcpy(line, queue[i].line, sizeof line);
        plat_mutex_unlock(&lock);
        if (!mine) continue;
        char reply[MAX_REPLY];
        run(line, reply, sizeof reply);
        plat_mutex_lock(&lock);
        memcpy(queue[i].reply, reply, sizeof reply);
        queue[i].state = DONE;
        plat_mutex_unlock(&lock);
        atomic_fetch_sub_explicit(&pending, 1, memory_order_release);
    }
}

// ---- the socket thread

// A first line a browser would send: an HTTP method, or an HTTP version anywhere in it.
static int http_line(const char *s) {
    static const char *const methods[] = {"GET ", "POST ", "PUT ", "HEAD ", "OPTIONS ", "DELETE ", "PATCH ",
                                          "CONNECT ", "TRACE "};
    for (size_t i = 0; i < sizeof methods / sizeof *methods; i++)
        if (!strncmp(s, methods[i], strlen(methods[i]))) return 1;
    return strstr(s, "HTTP/") != NULL;
}

static void drop(int slot, const char *why) {
    Client *c = &clients[slot];
    if (why) rt_log("control: closed a connection: %s", why);
    plat_tcp_close(c->sock);
    c->sock = NULL;
    c->gen++;
    c->used = 0;
    c->checked = 0;
    c->waiting = 0;   // its queued requests still run; their replies are dropped (gen)
}

// A whole line from a client: checked, then queued for control_frame. 0 when the connection must close.
static int line_done(int slot) {
    Client *c = &clients[slot];
    c->line[c->used] = 0;
    if (c->used && c->line[c->used - 1] == '\r') c->line[--c->used] = 0;
    if (!c->checked) {
        c->checked = 1;
        if (http_line(c->line)) {
            drop(slot, "it sent an HTTP request (a web page?)");
            return 0;
        }
    }
    plat_mutex_lock(&lock);
    int put_at = -1;
    for (int i = 0; i < MAX_QUEUE && put_at < 0; i++)
        if (queue[i].state == FREE) put_at = i;
    if (put_at >= 0) {
        Request *q = &queue[put_at];
        q->state = PENDING, q->slot = slot, q->gen = c->gen;
        memcpy(q->line, c->line, c->used + 1);
        c->waiting++;
    }
    plat_mutex_unlock(&lock);
    if (put_at >= 0) atomic_fetch_add_explicit(&pending, 1, memory_order_release);
    c->used = 0;
    return 1;
}

static void read_client(int slot) {
    Client *c = &clients[slot];
    char buf[1024];
    int n = plat_tcp_recv(c->sock, buf, sizeof buf);
    if (n < 0) {
        drop(slot, NULL);   // the client went away
        return;
    }
    for (int i = 0; i < n && c->sock; i++) {
        if (buf[i] == '\n') {
            if (!line_done(slot)) return;
        } else if (c->used == MAX_LINE) {
            drop(slot, "a line longer than 512 bytes");
            return;
        } else {
            c->line[c->used++] = buf[i];
        }
    }
}

// The answered requests: to their connection when it is still the one that asked.
static void send_replies(void) {
    for (int i = 0; i < MAX_QUEUE; i++) {
        char reply[MAX_REPLY];
        int slot = -1;
        unsigned gen = 0;
        plat_mutex_lock(&lock);
        if (queue[i].state == DONE) {
            slot = queue[i].slot, gen = queue[i].gen;
            memcpy(reply, queue[i].reply, sizeof reply);
            queue[i].state = FREE;
        }
        plat_mutex_unlock(&lock);
        if (slot < 0) continue;
        Client *c = &clients[slot];
        if (!c->sock || c->gen != gen) continue;
        c->waiting--;
        if (!plat_tcp_send(c->sock, reply, strlen(reply))) drop(slot, "it stopped reading its replies");
    }
}

// The setting went off or to another port: the listener and every connection close.
static void stop_listening(void) {
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].sock) drop(i, NULL), n++;
    plat_tcp_close(listener);
    listener = NULL;
    rt_log("control: stopped listening on 127.0.0.1:%d (%d connection%s closed)", port, n, n == 1 ? "" : "s");
}

static void control_main(void *arg) {
    (void)arg;
    for (;;) {
        int w = atomic_load_explicit(&want, memory_order_acquire);
        if (w != port) {
            if (listener) stop_listening();
            port = w;
            if (w) {
                listener = plat_tcp_listen_local((uint16_t)w);
                if (listener)
                    rt_log("control: listening on 127.0.0.1:%d (read only: help, pos, state; local connections only)", w);
                else
                    rt_log("control: cannot listen on 127.0.0.1:%d (another program has the port?); the control socket "
                           "stays off until the setting changes", w);
            }
            atomic_store_explicit(&status, listener ? w : -w, memory_order_release);
        }
        if (!listener) {   // off: the answers for closed connections are dropped, and the setting is looked at again
            send_replies();
            plat_sleep_ms(100);
            continue;
        }
        PlatTcp *socks[1 + MAX_CLIENTS];
        int ready[1 + MAX_CLIENTS];
        socks[0] = listener;
        for (int i = 0; i < MAX_CLIENTS; i++)   // a client with a full queue is not read until it has answers
            socks[1 + i] = clients[i].waiting < MAX_PER_CLIENT ? clients[i].sock : NULL;
        if (plat_tcp_wait(socks, 1 + MAX_CLIENTS, ready, WAIT_MS) > 0) {
            if (ready[0]) {
                PlatTcp *s = plat_tcp_accept(listener);
                int slot = -1;
                for (int i = 0; s && i < MAX_CLIENTS && slot < 0; i++)
                    if (!clients[i].sock) slot = i;
                if (s && slot < 0) {
                    static const char busy[] = "{\"ok\":false,\"error\":\"Too many connections\"}\n";
                    plat_tcp_send(s, busy, sizeof busy - 1);
                    plat_tcp_close(s);
                } else if (s) {
                    clients[slot].sock = s;
                }
            }
            for (int i = 0; i < MAX_CLIENTS; i++)
                if (ready[1 + i] && clients[i].sock) read_client(i);
        }
        send_replies();
    }
}

static int last_setting;   // the port_setting apply last saw

// The setting asks for port p (0 off, -1 not a port): the socket thread is told, and started the first time. Runs on
// the present thread (control_frame) or the main thread (control_init); no socket call here.
static void apply(int p) {
    if (p == last_setting) return;
    last_setting = p;
    if (p < 0) {
        rt_log("control: TOTK_CONTROL = %s is not a port (1 to 65535); the control socket is off", cfg_get("TOTK_CONTROL"));
        p = 0;
    }
    atomic_store_explicit(&want, p, memory_order_release);
    if (p && !running) {
        if (plat_thread_spawn(control_main, NULL, "control", 0))
            running = 1;
        else
            rt_log("control: cannot start its thread"), last_setting = 0;   // the next change tries again
    }
}

int control_status(void) { return atomic_load_explicit(&status, memory_order_acquire); }

void control_init(void) {
    cfg_seen = cfg_version();
    apply(port_setting());
}

// ---- --probe-control: the socket without the game. This thread stands in for the present thread (control_frame
// every 2 ms) and is the client too. With no guest loaded the Player walk finds nothing, so pos answers its error.
// Port: TOTK_CONTROL when set, else 47651. Needs no game files.

// The next reply line of c into out, running the queued commands meanwhile: 1 a line, 0 none within ms, -1 closed.
static int probe_line(PlatTcp *c, char *out, size_t cap, uint32_t ms) {
    static char buf[4096];
    static size_t have;
    uint64_t deadline = plat_now_ms() + ms;
    for (;;) {
        char *nl = memchr(buf, '\n', have);
        if (nl) {
            size_t n = (size_t)(nl - buf);
            snprintf(out, cap, "%.*s", (int)n, buf);
            memmove(buf, nl + 1, have - n - 1);
            have -= n + 1;
            return 1;
        }
        control_frame(plat_now_ms() * 1000000ull);
        int ready = 0;
        if (plat_tcp_wait(&c, 1, &ready, 2) > 0 && ready) {
            int got = plat_tcp_recv(c, buf + have, sizeof buf - have);
            if (got < 0) return -1;
            have += (size_t)got;
        }
        if (plat_now_ms() >= deadline) return 0;
    }
}

int probe_control(void) {
    int fails = 0;
#define CHECK(c, what) do { if (!(c)) { fails++; fprintf(stderr, "FAIL control: %s\n", what); } else fprintf(stderr, "ok   control: %s\n", what); } while (0)
    const char *v = cfg_get("TOTK_CONTROL");
    long p = v && *v ? strtol(v, NULL, 10) : 47651;
    if (p < 1 || p > 65535) p = 47651;
    cfg_seen = cfg_version();   // control_frame below must not take the unset setting for a change to off
    apply((int)p);   // as a change of the setting would; the socket thread listens a moment later
    for (int i = 0; i < 300 && control_status() != p; i++) plat_sleep_ms(10);
    if (control_status() != p) {
        fprintf(stderr, "FAIL control: cannot listen on 127.0.0.1:%ld\n", p);
        return 1;
    }
    char line[MAX_REPLY];
    PlatTcp *c = plat_tcp_connect_local((uint16_t)p);
    CHECK(c != NULL, "a client connects to 127.0.0.1");
    if (c) {
        static const char req[] = "help\npos\r\nbogus thing\npos now\nstate\n";
        CHECK(plat_tcp_send(c, req, sizeof req - 1), "five requests in one send");
        CHECK(probe_line(c, line, sizeof line, 2000) == 1 && strstr(line, "{\"ok\":true,\"data\":{\"commands\":[") == line &&
                  strstr(line, "\"name\":\"pos\"") && strstr(line, "\"name\":\"state\""),
              "help lists pos and state");
        CHECK(probe_line(c, line, sizeof line, 2000) == 1 && !strcmp(line, "{\"ok\":false,\"error\":\"No save loaded\"}"),
              "pos before a save is loaded answers No save loaded (a \\r before the \\n is ignored)");
        CHECK(probe_line(c, line, sizeof line, 2000) == 1 &&
                  !strcmp(line, "{\"ok\":false,\"error\":\"Unknown command: bogus (try help)\"}"),
              "an unknown command is an error");
        CHECK(probe_line(c, line, sizeof line, 2000) == 1 && !strcmp(line, "{\"ok\":false,\"error\":\"Usage: pos\"}"),
              "an argument to pos is an error");
        CHECK(probe_line(c, line, sizeof line, 2000) == 1 && strstr(line, "\"in_world\":false") &&
                  strstr(line, "\"game\":\""),
              "state answers, not in the world");
        static const char half1[] = "he", half2[] = "lp\n";   // a line split across two sends
        plat_tcp_send(c, half1, sizeof half1 - 1);
        plat_sleep_ms(50);
        plat_tcp_send(c, half2, sizeof half2 - 1);
        CHECK(probe_line(c, line, sizeof line, 2000) == 1 && strstr(line, "{\"ok\":true") == line, "a line in two sends");
        plat_tcp_close(c);
    }
    PlatTcp *h = plat_tcp_connect_local((uint16_t)p);
    if (h) {
        static const char get[] = "GET /pos HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        plat_tcp_send(h, get, sizeof get - 1);
        CHECK(probe_line(h, line, sizeof line, 2000) == -1, "a connection that starts with an HTTP request is closed unanswered");
        plat_tcp_close(h);
    }
    PlatTcp *g = plat_tcp_connect_local((uint16_t)p);
    if (g) {
        char longline[600];
        memset(longline, 'x', sizeof longline);
        plat_tcp_send(g, "help\n", 5);
        CHECK(probe_line(g, line, sizeof line, 2000) == 1, "a new connection answers");
        plat_tcp_send(g, longline, sizeof longline);
        CHECK(probe_line(g, line, sizeof line, 2000) == -1, "a line over 512 bytes closes the connection");
        plat_tcp_close(g);
    }
    // the menu switching it off, then on again: the listener and its connections close, then it listens anew
    PlatTcp *k = plat_tcp_connect_local((uint16_t)p);
    if (k) {
        plat_tcp_send(k, "help\n", 5);
        CHECK(probe_line(k, line, sizeof line, 2000) == 1, "a connection before the switch answers");
        apply(0);
        for (int i = 0; i < 300 && control_status() != 0; i++) plat_sleep_ms(10);
        CHECK(control_status() == 0 && probe_line(k, line, sizeof line, 2000) == -1,
              "switched off: the listener and the connection close");
        plat_tcp_close(k);
        PlatTcp *off = plat_tcp_connect_local((uint16_t)p);
        CHECK(!off, "switched off: a new connection is refused");
        plat_tcp_close(off);
        apply((int)p);
        for (int i = 0; i < 300 && control_status() != p; i++) plat_sleep_ms(10);
        k = control_status() == p ? plat_tcp_connect_local((uint16_t)p) : NULL;
        if (k) plat_tcp_send(k, "pos\n", 4);
        CHECK(k && probe_line(k, line, sizeof line, 2000) == 1 && !strcmp(line, "{\"ok\":false,\"error\":\"No save loaded\"}"),
              "switched on again: it listens and answers");
        plat_tcp_close(k);
    }
    fprintf(stderr, "control: probe %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
#undef CHECK
}
