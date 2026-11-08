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
#include "blueprint_attach.h"   // where a fused material hangs, from the game's own AttachmentParam data
#include "blueprint_picker.h"   // the build's rotation and the bounds it gives, so both preview paths frame alike
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
    float lo[3], hi[3];     // its box in model space, for the contact test between a part and its fused material
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


// The matrix that places one part: its CAI rotation (row-major 3x3), its position, and its fuseInfo as a scale unless the
// part carries a fused material.
//
// fuseInfo is a real scale: `SpObj_LiftGeneratorWing_A_01_MiniGame` names the full-size wing model and carries 0.50, and the
// game shows the small version by scaling that model; bridge doors in a 15 m assembly carry 0.30 and slider blocks 2.40. It
// is NOT a scale for a Fuse-made part - every fused weapon carries 1.4-1.5 and does not grow - so exactly those are excluded,
// which is what the complaint about an oversized fused shield was about.
void part_matrix_ex(const BpcBlueprint *bp, int k, int placement_only, float out[16]) {
    mat_identity(out);
    const int fused = bp->sub_names[k] && *bp->sub_names[k];
    const float s = placement_only ? 1.0f : (fused ? 1.0f : bp->scale[k]);
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++) out[c * 4 + r] = bp->rot[k][r][c] * s;
    out[12] = bp->pos[k][0];
    out[13] = bp->pos[k][1];
    out[14] = bp->pos[k][2];
}

void part_matrix(const BpcBlueprint *bp, int k, float out[16]) { part_matrix_ex(bp, k, 0, out); }

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

// The smallest shift that makes box A touch box B along one axis: 0 when they already overlap there, else the gap between
// them, signed towards the other box. The axis with the smallest such gap is the one a fused material slides along.
static float contact_shift(float a_lo, float a_hi, float b_lo, float b_hi) {
    if (a_hi < b_lo) return b_lo - a_hi;   // A is below B: move up to meet it
    if (b_hi < a_lo) return b_hi - a_lo;   // A is above B: move down
    return 0.0f;                            // they overlap on this axis
}

// The world box of a part: its model box turned and moved by the part matrix.
static void part_world_box(const BpcBlueprint *bp, int k, const float lo[3], const float hi[3], float wlo[3],
                           float whi[3]) {
    float m[16];
    part_matrix(bp, k, m);
    for (int c = 0; c < 8; c++) {
        const float p[3] = {(c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1], (c & 4) ? hi[2] : lo[2]};
        for (int r = 0; r < 3; r++) {
            const float v = m[r * 4 + 0] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
            if (c == 0 || v < wlo[r]) wlo[r] = v;
            if (c == 0 || v > whi[r]) whi[r] = v;
        }
    }
}

// A shape whose material is one of the model's `_Hide` ones: the blade of a weapon, which the game stops drawing once a
// material is fused onto it. The shape name carries its material after "__" (Weapon_Lsword_147_Blade__Mt_Blade_Hide), so the
// test is on the model's own naming and works for any weapon without a list of items.
static int shape_is_hidden_blade(const BfresModel *m, int index) {
    char name[128] = {0};
    if (!bfres_shape_name(m, index, name, sizeof name)) return 0;
    const char *mat = strstr(name, "__");
    return mat && strstr(mat, "_Hide") != NULL;
}

