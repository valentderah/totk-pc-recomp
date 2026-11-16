#include "rsf_fg_gpu.h"
#include "rsf_fg_map_spv.h"
#include "host.h"
#include <string.h>
#define COUNT 4
enum { FG_INVALID_INPUT = 1, FG_NO_LEASE = 2, FG_INVALID_RESOURCES = 3, FG_INVALID_HUDLESS = 4, FG_REFUSAL_COUNT = 5 };
enum { FG_IMAGES = 3, FG_GUIDES = 2, FG_BINDINGS = 5, FG_RECORDINGS = 2 };
typedef struct {
    float crop[4]; int dst[4]; int size[2];
    float scale[2], far_depth; int mode; float pad[2];
} FgPush;
_Static_assert(sizeof(FgPush) == 64, "FG map shader push layout");
struct RsfFgLease {
    RsfFgMap map; rsf_frame_record record;
    VkImage image[FG_IMAGES]; VmaAllocation memory[FG_IMAGES]; VkImageView view[FG_IMAGES], source[FG_IMAGES];
    unsigned w,h; int used; uint64_t fence; uint64_t source_key[FG_IMAGES]; VkCommandBuffer commands[FG_RECORDINGS];
};
static struct { VkDescriptorSetLayout descriptors; VkPipelineLayout layout; VkPipeline pipeline; VkSampler sampler;
    PFN_vkCmdPushDescriptorSetKHR push; struct RsfFgLease slots[COUNT]; } G;
