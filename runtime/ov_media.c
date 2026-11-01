// Adapted from MacNativeGames games/totk/runtime/opus_codec.c @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames
// contributors).
// The hardware Opus decoder (nn::codec), decoded with libopus on the host. The software keyboard (nn::swkbd) is
// in swkbd_win.c.
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>

#include <opus/opus.h>

#include "host.h"
#include "platform/platform.h"

#define CODEC_R_INVALID 0x1fcd1u   // MacNativeGames' "invalid argument" result for nn::codec
#define OPUS_WORK_SIZE 0x9000u     // what the game is told to reserve: libopus stereo decoder state (about 26 KiB) plus 4 KiB

// One record per initialised decoder object (the guest's HardwareOpusDecoderType). The registry lock only guards the
// list; a decoder's own lock serialises its libopus state, so different streams decode in parallel. Lock order:
// registry, then decoder (a caller takes the decoder lock before it drops the registry lock, so Finalize cannot free a
// decoder another thread is using).
typedef struct Decoder {
    uint64_t guest;
    OpusDecoder *dec;
    int rate, channels;
    PlatMutex use;
    struct Decoder *next;
} Decoder;
static Decoder *decoders;
static PlatMutex lock = PLAT_MUTEX_INIT;

static int valid_format(int rate, int channels) {
    return (rate == 48000 || rate == 24000 || rate == 16000 || rate == 12000 || rate == 8000) &&
           (channels == 1 || channels == 2);
}

// Returns the decoder with its use lock held.
static Decoder *acquire(uint64_t guest, const char *fn) {
    plat_mutex_lock_shared(&lock);
    Decoder *d = decoders;
    while (d && d->guest != guest) d = d->next;
    if (d) plat_mutex_lock(&d->use);
    plat_mutex_unlock_shared(&lock);
    if (!d) rt_fatal("nn::codec::%s: unknown hardware Opus decoder %#llx", fn, (unsigned long long)guest);
    return d;
}

static void release(Decoder *d) { plat_mutex_unlock(&d->use); }

static OpusDecoder *opus_new(int rate, int channels) {
    int err = 0;
    OpusDecoder *o = opus_decoder_create(rate, channels, &err);
    if (!o || err != OPUS_OK) rt_fatal("nn::codec: opus_decoder_create(%d, %d): %s", rate, channels, opus_strerror(err));
    return o;
}

// size_t GetHardwareOpusDecoderWorkBufferSize(int rate, int channels, int option)
static void ov_work_size(Cpu *c) {
    ret_x(c, valid_format((int)arg_x(c, 0), (int)arg_x(c, 1)) ? OPUS_WORK_SIZE : 0);
}

// Result InitializeHardwareOpusDecoder(HardwareOpusDecoderType *d, int rate, int channels, void *work, size_t size,
// int option). The state lives in libopus on the host; the guest's work buffer is reserved but unused. Initialising
// the same object again without Finalize replaces its decoder.
static void ov_initialize(Cpu *c) {
    uint64_t guest = arg_x(c, 0), size = arg_x(c, 4);
    int rate = (int)arg_x(c, 1), channels = (int)arg_x(c, 2);
    if (!guest || !valid_format(rate, channels) || size < OPUS_WORK_SIZE) { ret_x(c, CODEC_R_INVALID); return; }
    OpusDecoder *fresh = opus_new(rate, channels);
    plat_mutex_lock(&lock);
    Decoder *d = decoders;
    while (d && d->guest != guest) d = d->next;
    if (d) {
        plat_mutex_lock(&d->use);   // waits for a decode in flight
        OpusDecoder *old = d->dec;
        d->dec = fresh;
        d->rate = rate;
        d->channels = channels;
        plat_mutex_unlock(&d->use);
        plat_mutex_unlock(&lock);
        opus_decoder_destroy(old);
    } else {
        d = calloc(1, sizeof *d);
        if (!d) rt_fatal("nn::codec: decoder allocation");
        plat_mutex_init(&d->use);
        d->guest = guest;
        d->dec = fresh;
        d->rate = rate;
        d->channels = channels;
        d->next = decoders;
        decoders = d;
        plat_mutex_unlock(&lock);
    }
    ret_x(c, 0);
}

