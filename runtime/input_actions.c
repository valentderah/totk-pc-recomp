// Gameplay bindings and physical-input ownership. Menu buttons keep their existing map in UI contexts.
#include "input_actions.h"
#include "cfg.h"
#include "ui_text.h"
#include "platform/platform.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>

static PlatMutex action_lock = PLAT_MUTEX_INIT;
static const InputActionBackend *backend;
static InputMap maps[IC_COUNT+1], native_map;
static int native_seen;
static int routed_kind;
static unsigned seen = ~0u;
static uint8_t dead_keys[KEY_COUNT], dead_pads[PAD_COUNT];
static uint8_t shared_keys[KEY_COUNT], shared_pads[PAD_COUNT];
static uint64_t carry_keys[KEY_COUNT],carry_pads[PAD_COUNT];
static uint8_t last_keys[KEY_COUNT],last_pads[PAD_COUNT];
static uint32_t levels, taps, previous, handoff_held;
static int routing, initialized, frame_seen;
static uint64_t generation, epoch, frame_epoch;
static int64_t frame_clock;
static InputActionFrame cached;
static const char *const names[IA_COUNT] = {
#define ACTION(name,label,key1,key2,pad) #name,
#include "input_actions.def"
#undef ACTION
};
static const UiTextId labels[IA_COUNT] = {
#define ACTION(name,label,key1,key2,pad) label,
#include "input_actions.def"
#undef ACTION
};
static const int defaults[IA_COUNT][2] = {
#define ACTION(name,label,key1,key2,pad) {key1,key2},
#include "input_actions.def"
#undef ACTION
};
static const int default_buttons[IA_COUNT] = {
#define ACTION(name,label,key1,key2,pad) pad,
#include "input_actions.def"
#undef ACTION
};
static const char *const context_names[IC_COUNT]={"FOOT","CLIMB","SWIM","AIR","GLIDE","BOW","RIDING","ULTRAHAND","ULTRAHAND_HOLD","FUSE","ASCEND","RECALL","AUTOBUILD","CAMERA","AMIIBO","SCOPE","SURF","ROBOT","SLED","MENU","DIALOGUE"};
static const UiTextId context_labels[IC_COUNT]={UI_ACTION_CONTEXT_FOOT,UI_ACTION_CONTEXT_CLIMB,UI_ACTION_CONTEXT_SWIM,UI_ACTION_CONTEXT_AIR,UI_ACTION_CONTEXT_GLIDE,UI_ACTION_CONTEXT_BOW,UI_ACTION_CONTEXT_RIDING,UI_ACTION_CONTEXT_ULTRAHAND,UI_ACTION_CONTEXT_ULTRAHAND_HOLD,UI_ACTION_CONTEXT_FUSE,UI_ACTION_CONTEXT_ASCEND,UI_ACTION_CONTEXT_RECALL,UI_ACTION_CONTEXT_AUTOBUILD,UI_ACTION_CONTEXT_CAMERA,UI_ACTION_CONTEXT_AMIIBO,UI_ACTION_CONTEXT_SCOPE,UI_ACTION_CONTEXT_SURF,UI_ACTION_CONTEXT_ROBOT,UI_ACTION_CONTEXT_SLED,UI_ACTION_CONTEXT_MENU,UI_ACTION_CONTEXT_DIALOGUE};
int input_actions_advanced(void) { const char *s=cfg_get("TOTK_ACTION_MODE");return s && !strcasecmp(s,"advanced"); }
int input_actions_native_context(int c) { return c>=IC_ULTRAHAND && c<IC_MENU; }
int input_actions_split_guard_context(int c) { return c==IC_FOOT || c==IC_BOW || c==IC_AIR; }
const char *input_actions_context_name(int c) { return c>=0 && c<IC_COUNT?context_names[c]:"STANDARD"; }
const char *input_actions_context_label(int c) { return c>=0 && c<IC_COUNT?menu_text(context_labels[c]):menu_text(UI_ACTION_COMMON); }
// Only semantically different operations get independent bindings. Movement, looking and common controls
// always use the shared map, even when old configuration files contain full context maps.
int input_actions_binding_context(int c,int a) {
    if(c==IC_MENU) {
        switch(a) {case IA_JUMP:case IA_INTERACT:case IA_CANCEL:case IA_ATTACK:case IA_ABILITY:case IA_THROW:
            case IA_SELECT_WEAPON:case IA_SELECT_SHIELD:case IA_SELECT_ITEM:case IA_WHISTLE:return c;}
    }
    if(c==IC_DIALOGUE && (a==IA_JUMP || a==IA_INTERACT || a==IA_CANCEL || a==IA_ATTACK))return c;
    if(a==IA_JUMP && (c==IC_CLIMB || c==IC_SWIM || c==IC_AIR || c==IC_GLIDE || c==IC_RIDING))return c;
    if(a==IA_CANCEL && c==IC_CLIMB)return c;
    if(a==IA_THROW && (c==IC_AIR || c==IC_GLIDE))return IC_AIR;
    if(c==IC_RIDING && (a==IA_SPRINT || a==IA_INTERACT || a==IA_ABILITY || a==IA_CANCEL))return c;
    if((c==IC_ULTRAHAND || c==IC_ULTRAHAND_HOLD || c==IC_SCOPE) && a==IA_INTERACT)return c;
    if(c==IC_ULTRAHAND_HOLD && a==IA_THROW)return c;
    return INPUT_ACTIONS_STANDARD;
}
const char *input_actions_context_action(int c,int a) {
    if(c<0 && input_actions_advanced()) {
        if(a==IA_GUARD)return menu_text(UI_ACTION_GUARD);
        if(a==IA_JUMP)return menu_text(UI_ACTION_JUMP);
        if(a==IA_CANCEL)return menu_text(UI_ACTION_CANCEL);
    }
    if(input_actions_native_context(c)) {
        if(a==IA_JUMP)return menu_text(UI_ACTION_NATIVE_X);
        if(a==IA_SPRINT || a==IA_CANCEL)return menu_text(UI_ACTION_NATIVE_B);
        if(a==IA_INTERACT)return c==IC_SCOPE?menu_text(UI_ACTION_PIN):c==IC_ULTRAHAND?menu_text(UI_ACTION_GRAB):c==IC_ULTRAHAND_HOLD?menu_text(UI_ACTION_ATTACH):menu_text(UI_ACTION_NATIVE_A);
        if(a==IA_GUARD)return menu_text(UI_ACTION_NATIVE_ZL);
        if(a==IA_THROW)return c==IC_ULTRAHAND_HOLD?menu_text(UI_ACTION_ROTATE):menu_text(UI_ACTION_NATIVE_R);
        if(a==IA_ATTACK)return menu_text(UI_ACTION_NATIVE_Y);
        if(a==IA_SCOPE)return menu_text(UI_ACTION_LABEL_SCOPE);
    }
    if(a==IA_JUMP) {
        switch(c){case IC_FOOT:return menu_text(UI_ACTION_JUMP);case IC_CLIMB:return menu_text(UI_ACTION_CLIMB_JUMP);case IC_SWIM:return menu_text(UI_ACTION_SWIM_DASH);
                  case IC_AIR:return menu_text(UI_ACTION_GLIDE_OPEN);case IC_GLIDE:return menu_text(UI_ACTION_GLIDE_CLOSE);case IC_RIDING:return menu_text(UI_ACTION_RIDE_JUMP);case IC_MENU:case IC_DIALOGUE:return menu_text(UI_ACTION_ALTERNATE);}
    }
    if(c==IC_RIDING) {
        if(a==IA_SPRINT)return menu_text(UI_ACTION_SPUR);
        if(a==IA_INTERACT)return menu_text(UI_ACTION_DISMOUNT);
        if(a==IA_ABILITY)return menu_text(UI_ACTION_SOOTHE);
        if(a==IA_CANCEL)return menu_text(UI_ACTION_SLOW_DISMOUNT);
        if(a==IA_GUARD)return menu_text(UI_ACTION_RIDE_TARGET);
    }
    if(a==IA_CANCEL && c==IC_CLIMB)return menu_text(UI_ACTION_LET_GO);
    if(a==IA_CANCEL && c==IC_GLIDE)return menu_text(UI_ACTION_GLIDE_CLOSE);
    if(a==IA_THROW && (c==IC_AIR || c==IC_GLIDE))return menu_text(UI_ACTION_DIVE);
    if(c==IC_MENU || c==IC_DIALOGUE) {
        switch(a){case IA_INTERACT:return c==IC_DIALOGUE?menu_text(UI_ACTION_DIALOGUE_CONFIRM):menu_text(UI_ACTION_CONFIRM);
        case IA_CANCEL:return menu_text(UI_ACTION_BACK);case IA_ATTACK:return c==IC_DIALOGUE?menu_text(UI_ACTION_DIALOGUE_LOG):menu_text(UI_ACTION_SORT);
        case IA_ABILITY:return menu_text(UI_ACTION_PREV_TAB);case IA_THROW:return menu_text(UI_ACTION_NEXT_TAB);
        case IA_SELECT_WEAPON:return menu_text(UI_ACTION_NAV_RIGHT);case IA_SELECT_SHIELD:return menu_text(UI_ACTION_NAV_LEFT);
        case IA_SELECT_ITEM:return menu_text(UI_ACTION_NAV_UP);case IA_WHISTLE:return menu_text(UI_ACTION_NAV_DOWN);}
    }
    return input_actions_label(a);
}
void input_actions_config_key(int c,int a,char *out,unsigned capacity) {
    if(c<0)snprintf(out,capacity,"TOTK_ACTION_%s",input_actions_name(a));
    else snprintf(out,capacity,"TOTK_ACTION_%s_%s",input_actions_context_name(c),input_actions_name(a));
}
static int bank_for(int kind) {
    // Menu/dialogue always have their own controls, even in Standard mode.
    return input_actions_advanced() || kind==IC_MENU || kind==IC_DIALOGUE?kind+1:0;
}

