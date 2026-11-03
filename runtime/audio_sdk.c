// The game's own AudioRenderer, run in the process (TOTK_AUDIO_SDK; on by default for Breath of the Wild).
//
// The sdk module carries the whole AudioRenderer: the server (nn::audio::server: voices, mixes, effects, command
// generation) and the DSP's command processor (nn::audio::dsp::CommandListProcessor), CPU code recompiled with the rest
// of the game. nn::audio::OpenAudioRenderer reaches that server in the process (CreateAudioRendererManagerByDfc) when
// the parameter asks for the CPU rendering device (AudioRendererParameter +0x2c = 1), and with manual execution (+0x30
// = 1) it renders on the game's own work buffer, a frame per AudioRenderSystem::ExecuteAudioRendererRendering call. So
// InitializeAudioRendererParameter asks for both, and two guest threads of ours do the ADSP's part: one renders a 5 ms
// frame of every open renderer whenever the SDL stream runs low, each under its renderer's mutex (the one Update and
// Start take), the other signals the renderers' events. Each renderer's final mix is a DeviceSink command, which the
// sdk's processor skips (on the console the ADSP plays it): after CommandListProcessor::Process, which the sdk calls
// through its own PLT, its buffers are added into the frame that goes to SDL. Breath of the Wild opens one renderer
// (both OpenAudioRenderer overloads are observed, and the one with the event calls the other).
// The HLE audren:u mixer (audren.c, audio_mixer.c) serves the game when this is off.
//
// After Savage Wind Recomp's runtime/services/audio.c "the sdk's own AudioRenderer, in process"
// (https://github.com/Revivedx/SavageWindRecomp @ bf6146c14c37fee8781a601e5a544b93dff6d75e, MIT, Copyright (c) 2026
// Revivedx and the Savage Wind Recomp contributors, Copyright (c) 2026 Peiikai): the parameter fields, the two threads,
// the renderer mutex around rendering, the AudioRenderSystem at +0x70 of the renderer object the handle holds (whose
// ExecuteAudioRendererRendering, sdk+0x22d4fc in SDK 20.5.22, adds 0x70) and the DeviceSink command's layout. This
// sdk's AudioRenderSystem::Start takes the mutex at +0x2d0 and sets the started byte at +0x11.
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "audren.h"
#include "cfg.h"
#include "host.h"

#define SYM_PARAM "_ZN2nn5audio32InitializeAudioRendererParameterEPNS0_22AudioRendererParameterE"
#define SYM_OPEN_EVENT                                                                                                 \
    "_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm"
#define SYM_OPEN "_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleERKNS0_22AudioRendererParameterEPvm"
#define SYM_CLOSE "_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE"
#define SYM_PROCESS "_ZN2nn5audio3dsp20CommandListProcessor7ProcessEv"
#define SYM_EXECUTE "_ZN2nn5audio6server17AudioRenderSystem29ExecuteAudioRendererRenderingEv"
#define SYM_LOCK "_ZN2nn2os12SdkMutexType4LockEv"
#define SYM_UNLOCK "_ZN2nn2os12SdkMutexType6UnlockEv"
#define SYM_SIGNAL "_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE"
#define SYM_CREATE_THREAD "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii"
#define SYM_START_THREAD "_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE"
#define SYM_ALIGNED_ALLOC "aligned_alloc"

enum {
    SYSTEM_OFF = 0x70,      // the handle's renderer object -> its AudioRenderSystem
    MUTEX_OFF = 0x2d0,      // AudioRenderSystem: the renderer's mutex
    STARTED_OFF = 0x11,     // AudioRenderSystem: set by Start
    MAX_SAMPLES = 1024,
    MAX_RENDERERS = 4,
    STACK = 0x40000,
};

typedef struct {
    _Atomic int open;
    _Atomic int signal;          // a frame was rendered: the event thread signals this renderer's event
    uint64_t renderer, context;  // the game's AudioRendererHandle
    uint64_t system;             // its AudioRenderSystem
} Ren;

