// Opt-in action-channel prototype for isolated research builds. All game-specific contracts come from verified plans.
#include "input_action_channel.h"
#include "input_map.h"
#include "input_wsys.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern int64_t frame_step_world_us(void);
extern int menu_is_open(void);  // ui/menu.h also requires renderer headers; only its scalar query is used here.
extern void input_action_observe(uint64_t, uint64_t, int, int, int);
extern uint64_t input_context_activation(void) __attribute__((weak));

void research_action_update(ResearchActionState *s, int64_t clock, uint64_t context, int down, int active) {
    down = !!down;
    if (s->seen && s->clock == clock && s->context == context && active) return;
    int transition = !s->seen || s->context != context || clock < s->clock || clock - s->clock > 100000;
    int before = transition ? 0 : s->held;
    if (!active || transition) s->blocked = down;
    if (!down) s->blocked = 0;
    s->held = active && down && !s->blocked;
    s->pressed = s->held && !before;
    s->released = !s->held && before;
    s->seen = 1; s->clock = clock; s->context = context;
}

static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t configured_site, fields[3], bit;
static int key, enabled, script_enabled;
static unsigned phases = 7;  // pressed, held, released; experiments may restrict this to the verified native contract.
static struct { int64_t at, end; } pulses[256];
static unsigned pulse_count;
static ResearchActionState state;
static uint64_t last_activation;
static _Thread_local struct { uint64_t site; int values[3]; } current;

static int parse_contract(const char *s) {
    uint64_t values[5];
    for (unsigned i = 0; i < 5; i++) {
        if (!s || *s < '0' || *s > '9') return 0;
        errno = 0;
        char *end;
        values[i] = strtoull(s, &end, 0);
        if (errno || end == s || (i == 4 ? *end != 0 : *end != ':')) return 0;
        s = end + 1;
    }
    if (!values[0] || !values[4] || (values[4] & (values[4] - 1))) return 0;
    for (unsigned i = 1; i < 4; i++) if (values[i] > 4096 || values[i] % 8) return 0;
    if (values[1] == values[2] || values[1] == values[3] || values[2] == values[3]) return 0;
    configured_site = values[0]; bit = values[4];
    for (unsigned i = 0; i < 3; i++) fields[i] = values[i + 1];
    return 1;
}

static void channel_init(void) {
    const char *contract = getenv("TOTK_JUMP_CHANNEL");
    if (!contract || !*contract) return;
    if (!parse_contract(contract)) goto invalid;
    const char *phase_config = getenv("TOTK_ACTION_PHASES");
    if (phase_config && *phase_config) {
        char *end; errno = 0;
        unsigned long value = strtoul(phase_config, &end, 0);
        if (*phase_config < '0' || *phase_config > '9' || errno || *end || !value || value > 7) goto invalid;
        phases = (unsigned)value;
    }
    const char *name = getenv("TOTK_JUMP_KEY");
    if (name && *name) {
        for (int i = 1; i < KEY_COUNT; i++) {
            const char *candidate = input_key_name(i);
            if (candidate && !strcasecmp(name, candidate)) { key = i; break; }
        }
        if (!key) goto invalid;
    }
    const char *script = getenv("TOTK_JUMP_SCRIPT");
    if (script && *script) {
        FILE *f = fopen(script, "r");
        if (!f) goto invalid;
        char line[256];
        int valid = 1;
        while (fgets(line, sizeof line, f)) {
            char *comment = strchr(line, '#');
            if (comment) *comment = 0;
            if (strspn(line, " \t\r\n") == strlen(line)) continue;
            long long at, duration; char extra;
            if (sscanf(line, "%lld %lld %c", &at, &duration, &extra) != 2 || at < 0 || duration <= 0 ||
                at > 3600000 || duration > 60000 || pulse_count == 256) { valid = 0; break; }
            pulses[pulse_count].at = at * 1000;
            pulses[pulse_count++].end = (at + duration) * 1000;
        }
        if (ferror(f)) valid = 0;
        fclose(f);
        if (!valid) goto invalid;
        script_enabled = 1;
    }
    if (!key && !script_enabled) goto invalid;
    enabled = 1;
    fprintf(stderr, "Research action channel enabled at 0x%llx; key %s, scripted %d\n",
            (unsigned long long)configured_site, key ? input_key_name(key) : "none", script_enabled);
    return;
invalid:
    enabled = 0;
    fprintf(stderr, "Invalid research action channel configuration; original masks preserved\n");
}

void input_action_channel_begin(uint64_t site, uint64_t object) {
    pthread_once(&once, channel_init);
    current.site = 0;
    if (!enabled || site != configured_site) return;
    int64_t clock = frame_step_world_us();
    if (clock < 0) return;
    int down = 0;
    for (unsigned i = 0; i < pulse_count; i++) if (clock >= pulses[i].at && clock < pulses[i].end) down = 1;
    int active = !menu_is_open();
    uint64_t activation = input_context_activation ? input_context_activation() : 0;
    if (activation) active &= (activation & 3) == 1;
    // Scripted experiments run independently of desktop focus. A physical provider always respects window focus.
    if (key) {
        int foreground = ikb_foreground();
        if (foreground) down |= ikb_down(key);
        if (!script_enabled) active &= foreground;
    }
    pthread_mutex_lock(&lock);
    if (activation != last_activation) {
        state.seen = 0;
        last_activation = activation;
    }
    research_action_update(&state, clock, object, down, active);
    current.site = site;
    current.values[0] = state.pressed; current.values[1] = state.held; current.values[2] = state.released;
    input_action_observe(site, object, state.pressed, state.held, state.released);
    pthread_mutex_unlock(&lock);
}

uint64_t input_action_channel_apply(uint64_t site, uint64_t field, uint64_t original) {
    if (site != current.site || !enabled) return original;
    for (unsigned i = 0; i < 3; i++) if (field == fields[i])
        return phases & (1u << i) ? (original & ~bit) | (current.values[i] ? bit : 0) : original;
    return original;
}
