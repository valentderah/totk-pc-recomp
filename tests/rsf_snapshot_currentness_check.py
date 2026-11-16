"""Exercise real snapshot begin/read/end functions across reuse, without a device."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build/checks/rsf-storage-rebase-worker'
source = (root / 'runtime/vk_usnap.c').read_text()


def extract_region(source, begin, end):
    start = source.index(begin)
    return source[start:source.index(end, start)]


def extract_function(source, name):
    start = source.index(name)
    return source[start:source.index('\n}\n', start) + 3]


fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t VkBuffer;
typedef uint64_t VmaAllocation;
typedef struct { uint64_t buffer, offset, range; } VkDescriptorBufferInfo;
typedef struct { int alloc, pool, cls; uint64_t avail; } VkMemSpan;
#define ALIGN 256u
#define VK_MCLS_HOST_CACHED 1
#define VK_MCLS_HOST_UNC 2
#define RT_KNOB_ON(mode) (mode)
'''
start = source.index('typedef struct Arena {')
fixture += source[start:source.index('} U;', start) + 4]
fixture += r'''
static Arena arena;
static int temporal = 1, completions;
static uint8_t guest[256] __attribute__((aligned(256)));
static Rng collected[1];
static Reg regions[1];
static int rt_knob(const char *n, const char *v) { (void)n; (void)v; return 1; }
static void summary(void) {}
static uint64_t kern_now_ns(void) { return 1; }
static const void *nvn_op_first(uint64_t h) { (void)h; return NULL; }
static void collect(const void *h, int depth) {
    (void)h; (void)depth;
    U.col = collected; U.ncol = 1;
    collected[0].lo = (uintptr_t)guest; collected[0].hi = (uintptr_t)guest + sizeof guest;
}
static void sort_col(void) {}
static void wr_prune(void) {}
static int wr_hits(uint64_t a, uint64_t b) { (void)a; (void)b; return 0; }
static int vk_mem_resolve(uint64_t a, uint64_t n, VkMemSpan *sp) {
    assert(a == (uintptr_t)guest && n == sizeof guest);
    *sp = (VkMemSpan){.pool = 1, .cls = VK_MCLS_HOST_CACHED, .avail = sizeof guest};
    return 1;
}
static void *grow(void *p, int *cap, size_t size) { (void)p; (void)cap; (void)size; abort(); }
static Arena *arena_get(uint64_t n) { assert(n == sizeof guest); return &arena; }
static int vk_dlaa_temporal_available(void) { return temporal; }
static void rt_fatal(const char *s) { (void)s; abort(); }
typedef struct { int vma; } Ctx;
static Ctx *vk_ctx(void) { static Ctx ctx; return &ctx; }
static void vmaFlushAllocation(int vma, VmaAllocation a, uint64_t off, uint64_t n) {
    (void)vma; (void)a; (void)off; assert(n == sizeof guest);
}
static void arena_done(void *arg) { assert(arg == &arena); completions++; }
static void vk_completion_add(uint64_t fence, void (*done)(void *), void *arg) {
    assert(fence); done(arg);
}
'''
for name in ('void vk_usnap_begin(', 'void vk_usnap_end(', 'static int first_above(',
             'int vk_usnap_find('):
    fixture += extract_function(source, name)
start = source.index('static uint8_t *host_copy(')
fixture += source[start:source.index('// The upload made', start)]
fixture += r'''
int main(void) {
    uint8_t mapped[256]; arena.map = mapped; arena.size = sizeof mapped;
    U.reg = regions; U.creg = 1;
    uint64_t handle = 1;
    memset(guest, 0x11, sizeof guest);
    vk_usnap_begin(&handle, 1, 1);
    assert(arena.cpu && arena.cpu_current);
    assert(vk_usnap_map() == arena.cpu && vk_usnap_bytes((uintptr_t)guest, 8)[0] == 0x11);
    assert(!memcmp(mapped, guest, sizeof guest));
    VkDescriptorBufferInfo bi;
    regions[0].drop_at = 12;
    assert(vk_usnap_find((uintptr_t)guest, 8, 11, 1, &bi));
    assert(!vk_usnap_find((uintptr_t)guest, 8, 12, 1, &bi));
    vk_usnap_end(1);
    temporal = 0; memset(guest, 0x22, sizeof guest);
    vk_usnap_begin(&handle, 1, 2); // Same arena still owns an allocated, stale CPU shadow.
    assert(arena.cpu[0] == 0x11 && !arena.cpu_current);
    assert(vk_usnap_map() == mapped && vk_usnap_bytes((uintptr_t)guest, 8)[0] == 0x22);
    vk_usnap_end(2);
    temporal = 1; memset(guest, 0x33, sizeof guest);
    vk_usnap_begin(&handle, 1, 3);
    assert(arena.cpu_current && vk_usnap_map() == arena.cpu);
    assert(vk_usnap_bytes((uintptr_t)guest, 8)[0] == 0x33 && mapped[0] == 0x33);
    vk_usnap_end(3);
    assert(completions == 3 && !vk_usnap_map());
    free(arena.cpu);
    return 0;
}
'''
if __name__ == '__main__':
    out.mkdir(parents=True, exist_ok=True)
    (out / 'snapshot-currentness.c').write_text(fixture)
    subprocess.run(['C:/msys64/ucrt64/bin/clang.exe', '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', str(out / 'snapshot-currentness.c'), '-o',
                    str(out / 'snapshot-currentness.exe')], check=True)
    subprocess.run([str(out / 'snapshot-currentness.exe')], check=True)
    print('snapshot arena reuse with temporal on/off/on and CPU read currentness passed')
