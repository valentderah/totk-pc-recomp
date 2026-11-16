"""Compile the actual temporal frame boundary and exercise live history invalidation."""
import argparse
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', default=shutil.which('clang') or 'C:/msys64/ucrt64/bin/clang.exe')
    args = ap.parse_args()
    text = (ROOT / 'runtime/vk_dlaa.c').read_text(encoding='utf-8')
    start = text.index('void vk_dlaa_frame(void) {')
    end = text.index('// ---------------------------------------------------------------- the game', start)
    actual = text[start:end]
    fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define VK_NULL_HANDLE ((void *)0)
#define rt_log(...) ((void)0)
static void *vk_dlaa_swap_img, *vk_dlaa_swap_view;
static int temporal_available = 1, selected = 1, changed, quality_changed, dynres;
static struct {
    int fg_on, preset_dirty, reset, failed, jit_ok, jit_used, cam_prev_ok;
    int on, armed, done, gbuf, ended, up, hold, frames_missed, holds, ids_ok;
    void *jdepth, *nodepth;
    uint64_t cam_addr;
} D;
static void producer_trace_frame(void) {}
static int rsf_config_refresh(void) { int result = changed; changed = 0; return result; }
static int rsf_config_fg_backend(void) { return 0; }
static void res_dynres_live(void) { dynres++; }
static void dlss_mode(void) { if (quality_changed) { D.preset_dirty = 1; quality_changed = 0; } }
static void preset_check(void) {}
static int temporal_wanted(void) { return selected; }
static int dl_active(void) { return temporal_available && selected; }
''' + actual + r'''
int main(void) {
    vk_dlaa_swap_img = vk_dlaa_swap_view = (void *)1;
    D.on = D.done = 1; D.jdepth = (void *)1;
    vk_dlaa_frame();
    assert(!vk_dlaa_swap_img && !vk_dlaa_swap_view);
    assert(D.jdepth == (void *)1 && !D.preset_dirty); // ordinary frames retain their depth association
    D.done = 1; quality_changed = 1;
    vk_dlaa_frame();
    assert(!D.jdepth && D.preset_dirty); // resized depth must accept fresh camera bindings
    D.preset_dirty = 0; D.jdepth = (void *)2; D.failed = D.jit_ok = D.jit_used = D.cam_prev_ok = 1;
    changed = 1; selected = 0;
    vk_dlaa_frame();
    assert(!D.on && !D.failed && !D.jdepth && !D.jit_ok && !D.cam_prev_ok && D.reset && dynres == 1);
    D.preset_dirty = 0; changed = 1; selected = 1;
    vk_dlaa_frame();
    assert(D.on && D.preset_dirty && !D.jdepth && dynres == 2);
    puts("RSF live frame: SR transitions and quality changes reset stale depth/camera associations");
    return 0;
}
'''
    out = ROOT / 'build/checks/rsf-live'
    out.mkdir(parents=True, exist_ok=True)
    source = out / 'frame-history.c'
    source.write_text('// TOTK_RECOMP_' + 'GENERATED_FROM_GAME_CODE\n' + fixture, encoding='utf-8')
    exe = out / 'frame-history.exe'
    subprocess.run([args.cc, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
