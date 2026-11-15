// RSF is loaded through its exported C interface. No provider implementation or
// GPL header is incorporated here. Keep the module alive for all borrowed exports
// and for Vulkan dispatch callbacks that may outlive provider shutdown.
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "rsf_runtime.h"
#include <string.h>

static PlatMutex paths_lock = PLAT_MUTEX_INIT;
static int paths_ready;
static char runtime_path[PLAT_PATH_MAX], provider_paths[3][PLAT_PATH_MAX];

static void package_paths(void) {
    plat_mutex_lock(&paths_lock);
    if (!paths_ready) {
        char exe[PLAT_PATH_MAX];
        if (plat_exe_path(exe, sizeof exe)) {
            char *last = plat_last_sep(exe);
            if (last) {
                *last = 0;
                int length = snprintf(runtime_path, sizeof runtime_path, "%s/ReScaleFrame/ReScaleFrame.Runtime.dll", exe);
                if (length < 0 || (size_t)length >= sizeof runtime_path) runtime_path[0] = 0;
                const char *names[] = {"streamline", "fidelityfx", "xess"};
                for (unsigned i = 0; i < 3; i++) {
                    length = snprintf(provider_paths[i], sizeof provider_paths[i], "%s/ReScaleFrame/%s", exe, names[i]);
                    if (length < 0 || (size_t)length >= sizeof provider_paths[i]) provider_paths[i][0] = 0;
                }
            }
        }
        paths_ready = 1;
    }
    plat_mutex_unlock(&paths_lock);
}

const char *rsf_runtime_path(void) {
    const char *override = cfg_get("TOTK_RSF_RUNTIME");
    if (override && *override) return override;
    package_paths();
    return runtime_path[0] && plat_file_exists(runtime_path) ? runtime_path : NULL;
}

const char *rsf_provider_directory(unsigned backend) {
    const char *override = cfg_get("TOTK_RSF_PROVIDER_DIR");
    if (override && *override) return override;
    package_paths();
    if (backend != 1 && backend != 2 && backend != 3 && backend != 4 && backend != 5) return NULL;
    const char *path = provider_paths[backend == 1 ? 0 : backend == 5 ? 2 : 1];
    return *path ? path : NULL;
}

static void *module;
static int tried, ready;

int rsf_runtime_load(void) {
    if (tried) return ready;
    tried = 1;
    const char *path = rsf_runtime_path();
    if (!path || !path[0]) return 0;
    module = plat_lib_open(path);
    if (!module) {
        char why[256];
        plat_lib_error(why, sizeof why);
        rt_log("rsf: runtime load failed: %s", why);
        return 0;
    }
    // Validate every service needed by the replacement before allowing callers
    // to borrow an export. Export presence does not establish ABI compatibility
    // or provider availability; setup calls must check those independently.
    static const char *const required[] = {
        "rsf_get_runtime_version", "rsf_slvk_initialize",
        "rsf_latencyvk_instance_proc", "rsf_latencyvk_options",
        "rsf_latencyvk_cpu", "rsf_latencyvk_execution",
        "rsf_latencyvk_shutdown", "rsf_srvk_create", "rsf_srvk_plan",
        "rsf_srvk_evaluate", "rsf_srvk_retire_commands", "rsf_srvk_destroy",
        "rsf_fgvk_create_with_host", "rsf_fgvk_images", "rsf_fgvk_acquire",
        "rsf_fgvk_enable_options", "rsf_fgvk_prepare", "rsf_fgvk_hudless",
        "rsf_fgvk_submitted", "rsf_fgvk_present", "rsf_fgvk_destroy"
    };
    int missing = 0;
    for (unsigned i = 0; i < sizeof required / sizeof required[0]; i++) {
        if (!plat_lib_sym(module, required[i])) {
            rt_log("rsf: runtime lacks required export %s", required[i]);
            missing = 1;
        }
    }
    if (missing) return 0;
    ready = 1;
    rt_log("rsf: native Vulkan service exports loaded; providers await setup");
    return 1;
}

void *rsf_runtime_symbol(const char *name) {
    return ready && name ? plat_lib_sym(module, name) : NULL;
}
