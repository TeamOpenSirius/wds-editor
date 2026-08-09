#include <wds/chart_render/note_draw_order.hpp>
#include <wds/core/types.hpp>

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
  using wds::chart_editor::NoteType;
  using wds::chart_render::compare_generate_note_id;
  using wds::chart_render::is_visually_above;
  using wds::chart_render::build_draw_order_indices;
  using wds::chart_render::NoteVisualPass;

  // Pass enum order matches official sandwich.
  assert(static_cast<int>(NoteVisualPass::HoldBody) <
         static_cast<int>(NoteVisualPass::FlatBottom));
  assert(static_cast<int>(NoteVisualPass::FlatBottom) <
         static_cast<int>(NoteVisualPass::FlatTop));
  assert(static_cast<int>(NoteVisualPass::FlatTop) <
         static_cast<int>(NoteVisualPass::MidStar));
  assert(static_cast<int>(NoteVisualPass::MidStar) <
         static_cast<int>(NoteVisualPass::Arrow));

  // Same time: higher NoteType comes first in GenerateNoteId.
  assert(compare_generate_note_id(100, static_cast<int32_t>(NoteType::Hold), 100,
                                  static_cast<int32_t>(NoteType::Normal)));
  assert(!compare_generate_note_id(100, static_cast<int32_t>(NoteType::Normal), 100,
                                   static_cast<int32_t>(NoteType::Hold)));

  // Earlier time comes first regardless of type.
  assert(compare_generate_note_id(50, static_cast<int32_t>(NoteType::Normal), 100,
                                  static_cast<int32_t>(NoteType::HoldEighth)));

  // Visually above = earlier in GenerateNoteId (drawn last when reversed).
  assert(is_visually_above(50, static_cast<int32_t>(NoteType::Normal), 100,
                           static_cast<int32_t>(NoteType::Normal)));
  assert(is_visually_above(100, static_cast<int32_t>(NoteType::Hold), 100,
                           static_cast<int32_t>(NoteType::Normal)));

  struct Item {
    int64_t start = 0;
    int32_t type = 0;
  };
  // Indices: 0=late Normal, 1=early Hold, 2=early Normal
  const std::vector<Item> items = {
      {200, static_cast<int32_t>(NoteType::Normal)},
      {100, static_cast<int32_t>(NoteType::Hold)},
      {100, static_cast<int32_t>(NoteType::Normal)},
  };
  std::vector<size_t> draw;
  build_draw_order_indices(
      items.size(), draw, [&](size_t i) { return items[i].start; },
      [&](size_t i) { return items[i].type; });

  // GenerateNoteId: [1 Hold@100, 2 Normal@100, 0 Normal@200]
  // Draw reverse:    [0, 2, 1] — bottom-most first, early Hold on top (drawn last).
  assert(draw.size() == 3);
  assert(draw[0] == 0);
  assert(draw[1] == 2);
  assert(draw[2] == 1);

  return 0;
}
