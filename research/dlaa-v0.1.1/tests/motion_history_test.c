#include <assert.h>
#include <stdio.h>
#include "motion_history.h"
int main(void) {
    Mh h = {0};
    mh_begin(&h, 1); int a = mh_add(&h, 10, 20);
    assert(mh_previous(&h, a) == -1); mh_seal(&h);
    mh_begin(&h, 2); a = mh_add(&h, 10, 20);
    assert(mh_previous(&h, a) == 0);
    int b = mh_add(&h, 10, 20);
    assert(mh_previous(&h, a) == -1 && mh_previous(&h, b) == -1); mh_seal(&h);
    mh_begin(&h, 3); a = mh_add(&h, 10, 20);
    assert(mh_previous(&h, a) == -1); mh_seal(&h);
    mh_begin(&h, 5); a = mh_add(&h, 10, 20);
    assert(mh_previous(&h, a) == -1); /* frame gap */
    mh_begin(&h, 6); a = mh_add(&h, 10, 20);
    assert(mh_previous(&h, a) == -1); /* previous frame not sealed */
    mh_seal(&h); assert(mh_add(&h, 30, 40) == -1);
    assert(!mh_seal(&h)); /* second scene invalidates the frame */
    mh_begin(&h, 7); a = mh_add(&h, 10, 20);
    assert(mh_previous(&h, a) == -1);
    for (int i = 1; i < MH_CAP; i++) assert(mh_add(&h, i + 100, i + 200) >= 0);
    assert(mh_add(&h, 10000, 20000) == -1);
    assert(!mh_seal(&h)); /* overflow must invalidate the next frame too */
    mh_begin(&h, 8); a=mh_add(&h,10,20); assert(mh_previous(&h,a)==-1);
    float view0[3][4]={{1,0,0,0},{0,1,0,0},{0,0,1,0}};
    float view1[3][4]={{1,0,0,-6},{0,1,0,0},{0,0,1,0}};
    assert(mh_camera_distance2(view0,view1)==36);
    float rotated[3][4]={{0,0,1,0},{0,1,0,0},{-1,0,0,6}};
    assert(mh_camera_distance2(view1,rotated)==0);
    puts("motion history: passed"); return 0;
}
