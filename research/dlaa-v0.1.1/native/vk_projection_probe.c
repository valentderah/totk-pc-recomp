// One-frame projection experiments with optional motion and recognized reconstruction coordination.
// Legacy one-frame modes exclude NGX; a separate bounded sequence permits offscreen evaluation.
// Immutable descriptor copies leave submission snapshots untouched.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "vk_projection_probe.h"
#include "projection_camera.h"
#include "jitter_math.h"
#include "raster_jitter_schedule.h"
#include "dlaa_cpu_profile.h"
#include "projection_shader_pair.h"
#include "projection_validation_cache.h"
#include "projection_snapshot_match.h"
#include "vk_ngx_eval.h"
extern const void *vk_usnap_probe_read(uint64_t address,uint64_t bytes);
typedef struct Clone {
    struct Clone *next;VkBuffer buffer;VmaAllocation allocation;
    uint64_t frame,fence,address,id;uintptr_t snapshot_source;uint64_t snapshot_generation;unsigned bindings;
    unsigned stage_bindings[2],slot_mask[2],extent_mask,nprograms,program_overflow;int reconstruction;float jitter[2];
    struct {uint64_t id,vs,fs;unsigned slots[2];} programs[256];
    float source[592],bound[592];char report[2400];
} Clone;
static Clone *clones;static SRWLOCK cache_lock=SRWLOCK_INIT,report_lock=SRWLOCK_INIT;
static uint64_t serial,live_ready_frame;
void vk_projection_probe_live_ready(uint64_t frame,int ready){live_ready_frame=ready?frame:0;}
// Replay thread only, like the renderer descriptor state. Completion callbacks never access this anchor.
static uint64_t anchor_frame;static float anchor_vp[16];
static int flag(const char *name){const char *e=getenv(name);return e&&*e&&*e!='0';}
uint32_t totk_probe_drs_factor_bits(uint32_t original){
    if(!flag("TOTK_DRS_FEEDBACK_PROBE"))return original;
    const char *required[]={"TOTK_NGX_JITTER_SEQUENCE","TOTK_PROJECTION_JITTER_PROBE","TOTK_PROJECTION_MOTION_PROBE","TOTK_PROJECTION_RECONSTRUCTION_PROBE","TOTK_NGX_SEQUENCE","TOTK_NGX_EVALUATE","TOTK_NGX_PROBE"};
    for(unsigned i=0;i<sizeof required/sizeof required[0];i++)if(!flag(required[i]))rt_fatal("DRS feedback probe: incomplete private mode bundle");
    static LONG logged;if(!InterlockedCompareExchange(&logged,1,0))rt_log("DRS feedback probe: GPU-time factor reduced 1000x; private resolution qualification, performance invalid");
    return 0x358637bdu; // Positive finite 1e-6 instead of the game's 1e-3; no zero-time division.
}
int vk_projection_probe_enabled(void){
    static int checked,on;if(checked)return on;checked=1;on=flag("TOTK_PROJECTION_JITTER_PROBE");if(!on)return 0;
    const char *bad[]={"TOTK_NGX_PROBE","TOTK_NGX_EVALUATE","TOTK_NGX_SEQUENCE","TOTK_CAMERA_AUDIT","TOTK_MOTION_OBJECTS","TOTK_MOTION_GEOMETRY","TOTK_GPU_JITTER_PROBE","TOTK_GPU_TEST_INVALID_POSE","TOTK_GPU_TEST_INVALID_CAMERA","TOTK_GPU_TEST_COLOR_DISCONTINUITY","TOTK_GPU_TEST_PREVIOUS_POSE"};
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++)if(flag(bad[i])&&!(flag("TOTK_NGX_JITTER_SEQUENCE")&&i<3))rt_fatal("projection jitter: incompatible diagnostic %s",bad[i]);
    const char *bind=getenv("TOTK_BIND_SKIP"),*push=getenv("TOTK_PUSH_SKIP");
    if(!bind||strcmp(bind,"0")||!push||strcmp(push,"0"))rt_fatal("projection jitter: descriptor skipping must be disabled by the private probe runner");
    return 1;
}
int vk_projection_probe_motion_enabled(void){
    static int checked,on;if(checked)return on;checked=1;on=flag("TOTK_PROJECTION_MOTION_PROBE");if(on&&!vk_projection_probe_enabled())rt_fatal("projection motion: requires the private projection probe");return on;
}
int vk_projection_probe_reconstruction_enabled(void){
    static int checked,on;if(checked)return on;checked=1;on=flag("TOTK_PROJECTION_RECONSTRUCTION_PROBE");if(on&&!vk_projection_probe_motion_enabled())rt_fatal("projection reconstruction: requires coordinated projection motion");return on;
}
int vk_projection_probe_sequence_enabled(void){
    static int checked,on;if(checked)return on;checked=1;on=flag("TOTK_NGX_JITTER_SEQUENCE");if(on&&(!vk_projection_probe_reconstruction_enabled()||!flag("TOTK_NGX_SEQUENCE")||!flag("TOTK_NGX_EVALUATE")||!flag("TOTK_NGX_PROBE")))rt_fatal("raster jitter sequence: incomplete private mode bundle");return on;
}
unsigned vk_projection_probe_frame_count(void){
    static unsigned count;if(count)return count;const char *value=getenv("TOTK_NGX_JITTER_FRAMES");
    char *tail=NULL;unsigned long parsed=value?strtoul(value,&tail,10):8;
    if((parsed!=8&&parsed!=32)||(value&&(!*value||*tail))||(parsed!=8&&!flag("TOTK_NGX_JITTER_SEQUENCE")))rt_fatal("raster sequence: only explicit bounded 8/32 counts supported");
    count=(unsigned)parsed;return count;
}
int vk_projection_probe_expected_offset(uint64_t frame,float jitter[2]){
    jitter[0]=jitter[1]=0;if(vk_ngx_eval_live_enabled())return vk_ngx_eval_live_healthy()?rj_live_schedule(frame,live_ready_frame,jitter):0;static int checked;static uint64_t end;if(!checked){const char *e=getenv("TOTK_DUMP");end=e?strtoull(e,NULL,10):0;checked=1;}if(!end)return 0;
    if(vk_projection_probe_sequence_enabled())return rj_schedule_count(frame,end,vk_projection_probe_frame_count(),jitter);
    if(frame!=end)return 0;jitter[0]=.375f;jitter[1]=-.25f;return 1;
}
int vk_projection_probe_export_frame(uint64_t frame,uint64_t end){
    if(vk_ngx_eval_live_enabled()||!end)return 0;
    if(vk_projection_probe_sequence_enabled()){unsigned n=vk_projection_probe_frame_count();return end>=n&&frame>=end-n&&frame<=end+1;}
    return frame==end||frame+1==end||(vk_projection_probe_motion_enabled()&&frame==end+1);
}
static int validation_cache_enabled(void){static int checked,on;if(!checked){const char *v=getenv("TOTK_DLAA_CAMERA_VALIDATION_CACHE");on=v&&*v=='1';checked=1;}return on;}
static int projection_camera_valid(const float *source,unsigned bytes,unsigned width,unsigned height,uint64_t frame){
    if(!validation_cache_enabled())return pj_reconstruction_camera(source,bytes,width,height,anchor_vp);
    if(!source)return 0;
    // Only used AFTER the anchor was established. Snapshot storage and anchor_vp
    // are immutable for these identities; each drop/end bumps vk_usnap_gen().
    static PjValidationCache cache;
    PjValidationKey key={{(uint64_t)(uintptr_t)source,(uint64_t)(uintptr_t)anchor_vp,vk_usnap_gen(),frame,anchor_frame,bytes,width,height}};
    int accepted;if(pj_validation_find(&cache,&key,&accepted))return accepted;
    accepted=pj_reconstruction_camera(source,bytes,width,height,anchor_vp);pj_validation_store(&cache,&key,accepted);return accepted;
}
static int camera_offset_impl(VkExec *ex,uint64_t address,uint64_t bytes,float jitter[2],const float **validated_source){
    if(validated_source)*validated_source=NULL;
    jitter[0]=jitter[1]=0;if(!vk_projection_probe_enabled())return 0;
    uint64_t frame=__atomic_load_n(&gfx_counters.frames,__ATOMIC_RELAXED)+1;
    float expected[2];if(!vk_projection_probe_expected_offset(frame,expected)||bytes!=0x940)return 0;
    if(ex->has_vp&&(ex->vp[0][0]!=0||ex->vp[0][1]!=0||ex->vp[0][2]!=ex->pass_w||ex->vp[0][3]!=ex->pass_h))return 0;
    const float *source=vk_usnap_probe_read(address,bytes);
    if(vk_projection_probe_reconstruction_enabled()){
        if(anchor_frame!=frame){
            if(!ex->depth.image||!pj_camera(source,(unsigned)bytes,ex->pass_w,ex->pass_h)||
               !pj_reconstruction_camera(source,(unsigned)bytes,ex->pass_w,ex->pass_h,source+12))return 0;
            memcpy(anchor_vp,source+12,sizeof anchor_vp);anchor_frame=frame;
        }
        if(!projection_camera_valid(source,(unsigned)bytes,ex->pass_w,ex->pass_h,frame))return 0;
    }else if(!ex->depth.image||!pj_camera(source,(unsigned)bytes,ex->pass_w,ex->pass_h))return 0;
    if(validated_source)*validated_source=source;
    jitter[0]=expected[0];jitter[1]=expected[1];return 1;
}
int vk_projection_probe_camera_offset(VkExec *ex,uint64_t address,uint64_t bytes,float jitter[2]){return camera_offset_impl(ex,address,bytes,jitter,NULL);}
static void matrix(FILE *f,const float *a){for(int i=0;i<16;i++)fprintf(f,"%s%.9g",i?",":"",a[i]);}
static unsigned shader_hash_count;
static uint64_t shader_hash(const GfxShader *shader){
    static struct {const GfxShader *shader;uint64_t hash;} cache[8192];
    if(!shader)return 0;for(unsigned i=0;i<shader_hash_count;i++)if(cache[i].shader==shader)return cache[i].hash;
    uint64_t n=0,h=0xcbf29ce484222325ull;const uint8_t *bytes=(const uint8_t*)gfx_shader_spirv(shader,&n);
    for(uint64_t i=0;i<n*4;i++)h=(h^bytes[i])*0x100000001b3ull;
    if(shader_hash_count<8192){cache[shader_hash_count].shader=shader;cache[shader_hash_count++].hash=h;}return h;
}
// Replay-thread resolved shader state. VkExec.pipe itself intentionally has no resolved shader pointers.
static uint64_t active_program,active_vs,active_fs;
static PjShaderPair active_pair;
static uint64_t shader_identity(const void *shader){return shader_hash((const GfxShader*)shader);}
static int shader_pair_cache_enabled(void){static int checked,on;if(!checked){const char *v=getenv("TOTK_DLAA_SHADER_ID_CACHE");on=v&&*v=='1';checked=1;}return on;}
static void projection_shaders_impl(VkExec *ex,const GfxShader *vs,const GfxShader *fs){
    if(!vk_projection_probe_reconstruction_enabled())return;float jitter[2];
    if(!vk_projection_probe_expected_offset(__atomic_load_n(&gfx_counters.frames,__ATOMIC_RELAXED)+1,jitter))return;
    active_program=ex->prog?ex->prog->serial:0;
    // Preserve the original pointer-identity hash cache. If its capacity is exhausted,
    // uncached shaders must keep using the original lookup rather than retain a new shortcut.
    if(shader_pair_cache_enabled()&&shader_hash_count<8192){
        pj_shader_pair_update(&active_pair,vs,fs,shader_identity);active_vs=active_pair.hash[0];active_fs=active_pair.hash[1];
    }else{active_vs=shader_hash(vs);active_fs=shader_hash(fs);}
}
static void note_usage(Clone *p,VkExec *ex,int stage,int slot){
    p->bindings++;p->stage_bindings[stage]++;p->slot_mask[stage]|=1u<<slot;
    p->extent_mask|=ex->pass_w==2560&&ex->pass_h==1440?1u:ex->pass_w==1280&&ex->pass_h==720?2u:ex->pass_w==640&&ex->pass_h==360?4u:8u;
    if(!p->reconstruction)return;
    uint64_t program=ex->prog?ex->prog->serial:0;unsigned i=0;for(;i<p->nprograms;i++)if(p->programs[i].id==program)break;
    if(active_program!=program||!active_vs)rt_fatal("projection reconstruction: resolved shader evidence unavailable");
    if(i<p->nprograms&&(p->programs[i].vs!=active_vs||p->programs[i].fs!=active_fs))rt_fatal("projection reconstruction: shader variant changed within a camera copy");
    if(i==p->nprograms){if(i>=256){p->program_overflow++;return;}p->programs[i].id=program;p->programs[i].vs=active_vs;p->programs[i].fs=active_fs;p->nprograms++;}
    p->programs[i].slots[stage]|=1u<<slot;
}
static void completed(void *arg){
    Clone *p=arg;AcquireSRWLockExclusive(&cache_lock);Clone **at=&clones;while(*at&&*at!=p)at=&(*at)->next;if(*at)*at=p->next;ReleaseSRWLockExclusive(&cache_lock);
    if(vk_ngx_eval_live_enabled()){vmaDestroyBuffer(vk_ctx()->vma,p->buffer,p->allocation);free(p);return;}
    AcquireSRWLockExclusive(&report_lock);FILE *f=fopen(p->report,"a");if(!f)rt_fatal("projection jitter: report export failed");
    fprintf(f,"{\"id\":%llu,\"frame\":%llu,\"fence\":%llu,\"address\":\"%llx\",\"bindings\":%u,",(unsigned long long)p->id,(unsigned long long)p->frame,(unsigned long long)p->fence,(unsigned long long)p->address,p->bindings);
    fprintf(f,"\"vs_bindings\":%u,\"fs_bindings\":%u,\"vs_slot_mask\":%u,\"fs_slot_mask\":%u,\"extent_mask\":%u,\"reconstruction_probe_mode\":%s,\"vs_only\":%s,",p->stage_bindings[0],p->stage_bindings[1],p->slot_mask[0],p->slot_mask[1],p->extent_mask,p->reconstruction?"true":"false",p->stage_bindings[1]?"false":"true");
    fprintf(f,"\"jitter\":[%.9g,%.9g],\"gpu_completion_observed\":true,\"source_snapshot_unchanged_by_copy\":true,\"other_camera_bytes_unchanged\":true,\"full_scene_jitter_contract\":false,\"presented_via_ngx\":false,\"source_projection\":[",p->jitter[0],p->jitter[1]);
    matrix(f,p->source+28);fprintf(f,"],\"bound_projection\":[");matrix(f,p->bound+28);fprintf(f,"],\"source_vp\":[");matrix(f,p->source+12);fprintf(f,"],\"bound_vp\":[");matrix(f,p->bound+12);
    fprintf(f,"],\"program_overflow\":%u,\"programs\":[",p->program_overflow);
    for(unsigned i=0;i<p->nprograms;i++)fprintf(f,"%s{\"id\":%llu,\"vs_slot_mask\":%u,\"fs_slot_mask\":%u,\"vs\":\"%016llx\",\"fs\":\"%016llx\"}",i?",":"",(unsigned long long)p->programs[i].id,p->programs[i].slots[0],p->programs[i].slots[1],(unsigned long long)p->programs[i].vs,(unsigned long long)p->programs[i].fs);
    fprintf(f,"]}\n");fclose(f);ReleaseSRWLockExclusive(&report_lock);
    vmaDestroyBuffer(vk_ctx()->vma,p->buffer,p->allocation);free(p);
}
// Session environment is fixed before process creation; keep a CPU-owned copy off the per-UBO hot path.
static const char *projection_directory(void){
    static int mode=-1;static int checked;static char *directory;
    if(mode<0){const char *e=getenv("TOTK_DLAA_DIRECTORY_CACHE");mode=!(e&&*e=='0');}
    if(!mode)return getenv("TOTK_DUMP_DIR");
    if(!checked){const char *value=getenv("TOTK_DUMP_DIR");checked=1;
        if(value){size_t n=strlen(value)+1;directory=malloc(n);if(!directory)rt_fatal("projection directory allocation failed");memcpy(directory,value,n);}
    }return directory;
}
static int projection_override_impl(VkExec *ex,int stage,int slot,uint64_t address,uint64_t bytes,VkDescriptorBufferInfo *out){
    if(!vk_projection_probe_enabled())return 0;
    vk_ngx_eval_projection_tick(ex,__atomic_load_n(&gfx_counters.frames,__ATOMIC_RELAXED)+1);
    int reconstruction=vk_projection_probe_reconstruction_enabled();
    if(reconstruction){if((stage!=0&&stage!=1)||slot<3||slot>=21)return 0;}else if(stage!=0||slot!=4)return 0;
    const char *directory=projection_directory();if(!directory)return 0;
    int fast=validation_cache_enabled();const float *source=NULL;float jitter[2];
    if(fast){if(!camera_offset_impl(ex,address,bytes,jitter,&source))return 0;}
    else{if(!vk_projection_probe_camera_offset(ex,address,bytes,jitter))return 0;source=vk_usnap_probe_read(address,bytes);}
    if(!source)return 0;
    uint64_t frame=__atomic_load_n(&gfx_counters.frames,__ATOMIC_RELAXED)+1,generation=vk_usnap_gen();
    AcquireSRWLockExclusive(&cache_lock);
    for(Clone *p=clones;p;p=p->next)if(p->fence==ex->fence&&p->address==address&&pj_snapshot_matches(p->source,source,0x940,p->snapshot_source,p->snapshot_generation,generation,fast)){
        p->snapshot_source=(uintptr_t)source;p->snapshot_generation=generation;
        note_usage(p,ex,stage,slot);*out=(VkDescriptorBufferInfo){p->buffer,0,0x940};ReleaseSRWLockExclusive(&cache_lock);return 1;
    }
    Clone *p=calloc(1,sizeof *p);if(!p)rt_fatal("projection jitter: allocation failed");p->frame=frame;p->fence=ex->fence;p->address=address;p->id=++serial;p->snapshot_source=(uintptr_t)source;p->snapshot_generation=generation;p->reconstruction=reconstruction;memcpy(p->jitter,jitter,sizeof p->jitter);note_usage(p,ex,stage,slot);
    memcpy(p->source,source,0x940);memcpy(p->bound,source,0x940);
    if(!jm_offset(p->bound+28,p->source+28,jitter[0],jitter[1],2560,1440)||!jm_offset(p->bound+12,p->source+12,jitter[0],jitter[1],2560,1440))rt_fatal("projection jitter: projection offset failed");
    if(memcmp(p->source,p->bound,12*sizeof(float))||memcmp(p->source+44,p->bound+44,(592-44)*sizeof(float)))rt_fatal("projection jitter: unrelated camera bytes changed");
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=0x940,.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VmaAllocationCreateInfo ai={.usage=VMA_MEMORY_USAGE_AUTO,.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT};VmaAllocationInfo info;
    VkResult result=vmaCreateBuffer(vk_ctx()->vma,&bi,&ai,&p->buffer,&p->allocation,&info);if(result!=VK_SUCCESS)rt_fatal("projection jitter: uniform buffer creation %d",result);
    memcpy(info.pMappedData,p->bound,0x940);result=vmaFlushAllocation(vk_ctx()->vma,p->allocation,0,VK_WHOLE_SIZE);if(result!=VK_SUCCESS)rt_fatal("projection jitter: uniform flush %d",result);
    if(memcmp(source,p->source,0x940))rt_fatal("projection jitter: original snapshot changed during copy");
    snprintf(p->report,sizeof p->report,"%s/projection-jitter.jsonl",directory);p->next=clones;clones=p;*out=(VkDescriptorBufferInfo){p->buffer,0,0x940};
    ReleaseSRWLockExclusive(&cache_lock);vk_completion_add(ex->fence,completed,p);
    if(!vk_ngx_eval_live_enabled())rt_log("projection jitter: frame %llu cloned camera at %llx; reconstruction=%d; bounded NGX jitter=%d",(unsigned long long)frame,(unsigned long long)address,reconstruction,vk_projection_probe_sequence_enabled());return 1;
}

int vk_projection_probe_override(VkExec *ex,int stage,int slot,uint64_t address,uint64_t bytes,VkDescriptorBufferInfo *out){
    static unsigned sample;static DlaaCpuProfile profile;
    if(!dc_enabled()||(++sample&63))return projection_override_impl(ex,stage,slot,address,bytes,out);
    uint64_t begin=dc_start();int result=projection_override_impl(ex,stage,slot,address,bytes,out);
    dc_end(&profile,"projection bindings",begin,__atomic_load_n(&gfx_counters.frames,__ATOMIC_RELAXED)+1,64);return result;
}

void vk_projection_probe_shaders(VkExec *ex,const GfxShader *vs,const GfxShader *fs){
    static unsigned sample;static DlaaCpuProfile profile;
    if(!dc_enabled()||(++sample&63)){projection_shaders_impl(ex,vs,fs);return;}
    uint64_t begin=dc_start();projection_shaders_impl(ex,vs,fs);dc_end(&profile,"projection shader identities",begin,__atomic_load_n(&gfx_counters.frames,__ATOMIC_RELAXED)+1,64);
}
