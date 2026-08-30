#include "wds/renderer/texture.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

int failures = 0;

void check(bool cond, const char* expr, const char* file, int line) {
  if (!cond) {
    std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", expr, file, line);
    ++failures;
  }
}

template <typename A, typename B>
void check_eq(const A& a, const B& b, const char* expr_a, const char* expr_b, const char* file,
              int line) {
  if (!(a == b)) {
    std::fprintf(stderr, "CHECK_EQ failed: %s == %s (%s:%d)\n", expr_a, expr_b, file, line);
    ++failures;
  }
}

void check_near(float actual, float expected, const char* expr, const char* file, int line) {
  if (std::fabs(actual - expected) > 1.0e-6f) {
    std::fprintf(stderr, "CHECK_NEAR failed: %s (%s:%d) got %g expected %g\n", expr, file, line,
                 static_cast<double>(actual), static_cast<double>(expected));
    ++failures;
  }
}

#define CHECK(cond) ::check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::check_eq((a), (b), #a, #b, __FILE__, __LINE__)
#define CHECK_NEAR(actual, expected) ::check_near((actual), (expected), #actual, __FILE__, __LINE__)

struct FakeGpu {
  bool ready = true;
  int fail_at_create = -1;
  int create_count = 0;
  wds::renderer::TextureId next_id = 1;
  std::vector<wds::renderer::TextureId> created;
  std::vector<wds::renderer::TextureId> destroyed;
  std::vector<std::string> events;

  wds::renderer::TextureCacheBackend backend() {
    wds::renderer::TextureCacheBackend hooks;
    hooks.ready = [this]() { return ready; };
    hooks.create = [this](const unsigned char*, int width, int height) {
      ++create_count;
      if (fail_at_create >= 0 && create_count == fail_at_create) {
        events.push_back("create:fail");
        return wds::renderer::TextureInfo{};
      }
      wds::renderer::TextureInfo info;
      info.id = next_id++;
      info.width = width;
      info.height = height;
      created.push_back(info.id);
      events.push_back("create:" + std::to_string(info.id));
      return info;
    };
    hooks.destroy = [this](wds::renderer::TextureId id) {
      destroyed.push_back(id);
      events.push_back("destroy:" + std::to_string(id));
    };
    return hooks;
  }
};

std::vector<unsigned char> solid_rgba(int width, int height, unsigned char r, unsigned char g,
                                      unsigned char b, unsigned char a) {
  std::vector<unsigned char> px(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
  for (size_t i = 0; i < px.size(); i += 4) {
    px[i] = r;
    px[i + 1] = g;
    px[i + 2] = b;
    px[i + 3] = a;
  }
  return px;
}

// 1×1 opaque white RGBA PNG.
constexpr unsigned char kPng1x1[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F,
    0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0B, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8,
    0x0F, 0x04, 0x00, 0x09, 0xFB, 0x03, 0xFD, 0xFB, 0x5E, 0x6B, 0x2B, 0x00, 0x00, 0x00, 0x00,
    0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

std::string write_temp_png() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "wds_texture_cache_tests";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::filesystem::path path = dir / "px1.png";
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(kPng1x1), sizeof(kPng1x1));
  out.close();
  if (!out || !std::filesystem::exists(path)) {
    return {};
  }
  return path.string();
}

int count_id(const std::vector<wds::renderer::TextureId>& ids, wds::renderer::TextureId id) {
  int n = 0;
  for (wds::renderer::TextureId x : ids) {
    if (x == id) {
      ++n;
    }
  }
  return n;
}

void expect_2x2_then_1x1_uv(const wds::renderer::TextureInfo& big,
                            const wds::renderer::TextureInfo& tiny) {
  constexpr float kInv = 1.0f / 512.0f;
  CHECK_NEAR(big.u0, (2.0f + 0.5f) * kInv);
  CHECK_NEAR(big.v0, (2.0f + 0.5f) * kInv);
  CHECK_NEAR(big.u1, (2.0f + 2.0f - 0.5f) * kInv);
  CHECK_NEAR(big.v1, (2.0f + 2.0f - 0.5f) * kInv);
  CHECK_NEAR(tiny.u0, (6.0f + 0.5f) * kInv);
  CHECK_NEAR(tiny.v0, (2.0f + 0.5f) * kInv);
  CHECK_NEAR(tiny.u1, (6.0f + 1.0f - 0.5f) * kInv);
  CHECK_NEAR(tiny.v1, (2.0f + 1.0f - 0.5f) * kInv);
}

