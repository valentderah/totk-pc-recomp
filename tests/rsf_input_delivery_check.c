#include <assert.h>
#include <string.h>
#include "rsf_input.h"
#include "rsf_input_gamepad.h"
#include <rescaleframe/native_latency_vulkan.h>
static int enabled,available=1,lookups,count,watches;
static rsf_game_cpu_event events[64];
static SDL_EventFilter callback;
int cfg_bool(const char *name){return enabled&&!strcmp(name,"TOTK_RSF_ENABLED");}
int cfg_int(const char *name){(void)name;return 0;}
uint64_t plat_counter(void){return 123456;}
uint64_t plat_counter_hz(void){return 1000000;}
static void sink(const rsf_game_cpu_event *e){assert(count<64);events[count++]=*e;}
void *rsf_runtime_symbol(const char *name){assert(!strcmp(name,"rsf_latencyvk_cpu"));++lookups;return available?sink:NULL;}
bool SDL_AddEventWatch(SDL_EventFilter filter,void *user){assert(!user);++watches;callback=filter;return true;}
static void check(const rsf_game_cpu_event *e,unsigned kind,unsigned message,unsigned flags){
    assert(e->struct_size==sizeof *e&&e->event_version==RSF_GAME_CPU_EVENT_VERSION);
    assert(e->stage==RSF_GAME_CPU_INPUT_EVENT&&e->session_id==1&&!e->source_frame_id);
    assert(e->timestamp_qpc==123456&&e->qpc_frequency==1000000&&!e->render_expected);
    assert(e->input_kind==kind&&e->message_id==message&&e->flags==flags);
}
int main(void){
    rsf_input_received(RSF_INPUT_KEYBOARD);rsf_input_window_message(0xc123);assert(!count&&!lookups);
    enabled=1;available=0;rsf_input_received(RSF_INPUT_MOUSE);assert(!count&&lookups==1);
    available=1;rsf_input_received(0);rsf_input_received(8);assert(!count);
    rsf_input_win_dequeued(0x0100);assert(count==2);
    check(&events[0],0,0x0100,RSF_GAME_CPU_WINDOW_MESSAGE|RSF_GAME_CPU_INPUT_UNFRAMED);
    check(&events[1],RSF_INPUT_KEYBOARD,0,RSF_GAME_CPU_INPUT_UNFRAMED);
    rsf_input_win_dequeued(0xc123);check(&events[2],0,0xc123,RSF_GAME_CPU_WINDOW_MESSAGE|RSF_GAME_CPU_INPUT_UNFRAMED);
    rsf_input_win_dequeued(0x0200);check(&events[4],RSF_INPUT_MOUSE,0,RSF_GAME_CPU_INPUT_UNFRAMED);
    assert(rsf_input_gamepad_watch_start()&&rsf_input_gamepad_watch_start()&&watches==1&&callback);
    SDL_Event e={.gaxis={.type=SDL_EVENT_GAMEPAD_AXIS_MOTION,.which=7}};
    int before=count;assert(callback(NULL,&e)&&count==before); // no selected device
    rsf_input_gamepad_select(8);assert(callback(NULL,&e)&&count==before);
    rsf_input_gamepad_select(7);assert(callback(NULL,&e)&&count==before+1);
    check(&events[count-1],RSF_INPUT_CONTROLLER,0,RSF_GAME_CPU_INPUT_UNFRAMED);
    rsf_input_gamepad_select(0);assert(callback(NULL,&e)&&count==before+1);
    rsf_input_gamepad_select(7);e.type=SDL_EVENT_GAMEPAD_SENSOR_UPDATE;assert(callback(NULL,&e)&&count==before+1);
    e=(SDL_Event){.gbutton={.type=SDL_EVENT_GAMEPAD_BUTTON_DOWN,.which=7}};assert(callback(NULL,&e)&&count==before+2);
    e=(SDL_Event){.key={.type=SDL_EVENT_KEY_UP,.windowID=3}};before=count;
    rsf_input_sdl_dequeued(&e,4);assert(count==before);rsf_input_sdl_dequeued(&e,3);assert(count==before+1);
    check(&events[count-1],RSF_INPUT_KEYBOARD,0,RSF_GAME_CPU_INPUT_UNFRAMED);
    e=(SDL_Event){.wheel={.type=SDL_EVENT_MOUSE_WHEEL,.windowID=3}};rsf_input_sdl_dequeued(&e,3);
    check(&events[count-1],RSF_INPUT_MOUSE,0,RSF_GAME_CPU_INPUT_UNFRAMED);
    enabled=0;before=count;e=(SDL_Event){.gbutton={.type=SDL_EVENT_GAMEPAD_BUTTON_UP,.which=7}};
    assert(callback(NULL,&e)&&count==before);
    return 0;
}

int rsf_config_enabled(void) { return cfg_bool("TOTK_RSF_ENABLED"); }

int rsf_config_fg_backend(void) { return cfg_int("TOTK_RSF_FG"); }
int rsf_config_sr_backend(void) { return cfg_int("TOTK_RSF_BACKEND"); }
int rsf_config_low_latency(void) { return cfg_int("TOTK_RSF_LOW_LATENCY"); }
int rsf_requested(void) { return rsf_config_enabled() || rsf_config_fg_backend() || rsf_config_low_latency(); }
int rsf_temporal_requested(void) { return rsf_config_enabled() || rsf_config_fg_backend(); }

void *rsf_runtime_cpu_sink(void) { return rsf_runtime_symbol("rsf_latencyvk_cpu"); }
void *rsf_runtime_execution_sink(void) { return rsf_runtime_symbol("rsf_latencyvk_execution"); }
