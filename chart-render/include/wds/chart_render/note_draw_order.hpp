#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace wds::chart_render {

// Official flat-note sandwich (Unity SpriteRenderer sortingOrder tiers):
// hold bodies → all bottoms → all tops → mid-stars → arrows.
// Pass order is the stacking contract; z is only a fine bias.
enum class NoteVisualPass : int {
  HoldBody = 0,
  FlatBottom = 1,
  FlatTop = 2,
  MidStar = 3,
  Arrow = 4,
};

// Sirius.Game.NotationNoteProcessor.GenerateNoteId:
//   OrderBy(StartMilliseconds).ThenByDescending(NoteType)
// Returns true if `a` should appear before `b` in that array (smaller Id).
inline bool compare_generate_note_id(int64_t start_a, int32_t type_a, int64_t start_b,
                                     int32_t type_b) noexcept {
  if (start_a != start_b) {
    return start_a < start_b;
  }
  if (type_a != type_b) {
    return type_a > type_b;
  }
  return false;
}

// With depthWrite off, later draw calls win. Iterating GenerateNoteId order in
// reverse puts earlier times (and higher NoteType at the same time) on top.
inline bool is_visually_above(int64_t start_a, int32_t type_a, int64_t start_b,
                              int32_t type_b) noexcept {
  if (start_a != start_b) {
    return start_a < start_b;
  }
  if (type_a != type_b) {
    return type_a > type_b;
  }
  return false;
}

// Sort indices into GenerateNoteId order. Callers draw with rbegin()/rend().
template <typename GetStart, typename GetType>
void sort_indices_generate_note_id(std::vector<size_t>& indices, GetStart&& get_start,
                                   GetType&& get_type) {
  std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
    return compare_generate_note_id(get_start(a), get_type(a), get_start(b), get_type(b));
  });
}

// Fill `out` with [0, count) sorted for draw (bottom-most first = reverse GenerateNoteId).
template <typename GetStart, typename GetType>
void build_draw_order_indices(size_t count, std::vector<size_t>& out, GetStart&& get_start,
                              GetType&& get_type) {
  out.resize(count);
  for (size_t i = 0; i < count; ++i) {
    out[i] = i;
  }
  sort_indices_generate_note_id(out, std::forward<GetStart>(get_start),
                                std::forward<GetType>(get_type));
  std::reverse(out.begin(), out.end());
}

}  // namespace wds::chart_render