static struct {
    Ren ren[MAX_RENDERERS];
    _Atomic int nopen;
    _Atomic int threads;         // our render and event threads still running
    uint32_t rate, samples;
    SDL_AudioStream *stream;
    int target;                  // bytes queued in the stream that the render thread keeps
    float mix[2 * MAX_SAMPLES];  // the frame being rendered, stereo: every renderer's DeviceSink added
    uint32_t mix_n;              // samples in it (0: no sink yet this frame)
    int sinks;                   // DeviceSinks taken this frame
    uint64_t frames, render_ns, render_max, lock_max, empty;
    float peak;
} S;
static uint64_t a_render, a_event;   // synthetic entry points of our two threads (audio_sdk_init)

static int sdk_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = cfg_get("TOTK_AUDIO_SDK");
        on = v && *v ? strcmp(v, "0") != 0 : RT_BOTW;
        rt_log("audio: the game's own AudioRenderer %s (TOTK_AUDIO_SDK)", on ? "on" : "off: the HLE mixer serves it");
    }
    return on;
}

static uint64_t sym(const char *name) {
    uint64_t a = ldr_symbol(name);
    if (!a) rt_fatal("audio_sdk: %s is not exported", name);
    return a;
}

static int in_guest(uint64_t a, uint64_t n) { return a >= 0x1000000 && a < RT_ADDRESS_LIMIT && n < RT_ADDRESS_LIMIT - a; }

// TOTK_VOLUME, the player's volume in percent, as the HLE mixer applies it (audio_mixer.c host_volume).
static float volume(void) {
    static float v = 1.0f;
    static unsigned seen = ~0u;
    unsigned ver = cfg_version();
    if (ver != seen) {
        seen = ver;
        double pct = cfg_double("TOTK_VOLUME");
        v = (float)((pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100);
    }
    return v;
}

// The command list a processor ran: +0x04 command count, +0x14 samples per buffer, commands from +0x20, each with
// +0x04 bit 0 enabled, +0x05 id, +0x06 size. DeviceSinkCommand (id 0x14, 0x148 bytes): +0x128 its sample buffers (int32
// samples on the int16 scale), +0x130 the input count, +0x134 the input buffer indices (int16), FL FR FC LFE BL BR.
// Added into S.mix.
static void take_sink(uint64_t list) {
    if (!in_guest(list, 0x20)) return;
    uint32_t count = *(const uint32_t *)(uintptr_t)(list + 4), n = *(const uint32_t *)(uintptr_t)(list + 0x14);
    if (!n || n > MAX_SAMPLES) return;
    uint64_t cmd = list + 0x20;
    for (uint32_t i = 0; i < count && in_guest(cmd, 8); i++) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)cmd;
        int16_t size = *(const int16_t *)(p + 6);
        if (p[5] == 0x14 && (p[4] & 1) && in_guest(cmd, 0x148)) {
            uint64_t buf = *(const uint64_t *)(p + 0x128);
            uint32_t inputs = *(const uint32_t *)(p + 0x130);
            if (inputs > 6) inputs = 6;
            const int32_t *ch[6] = {0};
            for (uint32_t k = 0; k < inputs; k++) {
                int16_t idx = *(const int16_t *)(p + 0x134 + 2 * k);
                uint64_t a = buf + (uint64_t)idx * n * 4;
                if (idx >= 0 && in_guest(a, (uint64_t)n * 4)) ch[k] = (const int32_t *)(uintptr_t)a;
            }
            if (!S.mix_n) memset(S.mix, 0, sizeof(float) * 2 * n);
            for (uint32_t s = 0; s < n; s++) {
                float v[6] = {0};
                for (uint32_t k = 0; k < inputs; k++) v[k] = ch[k] ? (float)ch[k][s] : 0;
                float l = v[0], r = inputs == 1 ? v[0] : v[1];
                if (inputs >= 6) {   // 5.1 down to stereo
                    l += 0.707f * v[2] + 0.251f * v[3] + 0.707f * v[4];
                    r += 0.707f * v[2] + 0.251f * v[3] + 0.707f * v[5];
                }
                S.mix[2 * s] += l / 32768.f;
                S.mix[2 * s + 1] += r / 32768.f;
            }
            if (n > S.mix_n) S.mix_n = n;
            S.sinks++;
            return;
        }
        if (size <= 0) break;
        cmd += (uint64_t)size;
    }
}

