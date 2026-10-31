#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include "../runtime/macos/native_game_input.c"

static jmp_buf failed;
static unsigned wrapped,enabled,bound,updated;
static bool active;
static NativeMouseTurn motion;
_Noreturn void runtime_fail(const char *reason){(void)reason;longjmp(failed,1);}
void *runtime_hle_wrap_named(void *fn,const char *name){assert(name&&fn);wrapped++;return fn;}
void runtime_input_mouse_direct(int on){enabled=on;}
NativeMouseTurn runtime_input_mouse_turn(int on){active=on;return on?motion:(NativeMouseTurn){0};}
void native_prompt_cache_update(void *p){(void)p;}
void native_prompt_manager_update(void *p){(void)p;}
void native_prompt_atlas_bind(void *(*map)(void *),void (*cache)(void *),void (*manager)(void *)){
    assert(map&&cache&&manager);bound++;
}
static void original(void *owner,void *step){assert(owner&&step);updated++;}
static void word(uint8_t *base,size_t off,uint32_t value){memcpy(base+off,&value,4);}
static size_t destination(uint8_t *base,size_t off){
    uint32_t w;memcpy(&w,base+off,4);assert((w&0xfc000000)==0x14000000);
    return off+((int32_t)(w<<6)>>4);
}
int main(void){
    Module m={.name="main",.text_end=0x11a0100,.ro_off=0x11a4000,.file_size=0x11a4000};
    m.base=calloc(1,m.file_size);uint8_t *before=malloc(m.file_size);assert(m.base&&before);
    const size_t sites[]={0xae28b8,0x119adcc,0x9069b8};
    const uint32_t words[]={0x6db63bef,0xd10303ff,0xa9bd7bfd};
    word(m.base,0x30640,0xf0022f28);
    for(unsigned i=0;i<3;i++)word(m.base,sites[i],words[i]);
    size_t stub=m.text_end+128;
    memset(m.base+m.text_end,0x5a,128); // existing aspect veneers
    word(m.base,0x100,0xd53b0027);
    // A mismatch at the last site must not leave the first two patched.
    word(m.base,sites[2],0);memcpy(before,m.base,m.file_size);
    if(!setjmp(failed)){native_game_input_install(&m);assert(!"signature accepted");}
    assert(!memcmp(before,m.base,m.file_size)&&!wrapped&&!enabled&&!bound);
    word(m.base,sites[2],words[2]);m.base[stub+64+23]=1;memcpy(before,m.base,m.file_size);
    if(!setjmp(failed)){native_game_input_install(&m);assert(!"occupied padding accepted");}
    assert(!memcmp(before,m.base,m.file_size)&&!wrapped&&!enabled&&!bound);
    m.base[stub+64+23]=0;
    native_game_input_install(&m);assert(wrapped==3&&enabled&&bound==1);
    for(unsigned i=0;i<3;i++){
        assert(destination(m.base,sites[i])==stub+i*32);
        assert(destination(m.base,stub+i*32+20)==sites[i]+4);
        uint32_t saved;memcpy(&saved,m.base+stub+i*32+16,4);assert(saved==words[i]);
    }
    // Cache-register veneers must coexist with both earlier patch owners.
    size_t cache_stub=m.text_end+NATIVE_MAIN_TEXT_RESERVE;
    assert(native_fold_ctr(m.base,m.text_end,cache_stub,m.ro_off,NATIVE_CTR_EL0)==1);
    assert(destination(m.base,0x100)==cache_stub);
    for(unsigned i=0;i<128;i++)assert(m.base[m.text_end+i]==0x5a);
    for(unsigned i=0;i<3;i++)assert(destination(m.base,sites[i])==stub+i*32);
    uint8_t motor[0x200]={0},parameters[0x500]={0};uint8_t *p=parameters;
    memcpy(motor+0x20,&p,sizeof p);
    float *polar=(float *)(parameters+0x40c);polar[0]=10;polar[1]=10;polar[2]=20;
    *(float *)(motor+0xf8)=-85;*(float *)(motor+0xfc)=89;
    *(uint32_t *)(motor+0x180)=3;camera_update=original;
    motion=(NativeMouseTurn){10*M_PI/180,5*M_PI/180};
    camera_mouse_update(motor,motor);assert(active&&updated==1);
    assert(fabs(polar[1]-5)<1e-5&&fabs(polar[2]-10)<1e-5);
    assert(*(uint32_t *)(motor+0x180)==4&&*(float *)(motor+0x84)==1&&*(float *)(motor+0x10c)==0);
    *(uint32_t *)(motor+0x180)=2;
    camera_mouse_update(motor,motor);assert(!active&&updated==2&&polar[1]==5&&polar[2]==10);
    *(uint32_t *)(motor+0x180)=4;motion.pitch=-10;
    camera_mouse_update(motor,motor);assert(polar[1]==89);
    motion.pitch=10;camera_mouse_update(motor,motor);assert(polar[1]==-85);
    free(before);free(m.base);
    puts("Atomic input hook installation, camera direction, protected modes and pitch limits: PASS");
}
