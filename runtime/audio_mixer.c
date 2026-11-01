// Adapted from MacNativeGames games/totk/runtime/audio_mixer.c @ b75be8c (MIT, Copyright (c) 2026 MacNativeGames
// contributors): the mixing stage (voice decode and resampling, splitters, mixes, effects, sinks) and the IAudioDevice
// service. The CoreAudio output is replaced by SDL3 audio. Effect algorithms follow
// Ryujinx.Audio (MIT, Copyright (c) Ryujinx Team and Contributors).
//
// Per 5 ms frame the render thread (audren.c, renderer lock held) decodes every started voice (PCM8/16/24/32, float,
// DSP-ADPCM) along the sample path the voice clock consumes, resamples it linearly, runs the voice biquads, scales it
// into its destination mix, folds the submixes, runs the mix effects and writes the device sink's stereo mix into a
// single-producer single-consumer ring. The SDL audio thread drains that ring without taking any lock: an empty ring
// plays silence and never blocks the render thread or the guest. TOTK_AUDIO=none (or TOTK_AUDIO_OFF) mixes without a
// device; TOTK_AUDIO_PREFILL_MS (0..500, default 20) is the queued audio the output waits for after start or underrun.
// The device's clock paces the render thread, as the audio hardware paces the Switch's DSP (audio_mixer_pace_ns): a
// frame is rendered when the ring holds less than the output target, max(prefill, two device periods), so the queue and
// the delay from the game to the speakers stay there and the host and device clocks cannot drift apart. LASRecomp and
// SavageWindRecomp pace their renderers by the SDL stream the same way. Without a device, while the renderer is
// stopped and while the device takes nothing (a stall), the render thread keeps real time on its own timer; what piled
// up meanwhile is skipped on the SDL thread once the device resumes (more than QUEUE_SLACK_MS above the target plus the
// device's request), as MacNativeGames' audio_mixer.c trims its ring after a device stall.
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cfg.h"
#include "audio_effects.h"
#include "audio_reverb.h"
#include "audren.h"
#include "host.h"
#include "ipc.h"

#define MAX_FRAME 1024
#define RING_FRAMES 16384            // stereo frames, about 340 ms at 48 kHz
#define QUEUE_SLACK_MS 40            // queued audio above the target plus the device's request that ring_pull skips
#define STALL_MS 50                  // the device took nothing this long (at least 4 periods): the timer paces
#define SPLITTER_UNUSED 0xffffffffu
#define dm_guest_write_prepare(p, n) ((void)(p), (void)(n))   // guest memory is plain host memory on Windows

typedef struct { float vol[24]; int32_t dest; uint8_t used; AudrenBiquadFloat biquads[2]; float state[2][2]; } SplitDest;
typedef struct { int32_t ndest; int32_t *ids; } SplitInfo;
typedef struct {
    Reverb3d *reverb; Reverb3dParam param; int pending;
    float *delay, detector[6], compression[6], peak[6], minimum[6]; int position[6], delay_max, channels;
    AudioEffectDsp *dsp; unsigned type; uint8_t wire[160];
    float biquad_z[6][2], numerator[3], denominator[2]; uint8_t biquad_channels;
} EffectState;
typedef struct Mixer {
    float *buf; int nbuf, frame;
    int *offset;                                   // first mix buffer of each mix id
    SplitInfo *split; int nsplit; SplitDest *dest; int ndest;
    SDL_AudioStream *stream; int device_ok, rate;
    float ring[RING_FRAMES * 2];
    _Atomic uint64_t wr, rd, underruns, overruns, missing_frames, callback_frames, callback_nonzero, skipped_frames;
    _Atomic int playing; int ready;                // ready: the callback's own state
    int target, period;                            // output target and device period, frames (output_open)
    uint64_t stall_ns;
    _Atomic uint64_t pulled_ns;                    // kern_now_ns of the device's last request, 0 before the first
    _Atomic uint64_t flush_until; _Atomic unsigned stop_epoch; unsigned callback_epoch;
    EffectState *effect; int *effect_order; unsigned effect_reported;
    uint64_t *sink_address; uint32_t *sink_size, *sink_offset, *sink_old, *sink_written;
    unsigned reported;                             // one-time reports
    float voice_scratch[6][MAX_FRAME], filter_scratch[MAX_FRAME], output_scratch[MAX_FRAME * 2];
    double level_sum, level_peak; uint64_t level_count, level_frames; int first_sound_logged;
    int prefill;                                   // render blocks
} Mixer;

static _Atomic float device_volume = 1.0f;

// TOTK_VOLUME: the player's volume in percent (0..100) on top of the game's own; a live setting (the
// menu), parsed again when cfg_version changes. Render thread only.
static float host_volume(void) {
    static float v = 1.0f;
    static unsigned seen = ~0u;
    unsigned ver = cfg_version();
    if (ver == seen) return v;
    seen = ver;
    double pct = cfg_double("TOTK_VOLUME");
    v = (float)((pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100);
    return v;
}
static unsigned audio_revision(const AudioRenderer *r) { unsigned byte = r->revision >> 24; return byte >= '0' ? byte - '0' : 0; }
static void report_once(Mixer *m, unsigned bit, const char *msg, int value) {
    if (m->reported & (1u << bit)) return;
    m->reported |= 1u << bit;
    rt_log("audio: %s (%d)", msg, value);
}

// ---- output: SDL3 audio stream fed from the ring
// How much audio the output waits for before it starts, and again after an underrun: TOTK_AUDIO_PREFILL_MS rounded up
// to whole 5 ms render blocks. Returns -1 for a value outside 0..500 ms.
static int prefill_blocks_for(const char *e) {
    if (!e || !*e) return 4;
    char *end;
    double ms = strtod(e, &end);
    if (end == e || *end || !(ms >= 0) || ms > 500) return -1;
    return (int)ceil(ms / 5.0);
}

// Fills `frames` stereo frames from the ring (zeros for what is missing). Lock-free; runs on SDL's audio thread.
static void ring_pull(Mixer *m, float *out, uint32_t frames) {
    atomic_fetch_add(&m->callback_frames, frames);
    unsigned epoch = atomic_load_explicit(&m->stop_epoch, memory_order_acquire);
    uint64_t rd = atomic_load(&m->rd), wr = atomic_load_explicit(&m->wr, memory_order_acquire), have = wr - rd;
    if (epoch != m->callback_epoch) {   // the renderer stopped: drop what was queued before the stop
        uint64_t until = atomic_load_explicit(&m->flush_until, memory_order_acquire);
        if (until > rd) {
            rd = until < wr ? until : wr;
            atomic_store_explicit(&m->rd, rd, memory_order_release);
        }
        have = wr - rd;
        m->callback_epoch = epoch;
        m->ready = 0;
    }
    if (!atomic_load_explicit(&m->playing, memory_order_acquire)) {
        memset(out, 0, (size_t)frames * 2 * sizeof(float));
        m->ready = 0;
        atomic_store_explicit(&m->rd, wr, memory_order_release);
        return;
    }
    uint64_t prefill = (uint64_t)m->frame * (uint64_t)m->prefill;
    if (prefill < frames) prefill = frames;
    if (prefill > RING_FRAMES) prefill = RING_FRAMES;
    if (!m->ready) {
        if (have < prefill) {
            memset(out, 0, (size_t)frames * 2 * sizeof(float));
            return;
        }
        m->ready = 1;
    }
    uint64_t keep = (prefill > (uint64_t)m->target ? prefill : (uint64_t)m->target) + frames;
    uint64_t slack = (uint64_t)m->rate * QUEUE_SLACK_MS / 1000;
    if (have > keep + slack) {   // piled up while the device stalled: skip the oldest, keep target + request
        atomic_fetch_add(&m->skipped_frames, have - keep);
        rd = wr - keep;
        have = keep;
    }
    uint32_t n = frames < have ? frames : (uint32_t)have;
    uint64_t nonzero = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t k = (rd + i) % RING_FRAMES;
        out[2 * i] = m->ring[2 * k];
        out[2 * i + 1] = m->ring[2 * k + 1];
        if (out[2 * i] != 0 || out[2 * i + 1] != 0) nonzero++;
    }
    atomic_fetch_add(&m->callback_nonzero, nonzero);
    if (n < frames) {
        memset(out + 2 * n, 0, (size_t)(frames - n) * 2 * sizeof(float));
        atomic_fetch_add(&m->underruns, 1);
        atomic_fetch_add(&m->missing_frames, frames - n);
        m->ready = 0;
    }
    atomic_store_explicit(&m->rd, rd + n, memory_order_release);
}

