// Camera-only candidates. Static-world assumption does not establish object motion or identity.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "vk_camera_gpu.h"
#include "ngx_probe.h"
#include "vk_ngx_eval.h"
#include "vk_jitter_probe.h"
#include "vk_projection_probe.h"
#include "camera_vs_spv.h"
#include "camera_fs_spv.h"
typedef struct {VkImage image;VkImageView view;VmaAllocation alloc;} Image;
typedef struct {VkImage image;VkImageView view;VmaAllocation alloc;Image composite,bias,previous_color,mv;uint32_t w,h;int initialized;uint64_t color_frame;} Target;
typedef struct {VkBuffer buffer;VmaAllocation alloc;VkImageView view;void *map;size_t size;char path[600];} Buffer;
static struct {int initialized;VkDescriptorSetLayout set;VkPipelineLayout layout;VkShaderModule vs,fs;VkPipeline pipeline;VkSampler sampler;Target *target;} C;
static void check(VkResult result){if(result!=VK_SUCCESS)rt_fatal("camera GPU: Vulkan failure %d",result);}
static void target_free(void *arg){Target *t=arg;Image images[5]={{t->image,t->view,t->alloc},t->composite,t->bias,t->previous_color,t->mv};for(int i=0;i<5;i++){vkDestroyImageView(vk_ctx()->device,images[i].view,NULL);vmaDestroyImage(vk_ctx()->vma,images[i].image,images[i].alloc);}free(t);}
static void buffer_done(void *arg){
    Buffer *b=arg;
    if(b->path[0]){vmaInvalidateAllocation(vk_ctx()->vma,b->alloc,0,VK_WHOLE_SIZE);FILE *f=fopen(b->path,"wb");
        if(!f||fwrite(b->map,1,b->size,f)!=b->size)rt_fatal("camera GPU: export failed");fclose(f);}
    if(b->view)vkDestroyImageView(vk_ctx()->device,b->view,NULL);vmaDestroyBuffer(vk_ctx()->vma,b->buffer,b->alloc);free(b);
}
static Buffer *buffer(size_t bytes,int upload){
    Buffer *b=calloc(1,sizeof *b);if(!b)rt_fatal("camera GPU: buffer allocation");b->size=bytes;
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,.usage=upload?VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT:VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai={.usage=VMA_MEMORY_USAGE_AUTO,.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|(upload?VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT:VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT)};
    VmaAllocationInfo info;check(vmaCreateBuffer(vk_ctx()->vma,&bi,&ai,&b->buffer,&b->alloc,&info));b->map=info.pMappedData;return b;
}
static void transition(VkCommandBuffer cb,VkImage image,VkImageLayout old,VkImageLayout next){
    VkImageMemoryBarrier2 b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,.oldLayout=old,.newLayout=next,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkDependencyInfo info={.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b};vkCmdPipelineBarrier2(cb,&info);
}
static void barrier(VkCommandBuffer cb,VkImage image,VkImageLayout old){transition(cb,image,old,VK_IMAGE_LAYOUT_GENERAL);}
static Image image(uint32_t w,uint32_t h,VkFormat format,VkImageUsageFlags usage){
    Image result={0};VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=format,.extent={w,h,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=usage};
    VmaAllocationCreateInfo ai={.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};check(vmaCreateImage(vk_ctx()->vma,&ii,&ai,&result.image,&result.alloc,NULL));
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=result.image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};check(vkCreateImageView(vk_ctx()->device,&vi,NULL,&result.view));return result;
}
static VkShaderModule shader(const uint32_t *code,size_t bytes){VkShaderModule module;VkShaderModuleCreateInfo info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=bytes,.pCode=code};check(vkCreateShaderModule(vk_ctx()->device,&info,NULL,&module));return module;}
static void initialize(void){
    if(C.initialized)return;VkDevice device=vk_ctx()->device;
    VkDescriptorSetLayoutBinding bindings[6]={
        {.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding=2,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding=3,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding=4,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding=5,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT}};
    VkDescriptorSetLayoutCreateInfo si={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.flags=VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,.bindingCount=6,.pBindings=bindings};check(vkCreateDescriptorSetLayout(device,&si,NULL,&C.set));
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&C.set};check(vkCreatePipelineLayout(device,&li,NULL,&C.layout));
    VkSamplerCreateInfo sampler={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};check(vkCreateSampler(device,&sampler,NULL,&C.sampler));
    C.vs=shader(camera_vs,sizeof camera_vs);C.fs=shader(camera_fs,sizeof camera_fs);
    VkPipelineShaderStageCreateInfo stages[2]={{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=C.vs,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=C.fs,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState attachments[4]={{.colorWriteMask=15},{.colorWriteMask=15},{.colorWriteMask=15},{.colorWriteMask=15}};VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=4,.pAttachments=attachments};
    VkDynamicState states[2]={VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT};VkPipelineDynamicStateCreateInfo dynamic={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=states};
    VkFormat formats[4]={VK_FORMAT_R32G32B32A32_SFLOAT,VK_FORMAT_R32G32B32A32_SFLOAT,VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32G32_SFLOAT};VkPipelineRenderingCreateInfo rendering={.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,.colorAttachmentCount=4,.pColorAttachmentFormats=formats};
    VkGraphicsPipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=&rendering,.stageCount=2,.pStages=stages,.pVertexInputState=&vi,
        .pInputAssemblyState=&ia,.pViewportState=&vp,.pRasterizationState=&rs,.pMultisampleState=&ms,.pColorBlendState=&blend,.pDynamicState=&dynamic,.layout=C.layout};
    check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pi,NULL,&C.pipeline));C.initialized=1;
    ngx_probe_once();
}
static void metadata(const CameraGpuInput *in,int color_history){
    char path[600];snprintf(path,sizeof path,"%s/gpu-camera-%llu.json",in->directory,(unsigned long long)in->frame);
    FILE *f=fopen(path,"w");if(!f)rt_fatal("camera GPU: metadata export failed");
    fprintf(f,"{\"frame\":%llu,\"width\":%u,\"height\":%u,\"active\":%d,\"support\":%d,\"forced_invalid_test\":%d,\"static_world_assumption\":true,\"dlss_ready\":false,\"projection_probe_mode\":%s,",(unsigned long long)in->frame,in->width,in->height,in->active,in->support,in->forced_invalid,vk_projection_probe_enabled()?"true":"false");
    fprintf(f,"\"motion_projection_coordinated\":%s,\"current_jitter\":[%.9g,%.9g],\"previous_jitter\":[%.9g,%.9g],\"raw_current_vp\":[",in->motion_projection_coordinated?"true":"false",in->current_jitter[0],in->current_jitter[1],in->previous_jitter[0],in->previous_jitter[1]);
    for(int i=0;i<16;i++)fprintf(f,"%s%.17g",i?",":"",in->raw_current_vp[i]);fprintf(f,"],\"raw_previous_vp\":[");
    for(int i=0;i<16;i++)fprintf(f,"%s%.17g",i?",":"",in->raw_previous_vp[i]);fprintf(f,"],\"current_vp\":[");
    for(int i=0;i<16;i++)fprintf(f,"%s%.17g",i?",":"",in->current_vp[i]);fprintf(f,"],\"previous_vp\":[");
    for(int i=0;i<16;i++)fprintf(f,"%s%.17g",i?",":"",in->previous_vp[i]);fprintf(f,"],\"reprojection\":[");
    for(int i=0;i<16;i++)fprintf(f,"%s%.9g",i?",":"",in->reprojection[i]);fprintf(f,"],\"color_history_valid\":%d,\"color_discontinuity_test\":%d,\"guard_low\":0.04,\"guard_high\":0.20}\n",color_history,in->color_test);fclose(f);
}
static void export_image_aspect(const CameraGpuInput *in,VkImage image,size_t bytes,const char *stem,const char *suffix,VkImageAspectFlags aspect){
    VkExec *ex=in->ex;uint32_t w=in->width,h=in->height;Buffer *b=buffer(bytes,0);
    snprintf(b->path,sizeof b->path,"%s/%s-%llu-%ux%u.%s",in->directory,stem,(unsigned long long)in->frame,w,h,suffix);
    VkImageMemoryBarrier2 ib={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,.srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,.dstStageMask=VK_PIPELINE_STAGE_2_COPY_BIT,.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT,.oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_GENERAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={aspect,0,1,0,1}};
    VkDependencyInfo ii={.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&ib};vkCmdPipelineBarrier2(ex->cb,&ii);
    VkBufferImageCopy copy={.imageSubresource={aspect,0,0,1},.imageExtent={w,h,1}};
    vkCmdCopyImageToBuffer(ex->cb,image,VK_IMAGE_LAYOUT_GENERAL,b->buffer,1,&copy);
    VkMemoryBarrier2 mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,.srcStageMask=VK_PIPELINE_STAGE_2_COPY_BIT,.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT,.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT,.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di={.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.memoryBarrierCount=1,.pMemoryBarriers=&mb};vkCmdPipelineBarrier2(ex->cb,&di);vk_completion_add(ex->fence,buffer_done,b);ex->recorded+=3;
}
static void export_image(const CameraGpuInput *in,VkImage image,size_t bytes,const char *stem,const char *suffix){export_image_aspect(in,image,bytes,stem,suffix,VK_IMAGE_ASPECT_COLOR_BIT);}
void vk_camera_gpu_pass(const CameraGpuInput *in){
    initialize();VkExec *ex=in->ex;uint32_t w=in->width,h=in->height;
    if(!C.target||C.target->w!=w||C.target->h!=h){
        if(C.target)vk_completion_add(ex->fence,target_free,C.target);
        Target *t=calloc(1,sizeof *t);if(!t)rt_fatal("camera GPU: image allocation");t->w=w;t->h=h;
        VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R32G32B32A32_SFLOAT,.extent={w,h,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai={.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};check(vmaCreateImage(vk_ctx()->vma,&ii,&ai,&t->image,&t->alloc,NULL));
        VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=t->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R32G32B32A32_SFLOAT,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};check(vkCreateImageView(vk_ctx()->device,&vi,NULL,&t->view));
        VkImageUsageFlags output_usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        t->composite=image(w,h,VK_FORMAT_R32G32B32A32_SFLOAT,output_usage);t->bias=image(w,h,VK_FORMAT_R32_SFLOAT,output_usage);
        t->mv=image(w,h,VK_FORMAT_R32G32_SFLOAT,output_usage);
        t->previous_color=image(w,h,VK_FORMAT_B10G11R11_UFLOAT_PACK32,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);C.target=t;
    }
    Target *t=C.target;VkImageLayout old=t->initialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED;
    barrier(ex->cb,t->image,old);barrier(ex->cb,t->composite.image,old);barrier(ex->cb,t->bias.image,old);barrier(ex->cb,t->previous_color.image,old);barrier(ex->cb,t->mv.image,old);t->initialized=1;
    const VkExecTarget *source=in->color;
    int supported=source&&source->image&&source->format==VK_FORMAT_B10G11R11_UFLOAT_PACK32&&source->width==w&&source->height==h&&source->layers==1&&!source->level&&!source->base_layer&&source->samples<=1&&!source->is3d&&!source->blocks;
    int color_history=supported&&t->color_frame&&t->color_frame+1==in->frame;
    VkImageLayout source_layout=VK_IMAGE_LAYOUT_GENERAL;
    if(supported){for(int i=0;i<ex->nfbl;i++)if(ex->fbl_img[i]==source->image)source_layout=VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;barrier(ex->cb,source->image,source_layout);}
    VkImageView views[4]={t->view,t->composite.view,t->bias.view,t->mv.view};VkRenderingAttachmentInfo colors[4]={0};
    for(int i=0;i<4;i++)colors[i]=(VkRenderingAttachmentInfo){.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=views[i],.imageLayout=VK_IMAGE_LAYOUT_GENERAL,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE};
    colors[2].clearValue.color.float32[0]=1;
    VkRenderingInfo ri={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},{w,h}},.layerCount=1,.colorAttachmentCount=4,.pColorAttachments=colors};vkCmdBeginRendering(ex->cb,&ri);
    if(in->active){
        Buffer *u=buffer(80,1);memcpy(u->map,in->reprojection,64);float viewport[4]={(float)w,(float)h,(float)color_history,(float)in->color_test};memcpy((char*)u->map+64,viewport,16);vmaFlushAllocation(vk_ctx()->vma,u->alloc,0,VK_WHOLE_SIZE);
        VkImageView current_color=in->skin_motion;
        if(supported){VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=source->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=source->format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};check(vkCreateImageView(vk_ctx()->device,&vi,NULL,&u->view));current_color=u->view;}
        VkViewport vp={0,(float)h,(float)w,-(float)h,0,1};VkRect2D sc={{0,0},{w,h}};vkCmdSetViewportWithCount(ex->cb,1,&vp);vkCmdSetScissorWithCount(ex->cb,1,&sc);
        vkCmdBindPipeline(ex->cb,VK_PIPELINE_BIND_POINT_GRAPHICS,C.pipeline);
        VkDescriptorBufferInfo bi={u->buffer,0,80};VkDescriptorImageInfo images[5]={{C.sampler,in->current_depth,VK_IMAGE_LAYOUT_GENERAL},{C.sampler,in->previous_depth,VK_IMAGE_LAYOUT_GENERAL},
            {C.sampler,in->skin_motion,VK_IMAGE_LAYOUT_GENERAL},{C.sampler,current_color,VK_IMAGE_LAYOUT_GENERAL},{C.sampler,t->previous_color.view,VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[6]={{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&bi}};
        for(int i=1;i<6;i++)writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstBinding=(uint32_t)i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&images[i-1]};
        PFN_vkCmdPushDescriptorSetKHR push=(PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(vk_ctx()->device,"vkCmdPushDescriptorSetKHR");if(!push)rt_fatal("camera GPU: push unavailable");
        rec_push(push,ex->cb,VK_PIPELINE_BIND_POINT_GRAPHICS,C.layout,0,6,writes);vkCmdDraw(ex->cb,3,1,0,0);vk_completion_add(ex->fence,buffer_done,u);
    }
    vkCmdEndRendering(ex->cb);ex->recorded+=8;
    if(in->frame==in->export_frame&&supported)vk_jitter_probe_once(ex,in->frame,in->directory);
    dlaa_gpu_profile_mark(in->gpu_profile,ex,2);
    int sequence=ngx_probe_sequence_enabled();
    unsigned count=vk_projection_probe_sequence_enabled()?vk_projection_probe_frame_count():8;
    int live=vk_ngx_eval_live_enabled();
    int eval_window=live?in->frame>=1000:vk_projection_probe_sequence_enabled()?(in->export_frame>=count&&in->frame>=in->export_frame-count+1&&in->frame<=in->export_frame):sequence?(in->export_frame>=8&&in->frame+7>=in->export_frame&&in->frame<=in->export_frame+120):in->frame==in->export_frame;
    VkImage display_output=VK_NULL_HANDLE;VkNgxEvalInput eval={0};
    dlaa_gpu_profile_mark(in->gpu_profile,ex,3);
    if(eval_window&&in->active&&color_history&&supported&&!in->forced_invalid&&!in->color_test){
        eval=(VkNgxEvalInput){.ex=ex,.width=w,.height=h,.frame=in->frame,.jitter={in->current_jitter[0],in->current_jitter[1]},.directory=in->directory,
            .image={source->image,in->depth_image,t->mv.image,t->bias.image},
            .format={source->format,VK_FORMAT_D32_SFLOAT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R32_SFLOAT}};
        display_output=vk_ngx_eval_once(&eval);
    }
    dlaa_gpu_profile_mark(in->gpu_profile,ex,4);dlaa_gpu_profile_live(in->gpu_profile,display_output!=VK_NULL_HANDLE);
    if(vk_projection_probe_export_frame(in->frame,in->export_frame)){
        const char *replay=getenv("TOTK_DLAA_REPLAY_CAPTURE");
        if(replay&&*replay&&*replay!='0'){
            if(!vk_projection_probe_sequence_enabled())rt_fatal("replay depth: requires bounded raster/NGX sequence");
            export_image_aspect(in,in->depth_image,(size_t)w*h*4,"gpu-depth-current","d32f",VK_IMAGE_ASPECT_DEPTH_BIT);
        }
        metadata(in,color_history);export_image(in,t->image,(size_t)w*h*16,"gpu-camera","rgba32f");
        export_image(in,t->composite.image,(size_t)w*h*16,"gpu-composite","rgba32f");export_image(in,t->bias.image,(size_t)w*h*4,"gpu-uncertainty","r32f");
        export_image(in,t->mv.image,(size_t)w*h*8,"gpu-ngx-motion","rg32f");
        if(supported){export_image(in,source->image,(size_t)w*h*4,"gpu-hdr-current","r11g11b10");if(color_history)export_image(in,t->previous_color.image,(size_t)w*h*4,"gpu-hdr-previous","r11g11b10");}
        rt_log("camera GPU: frame %llu, active %d, camera support %d (static-world candidates)",(unsigned long long)in->frame,in->active,in->support);
    }
    if(supported){
        barrier(ex->cb,t->previous_color.image,VK_IMAGE_LAYOUT_GENERAL);
        VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={w,h,1}};
        vkCmdCopyImage(ex->cb,source->image,VK_IMAGE_LAYOUT_GENERAL,t->previous_color.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
        barrier(ex->cb,t->previous_color.image,VK_IMAGE_LAYOUT_GENERAL);transition(ex->cb,source->image,VK_IMAGE_LAYOUT_GENERAL,source_layout);t->color_frame=in->frame;
    }else t->color_frame=0;
    dlaa_gpu_profile_mark(in->gpu_profile,ex,5);
    // Save original color before replacing it; never feed resolved output into the raw-color guard.
    if(display_output){
        if(source_layout!=VK_IMAGE_LAYOUT_GENERAL)transition(ex->cb,source->image,source_layout,VK_IMAGE_LAYOUT_GENERAL);
        int presented=vk_ngx_eval_present(&eval,display_output);
        if(presented&&!live&&in->frame==in->export_frame)export_image(in,source->image,(size_t)w*h*4,"gpu-hdr-presented","r11g11b10");
        if(source_layout!=VK_IMAGE_LAYOUT_GENERAL)transition(ex->cb,source->image,VK_IMAGE_LAYOUT_GENERAL,source_layout);
    }
    dlaa_gpu_profile_mark(in->gpu_profile,ex,6);
    vk_projection_probe_live_ready(in->frame,in->active&&supported&&w==2560&&h==1440);
    ex->recorded+=8;
}
void vk_camera_gpu_shutdown(void){
    if(!C.initialized)return;VkDevice d=vk_ctx()->device;if(C.target)target_free(C.target);
    vkDestroyPipeline(d,C.pipeline,NULL);vkDestroyShaderModule(d,C.vs,NULL);vkDestroyShaderModule(d,C.fs,NULL);
    vkDestroySampler(d,C.sampler,NULL);vkDestroyPipelineLayout(d,C.layout,NULL);vkDestroyDescriptorSetLayout(d,C.set,NULL);memset(&C,0,sizeof C);
}
