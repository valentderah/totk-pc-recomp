// Observational input research: the generated wrappers pass scalar copies after the original query returns.
// This file never reads or writes guest memory. Query names deliberately describe fields, not gameplay actions.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include "input_player_state.h"

#define SLOTS 8192
#define EVENT_LIMIT 200000
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_file;
static unsigned events;
static uint64_t polls, buttons;
static int32_t axes[4];
static int have_input;
// Existing runtime clock: -1 before a save has loaded, then microseconds of stepped game time.
extern int64_t frame_step_world_us(void);
static struct Entry {
    uint64_t site, object, id, caller, value, last_ns, count;
    const char *type;
} entries[SLOTS];
static struct Controller {
    int used;
    uint64_t site, caller, index, object, last_ns, calls;
    uint32_t held, pressed, released, repeat;
    char name[64];
} controllers[256];

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

static int event_begin(const char *type) {
    if (!log_file || events >= EVENT_LIMIT) return 0;
    fprintf(log_file, "{\"type\":\"%s\",\"ns\":%llu,\"poll\":%llu,\"world_us\":%lld", type,
            (unsigned long long)now_ns(), (unsigned long long)polls, (long long)frame_step_world_us());
    events++;
    return 1;
}

static void event_end(void) {
    fputs("}\n", log_file);
    if (events == EVENT_LIMIT)
        fputs("{\"type\":\"limit\",\"message\":\"event limit reached; logging stopped\"}\n", log_file);
}

__attribute__((constructor)) static void logger_open(void) {
    const char *path = getenv("TOTK_QUERY_LOG");
    if (!path || !*path) return;
    log_file = fopen(path, "wx");
    if (!log_file) { perror("input query logger: create log"); return; }
    setvbuf(log_file, NULL, _IOLBF, 0);
    fprintf(log_file, "{\"type\":\"session\",\"ns\":%llu,\"schema\":1,"
                     "\"queries\":\"unverified field-bit candidates\",\"event_limit\":%u}\n",
            (unsigned long long)now_ns(), EVENT_LIMIT);
    fprintf(stderr, "Input query research log: %s\n", path);
}

__attribute__((destructor)) static void logger_close(void) {
    if (log_file) fclose(log_file);
}

static void result_observe(const char *type, uint64_t site, uint64_t object, uint64_t id, uint64_t caller, uint64_t result) {
    if (!log_file) return;
    pthread_mutex_lock(&lock);
    uint64_t h = (site ^ (object >> 4) ^ (caller >> 2) ^ id) * 0x9e3779b97f4a7c15ull;
    struct Entry *e = NULL;
    for (unsigned i = 0; i < SLOTS; i++) {
        struct Entry *p = &entries[((h >> 32) + i) & (SLOTS - 1)];
        if (!p->site || (p->site == site && p->object == object && p->id == id && p->caller == caller)) {
            e = p; break;
        }
    }
    uint64_t t = now_ns();
    if (!e) {
        if (event_begin("table_full")) event_end();
        // Bound both the table and output in pathological runs.
        events = EVENT_LIMIT;
    } else {
        e->count++;
        if (!e->site || strcmp(e->type, type) || e->value != result || t - e->last_ns >= 1000000000ull) {
            if (event_begin(type)) {
                fprintf(log_file, ",\"site\":\"0x%llx\",\"object\":\"0x%llx\",\"id\":%llu,"
                                  "\"caller\":\"0x%llx\",\"result\":%llu,\"calls\":%llu,\"buttons\":%llu",
                        (unsigned long long)site, (unsigned long long)object, (unsigned long long)id,
                        (unsigned long long)caller, (unsigned long long)result,
                        (unsigned long long)e->count, (unsigned long long)buttons);
                event_end();
            }
            e->last_ns = t;
        }
        e->site = site; e->object = object; e->id = id; e->caller = caller; e->value = result;
        e->type = type;
    }
    pthread_mutex_unlock(&lock);
}

void input_query_observe(uint64_t site, uint64_t object, uint64_t id, uint64_t caller, uint64_t result) {
    result_observe("query", site, object, id, caller, result);
}

void input_output_observe(uint64_t site, uint64_t object, uint64_t field, uint64_t caller, uint64_t result) {
    result_observe("output_mask", site, object, field, caller, result);
}

