#pragma once

#include <wds/chart_render/unity_api.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wds::chart_render::openwds_split_effects {

struct OpenWdsEffectBinding {
  std::int32_t id;
  std::uint32_t bundle_offset;
  std::uint16_t bundle_count;
};

struct SerializedReference {
  std::string field;
  std::int32_t file_id = 0;
  std::int64_t path_id = 0;
  std::string target_bundle;
};

struct SerializedObjectRecord {
  std::string bundle;
  std::int64_t path_id = 0;
  std::string serialized_type;
  std::string raw_data_sha256;
  // Complete generated object record, including the exact typetree field
  // object. The bundle remains byte authority; this is a lossless field view.
  std::string serialized_json;
  bool has_particle_metadata = false;
  bool auto_random_seed = true;
  std::uint32_t random_seed = 0;
  std::vector<SerializedReference> references;
};

struct SerializedVector3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  bool present = false;
};

struct SerializedQuaternion {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
  bool present = false;
};

struct SerializedColor {
  float r = 1.0f;
  float g = 1.0f;
  float b = 1.0f;
  float a = 1.0f;
  bool present = false;
};

struct SerializedTransformMetadata {
  SerializedVector3 local_position;
  SerializedQuaternion local_rotation;
  SerializedVector3 local_scale;
};

struct ControllerLineMetadata {
  SerializedReference sprite_renderer;
  std::string game_object_name;
  SerializedTransformMetadata transform;
  SerializedColor color;
};

struct ControllerElementMetadata {
  SerializedReference component;
  std::string game_object_name;
  SerializedTransformMetadata transform;
  SerializedColor line_color;
  std::vector<SerializedReference> line_effects;
};

struct SerializedObjectKey {
  std::string_view bundle;
  std::int64_t path_id = 0;
  bool operator==(const SerializedObjectKey& other) const noexcept {
    return path_id == other.path_id && bundle == other.bundle;
  }
};

struct SerializedObjectKeyHash {
  std::size_t operator()(const SerializedObjectKey& key) const noexcept {
    std::size_t h = std::hash<std::int64_t>{}(key.path_id);
    h ^= std::hash<std::string_view>{}(key.bundle) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
  }
};

struct LoadedEffect {
  const OpenWdsEffectBinding* binding = nullptr;
  std::string resource_root;
  std::string graph_path;
  std::vector<SerializedObjectRecord> objects;
  // Built inside Controller::initialize, never in load_effect.
  mutable std::unordered_map<SerializedObjectKey, const SerializedObjectRecord*,
                             SerializedObjectKeyHash>
      object_index;
  mutable bool object_index_ready = false;
  std::size_t game_object_count = 0;
  std::size_t transform_count = 0;
  std::size_t sprite_renderer_count = 0;
  std::size_t particle_system_count = 0;
  std::size_t renderer_count = 0;
  std::size_t controller_line_reference_count = 0;
  std::size_t controller_element_reference_count = 0;
  std::vector<SerializedReference> element_particle_references;
  std::vector<std::vector<SerializedReference>> element_particle_groups;
  std::vector<ControllerLineMetadata> controller_lines;
  std::vector<ControllerElementMetadata> controller_elements;
  bool complete = false;
  bool resolves_all_references = false;
  std::size_t known_external_reference_count = 0;
  std::vector<std::string> unresolved_references;
};

// The generated index is pinned to OpenWDS SplitLane 1.96.0. The serialized
// object tables under the repo-root `resources/` directory (shipped beside the
// executable) are the exact serialized resources; these constants are
// intentionally provenance, not a fallback to the older rounded particle catalog.
inline constexpr std::string_view kOpenWdsAssetVersion = "1.96.0";
inline constexpr std::string_view kOpenWdsSplitEffectIndexSha256 =
    "6f7cf8b24b7e17e913c30806c8c667b16f1d6e5e52c6bbc7bdc85a29d90adaf4";
inline constexpr std::size_t kOpenWdsEffectCount = 318;
inline constexpr std::size_t kOpenWdsBundleCount = 790;

// All public lookup functions are strict: an unknown ID returns nullptr or
// false and is never mapped to a nearby effect.
const OpenWdsEffectBinding* resource_for_effect(std::int32_t effect_id) noexcept;
std::vector<std::int32_t> resource_effect_ids();
std::vector<std::string_view> resource_bundle_paths(const OpenWdsEffectBinding&) noexcept;
bool resource_has_exact_id_set() noexcept;
LoadedEffect load_effect(std::int32_t effect_id,
                         std::string_view resource_root = {});

// Typetree → typed Unity 2022.3 modules. Implementations live in
// resources.cpp (Wave 1B). Do not route through catalog structs.
bool hydrate_particle_system(unity::ParticleSystem& dst,
                             const SerializedObjectRecord& ps,
                             const LoadedEffect& graph) noexcept;
bool hydrate_particle_system_renderer(unity::ParticleSystemRenderer& dst,
                                      const SerializedObjectRecord& psr,
                                      const LoadedEffect& graph) noexcept;

void ensure_serialized_object_index(const LoadedEffect& graph) noexcept;
const SerializedObjectRecord* find_serialized_object(
    const LoadedEffect& graph, const std::string& bundle, std::int64_t path_id) noexcept;
const SerializedObjectRecord* resolve_serialized_reference(
    const LoadedEffect& graph, const SerializedObjectRecord& referrer,
    const SerializedReference& ref) noexcept;
const SerializedObjectRecord* find_game_object_component(
    const LoadedEffect& graph, const SerializedObjectRecord& game_object,
    std::string_view serialized_type) noexcept;

// Authored element prefab: root GO + child GOs with names and local TRS only.
std::shared_ptr<unity::GameObject> materialize_element_prefab(
    const LoadedEffect& graph, const ControllerElementMetadata& element);

}  // namespace wds::chart_render::openwds_split_effects