int rsf_fg_gpu_commands_pending(VkCommandBuffer commands) {
    if (!commands) return 0;
    for (int i=0;i<COUNT;i++) for (int k=0;k<FG_RECORDINGS;k++)
        if (__atomic_load_n(&G.slots[i].commands[k],__ATOMIC_ACQUIRE)==commands) return 1;
    return 0;
}
int rsf_fg_gpu_pending(void) {
    for(int i=0;i<COUNT;i++) for(int k=0;k<FG_RECORDINGS;k++)
        if(__atomic_load_n(&G.slots[i].commands[k],__ATOMIC_ACQUIRE)) return 1;
    return 0;
}
void rsf_fg_gpu_submitted(VkCommandBuffer cb) {
    for(int i=0;i<COUNT;i++) for(int k=0;k<FG_RECORDINGS;k++) {
        VkCommandBuffer expected=cb;
        __atomic_compare_exchange_n(&G.slots[i].commands[k],&expected,VK_NULL_HANDLE,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
    }
}
static void refusal(unsigned reason,uint64_t source,uint64_t fence) {
    static uint64_t counts[FG_REFUSAL_COUNT];
    if(!(rt_trace&RT_TRACE_PRESENT)||reason>=FG_REFUSAL_COUNT) return;
    uint64_t n=++counts[reason];
    if(n!=1&&n%300) return;
    rt_log("rsf mapper: refusal %u count %llu source %llu recording fence %llu completed %llu",
        reason,(unsigned long long)n,(unsigned long long)source,(unsigned long long)fence,(unsigned long long)vk_sync_fence_done());
    for(unsigned i=0;i<COUNT;i++) rt_log("rsf mapper: slot %u used %d source %llu final fence %llu pending prepare %d HUDless %d",
        i,G.slots[i].used,(unsigned long long)G.slots[i].record.frame_id,(unsigned long long)G.slots[i].fence,
        G.slots[i].commands[0]!=VK_NULL_HANDLE,G.slots[i].commands[1]!=VK_NULL_HANDLE);
}
static const VkFormat formats[FG_IMAGES]={VK_FORMAT_R32_SFLOAT,VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_R8G8B8A8_UNORM};
static int init(void) {
    VkDevice device=vk_ctx()->device; if(G.pipeline) return 1;
    if(G.descriptors||G.layout||G.sampler) rsf_fg_gpu_release();
    VkPhysicalDevicePushDescriptorPropertiesKHR limits={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&limits};
    vkGetPhysicalDeviceProperties2(vk_ctx()->phys,&properties); if(limits.maxPushDescriptors<FG_BINDINGS) return 0;
    G.push=vk_pipeline_push_fn(); if(!G.push) return 0;
    VkDescriptorSetLayoutBinding b[FG_BINDINGS]={0}; for(unsigned i=0;i<FG_BINDINGS;i++) b[i]=(VkDescriptorSetLayoutBinding){i,i<FG_GUIDES?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
    VkDescriptorSetLayoutCreateInfo di={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.flags=VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,.bindingCount=FG_BINDINGS,.pBindings=b};
    VkPushConstantRange range={VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(FgPush)};
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&G.descriptors,.pushConstantRangeCount=1,.pPushConstantRanges=&range};
    VkSamplerCreateInfo si={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    if(vkCreateDescriptorSetLayout(device,&di,NULL,&G.descriptors)||vkCreatePipelineLayout(device,&li,NULL,&G.layout)||vkCreateSampler(device,&si,NULL,&G.sampler)) {rsf_fg_gpu_release();return 0;}
    VkShaderModule module; VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof rsf_fg_map_spv,.pCode=rsf_fg_map_spv};
    if(vkCreateShaderModule(device,&mi,NULL,&module)) {rsf_fg_gpu_release();return 0;}
    VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},.layout=G.layout};
    VkResult r=vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&ci,NULL,&G.pipeline); vkDestroyShaderModule(device,module,NULL); if(r!=VK_SUCCESS) rsf_fg_gpu_release(); return r==VK_SUCCESS;
}
static void free_slot(RsfFgLease *s) {
    VkCtx *c=vk_ctx(); for(int i=0;i<FG_IMAGES;i++) {
        if(s->source[i]) vkDestroyImageView(c->device,s->source[i],NULL);
        if(s->view[i]) vkDestroyImageView(c->device,s->view[i],NULL);
        if(s->image[i]) vmaDestroyImage(c->vma,s->image[i],s->memory[i]);
    } memset(s,0,offsetof(struct RsfFgLease,commands));
    for(int k=0;k<FG_RECORDINGS;k++) __atomic_store_n(&s->commands[k],VK_NULL_HANDLE,__ATOMIC_RELEASE);
}
void rsf_fg_gpu_recycle(void) { for(int i=0;i<COUNT;i++) { G.slots[i].used=0; G.slots[i].record.frame_id=0; } }
void rsf_fg_gpu_clear(void) {
    rsf_fg_gpu_recycle();
    for(int i=0;i<COUNT;i++) for(int k=0;k<3;k++) {
        if(G.slots[i].source[k]) vkDestroyImageView(vk_ctx()->device,G.slots[i].source[k],NULL);
        G.slots[i].source[k]=VK_NULL_HANDLE; G.slots[i].source_key[k]=0;
    }
}
void rsf_fg_gpu_release(void) {
    VkDevice d=vk_ctx()->device; for(int i=0;i<COUNT;i++) free_slot(&G.slots[i]);
    if(G.pipeline) vkDestroyPipeline(d,G.pipeline,NULL); if(G.sampler) vkDestroySampler(d,G.sampler,NULL);
    if(G.layout) vkDestroyPipelineLayout(d,G.layout,NULL); if(G.descriptors) vkDestroyDescriptorSetLayout(d,G.descriptors,NULL);
    memset(&G,0,offsetof(__typeof__(G),slots));
}
RsfFgLease *rsf_fg_gpu_find(uint64_t source) { for(int i=0;i<COUNT;i++) if(G.slots[i].used&&G.slots[i].record.frame_id==source) return &G.slots[i]; return NULL; }
static int allocate(RsfFgLease *s,unsigned w,unsigned h) {
    if(s->w==w&&s->h==h) return 1;
    VkCtx *c=vk_ctx(); for(int i=0;i<FG_IMAGES;i++) {
        VkFormatProperties prop; vkGetPhysicalDeviceFormatProperties(c->phys,formats[i],&prop);
        if(!(prop.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return 0;
        if(i==1) {
            VkFormatProperties3 flags={.sType=VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
            VkFormatProperties2 props={.sType=VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,.pNext=&flags};
            vkGetPhysicalDeviceFormatProperties2(c->phys,formats[i],&props);
            if(!(flags.optimalTilingFeatures&VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT)) return 0;
        }
        if(!vk_image2d(w,h,formats[i],VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       &s->image[i],&s->memory[i],&s->view[i])) return 0;
    } s->w=w; s->h=h; return 1;
}
static int source_view(const rsf_srvk_image *src,VkImageView *out,uint64_t *key) {
    if(!src->image||!src->width||!src->height||src->device!=vk_ctx()->device ||
       (src->layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&src->layout!=VK_IMAGE_LAYOUT_GENERAL)) return 0;
    if(*out&&*key==src->image) return 1; // A cached view already proved this immutable image format.
    VkFormatProperties props; vkGetPhysicalDeviceFormatProperties(vk_ctx()->phys,src->format,&props);
    if(!(props.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return 0;
    if(*out) vkDestroyImageView(vk_ctx()->device,*out,NULL);
    *out=VK_NULL_HANDLE; *key=src->image;
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=(VkImage)src->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=src->format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    return vkCreateImageView(vk_ctx()->device,&vi,NULL,out)==VK_SUCCESS;
}
static void dispatch(VkCommandBuffer cb,RsfFgLease *s,int mode,float mx,float my,const rsf_srvk_image *in0,const rsf_srvk_image *in1) {
    VkDescriptorImageInfo info[FG_BINDINGS]={{G.sampler,s->source[mode?2:0],in0->layout},{G.sampler,s->source[mode?2:1],in1->layout},
        {0,s->view[0],VK_IMAGE_LAYOUT_GENERAL},{0,s->view[1],VK_IMAGE_LAYOUT_GENERAL},{0,s->view[2],VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[FG_BINDINGS]={0}; for(unsigned i=0;i<FG_BINDINGS;i++) writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstBinding=i,.descriptorCount=1,.descriptorType=i<FG_GUIDES?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.pImageInfo=&info[i]};
    FgPush push={0};
    for(int i=0;i<4;i++) {push.crop[i]=(float)s->map.crop[i]/(i&1?s->map.game_h:s->map.game_w);push.dst[i]=s->map.dst[i];}
    push.size[0]=s->w;push.size[1]=s->h;push.scale[0]=mx;push.scale[1]=my;push.far_depth=s->record.camera.depth_inverted?0:1;push.mode=mode;
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,G.pipeline); rec_push(G.push,cb,VK_PIPELINE_BIND_POINT_COMPUTE,G.layout,0,FG_BINDINGS,writes);
    vkCmdPushConstants(cb,G.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof push,&push);vkCmdDispatch(cb,(s->w+7)/8,(s->h+7)/8,1);
}
static rsf_srvk_image output_image(RsfFgLease *s,int i) {return vk_rsf_image(s->image[i],formats[i],s->w,s->h,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);}
RsfFgLease *rsf_fg_gpu_prepare(VkCommandBuffer cb,uint64_t fence,const RsfFgMap *m,const RsfFgMap *old,const rsf_fgvk_frame *in,rsf_fgvk_frame *out) {
    if(!cb||!fence||!in||!in->record||!rsf_fg_map_valid(m)||!in->depth.width||!in->depth.height||!in->motion.width||!in->motion.height||
       in->record->render_width!=in->depth.width||in->record->render_height!=in->depth.height||
       in->depth.format!=VK_FORMAT_R32_SFLOAT||in->motion.format!=VK_FORMAT_R16G16_SFLOAT||!init()) {refusal(FG_INVALID_INPUT,in&&in->record?in->record->frame_id:0,fence);return NULL;}
    // Every provider copies these inputs into private resources in the producer
    // commands. Once that submission completes, its mapped backing is reusable.
    // A successful-submit notification alone does not prove GPU completion.
    uint64_t completed=vk_sync_fence_done();
    RsfFgLease *s=NULL;
    for(int i=0;i<COUNT;i++) {
        RsfFgLease *candidate=&G.slots[i];
        if(!candidate->used || (candidate->fence<=completed && !candidate->commands[0] && !candidate->commands[1])) {
            s=candidate;break;
        }
    }
    if(!s) {refusal(FG_NO_LEASE,in->record->frame_id,fence);return NULL;}
    if(s->w!=m->out_w||s->h!=m->out_h) free_slot(s);
    s->used=1;s->fence=fence;s->map=*m;s->record=*in->record;
    float scale[2];rsf_fg_map_motion(m,in->motion.width,in->motion.height,scale);
    if(!rsf_fg_map_camera(m,old,&s->record)||!allocate(s,m->out_w,m->out_h)||!source_view(&in->depth,&s->source[0],&s->source_key[0])||!source_view(&in->motion,&s->source[1],&s->source_key[1])) {refusal(FG_INVALID_RESOURCES,in->record->frame_id,fence);free_slot(s);return NULL;}
    for(int i=0;i<FG_IMAGES;i++) vk_image_layout(cb,s->image[i],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
    // Producer has made both inputs shader-readable; preserve its layouts.
    vk_image_layout(cb,(VkImage)in->depth.image,in->depth.layout,in->depth.layout);vk_image_layout(cb,(VkImage)in->motion.image,in->motion.layout,in->motion.layout);
    __atomic_store_n(&s->commands[0],cb,__ATOMIC_RELEASE);
    dispatch(cb,s,0,scale[0]*in->motion_scale_x,scale[1]*in->motion_scale_y,&in->depth,&in->motion);
    for(int i=0;i<FG_GUIDES;i++) vk_image_layout(cb,s->image[i],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // The SDK records directly; its input copies must follow every queued mapper command.
    rec_drain();
    *out=*in;out->record=&s->record;out->depth=output_image(s,0);out->motion=output_image(s,1);out->motion_scale_x=out->motion_scale_y=1;return s;
}
int rsf_fg_gpu_hudless(VkCommandBuffer cb,RsfFgLease *s,const rsf_fgvk_frame *in,rsf_fgvk_frame *out) {
    // HUD-less output can be recorded in a later guest submission than guides.
    // Its actual recording fence, not preparation's older fence, retires backing.
    uint64_t fence=vk_sync_record_fence(cb);
    if(!cb||!fence||!s||!in||in->hudless.format!=VK_FORMAT_R8G8B8A8_UNORM||!source_view(&in->hudless,&s->source[2],&s->source_key[2])) {
        refusal(FG_INVALID_HUDLESS,in&&in->record?in->record->frame_id:0,fence);return 0;
    }
    if(fence>s->fence) s->fence=fence;
    vk_image_layout(cb,(VkImage)in->hudless.image,in->hudless.layout,in->hudless.layout);
    // Bind all storage descriptors in GENERAL, even those untouched by this mode.
    for(int i=0;i<FG_GUIDES;i++) vk_image_layout(cb,s->image[i],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
    __atomic_store_n(&s->commands[1],cb,__ATOMIC_RELEASE);
    dispatch(cb,s,1,1,1,&in->hudless,&in->hudless);
    for(int i=0;i<FG_IMAGES;i++) vk_image_layout(cb,s->image[i],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    rec_drain(); // Preserve mapper/barrier order before the SDK's direct HUD-less copy.
    *out=*in;out->record=&s->record;out->hudless=output_image(s,2);
    // Guides were mapped to host-pixel motion during prepare. A HUD-less-only
    // packet has zero input scales, but provider validation still needs the
    // same nonzero scale contract as that accepted preparation.
    out->motion_scale_x=out->motion_scale_y=1;return 1;
}
