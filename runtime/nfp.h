// Nintendo Figurine Platform (nfp:user) Horizon OS IPC service emulation.
#pragma once
#include "ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

// sm: handler for "nfp:user" (nn::nfp::detail::IUserManager)
void nfp_user_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs);

#ifdef __cplusplus
}
#endif
