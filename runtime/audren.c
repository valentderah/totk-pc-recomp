// Adapted from MacNativeGames games/totk/runtime/audio_renderer.c @ b75be8c (MIT, Copyright (c) 2026
// MacNativeGames contributors). Its voice lifetime (play state, wave buffer release, flush) is ported from
// Ryujinx.Audio VoiceInfo / DataSourceHelper (MIT, Copyright (c) Ryujinx Team and Contributors).
// audren:u. The SDK's nn::audio client builds RequestUpdate input unchanged; this
// service validates every section against the header, keeps server-side voice state, advances it in real time (one
// frame of sample_count / sample_rate seconds per render tick, signalling the renderer's event) and writes the output
// status the client expects. The mixer behind it is audio_mixer.c.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audren.h"
#include "host.h"
#include "ipc.h"
#include "platform/platform.h"

static volatile int32_t g_live;

long audren_renderers_live(void) { return plat_cas32(&g_live, 0, 0); }

static int revision_number(uint32_t rev) { return (int)(rev >> 24) - '0'; }

static void need(int ok, const char *what) {
    if (ok) return;
    ipc_backtrace();
    rt_fatal("audren: %s", what);
}

static AudioRenderer *renderer_new(const AudrenParameter *p) {
    need(p->sample_rate == 48000 || p->sample_rate == 32000, "unsupported output rate");
    need(p->sample_count > 0 && p->sample_count <= 4800, "sample count");
    need(p->voice_count > 0 && p->voice_count <= 1024 && p->sink_count > 0 && p->sink_count <= 256 &&
             p->effect_count >= 0 && p->effect_count <= 4096 && p->submix_count >= 0 && p->submix_count < 256,
         "renderer counts");
    AudioRenderer *r = calloc(1, sizeof *r);
    need(r != NULL, "renderer allocation");
    r->param = *p;
    r->revision = p->revision;
    r->voices = calloc((size_t)p->voice_count, sizeof *r->voices);
    r->pools = calloc((size_t)p->effect_count + 4u * (size_t)p->voice_count, sizeof *r->pools);
    r->mixes = calloc((size_t)p->submix_count + 1, sizeof *r->mixes);
    r->sinks = calloc((size_t)p->sink_count, sizeof *r->sinks);
    r->effects = calloc((size_t)(p->effect_count ? p->effect_count : 1), sizeof *r->effects);
    r->channels = calloc((size_t)p->voice_count * 6, sizeof *r->channels);
    need(r->voices && r->pools && r->mixes && r->sinks && r->effects && r->channels, "renderer tables");
    plat_mutex_init(&r->lock);
    r->event = kern_event_new();
    r->state = 1;   // stopped
    return r;
}

static void renderer_free(AudioRenderer *r) {
    free(r->voices);
    free(r->pools);
    free(r->mixes);
    free(r->sinks);
    free(r->effects);
    free(r->channels);
    free(r);
}

// ---- RequestUpdate
typedef struct { const uint8_t *p, *end; } Reader;

static const uint8_t *take(Reader *in, uint32_t n, const char *what) {
    if ((size_t)(in->end - in->p) < n) {
        rt_log("audren: input section %s (%u bytes) overruns the request", what, n);
        need(0, "update input section size");
    }
    const uint8_t *p = in->p;
    in->p += n;
    return p;
}

static void voice_reset(ServerVoice *v) {
    memset(v->valid, 0, sizeof v->valid);
    memset(v->pending, 0, sizeof v->pending);
    v->state = SV_STOPPED;
    v->flush = v->flush_from = 0;
    v->index = 0;
    v->offset = 0;
    v->fraction = 0;
    v->played = 0;
    v->consumed = 0;
    v->loops_done = 0;
    memset(&v->adpcm, 0, sizeof v->adpcm);
    memset(v->biquad_state, 0, sizeof v->biquad_state);
}

