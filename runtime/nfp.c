// Horizon OS nfp:user IPC service implementation (nn::nfp::detail::IUserManager & IUser).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amiibo.h"
#include "host.h"
#include "ipc.h"
#include "kernel.h"
#include "nfp.h"

// Simulated NFC device handle: ID 1
static const uint8_t g_device_handle[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };

typedef struct {
    uint32_t activate_event;    // writable handle
    uint32_t deactivate_event;  // writable handle
    uint32_t avail_event;       // writable handle
} NfpUserState;

static void nfp_user_close(void *p) {
    NfpUserState *st = p;
    if (st) {
        if (st->activate_event) kern_handle_close(st->activate_event);
        if (st->deactivate_event) kern_handle_close(st->deactivate_event);
        if (st->avail_event) kern_handle_close(st->avail_event);
        free(st);
    }
    rt_log("nfp: IUser session closed");
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
        if (count > 0 && rq->nb > 0 && rq->b[0].size >= sizeof(g_device_handle)) {
            void *dst = ipc_guest(rq->b[0].addr, sizeof(g_device_handle));
            memcpy(dst, g_device_handle, sizeof(g_device_handle));
        }
        rs->result = 0;
        return;
    }
    case 3: {   // StartDetectionNfp(DeviceHandle)
        amiibo_start_detection();
        if (amiibo_has_tag() && st && st->activate_event) {
            kern_event_signal(st->activate_event);
        }
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
        amiibo_mount();
        rs->result = 0;
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
        if (rq->nb > 0 && rq->b[0].size >= 0x58) {
            void *dst = ipc_guest(rq->b[0].addr, 0x58);
            amiibo_get_tag_info(dst);
        }
        rs->result = 0;
        return;
    }
    case 14: {  // GetRegisterInfoNfp(DeviceHandle) -> buffer<RegisterInfo, 0x1a>
        if (!amiibo_is_mounted()) {
            rs->result = 0x927Fu;
            return;
        }
        if (rq->nb > 0 && rq->b[0].size >= 0x100) {
            void *dst = ipc_guest(rq->b[0].addr, 0x100);
            amiibo_get_register_info(dst);
        }
        rs->result = 0;
        return;
    }
    case 15: {  // GetCommonInfoNfp(DeviceHandle) -> buffer<CommonInfo, 0x1a>
        if (!amiibo_is_mounted()) {
            rs->result = 0x927Fu;
            return;
        }
        if (rq->nb > 0 && rq->b[0].size >= 0x40) {
            void *dst = ipc_guest(rq->b[0].addr, 0x40);
            amiibo_get_common_info(dst);
        }
        rs->result = 0;
        return;
    }
    case 16: {  // GetModelInfoNfp(DeviceHandle) -> buffer<ModelInfo, 0x1a>
        if (!amiibo_is_mounted()) {
            rs->result = 0x927Fu;
            return;
        }
        if (rq->nb > 0 && rq->b[0].size >= 0x40) {
            void *dst = ipc_guest(rq->b[0].addr, 0x40);
            amiibo_get_model_info(dst);
        }
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
        ipc_unimplemented(s, rq);
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
        // Signal availability immediately so game detects reader
        kern_event_signal(st->avail_event);

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
