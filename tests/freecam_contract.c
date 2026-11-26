// Native contract fixture for patches_v110_freecam_check.py. Only platform boundaries are stubbed.
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include "host.h"
#include "input.h"
#include "platform/platform.h"
#include "cfg.h"
#include "patches.h"
static GuestFn registered;
static int registrations, calls, patch_setting = -1, steady_setting = -1;
static uint64_t now = 1000000000;
const int rt_game_version = TEST_VERSION;
void rt_register_host(uint64_t at, GuestFn fn) {
    assert(at == RT_PATCH_PAGE + 0xf50);
    registered = fn;
    registrations++;
}
void rt_log(const char *s, ...) { (void)s; }
void rt_fatal(const char *s, ...) { (void)s; abort(); }
uint64_t plat_now_ns(void) { return now; }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
int mem_guest_readable(uint64_t at, uint64_t n) { return at != 0 && n <= 0x300; }
int plat_mem_commit(uint64_t at, size_t n) { return at == RT_PATCH_PAGE && n == 4096; }
const char *cfg_get(const char *key) {
    int value = !strcmp(key, "TOTK_FREECAM_PATCH") ? patch_setting :
                !strcmp(key, "TOTK_LOD_STEADY") ? steady_setting : -1;
    return value < 0 ? NULL : value ? "1" : "0";
}
const CfgMeta *cfg_meta(const char *key) {
    static const CfgMeta row = {"FREECAM_PATCH", "0", "0|1", 0};
    return !strcmp(key, "TOTK_FREECAM_PATCH") ? &row : NULL;
}
int cfg_bool(const char *key) {
    const char *v = cfg_get(key);
    return strcmp(v ? v : cfg_meta(key)->def, "0") != 0;
}
void rt_dispatch(Cpu *c) {
    assert(registered && c->pc == RT_PATCH_PAGE + 0xf50);
    calls++;
    registered(c);
}

#include "freecam_source.h"
extern uint8_t rt_patch_on[];
#include "freecam_lift.h"

