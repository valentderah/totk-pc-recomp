#pragma once
#include <string.h>
// The CPU observes exactly the bytes uploaded to Vulkan, never mutable guest memory.
static inline void us_copy_snapshot(void *gpu,void *shadow,const void *source,size_t bytes){
    if(shadow){memcpy(shadow,source,bytes);memcpy(gpu,shadow,bytes);}
    else memcpy(gpu,source,bytes);
}
