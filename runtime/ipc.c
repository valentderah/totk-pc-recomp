// Adapted from MacNativeGames games/totk/runtime/ipc.c @ b75be8c (MIT, Copyright (c) 2026 MacNativeGames
// contributors). Message framing per libnx sf/hipc.h, cmif.h, tipc.h (ISC), read for the layout only.
// In-process Horizon IPC: SVCs 0x15, 0x1F, 0x21, 0x22, the sm: service manager
// and the service table. The SDK client code runs unchanged: requests are read from the calling thread's message
// buffer (TLR, or the user buffer of 0x22) and answered in place. Unknown ports, services and commands stop with their
// name and id instead of returning a fake success. Also --probe-ipc.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audren.h"
#include "host.h"
#include "ipc.h"
#include "platform/platform.h"

#define CMIF_IN_MAGIC 0x49434653u    // "SFCI"
#define CMIF_OUT_MAGIC 0x4f434653u   // "SFCO"
#define POINTER_BUFFER_SIZE 0x8000
#define R_INVALID_SIZE 0xCA01u
#define R_INVALID_ADDRESS 0xCC01u

enum { T_CLOSE = 2, T_REQUEST = 4, T_CONTROL = 5, T_REQUEST_CTX = 6, T_CONTROL_CTX = 7, T_TIPC_CLOSE = 15,
       T_TIPC_BASE = 16 };

// Services sm: hands out. Anything else is a named fatal.
static const struct { const char *name; IpcHandler handler; } services[] = {
    {"audren:u", audren_manager_handler},
};

static volatile int32_t g_live;
static volatile int64_t g_released, g_requests;
static _Thread_local const Cpu *t_req;   // guest context of the request being served

long ipc_sessions_live(void) { return plat_cas32(&g_live, 0, 0); }
uint64_t ipc_sessions_released(void) { return (uint64_t)plat_cas64(&g_released, 0, 0); }

// ---- sessions
uint32_t ipc_session_new(const char *name, IpcHandler handler, void *state, void (*close)(void *state)) {
    IpcSession *s = calloc(1, sizeof *s);
    if (!s) rt_fatal("ipc: session allocation");
    *s = (IpcSession){name, handler, state, close, 1};
    plat_inc32(&g_live);
    return kern_handle_new(KERN_SESSION, s);
}

static uint32_t session_clone(IpcSession *s) {
    plat_inc32(&s->refs);
    return kern_handle_new(KERN_SESSION, s);
}

void ipc_handle_closed(int kind, void *obj) {
    if (kind == KERN_TMEM) {
        free(obj);
        return;
    }
    if (kind != KERN_SESSION) rt_fatal("ipc_handle_closed: handle kind %d", kind);
    IpcSession *s = obj;
    if (plat_dec32(&s->refs)) return;
    if (rt_trace & RT_TRACE_IPC) rt_log("ipc: %s closed", s->name);
    if (s->close) s->close(s->state);
    free(s);
    plat_dec32(&g_live);
    plat_inc64(&g_released);
}

// ---- helpers for services
static int readable(uint64_t a, uint64_t n) { return plat_mem_readable(a, n); }

void ipc_backtrace(void) {
    const Cpu *c = t_req;
    if (!c) return;
    char d[160];
    rt_logf("  ipc request from %s\n", ldr_describe(c->x[30], d, sizeof d));
    uint64_t fp = c->x[29];
    for (int n = 0; n < 24 && fp && !(fp & 7) && readable(fp, 16); n++) {
        uint64_t next = LD64(fp), lr = LD64(fp + 8);
        rt_logf("  guest frame %2d: %s\n", n, ldr_describe(lr, d, sizeof d));
        if (next <= fp || next - fp > (64u << 20)) break;
        fp = next;
    }
}

void *ipc_guest(uint64_t addr, uint64_t size) {
    if ((!addr && size) || addr + size < addr || addr + size > RT_ADDRESS_LIMIT) {
        ipc_backtrace();
        rt_fatal("ipc: client buffer %#llx+%#llx", (unsigned long long)addr, (unsigned long long)size);
    }
    return (void *)(uintptr_t)addr;
}

const void *ipc_arg(const IpcRequest *rq, uint32_t off, uint32_t n) {
    if (off + n > rq->data_size) {
        ipc_backtrace();
        rt_fatal("ipc: command %u argument %u+%u beyond %u bytes", rq->cmd, off, n, rq->data_size);
    }
    return rq->data + off;
}

void ipc_out(IpcResponse *rs, const void *p, uint32_t n) {
    if (rs->data_size + n > sizeof rs->data) rt_fatal("ipc: response larger than %u bytes", (unsigned)sizeof rs->data);
    memcpy(rs->data + rs->data_size, p, n);
    rs->data_size += n;
}

void ipc_unimplemented(const IpcSession *s, const IpcRequest *rq) {
    fflush(stdout);
    rt_logf("ipc: %s command %u (type %u, data %u bytes, x%u a%u b%u w%u c%u, copy %u move %u)\n", s->name,
            rq->cmd, rq->type, rq->data_size, rq->nx, rq->na, rq->nb, rq->nw, rq->nc, rq->ncopy, rq->nmove);
    ipc_backtrace();
    rt_fatal("ipc: %s command %u is not implemented", s->name, rq->cmd);
}

// ---- HIPC framing
static uint64_t static_addr(const uint32_t *d) {
    return d[1] | ((uint64_t)((d[0] >> 12) & 0xf) << 32) | ((uint64_t)((d[0] >> 6) & 0x3f) << 36);
}
static uint64_t buffer_addr(const uint32_t *d) {
    return d[1] | ((uint64_t)((d[2] >> 28) & 0xf) << 32) | ((uint64_t)((d[2] >> 2) & 0x3fffff) << 36);
}
static uint64_t buffer_size(const uint32_t *d) { return d[0] | ((uint64_t)((d[2] >> 24) & 0xf) << 32); }

