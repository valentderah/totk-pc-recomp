"""Check the direct camera hook's register contract and raw-count routing without game data.

Usage: python tests/mouse_direct_check.py --cc <clang.exe>
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include "mouse_direct.c"
const int rt_game_version = TEST_GAME_VERSION;
static int raw_x, raw_y, takes, captured=1, foreground=1, allowed=1, mode=1, patched=1, selecting;
static float ratio=0.5f;
static uint64_t now=1000000000;
static GuestFn hook, activity_hook, begin_hook, angles_hook, hand_hook, load_hook;
const char *cfg_get(const char *key) { (void)key; return mode ? "direct" : "stick"; }
const char *cfg_value(const char *key) { return cfg_get(key); }
double cfg_double(const char *key) { (void)key; return 1; }
int rt_patch_set(const char *s) { return strcmp(s,"mouse_direct") ? -1 : 0; }   // no fov / aspect set: 16:9, 50 deg
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
    else if(addr==HOST_MOUSE_HAND) hand_hook=fn; else if(addr==HOST_MOUSE_LOAD) load_hook=fn; else abort();
}
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
void rt_log(const char *fmt, ...) { (void)fmt; }
float cam_speed_applied(void) { return 1; }
uint64_t input_buttons_last(void) { return 0; }
int mem_guest_readable(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }   // no player: on-foot gains
static float scalar(__uint128_t v) { float f; memcpy(&f,&v,4); return f; }
#if TEST_GAME_VERSION == 143
int main(void) {
    assert(hook && activity_hook);
    Cpu c, expected;
    unsigned char target[0x500] = {0};
    memset(&c, 0x5a, sizeof c);
    c.x[20]=(uintptr_t)target; c.x[21]=0; c.v[9]=c.v[10]=0;
    expected=c;
    raw_x=100; raw_y=-50;
    hook(&c);
    // x is STICK_PER_COUNT a count over the frame ratio; y PITCH_GAIN times that, for the cameras' linear pitch
    assert(fabsf(scalar(c.v[9])-100*STICK_PER_COUNT/ratio)<1e-5f);
    assert(fabsf(scalar(c.v[10])-PITCH_GAIN*50*STICK_PER_COUNT/ratio)<1e-5f);
    float stored; memcpy(&stored,target+0x420,4); assert(stored==scalar(c.v[9]));
    expected.v[9]=c.v[9]; expected.v[10]=c.v[10]; assert(!memcmp(&expected,&c,sizeof c));
    assert(input_direct_mouse_active());
    // The next read sees no repeated movement. Gamepad/keyboard input is preserved.
    expected=c; hook(&c); assert(!memcmp(&expected,&c,sizeof c));
    // Equal mouse distance turns the camera equally at 30, 60 and 144 Hz, yaw by x and pitch linearly by y, and as
    // many degrees a count on both axes; there is no unit-circle clamp.
    float rates[]={30,60,144};
    for(int i=0;i<3;i++) {
        ratio=30/rates[i]; raw_x=10000; raw_y=10000; c.v[9]=c.v[10]=0; hook(&c);
        float y=scalar(c.v[10]);
        assert(fabsf(scalar(c.v[9])*ratio-10000*STICK_PER_COUNT)<0.001f);
        assert(fabsf(y*ratio/PITCH_GAIN+10000*STICK_PER_COUNT)<0.001f);
    }
    // The ability camera's offset mode turns the stored stick at its own scales: they are undone.
    {
        float x=0, y=0; mouse_stick_add(100,-50,0.04f,1,&x,&y);
        assert(fabsf(x-100*0.04f*ABILITY_GAIN_X)<1e-5f && fabsf(y-50*0.04f*ABILITY_GAIN_Y)<1e-5f);
        x=y=0; mouse_stick_add(100,-50,0.04f,0,&x,&y);
        assert(fabsf(x-4)<1e-5f && fabsf(y-PITCH_GAIN*2)<1e-5f);
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
        assert(fabsf(guest_float((uintptr_t)frame+0x44)-(30-50*DEG_PER_COUNT))<0.0001f);
        assert(fabsf(guest_float((uintptr_t)frame+0x48)-(40-100*DEG_PER_COUNT))<0.0001f);
        assert(fabsf(guest_float((uintptr_t)camera+0x15c)-(30-50*DEG_PER_COUNT))<0.0001f);
        c.x[0]=(uintptr_t)camera; begin_hook(&c); angles_hook(&c);
        assert(fabsf(guest_float((uintptr_t)frame+0x48)-(40-100*DEG_PER_COUNT))<0.0001f); // one sample cannot repeat next update
        // The next input frame is stationary and the game's parameter update rewrites the old default target.
        // Restoring the mouse's desired target makes the camera filter stay at the new pitch, not spring to zero.
        c.x[20]=(uintptr_t)target; c.v[9]=c.v[10]=0; raw_x=raw_y=0; hook(&c);
        guest_float_store((uintptr_t)camera+0x15c,0);
        c.x[0]=(uintptr_t)camera; begin_hook(&c);
        float current=guest_float((uintptr_t)camera+0x108), goal=guest_float((uintptr_t)camera+0x15c);
        assert(fabsf(goal-(30-50*DEG_PER_COUNT))<0.0001f && fabsf((current+goal)*0.5f-(30-50*DEG_PER_COUNT))<0.0001f);
        // A later game filter overwrites the pitch after the begin hook. Neither a stationary frame nor the next
        // tiny mouse delta may integrate from that recentered result.
        guest_float_store((uintptr_t)camera+0x108,0);
        guest_float_store((uintptr_t)camera+0x11c,5);
        guest_float_store((uintptr_t)camera+0x10c,20);
        guest_float_store((uintptr_t)camera+0x88,0.3f);
        guest_float_store((uintptr_t)frame+0x44,0);
        c.x[20]=(uintptr_t)camera; angles_hook(&c);
        assert(fabsf(guest_float((uintptr_t)frame+0x44)-(30-50*DEG_PER_COUNT))<0.0001f);
        assert(fabsf(guest_float((uintptr_t)camera+0x15c)-((30-50*DEG_PER_COUNT)-19))<0.0001f);
        assert(fabsf(guest_float((uintptr_t)camera+0x108)-((30-50*DEG_PER_COUNT)-14))<0.0001f);
        for(int i=0;i<100;i++) {
            guest_float_store((uintptr_t)frame+0x44,0); angles_hook(&c);
            assert(fabsf(guest_float((uintptr_t)camera+0x108)-((30-50*DEG_PER_COUNT)-14))<0.0001f);
        }
        c.x[20]=(uintptr_t)target; c.v[9]=c.v[10]=0; raw_y=1; hook(&c);
        c.x[0]=(uintptr_t)camera; begin_hook(&c);
        guest_float_store((uintptr_t)frame+0x44,0);
        c.x[20]=(uintptr_t)camera; angles_hook(&c);
        assert(fabsf(guest_float((uintptr_t)frame+0x44)-((30-50*DEG_PER_COUNT)+DEG_PER_COUNT))<0.0001f);
        // An explicit controller recenter still hands pitch back to the game.
        uint64_t recenter=0x20; memcpy(target+0x148,&recenter,8);
        c.x[20]=(uintptr_t)target; c.v[9]=c.v[10]=0; hook(&c);
        c.x[0]=(uintptr_t)camera; begin_hook(&c);
        guest_float_store((uintptr_t)frame+0x44,5);
        c.x[20]=(uintptr_t)camera; angles_hook(&c);
        assert(guest_float((uintptr_t)frame+0x44)==5);
        memset(target+0x148,0,8);
        // When the game lets go (the recenter or an aim ends), the mouse holds the pitch the camera has then: neither its
        // target from before nor the game's default target pulls the camera back.
        guest_float_store((uintptr_t)camera+0x108,3);   // the game left the pitch at 3 + the offset 14
        c.x[20]=(uintptr_t)target; c.v[9]=c.v[10]=0; hook(&c);
        c.x[0]=(uintptr_t)camera; begin_hook(&c);
        assert(fabsf(guest_float((uintptr_t)camera+0x15c)-(17-14-5))<0.0001f);
        guest_float_store((uintptr_t)frame+0x44,0);
        c.x[20]=(uintptr_t)camera; angles_hook(&c);
        assert(fabsf(guest_float((uintptr_t)frame+0x44)-17)<0.0001f);
        guest_float_store((uintptr_t)camera+0x15c,0);   // the game's parameter update writes its default target
        c.x[20]=(uintptr_t)target; hook(&c);
        c.x[0]=(uintptr_t)camera; begin_hook(&c);
        assert(fabsf(guest_float((uintptr_t)camera+0x15c)-(17-14-5))<0.0001f);
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
    assert(fabsf(guest_float((uintptr_t)camera+0x120)-(40-100*DEG_PER_COUNT))<0.001f);
    // A held object receives mouse deltas after its own curves at HAND_PER_COUNT a count, at most HAND_MAX full
    // tilts a step, once per sample, including with camera input off.
    c.x[20]=(uintptr_t)target; c.v[9]=c.v[10]=0; raw_x=200; raw_y=50; ratio=0.5f; hook(&c);
    c.x[19]=(uintptr_t)hand; c.x[8]=0x12345678; c.v[15]=0;
    guest_float_store((uintptr_t)frame+0x60,0); hand_hook(&c);
    assert(200*HAND_PER_COUNT/ratio>HAND_MAX && fabsf(guest_float((uintptr_t)frame+0x60)-HAND_MAX)<0.00001f);
    assert(fabsf(scalar(c.v[15])+50*HAND_PER_COUNT/ratio)<0.00001f);
    hand_hook(&c); assert(fabsf(scalar(c.v[15])+50*HAND_PER_COUNT/ratio)<0.00001f);
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
    // A shake: two direction changes of swings of at least 10 degrees within 0.35 s, then the stick follows the mouse
    // side by side and lets go when it rests; nothing plays on after the mouse
    {
        MouseShake s={0}; uint64_t t=0;
        assert(mouse_shake_step(&s,12,t+=100000000ull)==0);
        assert(mouse_shake_step(&s,-12,t+=100000000ull)==0);   // one change: not yet a shake
        assert(mouse_shake_step(&s,12,t+=100000000ull)==1);    // the second: the stick takes the mouse's side
        assert(mouse_shake_step(&s,-12,t+=100000000ull)==-1);  // and follows it
        assert(mouse_shake_step(&s,0,t+=30000000ull)==-1);     // a side is held two frames, so it is read
        assert(mouse_shake_step(&s,0,t+=40000000ull)==0);      // the mouse rests: neutral, no swing of its own
        for(int i=0;i<10;i++) assert(mouse_shake_step(&s,0,t+=50000000ull)==0);
        assert(!s.turns);                                       // and the shake is over
        // Fast reversals: a side never lasts less than two frames
        s=(MouseShake){0}; t=0; int side=0, flips=0; uint64_t last=0, shortest=~0ull;
        for(int i=0;i<20;i++) {
            int now_side=mouse_shake_step(&s,(i&1)?-12.f:12.f,t+=30000000ull);
            if(now_side!=side) { if(side&&now_side&&last) shortest=t-last<shortest?t-last:shortest; side=now_side; last=t; flips++; }
        }
        assert(flips>3 && shortest>=SHAKE_SIDE_NS);
        // A swing made of several small steps counts as one
        s=(MouseShake){0}; t=0; int got=0;
        for(int i=0;i<8;i++) got|=mouse_shake_step(&s,(i/2&1)?-6.f:6.f,t+=50000000ull)!=0;
        assert(got);
        s=(MouseShake){0}; t=0; got=0;   // the same swings 0.4 s apart: a held object turned back and forth
        for(int i=0;i<6;i++) got|=mouse_shake_step(&s,(i&1)?-12.f:12.f,t+=400000000ull)!=0;
        assert(!got);
        s=(MouseShake){0}; t=0; got=0;   // swings under 10 degrees
        for(int i=0;i<8;i++) got|=mouse_shake_step(&s,(i&1)?-4.f:4.f,t+=50000000ull)!=0;
        assert(!got);
    }
    // The aiming, scope, ability and horse cameras: the stick loads give the gamepad's part, the angle loads the
    // mouse's degrees, once per sample
    {
        assert(load_hook);
        unsigned char input[0x500]={0}, cam[0x200]={0}; float frame[0x20]={0};
        float pad_x=0.25f, pad_y=-0.5f;
        #define LOAD(site,reg) (c.x[30]=MOD_MAIN_BASE+(site)+4, load_hook(&c), scalar(c.v[reg]))
        #define SAMPLE(dx,dy) do { c.x[20]=(uintptr_t)input; c.x[21]=0; c.v[9]=0; c.v[10]=0; \
            memcpy(&c.v[9],&pad_x,4); memcpy(&c.v[10],&pad_y,4); raw_x=(dx); raw_y=(dy); hook(&c); \
            guest_float_store((uintptr_t)input+0x424,scalar(c.v[10])); } while(0)
        ratio=0.5f;
        SAMPLE(100,-50);
        float stick_x=guest_float((uintptr_t)input+0x420), stick_y=guest_float((uintptr_t)input+0x424);
        assert(stick_x!=pad_x && stick_y!=pad_y);   // the stick the game stored has the mouse in it
        c.x[19]=(uintptr_t)cam; c.x[22]=(uintptr_t)input; c.x[29]=(uintptr_t)(frame+0x18);
        guest_float_store((uintptr_t)cam+0x15c,10); frame[0x18-0x12]=40;   // [x29-0x48]
        // Aiming pitch: stick y is the pad's, the pitch target base gets dy counts of degrees
        assert(LOAD(0x23beb94,1)==pad_y);
        assert(fabsf(LOAD(0x23bebc8,1)-(10-50*DEG_PER_COUNT))<1e-5f);
        assert(LOAD(0x23bebc8,1)==10);   // a base load with no stick load before it is the game's own
        assert(LOAD(0x23beb94,1)==pad_y && LOAD(0x23bebc8,1)==10);   // the same sample turns the camera once
        // Aiming yaw: the base yaw on the frame gets dx counts
        assert(LOAD(0x23bee2c,1)==pad_x);
        assert(fabsf(LOAD(0x23beeec,1)-(40-100*DEG_PER_COUNT))<1e-5f);
        // A step without mouse motion keeps every load as the game made it: a stick value something else changed
        // after the input update stays, and so does a -0 base angle (bit for bit)
        {
            SAMPLE(0,0);
            guest_float_store((uintptr_t)input+0x420,pad_x+0.125f);
            assert(LOAD(0x23bee2c,1)==pad_x+0.125f);
            float neg_zero=-0.0f; memcpy(&frame[0x18-0x12],&neg_zero,4);
            float got=LOAD(0x23beeec,1); assert(!memcmp(&got,&neg_zero,4));
            frame[0x18-0x12]=40;
        }
        // Scope pitch near its lower limit: degrees up to the limit, the rest of the mouse's stick stays
        SAMPLE(0,-50);
        float mouse_y=guest_float((uintptr_t)input+0x424)-pad_y, want=-39-50*DEG_PER_COUNT;
        guest_float_store((uintptr_t)cam+0x160,-39); guest_float_store((uintptr_t)cam+0x1ac,-40);
        guest_float_store((uintptr_t)cam+0x1b0,40); c.x[23]=(uintptr_t)input;
        float y=LOAD(0x23c0a1c,11);
        assert(fabsf(y-(pad_y+mouse_y*((want+40)/(want+39))))<1e-5f);
        assert(fabsf(LOAD(0x23c0b00,0)-(-40))<1e-5f);
        // Inside the limits all of it is degrees
        SAMPLE(0,-50); guest_float_store((uintptr_t)cam+0x160,0);
        assert(LOAD(0x23c0a1c,11)==pad_y && fabsf(LOAD(0x23c0b00,0)+50*DEG_PER_COUNT)<1e-5f);
        // The ability camera's offset mode (input +0x15c bit 8) keeps the mouse in the stick and adds no degrees
        SAMPLE(100,0); uint32_t offset_mode=0x100; memcpy(input+0x15c,&offset_mode,4);
        assert(LOAD(0x23d2eec,8)==guest_float((uintptr_t)input+0x420));
        frame[0x18-0x14]=7; assert(LOAD(0x23d34cc,1)==7);   // [x29-0x50]
        offset_mode=0; memcpy(input+0x15c,&offset_mode,4);
        // Horse: the input is x20 / x21; another camera's input or a stale sample is left alone
        SAMPLE(100,-50); c.x[20]=(uintptr_t)input; c.x[21]=(uintptr_t)input;
        guest_float_store((uintptr_t)cam+0x68,5); guest_float_store((uintptr_t)cam+0x64,6);
        assert(LOAD(0x23cb274,8)==pad_x && fabsf(LOAD(0x23cb2a0,1)-(5-100*DEG_PER_COUNT))<1e-5f);
        assert(LOAD(0x23cb3f8,8)==pad_y && fabsf(LOAD(0x23cb424,1)-(6-50*DEG_PER_COUNT))<1e-5f);
        SAMPLE(100,-50); unsigned char other[0x500]; memcpy(other,input,sizeof other); c.x[20]=(uintptr_t)other;
        assert(LOAD(0x23cb274,8)==guest_float((uintptr_t)other+0x420) && LOAD(0x23cb2a0,1)==5);
        c.x[20]=(uintptr_t)input; now+=150000000ull;
        assert(LOAD(0x23cb274,8)==guest_float((uintptr_t)input+0x420) && LOAD(0x23cb2a0,1)==5);
        // The ability camera's offset mode (Recall's cursor): before its scale was read the mouse stays in the stick;
        // then the offset moves so that the aim, o * P + atan(o * tan(half view angle)), moves by the mouse's degrees
        now+=1; SAMPLE(100,-50); c.x[23]=(uintptr_t)input; offset_mode=0x100; memcpy(input+0x15c,&offset_mode,4);
        guest_float_store((uintptr_t)cam+0x1d8,1); guest_float_store((uintptr_t)cam+0x1dc,0);
        assert(LOAD(0x23d2f30,8)==guest_float((uintptr_t)input+0x420) && LOAD(0x23d2f5c,2)==1);
        float scale_p[2]={2,4}; c.x[0]=(uintptr_t)scale_p;
        assert(LOAD(0x23d3210,0)==4);
        SAMPLE(100,-50); c.x[23]=(uintptr_t)input;
        {
            float ty=tanf(25*RECALL_PI/180), tx=ty*16/9, k=180/RECALL_PI;
            #define AIM(o,r,t) ((o)*(r)+atanf((o)*(t))*k)
            assert(LOAD(0x23d2f30,8)==pad_x);
            float ox=LOAD(0x23d2f5c,2);
            assert(fabsf(AIM(ox,2,tx)-AIM(1,2,tx)-100*DEG_PER_COUNT)<1e-3f);
            assert(LOAD(0x23d2f6c,8)==pad_y);
            float oy=LOAD(0x23d2f98,2);
            assert(fabsf(AIM(oy,4,ty)-50*DEG_PER_COUNT)<1e-3f);
            assert(ox>1 && ox-1<100*DEG_PER_COUNT/2 && oy>0 && oy<50*DEG_PER_COUNT/4);   // slower than the camera alone
            #undef AIM
        }
        // outside the offset mode these sites are the game's
        offset_mode=0; memcpy(input+0x15c,&offset_mode,4); SAMPLE(100,-50); c.x[23]=(uintptr_t)input;
        assert(LOAD(0x23d2f30,8)==guest_float((uintptr_t)input+0x420) && LOAD(0x23d2f5c,2)==1);
        // The Purah Pad camera: pitch +0x50, yaw [sp+0x48]
        uint64_t spbuf[0x10]={0}; uint64_t old_sp=c.sp; c.sp=(uintptr_t)spbuf;
        SAMPLE(100,-50); c.x[21]=(uintptr_t)input;
        guest_float_store((uintptr_t)cam+0x50,3); guest_float_store((uintptr_t)spbuf+0x48,9);
        assert(LOAD(0x23c2890,13)==pad_y && fabsf(LOAD(0x23c28b4,1)-(3-50*DEG_PER_COUNT))<1e-5f);
        assert(LOAD(0x23c2934,13)==pad_x && fabsf(LOAD(0x23c2964,1)-(9-100*DEG_PER_COUNT))<1e-5f);
        c.sp=old_sp;
        // Lock-on: the stick gives the pad's part, both reads of the step's turn ([sp+0x20]) get the mouse's degrees
        {
            uint64_t lock_sp[0x10]={0}, keep_sp=c.sp; c.sp=(uintptr_t)lock_sp;
            SAMPLE(100,-50); c.x[24]=(uintptr_t)input; guest_float_store((uintptr_t)lock_sp+0x20,0.5f);
            assert(LOAD(0x23d756c,0)==guest_float((uintptr_t)input+0x420));   // the input test keeps the mouse
            assert(LOAD(0x23d7594,1)==pad_x);
            assert(fabsf(LOAD(0x23d75b4,0)-(0.5f-100*DEG_PER_COUNT))<1e-5f);
            assert(fabsf(LOAD(0x23d75c4,9)-(0.5f-100*DEG_PER_COUNT))<1e-5f);
            assert(LOAD(0x23d75c4,9)==0.5f);   // once both reads took them, the degrees are spent
            assert(LOAD(0x23d7998,1)==pad_y);
            assert(fabsf(LOAD(0x23d79b8,0)-(0.5f-50*DEG_PER_COUNT))<1e-5f);
            assert(fabsf(LOAD(0x23d79c8,10)-(0.5f-50*DEG_PER_COUNT))<1e-5f);
            c.sp=keep_sp;
        }
        // The bow drawn while locked on: the pad's stick, the offsets +0x198 (pitch) and +0x19c (yaw) get the degrees
        SAMPLE(100,-50); c.x[21]=(uintptr_t)input;
        guest_float_store((uintptr_t)cam+0x198,2); guest_float_store((uintptr_t)cam+0x19c,3);
        assert(LOAD(0x23cecb4,9)==pad_y && fabsf(LOAD(0x23cecf8,1)-(2-50*DEG_PER_COUNT))<1e-5f);
        assert(LOAD(0x23ced0c,9)==pad_x && fabsf(LOAD(0x23ced4c,1)-(3-100*DEG_PER_COUNT))<1e-5f);
        assert(LOAD(0x23ced4c,1)==3);   // the gyro path's join reads the offset with no stick load before it
        // Climbing (Tail): the pad's stick on both loads, yaw +0xc0 and pitch +0xb4 get the degrees
        SAMPLE(100,-50); c.x[20]=(uintptr_t)input;
        guest_float_store((uintptr_t)cam+0xc0,7); guest_float_store((uintptr_t)cam+0xb4,8);
        assert(LOAD(0x23d4b14,12)==pad_x && LOAD(0x23d4b1c,13)==pad_y);
        assert(fabsf(LOAD(0x23d4f34,0)-(7-100*DEG_PER_COUNT))<1e-5f && fabsf(LOAD(0x23d5028,1)-(8-50*DEG_PER_COUNT))<1e-5f);
        // Climbing an object: the patched mov has set x22 = x19 + 0x88; the pitch load writes no register but s1
        SAMPLE(100,-50); c.x[24]=(uintptr_t)input; c.x[22]=(uintptr_t)cam+0x88;
        guest_float_store((uintptr_t)cam+0x88,4); guest_float_store((uintptr_t)cam+0x90,5);
        assert(LOAD(0x23c3d64,1)==pad_y && fabsf(LOAD(0x23c3d70,1)-(4-50*DEG_PER_COUNT))<1e-5f);
        assert(c.x[22]==(uintptr_t)cam+0x88);
        assert(LOAD(0x23c3dd0,1)==pad_x && fabsf(LOAD(0x23c3dd8,1)-(5-100*DEG_PER_COUNT))<1e-5f);
        // Waterfall climbing: the yaw is loaded into w8 as bits; the height (stick y) stays the game's
        {
            float wf[0x20]={0}; uint64_t keep29=c.x[29]; c.x[29]=(uintptr_t)(wf+0x10);
            SAMPLE(100,-50); c.x[22]=(uintptr_t)input; wf[0x10-0x0d]=11;   // [x29-0x34]
            assert(LOAD(0x23dced8,13)==pad_x);
            c.x[30]=MOD_MAIN_BASE+0x23dd070+4; c.x[8]=~0ull; load_hook(&c);
            float w; uint32_t wb=(uint32_t)c.x[8]; memcpy(&w,&wb,4);
            assert(c.x[8]>>32==0 && fabsf(w-(11-100*DEG_PER_COUNT))<1e-5f);
            assert(LOAD(0x23dd008,9)==guest_float((uintptr_t)input+0x424));
            assert(sample_watched==sample.generation && sample_watched_mode==MODE_WATERFALL);
            c.x[29]=keep29;
        }
        // Dialogue: the yaw stick load is also the input test, so with the pad's x at rest it gives a tiny value in the
        // mouse's direction (input, no turn) and the yaw gets the degrees
        {
            float keep_x=pad_x; pad_x=0;
            SAMPLE(100,-50); c.x[23]=(uintptr_t)input;
            guest_float_store((uintptr_t)cam+0x5c,1); guest_float_store((uintptr_t)cam+0x64,2);
            float tx=LOAD(0x23c54d4,11);
            assert(tx>0 && tx<1e-9f && fabsf(LOAD(0x23c5554,1)-(2-100*DEG_PER_COUNT))<1e-5f);
            assert(LOAD(0x23c542c,11)==pad_y && fabsf(LOAD(0x23c546c,1)-(1-50*DEG_PER_COUNT))<1e-5f);
            SAMPLE(0,-50); c.x[23]=(uintptr_t)input; assert(LOAD(0x23c54d4,11)==0);   // no mouse x: the pad's 0
            pad_x=keep_x;
        }
        // Shield surfing: stick y keeps the mouse (other parameters follow it), the pitch +0xdc gets the degrees and the
        // pitch turn the game made from y is cut to the pad's share (the turn squares y)
        {
            uint64_t ssp[0x10]={0}, keep_sp=c.sp; c.sp=(uintptr_t)ssp;
            SAMPLE(100,-50); c.x[22]=(uintptr_t)input;
            float total=guest_float((uintptr_t)input+0x424);
            guest_float_store((uintptr_t)cam+0xdc,12); guest_float_store((uintptr_t)ssp+0x10,0.3f);
            guest_float_store((uintptr_t)ssp+0x3c,20);
            assert(LOAD(0x23d0100,1)==pad_x && LOAD(0x23d0104,0)==total);
            assert(fabsf(LOAD(0x23d02ac,0)-(12-50*DEG_PER_COUNT))<1e-5f);
            float cut=LOAD(0x23d0a34,0);
            assert(fabsf(cut-0.3f*(pad_y*fabsf(pad_y))/(total*fabsf(total)))<1e-6f);
            assert(LOAD(0x23d0a34,0)==0.3f);   // once
            assert(fabsf(LOAD(0x23d087c,9)-(20-100*DEG_PER_COUNT))<1e-5f);
            c.sp=keep_sp;
        }
        // The stick mode leaves every load to the game
        mode=0; c.x[20]=(uintptr_t)input; assert(LOAD(0x23cb274,8)==guest_float((uintptr_t)input+0x420)); mode=1;
        #undef LOAD
        #undef SAMPLE
    }
    puts("direct mouse: aiming, scope, ability, Recall cursor, horse, Purah Pad, lock-on, bow lock-on, climbing, waterfall, dialogue and shield cameras in degrees passed");
    puts("direct mouse: one turn rate, linear pitch, ability gains and the hold after a handoff passed");
    puts("direct mouse: a mouse shake followed side by side and released at rest, not a slow turn nor small swings passed");
}
#else
// 1.2.1: the same hooks with that version's registers and stack slots (MouseLayout layout_121) and its load sites
int main(void) {
    assert(hook && activity_hook && begin_hook && angles_hook && hand_hook && load_hook);
    (void)takes;
    Cpu c, expected;
    unsigned char target[0x500]={0}, camera[0x300]={0}, hand[0x300]={0};
    uint64_t frame[0x20]={0};
    memset(&c,0x5a,sizeof c);
    // The camera input update keeps x in s11 (y in s10) and stores s11 at +0x420; s9 is not the update's
    c.x[20]=(uintptr_t)target; c.x[21]=0; c.v[11]=c.v[10]=0; expected=c;
    raw_x=100; raw_y=-50; ratio=0.5f; hook(&c);
    assert(fabsf(scalar(c.v[11])-100*STICK_PER_COUNT/ratio)<1e-5f);
    assert(fabsf(scalar(c.v[10])-PITCH_GAIN*50*STICK_PER_COUNT/ratio)<1e-5f);
    float stored; memcpy(&stored,target+0x420,4); assert(stored==scalar(c.v[11]));
    expected.v[11]=c.v[11]; expected.v[10]=c.v[10]; assert(!memcmp(&expected,&c,sizeof c));
    // The activity hook: flags in w8, the displaced ldp d9, d8, [sp, #0x10]
    uint64_t stack[4]={0,0,0x123456789abcdef0ull,0x456789abcdef0123ull}; c.sp=(uintptr_t)stack;
    c.x[8]=0x28; activity_hook(&c); assert((uint32_t)c.x[8]==0x11 && c.v[9]==stack[2] && c.v[8]==stack[3]);
    // The free orbit: the final pitch and yaw at [sp+0x3c] / [sp+0x40], the displaced ldr s13, [sp, #0x3c]
    uint64_t input_address=(uintptr_t)target; memcpy(camera+0x20,&input_address,8);
    c.x[20]=(uintptr_t)target; c.v[11]=c.v[10]=0; raw_x=100; raw_y=-50; hook(&c);
    guest_float_store((uintptr_t)camera+0x108,30); guest_float_store((uintptr_t)camera+0x120,40);
    guest_float_store((uintptr_t)frame+0x3c,30); guest_float_store((uintptr_t)frame+0x40,40);
    c.x[0]=(uintptr_t)camera; begin_hook(&c);
    assert(guest_float((uintptr_t)target+0x420)==0 && guest_float((uintptr_t)target+0x424)==0);
    c.x[20]=(uintptr_t)camera; c.sp=(uintptr_t)frame; angles_hook(&c);
    assert(fabsf(guest_float((uintptr_t)frame+0x3c)-(30-50*DEG_PER_COUNT))<0.0001f);
    assert(fabsf(guest_float((uintptr_t)frame+0x40)-(40-100*DEG_PER_COUNT))<0.0001f);
    assert(scalar(c.v[13])==guest_float((uintptr_t)frame+0x3c));
    // Ultrahand: x in s9, y in s15, the displaced str w9, [sp, #0x18]
    c.x[20]=(uintptr_t)target; c.v[11]=c.v[10]=0; raw_x=20; raw_y=10; hook(&c);
    c.x[19]=(uintptr_t)hand; c.x[9]=0x12345678; c.v[9]=0; c.v[15]=0; hand_hook(&c);
    assert(fabsf(scalar(c.v[9])-20*HAND_PER_COUNT/ratio)<1e-5f && fabsf(scalar(c.v[15])+10*HAND_PER_COUNT/ratio)<1e-5f);
    uint32_t w9; memcpy(&w9,(unsigned char *)frame+0x18,4); assert(w9==0x12345678);
    // Load sites
    {
        unsigned char input[0x500]={0}, cam[0x200]={0}; uint64_t lsp[0x10]={0}, fp[0x20]={0};
        float pad_x=0.25f, pad_y=-0.5f;
        #define LOAD(site,reg) (c.x[30]=MOD_MAIN_BASE+(site)+4, load_hook(&c), scalar(c.v[reg]))
        #define SAMPLE(dx,dy) do { c.x[20]=(uintptr_t)input; c.x[21]=0; c.v[11]=0; c.v[10]=0; \
            memcpy(&c.v[11],&pad_x,4); memcpy(&c.v[10],&pad_y,4); raw_x=(dx); raw_y=(dy); hook(&c); \
            guest_float_store((uintptr_t)input+0x424,scalar(c.v[10])); } while(0)
        c.x[19]=(uintptr_t)cam; c.sp=(uintptr_t)lsp; c.x[29]=(uintptr_t)(fp+0x10);
        // Aiming: the pitch target the helper computed (+0x15c + turn) is stored with the degrees, and s0 holds it
        SAMPLE(100,-50); c.x[22]=(uintptr_t)input;
        assert(LOAD(0x1d34338,1)==pad_y);
        float target_pitch=12; c.v[0]=0; memcpy(&c.v[0],&target_pitch,4);
        c.x[30]=MOD_MAIN_BASE+0x1d34374+4; load_hook(&c);
        assert(fabsf(scalar(c.v[0])-(12-50*DEG_PER_COUNT))<1e-5f && guest_float((uintptr_t)cam+0x154)==scalar(c.v[0]));
        // with no stick load before it the store is the game's, bit for bit
        float neg_zero=-0.0f; memcpy(&c.v[0],&neg_zero,4); c.x[30]=MOD_MAIN_BASE+0x1d34374+4; load_hook(&c);
        float got=guest_float((uintptr_t)cam+0x154); assert(!memcmp(&got,&neg_zero,4));
        assert(LOAD(0x1d34584,1)==pad_x);
        guest_float_store((uintptr_t)(fp+0x10)-0x48,40);
        assert(fabsf(LOAD(0x1d345d0,1)-(40-100*DEG_PER_COUNT))<1e-5f);
        // Horse yaw: the turn ([sp+0x18]) the helper adds to +0x68 gets the degrees
        SAMPLE(100,-50); c.x[20]=(uintptr_t)input; guest_float_store((uintptr_t)lsp+0x18,0.5f);
        assert(LOAD(0x1d4070c,8)==pad_x && fabsf(LOAD(0x1d40740,0)-(0.5f-100*DEG_PER_COUNT))<1e-5f);
        // Wakeboard reads each axis twice: the first reads give the same values and leave the degrees to the second
        SAMPLE(100,-50); c.x[22]=(uintptr_t)input;
        float total=guest_float((uintptr_t)input+0x424);
        guest_float_store((uintptr_t)cam+0xd8,12); guest_float_store((uintptr_t)lsp+0x38,20);
        assert(LOAD(0x1d50738,0)==pad_x && LOAD(0x1d50740,1)==total);
        assert(LOAD(0x1d50768,0)==total && LOAD(0x1d5076c,1)==pad_x);
        assert(fabsf(LOAD(0x1d50e5c,0)-(12-50*DEG_PER_COUNT))<1e-5f);
        assert(fabsf(LOAD(0x1d50f20,12)-(20-100*DEG_PER_COUNT))<1e-5f);
        assert(LOAD(0x1d50f20,12)==20);   // once
        // Waterfall climbing: the yaw is loaded into w8 as bits by ldr w8, [sp, #0x38]
        SAMPLE(100,-50); c.x[22]=(uintptr_t)input; guest_float_store((uintptr_t)lsp+0x38,11);
        assert(LOAD(0x1d51e10,13)==pad_x);
        c.x[30]=MOD_MAIN_BASE+0x1d51fb8+4; c.x[8]=~0ull; load_hook(&c);
        float w; uint32_t wb=(uint32_t)c.x[8]; memcpy(&w,&wb,4);
        assert(c.x[8]>>32==0 && fabsf(w-(11-100*DEG_PER_COUNT))<1e-5f);
        // Dialogue: the patched mov set x23 = x19 + 0x5c; the pitch load is ldr s1, [x23]
        SAMPLE(100,-50); c.x[24]=(uintptr_t)input; c.x[23]=(uintptr_t)cam+0x5c; guest_float_store((uintptr_t)cam+0x5c,1);
        assert(LOAD(0x1d3a3f8,11)==pad_y && fabsf(LOAD(0x1d3a420,1)-(1-50*DEG_PER_COUNT))<1e-5f);
        assert(c.x[23]==(uintptr_t)cam+0x5c);
        #undef LOAD
        #undef SAMPLE
    }
    puts("direct mouse 1.2.1: camera input in s11, activity, orbit angles at sp+0x3c/0x40, Ultrahand in s9 passed");
    puts("direct mouse 1.2.1: aiming pitch store, horse turn, repeated stick reads, w8 yaw and dialogue loads passed");
}
#endif
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

def bl_target(address, word):
    """The target of a bl at address, None for another instruction."""
    if word >> 26 != 0x25:
        return None
    imm = word & 0x3ffffff
    return address + 4 * (imm - (1 << 26) if imm & (1 << 25) else imm)


def check_tables():
    """Each version's load-site table (runtime/mouse_direct.c) against its patch file: one bl to page +0xf80 a site, the
    same original word (a pre-indexed ldr s<t>, [x<n>, #imm]! is the plain ldr s<t>, [x<n>] after its patched mov)."""
    source = (ROOT / 'runtime' / 'mouse_direct.c').read_text()
    for version, page in (('143', 0x3adf000), ('121', 0x472b000)):
        block = re.search(r'load_sites_%s\[\] = \{(.*?)\n\};' % version, source, re.S)[1]
        table = {int(a, 16): int(w, 16) for a, w in re.findall(r'\{0x([0-9a-f]+), 0x([0-9a-f]+),', block)}
        assert len(table) == block.count('{0x'), f'v{version}: a site twice in the table'
        sites = {}
        for line in (ROOT / 'recomp' / f'patches-v{version}.txt').read_text().splitlines():
            m = re.match(r'site mouse_direct main ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+)', line)
            if m and bl_target(int(m[1], 16), int(m[3], 16)) == page + 0xf80:
                assert int(m[1], 16) not in sites, f'v{version} {m[1]} twice in the patch file'
                sites[int(m[1], 16)] = int(m[2], 16)
        assert sites.keys() == table.keys(), (version, sorted(hex(a) for a in set(sites) ^ set(table)))
        for a, orig in sites.items():
            # ldr s<t>, [x<n>, #imm]! -> ldr s<t>, [x<n>]
            plain = 0xbd400000 | (orig & 0x3ff) if (orig & 0xffe00c00) == 0xbc400c00 else orig
            assert table[a] == plain, f'v{version} {a:x}: table {table[a]:08x}, patch file {orig:08x}'
        print(f'direct mouse: the v{version} load-site table matches recomp/patches-v{version}.txt ({len(table)} sites)')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', required=True)
    ap.add_argument('--deps', type=Path, help='dependency include directory (SDL3/Vulkan), to also check input scripts')
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='totk-mouse-') as tmp:
        folder = Path(tmp)
        (folder / 'patches.h').write_text('#define RT_PATCH_PAGE 0x7003adf000ull\n')
        (folder / 'layout.h').write_text('#define MOD_MAIN_BASE 0x7000000000ull\n')
        source = folder / 'check.c'
        source.write_text(HARNESS)
        exe = folder / ('check.exe' if os.name == 'nt' else 'check')
        link = [] if os.name == 'nt' else ['-lm']
        for version in (143, 121):
            subprocess.run([args.cc, '-O2', '-Wall', '-Wextra', f'-DTEST_GAME_VERSION={version}', '-I', str(folder),
                            '-I', str(ROOT/'runtime'), str(source), *link, '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
        check_tables()
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
