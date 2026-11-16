"""CPU contracts for actual quality selection, provider negotiation and FG boundary admission."""
from pathlib import Path
import subprocess

from rsf_snapshot_currentness_check import extract_region, extract_function
root=Path(__file__).resolve().parents[1]
out=root/'build/checks/rsf-reduced-quality-worker';out.mkdir(parents=True,exist_ok=True)
s=(root/'runtime/vk_dlaa.c').read_text()
def between(a,b): return extract_region(s, a, b)
incs=['-I',str(root/'runtime')]
for d in ('runtime/orchestrator/include','runtime/contract/include','sdk/game/include'):
    incs+=['-I',str(root/'external/ReScaleFrame'/d)]
def run(name,code):
    code = 'static inline __attribute__((unused)) int rt_log_sampled(unsigned long long n,unsigned long long first,unsigned long long period) {return n<=first || (period && n%period==0);}\n' + "#define RSF_GAME_SESSION_ID 1u\n#define RSF_FALLBACK_SESSION_ID 2u\n#define RSF_GRAPHICS_QUEUES 4u\n" + '#define rsf_config_enabled() cfg_bool("TOTK_RSF_ENABLED")\n#define rsf_config_fg_backend() cfg_int("TOTK_RSF_FG")\n#define rsf_config_sr_backend() cfg_int("TOTK_RSF_BACKEND")\n#define rsf_config_low_latency() cfg_int("TOTK_RSF_LOW_LATENCY")\n#define rsf_requested() (rsf_config_enabled() || rsf_config_fg_backend() || rsf_config_low_latency())\n#define rsf_temporal_requested() (rsf_config_enabled() || rsf_config_fg_backend())\n' + code
    path=out/(name+'.c');path.write_text('// '+'TOTK_RECOMP_'+'GENERATED_FROM_GAME_CODE\n'+code)
    subprocess.run(['C:/msys64/ucrt64/bin/clang.exe','-std=gnu11','-O2','-Wall','-Werror',*incs,str(path),'-o',str(out/(name+'.exe'))],check=True)
    subprocess.run([str(out/(name+'.exe'))],check=True)
