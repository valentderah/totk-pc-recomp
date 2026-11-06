"""Gameplay ownership, frame edges, menu handoff and binding persistence against real mapping code."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class Actions(unittest.TestCase):
    def test_routing_and_live_bindings(self):
        source = r'''
#include "input_actions.h"
#include "cfg.h"
#include "platform/platform.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static struct {char key[80],value[256];} settings[2048];static int count;static unsigned version;
const char *cfg_get(const char *key){for(int i=0;i<count;i++)if(!strcmp(key,settings[i].key))return settings[i].value;return NULL;}
void cfg_set(const char *key,const char *value){int i;for(i=0;i<count;i++)if(!strcmp(key,settings[i].key))break;
    if(i==count){assert(count<2048);count++;snprintf(settings[i].key,80,"%s",key);}snprintf(settings[i].value,256,"%s",value);version++;}
unsigned cfg_version(void){return version;}
int cfg_bool(const char *key){const char *v=cfg_get(key);return v&&*v&&strcmp(v,"0");}
const char *cfg_value(const char *key){const char *v=cfg_get(key);return v?v:"0";}
double cfg_double(const char *key){return atof(cfg_value(key));}
int cfg_reset(const char *const *keys,int n){for(int i=0;i<n;i++){char k[80];snprintf(k,80,"TOTK_%s",keys[i]);cfg_set(k,"");}return n;}
void rt_log(const char *format,...){(void)format;}
void plat_mutex_lock(PlatMutex *m){(void)m;}void plat_mutex_unlock(PlatMutex *m){(void)m;}
static InputActionContext ctx={1,1,IC_FOOT,0};static int64_t clock_us;
static InputActionContext context(void){return ctx;}
static int available;
static int interact_available(void){return available;}
static const InputActionBackend backend={context,interact_available,NULL,NULL};
static uint8_t keys[KEY_COUNT],pads[PAD_COUNT];
static uint64_t raw;static InputActionFrame frame;
static void sample(int key,int pad,int allowed){memset(keys,0,sizeof keys);memset(pads,0,sizeof pads);
    if(key)keys[key]=1;if(pad)pads[pad]=1;raw=input_actions_sample(keys,pads,PAD_LAYOUT_NINTENDO,allowed);}
static void tick(int key,int pad){sample(key,pad,1);frame=input_actions_frame(++clock_us);}
int main(void){
    // Disabled builds are byte-for-byte native at the device boundary, even with a held confirm key.
    tick(KEY_E,0);assert(keys[KEY_E]&&raw==0&&!frame.active);
    input_actions_register(&backend);tick(KEY_E,0);assert(keys[KEY_E]&&!frame.active);
    cfg_set("TOTK_GAMEPLAY_ACTIONS","1");tick(KEY_SPACE,0);assert(!keys[KEY_SPACE]&&!frame.pressed);
    tick(0,0);tick(KEY_SPACE,0);assert(frame.pressed==(1u<<IA_JUMP)&&frame.held==(1u<<IA_JUMP)&&!raw);
    InputActionFrame repeated=input_actions_frame(clock_us);assert(!memcmp(&frame,&repeated,sizeof frame));
    tick(KEY_SPACE,0);assert(!frame.pressed);tick(0,0);assert(frame.released==(1u<<IA_JUMP));
    // A physical tap between game frames still creates one frame of action.
    sample(KEY_SPACE,0,1);sample(0,0,1);frame=input_actions_frame(++clock_us);assert(frame.pressed==(1u<<IA_JUMP));
    tick(0,0);assert(frame.released==(1u<<IA_JUMP));
    // Rebind Jump onto the legacy Interact key. It cannot produce both actions.
    InputMap m;input_actions_map(&m);assert(input_actions_set_key(&m,IA_JUMP,0,KEY_E)==-1);
    input_map_set_key(&m,IA_INTERACT,0,KEY_F2);input_actions_save(&m);
    tick(0,0);tick(KEY_E,0);assert(!keys[KEY_E]&&!raw&&frame.pressed==(1u<<IA_JUMP));
    tick(0,0);tick(KEY_F2,0);assert(raw==(1ull<<B_A)&&(frame.pressed==(1u<<IA_INTERACT)));
    // The frame includes all logical actions for diagnostics, although the adapter only injects selected virtual ones.
    assert(frame.held&(1u<<IA_INTERACT));
    // Menu entry suppresses its opening held control; a fresh press still uses native Confirm.
    ctx.active=0;ctx.generation++;tick(KEY_F2,0);assert(!keys[KEY_F2]&&!raw&&!frame.active);
    tick(0,0);tick(KEY_E,0);assert(keys[KEY_E]&&input_map_buttons(keys)==(1ull<<B_A)&&!frame.active);
    ctx.active=1;ctx.generation++;tick(KEY_E,0);assert(!raw&&!frame.pressed);
    tick(0,0);tick(KEY_E,0);assert(frame.pressed==(1u<<IA_JUMP));
    // A generation change missed between two polls cannot replay a held menu press.
    ctx.generation++;frame=input_actions_frame(++clock_us);assert(!frame.active);
    tick(KEY_E,0);assert(!frame.pressed);tick(0,0);tick(KEY_E,0);assert(frame.pressed==(1u<<IA_JUMP));
    // Physical pad changes swap within gameplay only, and serialize independently of native menu bindings.
    input_actions_map(&m);assert(input_actions_set_pad(&m,IA_JUMP,PAD_LAYOUT_NINTENDO,PAD_EAST)==-1);
    uint8_t shared[4];
    for(int l=0;l<PAD_LAYOUT_COUNT;l++) {
        assert(input_map_pads_of(&m,IA_JUMP,l,shared,4)==1&&shared[0]==PAD_EAST);
        assert(input_map_pads_of(&m,IA_INTERACT,l,shared,4)==1&&shared[0]==PAD_EAST);
    }
    input_actions_save(&m);tick(0,0);tick(0,PAD_EAST);assert(!pads[PAD_EAST]&&!raw&&(frame.pressed&(1u<<IA_JUMP)));
    input_actions_map(&m);uint8_t ids[8];assert(input_map_pads_of(&m,IA_JUMP,PAD_LAYOUT_NINTENDO,ids,8)==1&&ids[0]==PAD_EAST);
    sample(0,0,0);frame=input_actions_frame(++clock_us);assert(!frame.active);
    cfg_set("TOTK_GAMEPLAY_ACTIONS","0");tick(KEY_E,0);assert(keys[KEY_E]&&input_map_buttons(keys)==(1ull<<B_A));
    // Shared key/pad preserve both rows, prefer a prompt, and latch their choice until release.
    cfg_set("TOTK_GAMEPLAY_ACTIONS","1");ctx.active=1;input_actions_map(&m);
    input_actions_set_key(&m,IA_INTERACT,0,KEY_E);input_actions_save(&m);
    available=1;tick(0,0);tick(KEY_E,0);assert(raw==(1ull<<B_A)&&frame.pressed==(1u<<IA_INTERACT));
    available=0;tick(KEY_E,0);assert(raw==(1ull<<B_A)&&!(frame.held&(1u<<IA_JUMP)));
    tick(0,0);tick(KEY_E,0);assert(!raw&&frame.pressed==(1u<<IA_JUMP));
    available=1;tick(KEY_E,0);assert(!raw&&frame.held==(1u<<IA_JUMP));
    tick(0,0);tick(0,PAD_EAST);assert(raw==(1ull<<B_A)&&frame.pressed==(1u<<IA_INTERACT));
    tick(0,0);available=0;tick(0,PAD_EAST);assert(!raw&&frame.pressed==(1u<<IA_JUMP));
    tick(0,0);available=-1;tick(KEY_E,0);assert(raw==(1ull<<B_A)&&!(frame.held&(1u<<IA_JUMP)));
    ctx.active=0;ctx.generation++;tick(KEY_E,0);assert(!keys[KEY_E]&&!frame.active);
    tick(0,0);tick(KEY_E,0);assert(keys[KEY_E]&&!frame.active);
    // Moving an unrelated gameplay action onto a shared key resolves both conflicts.
    input_actions_map(&m);assert(input_actions_set_key(&m,IA_ATTACK,0,KEY_E)>=0);
    uint8_t check[8];assert(input_map_keys_of(&m,IA_JUMP,check,8)==0);
    assert(input_map_keys_of(&m,IA_INTERACT,check,8)==0);
    // Standard groups traversal; Advanced overrides only distinct actions and inherits once.
    input_actions_reset();ctx.active=1;ctx.kind=IC_FOOT;ctx.generation++;available=0;
    input_actions_map(&m);input_actions_set_key(&m,IA_JUMP,0,KEY_E);input_actions_save(&m);
    input_actions_initialize_advanced();input_actions_map_for(IC_CLIMB,&m);
    input_actions_set_key(&m,IA_JUMP,0,KEY_F3);input_actions_save_for(IC_CLIMB,&m);
    cfg_set("TOTK_ACTION_MODE","advanced");tick(0,0);tick(KEY_E,0);assert(frame.pressed==(1u<<IA_JUMP));
    ctx.kind=IC_CLIMB;tick(0,0);tick(KEY_F3,0);assert(frame.pressed==(1u<<IA_JUMP)&&frame.kind==IC_CLIMB);
    ctx.kind=IC_SWIM;tick(0,0);tick(KEY_E,0);assert(frame.pressed==(1u<<IA_JUMP));
    cfg_set("TOTK_ACTION_MODE","standard");ctx.kind=IC_CLIMB;tick(0,0);tick(KEY_E,0);assert(frame.pressed==(1u<<IA_JUMP));
    cfg_set("TOTK_ACTION_MODE","advanced");tick(0,0);tick(KEY_F3,0);assert(frame.pressed==(1u<<IA_JUMP));
    // Menu and dialogue never inherit the gameplay Jump binding.
    input_actions_map_for(IC_MENU,&m);input_actions_set_key(&m,IA_INTERACT,0,KEY_F4);input_actions_save_for(IC_MENU,&m);
    ctx.kind=IC_MENU;tick(0,0);tick(KEY_F4,0);assert(raw==(1ull<<B_A)&&frame.kind==IC_MENU);
    ctx.kind=IC_DIALOGUE;tick(0,0);tick(KEY_E,0);assert(raw==(1ull<<B_A)&&frame.kind==IC_DIALOGUE);
    input_actions_map_for(IC_DIALOGUE,&m);input_actions_set_key(&m,IA_INTERACT,0,KEY_F7);input_actions_save_for(IC_DIALOGUE,&m);
    tick(0,0);tick(KEY_E,0);assert(!raw);  // gameplay Jump E must not become the unrelated UI X action
    tick(0,0);tick(KEY_F7,0);assert(raw==(1ull<<B_A));
    input_actions_map_for(IC_DIALOGUE,&m);input_actions_set_key(&m,IA_INTERACT,0,KEY_E);input_actions_save_for(IC_DIALOGUE,&m);
    // Common controls cannot acquire per-context remaps, including from old full-context configurations.
    cfg_set("TOTK_ACTION_CLIMB_MOVE_UP","key:f9,pad:none");
    cfg_set("TOTK_ACTION_SWIM_LOOK_LEFT","key:f9,pad:none");
    input_actions_map(&m);input_actions_set_key(&m,IA_MOVE_UP,0,KEY_I);
    input_actions_set_key(&m,IA_LOOK_LEFT,0,KEY_J);input_actions_save(&m);
    for(int c=0;c<IC_COUNT;c++) {
        input_actions_map_for(c,&m);
        assert(input_map_keys_of(&m,IA_MOVE_UP,check,8)==1&&check[0]==KEY_I);
        assert(input_map_keys_of(&m,IA_LOOK_LEFT,check,8)==1&&check[0]==KEY_J);
    }
    ctx.kind=IC_CLIMB;tick(0,0);tick(KEY_I,0);int32_t shared_axes[4];input_actions_sticks(shared_axes);assert(shared_axes[1]==32767);
    ctx.kind=IC_SWIM;tick(KEY_I,0);input_actions_sticks(shared_axes);assert(shared_axes[1]==32767);
    input_actions_map(&m);input_actions_set_key(&m,IA_MOVE_UP,0,KEY_W);
    input_actions_set_key(&m,IA_LOOK_LEFT,0,KEY_LEFT);input_actions_save(&m);
    // Movement continues through gameplay context changes without generating a new button action.
    ctx.kind=IC_FOOT;tick(0,0);tick(KEY_W,0);int32_t axes[4];input_actions_sticks(axes);assert(axes[1]==32767);
    ctx.kind=IC_AIR;tick(KEY_W,0);input_actions_sticks(axes);assert(axes[1]==32767);
    // Held bow/target/sprint inputs survive compatible gameplay transitions without a new press.
    ctx.kind=IC_FOOT;tick(0,0);tick(KEY_F,0);assert(raw==(1ull<<B_ZR));
    ctx.kind=IC_BOW;tick(KEY_F,0);assert(raw==(1ull<<B_ZR)&&!(frame.pressed&(1u<<IA_BOW)));
    ctx.kind=IC_AIR;tick(KEY_F,0);assert(raw==(1ull<<B_ZR)&&!(frame.pressed&(1u<<IA_BOW)));
    ctx.kind=IC_FOOT;tick(0,0);tick(KEY_Z,0);assert(raw==(1ull<<B_ZL));
    ctx.kind=IC_AIR;tick(KEY_Z,0);assert(raw==(1ull<<B_ZL)&&!(frame.pressed&(1u<<IA_GUARD)));
    // Advanced Guard and Lock on may share, or be rebound independently; Standard keeps the combined group.
    input_actions_map_for(IC_FOOT,&m);input_actions_set_key(&m,IA_GUARD,0,KEY_F5);
    input_actions_set_key(&m,IA_GUARD,1,0);input_actions_set_key(&m,IA_LOCKON,0,KEY_F6);
    input_actions_set_key(&m,IA_LOCKON,1,0);input_actions_save_for(IC_FOOT,&m);
    ctx.kind=IC_FOOT;tick(0,0);tick(KEY_F5,0);assert(!raw&&(frame.held&(1u<<IA_GUARD))&&!input_actions_exclusive_target());
    tick(0,0);tick(KEY_F6,0);assert(raw==(1ull<<B_ZL)&&input_actions_exclusive_target());
    input_actions_map_for(IC_FOOT,&m);assert(input_actions_set_key(&m,IA_LOCKON,0,KEY_F5)==-1);
    input_actions_save_for(IC_FOOT,&m);tick(0,0);tick(KEY_F5,0);
    assert(raw==(1ull<<B_ZL)&&(frame.held&(1u<<IA_GUARD))&&(frame.held&(1u<<IA_LOCKON)));
    // Riding maps common Sprint to Spur and Interact to Dismount without changing ground bindings.
    ctx.kind=IC_RIDING;available=1;tick(0,0);tick(KEY_LSHIFT,0);assert(raw==(1ull<<B_A));
    tick(0,0);tick(KEY_E,0);assert(raw==(1ull<<B_B)&&frame.held==(1u<<IA_INTERACT));
    // Ordinary controls stay shared in native-action contexts and keep the game's X/B/Y/Crouch semantics.
    input_actions_map_for(IC_ASCEND,&m);input_actions_set_key(&m,IA_JUMP,0,KEY_SPACE);input_actions_set_key(&m,IA_JUMP,1,0);input_actions_set_key(&m,IA_INTERACT,0,KEY_F3);input_actions_save_for(IC_ASCEND,&m);
    ctx.kind=IC_ASCEND;tick(0,0);tick(KEY_F3,0);assert(raw==(1ull<<B_A));
    tick(0,0);tick(KEY_SPACE,0);assert(raw==(1ull<<B_X));
    tick(0,0);tick(KEY_LSHIFT,0);assert(raw==(1ull<<B_B));
    tick(0,0);tick(KEY_LCTRL,0);assert(raw==(1ull<<B_STICK_L));
    input_actions_map_for(IC_ASCEND,&m);input_actions_set_key(&m,IA_JUMP,0,KEY_F3);input_actions_save_for(IC_ASCEND,&m);
    tick(0,0);tick(KEY_F3,0);assert(raw==(1ull<<B_A));
    ctx.kind=IC_FOOT;available=1;tick(0,0);tick(KEY_F3,0);assert(raw==(1ull<<B_A));
    // Hold-to-open ability and item wheels retain only their opening button until release.
    ctx.kind=IC_FOOT;tick(0,0);tick(KEY_Q,0);assert(raw==(1ull<<B_L));
    ctx.kind=IC_MENU;ctx.generation++;tick(KEY_Q,0);assert(raw==(1ull<<B_L)&&!(frame.held&(1u<<IA_ABILITY)));
    tick(KEY_Q,0);assert(raw==(1ull<<B_L));tick(0,0);assert(!raw);
    ctx.kind=IC_FOOT;ctx.generation++;tick(0,0);tick(KEY_3,0);assert(raw==(1ull<<B_RIGHT));
    ctx.kind=IC_MENU;ctx.generation++;tick(KEY_3,0);assert(raw==(1ull<<B_RIGHT));tick(0,0);assert(!raw);
    // Native fallback contexts can open the same wheels without losing the held opener.
    input_actions_set_native_map(&input_map);ctx.kind=IC_FOOT;ctx.active=0;ctx.generation++;
    tick(0,0);tick(KEY_3,0);ctx.kind=IC_MENU;ctx.active=1;ctx.generation++;tick(KEY_3,0);
    assert(raw==(1ull<<B_RIGHT));tick(0,0);assert(!raw);
    // A button held across a context change still needs release.
    tick(0,0);tick(KEY_F3,0);ctx.kind=IC_CLIMB;tick(KEY_F3,0);assert(!(frame.held&(1u<<IA_JUMP)));
    // Repeated hand-edited tokens cannot overrun either fixed-size mapping table.
    char crowded[256]="", var[80];
    for(int i=0;i<20;i++)strcat(crowded,"pad:default,");
    for(int a=0;a<IA_COUNT;a++){snprintf(var,sizeof var,"TOTK_ACTION_%s",input_actions_name(a));cfg_set(var,crowded);}
    input_actions_map(&m);
    for(int l=0;l<PAD_LAYOUT_COUNT;l++)assert(m.npads[l]<=INPUT_PAD_BINDING_MAX);
    assert(input_actions_set_pad(&m,IA_COUNT,PAD_LAYOUT_NINTENDO,PAD_EAST)==-1);
    assert(input_actions_set_pad(&m,IA_JUMP,PAD_LAYOUT_COUNT,PAD_EAST)==-1);
    puts("gameplay routing checks passed");return 0;
}
'''
        workroot = ROOT / 'build/input-logger/tests'
        workroot.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=workroot) as td:
            p = Path(td)
            (p / 'test.c').write_text(source)
            subprocess.run(['clang', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'runtime'),
                            str(p / 'test.c'), str(ROOT / 'runtime/input_actions.c'), str(ROOT / 'runtime/input_map.c'),
                            '-lm', '-o', str(p / 'test')], check=True)
            subprocess.run([str(p / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