static void SDLCALL sdl_pull(void *user, SDL_AudioStream *stream, int additional, int total) {
    (void)total;
    Mixer *m = user;
    float tmp[2 * 1024];
    atomic_store_explicit(&m->pulled_ns, kern_now_ns(), memory_order_release);
    while (additional > 0) {
        uint32_t frames = (uint32_t)additional / (2 * sizeof(float));
        if (!frames) break;
        if (frames > 1024) frames = 1024;
        ring_pull(m, tmp, frames);
        if (!SDL_PutAudioStreamData(stream, tmp, (int)(frames * 2 * sizeof(float)))) break;
        additional -= (int)(frames * 2 * sizeof(float));
    }
}

static void output_open(Mixer *m, int rate) {
    const char *mode = cfg_get("TOTK_AUDIO");
    if ((mode && !strcmp(mode, "none")) || cfg_get("TOTK_AUDIO_OFF")) {
        rt_log("audio: TOTK_AUDIO=none: mixing without a device");
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        rt_log("audio: SDL audio init failed (%s): mixing without sound", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, rate};
    m->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, sdl_pull, m);
    if (!m->stream) {
        rt_log("audio: no output device (%s): mixing without sound", SDL_GetError());
        return;
    }
    if (!SDL_ResumeAudioStreamDevice(m->stream)) {
        rt_log("audio: starting the output failed (%s): mixing without sound", SDL_GetError());
        SDL_DestroyAudioStream(m->stream);
        m->stream = NULL;
        return;
    }
    m->device_ok = 1;
    SDL_AudioSpec dev;
    int period = 0;
    if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(m->stream), &dev, &period) || period <= 0) period = m->frame;
    m->period = period;
    m->target = m->frame * m->prefill > 2 * period ? m->frame * m->prefill : 2 * period;
    if (m->target > RING_FRAMES / 2) m->target = RING_FRAMES / 2;
    uint64_t period_ns = (uint64_t)period * 1000000000ull / (uint64_t)rate;
    m->stall_ns = 4 * period_ns > STALL_MS * 1000000ull ? 4 * period_ns : STALL_MS * 1000000ull;
    const char *name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(m->stream));
    rt_log("audio: SDL device '%s' opened, %d Hz stereo float, prefill %d ms, device period %d frames; the device paces "
           "rendering at %.1f ms queued", name ? name : "?", rate, m->prefill * 5, period, m->target * 1000.0 / rate);
}

int64_t audio_mixer_pace_ns(AudioRenderer *r, uint64_t now) {
    Mixer *m = r->mixer;
    if (!m || !m->device_ok || !atomic_load_explicit(&m->playing, memory_order_acquire)) return -1;
    uint64_t pulled = atomic_load_explicit(&m->pulled_ns, memory_order_acquire);
    if (!pulled || (now > pulled && now - pulled > m->stall_ns)) return -1;
    uint64_t have = atomic_load(&m->wr) - atomic_load_explicit(&m->rd, memory_order_acquire);
    if (have < (uint64_t)m->target) return 0;
    int64_t ns = (int64_t)((have - (uint64_t)m->target + 1) * 1000000000ull / (uint64_t)m->rate);
    return ns < 500000 ? 500000 : ns;   // until the device has taken it below the target
}

// Render thread only.
static void output_push(Mixer *m, const float *lr, int frames) {
    uint64_t wr = atomic_load(&m->wr), rd = atomic_load_explicit(&m->rd, memory_order_acquire);
    if (m->device_ok && wr + (uint64_t)frames - rd > RING_FRAMES) {   // device stalled: drop this frame
        atomic_fetch_add(&m->overruns, 1);
        return;
    }
    for (int i = 0; i < frames; i++) {
        uint64_t k = (wr + (uint64_t)i) % RING_FRAMES;
        m->ring[2 * k] = lr[2 * i];
        m->ring[2 * k + 1] = lr[2 * i + 1];
    }
    atomic_store_explicit(&m->wr, wr + (uint64_t)frames, memory_order_release);
    if (!m->device_ok) atomic_store(&m->rd, wr + (uint64_t)frames);   // no device: the ring only keeps the latest frames
}

// Level meter (TOTK_TRACE=audio): one line per 5 s of output, and the first nonzero frame.
static void level_note(AudioRenderer *r, Mixer *m, const float *lr, int frames) {
    if (!(rt_trace & RT_TRACE_AUDIO)) return;
    int sound = 0;
    for (int k = 0; k < 2 * frames; k++) {
        double x = lr[k];
        m->level_sum += x * x;
        if (fabs(x) > m->level_peak) m->level_peak = fabs(x);
        if (x != 0) sound = 1;
    }
    m->level_count += 2 * (uint64_t)frames;
    m->level_frames += (uint64_t)frames;
    if (sound && !m->first_sound_logged) {
        m->first_sound_logged = 1;
        rt_log("audio: first nonzero output after %llu frames", (unsigned long long)r->elapsed_frames);
    }
    if (m->level_frames >= (uint64_t)r->param.sample_rate * 5) {
        uint64_t wr = atomic_load(&m->wr), queued = wr - atomic_load(&m->rd);
        rt_log("audio: level rms %.4f peak %.4f over %.1f s; written %llu frames, underruns %llu, overruns %llu, "
               "missing frames %llu, callback frames %llu (nonzero %llu), queued %.1f ms, skipped %llu frames",
               sqrt(m->level_sum / (double)m->level_count), m->level_peak, m->level_frames / (double)r->param.sample_rate,
               (unsigned long long)wr, (unsigned long long)atomic_load(&m->underruns),
               (unsigned long long)atomic_load(&m->overruns), (unsigned long long)atomic_load(&m->missing_frames),
               (unsigned long long)atomic_load(&m->callback_frames), (unsigned long long)atomic_load(&m->callback_nonzero),
               queued * 1000.0 / r->param.sample_rate, (unsigned long long)atomic_load(&m->skipped_frames));
        m->level_sum = m->level_peak = 0;
        m->level_count = m->level_frames = 0;
    }
}