const char *input_actions_name(int a) { return a>=0 && a<IA_COUNT ? names[a] : ""; }
const char *input_actions_label(int a) { return a>=0 && a<IA_COUNT ? menu_text(labels[a]) : ""; }
void input_actions_register(const InputActionBackend *b) { __atomic_store_n(&backend,b,__ATOMIC_RELEASE); }
int input_actions_supported(void) { return __atomic_load_n(&backend,__ATOMIC_ACQUIRE)!=NULL; }

static int key_id(const char *name) {
    for(int k=1;k<KEY_COUNT;k++) if(!strcasecmp(name,input_key_name(k))) return k;
    return -1;
}
static int pad_id(const char *name) {
    for(int p=1;p<PAD_COUNT;p++) if(!strcasecmp(name,input_pad_name(p))) return p;
    return -1;
}
static void default_pad(InputMap *m,int a) {
    // Defaults follow the selected face-button layout, independently of the game's Jump/Sprint swap option.
    static const int nintendo[]={PAD_EAST,PAD_SOUTH,PAD_NORTH,PAD_WEST,PAD_LSTICK,PAD_RSTICK,PAD_L,PAD_R,PAD_ZL,PAD_ZR,PAD_START,PAD_BACK,PAD_DPAD_LEFT,PAD_DPAD_UP,PAD_DPAD_RIGHT,PAD_DPAD_DOWN};
    static const int xbox[]={PAD_SOUTH,PAD_EAST,PAD_WEST,PAD_NORTH,PAD_LSTICK,PAD_RSTICK,PAD_L,PAD_R,PAD_ZL,PAD_ZR,PAD_START,PAD_BACK,PAD_DPAD_LEFT,PAD_DPAD_UP,PAD_DPAD_RIGHT,PAD_DPAD_DOWN};
    if(default_buttons[a]<0) return;
    for(int l=0;l<PAD_LAYOUT_COUNT;l++) {
        int p=(l==PAD_LAYOUT_NINTENDO?nintendo:xbox)[default_buttons[a]];
        if(m->npads[l]<INPUT_PAD_BINDING_MAX)
            m->pads[l][m->npads[l]++]=(PadBinding){(uint8_t)p,(uint8_t)a};
    }
}
static void defaults_map(InputMap *m) {
    memset(m,0,sizeof *m);
    for(int a=0;a<IA_COUNT;a++) {
        for(int j=0;j<2;j++)if(defaults[a][j] && m->nkeys<INPUT_BINDING_MAX)m->keys[m->nkeys++]=(Binding){defaults[a][j],(uint8_t)a};
        default_pad(m,a);
    }
}
static void menu_defaults(InputMap *m) {
    InputMap native=native_map;
    if(!native_seen) {
        defaults_map(&native);
        // Before the first device poll the native button defaults match the host action defaults, except Cancel.
        for(int i=0;i<native.nkeys;i++)native.keys[i].action=(uint8_t)(native.keys[i].action>=IA_MOVE_LEFT?
            B_SL_LEFT+native.keys[i].action-IA_MOVE_LEFT:default_buttons[native.keys[i].action]>=0?default_buttons[native.keys[i].action]:B_B);
        for(int l=0;l<PAD_LAYOUT_COUNT;l++)for(int i=0;i<native.npads[l];i++)
            native.pads[l][i].action=(uint8_t)default_buttons[native.pads[l][i].action];
    }
    memset(m,0,sizeof *m);
    for(int a=0;a<IA_COUNT;a++) {
        int button=default_buttons[a];if(a==IA_CANCEL)button=B_B;
        if(a==IA_SPRINT || a==IA_LOCKON)continue;
        if(a>=IA_MOVE_LEFT && a<=IA_LOOK_DOWN)button=B_SL_LEFT+(a-IA_MOVE_LEFT);
        if(button<0)continue;
        for(int i=0;i<native.nkeys && m->nkeys<INPUT_BINDING_MAX;i++)if(native.keys[i].action==button)
            m->keys[m->nkeys++]=(Binding){native.keys[i].key,(uint8_t)a};
        for(int l=0;l<PAD_LAYOUT_COUNT;l++)for(int i=0;i<native.npads[l] && m->npads[l]<INPUT_PAD_BINDING_MAX;i++)
            if(native.pads[l][i].action==button)m->pads[l][m->npads[l]++]=(PadBinding){native.pads[l][i].pad,(uint8_t)a};
    }
}
static void inherit_native_controls(InputMap *m) {
    if(!native_seen)return;
    // New common controls inherit existing button/stick remaps; the original nine action rows keep their settings.
    for(int a=IA_ABILITY;a<IA_COUNT;a++) {
        int n=0;for(int i=0;i<m->nkeys;i++)if(m->keys[i].action!=a)m->keys[n++]=m->keys[i];m->nkeys=n;
        for(int l=0;l<PAD_LAYOUT_COUNT;l++){n=0;for(int i=0;i<m->npads[l];i++)if(m->pads[l][i].action!=a)m->pads[l][n++]=m->pads[l][i];m->npads[l]=n;}
        int button=a>=IA_MOVE_LEFT && a<=IA_LOOK_DOWN?B_SL_LEFT+a-IA_MOVE_LEFT:default_buttons[a];
        for(int i=0;i<native_map.nkeys && m->nkeys<INPUT_BINDING_MAX;i++)if(native_map.keys[i].action==button)
            m->keys[m->nkeys++]=(Binding){native_map.keys[i].key,(uint8_t)a};
        for(int l=0;l<PAD_LAYOUT_COUNT;l++)for(int i=0;i<native_map.npads[l] && m->npads[l]<INPUT_PAD_BINDING_MAX;i++)
            if(native_map.pads[l][i].action==button)m->pads[l][m->npads[l]++]=(PadBinding){native_map.pads[l][i].pad,(uint8_t)a};
    }
}
static void override_action(InputMap *m,int a,const char *value) {
    if(!value || !*value)return;
    int n=0;for(int i=0;i<m->nkeys;i++)if(m->keys[i].action!=a)m->keys[n++]=m->keys[i];m->nkeys=n;
    for(int l=0;l<PAD_LAYOUT_COUNT;l++){n=0;for(int i=0;i<m->npads[l];i++)if(m->pads[l][i].action!=a)m->pads[l][n++]=m->pads[l][i];m->npads[l]=n;}
    char text[256];snprintf(text,sizeof text,"%s",value);
    for(char *save=NULL,*token=strtok_r(text,",",&save);token;token=strtok_r(NULL,",",&save)) {
        while(*token==' ')token++;
        if(!strncasecmp(token,"key:",4)) {
            int key=key_id(token+4);
            if(key>0 && m->nkeys<INPUT_BINDING_MAX)m->keys[m->nkeys++]=(Binding){(uint8_t)key,(uint8_t)a};
        } else if(!strcasecmp(token,"pad:default"))default_pad(m,a);
        else if(!strncasecmp(token,"pad:",4)) {
            int pad=pad_id(token+4);
            if(pad>0)for(int l=0;l<PAD_LAYOUT_COUNT;l++)if(m->npads[l]<INPUT_PAD_BINDING_MAX)
                m->pads[l][m->npads[l]++]=(PadBinding){(uint8_t)pad,(uint8_t)a};
        }
    }
}
void input_actions_set_native_map(const InputMap *m) {
    plat_mutex_lock(&action_lock);
    if(!native_seen || memcmp(&native_map,m,sizeof *m)){native_map=*m;native_seen=1;seen=~0u;}
    plat_mutex_unlock(&action_lock);
}
static void refresh(void) {
    unsigned version=cfg_version();if(seen==version)return;
    defaults_map(&maps[0]);inherit_native_controls(&maps[0]);
    for(int a=0;a<IA_COUNT;a++) {
        char key[80];input_actions_config_key(-1,a,key,sizeof key);const char *value=cfg_get(key);
        // Scope previously appeared as Change camera view. Explicit new bindings take precedence over old saves.
        if(a==IA_SCOPE && (!value || !*value))value=cfg_get("TOTK_ACTION_CAMERA");
        override_action(&maps[0],a,value);
    }
    InputMap menu;menu_defaults(&menu);
    for(int c=0;c<IC_COUNT;c++) {
        maps[c+1]=maps[0];
        for(int a=0;a<IA_COUNT;a++) {
            int owner=input_actions_binding_context(c,a);
            if(owner<0)continue;
            if(c==IC_MENU || c==IC_DIALOGUE) {
                // Keep native menu defaults for distinct UI operations, while movement/look stay shared.
                int n=0;InputMap *m=&maps[c+1];
                for(int i=0;i<m->nkeys;i++)if(m->keys[i].action!=a)m->keys[n++]=m->keys[i];m->nkeys=n;
                for(int i=0;i<menu.nkeys && m->nkeys<INPUT_BINDING_MAX;i++)if(menu.keys[i].action==a)m->keys[m->nkeys++]=menu.keys[i];
                for(int l=0;l<PAD_LAYOUT_COUNT;l++) {
                    n=0;for(int i=0;i<m->npads[l];i++)if(m->pads[l][i].action!=a)m->pads[l][n++]=m->pads[l][i];m->npads[l]=n;
                    for(int i=0;i<menu.npads[l] && m->npads[l]<INPUT_PAD_BINDING_MAX;i++)if(menu.pads[l][i].action==a)m->pads[l][m->npads[l]++]=menu.pads[l][i];
                }
            }
            char key[80];input_actions_config_key(owner,a,key,sizeof key);override_action(&maps[c+1],a,cfg_get(key));
        }
    }
    seen=version;epoch++;initialized=0;levels=taps=0;
}
void input_actions_map_for(int c,InputMap *out) { plat_mutex_lock(&action_lock);refresh();*out=maps[c>=0 && c<IC_COUNT?c+1:0];plat_mutex_unlock(&action_lock); }
void input_actions_map(InputMap *out) { input_actions_map_for(-1,out); }
static void save_action(int c,const InputMap *m,int a) {
    char key[80],line[256]="";uint8_t keys[INPUT_BINDING_MAX],pad[INPUT_PAD_BINDING_MAX];
    int count=input_map_keys_of(m,a,keys,INPUT_BINDING_MAX);
    for(int i=0;i<count;i++){size_t n=strlen(line);snprintf(line+n,sizeof line-n,"key:%s,",input_key_name(keys[i]));}
    int n=input_map_pads_of(m,a,PAD_LAYOUT_NINTENDO,pad,INPUT_PAD_BINDING_MAX);
    uint8_t other[INPUT_PAD_BINDING_MAX];int nn=input_map_pads_of(m,a,PAD_LAYOUT_XBOX,other,INPUT_PAD_BINDING_MAX);
    size_t len=strlen(line);
    snprintf(line+len,sizeof line-len,"pad:%s",n && nn && pad[0]!=other[0]?"default":n?input_pad_name(pad[0]):"none");
    input_actions_config_key(c,a,key,sizeof key);const char *old=cfg_get(key);if(!old || strcmp(old,line))cfg_set(key,line);
}
void input_actions_save_for(int c,const InputMap *m) {
    for(int a=0;a<IA_COUNT;a++)save_action(input_actions_binding_context(c,a),m,a);
}
void input_actions_save(const InputMap *m) { input_actions_save_for(-1,m); }
void input_actions_initialize_advanced(void) {
    if(cfg_bool("TOTK_ACTION_ADVANCED_INITIALIZED"))return;
    InputMap standard;input_actions_map(&standard);
    // The initial separate targeting control follows the user's current combined shield/target binding.
    char key[80];input_actions_config_key(-1,IA_LOCKON,key,sizeof key);
    if(!cfg_get(key)) {
        InputMap target={0};
        for(int i=0;i<standard.nkeys;i++)if(standard.keys[i].action==IA_GUARD)
            target.keys[target.nkeys++]=(Binding){standard.keys[i].key,IA_LOCKON};
        for(int l=0;l<PAD_LAYOUT_COUNT;l++)for(int i=0;i<standard.npads[l];i++)if(standard.pads[l][i].action==IA_GUARD)
            target.pads[l][target.npads[l]++]=(PadBinding){standard.pads[l][i].pad,IA_LOCKON};
        save_action(-1,&target,IA_LOCKON);
    }
    for(int c=0;c<IC_MENU;c++)for(int a=0;a<IA_COUNT;a++)if(input_actions_binding_context(c,a)==c) {
        input_actions_config_key(c,a,key,sizeof key);
        if(!cfg_get(key))save_action(c,&standard,a);
    }
    cfg_set("TOTK_ACTION_ADVANCED_INITIALIZED","1");
}
void input_actions_reset(void) {
    char name[80];const char *key=name+5;
    for(int c=-1;c<IC_COUNT;c++)for(int a=0;a<IA_COUNT;a++){input_actions_config_key(c,a,name,sizeof name);cfg_reset(&key,1);}
    const char *initialized_key="ACTION_ADVANCED_INITIALIZED";cfg_reset(&initialized_key,1);
    const char *legacy_scope="ACTION_CAMERA";cfg_reset(&legacy_scope,1);
}
static int shared_pair(int a,int b) {
    return (a==IA_JUMP && b==IA_INTERACT) || (a==IA_INTERACT && b==IA_JUMP) ||
           (a==IA_GUARD && b==IA_LOCKON) || (a==IA_LOCKON && b==IA_GUARD);
}
int input_actions_set_key(InputMap *m,int a,int slot,int key) {
    if(a<0 || a>=IA_COUNT || slot<0 || slot>1 || key<0 || key>=KEY_COUNT)return -1;
    uint8_t mine[INPUT_BINDING_MAX];int n=input_map_keys_of(m,a,mine,INPUT_BINDING_MAX),lost=-1,count=0;
    for(int i=0;i<m->nkeys;i++) {
        Binding v=m->keys[i];
        if(v.action==a)continue;
        if(key && v.key==key && !shared_pair(a,v.action)){lost=v.action;continue;}
        m->keys[count++]=v;
    }
    if(key) {
        int w=0;for(int i=0;i<n;i++)if(mine[i]!=key)mine[w++]=mine[i];n=w;
        if(slot<n)mine[slot]=(uint8_t)key;else if(n<INPUT_BINDING_MAX)mine[n++]=(uint8_t)key;
    } else if(slot<n) { memmove(mine+slot,mine+slot+1,(size_t)(n-slot-1));n--; }
    for(int i=0;i<n && count<INPUT_BINDING_MAX;i++)m->keys[count++]=(Binding){mine[i],(uint8_t)a};
    m->nkeys=count;return lost;
}
int input_actions_set_pad(InputMap *m,int a,int layout,int pad) {
    if(a<0 || a>=IA_COUNT || layout<0 || layout>=PAD_LAYOUT_COUNT || pad<0 || pad>=PAD_COUNT)return -1;
    uint8_t mine[INPUT_PAD_BINDING_MAX],displaced[IA_COUNT]={0};
    int n=input_map_pads_of(m,a,layout,mine,INPUT_PAD_BINDING_MAX),other=-1,partner=-1;
    for(int i=0;i<m->npads[layout];i++) {
        PadBinding v=m->pads[layout][i];
        if(pad && v.pad==pad && v.action!=a) {
            if(shared_pair(a,v.action))partner=v.action;
            else displaced[v.action]=1,other=v.action;
        }
    }
    for(int l=0;l<PAD_LAYOUT_COUNT;l++) {
        int count=0;
        for(int i=0;i<m->npads[l];i++)if(m->pads[l][i].action!=a && m->pads[l][i].action!=partner && !displaced[m->pads[l][i].action])
            m->pads[l][count++]=m->pads[l][i];
        if(pad && count<INPUT_PAD_BINDING_MAX)m->pads[l][count++]=(PadBinding){(uint8_t)pad,(uint8_t)a};
        // Choosing a shared physical button freezes the pair in both face-button layouts.
        if(partner>=0 && count<INPUT_PAD_BINDING_MAX)m->pads[l][count++]=(PadBinding){(uint8_t)pad,(uint8_t)partner};
        for(int b=0;b<IA_COUNT;b++)if(displaced[b] && n && mine[0]!=pad && count<INPUT_PAD_BINDING_MAX)
            m->pads[l][count++]=(PadBinding){mine[0],(uint8_t)b};
        m->npads[l]=count;
    }
    return other;
}
// A shared physical press keeps its decision until release, even if a pickup removes its prompt while held.
static uint32_t choose_shared(uint32_t actions,uint8_t *choice,int available) {
    const uint32_t pair=(1u<<IA_JUMP)|(1u<<IA_INTERACT);
    if((actions&pair)==pair) {
        if(!*choice)*choice=available==0?1:2;  // unknown never asserts that no interaction is available
        actions&=~pair;actions|=1u<<(*choice==1?IA_JUMP:IA_INTERACT);
    }
    return actions;
}
static InputActionContext context(void) {
    const InputActionBackend *b=__atomic_load_n(&backend,__ATOMIC_ACQUIRE);
    return b ? b->context() : (InputActionContext){0,0,IC_FOOT,0};
}
uint64_t input_actions_sample(uint8_t keys[KEY_COUNT],uint8_t pads[PAD_COUNT],int layout,int allowed) {
    const InputActionBackend *b=__atomic_load_n(&backend,__ATOMIC_ACQUIRE);
    int enabled=b && cfg_bool("TOTK_GAMEPLAY_ACTIONS");
    InputActionContext c=enabled ? context() : (InputActionContext){0,0,IC_FOOT,0};
    plat_mutex_lock(&action_lock);refresh();
    if(c.kind<0 || c.kind>=IC_COUNT)c.active=0,c.kind=IC_FOOT;
    InputMap map=maps[bank_for(c.kind)];
    uint8_t physical_keys[KEY_COUNT],physical_pads[PAD_COUNT];
    memcpy(physical_keys,keys,sizeof physical_keys);memcpy(physical_pads,pads,sizeof physical_pads);
    if(!enabled) {
        routing=initialized=0;levels=taps=previous=0;epoch++;
        memset(dead_keys,0,sizeof dead_keys);memset(dead_pads,0,sizeof dead_pads);
        memset(carry_keys,0,sizeof carry_keys);memset(carry_pads,0,sizeof carry_pads);
        memset(shared_keys,0,sizeof shared_keys);memset(shared_pads,0,sizeof shared_pads);
        plat_mutex_unlock(&action_lock);return 0;
    }
    int active=allowed && c.active && cfg_bool("TOTK_GAMEPLAY_ACTIONS");
    if(!active || c.kind!=IC_MENU){memset(carry_keys,0,sizeof carry_keys);memset(carry_pads,0,sizeof carry_pads);}
    for(int k=1;k<KEY_COUNT;k++)if(!keys[k])carry_keys[k]=0;
    for(int p=1;p<PAD_COUNT;p++)if(!pads[p])carry_pads[p]=0;
    if(!initialized || active!=routing || generation!=c.generation || routed_kind!=c.kind) {
        // A press in another context is never replayed as an action or as menu Confirm/Back.
        InputMap *old=&maps[bank_for(routed_kind)];
        if(initialized && active && !routing && c.kind==IC_MENU && routed_kind<IC_MENU && native_seen) {
            // A wheel opened in a native fallback context must keep its physical opener too.
            const uint64_t opener_buttons=(1ull<<B_L)|(1ull<<B_RIGHT)|(1ull<<B_LEFT)|(1ull<<B_UP);
            for(int i=0;i<native_map.nkeys;i++){Binding v=native_map.keys[i];
                if(keys[v.key] && last_keys[v.key] && (opener_buttons&(1ull<<v.action)))carry_keys[v.key]|=1ull<<v.action;}
            int l=layout>=0 && layout<PAD_LAYOUT_COUNT?layout:PAD_LAYOUT_XBOX;
            for(int i=0;i<native_map.npads[l];i++){PadBinding v=native_map.pads[l][i];
                if(pads[v.pad] && last_pads[v.pad] && (opener_buttons&(1ull<<v.action)))carry_pads[v.pad]|=1ull<<v.action;}
        }
        if(initialized && active && routing && c.kind==IC_MENU && routed_kind<IC_MENU) {
            // Hold-to-open wheels keep their opening input; it must not become a new menu action.
            const uint32_t openers=(1u<<IA_ABILITY)|(1u<<IA_SELECT_WEAPON)|(1u<<IA_SELECT_SHIELD)|(1u<<IA_SELECT_ITEM);
            for(int i=0;i<old->nkeys;i++){Binding v=old->keys[i];if(keys[v.key] && (levels&openers&(1u<<v.action)))
                carry_keys[v.key]|=1ull<<default_buttons[v.action];}
            int l=layout>=0 && layout<PAD_LAYOUT_COUNT?layout:PAD_LAYOUT_XBOX;
            for(int i=0;i<old->npads[l];i++){PadBinding v=old->pads[l][i];if(pads[v.pad] && (levels&openers&(1u<<v.action)))
                carry_pads[v.pad]|=1ull<<default_buttons[v.action];}
        }
        int carry_movement=initialized && active && routing && generation==c.generation &&
            c.kind<IC_MENU && routed_kind<IC_MENU;
        const uint32_t continuous=(1u<<IA_SPRINT)|(1u<<IA_ATTACK)|(1u<<IA_BOW)|(1u<<IA_GUARD)|
            (((1u<<IA_COUNT)-1)&~((1u<<IA_MOVE_LEFT)-1));
        uint32_t carried=0;
        for(int pass=0;pass<2;pass++) {
            InputMap *m=pass?&map:old;
            for(int i=0;i<m->nkeys;i++) {
                int k=m->keys[i].key;uint32_t before=0,after=0;
                for(int j=0;j<old->nkeys;j++)if(old->keys[j].key==k)before|=1u<<old->keys[j].action;
                for(int j=0;j<map.nkeys;j++)if(map.keys[j].key==k)after|=1u<<map.keys[j].action;
                int keep=carry_movement && before==after && !(before&~continuous);
                if(keep && keys[k])carried|=before&levels;
                if(!carry_keys[k] && !keep)dead_keys[k]|=keys[k];
            }
        }
        int selected_layout=layout>=0 && layout<PAD_LAYOUT_COUNT?layout:PAD_LAYOUT_XBOX;
        for(int p=1;p<PAD_COUNT;p++) {
            uint32_t before=0,after=0;
            for(int j=0;j<old->npads[selected_layout];j++)if(old->pads[selected_layout][j].pad==p)before|=1u<<old->pads[selected_layout][j].action;
            for(int j=0;j<map.npads[selected_layout];j++)if(map.pads[selected_layout][j].pad==p)after|=1u<<map.pads[selected_layout][j].action;
            int keep=carry_movement && before==after && !(before&~continuous);
            if(keep && pads[p])carried|=before&levels;
            if(after && !carry_pads[p] && !keep)dead_pads[p]|=pads[p];
        }
        memset(shared_keys,0,sizeof shared_keys);memset(shared_pads,0,sizeof shared_pads);
        epoch++;taps=previous=0;levels=carried;handoff_held=carried;
    }
    initialized=1;routing=active;generation=c.generation;routed_kind=c.kind;
    uint8_t k[KEY_COUNT],p[PAD_COUNT];memcpy(k,keys,sizeof k);memcpy(p,pads,sizeof p);
    for(int i=1;i<KEY_COUNT;i++){if(!keys[i])dead_keys[i]=0;if(dead_keys[i])keys[i]=k[i]=0;}
    for(int i=1;i<PAD_COUNT;i++){if(!pads[i])dead_pads[i]=0;if(dead_pads[i])pads[i]=p[i]=0;}
    uint32_t now=0;
    for(int i=1;i<KEY_COUNT;i++)if(!k[i])shared_keys[i]=0;
    for(int i=1;i<PAD_COUNT;i++)if(!p[i])shared_pads[i]=0;
    if(active) {
        uint32_t key_actions[KEY_COUNT]={0},pad_actions[PAD_COUNT]={0};
        for(int i=0;i<map.nkeys;i++){Binding v=map.keys[i];
            if(v.action==IA_LOCKON && (!input_actions_advanced() || !input_actions_split_guard_context(c.kind)))continue;
            key_actions[v.key]|=1u<<v.action;keys[v.key]=0;}
        if(layout<0 || layout>=PAD_LAYOUT_COUNT)layout=PAD_LAYOUT_XBOX;
        for(int i=0;i<map.npads[layout];i++){PadBinding v=map.pads[layout][i];
            if(v.action==IA_LOCKON && (!input_actions_advanced() || !input_actions_split_guard_context(c.kind)))continue;
            pad_actions[v.pad]|=1u<<v.action;pads[v.pad]=0;}
        int available=-1,queried=0;
        const uint32_t pair=(input_actions_native_context(c.kind) || c.kind==IC_MENU || c.kind==IC_DIALOGUE)?0:(1u<<IA_JUMP)|(1u<<IA_INTERACT);
        for(int i=1;i<KEY_COUNT;i++)if(k[i] && !carry_keys[i]) {
            if(pair && (key_actions[i]&pair)==pair && !shared_keys[i] && !queried) {
                available=b && b->interact_available?b->interact_available():-1;queried=1;
            }
            now|=choose_shared(key_actions[i],&shared_keys[i],available);
        }
        for(int i=1;i<PAD_COUNT;i++)if(p[i] && !carry_pads[i]) {
            if(pair && (pad_actions[i]&pair)==pair && !shared_pads[i] && !queried) {
                available=b && b->interact_available?b->interact_available():-1;queried=1;
            }
            now|=choose_shared(pad_actions[i],&shared_pads[i],available);
        }
    }
    taps|=now&~levels;levels=now;
    uint64_t raw=0;
    for(int i=1;i<KEY_COUNT;i++)if(carry_keys[i])raw|=carry_keys[i],keys[i]=0;
    for(int i=1;i<PAD_COUNT;i++)if(carry_pads[i])raw|=carry_pads[i],pads[i]=0;
    if(now&(1u<<IA_INTERACT))raw|=1ull<<B_A;
    if(now&(1u<<IA_BOW))raw|=1ull<<B_ZR;
    if(now&(1u<<IA_THROW))raw|=1ull<<B_R;
    if((now&(1u<<IA_GUARD)) && (!input_actions_advanced() || !input_actions_split_guard_context(c.kind)))raw|=1ull<<B_ZL;
    if(now&(1u<<IA_LOCKON))raw|=1ull<<B_ZL;
    for(int a=IA_ABILITY;a<IA_MOVE_LEFT;a++)if(now&(1u<<a))raw|=1ull<<default_buttons[a];

    if(c.kind==IC_RIDING) {
        if(now&(1u<<IA_INTERACT)){raw&=~(1ull<<B_A);raw|=1ull<<B_B;}
        if(now&(1u<<IA_SPRINT))raw|=1ull<<B_A;

    }
    if(input_actions_native_context(c.kind)) {
        if(now&(1u<<IA_SPRINT))raw|=1ull<<B_B;
    }
    if(input_actions_native_context(c.kind) || c.kind==IC_MENU || c.kind==IC_DIALOGUE) {
        if(now&(1u<<IA_JUMP))raw|=1ull<<B_X;
        if(now&(1u<<IA_CROUCH))raw|=1ull<<B_STICK_L;
        if(now&(1u<<IA_ATTACK))raw|=1ull<<B_Y;
        if(now&(1u<<IA_CANCEL))raw|=1ull<<B_B;
    }
    memcpy(last_keys,physical_keys,sizeof last_keys);memcpy(last_pads,physical_pads,sizeof last_pads);
    plat_mutex_unlock(&action_lock);return raw;
}
int input_actions_exclusive_target(void) {
    plat_mutex_lock(&action_lock);
    int exclusive=routing && input_actions_advanced() && input_actions_split_guard_context(routed_kind) &&
        (levels&(1u<<IA_LOCKON)) && !(levels&(1u<<IA_GUARD));
    plat_mutex_unlock(&action_lock);return exclusive;
}
uint64_t input_actions_owned_buttons(void) { return input_actions_routing()?((1ull<<16)-1):0; }
void input_actions_sticks(int32_t axes[4]) {
    plat_mutex_lock(&action_lock);
    uint32_t held=routing?levels:0;
    axes[0]=32767*((int)((held>>IA_MOVE_RIGHT)&1)-(int)((held>>IA_MOVE_LEFT)&1));
    axes[1]=32767*((int)((held>>IA_MOVE_UP)&1)-(int)((held>>IA_MOVE_DOWN)&1));
    axes[2]=32767*((int)((held>>IA_LOOK_RIGHT)&1)-(int)((held>>IA_LOOK_LEFT)&1));
    axes[3]=32767*((int)((held>>IA_LOOK_UP)&1)-(int)((held>>IA_LOOK_DOWN)&1));
    // Match native keyboard sticks: diagonals keep the same full deflection as a single direction.
    for(int i=0;i<4;i+=2)if(axes[i] && axes[i+1])axes[i]=axes[i]>0?23170:-23170,axes[i+1]=axes[i+1]>0?23170:-23170;
    plat_mutex_unlock(&action_lock);
}
int input_actions_routing(void) { plat_mutex_lock(&action_lock);int r=routing;plat_mutex_unlock(&action_lock);return r; }
InputActionFrame input_actions_frame(int64_t clock) {
    InputActionContext c=context();
    plat_mutex_lock(&action_lock);
    int active=routing && c.active && generation==c.generation && routed_kind==c.kind && cfg_bool("TOTK_GAMEPLAY_ACTIONS");
    if(!frame_seen || clock!=frame_clock || frame_epoch!=epoch || !active) {
        uint32_t effective=active ? levels|taps : 0;
        uint32_t before=frame_seen && frame_epoch==epoch && active ? previous : active?handoff_held:0;
        cached=(InputActionFrame){effective,effective&~before,before&~effective,active,routed_kind,input_actions_advanced(),c.lockout};
        previous=effective;taps=0;handoff_held=0;frame_seen=1;frame_clock=clock;frame_epoch=epoch;
    }
    cached.lockout=c.lockout;
    InputActionFrame out=cached;plat_mutex_unlock(&action_lock);return out;
}
