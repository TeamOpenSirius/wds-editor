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

// Unity Sliced corners stay border_px/PPU even when they overlap (no shrink-to-fit).
inline SlicedCapLayout sliced_cap_layout(
    float border_l_px, float border_r_px, float dest_world_width,
    float ppu = wds::chart_editor::kOfficialNoteSpritePpu) noexcept {
  SlicedCapLayout out;
  out.bl = wds::chart_editor::official_sliced_cap_fraction(border_l_px, dest_world_width, ppu);
  out.br = wds::chart_editor::official_sliced_cap_fraction(border_r_px, dest_world_width, ppu);
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
  int32_t scratch_length = 0;
  // Sirius: lane_width * (end_lane-lane+1) * arrow_percent / W
  float sonolus_num = 1.0f;
  float anim_time_sec = 0.0f;
  float arrow_speed = 1.0f;
};

std::vector<ArrowInstance> layout_static_scratch_arrows(const StaticArrowLayoutParams& p);

std::vector<ArrowInstance> layout_animated_scratch_arrows(const AnimatedArrowLayoutParams& p);

}  // namespace wds::chart_render
