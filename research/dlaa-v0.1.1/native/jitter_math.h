#pragma once
#include <math.h>
#include <string.h>
// Row-major P. Pixel offsets are top-left image coordinates; Vulkan viewport height is negative.
// clip.x += 2*jx/width * clip.w; clip.y -= 2*jy/height * clip.w. Z and W are untouched.
static int jm_offset(float out[16],const float p[16],float jx,float jy,unsigned width,unsigned height){
    if(!out||!p||!width||!height||width>8192||height>8192||!isfinite(jx)||!isfinite(jy)||fabsf(jx)>.5f||fabsf(jy)>.5f)return 0;
    float result[16];for(int i=0;i<16;i++)if(!isfinite(p[i]))return 0;
    memcpy(result,p,sizeof result);
    for(int c=0;c<4;c++){result[c]+=2*jx/width*p[12+c];result[4+c]-=2*jy/height*p[12+c];}
    memcpy(out,result,sizeof result);return 1;
}
