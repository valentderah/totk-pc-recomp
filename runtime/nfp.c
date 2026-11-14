// Horizon OS nfp:user IPC service implementation (nn::nfp::detail::IUserManager & IUser).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amiibo.h"
#include "host.h"
#include "ipc.h"
#include "kernel.h"
#include "nfp.h"
#include "platform/platform.h"

// Simulated NFC device handle: ID 1
static const uint8_t g_device_handle[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };

typedef struct NfpUserState {
    struct NfpUserState *next;
    uint32_t activate_event;    // writable handle
    uint32_t deactivate_event;  // writable handle
    uint32_t avail_event;       // writable handle
} NfpUserState;

static PlatMutex event_lock = PLAT_MUTEX_INIT;
static NfpUserState *users;
static uint64_t system_events[2];

void nfp_set_system_event(bool activate, uint64_t event) {
    plat_mutex_lock(&event_lock);
    system_events[activate ? 1 : 0] = event;
    plat_mutex_unlock(&event_lock);
}

void nfp_notify_tag(bool present) {
    plat_mutex_lock(&event_lock);
    if (system_events[present ? 1 : 0]) kern_try_signal_system_event(system_events[present ? 1 : 0]);
    for (NfpUserState *st = users; st; st = st->next)
        kern_event_signal(present ? st->activate_event : st->deactivate_event);
    plat_mutex_unlock(&event_lock);
}

static void nfp_user_close(void *p) {
    NfpUserState *st = p;
    if (st) {
        plat_mutex_lock(&event_lock);
        NfpUserState **link = &users;
        while (*link && *link != st) link = &(*link)->next;
        if (*link) *link = st->next;
        if (st->activate_event) kern_handle_close(st->activate_event);
        if (st->deactivate_event) kern_handle_close(st->deactivate_event);
        if (st->avail_event) kern_handle_close(st->avail_event);
        plat_mutex_unlock(&event_lock);
        free(st);
    }
    rt_log("nfp: IUser session closed");
}

// Fixed-size NFP records use receive pointers; accept mapped buffers as well.
static void *nfp_output(const IpcRequest *rq, IpcResponse *rs, uint64_t size) {
    bool pointer = !(rq->nb && rq->b[0].size);
    IpcBuffer b = pointer ? (rq->nc ? rq->c[0] : (IpcBuffer){0}) : rq->b[0];
    if (!b.addr || b.size < size) {
        rs->result = 0xCC01u; // Invalid address/buffer.
        return NULL;
    }
    if (pointer) rs->xout[rs->nxout++] = (IpcBuffer){b.addr, size, b.index};
    return ipc_guest(b.addr, size);
}

