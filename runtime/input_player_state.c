// Guarded portable adaptation of Weeping of the Empire's player-state reader; observation only.
// Copyright (c) 2026 daniel-madrid-07
// Copyright (c) 2026 the totk-recomp contributors
// Adapted from engine/runtime/totk_player_state.cpp and .h at fe2345e82a43156183242abfdcc0eb1f5526eba6.
// MIT License; the full notice is in tools/licenses/weeping-player-state.txt.
#include "input_player_state.h"
#include <stddef.h>
#include <string.h>

extern int plat_mem_read(uint64_t, void *, size_t);

static int read_at(uint64_t p, uint32_t offset, void *out, size_t n) {
    return p && p <= UINT64_MAX - offset - n && plat_mem_read(p + offset, out, n);
}
static int object(const struct InputPlayerLayout *l, uint64_t p) {
    uint64_t vtable;
    return p && !(p & 7) && read_at(p, 0, &vtable, sizeof vtable) &&
           vtable >= l->main_start && vtable < l->main_end;
}
static void unknown(struct InputPlayerState *s) {
    memset(s, 0, sizeof *s);
    for (unsigned i = 0; i < 5; i++) s->verbs[i] = -1;
    s->ability = -1;
    s->identity = -1;
}

// Resolve the player's own link and reject deletion or a reused link id. A missing player differs from a failed read.
static int player(const struct InputPlayerLayout *l, uint64_t *actor, int32_t *identity) {
    uint64_t variable, root, data, proc;
    int32_t id, current_id;
    uint32_t state;
    if (!read_at(l->global_slot, 0, &variable, 8)) return IPS_UNKNOWN;
    if (!variable) return IPS_ABSENT;
    if (!read_at(variable, 0, &root, 8)) return IPS_UNKNOWN;
    if (!root) return IPS_ABSENT;
    if (root > UINT64_MAX - l->root_link) return IPS_UNKNOWN;
    const uint64_t link = root + l->root_link;
    if (!read_at(link, l->link_id, &id, 4)) return IPS_UNKNOWN;
    if (id == -1) return IPS_ABSENT;
    if (!read_at(link, l->link_data, &data, 8)) return IPS_UNKNOWN;
    if (!data) return IPS_ABSENT;
    if (!read_at(data, l->unit_id, &current_id, 4)) return IPS_UNKNOWN;
    if (id != current_id) return IPS_UNKNOWN;
    if (!read_at(data, l->unit_proc, &proc, 8)) return IPS_UNKNOWN;
    if (!proc) return IPS_ABSENT;
    if (!object(l, proc) || !read_at(proc, l->proc_state, &state, 4)) return IPS_UNKNOWN;
    if (state >= 7 && state <= 11) return IPS_ABSENT;
    *actor = proc;
    *identity = id;
    return IPS_READY;
}

void input_player_state_read(const struct InputPlayerLayout *l, uint64_t hint, struct InputPlayerState *s) {
    unknown(s);
    s->status = player(l, &s->actor, &s->identity);
    if (s->status != IPS_READY) {
        if (s->status == IPS_ABSENT) s->known = IPS_VALID;
        return;
    }
    if (hint && hint != s->actor) { unknown(s); return; }
    uint64_t comps, as;
    if (!read_at(s->actor, l->actor_components, &comps, 8) || !comps ||
        !read_at(comps, l->as_component, &as, 8) || !object(l, as)) { unknown(s); return; }
    s->flags = s->known = IPS_VALID;
    uint64_t slots[5];
    if (read_at(as, l->as_slots, slots, sizeof slots)) {
        for (unsigned i = 0; i < 5; i++) {
            int32_t v = 0;
            if (slots[i]) {
                // Slots are state records, not polymorphic objects; only their containing AS component has a vtable.
                if ((slots[i] & 7) || !read_at(slots[i], l->slot_verb, &v, 4)) continue;
                if ((uint32_t)v == l->invalid_verb && !read_at(slots[i], l->slot_alternate, &v, 4)) continue;
            }
            if (v < 0 || (uint32_t)v >= l->verb_count) continue;
            s->verbs[i] = v;
            s->slots_known |= 1u << i;
            s->flags |= l->any_flags[v];
        }
        // Null upper-body slots are valid empty slots. A null body slot has no current movement state.
        if ((s->slots_known & 1) && slots[0]) {
            s->known |= l->body_known;
            s->flags |= l->body_flags[s->verbs[0]];
            if (l->body_unknown) s->known &= ~l->body_unknown[s->verbs[0]];
        }
        if (s->slots_known == 31) s->known |= l->any_known;
        else s->known |= s->flags & l->any_known;  // a positive observation remains useful when another slot is unknown
    }
    uint64_t component;
    uint16_t state;
    if (read_at(comps, l->player_component, &component, 8) && object(l, component) &&
        read_at(component, l->state_bytes, &state, 2)) {
        // These bytes supplement verbs. Clearing a byte alone cannot disprove a positive verb.
        if (state & l->swim_mask) s->flags |= IPS_SWIM;
        if (state & l->climb_mask) s->flags |= IPS_CLIMB;
        if (s->flags & IPS_SWIM) s->known |= IPS_SWIM;
        if (s->flags & IPS_CLIMB) s->known |= IPS_CLIMB;
    } else {
        s->known &= ~(IPS_SWIM | IPS_CLIMB);
    }
    int32_t attention;
    if (read_at(comps, l->attention_component, &component, 8) && object(l, component) &&
        read_at(component, l->attention_state, &attention, 4)) {
        s->known |= IPS_LOCKON;
        if (attention == 2 || attention == 3) s->flags |= IPS_LOCKON;
    }
    uint32_t ability;
    if (read_at(comps, l->power_component, &component, 8) && object(l, component) &&
        read_at(component, l->power_current, &ability, 4) && ability <= 8) {
        s->ability = (int32_t)ability;  // selected ability can remain set while no ability is in use
        uint64_t hand;
        uint32_t hand_state;
        if (ability != l->holding_ability) {
            s->known |= IPS_HOLDING;
        } else if (read_at(component, l->power_hand, &hand, 8) && object(l, hand) &&
            read_at(hand, l->hand_state, &hand_state, 4)) {
            s->known |= IPS_HOLDING;
            if (hand_state == 4 || hand_state == 5) s->flags |= IPS_HOLDING;
        }
    }
    // Reject a sample spanning a player replacement, component replacement or root-link reuse.
    uint64_t check_actor = 0, check_comps = 0, check_as = 0;
    int32_t check_identity = -1;
    if (player(l, &check_actor, &check_identity) != IPS_READY || check_actor != s->actor ||
        check_identity != s->identity ||
        !read_at(s->actor, l->actor_components, &check_comps, 8) || check_comps != comps ||
        !read_at(comps, l->as_component, &check_as, 8) || check_as != as) unknown(s);
}