static void parse(const uint32_t *m, IpcRequest *rq, const uint32_t **data_words, unsigned *ndata) {
    memset(rq, 0, sizeof *rq);
    rq->type = m[0] & 0xffff;
    rq->nx = (m[0] >> 16) & 0xf;
    rq->na = (m[0] >> 20) & 0xf;
    rq->nb = (m[0] >> 24) & 0xf;
    rq->nw = (m[0] >> 28) & 0xf;
    *ndata = m[1] & 0x3ff;
    unsigned recv_mode = (m[1] >> 10) & 0xf, special = m[1] >> 31;
    if (rq->nx > IPC_MAX_BUF || rq->na > IPC_MAX_BUF || rq->nb > IPC_MAX_BUF || rq->nw > IPC_MAX_BUF)
        rt_fatal("ipc: too many buffers (x%u a%u b%u w%u)", rq->nx, rq->na, rq->nb, rq->nw);
    const uint32_t *p = m + 2;
    if (special) {
        uint32_t h = *p++;
        rq->has_pid = h & 1;
        rq->ncopy = (h >> 1) & 0xf;
        rq->nmove = (h >> 5) & 0xf;
        if (rq->ncopy > 8 || rq->nmove > 8) rt_fatal("ipc: too many handles (copy %u move %u)", rq->ncopy, rq->nmove);
        if (rq->has_pid) {
            memcpy(&rq->pid, p, 8);
            p += 2;
        }
        for (unsigned i = 0; i < rq->ncopy; i++) rq->copy[i] = *p++;
        for (unsigned i = 0; i < rq->nmove; i++) rq->move[i] = *p++;
    }
    for (unsigned i = 0; i < rq->nx; i++, p += 2) rq->x[i] = (IpcBuffer){static_addr(p), p[0] >> 16, p[0] & 0x3f};
    IpcBuffer *lists[3] = {rq->a, rq->b, rq->w};
    unsigned counts[3] = {rq->na, rq->nb, rq->nw};
    for (unsigned k = 0; k < 3; k++)
        for (unsigned i = 0; i < counts[k]; i++, p += 3) lists[k][i] = (IpcBuffer){buffer_addr(p), buffer_size(p), i};
    *data_words = p;
    const uint32_t *recv = p + *ndata;
    rq->nc = recv_mode == 2 ? 1 : recv_mode > 2 ? recv_mode - 2 : 0;
    if (rq->nc > IPC_MAX_BUF) rt_fatal("ipc: %u receive buffers", rq->nc);
    for (unsigned i = 0; i < rq->nc; i++, recv += 2)
        rq->c[i] = (IpcBuffer){recv[0] | ((uint64_t)(recv[1] & 0xffff) << 32), recv[1] >> 16, i};
    if ((const uint8_t *)recv - (const uint8_t *)m > IPC_MSG_BYTES) rt_fatal("ipc: request exceeds the message buffer");
}

static void respond(uint32_t *m, int tipc, const IpcResponse *rs) {
    uint32_t out[IPC_MSG_BYTES / 4];
    memset(out, 0, sizeof out);
    unsigned special = rs->ncopy || rs->nmove;
    uint32_t *p = out + 2;
    if (special) {
        *p++ = (rs->ncopy << 1) | (rs->nmove << 5);
        for (unsigned i = 0; i < rs->ncopy; i++) *p++ = rs->copy[i];
        for (unsigned i = 0; i < rs->nmove; i++) *p++ = rs->move[i];
    }
    for (unsigned i = 0; i < rs->nxout; i++, p += 2) {
        uint64_t a = rs->xout[i].addr;
        p[0] = (rs->xout[i].index & 0x3f) | (uint32_t)(((a >> 36) & 0x3f) << 6) | (uint32_t)(((a >> 32) & 0xf) << 12) |
               (uint32_t)(rs->xout[i].size << 16);
        p[1] = (uint32_t)a;
    }
    uint8_t *words = (uint8_t *)p, *payload;
    unsigned header = tipc ? 4 : 16;
    payload = tipc ? words : (uint8_t *)out + (((words - (uint8_t *)out) + 15) & ~15);
    unsigned used = (unsigned)(payload - words) + header + rs->data_size;
    unsigned nwords = tipc ? (used + 3) / 4 : (used + 16 + 3) / 4;   // CMIF keeps alignment slack like libnx
    if ((size_t)(words - (uint8_t *)out) + nwords * 4 > IPC_MSG_BYTES) rt_fatal("ipc: response exceeds the message buffer");
    if (tipc) memcpy(payload, &rs->result, 4);
    else {
        uint32_t h[4] = {CMIF_OUT_MAGIC, 0, rs->result, 0};
        memcpy(payload, h, 16);
    }
    memcpy(payload + header, rs->data, rs->data_size);
    out[0] = (rs->nxout & 0xf) << 16;
    out[1] = (nwords & 0x3ff) | (special ? 1u << 31 : 0);
    memcpy(m, out, IPC_MSG_BYTES);
}

// CMIF control requests. Domains are not implemented: MNG boots 1.2.1 without them.
static void control(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 3: { uint16_t n = POINTER_BUFFER_SIZE; IPC_OUT(rs, n); return; }   // QueryPointerBufferSize
    case 2: case 4: rs->move[rs->nmove++] = session_clone(s); return;          // CloneCurrentObject(Ex)
    default:
        ipc_backtrace();
        rt_fatal("ipc: %s control command %u is not implemented (domains are not served)", s->name, rq->cmd);
    }
}

