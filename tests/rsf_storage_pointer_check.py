"""Compile the real storage pointer resolver with synthetic submission snapshots.

Run with .venv/Scripts/python tests/rsf_storage_pointer_check.py. No Vulkan device.
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build/checks/rsf-storage-worker'
out.mkdir(parents=True, exist_ok=True)
source = (root / 'runtime/vk_exec_draw.c').read_text()
def function(name):
    start = source.index(name)
    return source[start:source.index('\n}\n', start) + 3]
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef struct { uint64_t addr, size; } p_bind_buf;
typedef struct { uint64_t lo, hi; const uint8_t *data; } VkExecUpd;
#define UPD_N 4096
typedef struct { uint64_t fence; uint32_t n; VkExecUpd u[UPD_N]; } UpdRing;
typedef struct { UpdRing UPD; } VkExecDraw;
typedef struct {
    p_bind_buf ubo[6][18]; int chunked; uint32_t drop_n, upd_n;
    uint64_t fence; VkExecDraw *draw; const VkExecUpd *upd;
} VkExec;
typedef struct { int cbuf_slot, handle; } GfxShaderRes;
typedef struct { uint64_t offset; } VkDescriptorBufferInfo;
typedef struct { int unused; } VkMemSpan;
static uint64_t snap_value = 0x1122334455667788;
static int snapshot = 1, backed = 1;
static uint64_t seen_at;
static uint64_t drop_at = UINT64_MAX;
static int vk_usnap_find(uint64_t a, uint64_t n, uint64_t at, uint64_t align,
                         VkDescriptorBufferInfo *b) {
    (void)a; assert(n == 8 && align == 1); seen_at = at; b->offset = 0;
    return snapshot && at < drop_at;
}
static const uint8_t *vk_usnap_map(void) { return (const uint8_t *)&snap_value; }
static int vk_mem_resolve(uint64_t a, uint64_t n, VkMemSpan *sp) {
    (void)a; (void)n; (void)sp; return backed;
}
'''
fixture += function('static int upd_read(')
fixture += function('static void upd_apply(')
fixture += function('static uint64_t ubo_ptr(')
fixture += r'''
int main(void) {
    uint64_t live[2] = {0, 0x9988776600dead00};
    VkExecDraw draw = {0};
    VkExec ex = {.draw = &draw, .fence = 9};
    draw.UPD.fence = 9;
    GfxShaderRes r = {.cbuf_slot = 5, .handle = 2};
    ex.ubo[0][2] = (p_bind_buf){(uintptr_t)live, sizeof live};
    assert(ubo_ptr(&ex, 0, &r) == snap_value); // Live memory belongs to the next frame.
    assert(seen_at == UINT32_MAX);
    ex.chunked = 1; ex.drop_n = 17;
    assert(ubo_ptr(&ex, 0, &r) == snap_value && seen_at == 17);
    ex.chunked = 0;
    uint64_t a = (uintptr_t)&live[1], whole = 0xabcdef1234567890;
    uint32_t low = 0xaabbccdd, high = 0xeeff0011, newer = 0x76543210;
    draw.UPD.u[0] = (VkExecUpd){a, a+4, (uint8_t *)&low}; draw.UPD.n = 1;
    assert(ubo_ptr(&ex, 0, &r) == 0x11223344aabbccdd); // Partial update preserves snapshot high word.
    draw.UPD.u[1] = (VkExecUpd){a+4, a+8, (uint8_t *)&high}; draw.UPD.n = 2;
    snapshot = 0; backed = 0;
    assert(ubo_ptr(&ex, 0, &r) == 0xeeff0011aabbccdd); // Split updates provide all bytes without a base.
    draw.UPD.u[2] = (VkExecUpd){a, a+4, (uint8_t *)&newer}; draw.UPD.n = 3;
    assert(ubo_ptr(&ex, 0, &r) == 0xeeff001176543210);
    draw.UPD.u[3] = (VkExecUpd){a, a+8, (uint8_t *)&whole}; draw.UPD.n = 4;
    assert(ubo_ptr(&ex, 0, &r) == whole); // Whole update works with neither backing nor snapshot.
    draw.UPD.u[4] = (VkExecUpd){a+4, a+8, (uint8_t *)&high}; draw.UPD.n = 5;
    assert(ubo_ptr(&ex, 0, &r) == 0xeeff001134567890); // New partial overrides old whole.
    // Chunk scan contains future updates; only upd_n preceding this draw may participate.
    ex.chunked = 1; ex.upd = draw.UPD.u; ex.upd_n = 1; ex.drop_n = 23;
    snapshot = 1;
    assert(ubo_ptr(&ex, 0, &r) == 0x11223344aabbccdd && seen_at == 23);
    drop_at = 24; ex.drop_n = 24; backed = 1;
    assert(ubo_ptr(&ex, 0, &r) == 0x99887766aabbccdd && seen_at == 24);
    drop_at = UINT64_MAX;
    ex.upd_n = 3;
    assert(ubo_ptr(&ex, 0, &r) == 0xeeff001176543210);
    // Overlapping 32-bit write across the low/high word boundary.
    ex.upd_n = 4; draw.UPD.u[3] = (VkExecUpd){a+2, a+6, (uint8_t *)&newer};
    assert(ubo_ptr(&ex, 0, &r) == 0xeeff765432103210);
    // A chunk's scan list is not a ring: retain early entries beyond UPD_N.
    VkExecUpd many[UPD_N+1] = {0}; many[0] = (VkExecUpd){a, a+8, (uint8_t *)&whole};
    ex.upd = many; ex.upd_n = UPD_N+1;
    assert(ubo_ptr(&ex, 0, &r) == whole);
    ex.chunked = 0; draw.UPD.n = 0; snapshot = 0; backed = 1;
    assert(ubo_ptr(&ex, 0, &r) == live[1]); // Dropped/disabled snapshot retains live path.
    draw.UPD.n = 1;
    assert(ubo_ptr(&ex, 0, &r) == 0x99887766aabbccdd); // Partial fallback overlays live high word.
    draw.UPD.fence = 8;
    assert(ubo_ptr(&ex, 0, &r) == live[1]); // Another submission's updates do not leak.
    backed = 0;
    assert(ubo_ptr(&ex, 0, &r) == 0);
    ex.ubo[0][2].size = 12;
    assert(ubo_ptr(&ex, 0, &r) == 0); // Pointer must fit the binding.
    return 0;
}
'''
(out / 'pointer.c').write_text(fixture)
subprocess.run(['C:/msys64/ucrt64/bin/clang.exe', '-std=gnu11', '-Wall', '-Wextra',
                '-Werror', str(out / 'pointer.c'), '-o', str(out / 'pointer.exe')], check=True)
subprocess.run([str(out / 'pointer.exe')], check=True)
print('storage pointer snapshots, split/overlapping updates, order, chunk prefix and fallback checks passed')
