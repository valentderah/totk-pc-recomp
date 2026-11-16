#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "rsf_runtime.h"
#include "platform/platform.h"

static const char *runtime_override, *provider_override;
static int packaged_runtime;
const char *cfg_get(const char *name) {
    return !strcmp(name, "TOTK_RSF_RUNTIME") ? runtime_override :
           !strcmp(name, "TOTK_RSF_PROVIDER_DIR") ? provider_override : NULL;
}
void plat_mutex_lock(PlatMutex *lock) { (void)lock; }
void plat_mutex_unlock(PlatMutex *lock) { (void)lock; }
int plat_exe_path(char *out, size_t cap) {
    const char *exe = "C:/Games/TotK/totk.exe";
    assert(strlen(exe) < cap);
    strcpy(out, exe);
    return (int)strlen(exe);
}
int plat_file_exists(const char *path) {
    assert(!strcmp(path, "C:/Games/TotK/ReScaleFrame/ReScaleFrame.Runtime.dll"));
    return packaged_runtime;
}
void rt_log(const char *format, ...) { (void)format; }
void *plat_lib_open(const char *path) { (void)path; abort(); }
void *plat_lib_sym(void *module, const char *name) { (void)module; (void)name; abort(); }
void plat_lib_error(char *out, size_t size) { (void)out; (void)size; abort(); }

static int settings_reads, setting_enabled=1, setting_backend=3;
int main(void) {
    assert(!rsf_runtime_path());
    assert(!rsf_runtime_present());
    packaged_runtime = 1;
    assert(rsf_runtime_present());
    assert(!strcmp(rsf_runtime_path(), "C:/Games/TotK/ReScaleFrame/ReScaleFrame.Runtime.dll"));
    assert(!strcmp(rsf_provider_directory(1), "C:/Games/TotK/ReScaleFrame/streamline"));
    assert(!strcmp(rsf_provider_directory(2), "C:/Games/TotK/ReScaleFrame/fidelityfx"));
    assert(!strcmp(rsf_provider_directory(3), rsf_provider_directory(4)));
    assert(!strcmp(rsf_provider_directory(5), "C:/Games/TotK/ReScaleFrame/xess"));
    assert(!rsf_provider_directory(0));
    runtime_override = "D:/Alternate/RSF.dll";
    provider_override = "D:/Alternate/SDK";
    assert(!strcmp(rsf_runtime_path(), runtime_override));
    for (unsigned i = 1; i <= 5; i++) assert(!strcmp(rsf_provider_directory(i), provider_override));
    runtime_override = provider_override = "";
    assert(!strcmp(rsf_provider_directory(5), "C:/Games/TotK/ReScaleFrame/xess"));
    assert(!strcmp(rsf_runtime_path(), "C:/Games/TotK/ReScaleFrame/ReScaleFrame.Runtime.dll"));
    assert(rsf_requested() && rsf_temporal_requested());
    assert(rsf_config_enabled() && settings_reads == 4);
    setting_enabled = 0; setting_backend=5;
    assert(rsf_config_enabled() && rsf_config_sr_backend()==3 && settings_reads == 4); // Restart-only selections stay latched.
    rsf_runtime_disable("test refusal");
    assert(!rsf_requested() && !rsf_temporal_requested() && !rsf_config_enabled());
    return 0;
}

int cfg_bool(const char *name) { (void)name; ++settings_reads; return setting_enabled; }
int cfg_int(const char *name) { ++settings_reads; return !strcmp(name,"TOTK_RSF_BACKEND") ? setting_backend : 0; }
