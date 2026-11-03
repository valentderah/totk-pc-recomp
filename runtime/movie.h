// Adapted from MacNativeGames games/totk/runtime/movie_host.h @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames
// contributors).
// What ov_movie.c (the host movie::Decoder) and movie_probe.c share. The game reaches the decoder through the
// HOST_OVERRIDE entries named in movie_symbols; the probe calls the same entries with a Cpu of its own and stand-ins for
// the guest functions the decoder calls.
#pragma once
#include <stdint.h>

#include "host.h"

// Entries in the order of movie_symbols[]: the imported constructor, then the 19 slots of the vtable for movie::Decoder
// (subsdk0+0x560700..0x560790, each an ABS64 relocation by name).
enum { MOVIE_CTOR, MOVIE_D2, MOVIE_D0, MOVIE_CONFIGURE, MOVIE_REGISTER_EVENTS, MOVIE_GET_OUTPUT_FORMAT, MOVIE_START,
       MOVIE_STOP, MOVIE_RESET, MOVIE_FLUSH, MOVIE_ACQUIRE_INPUT, MOVIE_GET_INPUT, MOVIE_SEND_INPUT, MOVIE_ACQUIRE_OUTPUT,
       MOVIE_GET_OUTPUT, MOVIE_GET_OUTPUT_SIZE, MOVIE_RELEASE_OUTPUT, MOVIE_GET_LAST_ERROR, MOVIE_GET_OUTPUT_MD,
       MOVIE_GET_OUTPUT_BUFFER_FORMAT, MOVIE_ENTRIES };
extern const char *const movie_symbols[MOVIE_ENTRIES];

// Guest addresses of the functions the decoder calls with rt_call: nn::os::SignalEvent, movie::Buffer and
// movie::MediaData methods, the vtable address the constructor stores (vtable + 0x10), and the original code of every
// entry for decoders left to the library. Filled from the loaded modules at the first entry call, or by the probe.
typedef struct {
    uint64_t signal_event, buf_set_range, buf_set_data, buf_base, buf_capacity;
    uint64_t md_find_int32, md_find_string, md_set_int32, md_set_rect, md_set_string;
    uint64_t vptr;
    uint64_t orig[MOVIE_ENTRIES];
} MovieGuest;

enum { MOVIE_EV_INPUT, MOVIE_EV_OUTPUT, MOVIE_EV_FORMAT, MOVIE_EV_ERROR };   // DecoderEvents: EventType * per event
enum { MOVIE_TYPE_VP9 = 3, MOVIE_MODE_BUFFER = 1, MOVIE_FORMAT_NV12 = 4, MOVIE_FLAG_EOS = 4 };

typedef struct { uint64_t frames, decode_ns, max_ns, errors, dropped, handed, fit_ns; } MovieStats;
// fit_ns: the part of decode_ns spent writing the NV12 frame

void movie_set_guest(const MovieGuest *g);                 // probe: stand-ins instead of the modules
void movie_set_clock(uint64_t (*clock)(void));             // probe: a fake clock (ns) for the pacing; NULL = real
void movie_set_pace(int on);                               // probe: 0 hands every decoded frame over at the next tick
int movie_stats(uint64_t obj, MovieStats *out);            // counters of the decoder at guest object obj; 0 if none
int movie_quiet(uint64_t obj);                             // 1 when its worker is idle and cannot take more input now

// Keeping a movie's shape on an output of another shape (ov_movie.c): the picture is shrunk inside the frame, with
// black (Y 16, U = V 128) around it. movie_fit_new returns NULL when the shapes agree within 1%: copy the frame.
typedef struct MovieFit MovieFit;
MovieFit *movie_fit_new(int w, int h, unsigned out_w, unsigned out_h);
void movie_fit_apply(const MovieFit *f, const uint8_t *const plane[3], const int stride[3], uint8_t *dst);
void movie_fit_free(MovieFit *f);
