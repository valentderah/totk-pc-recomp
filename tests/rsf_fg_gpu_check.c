// CPU allocation/lease fixture. All Vulkan calls below are inert.
#include "../runtime/rsf_fg_gpu.c"
#include <assert.h>
static VkCtx context;
static uint64_t completed;
uint64_t vk_sync_fence_done(void) {return completed;}
VkCtx *vk_ctx(void) {return &context;}
static unsigned creates,destroys,views,deleted_views,dispatches,drains;
static int routed_push;static int allocation_fail, init_fail;
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice p,VkPhysicalDeviceProperties2 *o){(void)p;((VkPhysicalDevicePushDescriptorPropertiesKHR*)o->pNext)->maxPushDescriptors=32;}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice p,VkFormat f,VkFormatProperties *o){(void)p;(void)f;o->optimalTilingFeatures=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice p,VkFormat f,VkFormatProperties2 *o){(void)p;(void)f;((VkFormatProperties3*)o->pNext)->optimalTilingFeatures=VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;}
static void VKAPI_CALL push(VkCommandBuffer cb,VkPipelineBindPoint b,VkPipelineLayout l,uint32_t s,uint32_t n,const VkWriteDescriptorSet *w){(void)cb;(void)b;(void)l;(void)s;assert(routed_push&&n==5&&w);}
void rec_push(PFN_vkCmdPushDescriptorSetKHR fn,VkCommandBuffer cb,VkPipelineBindPoint bp,VkPipelineLayout layout,uint32_t set,uint32_t n,const VkWriteDescriptorSet *w) {
    routed_push=1; fn(cb,bp,layout,set,n,w); routed_push=0;
}
void rec_drain(void) {assert(dispatches>drains);++drains;}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice d,const char*n){(void)d;(void)n;return (PFN_vkVoidFunction)push;}
#define CREATE(name,Info,Handle) VKAPI_ATTR VkResult VKAPI_CALL name(VkDevice d,const Info *i,const VkAllocationCallbacks*a,Handle*out){(void)d;(void)i;(void)a;if(init_fail&&!strcmp(#name,"vkCreateSampler"))return VK_ERROR_INITIALIZATION_FAILED;*out=(Handle)(uintptr_t)1;return VK_SUCCESS;}
CREATE(vkCreateDescriptorSetLayout,VkDescriptorSetLayoutCreateInfo,VkDescriptorSetLayout)
CREATE(vkCreatePipelineLayout,VkPipelineLayoutCreateInfo,VkPipelineLayout)
CREATE(vkCreateSampler,VkSamplerCreateInfo,VkSampler)
CREATE(vkCreateShaderModule,VkShaderModuleCreateInfo,VkShaderModule)
#define DESTROY(name,Handle) VKAPI_ATTR void VKAPI_CALL name(VkDevice d,Handle h,const VkAllocationCallbacks*a){(void)d;(void)h;(void)a;}
DESTROY(vkDestroyDescriptorSetLayout,VkDescriptorSetLayout)
DESTROY(vkDestroyPipelineLayout,VkPipelineLayout)
DESTROY(vkDestroySampler,VkSampler)
DESTROY(vkDestroyShaderModule,VkShaderModule)
DESTROY(vkDestroyPipeline,VkPipeline)
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice d,VkPipelineCache c,uint32_t n,const VkComputePipelineCreateInfo*i,const VkAllocationCallbacks*a,VkPipeline*out){(void)d;(void)c;(void)n;(void)i;(void)a;*out=(VkPipeline)(uintptr_t)1;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice d,const VkImageViewCreateInfo*i,const VkAllocationCallbacks*a,VkImageView*out){(void)d;(void)i;(void)a;*out=(VkImageView)(uintptr_t)++views;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice d,VkImageView v,const VkAllocationCallbacks*a){(void)d;(void)v;(void)a;++deleted_views;}
VkResult vmaCreateImage(VmaAllocator a,const VkImageCreateInfo*i,const VmaAllocationCreateInfo*ai,VkImage*out,VmaAllocation*memory,VmaAllocationInfo*info){(void)a;(void)ai;(void)info;assert(i->usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT);if(allocation_fail&&creates==1)return VK_ERROR_OUT_OF_DEVICE_MEMORY;*out=(VkImage)(uintptr_t)++creates;*memory=(VmaAllocation)(uintptr_t)creates;return VK_SUCCESS;}
void vmaDestroyImage(VmaAllocator a,VkImage i,VmaAllocation m){(void)a;(void)i;(void)m;++destroys;}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer c,const VkDependencyInfo*i){(void)c;(void)i;}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint b,VkPipeline p){(void)c;(void)b;(void)p;}
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer c,VkPipelineLayout l,VkShaderStageFlags s,uint32_t o,uint32_t n,const void*p){(void)c;(void)l;(void)s;(void)o;assert(n==64&&p);}
VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z){(void)c;assert(x&&y&&z==1);++dispatches;}
int main(void){
    RsfFgMap m={1920,1080,2560,1600,{240,120,1280,720},{0,80,2560,1440}};
    rsf_frame_record record={.frame_id=1,.render_width=1600,.render_height=900};
    for(int i=0;i<4;i++) record.camera.view_to_clip[i*5]=record.camera.clip_to_view[i*5]=record.camera.clip_to_previous_clip[i*5]=1;
    rsf_fgvk_frame in={.record=&record,.depth={0,11,VK_FORMAT_R32_SFLOAT,1600,900,VK_IMAGE_LAYOUT_GENERAL,1},
        .motion={0,12,VK_FORMAT_R16G16_SFLOAT,800,450,VK_IMAGE_LAYOUT_GENERAL,1},.motion_scale_x=1,.motion_scale_y=1,
        .hudless={0,13,VK_FORMAT_R8G8B8A8_UNORM,1920,1080,VK_IMAGE_LAYOUT_GENERAL,1}},out;
    VkCommandBuffer cb=(VkCommandBuffer)(uintptr_t)1;
    init_fail=1;assert(!rsf_fg_gpu_prepare(cb,1,&m,NULL,&in,&out));assert(!G.descriptors&&!G.layout&&!G.sampler&&!G.pipeline);init_fail=0;
    allocation_fail=1;assert(!rsf_fg_gpu_prepare(cb,1,&m,NULL,&in,&out));assert(!rsf_fg_gpu_pending()&&creates==1&&destroys==1);
    allocation_fail=0;RsfFgLease*s=rsf_fg_gpu_prepare(cb,1,&m,NULL,&in,&out);assert(s&&creates==4&&rsf_fg_gpu_pending()&&drains==1);
    assert(out.depth.width==2560&&out.motion.height==1600&&out.record->camera.output_height==1600);
    rsf_fg_gpu_submitted(cb);assert(!rsf_fg_gpu_pending());
    assert(rsf_fg_gpu_hudless(cb,s,&in,&out)&&rsf_fg_gpu_pending());rsf_fg_gpu_submitted(cb);
    rsf_fg_gpu_recycle();unsigned before=views;
    s=rsf_fg_gpu_prepare(cb,1,&m,&m,&in,&out);assert(s&&creates==4&&views==before); // Stable extent reuses backing and source views.
    rsf_fg_gpu_submitted(cb);assert(rsf_fg_gpu_hudless(cb,s,&in,&out));rsf_fg_gpu_submitted(cb);
    assert(views==before);
    // Fill all four slots while submitted input copies remain GPU-incomplete.
    for(unsigned i=2;i<=4;i++) {
        record.frame_id=i;
        assert(rsf_fg_gpu_prepare(cb,i,&m,&m,&in,&out));
        rsf_fg_gpu_submitted(cb);
    }
    record.frame_id=5;
    assert(!rsf_fg_gpu_prepare(cb,5,&m,&m,&in,&out));
    unsigned allocated=creates;
    completed=1;
    assert(rsf_fg_gpu_prepare(cb,5,&m,&m,&in,&out));
    assert(creates==allocated); // GPU completion, without a device-idle drain, permits reuse.
    assert(!rsf_fg_gpu_prepare(cb,6,&m,&m,&in,&out)); // New recording is still leased.
    completed=5;
    assert(rsf_fg_gpu_prepare(cb,6,&m,&m,&in,&out)); // A different completed slot can be used.
    // Pending commands keep their slot even if an unrelated fence is complete.
    assert(G.slots[0].commands[0]==cb&&G.slots[0].record.frame_id==5);
    rsf_fg_gpu_submitted(cb);
    assert(drains==dispatches);rsf_fg_gpu_release();assert(destroys==creates&&deleted_views==views);
    return 0;
}