// void FinalizeHardwareOpusDecoder(HardwareOpusDecoderType *d)
static void ov_finalize(Cpu *c) {
    uint64_t guest = arg_x(c, 0);
    plat_mutex_lock(&lock);
    Decoder **p = &decoders;
    while (*p && (*p)->guest != guest) p = &(*p)->next;
    if (!*p) rt_fatal("nn::codec::FinalizeHardwareOpusDecoder: unknown decoder %#llx", (unsigned long long)guest);
    Decoder *d = *p;
    plat_mutex_lock(&d->use);   // a decode in flight finishes first; the registry lock keeps new ones out
    *p = d->next;
    plat_mutex_unlock(&lock);
    opus_decoder_destroy(d->dec);
    free(d);
}

// int GetHardwareOpusDecoderSampleRate / GetHardwareOpusDecoderChannelCount(const HardwareOpusDecoderType *d)
static void ov_sample_rate(Cpu *c) {
    Decoder *d = acquire(arg_x(c, 0), "GetHardwareOpusDecoderSampleRate");
    int v = d->rate;
    release(d);
    ret_x(c, (uint64_t)v);
}

static void ov_channel_count(Cpu *c) {
    Decoder *d = acquire(arg_x(c, 0), "GetHardwareOpusDecoderChannelCount");
    int v = d->channels;
    release(d);
    ret_x(c, (uint64_t)v);
}

// Result ResetHardwareOpusDecoder(HardwareOpusDecoderType *d)
static void ov_reset(Cpu *c) {
    Decoder *d = acquire(arg_x(c, 0), "ResetHardwareOpusDecoder");
    int ok = opus_decoder_ctl(d->dec, OPUS_RESET_STATE);
    release(d);
    ret_x(c, ok == OPUS_OK ? 0 : CODEC_R_INVALID);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// Result DecodeOpusInterleavedWithHardware(HardwareOpusDecoderType *d, size_t *consumed, int *samples, int16_t *out,
// size_t out_size, const void *in, size_t in_size). The input is Nintendo's frame: an 8-byte big-endian header
// {payload size, final range} and the Opus packet. A header range of 0 is not a recorded value (some encoders write 0
// into every packet), so it is not checked. A range mismatch is logged once and the samples are kept.
static void ov_decode(Cpu *c) {
    static int32_t warned;
    uint64_t guest = arg_x(c, 0), out_size = arg_x(c, 4), in_size = arg_x(c, 6);
    size_t *consumed = arg_p(c, 1);
    int *samples = arg_p(c, 2);
    int16_t *out = arg_p(c, 3);
    const uint8_t *in = arg_p(c, 5);
    if (!consumed || !samples || !out || !in || in_size < 8) { ret_x(c, CODEC_R_INVALID); return; }
    uint32_t n = be32(in), range = be32(in + 4);
    if (n > in_size - 8 || n > INT_MAX) { ret_x(c, CODEC_R_INVALID); return; }
    Decoder *d = acquire(guest, "DecodeOpusInterleavedWithHardware");
    size_t capacity = out_size / (sizeof(int16_t) * (size_t)d->channels);
    if (capacity > INT_MAX) { release(d); ret_x(c, CODEC_R_INVALID); return; }
    int got = opus_decode(d->dec, in + 8, (opus_int32)n, out, (int)capacity, 0);
    opus_uint32 final = 0;
    if (got >= 0) opus_decoder_ctl(d->dec, OPUS_GET_FINAL_RANGE(&final));
    release(d);
    if (got < 0) {
        rt_log("nn::codec: opus_decode: %s", opus_strerror(got));
        ret_x(c, CODEC_R_INVALID);
        return;
    }
    if (n && range && final != range && !plat_xchg32(&warned, 1))
        rt_log("nn::codec: Opus final range %#x != stream %#x (logged once)", (unsigned)final, (unsigned)range);
    *consumed = 8 + n;
    *samples = got;
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn5codec36GetHardwareOpusDecoderWorkBufferSizeEiii", ov_work_size);
HOST_OVERRIDE("_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiPvmi", ov_initialize);
HOST_OVERRIDE("_ZN2nn5codec27FinalizeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", ov_finalize);
HOST_OVERRIDE("_ZN2nn5codec32GetHardwareOpusDecoderSampleRateEPKNS0_23HardwareOpusDecoderTypeE", ov_sample_rate);
HOST_OVERRIDE("_ZN2nn5codec34GetHardwareOpusDecoderChannelCountEPKNS0_23HardwareOpusDecoderTypeE", ov_channel_count);
HOST_OVERRIDE("_ZN2nn5codec24ResetHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", ov_reset);
HOST_OVERRIDE("_ZN2nn5codec33DecodeOpusInterleavedWithHardwareEPNS0_23HardwareOpusDecoderTypeEPmPiPsmPKvm", ov_decode);