static void dispatch(IpcSession *s, uint32_t *m) {
    IpcRequest rq;
    const uint32_t *words;
    unsigned ndata;
    parse(m, &rq, &words, &ndata);
    if (rq.type == T_CLOSE || rq.type == T_TIPC_CLOSE) return;
    plat_inc64(&g_requests);
    IpcResponse rs;
    memset(&rs, 0, sizeof rs);
    int tipc = rq.type >= T_TIPC_BASE;
    const uint8_t *bytes = (const uint8_t *)words, *end = bytes + ndata * 4;
    if (tipc) {
        rq.cmd = rq.type - T_TIPC_BASE;
        rq.data = bytes;
        rq.data_size = (uint32_t)(end - bytes);
    } else if (rq.type == T_REQUEST || rq.type == T_REQUEST_CTX || rq.type == T_CONTROL || rq.type == T_CONTROL_CTX) {
        const uint8_t *payload = (const uint8_t *)m + (((bytes - (const uint8_t *)m) + 15) & ~15);
        uint32_t h[4];
        memcpy(h, payload, 16);
        if (h[0] != CMIF_IN_MAGIC) {
            ipc_backtrace();
            rt_fatal("ipc: %s: CMIF magic %#x (domain sessions are not served)", s->name, h[0]);
        }
        rq.cmd = h[2];
        rq.data = payload + 16;
        rq.data_size = payload + 16 <= end ? (uint32_t)(end - payload - 16) : 0;
    } else {
        fflush(stdout);
        rt_logf("ipc: %s message type %u:", s->name, rq.type);
        for (unsigned i = 0; i < 16; i++) rt_logf(" %08x", m[i]);
        rt_log_raw("\n", 1);
        ipc_backtrace();
        rt_fatal("ipc: %s message type %u is not implemented", s->name, rq.type);
    }
    if (rq.type == T_CONTROL || rq.type == T_CONTROL_CTX) control(s, &rq, &rs);
    else s->handler(s, &rq, &rs);
    if (rt_trace & RT_TRACE_IPC) {
        char d[160];
        rt_log("ipc: %s %s %u -> %#x (copy %u move %u) from %s", s->name,
               tipc ? "tipc" : rq.type == T_CONTROL || rq.type == T_CONTROL_CTX ? "control" : "cmd", rq.cmd,
               rs.result, rs.ncopy, rs.nmove, t_req ? ldr_describe(t_req->x[30], d, sizeof d) : "host");
    }
    respond(m, tipc, &rs);
}

// ---- sm: service manager (CMIF, and TIPC as the SDK's sm client speaks it since 12.0.0)
static void sm_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: return;   // RegisterClient / Initialize (pid)
    case 1: {         // GetServiceHandle(name)
        char name[9] = {0};
        memcpy(name, ipc_arg(rq, 0, 8), 8);
        for (unsigned i = 0; i < sizeof services / sizeof *services; i++)
            if (!strcmp(services[i].name, name)) {
                rs->move[rs->nmove++] = ipc_session_new(services[i].name, services[i].handler, NULL, NULL);
                rt_log("ipc: opened service %s", services[i].name);
                return;
            }
        fflush(stdout);
        ipc_backtrace();
        rt_fatal("sm: service \"%s\" is not implemented", name);
    }
    default: ipc_unimplemented(s, rq);
    }
}

// ---- SVCs
int ipc_svc(Cpu *c, uint32_t imm) {
    uint64_t *x = c->x;
    switch (imm) {
    case 0x1F: {   // ConnectToNamedPort(x1 name -> w1 handle)
        char name[13] = {0};
        const char *p = (const char *)(uintptr_t)x[1];
        if (!p || !readable(x[1], 1)) rt_fatal("ConnectToNamedPort: name at %#llx", (unsigned long long)x[1]);
        for (int i = 0; i < 12 && p[i]; i++) name[i] = p[i];
        if (strcmp(name, "sm:")) {
            t_req = c;
            ipc_backtrace();
            rt_fatal("ConnectToNamedPort(\"%s\"): only sm: is served", name);
        }
        x[0] = 0;
        x[1] = ipc_session_new("sm:", sm_handler, NULL, NULL);
        return 1;
    }
    case 0x21: case 0x22: {   // SendSyncRequest(w0 handle) / WithUserBuffer(x0 buffer, x1 size, w2 handle)
        uint32_t h = (uint32_t)(imm == 0x21 ? x[0] : x[2]);
        IpcSession *s = kern_handle_get(h, KERN_SESSION);
        if (!s) {
            rt_log("ipc: svc %#x on %#x, not a session handle", imm, h);
            x[0] = IPC_R_BAD_HANDLE;
            return 1;
        }
        uint32_t *m = (uint32_t *)(uintptr_t)(imm == 0x21 ? c->tpidrro : x[0]);
        if (!m || (imm == 0x22 && x[1] < IPC_MSG_BYTES))
            rt_fatal("ipc: svc %#x message buffer %p size %#llx", imm, (void *)m, (unsigned long long)x[1]);
        const Cpu *outer = t_req;
        t_req = c;
        dispatch(s, m);
        t_req = outer;
        x[0] = 0;
        return 1;
    }
    case 0x15: {   // CreateTransferMemory(x1 address, x2 size, w3 permission -> w1 handle)
        if ((x[1] & 0xfff) || x[1] + x[2] < x[1] || x[1] + x[2] > RT_ADDRESS_LIMIT) {
            x[0] = R_INVALID_ADDRESS;
            return 1;
        }
        if (!x[2] || (x[2] & 0xfff)) {
            x[0] = R_INVALID_SIZE;
            return 1;
        }
        IpcTransferMemory *t = malloc(sizeof *t);
        if (!t) rt_fatal("ipc: transfer memory allocation");
        *t = (IpcTransferMemory){x[1], x[2], (uint32_t)x[3]};
        x[0] = 0;
        x[1] = kern_handle_new(KERN_TMEM, t);
        return 1;
    }
    }
    return 0;
}

// ---- --probe-ipc
// Hand-built CMIF/TIPC requests in the guest main thread's TLR go through rt_svc, the entry the lifted SVC wrappers
// use: sm: -> audren:u -> GetWorkBufferSize -> an audio renderer with the null mixer, whose frame event the probe
// times while it sends RequestUpdate every frame. A child process checks the unknown-service fatal.
#define PROBE_FRAMES 200
#define PROBE_CHILD_ENV "TOTK_PROBE_IPC_CHILD"
#define PROBE_UNKNOWN_SERVICE "lm"
#define H_CURRENT_PROCESS 0xFFFF8001u

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

