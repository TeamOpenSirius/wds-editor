#include <wds/chart_render/preview_visual_config.hpp>
#include <wds/chart_render/split_line_fade.hpp>
#include <wds/chart_render/split_line_official_colors.hpp>
#include <wds/chart_render/split_soft_profile.hpp>

#include <cmath>
#include <cstdio>

namespace {

using wds::chart_render::official_split_color_ids;
using wds::chart_render::official_split_line_color;
using wds::chart_render::split_line_pulse_peak;
using wds::chart_render::split_line_whiten_t;

int g_fails = 0;

void check(bool cond, const char* expr, const char* file, int line) {
  if (!cond) {
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    ++g_fails;
  }
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

bool near4(int32_t id, int32_t slot, float r, float g, float b, float a) {
  float cr = 0, cg = 0, cb = 0, ca = -1;
  if (!official_split_line_color(id, slot, cr, cg, cb, ca)) {
    return false;
  }
  return std::fabs(cr - r) < 0.02f && std::fabs(cg - g) < 0.02f && std::fabs(cb - b) < 0.02f &&
         std::fabs(ca - a) < 0.02f;
}

void test_4txt_official_element_slots() {
  // Official prefab Line index (before world X flip).
  CHECK(near4(11611, 0, 1.0f, 0.25f, 0.25f, 1.0f));
  CHECK(near4(11611, 1, 0.0f, 0.0f, 0.0f, 0.0f));
  CHECK(near4(11612, 5, 0.2123f, 0.3401f, 1.0f, 1.0f));
  CHECK(near4(11612, 1, 0.0f, 0.0f, 0.0f, 0.0f));
  CHECK(near4(11616, 1, 1.0f, 0.6332f, 0.3915f, 1.0f));
  CHECK(near4(11613, 2, 1.0f, 0.9373f, 0.3066f, 1.0f));
  CHECK(near4(11614, 3, 0.3066f, 1.0f, 0.3847f, 1.0f));
  CHECK(near4(11615, 4, 0.8239f, 0.3632f, 1.0f, 1.0f));
  CHECK(near4(11617, 6, 1.0f, 1.0f, 1.0f, 1.0f));
  CHECK(near4(10390, 0, 0.0f, 0.7373f, 0.1765f, 1.0f));
  CHECK(near4(10390, 3, 0.0f, 0.7373f, 0.1765f, 1.0f));  // broadcast

  // 10518 official controller order (10518_5 … 10518_1), not a second mirror.
  CHECK(near4(10518, 0, 0.9098f, 0.3333f, 0.4941f, 1.0f));
  CHECK(near4(10518, 4, 0.3059f, 0.3569f, 0.6588f, 1.0f));

  float r, g, b, a;
  CHECK(!official_split_line_color(0, 0, r, g, b, a));

  const auto ids = official_split_color_ids();
  CHECK(ids.size() == 318);
  CHECK(ids.front() == 1);
}

void test_official_default_split_line_opacity_is_100() {
  // GameSettings ctor / ResetGameSettings: SplitEffectLineOpacity = 100.
  CHECK(std::fabs(wds::renderer::PreviewVisualConfig{}.split_line_opacity - 1.0f) < 1e-5f);
}

void test_apply_split_line_opacity_keeps_alpha() {
  // Official Initialize: RGB *= settings/100; SpriteRenderer.a stays _lineColor.a.
  float r = 1.0f, g = 0.25f, b = 0.25f, a = 1.0f;
  wds::chart_render::apply_split_line_opacity(r, g, b, a, 0.40f, 1.0f);
  CHECK(std::fabs(r - 0.40f) < 1e-5f);
  CHECK(std::fabs(g - 0.10f) < 1e-5f);
  CHECK(std::fabs(b - 0.10f) < 1e-5f);
  CHECK(std::fabs(a - 1.0f) < 1e-5f);

  r = 1.0f;
  g = 0.25f;
  b = 0.25f;
  a = 1.0f;
  wds::chart_render::apply_split_line_opacity(r, g, b, a, 0.40f, 0.5f);
  CHECK(std::fabs(r - 0.40f) < 1e-5f);
  CHECK(std::fabs(a - 0.5f) < 1e-5f);

  r = 1.0f;
  g = 1.0f;
  b = 1.0f;
  a = 0.0f;
  wds::chart_render::apply_split_line_opacity(r, g, b, a, 0.40f, 1.0f);
  CHECK(std::fabs(a - 0.0f) < 1e-5f);
}

void test_write_white_soft_texel() {
  unsigned char px[4] = {12, 34, 56, 78};
  wds::renderer::write_white_soft_texel(px, 0.5f);
  CHECK(px[0] == 255);
  CHECK(px[1] == 255);
  CHECK(px[2] == 255);
  CHECK(px[3] == 128);
}

void test_whiten_and_pulse_anchor() {
  const float p0 = 0.0f;
  const float p1 = 0.75f;
  const float span = 0.75f * 0.82f;
  // 10392 / tip-grow fadeIn: growing head is p1 (judge).
  CHECK(split_line_whiten_t(p1, p0, p1, span, true) > 0.85f);
  CHECK(split_line_whiten_t(p0, p0, p1, span, true) < 0.15f);
  // 10390 / identity fadeIn uses [1-scale, 1]; mid sample near p0 is the head.
  const float b0 = 0.25f;
  const float b1 = 1.0f;
  CHECK(split_line_whiten_t(b0, b0, b1, span, false) > 0.85f);
  CHECK(split_line_whiten_t(b1, b0, b1, span, false) < 0.15f);

  CHECK(std::fabs(split_line_pulse_peak(0.2f, false) - 0.8f) < 1e-5f);
  CHECK(std::fabs(split_line_pulse_peak(0.2f, true) - 0.2f) < 1e-5f);
}

}  // namespace

int main() {
  test_4txt_official_element_slots();
  test_official_default_split_line_opacity_is_100();
  test_apply_split_line_opacity_keeps_alpha();
  test_write_white_soft_texel();
  test_whiten_and_pulse_anchor();
  if (g_fails == 0) {
    std::printf("All split_line official color tests passed.\n");
    return 0;
  }
  std::printf("%d test(s) failed.\n", g_fails);
  return 1;
}
