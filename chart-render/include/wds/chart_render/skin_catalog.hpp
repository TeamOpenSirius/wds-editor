#pragma once

#include <wds/chart_render/split_line_skins.hpp>
#include <wds/renderer/texture.hpp>

#include <string>

namespace wds::renderer {

// Official Sirius note skins under skins/ (Top/Bottom sliced sprites).
// SkinCatalog::load fails hard if any required note asset is missing.
struct SkinCatalog {
  // Official GameBackground theater plate (curtains / floor / lights).
  TextureInfo ingame_background;
  // Official BG_LaneBorder sprite (img_ingame_lane_border2).
  TextureInfo lane_border;
  TextureInfo judgeline;

  // Shared bottom + per-color tops (official A_NotesBottom / A_*NotesTop).
  TextureInfo note_bottom;
  TextureInfo note_red_top;
  TextureInfo note_yellow_top;
  TextureInfo note_blue_top;
  TextureInfo note_purple_top;

  // Hold ribbons baked from Unity Shader Graphs/LongNotesSprite (UV.x profile).
  // Hold ribbons are baked in SkinCatalog::load (LongNotesSprite UV profiles).
  TextureInfo hold_connection_blue;
  TextureInfo hold_connection_purple;
  TextureInfo sync_line;
  TextureInfo scratch_arrow;
  TextureInfo tick_blue;
  TextureInfo tick_purple;
  TextureInfo hidden_line;

  // Horizontal 3-slice borders (Unity Sprite.border / drawMode=Sliced).
  // Flat A_*Notes*: 268×108, m_Border L/R = 65. HoldLongNotes: 157×33, L/R = 10.
  float note_slice_border_l = 65.0f;
  float note_slice_border_r = 65.0f;
  float hold_slice_border_l = 10.0f;
  float hold_slice_border_r = 10.0f;
  // NoteConcurrentLine: 12×8, m_Border L/R = 4.
  float sync_slice_border_l = 4.0f;
  float sync_slice_border_r = 4.0f;
  // Flat note source height; hold caps use note_h_screen / this (same PPU as flats).
  float note_slice_tex_h = 108.0f;

  // Fallback when color id has no matching Sirius Split Line skin.
  TextureInfo split_line_1;
  TextureInfo split_line_2;

  SplitLineSkinBank split_lines;

  TextureInfo judge_auto;

  TextureInfo combo_ap_text;
  TextureInfo combo_ap_digit[10];

  TextureInfo effect_linear_bg;
  TextureInfo effect_linear_star;
  TextureInfo effect_circular;

  // Official Default Bomb Light layers under skins/effects/bomb/light/default/{type}/.
  // Missing type falls back to normal; missing all → Sonolus linear/circular.
  TextureInfo bomb_square_normal;
  TextureInfo bomb_flare_normal;
  TextureInfo bomb_square_critical;
  TextureInfo bomb_flare_critical;
  TextureInfo bomb_square_scratch;
  TextureInfo bomb_flare_scratch;
  TextureInfo bomb_square_hold;
  TextureInfo bomb_flare_hold;
  TextureInfo bomb_square_sound;
  TextureInfo bomb_flare_sound;

  TextureInfo soft_split_line;
  TextureInfo soft_disk;

  bool load(TextureCache& cache, const std::string& skins_directory);

  // Resolve Light bomb layers for a note family. Returns false if square is missing
  // (caller should use Sonolus fallback).
  bool bomb_light_for(const char* type_dir, TextureInfo& square, TextureInfo& flare) const noexcept;
};

}  // namespace wds::renderer
