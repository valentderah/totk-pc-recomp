#include "prompt_textures.h"
#include "input_prompts.h"
#include <CommonCrypto/CommonDigest.h>
#include <stdlib.h>
#include <string.h>

typedef struct {uint32_t code,width,height,bytes;uint8_t digest[32];} NativePromptTextureIdentity;
#include "native_prompt_textures.h"

uint32_t native_prompt_texture_identify(const void *pixels,size_t size,unsigned width,unsigned height) {
    if(!pixels || !size || size>UINT32_MAX)return 0;
    uint8_t hash[32];bool hashed=false;
    for(unsigned i=0;i<sizeof prompt_textures/sizeof *prompt_textures;i++) {
        const NativePromptTextureIdentity *p=&prompt_textures[i];
        if(!p->code || p->width!=width || p->height!=height || p->bytes!=size)continue;
        if(!hashed){CC_SHA256(pixels,(CC_LONG)size,hash);hashed=true;}
        if(!memcmp(hash,p->digest,sizeof hash))return p->code;
    }
    return 0;
}

// The replay owns this bounded cache. Variants are immutable, so a device
// switch cannot change a texture still being read by an in-flight GPU command.
typedef struct {
    NativePromptGlyph glyph;
    NativePromptSnapshot controls;
    uint32_t code;
    unsigned width,height;
    __strong id<MTLTexture> texture;
} PromptTexture;
static PromptTexture textures[256];
static unsigned texture_count;

id<MTLTexture> native_prompt_texture(id<MTLDevice> device,uint32_t code,unsigned width,unsigned height,
                                    void (*make_resident)(id<MTLTexture>)) {
    static NativePromptSnapshot snapshot;
    if(snapshot.revision!=native_prompts_revision())snapshot=native_prompts_snapshot();
    bool diagram=code==0xf000,shadow=code==0xf002;
    NativePromptGlyph glyph={NATIVE_GLYPH_GAMEPAD,"",0};
    if(!diagram && !shadow && !native_prompt_glyph(code,&snapshot,&glyph))return nil;
    for(unsigned i=0;i<texture_count;i++) {
        PromptTexture *t=&textures[i];
        if(t->code==code && t->width==width && t->height==height && t->glyph.kind==glyph.kind && t->glyph.detail==glyph.detail && !strcmp(t->glyph.label,glyph.label) &&
           (!diagram || (t->controls.device==snapshot.device && t->controls.swap_faces==snapshot.swap_faces &&
             !memcmp(t->controls.buttons,snapshot.buttons,sizeof snapshot.buttons))))return t->texture;
    }
    if(texture_count==sizeof textures/sizeof *textures || !width || !height || width>1024 || height>1024)return nil;
    unsigned stride=width*(diagram?4:1);
    uint8_t *pixels=calloc(stride,height);
    if(!pixels)return nil;
    bool drawn=shadow || (diagram?native_prompt_diagram(&snapshot,pixels,width,height):native_prompt_raster(&glyph,pixels,width,height));
    if(!drawn){free(pixels);return nil;}
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:diagram?MTLPixelFormatRGBA8Unorm_sRGB:MTLPixelFormatR8Unorm width:width height:height mipmapped:NO];
    d.storageMode=MTLStorageModeShared;
    d.usage=MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView;
    id<MTLTexture> texture=[device newTextureWithDescriptor:d];
    if(texture)[texture replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0 withBytes:pixels bytesPerRow:stride];
    free(pixels);
    if(!texture)return nil;
    if(make_resident)make_resident(texture);
    textures[texture_count++]=(PromptTexture){.glyph=glyph,.controls=snapshot,.code=code,.width=width,.height=height,.texture=texture};
    return texture;
}