// svcSleepThread: the virtual core goes to other threads meanwhile.
static void sleep_ns(Cpu *c, uint64_t ns) {
    c->x[0] = ns;
    rt_svc(c, 0x0B, 0);
}

// Our render thread (a guest thread: the sdk's code runs on it). A frame of every started renderer each time the stream
// drops below its target.
static void render_main(Cpu *c) {
    uint64_t exec = sym(SYM_EXECUTE), lock = sym(SYM_LOCK), unlock = sym(SYM_UNLOCK);
    uint32_t n0 = S.samples ? S.samples : 240;
    while (atomic_load(&S.nopen)) {
        if (S.stream && SDL_GetAudioStreamQueued(S.stream) >= S.target) {
            sleep_ns(c, 1000000);
            continue;
        }
        int started = 0;
        uint64_t t_lock = 0, t_render = 0;
        S.mix_n = 0, S.sinks = 0;
        for (int i = 0; i < MAX_RENDERERS; i++) {
            Ren *r = &S.ren[i];
            if (!atomic_load(&r->open) || !*(volatile const uint8_t *)(uintptr_t)(r->system + STARTED_OFF)) continue;
            started++;
            uint64_t t0 = plat_now_ns();
            rt_call(c, lock, 1, r->system + MUTEX_OFF);
            uint64_t t1 = plat_now_ns();
            uint64_t er = rt_call(c, exec, 1, r->system);
            rt_call(c, unlock, 1, r->system + MUTEX_OFF);
            static int logged;
            if ((uint32_t)er && logged++ < 3) rt_log("audio: ExecuteAudioRendererRendering -> %#x", (unsigned)er);
            atomic_store(&r->signal, 1);
            if (t1 - t0 > t_lock) t_lock = t1 - t0;
            t_render += plat_now_ns() - t1;
        }
        if (!started) {   // StartAudioRenderer not called yet
            sleep_ns(c, 5000000);
            continue;
        }
        uint32_t n = S.mix_n ? S.mix_n : n0;
        if (!S.mix_n) memset(S.mix, 0, sizeof(float) * 2 * n), S.empty++;
        float g = volume();
        for (uint32_t i = 0; i < 2 * n; i++) {
            float x = S.mix[i] * g;
            S.mix[i] = x < -1 ? -1 : x > 1 ? 1 : x;
            if (fabsf(S.mix[i]) > S.peak) S.peak = fabsf(S.mix[i]);
        }
        if (S.stream) SDL_PutAudioStreamData(S.stream, S.mix, (int)(n * 2 * sizeof(float)));
        static FILE *dump;   // TOTK_AUDIO_DUMP=<path>: the frames as raw float32 stereo, as the HLE mixer writes them
        static int dump_tried;
        if (!dump_tried) {
            dump_tried = 1;
            const char *path = cfg_get("TOTK_AUDIO_DUMP");
            if (path && *path) dump = fopen(path, "wb");
        }
        if (dump) fwrite(S.mix, sizeof(float), 2 * (size_t)n, dump);
        S.frames++;
        S.render_ns += t_render;
        if (t_render > S.render_max) S.render_max = t_render;
        if (t_lock > S.lock_max) S.lock_max = t_lock;
        if ((rt_trace & RT_TRACE_AUDIO) && S.frames % 1000 == 0) {   // every 5 s at 5 ms a frame
            rt_log("audio: sdk renderer %llu frames: render %.0f us avg, %.0f max; mutex wait max %.0f us; %llu without "
                   "a sink; %d renderers, %d sinks; peak %.3f; queued %.1f ms",
                   (unsigned long long)S.frames, S.render_ns / 1000 / 1e3, S.render_max / 1e3, S.lock_max / 1e3,
                   (unsigned long long)S.empty, started, S.sinks, S.peak,
                   S.stream ? SDL_GetAudioStreamQueued(S.stream) / (8.0 * S.rate / 1000) : 0.0);
            S.render_ns = S.render_max = S.lock_max = S.empty = 0;
            S.peak = 0;
        }
    }
    atomic_fetch_sub(&S.threads, 1);
}

