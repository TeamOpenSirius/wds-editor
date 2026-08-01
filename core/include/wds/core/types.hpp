#pragma once

#include <wds/common/time.hpp>

#include <cstdint>

namespace wds::chart_editor {

// AppConst.NoteType (official CSV column "type" / dump.cs).
// Instantaneous rows use end_tick == 0 in wdschart (official endTime == -1).
enum class NoteType : int32_t {
  // Official HiSpeed row: type=-1, endTime carries speed (not authored in editor).
  HiSpeed = -1,
  None = 0,
  Normal = 10,
  Critical = 20,
  Sound = 30,
  SoundPurple = 31,
  Scratch = 40,
  Flick = 50,
  HoldStart = 80,
  CriticalHoldStart = 81,
  ScratchHoldStart = 82,
  ScratchCriticalHoldStart = 83,
  Hold = 100,
  CriticalHold = 101,
  ScratchHold = 110,
  ScratchCriticalHold = 111,
  // Nontail bodies (Sirius NontailHold* = Hold* + 1000): no end note / no end VFX
  NontailHold = 1100,
  NontailCriticalHold = 1101,
  NontailScratchHold = 1110,
  NontailScratchCriticalHold = 1111,
  BlueTap = 200,
  HoldEighth = 900,
};

// AppConst.GimmickType (official CSV column "gimmickType").
// Official files may write JumpScratch / OneDirection as names.
enum class GimmickType : int32_t {
  None = 0,
  JumpScratch = 1,
  OneDirection = 2,
  Split1 = 11,
  Split2 = 12,
  Split3 = 13,
  Split4 = 14,
  Split5 = 15,
  Split6 = 16,
  FullSplit1 = 31,
  FullSplit2 = 32,
  FullSplit3 = 33,
  FullSplit4 = 34,
  FullSplit5 = 35,
  FullSplit6 = 36,
  LightSplit1 = 51,
  LightSplit2 = 52,
  LightSplit3 = 53,
  LightSplit4 = 54,
  LightSplit5 = 55,
  LightSplit6 = 56,
  IgnoreSplit1 = 71,
  IgnoreSplit2 = 72,
  IgnoreSplit3 = 73,
  IgnoreSplit4 = 74,
  IgnoreSplit5 = 75,
  IgnoreSplit6 = 76,
};

// Alias of the shared timeline playback state (common).
using PreviewPlaybackState = wds::common::PlaybackState;

// Visual state for a note at a specific preview time.
enum class PreviewNoteVisualState {
  Hidden,       // Outside spawn window or already expired
  Approaching,  // Visible and moving toward judge line
  AutoHit,      // Auto mode: judgment moment passed, show hit feedback
  Holding,      // Hold body currently pressed in auto mode
  Missed,       // Passed miss window without hit (editor usually keeps visible briefly)
};

enum class TimelineSyncMode {
  // Preview time is driven only by explicit seek/set calls.
  Manual,
  // Preview time advances with tick() while playing.
  Realtime,
};

// How the current document may be mutated.
enum class ChartEditMode {
  // Native .wdschart — full editor mutations + save.
  Editable,
  // Imported official CSV — preview / export only; mutations rejected.
  OfficialPreviewOnly,
};

// Sentinel for ChartDocument::add_note / set_notes when id should be auto-assigned.
inline constexpr int32_t kAutoNoteId = -1;

}  // namespace wds::chart_editor
