#include <wds/chart_render/skin_catalog.hpp>

#include <wds/chart_render/split_soft_profile.hpp>
#include <wds/common/utf8_path.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace wds::renderer {

namespace {

namespace fs = std::filesystem;

using wds::common::is_regular_file_utf8;
using wds::common::path_from_utf8;
using wds::common::path_to_utf8;

std::string queue_required(TextureCache& cache, const fs::path& dir, const char* name) {
  const fs::path path = dir / name;
  const std::string utf8 = path_to_utf8(path);
  if (!is_regular_file_utf8(utf8) || !cache.queue_png(utf8)) {
    std::fprintf(stderr, "SkinCatalog: missing required skin '%s'\n", name);
    return {};
  }
  return utf8;
}

std::string queue_optional(TextureCache& cache, const fs::path& dir,
                           std::initializer_list<const char*> names) {
  for (const char* name : names) {
    const fs::path path = dir / name;
    const std::string utf8 = path_to_utf8(path);
    if (is_regular_file_utf8(utf8) && cache.queue_png(utf8)) {
      return utf8;
    }
  }
  return {};
}

}  // namespace

bool SkinCatalog::load(TextureCache& cache, const std::string& skins_directory) {
  const fs::path dir = path_from_utf8(skins_directory);

  // Large stage plates stay out of the atlas (same reason as ingame_bg).
  auto find_skin = [&](std::initializer_list<const char*> names) -> std::string {
    for (const char* name : names) {
      const fs::path path = dir / name;
      const std::string utf8 = path_to_utf8(path);
      if (is_regular_file_utf8(utf8)) {
        return utf8;
      }
    }
    return {};
  };
  const std::string p_stage = find_skin({"_STAGE_COVER.png", "Sirius Stage Cover.png"});
  const std::string p_stage_bg =
      find_skin({"_STAGE_BOTTOM_BORDER.png", "Sirius Stage Bottom Border.png"});
  const std::string p_judgeline =
      queue_optional(cache, dir, {"_JUDGMENT_LINE.png", "Sirius Judgment Line.png"});

  // Official note set — all required (no fallback).
  const std::string p_bottom = queue_required(cache, dir, "Sirius Note Bottom.png");
  const std::string p_red = queue_required(cache, dir, "Sirius Note Red Top.png");
  const std::string p_yel = queue_required(cache, dir, "Sirius Note Yellow Top.png");
  const std::string p_blu = queue_required(cache, dir, "Sirius Note Blue Top.png");
  const std::string p_pur = queue_required(cache, dir, "Sirius Note Purple Top.png");
  const std::string p_sync = queue_required(cache, dir, "Sirius Simultaneous Connection.png");
  const std::string p_arrow = queue_required(cache, dir, "Sirius Scratch Arrow.png");
  const std::string p_tick_b = queue_required(cache, dir, "Sirius Note Tick Blue.png");
  const std::string p_tick_p = queue_required(cache, dir, "Sirius Note Tick Purple.png");

  if (p_stage.empty() || p_judgeline.empty() || p_bottom.empty() || p_red.empty() ||
      p_yel.empty() || p_blu.empty() || p_pur.empty() || p_sync.empty() || p_arrow.empty() ||
      p_tick_b.empty() || p_tick_p.empty()) {
    return false;
  }

  const std::string p_hidden = queue_optional(cache, dir, {"Sirius Hidden Line.png"});
  const std::string p_split1 = queue_optional(cache, dir, {"Sirius Split Line _1.png"});
  const std::string p_split2 = queue_optional(cache, dir, {"Sirius Split Line _2.png"});
  const std::string p_split_t1 =
      queue_optional(cache, dir, {"Sirius Split Line Transform 1 _1.png"});
  const std::string p_split_t2 =
      queue_optional(cache, dir, {"Sirius Split Line Transform 2 _1.png"});

  split_lines.queue_all(cache, skins_directory);

  const std::string p_auto = queue_optional(cache, dir, {"Sirius Judgment Auto.png"});
  const std::string p_pp = queue_optional(cache, dir, {"Sirius Judgment Perfect+.png"});
  const std::string p_flick_c = queue_optional(cache, dir, {"Sirius Flick Circle.png"});
  const std::string p_flick_s = queue_optional(cache, dir, {"Sirius Flick Star.png"});
  const std::string p_lin_bg = queue_optional(cache, dir, {"Sirius Linear Background.png"});
  const std::string p_lin_line = queue_optional(cache, dir, {"Sirius Linear Line.png"});
  const std::string p_lin_star = queue_optional(cache, dir, {"Sirius Linear Star.png"});

  auto queue_bomb_layer = [&](const char* type_dir, const char* layer) -> std::string {
    const fs::path path =
        dir / "effects" / "bomb" / "light" / "default" / type_dir / layer;
    const std::string utf8 = path_to_utf8(path);
    if (is_regular_file_utf8(utf8) && cache.queue_png(utf8)) {
      return utf8;
    }
    return {};
  };
  const std::string p_b_sq_n = queue_bomb_layer("normal", "square.png");
  const std::string p_b_fl_n = queue_bomb_layer("normal", "flare.png");
  const std::string p_b_sq_c = queue_bomb_layer("critical", "square.png");
  const std::string p_b_fl_c = queue_bomb_layer("critical", "flare.png");
  const std::string p_b_sq_s = queue_bomb_layer("scratch", "square.png");
  const std::string p_b_fl_s = queue_bomb_layer("scratch", "flare.png");
  const std::string p_b_sq_h = queue_bomb_layer("hold", "square.png");
  const std::string p_b_fl_h = queue_bomb_layer("hold", "flare.png");
  const std::string p_b_sq_o = queue_bomb_layer("sound", "square.png");
  const std::string p_b_fl_o = queue_bomb_layer("sound", "flare.png");

  const std::string p_combo_text = queue_optional(cache, dir, {"Sirius Combo AP.png"});
  const char* combo_digit_names[10] = {
      "Sirius Combo AP 0.png", "Sirius Combo AP 1.png", "Sirius Combo AP 2.png",
      "Sirius Combo AP 3.png", "Sirius Combo AP 4.png", "Sirius Combo AP 5.png",
      "Sirius Combo AP 6.png", "Sirius Combo AP 7.png", "Sirius Combo AP 8.png",
      "Sirius Combo AP 9.png"};
  std::string p_combo_digit[10];
  for (int i = 0; i < 10; ++i) {
    p_combo_digit[i] = queue_optional(cache, dir, {combo_digit_names[i]});
  }

  // Bake LongNotesSprite (_IsScratch 0/1, IsTouchMask=0). Texture RGB unused in shader;
  // color + side soft-edge come from UV.x profiles across ribbon width.
  constexpr const char* kHoldBlueKey = "__wds/hold_long_blue";
  constexpr const char* kHoldPurpleKey = "__wds/hold_long_purple";
  {
    // Match HoldLongNotes width (157) so m_Border 10/10 maps 1:1 to UV caps.
    auto bake_hold = [](bool scratch) {
      constexpr int kW = 157;
      constexpr int kH = 8;
      std::vector<unsigned char> px(static_cast<size_t>(kW) * static_cast<size_t>(kH) * 4);
      const float base_r = scratch ? 0.706069827f : 0.265174389f;
      const float base_g = scratch ? 0.26666671f : 0.831479371f;
      const float base_b = scratch ? 0.952941179f : 0.952830195f;
      const float high_r = scratch ? 0.933603287f : 0.617924571f;
      const float high_g = scratch ? 0.619607925f : 1.0f;
      const float high_b = scratch ? 1.0f : 0.906408608f;
      auto sat01 = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
      auto smooth = [](float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
      };
      for (int x = 0; x < kW; ++x) {
        const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(kW);
        const float outer0 = smooth((u - 0.7f) / 0.3f);
        const float outer1 = smooth((u - 0.3f) / -0.3f);
        const float outer_profile = std::min(std::pow(outer0 + outer1, 0.08f), 1.0f);
        const float edge_alpha = std::min((1.0f - outer_profile) * 200.0f, 1.0f);

        const float wide0 = smooth((u - 0.25f) / 0.75f);
        const float wide1 = smooth((u - 0.75f) / -0.75f);
        const float inner_profile = std::min(std::pow(wide0 + wide1, 1.2f), 1.0f);
        const float brightness = sat01(inner_profile * 0.5f + 0.3f);
        const float alpha = edge_alpha * brightness;

        const float r = base_r + high_r * (inner_profile * 0.5f);
        const float g = base_g + high_g * (inner_profile * 0.5f);
        const float b = base_b + high_b * (inner_profile * 0.5f);
        const unsigned char R = static_cast<unsigned char>(sat01(r) * 255.0f + 0.5f);
        const unsigned char G = static_cast<unsigned char>(sat01(g) * 255.0f + 0.5f);
        const unsigned char B = static_cast<unsigned char>(sat01(b) * 255.0f + 0.5f);
        const unsigned char A = static_cast<unsigned char>(sat01(alpha) * 255.0f + 0.5f);
        for (int y = 0; y < kH; ++y) {
          const size_t i =
              (static_cast<size_t>(y) * static_cast<size_t>(kW) + static_cast<size_t>(x)) * 4;
          px[i + 0] = R;
          px[i + 1] = G;
          px[i + 2] = B;
          px[i + 3] = A;
        }
      }
      return px;
    };
    cache.queue_rgba(kHoldBlueKey, bake_hold(false), 157, 8);
    cache.queue_rgba(kHoldPurpleKey, bake_hold(true), 157, 8);
  }

  constexpr const char* kSoftSplitKey = "__wds/soft_split_line";
  {
    // Wide gaussian glow plate (see split_soft_profile.hpp) — official lines are
    // soft beams that let the judgeline show through, not hard-core 8-tap bars.
    constexpr int kW = kSplitSoftPlateW;
    std::vector<unsigned char> px(static_cast<size_t>(kW) * 4);
    for (int x = 0; x < kW; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(kW);
      const float a = split_soft_edge_alpha(u);
      px[static_cast<size_t>(x) * 4 + 0] = 255;
      px[static_cast<size_t>(x) * 4 + 1] = 255;
      px[static_cast<size_t>(x) * 4 + 2] = 255;
      px[static_cast<size_t>(x) * 4 + 3] = static_cast<unsigned char>(a * 255.0f + 0.5f);
    }
    cache.queue_rgba(kSoftSplitKey, std::move(px), kW, 1);
  }
  constexpr const char* kSoftDiskKey = "__wds/soft_disk";
  {
    constexpr int kN = 64;
    const float mid = (static_cast<float>(kN) - 1.0f) * 0.5f;
    const float r = mid - 0.5f;
    std::vector<unsigned char> px(static_cast<size_t>(kN) * static_cast<size_t>(kN) * 4);
    for (int y = 0; y < kN; ++y) {
      for (int x = 0; x < kN; ++x) {
        const float dx = static_cast<float>(x) - mid;
        const float dy = static_cast<float>(y) - mid;
        const float d = std::sqrt(dx * dx + dy * dy);
        float a = std::clamp(1.0f - (d - (r - 1.5f)) / 1.5f, 0.0f, 1.0f);
        a = a * a * (3.0f - 2.0f * a);
        const size_t i =
            (static_cast<size_t>(y) * static_cast<size_t>(kN) + static_cast<size_t>(x)) * 4;
        px[i + 0] = 255;
        px[i + 1] = 255;
        px[i + 2] = 255;
        px[i + 3] = static_cast<unsigned char>(a * 255.0f);
      }
    }
    cache.queue_rgba(kSoftDiskKey, std::move(px), kN, kN);
  }

  if (!cache.bake_atlas()) {
    return false;
  }

  stage = p_stage.empty() ? TextureInfo{} : cache.load_standalone_png(p_stage);
  stage_background =
      p_stage_bg.empty() ? TextureInfo{} : cache.load_standalone_png(p_stage_bg);
  judgeline = cache.get(p_judgeline);

  // Official Texture2D/ingame_bg.png — large plate, keep out of the atlas.
  const std::string p_ingame_bg =
      path_to_utf8(dir / "ingame_bg.png");
  if (is_regular_file_utf8(p_ingame_bg)) {
    ingame_background = cache.load_standalone_png(p_ingame_bg);
  } else {
    ingame_background = {};
  }

  note_bottom = cache.get(p_bottom);
  note_red_top = cache.get(p_red);
  note_yellow_top = cache.get(p_yel);
  note_blue_top = cache.get(p_blu);
  note_purple_top = cache.get(p_pur);

  hold_connection_blue = cache.get(kHoldBlueKey);
  hold_connection_purple = cache.get(kHoldPurpleKey);
  sync_line = cache.get(p_sync);
  scratch_arrow = cache.get(p_arrow);
  tick_blue = cache.get(p_tick_b);
  tick_purple = cache.get(p_tick_p);
  hidden_line = cache.get(p_hidden);
  split_line_1 = cache.get(p_split1);
  split_line_2 = cache.get(p_split2);
  split_line_trans1 = cache.get(p_split_t1);
  split_line_trans2 = cache.get(p_split_t2);
  split_lines.bind_after_bake(cache, skins_directory);
  judge_auto = cache.get(p_auto);
  judge_perfect_plus = cache.get(p_pp);
  flick_circle = cache.get(p_flick_c);
  flick_star = cache.get(p_flick_s);
  effect_linear_bg = cache.get(p_lin_bg);
  effect_linear_line = cache.get(p_lin_line);
  effect_linear_star = cache.get(p_lin_star);
  effect_circular = flick_circle ? flick_circle : cache.get(p_flick_s);
  bomb_square_normal = cache.get(p_b_sq_n);
  bomb_flare_normal = cache.get(p_b_fl_n);
  bomb_square_critical = cache.get(p_b_sq_c);
  bomb_flare_critical = cache.get(p_b_fl_c);
  bomb_square_scratch = cache.get(p_b_sq_s);
  bomb_flare_scratch = cache.get(p_b_fl_s);
  bomb_square_hold = cache.get(p_b_sq_h);
  bomb_flare_hold = cache.get(p_b_fl_h);
  bomb_square_sound = cache.get(p_b_sq_o);
  bomb_flare_sound = cache.get(p_b_fl_o);
  combo_ap_text = cache.get(p_combo_text);
  for (int i = 0; i < 10; ++i) {
    combo_ap_digit[i] = cache.get(p_combo_digit[i]);
  }
  soft_split_line = cache.get(kSoftSplitKey);
  soft_disk = cache.get(kSoftDiskKey);

  note_slice_border_l = 65.0f;
  note_slice_border_r = 65.0f;
  hold_slice_border_l = 10.0f;
  hold_slice_border_r = 10.0f;
  note_slice_tex_h = 108.0f;

  return static_cast<bool>(stage) && static_cast<bool>(judgeline) &&
         static_cast<bool>(note_bottom) && static_cast<bool>(note_red_top) &&
         static_cast<bool>(note_yellow_top) && static_cast<bool>(note_blue_top) &&
         static_cast<bool>(note_purple_top) && static_cast<bool>(hold_connection_blue) &&
         static_cast<bool>(hold_connection_purple) && static_cast<bool>(sync_line) &&
         static_cast<bool>(scratch_arrow) && static_cast<bool>(tick_blue) &&
         static_cast<bool>(tick_purple);
}

