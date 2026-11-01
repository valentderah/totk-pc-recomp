#pragma once
#include <math.h>
#include "camera_math.h"
// Deliberately narrow diagnostic predicate, not a universal camera classifier.
static int pj_camera_structure(const float *a,unsigned bytes){
    if(!a||bytes!=0x940)return 0;
    for(int i=0;i<76;i++)if(!isfinite(a[i]))return 0;
    for(int i=0;i<3;i++)for(int j=0;j<3;j++){
        double dot=0;for(int k=0;k<3;k++)dot+=(double)a[i*4+k]*a[j*4+k];
        if(fabs(dot-(i==j))>1e-3)return 0;
    }
    if(fabsf(a[56]-.1f)>1e-5f||fabsf(a[57]-25000)>2.5f)return 0;
    const float *p=a+28;
    if(p[0]<=0||p[5]<=0||fabsf(p[5]/p[0]-16.0f/9)>1e-4f||p[10]>=-1||p[11]>=0)return 0;
    const int zero[]={1,2,3,4,6,7,8,9,12,13,15};for(unsigned i=0;i<sizeof zero/sizeof zero[0];i++)if(fabsf(p[zero[i]])>1e-6f)return 0;
    if(fabsf(p[14]+1)>1e-6f)return 0;
    double vp[16];if(!cm_vp(vp,a,p))return 0;
    for(int i=0;i<16;i++)if(fabs(vp[i]-a[12+i])>5e-4+2e-5*fabs(a[12+i]))return 0;
    return 1;
}
static int pj_camera(const float *a,unsigned bytes,unsigned width,unsigned height){
    return width==2560&&height==1440&&pj_camera_structure(a,bytes)&&fabsf(a[72]-width)<=.1f&&fabsf(a[73]-height)<=.1f;
}
static int pj_reconstruction_camera(const float *a,unsigned bytes,unsigned width,unsigned height,const float reference_vp[16]){
    if(!reference_vp||!width||!height||width>2560||height>1440||width*9!=height*16||!pj_camera_structure(a,bytes))return 0;
    int native=fabsf(a[72]-2560)<=.1f&&fabsf(a[73]-1440)<=.1f;
    int local=fabsf(a[72]-width)<=.1f&&fabsf(a[73]-height)<=.1f;
    if(!native&&!local)return 0;
    for(int i=0;i<16;i++)if(!isfinite(reference_vp[i])||fabs(a[12+i]-reference_vp[i])>5e-4+2e-5*fabs(reference_vp[i]))return 0;
    if(fabs((double)a[68]*a[28]-1)>1e-3||fabs((double)a[69]*a[33]-1)>1e-3)return 0;
    for(int i=0;i<3;i++)for(int j=0;j<4;j++){
        double value=j==3?a[44+i*4+3]:0;
        for(int k=0;k<3;k++)value+=(double)a[44+i*4+k]*a[k*4+j];
        if(fabs(value-(i==j))>5e-4)return 0;
    }
    return 1;
}
