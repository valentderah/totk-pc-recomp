#include <stdatomic.h>
#include <stdio.h>
#include "cfg.h"
#include "dlaa_preset.h"
#include "dlaa_settings.h"

static unsigned requested;
static _Atomic int evaluated;

int dlaa_settings_init(const char *seed) {
    unsigned selected;
    if (seed && *seed) {
        if (!dp_select(seed, &selected)) return 0;
        cfg_set("TOTK_DLAA_PRESET", selected == 11 ? "K" : "L");
        cfg_save(); // existing menu save-failure indicator reports unsuccessful writes
    }
    if (!dp_select(cfg_value("TOTK_DLAA_PRESET"), &selected)) return 0;
    requested = selected;
    atomic_store_explicit(&evaluated, 0, memory_order_relaxed);
    return 1;
}

unsigned dlaa_settings_requested(void) { return requested; }

void dlaa_settings_evaluated(int success) {
    atomic_store_explicit(&evaluated, success == 1 ? 1 : -1, memory_order_relaxed);
}

void dlaa_settings_note(char *out, size_t capacity) {
    unsigned selected = 0;
    int valid = dp_select(cfg_value("TOTK_DLAA_PRESET"), &selected);
    int state = atomic_load_explicit(&evaluated, memory_order_relaxed);
    const char *name = requested == 11 ? "K" : "L";
    const char *pending = !valid ? " | Invalid selection" : selected != requested ? " | Restart to apply" : "";
    if (state == 1) snprintf(out, capacity, "Active: %s%s", name, pending);
    else if (state < 0) snprintf(out, capacity, "DLAA unavailable%s", pending);
    else snprintf(out, capacity, "Waiting for DLAA: %s%s", name, pending);
}
