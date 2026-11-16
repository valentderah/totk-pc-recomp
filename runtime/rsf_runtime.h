// Process-lifetime RSF module lease. Called on the graphics initialization thread.
#pragma once

// Load and validate the native Vulkan service exports without initializing an SDK.
// Uses TOTK_RSF_RUNTIME when supplied, otherwise the package beside the executable.
int rsf_runtime_load(void);
// Borrow an export only after successful validation; the module remains mapped.
void *rsf_runtime_symbol(const char *name);
// Explicit overrides or the packaged ReScaleFrame folder beside the executable.
// The runtime path is NULL when no override and no packaged DLL exist.
const char *rsf_runtime_path(void);
const char *rsf_provider_directory(unsigned backend);

// Effective restart-only selections, initialized after cfg_init and shared by all owners.
int rsf_config_enabled(void);
int rsf_config_fg_backend(void);
int rsf_config_sr_backend(void);
int rsf_config_low_latency(void);
int rsf_requested(void);
int rsf_temporal_requested(void);

#define RSF_GAME_SESSION_ID 1u
#define RSF_FALLBACK_SESSION_ID 2u

// Hot event sinks are published only after successful export validation; never load a module.
void *rsf_runtime_cpu_sink(void);
void *rsf_runtime_execution_sink(void);

// Check the selected path once, retaining fresh filesystem presence and override semantics.
int rsf_runtime_present(void);

// Startup refusal disables optional features for this process without changing saved selections.
void rsf_runtime_disable(const char *reason);