void audio_mixer_open(AudioRenderer *r) {
    Mixer *m = calloc(1, sizeof *m);
    if (!m) rt_fatal("audio mixer allocation");
    m->frame = r->param.sample_count;
    m->nbuf = r->param.mix_buffer_count;
    m->rate = r->param.sample_rate;
    if (m->frame <= 0 || m->frame > MAX_FRAME || m->nbuf <= 0 || m->nbuf > 512) rt_fatal("audio mixer frame / buffer count");
    m->buf = calloc((size_t)m->nbuf * (size_t)m->frame, sizeof(float));
    m->offset = calloc((size_t)r->param.submix_count + 1, sizeof(int));
    m->nsplit = r->param.splitter_count;
    m->ndest = r->param.splitter_destination_count;
    m->split = calloc((size_t)(m->nsplit ? m->nsplit : 1), sizeof *m->split);
    m->dest = calloc((size_t)(m->ndest ? m->ndest : 1), sizeof *m->dest);
    if (!m->buf || !m->offset || !m->split || !m->dest) rt_fatal("audio mixer tables");
    m->effect = calloc((size_t)(r->param.effect_count ? r->param.effect_count : 1), sizeof *m->effect);
    m->effect_order = calloc((size_t)(r->param.effect_count ? r->param.effect_count : 1), sizeof *m->effect_order);
    if (!m->effect || !m->effect_order) rt_fatal("audio effect tables");
    size_t sinks = (size_t)r->param.sink_count;
    m->sink_address = calloc(sinks, sizeof(uint64_t));
    m->sink_size = calloc(sinks, sizeof(uint32_t));
    m->sink_offset = calloc(sinks, sizeof(uint32_t));
    m->sink_old = calloc(sinks, sizeof(uint32_t));
    m->sink_written = calloc(sinks, sizeof(uint32_t));
    if (!m->sink_address || !m->sink_size || !m->sink_offset || !m->sink_old || !m->sink_written)
        rt_fatal("audio sink state allocation");
    const char *pe = cfg_get("TOTK_AUDIO_PREFILL_MS");
    m->prefill = prefill_blocks_for(pe);
    if (m->prefill < 0) {
        rt_log("audio: TOTK_AUDIO_PREFILL_MS=%s is not 0..500 ms: using 20", pe);
        m->prefill = 4;
    }
    r->mixer = m;
    atomic_store(&m->playing, r->state == 0);
    output_open(m, r->param.sample_rate);
}

void audio_mixer_set_running(AudioRenderer *r, int running) {
    Mixer *m = r->mixer;
    if (!m) return;
    if (!running) {
        atomic_store_explicit(&m->flush_until, atomic_load_explicit(&m->wr, memory_order_acquire), memory_order_release);
        atomic_fetch_add_explicit(&m->stop_epoch, 1, memory_order_release);
    }
    atomic_store_explicit(&m->playing, running, memory_order_release);
}

