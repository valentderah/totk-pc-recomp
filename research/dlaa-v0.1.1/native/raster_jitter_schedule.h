#pragma once
#include <stdint.h>
static inline int rj_abort_needed_count(unsigned attempted,int scheduled,uint64_t frame,uint64_t end,unsigned count){
    return (count==8||count==32)&&end>=count&&frame>end&&attempted>0&&attempted<count&&!scheduled;
}
static inline int rj_abort_needed(unsigned attempted,int scheduled,uint64_t frame,uint64_t end){return rj_abort_needed_count(attempted,scheduled,frame,end,8);}
// Eight Halton(2,3) samples centered at zero, in native top-left pixel coordinates.
// Outside the bounded window the real raster returns to zero jitter.
static inline int rj_schedule_count(uint64_t frame,uint64_t end,unsigned count,float jitter[2]){
    static const float samples[8][2]={{0,-1.f/6},{-.25f,1.f/6},{.25f,-7.f/18},{-.375f,-1.f/18},{.125f,5.f/18},{-.125f,-5.f/18},{.375f,1.f/18},{-.4375f,7.f/18}};
    jitter[0]=jitter[1]=0;if((count!=8&&count!=32)||end<count||frame<end-count+1||frame>end)return 0;
    unsigned i=(unsigned)((frame-(end-count+1))%8);jitter[0]=samples[i][0];jitter[1]=samples[i][1];return 1;
}

static inline int rj_schedule(uint64_t frame,uint64_t end,float jitter[2]){return rj_schedule_count(frame,end,8,jitter);}
static inline int rj_live_schedule(uint64_t frame,uint64_t ready,float jitter[2]){
    jitter[0]=jitter[1]=0;if(frame<1000||!ready||ready+1!=frame)return 0;
    uint64_t phase=(frame-1000)%8;return rj_schedule_count(phase+1,8,8,jitter);
}
