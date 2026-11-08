// The blueprint preview rendered on the GPU: an offscreen colour and depth target, one draw call per part, and the
// finished image handed to ImGui as a texture.
//
// Why this exists: the preview used to be drawn as thousands of filled 2D rectangles through ImGui's draw list. That has
// no depth buffer - so faces had to be sorted on the CPU and interpenetrating parts showed through each other - and the
// whole list was rebuilt and re-submitted every frame. The reference viewer renders with three.js on the GPU, which is
// why it costs almost nothing: the GPU resolves depth in hardware and the CPU does a handful of draw calls.
//
// No allocation happens while a frame is being recorded. GPU memory is not thread safe to allocate from, and the game
// allocates from the same pools on other threads; doing it here corrupted memory and the driver faulted. Two large
// host-visible buffers are made once at initialisation instead, and a part's geometry is copied into them by bumping an
// offset. That also keeps a frame's CPU work to a few memcpys and a draw per part.
//
// The shading matches what the reference does: flat, two-sided, one material, ambient plus a camera-attached key light
// (see runtime/shaders/preview_fs.frag), and the per-actor palette colour as the albedo.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>
#include "vk_mem_alloc.h"

#include "third_party/imgui/backends/imgui_impl_vulkan.h"

// vk.h cannot be included from here: its command-recording macros (vk_rec.h) and cpu.h exist for the replay and are
// C only. vk_device.c hands over the two objects this file needs.
extern "C" void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern "C" VkDevice vk_device(void);
extern "C" VmaAllocator vk_vma(void);
extern "C" void vk_queue_lock(void);
extern "C" void vk_queue_unlock(void);

// The runtime's headers are C and carry no extern "C" guards of their own.
extern "C" {
#include "bfres.h"
#include "model_romfs.h"
#include "blueprint_cai.h"
#include "blueprint_shape.h"
}

// runtime/blueprint_picker.c: the blueprint on show. Its type, BpcBlueprint, comes from blueprint_cai.h, so the
// declaration lives after the includes.
extern "C" const BpcBlueprint *bp_picker_blueprint(void);

#include "preview3d.h"
#include "preview_fs_spv.h"
#include "preview_vs_spv.h"

namespace {

// One part's geometry, as offsets into the two big buffers.
struct GpuPart {
    char name[64];
    uint32_t vtx_bytes;     // byte offset into the vertex buffer
    uint32_t idx_bytes;     // byte offset into the index buffer
    uint32_t count;         // indices to draw
};

constexpr int kMaxParts = 2048;
// The offscreen target is one fixed square, made once. The panel's size only picks a viewport inside it: an image that is
// recreated on every size change would have to be waited for and freed mid-frame, which is what crashed the driver.
constexpr uint32_t kImageSize = 1024;
// Room for the whole catalogue's geometry: measured at 16 MB of positions and 4 MB of indices for 870 parts.
constexpr uint32_t kVtxCapacity = 48u << 20;
constexpr uint32_t kIdxCapacity = 12u << 20;

struct State {
    int tried, ok;
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator vma = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    VkImage color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE;
    VmaAllocation color_alloc = nullptr, depth_alloc = nullptr;
    VkImageView color_view = VK_NULL_HANDLE, depth_view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkDescriptorSet tex = VK_NULL_HANDLE;

    // The two big buffers, host visible and mapped for their whole life.
    VkBuffer vtx_buf = VK_NULL_HANDLE, idx_buf = VK_NULL_HANDLE;
    VmaAllocation vtx_alloc = nullptr, idx_alloc = nullptr;
    uint8_t *vtx_map = nullptr, *idx_map = nullptr;
    uint32_t vtx_used = 0, idx_used = 0;

