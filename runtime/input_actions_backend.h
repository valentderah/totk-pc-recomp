// Interface between the portable action backend and the install-time generated game adapter.
#pragma once
#include <stdint.h>
int input_actions_game_supported(void);
uint64_t input_actions_registry_slot(void);
uint32_t input_actions_pause_hash(void);
uint64_t input_context_activation(void);
int input_context_flag(uint32_t hash);
void input_actions_bridge_begin(uint64_t site, uint64_t object);
uint64_t input_actions_bridge_apply(unsigned phase, uint64_t original);
void input_actions_interaction_observe(uint64_t object, int64_t selected, int entry_valid);
