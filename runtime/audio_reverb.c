// Adapted from MacNativeGames games/totk/runtime/audio_reverb.c @ b75be8c (MIT, Copyright (c) 2026 MacNativeGames contributors).
/* Compatible 3D reverb network, based on Ryujinx.Audio's Reverb3dState/Command,
   DelayLine3d and DecayDelay. Copyright (c) Ryujinx Team and Contributors.
   MIT license: third_party/licenses/Ryujinx-Audio.txt. Samples are normalized floats. */
#include "audio_reverb.h"
#include "host.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float *data; unsigned cap, delay, read, write; } Delay;
struct Reverb3d {
    Reverb3dParam p; Delay fdn[4], allpass1[4], allpass2[4], pre, center;
    unsigned early_time[20], reflection_time;
    float pre_previous, pre_gain, pre_target, early_gain, late_gain, diffusion;
    float direct[4], current[4], output[4], history[4];
};
static const float fdn_min[4] = {5,6,13,14}, fdn_max[4] = {45.704f,82.782f,149.94f,271.58f};
static const float ap1_max[4] = {17,13,9,7}, ap2_max[4] = {19,11,10,6};
static const float early_times[20] = {.017136f,.059154f,.16173f,.39019f,.42526f,.45541f,.68974f,.74591f,.83384f,.8595f,0,.075024f,.16879f,.2999f,.33744f,.3719f,.59901f,.71674f,.81786f,.85166f};
static const float early_gains[20] = {.67096f,.61027f,1,.35680f,.68361f,.65978f,.51939f,.24712f,.45945f,.45021f,.64196f,.54879f,.92925f,.38270f,.72867f,.69794f,.5464f,.24563f,.45214f,.44042f};
static unsigned samples(unsigned khz, float ms) { return (unsigned)nearbyintf(khz * ms); }
static void delay_set(Delay *d, unsigned n) { if (n > d->cap) rt_fatal("reverb delay exceeds capacity"); d->delay = n; d->write = (d->read + n) % (d->cap + 1); }
static void delay_init(Delay *d, unsigned khz, float ms) { d->cap = samples(khz, ms); d->data = calloc(d->cap + 1, sizeof(float)); if (!d->data) rt_fatal("reverb delay allocation"); delay_set(d, d->cap); }
static float delay_update(Delay *d, float v) {
    d->data[d->write++] = v; float out = d->data[d->read++];
    /* The ring is cap + 1 slots, as delay_set and delay_tap index it. */
    if (d->read > d->cap) d->read = 0;
    if (d->write > d->cap) d->write = 0;
    return out;
}
static float delay_tap(const Delay *d, unsigned at) { int i = (int)d->write - (int)at - 1; if (i < 0) i += (int)d->cap + 1; return d->data[i]; }
static float allpass(Delay *d, float x, float gain) { float v = x - gain * d->data[d->read]; return delay_update(d, v) + gain * v; }
static float gain10(float x) { return x >= 0 ? 1 : x <= -5.3f ? 0 : powf(10, x); }
static float radians(float x) { return x * 3.14159265358979323846f / 180; }
static void validate(const Reverb3dParam *p) {
    if ((p->channels != 1 && p->channels != 2 && p->channels != 4 && p->channels != 6) || p->sample_rate < 1000 || p->sample_rate > 192000 ||
        !(p->decay_time > 0) || !(p->hf_decay_ratio > 0) || !(p->density >= 0 && p->density <= 100) ||
        !(p->reflection_delay >= 0 && p->reverb_delay >= 0 && p->reflection_delay + p->reverb_delay <= .4f)) rt_fatal("invalid reverb3d parameters");
}
Reverb3d *audio_reverb_create(const Reverb3dParam *p) {
    validate(p); Reverb3d *s = calloc(1, sizeof *s); if (!s) rt_fatal("reverb allocation"); unsigned khz = p->sample_rate / 1000;
    for (int i=0;i<4;i++) { delay_init(&s->fdn[i],khz,fdn_max[i]); delay_init(&s->allpass1[i],khz,ap1_max[i]); delay_init(&s->allpass2[i],khz,ap2_max[i]); }
    delay_init(&s->pre,khz,400); delay_init(&s->center,khz,5); audio_reverb_update(s,p); return s;
}
void audio_reverb_free(Reverb3d *s) { if (!s) return; for (int i=0;i<4;i++) { free(s->fdn[i].data); free(s->allpass1[i].data); free(s->allpass2[i].data); } free(s->pre.data); free(s->center.data); free(s); }
void audio_reverb_update(Reverb3d *s, const Reverb3dParam *p) {
    validate(p); s->p = *p; unsigned khz = p->sample_rate / 1000;
    memset(s->history,0,sizeof s->history); s->pre_previous=0;
    s->early_gain=gain10(fminf(p->room_gain+p->reflections_gain,5000)/2000); s->late_gain=gain10(fminf(p->room_gain+p->reverb_gain,5000)/2000);
    float hf=gain10(p->room_hf/2000);
    if (hf<1) { float a=1-hf,b=2-2*hf*cosf(radians(256*p->hf_reference/p->sample_rate)); s->pre_gain=(b-sqrtf(b*b-4*a*a))/(2*a); s->pre_target=1-s->pre_gain; }
    else { s->pre_gain=0; s->pre_target=1; }
    s->reflection_time=samples(khz,1000*(p->reflection_delay+p->reverb_delay)); s->diffusion=.6f*p->diffusion*.01f;
    for (int i=0;i<4;i++) {
        delay_set(&s->fdn[i],samples(khz,fdn_min[i]+p->density/100*(fdn_max[i]-fdn_min[i])));
        float n=(float)(s->fdn[i].delay+s->allpass1[i].delay+s->allpass2[i].delay);
        float a=-60*n/(p->decay_time*p->sample_rate),b=a/p->hf_decay_ratio;
        float angle=radians(128*.5f*p->hf_reference/p->sample_rate),c=cosf(angle)/sinf(angle),d=gain10((b-a)/40),e=gain10((b+a)/40)*.7071f;
        s->direct[i]=e*(d*c+1)/(c+d); s->current[i]=e*(1-d*c)/(c+d); s->output[i]=(c-d)/(c+d);
    }
    for (int i=0;i<20;i++) { unsigned n=samples(khz,p->reflection_delay*1000+early_times[i]*1000*(p->reverb_delay*.9998f+.02f)); s->early_time[i]=n<s->pre.cap?n:s->pre.cap; }
}
void audio_reverb_process(Reverb3d *s, float *const input[6], float *const output[6], int frames) {
    static const unsigned stereo[20]={0,0,0,1,1,1,1,0,0,0,1,1,1,0,0,0,0,1,1,1};
    static const unsigned quad[20]={0,0,0,1,1,1,1,2,2,2,1,1,1,0,0,0,0,3,3,3};
    /* Preserve the reference's legacy surround channel ordering (L,R,BL,BR,C,LFE). */
    static const unsigned surround[20]={4,5,0,5,0,5,1,5,1,5,1,5,1,5,2,5,2,5,2,5};
    const int channels=s->p.channels;
    for (int k=0;k<frames;k++) {
        float dry[6],early[6]={0},fb[4],v[4],sum=0,tap=delay_tap(&s->pre,s->reflection_time);
        for (int i=0;i<20;i++) { unsigned out=channels==1?0:channels==2?stereo[i]:channels==4?quad[i]:surround[i]; float x=delay_tap(&s->pre,s->early_time[i])*early_gains[i]; early[out]+=x; if(channels==6)early[out]+=x; }
        for (int c=0;c<channels;c++) { dry[c]=input[c][k]; sum+=dry[c]; }
        s->pre_previous=sum*s->pre_target+s->pre_previous*s->pre_gain; delay_update(&s->pre,s->pre_previous);
        for (int i=0;i<4;i++) { float x=s->fdn[i].data[s->fdn[i].read]; fb[i]=x*s->direct[i]+s->history[i]; s->history[i]=x*s->current[i]+fb[i]*s->output[i]; }
        const float feedback[4]={fb[2]+fb[1],-fb[0]-fb[3],fb[0]-fb[3],fb[1]-fb[2]};
        for (int i=0;i<4;i++) { v[i]=allpass(&s->allpass2[i],allpass(&s->allpass1[i],tap*s->late_gain+feedback[i],s->diffusion),-.9f*s->diffusion); delay_update(&s->fdn[i],v[i]); }
        for (int c=0;c<channels;c++) {
            if(channels==6 && c==4) output[c][k]+=early[c]*s->early_gain+delay_update(&s->center,(v[2]-v[3])*.5f)+dry[c]*s->p.dry_gain;
            else output[c][k]=early[c]*s->early_gain+v[c==5?3:c]+dry[c]*s->p.dry_gain;
        }
        if(channels==1)output[0][k]+=v[1];
    }
}
