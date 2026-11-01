// Private, opt-in diagnostic. Candidate geometry continuity is NOT game-object identity.
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "vk_motion_gpu.h"
#include "motion_history.h"
#include "camera_math.h"
#include "vk_camera_gpu.h"
#include "vk_projection_probe.h"
#include "jitter_math.h"
#include "dlaa_cpu_profile.h"
#include "motion_vs1_spv.h"
#include "motion_vs3_spv.h"
#include "motion_vs4_spv.h"
#include "motion_vs7_spv.h"
#include "motion_fs_spv.h"
#include "motion_gs_spv.h"
extern const void *vk_usnap_probe_read(uint64_t addr, uint64_t size);

typedef struct { float view[3][4], projection[4][4], origin[4], bones[384][4]; } Pose;
typedef struct { Pose now, old; float viewport[4]; } Uniform;
_Static_assert(sizeof(Uniform) == 12560, "std140 motion layout");
typedef struct {
    uint64_t shader, ibuf;
    NVNbufferRange vertex[16]; uint32_t attr[16]; vs_st stream[16];
    int influences, count, base, itype; uint32_t width, height;
} Key;
typedef struct { Key key; Pose pose; uint32_t bone_count; float raster_jitter[2]; } Packet;
typedef struct { VkImage image; VmaAllocation alloc; VkImageView view; } Image;
typedef struct { Image motion, z, previous; uint32_t w, h; int initialized; uint64_t depth_frame; } Targets;
typedef struct { Key key; VkPipeline pipeline; } Pipe;
typedef struct {
    int checked, enabled, initialized, scene, op, bounds_test, camera_reset_test, color_test, previous_pose_test; uint64_t export_frame;
    p_draw_idx args; Mh history; Packet packets[2][MH_CAP];
    Targets *targets; VkDescriptorSetLayout set; VkPipelineLayout layout; VkSampler sampler;
    VkShaderModule vs[8], fs, gs; Pipe pipes[64]; int npipes;
    FILE *log; char dir[400];
    int cpu_stage;uint8_t *cpu_uniforms;size_t cpu_uniform_capacity;
} Motion;
static Motion G;