static unsigned char camera[4][0x300], saved[4][0x300];
static _Alignas(16) unsigned char stack[128];
static void pose(int k, const double eye[3], const double target[3]) {
    memset(camera[k], 0xa5, sizeof camera[k]);
    uint64_t link = 0x123456789abcdef0ull + k;
    const float up[3] = {0.25f, 0.75f, -0.5f}, offset[3] = {2,3,4};
    memcpy(camera[k] + 0x18, &link, 8);
    memcpy(camera[k] + 0xe8, eye, 24);
    memcpy(camera[k] + 0x110, target, 24);
    memcpy(camera[k] + 0x128, up, 12);
    memset(camera[k] + 0x180, 0, 24);
    memset(camera[k] + 0x198, 0, 24);
    memcpy(camera[k] + 0x200, offset, 12);
    memcpy(saved[k], camera[k], sizeof camera[k]);
}
static void invoke(int k, int lifted) {
    Cpu c, want;
    memset(&c, 0x6b, sizeof c);
    c.x[19] = (uintptr_t)camera[k];
    c.sp = (uintptr_t)(stack + 96);
    want = c;
    memcpy(&want.x[8], camera[k] + 0x18, 8);
    if (lifted && rt_patch_enabled(rt_patch_set("freecam"))) want.pc = RT_PATCH_PAGE + 0xf50;
    if (lifted) test_lift(&c); else registered(&c);
    assert(!memcmp(&c, &want, sizeof c)); // all GPRs, full SIMD, flags, SP and runtime CPU fields
}
static void changed_only_overrides(int k) {
    for (size_t i=0; i<sizeof camera[k]; i++)
        if (!(i>=0x180 && i<0x1b0) && !(i>=0x200 && i<0x20c)) assert(camera[k][i] == saved[k][i]);
    float up[3], off[3];
    memcpy(up, camera[k]+0x128,12); memcpy(off,camera[k]+0x200,12);
    assert(up[0]+off[0] == 0 && up[1]+off[1] == 1 && up[2]+off[2] == 0);
}
int main(void) {
    int supported = TEST_VERSION == 110 || TEST_VERSION == 143;
    assert(registrations == supported);
    for (patch_setting=-1; patch_setting<=1; patch_setting++) {
        rt_patches_init();
        assert(freecam_available() == (supported && patch_setting == 1));
    }
    for (steady_setting=-1; steady_setting<=1; steady_setting++) {
        rt_patches_init();
        assert(rt_patch_set("steady") == 8);
        assert(rt_patch_enabled(rt_patch_set("steady")) == (steady_setting == 1));
    }
    patch_setting=0; rt_patches_init();
    const double eye[3]={10,20,30}, target[3]={10,20,40};
    pose(0,eye,target);
    invoke(0,1); assert(calls == 0 && !memcmp(camera[0],saved[0],sizeof camera[0]));
    if (!supported) { puts("freecam: unsupported version stays unavailable and patch-off load is intact"); return 0; }
    patch_setting=1; rt_patches_init();
    invoke(0,1); assert(calls == 1 && freecam_state() == FREECAM_OFF);
    assert(!memcmp(camera[0],saved[0],sizeof camera[0]));
    freecam_cycle(); assert(freecam_state() == FREECAM_MOVE);
    invoke(0,1); changed_only_overrides(0);
    assert(!memcmp(camera[0]+0x180,eye,24));
    FreecamPad p={.ly=1}; freecam_pad(&p);
    now += 100000000; invoke(0,1); changed_only_overrides(0);
    double moved[3]; memcpy(moved,camera[0]+0x180,24);
    assert(fabs(moved[2]-30.8)<1e-10 && moved[0]==10 && moved[1]==20);
    invoke(0,1); assert(!memcmp(camera[0]+0x180,moved,24)); // zero dt does not step
    now += 1000000000; invoke(0,1);
    memcpy(moved,camera[0]+0x180,24);
    assert(fabs(moved[2]-31.6)<1e-10); // a one-second hitch is capped at STEP_MAX_S
    now += 1; invoke(0,1);
    memcpy(moved,camera[0]+0x180,24);
    assert(fabs(moved[2]-31.600000008)<1e-10); // a one-nanosecond step stays proportional
    // A second component sharing the original eye joins; a default or distant camera stays untouched.
    pose(1,eye,target); invoke(1,0); changed_only_overrides(1);
    assert(!memcmp(camera[0]+0x180,camera[1]+0x180,24));
    const double defeye[3]={0,0,10}, deftarget[3]={0,0,0}, far[3]={100,200,300};
    pose(2,defeye,deftarget); invoke(2,0); assert(!memcmp(camera[2],saved[2],sizeof camera[2]));
    pose(3,far,target); invoke(3,0); assert(!memcmp(camera[3],saved[3],sizeof camera[3]));
    freecam_cycle(); assert(freecam_state() == FREECAM_LOCK);
    now += 100000000; invoke(0,1); invoke(1,0);
    assert(!memcmp(camera[0]+0x180,moved,24) && !memcmp(camera[1]+0x180,moved,24));
    now += 500000001; invoke(3,0); changed_only_overrides(3);
    assert(!memcmp(camera[3]+0x180,moved,24)); // a lost anchor is replaced without moving the locked flight
    invoke(0,1); invoke(1,0); // still-live components update before exit; stale ones may already have been freed
    freecam_cycle(); assert(freecam_state() == FREECAM_OFF);
    invoke(0,1); invoke(1,0); invoke(3,0);
    assert(!memcmp(camera[3]+0x200,saved[3]+0x200,12));
    assert(!memcmp(camera[0]+0x200,saved[0]+0x200,12));
    assert(!memcmp(camera[1]+0x200,saved[1]+0x200,12));
    // The game consumes/clears the one-shot overrides. The hook only restores additive up state on exit.
    memset(camera[0]+0x180,0,48); memset(camera[1]+0x180,0,48); memset(camera[3]+0x180,0,48);
    assert(!memcmp(camera[3],saved[3],sizeof camera[3]));
    assert(!memcmp(camera[0],saved[0],sizeof camera[0]) && !memcmp(camera[1],saved[1],sizeof camera[1]));
    puts("freecam: registration, defaults, on/off, emitted host call, Off/Move/Lock, shared/default/distant components, CPU state and up restoration passed");
    return 0;
}