typedef struct {
    unsigned type, cmd, pid, ncopy, na, nb;
    uint32_t copy[4];
    IpcBuffer a[2], b[2];
    const void *data;
    uint32_t size;
} Msg;

typedef struct {
    uint32_t result, copy[8], move[8];
    unsigned ncopy, nmove;
    const uint8_t *data;
} Reply;

static uint32_t sv(Cpu *c, uint32_t imm, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    c->x[0] = a0;
    c->x[1] = a1;
    c->x[2] = a2;
    c->x[3] = a3;
    rt_svc(c, imm, 0);
    return (uint32_t)c->x[0];
}

static uint32_t *put_buffer(uint32_t *p, IpcBuffer b) {
    p[0] = (uint32_t)b.size;
    p[1] = (uint32_t)b.addr;
    p[2] = (uint32_t)(((b.addr >> 36) & 0x3fffff) << 2) | (uint32_t)(((b.size >> 32) & 0xf) << 24) |
           (uint32_t)(((b.addr >> 32) & 0xf) << 28);
    return p + 3;
}

static void msg_build(uint32_t *m, const Msg *q) {
    memset(m, 0, IPC_MSG_BYTES);
    int tipc = q->type >= T_TIPC_BASE, special = q->pid || q->ncopy;
    m[0] = q->type | q->na << 20 | q->nb << 24;
    uint32_t *p = m + 2;
    if (special) {
        *p++ = (q->pid ? 1u : 0u) | q->ncopy << 1;
        if (q->pid) p += 2;   // the kernel fills the process id in
        for (unsigned i = 0; i < q->ncopy; i++) *p++ = q->copy[i];
    }
    for (unsigned i = 0; i < q->na; i++) p = put_buffer(p, q->a[i]);
    for (unsigned i = 0; i < q->nb; i++) p = put_buffer(p, q->b[i]);
    uint8_t *words = (uint8_t *)p;
    unsigned ndata;
    if (tipc) {
        memcpy(words, q->data, q->size);
        ndata = (q->size + 3) / 4;
    } else {
        uint8_t *payload = (uint8_t *)m + (((words - (uint8_t *)m) + 15) & ~15);
        uint32_t h[4] = {CMIF_IN_MAGIC, 0, q->cmd, 0};
        memcpy(payload, h, 16);
        if (q->size) memcpy(payload + 16, q->data, q->size);
        ndata = (16 + 16 + q->size + 3) / 4;   // libnx reserves the 16-byte alignment pad in the data size
    }
    m[1] = ndata | (special ? 1u << 31 : 0);
}

static void msg_parse(const uint32_t *m, int tipc, Reply *r) {
    memset(r, 0, sizeof *r);
    unsigned nx = (m[0] >> 16) & 0xf;
    const uint32_t *p = m + 2;
    if (m[1] >> 31) {
        uint32_t h = *p++;
        if (h & 1) p += 2;
        r->ncopy = (h >> 1) & 0xf;
        r->nmove = (h >> 5) & 0xf;
        for (unsigned i = 0; i < r->ncopy; i++) r->copy[i] = *p++;
        for (unsigned i = 0; i < r->nmove; i++) r->move[i] = *p++;
    }
    p += 2 * nx;
    if (tipc) {
        r->result = p[0];
        r->data = (const uint8_t *)(p + 1);
        return;
    }
    const uint8_t *payload = (const uint8_t *)m + ((((const uint8_t *)p - (const uint8_t *)m) + 15) & ~15);
    uint32_t h[4];
    memcpy(h, payload, 16);
    r->result = h[0] == CMIF_OUT_MAGIC ? h[2] : 0xFFFFFFFFu;
    r->data = payload + 16;
}

// One request on the TLR message buffer; the kernel result of SendSyncRequest, the service result in r->result.
static uint32_t call(Cpu *c, uint32_t h, const Msg *q, Reply *r) {
    uint32_t *m = (uint32_t *)(uintptr_t)c->tpidrro;
    msg_build(m, q);
    uint32_t k = sv(c, 0x21, h, 0, 0, 0);
    if (k) return k;
    msg_parse(m, q->type >= T_TIPC_BASE, r);
    return 0;
}

static uint32_t cmd(Cpu *c, uint32_t h, unsigned id, const void *data, uint32_t size, Reply *r) {
    Msg q = {.type = T_REQUEST, .cmd = id, .data = data, .size = size};
    uint32_t k = call(c, h, &q, r);
    return k ? k : r->result;
}

static uint32_t close_handle(Cpu *c, uint32_t h) { return sv(c, 0x16, h, 0, 0, 0); }

static uint32_t connect_sm(Cpu *c) {
    static const char port[] = "sm:";
    uint32_t r = sv(c, 0x1F, 0, (uint64_t)(uintptr_t)port, 0, 0);
    CHECK(r == 0 && c->x[1], "ConnectToNamedPort(sm:) %#x", r);
    return (uint32_t)c->x[1];
}

static uint32_t get_service(Cpu *c, uint32_t sm, const char *name, int tipc) {
    char n[8] = {0};
    memcpy(n, name, strlen(name));
    Msg q = {.type = tipc ? T_TIPC_BASE + 1 : T_REQUEST, .cmd = 1, .data = n, .size = 8};
    Reply r;
    uint32_t k = call(c, sm, &q, &r);
    CHECK(!k && !r.result && r.nmove == 1, "%s GetServiceHandle(%s): kernel %#x result %#x move %u",
          tipc ? "TIPC" : "CMIF", name, k, r.result, r.nmove);
    return r.nmove ? r.move[0] : 0;
}

// A TotK-like renderer: 48 kHz, 240-sample (5 ms) frames, final mix plus submixes, two sinks, REV11.
static const AudrenParameter g_param = {
    .sample_rate = 48000, .sample_count = 240, .mix_buffer_count = 24, .submix_count = 8, .voice_count = 192,
    .sink_count = 2, .effect_count = 12, .revision = AUDREN_REV(11)};

typedef struct {
    uint8_t *in, *out, *perf;
    uint32_t in_size, out_total, voices_out;
    int16_t *pcm;
} Update;

