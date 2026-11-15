"""CPU fixture compiling the actual FG preparation function with inert GPU calls."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / "build/checks/rsf-resize-worker"
out.mkdir(parents=True, exist_ok=True)
source = (root / "runtime/vk_dlaa.c").read_text(encoding="utf-8")
start = source.index("static void fg_prepare(")
end = source.index("\nstatic int rsf_scene(", start)
harness = "// " + ("TOTK_RECOMP_" + "GENERATED_FROM_GAME_CODE") + "\n"
harness += r'''#include <assert.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <rescaleframe/native_fg_vulkan.h>
typedef struct { VkCommandBuffer cb; uint64_t source_frame; } VkExec;
struct Size { unsigned w,h,ow,oh,fw,fh; VkImage device_depth,mv; rsf_frame_record fg_record; };
static unsigned calls;
static int refused;
#define cfg_int(...) 3
#define rt_log(...) ((void)0)
#define rec_drain() ((void)0)
#define vk_exec_draw_new_cb(...) ((void)0)
#define rsf_layout(...) ((void)0)
static rsf_srvk_image rsf_image(VkImage image, VkFormat format, unsigned w, unsigned h, VkImageLayout layout) {
    return (rsf_srvk_image){0, (uint64_t)image, format, w,h,layout,1};
}
static int vk_present_fg_prepare(VkCommandBuffer cb, const rsf_fgvk_frame *f) {
    (void)cb; ++calls;
    assert(f->record->output_width == 2560 && f->record->output_height == 1440);
    assert(f->record->camera.output_width == 2560 && f->record->camera.output_height == 1440);
    assert(f->record->render_width == 1600 && f->record->camera.render_width == 1600);
    assert(f->depth.width == 1600 && f->motion.height == 900);
    assert(f->motion_scale_x == 1 && f->motion_scale_y == 1);
    return refused;
}
'''
harness += source[start:end]
harness += r'''
int main(void) {
    VkExec ex = {.source_frame = 7};
    struct Size e = {.w=1600,.h=900,.ow=1920,.oh=1080,.fw=2560,.fh=1440};
    rsf_frame_record record = {.frame_id=7,.render_width=1600,.render_height=900,
        .output_width=1920,.output_height=1080,.camera={.render_width=1600,.render_height=900,
        .output_width=1920,.output_height=1080}};
    fg_prepare(&ex,&e,&record);
    assert(calls==1 && e.fg_record.frame_id==7 && e.fg_record.output_width==2560);
    assert(record.output_width==1920 && record.camera.output_width==1920);
    refused=-1; fg_prepare(&ex,&e,&record);
    assert(calls==2 && !e.fg_record.frame_id);
    e.fw=0; fg_prepare(&ex,&e,&record); assert(calls==2);
    return 0;
}
'''
fixture = out / "prepare.c"
fixture.write_text(harness, encoding="utf-8")
rsf = root.parent / "ReScaleFrame"
command = ["C:/msys64/ucrt64/bin/clang.exe", str(fixture), "-o", str(out / "prepare.exe")]
for directory in ("runtime/orchestrator/include", "runtime/contract/include", "sdk/game/include"):
    command += ["-I", str(rsf / directory)]
subprocess.run(command, check=True)
subprocess.run([str(out / "prepare.exe")], check=True)
print("FG resize preparation: passed")
