"""Check the direct camera hook's register contract and raw-count routing without game data.

Usage: python tests/mouse_direct_check.py --cc <clang.exe>
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include "mouse_direct.c"
static int raw_x, raw_y, takes, captured=1, foreground=1, allowed=1, mode=1, patched=1, selecting;
static float ratio=0.5f;
static uint64_t now=1000000000;
static GuestFn hook, activity_hook, begin_hook, angles_hook, hand_hook;
const char *cfg_get(const char *key) { (void)key; return mode ? "direct" : "stick"; }
const char *cfg_value(const char *key) { return cfg_get(key); }
double cfg_double(const char *key) { (void)key; return 1; }
int rt_patch_set(const char *s) { (void)s; return 0; }
int rt_patch_enabled(int s) { (void)s; return patched; }
int ikb_captured(void) { return captured; }
int ikb_foreground(void) { return foreground; }
int input_cursor_allowed(void) { return allowed; }
int input_direct_mouse_selector_active(void) { return selecting; }
void ikb_mouse_take(int32_t *x, int32_t *y) { ++takes; *x=raw_x; *y=raw_y; raw_x=raw_y=0; }
void input_direct_mouse_sample(int enabled, int32_t *x, int32_t *y) {
    *x=*y=0;
    if(enabled && captured && foreground && allowed) ikb_mouse_take(x,y);
}
uint64_t plat_now_ns(void) { return now; }
int frame_step_realtime(void) { return 1; }
double frame_step_limit_hz(void) { return 60; }
void *rt_patch_page(void) { return &ratio; }
void rt_register_host(uint64_t addr, GuestFn fn) {
    if(addr==HOST_MOUSE_DIRECT) hook=fn; else if(addr==HOST_MOUSE_ACTIVITY) activity_hook=fn;
    else if(addr==HOST_MOUSE_ORBIT_BEGIN) begin_hook=fn; else if(addr==HOST_MOUSE_ORBIT_ANGLES) angles_hook=fn;
    else if(addr==HOST_MOUSE_HAND) hand_hook=fn; else abort();
}
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
void rt_log(const char *fmt, ...) { (void)fmt; }
static float scalar(__uint128_t v) { float f; memcpy(&f,&v,4); return f; }
int main(void) {
    assert(hook && activity_hook);
    Cpu c, expected;
    unsigned char target[0x500] = {0};
    memset(&c, 0x5a, sizeof c);
    c.x[20]=(uintptr_t)target; c.x[21]=0; c.v[9]=c.v[10]=0;
    expected=c;
    raw_x=100; raw_y=-50;
    hook(&c);
    assert(fabsf(scalar(c.v[9])-2)<1e-6f);
    assert(fabsf(scalar(c.v[10])-1)<1e-6f);
    float stored; memcpy(&stored,target+0x420,4); assert(stored==scalar(c.v[9]));
    expected.v[9]=c.v[9]; expected.v[10]=c.v[10]; assert(!memcmp(&expected,&c,sizeof c));
    assert(input_direct_mouse_active());
    // The next read sees no repeated movement. Gamepad/keyboard input is preserved.
    expected=c; hook(&c); assert(!memcmp(&expected,&c,sizeof c));
    // Equal mouse distance produces equal integrated input at 30, 60 and 144 Hz; there is no unit-circle clamp.
    float rates[]={30,60,144};
    for(int i=0;i<3;i++) {
        ratio=30/rates[i]; raw_x=10000; raw_y=10000; c.v[9]=c.v[10]=0; hook(&c);
        assert(fabsf(scalar(c.v[9])*ratio-100)<0.001f);
        assert(fabsf(scalar(c.v[10])*ratio+100)<0.001f);
    }
    // One count is not discarded by a dead zone.
    ratio=0.5f; raw_x=1; raw_y=0; c.v[9]=c.v[10]=0; hook(&c); assert(scalar(c.v[9])>0);
    uint64_t stack[4]={0,0x123456789abcdef0ull,0x456789abcdef0123ull,0}; c.sp=(uintptr_t)stack;
    // The flags are in w8 at the site: the game's next instruction stores w8 to +0x418, so memory is not touched.
    uint32_t inactive=0x28, mem=0x77; memcpy(target+0x418,&mem,4);
    c.x[8]=inactive; activity_hook(&c);
    assert((uint32_t)c.x[8]==0x11);
    uint32_t after; memcpy(&after,target+0x418,4); assert(after==mem);
    assert(c.v[9]==stack[1] && c.v[8]==stack[2]);
    // A controller-only update keeps the game's activity bits.
    c.x[8]=inactive; activity_hook(&c); assert((uint32_t)c.x[8]==inactive);
    // Disabled game camera input leaves raw counts for the menu's legacy mouse-to-stick path.
    c.x[21]=1; raw_x=42; int before=takes; hook(&c);
    assert(takes==before && raw_x==42 && !input_direct_mouse_active()); c.x[21]=0;
    // Focus loss and host menus do not move the camera.
    captured=0; before=takes; hook(&c); assert(takes==before); captured=1;
    foreground=0; hook(&c); assert(takes==before); foreground=1;
    allowed=0; hook(&c); assert(takes==before); allowed=1;
    mode=0; hook(&c); assert(takes==before && !input_direct_mouse_active()); mode=1;
    patched=0; hook(&c); assert(takes==before && !input_direct_mouse_active()); patched=1;
    now+=150000001; assert(!input_direct_mouse_active());
    // Orbit input removes the mouse from the stick path and writes equal degrees per count at the final angles.
    unsigned char camera[0x300]={0}, frame[0x100]={0}, hand[0x300]={0};
    uint64_t input_address=(uintptr_t)target; memcpy(camera+0x20,&input_address,8);
    for(int rate=0;rate<3;rate++) {
        memset(orbit_seen,0,sizeof orbit_seen);
        ratio=30/rates[rate]; c.x[20]=(uintptr_t)target; c.x[21]=0; c.v[9]=c.v[10]=0;
        raw_x=100; raw_y=-50; hook(&c);
        guest_float_store((uintptr_t)camera+0x108,30); guest_float_store((uintptr_t)camera+0x120,40);
        guest_float_store((uintptr_t)frame+0x44,30); guest_float_store((uintptr_t)frame+0x48,40);
        c.x[0]=(uintptr_t)camera; begin_hook(&c);
        assert(guest_float((uintptr_t)target+0x420)==0 && guest_float((uintptr_t)target+0x424)==0);
        c.x[20]=(uintptr_t)camera; c.sp=(uintptr_t)frame; angles_hook(&c);
        assert(fabsf(guest_float((uintptr_t)frame+0x44)-26)<0.0001f);
        assert(fabsf(guest_float((uintptr_t)frame+0x48)-32)<0.0001f);
        assert(fabsf(guest_float((uintptr_t)camera+0x15c)-26)<0.0001f);
        c.x[0]=(uintptr_t)camera; begin_hook(&c); angles_hook(&c);
        assert(fabsf(guest_float((uintptr_t)frame+0x48)-32)<0.0001f); // one sample cannot repeat next update
        // The next input frame is stationary and the game's parameter update rewrites the old default target.
        // Restoring the mouse's desired target makes the camera filter stay at the new pitch, not spring to zero.
        c.x[20]=(uintptr_t)target; c.v[9]=c.v[10]=0; raw_x=raw_y=0; hook(&c);
        guest_float_store((uintptr_t)camera+0x15c,0);
        c.x[0]=(uintptr_t)camera; begin_hook(&c);
        float current=guest_float((uintptr_t)camera+0x108), goal=guest_float((uintptr_t)camera+0x15c);
        assert(fabsf(goal-26)<0.0001f && fabsf((current+goal)*0.5f-26)<0.0001f);
        // A later game filter overwrites the pitch after the begin hook. Neither a stationary frame nor the next
        // tiny mouse delta may integrate from that recentered result.
        guest_float_store((uintptr_t)camera+0x108,0);
        guest_float_store((uintptr_t)camera+0x11c,5);
        guest_float_store((uintptr_t)camera+0x10c,20);
        guest_float_store((uintptr_t)camera+0x88,0.3f);
        guest_float_store((uintptr_t)frame+0x44,0);
        c.x[20]=(uintptr_t)camera; angles_hook(&c);
        assert(fabsf(guest_float((uintptr_t)frame+0x44)-26)<0.0001f);
        assert(fabsf(guest_float((uintptr_t)camera+0x15c)-7)<0.0001f);
        assert(fabsf(guest_float((uintptr_t)camera+0x108)-12)<0.0001f);
        for(int i=0;i<100;i++) {
            guest_float_store((uintptr_t)frame+0x44,0); angles_hook(&c);
            assert(fabsf(guest_float((uintptr_t)camera+0x108)-12)<0.0001f);
        }
        c.x[20]=(uintptr_t)target; c.v[9]=c.v[10]=0; raw_y=1; hook(&c);
        c.x[0]=(uintptr_t)camera; begin_hook(&c);
        guest_float_store((uintptr_t)frame+0x44,0);
        c.x[20]=(uintptr_t)camera; angles_hook(&c);
        assert(fabsf(guest_float((uintptr_t)frame+0x44)-26.08f)<0.0001f);
        // An explicit controller recenter still hands pitch back to the game.
        uint64_t recenter=0x20; memcpy(target+0x148,&recenter,8);
        c.x[20]=(uintptr_t)target; c.v[9]=c.v[10]=0; hook(&c);
        c.x[0]=(uintptr_t)camera; begin_hook(&c);
        guest_float_store((uintptr_t)frame+0x44,5);
        c.x[20]=(uintptr_t)camera; angles_hook(&c);
        assert(guest_float((uintptr_t)frame+0x44)==5);
        memset(target+0x148,0,8);
        memset(camera+0x88,0,4); memset(camera+0x10c,0,4); memset(camera+0x11c,0,4);
    }
    // A gamepad-only step (no mouse motion, no pitch hold) leaves the camera input and the angles as the game made them.
    memset(orbit_seen,0,sizeof orbit_seen);
    c.x[20]=(uintptr_t)target; c.x[21]=0; raw_x=raw_y=0; c.v[9]=c.v[10]=0;
    { float px=0.25f, py=-0.5f; memcpy(&c.v[9],&px,4); memcpy(&c.v[10],&py,4); }
    hook(&c);
    guest_float_store((uintptr_t)target+0x420,0.25f); guest_float_store((uintptr_t)target+0x424,0.75f);
    guest_float_store((uintptr_t)frame+0x44,11); guest_float_store((uintptr_t)frame+0x48,22);
    guest_float_store((uintptr_t)camera+0x15c,3);
    c.x[0]=(uintptr_t)camera; begin_hook(&c);
    assert(guest_float((uintptr_t)target+0x420)==0.25f && guest_float((uintptr_t)target+0x424)==0.75f);
    c.x[20]=(uintptr_t)camera; c.sp=(uintptr_t)frame; angles_hook(&c);
    assert(guest_float((uintptr_t)frame+0x44)==11 && guest_float((uintptr_t)frame+0x48)==22);
    assert(guest_float((uintptr_t)camera+0x15c)==3);
    guest_float_store((uintptr_t)camera+0x108,30); guest_float_store((uintptr_t)camera+0x120,40);
    for(int count=0;count<100;count++) {
        c.x[20]=(uintptr_t)target; c.x[21]=0; c.v[9]=c.v[10]=0; raw_x=1; raw_y=0; hook(&c);
        c.x[0]=(uintptr_t)camera; begin_hook(&c); c.x[20]=(uintptr_t)camera;
        guest_float_store((uintptr_t)frame+0x44,guest_float((uintptr_t)camera+0x108));
        guest_float_store((uintptr_t)frame+0x48,guest_float((uintptr_t)camera+0x120));
        angles_hook(&c);
    }
    assert(fabsf(guest_float((uintptr_t)camera+0x120)-32)<0.001f);
    // A held object receives mouse deltas after its own curves, once per sample, including with camera input off.
    c.x[20]=(uintptr_t)target; c.v[9]=c.v[10]=0; raw_x=100; raw_y=50; ratio=0.5f; hook(&c);
    c.x[19]=(uintptr_t)hand; c.x[8]=0x12345678; c.v[15]=0;
    guest_float_store((uintptr_t)frame+0x60,0); hand_hook(&c);
    assert(fabsf(guest_float((uintptr_t)frame+0x60)-2)<0.00001f);
    assert(fabsf(scalar(c.v[15])+1)<0.00001f);
    hand_hook(&c); assert(fabsf(scalar(c.v[15])+1)<0.00001f);
    c.x[21]=1; raw_x=5; raw_y=6; hook(&c); assert(raw_x==0 && raw_y==0 && input_direct_mouse_active());
    // A radial selector retains its virtual cursor between mouse packets, including repeated HID polls.
    float cursor[2]={0}; int32_t selector[2], held_selector[2];
    input_direct_mouse_radial(100,50,1,cursor,selector);
    held_selector[0]=selector[0]; held_selector[1]=selector[1];
    for(int i=0;i<100;i++) {
        input_direct_mouse_radial(0,0,1,cursor,selector);
        assert(selector[0]==held_selector[0] && selector[1]==held_selector[1]);
    }
    input_direct_mouse_radial(-100,-50,1,cursor,selector); assert(!selector[0] && !selector[1]);
    input_direct_mouse_radial(100000,100000,1,cursor,selector);
    assert((int64_t)selector[0]*selector[0]+(int64_t)selector[1]*selector[1]<=32768ll*32768);
    // Quick lists must not latch a wheel direction or automatically drain a large flick after the mouse stops.
    InputMouseRow row={0}; uint64_t row_time=0;
    input_direct_mouse_row(10000,1,row_time,&row,selector); assert(selector[0]==32767 && !selector[1]);
    input_direct_mouse_row(0,1,row_time+10000000,&row,selector); assert(selector[0]==32767);
    for(int i=1;i<=100;i++) {
        input_direct_mouse_row(0,1,row_time+i*100000000ull,&row,selector); assert(!selector[0] && !selector[1]);
    }
    // Repeated HID polls do not rearm a pulse; a frame that skips its deadline must still observe neutral first.
    row=(InputMouseRow){0};
    for(int i=0;i<31;i++) { input_direct_mouse_row(1,1,0,&row,selector); assert(!selector[0]); }
    input_direct_mouse_row(1,1,0,&row,selector); assert(selector[0]==32767);
    input_direct_mouse_row(1000,1,100000000,&row,selector); assert(!selector[0]);
    input_direct_mouse_row(1000,1,110000000,&row,selector); assert(!selector[0]);
    input_direct_mouse_row(1000,1,140000000,&row,selector); assert(selector[0]==32767);
    input_direct_mouse_row(-1000,1,141000000,&row,selector); assert(!selector[0]);
    input_direct_mouse_row(-1,1,180000000,&row,selector); assert(selector[0]==-32767);
    selecting=1; c.x[20]=(uintptr_t)target; raw_x=42; raw_y=0; before=takes; hook(&c);
    assert(takes==before && raw_x==42); selecting=0; raw_x=0;
    puts("direct mouse: register contract, routing, linearity, frame ratio and capture checks passed");
    puts("direct mouse: final orbit angles, single-count motion, equal axes and held-object routing passed");
    puts("direct mouse: persistent pitch target, radial cursor hold and gamepad-only steps untouched passed");
    puts("direct mouse: final-pitch recenter rejection, explicit handoff and bounded quick-list steps passed");
}
'''

INPUT_HARNESS = r'''
#include <assert.h>
#include "input.c"
GfxStats gfx_counters;
InputMap input_map;
// Other input entry points are linked by PE unwind metadata; reaching them is outside this script-only test.
void ikb_cursor_update(void) { abort(); }
void ikb_menu_closed(void) { abort(); }
int cfg_bool(const char *key) { (void)key; abort(); }
int input_menu_gate(int *h,int open,uint64_t *b,int32_t s[4]) { (void)h;(void)open;(void)b;(void)s;abort(); }
int input_overlay_gate(int *h,uint64_t *b) { (void)h;(void)b;abort(); }
int menu_is_open(void) { abort(); }
void menu_toggle(void) { abort(); }
void menu_perf_cycle(void) { abort(); }
const char *input_action_name(int action) { (void)action;abort(); }
void input_map_configure(void) { abort(); }
int input_direct_mouse_active(void) { return 1; }
int input_direct_mouse_enabled(void) { return 1; }
static int row_calls, radial_calls, live_mouse;
void input_direct_mouse_row(int32_t dx,float sensitivity,uint64_t now,InputMouseRow *row,int32_t out[2]) {
    (void)dx;(void)sensitivity;(void)now; row_calls++; out[0]=123;out[1]=0;row->direction=1;
}
void input_direct_mouse_radial(int32_t dx,int32_t dy,float sensitivity,float cursor[2],int32_t out[2]) {
    (void)dx;(void)dy;(void)sensitivity;radial_calls++;cursor[0]=1;out[0]=456;out[1]=789;
}
static const char *script_path;
static uint64_t test_tick;
static int64_t test_world=-1;
const char *cfg_get(const char *key) {
    if (!strcmp(key,"TOTK_INPUT_SCRIPT")) return script_path;
    if (!strcmp(key,"TOTK_INPUT")) return "none";
    return NULL;
}
unsigned cfg_version(void) { return 0; }
double cfg_double(const char *key) { (void)key; return 1; }
uint64_t frame_step_ticks(void) { return test_tick; }
int64_t frame_step_world_us(void) { return test_world; }
void rt_log(const char *fmt, ...) { (void)fmt; }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
int ikb_captured(void) { return live_mouse; }
int ikb_foreground(void) { return 0; }
void ikb_mouse_take(int32_t *x,int32_t *y) { *x=*y=0; }
uint64_t plat_now_ns(void) { return 1000000000ull; }
int main(int argc,char **argv) {
    assert(argc==2); script_path=argv[1];
    uint64_t buttons=0; int32_t left[2]={0},right[2]={0},dx,dy;
    test_tick=10;
    for(int i=0;i<3;i++) script_apply(&buttons,left,right);
    input_direct_mouse_sample(1,&dx,&dy);
    assert(dx==123 && dy==-45 && !buttons && !right[0] && !right[1]);
    input_direct_mouse_sample(1,&dx,&dy); assert(!dx && !dy);
    test_tick=20; script_apply(&buttons,left,right);
    g_menu=1; input_direct_mouse_sample(1,&dx,&dy); assert(!dx && !dy);
    g_menu=0; input_direct_mouse_sample(1,&dx,&dy); assert(!dx && !dy);
    test_world=100000;
    for(int i=0;i<3;i++) script_apply(&buttons,left,right);
    rec=tmpfile(); assert(rec); rec_state=1;
    input_direct_mouse_sample(1,&dx,&dy); assert(dx==-100 && dy==50);
    rewind(rec); char line[128]; assert(fgets(line,sizeof line,rec));
    assert(strstr(line,"MOUSE -100 50")); assert(!fgets(line,sizeof line,rec));
    fclose(rec); rec_state=-1;
    live_mouse=1;
    mouse_camera(right,1); assert(radial_calls==1 && !row_calls && right[0]==456 && right[1]==789);
    assert(input_direct_mouse_selector_active());
    mouse_camera(right,2); assert(row_calls==1 && radial_calls==1 && right[0]==123 && !right[1]);
    mouse_camera(right,0); assert(!input_direct_mouse_selector_active() && !right[0] && !right[1]);
    mouse_camera(right,3); assert(row_calls==2 && right[0]==123 && !right[1]);
    live_mouse=0; mouse_camera(right,3);
    assert(!input_direct_mouse_selector_active() && !right[0] && !right[1]);
    puts("direct mouse: one-shot script replay, host-menu blocking and raw-count recording passed");
    puts("direct mouse: separate wheel/list routing, selector ownership and release passed");
}
'''

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', required=True)
    ap.add_argument('--deps', type=Path, help='dependency include directory (SDL3/Vulkan), to also check input scripts')
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='totk-mouse-') as tmp:
        folder = Path(tmp)
        (folder / 'patches.h').write_text('#define RT_PATCH_PAGE 0x7003adf000ull\n')
        source = folder / 'check.c'
        source.write_text(HARNESS)
        exe = folder / ('check.exe' if os.name == 'nt' else 'check')
        link = [] if os.name == 'nt' else ['-lm']
        subprocess.run([args.cc, '-O2', '-Wall', '-Wextra', '-I', str(folder), '-I', str(ROOT/'runtime'),
                        str(source), *link, '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        if args.deps:
            source.write_text(INPUT_HARNESS)
            script = folder / 'input.txt'
            script.write_text('10 MOUSE 123 -45\n20 MOUSE 1000 0\n@100 MOUSE -100 50\n')
            subprocess.run([args.cc, '-O2', '-flto', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', *(['-fuse-ld=lld', '-Wl,--exclude-all-symbols'] if os.name == 'nt' else []),
                            '-I', str(ROOT/'runtime'), '-I', str(args.deps), '-I', str(args.deps/'vma'),
                            str(source), *link, '-o', str(exe)], check=True)
            subprocess.run([str(exe), str(script)], check=True)

if __name__ == '__main__':
    main()
