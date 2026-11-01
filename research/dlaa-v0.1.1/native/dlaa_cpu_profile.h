#pragma once
#include <stdlib.h>
typedef struct {uint64_t first,total,peak,samples;} DlaaCpuProfile;
static int dc_enabled(void){static int checked,on;if(!checked){const char *p=getenv("TOTK_DLAA_CPU_STATS");on=p&&*p=='1';checked=1;}return on;}
static uint64_t dc_start(void){return dc_enabled()?kern_now_ns():0;}
static void dc_end(DlaaCpuProfile *s,const char *name,uint64_t begin,uint64_t frame,unsigned scale){
    if(!begin)return;uint64_t ns=kern_now_ns()-begin;
    if(!s->samples&&!s->first)s->first=frame;
    s->total+=ns;s->samples++;if(ns>s->peak)s->peak=ns;
    if(frame>=s->first+120){
        rt_log("DLAA CPU: %s: %.3f ms/frame, peak sample %.3f ms, %llu samples over %llu frames (scale %u)",name,(double)s->total*scale/(frame-s->first)/1e6,s->peak/1e6,(unsigned long long)s->samples,(unsigned long long)(frame-s->first),scale);
        s->first=frame;s->total=s->samples=s->peak=0;
    }
}