// The game's renderer events, signaled from a thread of their own: SignalSystemEvent takes the event's mutex, which the
// game's audio thread may hold while it waits for a core, and the next frame must not wait for that.
static void event_main(Cpu *c) {
    uint64_t sig = sym(SYM_SIGNAL);
    while (atomic_load(&S.nopen)) {
        int any = 0;
        for (int i = 0; i < MAX_RENDERERS; i++) {
            Ren *r = &S.ren[i];
            if (!atomic_exchange(&r->signal, 0)) continue;
            any = 1;
            if (atomic_load(&r->open))   // the event OpenAudioRenderer made
                rt_call(c, sig, 1, *(const uint64_t *)(uintptr_t)r->context);
        }
        if (!any) sleep_ns(c, 500000);
    }
    atomic_fetch_sub(&S.threads, 1);
}

// An nn::os thread (the sdk finds the current nn::os::ThreadType through TLS, and its mutexes use it) at entry. Its
// ThreadType and stack come from the game's heap (aligned_alloc): nn::os maps the stack into the stack region with
// svcMapMemory, which takes heap memory only.
static void start_thread(Cpu *c, uint64_t entry) {
    uint64_t alloc = sym(SYM_ALIGNED_ALLOC);
    uint64_t thread = rt_call(c, alloc, 2, (uint64_t)16, (uint64_t)0x400);
    uint64_t stack = rt_call(c, alloc, 2, (uint64_t)0x1000, (uint64_t)STACK);
    if (!thread || !stack) {
        rt_log("audio: no memory for the sdk renderer's threads");
        return;
    }
    memset((void *)(uintptr_t)thread, 0, 0x400);
    uint64_t res = rt_call(c, sym(SYM_CREATE_THREAD), 7, thread, entry, (uint64_t)0, stack, (uint64_t)STACK,
                           (uint64_t)0, (uint64_t)(int64_t)-2);
    if ((uint32_t)res) {
        rt_log("audio: nn::os::CreateThread for the sdk renderer failed: %#x", (unsigned)res);
        return;
    }
    atomic_fetch_add(&S.threads, 1);
    rt_call(c, sym(SYM_START_THREAD), 1, thread);
}

static void output_open(void) {
    const char *mode = cfg_get("TOTK_AUDIO");
    if ((mode && !strcmp(mode, "none")) || cfg_get("TOTK_AUDIO_OFF")) return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        rt_log("audio: SDL audio init failed (%s): rendering without sound", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, (int)S.rate};
    S.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!S.stream || !SDL_ResumeAudioStreamDevice(S.stream)) {
        rt_log("audio: no output device (%s): rendering without sound", SDL_GetError());
        if (S.stream) SDL_DestroyAudioStream(S.stream);
        S.stream = NULL;
        return;
    }
    SDL_AudioSpec dev;
    int period = 0;
    if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(S.stream), &dev, &period) || period <= 0) period = 480;
    const char *pf = cfg_get("TOTK_AUDIO_PREFILL_MS");
    int ms = pf && *pf ? atoi(pf) : 20;
    int frames = (int)S.rate * (ms < 5 ? 5 : ms > 500 ? 500 : ms) / 1000;
    if (frames < 2 * period) frames = 2 * period;
    S.target = frames * 2 * (int)sizeof(float);
    const char *name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(S.stream));
    rt_log("audio: SDL device '%s' opened for the sdk renderer, %u Hz stereo float, %.1f ms queued, device period %d",
           name ? name : "?", S.rate, frames * 1000.0 / S.rate, period);
}

static void obs_param(Cpu *c) {
    uint64_t p = c->x[0];
    rt_original(c);
    if (!sdk_on()) return;
    *(uint32_t *)(uintptr_t)(p + 0x2c) = 1;   // rendering device: CPU
    *(uint32_t *)(uintptr_t)(p + 0x30) = 1;   // execution mode: manual
}

