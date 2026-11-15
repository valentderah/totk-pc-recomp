// Native Vulkan SR service from the separately installed RSF SDK. Headers are
// consumed from that checkout; vendor or orchestrator source is not vendored.
#pragma once
#include <rescaleframe/native_sr_vulkan.h>

typedef struct RsfSrSession RsfSrSession;

// Returns success only after provider creation and render-size planning. On a
// cleanup failure, *out retains ownership despite a zero result; close it again.
int rsf_sr_open(const rsf_srvk_setup *setup, RsfSrSession **out, uint32_t *width, uint32_t *height);
// Calls are serialized by the graphics owner. Even failed recording must be submitted.
int rsf_sr_record(RsfSrSession *session, void *commands, const rsf_srvk_frame *frame);
// Call after GPU completion and command-buffer reset, before recording it again.
void rsf_sr_retire(RsfSrSession *session, void *commands);
// Renderer command-pool reuse boundary: prior GPU work is complete and a new
// recording has begun (implicitly resetting the command buffer's previous use).
void rsf_sr_commands_reset(void *commands);
// Caller drains all GPU references first. Failure retains the session for retry.
int rsf_sr_close(RsfSrSession **session);
