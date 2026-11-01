// Adapted from MacNativeGames games/totk/runtime/audio_effects.c @ b75be8c (MIT, Copyright (c) 2026 MacNativeGames contributors).
// Algorithms from Ryujinx.Audio (MIT, Copyright (c) Ryujinx Team and Contributors).
/* Delay, standard reverb and compressor DSP, ported from Ryujinx.Audio.
   Copyright (c) Ryujinx Team and Contributors. MIT license:
   third_party/licenses/Ryujinx-Audio.txt. Runtime samples are normalized floats. */
#include "audio_effects.h"
#include "host.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float *data; unsigned max, count, at; } DelayLine;
struct AudioEffectDsp {
    unsigned type, revision, channels;
    union { AudrenDelayParam delay; AudrenReverbParam reverb; AudrenCompressorParam compressor; } p;
    DelayLine line[10];
    float history[6], direct[4], previous[4], decay_rate;
    unsigned early_time[10], pre_time;
    float early_gain[10];
    float input_mean, gain_mean, last_target, last_alpha, output_gain, makeup;
    float stats_min, stats_mean, stats_last[6];
};
static int channel_ok(unsigned n) { return n==1 || n==2 || n==4 || n==6; }
static unsigned sample_count(unsigned rate, float ms) { return (unsigned)nearbyintf(rate*ms); }
/* The reference DelayLine wraps its cursor at CurrentSampleCount-1 but taps
   modulo CurrentSampleCount, so once per turn a tap reads a slot the cursor never
   writes (a one-sample dropout per tap, heard as grain in the reverb). Use one
   ring length of count samples for both; the delays are one sample longer. */
static void line_set(DelayLine *d, unsigned n) { d->count=n<d->max?n:d->max; d->at=0; }
static void line_init(DelayLine *d, unsigned rate, float ms) {
    d->max=sample_count(rate,ms); d->data=calloc((size_t)d->max+1,sizeof(float));
    if(!d->data)rt_fatal("audio effect delay allocation"); line_set(d,d->max);
}
static float line_read(const DelayLine *d) { return d->data[d->at]; }
static float line_update(DelayLine *d,float v) {
    float old=line_read(d);d->data[d->at++]=v;
    if(d->at >= d->count)d->at=0;
    return old;
}
static float line_tap(const DelayLine *d,unsigned sample,int offset) {
    int at=(int)d->at-(int)sample-offset;
    if(at<0)at+=(int)d->count;
    if(at>=(int)d->count && d->count)at-=(int)d->count;
    if(at<0 || (unsigned)at>d->max)rt_fatal("audio effect delay tap bounds");
    return d->data[at];
}
static float gain10(float x) { return x>=0?1:x<=-5.3f?0:powf(10,x); }
static const float fdn_time[4][4]={{53.953247f,79.192566f,116.238770f,130.615295f},{53.953247f,79.192566f,116.238770f,170.615295f},{5,10,5,10},{47.03f,71,103,170}};
static const float decay_time[4][4]={{7,9,13,17},{7,9,13,17},{1,1,1,1},{7,7,13,9}};
/* Early reflections are indexed by EarlyMode. These are Ryujinx.Audio's five
   tables; its own version reads the first row for every mode. */