void audio_mixer_close(AudioRenderer *r) {
    Mixer *m = r->mixer;
    if (!m) return;
    if (m->stream) {   // destroying the stream closes the device and waits for the callback to finish
        SDL_DestroyAudioStream(m->stream);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    for (int i = 0; i < m->nsplit; i++) free(m->split[i].ids);
    for (int i = 0; i < r->param.effect_count; i++) {
        audio_reverb_free(m->effect[i].reverb);
        audio_effect_free(m->effect[i].dsp);
        free(m->effect[i].delay);
    }
    free(m->sink_address);
    free(m->sink_size);
    free(m->sink_offset);
    free(m->sink_old);
    free(m->sink_written);
    free(m->effect);
    free(m->effect_order);
    free(m->buf);
    free(m->offset);
    free(m->split);
    free(m->dest);
    free(m);
    r->mixer = NULL;
}

/* ---------- routing input: splitters and mixes ---------- */
static void splitters_update(AudioRenderer *r, const uint8_t *p, uint32_t n) {
    Mixer *m = r->mixer;
    if (!n) return;
    uint32_t hdr[3];
    if (n < 0x20) rt_fatal("audio splitter section header");
    memcpy(hdr, p, 12);
    if (hdr[0] != 0x48444e53u) rt_fatal("audio splitter header magic (SNDH)");
    uint32_t ni = hdr[1], nd = hdr[2], at = 0x20;
    for (uint32_t i = 0; i < ni; i++) {
        uint32_t h[4]; if (at + 16 > n) rt_fatal("audio splitter info overruns section");
        memcpy(h, p + at, 16); at += 16;
        if (h[0] != 0x49444e53u || h[1] >= (uint32_t)m->nsplit || h[3] > (uint32_t)m->ndest || at + 4u * h[3] > n) {
            rt_log("audio: splitter info %u at %#x: %08x %08x %08x %08x (section %u bytes, infos %u, destinations %u)", i, at - 16, h[0], h[1], h[2], h[3], n, ni, nd);
            rt_fatal("audio splitter info (SNDI)");
        }
        SplitInfo *s = &m->split[h[1]]; free(s->ids);
        s->ndest = (int32_t)h[3]; s->ids = calloc(h[3] ? h[3] : 1, sizeof(int32_t));
        if (!s->ids) rt_fatal("audio splitter destinations");
        memcpy(s->ids, p + at, 4u * h[3]); at += 4u * h[3] + 12;   /* 12 reserved bytes follow the ids (measured, TOTK REV11) */
        if (at > n) rt_fatal("audio splitter info padding overruns section");
        for (uint32_t k = 0; k < h[3]; k++) if (s->ids[k] < 0 || s->ids[k] >= m->ndest) rt_fatal("audio splitter destination id");
    }
    for (uint32_t i = 0; i < nd; i++) {
        unsigned rev=(r->revision>>24)-'0',stride=rev>=15?0xa8:rev>=12?0x90:0x70;
        uint8_t e[0xa8]={0}; if (at>n||stride>n-at) rt_fatal("audio splitter destination overruns section");
        memcpy(e, p + at, stride); at += stride;
        uint32_t magic, id; int32_t dst; memcpy(&magic, e, 4); memcpy(&id, e + 4, 4); memcpy(&dst, e + 0x68, 4);
        if (magic != 0x44444e53u || id >= (uint32_t)m->ndest) rt_fatal("audio splitter destination (SNDD)");
        SplitDest *d = &m->dest[id]; memcpy(d->vol, e + 8, sizeof d->vol); d->dest = dst;
        d->used=e[rev>=15?0x9c:rev>=12?0x84:0x6c];
        for(unsigned f=0;f<2;f++){
            AudrenBiquadFloat b={0};
            if(rev>=15)memcpy(&b,e+0x6c+f*sizeof b,sizeof b);
            else if(rev>=12){AudrenBiquad q;memcpy(&q,e+0x6c+f*sizeof q,sizeof q);b.enable=q.enable;for(unsigned c=0;c<3;c++)b.numerator[c]=q.numerator[c]/16384.0f;for(unsigned c=0;c<2;c++)b.denominator[c]=q.denominator[c]/16384.0f;}
            if(b.enable){for(unsigned c=0;c<3;c++)if(!isfinite(b.numerator[c]))rt_fatal("audio splitter nonfinite numerator");for(unsigned c=0;c<2;c++)if(!isfinite(b.denominator[c]))rt_fatal("audio splitter nonfinite denominator");}
            if(!b.enable||!d->biquads[f].enable||!d->used)memset(d->state[f],0,sizeof d->state[f]);
            d->biquads[f]=b;
        }
    }
    if (at <= n && n - at < 16) at = n;   /* the section is padded to 16 bytes; the padding is not cleared by the game (measured) */
    if (at != n) { rt_log("audio: splitter section %u bytes, parsed %u (infos %u, destinations %u)", n, at, ni, nd); rt_fatal("audio splitter section size"); }
}
void audio_mixer_update_routing(AudioRenderer *r, const uint8_t *splitters, uint32_t splitter_bytes, const uint8_t *mixes, uint32_t mix_bytes, int mix_count) {
    splitters_update(r, splitters, splitter_bytes);
    if (mix_bytes == (uint32_t)mix_count * sizeof(AudrenMixIn)) { memcpy(r->mixes, mixes, mix_bytes); return; }
    /* MixInParameterDirtyOnly: 0x20-byte header {u32 magic 0xCAFEBABE, u32 count, pad} followed by the dirty mixes */
    if (mix_bytes >= 0x20 && (mix_bytes - 0x20) % sizeof(AudrenMixIn) == 0) {
        uint32_t magic, n; memcpy(&magic, mixes, 4); memcpy(&n, mixes + 4, 4);
        if (magic != 0xcafebabeu || n * sizeof(AudrenMixIn) != mix_bytes - 0x20) rt_fatal("audio dirty mix header");
        for (uint32_t i = 0; i < n; i++) {
            AudrenMixIn m; memcpy(&m, mixes + 0x20 + i * sizeof m, sizeof m);
            if (m.mix_id < 0 || m.mix_id >= mix_count) rt_fatal("audio mix id");
            r->mixes[m.mix_id] = m;
        }
        return;
    }
    rt_log("audio: mix section %u bytes for %d mixes", mix_bytes, mix_count);
    rt_fatal("audio mix section size");
}

/* Effect parameters arrive once per IPC update, while the DSP may process multiple frames. */
void audio_mixer_update_effects(AudioRenderer *r) {
    Mixer *m = r->mixer;
    for (int i=0;i<r->param.effect_count;i++) {
        const AudrenEffectIn *e=&r->effects[i]; EffectState *s=&m->effect[i];
        unsigned revision=audio_revision(r);
        int reset=e->is_new || s->type!=e->type;
        if(reset){audio_effect_free(s->dsp);s->dsp=NULL;memset(s->biquad_z,0,sizeof s->biquad_z);}
        if(e->type==3 || e->type==4 || e->type==9) {
            unsigned status,channels;int dimensions_changed=0;
            if(e->type==3){AudrenDelayParam p,old;memcpy(&p,e->specific,sizeof p);memcpy(&old,s->wire,sizeof old);status=p.status;channels=p.channels;dimensions_changed=p.channels!=old.channels || p.sample_rate!=old.sample_rate || p.delay_max!=old.delay_max;}
            else if(e->type==4){AudrenReverbParam p,old;memcpy(&p,e->specific,sizeof p);memcpy(&old,s->wire,sizeof old);status=p.status;channels=p.channels;dimensions_changed=p.channels!=old.channels || p.sample_rate!=old.sample_rate;}
            else {AudrenCompressorParam p,old;memcpy(&p,e->specific,sizeof p);memcpy(&old,s->wire,sizeof old);status=p.status;channels=p.channels;dimensions_changed=p.channels!=old.channels;}
            if(status==0 || dimensions_changed){audio_effect_free(s->dsp);s->dsp=NULL;}
            if(e->enabled && channels){if(!s->dsp)s->dsp=audio_effect_create(e->type,e->specific,revision);else audio_effect_update(s->dsp,e->specific);}
        } else if(s->dsp){audio_effect_free(s->dsp);s->dsp=NULL;}
        if(e->type==6){
            unsigned status;
            if(revision>=15){AudrenBiquadEffectV2 p;memcpy(&p,e->specific,sizeof p);s->biquad_channels=p.channels;status=p.status;memcpy(s->numerator,p.numerator,sizeof p.numerator);memcpy(s->denominator,p.denominator,sizeof p.denominator);}
            else {AudrenBiquadEffectV1 p;memcpy(&p,e->specific,sizeof p);s->biquad_channels=p.channels;status=p.status;for(int k=0;k<3;k++)s->numerator[k]=p.numerator[k]/16384.f;for(int k=0;k<2;k++)s->denominator[k]=p.denominator[k]/16384.f;}
            if(status==0 || (status==1 && revision<6))memset(s->biquad_z,0,sizeof s->biquad_z);
            if(e->enabled && (s->biquad_channels<1 || s->biquad_channels>6))rt_fatal("biquad effect channel count");
        }
        s->type=e->type;memcpy(s->wire,e->specific,sizeof s->wire);
        if(e->type!=5 || e->is_new) { audio_reverb_free(s->reverb); s->reverb=NULL; s->pending=1; }
        if(e->type==7) {
            AudrenLimiterParam p; memcpy(&p,e->specific,sizeof p);
            if(e->is_new || !s->delay || p.status==0 || p.channels!=s->channels || p.delay_max!=s->delay_max) {
                free(s->delay);s->delay=NULL;s->delay_max=p.delay_max;s->channels=p.channels;
                memset(s->detector,0,sizeof s->detector);memset(s->position,0,sizeof s->position);
                for(int c=0;c<6;c++)s->compression[c]=s->minimum[c]=1;
                memset(s->peak,0,sizeof s->peak);
                if(e->enabled) {
                    if(p.channels<1 || p.channels>6 || p.delay_max<1 || p.delay_max>192000)rt_fatal("limiter delay dimensions");
                    s->delay=calloc((size_t)p.channels*p.delay_max,sizeof(float));if(!s->delay)rt_fatal("limiter delay allocation");
                }
            }
            if(p.statistics_reset) { memset(s->peak,0,sizeof s->peak); for(int c=0;c<6;c++)s->minimum[c]=1; }
        } else if(s->delay) { free(s->delay);s->delay=NULL; }
        if(e->type!=5) continue;
        Reverb3dParam p; memcpy(&p,e->specific,sizeof p);
        if(s->reverb && p.sample_rate!=s->param.sample_rate) { audio_reverb_free(s->reverb); s->reverb=NULL; }
        if(memcmp(&p,&s->param,72) || p.status<2) s->pending=1;
        s->param=p;
    }
}

/* ---------- voice decoding along the voice clock's sample path ---------- */
typedef struct { ServerVoice *v; unsigned index; uint64_t offset; int32_t loops; uint8_t valid[4]; int ended; int16_t h1, h2; int32_t abuf; uint64_t aat; } Walk;
static int fmt_bytes(int f) { return f == FMT_PCM8 ? 1 : f == FMT_PCM16 ? 2 : f==FMT_PCM24?3:f == FMT_PCM32 || f == FMT_PCMF32 ? 4 : 0; }
/* DSP-ADPCM: 8-byte frames of a predictor/scale byte and 14 4-bit samples; the history runs across samples. */
static int16_t adpcm_next(Walk *w, const AudrenWaveBuffer *b, const int16_t *coef, uint64_t i) {
    const uint8_t *d = (const uint8_t *)(uintptr_t)b->address;
    uint64_t frame = i / 14, nib = i % 14;
    if ((frame + 1) * 8 > b->size) return 0;
    uint8_t ps = d[frame * 8]; int scale = 1 << (ps & 15), pred = (ps >> 4) & 7;
    int n = (d[frame * 8 + 1 + nib / 2] >> ((nib & 1) ? 0 : 4)) & 15; if (n >= 8) n -= 16;
    int64_t s = (int64_t)n * scale * 2048 + 1024 + (int64_t)coef[pred * 2] * w->h1 + (int64_t)coef[pred * 2 + 1] * w->h2;
    s >>= 11; if (s > 32767) s = 32767; if (s < -32768) s = -32768;
    w->h2 = w->h1; w->h1 = (int16_t)s; return (int16_t)s;
}
static float sample_at(Walk *w, int ch) {
    if (w->ended || !w->valid[w->index]) return 0;
    ServerVoice *v = w->v; const AudrenWaveBuffer *b = &v->buffers[w->index];
    uint64_t i = (uint64_t)b->start + w->offset; int f = v->in.sample_format, cc = (int)v->in.channel_count;
    if (f == FMT_ADPCM) {
        if (cc != 1 || !v->in.extra_address || v->in.extra_size < 32) return 0;
        const int16_t *coef = (const int16_t *)(uintptr_t)v->in.extra_address;
        if (w->abuf != (int32_t)w->index || w->aat > i + 1) {   /* (re)start this buffer: from its context, else from the frame boundary with carried history */
            w->abuf = (int32_t)w->index;
            if (b->context_address && b->context_size >= 6) { const int16_t *c = (const int16_t *)(uintptr_t)b->context_address; w->h1 = c[1]; w->h2 = c[2]; w->aat = (uint64_t)b->start; }
            else w->aat = (uint64_t)b->start - (uint64_t)b->start % 14;
            if (i < w->aat) w->aat = i;
        }
        while (w->aat <= i) adpcm_next(w, b, coef, w->aat++);
        return w->h1 / 32768.0f;
    }
    int bytes = fmt_bytes(f); if (!bytes || ch >= cc) return 0;
    uint64_t at = (i * (uint64_t)cc + (uint64_t)ch) * (uint64_t)bytes;
    if (at + (uint64_t)bytes > b->size) return 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)b->address + at;
    switch (f) {
    case FMT_PCM8: return (int8_t)p[0] / 128.0f;
    case FMT_PCM16: { int16_t s; memcpy(&s, p, 2); return s / 32768.0f; }
    case FMT_PCM24: {uint32_t word=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16;return ((int32_t)(word<<8)>>8)/8388608.f;}
    case FMT_PCM32: { int32_t s; memcpy(&s, p, 4); return (float)(s / 2147483648.0); }
    default: { float s; memcpy(&s, p, 4); return s; }
    }
}
static void walk_skip_empty(Walk *w) {   /* the same as skip_empty in audio_renderer.c: no samples, played on arrival */
    while (!w->ended && w->valid[w->index] && audio_buffer_empty(&w->v->buffers[w->index])) {
        const AudrenWaveBuffer *b = &w->v->buffers[w->index];
        if (b->loop && b->loop_count < 0) break;
        w->valid[w->index] = 0; w->offset = 0; w->loops = 0; w->index = (w->index + 1) & 3;
        if (b->end_of_stream) w->ended = 1;
    }
}
static void walk_step(Walk *w) {   /* the same transitions as advance_voice in audio_renderer.c */
    if (w->ended || !w->valid[w->index]) { w->ended = 1; return; }
    const AudrenWaveBuffer *b = &w->v->buffers[w->index];
    uint64_t length = (uint64_t)(b->end - b->start);
    if (++w->offset < length) return;
    if (b->loop && (b->loop_count < 0 || w->loops < b->loop_count)) { w->loops++; w->offset = b->loop_start > b->start && b->loop_start < b->end ? (uint64_t)(b->loop_start - b->start) : 0; return; }
    w->valid[w->index] = 0; w->offset = 0; w->loops = 0; w->index = (w->index + 1) & 3;
    if (b->end_of_stream) w->ended = 1;
    walk_skip_empty(w);
}
/* Decodes `frames` output samples per channel into out[c][k]; the renderer advances the voice afterwards by the same amount. */
static void voice_render(AudioRenderer *r, ServerVoice *v, float out[6][MAX_FRAME], int frames) {
    const double step = v->in.sample_rate * (v->in.pitch > 0 ? v->in.pitch : 1.0) / r->param.sample_rate;
    Walk w = {v, v->index, v->offset, v->loops_done, {0}, 0, v->adpcm.history[0], v->adpcm.history[1], v->adpcm.buffer, v->adpcm.at};
    memcpy(w.valid, v->valid, sizeof w.valid);
    walk_skip_empty(&w);
    int cc = (int)v->in.channel_count; double frac = v->fraction;
    for (int k = 0; k < frames; k++) {
        Walk next = w; walk_step(&next);
        for (int c = 0; c < cc; c++) { float s0 = sample_at(&w, c), s1 = sample_at(&next, c); out[c][k] = (float)(s0 + (s1 - s0) * frac); }
        frac += step;
        while (frac >= 1.0) { frac -= 1.0; walk_step(&w); }
    }
    v->adpcm.history[0] = w.h1; v->adpcm.history[1] = w.h2; v->adpcm.buffer = w.abuf; v->adpcm.at = w.aat;
}

