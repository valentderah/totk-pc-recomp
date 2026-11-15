"""Compile actual swapchain build/extent refresh against changing host surface facts."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1]
out=root/'build/checks/fg-history-worker';out.mkdir(parents=True,exist_ok=True)
text=(root/'runtime/gfx_present.c').read_text()
start=text.index('static int surface_extent_refresh(')
end=text.index('// Returns 1 with a usable swapchain',start)
actual=text[start:end]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "rsf_present.h"
#define MAX_IMAGES 8
#define rt_log(...) ((void)0)
#define vk_hdr_set_active(...) ((void)0)
#define vk_hdr_metadata(...) ((void)0)
#define free_capture() ((void)0)
#define reflex_apply_locked() ((void)0)
typedef struct {VkDevice device;VkPhysicalDevice phys;unsigned rsf_queue_count;} VkCtx;
static VkCtx context={.rsf_queue_count=4};
static VkCtx *vk_ctx(void) {return &context;}
static VkSurfaceKHR g_surface=(VkSurfaceKHR)1;
static VkSwapchainKHR g_sc;
static RsfPresent *g_rsf_present;
static VkFormat g_sc_fmt;
static int g_sc_hdr,g_dirty;
static VkExtent2D g_sc_ext;
static uint32_t g_sc_n;
static VkImage g_sc_img[MAX_IMAGES];
static VkImageView g_sc_view[MAX_IMAGES];
static VkSemaphore g_render_done[MAX_IMAGES];
static int phase,creates,query_fail,dirty_on_retire,client_w=1920,client_h=1080;
static VkSurfaceCapabilitiesKHR live;
static int plat_cas32(int *p,int expected,int desired) {(void)expected;(void)desired;return *p;}
static void wsys_client_size(int *w,int *h) {*w=client_w;*h=client_h;}
static void flip_locked(void) {}
static int retire_swapchain(void) {
    phase=1;g_dirty=dirty_on_retire;
    live=(VkSurfaceCapabilitiesKHR){.currentExtent={2560,1600},.minImageExtent={2560,1600},.maxImageExtent={2560,1600},
        .minImageCount=3,.maxImageCount=8,.currentTransform=VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR};
    return 1;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice p,VkSurfaceKHR s,VkSurfaceCapabilitiesKHR *c) {
    (void)p;assert(s==g_surface);if(query_fail)return VK_ERROR_SURFACE_LOST_KHR;*c=live;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice p,VkSurfaceKHR s,uint32_t *n,VkSurfaceFormatKHR *f) {
    (void)p;(void)s;*n=1;if(f)*f=(VkSurfaceFormatKHR){VK_FORMAT_B8G8R8A8_UNORM,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};return 0;
}
static int vk_hdr_surface_format(const VkSurfaceFormatKHR *f,uint32_t n,VkSurfaceFormatKHR *out) {(void)f;(void)n;(void)out;return 0;}
static VkPresentModeKHR pick_present_mode(VkPhysicalDevice p) {(void)p;return VK_PRESENT_MODE_FIFO_KHR;}
static int cfg_int(const char *s) {(void)s;return 3;}
static VkResult open_rsf_swapchain(const VkSwapchainCreateInfoKHR *ci,uint32_t backend) {
    assert(phase==1 && backend==3 && !g_dirty);
    assert(ci->imageExtent.width==2560 && ci->imageExtent.height==1600);
    assert(ci->minImageCount==4 && ci->preTransform==VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR);
    creates++;g_rsf_present=(void *)2;g_sc=(VkSwapchainKHR)3;return 0;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice d,const VkSwapchainCreateInfoKHR *c,const VkAllocationCallbacks *a,VkSwapchainKHR *s) {
    (void)d;(void)c;(void)a;(void)s;assert(0);return 0;
}
int32_t rsf_present_images(RsfPresent *p,uint32_t *count,uint64_t *images) {assert(p);*count=2;images[0]=4;images[1]=5;return 0;}
VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice d,VkSwapchainKHR s,uint32_t *n,VkImage *i) {(void)d;(void)s;(void)n;(void)i;assert(0);return 0;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice d,const VkSemaphoreCreateInfo *c,const VkAllocationCallbacks *a,VkSemaphore *s) {
    (void)d;(void)c;(void)a;*s=(VkSemaphore)6;return 0;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice d,const VkImageViewCreateInfo *c,const VkAllocationCallbacks *a,VkImageView *v) {
    (void)d;(void)c;(void)a;*v=(VkImageView)7;return 0;
}
'''+actual+r'''
int main(void) {
    // Retirement changes the actual surface after the old windowed dimensions.
    live.currentExtent=(VkExtent2D){1920,1080};
    assert(build_swapchain() && creates==1 && g_sc_ext.width==2560 && g_sc_ext.height==1600);
    dirty_on_retire=1;
    assert(!build_swapchain() && creates==1); // in-progress fullscreen/resize is deferred
    VkSurfaceCapabilitiesKHR caps;
    VkExtent2D extent;
    g_dirty=0;live.currentExtent=(VkExtent2D){UINT32_MAX,UINT32_MAX};
    live.minImageExtent=(VkExtent2D){100,200};live.maxImageExtent=(VkExtent2D){1000,1100};
    client_w=20;client_h=2000;
    assert(surface_extent_refresh(&context,&caps,&extent) && extent.width==100 && extent.height==1100);
    client_w=0;assert(!surface_extent_refresh(&context,&caps,&extent));
    query_fail=1;assert(!surface_extent_refresh(&context,&caps,&extent));
    query_fail=0;g_dirty=1;client_w=500;
    assert(!surface_extent_refresh(&context,&caps,&extent));
    return 0;
}
'''
source=out/'surface-extent.c';source.write_text('// '+'TOTK_RECOMP_'+'GENERATED_FROM_GAME_CODE\n'+code)
command=['C:/msys64/ucrt64/bin/clang.exe','-std=gnu11','-O2','-Wall','-Werror',str(source),'-o',str(out/'surface-extent.exe')]
for d in ('runtime','external/ReScaleFrame/runtime/orchestrator/include','external/ReScaleFrame/runtime/contract/include','external/ReScaleFrame/sdk/game/include'):
    command+=['-I',str(root/d)]
subprocess.run(command,check=True)
subprocess.run([str(out/'surface-extent.exe')],check=True)
print('Actual post-retirement swapchain extent/resize admission: passed (CPU only)')
