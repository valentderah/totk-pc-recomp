#ifndef TOTK_NATIVE_PROMPT_ATLAS_H
#define TOTK_NATIVE_PROMPT_ATLAS_H
#include <stdint.h>
void native_prompt_atlas_bind(void *(*buffer_map)(void *), void (*cache_update)(void *), void (*manager_update)(void *));
void native_prompt_cache_update(void *cache);
void native_prompt_manager_update(void *manager);
#endif
