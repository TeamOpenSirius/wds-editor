#pragma once

#include <wds/chart_render/skin_catalog.hpp>
#include <wds/core/official_playfield.hpp>
#include <wds/renderer/texture.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace wds::chart_render {

// Cap scale for Unity-style Sliced holds/flats.
// `flat_height` must be in the SAME units as the destination quad edges (NDC),
// never raw framebuffer/logical pixels mixed into an NDC dest_w.
inline float border_scale_from_flat_height(float flat_height, float note_slice_tex_h) noexcept {
  return flat_height / (note_slice_tex_h > 1e-6f ? note_slice_tex_h : 1.0f);
}

inline float border_scale_from_flat_height(float flat_height,
                                          const wds::renderer::SkinCatalog& skin) noexcept {
  return border_scale_from_flat_height(flat_height, skin.note_slice_tex_h);
}

// dest units per source pixel so cap_world = border_px / PPU maps onto dest_w.
inline float border_scale_from_ppu(float dest_w, float dest_world_width,
                                   float ppu = wds::chart_editor::kOfficialNoteSpritePpu) noexcept {
  return dest_w / (std::max(dest_world_width, 1e-6f) * std::max(ppu, 1e-6f));
}

struct SlicedCapLayout {
  float bl = 0.0f;
  float br = 0.0f;
  bool emit_middle = true;
};

// Temporary approximation (not Unity SpriteRenderer Sliced):
// Official corners stay border_px/PPU and overlap when they cannot fit
// (1-wide tap: 0.65+0.65 > 0.765). Overlapping opaque caps hide the left
// decoration. Until Unity's mesh/compositing is dumped, shrink both caps
// proportionally so they sit side-by-side (uGUI GetAdjustedBorders style).
inline SlicedCapLayout sliced_cap_layout(
    float border_l_px, float border_r_px, float dest_world_width,
    float ppu = wds::chart_editor::kOfficialNoteSpritePpu) noexcept {
  SlicedCapLayout out;
  out.bl = wds::chart_editor::official_sliced_cap_fraction(border_l_px, dest_world_width, ppu);
  out.br = wds::chart_editor::official_sliced_cap_fraction(border_r_px, dest_world_width, ppu);
  const float sum = out.bl + out.br;
  if (sum > 1.0f - 1e-5f && sum > 1e-6f) {
    const float s = 1.0f / sum;
    out.bl *= s;
    out.br *= s;
  }
  out.emit_middle = (out.bl + out.br) < 1.0f - 1e-5f;
  return out;
}

// Hold end-cap tops: ScratchHold → purple; regular Hold → blue. Shared bottom.
struct HoldTailLayers {
  wds::renderer::TextureInfo bottom;
  wds::renderer::TextureInfo top;
  bool is_scratch_family = false;
};

HoldTailLayers hold_tail_layers(const wds::renderer::SkinCatalog& skin,
                                bool scratch_hold) noexcept;

// Official HoldLongNotes bake (157×8, Sprite.border L/R = 10). Shared by the
// Vulkan preview atlas and the Qt editor so both ribbons use one profile.
inline constexpr int kHoldLongTexW = 157;
inline constexpr int kHoldLongTexH = 8;
inline constexpr float kHoldLongSliceBorderL = 10.0f;
inline constexpr float kHoldLongSliceBorderR = 10.0f;
// Flat A_*Notes* (268×108, Sprite.border L/R = 65). Qt editor 3-slice uses the
// same world-space caps as add_sliced_note (shrink only when 1-wide).
inline constexpr float kNoteSliceBorderL = 65.0f;
inline constexpr float kNoteSliceBorderR = 65.0f;
// drawHoldEighth idle alpha (PreviewVisualConfig::hold_body_alpha).
inline constexpr float kHoldBodyAlpha = 0.8f;

std::vector<unsigned char> bake_hold_long_rgba(bool scratch);

// scratch_length: - left only, + right only, 0 both (Sirius / utils.cpp).
struct ScratchArrowSides {
  bool draw_left = false;
  bool draw_right = false;
};

inline ScratchArrowSides scratch_arrow_sides(int32_t scratch_length) noexcept {
  return ScratchArrowSides{scratch_length <= 0, scratch_length >= 0};
}

enum class ArrowStyle : uint8_t {
  Static,    // Edit timeline: dense pixel packing, alpha = 1
  Animated,  // Preview stage: Sirius density + scrolling alpha
};

// One arrow quad in a 1D lane span [span_left, span_right] (same axis units as W).
// Callers build screen/NDC quads from x0/x1 (and optional y).
struct ArrowInstance {
  float x0 = 0.0f;
  float x1 = 0.0f;
  bool flip_x = false;  // true = right-pointing (UV/geometry mirrored)
  float alpha = 1.0f;
  // Full sprite U. Official NotesArrows never UV-clip; heads may sit past the note.
  float u0 = 0.0f;
  float u1 = 1.0f;
};

struct StaticArrowLayoutParams {
  float span_left = 0.0f;
  float span_right = 0.0f;
  float arrow_w = 0.0f;
  int32_t scratch_length = 0;
};

struct AnimatedArrowLayoutParams {
  float span_left = 0.0f;
  float span_right = 0.0f;
  float arrow_w = 0.0f;
  // Official NotesArrows: interval * 0.7 in dest units. 0 → arrow_w * 0.5.
  float arrow_step = 0.0f;
  // Official parent |x| from note center (width/2 - 0.145, or jump half-strip).
  // 0 → first center sits half a sprite inside the active edge.
  float group_offset = 0.0f;
  // Official ActivateArrowSpriteRenderer count. 0 → sonolus_num.
  int32_t arrow_count = 0;
  // One-way only: keep official step from the head parent until the tail would
  // pass the far edge. Official has no UV clip; the first head may sit past
  // the near edge. Bidirectional must stay false (same flick count per side).
  bool fill_to_far_edge = false;
  int32_t scratch_length = 0;
  float sonolus_num = 1.0f;
  float anim_time_sec = 0.0f;
  float arrow_speed = 1.0f;
};

std::vector<ArrowInstance> layout_static_scratch_arrows(const StaticArrowLayoutParams& p);

std::vector<ArrowInstance> layout_animated_scratch_arrows(const AnimatedArrowLayoutParams& p);

}  // namespace wds::chart_render