static void check(VkResult r, const char *what) { if (r != VK_SUCCESS) rt_fatal("motion GPU: %s failed (%d)", what, r); }
static uint64_t frame_now(void) { return __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1; }
static int enabled(void) {
    if (!G.checked) {
        const char *e = getenv("TOTK_MOTION_GPU"); G.checked = 1; G.enabled = e && *e && *e != '0';
        const char *stage=getenv("TOTK_MOTION_CPU_STAGE");G.cpu_stage=stage&&*stage=='1';rt_log("motion CPU staging: %d",G.cpu_stage);
        const char *d = getenv("TOTK_DUMP_DIR"); snprintf(G.dir, sizeof G.dir, "%s", d && *d ? d : ".");
        const char *f = getenv("TOTK_DUMP"); G.export_frame = f ? strtoull(f, NULL, 10) : 0;
        const char *b = getenv("TOTK_GPU_TEST_INVALID_POSE"); G.bounds_test=b && *b && *b!='0';
        const char *c = getenv("TOTK_GPU_TEST_INVALID_CAMERA"); G.camera_reset_test=c && *c && *c!='0';
        const char *v = getenv("TOTK_GPU_TEST_COLOR_DISCONTINUITY"); G.color_test=v && *v && *v!='0';
        const char *p = getenv("TOTK_GPU_TEST_PREVIOUS_POSE"); G.previous_pose_test=p && *p && *p!='0';
    }
    return G.enabled;
}
static uint64_t hash_shader(const GfxShader *s) {
    static struct { const GfxShader *s; uint64_t hash; } cache[8192]; static int count;
    if (!s) return 0;
    for (int i = 0; i < count; i++) if (cache[i].s == s) return cache[i].hash;
    uint64_t n = 0, hash = 0xcbf29ce484222325ull; const uint8_t *p = (const uint8_t *)gfx_shader_spirv(s, &n);
    for (uint64_t i = 0; i < n * 4; i++) hash = (hash ^ p[i]) * 0x100000001b3ull;
    if (count < 8192) cache[count].s = s, cache[count++].hash = hash;
    return hash;
}
static int influences(uint64_t shader) {
    switch (shader) {
    case 0x50c84126ec0b9b5cull: case 0x4c8e0ace5e7a2f9bull: case 0x68e66bf9643995b0ull:
    case 0x2b6aff83dd577afbull: case 0xce008d795b8bc917ull: case 0xf3a125b0cfb0f65cull: return 1;
    case 0xcbe98abcb55b9281ull: case 0x747e61bf44f2647bull: case 0xac64fcf016e270ecull: return 3;
    case 0x78b1b402c3089a3full: case 0x1cdca31fde4af6aaull: case 0x7907a6ee3a1708b2ull:
    case 0x10e76c545a25b039ull: return 4;
    case 0x2f48a30d571fe91aull: return 7;
    default: return 0;
    }
}
static uint32_t locations(int n) { return (1u << 0) | (1u << 6) | (n > 1 ? 1u << 4 : 0) | (n == 7 ? (1u << 5) | (1u << 7) : 0); }
void vk_motion_gpu_op(uint32_t op, const void *payload) {
    if (!enabled()) return;
    if (op == OP_DRAW_IDX) G.op = op, G.args = *(const p_draw_idx *)payload;
    else if (op == OP_DRAW || op == OP_DRAW_IDX_IND) G.op = 0;
}
static void motion_gpu_draw_impl(VkExec *ex, const VkPipeState *s, const VkPipeInfo *info) {
    if (!enabled()) return;
    if (hash_shader(s->vs) == 0x82392b997f526831ull && hash_shader(s->fs) == 0xa139337db31fc946ull) G.scene = 1;
    if (G.op != OP_DRAW_IDX || ex->pass_w < 640 || ex->pass_h < 360 || ex->pass_samples > 1 || !ex->depth.image) return;
    if (ex->pass_w * 9 != ex->pass_h * 16 || G.args.prim != 4 || G.args.count <= 0 || G.args.itype < 1 || G.args.itype > 2 ||
        G.args.instances > 1 || G.args.instances < 0 || G.args.base_instance || ex->prim_restart.enable) return;
    int pose_used = 0;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        if (b->res && b->kind == GFX_RES_UBO && b->stage == GFX_STAGE_VS && b->res->slot == 5) pose_used = 1;
    }
    if (!pose_used || ex->ubo[0][1].size != 0x940) return;
    if (ex->has_vp && (ex->vp[0][0] != 0 || ex->vp[0][1] != 0 || ex->vp[0][2] != ex->pass_w || ex->vp[0][3] != ex->pass_h)) return;
    if (ex->depth_range[0] != 0 || ex->depth_range[1] != 1) return;
    uint64_t shader = hash_shader(s->vs); int count = influences(shader);
    const p_bind_buf *p = &ex->ubo[0][2], *camera = &ex->ubo[0][1], *origin = &ex->ubo[0][4];
    if (!count || !p->size || p->size > 6144 || p->size % 48 || camera->size < 176 || origin->size < 128) return;
    const float *a = vk_usnap_probe_read(camera->addr, 176), *b = vk_usnap_probe_read(p->addr, p->size), *c = vk_usnap_probe_read(origin->addr, 128);
    if (!a || !b || !c) return;
    Packet packet; memset(&packet, 0, sizeof packet);
    packet.bone_count=(uint32_t)(p->size/48);
    packet.key.shader = shader; packet.key.influences = count; packet.key.width = ex->pass_w; packet.key.height = ex->pass_h;
    packet.key.ibuf = G.args.ibuf; packet.key.count = G.args.count; packet.key.base = G.args.base_vertex; packet.key.itype = G.args.itype;
    uint32_t streams = 0, mask = locations(count);
    for (int k = 0; k < 16; k++) if (mask & (1u << k)) {
        va_st attr; memcpy(&attr, &s->vattr[k], sizeof attr);
        if (!attr.fmt || attr.stream >= 16 || s->vstream[attr.stream].divisor) return;
        int type; VkFormat f = vk_vertex_format(attr.fmt, &type);
        if (k == 0 ? (f != VK_FORMAT_R32G32B32_SFLOAT && f != VK_FORMAT_R16G16B16A16_SFLOAT) :
            (k == 6 || k == 7) ? (f != VK_FORMAT_R8_UINT && f != VK_FORMAT_R8G8B8A8_UINT) : (f != VK_FORMAT_R8G8B8A8_UNORM)) return;
        packet.key.attr[k] = s->vattr[k]; streams |= 1u << attr.stream;
    }
    for (int k = 0; k < 16; k++) if (streams & (1u << k)) {
        if (!ex->vbuf[k].address || !ex->vbuf[k].size) return;
        packet.key.vertex[k] = ex->vbuf[k]; packet.key.stream[k] = s->vstream[k];
    }
    memcpy(packet.pose.view, a, 48); memcpy(packet.pose.projection, a + 28, 64);
    memcpy(packet.pose.origin, c + 28, 16); memcpy(packet.pose.bones, b, (size_t)p->size);
    const float *f = (const float *)&packet.pose;
    for (size_t i = 0; i < sizeof(Pose) / 4; i++) if (!isfinite(f[i])) return;
    vk_projection_probe_camera_offset(ex,camera->addr,camera->size,packet.raster_jitter);
    uint64_t h1 = 0xcbf29ce484222325ull, h2 = 0x9e3779b97f4a7c15ull; const uint8_t *raw = (const uint8_t *)&packet.key;
    for (size_t i = 0; i < sizeof(Key); i++) h1 = (h1 ^ raw[i]) * 0x100000001b3ull, h2 = (h2 + raw[i]) * 0x9e3779b185ebca87ull;
    mh_begin(&G.history, frame_now()); int index = mh_add(&G.history, h1, h2);
    if (index >= 0) G.packets[G.history.side][index] = packet;
}