static void update_voice(AudioRenderer *r, const AudrenVoiceIn *in) {
    need(in->id < (uint32_t)r->param.voice_count, "voice id out of range");
    ServerVoice *v = &r->voices[in->id];
    if (in->is_new || !in->in_use) voice_reset(v);
    v->in = *in;
    v->use_float_biquads = 0;
    if (!in->in_use) return;
    need(in->wave_buffer_index < 4, "wave buffer index");
    // The play state as the DSP keeps it (Ryujinx VoiceInfo.UpdatePlayState): Stop on a voice that is not stopped
    // becomes Stopping, which the next render frame turns into Stopped after giving back every queued buffer.
    if (in->play_state == PLAY_STARTED) v->state = SV_STARTED;
    else if (in->play_state == PLAY_PAUSED) v->state = SV_PAUSED;
    else if (v->state != SV_STOPPED) v->state = SV_STOPPING;
    for (unsigned i = 0; i < 4; i++) {   // a new buffer waits until a render frame finds the voice playing
        const AudrenWaveBuffer *w = &in->wave_buffers[i];
        if (!w->sent) {
            v->buffers[i] = *w;
            v->pending[i] = 1;
        }
    }
    v->flush += in->flush_wave_buffer_count;
    v->flush_from = in->wave_buffer_index;
    if (in->clear_voice_drop) v->drops = 0;
}

// No samples (no memory, no size or an empty range): the DSP counts the buffer as played when it reaches it, so a
// stream's empty last buffer does not hold up the buffers queued behind it.
int audio_buffer_empty(const AudrenWaveBuffer *b) { return !b->address || !b->size || b->end <= b->start; }

static void skip_empty(AudioRenderer *r, ServerVoice *v) {
    while (v->valid[v->index] && audio_buffer_empty(&v->buffers[v->index])) {
        const AudrenWaveBuffer *b = &v->buffers[v->index];
        if (b->loop && b->loop_count < 0) break;   // an endless empty loop starves the voice, as on the DSP
        v->valid[v->index] = 0;
        v->consumed++;
        v->offset = 0;
        v->loops_done = 0;
        v->index = (v->index + 1) & 3;
        r->empty_played++;
        if (b->end_of_stream) {
            v->played = 0;
            break;
        }
    }
}

// Each render frame, before mixing (Ryujinx VoiceInfo.UpdateParametersForCommandGeneration): flush, stop, start.
static void voices_prepare(AudioRenderer *r) {
    for (int k = 0; k < r->param.voice_count; k++) {
        ServerVoice *v = &r->voices[k];
        if (!v->in.in_use) continue;
        if (v->flush) {   // FlushWaveBuffers: from the client's current buffer, the queued ones count as played
            unsigned i = v->flush_from & 3;
            for (uint32_t n = v->flush; n; n--, i = (i + 1) & 3) {
                v->pending[i] = 0;
                if (v->valid[i]) {
                    v->valid[i] = 0;
                    v->consumed++;
                    v->index = (v->index + 1) & 3;
                    v->offset = 0;
                    v->loops_done = 0;
                    r->flushed++;
                }
            }
            v->flush = 0;
        }
        if (v->state == SV_STOPPING) {   // every queued buffer counts as played; unsent ones are dropped
            for (int i = 0; i < 4; i++) {
                if (v->valid[i]) {
                    v->consumed++;
                    v->index = (v->index + 1) & 3;
                    r->stop_released++;
                }
                v->valid[i] = 0;
                v->pending[i] = 0;
            }
            v->offset = 0;
            v->played = 0;
            v->fraction = 0;
            v->loops_done = 0;
            v->state = SV_STOPPED;
            continue;
        }
        if (v->state == SV_STARTED) {
            for (int i = 0; i < 4; i++)
                if (v->pending[i]) {
                    v->pending[i] = 0;
                    v->valid[i] = 1;
                    if ((unsigned)i == v->index) {
                        v->offset = 0;
                        v->loops_done = 0;
                    }
                }
            skip_empty(r, v);
        }
    }
}

static uint32_t effect_out_bytes(const AudioRenderer *r) { return revision_number(r->revision) >= 9 ? 0x90u : 0x10u; }

