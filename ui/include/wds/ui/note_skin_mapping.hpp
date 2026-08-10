#pragma once

#include <wds/chart_render/note_visual_policy.hpp>
#include <wds/chart_render/skin_catalog.hpp>
#include <wds/core/types.hpp>
#include <wds/renderer/draw_batch.hpp>

namespace wds::ui {

// Shared NoteType → official Top/Bottom skin mapping for preview and edit.
struct NoteSprites {
  wds::renderer::TextureInfo bottom;
  wds::renderer::TextureInfo top;
  wds::renderer::TextureInfo connection;
  wds::renderer::TextureInfo tick;
  float connection_r = 1.0f;
  float connection_g = 1.0f;
  float connection_b = 1.0f;
  bool is_scratch_family = false;
  bool is_tick = false;
};

// Hold body end caps (edit + preview). Preserves connection tint from `base` when set.
inline void apply_hold_tail_sprites(NoteSprites& tail, const wds::renderer::SkinCatalog& skin,
                                    bool scratch_hold) {
  const auto layers = wds::chart_render::hold_tail_layers(skin, scratch_hold);
  tail.bottom = layers.bottom;
  tail.top = layers.top;
  tail.is_scratch_family = layers.is_scratch_family;
}

inline NoteSprites sprites_for(const wds::renderer::SkinCatalog& skin,
                               wds::chart_editor::NoteType type) {
  using wds::chart_editor::NoteType;
  NoteSprites s;
  s.bottom = skin.note_bottom;
  const auto use_blue_hold = [&] {
    s.connection = skin.hold_connection_blue;
    s.connection_r = 1.0f;
    s.connection_g = 1.0f;
    s.connection_b = 1.0f;
  };
  const auto use_purple_hold = [&] {
    s.connection = skin.hold_connection_purple;
    s.connection_r = 1.0f;
    s.connection_g = 1.0f;
    s.connection_b = 1.0f;
  };

  switch (type) {
    case NoteType::Critical:
    case NoteType::CriticalHoldStart:
    case NoteType::CriticalHold:
    case NoteType::NontailCriticalHold:
      s.top = skin.note_yellow_top;
      use_blue_hold();
      break;
    case NoteType::HoldStart:
    case NoteType::Hold:
    case NoteType::NontailHold:
    case NoteType::BlueTap:
      s.top = skin.note_blue_top;
      use_blue_hold();
      break;
    case NoteType::HoldEighth:
      break;
    case NoteType::ScratchHoldStart:
      s.top = skin.note_red_top;
      use_purple_hold();
      break;
    case NoteType::ScratchCriticalHoldStart:
      s.top = skin.note_yellow_top;
      use_purple_hold();
      break;
    case NoteType::ScratchHold:
    case NoteType::ScratchCriticalHold:
    case NoteType::NontailScratchHold:
    case NoteType::NontailScratchCriticalHold:
    case NoteType::Flick:
      s.top = skin.note_purple_top;
      use_purple_hold();
      s.is_scratch_family = true;
      break;
    case NoteType::Sound:
      s.tick = skin.tick_blue;
      s.is_tick = true;
      break;
    case NoteType::ScratchSound:
      s.tick = skin.tick_purple;
      s.is_tick = true;
      break;
    case NoteType::Normal:
    default:
      s.top = skin.note_red_top;
      use_blue_hold();
      break;
  }
  return s;
}

}  // namespace wds::ui