static VkShaderModule shader(const uint32_t *words, size_t bytes) {
    VkShaderModule result; VkShaderModuleCreateInfo info = {.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=bytes,.pCode=words};
    check(vkCreateShaderModule(vk_ctx()->device, &info, NULL, &result), "shader"); return result;
}
static void initialize(void) {
    if (G.initialized) return;
    VkDevice device = vk_ctx()->device;
    VkDescriptorSetLayoutBinding bindings[3] = {
        {.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding=2,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT}};
    VkDescriptorSetLayoutCreateInfo si = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags=VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,.bindingCount=3,.pBindings=bindings};
    check(vkCreateDescriptorSetLayout(device,&si,NULL,&G.set),"descriptor layout");
    VkPipelineLayoutCreateInfo li = {.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&G.set};
    check(vkCreatePipelineLayout(device,&li,NULL,&G.layout),"pipeline layout");
    VkSamplerCreateInfo sampler = {.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.maxLod=0};
    check(vkCreateSampler(device,&sampler,NULL,&G.sampler),"sampler");
    G.vs[1]=shader(motion_vs1,sizeof motion_vs1); G.vs[3]=shader(motion_vs3,sizeof motion_vs3);
    G.vs[4]=shader(motion_vs4,sizeof motion_vs4); G.vs[7]=shader(motion_vs7,sizeof motion_vs7); G.fs=shader(motion_fs,sizeof motion_fs);
    G.gs=shader(motion_gs,sizeof motion_gs);
    char path[600]; snprintf(path,sizeof path,"%s/gpu-motion-log.jsonl",G.dir); G.log=fopen(path,"w");
    G.initialized=1; rt_log("motion GPU: isolated diagnostic enabled; no DLSS input claim");
}
static Image make_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    Image image = {0}; VkCtx *ctx=vk_ctx();
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=format,.extent={w,h,1},
        .mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=usage,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai={.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    check(vmaCreateImage(ctx->vma,&ii,&ai,&image.image,&image.alloc,NULL),"image allocation");
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image.image,.viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=format,.subresourceRange={aspect,0,1,0,1}};
    check(vkCreateImageView(ctx->device,&vi,NULL,&image.view),"image view"); return image;
}
static void free_targets(void *arg) {
    Targets *t=arg; Image images[3]={t->motion,t->z,t->previous};
    for(int i=0;i<3;i++) { vkDestroyImageView(vk_ctx()->device,images[i].view,NULL); vmaDestroyImage(vk_ctx()->vma,images[i].image,images[i].alloc); }
    free(t);
}
static void barrier(VkCommandBuffer cb,VkImage image,VkImageAspectFlags aspect,VkImageLayout old,VkImageLayout next) {
    VkImageMemoryBarrier2 b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,.oldLayout=old,.newLayout=next,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={aspect,0,1,0,1}};
    VkDependencyInfo di={.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b}; vkCmdPipelineBarrier2(cb,&di);
}
static VkPipeline pipeline(const Key *key) {
    // Pipeline keys include only the vertex layout, not transient buffer addresses.
    Key layout={0}; layout.influences=key->influences;
    memcpy(layout.attr,key->attr,sizeof layout.attr); memcpy(layout.stream,key->stream,sizeof layout.stream);
    for(int i=0;i<G.npipes;i++) if(!memcmp(&G.pipes[i].key,&layout,sizeof layout)) return G.pipes[i].pipeline;
    if(G.npipes==64) return VK_NULL_HANDLE;
    VkVertexInputAttributeDescription attributes[16]; VkVertexInputBindingDescription bindings[16]; uint32_t na=0,nb=0,seen=0;
    uint32_t mask=locations(key->influences);
    for(int k=0;k<16;k++) if(mask&(1u<<k)) {
        va_st a; memcpy(&a,&key->attr[k],sizeof a); int type;
        attributes[na++]=(VkVertexInputAttributeDescription){k,a.stream,vk_vertex_format(a.fmt,&type),a.offset};
        if(!(seen&(1u<<a.stream))) { seen|=1u<<a.stream; bindings[nb++]=(VkVertexInputBindingDescription){a.stream,key->stream[a.stream].stride,VK_VERTEX_INPUT_RATE_VERTEX}; }
    }
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,.vertexBindingDescriptionCount=nb,
        .pVertexBindingDescriptions=bindings,.vertexAttributeDescriptionCount=na,.pVertexAttributeDescriptions=attributes};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,
        .cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable=VK_TRUE,.depthWriteEnable=VK_TRUE,.depthCompareOp=VK_COMPARE_OP_LESS};
    VkPipelineColorBlendAttachmentState attachment={.colorWriteMask=15};
    VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment};
    VkDynamicState dynamic[2]={VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT};
    VkPipelineDynamicStateCreateInfo dyn={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=dynamic};
    VkPipelineShaderStageCreateInfo stages[3]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=G.vs[key->influences],.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=G.fs,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_GEOMETRY_BIT,.module=G.gs,.pName="main"}};
    VkFormat format=VK_FORMAT_R32G32B32A32_SFLOAT;
    VkPipelineRenderingCreateInfo rendering={.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,.colorAttachmentCount=1,.pColorAttachmentFormats=&format,
        .depthAttachmentFormat=VK_FORMAT_D32_SFLOAT};
    VkGraphicsPipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=&rendering,.stageCount=3,.pStages=stages,
        .pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vp,.pRasterizationState=&rs,.pMultisampleState=&ms,
        .pDepthStencilState=&ds,.pColorBlendState=&blend,.pDynamicState=&dyn,.layout=G.layout};
    VkPipeline p; check(vkCreateGraphicsPipelines(vk_ctx()->device,VK_NULL_HANDLE,1,&pi,NULL,&p),"graphics pipeline");
    G.pipes[G.npipes++]=(Pipe){layout,p}; return p;
}

