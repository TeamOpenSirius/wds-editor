#include <wds/chart_render/openwds_split_effects/controller.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wds::chart_render::openwds_split_effects {

namespace {

void apply_transform_metadata(unity::Transform& transform,
                              const SerializedTransformMetadata& metadata) noexcept {
  if (metadata.local_position.present) {
    transform.localPosition = {metadata.local_position.x, metadata.local_position.y,
                               metadata.local_position.z};
  }
  if (metadata.local_rotation.present) {
    transform.localRotation = {metadata.local_rotation.x, metadata.local_rotation.y,
                               metadata.local_rotation.z, metadata.local_rotation.w};
  }
  if (metadata.local_scale.present) {
    transform.localScale = {metadata.local_scale.x, metadata.local_scale.y,
                            metadata.local_scale.z};
  }
}

unity::Color serialized_color_or(const SerializedColor& color, unity::Color fallback) noexcept {
  if (!color.present) return fallback;
  return {color.r, color.g, color.b, color.a};
}

std::string source_identity_of(const SerializedReference& ref,
                               const SerializedObjectRecord* referrer) {
  const std::string bundle =
      ref.file_id == 0 ? (referrer != nullptr ? referrer->bundle : ref.target_bundle)
                       : ref.target_bundle;
  return bundle + "#" + std::to_string(ref.path_id);
}

void index_cloned_game_objects(
    const std::shared_ptr<unity::GameObject>& root,
    std::unordered_map<std::int64_t, std::shared_ptr<unity::GameObject>>& out) {
  if (!root) return;
  if (root->openwds_path_id != 0) out.emplace(root->openwds_path_id, root);
  for (const auto& child : root->children_owned) index_cloned_game_objects(child, out);
}

const SerializedObjectRecord* game_object_of_component(
    const LoadedEffect& graph, const SerializedObjectRecord& component) {
  for (const auto& ref : component.references) {
    if (ref.field != "m_GameObject") continue;
    return resolve_serialized_reference(graph, component, ref);
  }
  return nullptr;
}

// In-call only: copy hydrated modules + renderer onto a new owner. Does not
// copy owner_, particles_, time_, or playing_. Material*/Mesh* stay shared
// (hydrate allocates them; bind reads name, not pointer identity).
void copy_hydrated_modules(unity::ParticleSystem& dst, const unity::ParticleSystem& src) {
  dst.main() = src.main();
  dst.emission() = src.emission();
  dst.shape() = src.shape();
  dst.velocityOverLifetime() = src.velocityOverLifetime();
  dst.forceOverLifetime() = src.forceOverLifetime();
  dst.limitVelocityOverLifetime() = src.limitVelocityOverLifetime();
  dst.colorOverLifetime() = src.colorOverLifetime();
  dst.colorBySpeed() = src.colorBySpeed();
  dst.sizeOverLifetime() = src.sizeOverLifetime();
  dst.sizeBySpeed() = src.sizeBySpeed();
  dst.rotationOverLifetime() = src.rotationOverLifetime();
  dst.rotationBySpeed() = src.rotationBySpeed();
  dst.noise() = src.noise();
  dst.inheritVelocity() = src.inheritVelocity();
  dst.lights() = src.lights();
  dst.textureSheetAnimation() = src.textureSheetAnimation();
  dst.trails() = src.trails();
  dst.renderer() = src.renderer();
  dst.particleSystemRenderer() = src.particleSystemRenderer();
  dst.subEmitters() = src.subEmitters();
  dst.lifetimeByEmitterSpeed() = src.lifetimeByEmitterSpeed();
  dst.externalForces() = src.externalForces();
  dst.collision() = src.collision();
  dst.trigger() = src.trigger();
  dst.customData() = src.customData();
  dst.setUseAutoRandomSeed(src.useAutoRandomSeed());
  dst.setRandomSeed(src.randomSeed());
}

struct HydrateReuseKey {
  const SerializedObjectRecord* prefab = nullptr;
  const SerializedObjectRecord* ps_record = nullptr;
  bool operator==(const HydrateReuseKey& other) const noexcept {
    return prefab == other.prefab && ps_record == other.ps_record;
  }
};

struct HydrateReuseKeyHash {
  std::size_t operator()(const HydrateReuseKey& key) const noexcept {
    std::size_t h = std::hash<const void*>{}(key.prefab);
    h ^= std::hash<const void*>{}(key.ps_record) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
  }
};

}  // namespace

