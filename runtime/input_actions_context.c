// Guarded read-only pause-registry lookup for the generated independent-controls adapter. No logging or guest calls.
#include "input_actions_backend.h"
#include "platform/platform.h"
#include <stddef.h>
#include <stdatomic.h>
extern int plat_mem_read(uint64_t, void *, size_t);
static PlatMutex lock=PLAT_MUTEX_INIT;
static uint64_t activation;
static uint64_t pointer(uint64_t at) {
    uint64_t value=0;
    return at && plat_mem_read(at,&value,8)?value:0;
}
// Bounded open-addressed map: u32 keys and u8 values select a bit in the manager's four flag words.
static int flag(uint32_t hash) {
    uint64_t manager=pointer(input_actions_registry_slot()),table=manager?pointer(manager+0x40):0;
    uint32_t capacity=0;
    if(!table || !plat_mem_read(table+0x14,&capacity,4) || !capacity || capacity>65536)return -1;
    uint64_t keys=pointer(table),values=pointer(table+8);
    if(!keys || !values)return -1;
    uint32_t index=hash%capacity;
    for(unsigned step=0;step<capacity && step<256;step++) {
        uint32_t key=0;
        if(!plat_mem_read(keys+(uint64_t)index*4,&key,4) || !key)return -1;
        if(key==hash) {
            uint8_t bit=0;uint32_t word=0;
            if(!plat_mem_read(values+index,&bit,1) || bit>=128 ||
               !plat_mem_read(manager+0x30+(bit/32)*4,&word,4))return -1;
            return (word>>(bit%32))&1;
        }
        if(++index==capacity)index=0;
    }
    return -1;
}
int input_context_flag(uint32_t hash) {
    if(!input_actions_game_supported())return -1;
    plat_mutex_lock(&lock);int result=flag(hash);plat_mutex_unlock(&lock);return result;
}
uint64_t input_context_activation(void) {
    if(!input_actions_game_supported())return 0;
    plat_mutex_lock(&lock);
    uint64_t status=(uint64_t)(flag(input_actions_pause_hash())+1);
    if((activation&3)!=status)activation=(((activation&~(1ull<<63))>>2)+1)<<2|status;
    activation|=1ull<<63;
    uint64_t result=activation;plat_mutex_unlock(&lock);return result;
}
