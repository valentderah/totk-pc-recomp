// Guard mirroring adapted from Weeping of the Empire at fe2345e82a43156183242abfdcc0eb1f5526eba6.
// Copyright (c) 2026 daniel-madrid-07; MIT License, see tools/licenses/weeping-player-state.txt.
// Production adapter for independent controls. Game-specific values are generated from the supported executable.
#include "input_actions.h"
#include "input_player_state.h"
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include "cfg.h"
#include "platform/platform.h"
#include "input_actions_backend.h"
#include <math.h>

extern int plat_mem_read(uint64_t,void *,size_t);
extern int64_t frame_step_world_us(void);
extern uint64_t input_context_activation(void);
extern uint32_t input_actions_lockout_offset(void);
extern int input_actions_ride_verb(void);
extern int input_actions_special_context(const int32_t verbs[5],int ability);
extern uint32_t input_actions_ui_pause_hash(void);
extern uint32_t input_actions_menu_pause_hash(void);
extern int input_context_flag(uint32_t);
extern const uint64_t *input_actions_virtual_masks(unsigned phase);
extern uint64_t input_actions_virtual_owned(unsigned phase);
extern uint64_t input_actions_jump_mask(int kind,unsigned phase);
extern uint64_t input_actions_guard_mask(void);
extern uint64_t input_actions_guard_source(void);
extern uint64_t input_actions_lockout_mask(unsigned phase);
extern uint32_t input_actions_off_hash(void);
static _Atomic uint64_t last_frame;
static uint64_t now_ns(void) { return plat_now_ns(); }

