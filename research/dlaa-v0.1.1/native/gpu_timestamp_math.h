#pragma once
#include <stdint.h>
#include <math.h>
static int gt_intervals(const uint64_t values[8],unsigned mask,unsigned bits,double period,double out[7]){
    if(mask!=255||!bits||bits>64||!isfinite(period)||period<=0)return 0;
    uint64_t wrap=bits==64?UINT64_MAX:((UINT64_C(1)<<bits)-1);
    for(unsigned i=0;i<7;i++)out[i]=(double)((values[i+1]-values[i])&wrap)*period/1000000.0;
    return 1;
}
