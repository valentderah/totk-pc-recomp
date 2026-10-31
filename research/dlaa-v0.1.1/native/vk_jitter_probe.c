// Synthetic raster calibration only. Never modifies game constants, targets or NGX inputs.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "vk_jitter_probe.h"
#include "jitter_math.h"
#include "jitter_vs_spv.h"
#include "jitter_fs_spv.h"
#define JW 128u
#define JH 96u
#define JBYTES ((size_t)JW*JH*16)
static const float cases[4][4]={{0,0,0,0},{.375f,0,-.125f,0},{0,-.25f,0,.25f},{.375f,-.25f,-.125f,.25f}};
typedef struct {
    VkImage image;VmaAllocation image_alloc;VkImageView view;
    VkBuffer uniform,readback;VmaAllocation uniform_alloc,readback_alloc;void *mapped;
    VkDescriptorSetLayout set;VkPipelineLayout layout;VkPipeline pipeline;VkShaderModule vs,fs;
    uint64_t frame;char directory[2048];
} Probe;
static void check(VkResult r){if(r!=VK_SUCCESS)rt_fatal("jitter calibration: Vulkan failure %d",r);}
static void barrier(VkCommandBuffer cb,VkImage image,VkImageLayout old){
    VkImageMemoryBarrier2 b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,.srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,.oldLayout=old,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkDependencyInfo di={.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b};vkCmdPipelineBarrier2(cb,&di);
}
static VkShaderModule shader(const uint32_t *code,size_t bytes){VkShaderModule module;VkShaderModuleCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=bytes,.pCode=code};check(vkCreateShaderModule(vk_ctx()->device,&ci,NULL,&module));return module;}
static void completed(void *arg){
    Probe *p=arg;VkCtx *c=vk_ctx();check(vmaInvalidateAllocation(c->vma,p->readback_alloc,0,VK_WHOLE_SIZE));char path[2400];
    for(int i=0;i<4;i++){
        snprintf(path,sizeof path,"%s/jitter-calibration-%d.rgba32f",p->directory,i);FILE *f=fopen(path,"wb");
        if(!f||fwrite((char*)p->mapped+i*JBYTES,1,JBYTES,f)!=JBYTES)rt_fatal("jitter calibration: output export failed");fclose(f);
    }
    snprintf(path,sizeof path,"%s/jitter-calibration.json",p->directory);FILE *f=fopen(path,"w");if(!f)rt_fatal("jitter calibration: report export failed");
    fprintf(f,"{\"geometry_version\":2,\"subpixel_precision_bits\":%u,\"width\":%u,\"height\":%u,\"frame\":%llu,\"gpu_completion_observed\":true,\"vulkan_validation_enabled\":%s,\"game_projection_modified\":false,\"presented\":false,\"cases\":[",c->props.limits.subPixelPrecisionBits,JW,JH,(unsigned long long)p->frame,c->validation?"true":"false");
    for(int i=0;i<4;i++)fprintf(f,"%s{\"index\":%d,\"current\":[%.9g,%.9g],\"previous\":[%.9g,%.9g]}",i?",":"",i,cases[i][0],cases[i][1],cases[i][2],cases[i][3]);fprintf(f,"]}\n");fclose(f);
    vkDestroyPipeline(c->device,p->pipeline,NULL);vkDestroyShaderModule(c->device,p->vs,NULL);vkDestroyShaderModule(c->device,p->fs,NULL);
    vkDestroyPipelineLayout(c->device,p->layout,NULL);vkDestroyDescriptorSetLayout(c->device,p->set,NULL);vkDestroyImageView(c->device,p->view,NULL);
    vmaDestroyImage(c->vma,p->image,p->image_alloc);vmaDestroyBuffer(c->vma,p->uniform,p->uniform_alloc);vmaDestroyBuffer(c->vma,p->readback,p->readback_alloc);
    rt_log("jitter calibration: four synthetic outputs completed; original rendering unchanged");free(p);
}
void vk_jitter_probe_once(VkExec *ex,uint64_t frame,const char *directory){
    static int attempted;const char *enabled=getenv("TOTK_GPU_JITTER_PROBE");if(attempted||!enabled||!*enabled||*enabled=='0')return;attempted=1;
    VkCtx *c=vk_ctx();Probe *p=calloc(1,sizeof *p);if(!p)rt_fatal("jitter calibration: allocation");p->frame=frame;snprintf(p->directory,sizeof p->directory,"%s",directory);
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R32G32B32A32_SFLOAT,.extent={JW,JH,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai={.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};check(vmaCreateImage(c->vma,&ii,&ai,&p->image,&p->image_alloc,NULL));
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=p->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=ii.format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};check(vkCreateImageView(c->device,&vi,NULL,&p->view));
    size_t align=c->props.limits.minUniformBufferOffsetAlignment,stride=(144+align-1)/align*align;
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=stride*4,.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};VmaAllocationInfo info;
    VmaAllocationCreateInfo ba={.usage=VMA_MEMORY_USAGE_AUTO,.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT};
    check(vmaCreateBuffer(c->vma,&bi,&ba,&p->uniform,&p->uniform_alloc,&info));
    const float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    for(int i=0;i<4;i++){
        float *u=(float*)((char*)info.pMappedData+i*stride);
        if(!jm_offset(u,identity,cases[i][0],cases[i][1],JW,JH)||!jm_offset(u+16,identity,cases[i][2],cases[i][3],JW,JH))rt_fatal("jitter calibration: invalid matrices");
        u[32]=JW;u[33]=JH;u[34]=u[35]=0;
    }
    check(vmaFlushAllocation(c->vma,p->uniform_alloc,0,VK_WHOLE_SIZE));bi.size=4*JBYTES;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    ba.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;check(vmaCreateBuffer(c->vma,&bi,&ba,&p->readback,&p->readback_alloc,&info));p->mapped=info.pMappedData;
    VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo si={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.flags=VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,.bindingCount=1,.pBindings=&binding};check(vkCreateDescriptorSetLayout(c->device,&si,NULL,&p->set));
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&p->set};check(vkCreatePipelineLayout(c->device,&li,NULL,&p->layout));
    p->vs=shader(jitter_vs,sizeof jitter_vs);p->fs=shader(jitter_fs,sizeof jitter_fs);
    VkPipelineShaderStageCreateInfo stages[2]={{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=p->vs,.pName="main"},{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=p->fs,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vertex={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};VkPipelineColorBlendAttachmentState attachment={.colorWriteMask=15};VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment};
    VkDynamicState states[2]={VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT};VkPipelineDynamicStateCreateInfo dynamic={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=states};
    VkFormat format=ii.format;VkPipelineRenderingCreateInfo rendering={.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,.colorAttachmentCount=1,.pColorAttachmentFormats=&format};
    VkGraphicsPipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=&rendering,.stageCount=2,.pStages=stages,.pVertexInputState=&vertex,.pInputAssemblyState=&assembly,.pViewportState=&viewport,.pRasterizationState=&raster,.pMultisampleState=&ms,.pColorBlendState=&blend,.pDynamicState=&dynamic,.layout=p->layout};check(vkCreateGraphicsPipelines(c->device,VK_NULL_HANDLE,1,&pi,NULL,&p->pipeline));
    PFN_vkCmdPushDescriptorSetKHR push=(PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device,"vkCmdPushDescriptorSetKHR");if(!push)rt_fatal("jitter calibration: push descriptors unavailable");
    for(int i=0;i<4;i++){
        barrier(ex->cb,p->image,i?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED);
        VkRenderingAttachmentInfo color={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=p->view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},{JW,JH}},.layerCount=1,.colorAttachmentCount=1,.pColorAttachments=&color};vkCmdBeginRendering(ex->cb,&ri);
        VkViewport vp={0,JH,JW,-(float)JH,0,1};VkRect2D sc={{0,0},{JW,JH}};vkCmdSetViewportWithCount(ex->cb,1,&vp);vkCmdSetScissorWithCount(ex->cb,1,&sc);vkCmdBindPipeline(ex->cb,VK_PIPELINE_BIND_POINT_GRAPHICS,p->pipeline);
        VkDescriptorBufferInfo buffer={p->uniform,stride*i,144};VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&buffer};rec_push(push,ex->cb,VK_PIPELINE_BIND_POINT_GRAPHICS,p->layout,0,1,&write);
        vkCmdDraw(ex->cb,3,1,0,0);vkCmdEndRendering(ex->cb);barrier(ex->cb,p->image,VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy copy={.bufferOffset=i*JBYTES,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={JW,JH,1}};vkCmdCopyImageToBuffer(ex->cb,p->image,VK_IMAGE_LAYOUT_GENERAL,p->readback,1,&copy);
    }
    VkMemoryBarrier2 mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,.srcAccessMask=VK_ACCESS_2_MEMORY_WRITE_BIT,.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT,.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT};VkDependencyInfo di={.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.memoryBarrierCount=1,.pMemoryBarriers=&mb};vkCmdPipelineBarrier2(ex->cb,&di);
    vk_completion_add(ex->fence,completed,p);ex->recorded+=40;rt_log("jitter calibration: four synthetic cases recorded at frame %llu",(unsigned long long)frame);
}
