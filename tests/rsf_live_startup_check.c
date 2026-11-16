// Optional SDK failures must not prevent provisioning the other providers for live switches.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <rescaleframe/native_latency_vulkan_requirements.h>
#include <rescaleframe/native_streamline_vulkan.h>
#include <rescaleframe/native_xess_vulkan_requirements.h>
#include "rsf_vulkan.h"

static int xess_init, sl_init, sl_refuse, xess_refuse = 1, disabled;
static VkPhysicalDeviceFeatures2 native_features;
static rsf_backend_result init_xess(const rsf_xessvk_setup *s) { assert(s->runtime_directory_utf8); xess_init++; return 0; }
static rsf_backend_result init_sl(const rsf_slvk_setup *s) {
    assert(s->features == (RSF_SLVK_DLSS_SR | RSF_SLVK_DLSS_FG));
    sl_init++; return sl_refuse ? RSF_BACKEND_ERROR_NOT_SUPPORTED : RSF_BACKEND_OK;
}
static rsf_backend_result require_xess(void *instance, void *physical, uint32_t *n, const char *const **names, void **features) {
    (void)instance; (void)physical;
    static const char *const extensions[] = {"XeSS_extension"};
    *n = 1; *names = extensions;
    ((VkPhysicalDeviceFeatures2 *)*features)->features.robustBufferAccess = VK_TRUE;
    return xess_refuse ? RSF_BACKEND_ERROR_NOT_SUPPORTED : RSF_BACKEND_OK;
}
static rsf_backend_result require_sl(const rsf_latencyvk_device_query *q, rsf_latencyvk_device_requirements *r) {
    assert(!memcmp(q->features, &native_features, sizeof native_features)); // refused XeSS changes were undone
    r->features = q->features;
    return 0;
}
static rsf_backend_result shutdown_sdk(void) { return 0; }
static rsf_vk_function instance_proc(void *instance, const char *name) { (void)instance; (void)name; return NULL; }
int rsf_requested(void) { return 0; } // boot with every feature off
int rsf_runtime_present(void) { return 1; }
const char *rsf_runtime_path(void) { return "test runtime"; }
int rsf_runtime_load(void) { return 1; }
const char *rsf_provider_directory(unsigned backend) { (void)backend; return "test provider"; }
void *rsf_runtime_symbol(const char *name) {
    if (!strcmp(name, "rsf_xessvk_initialize")) return init_xess;
    if (!strcmp(name, "rsf_slvk_initialize")) return init_sl;
    if (!strcmp(name, "rsf_latencyvk_instance_proc")) return instance_proc;
    if (!strcmp(name, "rsf_xessvk_device_requirements")) return require_xess;
    if (!strcmp(name, "rsf_latencyvk_require_device")) return require_sl;
    if (!strcmp(name, "rsf_xessvk_shutdown") || !strcmp(name, "rsf_latencyvk_shutdown")) return shutdown_sdk;
    return NULL;
}
void *rsf_runtime_execution_sink(void) { return NULL; }
void rsf_runtime_disable(const char *reason) { (void)reason; disabled++; }
unsigned rt_trace;
void rt_log(const char *format, ...) { (void)format; }

int main(void) {
    native_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    assert(rsf_vk_prepare() && xess_init == 1 && sl_init == 1 && !disabled);
    VkPhysicalDeviceFeatures2 features = native_features;
    void *enabled = &features;
    const char *extensions[8]; uint32_t count = 0;
    assert(rsf_vk_device_requirements(NULL, &features, sizeof features, extensions, &count, 8, &enabled));
    assert(!memcmp(&features, &native_features, sizeof features) && enabled == &features && !count);
    assert(rsf_vk_shutdown()); rsf_vk_reset();
    sl_refuse = 1; xess_refuse = 0;
    assert(rsf_vk_prepare() && xess_init == 2 && sl_init == 2 && !disabled);
    assert(rsf_vk_device_requirements(NULL, &features, sizeof features, extensions, &count, 8, &enabled));
    assert(features.features.robustBufferAccess && count == 1 && !strcmp(extensions[0], "XeSS_extension"));
    assert(rsf_vk_shutdown());
    puts("RSF live startup: boot-off provisioning and independent optional SDK refusal passed");
    return 0;
}