static void request_update(AudioRenderer *r, const uint8_t *input, uint64_t in_size, uint8_t *output,
                           uint64_t out_size, uint64_t perf_size) {
    need(in_size >= sizeof(AudrenHeader), "update input too small");
    AudrenHeader h;
    memcpy(&h, input, sizeof h);
    need(h.total <= in_size, "update total size");
    Reader in = {input + sizeof h, input + h.total};
    plat_mutex_lock(&r->lock);
    r->revision = h.revision;
    const int vc = r->param.voice_count, ec = r->param.effect_count, sc = r->param.sink_count,
              mc = r->param.submix_count + 1;
    const uint32_t npools = (uint32_t)ec + 4u * (uint32_t)vc;
    if (!r->logged) {
        r->logged = 1;
        rt_log("audren: update REV%d sizes behavior=%u pools=%u voices=%u channels=%u effects=%u mixes=%u sinks=%u "
               "perf=%u splitters=%u render=%u total=%u out=%llu perf_out=%llu",
               revision_number(h.revision), h.behavior, h.memory_pools, h.voices, h.voice_resources, h.effects,
               h.mixes, h.sinks, h.performance, h.splitters, h.render_info, h.total, (unsigned long long)out_size,
               (unsigned long long)perf_size);
    }
    need(h.behavior == 0x10, "behavior section size");
    const uint8_t *behavior = take(&in, h.behavior, "behavior");
    memcpy(&r->behavior_flags, behavior + 8, 8);
    need(h.memory_pools == npools * sizeof(AudrenPoolIn), "memory pool section size");
    const AudrenPoolIn *pools = (const AudrenPoolIn *)take(&in, h.memory_pools, "memory pools");
    need(h.voice_resources % sizeof(AudrenChannelIn) == 0 &&
             h.voice_resources / sizeof(AudrenChannelIn) <= (uint32_t)vc * 6,
         "voice channel section size");
    const AudrenChannelIn *chans = (const AudrenChannelIn *)take(&in, h.voice_resources, "voice channels");
    const int float_voices = revision_number(h.revision) >= 15;
    const size_t voice_stride = float_voices ? sizeof(AudrenVoiceInV2) : sizeof(AudrenVoiceIn);
    need(h.voices == (uint32_t)vc * voice_stride, "voice section size");
    const uint8_t *voices = take(&in, h.voices, "voices");
    need(h.effects == (uint32_t)ec * sizeof(AudrenEffectIn), "effect section size");
    const AudrenEffectIn *effects = (const AudrenEffectIn *)take(&in, h.effects, "effects");
    const uint8_t *splitters = take(&in, h.splitters, "splitters");
    const uint8_t *mixes = take(&in, h.mixes, "mixes");
    need(h.sinks == (uint32_t)sc * sizeof(AudrenSinkIn), "sink section size");
    const AudrenSinkIn *sinks = (const AudrenSinkIn *)take(&in, h.sinks, "sinks");
    take(&in, h.performance, "performance");
    take(&in, h.render_info, "render info");
    need(in.p == in.end, "unconsumed update input");

    for (uint32_t i = 0; i < h.voice_resources / sizeof(AudrenChannelIn); i++) {
        need(chans[i].id < (uint32_t)vc * 6, "channel resource id");
        r->channels[chans[i].id] = chans[i];
    }
    for (int i = 0; i < vc; i++) {
        AudrenVoiceIn v;
        AudrenVoiceInV2 v2;
        if (float_voices) {
            memcpy(&v2, voices + i * voice_stride, sizeof v2);
            memset(&v, 0, sizeof v);
            memcpy(&v, v2.prefix, sizeof v2.prefix);
            memcpy((uint8_t *)&v + 0x3c, v2.tail, sizeof v2.tail);
            for (unsigned f = 0; f < 2; f++) {
                v.biquads[f].enable = v2.biquads[f].enable;
                if (!v.biquads[f].enable) continue;
                for (unsigned c = 0; c < 3; c++) need(isfinite(v2.biquads[f].numerator[c]), "voice biquad numerator");
                for (unsigned c = 0; c < 2; c++)
                    need(isfinite(v2.biquads[f].denominator[c]), "voice biquad denominator");
            }
        } else
            memcpy(&v, voices + i * voice_stride, sizeof v);
        update_voice(r, &v);
        ServerVoice *server = &r->voices[v.id];
        server->use_float_biquads = (uint8_t)float_voices;
        if (float_voices) memcpy(server->float_biquads, v2.biquads, sizeof v2.biquads);
    }
    for (int i = 0; i < ec; i++) r->effects[i] = effects[i];
    audio_mixer_update_effects(r);
    audio_mixer_update_routing(r, splitters, h.splitters, mixes, h.mixes, mc);
    for (int i = 0; i < sc; i++) r->sinks[i] = sinks[i];

    // ---- output
    const uint32_t eo = effect_out_bytes(r);
    AudrenHeader o = {0};
    o.revision = AUDREN_REV(revision_number(h.revision) >= 1 ? revision_number(h.revision) : 1);
    o.memory_pools = npools * sizeof(AudrenPoolOut);
    o.voices = (uint32_t)vc * sizeof(AudrenVoiceOut);
    o.effects = (uint32_t)ec * eo;
    o.sinks = (uint32_t)sc * sizeof(AudrenSinkOut);
    o.performance = 0x10;
    o.behavior = AUDREN_BEHAVIOR_OUT;
    o.render_info = AUDREN_RENDER_INFO_OUT;
    o.total = (uint32_t)sizeof o + o.memory_pools + o.voices + o.effects + o.sinks + o.performance + o.behavior +
              o.render_info;
    if (o.total > out_size) {
        rt_log("audren: output needs %u bytes, the client gave %llu (effect out %u)", o.total,
               (unsigned long long)out_size, eo);
        need(0, "update output size");
    }
    memset(output, 0, out_size);
    uint8_t *w = output;
    memcpy(w, &o, sizeof o);
    w += sizeof o;
    for (uint32_t i = 0; i < npools; i++) {
        uint32_t s = pools[i].state;
        if (s == POOL_REQUEST_ATTACH) s = POOL_ATTACHED;
        else if (s == POOL_REQUEST_DETACH) s = POOL_DETACHED;
        r->pools[i] = pools[i];
        r->pools[i].state = s;
        AudrenPoolOut po = {s, {0}};
        memcpy(w, &po, sizeof po);
        w += sizeof po;
    }
    for (int i = 0; i < vc; i++) {
        AudrenVoiceOut vo = {r->voices[i].played, r->voices[i].consumed, r->voices[i].drops};
        memcpy(w, &vo, sizeof vo);
        w += sizeof vo;
    }
    for (int i = 0; i < ec; i++) {   // EffectStatus Enabled 3 / Disabled 4
        w[0] = effects[i].enabled ? 3 : 4;
        if (eo > 16) audio_mixer_effect_result(r, i, w + 16, eo - 16);
        w += eo;
    }
    for (int i = 0; i < sc; i++) {
        AudrenSinkOut so = {0};
        if (r->sinks[i].type == SINK_CIRCULAR) so.last_written_offset = audio_mixer_sink_written(r, i);
        memcpy(w, &so, sizeof so);
        w += sizeof so;
    }
    w += 0x10;                    // performance: nothing written
    w += AUDREN_BEHAVIOR_OUT;     // behavior: no errors
    memcpy(w, &r->elapsed_frames, 8);
    plat_mutex_unlock(&r->lock);
}