static const float early_time[5][10]={
    {0,3.5f,2.8f,3.9f,2.7f,13.4f,7.9f,8.4f,9.9f,12},
    {0,11.8f,5.5f,11.2f,10.4f,38.1f,22.2f,29.6f,21.2f,24.8f},
    {0,41.5f,20.5f,41.3f,0,29.5f,33.8f,45.2f,46.8f,0},
    {33.1f,43.3f,22.8f,37.9f,14.9f,35.3f,17.9f,34.2f,0,43.3f},
    {0}
};
static const float early_gain[5][10]={
    {.70f,.68f,.70f,.68f,.70f,.68f,.70f,.68f,.68f,.68f},
    {.70f,.68f,.70f,.68f,.70f,.68f,.68f,.68f,.68f,.68f},
    {.50f,.70f,.70f,.68f,.50f,.68f,.68f,.70f,.68f,0},
    {.93f,.92f,.87f,.86f,.94f,.81f,.80f,.77f,.76f,.65f},
    {0}
};
static const float pre_time[5]={12.5f,40,50,50,0};
static void validate_delay(const AudrenDelayParam *p) {
    unsigned rate=(p->sample_rate>>14)/1000;
    if(!channel_ok(p->channels) || p->channel_max<p->channels || rate<1 || rate>192 || p->delay_max>20000 || p->delay>p->delay_max)
        rt_fatal("audio delay parameters");
}
static void validate_reverb(const AudrenReverbParam *p) {
    if(!channel_ok(p->channels) || p->channel_max<p->channels || p->sample_rate<16384 || p->sample_rate>192*16384 ||
        p->early_mode<0 || p->early_mode>4 || p->late_mode<0 || p->late_mode>3 || p->decay<=0 || p->hf_decay<=0 || p->pre_delay<0)
        rt_fatal("audio standard reverb parameters");
}
static void validate_compressor(const AudrenCompressorParam *p) {
    if(!channel_ok(p->channels) || p->channel_max<p->channels || !isfinite(p->threshold) || !(p->ratio>0) || !isfinite(p->ratio) ||
        !(p->input_gain>=0 && p->input_gain<=1) || !(p->attack>=0 && p->attack<=1) || !(p->release>=0 && p->release<=1) || !isfinite(p->output_gain))
        rt_fatal("audio compressor parameters");
}
AudioEffectDsp *audio_effect_create(unsigned type,const void *param,unsigned revision) {
    AudioEffectDsp *s=calloc(1,sizeof *s);if(!s)rt_fatal("audio effect allocation");
    s->type=type;s->revision=revision;s->gain_mean=s->last_target=s->stats_min=1;
    if(type==3) {
        AudrenDelayParam p;memcpy(&p,param,sizeof p);validate_delay(&p);s->channels=p.channels;
        for(unsigned c=0;c<s->channels;c++){line_init(&s->line[c],(p.sample_rate>>14)/1000,p.delay_max);line_set(&s->line[c],sample_count((p.sample_rate>>14)/1000,p.delay));}
        p.status=0;audio_effect_update(s,&p);
    } else if(type==4) {
        AudrenReverbParam p;memcpy(&p,param,sizeof p);validate_reverb(&p);s->channels=p.channels;unsigned khz=(unsigned)p.sample_rate>>14;
        for(int i=0;i<4;i++){line_init(&s->line[i],khz,fdn_time[1][i]);line_init(&s->line[i+4],khz,decay_time[1][i]);}
        line_init(&s->line[8],khz,revision>=3?350:150);line_init(&s->line[9],khz,5);
        p.status=0;audio_effect_update(s,&p);
    } else if(type==9) {
        AudrenCompressorParam p;memcpy(&p,param,sizeof p);validate_compressor(&p);s->channels=p.channels;
        p.status=0;audio_effect_update(s,&p);
    } else rt_fatal("audio effect DSP type");
    return s;
}
void audio_effect_free(AudioEffectDsp *s) { if(s){for(int i=0;i<10;i++)free(s->line[i].data);free(s);} }
void audio_effect_update(AudioEffectDsp *s,const void *param) {
    if(s->type==3){
        AudrenDelayParam p;memcpy(&p,param,sizeof p);validate_delay(&p);
        if(p.channels!=s->channels || (s->p.delay.sample_rate && (p.sample_rate!=s->p.delay.sample_rate || p.delay_max!=s->p.delay.delay_max)))rt_fatal("audio delay requires reset");
        s->p.delay=p;
    } else if(s->type==4){
        AudrenReverbParam p;memcpy(&p,param,sizeof p);validate_reverb(&p);if(p.channels!=s->channels)rt_fatal("audio reverb requires reset");s->p.reverb=p;
        if(p.status>=2)return;
        unsigned rate=(unsigned)p.sample_rate>>14;
        for(int i=0;i<10;i++){
            unsigned n=sample_count(rate,early_time[p.early_mode][i]+p.pre_delay/16384.f);s->early_time[i]=(n<s->line[8].max?n:s->line[8].max)+1;
            s->early_gain[i]=early_gain[p.early_mode][i]*(p.early_gain/16384.f);
        }
        if(p.channels==2){s->early_gain[4]*=.5f;s->early_gain[5]*=.5f;}
        unsigned n=sample_count(rate,pre_time[p.early_mode]+p.pre_delay/16384.f);s->pre_time=n<s->line[8].max?n:s->line[8].max;
        float decay=p.decay/16384.f,hf=p.hf_decay/16384.f,co=cosf((1280.f/rate)*(3.14159265358979323846f/180));
        s->decay_rate=.6f*(1-p.coloration/16384.f);
        for(int i=0;i<4;i++){
            line_set(&s->line[i],sample_count(rate,fdn_time[p.late_mode][i]));line_set(&s->line[4+i],sample_count(rate,decay_time[p.late_mode][i]));
            float a=-3.f*(s->line[i].count+s->line[i+4].count),b=a/(decay*rate),c=0;
            if(hf<.995f){float e=gain10(((1/hf-1)*2)/100*(b/10)),f=1-e,g=2-e*2*co;c=(g-sqrtf(g*g-f*f*4))/(f*2);}
            s->direct[i]=gain10(b/1000)*(1-c)*.7071f;s->previous[i]=c;s->history[i]=0;
        }
    } else {
        AudrenCompressorParam p;memcpy(&p,param,sizeof p);validate_compressor(&p);if(p.channels!=s->channels)rt_fatal("audio compressor requires reset");s->p.compressor=p;
        if(p.status<2){s->last_alpha=p.attack;s->makeup=p.makeup?p.threshold*.5f*(1/p.ratio-1)-3:0;s->output_gain=powf(10,(p.output_gain+s->makeup)/20);}
    }
}
static void delay_process(AudioEffectDsp *s,float *const in[6],float *const out[6],int frames) {
    const AudrenDelayParam *p=&s->p.delay;unsigned n=s->channels;
    float fb=p->feedback/16384.f,spread=p->spread/16384.f,base=(1-spread)*fb*.98f,cross=spread*fb*.98f/(n>=4?2:1),lp=.95f*(p->lowpass/16384.f);
    static const int quad[4][2]={{1,2},{0,3},{0,3},{1,2}};
    static const int surround[6][2]={{2,4},{2,5},{0,1},{3,3},{0,5},{1,4}};
    for(int k=0;k<frames;k++){
        float x[6],d[6],v[6];for(unsigned c=0;c<n;c++){x[c]=in[c][k]*64;d[c]=line_read(&s->line[c]);}
        for(unsigned c=0;c<n;c++){
            float feedback=n==1 || (n==6 && c==3)?d[c]*fb:d[c]*base;
            if(n==2)feedback+=d[1-c]*cross;
            else if(n>=4 && !(n==6 && c==3)){const int *q=n==4?quad[c]:surround[c];feedback+=(d[q[0]]+d[q[1]])*cross;}
            v[c]=x[c]*(p->in_gain/16384.f)+feedback;
            float z=lp*s->history[c]+v[c]*(1-lp);s->history[c]=z;line_update(&s->line[c],z);
            out[c][k]=(x[c]*(p->dry_gain/16384.f)+d[c]*(p->out_gain/16384.f))/64;
        }
    }
}
static void reverb_process(AudioEffectDsp *s,float *const in[6],float *const out[6],int frames) {
    const AudrenReverbParam *p=&s->p.reverb;unsigned n=s->channels;
    static const int mono[10]={0,0,0,0,0,0,0,0,0,0},stereo[10]={0,0,1,1,0,1,0,0,1,1},quad[10]={0,0,1,1,0,1,2,2,3,3},surround[10]={0,0,1,1,4,4,2,2,3,3};
    static const int feedback_stereo[4]={2,0,3,1},output_stereo[4]={0,0,1,1};
    const int *map=n==1?mono:n==2?stereo:n==4?quad:surround;
    for(int k=0;k<frames;k++){
        float y[6]={0},x[6],fb[4],v[4],sum=0;
        for(int i=0;i<10;i++){float tap=line_tap(&s->line[8],s->early_time[i],0)*s->early_gain[i];y[map[i]]+=tap;if(n==6)y[5]+=tap;}
        if(n==6)y[5]*=.2f;
        for(unsigned c=0;c<n;c++){x[c]=in[c][k]*64;sum+=x[c]*(p->reverb_gain/16384.f);}line_update(&s->line[8],sum);
        unsigned at=s->pre_time;if(at>=s->line[8].count)at=s->line[8].count-1;
        float late=line_tap(&s->line[8],at,-1)*(p->late_gain/16384.f);
        for(int i=0;i<4;i++){v[i]=line_read(&s->line[i])*s->direct[i]+s->history[i]*s->previous[i];s->history[i]=v[i];}
        fb[0]=v[2]+v[1];fb[1]=-v[0]-v[3];fb[2]=v[0]-v[3];fb[3]=v[1]-v[2];
        for(int i=0;i<4;i++){float d=line_read(&s->line[4+i]),z=fb[i]+late-s->decay_rate*d;v[i]=line_update(&s->line[4+i],z)+z*s->decay_rate;line_update(&s->line[i],v[i]);}
        if(n==1)for(int i=0;i<4;i++)y[0]+=v[i];
        else if(n==2)for(int i=0;i<4;i++)y[output_stereo[i]]+=v[feedback_stereo[i]];
        else {for(int i=0;i<4;i++)y[i]+=v[i];if(n==6){y[5]+=v[3];y[4]+=line_update(&s->line[9],(v[2]-v[3])*.5f);}}
        for(unsigned c=0;c<n;c++)out[c][k]=(y[c]*(p->out_gain/16384.f)+x[c]*(p->dry_gain/16384.f))/64;
    }
}
static void compressor_process(AudioEffectDsp *s,float *const in[6],float *const out[6],int frames) {
    const AudrenCompressorParam *p=&s->p.compressor;
    if(p->reset_statistics){s->stats_min=1;s->stats_mean=0;memset(s->stats_last,0,sizeof s->stats_last);}
    for(int k=0;k<frames;k++){
        float x[6],mean=0;for(unsigned c=0;c<s->channels;c++){x[c]=in[c][k];mean+=x[c]*x[c];}mean/=s->channels;
        s->input_mean+=p->input_gain*(mean-s->input_mean);float db=10*log10f(fmaxf(s->input_mean,1e-10f)),z=1;
        if(db>=p->threshold-1.5f){float g=db>=p->threshold+1.5f?(1/p->ratio-1)*(db-p->threshold):(db-p->threshold+1.5f)*(db-p->threshold+1.5f)*(-(1-1/p->ratio)/6);z=powf(10,g/20);}
        float alpha,target=z,diff=s->last_target-z;
        if(diff>.08f)alpha=p->attack;
        else if(diff<-.08f)alpha=p->release;
        else {if(fabsf(s->gain_mean-z)>=.001f)target=s->last_target;alpha=s->last_alpha;}
        s->gain_mean+=alpha*(z-s->gain_mean);s->last_target=target;s->last_alpha=alpha;
        for(unsigned c=0;c<s->channels;c++){out[c][k]=x[c]*s->gain_mean*s->output_gain;s->stats_last[c]=fabsf(x[c]);}
        s->stats_min=fminf(s->stats_min,s->gain_mean*s->output_gain);s->stats_mean=fmaxf(s->stats_mean,mean);
    }
}
void audio_effect_process(AudioEffectDsp *s,float *const input[6],float *const output[6],int frames) {
    if(s->type==3)delay_process(s,input,output,frames);else if(s->type==4)reverb_process(s,input,output,frames);else compressor_process(s,input,output,frames);
}
void audio_effect_statistics(AudioEffectDsp *s,void *output,unsigned bytes) {
    if(s && s->type==9 && bytes>=32){float stats[8]={s->stats_mean,s->stats_min};memcpy(stats+2,s->stats_last,24);memcpy(output,stats,32);}
}
