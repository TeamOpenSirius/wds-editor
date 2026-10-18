#include "wds/ui/regions/edit/split_effect_tags.hpp"

#include "wds/ui/regions/edit/edit_gutters.hpp"

#include <algorithm>
#include <iterator>

namespace wds::ui {
namespace {

// kSplitEffectTagRows (sorted by id) and kSplitEffectThemeRows.
#include "split_effect_tags.inc"

}  // namespace

const std::vector<SplitEffectTagInfo>& split_effect_layer_tags() {
  static const std::vector<SplitEffectTagInfo> kTags = {
      // Raw UTF-8 literals (project builds with /utf-8 on MSVC; do not use u8"" here).
      {kSplitTagSparkle, "闪光", "十字星、四角星等闪光粒子"},
      {kSplitTagSmoke, "烟雾", "烟、云、飞溅、火焰"},
      {kSplitTagSolid, "实体", "不发光的实体粒子（花瓣、羽毛等）"},
      {kSplitTagStreak, "流光", "沿速度拉伸的流光粒子"},
      {kSplitTagRibbon, "丝带", "连成一条带的丝带拖尾"},
      {kSplitTagFirework, "烟花", "带拖尾的爆发/环绕粒子"},
      {kSplitTagLineOnly, "纯线", "只有线条，没有粒子"},
  };
  return kTags;
}

const std::vector<SplitEffectTagInfo>& split_effect_theme_tags() {
  static const std::vector<SplitEffectTagInfo> kTags = [] {
    std::vector<SplitEffectTagInfo> v;
    v.reserve(std::size(kSplitEffectThemeRows));
    for (const auto& row : kSplitEffectThemeRows) {
      v.push_back({row.bit, row.label, row.tooltip});
    }
    return v;
  }();
  return kTags;
}

uint32_t split_effect_tags(int32_t color_id) noexcept {
  const auto* begin = std::begin(kSplitEffectTagRows);
  const auto* end = std::end(kSplitEffectTagRows);
  const auto* it = std::lower_bound(
      begin, end, color_id,
      [](const SplitEffectTagRow& row, int32_t id) { return row.id < id; });
  if (it == end || it->id != color_id) return 0;
  return it->tags;
}

bool split_effect_tags_match(uint32_t tags, const SplitTagFilter& filter) noexcept {
  if (filter.layer_any != 0 && (tags & filter.layer_any) == 0) return false;
  if (filter.theme_any != 0 && (tags & filter.theme_any) == 0) return false;
  if ((tags & filter.exclude) != 0) return false;
  return true;
}

std::vector<int32_t> filter_split_picker_color_ids(const std::string& query,
                                                   const SplitTagFilter& filter) {
  std::vector<int32_t> ids = filter_split_picker_color_ids(query);
  if (filter.empty()) return ids;
  ids.erase(std::remove_if(ids.begin(), ids.end(),
                           [&](int32_t id) {
                             return !split_effect_tags_match(split_effect_tags(id), filter);
                           }),
            ids.end());
  return ids;
}

}  // namespace wds::ui