static void update_build(Update *u, int first) {
    const AudrenParameter *p = &g_param;
    uint32_t npools = (uint32_t)p->effect_count + 4u * (uint32_t)p->voice_count, mc = (uint32_t)p->submix_count + 1;
    AudrenHeader h = {.revision = p->revision, .behavior = 0x10, .memory_pools = npools * sizeof(AudrenPoolIn),
                      .voices = (uint32_t)p->voice_count * sizeof(AudrenVoiceIn),
                      .effects = (uint32_t)p->effect_count * sizeof(AudrenEffectIn), .mixes = mc * sizeof(AudrenMixIn),
                      .sinks = (uint32_t)p->sink_count * sizeof(AudrenSinkIn), .performance = 0x10};
    h.total = sizeof h + h.behavior + h.memory_pools + h.voice_resources + h.voices + h.effects + h.splitters +
              h.mixes + h.sinks + h.performance + h.render_info;
    u->in_size = h.total;
    memset(u->in, 0, h.total);
    uint8_t *w = u->in;
    memcpy(w, &h, sizeof h);
    w += sizeof h + h.behavior + h.memory_pools + h.voice_resources;
    for (int i = 0; i < p->voice_count; i++) {
        AudrenVoiceIn v = {.id = (uint32_t)i};
        if (i == 0) {   // one PCM16 voice with a 100 ms buffer: consumed after 20 frames
            v = (AudrenVoiceIn){.id = 0, .is_new = (uint8_t)first, .in_use = 1, .play_state = PLAY_STARTED,
                                .sample_format = FMT_PCM16, .sample_rate = 48000, .channel_count = 1, .pitch = 1,
                                .volume = 1, .mix_id = 0, .splitter_id = 0xffffffffu, .wave_buffer_count = 1};
            v.wave_buffers[0] = (AudrenWaveBuffer){.address = (uint64_t)(uintptr_t)u->pcm, .size = 4800 * 2,
                                                   .end = 4800, .sent = (uint8_t)!first};
            for (int k = 1; k < 4; k++) v.wave_buffers[k].sent = 1;
        }
        memcpy(w, &v, sizeof v);
        w += sizeof v;
    }
    w += h.effects + h.splitters;
    AudrenMixIn mix = {.volume = 1, .sample_rate = 48000, .buffer_count = 6, .in_use = 1, .dest_mix_id = 0x7fffffff,
                       .dest_splitter_id = 0xffffffffu};
    memcpy(w, &mix, sizeof mix);
    w += h.mixes;
    AudrenSinkIn sink = {.type = SINK_DEVICE, .in_use = 1, .input_count = 2, .inputs = {0, 1}};
    memcpy(w, &sink, sizeof sink);
    u->out_total = (uint32_t)sizeof h + npools * sizeof(AudrenPoolOut) + (uint32_t)p->voice_count * sizeof(AudrenVoiceOut) +
                   (uint32_t)p->effect_count * 0x90u + (uint32_t)p->sink_count * sizeof(AudrenSinkOut) + 0x10 +
                   AUDREN_BEHAVIOR_OUT + AUDREN_RENDER_INFO_OUT;
    u->voices_out = (uint32_t)sizeof h + npools * sizeof(AudrenPoolOut);
}

static uint32_t request_update(Cpu *c, uint32_t ren, Update *u, uint64_t *frames, AudrenVoiceOut *voice0) {
    Msg q = {.type = T_REQUEST, .cmd = 4, .na = 1, .nb = 2};
    q.a[0] = (IpcBuffer){(uint64_t)(uintptr_t)u->in, u->in_size, 0};
    q.b[0] = (IpcBuffer){(uint64_t)(uintptr_t)u->out, 0x8000, 0};
    q.b[1] = (IpcBuffer){(uint64_t)(uintptr_t)u->perf, 0x100, 1};
    Reply r;
    uint32_t k = call(c, ren, &q, &r);
    if (k || r.result) return k ? k : r.result;
    AudrenHeader o;
    memcpy(&o, u->out, sizeof o);
    if (o.revision != g_param.revision || o.total != u->out_total) return 0xFFFFFFFFu;
    memcpy(frames, u->out + o.total - AUDREN_RENDER_INFO_OUT, 8);
    memcpy(voice0, u->out + u->voices_out, sizeof *voice0);
    return 0;
}

