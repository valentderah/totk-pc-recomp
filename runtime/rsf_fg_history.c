#include "rsf_fg_history.h"
#include <string.h>
static int valid_identity(const rsf_frame_record *r) {
    return r && r->struct_size >= sizeof *r && r->abi_version == RSF_GAME_FRAME_ABI_VERSION &&
        r->session_id && r->frame_id && r->view_id && r->resource_generation &&
        !(r->flags & (RSF_FRAME_FLAG_NO_FG | RSF_FRAME_FLAG_AMBIGUOUS_ID));
}
static int same_identity(const rsf_frame_record *a, const rsf_frame_record *b) {
    return a->session_id == b->session_id && a->frame_id == b->frame_id &&
        a->view_id == b->view_id && a->resource_generation == b->resource_generation;
}
static int find(const RsfFgHistory *h, uint64_t source) {
    if (h && source) for (unsigned i = 0; i < RSF_FG_HISTORY_COUNT; ++i)
        if (h->entries[i].state && h->entries[i].snapshot.record.frame_id == source) return (int)i;
    return -1;
}
void rsf_fg_history_clear(RsfFgHistory *h) { if (h) memset(h, 0, sizeof *h); }
int rsf_fg_history_remember(RsfFgHistory *h, const rsf_frame_record *r, const RsfFgMap *map) {
    if (!h || !valid_identity(r) || !rsf_fg_map_valid(map)) return 0;
    int existing = find(h, r->frame_id);
    if (existing >= 0) {
        // A source number identifying two different accepted prepares is
        // ambiguous. Refuse it instead of borrowing either record as latest.
        if (h->entries[existing].state != 1 ||
            !same_identity(&h->entries[existing].snapshot.record, r) ||
            !rsf_fg_map_equal(&h->entries[existing].snapshot.map, map)) {
            h->entries[existing].state = -1;
            return 0;
        }
        return 1;
    }
    unsigned index = h->next++ % RSF_FG_HISTORY_COUNT;
    h->entries[index].snapshot = (RsfFgSnapshot){*r, *map};
    h->entries[index].state = 1;
    return 1;
}
int rsf_fg_history_matches(const RsfFgHistory *h, const rsf_frame_record *r) {
    int index = valid_identity(r) ? find(h, r->frame_id) : -1;
    return index >= 0 && h->entries[index].state == 1 &&
        same_identity(&h->entries[index].snapshot.record, r);
}
int rsf_fg_history_snapshot(const RsfFgHistory *h, uint64_t source, const RsfFgMap *actual, RsfFgSnapshot *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    int index = find(h, source);
    if (index < 0 || h->entries[index].state != 1 || !rsf_fg_map_valid(actual) ||
        !rsf_fg_map_equal(&h->entries[index].snapshot.map, actual)) return 0;
    *out = h->entries[index].snapshot;
    return 1;
}
