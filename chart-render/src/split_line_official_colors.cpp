#include <wds/chart_render/split_line_official_colors.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace wds::chart_render {
namespace {

#include "split_line_official_colors.inc"

const OfficialSplitColorEntry* find_entry(int32_t color_id) noexcept {
  const auto begin = std::begin(kOfficialSplitColors);
  const auto end = std::end(kOfficialSplitColors);
  const auto it = std::lower_bound(
      begin, end, color_id,
      [](const OfficialSplitColorEntry& e, int32_t id) { return e.id < id; });
  if (it == end || it->id != color_id) {
    return nullptr;
  }
  return &(*it);
}

}  // namespace

bool official_split_line_color(int32_t color_id, int32_t official_slot, float& r, float& g,
                               float& b, float& a) noexcept {
  const OfficialSplitColorEntry* entry = find_entry(color_id);
  if (entry == nullptr || entry->slot_count == 0 || entry->rgba == nullptr) {
    return false;
  }
  const int32_t n = static_cast<int32_t>(entry->slot_count);
  int32_t slot = official_slot % n;
  if (slot < 0) {
    slot += n;
  }
  const float* rgba = entry->rgba + static_cast<size_t>(slot) * 4u;
  r = rgba[0];
  g = rgba[1];
  b = rgba[2];
  a = rgba[3];
  return true;
}

std::vector<int32_t> official_split_color_ids() {
  std::vector<int32_t> out;
  out.reserve(std::size(kOfficialSplitColors));
  for (const auto& entry : kOfficialSplitColors) {
    out.push_back(entry.id);
  }
  return out;
}

}  // namespace wds::chart_render