static void probe_renderer(Cpu *c, uint32_t audren) {
    Reply r;
    // GetWorkBufferSize(AudioRendererParameterInternal) -> u64
    uint32_t res = cmd(c, audren, 1, &g_param, sizeof g_param, &r);
    uint64_t work = 0;
    if (!res) memcpy(&work, r.data, 8);
    printf("audren:u GetWorkBufferSize(48000 Hz x240, %d voices, %d submixes, %d effects, REV11) -> result %#x, "
           "%#llx bytes\n", g_param.voice_count, g_param.submix_count, g_param.effect_count, res,
           (unsigned long long)work);
    CHECK(!res && work >= 0x1000 && work < (256u << 20) && !(work & 0xfff), "GetWorkBufferSize");
    if (res || !work) return;

    // OpenAudioRenderer(param, pad, work size, aruid; pid; copy transfer memory, current process)
    uint8_t *wb = mem_kernel_alloc(work, 0x1000);
    uint32_t tm = 0;
    res = sv(c, 0x15, 0, (uint64_t)(uintptr_t)wb, work, 0);
    tm = (uint32_t)c->x[1];
    CHECK(!res, "CreateTransferMemory %#x", res);
    struct { AudrenParameter p; uint32_t pad; uint64_t work, aruid; } open = {g_param, 0, work, 0};
    Msg q = {.type = T_REQUEST, .cmd = 0, .pid = 1, .ncopy = 2, .copy = {tm, H_CURRENT_PROCESS}, .data = &open,
             .size = sizeof open};
    long live0 = ipc_sessions_live();
    uint64_t released0 = ipc_sessions_released();
    uint32_t k = call(c, audren, &q, &r);
    CHECK(!k && !r.result && r.nmove == 1 && audren_renderers_live() == 1, "OpenAudioRenderer: kernel %#x result %#x",
          k, r.result);
    if (k || r.result || !r.nmove) return;
    uint32_t ren = r.move[0];
    int32_t rate = 0, count = 0;
    if (!cmd(c, ren, 0, NULL, 0, &r)) memcpy(&rate, r.data, 4);
    if (!cmd(c, ren, 1, NULL, 0, &r)) memcpy(&count, r.data, 4);
    CHECK(rate == 48000 && count == 240, "GetSampleRate %d, GetSampleCount %d", rate, count);
    res = cmd(c, ren, 7, NULL, 0, &r);   // QuerySystemEvent
    uint32_t ev = !res && r.ncopy == 1 ? r.copy[0] : 0;
    CHECK(ev, "QuerySystemEvent: result %#x, %u copy handles", res, r.ncopy);

    Update u = {mem_kernel_alloc(0x40000, 64), mem_kernel_alloc(0x8000, 64), mem_kernel_alloc(0x100, 64), 0, 0, 0,
                mem_kernel_alloc(4800 * 2, 64)};
    for (int i = 0; i < 4800; i++) u.pcm[i] = (int16_t)(i * 7);
    uint64_t frames = ~0ull;
    AudrenVoiceOut v0 = {0};
    update_build(&u, 1);
    res = request_update(c, ren, &u, &frames, &v0);
    CHECK(!res && frames == 0, "RequestUpdate before Start: %#x, %llu frames", res, (unsigned long long)frames);
    update_build(&u, 0);
    CHECK(!cmd(c, ren, 5, NULL, 0, &r), "Start");
    uint32_t state = 1;
    if (!cmd(c, ren, 3, NULL, 0, &r)) memcpy(&state, r.data, 4);
    CHECK(state == 0, "GetState after Start: %u", state);

    // Every frame, as the game's audio thread does: wait for the event, clear it, RequestUpdate.
    uint32_t hs[1] = {ev};
    sv(c, 0x17, ev, 0, 0, 0);
    uint64_t t0 = 0, t1 = 0, f0 = 0, f1 = 0, worst = 0, prev = 0;
    int waits_ok = 0, updates_ok = 0;
    for (int i = 0; i < PROBE_FRAMES; i++) {
        uint32_t w = sv(c, 0x18, 0, (uint64_t)(uintptr_t)hs, 1, 1000000000ull);
        uint64_t now = kern_now_ns();
        waits_ok += w == 0;
        sv(c, 0x17, ev, 0, 0, 0);
        uint64_t f = 0;
        updates_ok += request_update(c, ren, &u, &f, &v0) == 0;
        if (i == 0) t0 = now, f0 = f;
        if (i && now - prev > worst) worst = now - prev;
        prev = t1 = now;
        f1 = f;
    }
    double period = (double)(t1 - t0) / (PROBE_FRAMES - 1) / 1e6, expect = 1e3 * g_param.sample_count / g_param.sample_rate;
    double rendered = (double)(f1 - f0), elapsed = (double)(t1 - t0) / 1e6 / expect;
    printf("render tick: %d waits on the frame event (%d ok), %d RequestUpdate (%d ok); mean period %.3f ms (expect "
           "%.3f), worst gap %.3f ms; frames rendered %llu over %.1f periods\n", PROBE_FRAMES, waits_ok, PROBE_FRAMES,
           updates_ok, period, expect, (double)worst / 1e6, (unsigned long long)(f1 - f0), elapsed);
    CHECK(waits_ok == PROBE_FRAMES && updates_ok == PROBE_FRAMES, "event waits / updates");
    CHECK(period > expect * 0.9 && period < expect * 1.1, "frame event period %.3f ms", period);
    CHECK(rendered > elapsed - 3 && rendered < elapsed + 3, "frames rendered %.0f vs elapsed %.1f", rendered, elapsed);
    printf("voice 0 (PCM16, 4800 samples): played %llu, wave buffers consumed %u\n",
           (unsigned long long)v0.played_samples, v0.wave_buffers_consumed);
    CHECK(v0.played_samples == 4800 && v0.wave_buffers_consumed == 1, "voice clock with the null mixer");

    CHECK(!cmd(c, ren, 6, NULL, 0, &r), "Stop");
    if (!cmd(c, ren, 3, NULL, 0, &r)) memcpy(&state, r.data, 4);
    CHECK(state == 1, "GetState after Stop: %u", state);
    res = close_handle(c, ren);
    long live1 = ipc_sessions_live();
    uint64_t released1 = ipc_sessions_released();
    printf("CloseHandle(IAudioRenderer) -> %#x: sessions %ld -> %ld, released +%llu, renderers running %ld\n", res,
           live0 + 1, live1, (unsigned long long)(released1 - released0), audren_renderers_live());
    CHECK(!res && live1 == live0 && released1 == released0 + 1 && audren_renderers_live() == 0, "renderer close");
    CHECK(!close_handle(c, ev) && !close_handle(c, tm), "close event and transfer memory");
    mem_kernel_free(u.in, 0x40000);
    mem_kernel_free(u.out, 0x8000);
    mem_kernel_free(u.perf, 0x100);
    mem_kernel_free(u.pcm, 4800 * 2);
    mem_kernel_free(wb, work);
}

