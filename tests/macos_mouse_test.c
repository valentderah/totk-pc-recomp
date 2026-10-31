#include "mouse_input.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(value) do { assert(value); ++checks; } while (0)
static int same(NativeMouseSample a, NativeMouseSample b) {
    return a.buttons==b.buttons && a.x==b.x && a.y==b.y;
}

int main(void) {
    NativeMouseInput m = {0};
    native_mouse_configure(&m,1.0,false);
    CHECK(!native_mouse_motion(&m,5,5,1000000000));
    CHECK(!native_mouse_button(&m,0,true));
    CHECK(same(native_mouse_sample(&m,1,1000000000),(NativeMouseSample){0}));
    native_mouse_capture(&m,true,1000000000);
    CHECK(native_mouse_motion(&m,15,-15,1005000000));
    NativeMouseSample first=native_mouse_sample(&m,2,1010000000);
    CHECK(first.x==16384 && first.y==-16384 && !first.buttons);
    // Another SDK poll in the same frame must neither erase nor reconsume input.
    CHECK(same(first,native_mouse_sample(&m,2,1011000000)));
    CHECK(native_mouse_motion(&m,30,0,1012000000));
    CHECK(same(first,native_mouse_sample(&m,2,1013000000)));
    CHECK(native_mouse_sample(&m,3,1020000000).x==32767);
    CHECK(!native_mouse_sample(&m,4,1030000000).x);
    CHECK(!native_mouse_motion(&m,NAN,1,1030000000));
    CHECK(!native_mouse_motion(&m,1,INFINITY,1030000000));

    // A click shorter than a game frame survives all reads of that frame.
    const uint64_t mappings[]={8,256,512,1,2};
    uint64_t frame=5, now=1040000000;
    for (unsigned button=0;button<5;button++) {
        CHECK(native_mouse_button(&m,button,true));
        CHECK(!native_mouse_button(&m,button,true));
        CHECK(native_mouse_button(&m,button,false));
        NativeMouseSample pulse=native_mouse_sample(&m,frame,now);
        CHECK(pulse.buttons==mappings[button]);
        CHECK(same(pulse,native_mouse_sample(&m,frame,now+1000)));
        CHECK(!native_mouse_sample(&m,++frame,now+=10000000).buttons);
        frame++;now+=10000000;
    }
    CHECK(!native_mouse_button(&m,5,true));
    CHECK(native_mouse_button(&m,0,true));
    CHECK(native_mouse_button(&m,1,true));
    CHECK(native_mouse_sample(&m,++frame,now+=10000000).buttons==(8|256));
    native_mouse_capture(&m,false,now);
    CHECK(same(native_mouse_sample(&m,++frame,now),(NativeMouseSample){0}));
    native_mouse_capture(&m,true,now);
    CHECK(same(native_mouse_sample(&m,++frame,now+10000000),(NativeMouseSample){0}));
    // Focus transitions discard deltas, held buttons and latched edges.
    native_mouse_motion(&m,500,500,now);
    native_mouse_button(&m,0,true);
    native_mouse_clear(&m,now);
    CHECK(same(native_mouse_sample(&m,++frame,now+10000000),(NativeMouseSample){0}));

    // The same physical velocity yields the same stick position at every FPS.
    const unsigned rates[]={30,60,90,144,240};
    for (unsigned rate=0;rate<5;rate++) {
        uint64_t dt=1000000000ULL/rates[rate];
        native_mouse_capture(&m,true,1000000000);
        for (unsigned tick=1;tick<=rates[rate];tick++) {
            uint64_t t=1000000000+tick*dt;
            native_mouse_motion(&m,750.0*dt/1e9,1500.0*dt/1e9,t);
            NativeMouseSample s=native_mouse_sample(&m,tick,t);
            CHECK(abs(s.x-8192)<=1 && abs(s.y-16384)<=1);
        }
    }
    native_mouse_capture(&m,true,1000000000);
    native_mouse_configure(&m,2.0,true);
    native_mouse_motion(&m,15,15,1005000000);
    NativeMouseSample inverted=native_mouse_sample(&m,1,1010000000);
    CHECK(inverted.x==32767 && inverted.y==-32767);
    native_mouse_motion(&m,1000,1000,1020000000);
    CHECK(same(native_mouse_sample(&m,2,1200000000),(NativeMouseSample){0}));
    native_mouse_configure(&m,0,false);
    CHECK(!native_mouse_motion(&m,1,1,1200000000));
    native_mouse_configure(&m,NAN,false);CHECK(m.sensitivity==1);
    native_mouse_configure(&m,100,false);CHECK(m.sensitivity==10);
    native_mouse_configure(&m,-1,false);CHECK(!m.sensitivity);

    // Direct camera motion preserves distance, including sub-count movements
    // and flicks beyond the analog stick's maximum, at every update rate.
    native_mouse_configure(&m,1,false);
    m.direct=true;
    const double radians_per_count=0.08*3.14159265358979323846/180.0;
    for(unsigned rate=0;rate<5;rate++) {
        native_mouse_capture(&m,true,1000000000);
        double yaw=0,pitch=0;
        for(unsigned tick=1;tick<=rates[rate];tick++) {
            uint64_t t=1000000000+tick*(1000000000ULL/rates[rate]);
            native_mouse_camera(&m,tick-1,true);
            native_mouse_motion(&m,3600.0/rates[rate],-1200.0/rates[rate],t);
            NativeMouseSample pad=native_mouse_sample(&m,tick,t);
            CHECK(!pad.x && !pad.y);
            NativeMouseTurn turn=native_mouse_turn(&m,t);
            yaw+=turn.yaw;pitch+=turn.pitch;
            turn=native_mouse_turn(&m,t);
            CHECK(!turn.yaw && !turn.pitch);
        }
        CHECK(fabs(yaw-3600*radians_per_count)<1e-12);
        CHECK(fabs(pitch+1200*radians_per_count)<1e-12);
    }
    native_mouse_capture(&m,true,1000000000);
    native_mouse_camera(&m,1,true);
    native_mouse_motion(&m,30,0,1005000000);
    CHECK(!native_mouse_sample(&m,2,1010000000).x);
    // A paused normal camera relinquishes motion to a menu after one frame.
    CHECK(native_mouse_sample(&m,3,1020000000).x>0);
    CHECK(!native_mouse_turn(&m,1020000000).yaw);
    native_mouse_camera(&m,3,true);m.stick_override=true;
    native_mouse_motion(&m,30,0,1025000000);
    CHECK(!native_mouse_turn(&m,1025000000).yaw);
    CHECK(native_mouse_sample(&m,4,1030000000).x>0);
    m.stick_override=false;native_mouse_camera(&m,4,true);
    native_mouse_motion(&m,30,0,1035000000);
    CHECK(!native_mouse_sample(&m,5,1040000000).x);
    CHECK(fabs(native_mouse_turn(&m,1040000000).yaw-30*radians_per_count)<1e-12);
    native_mouse_capture(&m,true,1000000000);
    native_mouse_motion(&m,0.125,0.25,1001000000);
    NativeMouseTurn fine=native_mouse_turn(&m,1002000000);
    CHECK(fabs(fine.yaw-0.125*radians_per_count)<1e-12);
    CHECK(fabs(fine.pitch-0.25*radians_per_count)<1e-12);
    native_mouse_motion(&m,4500,0,1003000000);
    CHECK(fabs(native_mouse_turn(&m,1004000000).yaw-2*3.14159265358979323846)<1e-12);
    native_mouse_motion(&m,100,100,1005000000);
    NativeMouseTurn stale=native_mouse_turn(&m,1200000000);
    CHECK(!stale.yaw && !stale.pitch);
    native_mouse_motion(&m,100,100,1201000000);
    native_mouse_capture(&m,false,1202000000);
    NativeMouseTurn released=native_mouse_turn(&m,1203000000);
    CHECK(!released.yaw && !released.pitch);
    printf("{\"probe\":\"mouse-input\",\"status\":\"pass\",\"checks\":%u}\n",checks);
}
