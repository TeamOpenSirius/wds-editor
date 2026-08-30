#pragma once

#include <wds/core/notation.hpp>

#include <cstdint>
#include <vector>

namespace wds::chart_editor {

// Inclusive-lane overlap at a shared tick between two included occupancy events.
struct NoteOverlapPair {
  int32_t tick = 0;
  int32_t first_note_id = 0;
  int32_t second_note_id = 0;
};

struct ChartValidationResult {
  // Sorted by (tick, first_note_id, second_note_id). first_note_id < second_note_id.
  std::vector<NoteOverlapPair> pairs;
  // Unique ticks from pairs, ascending.
  std::vector<int32_t> error_ticks;
};

// Pure note-overlap scan. Does not mutate notes or depend on UI.
//
// Included occupancies: ordinary playable starts (taps/flicks/heads) and the
// terminal tail of holds that have a tail (end_tick > start_tick). ScratchHold
// tails use resolve_end_lane_span; ordinary tails use the encoded body span
// (same helper, which falls back to [lane, width]).
//
// Excluded: split gimmicks, HiSpeed/None, hold-body starts, Sound/ScratchSound,
// HoldEighth. Legal head/body pairs and ScratchHold chain joints are not
// reported merely because structural endpoints coincide.
//
// Malformed geometry: width < 1 is clamped to 1; inverted hold duration skips
// the tail; interval math uses int64 to avoid overflow. Unknown types that are
// not excluded contribute a start occupancy.
ChartValidationResult find_note_overlaps(const std::vector<NotationNote>& notes);

}  // namespace wds::chart_editor
