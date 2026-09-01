#include <wds/chart_render/note_visual_policy.hpp>

#include <algorithm>
#include <cmath>

namespace wds::chart_render {

HoldTailLayers hold_tail_layers(const wds::renderer::SkinCatalog& skin,
                                bool scratch_hold) noexcept {
  HoldTailLayers out;
  out.bottom = skin.note_bottom;
  if (scratch_hold) {
    out.top = skin.note_purple_top;
    out.is_scratch_family = true;
  } else {
    out.top = skin.note_blue_top;
    out.is_scratch_family = false;
  }
  return out;
}

std::vector<ArrowInstance> layout_static_scratch_arrows(const StaticArrowLayoutParams& p) {
  std::vector<ArrowInstance> out;
  const float aw = p.arrow_w;
  const float span = p.span_right - p.span_left;
  if (aw <= 1e-5f || span <= 1e-5f) {
    return out;
  }
  const auto sides = scratch_arrow_sides(p.scratch_length);
  const float step = (p.scratch_length == 0) ? aw * 0.9f : aw * 0.55f;
  const int max_n = std::max(1, static_cast<int>(span / std::max(step, 1e-5f)) + 1);
  const float mid = p.span_left + span * 0.5f;

  if (sides.draw_left) {
    int drawn = 0;
    for (int i = 0; i < max_n; ++i) {
      const float ax = p.span_left + static_cast<float>(i) * step;
      if (ax + aw > p.span_right + 0.5f) break;
      if (p.scratch_length == 0 && ax + aw > mid + 0.5f) break;
      out.push_back(ArrowInstance{ax, ax + aw, false, 1.0f});
      ++drawn;
    }
    if (drawn == 0) {
      out.push_back(ArrowInstance{p.span_left, p.span_left + aw, false, 1.0f});
    }
  }
  if (sides.draw_right) {
    int drawn = 0;
    for (int i = 0; i < max_n; ++i) {
      const float ax = p.span_right - aw - static_cast<float>(i) * step;
      if (ax < p.span_left - 0.5f) break;
      if (p.scratch_length == 0 && ax < mid - 0.5f) break;
      out.push_back(ArrowInstance{ax, ax + aw, true, 1.0f});
      ++drawn;
    }
    if (drawn == 0) {
      const float ax = p.span_right - aw;
      out.push_back(ArrowInstance{ax, ax + aw, true, 1.0f});
    }
  }
  return out;
}

std::vector<ArrowInstance> layout_animated_scratch_arrows(const AnimatedArrowLayoutParams& p) {
  std::vector<ArrowInstance> out;
  const float L = p.span_left;
  const float R = p.span_right;
  const float W = p.arrow_w;
  if (W <= 1e-5f || R <= L) {
    return out;
  }
  const auto sides = scratch_arrow_sides(p.scratch_length);
  // Official NotesArrowsObject: pivot-center sprites, local x = i * interval,
  // parent scale 0.7, NotesRight Y-180. Flick parent at ±(GetNoteWidth/2 - 0.145).
  // ActivateArrowSpriteRenderer enables the same count on both arrays; OneDirection
  // only SetActive's one GO. No UV / sprite-mask clip on the note — the first
  // head naturally sits ~0.09wu past the notation edge.
  const float step = (p.arrow_step > 1e-5f) ? p.arrow_step : W * 0.5f;
  const int table_count = std::max(
      1, p.arrow_count > 0 ? p.arrow_count
                           : static_cast<int>(std::lround(std::max(p.sonolus_num, 1.0f))));
  const float half = W * 0.5f;
  const float mid = (L + R) * 0.5f;
  const float offset = (p.group_offset > 1e-5f) ? p.group_offset : ((R - L) * 0.5f - half);
  const bool fill = p.fill_to_far_edge && (sides.draw_left != sides.draw_right);

  auto count_toward = [&](float parent, bool toward_positive, float far_edge) {
    if (!fill) {
      return table_count;
    }
    int n = 0;
    for (int i = 0; i < 64; ++i) {
      const float cx =
          parent + (toward_positive ? 1.0f : -1.0f) * static_cast<float>(i) * step;
      const float tail = toward_positive ? (cx + half) : (cx - half);
      if (toward_positive && tail > far_edge + 1e-4f) {
        break;
      }
      if (!toward_positive && tail < far_edge - 1e-4f) {
        break;
      }
      n = i + 1;
    }
    return std::max(1, n);
  };

  const int left_count = sides.draw_left ? count_toward(mid - offset, true, R) : 0;
  const int right_count = sides.draw_right ? count_toward(mid + offset, false, L) : 0;
  const float n = static_cast<float>(std::max({1, left_count, right_count}));

  auto arrow_alpha = [&](float i) {
    const float phase =
        std::fmod(i + static_cast<float>(p.anim_time_sec) * p.arrow_speed, n);
    return 1.0f - 0.8f * phase / n;
  };

  auto emit = [&](float cx, bool flip, int index) {
    const float x0 = flip ? (cx + half) : (cx - half);
    const float x1 = flip ? (cx - half) : (cx + half);
    out.push_back(
        ArrowInstance{x0, x1, flip, arrow_alpha(static_cast<float>(index + 1)), 0.0f, 1.0f});
  };

  if (sides.draw_left) {
    const float parent = mid - offset;
    for (int i = 0; i < left_count; ++i) {
      emit(parent + static_cast<float>(i) * step, false, i);
    }
  }
  if (sides.draw_right) {
    const float parent = mid + offset;
    for (int i = 0; i < right_count; ++i) {
      emit(parent - static_cast<float>(i) * step, true, i);
    }
  }
  return out;
}

}  // namespace wds::chart_render
