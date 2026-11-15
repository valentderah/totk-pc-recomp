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
