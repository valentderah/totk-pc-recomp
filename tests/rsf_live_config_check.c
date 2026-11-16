// Exercise the real cross-thread selection snapshot, with a controllable settings writer.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform/platform.h"
#include "rsf_runtime.h"

static unsigned version;
static int enabled, backend = 3, fg, latency, interrupt_read;
unsigned cfg_version(void) { return version; }
int cfg_bool(const char *name) {
    assert(!strcmp(name, "TOTK_RSF_ENABLED"));
    if (interrupt_read) { interrupt_read = 0; version++; }
    return enabled;
}
int cfg_int(const char *name) {
    if (!strcmp(name, "TOTK_RSF_BACKEND")) return backend;
    if (!strcmp(name, "TOTK_RSF_FG")) return fg;
    assert(!strcmp(name, "TOTK_RSF_LOW_LATENCY"));
    return latency;
}
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
void rt_log(const char *format, ...) { (void)format; }
// Loader paths are linked with the same translation unit, but must never run in this contract test.
const char *cfg_get(const char *name) { (void)name; assert(0); return NULL; }
int plat_file_exists(const char *path) { (void)path; assert(0); return 0; }
int plat_exe_path(char *out, size_t cap) { (void)out; (void)cap; assert(0); return 0; }
void *plat_lib_open(const char *name) { (void)name; assert(0); return NULL; }
void *plat_lib_sym(void *lib, const char *name) { (void)lib; (void)name; assert(0); return NULL; }
void plat_lib_error(char *out, size_t cap) { (void)out; (void)cap; assert(0); }

int main(void) {
    assert(!rsf_requested() && !rsf_temporal_requested());
    assert(rsf_config_sr_backend() == 3 && !rsf_config_refresh());
    enabled = 1; backend = 5; version++;
    assert(!rsf_requested() && rsf_config_sr_backend() == 3); // menu writes do not change an in-progress frame
    interrupt_read = 1;
    assert(!rsf_config_refresh() && !rsf_requested()); // retry a read crossed by a settings write
    assert(rsf_config_refresh() && rsf_config_enabled() && rsf_config_sr_backend() == 5);
    assert(rsf_requested() == 1 && rsf_temporal_requested() == 1);
    assert(!rsf_config_refresh());
    for (int i = 1; i <= 5; i++) {
        enabled = 0; backend = i; fg = i == 2 ? 0 : i; version++;
        assert(rsf_config_refresh());
        assert(!rsf_config_enabled() && rsf_config_sr_backend() == i && rsf_config_fg_backend() == fg);
        assert(rsf_requested() == (fg != 0) && rsf_temporal_requested() == (fg != 0));
    }
    fg = 0; latency = 2; version++;
    assert(rsf_config_refresh() && !rsf_requested() && !rsf_config_low_latency()); // latency still needs startup
    enabled = 1; version++;
    assert(rsf_config_refresh());
    rsf_runtime_disable("test startup refusal");
    assert(!rsf_requested() && !rsf_temporal_requested());
    backend = 1; fg = 1; version++;
    assert(rsf_config_refresh() && !rsf_config_enabled() && !rsf_config_fg_backend());
    puts("RSF live config: frame publication, crossed writes, independent SR/FG and startup refusal passed");
    return 0;
}
