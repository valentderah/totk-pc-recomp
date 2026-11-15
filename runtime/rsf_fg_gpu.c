#include "rsf_fg_gpu.h"
#include "rsf_fg_map_spv.h"
#include <string.h>
#define COUNT 4
struct RsfFgLease {
    RsfFgMap map; rsf_frame_record record;
    VkImage image[3]; VmaAllocation memory[3]; VkImageView view[3], source[3];
    unsigned w,h; int used; uint64_t fence; VkCommandBuffer commands[2]; uint64_t source_key[3];
};
static struct { VkDescriptorSetLayout descriptors; VkPipelineLayout layout; VkPipeline pipeline; VkSampler sampler;
    PFN_vkCmdPushDescriptorSetKHR push; struct RsfFgLease slots[COUNT]; } G;
int rsf_fg_gpu_pending(void) { for(int i=0;i<COUNT;i++) for(int k=0;k<2;k++) if(G.slots[i].commands[k]) return 1; return 0; }
void rsf_fg_gpu_submitted(VkCommandBuffer cb) { for(int i=0;i<COUNT;i++) for(int k=0;k<2;k++) if(G.slots[i].commands[k]==cb) G.slots[i].commands[k]=VK_NULL_HANDLE; }
static const VkFormat formats[3]={VK_FORMAT_R32_SFLOAT,VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_R8G8B8A8_UNORM};
static void barrier(VkCommandBuffer cb,VkImage image,VkImageLayout old,VkImageLayout next) {
    VkImageMemoryBarrier2 b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,.oldLayout=old,.newLayout=next,.image=image,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkDependencyInfo d={.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b}; vkCmdPipelineBarrier2(cb,&d);
}
static int init(void) {
    VkDevice device=vk_ctx()->device; if(G.pipeline) return 1;
    if(G.descriptors||G.layout||G.sampler) rsf_fg_gpu_release();
    VkPhysicalDevicePushDescriptorPropertiesKHR limits={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&limits};
    vkGetPhysicalDeviceProperties2(vk_ctx()->phys,&properties); if(limits.maxPushDescriptors<5) return 0;
    G.push=(PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(device,"vkCmdPushDescriptorSetKHR"); if(!G.push) return 0;
    VkDescriptorSetLayoutBinding b[5]={0}; for(unsigned i=0;i<5;i++) b[i]=(VkDescriptorSetLayoutBinding){i,i<2?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
    VkDescriptorSetLayoutCreateInfo di={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.flags=VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,.bindingCount=5,.pBindings=b};
    VkPushConstantRange range={VK_SHADER_STAGE_COMPUTE_BIT,0,64};
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
    VkCtx *c=vk_ctx(); for(int i=0;i<3;i++) {
        if(s->source[i]) vkDestroyImageView(c->device,s->source[i],NULL);
        if(s->view[i]) vkDestroyImageView(c->device,s->view[i],NULL);
        if(s->image[i]) vmaDestroyImage(c->vma,s->image[i],s->memory[i]);
    } memset(s,0,sizeof *s);
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
    memset(&G,0,sizeof G);
}
RsfFgLease *rsf_fg_gpu_find(uint64_t source) { for(int i=0;i<COUNT;i++) if(G.slots[i].used&&G.slots[i].record.frame_id==source) return &G.slots[i]; return NULL; }
const RsfFgMap *rsf_fg_gpu_map(const RsfFgLease *s) { return s?&s->map:NULL; }
static int allocate(RsfFgLease *s,unsigned w,unsigned h) {
    if(s->w==w&&s->h==h) return 1;
    VkCtx *c=vk_ctx(); for(int i=0;i<3;i++) {
        VkFormatProperties prop; vkGetPhysicalDeviceFormatProperties(c->phys,formats[i],&prop);
        if(!(prop.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return 0;
        if(i==1) {
            VkFormatProperties3 flags={.sType=VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
            VkFormatProperties2 props={.sType=VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,.pNext=&flags};
            vkGetPhysicalDeviceFormatProperties2(c->phys,formats[i],&props);
            if(!(flags.optimalTilingFeatures&VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT)) return 0;
        }
        VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=formats[i],.extent={w,h,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai={.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if(vmaCreateImage(c->vma,&ci,&ai,&s->image[i],&s->memory[i],NULL)) return 0;
        VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=s->image[i],.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=formats[i],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        if(vkCreateImageView(c->device,&vi,NULL,&s->view[i])) return 0;
    } s->w=w; s->h=h; return 1;
}
static int source_view(const rsf_srvk_image *src,VkImageView *out,uint64_t *key) {
    if(!src->image||!src->width||!src->height||src->device!=vk_ctx()->device ||
       (src->layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&src->layout!=VK_IMAGE_LAYOUT_GENERAL)) return 0;
    VkFormatProperties props; vkGetPhysicalDeviceFormatProperties(vk_ctx()->phys,src->format,&props);
    if(!(props.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return 0;
    if(*out&&*key==src->image) return 1;
    if(*out) vkDestroyImageView(vk_ctx()->device,*out,NULL);
    *out=VK_NULL_HANDLE; *key=src->image;
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=(VkImage)src->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=src->format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    return vkCreateImageView(vk_ctx()->device,&vi,NULL,out)==VK_SUCCESS;
}
static void dispatch(VkCommandBuffer cb,RsfFgLease *s,int mode,float mx,float my,const rsf_srvk_image *in0,const rsf_srvk_image *in1) {
    VkDescriptorImageInfo info[5]={{G.sampler,s->source[mode?2:0],in0->layout},{G.sampler,s->source[mode?2:1],in1->layout},
        {0,s->view[0],VK_IMAGE_LAYOUT_GENERAL},{0,s->view[1],VK_IMAGE_LAYOUT_GENERAL},{0,s->view[2],VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[5]={0}; for(unsigned i=0;i<5;i++) writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstBinding=i,.descriptorCount=1,.descriptorType=i<2?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.pImageInfo=&info[i]};
    struct {float crop[4]; int dst[4]; int size[2]; float scale[2],far_depth; int mode; float pad[2];} push={0};
    for(int i=0;i<4;i++) {push.crop[i]=(float)s->map.crop[i]/(i&1?s->map.game_h:s->map.game_w);push.dst[i]=s->map.dst[i];}
    push.size[0]=s->w;push.size[1]=s->h;push.scale[0]=mx;push.scale[1]=my;push.far_depth=s->record.camera.depth_inverted?0:1;push.mode=mode;
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,G.pipeline); rec_push(G.push,cb,VK_PIPELINE_BIND_POINT_COMPUTE,G.layout,0,5,writes);
    vkCmdPushConstants(cb,G.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof push,&push);vkCmdDispatch(cb,(s->w+7)/8,(s->h+7)/8,1);
}
static rsf_srvk_image output_image(RsfFgLease *s,int i) {return (rsf_srvk_image){vk_ctx()->device,(uint64_t)s->image[i],formats[i],s->w,s->h,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,1};}
RsfFgLease *rsf_fg_gpu_prepare(VkCommandBuffer cb,uint64_t fence,const RsfFgMap *m,const RsfFgMap *old,const rsf_fgvk_frame *in,rsf_fgvk_frame *out) {
    if(!cb||!fence||!in||!in->record||!rsf_fg_map_valid(m)||!in->depth.width||!in->depth.height||!in->motion.width||!in->motion.height||
       in->record->render_width!=in->depth.width||in->record->render_height!=in->depth.height||
       in->depth.format!=VK_FORMAT_R32_SFLOAT||in->motion.format!=VK_FORMAT_R16G16_SFLOAT||!init()) return NULL;
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
    if(!s) return NULL;
    if(s->w!=m->out_w||s->h!=m->out_h) free_slot(s);
    s->used=1;s->fence=fence;s->map=*m;s->record=*in->record;
    float scale[2];rsf_fg_map_motion(m,in->motion.width,in->motion.height,scale);
    if(!rsf_fg_map_camera(m,old,&s->record)||!allocate(s,m->out_w,m->out_h)||!source_view(&in->depth,&s->source[0],&s->source_key[0])||!source_view(&in->motion,&s->source[1],&s->source_key[1])) {free_slot(s);return NULL;}
    for(int i=0;i<3;i++) barrier(cb,s->image[i],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
    // Producer has made both inputs shader-readable; preserve its layouts.
    barrier(cb,(VkImage)in->depth.image,in->depth.layout,in->depth.layout);barrier(cb,(VkImage)in->motion.image,in->motion.layout,in->motion.layout);
    s->commands[0]=cb;
    dispatch(cb,s,0,scale[0]*in->motion_scale_x,scale[1]*in->motion_scale_y,&in->depth,&in->motion);
    for(int i=0;i<2;i++) barrier(cb,s->image[i],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // The SDK records directly; its input copies must follow every queued mapper command.
    rec_drain();
    *out=*in;out->record=&s->record;out->depth=output_image(s,0);out->motion=output_image(s,1);out->motion_scale_x=out->motion_scale_y=1;return s;
}
int rsf_fg_gpu_hudless(VkCommandBuffer cb,RsfFgLease *s,const rsf_fgvk_frame *in,rsf_fgvk_frame *out) {
    if(!s||!in||in->hudless.format!=VK_FORMAT_R8G8B8A8_UNORM||!source_view(&in->hudless,&s->source[2],&s->source_key[2])) return 0;
    barrier(cb,(VkImage)in->hudless.image,in->hudless.layout,in->hudless.layout);
    // Bind all storage descriptors in GENERAL, even those untouched by this mode.
    for(int i=0;i<2;i++) barrier(cb,s->image[i],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
    s->commands[1]=cb;
    dispatch(cb,s,1,1,1,&in->hudless,&in->hudless);
    for(int i=0;i<3;i++) barrier(cb,s->image[i],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    rec_drain(); // Preserve mapper/barrier order before the SDK's direct HUD-less copy.
    *out=*in;out->record=&s->record;out->hudless=output_image(s,2);return 1;
}
