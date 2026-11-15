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
static int cfg_int(const char *key) { (void)key; return 1; }
static int cfg_bool(const char *key) { (void)key; return 1; }
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


if __name__ == '__main__':
    main()
