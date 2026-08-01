#include <wds/chart_render/skin_catalog.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace wds::renderer {

namespace {

namespace fs = std::filesystem;

std::string queue_first(TextureCache& cache, const fs::path& dir,
                        std::initializer_list<const char*> names) {
  for (const char* name : names) {
    const fs::path path = dir / name;
    if (fs::exists(path) && cache.queue_png(path.string())) {
      return path.string();
    }
  }
  return {};
}

}  // namespace

bool SkinCatalog::load(TextureCache& cache, const std::string& skins_directory) {
  const fs::path dir(skins_directory);

  const std::string p_stage =
      queue_first(cache, dir, {"_STAGE_COVER.png", "Sirius Stage Cover.png"});
  const std::string p_stage_bg =
      queue_first(cache, dir, {"_STAGE_BOTTOM_BORDER.png", "Sirius Stage Bottom Border.png"});
  const std::string p_judgeline =
      queue_first(cache, dir, {"_JUDGMENT_LINE.png", "Sirius Judgment Line.png"});

  const std::string p_red_l = queue_first(cache, dir, {"Sirius Note Red Left.png"});
  const std::string p_red_m = queue_first(cache, dir, {"Sirius Note Red Middle.png"});
  const std::string p_red_r = queue_first(cache, dir, {"Sirius Note Red Right.png"});

  const std::string p_yel_l = queue_first(cache, dir, {"Sirius Note Yellow Left.png"});
  const std::string p_yel_m = queue_first(cache, dir, {"Sirius Note Yellow Middle.png"});
  const std::string p_yel_r = queue_first(cache, dir, {"Sirius Note Yellow Right.png"});

  const std::string p_blu_l = queue_first(cache, dir, {"Sirius Note Blue Left.png"});
  const std::string p_blu_m = queue_first(cache, dir, {"Sirius Note Blue Middle.png"});
  const std::string p_blu_r = queue_first(cache, dir, {"Sirius Note Blue Right.png"});

  const std::string p_pur_l = queue_first(cache, dir, {"Sirius Note Purple Left.png"});
  const std::string p_pur_m = queue_first(cache, dir, {"Sirius Note Purple Middle.png"});
  const std::string p_pur_r = queue_first(cache, dir, {"Sirius Note Purple Right.png"});

  const std::string p_hold_b =
      queue_first(cache, dir, {"_NOTE_CONNECTION_BLUE.png", "Sirius Note Connection Blue.png"});
  const std::string p_hold_p =
      queue_first(cache, dir, {"_NOTE_CONNECTION_PURPLE.png", "Sirius Note Connection Purple.png"});
  const std::string p_sync = queue_first(
      cache, dir,
      {"_SIMULTANEOUS_CONNECTION_NEUTRAL.png", "Sirius Simultaneous Connection.png",
       "Sirius Horizontal Line.png"});
  const std::string p_arrow =
      queue_first(cache, dir, {"_DIRECTIONAL_MARKER_PURPLE.png", "Sirius Scratch Arrow.png"});
  const std::string p_tick_b =
      queue_first(cache, dir, {"_NOTE_TICK_BLUE.png", "Sirius Note Tick Blue.png"});
  const std::string p_tick_p =
      queue_first(cache, dir, {"_NOTE_TICK_PURPLE.png", "Sirius Note Tick Purple.png"});
  const std::string p_hidden = queue_first(cache, dir, {"Sirius Hidden Line.png"});
  const std::string p_split1 = queue_first(cache, dir, {"Sirius Split Line _1.png"});
  const std::string p_split2 = queue_first(cache, dir, {"Sirius Split Line _2.png"});
  const std::string p_split_t1 =
      queue_first(cache, dir, {"Sirius Split Line Transform 1 _1.png"});
  const std::string p_split_t2 =
      queue_first(cache, dir, {"Sirius Split Line Transform 2 _1.png"});

  // Queue every Sirius Split Line / Transform color variant (tiny 1×256 sprites).
  split_lines.queue_all(cache, skins_directory);

  const std::string p_auto = queue_first(cache, dir, {"Sirius Judgment Auto.png"});
  const std::string p_pp = queue_first(cache, dir, {"Sirius Judgment Perfect+.png"});
  const std::string p_flick_c = queue_first(cache, dir, {"Sirius Flick Circle.png"});
  const std::string p_flick_s = queue_first(cache, dir, {"Sirius Flick Star.png"});
  const std::string p_lin_bg = queue_first(cache, dir, {"Sirius Linear Background.png"});
  const std::string p_lin_line = queue_first(cache, dir, {"Sirius Linear Line.png"});
  const std::string p_lin_star = queue_first(cache, dir, {"Sirius Linear Star.png"});
  const std::string p_combo_text = queue_first(cache, dir, {"Sirius Combo AP.png"});
  const char* combo_digit_names[10] = {
      "Sirius Combo AP 0.png", "Sirius Combo AP 1.png", "Sirius Combo AP 2.png",
      "Sirius Combo AP 3.png", "Sirius Combo AP 4.png", "Sirius Combo AP 5.png",
      "Sirius Combo AP 6.png", "Sirius Combo AP 7.png", "Sirius Combo AP 8.png",
      "Sirius Combo AP 9.png"};
  std::string p_combo_digit[10];
  for (int i = 0; i < 10; ++i) {
    p_combo_digit[i] = queue_first(cache, dir, {combo_digit_names[i]});
  }
  // Circular tap approximation: Flick Circle (Sonolus #NOTE_CIRCULAR_TAP_* equivalent pack)
  // Soft gray split-line fallback (32×1 smoothstep) — same atlas as skins → one bind.
  constexpr const char* kSoftSplitKey = "__wds/soft_split_line";
  {
    constexpr int kW = 32;
    std::vector<unsigned char> px(static_cast<size_t>(kW) * 4);
    for (int x = 0; x < kW; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(kW);
      float a = 1.0f - std::abs(u - 0.5f) * 2.0f;
      a = std::clamp(a, 0.0f, 1.0f);
      a = a * a * (3.0f - 2.0f * a);
      px[static_cast<size_t>(x) * 4 + 0] = 200;
      px[static_cast<size_t>(x) * 4 + 1] = 200;
      px[static_cast<size_t>(x) * 4 + 2] = 200;
      px[static_cast<size_t>(x) * 4 + 3] = static_cast<unsigned char>(a * 255.0f);
    }
    cache.queue_rgba(kSoftSplitKey, std::move(px), kW, 1);
  }
  // Soft white disk (64×64) for UI fill_circle — one tinted sprite instead of per-row strips.
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
        // Soft edge ~1.5px so scaled disks stay AA'd without MSAA.
        float a = std::clamp(1.0f - (d - (r - 1.5f)) / 1.5f, 0.0f, 1.0f);
        a = a * a * (3.0f - 2.0f * a);
        const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(kN) + static_cast<size_t>(x)) * 4;
        px[i + 0] = 255;
        px[i + 1] = 255;
        px[i + 2] = 255;
        px[i + 3] = static_cast<unsigned char>(a * 255.0f);
      }
    }
    cache.queue_rgba(kSoftDiskKey, std::move(px), kN, kN);
  }

  if (p_stage.empty() || p_judgeline.empty() || p_red_m.empty()) {
    return false;
  }
  if (!cache.bake_atlas()) {
    return false;
  }

  stage = cache.get(p_stage);
  stage_background = cache.get(p_stage_bg);
  judgeline = cache.get(p_judgeline);

  note_red_left = cache.get(p_red_l);
  note_red_middle = cache.get(p_red_m);
  note_red_right = cache.get(p_red_r);

  note_yellow_left = cache.get(p_yel_l);
  note_yellow_middle = cache.get(p_yel_m);
  note_yellow_right = cache.get(p_yel_r);

  note_blue_left = cache.get(p_blu_l);
  note_blue_middle = cache.get(p_blu_m);
  note_blue_right = cache.get(p_blu_r);

  note_purple_left = cache.get(p_pur_l);
  note_purple_middle = cache.get(p_pur_m);
  note_purple_right = cache.get(p_pur_r);

  hold_connection_blue = cache.get(p_hold_b);
  hold_connection_purple = cache.get(p_hold_p);
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
  combo_ap_text = cache.get(p_combo_text);
  for (int i = 0; i < 10; ++i) {
    combo_ap_digit[i] = cache.get(p_combo_digit[i]);
  }
  soft_split_line = cache.get(kSoftSplitKey);
  soft_disk = cache.get(kSoftDiskKey);

  return static_cast<bool>(stage) && static_cast<bool>(judgeline) &&
         static_cast<bool>(note_red_middle);
}

}  // namespace wds::renderer