Controller::Controller(std::int32_t effect_id, std::string_view resource_root)
    : binding_(resource_for_effect(effect_id)), loaded_(load_effect(effect_id, resource_root)) {
  if (!binding_) return;
  root_ = std::make_shared<unity::GameObject>("SplitEffect");
  animator_ = std::make_shared<unity::Animator>();
  // SplitEffectController writes LineHeight / LineOffsetPosition onto
  // _splitEffectAnimator.transform, a child of the controller root — not the
  // root itself (OpenWDS SplitEffectController.cs Initialize).
  animator_->transform.SetParent(&root_->transform, false);
  // Line count is the serialized _splitLines array recovered into
  // loaded_.controller_lines. An empty array stays empty; do not invent
  // placeholders.
  lines_.resize(loaded_.controller_lines.size());
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    auto object = std::make_shared<unity::GameObject>("Line" + std::to_string(i + 1));
    object->transform.SetParent(&root_->transform, false);
    lines_[i].game_object = object;
    lines_[i].renderer = std::make_shared<unity::SpriteRenderer>();
    lines_[i].renderer->transform = object->transform;
    lines_[i].renderer->gameObject = object.get();
  }
}

Controller::~Controller() { clear(); }

bool Controller::initialize(bool split_effect_light_setting,
                            int split_effect_line_opacity_settings,
                            int min_opacity_value,
                            int max_opacity_value) noexcept {
  if (!binding_ || initialized_) return binding_ != nullptr;
  initialized_ = true;
  light_setting_ = split_effect_light_setting;
  const int lo = std::min(min_opacity_value, max_opacity_value);
  const int hi = std::max(min_opacity_value, max_opacity_value);
  opacity_ = std::clamp(split_effect_line_opacity_settings, lo, hi);

  const char* profile_env = std::getenv("WDS_CTRL_INIT_PROFILE");
  const bool profile = profile_env != nullptr && profile_env[0] != '\0' && profile_env[0] != '0';
  using Clock = std::chrono::steady_clock;
  const auto t_init0 = Clock::now();

  // OfflineRhythmPreview's authored animator node is initialized by the
  // original controller, not by the prefab root.
  animator_->transform.localPosition.y = -5.0f;
  animator_->transform.localScale.y = 27.0f;
  ensure_serialized_object_index(loaded_);
  const std::string asset_version(kOpenWdsAssetVersion);
  const std::string index_sha(kOpenWdsSplitEffectIndexSha256);
  std::map<std::string, std::shared_ptr<unity::GameObject>> source_trees;

  struct HydrateJob {
    std::size_t line_i = 0;
    std::size_t unique_index = 0;
    std::shared_ptr<unity::GameObject> owner;
    const SerializedObjectRecord* ps_record = nullptr;
    const SerializedObjectRecord* source_go = nullptr;
    SerializedReference pptr;
    std::uint32_t random_seed = 0;
    const SerializedObjectRecord* element_behaviour = nullptr;
    std::shared_ptr<unity::ParticleSystem> system;
  };
  std::vector<HydrateJob> jobs;
  jobs.reserve(32);

  for (std::size_t i = 0; i < lines_.size(); ++i) {
    auto& line = lines_[i];
    line.source_identities.clear();
    line.game_object->transform.localPosition = {};
    line.game_object->transform.localRotation = {};
    line.game_object->transform.localScale = {1.0f, 1.0f, 1.0f};
    if (i < loaded_.controller_lines.size())
      apply_transform_metadata(line.game_object->transform,
                               loaded_.controller_lines[i].transform);
    if (line.renderer) line.renderer->transform = line.game_object->transform;
    line.game_object->name = "_Line" + std::to_string(i + 1);
    const ControllerElementMetadata* element_metadata = nullptr;
    if (!loaded_.controller_elements.empty())
      element_metadata = &loaded_.controller_elements[i % loaded_.controller_elements.size()];
    const SerializedColor* authored_color = nullptr;
    if (element_metadata != nullptr && element_metadata->line_color.present)
      authored_color = &element_metadata->line_color;
    else if (i < loaded_.controller_lines.size() && loaded_.controller_lines[i].color.present)
      authored_color = &loaded_.controller_lines[i].color;
    line.source_color = authored_color != nullptr
                            ? serialized_color_or(*authored_color, {1.0f, 1.0f, 1.0f, 1.0f})
                            : unity::Color{1.0f, 1.0f, 1.0f, 1.0f};
    const float opacity = static_cast<float>(opacity_) / 100.0f;
    line.color = line.source_color;
    line.color.r *= opacity;
    line.color.g *= opacity;
    line.color.b *= opacity;
    if (line.renderer) {
      line.renderer->color = line.color;
      line.renderer->enabled = false;
    }
    line.element_objects.clear();
    line.particle_systems.clear();
    // SplitEffectController Instantiates the authored element prefab under
    // each line. Empty LineEffects stays empty — no placeholder ParticleSystem.
    const auto& groups = loaded_.element_particle_groups;
    const std::vector<SerializedReference>* group = nullptr;
    if (element_metadata != nullptr && !element_metadata->line_effects.empty())
      group = &element_metadata->line_effects;
    else if (!groups.empty())
      group = &groups[i % groups.size()];
    if (group == nullptr || group->empty()) {
      line.particle_system = nullptr;
      line.enabled = false;
      continue;
    }
    std::shared_ptr<unity::GameObject> source;
    if (element_metadata != nullptr) {
      const std::string source_key =
          element_metadata->component.target_bundle + "#" +
          std::to_string(element_metadata->component.path_id);
      auto cached = source_trees.find(source_key);
      if (cached == source_trees.end()) {
        source = materialize_element_prefab(loaded_, *element_metadata);
        source_trees.emplace(source_key, source);
      } else {
        source = cached->second;
      }
    }
    if (!source) {
      line.particle_system = nullptr;
      line.enabled = false;
      continue;
    }
    // C# Instantiate(source, line.transform) is worldPositionStays=false, then
    // only localPosition.y is overwritten (EffectOffsetPosition = 2.5).
    auto element = unity::Instantiate(source, &line.game_object->transform);
    element->name = "SplitEffectElement_" + std::to_string(i + 1);
    element->transform.localPosition.y = 2.5f;
    line.element_objects.push_back(element);
    const std::uint32_t random_seed =
        static_cast<std::uint32_t>(unity::Random::Range(0, 1000));
    const SerializedObjectRecord* element_behaviour = nullptr;
    if (element_metadata != nullptr && element_metadata->component.path_id != 0) {
      element_behaviour = find_serialized_object(loaded_,
                                                 element_metadata->component.target_bundle,
                                                 element_metadata->component.path_id);
    }
    std::unordered_map<std::int64_t, std::shared_ptr<unity::GameObject>> clone_by_path;
    clone_by_path.reserve(16);
    index_cloned_game_objects(element, clone_by_path);
    for (const auto& pptr : *group) {
      const SerializedObjectRecord* ps_record = nullptr;
      if (element_behaviour != nullptr)
        ps_record = resolve_serialized_reference(loaded_, *element_behaviour, pptr);
      if (ps_record == nullptr)
        ps_record = find_serialized_object(loaded_, pptr.target_bundle, pptr.path_id);
      if (ps_record == nullptr || ps_record->serialized_type != "ParticleSystem") continue;
      const auto* source_go = game_object_of_component(loaded_, *ps_record);
      std::shared_ptr<unity::GameObject> owner;
      if (source_go != nullptr) {
        const auto it = clone_by_path.find(source_go->path_id);
        if (it != clone_by_path.end()) owner = it->second;
      }
      if (!owner) continue;
      HydrateJob job;
      job.line_i = i;
      job.owner = std::move(owner);
      job.ps_record = ps_record;
      job.source_go = source_go;
      job.pptr = pptr;
      job.random_seed = random_seed;
      job.element_behaviour = element_behaviour;
      jobs.push_back(std::move(job));
    }
  }

  // Reuse a hydrated PS/renderer only inside this initialize(): same prefab
  // source + same serialized record → hydrate once, clone modules per line.
  // Local map; nothing kept on Controller / LoadedEffect after return.
  struct UniqueHydrate {
    const SerializedObjectRecord* ps_record = nullptr;
    const SerializedObjectRecord* source_go = nullptr;
    std::shared_ptr<unity::ParticleSystem> system;
    bool has_psr = false;
  };
  std::vector<UniqueHydrate> unique;
  unique.reserve(jobs.size());
  {
    std::unordered_map<HydrateReuseKey, std::size_t, HydrateReuseKeyHash> index;
    index.reserve(jobs.size());
    for (auto& job : jobs) {
      const HydrateReuseKey key{job.element_behaviour, job.ps_record};
      const auto found = index.find(key);
      if (found == index.end()) {
        job.unique_index = unique.size();
        index.emplace(key, job.unique_index);
        UniqueHydrate entry;
        entry.ps_record = job.ps_record;
        entry.source_go = job.source_go;
        unique.push_back(std::move(entry));
      } else {
        job.unique_index = found->second;
      }
    }
  }

  const auto hydrate_unique = [&](UniqueHydrate& entry) {
    entry.system = std::make_shared<unity::ParticleSystem>();
    hydrate_particle_system(*entry.system, *entry.ps_record, loaded_);
    if (entry.source_go != nullptr) {
      if (const auto* psr_record = find_game_object_component(loaded_, *entry.source_go,
                                                             "ParticleSystemRenderer")) {
        hydrate_particle_system_renderer(entry.system->particleSystemRenderer(), *psr_record,
                                         loaded_);
        entry.has_psr = true;
      }
    }
  };

  const auto finish_job = [&](HydrateJob& job) {
    const UniqueHydrate& src = unique[job.unique_index];
    job.system = std::make_shared<unity::ParticleSystem>(job.owner);
    copy_hydrated_modules(*job.system, *src.system);
    if (src.has_psr) job.owner->particleSystemRenderer = &job.system->particleSystemRenderer();
    unity::SplitEffectResourceProvenance provenance;
    provenance.effectId = binding_->id;
    provenance.assetVersion = asset_version;
    provenance.bundleSha256 = index_sha;
    provenance.objects.push_back({job.pptr.path_id, "ParticleSystem", {}});
    job.system->set_resource_provenance(std::move(provenance));
    job.system->Stop(true, unity::ParticleSystemStopBehavior::StopEmittingAndClear);
    if (!job.ps_record->auto_random_seed) job.system->setRandomSeed(job.random_seed);
  };

  const auto run_parallel = [](std::size_t count, const auto& fn) {
    if (count == 0) return;
    if (count == 1) {
      fn(0);
      return;
    }
    const unsigned hw = std::thread::hardware_concurrency();
    const std::size_t workers =
        std::min(count, hw > 0 ? static_cast<std::size_t>(hw) : std::size_t{1});
    if (workers <= 1) {
      for (std::size_t i = 0; i < count; ++i) fn(i);
      return;
    }
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (std::size_t w = 0; w < workers; ++w) {
      threads.emplace_back([&] {
        for (;;) {
          const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
          if (i >= count) break;
          fn(i);
        }
      });
    }
    for (auto& t : threads) t.join();
  };

  const auto t_u0 = Clock::now();
  run_parallel(unique.size(), [&](std::size_t i) { hydrate_unique(unique[i]); });
  const double unique_ms = std::chrono::duration<double, std::milli>(Clock::now() - t_u0).count();

  const auto t_c0 = Clock::now();
  run_parallel(jobs.size(), [&](std::size_t i) { finish_job(jobs[i]); });
  const double clone_ms = std::chrono::duration<double, std::milli>(Clock::now() - t_c0).count();
  const double hydrate_ms = unique_ms + clone_ms;
  const std::size_t unique_ps = unique.size();

  for (auto& job : jobs) {
    auto& line = lines_[job.line_i];
    line.particle_systems.push_back(std::move(job.system));
    line.source_identities.push_back(source_identity_of(job.pptr, job.element_behaviour));
  }
  for (auto& line : lines_) {
    line.particle_system = line.particle_systems.empty() ? nullptr : line.particle_systems.front();
    line.enabled = false;
  }
  if (profile) {
    const double init_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - t_init0).count();
    std::fprintf(stderr,
                 "CTRL_INIT_PROFILE effect=%d init_ms=%.3f unique_ms=%.3f clone_ms=%.3f "
                 "hydrate_ms=%.3f jobs=%zu unique_ps=%zu reuse_frac=%.3f\n",
                 binding_->id, init_ms, unique_ms, clone_ms, hydrate_ms, jobs.size(), unique_ps,
                 jobs.empty() ? 0.0
                              : static_cast<double>(unique_ps) / static_cast<double>(jobs.size()));
  }
  return true;
}

