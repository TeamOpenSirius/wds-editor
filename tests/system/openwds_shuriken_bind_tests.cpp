#include <wds/chart_render/openwds_shuriken_bind.hpp>
#include <wds/chart_render/openwds_split_effects/controller.hpp>
#include <wds/chart_render/openwds_split_effects/resources.hpp>
#include <wds/chart_render/shuriken/evaluate.hpp>
#include <wds/chart_render/split_effect_catalog.hpp>
#include <wds/chart_render/unity_api.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#ifndef WDS_TEST_REPO_ROOT
#define WDS_TEST_REPO_ROOT "."
#endif

namespace {

int failures = 0;

void check(bool value, const char* expr, int line) {
  if (!value) {
    std::fprintf(stderr, "FAIL:%d: %s\n", line, expr);
    ++failures;
  }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

bool catalog_is_r6_of(float catalog, float graph) noexcept {
  const double rounded = std::round(static_cast<double>(graph) * 1e6) / 1e6;
  return std::fabs(static_cast<double>(catalog) - rounded) <= 5e-7;
}

struct CompareStats {
  int effects = 0;
  int particles = 0;
  int fields = 0;
  int diff_gt_1e6 = 0;
  int rounding_r6 = 0;
  int missing = 0;
  int int_mismatch = 0;
  std::map<std::string, int> field_diffs;
};

struct FieldDiff {
  int32_t id = 0;
  const char* field = "";
  float graph = 0.0f;
  float catalog = 0.0f;
};

void cmp_f(CompareStats& stats, std::vector<FieldDiff>& diffs, int32_t id, const char* field,
           float graph, float catalog) {
  ++stats.fields;
  const float d = std::fabs(graph - catalog);
  if (d <= 1e-6f) {
    return;
  }
  ++stats.diff_gt_1e6;
  ++stats.field_diffs[field];
  if (catalog_is_r6_of(catalog, graph)) {
    ++stats.rounding_r6;
  }
  if (diffs.size() < 24u) {
    diffs.push_back({id, field, graph, catalog});
  }
}

void cmp_u8(CompareStats& stats, int32_t id, const char* field, uint8_t graph,
            uint8_t catalog) {
  if (graph == catalog) {
    return;
  }
  ++stats.int_mismatch;
  std::fprintf(stderr, "INT %d %s graph=%u catalog=%u\n", static_cast<int>(id), field,
               static_cast<unsigned>(graph), static_cast<unsigned>(catalog));
}

void compare_particle(CompareStats& stats, std::vector<FieldDiff>& diffs, int32_t id,
                      const wds::chart_render::SplitEffectParticle& graph,
                      const wds::chart_render::SplitEffectParticle& catalog) {
  ++stats.particles;
  if (catalog.max_particles > 0 && graph.max_particles == 0) {
    ++stats.missing;
    std::fprintf(stderr, "MISSING %d max_particles graph=0 catalog=%u\n", static_cast<int>(id),
                 static_cast<unsigned>(catalog.max_particles));
  }
  cmp_u8(stats, id, "shape_type", graph.shape_type, catalog.shape_type);
  cmp_u8(stats, id, "render_mode", graph.render_mode, catalog.render_mode);
  cmp_u8(stats, id, "start_color_state", graph.start_color_state, catalog.start_color_state);
  cmp_f(stats, diffs, id, "max_particles", static_cast<float>(graph.max_particles),
        static_cast<float>(catalog.max_particles));
  cmp_f(stats, diffs, id, "length_in_sec", graph.length_in_sec, catalog.length_in_sec);
  cmp_f(stats, diffs, id, "start_delay", graph.start_delay,
        wds::chart_render::official_split_effect_start_delay(catalog));
  cmp_f(stats, diffs, id, "flip_x", graph.flip_x, catalog.flip_x);
  cmp_f(stats, diffs, id, "flip_y", graph.flip_y, catalog.flip_y);
  cmp_u8(stats, id, "go_active", graph.go_active, catalog.go_active);
  cmp_f(stats, diffs, id, "size", graph.size, catalog.size);
  cmp_f(stats, diffs, id, "start_speed", graph.start_speed, catalog.start_speed);
  cmp_f(stats, diffs, id, "length_scale", graph.length_scale, catalog.length_scale);
  cmp_f(stats, diffs, id, "max_particle_size", graph.max_particle_size, catalog.max_particle_size);
  cmp_f(stats, diffs, id, "noise_position_amount", graph.noise_position_amount,
        catalog.noise_position_amount);
  cmp_f(stats, diffs, id, "gravity_modifier", graph.gravity_modifier, catalog.gravity_modifier);
  cmp_f(stats, diffs, id, "color_a_min", graph.color_a_min, catalog.color_a_min);
  cmp_f(stats, diffs, id, "shape_length", graph.shape_length, catalog.shape_length);
  cmp_f(stats, diffs, id, "vol_speed_mod", graph.vol_speed_mod, catalog.vol_speed_mod);
  cmp_f(stats, diffs, id, "orbital_offset_x", graph.orbital_offset_x, catalog.orbital_offset_x);
  cmp_f(stats, diffs, id, "local_pos_x", graph.local_pos_x, catalog.local_pos_x);
  if (catalog.size_3d != 0) {
    cmp_f(stats, diffs, id, "start_size_y", graph.start_size_y, catalog.start_size_y);
  }
  cmp_f(stats, diffs, id, "trail_ratio", graph.trail_ratio, catalog.trail_ratio);
}

void test_hand_built() {
  wds::chart_render::unity::ParticleSystem ps;
  ps.main().maxParticles = 1000;
  ps.main().startSize.mode = wds::chart_render::unity::ParticleSystemCurveMode::Constant;
  ps.main().startSize.constant = 2.0f;
  ps.particleSystemRenderer().lengthScale = 3.0f;
  ps.renderer().lengthScale = 3.0f;
  ps.particleSystemRenderer().flip = {0.5f, 0.25f, 1.0f};

  wds::chart_render::SplitEffectParticle dst{};
  wds::chart_render::bind_from_unity(ps, dst);
  CHECK(dst.max_particles == 1000);
  CHECK(std::fabs(dst.size - 2.0f) <= 1e-6f);
  CHECK(std::fabs(dst.length_scale - 3.0f) <= 1e-6f);
  CHECK(std::fabs(dst.flip_x - 0.5f) <= 1e-6f);
  CHECK(std::fabs(dst.flip_y - 0.25f) <= 1e-6f);
  CHECK(dst.go_active == 1);
  CHECK(dst.size_curve_begin == 0);
  CHECK(dst.size_curve_count == 0);
}

void test_inactive_gameobject_does_not_evaluate() {
  wds::chart_render::unity::ParticleSystem ps;
  ps.gameObject()->SetActive(false);
  ps.emission().enabled = true;
  ps.emission().rateOverTime.constant = 10.0f;
  ps.main().startLifetime.constant = 1.0f;
  ps.main().duration = 1.0f;
  wds::chart_render::SplitEffectParticle dst{};
  wds::chart_render::bind_from_unity(ps, dst);
  CHECK(dst.go_active == 0);
  wds::chart_render::shuriken::EmitContext ctx;
  ctx.now_t = 0.5f;
  ctx.fade_scale = 1.0f;
  ctx.line_alpha = 1.0f;
  std::vector<wds::chart_render::SplitEffectLiveParticle> live(64);
  CHECK(wds::chart_render::shuriken::evaluate(dst, ctx, live.data(), 64) == 0);
}

void test_bind_keeps_authored_zero_length_and_speedmod() {
  wds::chart_render::unity::ParticleSystem ps;
  ps.shape().length = 0.0f;
  ps.velocityOverLifetime().enabled = true;
  ps.velocityOverLifetime().speedModifier.constant = 0.0f;
  ps.velocityOverLifetime().orbitalOffsetX.constant = -1.0f;
  wds::chart_render::SplitEffectParticle dst{};
  wds::chart_render::bind_from_unity(ps, dst);
  CHECK(std::fabs(dst.shape_length) <= 1e-6f);
  CHECK(std::fabs(dst.vol_speed_mod) <= 1e-6f);
  CHECK(std::fabs(dst.orbital_offset_x - (-1.0f)) <= 1e-6f);
}

void test_emission_disabled_zeros_rates() {
  wds::chart_render::unity::ParticleSystem ps;
  ps.emission().enabled = false;
  ps.emission().rateOverTime.constant = 12.0f;
  ps.emission().rateOverDistance.constant = 5.0f;
  wds::chart_render::SplitEffectParticle dst{};
  wds::chart_render::bind_from_unity(ps, dst);
  CHECK(std::fabs(dst.rate_over_time) <= 1e-6f);
  CHECK(std::fabs(dst.rate_over_distance) <= 1e-6f);
}

void test_oracle_compare() {
  const std::string root = std::string(WDS_TEST_REPO_ROOT) + "/resources";
  CompareStats stats;
  std::vector<FieldDiff> diffs;
  // Sub Emitters children outside _lineEffects (100101, 10790_2) have catalog
  // rows but no LineEffects system to hydrate from.
  std::vector<bool> sub_child;
  for (uint16_t i = 0; i < wds::chart_render::official_split_effect_subemitter_count(); ++i) {
    const size_t c = wds::chart_render::official_split_effect_subemitter_at(i)->child_index;
    if (c >= sub_child.size()) {
      sub_child.resize(c + 1, false);
    }
    sub_child[c] = true;
  }

  for (const int32_t id : wds::chart_render::official_split_effect_ids()) {
    if (wds::chart_render::openwds_split_effects::resource_for_effect(id) == nullptr) {
      continue;
    }
    const auto* entry = wds::chart_render::official_split_effect(id);
    CHECK(entry != nullptr);
    if (entry == nullptr) {
      continue;
    }
    auto controller = wds::chart_render::openwds_split_effects::create_controller(id, root);
    CHECK(controller != nullptr && controller->valid());
    if (!controller || !controller->valid()) {
      continue;
    }
    const bool initialized = controller->initialize(false, 100, 10, 100);
    CHECK(initialized);
    if (!initialized) {
      continue;
    }

    uint16_t catalog_count = 0;
    const auto* catalog = wds::chart_render::official_split_effect_particles(*entry, &catalog_count);
    const uint8_t element_count = entry->element_count == 0 ? 1 : entry->element_count;
    std::vector<std::vector<const wds::chart_render::SplitEffectParticle*>> groups(
        static_cast<std::size_t>(element_count));
    for (uint16_t i = 0; i < catalog_count; ++i) {
      const auto& p = catalog[i];
      if (p.element_index < element_count) {
        groups[static_cast<std::size_t>(p.element_index)].push_back(&p);
      }
    }

    ++stats.effects;
    const auto& lines = controller->lines();
    for (std::size_t line_i = 0; line_i < lines.size(); ++line_i) {
      const auto& line = lines[line_i];
      const std::size_t elem = static_cast<std::size_t>(line_i % static_cast<std::size_t>(element_count));
      const auto& group = groups[elem];
      std::size_t j = 0;
      for (const auto& ps : line.particle_systems) {
        if (!ps) {
          continue;
        }
        if (j >= group.size()) {
          break;
        }
        const auto* oracle = group[j];
        wds::chart_render::SplitEffectParticle dst{};
        wds::chart_render::bind_from_unity(*ps, dst, oracle);
        compare_particle(stats, diffs, id, dst, *oracle);
        ++j;
      }
      for (; j < group.size(); ++j) {
        const size_t row = static_cast<size_t>(entry->particle_begin) +
                           static_cast<size_t>(group[j] - catalog);
        if (row < sub_child.size() && sub_child[row]) {
          continue;
        }
        if (group[j]->max_particles > 0) {
          ++stats.missing;
          std::fprintf(stderr, "MISSING %d line=%zu j=%zu hydrate/bind particle\n",
                       static_cast<int>(id), line_i, j);
        }
      }
    }
  }

  std::fprintf(stdout,
               "bind_oracle_compare effects=%d particles=%d fields=%d diffs_|a-b|>1e-6=%d "
               "r6_rounding=%d missing=%d int_mismatch=%d\n",
               stats.effects, stats.particles, stats.fields, stats.diff_gt_1e6, stats.rounding_r6,
               stats.missing, stats.int_mismatch);
  for (const auto& kv : stats.field_diffs) {
    std::fprintf(stdout, "  field_diff %s %d\n", kv.first.c_str(), kv.second);
  }
  for (const auto& d : diffs) {
    std::fprintf(stdout, "  diff id=%d %s graph=%.8f catalog=%.8f\n", static_cast<int>(d.id),
                 d.field, static_cast<double>(d.graph), static_cast<double>(d.catalog));
  }
  CHECK(stats.effects > 0);
  CHECK(stats.particles > 0);
  CHECK(stats.missing == 0);
  CHECK(stats.int_mismatch == 0);
  CHECK(stats.diff_gt_1e6 == 0);
}

}  // namespace

int main() {
  test_hand_built();
  test_bind_keeps_authored_zero_length_and_speedmod();
  test_emission_disabled_zeros_rates();
  test_inactive_gameobject_does_not_evaluate();
  test_oracle_compare();
  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