// ---- render tick: one frame each time the output device has taken the ring below its target (audio_mixer_pace_ns),
// or every sample_count / sample_rate seconds of the host clock without a device, while stopped or while it stalls
static void advance_voice(AudioRenderer *r, ServerVoice *v, double out_seconds) {
    if (!v->in.in_use || v->state != SV_STARTED || !v->in.sample_rate) return;
    double want = v->fraction + out_seconds * v->in.sample_rate * (v->in.pitch > 0 ? v->in.pitch : 1.0);
    uint64_t samples = (uint64_t)want;
    v->fraction = want - (double)samples;
    while (samples && v->valid[v->index]) {
        AudrenWaveBuffer *b = &v->buffers[v->index];
        uint64_t length = (uint64_t)(b->end - b->start), left = length - v->offset;
        uint64_t step = samples < left ? samples : left;
        v->offset += step;
        v->played += step;
        samples -= step;
        if (v->offset < length) break;
        if (b->loop && (b->loop_count < 0 || v->loops_done < b->loop_count)) {
            v->loops_done++;
            v->offset = b->loop_start > b->start && b->loop_start < b->end ? (uint64_t)(b->loop_start - b->start) : 0;
            continue;
        }
        v->valid[v->index] = 0;
        v->consumed++;
        v->offset = 0;
        v->loops_done = 0;
        v->index = (v->index + 1) & 3;
        if (b->end_of_stream) {
            v->played = 0;
            break;
        }
        skip_empty(r, v);
    }
}

