#pragma once
static inline int dd_live_eligible(unsigned w,unsigned h,int failed){return w==2560&&h==1440&&!failed;}
// Vulkan values pinned by static assertions in vk_ngx_eval.c. Never blit depth or a reduced scene.
static inline int dd_supported(unsigned w,unsigned h,unsigned source,unsigned target,unsigned source_features,unsigned target_features){
    return w==2560&&h==1440&&source==97&&target==122&&(source_features&0x400)&&(target_features&0x800);
}
