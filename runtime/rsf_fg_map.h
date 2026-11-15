#pragma once
#include <rescaleframe/game_frame.h>
// Pixel rectangles use top-left origins; camera matrices use row vectors and Y-up clip space.
typedef struct RsfFgMap { unsigned game_w, game_h, out_w, out_h; int crop[4], dst[4]; } RsfFgMap;
int rsf_fg_map_valid(const RsfFgMap *m);
int rsf_fg_map_equal(const RsfFgMap *a, const RsfFgMap *b);
int rsf_fg_map_sample(const RsfFgMap *m, unsigned sw, unsigned sh, int x, int y, int source[2]);
void rsf_fg_map_motion(const RsfFgMap *m, unsigned sw, unsigned sh, float scale[2]);
int rsf_fg_map_camera(const RsfFgMap *m, const RsfFgMap *previous, rsf_frame_record *record);
