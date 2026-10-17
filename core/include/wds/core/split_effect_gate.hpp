#pragma once

#include <wds/core/gimmick.hpp>
#include <wds/core/official_playfield.hpp>

#include <cstdint>

namespace wds::chart_editor {

// Official OnFadeIn particle gate (so.c 3702795) plus Rich ConvertGimmickType +=20
// (so.c 23911511): Split1–6 → FullSplit1–6 when quality == Rich(0).
//
//   lightSetting = (quality == Light)
//   playType     = (quality == Rich && type == BothEnds) ? Full : type
//   play         = (playType == Ignore) || (!lightSetting && playType != Light)
//   emit i in 0..splitCount unless playType == BothEnds && i != 0 && i != splitCount
//
// Line plates draw at every quality. emit is for LineEffects particles.

inline SplitLaneType official_split_effect_play_type(int quality,
                                                     SplitLaneType type) noexcept {
  if (official_clamp_sprit_effect_setting(quality) == kOfficialSpritEffectRich &&
      type == SplitLaneType::BothEnds) {
    return SplitLaneType::Full;
  }
  return type;
}

inline bool official_split_effect_should_play(int quality, SplitLaneType type,
                                              int32_t split_count) noexcept {
  if (split_count < 0) {
    return false;
  }
  const SplitLaneType play = official_split_effect_play_type(quality, type);
  const bool light_setting =
      official_clamp_sprit_effect_setting(quality) == kOfficialSpritEffectLight;
  return play == SplitLaneType::Ignore || (!light_setting && play != SplitLaneType::Light);
}

inline bool official_split_effect_should_emit(int quality, SplitLaneType type,
                                              int32_t split_count, int32_t index) noexcept {
  if (!official_split_effect_should_play(quality, type, split_count)) {
    return false;
  }
  if (index < 0 || index > split_count) {
    return false;
  }
  const SplitLaneType play = official_split_effect_play_type(quality, type);
  if (play == SplitLaneType::BothEnds && index != 0 && index != split_count) {
    return false;
  }
  return true;
}

template <typename Fn>
inline void official_split_effect_for_each_emit(int quality, SplitLaneType type,
                                                int32_t split_count, Fn&& fn) {
  if (!official_split_effect_should_play(quality, type, split_count)) {
    return;
  }
  const SplitLaneType play = official_split_effect_play_type(quality, type);
  for (int32_t i = 0; i <= split_count; ++i) {
    if (play == SplitLaneType::BothEnds && i != 0 && i != split_count) {
      continue;
    }
    fn(i);
  }
}

}  // namespace wds::chart_editor