void Controller::on_fade_in(int split_count, int split_lane_type) noexcept {
  if (!initialize(false, 100)) return;
  split_count = std::clamp(split_count, 1, 6);
  active_ = true;
  root_->SetActive(true);
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    auto& line = lines_[i];
    line.enabled = i <= static_cast<std::size_t>(split_count);
    if (line.renderer) line.renderer->enabled = line.enabled;
    if (!line.enabled || line.particle_systems.empty()) continue;
    // Ignore(7) overrides light; BothEnds(1) only plays the outer elements.
    const bool should_play = split_lane_type == 7 ||
                             (split_lane_type != 5 && !light_setting_);
    const bool outer = split_lane_type == 1 && i != 0 && i != static_cast<std::size_t>(split_count);
    if (should_play && !outer) {
      for (const auto& particle_system : line.particle_systems) {
        if (particle_system->isPlaying()) particle_system->Stop(true);
        particle_system->Play(true);
      }
    }
  }
  animator_->enabled = true;
  animator_->Play("SplitEffect_fadeIn_anim", 0, 0.0f);
  apply_original_split_positions(split_count);
}

void Controller::on_fade_out() noexcept {
  // The closed Animator state machine is intentionally represented by the
  // open API object; its exact clip curves remain available in the graph.
  if (animator_) animator_->Play("SplitEffect_fadeOut_anim", 0, 0.0f);
  active_ = false;
}

