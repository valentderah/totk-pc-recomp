"""Prompt freshness, ownership and pause-epoch checks against the actual gameplay adapter."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

class Bridge(unittest.TestCase):
    def test_prompt_contract(self):
        source = r'''
#include "input_actions.h"
#include "input_player_state.h"
#include <assert.h>
#include <string.h>
#include <stddef.h>
#include "platform/platform.h"
void plat_mutex_lock(PlatMutex *m){(void)m;}
void plat_mutex_unlock(PlatMutex *m){(void)m;}
static uint64_t host_now=1000000000;
uint64_t plat_now_ns(void){return host_now++;}
int input_actions_game_supported(void){return 1;}
int cfg_bool(const char *key){(void)key;return 1;}
static const InputActionBackend *backend;
static int64_t clock_us=1000000;
static uint64_t activation=(1ull<<63)|1,actor[2],components[3],attention[2],player[2];
static struct InputPlayerLayout layout={.actor_components=0,.attention_component=8,.player_component=16};
static struct InputPlayerState state;
void input_actions_register(const InputActionBackend *b){backend=b;}
static int exclusive_target;
int input_actions_exclusive_target(void){return exclusive_target;}
int input_actions_native_context(int c){return c>=IC_ULTRAHAND&&c<IC_MENU;}
int input_actions_special_context(const int32_t verbs[5],int ability){return verbs[1]==23&&ability==1?IC_ULTRAHAND:-1;}
int input_actions_split_guard_context(int c){return c==IC_FOOT||c==IC_BOW||c==IC_AIR;}
uint64_t input_actions_guard_mask(void){return 0x800;}
uint64_t input_actions_guard_source(void){return 0x40;}
static InputActionFrame next_frame;
InputActionFrame input_actions_frame(int64_t clock){(void)clock;return next_frame;}
int input_actions_test_scripted(void){return 0;}
void input_actions_test_devices(uint8_t *keys,uint8_t *pads){(void)keys;(void)pads;}
int64_t frame_step_world_us(void){return clock_us;}
uint64_t input_context_activation(void){return activation;}
static int ui_paused=1,menu_paused;
uint32_t input_actions_ui_pause_hash(void){return 1;}
uint32_t input_actions_menu_pause_hash(void){return 2;}
static int input_off;
uint32_t input_actions_off_hash(void){return 3;}
int input_context_flag(uint32_t hash){return hash==3?input_off:hash==1?ui_paused:menu_paused;}
uint64_t input_actions_lockout_mask(unsigned phase){return phase<2?0x40:0;}
int input_actions_ride_verb(void){return 19;}
uint32_t input_actions_lockout_offset(void){return 0;}
const uint64_t *input_actions_virtual_masks(unsigned phase){static uint64_t masks[IA_VIRTUAL_COUNT]={[IA_JUMP]=0x100,[IA_CANCEL]=0x200};(void)phase;return masks;}
uint64_t input_actions_jump_mask(int kind,unsigned phase){(void)kind;(void)phase;return 0x100;}
uint64_t input_actions_virtual_owned(unsigned phase){(void)phase;return 0x300;}
int plat_mem_read(uint64_t address,void *out,size_t size){if(address<4096)return 0;memcpy(out,(void *)address,size);return 1;}
const struct InputPlayerLayout *input_player_state_config(void){return &layout;}
void input_player_state_read(const struct InputPlayerLayout *l,uint64_t hint,struct InputPlayerState *out){(void)l;(void)hint;*out=state;}
void input_actions_bridge_begin(uint64_t,uint64_t);
uint64_t input_actions_bridge_apply(unsigned,uint64_t);
void input_actions_interaction_observe(uint64_t,int64_t,int);
int main(void){
    actor[0]=(uint64_t)components;components[1]=(uint64_t)attention;components[2]=(uint64_t)player;
    state.actor=(uint64_t)actor;state.status=IPS_READY;state.slots_known=31;state.known=~0u;
    assert(backend && !backend->context().active);
    input_actions_bridge_begin(0,0);assert(backend->context().active);
    assert(backend->interact_available()==-1);
    input_actions_interaction_observe((uint64_t)attention,0,1);assert(backend->interact_available()==1);
    input_actions_interaction_observe((uint64_t)attention,-1,0);assert(backend->interact_available()==0);
    input_actions_interaction_observe((uint64_t)attention,0,0);assert(backend->interact_available()==-1);
    input_actions_interaction_observe((uint64_t)attention,-2,0);assert(backend->interact_available()==-1);
    input_actions_interaction_observe((uint64_t)actor,0,1);assert(backend->interact_available()==-1);
    input_actions_interaction_observe((uint64_t)attention,0,1);activation+=4;assert(backend->interact_available()==-1);
    input_actions_interaction_observe((uint64_t)attention,0,1);clock_us+=100001;assert(backend->interact_available()==-1);
    input_actions_interaction_observe((uint64_t)attention,0,1);clock_us--;assert(backend->interact_available()==-1);
    clock_us++;activation=(1ull<<63)|1;
    state.flags=IPS_CLIMB;assert(backend->context().kind==IC_CLIMB);
    state.flags=IPS_SWIM;assert(backend->context().kind==IC_SWIM);
    state.flags=IPS_AIR;assert(backend->context().kind==IC_AIR);
    state.known&=~IPS_HORSE;assert(backend->context().active&&backend->context().kind==IC_AIR);
    state.flags=0;assert(!backend->context().active);state.known=~0u;state.flags=IPS_AIR;
    state.flags=IPS_AIR|IPS_GLIDE;assert(backend->context().kind==IC_GLIDE);
    state.flags=IPS_AIM;assert(backend->context().kind==IC_BOW);
    state.flags=IPS_HORSE;assert(!backend->context().active);state.verbs[0]=19;assert(backend->context().active&&backend->context().kind==IC_RIDING);assert(backend->interact_available()==1);state.flags=0;
    state.verbs[1]=23;state.ability=1;state.flags=IPS_ABILITY;assert(backend->context().active&&backend->context().kind==IC_ULTRAHAND);
    state.flags|=IPS_HOLDING;assert(backend->context().kind==IC_ULTRAHAND_HOLD);
    state.known&=~IPS_HOLDING;assert(!backend->context().active);state.known=~0u;
    state.slots_known=1;assert(!backend->context().active);state.slots_known=31;state.verbs[1]=0;state.flags=0;
    activation=(1ull<<63)|2;assert(!backend->context().active);
    ui_paused=0;menu_paused=1;assert(backend->context().active&&backend->context().kind==IC_MENU);
    menu_paused=0;assert(backend->context().active&&backend->context().kind==IC_DIALOGUE);
    menu_paused=-1;assert(!backend->context().active);
    next_frame=(InputActionFrame){.pressed=1u<<IA_JUMP,.held=1u<<IA_JUMP,.active=1,.kind=IC_GLIDE};
    input_actions_bridge_begin(0,0);assert(input_actions_bridge_apply(0,0x400)==0x600);
    next_frame.kind=IC_FOOT;input_actions_bridge_begin(0,0);assert(input_actions_bridge_apply(0,0x400)==0x500);
    next_frame.kind=IC_ULTRAHAND;input_actions_bridge_begin(0,0);assert(input_actions_bridge_apply(0,0x700)==0x700);
    next_frame.kind=IC_MENU;input_actions_bridge_begin(0,0);assert(input_actions_bridge_apply(0,0x700)==0x700);
    next_frame=(InputActionFrame){0};input_actions_bridge_begin(0,0);
    assert(input_actions_bridge_apply(1,0x40)==0x840);  // disabled/native still guards through the private mirror
    exclusive_target=1;assert(input_actions_bridge_apply(1,0x40)==0x40);exclusive_target=0;
    next_frame=(InputActionFrame){.held=1u<<IA_GUARD,.active=1,.kind=IC_FOOT,.advanced=1};
    input_actions_bridge_begin(0,0);assert(input_actions_bridge_apply(1,0)==0x840);
    next_frame.held=1u<<IA_LOCKON;input_actions_bridge_begin(0,0);assert(input_actions_bridge_apply(1,0x40)==0x40);
    activation=(1ull<<63)|1;state.flags=0;*(float *)player=1.0f;
    assert(backend->context().active&&backend->context().lockout);
    next_frame.lockout=1;next_frame.held=1u<<IA_JUMP;input_actions_bridge_begin(0,0);assert(input_actions_bridge_apply(1,0)==0);
    next_frame.held=1u<<IA_GUARD;input_actions_bridge_begin(0,0);assert(input_actions_bridge_apply(1,0)==0x840);
    input_off=1;assert(!backend->context().active);
    return 0;
}
'''
        workroot = ROOT / 'build/tests/independent-controls'
        workroot.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=workroot) as td:
            p = Path(td)
            (p / 'test.c').write_text(source)
            for adapter in ('runtime/input_actions_backend.c',):
                with self.subTest(adapter=adapter):
                    subprocess.run(['clang', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'runtime'),
                                    '-I', str(ROOT / 'tools'), str(p / 'test.c'), str(ROOT / adapter),
                                    '-pthread', '-o', str(p / 'test')], check=True)
                    subprocess.run([str(p / 'test')], check=True)

if __name__ == '__main__':
    unittest.main()