// Handler for nn::nfp::detail::IUser
static void nfp_user_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    NfpUserState *st = (NfpUserState *)s->state;

    switch (rq->cmd) {
    case 0: {   // InitializeNfp(u64 aruid, u64 m_pid, pid, buffer<unknown, 5>)
        rt_log("nfp: Initialize");
        rs->result = 0;
        return;
    }
    case 1: {   // FinalizeNfp()
        rt_log("nfp: Finalize");
        amiibo_stop_detection();
        amiibo_unmount();
        rs->result = 0;
        return;
    }
    case 2: {   // ListDevicesNfp() -> (s32 total_out, buffer<DeviceHandle, 0xa>)
        int32_t count = amiibo_is_device_present() ? 1 : 0;
        IPC_OUT(rs, count);
        if (count > 0) {
            void *dst = nfp_output(rq, rs, sizeof(g_device_handle));
            if (!dst) return;
            memcpy(dst, g_device_handle, sizeof(g_device_handle));
        }
        rs->result = 0;
        return;
    }
    case 3: {   // StartDetectionNfp(DeviceHandle)
        amiibo_start_detection();
        rs->result = 0;
        return;
    }
    case 4: {   // StopDetectionNfp(DeviceHandle)
        amiibo_stop_detection();
        rs->result = 0;
        return;
    }
    case 5: {   // MountNfp(DeviceHandle, u32 ModelType, u32 MountTarget)
        if (!amiibo_has_tag()) {
            rs->result = 0x8073u;   // Device/Tag not found
            return;
        }
        rs->result = amiibo_mount() ? 0 : 0x8073u;
        return;
    }
    case 6: {   // UnmountNfp(DeviceHandle)
        amiibo_unmount();
        if (st && st->deactivate_event) {
            kern_event_signal(st->deactivate_event);
        }
        rs->result = 0;
        return;
    }
    case 7: {   // OpenApplicationAreaNfp(DeviceHandle, u32 access_id)
        if (!amiibo_is_mounted()) {
            rs->result = 0x927Fu;   // not mounted
            return;
        }
        rs->result = 0;
        return;
    }
    case 8: {   // GetApplicationAreaNfp(DeviceHandle) -> (u32 size, buffer<data, 0x6>)
        if (!amiibo_is_mounted()) {
            rs->result = 0x927Fu;
            return;
        }
        uint32_t area_size = 0xD8;
        if (rq->nb > 0 && rq->b[0].size > 0) {
            void *dst = ipc_guest(rq->b[0].addr, rq->b[0].size);
            area_size = (uint32_t)amiibo_get_application_area(dst, (uint32_t)rq->b[0].size);
        }
        IPC_OUT(rs, area_size);
        rs->result = 0;
        return;
    }
    case 13: {  // GetTagInfoNfp(DeviceHandle) -> buffer<TagInfo, 0x1a>
        if (!amiibo_has_tag() && !amiibo_is_mounted()) {
            rs->result = 0x8073u;
            return;
        }
        void *dst = nfp_output(rq, rs, 0x58);
        if (!dst) return;
        amiibo_get_tag_info(dst);
        rs->result = 0;
        return;
    }
    case 14: {  // GetRegisterInfoNfp(DeviceHandle) -> buffer<RegisterInfo, 0x1a>
        if (!amiibo_is_mounted()) {
            rs->result = 0x927Fu;
            return;
        }
        void *dst = nfp_output(rq, rs, 0x100);
        if (!dst) return;
        amiibo_get_register_info(dst);
        rs->result = 0;
        return;
    }
    case 15: {  // GetCommonInfoNfp(DeviceHandle) -> buffer<CommonInfo, 0x1a>
        if (!amiibo_is_mounted()) {
            rs->result = 0x927Fu;
            return;
        }
        void *dst = nfp_output(rq, rs, 0x40);
        if (!dst) return;
        amiibo_get_common_info(dst);
        rs->result = 0;
        return;
    }
    case 16: {  // GetModelInfoNfp(DeviceHandle) -> buffer<ModelInfo, 0x1a>
        if (!amiibo_is_mounted()) {
            rs->result = 0x927Fu;
            return;
        }
        void *dst = nfp_output(rq, rs, 0x40);
        if (!dst) return;
        amiibo_get_model_info(dst);
        rs->result = 0;
        return;
    }
    case 17: {  // GetActivateEventHandleNfp(DeviceHandle) -> handle<copy>
        if (st && st->activate_event) {
            rs->copy[rs->ncopy++] = kern_event_readable(st->activate_event);
        }
        rs->result = 0;
        return;
    }
    case 18: {  // GetDeactivateEventHandleNfp(DeviceHandle) -> handle<copy>
        if (st && st->deactivate_event) {
            rs->copy[rs->ncopy++] = kern_event_readable(st->deactivate_event);
        }
        rs->result = 0;
        return;
    }
    case 19: {  // GetStateNfp() -> u32 state
        uint32_t state = 1; // Initialized
        IPC_OUT(rs, state);
        rs->result = 0;
        return;
    }
    case 20: {  // GetDeviceStateNfp(DeviceHandle) -> u32 device_state
        uint32_t dev_state = (uint32_t)amiibo_get_device_state();
        IPC_OUT(rs, dev_state);
        rs->result = 0;
        return;
    }
    case 21: {  // GetNpadIdNfp(DeviceHandle) -> u32 npad_id
        uint32_t npad_id = 0; // Handheld / Player 1
        IPC_OUT(rs, npad_id);
        rs->result = 0;
        return;
    }
    case 22: {  // GetApplicationAreaSizeNfp(DeviceHandle) -> u32 size
        uint32_t size = 0xD8;
        IPC_OUT(rs, size);
        rs->result = 0;
        return;
    }
    case 23: {  // GetAvailabilityChangeEventHandleNfp() -> handle<copy>
        if (st && st->avail_event) {
            rs->copy[rs->ncopy++] = kern_event_readable(st->avail_event);
        }
        rs->result = 0;
        return;
    }
    default:
        // Optional NFC operations must report a failure to the SDK, never abort the host.
        rs->result = IPC_R_UNKNOWN_COMMAND;
        return;
    }
}

// Handler for nn::nfp::detail::IUserManager (nfp:user)
void nfp_user_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: {   // CreateUserInterface() -> move handle to IUser
        NfpUserState *st = calloc(1, sizeof(*st));
        if (!st) rt_fatal("nfp: failed to allocate user state");
        st->activate_event = kern_event_new();
        st->deactivate_event = kern_event_new();
        st->avail_event = kern_event_new();
        plat_mutex_lock(&event_lock);
        st->next = users;
        users = st;
        plat_mutex_unlock(&event_lock);
        // The reader is already present in ListDevices; no availability change has occurred.

        uint32_t user_session = ipc_session_new("nfp:IUser", nfp_user_handler, st, nfp_user_close);
        rs->move[rs->nmove++] = user_session;
        rs->result = 0;
        rt_log("nfp: created IUser session handle %#x", user_session);
        return;
    }
    default:
        ipc_unimplemented(s, rq);
    }
}
