"""Combine actual snapshot/lease/history functions with synthetic uploads and delayed fences."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
from rsf_snapshot_currentness_check import fixture as base_fixture, extract_function
fixture = base_fixture.split('int main(void) {')[0]
snapshot = (root / 'runtime/vk_usnap.c').read_text()
history = (root / 'runtime/vk_mvprev.c').read_text()




fixture = fixture.replace('static Arena arena;', 'static Arena arena, second;')
fixture = fixture.replace('static Arena *arena_get(uint64_t n) { assert(n == sizeof guest); return &arena; }', r'''
static Arena *arena_get(uint64_t n) {
    assert(n == sizeof guest);
    Arena *a = !arena.refs ? &arena : &second;
    assert(!a->refs); // Never reuse an arena retained by history or unfinished GPU consumption.
    a->refs = 1; a->use_fence = 0; return a;
}''')
start = fixture.index('static void arena_done(')
end = fixture.index('void vk_usnap_begin(', start)
fixture = fixture[:start] + r'''
static Arena *free_arenas;
static int free_lock;
static void plat_mutex_lock(int *lock) { (void)lock; }
static void plat_mutex_unlock(int *lock) { (void)lock; }
static struct { uint64_t fence; void (*done)(void *); void *arg; } pending[32];
static int npending;
static void vk_completion_add(uint64_t fence, void (*done)(void *), void *arg) {
    pending[npending++] = (__typeof__(pending[0])){fence, done, arg};
}
static void complete(uint64_t fence) {
    for (int i = 0; i < npending; i++)
        if (pending[i].fence <= fence && pending[i].done) {
            pending[i].done(pending[i].arg); pending[i].done = NULL; completions++;
        }
}
''' + extract_function(snapshot, 'static void arena_done(') + fixture[end:]
fixture += extract_function(snapshot, 'void *vk_usnap_retain(')
fixture += 'void vk_usnap_release(void *lease) { if (lease) arena_done(lease); }\n'
fixture += extract_function(snapshot, 'void vk_usnap_use(')
fixture += r'''
#include <math.h>
#define VK_FORMAT_D32_SFLOAT 126
#define DLAA_SCENE_MIN_W 640
#define MVPREV_NONE 0
#define MVPREV_SELF 1
#define MVPREV_SIDE 2
#define MVPREV_NEARER 3
typedef struct { uint64_t addr, size; } p_bind_buf;
typedef struct { uint64_t image; int width, format; } VkExecTarget;
typedef struct { VkExecTarget depth; p_bind_buf ubo[6][18]; } VkExec;
static uint64_t vk_usnap_gen(void) { return U.gen; }
static void rt_log(const char *format, ...) { (void)format; }
'''
fixture += history[history.index('#define CHECK'):]
fixture += r'''
int main(void) {
    uint8_t map1[256], map2[256];
    arena.map = map1; second.map = map2; arena.size = second.size = sizeof guest;
    arena.buf = 1; second.buf = 2;
    U.reg = regions; U.creg = 1;
    VkExec ex = {0}; ex.depth.image = 1; ex.depth.width = 640; ex.depth.format = 126;
    ex.ubo[0][BONES] = (p_bind_buf){(uintptr_t)guest, 128};
    uint64_t handle = 1;
    VkDescriptorBufferInfo previous; const uint8_t *bytes;
    vk_mvprev_frame();
    memset(guest, 0x11, sizeof guest);
    vk_usnap_begin(&handle, 1, 1); vk_mvprev_draw(&ex); vk_usnap_end(1);
    assert(arena.refs == 2); complete(1); assert(arena.refs == 1);
    vk_mvprev_frame();
    memset(guest, 0x22, sizeof guest); // Same guest bone address, different frame contents.
    vk_usnap_begin(&handle, 1, 2); vk_mvprev_draw(&ex);
    assert(vk_mvprev_self(BONES, (uintptr_t)guest, 128, 2, &previous, &bytes));
    assert(previous.buffer == 1 && previous.range == 128 && bytes[0] == 0x11);
    assert(vk_usnap_bytes((uintptr_t)guest, 128)[0] == 0x22);
    assert(map1[0] == 0x11 && map2[0] == 0x22); // Explicit uploaded bytes, not only CPU shadows.
    int count = npending;
    assert(vk_mvprev_self(BONES, (uintptr_t)guest, 128, 2, &previous, &bytes));
    assert(npending == count); // One consumer fence hold per arena/submission.
    assert(!vk_mvprev_self(BONES, (uintptr_t)guest, 64, 2, &previous, &bytes));
    assert(!vk_mvprev_self(BONES, (uintptr_t)guest, 129, 2, &previous, &bytes));
    assert(!vk_mvprev_self(BONES, (uintptr_t)guest+1, 128, 2, &previous, &bytes));
    assert(!vk_mvprev_self(BONES, (uintptr_t)guest, 0, 2, &previous, &bytes));
    assert(!vk_mvprev_self(BONES, (uintptr_t)guest, 128, 0, &previous, &bytes));
    vk_usnap_end(2); complete(2);
    vk_mvprev_frame(); // Previous now holds frame 2; frame 1's CPU lease retires.
    assert(!arena.refs && second.refs == 1);
    vk_usnap_begin(&handle, 1, 3); vk_mvprev_draw(&ex); // Unchanged contents, reused free arena.
    assert(vk_mvprev_self(BONES, (uintptr_t)guest, 128, 3, &previous, &bytes));
    assert(!memcmp(bytes, vk_usnap_bytes((uintptr_t)guest, 128), 128));
    vk_usnap_end(3);
    vk_mvprev_clear(); // Disable/reset before GPU work retires must not recycle either allocation.
    assert(arena.refs == 1 && second.refs == 1);
    assert(!vk_mvprev_self(BONES, (uintptr_t)guest, 128, 3, &previous, &bytes));
    complete(3); assert(!arena.refs && !second.refs && completions == 5);
    free(arena.cpu); free(second.cpu); free(P.t[0]); free(P.t[1]);
    free(P.bytes[0]); free(P.bytes[1]);
    return 0;
}
'''
if __name__ == '__main__':
    out = root / 'build/checks/rsf-retained-history-worker'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'retained.c').write_text(fixture)
    subprocess.run(['C:/msys64/ucrt64/bin/clang.exe', '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', str(out / 'retained.c'), '-o', str(out / 'retained.exe')], check=True)
    subprocess.run([str(out / 'retained.exe')], check=True)
    print('retained same-address history, uploaded bytes, unchanged/range cases and delayed retirement passed')
