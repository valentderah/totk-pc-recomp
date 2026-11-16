"""Exercise the actual device init/shutdown state machine without a GPU."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/checks/rsf-device-retirement'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    source = (ROOT / 'runtime/vk_device.c').read_text(encoding='utf-8')
    functions = source[source.index('int vk_init(void) {'):source.index('static const char *fault_address_type')]
    fixture = r'''

#define rsf_config_enabled() cfg_bool("TOTK_RSF_ENABLED")
#define rsf_config_fg_backend() cfg_int("TOTK_RSF_FG")
#define rsf_config_sr_backend() cfg_int("TOTK_RSF_BACKEND")
#define rsf_config_low_latency() cfg_int("TOTK_RSF_LOW_LATENCY")
#define rsf_requested() (rsf_config_enabled() || rsf_config_fg_backend() || rsf_config_low_latency())
#define rsf_temporal_requested() (rsf_config_enabled() || rsf_config_fg_backend())
#include <assert.h>
#include <string.h>
typedef struct { unsigned identity; } VkCtx;
typedef int PlatMutex;
static VkCtx g_vk;
static int g_ready, g_stopping, g_lock, locks, releases, refuse, creates, fail_create;
static void plat_mutex_lock(PlatMutex *m) { (void)m; assert(!locks); locks++; }
static void plat_mutex_unlock(PlatMutex *m) { (void)m; assert(locks==1); locks--; }
static void vk_memx_init(void) {}
static void vk_mem_host_probe(void) {}
static void vk_upload_pools_init(void) {}
static void vk_headroom_start(void) {}
static void vk_dlaa_init(void) {}
static int cfg_bool(const char *key) { (void)key; return 1; }
static int rsf_runtime_present(void) { return 1; }
static int vk_motion_texture_start(int enabled) { assert(enabled); return 1; }
#define rt_log(...) ((void)0)
static int create_instance(void) { creates++; g_vk.identity=77; return !fail_create; }
static int pick_device(void) { return 1; }
static int create_device(void) { return 1; }
static int destroy_all(void) {
    releases++;
    if(refuse) return 0;
    memset(&g_vk,0,sizeof g_vk); return 1;
}
''' + functions + r'''
int main(void) {
    assert(vk_init() && creates==1 && vk_ctx()->identity==77);
    refuse=1; vk_shutdown();
    assert(g_stopping && g_ready && vk_ctx()->identity==77 && releases==1);
    assert(!vk_init() && creates==1 && vk_ctx()->identity==77 && releases==2);
    refuse=0; vk_shutdown();
    assert(!g_stopping && !g_ready && !vk_ctx() && !g_vk.identity);
    assert(vk_init() && creates==2 && vk_ctx()->identity==77);
    vk_shutdown();
    // Partial creation failure also retains its owners until checked cleanup.
    fail_create=refuse=1;
    assert(!vk_init() && g_stopping && !g_ready && g_vk.identity==77);
    int before=creates;
    assert(!vk_init() && creates==before && g_vk.identity==77);
    refuse=0; vk_shutdown();
    assert(!g_stopping && !g_ready && !g_vk.identity);
    fail_create=0;
    assert(vk_init() && creates==before+1);
    vk_shutdown(); assert(!locks);
    return 0;
}
'''
    c = OUT / 'device-retirement.c'
    c.write_text('// TOTK_RECOMP_' + 'GENERATED_FROM_GAME_CODE\n' + fixture, encoding='utf-8')
    exe = OUT / 'device-retirement.exe'
    subprocess.run(['C:/msys64/ucrt64/bin/clang.exe', '-std=gnu11', '-O2', '-Wall', '-Wextra',
                    '-Werror', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    print('Actual device retirement state machine passed (CPU only)')


def startup_checks():
    """Actual startup adapters and the device-owner fallback block, without a GPU."""
    owner = (ROOT / 'runtime/vk_device.c').read_text(encoding='utf-8')
    begin = owner.index('    // RSF may enable provider-specific nodes.')
    block = owner[begin:owner.index('    vkGetDeviceQueue(g_vk.device', begin)]
    code = r'''
#define RSF_VULKAN_IMPL
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "rsf_vulkan.h"
#include "rsf_runtime.h"
#include <rescaleframe/native_streamline_vulkan.h>
#include <rescaleframe/native_latency_vulkan_requirements.h>
#include <rescaleframe/native_xess_vulkan_requirements.h>
static int mode, disabled, sl_calls, xess_calls, xess_instances, xess_devices, native_instances;
unsigned rt_trace;
int rsf_config_enabled(void) { return !disabled && mode!=4 && mode!=5 && mode!=6 && mode!=13 && mode!=11; }
int rsf_config_sr_backend(void) { return 1; }
int rsf_config_fg_backend(void) { return !disabled && (mode==4 || mode==5 || mode==6 || mode==13) ? 5:0; }
int rsf_config_low_latency(void) { return 0; }
int rsf_requested(void) { return rsf_config_enabled() || rsf_config_fg_backend(); }
void rsf_runtime_disable(const char *reason) { assert(reason); disabled=1; }
void rt_log(const char *format,...) { (void)format; }
const char *rsf_runtime_path(void) { return mode==14 ? NULL:"mock"; }
int rsf_runtime_load(void) { return mode!=0; }
int rsf_runtime_present(void) { return mode!=14; }
const char *rsf_provider_directory(unsigned backend) { (void)backend; return "mock"; }
void *rsf_runtime_execution_sink(void) { return NULL; }
static VkResult VKAPI_CALL native_instance(const VkInstanceCreateInfo *i,const VkAllocationCallbacks *a,VkInstance *out) {
    (void)i;(void)a;native_instances++;*out=(VkInstance)1;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *i,const VkAllocationCallbacks *a,VkInstance *out) {return native_instance(i,a,out);}
static VkResult VKAPI_CALL native_device(VkPhysicalDevice p,const VkDeviceCreateInfo *i,const VkAllocationCallbacks *a,VkDevice *out) {
    (void)p;(void)a;assert(((const VkPhysicalDeviceFeatures2 *)i->pNext)->features.robustBufferAccess);
    assert(i->enabledExtensionCount==0);*out=(VkDevice)2;return mode==10 ? VK_ERROR_DEVICE_LOST:VK_SUCCESS;
}
static VkResult VKAPI_CALL native_enumerate(VkInstance i,uint32_t *n,VkPhysicalDevice *p) {(void)i;if(p)*p=(VkPhysicalDevice)3;*n=1;return VK_SUCCESS;}
static VkResult VKAPI_CALL interposed_enumerate(VkInstance i,uint32_t *n,VkPhysicalDevice *p) {return mode==16?VK_ERROR_INITIALIZATION_FAILED:native_enumerate(i,n,p);}
static VkResult VKAPI_CALL interposed_instance(const VkInstanceCreateInfo *i,const VkAllocationCallbacks *a,VkInstance *out) {
    return mode==7 ? VK_ERROR_INITIALIZATION_FAILED:native_instance(i,a,out);
}
static VkResult VKAPI_CALL interposed_device(VkPhysicalDevice p,const VkDeviceCreateInfo *i,const VkAllocationCallbacks *a,VkDevice *out) {
    return mode==8 ? VK_ERROR_INITIALIZATION_FAILED:native_device(p,i,a,out);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance i,const char *name) {
    (void)i;if(!strcmp(name,"vkEnumeratePhysicalDevices"))return (PFN_vkVoidFunction)native_enumerate;
    if(!strcmp(name,"vkCreateInstance"))return (PFN_vkVoidFunction)native_instance;
    if(!strcmp(name,"vkCreateDevice"))return (PFN_vkVoidFunction)native_device;
    return NULL;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice d,const char *n) {(void)d;(void)n;return NULL;}
static rsf_vk_function interposed(void *i,const char *name) {
    (void)i;if(!strcmp(name,"vkEnumeratePhysicalDevices"))return (rsf_vk_function)interposed_enumerate;
    if(!strcmp(name,"vkCreateInstance"))return (rsf_vk_function)interposed_instance;
    if(!strcmp(name,"vkCreateDevice"))return (rsf_vk_function)interposed_device;
    return NULL;
}
static int32_t sl_init(const rsf_slvk_setup *s) {(void)s;sl_calls++;return mode==1 ? -1:0;}
static int32_t xess_init(const rsf_xessvk_setup *s) {(void)s;xess_calls++;return mode==5 ? -1:0;}
static int32_t shutdown_sdk(void) {return 0;}
static int32_t require_device(const rsf_latencyvk_device_query *q,rsf_latencyvk_device_requirements *r) {
    r->features=q->features;
    if(mode==3) {((VkPhysicalDeviceFeatures2 *)q->features)->features.robustBufferAccess=0;return -1;}
    static const char *const extra[]={"optional"};
    if(mode==9) {r->extensions=extra;r->extension_count=1;}
    return 0;
}
static int32_t xess_instance(uint32_t *n,const char *const **names,uint32_t *api) {
    xess_instances++;*n=0;*names=NULL;*api=VK_API_VERSION_1_3;return mode==6 ? -1:0;
}
static int32_t xess_device(void *i,void *p,uint32_t *n,const char *const **names,void **features) {
    (void)i;(void)p;(void)features;xess_devices++;*n=0;*names=NULL;return mode==13 ? -1:0;
}
void *rsf_runtime_symbol(const char *name) {
    if(!strcmp(name,"rsf_slvk_initialize"))return mode==2 ? NULL:(void *)sl_init;
    if(!strcmp(name,"rsf_latencyvk_instance_proc"))return mode==12 ? NULL:(void *)interposed;
    if(!strcmp(name,"rsf_latencyvk_require_device"))return mode==15 ? NULL:(void *)require_device;
    if(!strcmp(name,"rsf_xessvk_initialize"))return (void *)xess_init;
    if(!strcmp(name,"rsf_xessvk_instance_requirements"))return (void *)xess_instance;
    if(!strcmp(name,"rsf_xessvk_device_requirements"))return (void *)xess_device;
    if(!strcmp(name,"rsf_latencyvk_shutdown")||!strcmp(name,"rsf_xessvk_shutdown"))return (void *)shutdown_sdk;
    return NULL;
}
typedef struct {VkPhysicalDeviceFeatures2 f2;} FeatureChain;
static struct {VkPhysicalDevice phys;VkDevice device;} g_vk;
#define vkCreateDevice rsf_vk_create_device
static int device_owner(void) {
    FeatureChain *c=calloc(1,sizeof *c);c->f2.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;c->f2.features.robustBufferAccess=1;
    const char *ext[8];uint32_t n_ext=0,nqi=1;uint32_t RSF_EXT_CAPACITY=mode==9 ? 0:8;
    float priority=1;VkDeviceQueueCreateInfo qi[1]={{.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&priority}};
''' + block + r'''
    return 1;
}
int main(int argc,char **argv) {
    assert(argc==2);mode=atoi(argv[1]);assert(rsf_vk_prepare() && rsf_vk_prepare());
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};VkInstance instance;
    assert(rsf_vk_create_instance(&info,NULL,&instance)==VK_SUCCESS && native_instances==1);
    uint32_t count=0;assert(rsf_vk_enumerate_physical_devices(instance,&count,NULL)==VK_SUCCESS && count==1);
    g_vk.phys=(VkPhysicalDevice)3;assert(device_owner()==(mode!=10));
    if(mode==4 || mode==11) assert(xess_calls==1 && xess_instances==1 && xess_devices==1 && sl_calls==1 && !disabled);
    if(mode==0 || mode==3 || mode==6 || mode==7 || mode==8 || mode==9 || mode==10 || mode==14 || mode==15 || mode==16)
        assert(disabled && !rsf_requested());
    else assert(!disabled); // Optional SDK refusal preserves the other providers, including boot-off provisioning.
    return 0;
}
'''
    c = OUT / 'startup-fallback.c'
    c.write_text('// TOTK_RECOMP_' + 'GENERATED_FROM_GAME_CODE\n' + code, encoding='utf-8')
    exe = OUT / 'startup-fallback.exe'
    includes = ['-I', str(ROOT / 'runtime')]
    for d in ('runtime/orchestrator/include', 'runtime/contract/include', 'sdk/game/include'):
        includes += ['-I', str(ROOT / 'external/ReScaleFrame' / d)]
    subprocess.run(['C:/msys64/ucrt64/bin/clang.exe','-std=gnu11','-O2','-Wall','-Wextra','-Werror',*includes,
                    str(c),str(ROOT/'runtime/rsf_vulkan.c'),'-o',str(exe)],check=True)
    for mode in range(17):
        subprocess.run([str(exe),str(mode)],check=True)
    print('Actual startup fallback, native feature restoration and boot-off provider provisioning passed (CPU only)')


if __name__ == '__main__':
    main()
    startup_checks()
