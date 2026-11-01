// One-shot offscreen SDK evaluation. Original game attachments are never used as output.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "gfx.h"
#include <limits.h>
#include "ngx_probe.h"
#include "vk_ngx_eval.h"
#include "vk_projection_probe.h"
#include "raster_jitter_schedule.h"
#include "dlaa_display_guard.h"
#include "dlaa_cpu_profile.h"
#include "live_flag_load.h"
_Static_assert(VK_FORMAT_R16G16B16A16_SFLOAT==97 && VK_FORMAT_B10G11R11_UFLOAT_PACK32==122 && VK_FORMAT_FEATURE_BLIT_SRC_BIT==0x400 && VK_FORMAT_FEATURE_BLIT_DST_BIT==0x800,"display guard Vulkan values");
static volatile LONG live_stopped,live_failed;
static LONG read_live_flag(volatile LONG *flag){
    static int checked,on;if(!checked){const char *v=getenv("TOTK_DLAA_ACQUIRE_HEALTH_LOADS");on=v&&*v=='1';checked=1;}return lf_load(flag,on);
}
int vk_ngx_eval_live_healthy(void){return !read_live_flag(&live_failed);}
static SRWLOCK live_shutdown_lock=SRWLOCK_INIT;
int vk_ngx_eval_live_enabled(void){
    static int checked,on;if(!checked){const char *e=getenv("TOTK_DLAA_LIVE");on=e&&*e&&*e!='0';checked=1;
        if(on){const char *p=getenv("TOTK_DLAA_PREVIEW");if(!p||*p!='1')rt_fatal("Live DLAA requires explicit preview flag");}
    }return on;
}
static unsigned attempted;static uint64_t previous_frame;static int abort_scheduled;
typedef struct {unsigned attempted;uint64_t frame;char path[2400];} Abort;
static void aborted_completed(void *arg){
    Abort *a=arg;vk_queue_lock();int cleanup=ngx_probe_finish_eval();vk_queue_unlock();
    FILE *f=fopen(a->path,"w");if(!f)rt_fatal("NGX bounded abort: report unavailable");
    fprintf(f,"{\"evaluations\":%u,\"cutoff_frame\":%llu,\"gpu_completion_observed\":true,\"sdk_cleanup\":%d,\"complete_sequence\":false,\"presented\":false}\n",a->attempted,(unsigned long long)a->frame,cleanup);fclose(f);free(a);
    if(cleanup!=1)rt_fatal("NGX bounded abort: SDK shutdown unconfirmed");
    rt_log("NGX bounded abort: partial sequence cleaned after GPU completion");
}
void vk_ngx_eval_projection_tick(VkExec *ex,uint64_t frame){
    if(vk_ngx_eval_live_enabled()||!attempted||abort_scheduled)return;
    if(!vk_projection_probe_sequence_enabled()||attempted>=vk_projection_probe_frame_count())return;
    const char *end=getenv("TOTK_DUMP"),*directory=getenv("TOTK_DUMP_DIR");if(!end||!directory)return;
    if(!rj_abort_needed_count(attempted,abort_scheduled,frame,strtoull(end,NULL,10),vk_projection_probe_frame_count()))return;
    Abort *a=calloc(1,sizeof *a);if(!a)rt_fatal("NGX bounded abort: allocation failed");a->attempted=attempted;a->frame=frame;
    snprintf(a->path,sizeof a->path,"%s/ngx-aborted-sequence.json",directory);abort_scheduled=1;vk_completion_add(ex->fence,aborted_completed,a);
}
typedef struct {VkImage output;VmaAllocation image_alloc;VkImageView views[5];VkBuffer readback;VmaAllocation buffer_alloc;void *mapped;size_t bytes;int result,sequence,last,index,reset,presented,live;uint32_t width,height;uint64_t frame;char path[2048],report[2048];} Eval;
static Eval *pending_display;
static void check(VkResult r){if(r!=VK_SUCCESS)rt_fatal("NGX offscreen: Vulkan resource failure %d",r);}
static void barrier(VkCommandBuffer cb,VkImage image,VkImageAspectFlags aspect,VkImageLayout old){
    VkImageMemoryBarrier2 b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,.srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,.oldLayout=old,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={aspect,0,1,0,1}};
    VkDependencyInfo info={.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b};vkCmdPipelineBarrier2(cb,&info);
}
static void completed(void *arg){
    Eval *e=arg;VkCtx *ctx=vk_ctx();
    if(e->result==1&&e->readback){
        vmaInvalidateAllocation(ctx->vma,e->buffer_alloc,0,VK_WHOLE_SIZE);FILE *file=fopen(e->path,"wb");
        if(!file||fwrite(e->mapped,1,e->bytes,file)!=e->bytes)rt_fatal("NGX offscreen: output export failed");fclose(file);
    }
    int cleanup=0;
    if(!e->sequence||e->last){vk_queue_lock();cleanup=ngx_probe_finish_eval();vk_queue_unlock();}
    FILE *report=fopen(e->report,"w");if(report){fprintf(report,"{\"index\":%d,\"reset\":%d,\"last\":%s,\"frame\":%llu,\"width\":%u,\"height\":%u,\"record_result\":%d,\"gpu_completion_observed\":true,\"sdk_cleanup\":%d,\"presented\":%s}\n",e->index,e->reset,e->last?"true":"false",(unsigned long long)e->frame,e->width,e->height,e->result,cleanup,e->presented?"true":"false");fclose(report);}
    if((!e->sequence||e->last)&&cleanup!=1)rt_fatal("NGX offscreen: SDK shutdown not confirmed; stop isolated test before freeing resources");
    for(int i=0;i<5;i++)vkDestroyImageView(ctx->device,e->views[i],NULL);
    if(e->readback)vmaDestroyBuffer(ctx->vma,e->readback,e->buffer_alloc);vmaDestroyImage(ctx->vma,e->output,e->image_alloc);
    if(!e->live||e->index<4||e->index%120==0)rt_log("NGX offscreen: frame %llu GPU completion; record=%d, cleanup=%d; presented=%d",(unsigned long long)e->frame,e->result,cleanup,e->presented);free(e);
}
VkImage vk_ngx_eval_once(const VkNgxEvalInput *in){
    int live=vk_ngx_eval_live_enabled();
    if(live&&!dd_live_eligible(in->width,in->height,read_live_flag(&live_failed)))return VK_NULL_HANDLE;
    int sequence=ngx_probe_sequence_enabled();unsigned limit=live?UINT_MAX:vk_projection_probe_sequence_enabled()?vk_projection_probe_frame_count():sequence?8:1;
    if(read_live_flag(&live_stopped)||!ngx_probe_eval_enabled()||attempted>=limit)return VK_NULL_HANDLE;
    if(vk_projection_probe_sequence_enabled()){
        float expected[2];int offset_active=vk_projection_probe_expected_offset(in->frame,expected);if((!live&&!offset_active)||memcmp(expected,in->jitter,sizeof expected)||(!live&&attempted&&previous_frame+1!=in->frame))rt_fatal("raster jitter sequence: missing raster offset or discontinuous history");
        const char *end=getenv("TOTK_DUMP");if(!live&&!attempted&&(!end||in->frame+limit-1!=strtoull(end,NULL,10)))rt_fatal("raster jitter sequence: first frame was skipped");
    }
    uint64_t cpu_begin=dc_start();static DlaaCpuProfile allocations,drain,queue_wait,sdk_call;
    attempted++;
    VkExec *ex=in->ex;VkCtx *ctx=vk_ctx();Eval *e=calloc(1,sizeof *e);if(!e)rt_fatal("NGX offscreen: host allocation failed");
    e->live=live;e->sequence=sequence;e->index=attempted;e->last=attempted==limit;e->reset=!sequence||attempted==1||previous_frame+1!=in->frame;previous_frame=in->frame;
    e->width=in->width;e->height=in->height;e->frame=in->frame;e->bytes=(size_t)in->width*in->height*8;
    snprintf(e->path,sizeof e->path,"%s/ngx-output-%llu-%ux%u.rgba16f",in->directory,(unsigned long long)in->frame,in->width,in->height);
    if(live)snprintf(e->report,sizeof e->report,"%s/ngx-live-frame.json",in->directory);
    else if(sequence&&!e->last)snprintf(e->report,sizeof e->report,"%s/ngx-sequence-host-%u.json",in->directory,attempted);
    else snprintf(e->report,sizeof e->report,"%s/ngx-evaluate-host.json",in->directory);
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R16G16B16A16_SFLOAT,.extent={in->width,in->height,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai={.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};check(vmaCreateImage(ctx->vma,&ii,&ai,&e->output,&e->image_alloc,NULL));
    TotkNgxEvalFrame frame={.abi_version=TOTK_NGX_EVAL_ABI,.struct_size=sizeof frame,.width=in->width,.height=in->height,.command_buffer=(uint64_t)(uintptr_t)ex->cb,.reset=e->reset,.jitter_x=in->jitter[0],.jitter_y=in->jitter[1],.frame_time_ms=16.666667f};
    for(int i=0;i<5;i++){
        VkImage image=i==4?e->output:in->image[i];VkFormat format=i==4?VK_FORMAT_R16G16B16A16_SFLOAT:in->format[i];VkImageAspectFlags aspect=i==1?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=format,.subresourceRange={aspect,0,1,0,1}};check(vkCreateImageView(ctx->device,&vi,NULL,&e->views[i]));
        frame.image[i]=(uint64_t)(uintptr_t)image;frame.view[i]=(uint64_t)(uintptr_t)e->views[i];frame.format[i]=(uint32_t)format;
        barrier(ex->cb,image,aspect,i==4?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_GENERAL);
    }
    VkClearColorValue zero={0};VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(ex->cb,e->output,VK_IMAGE_LAYOUT_GENERAL,&zero,1,&range);barrier(ex->cb,e->output,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL);
    if(!live){
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=e->bytes,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ba={.usage=VMA_MEMORY_USAGE_AUTO,.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT};VmaAllocationInfo info;
    check(vmaCreateBuffer(ctx->vma,&bi,&ba,&e->readback,&e->buffer_alloc,&info));e->mapped=info.pMappedData;
    }
    // SDK records directly into this CB. Stop queued wrapper recording first; resume only after the API returns.
    dc_end(&allocations,"NGX resources",cpu_begin,in->frame,1);
    cpu_begin=dc_start();rec_drain();dc_end(&drain,"recorder drain",cpu_begin,in->frame,1);
    cpu_begin=dc_start();vk_queue_lock();dc_end(&queue_wait,"queue lock",cpu_begin,in->frame,1);
    cpu_begin=dc_start();e->result=ngx_probe_record_eval(&frame);dc_end(&sdk_call,"NGX evaluate record",cpu_begin,in->frame,1);vk_queue_unlock();
    if(live&&e->result!=1){
        InterlockedExchange(&live_failed,1);previous_frame=0;
        char path[2048];snprintf(path,sizeof path,"%s/ngx-live-failure.json",in->directory);FILE *f=fopen(path,"w");
        if(f){fprintf(f,"{\"frame\":%llu,\"record_result\":%d,\"disabled_until_restart\":true}\n",(unsigned long long)in->frame,e->result);fclose(f);}
        rt_log("Live DLAA: recording failed (%d), native image retained until restart",e->result);
    }
    if(e->result==1&&!live){
        barrier(ex->cb,e->output,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL);VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={in->width,in->height,1}};
        vkCmdCopyImageToBuffer(ex->cb,e->output,VK_IMAGE_LAYOUT_GENERAL,e->readback,1,&copy);
        VkMemoryBarrier2 mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,.srcAccessMask=VK_ACCESS_2_MEMORY_WRITE_BIT,.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT,.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT};
        VkDependencyInfo di={.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.memoryBarrierCount=1,.pMemoryBarriers=&mb};vkCmdPipelineBarrier2(ex->cb,&di);
    }
    vk_completion_add(ex->fence,completed,e);ex->recorded+=14;
    if(!live||attempted<4||attempted%120==0)rt_log("NGX offscreen: frame %llu recorded result %d; wait for GPU completion",(unsigned long long)in->frame,e->result);
    const char *preview=getenv("TOTK_DLAA_PREVIEW");
    if(preview&&*preview&&*preview!='0'&&e->result==1){
        if(!vk_projection_probe_sequence_enabled())rt_fatal("DLAA display requires coordinated raster sequence");
        pending_display=e;return e->output;
    }
    return VK_NULL_HANDLE;
}

