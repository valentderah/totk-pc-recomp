// In-memory IPC checks of the production Discord frame reader. No Discord client is contacted.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "discord_frames.c"

struct PlatIpc {
    uint8_t input[4096], output[4096];
    size_t input_n, read_at, output_n, chunk;
    int closed, write_fail, read_calls;
    uint32_t header_timeout, payload_timeout;
};

enum { CLOCK_FIXED, CLOCK_HEADER_CROSS, CLOCK_PAYLOAD_CROSS, CLOCK_WHEN_EMPTY };
static int clock_mode;
static unsigned clock_calls;
static PlatIpc *clock_pipe;

uint64_t plat_now_ns(void) {
    uint64_t ms = 0;
    if (clock_mode == CLOCK_HEADER_CROSS)
        ms = clock_calls++ ? 101 : 99;
    else if (clock_mode == CLOCK_WHEN_EMPTY)
        ms = clock_pipe->read_at == clock_pipe->input_n ? 101 : 0;
    else if (clock_mode == CLOCK_PAYLOAD_CROSS) {
        if (clock_pipe->read_at < 8) ms = 90;
        else ms = clock_calls++ ? 101 : 99;
    }
    return ms * 1000000ull;
}

int plat_ipc_read(PlatIpc *p, void *out, size_t n, uint32_t ms) {
    uint32_t *timeout = p->read_at < 8 ? &p->header_timeout : &p->payload_timeout;
    if (ms > *timeout) *timeout = ms;
    p->read_calls++;
    size_t available = p->input_n - p->read_at;
    if (!available) return p->closed ? -1 : 0;
    size_t take = available < n ? available : n;
    if (p->chunk && take > p->chunk) take = p->chunk;
    memcpy(out, p->input + p->read_at, take);
    p->read_at += take;
    return (int)take;
}

int plat_ipc_write(PlatIpc *p, const void *data, size_t n) {
    if (p->write_fail || n > sizeof p->output - p->output_n) return 0;
    memcpy(p->output + p->output_n, data, n);
    p->output_n += n;
    return 1;
}

static int checks, failures;
#define CHECK(condition, name) do { \
    checks++; \
    if (!(condition)) { failures++; printf("FAIL Discord frames: %s\n", name); } \
} while (0)

static void reset_clock(void) {
    clock_mode = CLOCK_FIXED;
    clock_calls = 0;
    clock_pipe = NULL;
}

static void append_frame(PlatIpc *p, uint32_t op, const char *payload) {
    size_t n = strlen(payload);
    if (n + 8 > sizeof p->input - p->input_n) {
        fprintf(stderr, "Discord fixture input overflow\n");
        return;
    }
    le32(p->input + p->input_n, op);
    le32(p->input + p->input_n + 4, (uint32_t)n);
    memcpy(p->input + p->input_n + 8, payload, n);
    p->input_n += 8 + n;
}

static void check_fragmented_payload(void) {
    PlatIpc p = {.chunk = 2};
    append_frame(&p, OP_FRAME, "abcdef");
    char out[32]; memset(out, '!', sizeof out);
    uint32_t op = 0;
    int result = read_one_frame(&p, &op, out, sizeof out, 100);
    CHECK(result == 1 && op == OP_FRAME && !memcmp(out, "abcdef\0", 7) && p.read_at == p.input_n,
          "fragmented header and payload retain every byte");
}

static void check_fragmented_ping(void) {
    PlatIpc p = {.chunk = 2};
    append_frame(&p, OP_PING, "abcdef");
    append_frame(&p, OP_FRAME, "next");
    char out[32]; memset(out, '!', sizeof out);
    uint32_t op = 0;
    int result = read_frame(&p, &op, out, sizeof out, 100);
    uint8_t expected[14];
    le32(expected, OP_PONG); le32(expected + 4, 6); memcpy(expected + 8, "abcdef", 6);
    CHECK(p.output_n == sizeof expected && !memcmp(p.output, expected, sizeof expected),
          "fragmented Ping is echoed with its exact payload and length");
    CHECK(result == 1 && op == OP_FRAME && !memcmp(out, "next\0", 5) && p.read_at == p.input_n,
          "the Ready frame following a fragmented Ping stays intact");
}