// Copies one part's geometry into the big buffers. No allocation, so this is safe during a frame.
// `as_model_name` says the name is already a MODEL name (a fused material's SubModel) rather than an actor: those have no
// Pack/Actor entry to read a ModelInfo from, so they take the direct model path.
// `hide_blade` drops the shapes whose material is one of the model's `_Hide` materials: a fused weapon keeps its handle and
// loses its blade, and the model says which shapes those are - the blade's material is `Mt_Blade_Hide`, so the rule is the
// model's own material naming, not a list of items.
static int upload_part_named(const char *name, int as_model_name, int hide_blade) {
    char key[96];
    snprintf(key, sizeof key, "%s%s", name, hide_blade ? "|<fused>" : "");
    for (int i = 0; i < S.part_count; i++)
        if (!strcmp(S.parts[i].name, key)) return i;
    if (S.part_count >= kMaxParts) return -1;

    // The runtime model source first: read the .bfres.mc from the RomFS and decode it, then upload each shape's vertices
    // and indices with its bone transform already applied (bfres_shape_geometry). Fall back to the offline pack and the
    // collision shapes only when the runtime source is unavailable.
    const BfresModel *model = as_model_name ? model_romfs_resolve_model(name) : model_romfs_resolve(name);
    // Some actors are two models: their own, and an `_Inside_Reduction_` one that fills the inside (a spring's housing, a
    // Zonai pile's interior). Both belong to this part at the same transform, so they are uploaded into one GpuPart and
    // every later rule - contact, glue, framing - treats them as the one object the actor is.
    const BfresModel *inside = (!as_model_name && model) ? model_romfs_resolve_inside(name) : NULL;
    const BfresModel *models[2] = {model, inside};
    const int nmodel = inside ? 2 : 1;
    if (model) {
        int total_verts = 0, total_tris = 0;
        for (int mi = 0; mi < nmodel; mi++)
            for (int s = 0; s < models[mi]->count; s++) {
                if (hide_blade && shape_is_hidden_blade(models[mi], s)) continue;
                total_verts += bfres_shape_vertices(models[mi], s);
                total_tris += bfres_shape_triangles(models[mi], s);
            }
        if (total_verts < 3 || total_tris < 1) return -1;
        const uint32_t vtx_bytes = (uint32_t)total_verts * 12u;
        const uint32_t idx_bytes = (uint32_t)total_tris * 6u;
        if (S.vtx_used + vtx_bytes > kVtxCapacity || S.idx_used + idx_bytes > kIdxCapacity) return -1;

        GpuPart &p = S.parts[S.part_count];
        snprintf(p.name, sizeof p.name, "%s", key);
        p.vtx_bytes = S.vtx_used;
        p.idx_bytes = S.idx_used;
        p.count = (uint32_t)total_tris * 3u;
        float box_lo[3] = {1e30f, 1e30f, 1e30f}, box_hi[3] = {-1e30f, -1e30f, -1e30f};

        int vbase = 0, tbase = 0;
        for (int mi = 0; mi < nmodel; mi++)
            for (int s = 0; s < models[mi]->count; s++) {
                if (hide_blade && shape_is_hidden_blade(models[mi], s)) continue;
                const int nverts = bfres_shape_vertices(models[mi], s);
                const int ntris = bfres_shape_triangles(models[mi], s);
            if (nverts <= 0 || ntris <= 0) continue;
            float *verts = (float *)alloca((size_t)nverts * 3 * sizeof(float));
            uint16_t *tris = (uint16_t *)alloca((size_t)ntris * 3 * sizeof(uint16_t));
            const int vc = bfres_shape_geometry(models[mi], s, verts, nverts, tris, ntris);
            if (vc <= 0) continue;
            if (S.frame < 3)   // diagnostic: the first vertices, to see whether the decode/bone produced sane numbers
                rt_log("preview3d: %s shape%d v0=(%.2f,%.2f,%.2f) v1=(%.2f,%.2f,%.2f) tris0=(%u,%u,%u)",
                       name, s, verts[0], verts[1], verts[2], verts[3], verts[4], verts[5], tris[0], tris[1], tris[2]);
            memcpy(S.vtx_map + S.vtx_used + (size_t)vbase * 12u, verts, (size_t)vc * 12u);
            for (int v = 0; v < vc; v++)
                for (int a = 0; a < 3; a++) {
                    const float x = verts[(size_t)v * 3 + a];
                    if (x < box_lo[a]) box_lo[a] = x;
                    if (x > box_hi[a]) box_hi[a] = x;
                }
            for (int t = 0; t < ntris; t++) {
                uint16_t idx[3] = {(uint16_t)(tris[t * 3] + vbase), (uint16_t)(tris[t * 3 + 1] + vbase),
                                   (uint16_t)(tris[t * 3 + 2] + vbase)};
                memcpy(S.idx_map + S.idx_used + (size_t)(tbase + t) * 6u, idx, 6u);
            }
            vbase += vc;
            tbase += ntris;
        }
        for (int a = 0; a < 3; a++) {
            p.lo[a] = box_lo[a];
            p.hi[a] = box_hi[a];
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
    snprintf(p.name, sizeof p.name, "%s", key);
    p.vtx_bytes = S.vtx_used;
    p.idx_bytes = S.idx_used;
    p.count = (uint32_t)ntris * 3u;
    for (int a = 0; a < 3; a++) {
        p.lo[a] = 1e30f;
        p.hi[a] = -1e30f;
    }
    for (int v = 0; v < nverts; v++)
        for (int a = 0; a < 3; a++) {
            const float x = positions[(size_t)v * 3 + a];
            if (x < p.lo[a]) p.lo[a] = x;
            if (x > p.hi[a]) p.hi[a] = x;
        }
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

int upload_part(const char *name) { return upload_part_named(name, 0, 0); }
// A part with a fused material: the weapon keeps its handle and loses the blade shapes (see shape_is_hidden_blade).
int upload_part_fused(const char *name) { return upload_part_named(name, 0, 1); }
// A fused material names a model directly, which has no ModelInfo to read.
int upload_model(const char *model_name) { return upload_part_named(model_name, 1, 0); }

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

    float cam[16], rotm[16], camrot[16];
    // The build's own turn, and the bounds that turn gives: the camera frames the turned build, exactly as the CPU
    // projection does, so the two paths draw the same picture and the build never leaves the panel at any angle.
    float rot[9], rlo[3], rhi[3];
    bp_picker_preview_rot(rot);
    bp_picker_preview_bounds(rlo, rhi);
    camera_matrix(rlo, rhi, S.yaw, S.pitch, S.zoom, cam);
    mat_identity(rotm);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) rotm[c * 4 + r] = rot[r * 3 + c];   // row-major to Vulkan's column-major
    mat_mul(cam, rotm, camrot);
    if (S.frame < 4)   // diagnose once: the box the camera is framing must be non-degenerate
        rt_log("preview3d: bbox %.2f..%.2f x %.2f..%.2f y %.2f..%.2f z, %d actors", rlo[0], rhi[0], rlo[1],
               rhi[1], rlo[2], rhi[2], bp->actors);

    // The CAI's 12-byte link records are connection data, not a placement rule: the green glue is something
    // Ultrahand creates in the game when it joins two objects, and a blueprint that has not been built has none.
    // Nothing is slid onto anything here.

    int drawn = 0;
    for (int k = 0; k < bp->actors; k++) {
        const char *name = bp->names[k];
        if (!name || !*name) continue;
        const char *sub = bp->sub_names[k];
        const int fused = sub && *sub;
        // A fused part loses its covered shapes only when the material brings a replacement model for this weapon class; a
        // fusion that merely adds a material on top leaves the weapon whole (a spear keeps its blade under a horn).
        int replaces = 0;
        if (fused) {
            BpAttach probe;
            if (bp_attach_resolve(name, sub, &probe)) replaces = probe.has_sub_model;
        }
        const int slot = replaces ? upload_part_fused(name) : upload_part(name);
        if (slot < 0) continue;
        const GpuPart &p = S.parts[slot];

        float model[16], mvp[16];
        part_matrix(bp, k, model);
        mat_mul(camrot, model, mvp);

        Push push{};
        memcpy(push.mvp, mvp, sizeof push.mvp);
        palette_rgb(k, push.color);
        vkCmdPushConstants(cb, S.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, &push);
        vkCmdDrawIndexed(cb, p.count, 1, p.idx_bytes / 2, (int32_t)(p.vtx_bytes / 12), 0);
        drawn++;

        // A part that was fused with a material is two objects: draw the material on the socket the game names for it.
        // Where that is comes from the game's AttachmentParam data (runtime/blueprint_attach.c); the socket is a bone of
        // the weapon model, so its world transform goes first and the material's own rotation, scale and offset follow.
        if (fused) {
            BpAttach at;
            if (bp_attach_resolve(name, sub, &at)) {
                const int sub_slot = upload_model(at.model);
                if (sub_slot >= 0) {
                    float bone[16];
                    // A record that names a BoundBoxLength is fitted to it, which is how the game keeps a large fused object
                    // readable; the material's own largest side comes from the decoded model.
                    const BfresModel *weapon = model_romfs_resolve(name);
                    float fit = 1.0f;
                    {
                        const float mat_extent = model_romfs_extent(model_romfs_resolve_model(at.model));
                        if (at.bound_box_length > 0.0f && mat_extent > 0.0005f) {
                            fit = at.bound_box_length / mat_extent;   // the fitted length this class names
                        } else if (!at.has_own_scale && mat_extent > 0.0005f) {
                            // No size of its own: fit it to the weapon, so a fused world object is the size the weapon can
                            // carry rather than its size in the world. This is the rule behind "a boulder looks smaller on
                            // a shield": it is the shield's size that wins.
                            const float weapon_extent = model_romfs_extent(weapon);
                            if (weapon_extent > 0.0005f) fit = weapon_extent / mat_extent;
                        }
                    }
                    const int bi = weapon ? bfres_bone_find(weapon, at.bone) : -1;
                    if (bi < 0 || !bfres_bone_world(weapon, bi, bone))
                        mat_identity(bone);   // no socket found: draw the material at the part's own origin rather than drop it
                    float attach_row[16], attach_col[16], placed[16], sub_mvp[16], material_part[16];
                    bp_attach_matrix(&at, bone, fit, attach_row);
                    for (int r = 0; r < 4; r++)
                        for (int c = 0; c < 4; c++) attach_col[c * 4 + r] = attach_row[r * 4 + c];
                    // fuseInfo is the WEAPON's scale, so it is left out here: a fuse joins two objects that keep their own size.
                    part_matrix_ex(bp, k, 1, material_part);
                    mat_mul(material_part, attach_col, placed);   // the material sits in the weapon's model space
                    // A fused object sits ON its weapon: the socket and the record's offset put it near, and this slides it
                    // along the one axis where the gap is smallest, so a blade and its horn meet instead of floating apart.
                    // Both boxes are measured in the weapon's model space, which is where `placed` lives.
                    {
                        const GpuPart &w = S.parts[slot];
                        const GpuPart &m = S.parts[sub_slot];
                        if (w.lo[0] <= w.hi[0] && m.lo[0] <= m.hi[0]) {
                            float mlo[3], mhi[3], wlo[3], whi[3];
                            for (int c = 0; c < 8; c++) {
                                const float pm[3] = {(c & 1) ? m.hi[0] : m.lo[0], (c & 2) ? m.hi[1] : m.lo[1],
                                                     (c & 4) ? m.hi[2] : m.lo[2]};
                                const float pw[3] = {(c & 1) ? w.hi[0] : w.lo[0], (c & 2) ? w.hi[1] : w.lo[1],
                                                     (c & 4) ? w.hi[2] : w.lo[2]};
                                for (int r = 0; r < 3; r++) {
                                    const float vm = attach_row[r * 4 + 0] * pm[0] + attach_row[r * 4 + 1] * pm[1] +
                                                     attach_row[r * 4 + 2] * pm[2] + attach_row[r * 4 + 3];
                                    if (c == 0 || vm < mlo[r]) mlo[r] = vm;
                                    if (c == 0 || vm > mhi[r]) mhi[r] = vm;
                                    if (c == 0 || pw[r] < wlo[r]) wlo[r] = pw[r];
                                    if (c == 0 || pw[r] > whi[r]) whi[r] = pw[r];
                                }
                            }
                            int axis = -1;
                            float best = 0.0f;
                            for (int a = 0; a < 3; a++) {
                                const float gap = contact_shift(mlo[a], mhi[a], wlo[a], whi[a]);
                                if (gap == 0.0f) continue;   // touching on this axis
                                if (axis < 0 || fabsf(gap) < fabsf(best)) { best = gap; axis = a; }
                            }
                            if (axis >= 0) {
                                float move[16], shifted[16];
                                mat_identity(move);
                                move[axis * 4 + 3] = best;   // column-major translation
                                mat_mul(placed, move, shifted);
                                memcpy(placed, shifted, sizeof placed);
                            }
                        }
                    }
                    mat_mul(camrot, placed, sub_mvp);
                    Push sub_push{};
                    memcpy(sub_push.mvp, sub_mvp, sizeof sub_push.mvp);
                    palette_rgb(k, sub_push.color);
                    vkCmdPushConstants(cb, S.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof sub_push, &sub_push);
                    vkCmdDrawIndexed(cb, S.parts[sub_slot].count, 1, S.parts[sub_slot].idx_bytes / 2,
                                     (int32_t)(S.parts[sub_slot].vtx_bytes / 12), 0);
                    drawn++;
                }
            }
        }
    }
    vkCmdEndRendering(cb);
    barrier(cb, S.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    rt_log("preview3d: %d parts in %d draws (%ux%u)", bp->actors, drawn, w, h);
}