void input_state_observe(uint64_t site, uint64_t object, uint64_t field, uint64_t caller, uint64_t value) {
    result_observe("object_state", site, object, field, caller, value);
}

void input_scalar_observe(uint64_t site, uint64_t object, uint64_t caller, uint64_t value) {
    result_observe("scalar_result", site, object, 0, caller, value);
}

void input_return_field_observe(uint64_t site, uint64_t object, uint64_t field, uint64_t caller,
                               uint64_t value, int valid) {
    result_observe(valid ? "return_field" : "return_field_unavailable", site, object, field, caller, value);
}

void input_player_state_observe(uint64_t site, int from_publisher, const struct InputPlayerState *s) {
    static struct InputPlayerState previous;
    static uint64_t previous_site, last_ns, samples, publisher_samples, poll_samples;
    if (!log_file) return;
    pthread_mutex_lock(&lock);
    samples++;
    if (from_publisher) publisher_samples++; else poll_samples++;
    const uint64_t now = now_ns();
    if (site != previous_site || memcmp(s, &previous, sizeof *s) || now - last_ns >= 1000000000ull) {
        if (event_begin("player_state")) {
            fprintf(log_file, ",\"site\":\"0x%llx\",\"actor\":\"0x%llx\",\"source\":\"%s\","
                              "\"identity\":%d,\"status\":%u,\"flags\":%u,\"known\":%u,\"slots_known\":%u,"
                              "\"verbs\":[%d,%d,%d,%d,%d],\"ability\":%d,"
                              "\"samples\":%llu,\"publisher_samples\":%llu,\"poll_samples\":%llu",
                    (unsigned long long)site, (unsigned long long)s->actor, from_publisher ? "publisher" : "poll",
                    s->identity, s->status, s->flags, s->known, s->slots_known,
                    s->verbs[0], s->verbs[1], s->verbs[2], s->verbs[3], s->verbs[4], s->ability,
                    (unsigned long long)samples, (unsigned long long)publisher_samples, (unsigned long long)poll_samples);
            event_end();
        }
        previous = *s; previous_site = site; last_ns = now;
    }
    pthread_mutex_unlock(&lock);
}

void input_action_observe(uint64_t site, uint64_t object, int pressed, int held, int released) {
    result_observe("virtual_action", site, object, 0, 0, pressed | (held << 1) | (released << 2));
}

void input_context_observe(uint64_t global, uint32_t hash, int status) {
    // 0 unknown/unavailable, 1 clear, 2 set. Missing data is never interpreted as an inactive flag.
    result_observe("context_flag", global, 0, hash, 0, (uint64_t)(status + 1));
}

void input_context_word_observe(uint64_t global, unsigned index, uint32_t word) {
    result_observe("context_word", global, 0, index, 0, word);
}

void input_context_entry_observe(uint64_t global, uint32_t hash, uint8_t bit) {
    result_observe("context_entry", global, 0, hash, 1, bit);
}

void input_position_observe(uint64_t site, uint64_t actor, float x, float y, float z) {
    if (!log_file || !isfinite(x) || !isfinite(y) || !isfinite(z)) return;
    static uint64_t previous_site, previous_actor;
    static int64_t previous_clock = -1;
    int64_t clock = frame_step_world_us();
    pthread_mutex_lock(&lock);
    if (site != previous_site || actor != previous_actor || clock < previous_clock || clock - previous_clock >= 50000) {
        if (event_begin("actor_position")) {
            fprintf(log_file, ",\"site\":\"0x%llx\",\"actor\":\"0x%llx\",\"position\":[%.6f,%.6f,%.6f]",
                    (unsigned long long)site, (unsigned long long)actor, x, y, z);
            event_end();
        }
        previous_site = site; previous_actor = actor; previous_clock = clock;
    }
    pthread_mutex_unlock(&lock);
}

static pthread_once_t filter_once = PTHREAD_ONCE_INIT;
static uint64_t filter_site, filter_clear;