static void check_oversized_frame(void) {
    PlatIpc p = {0};
    append_frame(&p, OP_FRAME, "abcdefghijkl");
    append_frame(&p, OP_FRAME, "next");
    char out[5]; memset(out, '!', sizeof out);
    uint32_t op = 0;
    int result = read_one_frame(&p, &op, out, sizeof out, 100);
    CHECK(result == 1 && op == OP_FRAME && !memcmp(out, "abcd\0", 5),
          "an oversized frame retains the first cap-1 bytes");
    result = read_one_frame(&p, &op, out, sizeof out, 100);
    CHECK(result == 1 && op == OP_FRAME && !memcmp(out, "next\0", 5) && p.read_at == p.input_n,
          "the discarded oversized suffix is drained before the next frame");
}

static void check_deadlines(void) {
    PlatIpc header = {0};
    clock_mode = CLOCK_HEADER_CROSS;
    clock_calls = 0;
    char out[32]; memset(out, '!', sizeof out);
    uint32_t op = 0;
    int result = read_one_frame(&header, &op, out, sizeof out, 100);
    printf("header deadline read timeout: %u ms\n", header.header_timeout);
    CHECK(result == 0 && header.read_calls == 1 && header.header_timeout <= 1,
          "header deadline crossing cannot underflow its read timeout");
    reset_clock();
    PlatIpc payload = {0};
    append_frame(&payload, OP_FRAME, "abcdef");
    payload.input_n = 8;   // The header arrived; the peer has not supplied the payload yet.
    clock_mode = CLOCK_PAYLOAD_CROSS;
    clock_pipe = &payload;
    clock_calls = 0;
    result = read_one_frame(&payload, &op, out, sizeof out, 100);
    printf("payload deadline read timeout: %u ms\n", payload.payload_timeout);
    CHECK(result == -1 && payload.read_calls == 2 && payload.payload_timeout <= 1,
          "payload deadline crossing cannot underflow its read timeout");
    reset_clock();
}

static void check_partial_timeouts(void) {
    char out[32];
    uint32_t op = 0;
    for (int length = 3; length <= 10; length += 7) {
        PlatIpc partial = {0};
        append_frame(&partial, OP_FRAME, "abcdef");
        partial.input_n = (size_t)length;
        clock_mode = CLOCK_WHEN_EMPTY;
        clock_pipe = &partial;
        int result = read_frame(&partial, &op, out, sizeof out, 100);
        CHECK(result == -1, length == 3 ? "partial header timeout requires reconnect"
                                       : "partial payload timeout requires reconnect");
        reset_clock();
    }
}

static void check_other_outcomes(void) {
    char out[32]; memset(out, '!', sizeof out);
    uint32_t op = 0;
    PlatIpc empty = {0};
    CHECK(read_one_frame(&empty, &op, out, sizeof out, 0) == 0, "no bytes before deadline returns timeout");
    PlatIpc closed = {.closed = 1};
    CHECK(read_one_frame(&closed, &op, out, sizeof out, 100) == -1, "closed IPC returns failure");
    PlatIpc close_frame = {0};
    append_frame(&close_frame, OP_CLOSE, "gone");
    CHECK(read_frame(&close_frame, &op, out, sizeof out, 100) == -1, "Close frame ends the connection");
    PlatIpc failed_write = {.write_fail = 1};
    append_frame(&failed_write, OP_PING, "ping");
    CHECK(read_frame(&failed_write, &op, out, sizeof out, 100) == -1, "failed Pong write ends the connection");
    PlatIpc no_payload = {0};
    append_frame(&no_payload, OP_FRAME, "");
    out[0] = '!';
    CHECK(read_one_frame(&no_payload, &op, out, sizeof out, 100) == 1 && op == OP_FRAME && !out[0],
          "empty frame payload is terminated");
}

int main(void) {
    reset_clock();
    check_fragmented_payload();
    check_fragmented_ping();
    check_oversized_frame();
    check_deadlines();
    check_partial_timeouts();
    check_other_outcomes();
    printf("Discord frame reader: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