void test_first_bake_success() {
  FakeGpu gpu;
  wds::renderer::TextureCache cache(gpu.backend());
  CHECK(cache.queue_rgba("big", solid_rgba(2, 2, 255, 0, 0, 255), 2, 2));
  CHECK(cache.queue_rgba("tiny", solid_rgba(1, 1, 0, 255, 0, 255), 1, 1));
  CHECK(cache.bake_atlas());
  CHECK(cache.baked());
  CHECK(gpu.create_count == 1);
  CHECK(gpu.destroyed.empty());

  const auto big = cache.get("big");
  const auto tiny = cache.get("tiny");
  CHECK(big);
  CHECK(tiny);
  CHECK_EQ(big.id, tiny.id);
  CHECK_EQ(big.id, cache.atlas_id());
  CHECK_EQ(big.width, 2);
  CHECK_EQ(big.height, 2);
  CHECK_EQ(tiny.width, 1);
  CHECK_EQ(tiny.height, 1);
  expect_2x2_then_1x1_uv(big, tiny);
}

void test_queue_keeps_baked_usable() {
  FakeGpu gpu;
  wds::renderer::TextureCache cache(gpu.backend());
  CHECK(cache.queue_rgba("a", solid_rgba(1, 1, 1, 0, 0, 255), 1, 1));
  CHECK(cache.bake_atlas());
  const auto before = cache.get("a");
  CHECK(cache.queue_rgba("b", solid_rgba(1, 1, 0, 1, 0, 255), 1, 1));
  CHECK(cache.baked());
  CHECK(cache.atlas_id() == before.id);
  CHECK_EQ(cache.atlas_id(), before.id);
  const auto still = cache.get("a");
  CHECK(still.id == before.id);
  CHECK_EQ(still.id, before.id);
  CHECK_NEAR(still.u0, before.u0);
  CHECK_NEAR(still.v0, before.v0);
  CHECK_NEAR(still.u1, before.u1);
  CHECK_NEAR(still.v1, before.v1);
  CHECK(!cache.get("b"));
}

void test_create_fail_keeps_old_state() {
  FakeGpu gpu;
  wds::renderer::TextureCache cache(gpu.backend());
  CHECK(cache.queue_rgba("old", solid_rgba(2, 2, 9, 8, 7, 255), 2, 2));
  CHECK(cache.bake_atlas());
  const wds::renderer::TextureId old_id = cache.atlas_id();
  const auto old_info = cache.get("old");
  CHECK(old_info);

  gpu.fail_at_create = 2;
  CHECK(cache.queue_rgba("fresh", solid_rgba(1, 1, 3, 4, 5, 255), 1, 1));
  CHECK(!cache.bake_atlas());
  CHECK(cache.baked());
  CHECK(cache.atlas_id() == old_id);
  CHECK_EQ(cache.atlas_id(), old_id);
  CHECK(gpu.destroyed.empty());
  CHECK(count_id(gpu.created, old_id) == 1);

  const auto still = cache.get("old");
  CHECK(still.id == old_id);
  CHECK_EQ(still.id, old_id);
  CHECK_EQ(still.width, old_info.width);
  CHECK_EQ(still.height, old_info.height);
  CHECK_NEAR(still.u0, old_info.u0);
  CHECK_NEAR(still.v0, old_info.v0);
  CHECK_NEAR(still.u1, old_info.u1);
  CHECK_NEAR(still.v1, old_info.v1);
  CHECK(!cache.get("fresh"));
}