/* ---------- mixing ---------- */
static float *mixbuf(Mixer *m, int i) { return m->buf + (size_t)i * (size_t)m->frame; }
static void accumulate(Mixer *m, int buffer, const float *src, float vol, int frames) {
    if (vol == 0 || buffer < 0 || buffer >= m->nbuf) return;
    float *d = mixbuf(m, buffer); for (int k = 0; k < frames; k++) d[k] += src[k] * vol;
}
static int mix_valid(AudioRenderer *r, int32_t id) { return id >= 0 && id <= r->param.submix_count && r->mixes[id].in_use; }
static int mix_depth(AudioRenderer *r, int id) {
    int d = 0;
    for (int guard = 0; id > 0 && guard < 64; guard++, d++) {
        if (!mix_valid(r, r->mixes[id].dest_mix_id)) return d + 1;
        id = r->mixes[id].dest_mix_id;
    }
    return d;
}

/* Guest custom effects exchange int32 samples in two rings. Each ring has CPU and DSP
   0x40-byte headers followed by samples; only the DSP header's owned cursor is advanced. */
static void mix_aux(AudioRenderer *r, int mix, const AudrenEffectIn *e) {
    Mixer *m=r->mixer; AudrenAuxParam p; memcpy(&p,e->specific,sizeof p);
    if(!p.send_info || !p.return_info || !p.capacity || !p.channels) return;
    if(p.channels>24 || p.capacity>16*1024*1024)rt_fatal("auxiliary effect dimensions");
    uint8_t *send=ipc_guest(p.send_info,128+(uint64_t)p.capacity*4), *ret=ipc_guest(p.return_info,128+(uint64_t)p.capacity*4);
    uint32_t *sh=(uint32_t *)(send+64),*rh=(uint32_t *)(ret+64); int32_t *sd=(int32_t *)(send+128),*rd=(int32_t *)(ret+128);
    uint32_t wr=sh[1],read=rh[0],total=p.channels*(uint32_t)m->frame;
    for(unsigned c=0;c<p.channels;c++) {
        if(p.input[c]>=r->mixes[mix].buffer_count || p.output[c]>=r->mixes[mix].buffer_count)rt_fatal("auxiliary effect mix buffer");
        float *input=mixbuf(m,m->offset[mix]+p.input[c]),*output=mixbuf(m,m->offset[mix]+p.output[c]);
        if(!e->enabled) { if(input!=output)memmove(output,input,m->frame*sizeof(float)); continue; }
        uint64_t w=(uint64_t)wr+c*m->frame,at=(uint64_t)read+c*m->frame;
        for(int k=0;k<m->frame;k++) {
            double scaled=trunc((double)input[k]*32768); int32_t value=scaled>INT32_MAX?INT32_MAX:scaled<INT32_MIN?INT32_MIN:!isfinite(scaled)?0:(int32_t)scaled;
            input[k]=value/32768.f;
            if(w<=p.capacity)sd[(w+(unsigned)k)%p.capacity]=value;
        }
        for(int k=0;k<m->frame;k++)output[k]=at<=p.capacity?rd[(at+(unsigned)k)%p.capacity]/32768.f:0;
        if(c==p.channels-1) { if(w<=p.capacity)sh[1]=(wr+total)%p.capacity; if(at<=p.capacity)rh[0]=(read+total)%p.capacity; }
    }
    if(!e->enabled) { sh[0]=sh[1]=sh[3]=0; rh[0]=rh[1]=rh[3]=0; }
}