run('live-modes',r'''

#include <assert.h>
#include <stdio.h>
#include <string.h>
static int temporal_available=1;
static struct { int preset_dirty,reset,failed,jit_ok,jit_used,cam_prev_ok,on; } D={.on=1};
static int enabled=1,selected=1,bias_calls,mode_calls;
static float negotiated_bias=-.5f;
static char requested[32]="quality",posted[32];
static int vk_dlaa_wanted(void) {return enabled;}
static int cfg_int(const char *n) {(void)n;return selected;}
static const char *cfg_value(const char *n) {(void)n;return requested;}
static void vk_sampler_dlss_bias(float b) {(void)b;bias_calls++;}
static float res_dlss_lod_bias(void) {return negotiated_bias;}
static int res_dlss_mode(const char *m) {snprintf(posted,sizeof posted,"%s",m);mode_calls++;return 1;}
'''+between('static void dlss_mode(', '// The preset, read again')+r'''
int main(void) {
    dlss_mode();assert(D.preset_dirty && D.reset && !strcmp(posted,"quality"));
    D.preset_dirty=D.reset=0;dlss_mode();assert(!D.preset_dirty);
    int old_bias=bias_calls;negotiated_bias=-.6f;dlss_mode();assert(bias_calls==old_bias+1 && !D.preset_dirty);
    selected=5;D.failed=1;D.jit_ok=D.jit_used=D.cam_prev_ok=1;dlss_mode();
    assert(D.preset_dirty && D.reset && !D.failed && !D.jit_ok && !D.cam_prev_ok);
    strcpy(requested,"balanced");dlss_mode();assert(!strcmp(posted,"balanced"));
    strcpy(requested,"performance");dlss_mode();assert(!strcmp(posted,"performance"));
    enabled=0;dlss_mode();assert(!strcmp(posted,"off"));int n=mode_calls;dlss_mode();assert(mode_calls==n);
    enabled=1;dlss_mode();assert(!strcmp(posted,"performance") && mode_calls==n+1);
    strcpy(requested,"native");dlss_mode();assert(!strcmp(posted,"dlaa"));
    return 0;
}
''')
run('accepted-fg-owner',r'''
#include <assert.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <rescaleframe/native_fg_vulkan.h>
typedef struct {VkCommandBuffer cb;uint64_t source_frame;} VkExec;
struct Size {unsigned w,h,ow,oh,fw,fh;VkImage device_depth,mv;rsf_frame_record fg_record;};
static struct {struct Size *fg_last,*last;} D;
static int refuse;
#define cfg_int(...) 3
#define rt_log(...) ((void)0)
#define rec_drain() ((void)0)
#define vk_exec_draw_new_cb(...) ((void)0)
#define vk_image_layout(...) ((void)0)
static rsf_srvk_image vk_rsf_image(VkImage i,VkFormat f,unsigned w,unsigned h,VkImageLayout l) {
    return (rsf_srvk_image){0,(uint64_t)i,f,w,h,l,1};
}
static int vk_present_fg_prepare(VkCommandBuffer cb,const rsf_fgvk_frame *f) {
    (void)cb;assert(f->depth.width==1280 && f->motion.height==720 && f->motion_scale_x==1 && f->motion_scale_y==1);
    assert(f->record->render_width==1280 && f->record->output_width==2560 && f->record->camera.render_height==720);
    return refuse;
}
'''+between('static void fg_prepare(', 'static int rsf_scene(')+r'''
int main(void) {
    VkExec ex={.source_frame=100};struct Size old={0},e={.w=1280,.h=720,.ow=1920,.oh=1080,.fw=2560,.fh=1600};
    D.last=&old;
    rsf_frame_record r={.frame_id=100,.render_width=1280,.render_height=720,.output_width=1920,.output_height=1080,
        .camera={.render_width=1280,.render_height=720,.output_width=1920,.output_height=1080}};
    fg_prepare(&ex,&e,&r);assert(D.fg_last==&e && D.last==&old && e.fg_record.frame_id==100);
    // SR may now return pending: accepted FG must not be redirected to D.last.
    assert(r.output_width==1920 && e.fg_record.output_width==2560);
    refuse=-1;fg_prepare(&ex,&e,&r);assert(!e.fg_record.frame_id);
    return 0;
}
''')
run('scaling-fg-boundary',r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define RT_V143 1
#define rt_log(...) ((void)0)
static struct {int up,fg_on;} D;
static struct {uint64_t frames;} gfx_counters;
typedef struct {int unused;} GfxShader;
typedef struct {struct {int image;} depth;unsigned pass_w;} VkExec;
static struct {unsigned width;} texture={100},*vk_dlaa_swap_img=&texture;
static const uint64_t FXAA_FS[]={123};
static uint64_t spirv_hash(const GfxShader *f) {(void)f;return 123;}
static int cfg_bool(const char *key) {(void)key;return 1;}
static int vk_exec_draw_samples(VkExec *ex,const GfxShader *fs,const void *t) {(void)ex;(void)fs;(void)t;return 0;}
static unsigned captured;
static void vk_dlaa_fg_after_draw(VkExec *ex,const GfxShader *fs) {(void)ex;(void)fs;if (D.fg_on) captured++;}
'''+s[s.index('static int is_fxaa_shader_hash('):s.index('\n}', s.index('static int is_fxaa_shader_hash('))+2]+s[s.index('int vk_dlaa_skip_draw('):]+r'''
int main(void) {
    VkExec ex={0};GfxShader fs={0};D.up=1;
    assert(vk_dlaa_skip_draw(&ex,&fs));D.fg_on=1;
    assert(vk_dlaa_skip_draw(&ex,&fs) && captured==1); // capture SR output without scaling it again
    ex.depth.image=1;assert(!vk_dlaa_skip_draw(&ex,&fs) && captured==1);
    ex.depth.image=0;assert(!vk_dlaa_skip_draw(&ex,NULL) && captured==1);
    D.up=0;assert(!vk_dlaa_skip_draw(&ex,&fs) && captured==1);return 0;
}
''')
print('Actual live quality/backend/reset, accepted FG owner and scaling capture boundary: passed (CPU only)')

run('provider-plans',r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "rsf_sr.h"
typedef struct {void *device,*instance,*phys;int has_rsf_sr_split;} VkCtx;
typedef struct {VkCommandBuffer cb;} VkExec;
struct Size {unsigned w,h,ow,oh,rsf_w,rsf_h;RsfSrSession *rsf;VkImage sr_color,device_depth,mv,mask,out;void *sr_color_mem;VkImageView sr_color_view;};
static struct {struct {int inverted,infinite;} projection;struct {unsigned level,base_layer;VkImage image;} color;} D;
static struct {int on;uint64_t cpu_eval;} T;
static VkCtx ctx={.has_rsf_sr_split=1};
static int backend=1,quality=1,posted,recorded;
static unsigned planned_w,planned_h;
static int resize_state=1, execute_result=RSF_SR_RECORDED;
static const char *names[]={"native","quality","balanced","performance"};
#define rt_log(...) ((void)0)
#define rt_fatal(...) abort()
#define rsf_provider_log NULL
#define vk_image2d(...) 1
static VkCtx *vk_ctx(void) {return &ctx;}
static int cfg_int(const char *n) {(void)n;return backend;}
static const char *cfg_value(const char *n) {(void)n;return names[quality];}
static const char *rsf_provider_directory(unsigned b) {assert(b==(unsigned)backend);return "ignored";}
static rsf_vk_function rsf_vk_generic_instance_proc(void *o,const char *n) {(void)o;(void)n;return NULL;}
static rsf_vk_function rsf_vk_generic_device_proc(void *o,const char *n) {(void)o;(void)n;return NULL;}
static int vk_sync_sr_prepare(VkCommandBuffer cb,rsf_fsr4vk_handoff *h) {assert(cb);(void)h;return 1;}
static void vk_sync_sr_cancel(void) {}
static void res_sizes(unsigned *o,unsigned *scene) {o[0]=1920;o[1]=1080;scene[0]=1136;scene[1]=640;}
static int res_rsf_scene(unsigned w,unsigned h) {assert((w==planned_w && h==planned_h) || (resize_state==2 && w==1130 && h==636));posted++;return resize_state;}
int rsf_sr_open(const rsf_srvk_setup *setup,RsfSrSession **owner,uint32_t *w,uint32_t *h) {
    assert(setup->backend==(unsigned)backend && setup->quality==(unsigned)quality && setup->output_width==1920 && setup->output_height==1080);
    *owner=(void *)1;*w=planned_w;*h=planned_h;return 1;
}
int rsf_sr_open_split(const rsf_srvk_setup *setup,const rsf_fsr4vk_handoff *handoff,RsfSrSession **owner,uint32_t *w,uint32_t *h) {
    (void)handoff;assert(backend==4);return rsf_sr_open(setup,owner,w,h);
}
static uint64_t kern_now_ns(void) {return 0;}
static void rec_drain(void) {}
static void vk_image_layout(VkCommandBuffer cb,VkImage image,VkImageLayout a,VkImageLayout b) {(void)cb;(void)image;(void)a;(void)b;}
static rsf_srvk_image vk_rsf_image(VkImage i,VkFormat f,unsigned w,unsigned h,VkImageLayout l) {return (rsf_srvk_image){ctx.device,(uint64_t)i,f,w,h,l,1};}
VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer c,VkImage a,VkImageLayout b,VkImage d,VkImageLayout e,uint32_t n,const VkImageBlit *r,VkFilter f) {
    (void)c;(void)a;(void)b;(void)d;(void)e;(void)n;(void)f;assert(r->srcOffsets[1].x==(int)planned_w && r->dstOffsets[1].y==(int)planned_h);
}
int rsf_sr_record_split(RsfSrSession *owner,void **commands,const rsf_srvk_frame *frame) {
    assert(owner && *commands);
    assert(frame->color.width==planned_w && frame->color.height==planned_h && frame->color.format==VK_FORMAT_R16G16B16A16_SFLOAT);
    assert(frame->depth.width==planned_w && frame->depth.format==VK_FORMAT_R32_SFLOAT && frame->motion.height==planned_h && frame->motion.format==VK_FORMAT_R16G16_SFLOAT);
    assert(frame->output.width==1920 && frame->output.height==1080 && frame->output.layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    assert(frame->motion_scale_x==1 && frame->motion_scale_y==1);
    assert(frame->record->render_width==planned_w && frame->record->camera.render_height==planned_h);
    assert(frame->record->camera.jitter_pixels[0]==.25f && frame->record->camera.jitter_pixels[1]==-.25f);
    recorded++;return execute_result;
}
static void *rsf_runtime_symbol(const char *name) {(void)name;return NULL;}
static void vk_exec_draw_new_cb(VkExec *ex) {(void)ex;}
'''+between('static int rsf_scene(', '// Previous-frame producer facts')+r'''
int main(void) {
    int backends[]={1,3,5,4};unsigned widths[]={0,1280,1114,960},heights[]={0,720,627,540};
    for(unsigned b=0;b<4;b++) for(quality=1;quality<=3;quality++) {
        backend=backends[b];planned_w=widths[quality]+b;planned_h=heights[quality]+b;
        struct Size e={.w=1600,.h=900,.ow=1920,.oh=1080,.sr_color=(VkImage)1};
        VkExec ex={.cb=(void *)1};rsf_frame_record record={0};
        int before=posted;assert(rsf_scene(&ex,&e,0,&record)==RSF_SR_PENDING && posted==before+1);
        e.w=planned_w;e.h=planned_h;record.render_width=record.camera.render_width=e.w;record.render_height=record.camera.render_height=e.h;
        record.camera.jitter_pixels[0]=.25f;record.camera.jitter_pixels[1]=-.25f;
        assert(rsf_scene(&ex,&e,0,&record)==RSF_SR_RECORDED);
    }
    assert(recorded==12);
    backend=5;quality=1;planned_w=1136;planned_h=640;resize_state=2;
    struct Size rounded={.w=1136,.h=640,.ow=1920,.oh=1080,.rsf_w=1130,.rsf_h=636,.rsf=(void *)1,.sr_color=(VkImage)1};
    VkExec ex={.cb=(void *)1};rsf_frame_record record={.render_width=1136,.render_height=640,.camera={.render_width=1136,.render_height=640,.jitter_pixels={.25f,-.25f}}};
    assert(rsf_scene(&ex,&rounded,0,&record)==RSF_SR_RECORDED);
    execute_result=RSF_SR_FAILED;
    assert(rsf_scene(&ex,&rounded,0,&record)==RSF_SR_FAILED); // unsupported nearest size is explicit
    return 0;
}
''')
print('Actual quality/balanced/performance provider-plan negotiation for DLSS/FSR3/XeSS/split FSR4: passed (CPU only)')