void test_retry_success_swaps_then_destroys() {
  FakeGpu gpu;
  wds::renderer::TextureCache cache(gpu.backend());
  CHECK(cache.queue_rgba("old", solid_rgba(2, 2, 1, 2, 3, 255), 2, 2));
  CHECK(cache.bake_atlas());
  const wds::renderer::TextureId old_id = cache.atlas_id();

  gpu.fail_at_create = 2;
  CHECK(cache.queue_rgba("fresh", solid_rgba(1, 1, 4, 5, 6, 255), 1, 1));
  CHECK(!cache.bake_atlas());
  CHECK(cache.atlas_id() == old_id);
  CHECK_EQ(cache.atlas_id(), old_id);
  CHECK(gpu.destroyed.empty());

  gpu.fail_at_create = -1;
  CHECK(cache.bake_atlas());
  CHECK(cache.baked());
  const wds::renderer::TextureId new_id = cache.atlas_id();
  CHECK(new_id != old_id);
  CHECK(cache.get("old"));
  CHECK(cache.get("fresh"));
  CHECK(cache.get("old").id == new_id);
  CHECK(cache.get("fresh").id == new_id);
  CHECK_EQ(cache.get("old").id, new_id);
  CHECK_EQ(cache.get("fresh").id, new_id);
  CHECK_EQ(cache.get("old").width, 2);
  CHECK_EQ(cache.get("fresh").width, 1);
  expect_2x2_then_1x1_uv(cache.get("old"), cache.get("fresh"));
  CHECK(gpu.destroyed.size() == static_cast<size_t>(1));
  CHECK_EQ(gpu.destroyed.size(), static_cast<size_t>(1));
  CHECK(gpu.destroyed.back() == old_id);
  CHECK_EQ(gpu.destroyed.back(), old_id);
  CHECK(gpu.events.size() >= 2);
  CHECK(gpu.events[gpu.events.size() - 2] == ("create:" + std::to_string(new_id)));
  CHECK(gpu.events.back() == ("destroy:" + std::to_string(old_id)));
}

void test_standalone_survives_rebake() {
  FakeGpu gpu;
  wds::renderer::TextureCache cache(gpu.backend());
  CHECK(cache.queue_rgba("atlas", solid_rgba(1, 1, 10, 20, 30, 255), 1, 1));
  CHECK(cache.bake_atlas());

  const std::string png = write_temp_png();
  CHECK(!png.empty());
  const auto standalone = cache.load_standalone_png(png);
  CHECK(standalone);
  CHECK(standalone.id != cache.atlas_id());
  const wds::renderer::TextureId stand_id = standalone.id;

  CHECK(cache.queue_rgba("more", solid_rgba(1, 1, 40, 50, 60, 255), 1, 1));
  CHECK(cache.bake_atlas());
  const auto kept = cache.get(png);
  CHECK(kept);
  CHECK(kept.id == stand_id);
  CHECK_EQ(kept.id, stand_id);
  CHECK(kept.id != cache.atlas_id());
  CHECK(cache.get("atlas"));
  CHECK(cache.get("more"));
  CHECK(count_id(gpu.destroyed, stand_id) == 0);
}

void test_clear_destroys_exactly_once() {
  FakeGpu gpu;
  wds::renderer::TextureCache cache(gpu.backend());
  CHECK(cache.queue_rgba("atlas", solid_rgba(1, 1, 1, 1, 1, 255), 1, 1));
  CHECK(cache.bake_atlas());
  const wds::renderer::TextureId atlas = cache.atlas_id();
  const std::string png = write_temp_png();
  CHECK(!png.empty());
  const auto standalone = cache.load_standalone_png(png);
  CHECK(standalone);
  const wds::renderer::TextureId stand_id = standalone.id;
  CHECK(stand_id != atlas);

  cache.clear();
  CHECK(!cache.baked());
  CHECK(cache.atlas_id() == wds::renderer::kInvalidTextureId);
  CHECK_EQ(cache.atlas_id(), wds::renderer::kInvalidTextureId);
  CHECK(!cache.get("atlas"));
  CHECK(count_id(gpu.destroyed, atlas) == 1);
  CHECK(count_id(gpu.destroyed, stand_id) == 1);
  CHECK(gpu.destroyed.size() == static_cast<size_t>(2));
  CHECK_EQ(count_id(gpu.destroyed, atlas), 1);
  CHECK_EQ(count_id(gpu.destroyed, stand_id), 1);
  CHECK_EQ(gpu.destroyed.size(), static_cast<size_t>(2));

  const size_t after_first = gpu.destroyed.size();
  cache.clear();
  CHECK(gpu.destroyed.size() == after_first);
  CHECK_EQ(gpu.destroyed.size(), after_first);
  CHECK(count_id(gpu.destroyed, atlas) == 1);
  CHECK(count_id(gpu.destroyed, stand_id) == 1);
  CHECK_EQ(count_id(gpu.destroyed, atlas), 1);
  CHECK_EQ(count_id(gpu.destroyed, stand_id), 1);
}