typedef struct { VkBuffer buffer; VmaAllocation alloc; VkImageView view; } Transient;
static void transient_done(void *arg) {
    Transient *t=arg;
    if(t->view) vkDestroyImageView(vk_ctx()->device,t->view,NULL);
    if(t->buffer) vmaDestroyBuffer(vk_ctx()->vma,t->buffer,t->alloc);
    free(t);
}
typedef struct { VkBuffer buffer; VmaAllocation alloc; const void *map; size_t bytes; char path[600]; } Export;
static void export_done(void *arg) {
    Export *e=arg; vmaInvalidateAllocation(vk_ctx()->vma,e->alloc,0,VK_WHOLE_SIZE);
    FILE *f=fopen(e->path,"wb"); if(!f || fwrite(e->map,1,e->bytes,f)!=e->bytes) rt_fatal("motion GPU: export failed");
    fclose(f); vmaDestroyBuffer(vk_ctx()->vma,e->buffer,e->alloc); free(e);
}
static void export_motion(VkExec *ex,Targets *t,uint64_t frame) {
    if(!vk_projection_probe_export_frame(frame,G.export_frame)) return;
    Export *e=calloc(1,sizeof *e); if(!e) rt_fatal("motion GPU: export allocation");
    e->bytes=(size_t)t->w*t->h*16;
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=e->bytes,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai={.usage=VMA_MEMORY_USAGE_AUTO,.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info; check(vmaCreateBuffer(vk_ctx()->vma,&bi,&ai,&e->buffer,&e->alloc,&info),"export buffer"); e->map=info.pMappedData;
    snprintf(e->path,sizeof e->path,"%s/gpu-motion-%llu-%ux%u.rgba32f",G.dir,(unsigned long long)frame,t->w,t->h);
    barrier(ex->cb,t->motion.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={t->w,t->h,1}};
    vkCmdCopyImageToBuffer(ex->cb,t->motion.image,VK_IMAGE_LAYOUT_GENERAL,e->buffer,1,&copy);
    VkMemoryBarrier2 mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,.srcStageMask=VK_PIPELINE_STAGE_2_COPY_BIT,.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT,.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di={.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.memoryBarrierCount=1,.pMemoryBarriers=&mb}; vkCmdPipelineBarrier2(ex->cb,&di);
    vk_completion_add(ex->fence,export_done,e); ex->recorded+=3;
}
void vk_motion_gpu_pass_end(VkExec *ex) {
    if(!enabled() || !G.scene) return;
    uint64_t cpu_begin=dc_start();static DlaaCpuProfile motion_total,allocation_phase,uniform_phase,draw_phase,camera_phase,upload_phase;uint64_t phase=dc_start();
    G.scene=0; uint64_t frame=frame_now(); mh_begin(&G.history,frame);
    const VkExecTarget *source=&ex->depth;
    if(!source->image || source->format!=VK_FORMAT_D32_SFLOAT || source->samples>1 || source->level || source->base_layer || source->layers!=1) {
        G.history.sealed=2; G.history.ready=0; return;
    }
    initialize(); uint32_t w=source->width,h=source->height;
    if(w<640 || h<360) return;
    DlaaGpuProfile *gpu_profile=dlaa_gpu_profile_begin(ex,frame);
    if(!G.targets || G.targets->w!=w || G.targets->h!=h) {
        if(G.targets) vk_completion_add(ex->fence,free_targets,G.targets);
        G.targets=calloc(1,sizeof *G.targets); if(!G.targets) rt_fatal("motion GPU: targets allocation");
        Targets *t=G.targets;t->w=w;t->h=h;
        t->motion=make_image(w,h,VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
        t->z=make_image(w,h,VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
        t->previous=make_image(w,h,VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
        G.history.ready=0;
    }
    Targets *t=G.targets; int history_ok=mh_seal(&G.history) && t->depth_frame+1==frame && t->depth_frame;
    VkImageLayout old=t->initialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED;
    barrier(ex->cb,t->motion.image,VK_IMAGE_ASPECT_COLOR_BIT,old,VK_IMAGE_LAYOUT_GENERAL);
    barrier(ex->cb,t->z.image,VK_IMAGE_ASPECT_DEPTH_BIT,old,VK_IMAGE_LAYOUT_GENERAL);
    barrier(ex->cb,t->previous.image,VK_IMAGE_ASPECT_DEPTH_BIT,old,VK_IMAGE_LAYOUT_GENERAL);t->initialized=1;
    VkImageLayout source_layout=VK_IMAGE_LAYOUT_GENERAL;
    for(int i=0;i<ex->nfbl;i++)if(ex->fbl_img[i]==source->image)source_layout=VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
    barrier(ex->cb,source->image,VK_IMAGE_ASPECT_DEPTH_BIT,source_layout,VK_IMAGE_LAYOUT_GENERAL);
    Transient *transient=calloc(1,sizeof *transient); if(!transient) rt_fatal("motion GPU: transient allocation");
    VkImageViewCreateInfo svi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=source->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=VK_FORMAT_D32_SFLOAT,.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1}};
    check(vkCreateImageView(vk_ctx()->device,&svi,NULL,&transient->view),"scene depth view");
    size_t align=(size_t)vk_ctx()->props.limits.minUniformBufferOffsetAlignment;if(!align)align=1;
    size_t stride=(sizeof(Uniform)+align-1)&~(align-1);int n=G.history.n[G.history.side];
    VmaAllocationInfo upload={0};
    if(n) {
        VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=stride*n,.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
        VmaAllocationCreateInfo ai={.usage=VMA_MEMORY_USAGE_AUTO,.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT};
        check(vmaCreateBuffer(vk_ctx()->vma,&bi,&ai,&transient->buffer,&transient->alloc,&upload),"transform upload");
    }
    if(n&&G.cpu_stage&&stride*(size_t)n>G.cpu_uniform_capacity){
        size_t bytes=stride*(size_t)n;uint8_t *next=realloc(G.cpu_uniforms,bytes);
        if(!next)rt_fatal("motion CPU uniform staging allocation failed");
        memset(next+G.cpu_uniform_capacity,0,bytes-G.cpu_uniform_capacity);G.cpu_uniforms=next;G.cpu_uniform_capacity=bytes;
    }
    dc_end(&allocation_phase,"motion allocation",phase,frame,1);phase=dc_start();
    int previous[MH_CAP]; memset(previous,-1,sizeof previous);int eligible=0,duplicates=0,camera_conflict=0;
    CameraGpuInput camera_input={.gpu_profile=gpu_profile,.ex=ex,.current_depth=transient->view,.previous_depth=t->previous.view,.skin_motion=t->motion.view,.depth_image=source->image,.color=&ex->color[0],
        .width=w,.height=h,.frame=frame,.export_frame=G.export_frame,.directory=G.dir};
    int coordinate_projection=vk_projection_probe_motion_enabled();
    const Pose *camera_now=NULL,*camera_old=NULL,*raw_now=NULL,*raw_old=NULL;const Packet *camera_packet=NULL,*previous_packet=NULL;
    for(int i=0;i<n;i++) {
        Packet *now=&G.packets[G.history.side][i]; int j=history_ok?mh_previous(&G.history,i):-1;
        if(G.history.slot[G.history.side][i].duplicate)duplicates++;
        if(now->key.width!=w || now->key.height!=h)continue;
        // A current surface with missing/ambiguous history must block camera fallback.
        Uniform *u=(Uniform *)((G.cpu_stage?G.cpu_uniforms:(uint8_t *)upload.pMappedData)+stride*i);u->now=u->old=now->pose;
        if(coordinate_projection){if(!jm_offset((float*)u->now.projection,(float*)now->pose.projection,now->raster_jitter[0],now->raster_jitter[1],w,h))rt_fatal("projection motion: invalid current raster projection");u->old=u->now;}
        u->viewport[0]=(float)w;u->viewport[1]=(float)h;u->viewport[2]=(float)now->bone_count;u->viewport[3]=0;
        previous[i]=-2; // coverage only, no usable old pose
        if(G.bounds_test && frame+1==G.export_frame)u->viewport[2]=0;
        if(j<0)continue;
        Packet *before=&G.packets[G.history.side^1][j];
        if(memcmp(&now->key,&before->key,sizeof(Key)) || now->bone_count!=before->bone_count)continue;
        float trace=0,delta=0;
        for(int a=0;a<3;a++)for(int b=0;b<3;b++)trace+=now->pose.view[a][b]*before->pose.view[a][b];
        for(int k=0;k<3;k++){float d=now->pose.origin[k]-before->pose.origin[k];delta+=d*d;}
        if(trace<2.879385f || delta>25 || mh_camera_distance2(now->pose.view,before->pose.view)>25)continue;
        u->now=now->pose;u->old=before->pose;
        if(coordinate_projection){
            if(!jm_offset((float*)u->now.projection,(float*)now->pose.projection,now->raster_jitter[0],now->raster_jitter[1],w,h)||
               !jm_offset((float*)u->old.projection,(float*)before->pose.projection,before->raster_jitter[0],before->raster_jitter[1],w,h))rt_fatal("projection motion: invalid raster history projection");
        }
        u->viewport[0]=(float)w;u->viewport[1]=(float)h;
        u->viewport[2]=(float)now->bone_count;u->viewport[3]=(float)before->bone_count;
        if(G.bounds_test && frame+1==G.export_frame)u->viewport[2]=u->viewport[3]=0;
        if(G.previous_pose_test && frame+1==G.export_frame)u->viewport[3]=0;
        previous[i]=j;eligible++;
        if(!camera_now){camera_now=&u->now;camera_old=&u->old;raw_now=&now->pose;raw_old=&before->pose;camera_packet=now;previous_packet=before;}
        else if(!cm_agrees((const float*)camera_now->view,(const float*)u->now.view,12)||
            !cm_agrees((const float*)camera_now->projection,(const float*)u->now.projection,16)||
            !cm_agrees((const float*)camera_old->view,(const float*)u->old.view,12)||
            !cm_agrees((const float*)camera_old->projection,(const float*)u->old.projection,16)||
            !cm_agrees(camera_packet->raster_jitter,now->raster_jitter,2)||!cm_agrees(previous_packet->raster_jitter,before->raster_jitter,2))camera_conflict=1;
        camera_input.support++;
    }
    if(camera_now&&!camera_conflict){
        camera_input.active=cm_reprojection(camera_input.reprojection,(const float*)camera_now->view,(const float*)camera_now->projection,(const float*)camera_old->view,(const float*)camera_old->projection);
        if(camera_input.active){cm_vp(camera_input.current_vp,(const float*)camera_now->view,(const float*)camera_now->projection);cm_vp(camera_input.previous_vp,(const float*)camera_old->view,(const float*)camera_old->projection);}
    }
    if(camera_input.active){
        cm_vp(camera_input.raw_current_vp,(const float*)raw_now->view,(const float*)raw_now->projection);
        cm_vp(camera_input.raw_previous_vp,(const float*)raw_old->view,(const float*)raw_old->projection);
        memcpy(camera_input.current_jitter,camera_packet->raster_jitter,8);memcpy(camera_input.previous_jitter,previous_packet->raster_jitter,8);
        camera_input.motion_projection_coordinated=coordinate_projection;
    }
    // CPU math/consensus must not read back through a write-combined/device-local upload mapping.
    // Copy the finished immutable uniform bytes once; GPU descriptors still reference the fenced upload buffer.
    dc_end(&uniform_phase,"motion uniform math",phase,frame,1);phase=dc_start();
    if(n&&G.cpu_stage)memcpy(upload.pMappedData,G.cpu_uniforms,stride*(size_t)n);
    if(n) vmaFlushAllocation(vk_ctx()->vma,transient->alloc,0,VK_WHOLE_SIZE);
    dc_end(&upload_phase,"motion uniform upload",phase,frame,1);phase=dc_start();
    VkRenderingAttachmentInfo color={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=t->motion.view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.clearValue={.color={{0,0,0,0}}}};
    VkRenderingAttachmentInfo depth={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=t->z.view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.clearValue={.depthStencil={1,0}}};
    VkRenderingInfo ri={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},{w,h}},.layerCount=1,
        .colorAttachmentCount=1,.pColorAttachments=&color,.pDepthAttachment=&depth};
    vkCmdBeginRendering(ex->cb,&ri); VkViewport viewport={0,(float)h,(float)w,-(float)h,0,1};VkRect2D scissor={{0,0},{w,h}};
    vkCmdSetViewportWithCount(ex->cb,1,&viewport);vkCmdSetScissorWithCount(ex->cb,1,&scissor);int drawn=0;
    PFN_vkCmdPushDescriptorSetKHR push=(PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(vk_ctx()->device,"vkCmdPushDescriptorSetKHR");
    for(int i=0;i<n;i++) if(previous[i]!=-1) {
        Packet *packet=&G.packets[G.history.side][i];Key *key=&packet->key;VkMemSpan index;
        if(!vk_mem_resolve(key->ibuf,(uint64_t)key->count*(1u<<key->itype),&index))continue;
        VK_MEM_PICK(index);VkBuffer vb[16];VkDeviceSize offsets[16],sizes[16];int bound=1;
        for(int k=0;k<16;k++)if(key->vertex[k].address) {
            VkMemSpan span;if(!vk_mem_resolve(key->vertex[k].address,key->vertex[k].size,&span)){bound=0;break;}
            VK_MEM_PICK(span);vb[k]=span.buffer;offsets[k]=span.offset;sizes[k]=key->vertex[k].size;
        }
        if(!bound)continue;VkPipeline p=pipeline(key);if(!p)continue;
        vkCmdBindPipeline(ex->cb,VK_PIPELINE_BIND_POINT_GRAPHICS,p);
        for(int k=0;k<16;k++)if(key->vertex[k].address)vkCmdBindVertexBuffers2(ex->cb,k,1,&vb[k],&offsets[k],&sizes[k],NULL);
        vkCmdBindIndexBuffer(ex->cb,index.buffer,index.offset,key->itype==1?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32);
        VkDescriptorBufferInfo buffer={transient->buffer,stride*i,sizeof(Uniform)};
        VkDescriptorImageInfo image[2]={{G.sampler,transient->view,VK_IMAGE_LAYOUT_GENERAL},{G.sampler,t->previous.view,VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[3]={
            {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&buffer},
            {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&image[0]},
            {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstBinding=2,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&image[1]}};
        if(!push)rt_fatal("motion GPU: push descriptors unavailable");
        rec_push(push,ex->cb,VK_PIPELINE_BIND_POINT_GRAPHICS,G.layout,0,3,writes);
        vkCmdDrawIndexed(ex->cb,(uint32_t)key->count,1,0,key->base,0);drawn++;ex->recorded+=5;
    }
    vkCmdEndRendering(ex->cb);
    barrier(ex->cb,t->motion.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
    export_motion(ex,t,frame);
    dc_end(&draw_phase,"motion draw recording",phase,frame,1);phase=dc_start();
    dlaa_gpu_profile_mark(gpu_profile,ex,1);
    camera_input.color_test=G.color_test&&frame==G.export_frame;
    if(G.camera_reset_test&&frame+1==G.export_frame){camera_input.active=0;camera_input.forced_invalid=1;}
    vk_camera_gpu_pass(&camera_input);
    dc_end(&camera_phase,"camera pass including NGX",phase,frame,1);
    barrier(ex->cb,t->previous.image,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
    // Fragment reads precede overwrite of the previous-depth history, including across submissions.
    barrier(ex->cb,source->image,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
    VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1},.dstSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1},.extent={w,h,1}};
    vkCmdCopyImage(ex->cb,source->image,VK_IMAGE_LAYOUT_GENERAL,t->previous.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
    barrier(ex->cb,t->previous.image,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
    if(source_layout!=VK_IMAGE_LAYOUT_GENERAL)barrier(ex->cb,source->image,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_LAYOUT_GENERAL,source_layout);
    dlaa_gpu_profile_end(gpu_profile,ex);
    t->depth_frame=frame; vk_completion_add(ex->fence,transient_done,transient);
    vk_exec_draw_new_cb();ex->dirty|=VK_EXEC_DIRTY_ALL;ex->recorded+=12;
    if(G.log)fprintf(G.log,"{\"frame\":%llu,\"width\":%u,\"height\":%u,\"collected\":%d,\"eligible\":%d,\"drawn\":%d,\"duplicates\":%d,\"history\":%d,\"invalid_pose_test\":%d}\n",
        (unsigned long long)frame,w,h,n,eligible,drawn,duplicates,history_ok,G.bounds_test && frame+1==G.export_frame);
    if(G.log && (frame==G.export_frame || frame+1==G.export_frame))fflush(G.log);
    dc_end(&motion_total,"motion pass including NGX",cpu_begin,frame,1);
    if(frame%120==0 || frame==G.export_frame || frame+1==G.export_frame)rt_log("motion GPU: frame %llu, collected %d, eligible %d, drawn %d, duplicates %d",
        (unsigned long long)frame,n,eligible,drawn,duplicates);
}
void vk_motion_gpu_shutdown(void) {
    vk_camera_gpu_shutdown();
    if(!G.initialized)return;VkDevice device=vk_ctx()->device;
    if(G.targets)free_targets(G.targets);
    for(int i=0;i<G.npipes;i++)vkDestroyPipeline(device,G.pipes[i].pipeline,NULL);
    for(int i=0;i<8;i++)if(G.vs[i])vkDestroyShaderModule(device,G.vs[i],NULL);
    vkDestroyShaderModule(device,G.fs,NULL);vkDestroySampler(device,G.sampler,NULL);
    vkDestroyShaderModule(device,G.gs,NULL);
    vkDestroyPipelineLayout(device,G.layout,NULL);vkDestroyDescriptorSetLayout(device,G.set,NULL);
    if(G.log)fclose(G.log);free(G.cpu_uniforms);memset(&G,0,sizeof G);
}

void vk_motion_gpu_draw(VkExec *ex,const VkPipeState *s,const VkPipeInfo *info){
    static unsigned sample;static DlaaCpuProfile profile;
    if(!dc_enabled()||(++sample&63)){motion_gpu_draw_impl(ex,s,info);return;}
    uint64_t begin=dc_start();motion_gpu_draw_impl(ex,s,info);dc_end(&profile,"motion draw collection",begin,frame_now(),64);
}
