"""Actual storage range/history/descriptor functions with distinct old/new pointees and delayed fences."""
from pathlib import Path
import runpy
import subprocess

root = Path(__file__).resolve().parents[1]
base = runpy.run_path(str(root / 'tests/rsf_retained_history_check.py'))
fixture = base['fixture'].split('int main(void) {')[0]
draw = (root / 'runtime/vk_exec_draw.c').read_text()
fixture = fixture.replace('assert(a == (uintptr_t)guest && n == sizeof guest);',
                          'assert(a >= (uintptr_t)guest && a < (uintptr_t)guest+256);')
fixture = fixture.replace('.avail = sizeof guest};', '.avail = n == 1 ? 64 : sizeof guest};')
fixture += r'''
typedef struct { int flags, cbuf_slot, handle; } GfxShaderRes;
typedef struct { GfxShaderRes resource; } GfxShader;
#define GFX_RES_SSBO 1
static int gfx_shader_resources(const GfxShader *vs, int kind, const GfxShaderRes **r) {
    assert(kind == GFX_RES_SSBO); *r = &vs->resource; return 1;
}
#define NVN_STAGE_VS 0
static struct { uint64_t sbo_max, sbo_align; } LIM = {64, 16};
static struct { uint64_t fence; } MQ = {2};
static p_bind_buf bound;
static const p_bind_buf *ssbo_bound(VkExec *ex, int st, const GfxShaderRes *r, p_bind_buf *up, int *ubo) {
    (void)ex; (void)st; (void)r; (void)up; *ubo = 0; return &bound;
}
static int ubo_ptr_res(const GfxShaderRes *r) { return r->cbuf_slot >= 3 && r->cbuf_slot <= 20; }
static uint64_t sbuf_lead(uint64_t addr) { return addr & 15; }
'''
for name in ('static int mo_storage_range(', 'static int mo_storage_key(', 'static void mo_storage_capture(', 'static int mo_storage_previous('):
    # mo_storage_key is deliberately a one-line helper.
    start = draw.index(name)
    end = draw.index('\n', start) + 1 if name.endswith('key(') else draw.index('\n}\n', start) + 3
    fixture += draw[start:end]
fixture += r'''
int main(void) {
    uint8_t map1[256], map2[256]; arena.map = map1; second.map = map2;
    arena.size = second.size = 256; arena.buf = 1; second.buf = 2;
    U.reg = regions; U.creg = 1;
    VkExec ex = {0}; uint64_t handle = 1, identity = (uintptr_t)guest+240;
    ex.ubo[0][3].addr = identity; // c6 pointer owner stays put; its storage pointee moves.
    GfxShaderRes current = {.cbuf_slot = 6}, previous = {.cbuf_slot = 70};
    GfxShader vs = {.resource = current};
    uint64_t id, addr, size; VkDescriptorBufferInfo old;
    vk_mvprev_frame(); memset(guest, 0x11, 256);
    uint64_t pointer = (uintptr_t)guest;
    memcpy(guest+240, &pointer, 8);
    vk_usnap_begin(&handle, 1, 1);
    bound.size = 64; memcpy(&bound.addr, vk_usnap_bytes(identity, 8), 8);
    assert(mo_storage_range(&ex, &current, &id, &addr, &size));
    mo_storage_capture(&ex, &vs);
    vk_usnap_end(1); complete(1); assert(arena.refs == 1);
    vk_mvprev_frame(); memset(guest, 0x22, 256);
    pointer = (uintptr_t)guest+128; memcpy(guest+240, &pointer, 8);
    vk_usnap_begin(&handle, 1, 2);
    memcpy(&bound.addr, vk_usnap_bytes(identity, 8), 8);
    assert(mo_storage_range(&ex, &current, &id, &addr, &size));
    assert(id == identity && addr == bound.addr && size == 64);
    mo_storage_capture(&ex, &vs);
    assert(mo_storage_previous(&ex, &previous, &old));
    assert(old.buffer == 1 && old.offset == 0 && old.range == 64 && map1[old.offset] == 0x11);
    memcpy(&pointer, map1+240, 8); assert(pointer == (uintptr_t)guest);
    assert(vk_usnap_bytes(bound.addr, 64)[0] == 0x22 && map2[128] == 0x22);
    int held = npending;
    assert(mo_storage_previous(&ex, &previous, &old) && npending == held);
    bound.size = 32; assert(!mo_storage_previous(&ex, &previous, &old));
    bound.size = 64; bound.addr++; assert(!mo_storage_previous(&ex, &previous, &old)); bound.addr--;
    previous.flags = 1; assert(!mo_storage_previous(&ex, &previous, &old)); previous.flags = 0;
    ex.ubo[0][3].addr++; assert(!mo_storage_previous(&ex, &previous, &old)); ex.ubo[0][3].addr--;
    vk_usnap_end(2); vk_mvprev_clear();
    assert(arena.refs == 1 && second.refs == 1); // Pending consumers and original upload still own allocations.
    assert(!mo_storage_previous(&ex, &previous, &old));
    complete(2); assert(!arena.refs && !second.refs);
    // A range whose snapshot was dropped must not become next frame's previous storage.
    vk_usnap_begin(&handle, 1, 3); mo_storage_capture(&ex, &vs);
    regions[0].drop_at = 0; U.gen++;
    mo_storage_capture(&ex, &vs); vk_usnap_end(3); complete(3);
    vk_mvprev_frame(); MQ.fence = 4;
    assert(!mo_storage_previous(&ex, &previous, &old));
    vk_mvprev_clear();
    free(arena.cpu); free(second.cpu); free(P.t[0]); free(P.t[1]); free(P.bytes[0]); free(P.bytes[1]);
    return 0;
}
'''
out = root / 'build/checks/rsf-storage-variant-worker'
out.mkdir(parents=True, exist_ok=True)
(out / 'previous-storage.c').write_text(fixture)
subprocess.run(['C:/msys64/ucrt64/bin/clang.exe', '-std=gnu11', '-Wall', '-Wextra',
                '-Werror', str(out / 'previous-storage.c'), '-o', str(out / 'previous-storage.exe')], check=True)
subprocess.run([str(out / 'previous-storage.exe')], check=True)
print('previous storage address/content, ranges, leases, alignment and missing-history refusals passed')
