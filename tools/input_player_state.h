// Read-only player state observations; game addresses and verb mappings come from a verified local build plan.
// Copyright (c) 2026 daniel-madrid-07
// Copyright (c) 2026 the totk-recomp contributors
// Adapted from Weeping of the Empire at fe2345e82a43156183242abfdcc0eb1f5526eba6.
// MIT License; see tools/licenses/weeping-player-state.txt.
#ifndef INPUT_PLAYER_STATE_H
#define INPUT_PLAYER_STATE_H
#include <stdint.h>

enum InputPlayerFlag {
    IPS_VALID = 1u << 0, IPS_CLIMB = 1u << 1, IPS_AIR = 1u << 2, IPS_GLIDE = 1u << 3,
    IPS_SWIM = 1u << 4, IPS_AIM = 1u << 5, IPS_GUARD = 1u << 6, IPS_LOCKON = 1u << 7,
    IPS_ABILITY = 1u << 8, IPS_HORSE = 1u << 9, IPS_VEHICLE = 1u << 10, IPS_SURF = 1u << 11,
    IPS_TALK = 1u << 12, IPS_CAMERA = 1u << 13, IPS_HOLDING = 1u << 15
};
// AIM means the bow-use verb is active, not that the bow is currently drawn. GUARD means the guard-keep verb,
// not every raised-shield pose. A clear flag is meaningful only when its bit is also set in known.
enum InputPlayerStatus { IPS_UNKNOWN, IPS_ABSENT, IPS_READY };
struct InputPlayerLayout {
    uint64_t main_start, main_end, global_slot, site;
    uint32_t root_link, link_data, link_id, unit_proc, unit_id, proc_state;
    uint32_t actor_components, as_component, player_component, attention_component, power_component;
    uint32_t as_slots, slot_verb, slot_alternate, invalid_verb, verb_count;
    uint32_t state_bytes, swim_mask, climb_mask, attention_state, power_current, power_hand, hand_state, holding_ability;
    uint32_t body_known, any_known;
    const uint32_t *body_flags, *any_flags, *body_unknown;
};
struct InputPlayerState {
    uint64_t actor;
    uint32_t status, flags, known, slots_known;
    int32_t verbs[5], ability, identity;
};

// No guest calls or writes. actor_hint is the publisher's own actor, or zero on a poll. A mismatching hint is unknown.
void input_player_state_read(const struct InputPlayerLayout *, uint64_t actor_hint, struct InputPlayerState *);
const struct InputPlayerLayout *input_player_state_config(void);
void input_player_state_frame(uint64_t site, uint64_t actor_hint);
void input_player_state_tick(void);
void input_player_state_observe(uint64_t site, int from_publisher, const struct InputPlayerState *);
#endif
