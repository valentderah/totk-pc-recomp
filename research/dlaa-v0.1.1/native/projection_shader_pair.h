#pragma once
#include <stdint.h>
#include <string.h>
typedef struct {const void *shader[2];uint64_t hash[2];unsigned valid;} PjShaderPair;
static void pj_shader_pair_update(PjShaderPair *p,const void *vs,const void *fs,uint64_t (*lookup)(const void*)){
    const void *input[2]={vs,fs};
    for(unsigned i=0;i<2;i++)if(!(p->valid&(1u<<i))||p->shader[i]!=input[i]){
        p->hash[i]=lookup(input[i]);p->shader[i]=input[i];p->valid|=1u<<i;
    }
}