static void render_thread(void *p) {
    AudioRenderer *r = p;
    plat_thread_priority(PLAT_PRIO_HIGHEST);
    const uint64_t frame_ns = (uint64_t)r->param.sample_count * 1000000000ull / (uint64_t)r->param.sample_rate;
    const double frame = (double)r->param.sample_count / r->param.sample_rate;
    uint64_t next = kern_now_ns();
    while (plat_cas32(&r->running, 0, 0)) {
        uint64_t now = kern_now_ns();
        int64_t pace = audio_mixer_pace_ns(r, now);
        if (pace > 0) {
            plat_wait_ns((uint64_t)pace);
            continue;
        }
        if (pace == 0) {
            next = now + frame_ns;   // where the timer goes on if the device stops pacing
        } else {
            if (next > now) plat_wait_ns(next - now);
            else if (now - next > 100000000ull) next = now;   // fell far behind (debugger, sleep): resynchronise
            next += frame_ns;
        }
        plat_mutex_lock(&r->lock);
        if (r->state == 0) {
            voices_prepare(r);
            audio_mixer_render(r, frame);
            for (int i = 0; i < r->param.voice_count; i++) advance_voice(r, &r->voices[i], frame);
            r->elapsed_frames++;
            uint64_t given = r->stop_released + r->empty_played + r->flushed;
            if ((rt_trace & RT_TRACE_IPC) && given != r->logged_released && r->elapsed_frames % 1000 == 0) {
                r->logged_released = given;
                rt_log("audren: wave buffers given back: %llu by stopped voices, %llu without samples, %llu flushed",
                       (unsigned long long)r->stop_released, (unsigned long long)r->empty_played,
                       (unsigned long long)r->flushed);
            }
        }
        plat_mutex_unlock(&r->lock);
        kern_event_signal(r->event);
    }
}

// ---- IAudioRenderer
static IpcBuffer pick(const IpcBuffer *a, unsigned na, const IpcBuffer *b, unsigned nb, unsigned i) {
    IpcBuffer x = i < na ? a[i] : (IpcBuffer){0}, y = i < nb ? b[i] : (IpcBuffer){0};
    return x.size ? x : y;
}

