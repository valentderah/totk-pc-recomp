"""Compile actual sync handoff and recorder submit functions with CPU-only dispatch."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / "build/checks/fsr4-adapter-worker"
out.mkdir(parents=True, exist_ok=True)
marker = "// " + "TOTK_RECOMP_" + "GENERATED_FROM_GAME_CODE\n"
compiler = "C:/msys64/ucrt64/bin/clang.exe"
includes = [root / "runtime"] + [root / "external/ReScaleFrame" / d for d in
    ("runtime/orchestrator/include", "runtime/contract/include", "sdk/game/include")]

def run(name, code):
    code = "#define RSF_GAME_SESSION_ID 1u\n#define RSF_FALLBACK_SESSION_ID 2u\n#define RSF_GRAPHICS_QUEUES 4u\n" + "#define rec_commands_reset(cb) (rsf_sr_commands_reset(cb), vk_motion_texture_commands_reset(cb))\n" + '#define rsf_config_enabled() cfg_bool("TOTK_RSF_ENABLED")\n#define rsf_config_fg_backend() cfg_int("TOTK_RSF_FG")\n#define rsf_config_sr_backend() cfg_int("TOTK_RSF_BACKEND")\n#define rsf_config_low_latency() cfg_int("TOTK_RSF_LOW_LATENCY")\n#define rsf_requested() (rsf_config_enabled() || rsf_config_fg_backend() || rsf_config_low_latency())\n#define rsf_temporal_requested() (rsf_config_enabled() || rsf_config_fg_backend())\n' + code
    source = out / (name + ".c")
    source.write_text(marker + code, encoding="utf-8")
    command = [compiler, "-std=gnu11", "-O1", "-Wall", "-Werror", str(source), str(root / "runtime/rsf_split.c"), "-o", str(out / (name + ".exe"))]
    for directory in includes:
        command += ["-I", str(directory)]
    subprocess.run(command, check=True)
    subprocess.run([str(out / (name + ".exe"))], check=True)

sync = (root / "runtime/vk_sync.c").read_text()
claim = sync[sync.index("static int claim_slot("):sync.index("static CmdSlot *take_slot(")]
handoff = sync[sync.index("static _Thread_local struct { int prepared"):sync.index("// The copies the replay")]
run("record-handoff", r'''

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#include <rescaleframe/native_sr_split_vulkan.h>
#include "rsf_split.h"
typedef struct { VkDevice device; VkSemaphore timeline; uint32_t gfx_family; int has_rsf_sr_split; } VkCtx;
typedef struct { VkCommandBuffer cb; uint64_t fence; int sr_recorder, sr_previous; RsfSplitSection sr; } CmdSlot;
static VkCtx context = {.device=(void *)1,.gfx_family=3,.has_rsf_sr_split=1};
static CmdSlot *slots;
static int nslots, cur_slot, calls, retires, resets, ended, began, submitted;
static uint64_t cur_fence = 100, completed = 99;
static int cur_recording = 1;
static VkCommandPool cmd_pool = (VkCommandPool)1;
static VkCtx *vk_ctx(void) { return &context; }
static VkResult rec_submit(const VkSubmitInfo2 *si, uint64_t source, uint64_t final_fence) {
    assert(final_fence==100);
    assert(source==33 && si->commandBufferInfoCount==1);
    if (!submitted) {
        assert(si->pCommandBufferInfos[0].commandBuffer==slots[0].cb);
        assert(!si->waitSemaphoreInfoCount && si->signalSemaphoreInfoCount==1 && si->pSignalSemaphoreInfos[0].value==1);
    } else {
        assert(si->pCommandBufferInfos[0].commandBuffer==slots[1].cb);
        assert(si->waitSemaphoreInfoCount==1 && si->pWaitSemaphoreInfos[0].value==2);
        assert(si->signalSemaphoreInfoCount==1 && si->pSignalSemaphoreInfos[0].value==100);
    }
    submitted++;return VK_SUCCESS;
}
static VkResult rec_submit_recorded(const VkSubmitInfo2 *si, uint64_t source, int recorder_owned, uint64_t final_fence) { assert(!recorder_owned && final_fence==100); return rec_submit(si,source,final_fence); }
static void rec_drain(void) { calls++; }
static void ts_begin(VkCommandBuffer cb) { assert(cb); }
static void ts_end(VkCommandBuffer cb, uint64_t fence) { assert(cb && fence == 100); }
static int rec_end_cb(VkCommandBuffer cb, int crumb, uint64_t n) { assert(cb && !crumb && !n); ended++; return 0; }
static int rec_begin_continuation(VkCommandBuffer cb, uint64_t fence) { assert(cb && fence==100); began++; return 0; }
static void rsf_sr_commands_reset(void *cb) { assert(cb && resets==retires+1); retires++; }
static void vk_motion_texture_commands_reset(VkCommandBuffer cb) { assert(cb && resets==retires); }
#define rt_fatal(...) abort()
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags flags) { assert(cb && !flags); resets++; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer cb) { assert(cb); return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkGetSemaphoreCounterValue(VkDevice d, VkSemaphore s, uint64_t *value) {
    (void)d; (void)s; *value=completed; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *cb) {
    (void)d; assert(ai->commandPool==cmd_pool); *cb=(void *)(uintptr_t)(10+nslots); return VK_SUCCESS;
}
''' + claim + handoff + r'''
int main(void) {
    nslots=1; slots=calloc(1,sizeof *slots);
    slots[0]=(CmdSlot){.cb=(void *)10,.fence=100,.sr_previous=-1};
    rsf_fsr4vk_handoff h;
    assert(vk_sync_record_fence((void *)10)==100 && !vk_sync_record_fence((void *)22));
    assert(!vk_sync_sr_prepare((void *)22,&h) && nslots==1);
    assert(vk_sync_sr_prepare((void *)10,&h) && nslots==2 && retires==1);
    assert(h.abi_version==2 && h.queue_family==3 && h.timeline_enabled);
    void *next=NULL;
    assert(h.split_record(h.user,(void *)22,7,0,1,2,&next)!=0 && !ended);
    assert(!h.split_record(h.user,(void *)10,7,0,1,2,&next));
    assert(next==slots[1].cb && cur_slot==1 && ended==1 && began==1);
    assert(vk_sync_record_fence(next)==100 && !vk_sync_record_fence((void *)10));
    assert(slots[0].fence==100 && slots[1].fence==100);
    assert(slots[0].sr.signal_value==1 && !slots[0].sr.wait_value);
    assert(slots[1].sr.wait_value==2 && slots[1].sr_previous==0);
    VkCommandBufferSubmitInfo command={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,.commandBuffer=slots[1].cb};
    VkSemaphoreSubmitInfo signal={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,.value=100};
    VkSubmitInfo2 si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO_2,.commandBufferInfoCount=1,.pCommandBufferInfos=&command,
        .signalSemaphoreInfoCount=1,.pSignalSemaphoreInfos=&signal};
    assert(!submit_graph(&si,33) && submitted==2 && slots[1].sr_previous==-1);
    assert(h.split_record(h.user,next,7,2,3,4,&next)!=0 && ended==1);
    vk_sync_sr_cancel();
    assert(slots[1].fence==100); // cancellation never retires consumed work
    assert(vk_sync_sr_prepare(slots[1].cb,&h));
    int spare=sr_split.next_slot;
    assert(spare==2 && slots[spare].fence==100);
    vk_sync_sr_cancel();
    assert(!slots[spare].fence); // unused preparation can be recycled
    assert(vk_sync_sr_prepare(slots[1].cb,&h) && sr_split.next_slot==spare);
    vk_sync_sr_cancel(); cur_recording=0; assert(!vk_sync_record_fence(slots[1].cb)); free(slots); return 0;
}
''')

rec = (root / "runtime/vk_rec.c").read_text()
submit = rec[rec.index("// At most two command buffers"):rec.index("void rec_queue_wait(")]
# Forward declare the fixture drain and the actual consumer so packet publication
# can execute that consumer, rather than a model of its submission logic.
run("record-submit", r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
static int sub_pending, rec_async=1, locked, pending, notices, driver_calls, queued, motion_notices, fg_lease, fg_notices;
static volatile VkResult r_sub_res, r_sub_err;
static volatile uint64_t q_subs,r_subs;
static struct { uint64_t submits; } RS_;
static struct { VkQueue gfx_queue; } context;
static void *vk_ctx(void) { return &context; }
#define vk_ctx() (&context)
#define R_Submit 1
#define RT_KNOB_ON(x) (x)
#define rt_fatal(...) abort()
static void *packet;
static void *put(int op,size_t bytes) { assert(op==R_Submit); free(packet); packet=calloc(1,bytes); queued=1; return packet; }
static void pub_now(void) { }
static void pub(void) { }
static void rec_drain(void);
int cfg_int(const char *key) { (void)key; return 1; }
static int rt_knob(const char *key,const char *def) { (void)key;(void)def; return 1; }
static void rsf_vk_submission(uint64_t frame,int begin) { assert(frame==33); (void)begin; }
static void vk_queue_lock(void) { assert(!locked++); }
static void vk_queue_unlock(void) { assert(!--locked); }
static int rsf_sr_commands_pending(void *cb) { assert(cb); return pending; }
static int rsf_sr_commands_submitted(void *cb) { assert(cb && !locked); notices++; return 1; }
static int rsf_fg_gpu_commands_pending(VkCommandBuffer cb) {return cb==(void *)10 && fg_lease;}
static void vk_present_fg_submitted(const VkSubmitInfo2 *si) {(void)si;fg_notices++;}
static VkResult result;
static void vk_motion_texture_submitted(VkCommandBuffer cb, uint64_t final_fence) {
    assert(cb==(void *)10 && final_fence==91 && final_fence!=41 && !locked && !result);
    motion_notices++;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue q,uint32_t n,const VkSubmitInfo2 *si,VkFence f) {
    (void)q;assert(locked==1 && n==1 && !f);
    assert(si->waitSemaphoreInfoCount==2 && si->signalSemaphoreInfoCount==2);
    assert(si->pWaitSemaphoreInfos[1].value==40 && si->pSignalSemaphoreInfos[1].value==41);
    assert(si->pCommandBufferInfos[0].commandBuffer==(void *)10);
    driver_calls++; return result;
}
'''.replace('static void *vk_ctx(void) { return &context; }\n','') + submit + r'''
static void rec_drain(void) { if(queued) { queued=0; submit_run(packet); } }
int main(void) {
    VkCommandBufferSubmitInfo cb={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,.commandBuffer=(void *)10};
    VkSemaphoreSubmitInfo waits[2]={{.value=0},{.value=40}}, signals[2]={{.value=0},{.value=41}};
    VkSubmitInfo2 si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO_2,.commandBufferInfoCount=1,.pCommandBufferInfos=&cb,
        .waitSemaphoreInfoCount=2,.pWaitSemaphoreInfos=waits,.signalSemaphoreInfoCount=2,.pSignalSemaphoreInfos=signals};
    // A tracked recording forces actual submission before the producer notifies.
    pending=1; sub_pending=1;
    assert(rec_submit(&si,33,91)==VK_SUCCESS && driver_calls==1 && notices==1 && motion_notices==1);
    result=VK_ERROR_DEVICE_LOST; sub_pending=1;
    assert(rec_submit_recorded(&si,33,1,91)==VK_ERROR_DEVICE_LOST && driver_calls==2 && notices==1 && motion_notices==1);
    result=VK_SUCCESS; r_sub_err=VK_SUCCESS; pending=0; sub_pending=1;
    assert(rec_submit(&si,33,91)==VK_SUCCESS && driver_calls==2 && queued && motion_notices==1);
    waits[1].value=999;signals[1].value=999;cb.commandBuffer=(void *)99;
    rec_drain(); // actual consumer owns deep packet copies of all dependencies
    assert(driver_calls==3 && motion_notices==2);
    cb.commandBuffer=(void *)10;waits[1].value=40;signals[1].value=41;
    assert(!rec_submit(&si,33,91) && driver_calls==4 && motion_notices==3 && notices==2);
    fg_lease=1; sub_pending=1;
    assert(!rec_submit(&si,33,91) && driver_calls==5 && !queued && fg_notices==1);
    fg_lease=0;
    free(packet); return 0;
}
''')
print("FSR4 actual recording and submit functions: passed (CPU only)")

exec_source = (root / "runtime/vk_exec.c").read_text()
chunked = exec_source[exec_source.index("static int chunked_on("):exec_source.index("int vk_exec_record(")]
run("serial-admission", r'''
#include <rescaleframe/sr_session.h>
#include <assert.h>
#include <string.h>
static struct { int workers,chunks,min_draws,check,skew_us; } SC={.workers=2,.chunks=4};
static int vk_dump_armed, backend=4, enabled=1, fg;
#define rt_log(...) ((void)0)
static int cfg_int(const char *key) {
    if (!strcmp(key,"TOTK_RSF_BACKEND")) return backend;
    if (!strcmp(key,"TOTK_RSF_FG")) return fg;
    return 0;
}
static int cfg_bool(const char *key) { (void)key; return 0; }
static int vk_dlaa_wanted(void) { return enabled; }
''' + chunked + r'''
int main(void) {
    assert(!chunked_on());
    enabled=0; assert(chunked_on()); // native rendering retains chunking
    enabled=1;backend=1; assert(chunked_on()); // other SR providers unchanged
    fg=3; assert(!chunked_on()); // preserve existing FG serial admission
    fg=0;vk_dump_armed=1; assert(!chunked_on());
    return 0;
}
''')

scene_source = (root / "runtime/vk_dlaa.c").read_text()
scene = scene_source[scene_source.index("static int rsf_scene("):scene_source.index("// Previous-frame producer facts", scene_source.index("static int rsf_scene("))]
run("scene-continuation", r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "rsf_sr.h"
typedef struct { VkDevice device; VkPhysicalDevice phys; VkInstance instance; int has_rsf_sr_split; } VkCtx;
typedef struct { VkCommandBuffer cb; } VkExec;
struct Size { unsigned w,h,ow,oh,rsf_w,rsf_h; RsfSrSession *rsf; VkImage sr_color,device_depth,mv,mask,out; void *sr_color_mem; VkImageView sr_color_view; };
static struct { struct { int inverted,infinite; } projection; struct { unsigned level,base_layer; VkImage image; } color; } D;
static struct { int on; uint64_t cpu_eval; } T;
static VkCtx context={.has_rsf_sr_split=1};
static VkCommandBuffer initial=(void *)10, resumed=(void *)11;
static int prepare=1, opened, cancelled, fallback_calls, changed, transitions;
static int status=RSF_SR_FAILED, replace_commands=1, selected_backend=4;
static VkImage expected_output=(VkImage)55;
static VkImageLayout output_layout=VK_IMAGE_LAYOUT_GENERAL;
static const VkCtx *vk_ctx(void) { return &context; }
static int cfg_int(const char *key) { (void)key; return selected_backend; }
static const char *cfg_value(const char *key) { (void)key; return "quality"; }
static const char *rsf_provider_directory(unsigned backend) { assert(backend==4); return "ignored"; }
#define rt_log(...) ((void)0)
#define rt_fatal(...) abort()
static uint64_t kern_now_ns(void) { return 0; }
static void rec_drain(void) { }
static rsf_vk_function rsf_vk_generic_instance_proc(void *o,const char *n) { (void)o;(void)n; return NULL; }
static rsf_vk_function rsf_vk_generic_device_proc(void *o,const char *n) { (void)o;(void)n; return NULL; }
static int vk_sync_sr_prepare(VkCommandBuffer cb,rsf_fsr4vk_handoff *h) { assert(cb==initial); (void)h; return prepare; }
static void vk_sync_sr_cancel(void) { cancelled++; }
int rsf_sr_open_split(const rsf_srvk_setup *s,const rsf_fsr4vk_handoff *h,RsfSrSession **out,uint32_t *w,uint32_t *height) {
    (void)s;(void)h;*out=(void *)1;*w=640;*height=360;opened++;return 1;
}
int rsf_sr_open(const rsf_srvk_setup *s,RsfSrSession **o,uint32_t *w,uint32_t *h) { (void)s;(void)o;(void)w;(void)h;abort(); }
static void res_sizes(unsigned *o,unsigned *scene) {o[0]=1920;o[1]=1080;scene[0]=1128;scene[1]=632;}
static int res_rsf_scene(unsigned w,unsigned h) { (void)w;(void)h;return 1; }
#define rsf_provider_log NULL
#define vk_image2d(...) 1
static rsf_srvk_image vk_rsf_image(VkImage image,VkFormat format,unsigned w,unsigned h,VkImageLayout layout) {
    return (rsf_srvk_image){0,(uint64_t)image,format,w,h,layout,1};
}
static void vk_image_layout(VkCommandBuffer cb,VkImage image,VkImageLayout before,VkImageLayout after) {
    assert(cb==(changed?resumed:initial)); transitions++;
    if (image==expected_output) { assert(before==output_layout);output_layout=after; }
}
VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer cb,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,
    uint32_t n,const VkImageBlit *copy,VkFilter f) {
    (void)src;(void)sl;(void)dst;(void)dl;(void)n;(void)copy;(void)f;assert(cb==initial && !changed);
}
int rsf_sr_record_split(RsfSrSession *s,void **cb,const rsf_srvk_frame *f) {
    assert(s && *cb==initial && f->color.width==640 && f->depth.height==360);
    assert(f->output.image==(uint64_t)expected_output && f->output.format==VK_FORMAT_R16G16B16A16_SFLOAT);
    assert(f->output.width==1280 && f->output.height==720 && f->output.layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    assert(output_layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (replace_commands) { *cb=resumed;changed=1; }
    return status;
}
static int32_t fallback(const rsf_srvk_fallback *call) {
    assert(call->command_buffer==(changed?resumed:initial) && call->color.width==640 && call->output.width==1280);
    assert(call->output.format==VK_FORMAT_R16G16B16A16_SFLOAT && call->output.height==720);
    assert(call->output.layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && output_layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    fallback_calls++;return 0;
}
static void *rsf_runtime_symbol(const char *name) { assert(!strcmp(name,"rsf_srvk_spatial_fallback"));return fallback; }
static void vk_exec_draw_new_cb(VkExec *ex) { assert(ex->cb==(changed?resumed:initial)); }
''' + scene + r'''
int main(void) {
    VkExec ex={.cb=initial};
    struct Size e={.w=640,.h=360,.ow=1280,.oh=720,.sr_color=(VkImage)1,.out=(VkImage)55};
    rsf_frame_record record={0};
    assert(rsf_scene(&ex,&e,0,&record)==RSF_SR_FALLBACK);
    assert(opened==1 && cancelled==1 && fallback_calls==1 && ex.cb==resumed && transitions==8 && output_layout==VK_IMAGE_LAYOUT_GENERAL);
    ex.cb=initial;changed=0;status=RSF_SR_RECORDED;
    assert(rsf_scene(&ex,&e,0,&record)==RSF_SR_RECORDED && fallback_calls==1 && output_layout==VK_IMAGE_LAYOUT_GENERAL);
    ex.cb=initial;changed=0;replace_commands=0;status=RSF_SR_FAILED;
    assert(rsf_scene(&ex,&e,0,&record)==RSF_SR_FALLBACK && ex.cb==initial && output_layout==VK_IMAGE_LAYOUT_GENERAL);
    replace_commands=1;
    prepare=0;ex.cb=initial;changed=0;
    assert(rsf_scene(&ex,&e,0,&record)==RSF_SR_PENDING && ex.cb==initial);
    context.has_rsf_sr_split=0;
    assert(rsf_scene(&ex,&e,0,&record)==RSF_SR_FAILED && ex.cb==initial);
    selected_backend=3;status=RSF_SR_RECORDED;replace_commands=0;
    assert(rsf_scene(&ex,&e,0,&record)==RSF_SR_RECORDED && ex.cb==initial && output_layout==VK_IMAGE_LAYOUT_GENERAL);
    return 0;
}
''')
print("FSR4 serial admission and actual scene continuation/fallback: passed")

# Allocation identity is part of the service contract, not the present source ID.
# Compile the actual work-image allocator and record producer together so a new
# allocation with an unstamped generation fails this regression.
size_start = scene_source.index("struct Size {")
size_end = scene_source.index("} sizes[5], *last, *fg_last;", size_start)
size_type = scene_source[size_start:size_end] + "};\n"
allocation = scene_source[scene_source.index("static uint32_t next_resource_generation("):scene_source.index("static void full_barrier(")]
record = scene_source[scene_source.index("static rsf_frame_record scene_record("):scene_source.index("static void fg_prepare(")]
run("scene-resource-generation", r'''
#define DLSS_SIZES (sizeof D.sizes / sizeof D.sizes[0])
static int cfg_int(const char *key) {(void)key;return 1;}
static const float fallback_frame_ms=16.67f;
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "rsf_sr.h"
#include "rsf_camera.h"
typedef void *VmaAllocation;
typedef struct { uint64_t source_frame; } VkExec;
''' + size_type + r'''
static struct {
    struct Size sizes[5], *last;
    uint64_t last_eval_ns, evals;
    int reset;
    float previous_record_jitter[2];
    RsfCameraProjection projection;
    RsfCameraMatrices camera_matrices;
} D;
#define rt_fatal(...) abort()
static unsigned next_image;
static int vk_image2d(uint32_t w,uint32_t h,VkFormat f,VkImageUsageFlags u,VkImage *i,VmaAllocation *a,VkImageView *v) {
    (void)f;(void)u;(void)a;(void)v;assert(w && h);*i=(VkImage)(uintptr_t)++next_image;return 1;
}
static int free_size(struct Size *e) { memset(e,0,sizeof *e);return 1; }
static void res_sizes(unsigned *game,unsigned *scene) { game[0]=1920;game[1]=1080;scene[0]=640;scene[1]=360; }
static int vk_present_fg_output(unsigned w,unsigned h,unsigned *out) { out[0]=w;out[1]=h;return 1; }
static uint64_t plat_now_ns(void) { return 1000000000; }
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer cb,const VkDependencyInfo *di) { (void)cb;assert(di->imageMemoryBarrierCount==6); }
''' + allocation + record + r'''
int main(void) {
    struct Size *e=size_for((void *)1,640,360,1280,720);
    assert(e && e->resource_generation);
    VkExec ex={.source_frame=1001};
    rsf_frame_record first=scene_record(&ex,e,.25f,-.125f);
    assert(first.resource_generation==e->resource_generation && first.resource_generation);
    assert(first.render_width==640 && first.render_height==360 && first.output_width==1280 && first.output_height==720);
    assert(first.camera.render_width==640 && first.camera.output_height==720);
    assert(size_for((void *)1,640,360,1280,720)==e);
    ex.source_frame++;
    rsf_frame_record second=scene_record(&ex,e,-.25f,.125f);
    assert(second.camera.previous_jitter_pixels[0]==.25f && second.camera.previous_jitter_pixels[1]==-.125f);
    D.reset=1;rsf_frame_record reset=scene_record(&ex,e,0,0);
    assert(!reset.camera.previous_jitter_pixels[0] && !reset.camera.previous_jitter_pixels[1]);
    assert(second.frame_id!=first.frame_id && second.resource_generation==first.resource_generation);
    struct Size *other=size_for((void *)1,800,450,1600,900);
    assert(other && other->resource_generation && other->resource_generation!=e->resource_generation);
    free_size(e);
    struct Size *replacement=size_for((void *)1,640,360,1280,720);
    assert(replacement==e && replacement->resource_generation!=first.resource_generation);
    assert(first.resource_generation==second.resource_generation); // copied metadata remains immutable
    return 0;
}
''')
print("FSR4 actual allocation/record resource generations: passed")
