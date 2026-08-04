#pragma once

#include <wds/audio/hit_sfx.hpp>

#include <wds/core/types.hpp>

namespace wds::ui {

inline wds::audio::HitSfxClip hit_sfx_clip_for_head(wds::chart_editor::NoteType type) noexcept {
  using wds::chart_editor::NoteType;
  switch (type) {
    case NoteType::Critical:
    case NoteType::CriticalHoldStart:
    case NoteType::ScratchCriticalHoldStart:
      return wds::audio::HitSfxClip::Critical;
    case NoteType::Flick:
      return wds::audio::HitSfxClip::Scratch;
    case NoteType::Sound:
    case NoteType::ScratchSound:
      return wds::audio::HitSfxClip::Sound;
    case NoteType::Normal:
    case NoteType::BlueTap:
    case NoteType::HoldStart:
    case NoteType::ScratchHoldStart:
    default:
      return wds::audio::HitSfxClip::Perfect;
  }
}

inline wds::audio::HitSfxClip hit_sfx_clip_for_hold_body_start(
    wds::chart_editor::NoteType body_type) noexcept {
  using wds::chart_editor::NoteType;
  switch (body_type) {
    case NoteType::CriticalHold:
    case NoteType::ScratchCriticalHold:
    case NoteType::NontailCriticalHold:
    case NoteType::NontailScratchCriticalHold:
      return wds::audio::HitSfxClip::Critical;
    case NoteType::Hold:
    case NoteType::ScratchHold:
    case NoteType::NontailHold:
    case NoteType::NontailScratchHold:
      return wds::audio::HitSfxClip::Perfect;
    default:
      return wds::audio::HitSfxClip::Count;
  }
}

inline wds::audio::HitSfxClip hit_sfx_clip_for_hold_tail(
    wds::chart_editor::NoteType body_type) noexcept {
  using wds::chart_editor::NoteType;
  switch (body_type) {
    case NoteType::Hold:
    case NoteType::CriticalHold:
      return wds::audio::HitSfxClip::Perfect;
    case NoteType::ScratchHold:
    case NoteType::ScratchCriticalHold:
      return wds::audio::HitSfxClip::Scratch;
    default:
      return wds::audio::HitSfxClip::Count;
  }
}

inline wds::audio::HitSfxClip hit_sfx_clip_for_mid_star(
    wds::chart_editor::NoteType type) noexcept {
  using wds::chart_editor::NoteType;
  switch (type) {
    case NoteType::Sound:
    case NoteType::ScratchSound:
      return wds::audio::HitSfxClip::Sound;
    default:
      return wds::audio::HitSfxClip::Count;
  }
}

}  // namespace wds::ui
