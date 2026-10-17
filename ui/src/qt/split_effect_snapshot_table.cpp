#include "wds/ui/qt/split_effect_snapshot_data.hpp"

#include <wds/core/gimmick.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>

// Baked static split-line particle indicator table. Generated from the real split-effect sim by
// chart-render/tools/split_effect_snapshot_bake.cpp (see the header of the .inc for how to
// regenerate). Keyed by (color_id, slot): slot = split_color_slot(color_id, split_count, line) %
// slot_count, so every line sharing a slot reuses one block of particles.

namespace wds::ui {
namespace {

struct SnapSlot {
  uint32_t begin;
  uint32_t count;
};

struct SnapId {
  int32_t color_id;
  int32_t slot_count;
  uint32_t first_slot;  // index into kSnapSlots; slot s lives at first_slot + s
};

#include "split_effect_snapshot_table.inc"

}  // namespace

const SplitSnapStripPoint* split_snap_strip_points() {
  return kSnapStripPoints;
}

const SplitSnapParticle* split_snap_particles(int32_t color_id, int32_t split_count, int32_t line_index,
                                              int* count) {
  if (count != nullptr) {
    *count = 0;
  }
  if (split_count < 1 || line_index < 0 || line_index > split_count) {
    return nullptr;
  }
  const SnapId* begin = std::begin(kSnapIds);
  const SnapId* end = std::end(kSnapIds);
  const SnapId* it = std::lower_bound(begin, end, color_id,
                                      [](const SnapId& e, int32_t id) { return e.color_id < id; });
  if (it == end || it->color_id != color_id || it->slot_count < 1) {
    return nullptr;
  }
  const int32_t slot =
      wds::chart_editor::split_color_slot(color_id, split_count, line_index) % it->slot_count;
  if (slot < 0) {
    return nullptr;
  }
  const SnapSlot& row = kSnapSlots[it->first_slot + static_cast<uint32_t>(slot)];
  if (row.count == 0) {
    return nullptr;
  }
  if (count != nullptr) {
    *count = static_cast<int>(row.count);
  }
  return kSnapParticles + row.begin;
}

}  // namespace wds::ui
