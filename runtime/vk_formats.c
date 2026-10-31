// NVN format -> VkFormat for the textures of the graphics layer.
// Ported from the format table of LAS Recomp runtime/nvn/vulkan/textures.c @ da35b83a (MIT, Copyright (c) 2026 Peiikai).
// Differences from LAS: the table is indexed by the NVNformat values of nvn.h and covers all of them, ASTC decodes to
// RGBA8 unless the device samples it (vk_astc.c, TOTK_ASTC), D24S8 is always D32S8 (no probing: the RX 9070 lacks
// it), 3-component formats expand to 4 components, and formats with no Vulkan equivalent are reported with a reason.
#include <string.h>
#include "tex.h"

#define S VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
#define TX (VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)
#define DS VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT

enum { F_RGB = 1, F_X = 2 };   // expand_rgb, alpha_one

// chan: the component order vk holds (TexVkFormat.chan), zero when it is NVN's. alt, alt_chan: a mandatory format
// standing in, with its order, when the device lacks vk.
typedef struct { VkFormat vk; uint8_t flags; const char *note; uint8_t chan[4]; VkFormat alt; uint8_t alt_chan[4]; } Row;

#define R(nvn, vkf) [nvn] = {vkf, 0, NULL}
#define RGB(nvn, vkf) [nvn] = {vkf, F_RGB, "3-component format: each texel is expanded to 4 components on upload"}
#define X(nvn, vkf) [nvn] = {vkf, F_X, "X format: alpha is sampled as 1 through the view swizzle"}
#define N(nvn, vkf, why) [nvn] = {vkf, 0, why}

// 16-bit packed formats. NVN names their components from the least significant bit, as it does for RGB10A2
// (A2B10G10R10): RGB565 holds R in bits 0-4, RGB5A1 R in bits 0-4 and A in bit 15. Vulkan names them from the most
// significant bit, so RGB565 is B5G6R5. RGB5A1's counterpart A1B5G5R5 needs VK_KHR_maintenance5, so it stands in as
// A1R5G5B5 with R and B swapped by the view swizzle: sampling is right, rendering into it would store R and B swapped
// (no TotK pass does). The terrain's normal (RGB5A1) and blend (RGB565) maps read with the old order stretched the
// ground textures along one axis.
enum { CR = NVN_TEXTURE_SWIZZLE_R, CG = NVN_TEXTURE_SWIZZLE_G, CB = NVN_TEXTURE_SWIZZLE_B, CA = NVN_TEXTURE_SWIZZLE_A };
#define P(nvn, vkf, fl, c0, c1, c2, c3, altf, a0, a1, a2, a3) \
    [nvn] = {vkf, fl, "16-bit packed format, NVN names components from the least significant bit", {c0, c1, c2, c3}, altf, \
             {a0, a1, a2, a3}}

