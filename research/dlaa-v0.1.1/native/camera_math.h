#pragma once
#include <math.h>
#include <string.h>
// Row-major input. Preserve double precision through inversion/composition, then upload float rows.
static int cm_vp(double out[16],const float view[12],const float projection[16]) {
    for(int i=0;i<12;i++)if(!isfinite(view[i]))return 0;
    for(int i=0;i<16;i++)if(!isfinite(projection[i]))return 0;
    for(int i=0;i<4;i++)for(int j=0;j<4;j++){
        double value=j==3?projection[i*4+3]:0;
        for(int k=0;k<3;k++)value+=(double)projection[i*4+k]*view[k*4+j];
        out[i*4+j]=value;
    }
    return 1;
}
static int cm_reprojection(float out[16],const float view[12],const float projection[16],const float old_view[12],const float old_projection[16]) {
    double now[16],old[16],aug[4][8];
    if(!cm_vp(now,view,projection)||!cm_vp(old,old_view,old_projection))return 0;
    for(int i=0;i<4;i++)for(int j=0;j<8;j++)aug[i][j]=j<4?now[i*4+j]:(j-4==i);
    for(int k=0;k<4;k++){
        int pivot=k;for(int i=k+1;i<4;i++)if(fabs(aug[i][k])>fabs(aug[pivot][k]))pivot=i;
        if(fabs(aug[pivot][k])<1e-12)return 0;
        for(int j=0;j<8;j++){double a=aug[k][j];aug[k][j]=aug[pivot][j];aug[pivot][j]=a;}
        double divisor=aug[k][k];for(int j=0;j<8;j++)aug[k][j]/=divisor;
        for(int i=0;i<4;i++)if(i!=k){double factor=aug[i][k];for(int j=0;j<8;j++)aug[i][j]-=factor*aug[k][j];}
    }
    for(int i=0;i<4;i++)for(int j=0;j<4;j++){
        double value=0;for(int k=0;k<4;k++)value+=old[i*4+k]*aug[k][j+4];
        if(!isfinite(value)||fabs(value)>1e30)return 0;
        out[i*4+j]=(float)value;
    }
    return 1;
}
static int cm_agrees(const float *a,const float *b,int n) {
    for(int i=0;i<n;i++)if(!isfinite(a[i])||!isfinite(b[i])||fabsf(a[i]-b[i])>1e-4f+1e-5f*fabsf(a[i]))return 0;
    return 1;
}
