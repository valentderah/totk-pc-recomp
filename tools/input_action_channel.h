// Research action state, independent of Switch button mapping and any physical-input backend.
#pragma once
#include <stdint.h>

typedef struct {
    int seen, held, blocked, pressed, released;
    int64_t clock;
    uint64_t context;
} ResearchActionState;

// Call once per consumer frame. Repeated calls for the same clock/context return the same edges. A held input on entry,
// after an inactive gap, or on a context switch requires release before it can become a fresh action press.
void research_action_update(ResearchActionState *s, int64_t clock, uint64_t context, int down, int active);
void input_action_channel_begin(uint64_t site, uint64_t object);
uint64_t input_action_channel_apply(uint64_t site, uint64_t field, uint64_t original);