static void filter_init(void) {
    const char *s = getenv("TOTK_MASK_CLEAR");
    if (!s || !*s) return;
    if (*s == '-') goto invalid;
    char *end;
    errno = 0;
    uint64_t site = strtoull(s, &end, 0);
    if (errno || end == s || *end != ':') goto invalid;
    s = end + 1;
    if (*s == '-') goto invalid;
    errno = 0;
    uint64_t bits = strtoull(s, &end, 0);
    if (errno || end == s || *end || !site || !bits) goto invalid;
    filter_site = site; filter_clear = bits;
    fprintf(stderr, "Research mask clear enabled: site 0x%llx bits 0x%llx\n",
            (unsigned long long)site, (unsigned long long)bits);
    return;
invalid:
    fprintf(stderr, "Invalid TOTK_MASK_CLEAR; filter disabled\n");
}

uint64_t input_output_filter(uint64_t site, uint64_t field, uint64_t value) {
    pthread_once(&filter_once, filter_init);
    uint64_t result = site == filter_site ? value & ~filter_clear : value;
    if (result != value) result_observe("filtered_mask", site, 0, field, 0, result);
    return result;
}

static void json_string(const char *s) {
    fputc('"', log_file);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', log_file);
        if (*p < 32 || *p >= 127) fprintf(log_file, "\\u%04x", *p);
        else fputc(*p, log_file);
    }
    fputc('"', log_file);
}

void input_controller_observe(uint64_t site, uint64_t caller, uint64_t index, const char *name, uint64_t object,
                              uint32_t held, uint32_t pressed, uint32_t released, uint32_t repeat) {
    const char *controller_events=getenv("TOTK_QUERY_CONTROLLERS");
    if(controller_events && !strcmp(controller_events,"0"))return;
    if (!log_file) return;
    pthread_mutex_lock(&lock);
    struct Controller *e = NULL;
    for (unsigned i = 0; i < sizeof controllers / sizeof *controllers; i++) {
        struct Controller *p = &controllers[i];
        if (!p->used || (p->site == site && p->caller == caller && p->index == index && !strcmp(p->name, name))) {
            e = p; break;
        }
    }
    uint64_t t = now_ns();
    if (!e) {
        if (event_begin("controller_table_full")) event_end();
        events = EVENT_LIMIT;
    } else {
        e->calls++;
        if (!e->used || e->object != object || e->held != held || e->pressed != pressed ||
                e->released != released || e->repeat != repeat || t - e->last_ns >= 1000000000ull) {
            if (event_begin("controller")) {
                fputs(",\"name\":", log_file); json_string(name);
                fprintf(log_file, ",\"site\":\"0x%llx\",\"caller\":\"0x%llx\",\"index\":%llu,"
                                  "\"object\":\"0x%llx\",\"held\":%u,\"pressed\":%u,\"released\":%u,"
                                  "\"repeat\":%u,\"calls\":%llu",
                        (unsigned long long)site, (unsigned long long)caller, (unsigned long long)index,
                        (unsigned long long)object, held, pressed, released, repeat, (unsigned long long)e->calls);
                event_end();
            }
            e->last_ns = t;
        }
        e->used = 1; e->site = site; e->caller = caller; e->index = index; e->object = object;
        e->held = held; e->pressed = pressed; e->released = released; e->repeat = repeat;
        snprintf(e->name, sizeof e->name, "%s", name);
    }
    pthread_mutex_unlock(&lock);
}

void __real_input_poll(uint64_t *, int32_t *, int32_t *, int32_t *, int32_t *);
extern void input_context_tick(void) __attribute__((weak));
extern void input_player_state_tick(void) __attribute__((weak));
void __wrap_input_poll(uint64_t *b, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    __real_input_poll(b, lx, ly, rx, ry);
    if (input_context_tick) input_context_tick();
    if (input_player_state_tick) input_player_state_tick();
    if (!log_file) return;
    pthread_mutex_lock(&lock);
    polls++;
    int changed = !have_input || buttons != *b;
    int32_t next[4] = {*lx, *ly, *rx, *ry};
    // Quantize only the recorded axes to avoid a line for every small analog fluctuation.
    for (int i = 0; i < 4; i++) if (axes[i] / 4096 != next[i] / 4096) changed = 1;
    buttons = *b;
    for (int i = 0; i < 4; i++) axes[i] = next[i];
    have_input = 1;
    if (changed && event_begin("input")) {
        fprintf(log_file, ",\"buttons\":%llu,\"axes\":[%d,%d,%d,%d]",
                (unsigned long long)*b, *lx, *ly, *rx, *ry);
        event_end();
    }
    pthread_mutex_unlock(&lock);
}