static const Row rows[0x9D] = {
    R(0x01, VK_FORMAT_R8_UNORM), R(0x02, VK_FORMAT_R8_SNORM), R(0x03, VK_FORMAT_R8_UINT), R(0x04, VK_FORMAT_R8_SINT),
    R(0x05, VK_FORMAT_R16_SFLOAT), R(0x06, VK_FORMAT_R16_UNORM), R(0x07, VK_FORMAT_R16_SNORM),
    R(0x08, VK_FORMAT_R16_UINT), R(0x09, VK_FORMAT_R16_SINT), R(0x0A, VK_FORMAT_R32_SFLOAT),
    R(0x0B, VK_FORMAT_R32_UINT), R(0x0C, VK_FORMAT_R32_SINT),
    R(0x0D, VK_FORMAT_R8G8_UNORM), R(0x0E, VK_FORMAT_R8G8_SNORM), R(0x0F, VK_FORMAT_R8G8_UINT),
    R(0x10, VK_FORMAT_R8G8_SINT), R(0x11, VK_FORMAT_R16G16_SFLOAT), R(0x12, VK_FORMAT_R16G16_UNORM),
    R(0x13, VK_FORMAT_R16G16_SNORM), R(0x14, VK_FORMAT_R16G16_UINT), R(0x15, VK_FORMAT_R16G16_SINT),
    R(0x16, VK_FORMAT_R32G32_SFLOAT), R(0x17, VK_FORMAT_R32G32_UINT), R(0x18, VK_FORMAT_R32G32_SINT),
    RGB(0x19, VK_FORMAT_R8G8B8A8_UNORM), RGB(0x1A, VK_FORMAT_R8G8B8A8_SNORM), RGB(0x1B, VK_FORMAT_R8G8B8A8_UINT),
    RGB(0x1C, VK_FORMAT_R8G8B8A8_SINT), RGB(0x1D, VK_FORMAT_R16G16B16A16_SFLOAT), RGB(0x1E, VK_FORMAT_R16G16B16A16_UNORM),
    RGB(0x1F, VK_FORMAT_R16G16B16A16_SNORM), RGB(0x20, VK_FORMAT_R16G16B16A16_UINT), RGB(0x21, VK_FORMAT_R16G16B16A16_SINT),
    RGB(0x22, VK_FORMAT_R32G32B32A32_SFLOAT), RGB(0x23, VK_FORMAT_R32G32B32A32_UINT), RGB(0x24, VK_FORMAT_R32G32B32A32_SINT),
    R(0x25, VK_FORMAT_R8G8B8A8_UNORM), R(0x26, VK_FORMAT_R8G8B8A8_SNORM), R(0x27, VK_FORMAT_R8G8B8A8_UINT),
    R(0x28, VK_FORMAT_R8G8B8A8_SINT), R(0x29, VK_FORMAT_R16G16B16A16_SFLOAT), R(0x2A, VK_FORMAT_R16G16B16A16_UNORM),
    R(0x2B, VK_FORMAT_R16G16B16A16_SNORM), R(0x2C, VK_FORMAT_R16G16B16A16_UINT), R(0x2D, VK_FORMAT_R16G16B16A16_SINT),
    R(0x2E, VK_FORMAT_R32G32B32A32_SFLOAT), R(0x2F, VK_FORMAT_R32G32B32A32_UINT), R(0x30, VK_FORMAT_R32G32B32A32_SINT),
    R(0x31, VK_FORMAT_S8_UINT), R(0x32, VK_FORMAT_D16_UNORM),
    N(0x33, VK_FORMAT_D32_SFLOAT, "DEPTH24 has no Vulkan format on this GPU; D32_SFLOAT"),
    R(0x34, VK_FORMAT_D32_SFLOAT),
    N(0x35, VK_FORMAT_D32_SFLOAT_S8_UINT, "the RX 9070 has no D24S8 attachments; D32_SFLOAT_S8_UINT"),
    R(0x36, VK_FORMAT_D32_SFLOAT_S8_UINT),
    X(0x37, VK_FORMAT_R8G8B8A8_SRGB), R(0x38, VK_FORMAT_R8G8B8A8_SRGB),
    // RGBA4: A4B4G4R4 (core in 1.3, optional), else B4G4R4A4 whose A, R, G, B hold NVN's R, G, B, A.
    P(0x39, VK_FORMAT_A4B4G4R4_UNORM_PACK16, 0, 0, 0, 0, 0, VK_FORMAT_B4G4R4A4_UNORM_PACK16, CA, CR, CG, CB),
    P(0x3A, VK_FORMAT_A1R5G5B5_UNORM_PACK16, F_X, CB, CG, CR, CA, 0, 0, 0, 0, 0),
    P(0x3B, VK_FORMAT_A1R5G5B5_UNORM_PACK16, 0, CB, CG, CR, CA, 0, 0, 0, 0, 0),
    P(0x3C, VK_FORMAT_B5G6R5_UNORM_PACK16, 0, 0, 0, 0, 0, VK_FORMAT_R5G6B5_UNORM_PACK16, CB, CG, CR, CA),
    R(0x3D, VK_FORMAT_A2B10G10R10_UNORM_PACK32), R(0x3E, VK_FORMAT_A2B10G10R10_UINT_PACK32),
    R(0x3F, VK_FORMAT_B10G11R11_UFLOAT_PACK32), R(0x40, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32),
    R(0x41, VK_FORMAT_BC1_RGB_UNORM_BLOCK), R(0x42, VK_FORMAT_BC1_RGBA_UNORM_BLOCK),
    R(0x43, VK_FORMAT_BC2_UNORM_BLOCK), R(0x44, VK_FORMAT_BC3_UNORM_BLOCK),
    R(0x45, VK_FORMAT_BC1_RGB_SRGB_BLOCK), R(0x46, VK_FORMAT_BC1_RGBA_SRGB_BLOCK),
    R(0x47, VK_FORMAT_BC2_SRGB_BLOCK), R(0x48, VK_FORMAT_BC3_SRGB_BLOCK),
    R(0x49, VK_FORMAT_BC4_UNORM_BLOCK), R(0x4A, VK_FORMAT_BC4_SNORM_BLOCK),
    R(0x4B, VK_FORMAT_BC5_UNORM_BLOCK), R(0x4C, VK_FORMAT_BC5_SNORM_BLOCK),
    R(0x4D, VK_FORMAT_BC7_UNORM_BLOCK), R(0x4E, VK_FORMAT_BC7_SRGB_BLOCK),
    R(0x4F, VK_FORMAT_BC6H_SFLOAT_BLOCK), R(0x50, VK_FORMAT_BC6H_UFLOAT_BLOCK),
    // 0x51-0x68 (integer storage sampled as float) and 0x6B, 0x6C: unmapped, see the reason in tex_vk_format.
    R(0x69, VK_FORMAT_A2B10G10R10_SNORM_PACK32), R(0x6A, VK_FORMAT_A2B10G10R10_SINT_PACK32),
    X(0x6D, VK_FORMAT_R8G8B8A8_UNORM), X(0x6E, VK_FORMAT_R8G8B8A8_SNORM), X(0x6F, VK_FORMAT_R8G8B8A8_UINT),
    X(0x70, VK_FORMAT_R8G8B8A8_SINT), X(0x71, VK_FORMAT_R16G16B16A16_SFLOAT), X(0x72, VK_FORMAT_R16G16B16A16_UNORM),
    X(0x73, VK_FORMAT_R16G16B16A16_SNORM), X(0x74, VK_FORMAT_R16G16B16A16_UINT), X(0x75, VK_FORMAT_R16G16B16A16_SINT),
    X(0x76, VK_FORMAT_R32G32B32A32_SFLOAT), X(0x77, VK_FORMAT_R32G32B32A32_UINT), X(0x78, VK_FORMAT_R32G32B32A32_SINT),
    // 0x79-0x94 ASTC: handled in tex_vk_format.
    P(0x95, VK_FORMAT_R5G6B5_UNORM_PACK16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
    P(0x96, VK_FORMAT_A1R5G5B5_UNORM_PACK16, F_X, 0, 0, 0, 0, 0, 0, 0, 0, 0),
    P(0x97, VK_FORMAT_A1R5G5B5_UNORM_PACK16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
    P(0x98, VK_FORMAT_R5G5B5A1_UNORM_PACK16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
    R(0x99, VK_FORMAT_B8G8R8A8_UNORM), R(0x9A, VK_FORMAT_B8G8R8A8_UNORM),
    R(0x9B, VK_FORMAT_B8G8R8A8_SRGB), R(0x9C, VK_FORMAT_B8G8R8A8_SRGB),
};

// ASTC block sizes in the order of NVN 0x79..0x86 (linear) and 0x87..0x94 (sRGB), with the Vulkan formats they stand for.
static const struct { uint8_t bw, bh; VkFormat unorm, srgb; } astc[14] = {
    {4, 4, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_4x4_SRGB_BLOCK},
    {5, 4, VK_FORMAT_ASTC_5x4_UNORM_BLOCK, VK_FORMAT_ASTC_5x4_SRGB_BLOCK},
    {5, 5, VK_FORMAT_ASTC_5x5_UNORM_BLOCK, VK_FORMAT_ASTC_5x5_SRGB_BLOCK},
    {6, 5, VK_FORMAT_ASTC_6x5_UNORM_BLOCK, VK_FORMAT_ASTC_6x5_SRGB_BLOCK},
    {6, 6, VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_SRGB_BLOCK},
    {8, 5, VK_FORMAT_ASTC_8x5_UNORM_BLOCK, VK_FORMAT_ASTC_8x5_SRGB_BLOCK},
    {8, 6, VK_FORMAT_ASTC_8x6_UNORM_BLOCK, VK_FORMAT_ASTC_8x6_SRGB_BLOCK},
    {8, 8, VK_FORMAT_ASTC_8x8_UNORM_BLOCK, VK_FORMAT_ASTC_8x8_SRGB_BLOCK},
    {10, 5, VK_FORMAT_ASTC_10x5_UNORM_BLOCK, VK_FORMAT_ASTC_10x5_SRGB_BLOCK},
    {10, 6, VK_FORMAT_ASTC_10x6_UNORM_BLOCK, VK_FORMAT_ASTC_10x6_SRGB_BLOCK},
    {10, 8, VK_FORMAT_ASTC_10x8_UNORM_BLOCK, VK_FORMAT_ASTC_10x8_SRGB_BLOCK},
    {10, 10, VK_FORMAT_ASTC_10x10_UNORM_BLOCK, VK_FORMAT_ASTC_10x10_SRGB_BLOCK},
    {12, 10, VK_FORMAT_ASTC_12x10_UNORM_BLOCK, VK_FORMAT_ASTC_12x10_SRGB_BLOCK},
    {12, 12, VK_FORMAT_ASTC_12x12_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_SRGB_BLOCK},
};

int tex_vk_format(int nvn_format, TexVkFormat *o) {
    memset(o, 0, sizeof *o);
    o->vk = o->ideal = VK_FORMAT_UNDEFINED;
    if (nvn_format <= 0 || nvn_format > 0x9C) {
        o->reason = nvn_format == 0 ? "NVN_FORMAT_NONE" : "not an NVN format";
        return 0;
    }
    unsigned f = (unsigned)nvn_format;
    o->need = S | TX;
    if (f >= 0x79 && f <= 0x94) {
        unsigned i = (f - 0x79) % 14;
        int srgb = f >= 0x87;
        o->ideal = srgb ? astc[i].srgb : astc[i].unorm;
        if (vk_astc_native(o->ideal)) {   // TOTK_ASTC auto / native on a device that samples ASTC: raw blocks, like BC
            o->vk = o->ideal;
            o->note = "native ASTC";
            return 1;
        }
        o->vk = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        o->astc_bw = astc[i].bw;
        o->astc_bh = astc[i].bh;
        o->astc_srgb = (uint8_t)srgb;
        o->note = "no native ASTC: decoded to RGBA8 (vk_astc.c compute decoder, or tex_astc.c with TOTK_ASTC=cpu)";
        if (vk_astc_bc()) {
            o->vk = srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
            o->astc_bc = 1;
            o->note = "no native ASTC: decoded and encoded to BC3 on the GPU (vk_astc.c, TOTK_ASTC_BC)";
        }
        return 1;
    }
    if ((f >= 0x51 && f <= 0x68) || f == 0x6B || f == 0x6C) {
        o->reason = "integer storage sampled as float (UI2F/I2F): Vulkan has no such format, needs a shader-side conversion";
        return 0;
    }
    const Row *r = &rows[f];
    if (r->vk == VK_FORMAT_UNDEFINED) {
        o->reason = "no table entry";
        return 0;
    }
    o->vk = o->ideal = r->vk;
    o->expand_rgb = (r->flags & F_RGB) != 0;
    o->alpha_one = (r->flags & F_X) != 0;
    o->note = r->note;
    memcpy(o->chan, r->chan, sizeof o->chan);
    if (r->alt && vk_ctx() && !tex_vk_format_supported(o)) {
        o->vk = r->alt;
        memcpy(o->chan, r->alt_chan, sizeof o->chan);
        o->note = "16-bit packed format: the device lacks the exact Vulkan format, a mandatory one stands in with the "
                  "components reordered by the view swizzle";
    }
    if (f == 0x31) o->stencil = 1;
    if (f >= 0x32 && f <= 0x34) o->depth = 1;
    if (f == 0x35 || f == 0x36) o->depth = o->stencil = 1;
    if (f == 0x35) o->ideal = VK_FORMAT_D24_UNORM_S8_UINT;
    if (o->depth || o->stencil) o->need = S | TX | DS;
    return 1;
}

int tex_vk_format_supported(const TexVkFormat *f) {
    VkCtx *c = vk_ctx();
    if (!c || f->vk == VK_FORMAT_UNDEFINED) return 0;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(c->phys, f->vk, &p);
    return (p.optimalTilingFeatures & f->need) == f->need;
}
