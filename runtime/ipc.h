// Adapted from MacNativeGames games/totk/runtime/ipc.h @ b75be8c (MIT, Copyright (c) 2026 MacNativeGames
// contributors). Message framing per libnx sf/hipc.h, cmif.h, tipc.h (ISC), read for the layout only.
// In-process Horizon IPC: HIPC messages with CMIF or TIPC payloads, answered in
// place in the caller's message buffer. Services are host implementations looked up by name through sm:.
#pragma once
#include <stdint.h>

#include "host.h"

#define IPC_MAX_BUF 8
#define IPC_MSG_BYTES 0x100
#define IPC_R_BAD_HANDLE 0xE401u           // kernel: invalid handle
#define IPC_R_UNKNOWN_COMMAND 0xF601u      // sf: unknown command (module 10, description 123)

typedef struct { uint64_t addr, size; unsigned index; } IpcBuffer;

typedef struct {
    unsigned type, cmd;                    // HIPC type (4/5/6/7 CMIF, >= 16 TIPC) and command id
    int has_pid;
    uint64_t pid;
    uint32_t copy[8], move[8];
    unsigned ncopy, nmove;
    IpcBuffer x[IPC_MAX_BUF], a[IPC_MAX_BUF], b[IPC_MAX_BUF], w[IPC_MAX_BUF], c[IPC_MAX_BUF];
    unsigned nx, na, nb, nw, nc;
    const uint8_t *data;                   // command arguments after the CMIF header (TIPC: after the HIPC header)
    uint32_t data_size;
} IpcRequest;

typedef struct {
    uint32_t result;
    uint8_t data[0x100];
    uint32_t data_size;
    uint32_t copy[8], move[8];
    unsigned ncopy, nmove;
    IpcBuffer xout[IPC_MAX_BUF];           // pointer data the service placed into the client's C buffers
    unsigned nxout;
} IpcResponse;

typedef struct IpcSession IpcSession;
typedef void (*IpcHandler)(IpcSession *s, const IpcRequest *rq, IpcResponse *rs);

// One server object. Every kernel handle to it (the first one, CloneCurrentObject copies) holds a reference; the
// last CloseHandle calls close(state).
struct IpcSession {
    const char *name;
    IpcHandler handler;
    void *state;
    void (*close)(void *state);
    int32_t refs;
};

uint32_t ipc_session_new(const char *name, IpcHandler handler, void *state, void (*close)(void *state));   // handle
void ipc_out(IpcResponse *rs, const void *p, uint32_t n);
const void *ipc_arg(const IpcRequest *rq, uint32_t off, uint32_t n);
void *ipc_guest(uint64_t addr, uint64_t size);   // a client buffer as a host pointer (identity map), range checked
__attribute__((noreturn)) void ipc_unimplemented(const IpcSession *s, const IpcRequest *rq);
#define IPC_OUT(rs, v) ipc_out((rs), &(v), sizeof(v))
#define IPC_ARG(rq, T, off) (*(const T *)ipc_arg((rq), (off), sizeof(T)))

// Transfer memory from svcCreateTransferMemory (0x15), a KERN_TMEM handle object.
typedef struct { uint64_t addr, size; uint32_t perm; } IpcTransferMemory;

// Server objects alive now and closed so far (the probe checks CloseHandle releases them).
long ipc_sessions_live(void);
uint64_t ipc_sessions_released(void);

// The guest frame chain of the request being served, for fatal reports from services.
void ipc_backtrace(void);
