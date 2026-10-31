// Private opt-in timestamps. No queries, callbacks or timer reads when disabled.
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "dlaa_gpu_profile.h"
#include "gpu_timestamp_math.h"
struct DlaaGpuProfile {VkQueryPool pool;uint64_t frame;unsigned mask,next;int evaluated;};
static struct {int checked,on,cap_checked;unsigned bits;double period;double sum[7],peak[7];unsigned samples;} Q;
static int enabled(void){
    if(!Q.checked){const char *v=getenv("TOTK_DLAA_GPU_STATS");Q.on=v&&*v=='1';Q.checked=1;}return Q.on;
}
static int capability(void){
    if(Q.cap_checked)return Q.bits!=0;Q.cap_checked=1;VkCtx *c=vk_ctx();uint32_t count=0;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys,&count,NULL);
    VkQueueFamilyProperties *p=calloc(count?count:1,sizeof *p);if(!p)return 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys,&count,p);
    if(c->gfx_family<count)Q.bits=p[c->gfx_family].timestampValidBits;free(p);Q.period=c->props.limits.timestampPeriod;
    if(!Q.bits||Q.bits>64||!isfinite(Q.period)||Q.period<=0){Q.bits=0;rt_log("DLAA GPU: timestamps unavailable");}
    return Q.bits!=0;
}
static void completed(void *arg){
    DlaaGpuProfile *p=arg;uint64_t values[8];double elapsed[7];VkCtx *c=vk_ctx();
    VkResult r=vkGetQueryPoolResults(c->device,p->pool,0,8,sizeof values,values,sizeof values[0],VK_QUERY_RESULT_64_BIT);
    if(r!=VK_SUCCESS||!gt_intervals(values,p->mask,Q.bits,Q.period,elapsed))rt_log("DLAA GPU: invalid query results at frame %llu: %d, mask %u",(unsigned long long)p->frame,r,p->mask);
    else if(p->evaluated){
        for(unsigned i=0;i<7;i++){Q.sum[i]+=elapsed[i];if(elapsed[i]>Q.peak[i])Q.peak[i]=elapsed[i];}
        if(++Q.samples==120){
            const char *names[7]={"motion geometry","camera reconstruction","NGX prerequisites","NGX commands","raw color history","HDR replacement","depth history"};
            for(unsigned i=0;i<7;i++){rt_log("DLAA GPU: frame %llu: %s: %.3f ms mean, %.3f ms peak over %u evaluations",(unsigned long long)p->frame,names[i],Q.sum[i]/Q.samples,Q.peak[i],Q.samples);Q.sum[i]=Q.peak[i]=0;}
            Q.samples=0;
        }
    }
    vkDestroyQueryPool(c->device,p->pool,NULL);free(p);
}
DlaaGpuProfile *dlaa_gpu_profile_begin(VkExec *ex,uint64_t frame){
    if(!enabled()||frame<1000||!capability())return NULL;
    DlaaGpuProfile *p=calloc(1,sizeof *p);if(!p)rt_fatal("DLAA GPU: query context allocation");p->frame=frame;
    VkQueryPoolCreateInfo info={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=8};
    VkResult r=vkCreateQueryPool(vk_ctx()->device,&info,NULL,&p->pool);if(r!=VK_SUCCESS){rt_log("DLAA GPU: query allocation failed %d",r);free(p);return NULL;}
    vkCmdResetQueryPool(ex->cb,p->pool,0,8);ex->recorded++;dlaa_gpu_profile_mark(p,ex,0);return p;
}
void dlaa_gpu_profile_mark(DlaaGpuProfile *p,VkExec *ex,unsigned index){
    if(!p)return;if(index>=8||index!=p->next)rt_fatal("DLAA GPU: timestamp order mismatch");
    vkCmdWriteTimestamp(ex->cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,p->pool,index);p->mask|=1u<<index;p->next++;ex->recorded++;
}
void dlaa_gpu_profile_live(DlaaGpuProfile *p,int evaluated){if(p)p->evaluated=evaluated;}
void dlaa_gpu_profile_end(DlaaGpuProfile *p,VkExec *ex){if(!p)return;dlaa_gpu_profile_mark(p,ex,7);vk_completion_add(ex->fence,completed,p);}