static void opened(Cpu *c, uint64_t h, uint64_t p) {
    const uint32_t *pp = (const uint32_t *)(uintptr_t)p;
    uint32_t res = (uint32_t)c->x[0];
    rt_log("audio: OpenAudioRenderer %u Hz, %u samples, %u mix buffers, %u voices, %u effects, device %u, mode %u -> %#x",
           pp[0], pp[1], pp[2], pp[4], pp[6], pp[11], pp[12], res);
    if (res || pp[11] != 1 || pp[12] != 1 || !sdk_on()) return;
    // The overload with the event calls the other one, and both are observed: one renderer, seen twice. Rendering it
    // twice a frame summed two consecutive frames (clicks at every block edge).
    for (int i = 0; i < MAX_RENDERERS; i++)
        if (atomic_load(&S.ren[i].open) && S.ren[i].renderer == *(const uint64_t *)(uintptr_t)h) return;
    Ren *r = NULL;
    for (int i = 0; i < MAX_RENDERERS && !r; i++)
        if (!atomic_load(&S.ren[i].open)) r = &S.ren[i];
    if (!r) {
        rt_log("audio: more than %d sdk renderers: this one is not rendered", MAX_RENDERERS);
        return;
    }
    uint64_t x[8];
    memcpy(x, c->x, sizeof x);
    r->renderer = *(const uint64_t *)(uintptr_t)h;
    r->context = *(const uint64_t *)(uintptr_t)(h + 8);
    r->system = r->renderer + SYSTEM_OFF;
    if (!S.rate) S.rate = pp[0], S.samples = pp[1] <= MAX_SAMPLES ? pp[1] : MAX_SAMPLES;
    if (!S.stream) output_open();
    atomic_store(&r->open, 1);
    if (!atomic_fetch_add(&S.nopen, 1) && !atomic_load(&S.threads)) {
        start_thread(c, a_render);
        start_thread(c, a_event);
    }
    memcpy(c->x, x, sizeof x);
}

// OpenAudioRenderer(AudioRendererHandle *, os::SystemEvent *, const AudioRendererParameter &, void *, size_t)
static void obs_open_event(Cpu *c) {
    uint64_t h = c->x[0], p = c->x[2];
    rt_original(c);
    opened(c, h, p);
}

// OpenAudioRenderer(AudioRendererHandle *, const AudioRendererParameter &, void *, size_t)
static void obs_open(Cpu *c) {
    uint64_t h = c->x[0], p = c->x[1];
    rt_original(c);
    opened(c, h, p);
}

// CloseAudioRenderer(AudioRendererHandle): the render thread finishes its frame of the renderer (it holds the
// renderer's mutex meanwhile) before the renderer goes away.
static void obs_close(Cpu *c) {
    for (int i = 0; i < MAX_RENDERERS; i++) {
        Ren *r = &S.ren[i];
        if (!atomic_load(&r->open) || c->x[0] != r->renderer) continue;
        uint64_t x[8];
        memcpy(x, c->x, sizeof x);
        rt_call(c, sym(SYM_LOCK), 1, r->system + MUTEX_OFF);
        atomic_store(&r->open, 0);
        atomic_store(&r->signal, 0);
        rt_call(c, sym(SYM_UNLOCK), 1, r->system + MUTEX_OFF);
        if (atomic_fetch_sub(&S.nopen, 1) == 1)
            while (atomic_load(&S.threads)) sleep_ns(c, 1000000);   // the last one: our threads end first
        memcpy(c->x, x, sizeof x);
    }
    rt_original(c);
}

// CommandListProcessor::Process (x0: the processor, whose +0x00 is its command list): the DeviceSink after it.
static void obs_process(Cpu *c) {
    uint64_t proc = c->x[0];
    uint64_t list = in_guest(proc, 8) ? *(const uint64_t *)(uintptr_t)proc : 0;
    rt_original(c);
    if (atomic_load(&S.nopen)) take_sink(list);
}

HOST_OBSERVER(SYM_PARAM, obs_param);
HOST_OBSERVER(SYM_OPEN_EVENT, obs_open_event);
HOST_OBSERVER(SYM_OPEN, obs_open);
HOST_OBSERVER(SYM_CLOSE, obs_close);
HOST_OBSERVER(SYM_PROCESS, obs_process);

void audio_sdk_init(void) {
    a_render = hostfn_register("<audio sdk render thread>", render_main);
    a_event = hostfn_register("<audio sdk event thread>", event_main);
}
