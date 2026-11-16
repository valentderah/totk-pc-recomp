#pragma once
#include <stdint.h>

// Sidecar ownership follows the observed native queue lifecycle and FIFO order.
void rsf_handoff_initialize(uint64_t queue);
// Before a verified blocking framework draw publication. Source zero is explicit
// unassociated work. A refusal never changes the native queue operation.
int rsf_handoff_publish(uint64_t queue, uint64_t message, void *publisher, uint64_t framework, uint64_t source);
// A different publication operation invalidates association until reinitialization.
void rsf_handoff_foreign(uint64_t queue);
// After successful native receive. A tracked but invalid queue returns source zero.
int rsf_handoff_receive(uint64_t queue, uint64_t message, uint64_t *source);
