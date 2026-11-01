#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
// Called before boot starts worker threads. Optional launcher seed becomes a normal saved setting.
int dlaa_settings_init(const char *seed);
unsigned dlaa_settings_requested(void);
// Called after SDK record; successful evaluation, not merely selection, establishes the active profile.
void dlaa_settings_evaluated(int success);
void dlaa_settings_note(char *out, size_t capacity);
#ifdef __cplusplus
}
#endif
