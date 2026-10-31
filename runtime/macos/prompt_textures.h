#ifndef TOTK_NATIVE_PROMPT_TEXTURES_H
#define TOTK_NATIVE_PROMPT_TEXTURES_H
#include <stddef.h>
#include <stdint.h>
uint32_t native_prompt_texture_identify(const void *, size_t, unsigned width, unsigned height);
#ifdef __OBJC__
#import <Metal/Metal.h>
id<MTLTexture> native_prompt_texture(id<MTLDevice>, uint32_t code, unsigned width, unsigned height,
                                    void (*make_resident)(id<MTLTexture>));
#endif
#endif