run('independent-fg-and-jitter',r'''
#include <assert.h>
#include <vulkan/vulkan.h>
static int temporal_available=1;
static struct {int on,failed,fg_on,jit_ok,jit_used;VkImage jdepth;float jx,jy,ux,uy;} D;
static int enabled=1,wanted_reads;
#define rt_log(...) ((void)0)
typedef struct {struct {VkImage image;} depth;} VkExec;
static int vk_dlaa_wanted(void) {wanted_reads++;return enabled;}
static int ended_at(const VkExec *ex) {(void)ex;return 0;}
'''+between('static int dl_active(', 'static int free_size(')+between('void vk_dlaa_viewport(', '// The scene camera:')+r'''
int main(void) {
    D.on=D.fg_on=D.jit_ok=1;D.jdepth=(VkImage)1;D.jx=.25f;D.jy=-.25f;
    assert(dl_active());dl_fail("test",-1);assert(dl_active() && !D.jit_ok);
    VkExec ex={.depth.image=(VkImage)1};VkViewport v={.x=3,.y=4};
    int reads=wanted_reads;vk_dlaa_viewport(&ex,&v);assert(wanted_reads==reads);
    D.jit_ok=1;vk_dlaa_viewport(&ex,&v);assert(v.x==3 && v.y==4); // failed SR leaves FG unjittered
    D.failed=0;enabled=0;vk_dlaa_viewport(&ex,&v);assert(v.x==3 && v.y==4); // first disabled frame too
    enabled=1;vk_dlaa_viewport(&ex,&v);assert(v.x==3.25f && v.y==3.75f);
    D.failed=1;D.fg_on=0;assert(!dl_active());return 0;
}
''')
print('Independent FG admission and unjittered disabled/failed SR: passed (CPU only)')


