// Accepted frame identity and crop metadata outlive reusable mapped GPU backing.
#pragma once
#include "rsf_fg_map.h"
#define RSF_FG_HISTORY_COUNT 64u
typedef struct RsfFgSnapshot { rsf_frame_record record; RsfFgMap map; } RsfFgSnapshot;
typedef struct RsfFgHistory {
    struct { RsfFgSnapshot snapshot; int state; } entries[RSF_FG_HISTORY_COUNT];
    unsigned next;
} RsfFgHistory;
// Caller serializes on the presentation lock. Remember only accepted SDK prepares.
void rsf_fg_history_clear(RsfFgHistory *);
int rsf_fg_history_remember(RsfFgHistory *, const rsf_frame_record *, const RsfFgMap *);
int rsf_fg_history_matches(const RsfFgHistory *, const rsf_frame_record *);
// Produces an owned copy for a pending swapchain image. Never returns latest.
int rsf_fg_history_snapshot(const RsfFgHistory *, uint64_t source, const RsfFgMap *actual, RsfFgSnapshot *);
