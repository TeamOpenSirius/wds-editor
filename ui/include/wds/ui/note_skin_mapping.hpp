#pragma once

#include <wds/chart_render/skin_catalog.hpp>
#include <wds/core/types.hpp>
#include <wds/renderer/draw_batch.hpp>

namespace wds::ui {

// Shared NoteType → skin sprite mapping for preview and edit canvases.
struct NoteSprites {
  wds::renderer::TextureInfo left;
  wds::renderer::TextureInfo middle;
  wds::renderer::TextureInfo right;
  wds::renderer::TextureInfo connection;
  wds::renderer::TextureInfo tick;
  bool is_scratch_family = false;
  bool is_tick = false;
};

inline NoteSprites sprites_for(const wds::renderer::SkinCatalog& skin,
                               wds::chart_editor::NoteType type) {
  using wds::chart_editor::NoteType;
  NoteSprites s;
  switch (type) {
    case NoteType::Critical:
    case NoteType::CriticalHoldStart:
    case NoteType::CriticalHold:
    case NoteType::NontailCriticalHold:
      s.left = skin.note_yellow_left;
      s.middle = skin.note_yellow_middle;
      s.right = skin.note_yellow_right;
      s.connection = skin.hold_connection_blue;
      break;
    case NoteType::HoldStart:
    case NoteType::Hold:
    case NoteType::NontailHold:
    case NoteType::BlueTap:
      s.left = skin.note_blue_left;
      s.middle = skin.note_blue_middle;
      s.right = skin.note_blue_right;
      s.connection = skin.hold_connection_blue;
      break;
    // HoldEighth is an invisible soft-judge point in Sirius.
    case NoteType::HoldEighth:
      break;
    // Sirius ScratchHoldStart → NormalNote (red tap). Only the flick/end is purple.
    case NoteType::ScratchHoldStart:
      s.left = skin.note_red_left;
      s.middle = skin.note_red_middle;
      s.right = skin.note_red_right;
      s.connection = skin.hold_connection_purple;
      break;
    case NoteType::ScratchCriticalHoldStart:
      s.left = skin.note_yellow_left;
      s.middle = skin.note_yellow_middle;
      s.right = skin.note_yellow_right;
      s.connection = skin.hold_connection_purple;
      break;
    case NoteType::Scratch:
    case NoteType::ScratchHold:
    case NoteType::ScratchCriticalHold:
    case NoteType::NontailScratchHold:
    case NoteType::NontailScratchCriticalHold:
    case NoteType::Flick:
      s.left = skin.note_purple_left;
      s.middle = skin.note_purple_middle;
      s.right = skin.note_purple_right;
      s.connection = skin.hold_connection_purple;
      s.is_scratch_family = true;
      break;
    case NoteType::Sound:
      s.tick = skin.tick_blue;
      s.is_tick = true;
      break;
    case NoteType::SoundPurple:
      s.tick = skin.tick_purple;
      s.is_tick = true;
      break;
    case NoteType::Normal:
    default:
      s.left = skin.note_red_left;
      s.middle = skin.note_red_middle;
      s.right = skin.note_red_right;
      s.connection = skin.hold_connection_blue;
      break;
  }
  return s;
}

}  // namespace wds::ui
