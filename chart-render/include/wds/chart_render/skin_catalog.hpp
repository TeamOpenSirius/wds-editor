#pragma once

#include <wds/chart_render/split_line_skins.hpp>
#include <wds/renderer/texture.hpp>

#include <string>

namespace wds::renderer {

// Maps logical sprite names to files under skins/. Prefer Sirius-named assets;
// fall back to Sonolus #NAME.png convention when present.
struct SkinCatalog {
  TextureInfo stage;
  TextureInfo stage_background;
  TextureInfo judgeline;

  TextureInfo note_red_left, note_red_middle, note_red_right;
  TextureInfo note_yellow_left, note_yellow_middle, note_yellow_right;
  TextureInfo note_blue_left, note_blue_middle, note_blue_right;
  TextureInfo note_purple_left, note_purple_middle, note_purple_right;

  TextureInfo hold_connection_blue;
  TextureInfo hold_connection_purple;
  TextureInfo sync_line;
  TextureInfo scratch_arrow;
  TextureInfo tick_blue;
  TextureInfo tick_purple;
  TextureInfo hidden_line;

  // Fallback when color id has no matching Sirius Split Line skin.
  TextureInfo split_line_1;
  TextureInfo split_line_2;
  TextureInfo split_line_trans1;
  TextureInfo split_line_trans2;

  // Official color-id bank (scratchLength → Sirius Split Line _id).
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

  // Procedural soft gray split-line fallback (packed into the skin atlas).
  TextureInfo soft_split_line;
  // Procedural soft white disk for UI circles (checkbox etc.) — 1 sprite vs N strips.
  TextureInfo soft_disk;

  bool load(TextureCache& cache, const std::string& skins_directory);
};

}  // namespace wds::renderer