    GpuPart parts[kMaxParts];
    int part_count = 0;
    uint64_t frame = 0;
    int dirty = 1;
    float yaw = 0.610865f, pitch = 0.488692f, zoom = 1.0f;
    int first_use = 1;                       // the colour image is UNDEFINED until the first record
    uint32_t want_w = 512, want_h = 512;     // the panel's size, clamped to the fixed target
};

State S;

// Push constant block: the camera-and-placement matrix plus the part's albedo. 80 bytes.
struct Push {
    float mvp[16];
    float color[4];
};

// The palette the game's own viewer uses for a draft's parts, by the actor's place in the list.
const unsigned kPalette[21] = {
    0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF, 0xFFA500, 0x800080, 0xFFC0CB, 0x00FF7F,
    0xFFD700, 0x008080, 0x9400D3, 0x00CED1, 0xFF4500, 0x00FF99, 0xFF1493, 0xFF6347, 0x87CEEB, 0xFF69B4,
    0x32CD32,
};

void palette_rgb(int index, float out[4]) {
    const unsigned c = kPalette[index % 21];
    out[0] = ((c >> 16) & 0xFF) / 255.0f;
    out[1] = ((c >> 8) & 0xFF) / 255.0f;
    out[2] = (c & 0xFF) / 255.0f;
    out[3] = 1.0f;
}

// Column-major matrices, as Vulkan's shaders expect them: out = a * b.
void mat_mul(const float a[16], const float b[16], float out[16]) {
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
}

void mat_identity(float m[16]) {
    memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// The camera. The picker's CPU projection treats the blueprint as Y-up (yaw spins around Y, pitch tilts around X), so
// this agrees: the eye sits on a sphere around the build's centre, up is +Y, and it looks at the centre. A standard
// right-handed look-at followed by a perspective that maps view-space -Z into Vulkan's 0..1 depth gives the depth buffer
// a real value per fragment, which is what hides a part behind another. The earlier matrix wrote clip.z as a constant,
// so nothing was ever hidden.
void camera_matrix(const float lo[3], const float hi[3], float yaw, float pitch, float zoom, float out[16]) {
    float centre[3], half = 0;
    for (int i = 0; i < 3; i++) {
        centre[i] = 0.5f * (lo[i] + hi[i]);
        const float span = hi[i] - lo[i];
        if (span > half) half = span;
    }
    half = half > 0.0005f ? half * 0.5f : 0.0005f;
    const float inv = 1.0f / half;

    const float d = 4.0f;
    const float cp = cosf(pitch), sp = sinf(pitch);

    // Two transforms, kept separate so their units never mix. First the model matrix t: translate the build's centre to
    // the origin and scale its largest side to 2 (range -1..1). Then, in that normalised space, the eye sits on a sphere
    // of radius d around the origin looking at it - so the camera distance is exactly d in normalised units whatever the
    // build's real size, and yaw/pitch orbit the build.
    const float t[16] = {
        inv, 0, 0, 0,
        0, inv, 0, 0,
        0, 0, inv, 0,
        -centre[0] * inv, -centre[1] * inv, -centre[2] * inv, 1,
    };
    // Eye on a sphere of radius d, at yaw around +Y then pitch up. yaw = 0 puts the eye at -Z looking toward +Z (the
    // standard right-handed camera the perspective below assumes: view space looks down -Z, so the eye must start on the
    // -Z side for the build to be in front of it).
    const float eye[3] = { d * cp * sinf(yaw), d * sp, -d * cp * cosf(yaw) };

    // Forward = -eye (look at the origin), already unit length because |eye| = d.
    float f[3] = {-eye[0], -eye[1], -eye[2]};
    const float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    f[0] /= fl; f[1] /= fl; f[2] /= fl;
    // The camera's right is horizontal and follows yaw directly: (cos yaw, 0, sin yaw). It stays unit length and exactly
    // orthogonal to f at every pitch, including past the poles, where the old "up x forward" cross product shrank to zero
    // and the normalised right turned into garbage - the instant up-down flip mid-drag. up2 = forward x right finishes the
    // frame.
    float r[3] = {cosf(yaw), 0.0f, sinf(yaw)};
    float u2[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};

    // view (column-major). A right-handed camera looks down view-space -Z, so the view's third column is -forward (the
    // build lies in front of the eye, at negative view-space z). Translation is -dot(axis, eye) for the right/up rows
    // and +dot(forward, eye) for the -forward row.
    const float view[16] = {
        r[0], u2[0], -f[0], 0,
        r[1], u2[1], -f[1], 0,
        r[2], u2[2], -f[2], 0,
        -(r[0] * eye[0] + r[1] * eye[1] + r[2] * eye[2]),
        -(u2[0] * eye[0] + u2[1] * eye[1] + u2[2] * eye[2]),
        (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]),
        1,
    };
    float v[16];
    mat_mul(view, t, v);

    // Perspective, right-handed, Vulkan [0,1] depth. The build is at radius ~1.7 from the eye at distance d, so near is
    // well inside the build and far is past it; both are generous so no part is clipped at any orbit angle.
    const float n = 0.5f, f2 = d + 3.0f;
    const float s = 0.36f * zoom;
    const float aspect = (float)S.want_w / (float)(S.want_h ? S.want_h : 1);
    const float proj[16] = {
        s / (aspect > 0.01f ? aspect : 1.0f), 0, 0, 0,
        0, s, 0, 0,
        0, 0, f2 / (n - f2), -1.0f,
        0, 0, f2 * n / (n - f2), 0,
    };
    mat_mul(proj, v, out);
}


// The matrix that places one part: rotate by its CAI rotation (row-major 3x3), scale by fuseInfo, then translate.
void part_matrix(const BpcBlueprint *bp, int k, float out[16]) {
    mat_identity(out);
    const float s = bp->scale[k];
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++) out[c * 4 + r] = bp->rot[k][r][c] * s;
    out[12] = bp->pos[k][0];
    out[13] = bp->pos[k][1];
    out[14] = bp->pos[k][2];
}

