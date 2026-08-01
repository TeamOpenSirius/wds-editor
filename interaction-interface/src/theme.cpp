#include "wds/interaction/theme.hpp"

namespace wds::interaction::theme {
namespace {

float g_content_scale = 1.0f;

}  // namespace

float ui_content_scale() noexcept { return g_content_scale; }

void apply_content_scale(float content_scale) noexcept {
  g_content_scale = std::clamp(content_scale, 0.5f, 4.0f);
}

float content_scale_tier(float content_scale) noexcept {
  static constexpr float kTiers[] = {1.0f, 1.25f, 1.5f, 2.0f, 3.0f};
  const float s = std::clamp(content_scale, 0.5f, 4.0f);
  for (float tier : kTiers) {
    if (s <= tier + 0.001f) {
      return tier;
    }
  }
  return kTiers[sizeof(kTiers) / sizeof(kTiers[0]) - 1];
}

}  // namespace wds::interaction::theme