void Controller::clear() noexcept {
  for (auto& line : lines_)
    for (const auto& particle_system : line.particle_systems)
      if (particle_system)
        particle_system->Stop(true);
  if (root_) root_->SetActive(false);
  active_ = false;
}

void Controller::set_line_x(std::size_t index, float value) noexcept {
  if (index >= lines_.size() || !lines_[index].game_object) return;
  lines_[index].game_object->transform.localPosition.x = value;
  if (lines_[index].renderer)
    lines_[index].renderer->transform.localPosition.x = value;
}

void Controller::apply_original_split_positions(int split_count) noexcept {
  // Values copied from SplitEffectController.ApplyOriginalSplitPositions.
  switch (split_count) {
    case 1: set_line_x(0, -5.55f); set_line_x(1, 5.55f); break;
    case 2: set_line_x(0, -5.55f); set_line_x(1, 0.0f); set_line_x(2, 5.55f); break;
    case 3: set_line_x(0, -5.55f); set_line_x(1, -1.85f); set_line_x(2, 1.85f); set_line_x(3, 5.55f); break;
    case 4: set_line_x(0, -5.55f); set_line_x(1, -2.775f); set_line_x(2, 0.0f); set_line_x(3, 2.775f); set_line_x(4, 5.55f); break;
    case 5: set_line_x(0, -5.55f); set_line_x(1, -2.775f); set_line_x(2, -0.925f); set_line_x(3, 0.925f); set_line_x(4, 2.775f); set_line_x(5, 5.55f); break;
    case 6: set_line_x(0, -5.55f); set_line_x(1, -3.7f); set_line_x(2, -1.85f); set_line_x(3, 0.0f); set_line_x(4, 1.85f); set_line_x(5, 3.7f); set_line_x(6, 5.55f); break;
    default: break;
  }
}

std::unique_ptr<Controller> create_controller(std::int32_t effect_id,
                                              std::string_view resource_root) {
  auto result = std::make_unique<Controller>(effect_id, resource_root);
  if (!result->valid()) return nullptr;
  return result;
}

}  // namespace wds::chart_render::openwds_split_effects