int vk_ngx_eval_present(const VkNgxEvalInput *in,VkImage output){
    if(!output||!pending_display||pending_display->output!=output||pending_display->frame!=in->frame)return 0;
    VkFormatProperties a,b;vkGetPhysicalDeviceFormatProperties(vk_ctx()->phys,VK_FORMAT_R16G16B16A16_SFLOAT,&a);vkGetPhysicalDeviceFormatProperties(vk_ctx()->phys,in->format[0],&b);
    if(!dd_supported(in->width,in->height,VK_FORMAT_R16G16B16A16_SFLOAT,in->format[0],a.optimalTilingFeatures,b.optimalTilingFeatures))rt_fatal("DLAA display: unsupported format conversion");
    VkExec *ex=in->ex;barrier(ex->cb,output,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL);barrier(ex->cb,in->image[0],VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL);
    VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.srcOffsets={{0,0,0},{(int)in->width,(int)in->height,1}},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstOffsets={{0,0,0},{(int)in->width,(int)in->height,1}}};
    vkCmdBlitImage(ex->cb,output,VK_IMAGE_LAYOUT_GENERAL,in->image[0],VK_IMAGE_LAYOUT_GENERAL,1,&blit,VK_FILTER_NEAREST);
    barrier(ex->cb,in->image[0],VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL);pending_display->presented=1;pending_display=NULL;ex->recorded+=4;return 1;
}

