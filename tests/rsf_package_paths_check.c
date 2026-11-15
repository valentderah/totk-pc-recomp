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

int main(void) {
    assert(!rsf_runtime_path());
    packaged_runtime = 1;
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
    return 0;
}