bool SkinCatalog::bomb_light_for(const char* type_dir, TextureInfo& square,
                                 TextureInfo& flare) const noexcept {
  auto pick = [&](const TextureInfo& typed, const TextureInfo& normal) -> TextureInfo {
    return typed ? typed : normal;
  };
  square = {};
  flare = {};
  if (type_dir != nullptr) {
    const std::string t(type_dir);
    if (t == "critical") {
      square = pick(bomb_square_critical, bomb_square_normal);
      flare = pick(bomb_flare_critical, bomb_flare_normal);
    } else if (t == "scratch") {
      square = pick(bomb_square_scratch, bomb_square_normal);
      flare = pick(bomb_flare_scratch, bomb_flare_normal);
    } else if (t == "hold") {
      square = pick(bomb_square_hold, bomb_square_normal);
      flare = pick(bomb_flare_hold, bomb_flare_normal);
    } else if (t == "sound") {
      square = pick(bomb_square_sound, bomb_square_normal);
      flare = pick(bomb_flare_sound, bomb_flare_normal);
    } else {
      square = bomb_square_normal;
      flare = bomb_flare_normal;
    }
  } else {
    square = bomb_square_normal;
    flare = bomb_flare_normal;
  }
  return static_cast<bool>(square);
}

}  // namespace wds::renderer