void test_pending_overwrite_and_overflow() {
  FakeGpu gpu;
  wds::renderer::TextureCache cache(gpu.backend());
  CHECK(cache.queue_rgba("sprite", solid_rgba(1, 1, 1, 0, 0, 255), 1, 1));
  CHECK(cache.queue_rgba("sprite", solid_rgba(3, 2, 0, 1, 0, 255), 3, 2));
  CHECK(cache.queue_rgba("keep", solid_rgba(1, 1, 0, 0, 1, 255), 1, 1));
  CHECK(!cache.queue_rgba("", solid_rgba(1, 1, 1, 1, 1, 255), 1, 1));
  CHECK(!cache.queue_rgba("bad", std::vector<unsigned char>(4, 0), 2, 1));
  CHECK(!cache.queue_rgba("overflow", {}, std::numeric_limits<int>::max(),
                         std::numeric_limits<int>::max()));
  CHECK(cache.bake_atlas());
  CHECK(cache.get("sprite").width == 3);
  CHECK_EQ(cache.get("sprite").width, 3);
  CHECK_EQ(cache.get("sprite").height, 2);
  CHECK(cache.get("keep"));
  CHECK(!cache.get("overflow"));
  CHECK(!cache.get("bad"));

  CHECK(cache.queue_rgba("sprite", solid_rgba(4, 1, 9, 9, 9, 255), 4, 1));
  CHECK(cache.baked());
  CHECK(cache.bake_atlas());
  CHECK(cache.get("sprite").width == 4);
  CHECK_EQ(cache.get("sprite").width, 4);
  CHECK_EQ(cache.get("sprite").height, 1);
  CHECK(cache.get("keep"));
}

void test_first_atlas_create_failure() {
  FakeGpu gpu;
  gpu.fail_at_create = 1;
  wds::renderer::TextureCache cache(gpu.backend());
  CHECK(cache.queue_rgba("only", solid_rgba(1, 1, 8, 8, 8, 255), 1, 1));
  CHECK(!cache.bake_atlas());
  CHECK(!cache.baked());
  CHECK(cache.atlas_id() == wds::renderer::kInvalidTextureId);
  CHECK_EQ(cache.atlas_id(), wds::renderer::kInvalidTextureId);
  CHECK(!cache.get("only"));
  CHECK(gpu.destroyed.empty());
  CHECK(gpu.create_count == 1);

  gpu.fail_at_create = -1;
  CHECK(cache.bake_atlas());
  CHECK(cache.baked());
  CHECK(cache.get("only"));
  CHECK(cache.get("only").id == cache.atlas_id());
}

void test_queue_png_source_retained() {
  FakeGpu gpu;
  wds::renderer::TextureCache cache(gpu.backend());
  const std::string png = write_temp_png();
  CHECK(!png.empty());
  CHECK(cache.queue_png(png));
  CHECK(cache.bake_atlas());
  const auto first = cache.get(png);
  CHECK(first);
  CHECK(first.width == 1);
  CHECK(first.height == 1);

  CHECK(cache.queue_rgba("extra", solid_rgba(2, 2, 1, 2, 3, 255), 2, 2));
  CHECK(cache.baked());
  CHECK(cache.bake_atlas());
  const auto again = cache.get(png);
  CHECK(again);
  CHECK(cache.get("extra"));
  CHECK(again.id == cache.atlas_id());
  CHECK(cache.get("extra").id == cache.atlas_id());
  CHECK(again.id != first.id);
  CHECK_EQ(again.width, 1);
  CHECK_EQ(again.height, 1);
  expect_2x2_then_1x1_uv(cache.get("extra"), again);
}

void test_cpu_throw_on_first_bake_skips_create() {
  FakeGpu gpu;
  auto hooks = gpu.backend();
  hooks.before_atlas_cpu_cache = [] { throw std::runtime_error("atlas cpu cache"); };
  wds::renderer::TextureCache cache(hooks);
  CHECK(cache.queue_rgba("only", solid_rgba(1, 1, 1, 1, 1, 255), 1, 1));
  CHECK(cache.bake_atlas() == false);
  CHECK(gpu.create_count == 0);
  CHECK(!cache.baked());
  CHECK(cache.atlas_id() == wds::renderer::kInvalidTextureId);
  CHECK_EQ(cache.atlas_id(), wds::renderer::kInvalidTextureId);
  CHECK(!cache.get("only"));
}

