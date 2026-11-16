#pragma once
#include <stdint.h>
// Delivered input kinds, never keys/buttons, source IDs or vendor marker choices.
#define RSF_INPUT_KEYBOARD 1u
#define RSF_INPUT_MOUSE 2u
#define RSF_INPUT_CONTROLLER 4u
void rsf_input_received(uint32_t kind);
// Call only for an actually removed OS queue message, before dispatching it.
void rsf_input_window_message(uint32_t message);
// Queue removal classifier; real input kinds carry message_id zero separately.
void rsf_input_win_dequeued(uint32_t message);
