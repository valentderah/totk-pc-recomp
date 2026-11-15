// Process-lifetime RSF module lease. Called on the graphics initialization thread.
#pragma once

// Load and validate the native Vulkan service exports without initializing an SDK.
// An empty TOTK_RSF_RUNTIME leaves the module unloaded during migration.
int rsf_runtime_load(void);
// Borrow an export only after successful validation; the module remains mapped.
void *rsf_runtime_symbol(const char *name);