void test_cpu_throw_during_rebake_does_not_create() {
  FakeGpu gpu;
  auto hooks = gpu.backend();
  bool throw_next = false;
  hooks.before_atlas_cpu_cache = [&] {
    if (throw_next) {
      throw std::runtime_error("cpu after first bake");
    }
  };
  wds::renderer::TextureCache cache(hooks);
  CHECK(cache.queue_rgba("old", solid_rgba(2, 2, 9, 8, 7, 255), 2, 2));
  CHECK(cache.bake_atlas());
  const wds::renderer::TextureId old_id = cache.atlas_id();
  const auto old_uv = cache.get("old");
  const int creates_before = gpu.create_count;
  CHECK(cache.queue_rgba("fresh", solid_rgba(1, 1, 3, 4, 5, 255), 1, 1));
  throw_next = true;
  CHECK(cache.bake_atlas() == false);
  CHECK(gpu.create_count == creates_before);
  CHECK(cache.baked());
  CHECK(cache.atlas_id() == old_id);
  CHECK_EQ(cache.atlas_id(), old_id);
  CHECK(cache.get("old").id == old_id);
  CHECK_NEAR(cache.get("old").u0, old_uv.u0);
  CHECK(!cache.get("fresh"));
  CHECK(gpu.destroyed.empty());
}

void test_standalone_commit_rollback_destroys_gpu() {
  std::unordered_set<std::string> keys;
  std::vector<wds::renderer::TextureId> ids;
  std::unordered_map<std::string, wds::renderer::TextureInfo> cache;
  wds::renderer::TextureInfo info;
  info.id = 7;
  info.width = 1;
  info.height = 1;
  bool threw = false;
  try {
    wds::renderer::commit_standalone_records(keys, ids, cache, "k", info, [](int step) {
      if (step == 1) {
        throw std::runtime_error("ids");
      }
    });
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
  CHECK(keys.empty());
  CHECK(ids.empty());
  CHECK(cache.empty());

  FakeGpu gpu;
  auto hooks = gpu.backend();
  hooks.before_standalone_commit = [](int step) {
    if (step == 2) {
      throw std::runtime_error("cache");
    }
  };
  wds::renderer::TextureCache tex(hooks);
  const std::string png = write_temp_png();
  CHECK(!png.empty());
  const auto loaded = tex.load_standalone_png(png);
  CHECK(!loaded);
  CHECK(gpu.created.size() == static_cast<size_t>(1));
  CHECK(gpu.destroyed.size() == static_cast<size_t>(1));
  if (!gpu.created.empty() && !gpu.destroyed.empty()) {
    CHECK(gpu.destroyed[0] == gpu.created[0]);
    CHECK_EQ(gpu.destroyed[0], gpu.created[0]);
  }
  CHECK(!tex.get(png));
}

void test_partial_backend_not_split_and_set_renderer_clears() {
  FakeGpu gpu;
  wds::renderer::TextureCacheBackend partial = gpu.backend();
  partial.destroy = {};
  wds::renderer::TextureCache partial_cache(partial);
  CHECK(partial_cache.queue_rgba("a", solid_rgba(1, 1, 1, 1, 1, 255), 1, 1));
  CHECK(!partial_cache.bake_atlas());
  CHECK(gpu.create_count == 0);
  CHECK(!partial_cache.baked());

  wds::renderer::TextureCache full(gpu.backend());
  CHECK(full.queue_rgba("a", solid_rgba(1, 1, 1, 1, 1, 255), 1, 1));
  full.set_renderer(nullptr);
  CHECK(!full.bake_atlas());
  CHECK(gpu.create_count == 0);
  CHECK(!full.baked());
}

}  // namespace

void run_texture_cache_tests() {
  test_first_bake_success();
  test_queue_keeps_baked_usable();
  test_create_fail_keeps_old_state();
  test_retry_success_swaps_then_destroys();
  test_standalone_survives_rebake();
  test_clear_destroys_exactly_once();
  test_pending_overwrite_and_overflow();
  test_first_atlas_create_failure();
  test_queue_png_source_retained();
  test_cpu_throw_on_first_bake_skips_create();
  test_cpu_throw_during_rebake_does_not_create();
  test_standalone_commit_rollback_destroys_gpu();
  test_partial_backend_not_split_and_set_renderer_clears();

  if (failures != 0) {
    std::fprintf(stderr, "texture_cache_tests: %d CHECK(s) failed\n", failures);
    std::exit(1);
  }
  std::fprintf(stdout, "texture_cache_tests: ok\n");
}
