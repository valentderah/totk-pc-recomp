#include "runtime.h"
#include "mouse_input.h"
#include "prompt_atlas.h"
#include "native_cache_register.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(TOTK_GAME_V121)
static void (*camera_update)(void *,void *);

static void camera_mouse_update(void *owner,void *step) {
    uint8_t *motor=owner;
    uint32_t *mode=(uint32_t *)(motor+0x180);
    bool normal=*mode==3 || *mode==4;
    NativeMouseTurn turn=runtime_input_mouse_turn(normal);
    if(normal && (turn.yaw || turn.pitch)) {
        uint8_t *parameters=*(uint8_t **)(motor+0x20);
        float *polar=(float *)(parameters+0x40c);
        // The normal camera owns collision correction and the target position.
        // Supply its angular state before that work, without a stick dead zone
        // or speed ceiling. Other camera modes keep their own input path.
        float low=*(float *)(motor+0xf8),high=*(float *)(motor+0xfc);
        if(!isfinite(low) || !isfinite(high) || low>high || low < -90 || high>90) {
            low=-85;high=85;
        }
        double pitch=fmax(low,fmin(high,polar[1]-turn.pitch*(180.0/M_PI)));
        polar[1]=(float)pitch;
        polar[2]=(float)remainder(polar[2]-turn.yaw*(180.0/M_PI),360);
        *mode=4;
        *(float *)(motor+0x108)=(float)pitch;
        *(float *)(motor+0x10c)=0;
        *(float *)(motor+0x84)=1;
        *(float *)(motor+0x88)=1;
    }
    camera_update(owner,step);
}

typedef struct {size_t site;uint32_t expected;void *replacement;const char *name;} InputHook;

void native_game_input_install(Module *m) {
    const InputHook hooks[]={
        {0xae28b8,0x6db63bef,camera_mouse_update,"mouse-camera-turn"},
        {0x119adcc,0xd10303ff,native_prompt_cache_update,"input-prompt-cache"},
        {0x9069b8,0xa9bd7bfd,native_prompt_manager_update,"input-prompt-manager"},
    };
    const size_t count=sizeof hooks/sizeof *hooks;
    // The first 128 bytes are reserved by the existing aspect-ratio adapter.
    size_t first=((m->text_end+15)&~(size_t)15)+128;
    size_t end=(m->text_end+0x3fff)&~(size_t)0x3fff;
    if(end>m->ro_off)end=m->ro_off;
    if(end>m->file_size)end=m->file_size;
    if(first<m->text_end || first>end || count*32>end-first)
        runtime_fail("input adapter has no executable padding");
    uint32_t map_word;
    if(m->text_end<0x30644)runtime_fail("input adapter module is truncated");
    memcpy(&map_word,m->base+0x30640,4);
    if(map_word!=0xf0022f28)runtime_fail("input adapter buffer-map signature");
    uint32_t branches[3],returns[3];
    // Check every prologue and destination before changing any game code.
    for(size_t i=0;i<count;i++) {
        const InputHook *h=&hooks[i];uint32_t word;
        if(h->site>m->text_end-4)runtime_fail("input adapter site is outside text");
        memcpy(&word,m->base+h->site,4);
        size_t stub=first+i*32;
        if(word!=h->expected || !native_ctr_branch(h->site,stub,&branches[i]) ||
           !native_ctr_branch(stub+20,h->site+4,&returns[i]))
            runtime_fail("input adapter signature or branch range");
        for(size_t n=0;n<24;n++)if(m->base[stub+n])
            runtime_fail("input adapter padding is already used");
    }
    uintptr_t hosts[3];void *originals[3];
    for(size_t i=0;i<count;i++)
        hosts[i]=(uintptr_t)runtime_hle_wrap_named(hooks[i].replacement,hooks[i].name);
    for(size_t i=0;i<count;i++) {
        size_t stub=first+i*32;
        const uint32_t relay[]={0x58000050,0xd61f0200}; // ldr x16, +8; br x16
        memcpy(m->base+stub,relay,8);memcpy(m->base+stub+8,&hosts[i],8);
        memcpy(m->base+stub+16,&hooks[i].expected,4);
        memcpy(m->base+stub+20,&returns[i],4);
        memcpy(m->base+hooks[i].site,&branches[i],4);
        originals[i]=m->base+stub+16;
    }
    camera_update=originals[0];
    native_prompt_atlas_bind((void *)(m->base+0x30640),originals[1],originals[2]);
    runtime_input_mouse_direct(1);
    fprintf(stderr,"[totk-input] direct camera and device prompt adapters enabled\n");
}
#else
void native_game_input_install(Module *m) {
    (void)m;
    fprintf(stderr,"[totk-input] this game version uses analog mouse camera input\n");
}
#endif
