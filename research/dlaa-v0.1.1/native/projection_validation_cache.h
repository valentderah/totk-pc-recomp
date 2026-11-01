#pragma once
#include <stdint.h>
#include <string.h>
// Exact identity of immutable predicate inputs. Pointers are identities only,
// never dereferenced here; snapshot/anchor generations are supplied by the owner.
typedef struct {uint64_t words[8];} PjValidationKey;
typedef struct {PjValidationKey key;int result;unsigned valid;} PjValidationEntry;
typedef struct {PjValidationEntry entries[64];} PjValidationCache;
static unsigned pj_validation_slot(const PjValidationKey *key){
    uint64_t p=key->words[0];return (unsigned)((p>>4)^(p>>13)^(p>>23)^key->words[2]^(key->words[3]<<1)^(key->words[6]<<3)^key->words[7])&63;
}
static int pj_validation_find(const PjValidationCache *cache,const PjValidationKey *key,int *out){
    const PjValidationEntry *e=&cache->entries[pj_validation_slot(key)];
    if(!e->valid||memcmp(e->key.words,key->words,sizeof key->words))return 0;
    *out=e->result;return 1;
}
static void pj_validation_store(PjValidationCache *cache,const PjValidationKey *key,int result){
    PjValidationEntry *e=&cache->entries[pj_validation_slot(key)];e->key=*key;e->result=result;e->valid=1;
}
