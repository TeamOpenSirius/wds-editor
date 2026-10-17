#include <wds/chart_render/split_effect_particles.hpp>
#include <wds/chart_render/shuriken/evaluate.hpp>
#include <wds/core/official_playfield.hpp>
#include <wds/core/preview_snapshot.hpp>

#include <array>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

int capability_notifications = 0;
wds::chart_render::shuriken::RuntimeCapabilityStatus last_capability_status =
    wds::chart_render::shuriken::RuntimeCapabilityStatus::Supported;

void capture_capability(const wds::chart_render::SplitEffectParticle&,
                        const wds::chart_render::shuriken::EmitContext&,
                        wds::chart_render::shuriken::RuntimeCapabilityStatus status) noexcept {
  ++capability_notifications;
  last_capability_status = status;
}

void check(bool value, const char* expr, int line) {
  if (!value) {
    std::fprintf(stderr, "FAIL:%d: %s\n", line, expr);
    ++failures;
  }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

wds::chart_editor::PreviewSplitLaneInstance make_2020() {
  wds::chart_editor::PreviewSplitLaneInstance split;
  split.apply_identity(2020, 6, wds::chart_editor::SplitLaneType::Ignore, 2020, 1000, 3000);
  split.apply_state(true, false, 7);
  split.apply_animation(1.0f, 0.0f, 1.0f, 0.0f, 1);
  return split;
}

std::array<int, 4> counts_by_catalog(const std::vector<wds::chart_render::SplitEffectLiveParticle>& p,
                                     int n) {
  std::array<int, 4> counts{};
  for (int i = 0; i < n; ++i) {
    const uint32_t catalog = p[static_cast<size_t>(i)].catalog_index;
    if (catalog >= 49u && catalog <= 52u) {
      ++counts[static_cast<size_t>(catalog - 49u)];
    }
  }
  return counts;
}

void test_playback_keeps_first_sample_rod() {
  auto split = make_2020();
  std::vector<wds::chart_render::SplitEffectLiveParticle> raw(
      static_cast<size_t>(wds::chart_render::kSplitEffectParticlePool));
  std::vector<wds::chart_render::SplitEffectLiveParticle> playback(raw.size());

  wds::chart_render::reset_split_effect_auto_seed_session();
  const int raw_first = wds::chart_render::collect_split_effect_particles(
      split, wds::chart_editor::kOfficialSpritEffectNormal, 0.04, raw.data(),
      static_cast<int>(raw.size()));
  CHECK(raw_first > 0);
  const auto raw_first_by_catalog = counts_by_catalog(raw, raw_first);

  wds::chart_render::SplitEffectPlaybackState state;
  const int playback_first = wds::chart_render::collect_split_effect_particles_playback(
      split, wds::chart_editor::kOfficialSpritEffectNormal, 0.04, state, playback.data(),
      static_cast<int>(playback.size()));
  // OpenWDS OnFadeIn then Simulate includes the first LineHight
  // |Δps.worldPosition|. Playback must keep that RateOverDistance cohort
  // (10030 near-camera halo; 2020 catalog 49..52).
  CHECK(playback_first == raw_first);
  for (const int c : raw_first_by_catalog) CHECK(c > 0);

  const int raw_second = wds::chart_render::collect_split_effect_particles(
      split, wds::chart_editor::kOfficialSpritEffectNormal, 0.06, raw.data(),
      static_cast<int>(raw.size()));
  const auto raw_second_by_catalog = counts_by_catalog(raw, raw_second);
  const int playback_second = wds::chart_render::collect_split_effect_particles_playback(
      split, wds::chart_editor::kOfficialSpritEffectNormal, 0.06, state, playback.data(),
      static_cast<int>(playback.size()));
  const auto playback_second_by_catalog = counts_by_catalog(playback, playback_second);
  CHECK(playback_second == raw_second);
  for (size_t i = 0; i < raw_second_by_catalog.size(); ++i) {
    CHECK(playback_second_by_catalog[i] == raw_second_by_catalog[i]);
  }

  const int raw_again = wds::chart_render::collect_split_effect_particles(
      split, wds::chart_editor::kOfficialSpritEffectNormal, 0.06, raw.data(),
      static_cast<int>(raw.size()));
  CHECK(raw_again == raw_second);
}

void test_reset_and_non_monotonic_lifecycle() {
  auto split = make_2020();
  std::vector<wds::chart_render::SplitEffectLiveParticle> pool(
      static_cast<size_t>(wds::chart_render::kSplitEffectParticlePool));
  wds::chart_render::SplitEffectPlaybackState state;

  CHECK(wds::chart_render::collect_split_effect_particles_playback(
            split, wds::chart_editor::kOfficialSpritEffectNormal, 0.02, state, pool.data(),
            static_cast<int>(pool.size())) > 0);
  CHECK(wds::chart_render::collect_split_effect_particles_playback(
            split, wds::chart_editor::kOfficialSpritEffectNormal, 0.04, state, pool.data(),
            static_cast<int>(pool.size())) > 0);

  // The wrapper refuses an implicit backwards seek.  A caller must reset at
  // Play/seek, which keeps lifecycle policy explicit and deterministic.
  CHECK(wds::chart_render::collect_split_effect_particles_playback(
            split, wds::chart_editor::kOfficialSpritEffectNormal, 0.03, state, pool.data(),
            static_cast<int>(pool.size())) == 0);
  state.reset();
  CHECK(wds::chart_render::collect_split_effect_particles_playback(
            split, wds::chart_editor::kOfficialSpritEffectNormal, 0.04, state, pool.data(),
            static_cast<int>(pool.size())) > 0);
  CHECK(wds::chart_render::collect_split_effect_particles_playback(
            split, wds::chart_editor::kOfficialSpritEffectNormal, 0.06, state, pool.data(),
            static_cast<int>(pool.size())) > 0);
}

void test_multiple_split_events_have_independent_first_samples() {
  auto first = make_2020();
  auto second = make_2020();
  // A snapshot can contain multiple active split events at the same timeline
  // sample. Vary count/type so two overlapping lines keep independent collects.
  second.apply_identity(2020, 5, wds::chart_editor::SplitLaneType::BothEnds, 2020, 1000, 3000);
  second.apply_state(true, false, 7);
  second.apply_animation(1.0f, 0.0f, 1.0f, 0.0f, 1);
  std::vector<wds::chart_render::SplitEffectLiveParticle> pool(
      static_cast<size_t>(wds::chart_render::kSplitEffectParticlePool));
  std::vector<wds::chart_render::SplitEffectLiveParticle> raw(pool.size());
  wds::chart_render::SplitEffectPlaybackState state;
  wds::chart_render::reset_split_effect_auto_seed_session();
  const int raw_second_02 = wds::chart_render::collect_split_effect_particles(
      second, wds::chart_editor::kOfficialSpritEffectNormal, 0.02, raw.data(),
      static_cast<int>(raw.size()));
  const int raw_second_04 = wds::chart_render::collect_split_effect_particles(
      second, wds::chart_editor::kOfficialSpritEffectNormal, 0.04, raw.data(),
      static_cast<int>(raw.size()));
  wds::chart_render::reset_split_effect_auto_seed_session();

  CHECK(wds::chart_render::collect_split_effect_particles_playback(
            first, wds::chart_editor::kOfficialSpritEffectNormal, 0.02, state, pool.data(),
            static_cast<int>(pool.size())) > 0);
  CHECK(wds::chart_render::collect_split_effect_particles_playback(
            second, wds::chart_editor::kOfficialSpritEffectNormal, 0.02, state, pool.data(),
            static_cast<int>(pool.size())) == raw_second_02);

  CHECK(wds::chart_render::collect_split_effect_particles_playback(
            first, wds::chart_editor::kOfficialSpritEffectNormal, 0.04, state, pool.data(),
            static_cast<int>(pool.size())) > 0);
  const int playback_second_04 = wds::chart_render::collect_split_effect_particles_playback(
      second, wds::chart_editor::kOfficialSpritEffectNormal, 0.04, state, pool.data(),
      static_cast<int>(pool.size()));
  CHECK(playback_second_04 == raw_second_04);
}

void test_custom_runtime_capability_is_observation_only() {
  using namespace wds::chart_render;
  using namespace wds::chart_render::shuriken;

  SplitEffectParticle world{};
  world.lifetime = 2.0f;
  world.size = 1.0f;
  world.color_r = world.color_g = world.color_b = world.color_a = 1.0f;
  world.emission = 1.0f;
  world.emission_kind = static_cast<uint8_t>(SplitEffectEmission::Burst);
  world.length_in_sec = 1.0f;
  world.shape_scale_x = world.shape_scale_y = world.shape_scale_z = 1.0f;
  world.simulation_space = static_cast<uint8_t>(SplitEffectSimulationSpace::World);
  SplitEffectParticle custom = world;
  custom.simulation_space = static_cast<uint8_t>(SplitEffectSimulationSpace::Custom);

  EmitContext ctx;
  ctx.color_id = 2020;
  ctx.now_t = 0.25f;
  SplitEffectLiveParticle baseline[1];
  SplitEffectLiveParticle observed[1];
  set_runtime_capability_observer(nullptr);
  const int baseline_n = evaluate(world, ctx, baseline, 1);

  capability_notifications = 0;
  set_runtime_capability_observer(capture_capability);
  const int observed_n = evaluate(world, ctx, observed, 1);
  CHECK(observed_n == baseline_n);
  CHECK(capability_notifications == 1);
  CHECK(last_capability_status == RuntimeCapabilityStatus::Supported);
  if (baseline_n > 0 && observed_n > 0) {
    CHECK(observed[0].world_x == baseline[0].world_x);
    CHECK(observed[0].main_y == baseline[0].main_y);
    CHECK(observed[0].z == baseline[0].z);
  }

  capability_notifications = 0;
  SplitEffectLiveParticle custom_out[1];
  const int custom_n = evaluate(custom, ctx, custom_out, 1);
  CHECK(custom_n == baseline_n);
  CHECK(capability_notifications == 1);
  CHECK(last_capability_status == RuntimeCapabilityStatus::CustomSimulationSpaceUnavailable);
  CHECK(runtime_capability_status(world) == RuntimeCapabilityStatus::Supported);
  CHECK(runtime_capability_status(custom) == RuntimeCapabilityStatus::CustomSimulationSpaceUnavailable);
  if (baseline_n > 0 && custom_n > 0) {
    CHECK(custom_out[0].world_x == baseline[0].world_x);
    CHECK(custom_out[0].main_y == baseline[0].main_y);
    CHECK(custom_out[0].z == baseline[0].z);
  }
  set_runtime_capability_observer(nullptr);
}

}  // namespace

int main() {
  test_playback_keeps_first_sample_rod();
  test_reset_and_non_monotonic_lifecycle();
  test_multiple_split_events_have_independent_first_samples();
  test_custom_runtime_capability_is_observation_only();
  if (failures == 0) {
    std::puts("split_effect_playback_tests passed");
    return 0;
  }
  std::fprintf(stderr, "%d playback test(s) failed\n", failures);
  return 1;
}
