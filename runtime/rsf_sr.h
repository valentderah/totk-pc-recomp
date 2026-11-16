// Native Vulkan SR service from the separately installed RSF SDK. Headers are
// consumed from that checkout; vendor or orchestrator source is not vendored.
#pragma once
#include <rescaleframe/native_sr_vulkan.h>
#include <rescaleframe/native_sr_split_vulkan.h>

typedef struct RsfSrSession RsfSrSession;

// Returns success only after provider creation and render-size planning. On a
// cleanup failure, *out retains ownership despite a zero result; close it again.
int rsf_sr_open(const rsf_srvk_setup *setup, RsfSrSession **out, uint32_t *width, uint32_t *height);
enum { RSF_SR_FAILED = 0, RSF_SR_RECORDED = 1, RSF_SR_PENDING = 2, RSF_SR_FALLBACK = 3 };
// PENDING retains the owner for a later frame; it never authorizes using output.
// Calls are serialized by the graphics owner. Even failed recording must be submitted.
int rsf_sr_record(RsfSrSession *session, void *commands, const rsf_srvk_frame *frame);
// Renderer command-pool reuse boundary: prior GPU work is complete and a new
// recording has begun (implicitly resetting the command buffer's previous use).
void rsf_sr_commands_reset(void *commands);
// Caller drains all GPU references first. Failure retains the session for retry.
int rsf_sr_close(RsfSrSession **session);

// FSR4 uses a recording split on the same graphics queue, never a second runtime.
int rsf_sr_open_split(const rsf_srvk_setup *, const rsf_fsr4vk_handoff *, RsfSrSession **, uint32_t *, uint32_t *);
// Replaces *commands even on SDK failure. NULL is terminal, never record into it.
int rsf_sr_record_split(RsfSrSession *, void **commands, const rsf_srvk_frame *);
int rsf_sr_commands_pending(void *commands);
// Only after successful actual submission, outside the queue lock.
int rsf_sr_commands_submitted(void *commands);
