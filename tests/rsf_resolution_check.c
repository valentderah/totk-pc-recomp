// Exercise the real resolution mailbox and game-thread application with host stubs.
#include <assert.h>
#include "../runtime/resolution.c"

const int rt_game_version = 143;
volatile int g_fnprof;
static unsigned settings_version = 1, render_calls;
static unsigned char patch_page[64];
// Other resolution entry points are linked too on Windows. None may run in this fixture.
const char *cfg_get(const char *name) { (void)name; abort(); }
const char *cfg_value(const char *name) { (void)name; abort(); }
int cfg_int(const char *name) { (void)name; abort(); }
int cfg_bool(const char *name) { (void)name; abort(); }
int rt_patch_set(const char *name) { (void)name; abort(); }
void rt_patch_switch(int set, int on) { (void)set; (void)on; abort(); }
void fs_init(void) { abort(); }
int romfs_shaped_ready(unsigned w, unsigned h) { (void)w; (void)h; abort(); }
int plat_file_exists(const char *path) { (void)path; abort(); }
uint8_t *plat_file_read(const char *path, uint64_t *size) { (void)path; (void)size; abort(); }
void mods_peek_romfs(void (*callback)(const char *, const char *, void *), void *context) {
    (void)callback; (void)context; abort();
}
unsigned cfg_version(void) { return settings_version; }
int ov_handheld(void) { return 0; }
void *rt_patch_page(void) { return patch_page; }
void rt_log(const char *format, ...) { (void)format; }
uint64_t rt_call(Cpu *cpu, uint64_t function, int count, ...) {
    assert(function == SET_RENDER_SIZE && count == 2);
    ++render_calls;
    memset(cpu->x, 0xc5, sizeof cpu->x);
    memset(cpu->v, 0xc5, sizeof cpu->v);
    cpu->nzcv = cpu->fpsr = 0;
    return 0;
}

int main(void) {
    R.on = R.live = 1;
    R.ow = R.hw = 1920;
    R.oh = R.hh = 1080;
    R.sw = 1280; R.sh = 720;
    renderer = 1;
    Cpu cpu = {0};
    cpu.x[0] = 123;
    cpu.nzcv = 7; cpu.fpsr = 8;
    Cpu before = cpu;
    assert(res_rsf_scene(1920, 1080));
    assert(R.sw == 1280 && !render_calls); // Producer never calls guest code.
    res_present(&cpu);
    assert(render_calls == 1 && R.sw == 1920 && R.sh == 1080);
    assert(!memcmp(&before, &cpu, sizeof cpu));
    float scene[2];
    memcpy(scene, patch_page + 16, sizeof scene);
    assert(scene[0] == 1920 && scene[1] == 1080);
    assert(res_rsf_scene(1280, 720));
    settings_version++;
    res_present(&cpu);
    assert(render_calls == 1 && R.sw == 1920); // Stale settings cannot resize the game.
    assert(!res_rsf_scene(1921, 1080));
    assert(!res_rsf_scene(1920, 1081));
    assert(!res_rsf_scene(0, 720));
    assert(res_rsf_scene(1920, 1080));
    res_present(&cpu);
    assert(render_calls == 1); // Identical negotiated dimensions need no rebuild.
    R.live = 0;
    assert(!res_rsf_scene(1280, 720));
    return 0;
}