static void probe_main(Cpu *c) {
    Reply r;
    uint32_t sm = connect_sm(c);
    Msg reg = {.type = T_REQUEST, .cmd = 0, .pid = 1, .data = (const uint64_t[]){0}, .size = 8};
    uint32_t k = call(c, sm, &reg, &r);
    CHECK(!k && !r.result, "sm: RegisterClient: kernel %#x result %#x", k, r.result);
    uint32_t audren = get_service(c, sm, "audren:u", 0);
    uint32_t dup = get_service(c, sm, "audren:u", 1);
    printf("sm: connected, RegisterClient ok, GetServiceHandle(audren:u) -> %#x (CMIF), %#x (TIPC)\n", audren, dup);
    uint64_t released0 = ipc_sessions_released();
    CHECK(!close_handle(c, dup) && ipc_sessions_released() == released0 + 1, "CloseHandle(TIPC audren:u)");
    Msg qpb = {.type = T_CONTROL, .cmd = 3};
    k = call(c, audren, &qpb, &r);
    uint16_t pbs = 0;
    if (!k && !r.result) memcpy(&pbs, r.data, 2);
    CHECK(pbs == POINTER_BUFFER_SIZE, "QueryPointerBufferSize %#x", pbs);
    CHECK(sv(c, 0x21, 0x0fff, 0, 0, 0) == IPC_R_BAD_HANDLE, "SendSyncRequest on a bad handle");
    if (audren) probe_renderer(c, audren);
    CHECK(!close_handle(c, audren) && !close_handle(c, sm), "close audren:u and sm:");
    printf("sessions alive at the end: %ld, released %llu\n", ipc_sessions_live(),
           (unsigned long long)ipc_sessions_released());
    CHECK(ipc_sessions_live() == 0, "sessions leaked");
}

static void child_main(Cpu *c) {
    uint32_t sm = connect_sm(c);
    get_service(c, sm, PROBE_UNKNOWN_SERVICE, 0);
    printf("child: GetServiceHandle(%s) returned\n", PROBE_UNKNOWN_SERVICE);
}

// Re-run this executable with the same arguments and PROBE_CHILD_ENV set; it must stop with the named fatal.
static void probe_unknown_service(void) {
    static char out[65536];
    int code = plat_run_self(PROBE_CHILD_ENV, "1", out, sizeof out, 30000);
    if (code < 0) rt_fatal("probe-ipc: cannot start the child");
    const char *want = "FATAL: sm: service \"" PROBE_UNKNOWN_SERVICE "\" is not implemented";
    const char *line = strstr(out, want);
    printf("child GetServiceHandle(%s): exit code %d, %s\n", PROBE_UNKNOWN_SERVICE, code,
           line ? "named fatal seen" : "named fatal MISSING");
    if (line) printf("  child: %.*s\n", (int)strcspn(line, "\r\n"), line);
    CHECK(code == 70 && line, "unknown service fatal (child output: %s)", out);
}

// ---- the game's own nn::audio client: main's PLT stubs -> lifted sdk -> SVCs -> this file
#define SYM_INIT_PARAM "_ZN2nn5audio32InitializeAudioRendererParameterEPNS0_22AudioRendererParameterE"
#define SYM_WORK_SIZE "_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE"
#define SYM_OPEN "_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm"
#define SYM_CONFIG_SIZE "_ZN2nn5audio36GetAudioRendererConfigWorkBufferSizeERKNS0_22AudioRendererParameterE"
#define SYM_CONFIG_INIT "_ZN2nn5audio29InitializeAudioRendererConfigEPNS0_19AudioRendererConfigERKNS0_22AudioRendererParameterEPvm"
#define SYM_START "_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE"
#define SYM_STOP "_ZN2nn5audio17StopAudioRendererENS0_19AudioRendererHandleE"
#define SYM_UPDATE "_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE"
#define SYM_FRAMES "_ZN2nn5audio33GetAudioRendererElapsedFrameCountEPKNS0_19AudioRendererConfigE"
#define SYM_CLOSE "_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE"
#define SYM_WAIT_EVENT "_ZN2nn2os15WaitSystemEventEPNS0_15SystemEventTypeE"

static int revision_number_of(uint32_t rev) { return (int)(rev >> 24) - '0'; }

static uint64_t stub(const char *sym) {
    uint64_t a = ldr_plt_stub("main", sym);
    if (!a) rt_fatal("probe-ipc: no PLT stub for %s in main", sym);
    return a;
}

// Without nn::init::Start the SDK's own initialisation has not run; the probe sets up the two things the audio client
// path reads (found by running it):
// - the "current thread object" at TLR+0x1F8 is the kernel's zeroed placeholder. SdkMutex compares its lock tag with
//   ThreadType+0x1B0 (sdk+0x13186c), which nnosInitialize sets to the thread's handle; a zero there makes every free
//   mutex look owned by the caller (__cxa_guard_acquire in nn::sm::Initialize aborts);
// - sdk's full DT_INIT needs nn::os initialised (its TLS slot constructor reaches ThreadManager), which is the boot
//   sequence's job. The audio client only needs its two sf heap constructors: 16 KiB ExpHeaps on static buffers,
//   stored at sdk+0xd37e90 and sdk+0xd3bec0 (CreateAudioRendererManagerByHipc reads the second). The 1.4.3 SDK creates
//   that heap lazily; its OpenAudioRenderer takes nn::audio::detail::g_RendererResourceSemaphore first, which the
//   constructor at sdk+0x221fb0 initializes (two renderers).
static void sdk_prepare(Cpu *c) {
    Cpu *again;
    ST32(LD64(c->tpidrro + 0x1F8) + 0x1B0, kern_main_thread(&again));
    const RtLoaded *sdk = ldr_module(2);
    static const uint64_t ctors_121[] = {0x1dcbd0, 0x1de8a0}, ctors_143[] = {0x221fb0};
    const uint64_t *heap_ctors = RT_V143 ? ctors_143 : ctors_121;
    unsigned nctors = RT_V143 ? sizeof ctors_143 / sizeof *ctors_143 : sizeof ctors_121 / sizeof *ctors_121;
    for (unsigned k = 0; k < nctors; k++) {
        int found = 0;
        for (uint64_t e = 0; e < sdk->init_array_size; e += 8)
            found |= LD64(sdk->init_array + e) == sdk->base + heap_ctors[k];
        if (!found) rt_fatal("probe-ipc: sdk+%#llx is not an sdk constructor", (unsigned long long)heap_ctors[k]);
        rt_call(c, sdk->base + heap_ctors[k], 0);
    }
}

