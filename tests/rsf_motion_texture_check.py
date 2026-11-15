"""Actual pool, snapshot producer/consumer and native-pass hooks with a real worker and synthetic GPU commands."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build/checks/rsf-texture-caller-worker'
out.mkdir(parents=True, exist_ok=True)
source = (root / 'runtime/vk_exec.c').read_text()


def function(name):
    start = source.index(name)
    return source[start:source.index('\n}\n', start) + 3]


fixture = r'''
#define SNAPSHOT_NO_MAIN
#include "tests/rsf_snapshot_pool_check.c"
#include "runtime/vk_motion_texture.c"
static uint64_t completed;
static char events[64]; static int nevents;
static int native_pixel, copied_pixel[128];
uint64_t vk_sync_fence_done(void) { return completed; }
void rt_fatal(const char *s, ...) { (void)s; abort(); }
VkSnapshotPool *vk_snapshot_pool_start_vulkan(const VkSnapshotLimits *limits) {
    (void)limits; return test_pool(128,128,8);
}
void rc_vkCmdEndRendering(VkCommandBuffer cb) { assert(cb); events[nevents++]='E'; }
void rc_vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    assert(cb && ri->colorAttachmentCount==1 && ri->pColorAttachments[0].imageView==(VkImageView)34);
    assert(ri->pColorAttachments[0].loadOp==VK_ATTACHMENT_LOAD_OP_LOAD);
    assert(ri->pColorAttachments[0].storeOp==VK_ATTACHMENT_STORE_OP_STORE); events[nevents++]='R';
}
void rc_vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di) {
    assert(cb && di->imageMemoryBarrierCount==2); events[nevents++]='B';
}
void rc_vkCmdCopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl,
                       uint32_t n, const VkImageCopy *regions) {
    assert(cb && s==(VkImage)1000 && sl==VK_IMAGE_LAYOUT_GENERAL && dl==VK_IMAGE_LAYOUT_GENERAL);
    assert(n==1 && regions[0].extent.width==4 && regions[0].extent.height==4);
    copied_pixel[(uintptr_t)d]=native_pixel; events[nevents++]='C';
}
'''
fixture += function('static uint64_t native_signature(')
fixture += function('int vk_exec_native_suspend(')
fixture += function('int vk_exec_native_resume(')
fixture += r'''
int main(void) {
    recording=pthread_self(); assert(vk_motion_texture_start(1));
    VkTexImage color={.image=(VkImage)2000};
    VkExec ex={.cb=(VkCommandBuffer)1,.fence=41,.source_frame=1,.in_pass=1,.rt_resolved=1,
        .pass_w=4,.pass_h=4,.pass_layers=1,.pass_ncolor=1,.pass_samples=1,.oq_open=-1};
    ex.color[0].image=color.image; ex.color[0].img=&color; ex.color[0].layers=1;
    ex.resolved_native_colors[0]=(VkImageView)34;
    VkMotionTextureFact fact={(VkImage)1000,1,{VK_FORMAT_R8G8B8A8_UNORM,4,4,1}};
    VkSampler sampler=(VkSampler)7; VkDescriptorImageInfo old;
    native_pixel=11;
    assert(!vk_motion_texture_capture(&ex,123,&fact,sampler)); wait_shape(M.pool,fact.shape,READY);
    assert(vk_motion_texture_capture(&ex,123,&fact,sampler)); events[nevents++]='D';
    assert(nevents==6 && !memcmp(events,"EBCBRD",6));
    assert(native_pixel==11 && copied_pixel[1]==11 && ex.in_pass);
    assert(!vk_motion_texture_previous(&ex,123,&fact,sampler,&old));
    vk_motion_texture_submitted(ex.cb,91); // actual final fence is not source 1 or ex.fence 41
    ex.cb=(VkCommandBuffer)2; ex.fence=42; ex.source_frame=2; native_pixel=22;
    assert(!vk_motion_texture_capture(&ex,123,&fact,sampler)); wait_shape(M.pool,fact.shape,READY);
    assert(vk_motion_texture_capture(&ex,123,&fact,sampler));
    assert(vk_motion_texture_previous(&ex,123,&fact,sampler,&old));
    assert(old.imageView==(VkImageView)1 && copied_pixel[(uintptr_t)old.imageView]==11);
    assert(copied_pixel[2]==22 && native_pixel==22);
    fact.generation++; assert(!vk_motion_texture_previous(&ex,123,&fact,sampler,&old)); fact.generation--;
    assert(!vk_motion_texture_previous(&ex,123,&fact,(VkSampler)8,&old));
    fact.shape.width++; assert(!vk_motion_texture_previous(&ex,123,&fact,sampler,&old)); fact.shape.width--;
    assert(!vk_motion_texture_capture(&ex,123,&fact,sampler)); // duplicate draw identity is ambiguous
    assert(!vk_motion_texture_previous(&ex,123,&fact,sampler,&old));
    vk_motion_texture_submitted(ex.cb,92); completed=91; assert(!vk_motion_texture_stop());
    completed=92; assert(vk_motion_texture_stop()); assert(created==destroyed);
    // Exact native pass identity: no callbacks, no view lookup; changed state refuses without reopening.
    VkNativePass pass; ex.in_pass=1; ex.source_frame=3;
    assert(vk_exec_native_suspend(&ex,&pass)); ex.tex_serial++;
    assert(!vk_exec_native_resume(&ex,&pass)); ex.tex_serial--; assert(vk_exec_native_resume(&ex,&pass));
    assert(!vk_exec_native_resume(&ex,&pass));
    ex.nfbl=1; assert(!vk_exec_native_suspend(&ex,&pass)); ex.nfbl=0;
    ex.oq_open=0; assert(!vk_exec_native_suspend(&ex,&pass)); ex.oq_open=-1;
    ex.chunked=1; assert(!vk_exec_native_suspend(&ex,&pass));
    return 0;
}
'''
(out / 'caller.c').write_text(fixture)
subprocess.run(['C:/msys64/ucrt64/bin/clang.exe', '-std=gnu11', '-O1', '-Wall', '-Wextra',
    '-Werror', '-Wno-unused-function', '-I', str(root), '-I', str(root / 'runtime'),
    '-I', 'C:/msys64/ucrt64/include/vma', '-I', str(root / 'external/ReScaleFrame/runtime/orchestrator/include'),
    '-I', str(root / 'external/ReScaleFrame/runtime/contract/include'), '-I', str(root / 'external/ReScaleFrame/sdk/game/include'),
    str(out / 'caller.c'), '-l:libwinpthread.a', '-o', str(out / 'caller.exe')], check=True)
subprocess.run([str(out / 'caller.exe')], check=True)
print('actual async pool, copy/read adjacency, old/new texture contents, exact resume and final-fence retirement passed')