static void renderer_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    AudioRenderer *r = s->state;
    switch (rq->cmd) {
    case 0: { int32_t v = r->param.sample_rate; IPC_OUT(rs, v); return; }       // GetSampleRate
    case 1: { int32_t v = r->param.sample_count; IPC_OUT(rs, v); return; }      // GetSampleCount
    case 2: { int32_t v = r->param.mix_buffer_count; IPC_OUT(rs, v); return; }  // GetMixBufferCount
    case 3: { uint32_t v = r->state; IPC_OUT(rs, v); return; }                  // GetState
    case 4: case 10: {   // RequestUpdate(Auto): out(output), out(performance), in(input)
        IpcBuffer in = pick(rq->a, rq->na, rq->x, rq->nx, 0);
        IpcBuffer out = pick(rq->b, rq->nb, rq->c, rq->nc, 0), perf = pick(rq->b, rq->nb, rq->c, rq->nc, 1);
        if (!r->logged)
            rt_log("audren: RequestUpdate cmd %u in=%#llx+%#llx out=%#llx+%#llx perf=%#llx+%#llx (a%u b%u x%u c%u)",
                   rq->cmd, (unsigned long long)in.addr, (unsigned long long)in.size, (unsigned long long)out.addr,
                   (unsigned long long)out.size, (unsigned long long)perf.addr, (unsigned long long)perf.size, rq->na,
                   rq->nb, rq->nx, rq->nc);
        need(in.size && out.size, "RequestUpdate buffers");
        request_update(r, ipc_guest(in.addr, in.size), in.size, ipc_guest(out.addr, out.size), out.size, perf.size);
        for (unsigned i = 0; i < rq->nc && rq->cmd == 10; i++)
            if (rq->c[i].size && (i >= rq->nb || !rq->b[i].size))
                rs->xout[rs->nxout++] = (IpcBuffer){rq->c[i].addr, rq->c[i].size, i};
        return;
    }
    case 5:   // Start
        plat_mutex_lock(&r->lock);
        r->state = 0;
        audio_mixer_set_running(r, 1);
        plat_mutex_unlock(&r->lock);
        return;
    case 6:   // Stop
        plat_mutex_lock(&r->lock);
        r->state = 1;
        audio_mixer_set_running(r, 0);
        plat_mutex_unlock(&r->lock);
        return;
    case 7: rs->copy[rs->ncopy++] = kern_event_readable(r->event); return;   // QuerySystemEvent
    case 8: r->time_limit = IPC_ARG(rq, uint32_t, 0); return;                // SetRenderingTimeLimit
    case 9: { uint32_t v = r->time_limit ? r->time_limit : 100; IPC_OUT(rs, v); return; }   // GetRenderingTimeLimit
    default: ipc_unimplemented(s, rq);
    }
}

static void renderer_close(void *p) {
    AudioRenderer *r = p;
    plat_xchg32(&r->running, 0);
    int away = kern_core_away();
    plat_thread_join(r->thread);
    kern_core_back(&away);
    plat_dec32(&g_live);
    audio_mixer_close(r);
    kern_handle_close(r->event);
    rt_log("audren: renderer closed after %llu frames", (unsigned long long)r->elapsed_frames);
    renderer_free(r);
}

// ---- IAudioRendererManager
void audren_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: {   // OpenAudioRenderer(param, pad, work size, aruid; copy tmem, process; pid)
        AudrenParameter p = IPC_ARG(rq, AudrenParameter, 0);
        uint64_t work = IPC_ARG(rq, uint64_t, 0x38);
        need(rq->ncopy >= 1, "OpenAudioRenderer without a transfer memory handle");
        IpcTransferMemory *tm = kern_handle_get(rq->copy[0], KERN_TMEM);
        need(tm && tm->size >= work, "OpenAudioRenderer work buffer is not a transfer memory of the stated size");
        AudioRenderer *r = renderer_new(&p);
        r->running = 1;
        audio_mixer_open(r);
        r->thread = plat_thread_new(render_thread, r, "audren");
        if (!r->thread) rt_fatal("audren: cannot start the render thread");
        plat_inc32(&g_live);
        rt_log("audren: renderer open %d Hz x%d, mix buffers %d, voices %d, effects %d, submixes %d, sinks %d, "
               "splitters %d/%d, REV%d, work buffer %#llx+%#llx",
               p.sample_rate, p.sample_count, p.mix_buffer_count, p.voice_count, p.effect_count, p.submix_count,
               p.sink_count, p.splitter_count, p.splitter_destination_count, revision_number(p.revision),
               (unsigned long long)tm->addr, (unsigned long long)work);
        rs->move[rs->nmove++] = ipc_session_new("IAudioRenderer", renderer_handler, r, renderer_close);
        return;
    }
    case 1: {   // GetWorkBufferSize(param) -> u64
        AudrenParameter p = IPC_ARG(rq, AudrenParameter, 0);
        need(p.sample_rate == 48000 || p.sample_rate == 32000, "GetWorkBufferSize: unsupported output rate");
        uint64_t v = AUDREN_WORK_BUFFER_BYTES;
        IPC_OUT(rs, v);
        return;
    }
    case 2: case 4:   // GetAudioDeviceService(aruid), GetAudioDeviceServiceWithRevisionInfo(revision, aruid)
        rs->move[rs->nmove++] = ipc_session_new("IAudioDevice", audio_device_handler, NULL, NULL);
        return;
    default: ipc_unimplemented(s, rq);
    }
}
