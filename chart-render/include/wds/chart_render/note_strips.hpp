#pragma once

#include <wds/renderer/draw_batch.hpp>

#include <cstdint>

namespace wds::renderer {

// Official-style horizontal 3-slice (Unity SpriteRenderer drawMode = Sliced).
//
// border_l / border_r are source pixels (A_*Notes* → 65 on 268×108, PPU 100;
// HoldLongNotes → 10 on 157×33, PPU 100).
//
// When dest_world_width > 0: cap fraction = (border_px / PPU) / dest_world_width.
// Unity keeps that world size even if the two caps overlap (no shrink-to-fit).
//
// Otherwise cap size = border_px * border_scale_px in dest units (edit timeline).
// When border_scale_px < 0, scale = (quad vertical length) / tex_height.
//
// alpha_near applies to lb/rb; alpha_far to lt/rt (preview hold tip fade).
void add_sliced_note(DrawBatch& batch, const TextureInfo& sprite, const Quad& quad,
                     float border_l_px, float border_r_px, float z, float alpha_near,
                     float alpha_far, float border_scale_px = -1.0f, float r = 1.0f,
                     float g = 1.0f, float b = 1.0f, float dest_world_width = 0.0f);

inline void add_sliced_note(DrawBatch& batch, const TextureInfo& sprite, const Quad& quad,
                            float border_l_px, float border_r_px, float z, float alpha,
                            float border_scale_px = -1.0f, float r = 1.0f, float g = 1.0f,
                            float b = 1.0f) {
  add_sliced_note(batch, sprite, quad, border_l_px, border_r_px, z, alpha, alpha,
                  border_scale_px, r, g, b);
}

// Same as add_sliced_note, but atlas V for near(lb) / far(lt) are explicit (spawn clip).
void add_sliced_note_v(DrawBatch& batch, const TextureInfo& sprite, const Quad& quad,
                       float border_l_px, float border_r_px, float z, float alpha,
                       float border_scale_px, float v_near_atlas, float v_far_atlas,
                       float r = 1.0f, float g = 1.0f, float b = 1.0f,
                       float dest_world_width = 0.0f);

}  // namespace wds::renderer
