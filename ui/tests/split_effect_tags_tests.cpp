#include "wds/ui/regions/edit/edit_gutters.hpp"
#include "wds/ui/regions/edit/split_effect_tags.hpp"

#include <wds/chart_render/split_line_official_colors.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* expr, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    ++g_failures;
  }
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b) check((a) == (b), #a " == " #b, __FILE__, __LINE__)

using namespace wds::ui;

void test_pure_line_ids() {
  const std::set<int32_t> expected = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10393, 11771};
  std::set<int32_t> actual;
  for (int32_t id : wds::chart_render::official_split_color_ids()) {
    const uint32_t tags = split_effect_tags(id);
    if ((tags & kSplitTagLineOnly) != 0) {
      actual.insert(id);
      CHECK_EQ(tags, kSplitTagLineOnly);  // pure line carries no other tag
    }
  }
  CHECK(actual == expected);
  for (int32_t id = 1; id <= 9; ++id) CHECK_EQ(split_effect_tags(id), kSplitTagLineOnly);
  CHECK_EQ(split_effect_tags(-12345), 0u);
  CHECK_EQ(split_effect_tags(0), 0u);
}

void test_layer_tags_cover_ids() {
  // Every official id is known to the table; non-pure-line ids only use defined bits.
  uint32_t seen = 0;
  for (int32_t id : wds::chart_render::official_split_color_ids()) {
    const uint32_t tags = split_effect_tags(id);
    seen |= tags;
    CHECK((tags & kSplitTagLayerMask & ~0x7Fu) == 0);
  }
  for (const auto& tag : split_effect_layer_tags()) {
    CHECK((seen & tag.bit) != 0);  // each layer chip matches at least one effect
  }
}

void test_tag_lists() {
  const auto& layers = split_effect_layer_tags();
  CHECK_EQ(layers.size(), size_t{7});
  std::set<uint32_t> bits;
  for (const auto& t : layers) {
    CHECK(t.label_utf8 != nullptr && t.label_utf8[0] != '\0');
    CHECK(t.tooltip_utf8 != nullptr && t.tooltip_utf8[0] != '\0');
    CHECK((t.bit & kSplitTagLayerMask) == t.bit);
    CHECK(bits.insert(t.bit).second);
  }
  const auto& themes = split_effect_theme_tags();
  CHECK(!themes.empty());
  CHECK(themes.size() <= 16u);
  std::set<uint32_t> theme_bits;
  for (size_t i = 0; i < themes.size(); ++i) {
    CHECK_EQ(themes[i].bit, kSplitTagThemeBase << i);
    CHECK(themes[i].label_utf8 != nullptr && themes[i].label_utf8[0] != '\0');
    CHECK(themes[i].tooltip_utf8 != nullptr);
    CHECK(theme_bits.insert(themes[i].bit).second);
  }
}

void test_match_semantics() {
  const uint32_t theme0 = kSplitTagThemeBase;
  const uint32_t theme1 = kSplitTagThemeBase << 1;
  const uint32_t tags = kSplitTagSparkle | kSplitTagStreak | theme0;

  SplitTagFilter f;
  CHECK(f.empty());
  CHECK(split_effect_tags_match(tags, f));
  CHECK(split_effect_tags_match(0, f));

  f.layer_any = kSplitTagSparkle | kSplitTagSmoke;  // OR within group
  CHECK(split_effect_tags_match(tags, f));
  f.layer_any = kSplitTagSmoke | kSplitTagRibbon;
  CHECK(!split_effect_tags_match(tags, f));

  f.layer_any = kSplitTagSparkle;
  f.theme_any = theme0 | theme1;  // AND across groups
  CHECK(split_effect_tags_match(tags, f));
  f.theme_any = theme1;
  CHECK(!split_effect_tags_match(tags, f));

  f = SplitTagFilter{};
  f.exclude = kSplitTagStreak;  // AND-NOT
  CHECK(!split_effect_tags_match(tags, f));
  f.exclude = kSplitTagSmoke;
  CHECK(split_effect_tags_match(tags, f));
  CHECK(!f.empty());

  f = SplitTagFilter{};
  f.theme_any = theme0;
  f.exclude = kSplitTagSparkle;
  CHECK(!split_effect_tags_match(tags, f));
}

void test_filter_ids() {
  const auto all = wds::chart_render::official_split_color_ids();
  CHECK(filter_split_picker_color_ids("", SplitTagFilter{}) == all);
  CHECK(filter_split_picker_color_ids("1", SplitTagFilter{}) == filter_split_picker_color_ids("1"));

  SplitTagFilter line_only;
  line_only.layer_any = kSplitTagLineOnly;
  const auto lines = filter_split_picker_color_ids("", line_only);
  CHECK_EQ(lines.size(), size_t{11});

  const auto hit = filter_split_picker_color_ids("103", line_only);
  CHECK(hit == std::vector<int32_t>{10393});

  SplitTagFilter no_line;
  no_line.exclude = kSplitTagLineOnly;
  CHECK_EQ(filter_split_picker_color_ids("", no_line).size(), all.size() - 11);
  CHECK(filter_split_picker_color_ids("00000", line_only).empty());
}

}  // namespace

int main() {
  test_pure_line_ids();
  test_layer_tags_cover_ids();
  test_tag_lists();
  test_match_semantics();
  test_filter_ids();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("split_effect_tags_tests: ok\n");
  return 0;
}
