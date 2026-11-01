#pragma once
#include <stdint.h>
#include <string.h>
// 'current' must be a freshly validated immutable snapshot, owned by generation.
// The stored source identity is an integer tag, not a pointer to dereference.
static int pj_snapshot_matches(const void *copy,const void *current,size_t bytes,uintptr_t stored_source,uint64_t stored_generation,uint64_t generation,int fast){
    if(!copy||!current)return 0;
    if(fast&&stored_source==(uintptr_t)current&&stored_generation==generation)return 1;
    return memcmp(copy,current,bytes)==0;
}
