// RSF is loaded through its exported C interface. No provider implementation or
// GPL header is incorporated here. Keep the module alive for all borrowed exports
// and for Vulkan dispatch callbacks that may outlive provider shutdown.
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "rsf_runtime.h"
#include <string.h>
#include <rescaleframe/sr_session.h>

static PlatMutex config_lock = PLAT_MUTEX_INIT;
static int config_ready, disabled;
// One publication keeps the render, presentation and input threads on the same selection.
static unsigned config, config_seen;

static unsigned config_read(void) {
    return (unsigned)cfg_bool("TOTK_RSF_ENABLED") | (((unsigned)cfg_int("TOTK_RSF_FG") & 15) << 4) |
           (((unsigned)cfg_int("TOTK_RSF_BACKEND") & 15) << 8);
}

static void config_init(void) {
    if (__atomic_load_n(&config_ready, __ATOMIC_ACQUIRE)) return;
    plat_mutex_lock(&config_lock);
    if (!config_ready) {
        config = config_read() | ((unsigned)cfg_int("TOTK_RSF_LOW_LATENCY") << 12);
        config_seen = cfg_version();
        __atomic_store_n(&config_ready, 1, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&config_lock);
}
static unsigned config_get(void) {
    config_init();
    return __atomic_load_n(&config, __ATOMIC_ACQUIRE);
}
int rsf_config_enabled(void) { return !__atomic_load_n(&disabled, __ATOMIC_ACQUIRE) && (config_get() & 1); }
int rsf_config_fg_backend(void) { return __atomic_load_n(&disabled, __ATOMIC_ACQUIRE) ? 0 : (config_get() >> 4) & 15; }
int rsf_config_sr_backend(void) { return (config_get() >> 8) & 15; }
int rsf_config_low_latency(void) { return __atomic_load_n(&disabled, __ATOMIC_ACQUIRE) ? 0 : (config_get() >> 12) & 15; }
int rsf_requested(void) { return !__atomic_load_n(&disabled, __ATOMIC_ACQUIRE) && !!(config_get() & 0xf0f1); }
int rsf_temporal_requested(void) { return !__atomic_load_n(&disabled, __ATOMIC_ACQUIRE) && !!(config_get() & 0xf1); }

int rsf_config_refresh(void) {
    unsigned old = config_get(), version = cfg_version();
    if (version == config_seen) return 0;
    unsigned next = config_read() | (old & 0xf000); // latency remains a startup selection
    if (version != cfg_version()) return 0; // a menu write crossed the read; retry next frame
    config_seen = version;
    __atomic_store_n(&config, next, __ATOMIC_RELEASE);
    if (next == old) return 0;
    rt_log("rsf: live selection: upscaling %u, backend %u, frame generation %u", next & 1,
           (next >> 8) & 15, (next >> 4) & 15);
    return 1;
}

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

int rsf_runtime_present(void) {
    const char *path = cfg_get("TOTK_RSF_RUNTIME");
    if (!path || !*path) {
        package_paths();
        path = runtime_path;
    }
    return path && *path && plat_file_exists(path);
}

const char *rsf_provider_directory(unsigned backend) {
    const char *override = cfg_get("TOTK_RSF_PROVIDER_DIR");
    if (override && *override) return override;
    package_paths();
    unsigned provider;
    switch (backend) {
    case RSF_SR_DLSS: provider = 0; break;
    case RSF_SR_FSR2: case RSF_SR_FSR3: case RSF_SR_FSR4: provider = 1; break;
    case RSF_SR_XESS: provider = 2; break;
    default: return NULL;
    }
    const char *path = provider_paths[provider];
    return *path ? path : NULL;
}

// Export pointers are immutable after successful load; the module lease is process-wide.
static struct { const char *name; void *address; int required; } exports[] = {
    {"rsf_fgvk_acquire", NULL, 1},
    {"rsf_fgvk_create_with_host", NULL, 1},
    {"rsf_fgvk_destroy", NULL, 1},
    {"rsf_fgvk_drain", NULL, 0},
    {"rsf_fgvk_enable_options", NULL, 1},
    {"rsf_fgvk_get_status", NULL, 0},
    {"rsf_fgvk_hudless", NULL, 1},
    {"rsf_fgvk_image_layout", NULL, 0},
    {"rsf_fgvk_images", NULL, 1},
    {"rsf_fgvk_prepare", NULL, 1},
    {"rsf_fgvk_present_source", NULL, 1},
    {"rsf_fgvk_submitted", NULL, 1},
    {"rsf_get_runtime_version", NULL, 1},
    {"rsf_latencyvk_cpu", NULL, 1},
    {"rsf_latencyvk_device_created", NULL, 0},
    {"rsf_latencyvk_device_destroyed", NULL, 0},
    {"rsf_latencyvk_execution", NULL, 1},
    {"rsf_latencyvk_get_status", NULL, 0},
    {"rsf_latencyvk_instance_proc", NULL, 1},
    {"rsf_latencyvk_options", NULL, 1},
    {"rsf_latencyvk_require_device", NULL, 0},
    {"rsf_latencyvk_shutdown", NULL, 1},
    {"rsf_slvk_initialize", NULL, 1},
    {"rsf_native_cpu_read", NULL, 0},
    {"rsf_native_cpu_counters_get", NULL, 0},
    {"vkGetDeviceProcAddr", NULL, 0},
    {"rsf_srvk_create", NULL, 1},
    {"rsf_srvk_destroy", NULL, 1},
    {"rsf_srvk_evaluate", NULL, 1},
    {"rsf_srvk_fsr4_create", NULL, 0},
    {"rsf_srvk_fsr4_destroy", NULL, 0},
    {"rsf_srvk_fsr4_evaluate", NULL, 0},
    {"rsf_srvk_fsr4_info", NULL, 0},
    {"rsf_srvk_fsr4_plan", NULL, 0},
    {"rsf_srvk_fsr4_retire_commands", NULL, 0},
    {"rsf_srvk_fsr4_submitted", NULL, 0},
    {"rsf_srvk_plan", NULL, 1},
    {"rsf_srvk_retire_commands", NULL, 1},
    {"rsf_srvk_spatial_fallback", NULL, 0},
    {"rsf_xessvk_device_requirements", NULL, 0},
    {"rsf_xessvk_initialize", NULL, 0},
    {"rsf_xessvk_instance_requirements", NULL, 0},
    {"rsf_xessvk_shutdown", NULL, 0},
};

static void *module;
static int tried, ready;
static void *cpu_sink, *execution_sink;

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
    int missing = 0;
    for (unsigned i = 0; i < sizeof exports / sizeof exports[0]; i++) {
        exports[i].address = plat_lib_sym(module, exports[i].name);
        if (exports[i].required && !exports[i].address) {
            rt_log("rsf: runtime lacks required export %s", exports[i].name);
            missing = 1;
        }
    }
    if (missing) return 0;
    ready = 1;
    __atomic_store_n(&cpu_sink, rsf_runtime_symbol("rsf_latencyvk_cpu"), __ATOMIC_RELEASE);
    __atomic_store_n(&execution_sink, rsf_runtime_symbol("rsf_latencyvk_execution"), __ATOMIC_RELEASE);
    rt_log("rsf: native Vulkan service exports loaded; providers await setup");
    return 1;
}

void *rsf_runtime_symbol(const char *name) {
    if (!ready || !name) return NULL;
    for (unsigned i = 0; i < sizeof exports / sizeof exports[0]; i++)
        if (!strcmp(name, exports[i].name)) return exports[i].address;
    return plat_lib_sym(module, name);
}

void *rsf_runtime_cpu_sink(void) { return __atomic_load_n(&cpu_sink, __ATOMIC_ACQUIRE); }
void *rsf_runtime_execution_sink(void) { return __atomic_load_n(&execution_sink, __ATOMIC_ACQUIRE); }

void rsf_runtime_disable(const char *reason) {
    if (!__atomic_exchange_n(&disabled, 1, __ATOMIC_ACQ_REL))
        rt_log("rsf: %s; optional RSF features disabled, using native Vulkan", reason);
}
