"""Actual presentation wrappers with a single immediately reusable fake backing slot."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1]
out=root/'build/checks/fg-history-worker';out.mkdir(parents=True,exist_ok=True)
text=(root/'runtime/gfx_present.c').read_text()
def between(a,b): return text[text.index(a):text.index(b,text.index(a))]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "rsf_present.h"
#include "rsf_fg_history.h"
typedef struct { VkQueue gfx_queue; } VkCtx;
static VkCtx context;
static VkCtx *vk_ctx(void) {return &context;}
static int g_pl,locked,g_dirty,rt_trace;
#define RT_TRACE_PRESENT 1
#define rt_log(...) ((void)0)
static RsfPresent *g_rsf_present=(void *)1;
static VkSwapchainKHR g_sc=(VkSwapchainKHR)2;
static VkExtent2D g_sc_ext={1920,1080};
static VkSemaphore g_render_done[1];
static uint32_t g_flip_idx;
static uint64_t g_flip_source,g_flip_pid;
static int g_flip_on,g_flip_fg_valid;
static RsfFgSnapshot g_flip_fg;
static RsfFgHistory fg_history;
static RsfFgMap fg_mapping,fg_previous_mapping;
static int fg_mapping_known,fg_previous_known;
static unsigned fg_previous_render_w,fg_previous_render_h;
static void plat_rmutex_lock(int *p) {(void)p;assert(!locked++);}
static void plat_rmutex_unlock(int *p) {(void)p;assert(!--locked);}
static void vk_queue_lock(void) {}
static void vk_queue_unlock(void) {}
static int plat_xchg32(int *p,int v) {int old=*p;*p=v;return old;}
static void rsf_vk_present_event(uint64_t source,int begin) {(void)source;(void)begin;}
static void latency_note(uint64_t p) {(void)p;}
static void vk_device_fault_report(VkResult r) {assert(!r);}
static rsf_frame_record presented;
static int anonymous,flips,prepare_refused,hudless_calls;
int32_t rsf_present_flip(RsfPresent *p,const void *pi,const rsf_frame_record *r) {
    (void)p;(void)pi;flips++;anonymous=!r;
    if(r) {assert(r->resource_generation);presented=*r;}
    return 0;
}
int32_t rsf_present_status(RsfPresent *p,rsf_fgvk_status *s) {(void)p;(void)s;return -1;}
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue q,const VkPresentInfoKHR *p) {(void)q;(void)p;assert(0);return 0;}
typedef struct RsfFgLease {rsf_frame_record record;RsfFgMap map;} RsfFgLease;
static RsfFgLease backing;
static uint64_t vk_sync_record_fence(VkCommandBuffer cb) {assert(cb);return 100;}
static RsfFgLease *rsf_fg_gpu_prepare(VkCommandBuffer cb,uint64_t fence,const RsfFgMap *m,const RsfFgMap *old,
    const rsf_fgvk_frame *in,rsf_fgvk_frame *out) {
    assert(cb && fence);(void)old;backing=(RsfFgLease){*in->record,*m};
    *out=*in;out->record=&backing.record;return &backing;
}
static RsfFgLease *rsf_fg_gpu_find(uint64_t source) {return source==backing.record.frame_id?&backing:NULL;}
static int rsf_fg_gpu_hudless(VkCommandBuffer cb,RsfFgLease *s,const rsf_fgvk_frame *in,rsf_fgvk_frame *out) {
    assert(cb && s==&backing);*out=*in;out->record=&s->record;return 1;
}
int32_t rsf_present_prepare(RsfPresent *p,void *cb,const rsf_fgvk_frame *f) {assert(p && cb && f->record);return prepare_refused;}
int32_t rsf_present_hudless(RsfPresent *p,void *cb,const rsf_fgvk_frame *f) {assert(p && cb && f->record);hudless_calls++;return 0;}
'''
code+=between('static void flip_locked(void) {','static void free_capture(')
code+=between('static void fg_forget(void) {','int vk_present_fg_output(')
code+=between('int vk_present_fg_prepare(','void vk_present_fg_submitted(')
code+=r'''
static rsf_frame_record record(uint64_t frame,uint32_t generation) {
    return (rsf_frame_record){.struct_size=sizeof(rsf_frame_record),.abi_version=RSF_GAME_FRAME_ABI_VERSION,
        .frame_id=frame,.session_id=3,.view_id=2,.resource_generation=generation,.render_width=1600,.render_height=900};
}
static void queue(uint64_t source) {g_flip_source=source;fg_pending_snapshot(source);g_flip_on=1;}
int main(void) {
    fg_mapping=(RsfFgMap){.game_w=1600,.game_h=900,.out_w=1920,.out_h=1080,.crop={0,0,1600,900},.dst={0,0,1920,1080}};
    fg_mapping_known=1;
    rsf_frame_record n=record(100,7),next=record(101,8);
    rsf_fgvk_frame frame={.record=&n};
    assert(!vk_present_fg_prepare((void *)1,&frame));
    assert(!vk_present_fg_hudless((void *)1,&frame) && hudless_calls==1);
    n.resource_generation=9;
    assert(vk_present_fg_hudless((void *)1,&frame)!=0 && hudless_calls==1);
    n.resource_generation=7;
    frame.record=&next;assert(!vk_present_fg_prepare((void *)1,&frame));
    assert(!rsf_fg_gpu_find(100)); // backing reused before Present N
    queue(100);assert(g_flip_fg_valid);
    rsf_frame_record later=record(102,9);frame.record=&later;
    assert(!vk_present_fg_prepare((void *)1,&frame)); // prep after deferred snapshot
    flip_locked();assert(!anonymous && presented.frame_id==100 && presented.session_id==3 && presented.view_id==2 && presented.resource_generation==7);
    assert(!g_flip_fg_valid && !g_flip_fg.record.frame_id);
    queue(101);flip_locked();assert(!anonymous && presented.frame_id==101 && presented.resource_generation==8);
    queue(999);flip_locked();assert(anonymous);
    fg_mapping.dst[0]=1;fg_mapping.dst[2]--;
    queue(102);flip_locked();assert(anonymous); // captured crop/fit differs from actual Present
    fg_mapping.dst[0]=0;fg_mapping.dst[2]++;
    prepare_refused=-1;later=record(103,10);frame.record=&later;
    assert(vk_present_fg_prepare((void *)1,&frame)==-1);
    queue(103);flip_locked();assert(anonymous); // no metadata for refused prepare
    prepare_refused=0;
    for(unsigned i=0;i<RSF_FG_HISTORY_COUNT;i++) {
        later=record(200+i,11+i);frame.record=&later;assert(!vk_present_fg_prepare((void *)1,&frame));
    }
    queue(100);flip_locked();assert(anonymous); // expiration refuses, never borrows latest
    queue(263);assert(g_flip_fg_valid);fg_forget();flip_locked();assert(anonymous);
    assert(flips==7);return 0;
}
'''
fixture=out/'present-history.c'
fixture.write_text('// '+'TOTK_RECOMP_'+'GENERATED_FROM_GAME_CODE\n'+code)
command=['C:/msys64/ucrt64/bin/clang.exe','-std=gnu11','-O2','-Wall','-Werror',str(fixture),str(root/'runtime/rsf_fg_history.c'),str(root/'runtime/rsf_fg_map.c'),'-o',str(out/'present-history.exe')]
for d in ('runtime','external/ReScaleFrame/runtime/orchestrator/include','external/ReScaleFrame/runtime/contract/include','external/ReScaleFrame/sdk/game/include'):
    command+=['-I',str(root/d)]
subprocess.run(command,check=True)
subprocess.run([str(out/'present-history.exe')],check=True)
print('Actual prepare, HUD-less admission, snapshot and deferred flip: passed (CPU only)')