VkImageView make_view(VkImage img, VkFormat fmt, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image = img;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = fmt;
    ci.subresourceRange = {aspect, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(S.device, &ci, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    return v;
}

int make_targets(uint32_t w, uint32_t h) {
    for (int i = 0; i < 2; i++) {
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.extent = {w, h, 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        VkImage img = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        if (i == 0) {
            ci.format = S.color_format;
            ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            if (vmaCreateImage(S.vma, &ci, &ai, &img, &alloc, NULL) != VK_SUCCESS) return 0;
            S.color = img;
            S.color_alloc = alloc;
            if (!(S.color_view = make_view(img, S.color_format, VK_IMAGE_ASPECT_COLOR_BIT))) return 0;
        } else {
            ci.format = S.depth_format;
            ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            if (vmaCreateImage(S.vma, &ci, &ai, &img, &alloc, NULL) != VK_SUCCESS) return 0;
            S.depth = img;
            S.depth_alloc = alloc;
            if (!(S.depth_view = make_view(img, S.depth_format, VK_IMAGE_ASPECT_DEPTH_BIT))) return 0;
        }
    }
    // The image is sampled by ImGui, so it is handed over in the layout that descriptor expects.
    S.tex = ImGui_ImplVulkan_AddTexture(S.sampler, S.color_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    S.extent = {w, h};
    return S.tex != VK_NULL_HANDLE;
}

// The two big buffers, made once. Host visible and mapped: writes are memcpys and no allocator is touched afterwards.
int make_streams(void) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};

    bi.size = kVtxCapacity;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (vmaCreateBuffer(S.vma, &bi, &ai, &S.vtx_buf, &S.vtx_alloc, &info) != VK_SUCCESS) return 0;
    S.vtx_map = (uint8_t *)info.pMappedData;

    bi.size = kIdxCapacity;
    bi.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (vmaCreateBuffer(S.vma, &bi, &ai, &S.idx_buf, &S.idx_alloc, &info) != VK_SUCCESS) return 0;
    S.idx_map = (uint8_t *)info.pMappedData;
    return S.vtx_map && S.idx_map;
}

// Copies one part's geometry into the big buffers. No allocation, so this is safe during a frame.
int upload_part(const char *name) {
    for (int i = 0; i < S.part_count; i++)
        if (!strcmp(S.parts[i].name, name)) return i;
    if (S.part_count >= kMaxParts) return -1;

    // The runtime model source first: read the .bfres.mc from the RomFS and decode it, then upload each shape's vertices
    // and indices with its bone transform already applied (bfres_shape_geometry). Fall back to the offline pack and the
    // collision shapes only when the runtime source is unavailable.
    const BfresModel *model = model_romfs_resolve(name);
    if (model) {
        int total_verts = 0, total_tris = 0;
        for (int s = 0; s < model->count; s++) {
            total_verts += bfres_shape_vertices(model, s);
            total_tris += bfres_shape_triangles(model, s);
        }
        if (total_verts < 3 || total_tris < 1) return -1;
        const uint32_t vtx_bytes = (uint32_t)total_verts * 12u;
        const uint32_t idx_bytes = (uint32_t)total_tris * 6u;
        if (S.vtx_used + vtx_bytes > kVtxCapacity || S.idx_used + idx_bytes > kIdxCapacity) return -1;

        GpuPart &p = S.parts[S.part_count];
        snprintf(p.name, sizeof p.name, "%s", name);
        p.vtx_bytes = S.vtx_used;
        p.idx_bytes = S.idx_used;
        p.count = (uint32_t)total_tris * 3u;

        int vbase = 0, tbase = 0;
        for (int s = 0; s < model->count; s++) {
            const int nverts = bfres_shape_vertices(model, s);
            const int ntris = bfres_shape_triangles(model, s);
            if (nverts <= 0 || ntris <= 0) continue;
            float *verts = (float *)alloca((size_t)nverts * 3 * sizeof(float));
            uint16_t *tris = (uint16_t *)alloca((size_t)ntris * 3 * sizeof(uint16_t));
            const int vc = bfres_shape_geometry(model, s, verts, nverts, tris, ntris);
            if (vc <= 0) continue;
            if (S.frame < 3)   // diagnostic: the first vertices, to see whether the decode/bone produced sane numbers
                rt_log("preview3d: %s shape%d v0=(%.2f,%.2f,%.2f) v1=(%.2f,%.2f,%.2f) tris0=(%u,%u,%u)",
                       name, s, verts[0], verts[1], verts[2], verts[3], verts[4], verts[5], tris[0], tris[1], tris[2]);
            memcpy(S.vtx_map + S.vtx_used + (size_t)vbase * 12u, verts, (size_t)vc * 12u);
            for (int t = 0; t < ntris; t++) {
                uint16_t idx[3] = {(uint16_t)(tris[t * 3] + vbase), (uint16_t)(tris[t * 3 + 1] + vbase),
                                   (uint16_t)(tris[t * 3 + 2] + vbase)};
                memcpy(S.idx_map + S.idx_used + (size_t)(tbase + t) * 6u, idx, 6u);
            }
            vbase += vc;
            tbase += ntris;
        }
        S.vtx_used += (uint32_t)vbase * 12u;
        S.idx_used += (uint32_t)tbase * 6u;
        p.count = (uint32_t)tbase * 3u;
        // The mapped memory may be non-coherent, so the CPU writes must be flushed before the GPU reads them.
        vmaFlushAllocation(S.vma, S.vtx_alloc, p.vtx_bytes, (uint32_t)vbase * 12u);
        vmaFlushAllocation(S.vma, S.idx_alloc, p.idx_bytes, (uint32_t)tbase * 6u);
        return S.part_count++;
    }

    // No model for this part: draw its collision body instead, the same fallback the reference viewer uses for the parts
    // it has no model for. It arrives in the same (positions, uint16 indices) layout, so it uploads the same way.
    const BpShapeMesh *shape = bp_shape_resolve(name);
    const float *positions = shape ? shape->verts : nullptr;
    const uint16_t *indices = shape ? shape->tris : nullptr;
    const int nverts = shape ? shape->nverts : 0;
    const int ntris = shape ? shape->ntris : 0;
    if (!positions || !indices || nverts < 3 || ntris < 1) return -1;

    const uint32_t vtx_bytes = (uint32_t)nverts * 12u;
    const uint32_t idx_bytes = (uint32_t)ntris * 6u;
    if (S.vtx_used + vtx_bytes > kVtxCapacity || S.idx_used + idx_bytes > kIdxCapacity) return -1;

    GpuPart &p = S.parts[S.part_count];
    snprintf(p.name, sizeof p.name, "%s", name);
    p.vtx_bytes = S.vtx_used;
    p.idx_bytes = S.idx_used;
    p.count = (uint32_t)ntris * 3u;
    memcpy(S.vtx_map + S.vtx_used, positions, vtx_bytes);
    memcpy(S.idx_map + S.idx_used, indices, idx_bytes);
    // The mapped memory may be host-visible but not host-coherent (device-local with resizable BAR), so the CPU writes
    // must be flushed before the GPU reads them.
    vmaFlushAllocation(S.vma, S.vtx_alloc, S.vtx_used, vtx_bytes);
    vmaFlushAllocation(S.vma, S.idx_alloc, S.idx_used, idx_bytes);
    S.vtx_used += vtx_bytes;
    S.idx_used += idx_bytes;
    return S.part_count++;
}

int init_pipeline(void) {
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(S.device, &li, NULL, &S.layout) != VK_SUCCESS) return 0;

    VkShaderModuleCreateInfo mi{};
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = sizeof preview_vs_spv;
    mi.pCode = preview_vs_spv;
    VkShaderModule vs = VK_NULL_HANDLE;
    if (vkCreateShaderModule(S.device, &mi, NULL, &vs) != VK_SUCCESS) return 0;
    mi.codeSize = sizeof preview_fs_spv;
    mi.pCode = preview_fs_spv;
    VkShaderModule fs = VK_NULL_HANDLE;
    if (vkCreateShaderModule(S.device, &mi, NULL, &fs) != VK_SUCCESS) return 0;

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bind{0, 12, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions = &attr;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;   // the reference draws both sides: several parts are single-sided sheets
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState ba{};
    ba.blendEnable = VK_FALSE;
    ba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo rr{};
    rr.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rr.colorAttachmentCount = 1;
    rr.pColorAttachmentFormats = &S.color_format;
    rr.depthAttachmentFormat = S.depth_format;

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext = &rr;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = S.layout;
    const VkResult r = vkCreateGraphicsPipelines(S.device, VK_NULL_HANDLE, 1, &ci, NULL, &S.pipe);
    vkDestroyShaderModule(S.device, vs, NULL);
    vkDestroyShaderModule(S.device, fs, NULL);
    return r == VK_SUCCESS;
}

void barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
             VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

}  // namespace

extern "C" int preview3d_init(void) {
    if (S.tried) return S.ok;
    S.tried = 1;
    S.device = vk_device();
    S.vma = vk_vma();
    if (S.device == VK_NULL_HANDLE || S.vma == nullptr) return 0;

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0;
    if (vkCreateSampler(S.device, &si, NULL, &S.sampler) != VK_SUCCESS) return 0;
    if (!init_pipeline()) return 0;
    vk_queue_lock();   // everything the allocator does here happens with no frame in flight and no other allocator user
    const int targets = make_targets(kImageSize, kImageSize);
    const int streams = targets ? make_streams() : 0;
    vk_queue_unlock();
    S.want_w = S.want_h = kImageSize;
    S.ok = targets && streams;
    rt_log("preview3d: %s", S.ok ? "offscreen target and streams ready" : "offscreen target failed");
    return S.ok;
}

extern "C" int preview3d_ready(void) { return S.ok; }

extern "C" void preview3d_set_camera(float yaw, float pitch, float zoom) {
    if (yaw != S.yaw || pitch != S.pitch || zoom != S.zoom) {
        if (S.frame < 10)
            rt_log("preview3d: camera yaw %.4f->%.4f pitch %.4f->%.4f zoom %.4f->%.4f", S.yaw, yaw, S.pitch, pitch, S.zoom, zoom);
        S.yaw = yaw;
        S.pitch = pitch;
        S.zoom = zoom;
        S.dirty = 1;
    }
}

extern "C" void preview3d_invalidate(void) { S.dirty = 1; }

extern "C" int preview3d_resize(int w, int h) {
    if (!S.ok) return 0;
    if (w <= 0 || h <= 0) return 1;   // nothing to draw into this frame; the next one with a size is used
    const uint32_t cw = (uint32_t)w > kImageSize ? kImageSize : (uint32_t)w;
    const uint32_t ch = (uint32_t)h > kImageSize ? kImageSize : (uint32_t)h;
    if (cw != S.want_w || ch != S.want_h) {
        S.want_w = cw;
        S.want_h = ch;
        S.dirty = 1;   // the viewport changed, so the image has to be drawn again - no device calls here
    }
    return 1;
}

// The fraction of the image the panel's size occupies, so the quad samples only the drawn part and nothing is stretched.
extern "C" void preview3d_uv(float *u, float *v) {
    *u = S.ok ? (float)S.want_w / (float)kImageSize : 1.0f;
    *v = S.ok ? (float)S.want_h / (float)kImageSize : 1.0f;
}

extern "C" void *preview3d_texture(void) {
    return S.tex ? (void *)(uintptr_t)S.tex : NULL;
}

extern "C" void preview3d_record(void *command_buffer) {
    if (!S.ok || !S.dirty) return;
    const BpcBlueprint *bp = bp_picker_blueprint();
    if (!bp || bp->actors < 1) return;
    VkCommandBuffer cb = (VkCommandBuffer)command_buffer;
    S.dirty = 0;
    S.frame++;

    const uint32_t w = S.want_w, h = S.want_h;
    // A freshly made image is UNDEFINED; only after a frame has been drawn is it readable, so the source layout of the
    // transition depends on whether this is the first record.
    const VkImageLayout from = S.first_use ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkAccessFlags from_access = S.first_use ? 0 : VK_ACCESS_SHADER_READ_BIT;
    const VkPipelineStageFlags from_stage =
        S.first_use ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    barrier(cb, S.color, VK_IMAGE_ASPECT_COLOR_BIT, from, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, from_access,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, from_stage, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    S.first_use = 0;
    // The depth image is cleared every frame, so it may start UNDEFINED without losing anything.
    barrier(cb, S.depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);

    VkRenderingAttachmentInfo ca{};
    ca.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    ca.imageView = S.color_view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color = {{0.06f, 0.07f, 0.06f, 1.0f}};
    VkRenderingAttachmentInfo da{};
    da.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    da.imageView = S.depth_view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {w, h}};   // the panel-sized part of the fixed image
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;

    vkCmdBeginRendering(cb, &ri);
    VkViewport vpo{0, 0, (float)w, (float)h, 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vpo);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pipe);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &S.vtx_buf, &zero);
    vkCmdBindIndexBuffer(cb, S.idx_buf, 0, VK_INDEX_TYPE_UINT16);

    float cam[16];
    camera_matrix(bp->bbox_min, bp->bbox_max, S.yaw, S.pitch, S.zoom, cam);
    if (S.frame < 4)   // diagnose once: the box the camera is framing must be non-degenerate
        rt_log("preview3d: bbox %.2f..%.2f x %.2f..%.2f y %.2f..%.2f z, %d actors", bp->bbox_min[0], bp->bbox_max[0],
               bp->bbox_min[1], bp->bbox_max[1], bp->bbox_min[2], bp->bbox_max[2], bp->actors);

    int drawn = 0;
    for (int k = 0; k < bp->actors; k++) {
        const char *name = bp->names[k];
        if (!name || !*name) continue;
        const int slot = upload_part(name);
        if (slot < 0) continue;
        const GpuPart &p = S.parts[slot];

        float model[16], mvp[16];
        part_matrix(bp, k, model);
        mat_mul(cam, model, mvp);

        Push push{};
        memcpy(push.mvp, mvp, sizeof push.mvp);
        palette_rgb(k, push.color);
        vkCmdPushConstants(cb, S.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, &push);
        vkCmdDrawIndexed(cb, p.count, 1, p.idx_bytes / 2, (int32_t)(p.vtx_bytes / 12), 0);
        drawn++;
    }
    vkCmdEndRendering(cb);
    barrier(cb, S.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    rt_log("preview3d: %d parts in %d draws (%ux%u)", bp->actors, drawn, w, h);
}