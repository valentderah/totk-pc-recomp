"""Exercise actual motion cache functions with changing range and snapshot validity."""
from pathlib import Path
import subprocess

from rsf_snapshot_currentness_check import extract_function

root = Path(__file__).resolve().parents[1]
out = root / 'build/checks/rsf-storage-rebase-worker'
out.mkdir(parents=True, exist_ok=True)
source = (root / 'runtime/vk_exec_draw.c').read_text()




fixture = r'''
#include <stdint.h>
static uint64_t rt_fnv_mix(uint64_t h,uint64_t word) { return (h^word)*0x100000001b3ull; }
#include <assert.h>
typedef struct { uint64_t addr, size; } p_bind_buf;
typedef struct { uint64_t buffer, offset, range; } VkDescriptorBufferInfo;
typedef struct { p_bind_buf ubo[6][18]; } VkExec;
#define NVN_STAGE_VS 0
static uint64_t mo_gen = 1, snap_gen = 1;
static int calls, available = 1;
static uint64_t vk_usnap_gen(void) { return snap_gen; }
static int mo_prev_block(int index, const p_bind_buf *u, VkDescriptorBufferInfo *out, int *changed) {
    assert(index == 2); calls++; *changed = available;
    if (!available) return 0;
    *out = (VkDescriptorBufferInfo){123, u->addr, u->size}; return 1;
}
'''
fixture += extract_function(source, 'static int mo_prev_cached(')
fixture += extract_function(source, 'static uint64_t mo_blocks_key(')
fixture += r'''
int main(void) {
    VkExec ex = {0}; ex.ubo[0][2] = (p_bind_buf){0x1000, 128};
    p_bind_buf *bones = &ex.ubo[0][2];
    VkDescriptorBufferInfo out; int changed;
    uint32_t banks = (1u<<4) | (1u<<5);
    uint64_t original = mo_blocks_key(&ex, banks);
    assert(mo_prev_cached(2, bones, &out, &changed) && calls == 1);
    assert(mo_prev_cached(2, bones, &out, &changed) && calls == 1);
    bones->size = 256;
    assert(mo_blocks_key(&ex, banks) != original);
    assert(mo_prev_cached(2, bones, &out, &changed) && calls == 2 && out.range == 256);
    uint64_t resized = mo_blocks_key(&ex, banks);
    snap_gen++; available = 0; // The producer dropped this snapshot after a recorded write.
    assert(mo_blocks_key(&ex, banks) != resized);
    assert(!mo_prev_cached(2, bones, &out, &changed) && calls == 3 && !out.buffer);
    assert(!mo_prev_cached(2, bones, &out, &changed) && calls == 3);
    snap_gen++; available = 1; // New snapshot cannot retain the earlier cached refusal either.
    assert(mo_prev_cached(2, bones, &out, &changed) && calls == 4);
    mo_gen++;
    assert(mo_prev_cached(2, bones, &out, &changed) && calls == 5);
    return 0;
}
'''
(out / 'motion-cache.c').write_text(fixture)
subprocess.run(['C:/msys64/ucrt64/bin/clang.exe', '-std=gnu11', '-Wall', '-Wextra',
                '-Werror', str(out / 'motion-cache.c'), '-o', str(out / 'motion-cache.exe')], check=True)
subprocess.run([str(out / 'motion-cache.exe')], check=True)
print('motion cache range changes, snapshot drops/renewals and list ownership passed')