static void probe_sdk_client(Cpu *c) {
    unsigned trace = rt_trace;
    rt_trace |= RT_TRACE_IPC;   // log the requests the SDK sends during set-up
    sdk_prepare(c);
    uint64_t requests0 = (uint64_t)plat_cas64(&g_requests, 0, 0);
    uint64_t released0 = ipc_sessions_released();
    int32_t *param = mem_kernel_alloc(0x100, 64);
    rt_call(c, stub(SYM_INIT_PARAM), 1, (uint64_t)(uintptr_t)param);
    printf("sdk: InitializeAudioRendererParameter -> %d Hz x%d, mix buffers %d, revision word %#x (REV%d)\n", param[0],
           param[1], param[2], (uint32_t)param[13], revision_number_of((uint32_t)param[13]));
    CHECK(param[0] == 48000 && param[1] == 240 && (uint32_t)param[13] == AUDREN_REV(RT_V143 ? 15 : 11),
          "SDK default parameter (TotK 1.2.1 builds REV11 requests, 1.4.3 REV15)");
    param[2] = g_param.mix_buffer_count, param[3] = g_param.submix_count, param[4] = g_param.voice_count;
    param[5] = g_param.sink_count, param[6] = g_param.effect_count;
    uint64_t work = rt_call(c, stub(SYM_WORK_SIZE), 1, (uint64_t)(uintptr_t)param);
    printf("sdk: GetAudioRendererWorkBufferSize -> %#llx (service %#x + client part)\n", (unsigned long long)work,
           AUDREN_WORK_BUFFER_BYTES);
    CHECK(work >= AUDREN_WORK_BUFFER_BYTES && work < (64u << 20), "SDK GetAudioRendererWorkBufferSize %#llx",
          (unsigned long long)work);
    uint8_t *wb = mem_kernel_alloc(work, 0x1000), *event = mem_kernel_alloc(0x100, 64);
    uint64_t *handle = mem_kernel_alloc(0x40, 64);   // AudioRendererHandle: 16 bytes, passed in x0 and x1
    uint32_t r = (uint32_t)rt_call(c, stub(SYM_OPEN), 5, (uint64_t)(uintptr_t)handle, (uint64_t)(uintptr_t)event,
                                   (uint64_t)(uintptr_t)param, (uint64_t)(uintptr_t)wb, work);
    printf("sdk: OpenAudioRenderer -> %#x, renderers running %ld\n", r, audren_renderers_live());
    CHECK(!r && handle[0] && audren_renderers_live() == 1, "SDK OpenAudioRenderer %#x", r);
    if (r) return;
    uint64_t csize = rt_call(c, stub(SYM_CONFIG_SIZE), 1, (uint64_t)(uintptr_t)param);
    uint8_t *cbuf = mem_kernel_alloc(csize, 0x1000), *config = mem_kernel_alloc(0x200, 64);
    rt_call(c, stub(SYM_CONFIG_INIT), 4, (uint64_t)(uintptr_t)config, (uint64_t)(uintptr_t)param,
            (uint64_t)(uintptr_t)cbuf, csize);
    rt_call(c, stub(SYM_START), 2, handle[0], handle[1]);
    uint64_t t0 = 0, t1 = 0;
    int bad = 0, f0 = 0, f1 = 0;
    for (int i = 0; i < PROBE_FRAMES; i++) {
        rt_call(c, stub(SYM_WAIT_EVENT), 1, (uint64_t)(uintptr_t)event);
        uint64_t now = kern_now_ns();
        bad += rt_call(c, stub(SYM_UPDATE), 3, handle[0], handle[1], (uint64_t)(uintptr_t)config) != 0;
        int f = (int)rt_call(c, stub(SYM_FRAMES), 1, (uint64_t)(uintptr_t)config);
        if (!i) t0 = now, f0 = f;
        t1 = now, f1 = f;
        rt_trace = trace;   // the first update is logged, the rest is quiet
    }
    double period = (double)(t1 - t0) / (PROBE_FRAMES - 1) / 1e6;
    printf("sdk: %d x WaitSystemEvent + RequestUpdateAudioRenderer: %d failed, mean period %.3f ms, elapsed frames "
           "%d -> %d\n", PROBE_FRAMES, bad, period, f0, f1);
    CHECK(!bad && period > 4.5 && period < 5.5 && f1 - f0 >= PROBE_FRAMES - 4 && f1 - f0 <= PROBE_FRAMES + 2,
          "SDK render loop");
    rt_call(c, stub(SYM_STOP), 2, handle[0], handle[1]);
    rt_call(c, stub(SYM_CLOSE), 2, handle[0], handle[1]);
    uint64_t requests = (uint64_t)plat_cas64(&g_requests, 0, 0) - requests0;
    printf("sdk: CloseAudioRenderer: renderers running %ld, sessions released +%llu; %llu requests served for the "
           "lifted client (sm: kept open by the SDK: %ld session)\n", audren_renderers_live(),
           (unsigned long long)(ipc_sessions_released() - released0), (unsigned long long)requests,
           ipc_sessions_live());
    // The 1.2.1 SDK closes the renderer, device and manager sessions and keeps sm; the 1.4.3 SDK keeps the
    // audren:u manager it created lazily, beside sm.
    CHECK(audren_renderers_live() == 0 && ipc_sessions_released() - released0 >= (RT_V143 ? 1u : 3u) &&
              ipc_sessions_live() == (RT_V143 ? 2 : 1),
          "SDK CloseAudioRenderer");
    rt_trace = trace;
}

static void guest_main(Cpu *c) {
    probe_main(c);
    probe_sdk_client(c);
}

int probe_ipc(void) {
    if (getenv(PROBE_CHILD_ENV)) {
        kern_run_on_guest_main(child_main);
        return 0;
    }
    ldr_load(rt_config()->exefs);
    kern_run_on_guest_main(guest_main);
    probe_unknown_service();
    RtStats s = rt_stats();
    printf("stats: %llu SVCs, %llu threads, %llu host calls\n", (unsigned long long)s.svcs,
           (unsigned long long)s.threads, (unsigned long long)s.host_calls);
    if (failures) {
        printf("probe-ipc: FAILED (%d failures)\n", failures);
        return 1;
    }
    printf("probe-ipc: passed\n");
    return 0;
}