static InputActionContext context(void) {
    const uint64_t activation=input_context_activation();
    InputActionContext c={0,activation,IC_FOOT,0};
    if(input_context_flag(input_actions_off_hash())!=0)return c;
    if(!(activation>>63) || !(activation&3) || frame_step_world_us()<0)return c;
    struct InputPlayerState state;
    const struct InputPlayerLayout *l=input_player_state_config();
    input_player_state_read(l,0,&state);
    if(state.status!=IPS_READY)return c;
    c.generation^=state.actor^(uint64_t)(uint32_t)state.identity<<32;
    if((activation&3)==2) {
        // Read explicit UI/pouch pause rows; a player pause alone is not sufficient to route menu controls.
        int ui=input_context_flag(input_actions_ui_pause_hash());
        int menu=input_context_flag(input_actions_menu_pause_hash());
        if(ui!=0 || menu<0)return c;
        c.kind=menu?IC_MENU:IC_DIALOGUE;c.active=1;return c;
    }
    uint64_t last=atomic_load(&last_frame),now=now_ns();
    if(!last || now-last>100000000)return c;
    int special=input_actions_special_context(state.verbs,state.ability);
    if(special>=0) {
        if(state.slots_known!=31 || (state.known&IPS_TALK)==0 || (state.flags&IPS_TALK))return c;
        if(special==IC_ULTRAHAND) {
            if(!(state.known&IPS_HOLDING))return c;
            if(state.flags&IPS_HOLDING)special=IC_ULTRAHAND_HOLD;
        }
        c.kind=special;c.active=1;return c;
    }
    const uint32_t excluded=IPS_ABILITY|IPS_VEHICLE|IPS_SURF|IPS_TALK|IPS_CAMERA;
    const uint32_t classified=excluded|IPS_HORSE|IPS_CLIMB|IPS_SWIM|IPS_AIR|IPS_GLIDE|IPS_AIM;
    uint32_t required=classified;
    // A positively identified airborne transition need not claim that an old mount association is known-clear.
    if((state.flags&state.known&IPS_AIR)!=0)required&=~IPS_HORSE;
    if(state.slots_known!=31 || (state.known&required)!=required || (state.flags&excluded))return c;
    if(state.flags&IPS_HORSE) {
        if(state.verbs[0]!=input_actions_ride_verb())return c;
        c.kind=IC_RIDING;
    } else if(state.flags&IPS_SWIM)c.kind=IC_SWIM;
    else if(state.flags&IPS_CLIMB)c.kind=IC_CLIMB;
    else if(state.flags&IPS_GLIDE)c.kind=IC_GLIDE;
    else if(state.flags&IPS_AIR)c.kind=IC_AIR;
    else if(state.flags&IPS_AIM)c.kind=IC_BOW;
    uint64_t comps=0,player=0;float lockout;
    if(!plat_mem_read(state.actor+l->actor_components,&comps,8) || !comps ||
       !plat_mem_read(comps+l->player_component,&player,8) || !player ||
       !plat_mem_read(player+input_actions_lockout_offset(),&lockout,4) || !isfinite(lockout))return c;
    c.lockout=lockout>0.00000011920928955078125f;
    c.active=1;
    return c;
}
static PlatMutex prompt_lock=PLAT_MUTEX_INIT;
static struct {uint64_t object,activation,ns;int64_t clock;int available;} prompt;
void input_actions_interaction_observe(uint64_t object,int64_t selected,int entry_valid) {
    uint64_t activation=input_context_activation();
    plat_mutex_lock(&prompt_lock);
    prompt.object=object;prompt.activation=activation;prompt.ns=now_ns();prompt.clock=frame_step_world_us();
    prompt.available=selected==-1?0:selected>=0 && entry_valid?1:-1;
    plat_mutex_unlock(&prompt_lock);
}
static int interact_available(void) {
    uint64_t activation=input_context_activation(),comps=0,attention=0;
    struct InputPlayerState state;
    const struct InputPlayerLayout *l=input_player_state_config();
    input_player_state_read(l,0,&state);
    if(state.status==IPS_READY && (state.flags&IPS_HORSE) && state.verbs[0]==input_actions_ride_verb())return 1;
    if(state.status!=IPS_READY || !plat_mem_read(state.actor+l->actor_components,&comps,8) || !comps ||
       !plat_mem_read(comps+l->attention_component,&attention,8) || !attention)return -1;
    uint64_t now=now_ns();int64_t clock=frame_step_world_us();
    plat_mutex_lock(&prompt_lock);
    int fresh=prompt.object==attention && prompt.activation==activation && prompt.ns && now>=prompt.ns &&
        now-prompt.ns<=100000000 && clock>=prompt.clock && clock-prompt.clock<=100000;
    int available=fresh?prompt.available:-1;
    plat_mutex_unlock(&prompt_lock);
    return available;
}
static const InputActionBackend backend={context,interact_available,0,0};
__attribute__((constructor)) static void register_backend(void) { if(input_actions_game_supported())input_actions_register(&backend); }
static _Thread_local InputActionFrame current;
void input_actions_bridge_begin(uint64_t site,uint64_t object) {
    (void)site;(void)object;
    atomic_store(&last_frame,now_ns());
    // Disabled bindings only need the native guard mirror; avoid player/registry reads on this hot path.
    current=cfg_bool("TOTK_GAMEPLAY_ACTIONS")?input_actions_frame(frame_step_world_us()):(InputActionFrame){0};
}
uint64_t input_actions_bridge_apply(unsigned phase,uint64_t original) {
    if(phase>2)return original;
    const uint64_t guard=input_actions_guard_mask();
    // The generated guard readers use a private key. Mirror native target input even when remapping is disabled.
    if(original&input_actions_guard_source())original|=guard;
    if(input_actions_exclusive_target())original&=~guard;
    if(!current.active || input_actions_native_context(current.kind) || current.kind==IC_MENU || current.kind==IC_DIALOGUE)return original;
    uint32_t levels=phase==0?current.pressed:phase==1?current.held:current.released;
    const uint64_t *masks=input_actions_virtual_masks(phase);
    uint64_t result=original&~input_actions_virtual_owned(phase);
    if(current.advanced && input_actions_split_guard_context(current.kind)) {
        result&=~guard;
        // Guard selectors also need the shared stance bit. Supply it virtually, without invoking the native ZL tail.
        if(levels&(1u<<IA_GUARD))result|=guard|(phase<2?input_actions_guard_source():0);
    }
    for(unsigned a=0;a<IA_VIRTUAL_COUNT;a++)if(levels&(1u<<a)) {
        if(current.kind==IC_RIDING && a==IA_SPRINT)continue;
        unsigned mapped=(current.kind==IC_GLIDE && a==IA_JUMP) || (current.kind==IC_RIDING && a==IA_INTERACT)?IA_CANCEL:a;
        result|=mapped==IA_JUMP?input_actions_jump_mask(current.kind,phase):masks[mapped];
    }
    if(current.lockout) {
        uint64_t allowed=input_actions_lockout_mask(phase);
        if(allowed&input_actions_guard_source())allowed|=guard;
        result&=allowed;
    }
    return result;
}
