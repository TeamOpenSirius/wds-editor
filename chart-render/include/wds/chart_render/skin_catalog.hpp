#pragma once

#include <wds/chart_render/split_line_skins.hpp>
#include <wds/renderer/texture.hpp>

#include <string>

namespace wds::renderer {

// Official Sirius note skins under skins/ (Top/Bottom sliced sprites).
// SkinCatalog::load fails hard if any required note asset is missing.
struct SkinCatalog {
  TextureInfo stage;
  TextureInfo stage_background;
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

  // Horizontal 3-slice borders for flat tops/bottoms (pixels on 268-wide sprites).
  float note_slice_border_l = 65.0f;
  float note_slice_border_r = 65.0f;

  // Fallback when color id has no matching Sirius Split Line skin.
  TextureInfo split_line_1;
  TextureInfo split_line_2;
  TextureInfo split_line_trans1;
  TextureInfo split_line_trans2;

  SplitLineSkinBank split_lines;

  TextureInfo judge_auto;
  TextureInfo judge_perfect_plus;
  TextureInfo flick_circle;
  TextureInfo flick_star;

  TextureInfo combo_ap_text;
  TextureInfo combo_ap_digit[10];

  TextureInfo effect_linear_bg;
  TextureInfo effect_linear_line;
  TextureInfo effect_linear_star;
  TextureInfo effect_circular;

  TextureInfo soft_split_line;
  TextureInfo soft_disk;

  bool load(TextureCache& cache, const std::string& skins_directory);
};

}  // namespace wds::renderer