void audio_mixer_effect_result(AudioRenderer *r, int id, void *output, unsigned size) {
    Mixer *m=r->mixer;
    if(size>=48 && r->effects[id].type==7) { memcpy(output,m->effect[id].peak,24);memcpy((uint8_t *)output+24,m->effect[id].minimum,24); }
    if(r->effects[id].type==9)audio_effect_statistics(m->effect[id].dsp,output,size);
}
static void mix_limiter(AudioRenderer *r, int mix, int id) {
    Mixer *m=r->mixer; EffectState *s=&m->effect[id]; const AudrenEffectIn *e=&r->effects[id];
    AudrenLimiterParam p;memcpy(&p,e->specific,sizeof p);
    if(p.channels<1 || p.channels>6) { if(e->enabled)rt_fatal("limiter channel count");return; }
    if(e->enabled && (!s->delay || p.delay_min<1 || p.delay_min>p.delay_max || p.attack<0 || p.attack>1 || p.release<0 || p.release>1))rt_fatal("limiter parameters");
    for(int c=0;c<p.channels;c++) {
        if(p.input[c]>=r->mixes[mix].buffer_count || p.output[c]>=r->mixes[mix].buffer_count)rt_fatal("limiter mix buffer");
        float *input=mixbuf(m,m->offset[mix]+p.input[c]),*output=mixbuf(m,m->offset[mix]+p.output[c]);
        if(!e->enabled) { if(input!=output)memmove(output,input,m->frame*sizeof(float));continue; }
        for(int k=0;k<m->frame;k++) {
            float x=(input[k]*(32768.f/32767.f))*p.input_gain,level=fabsf(x);
            float coeff=level>s->detector[c]?p.attack:p.release;s->detector[c]+=coeff*(level-s->detector[c]);
            float attenuation=s->detector[c]>p.threshold?p.threshold/s->detector[c]:1;
            coeff=s->compression[c]>attenuation?p.attack:p.release;s->compression[c]+=coeff*(attenuation-s->compression[c]);
            int slot=c*p.delay_max+s->position[c];output[k]=s->delay[slot]*s->compression[c]*p.output_gain*(32767.f/32768.f);
            s->delay[slot]=x;s->position[c]=(s->position[c]+1)%p.delay_min;
            if(p.statistics_enabled) { s->peak[c]=fmaxf(s->peak[c],level);s->minimum[c]=fminf(s->minimum[c],s->compression[c]); }
        }
    }
}

static int32_t effect_integer(float value) {
    double x=trunc((double)value*32768);return !isfinite(x)?0:x>INT32_MAX?INT32_MAX:x<INT32_MIN?INT32_MIN:(int32_t)x;
}
static void prepare_ring_write(uint8_t *base,uint32_t size,uint32_t at,uint64_t bytes) {
    if(bytes>=size){dm_guest_write_prepare(base,size);return;}
    uint32_t first=(uint32_t)bytes;if(first>size-at)first=size-at;
    dm_guest_write_prepare(base+at,first);if(bytes>first)dm_guest_write_prepare(base,(size_t)bytes-first);
}
/* SDK SampleCounter::Add advances the producer counter itself, not the
   producer-consumer difference or the ring cursor. Saturate on diff overflow. */
static void effect_counter_add(uint32_t cpu,uint32_t *dsp,uint32_t count) {
    uint32_t previous=*dsp-cpu,next=*dsp+count;
    *dsp=next-cpu<previous?cpu-1:next;
}
static void mix_capture(AudioRenderer *r,int mix,const AudrenEffectIn *e) {
    Mixer *m=r->mixer;AudrenAuxParam p;memcpy(&p,e->specific,sizeof p);
    if(!p.send_info || !p.capacity || !p.channels)return;
    if(p.channels>24 || p.capacity>16*1024*1024)rt_fatal("capture effect dimensions");
    uint8_t *base=ipc_guest(p.send_info,128+(uint64_t)p.capacity*4);uint32_t *cpu=(uint32_t *)base,*dsp=(uint32_t *)(base+64);int32_t *data=(int32_t *)(base+128);
    dm_guest_write_prepare(base+64,64);
    if(!e->enabled){dsp[0]=dsp[1]=dsp[3]=0;return;}
    uint32_t wr=dsp[1],total=p.channels*(uint32_t)m->frame;
    for(unsigned c=0;c<p.channels;c++){
        if(p.input[c]>=r->mixes[mix].buffer_count)rt_fatal("capture effect input buffer");
        float *in=mixbuf(m,m->offset[mix]+p.input[c]);uint64_t at=(uint64_t)wr+c*m->frame;
        if(at<=p.capacity)prepare_ring_write((uint8_t *)data,p.capacity*4,(uint32_t)(at%p.capacity)*4,(uint64_t)m->frame*4);
        for(int k=0;k<m->frame;k++){int32_t value=effect_integer(in[k]);in[k]=value/32768.f;if(at<=p.capacity)data[(at+(unsigned)k)%p.capacity]=value;}
    }
    if((uint64_t)wr+(p.channels-1)*(uint32_t)m->frame<=p.capacity){
        uint32_t diff=dsp[3]-cpu[3];
        if(diff>=p.capacity)effect_counter_add(cpu[2],&dsp[2],total);
        dsp[1]=(wr+total)%p.capacity;effect_counter_add(cpu[3],&dsp[3],total);
    }
}
static void mix_simple_effect(AudioRenderer *r,int mix,int id) {
    Mixer *m=r->mixer;const AudrenEffectIn *e=&r->effects[id];EffectState *s=&m->effect[id];
    if(e->type==1){
        AudrenBufferMixParam p;memcpy(&p,e->specific,sizeof p);if(!e->enabled)return;
        if(p.count>24)rt_fatal("buffer-mix effect count");
        for(unsigned c=0;c<p.count;c++){
            if(p.input[c]>=r->mixes[mix].buffer_count || p.output[c]>=r->mixes[mix].buffer_count)rt_fatal("buffer-mix effect index");
            float *in=mixbuf(m,m->offset[mix]+p.input[c]),*out=mixbuf(m,m->offset[mix]+p.output[c]);
            for(int k=0;k<m->frame;k++)out[k]+=ceilf(in[k]*32768*p.volume[c])/32768;
        }return;
    }
    unsigned channels;if(e->type==6)channels=s->biquad_channels;else {uint16_t n;memcpy(&n,e->specific+14,2);channels=n;}
    if(channels<1 || channels>6){if(e->enabled)rt_fatal("audio DSP effect channels");return;}
    float *in[6]={0},*out[6]={0};unsigned revision=audio_revision(r);
    for(unsigned c=0;c<channels;c++){
        unsigned idx=c;
        if(channels==6 && ((e->type==3 && revision<11)||(e->type==4 && revision>=11))){static const unsigned map[6]={0,1,4,5,2,3};idx=map[c];}
        if(e->specific[idx]>=r->mixes[mix].buffer_count || e->specific[6+idx]>=r->mixes[mix].buffer_count)rt_fatal("audio DSP effect buffer index");
        in[c]=mixbuf(m,m->offset[mix]+e->specific[idx]);out[c]=mixbuf(m,m->offset[mix]+e->specific[6+idx]);
    }
    if(!e->enabled){for(unsigned c=0;c<channels;c++)if(in[c]!=out[c])memmove(out[c],in[c],(size_t)m->frame*sizeof(float));return;}
    if(e->type==6){
        for(unsigned c=0;c<channels;c++)for(int k=0;k<m->frame;k++){
            float x=in[c][k],y=s->numerator[0]*x+s->biquad_z[c][0];
            s->biquad_z[c][0]=s->numerator[1]*x+s->denominator[0]*y+s->biquad_z[c][1];s->biquad_z[c][1]=s->numerator[2]*x+s->denominator[1]*y;out[c][k]=y;
        }
    } else {if(!s->dsp)rt_fatal("audio DSP effect state missing");audio_effect_process(s->dsp,in,out,m->frame);}
}
static void mix_effects(AudioRenderer *r, int mix) {
    Mixer *m=r->mixer; int count=0;
    for(int i=0;i<r->param.effect_count;i++) if(r->effects[i].type && r->effects[i].mix_id==mix) {
        int j=count++; while(j>0 && r->effects[m->effect_order[j-1]].order>r->effects[i].order) { m->effect_order[j]=m->effect_order[j-1]; j--; } m->effect_order[j]=i;
    }
    for(int n=0;n<count;n++) {
        int id=m->effect_order[n]; const AudrenEffectIn *e=&r->effects[id]; EffectState *s=&m->effect[id];
        if(e->type==1 || e->type==3 || e->type==4 || e->type==6 || e->type==9){mix_simple_effect(r,mix,id);continue;}
        if(e->type==2) { mix_aux(r,mix,e); continue; }
        if(e->type==8) { mix_capture(r,mix,e);continue; }
        if(e->type==7) { mix_limiter(r,mix,id); continue; }
        if(e->type!=5) {
            if(e->enabled && e->type<32 && !(m->effect_reported&(1u<<e->type))) { m->effect_reported|=1u<<e->type; rt_log("audio: effect type %u not implemented",e->type); }
            continue;
        }
        Reverb3dParam *p=&s->param; float *input[6]={0},*output[6]={0};
        if(p->channels!=1 && p->channels!=2 && p->channels!=4 && p->channels!=6) { if(e->enabled)rt_fatal("audio reverb channel count"); continue; }
        for(int c=0;c<p->channels;c++) {
            int idx=c; /* REV11+ channel resources use L,R,C,LFE,BL,BR; the reference DSP uses legacy order. */
            if(p->channels==6 && (r->revision>>24)>='0'+11) { static const int map[6]={0,1,4,5,2,3}; idx=map[c]; }
            if(p->input[idx]>=r->mixes[mix].buffer_count || p->output[idx]>=r->mixes[mix].buffer_count)rt_fatal("audio reverb buffer index");
            input[c]=mixbuf(m,m->offset[mix]+p->input[idx]); output[c]=mixbuf(m,m->offset[mix]+p->output[idx]);
        }
        if(!e->enabled) { for(int c=0;c<p->channels;c++)if(input[c]!=output[c])memmove(output[c],input[c],m->frame*sizeof(float)); continue; }
        if(!s->reverb)s->reverb=audio_reverb_create(p); else if(s->pending)audio_reverb_update(s->reverb,p);
        s->pending=0; audio_reverb_process(s->reverb,input,output,m->frame);
    }
}