cfg_source = (root / 'runtime/cfg.c').read_text(encoding='utf-8')
run('legacy-backend', r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#define strcasecmp _stricmp
#define PREFIX "TOTK_"
#define PREFIX_LEN 5
static const char *legacy_enabled, *legacy_quality, *explicit_enabled, *explicit_backend;
static const char *raw_get(const char *name) {
    if (!strcmp(name,"TOTK_DLAA")) return legacy_enabled;
    if (!strcmp(name,"TOTK_DLSS")) return legacy_quality;
    if (!strcmp(name,"TOTK_RSF_ENABLED")) return explicit_enabled;
    if (!strcmp(name,"TOTK_RSF_BACKEND")) return explicit_backend;
    return NULL;
}
static int profile_of(void) {return 0;}
static int shape_keeps_prompts(void) {return 0;}
static char screen_res[1];
static const struct {const char *key,*value;} DECK[]={{"",""}};
''' + extract_function(cfg_source, 'const char *cfg_get(') + r'''
static int backend(void) {const char *v=cfg_get("TOTK_RSF_BACKEND");return v?atoi(v):3;}
int main(void) {
    assert(backend()==3);
    legacy_enabled="1";legacy_quality="quality";
    assert(!strcmp(cfg_get("TOTK_RSF_ENABLED"),"1") && backend()==1);
    explicit_backend="5";assert(backend()==5);explicit_backend=NULL;
    explicit_enabled="1";assert(backend()==3);
    explicit_enabled="0";assert(backend()==3);
    explicit_enabled=NULL;legacy_enabled="0";assert(backend()==3);
    legacy_enabled=NULL;assert(backend()==3);
    return 0;
}
''')
print('Actual legacy reconstruction backend migration and explicit/new-config precedence passed')

evaluate_prefix = s[s.index('static void evaluate('):s.index('    if (e != D.last)', s.index('static void evaluate('))]
run('failed-size-jitter', r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
typedef uint64_t VkCommandBuffer;
typedef struct {unsigned width,height;} VkExecTarget;
typedef struct {VkCommandBuffer cb;uint64_t source_frame;} VkExec;
struct Size {int out;};
static struct {VkExecTarget color,depth;int jit_ok,preset_dirty;struct Size sizes[5],*last,*fg_last;} D;
#define DLSS_SIZES (sizeof D.sizes/sizeof D.sizes[0])
enum {PRODUCER_ENTERED=1,PRODUCER_NO_SIZE=1,PRODUCER_RETIREMENT_PENDING=5};
static struct {unsigned stage,reason,w,h,ow,oh;uint64_t source;} producer_trace;
static int free_ok,size_ok;
static void vk_exec_barrier(VkExec *ex) {(void)ex;}
static int free_size(struct Size *e) {(void)e;return free_ok;}
static struct Size *size_for(VkCommandBuffer cb,unsigned w,unsigned h,unsigned ow,unsigned oh) {
    (void)cb;(void)w;(void)h;(void)ow;(void)oh;return size_ok?&D.sizes[0]:NULL;
}
''' + evaluate_prefix + r'''
    (void)z;
}
int main(void) {
    VkExec ex={1,1};D.color=(VkExecTarget){64,64};D.depth=D.color;
    D.sizes[0].out=1;D.preset_dirty=D.jit_ok=1;
    evaluate(&ex,NULL);assert(!D.jit_ok && D.preset_dirty);
    free_ok=1;D.jit_ok=1;
    evaluate(&ex,NULL);assert(!D.jit_ok && !D.preset_dirty);
    size_ok=1;D.jit_ok=1;
    evaluate(&ex,NULL);assert(D.jit_ok);
    return 0;
}
''')
print('Actual size-retirement/allocation refusal clears reconstruction jitter')

