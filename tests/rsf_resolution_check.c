// Exercise the real resolution mailbox and game-thread application with host stubs.
#include <assert.h>
#include "../runtime/resolution.c"

const int rt_game_version = 143;
volatile int g_fnprof;
static unsigned settings_version = 1, render_calls;
static int sr_enabled = 1, dynamic_requested = 1, fixed_patch, patch_calls;
static unsigned char patch_page[64];
const char *rsf_runtime_path(void) { abort(); }
// Other resolution entry points are linked too on Windows. None may run in this fixture.
const char *cfg_get(const char *name) { (void)name; abort(); }
const char *cfg_value(const char *name) { (void)name; abort(); }
int cfg_int(const char *name) { (void)name; abort(); }
int cfg_bool(const char *name) {
    if (!strcmp(name,"TOTK_RSF_ENABLED")) return sr_enabled;
    if (!strcmp(name,"TOTK_DYNAMIC_RES")) return dynamic_requested;
    abort();
}
int rt_patch_set(const char *name) { assert(!strcmp(name,"fixres")); return 3; }
void rt_patch_switch(int set, int on) { assert(set==3);fixed_patch=on;patch_calls++; }
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
    R.decided = 1;
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
    assert(res_rsf_scene(1130,636)==1);
    res_present(&cpu);assert(R.sw==1136 && R.sh==640 && render_calls==2);
    assert(res_rsf_scene(1130,636)==2); // no impossible optimal-size request loop
    res_present(&cpu);assert(render_calls==2);
    assert(!res_rsf_scene(7,7));
    res_dynres_live(); assert(fixed_patch && patch_calls==1); // exact SR plan owns resolution
    sr_enabled=0;res_dynres_live();assert(!fixed_patch && patch_calls==2); // FG-only restores game DRS
    assert(res_rsf_scene(1280,720)); res_present(&cpu);
    assert(R.sw==1136 && render_calls==2); // disabled SR cannot apply an old plan
    dynamic_requested=0;res_dynres_live();assert(fixed_patch && patch_calls==3);
    sr_enabled=1;res_dynres_live();assert(fixed_patch && patch_calls==3); // respects requested fixed mode
    dynamic_requested=1;res_dynres_live();assert(fixed_patch); // user DRS waits until SR releases ownership
    sr_enabled=0;res_dynres_live();assert(!fixed_patch);
    R.live = 0;
    assert(!res_rsf_scene(1280, 720));
    R.dlss_k=.58;R.sw=1114;R.ow=1920;
    assert(fabs(res_dlss_lod_bias()-log2(1114.0/1920))<1e-6);
    R.dlss_k=0;assert(res_dlss_lod_bias()==0);
    return 0;
}