// Called by pinned boot/window exit paths, before their _exit/ExitProcess (no atexit callbacks).
void vk_ngx_eval_live_shutdown(void){
    if(!vk_ngx_eval_live_enabled())return;
    AcquireSRWLockExclusive(&live_shutdown_lock);
    if(live_stopped){ReleaseSRWLockExclusive(&live_shutdown_lock);return;}
    InterlockedExchange(&live_stopped,1);
    // No later submission records NGX. Drain the backend/replay and all issued GPU work before releasing SDK state.
    if(vk_ctx()){gfx_backend()->finish();extern void vk_sync_ngx_drain(void);vk_sync_ngx_drain();}
    vk_queue_lock();int cleanup=ngx_probe_finish_eval();vk_queue_unlock();
    const char *directory=getenv("TOTK_DUMP_DIR");char path[2048];snprintf(path,sizeof path,"%s/ngx-live-shutdown.json",directory?directory:".");
    FILE *f=fopen(path,"w");if(!f)rt_fatal("Live DLAA shutdown report unavailable");
    fprintf(f,"{\"evaluations\":%u,\"gpu_completion_observed\":true,\"sdk_cleanup\":%d,\"continuous\":true}\n",attempted,cleanup);fclose(f);
    if(cleanup!=1)rt_fatal("Live DLAA SDK shutdown unconfirmed");
    rt_log("Live DLAA: SDK shutdown after %u evaluations",attempted);
    extern void dlaa_profile_present_flush(void);dlaa_profile_present_flush();
    InterlockedExchange(&live_stopped,2);ReleaseSRWLockExclusive(&live_shutdown_lock);
}