static const float *splitter_filter(SplitDest *d,const float *input,float *output,int frames){
    if(!d->biquads[0].enable&&!d->biquads[1].enable)return input;
    memcpy(output,input,(size_t)frames*sizeof *output);
    for(unsigned f=0;f<2;f++)if(d->biquads[f].enable){
        const AudrenBiquadFloat *b=&d->biquads[f];float *z=d->state[f];
        for(int k=0;k<frames;k++){
            float x=output[k],y=b->numerator[0]*x+z[0];
            z[0]=b->numerator[1]*x+b->denominator[0]*y+z[1];
            z[1]=b->numerator[2]*x+b->denominator[1]*y;output[k]=y;
        }
    }
    return output;
}
typedef struct {
    uint64_t address;uint32_t size,channels,samples,last_read;
    uint8_t format,reserved[3],input[6],padding[2];
} CircularSinkParam;
_Static_assert(__builtin_offsetof(CircularSinkParam,input)==28,"circular sink wire layout");
static void mix_circular_sink(AudioRenderer *r,int id) {
    Mixer *m=r->mixer;CircularSinkParam p;memcpy(&p,r->sinks[id].name,sizeof p);
    if(!p.address || !p.size)return;
    if(p.channels<1 || p.channels>6 || p.format!=FMT_PCM16 || p.size>256*1024*1024 || p.size%2)rt_fatal("circular audio sink parameters");
    if(m->sink_address[id]!=p.address || m->sink_size[id]!=p.size){m->sink_address[id]=p.address;m->sink_size[id]=p.size;m->sink_offset[id]=m->sink_old[id]=m->sink_written[id]=0;}
    uint8_t *data=ipc_guest(p.address,p.size);uint32_t at=m->sink_offset[id];
    prepare_ring_write(data,p.size,at,(uint64_t)p.channels*m->frame*2);
    float gain=r->mixes[0].in_use?r->mixes[0].volume:1;
    for(unsigned c=0;c<p.channels;c++){
        if(p.input[c]>=m->nbuf)rt_fatal("circular audio sink input index");
        float *in=mixbuf(m,p.input[c]);
        for(int k=0;k<m->frame;k++){
            float scaled=in[k]*gain*32768;int16_t value=!isfinite(scaled)?0:scaled>=32767?32767:scaled<=-32768?-32768:(int16_t)scaled;
            memcpy(data+at,&value,2);at=(at+2)%p.size;
        }
    }
    m->sink_written[id]=m->sink_old[id];m->sink_old[id]=m->sink_offset[id];m->sink_offset[id]=at;
}
uint32_t audio_mixer_sink_written(AudioRenderer *r,int id) {Mixer *m=r->mixer;return m?m->sink_written[id]:0;}

