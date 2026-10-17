#pragma once
#include <cstdint>

namespace wds::ui {

// One baked particle, top-down editor frame: +x = right across lanes, +y = farther along the lane (up on screen).
// World units (1 lane pitch = 0.925). Relative to the owning split line's origin.
struct SplitSnapParticle {
  int32_t catalog_index = 0;  // particle kind (global system id)
  uint16_t uv = 0xFFFF;       // uv_name_index; 0xFFFF = none (white tex / trail tex by blend)
  uint8_t blend = 0;          // 0 Additive, 1 Alpha, 2 Other (trail material, additive x2)
  uint8_t shape = 0;          // 0 billboard, 1 stretched billboard, 2 ring mesh, 3 cube mesh, 4 butterfly,
                              // 5 strip (ribbon / particle trail; vertices in the point pool; r,g,b,a = 1, use vertex
                              // tint; uv/blend/flip/sorting_order apply; V runs across the width 0..1; dx,dy = first vertex)
  uint8_t flip = 0;           // bit0 flip_u, bit1 flip_v
  int8_t sorting_order = 0;
  uint8_t rank = 0;  // selection rank within its kind on this line: 0,1 = the round-2 picks (picker shows rank < 2);
                     // 2.. = extra picks for denser views (editor shows rank < kEditorIndicatorMaxRank). Strips: 0.
  float dx = 0, dy = 0;       // offset from line origin
  float w = 0, h = 0;         // drawn quad size (billboard width/height after clamps; stretch: w = length along the velocity (sprite U axis), h = width across; angle = velocity direction, so stretch draws like a billboard)
  float angle_deg = 0;        // CCW rotation in the y-up frame (renderer converts to Qt y-down)
  float r = 1, g = 1, b = 1, a = 1;  // display-referred tint, may exceed 1
  uint32_t strip_first = 0;  // shape 5 only: first vertex in split_snap_strip_points()
  uint16_t strip_count = 0;  // shape 5 only: vertex count (>= 2)
};

// One vertex of a baked strip (ribbon or particle trail). Same frame/units as SplitSnapParticle.
struct SplitSnapStripPoint {
  float dx = 0, dy = 0;               // offset from the owning line origin
  float width = 0;                    // full strip width at this vertex (world units)
  float u = 0;                        // texture U at this vertex, as the preview computes it for the trail texture mode
  float r = 1, g = 1, b = 1, a = 1;   // final vertex tint (display-referred, may exceed 1)
};

// Global point pool; SplitSnapParticle::strip_first indexes into it.
const SplitSnapStripPoint* split_snap_strip_points();

// Baked particles for one emitting split line, already sorted in preview draw order.
// Returns nullptr / *count = 0 when that line has nothing (unknown id, empty kind list).
const SplitSnapParticle* split_snap_particles(int32_t color_id, int32_t split_count, int32_t line_index,
                                              int* count);

}  // namespace wds::ui