void audio_mixer_render(AudioRenderer *r, double seconds) {
    (void)seconds;
    Mixer *m = r->mixer; if (!m) return;
    const int frames = m->frame, mc = r->param.submix_count + 1;
    memset(m->buf, 0, (size_t)m->nbuf * (size_t)frames * sizeof(float));
    int at = 0;
    for (int i = 0; i < mc; i++) { m->offset[i] = at; if (r->mixes[i].in_use) at += r->mixes[i].buffer_count; }
    if (at > m->nbuf) { rt_log("audio: mixes use %d buffers, renderer has %d", at, m->nbuf); rt_fatal("audio mix buffer layout"); }

    float (*vs)[MAX_FRAME]=m->voice_scratch,*filtered=m->filter_scratch;
    for (int i = 0; i < r->param.voice_count; i++) {
        ServerVoice *v = &r->voices[i];
        if (!v->in.in_use || v->state != SV_STARTED || !v->in.sample_rate || !v->valid[v->index]) continue;
        int cc = (int)v->in.channel_count; if (cc < 1 || cc > 6) { report_once(m, 2, "voice channel count unsupported", cc); continue; }
        voice_render(r, v, vs, frames);
        for (int f = 0; f < 2; f++) if (v->in.biquads[f].enable) {
            const AudrenBiquad *b = &v->in.biquads[f];
            const AudrenBiquadFloat *bf=&v->float_biquads[f];
            const float a0 = v->use_float_biquads?bf->numerator[0]:b->numerator[0]/16384.0f;
            const float a1 = v->use_float_biquads?bf->numerator[1]:b->numerator[1]/16384.0f;
            const float a2 = v->use_float_biquads?bf->numerator[2]:b->numerator[2]/16384.0f;
            const float b1 = v->use_float_biquads?bf->denominator[0]:b->denominator[0]/16384.0f;
            const float b2 = v->use_float_biquads?bf->denominator[1]:b->denominator[1]/16384.0f;
            for (int c = 0; c < cc; c++) {
                float *z = v->biquad_state[f][c];
                for (int k = 0; k < frames; k++) {
                    float x = vs[c][k], y = a0 * x + z[0];
                    z[0] = a1 * x + b1 * y + z[1];
                    z[1] = a2 * x + b2 * y;
                    vs[c][k] = y;
                }
            }
        }
        if (v->in.splitter_id != SPLITTER_UNUSED && v->in.splitter_id < (uint32_t)m->nsplit) {
            const SplitInfo *s = &m->split[v->in.splitter_id];
            for (int c = 0; c < cc; c++) for (int k = c; k < s->ndest; k += cc) {
                SplitDest *d = &m->dest[s->ids[k]];
                if (!d->used || !mix_valid(r, d->dest)) continue;
                const float *source=splitter_filter(d,vs[c],filtered,frames);
                for (int b = 0; b < r->mixes[d->dest].buffer_count && b < 24; b++) accumulate(m, m->offset[d->dest] + b, source, d->vol[b] * v->in.volume, frames);
            }
        } else if (mix_valid(r, v->in.mix_id)) {
            for (int c = 0; c < cc; c++) {
                int32_t id = v->in.channel_ids[c]; if (id < 0 || id >= r->param.voice_count * 6) continue;
                const AudrenChannelIn *ch = &r->channels[id];
                for (int b = 0; b < r->mixes[v->in.mix_id].buffer_count && b < 24; b++) accumulate(m, m->offset[v->in.mix_id] + b, vs[c], ch->mix[b] * v->in.volume, frames);
            }
        }
    }
    /* submixes: deepest first, so every mix is complete before it is folded into its destination */
    int order[256], n = 0, depth[256];
    for (int i = 1; i < mc && n < 256; i++) if (r->mixes[i].in_use) { depth[n] = mix_depth(r, i); order[n++] = i; }
    for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++) if (depth[b] > depth[a]) { int t = order[a]; order[a] = order[b]; order[b] = t; t = depth[a]; depth[a] = depth[b]; depth[b] = t; }
    for (int a = 0; a < n; a++) {
        mix_effects(r, order[a]);
        const AudrenMixIn *x = &r->mixes[order[a]]; int src = m->offset[order[a]];
        if (x->dest_splitter_id != SPLITTER_UNUSED && x->dest_splitter_id < (uint32_t)m->nsplit) {
            const SplitInfo *s = &m->split[x->dest_splitter_id];
            for (int c = 0; c < x->buffer_count && c < 24; c++) for (int k = c; k < s->ndest; k += x->buffer_count) {
                SplitDest *d = &m->dest[s->ids[k]];
                if (!d->used || !mix_valid(r, d->dest)) continue;
                const float *source=splitter_filter(d,mixbuf(m,src+c),filtered,frames);
                for (int b = 0; b < r->mixes[d->dest].buffer_count && b < 24; b++) accumulate(m, m->offset[d->dest] + b, source, d->vol[b] * x->volume, frames);
            }
        } else if (mix_valid(r, x->dest_mix_id)) {
            for (int c = 0; c < x->buffer_count && c < 24; c++)
                for (int b = 0; b < r->mixes[x->dest_mix_id].buffer_count && b < 24; b++) accumulate(m, m->offset[x->dest_mix_id] + b, mixbuf(m, src + c), x->mix[c][b] * x->volume, frames);
        }
    }
    if (r->mixes[0].in_use) mix_effects(r, 0);
    /* device sink */
    const AudrenSinkIn *sink = NULL;int circular=0;
    for (int i = 0; i < r->param.sink_count; i++) if (r->sinks[i].in_use) {
        if(r->sinks[i].type!=SINK_CIRCULAR){m->sink_address[i]=0;m->sink_size[i]=m->sink_offset[i]=m->sink_old[i]=m->sink_written[i]=0;}
        if (r->sinks[i].type == SINK_DEVICE && !sink) sink = &r->sinks[i];
        else if (r->sinks[i].type == SINK_CIRCULAR){mix_circular_sink(r,i);circular=1;}
    } else {m->sink_address[i]=0;m->sink_size[i]=m->sink_offset[i]=m->sink_old[i]=m->sink_written[i]=0;}
    float *lr=m->output_scratch;
    memset(lr, 0, sizeof(float) * 2 * (size_t)frames);
    if (sink && sink->input_count >= 1 && sink->input_count <= 6) {
        const float gain = host_volume() * atomic_load(&device_volume) * (r->mixes[0].in_use ? r->mixes[0].volume : 1.0f);
        const float *ch[6] = {0};
        for (uint32_t c = 0; c < sink->input_count; c++) if (sink->inputs[c] >= 0 && sink->inputs[c] < m->nbuf) ch[c] = mixbuf(m, sink->inputs[c]);
        for (int k = 0; k < frames; k++) {
            float l, rr;
            if (sink->input_count >= 6) {   /* FL FR C LFE RL RR -> stereo */
                float ff = sink->downmix_enabled ? sink->downmix[0] : 1.0f, cf = sink->downmix_enabled ? sink->downmix[1] : 0.7071f, sf = sink->downmix_enabled ? sink->downmix[2] : 0.7071f;
                float C = ch[2] ? ch[2][k] : 0;
                l = ff * (ch[0] ? ch[0][k] : 0) + cf * C + sf * (ch[4] ? ch[4][k] : 0);
                rr = ff * (ch[1] ? ch[1][k] : 0) + cf * C + sf * (ch[5] ? ch[5][k] : 0);
            } else if (sink->input_count >= 2) { l = ch[0] ? ch[0][k] : 0; rr = ch[1] ? ch[1][k] : 0; }
            else l = rr = ch[0] ? ch[0][k] : 0;
            l *= gain; rr *= gain;
            lr[2 * k] = l > 1 ? 1 : l < -1 ? -1 : l; lr[2 * k + 1] = rr > 1 ? 1 : rr < -1 ? -1 : rr;
        }
    } else if (!sink && !circular) report_once(m, 5, "no device sink in use", r->param.sink_count);
    level_note(r, m, lr, frames);
    output_push(m, lr, frames);
}

// ---- IAudioDevice: one stereo output device
static const char device_name[] = "AudioTvOutput";

static void put_name(const IpcBuffer *b, IpcResponse *rs, int pointer) {
    if (!b->size) return;
    char *dst = ipc_guest(b->addr, b->size);
    size_t n = b->size < sizeof device_name ? b->size : sizeof device_name;
    memset(dst, 0, b->size);
    memcpy(dst, device_name, n);
    if (pointer) rs->xout[rs->nxout++] = (IpcBuffer){b->addr, b->size, b->index};
}

static IpcBuffer out_buffer(const IpcRequest *rq, int *pointer) {
    *pointer = !(rq->nb && rq->b[0].size) && rq->nc && rq->c[0].size;
    return *pointer ? rq->c[0] : rq->nb ? rq->b[0] : (IpcBuffer){0};
}

void audio_device_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    static uint32_t change_event;   // device change events: never signalled, the device does not change
    int pointer;
    switch (rq->cmd) {
    case 0: case 6: case 14: {   // ListAudioDeviceName(Auto), ListAudioOutputDeviceName [13.0.0+]
        IpcBuffer b = out_buffer(rq, &pointer);
        int32_t n = b.size >= 0x100 ? 1 : 0;
        if (n) put_name(&b, rs, pointer);
        IPC_OUT(rs, n);
        return;
    }
    case 1: case 7: {   // SetAudioDeviceOutputVolume(Auto)
        float v = IPC_ARG(rq, float, 0);
        atomic_store(&device_volume, isfinite(v) ? v : 0.0f);
        return;
    }
    case 2: case 8: { float v = atomic_load(&device_volume); IPC_OUT(rs, v); return; }   // GetAudioDeviceOutputVolume(Auto)
    case 3: case 10: case 13: {   // GetActiveAudioDeviceName(Auto), GetActiveAudioOutputDeviceName [13.0.0+]
        IpcBuffer b = out_buffer(rq, &pointer);
        put_name(&b, rs, pointer);
        return;
    }
    case 5: { int32_t n = 2; IPC_OUT(rs, n); return; }   // GetActiveChannelCount
    case 4: case 11: case 12: {   // QueryAudioDeviceSystemEvent, ...InputEvent, ...OutputEvent
        uint32_t ev = __atomic_load_n(&change_event, __ATOMIC_ACQUIRE);
        if (!ev) {
            uint32_t mine = kern_event_new(), expected = 0;
            if (__atomic_compare_exchange_n(&change_event, &expected, mine, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                ev = mine;
            else
                ev = expected;   // the loser's event stays unused (no host-side close yet)
        }
        rs->copy[rs->ncopy++] = kern_event_readable(ev);
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}
