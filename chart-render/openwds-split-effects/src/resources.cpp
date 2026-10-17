#include <wds/chart_render/openwds_split_effects/resources.hpp>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <cstdlib>
#include <memory>
#include <new>
#include <string_view>
#include <utility>

namespace wds::chart_render::openwds_split_effects {
namespace {
#include "bindings.inc"
constexpr std::size_t kBindingCount = sizeof(kOpenWdsEffectBindings) /
                                       sizeof(kOpenWdsEffectBindings[0]);
constexpr std::size_t kBundlePathCount = sizeof(kOpenWdsBundlePaths) /
                                          sizeof(kOpenWdsBundlePaths[0]);
}  // namespace

const OpenWdsEffectBinding* resource_for_effect(std::int32_t effect_id) noexcept {
  const auto* begin = std::begin(kOpenWdsEffectBindings);
  const auto* end = begin + kBindingCount;
  const auto it = std::lower_bound(begin, end, effect_id,
                                   [](const OpenWdsEffectBinding& b, std::int32_t id) {
                                     return b.id < id;
                                   });
  return it != end && it->id == effect_id ? it : nullptr;
}

std::vector<std::int32_t> resource_effect_ids() {
  std::vector<std::int32_t> ids;
  ids.reserve(kBindingCount);
  for (const auto& binding : kOpenWdsEffectBindings) ids.push_back(binding.id);
  return ids;
}

std::vector<std::string_view> resource_bundle_paths(const OpenWdsEffectBinding& binding) noexcept {
  if (binding.bundle_offset > kBundlePathCount ||
      binding.bundle_count > kBundlePathCount - binding.bundle_offset) return {};
  std::vector<std::string_view> paths;
  paths.reserve(binding.bundle_count);
  for (std::size_t i = 0; i < binding.bundle_count; ++i)
    paths.emplace_back(kOpenWdsBundlePaths[binding.bundle_offset + i]);
  return paths;
}

bool resource_has_exact_id_set() noexcept {
  if (kBindingCount != kOpenWdsEffectCount) return false;
  for (std::size_t i = 1; i < kBindingCount; ++i)
    if (kOpenWdsEffectBindings[i - 1].id >= kOpenWdsEffectBindings[i].id) return false;
  return true;
}

namespace {
std::vector<std::string> split(const std::string& value, char delimiter) {
  std::vector<std::string> result;
  std::size_t begin = 0;
  while (begin <= value.size()) {
    const auto end = value.find(delimiter, begin);
    result.push_back(value.substr(begin, end == std::string::npos ? end : end - begin));
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return result;
}

bool json_bool(const std::string& json, const char* key, bool fallback) {
  const std::string needle = std::string("\"") + key + "\":";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) return fallback;
  const auto value = json.substr(pos + needle.size());
  if (value.rfind("true", 0) == 0) return true;
  if (value.rfind("false", 0) == 0) return false;
  return fallback;
}
std::uint32_t json_u32(const std::string& json, const char* key, std::uint32_t fallback) {
  const std::string needle = std::string("\"") + key + "\":";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) return fallback;
  try { return static_cast<std::uint32_t>(std::stoul(json.substr(pos + needle.size()))); }
  catch (...) { return fallback; }
}

bool json_float_after(const std::string& json, const std::string& marker, float& out) {
  const auto pos = json.find(marker);
  if (pos == std::string::npos) return false;
  const char* begin = json.c_str() + pos + marker.size();
  char* end = nullptr;
  const float value = std::strtof(begin, &end);
  if (end == begin) return false;
  out = value;
  return true;
}

SerializedVector3 json_vector3(const std::string& json, const char* key) {
  SerializedVector3 out;
  const std::string prefix = std::string("\"") + key + "\":{";
  const auto pos = json.find(prefix);
  if (pos == std::string::npos) return out;
  const std::string body = json.substr(pos + prefix.size());
  out.present = json_float_after(body, "\"x\":", out.x) &&
                json_float_after(body, "\"y\":", out.y) &&
                json_float_after(body, "\"z\":", out.z);
  return out;
}

SerializedQuaternion json_quaternion(const std::string& json, const char* key) {
  SerializedQuaternion out;
  const std::string prefix = std::string("\"") + key + "\":{";
  const auto pos = json.find(prefix);
  if (pos == std::string::npos) return out;
  const std::string body = json.substr(pos + prefix.size());
  out.present = json_float_after(body, "\"x\":", out.x) &&
                json_float_after(body, "\"y\":", out.y) &&
                json_float_after(body, "\"z\":", out.z) &&
                json_float_after(body, "\"w\":", out.w);
  return out;
}

SerializedColor json_color(const std::string& json, const char* key) {
  SerializedColor out;
  const std::string prefix = std::string("\"") + key + "\":{";
  const auto pos = json.find(prefix);
  if (pos == std::string::npos) return out;
  const std::string body = json.substr(pos + prefix.size());
  out.present = json_float_after(body, "\"r\":", out.r) &&
                json_float_after(body, "\"g\":", out.g) &&
                json_float_after(body, "\"b\":", out.b) &&
                json_float_after(body, "\"a\":", out.a);
  return out;
}

std::string json_string(const std::string& json, const char* key) {
  const std::string prefix = std::string("\"") + key + "\":\"";
  const auto pos = json.find(prefix);
  if (pos == std::string::npos) return {};
  const auto begin = pos + prefix.size();
  const auto end = json.find('"', begin);
  return end == std::string::npos ? std::string{} : json.substr(begin, end - begin);
}
}  // namespace

LoadedEffect load_effect(std::int32_t effect_id, std::string_view resource_root) {
  LoadedEffect loaded;
  loaded.binding = resource_for_effect(effect_id);
  if (!loaded.binding) return loaded;
  loaded.resource_root = resource_root.empty()
                             ? std::string("resources")
                             : std::string(resource_root);
  const std::string base = loaded.resource_root + "/";
  loaded.graph_path = base + "offlinepreview-object-graph.jsonl";
  bool complete = true;
  for (const auto bundle_view : resource_bundle_paths(*loaded.binding)) {
    const std::string bundle(bundle_view);
    std::ifstream file(base + bundle + ".objects.tsv");
    if (!file) { complete = false; continue; }
    std::string line;
    while (std::getline(file, line)) {
      const auto fields = split(line, '\t');
      if (fields.size() < 3) { complete = false; continue; }
      SerializedObjectRecord object;
      object.bundle = bundle;
      try {
        object.path_id = std::stoll(fields[0]);
      } catch (...) { complete = false; continue; }
      object.serialized_type = fields[1];
      object.raw_data_sha256 = fields[2];
      if (fields.size() >= 4 && !fields[3].empty()) {
        for (const auto& encoded : split(fields[3], ';')) {
          const auto ref_fields = split(encoded, '|');
          if (ref_fields.size() < 4) { complete = false; continue; }
          SerializedReference ref;
          ref.field = ref_fields[0];
          try {
            ref.file_id = std::stoi(ref_fields[1]);
            ref.path_id = std::stoll(ref_fields[2]);
          } catch (...) { complete = false; continue; }
          ref.target_bundle = ref_fields[3];
          object.references.push_back(std::move(ref));
        }
      }
      if (object.serialized_type == "GameObject") ++loaded.game_object_count;
      if (object.serialized_type == "Transform") ++loaded.transform_count;
      if (object.serialized_type == "SpriteRenderer") ++loaded.sprite_renderer_count;
      if (object.serialized_type == "ParticleSystem") ++loaded.particle_system_count;
      if (object.serialized_type == "ParticleSystemRenderer") ++loaded.renderer_count;
      loaded.objects.push_back(std::move(object));
    }
    std::ifstream fields(base + bundle + ".objects.jsonl");
    if (!fields) { complete = false; continue; }
    while (std::getline(fields, line)) {
      const std::string key = "\"pathId\":";
      const auto key_pos = line.find(key);
      if (key_pos == std::string::npos) { complete = false; continue; }
      std::size_t end = key_pos + key.size();
      try {
        const auto path_id = std::stoll(line.substr(end), &end);
        auto it = std::find_if(loaded.objects.rbegin(), loaded.objects.rend(),
                               [&](const SerializedObjectRecord& object) {
                                 return object.bundle == bundle && object.path_id == path_id;
                               });
        if (it != loaded.objects.rend()) it->serialized_json = line;
        else complete = false;
      } catch (...) { complete = false; }
    }
    for (auto& object : loaded.objects) {
      if (object.bundle != bundle || object.serialized_type != "ParticleSystem" ||
          object.serialized_json.empty()) continue;
      object.has_particle_metadata = true;
      object.auto_random_seed = json_bool(object.serialized_json, "autoRandomSeed", true);
      object.random_seed = json_u32(object.serialized_json, "randomSeed", 0);
    }
  }
  loaded.complete = complete && !loaded.objects.empty();
  // Validate local and external identities without collapsing duplicate PPtrs:
  // the source identity is the (bundle,pathId) pair for every object record.
  std::vector<std::string> identities;
  identities.reserve(loaded.objects.size());
  for (const auto& object : loaded.objects)
    identities.push_back(object.bundle + "#" + std::to_string(object.path_id));
  std::sort(identities.begin(), identities.end());
  bool refs_ok = true;
  for (const auto& object : loaded.objects) {
    for (const auto& ref : object.references) {
      if (ref.path_id == 0) continue;
      const std::string target = (ref.file_id == 0 ? object.bundle : ref.target_bundle) +
                                 "#" + std::to_string(ref.path_id);
      if (!std::binary_search(identities.begin(), identities.end(), target)) {
        // Unity's built-in default resources are an intentional external
        // dependency of Mesh/Shader references. Keep them explicit rather
        // than inventing a local object; all other missing PPtrs fail load.
        if (ref.target_bundle.find("unity default resources") != std::string::npos) {
          ++loaded.known_external_reference_count;
        } else {
          refs_ok = false;
          loaded.unresolved_references.push_back(target);
        }
      }
    }
  }
  loaded.resolves_all_references = refs_ok;
  // Recover the source controller's serialized arrays. The component object
  // owns _splitLines and _splitEffectElements PPtrs; each element component
  // then owns its _lineEffects PPtrs. Keep those identities as references so
  // runtime clones can be attributed to the exact authored PS object.
  auto find_object = [&](const std::string& bundle, std::int64_t path_id)
      -> const SerializedObjectRecord* {
    for (const auto& object : loaded.objects)
      if (object.bundle == bundle && object.path_id == path_id) return &object;
    return nullptr;
  };
  // live dump v1: element prefab can hold an independent playOnAwake PS that
  // is not listed in _lineEffects (10440_2 (2), 11151 extra). SubEmitter
  // children stay off the play list (100101, 10790 child).
  auto append_independent_element_particle_systems =
      [&](const SerializedObjectRecord& element, std::vector<SerializedReference>& group) {
        std::set<std::int64_t> listed;
        for (const auto& ref : group) listed.insert(ref.path_id);
        std::set<std::int64_t> children;
        for (const auto& object : loaded.objects) {
          if (object.bundle != element.bundle || object.serialized_type != "ParticleSystem") {
            continue;
          }
          for (const auto& ref : object.references) {
            if (ref.field.find("SubModule.subEmitters") == 0 &&
                ref.field.find(".emitter") != std::string::npos && ref.path_id != 0) {
              children.insert(ref.path_id);
            }
          }
        }
        std::vector<const SerializedObjectRecord*> extras;
        for (const auto& object : loaded.objects) {
          if (object.bundle != element.bundle || object.serialized_type != "ParticleSystem") {
            continue;
          }
          if (listed.count(object.path_id) || children.count(object.path_id)) continue;
          extras.push_back(&object);
        }
        std::sort(extras.begin(), extras.end(),
                  [](const SerializedObjectRecord* a, const SerializedObjectRecord* b) {
                    return a->path_id < b->path_id;
                  });
        for (const auto* object : extras) {
          SerializedReference extra;
          extra.field = "_lineEffects[" + std::to_string(group.size()) + "]";
          extra.file_id = 0;
          extra.path_id = object->path_id;
          extra.target_bundle = object->bundle;
          listed.insert(object->path_id);
          group.push_back(std::move(extra));
        }
      };
  const SerializedObjectRecord* controller_object = nullptr;
  for (const auto& object : loaded.objects) {
    if (object.serialized_type != "MonoBehaviour" ||
        object.serialized_json.find("_splitEffectElements") == std::string::npos) continue;
    controller_object = &object;
    for (const auto& ref : object.references) {
      if (ref.field.find("_splitLines[") == 0) ++loaded.controller_line_reference_count;
      if (ref.field.find("_splitEffectElements[") == 0) {
        ++loaded.controller_element_reference_count;
        const auto* element = find_object(ref.target_bundle, ref.path_id);
        if (element == nullptr) continue;
        std::vector<SerializedReference> group;
        std::set<std::int64_t> seen_pptr;
        for (const auto& element_ref : element->references)
          if (element_ref.field.find("_lineEffects[") == 0) {
            if (!seen_pptr.insert(element_ref.path_id).second) continue;
            loaded.element_particle_references.push_back(element_ref);
            group.push_back(element_ref);
          }
        append_independent_element_particle_systems(*element, group);
        for (std::size_t i = seen_pptr.size(); i < group.size(); ++i)
          loaded.element_particle_references.push_back(group[i]);
        loaded.element_particle_groups.push_back(std::move(group));
      }
    }
  }
  auto transform_for_game_object = [&](const SerializedObjectRecord* game_object)
      -> const SerializedObjectRecord* {
    if (game_object == nullptr) return nullptr;
    for (const auto& ref : game_object->references) {
      if (ref.field.find("m_Component[") != 0) continue;
      const auto* component = find_object(ref.target_bundle, ref.path_id);
      if (component != nullptr && component->serialized_type == "Transform") return component;
    }
    return nullptr;
  };
  auto fill_transform = [](const SerializedObjectRecord* object,
                           SerializedTransformMetadata& metadata) {
    if (object == nullptr) return;
    metadata.local_position = json_vector3(object->serialized_json, "m_LocalPosition");
    metadata.local_rotation = json_quaternion(object->serialized_json, "m_LocalRotation");
    metadata.local_scale = json_vector3(object->serialized_json, "m_LocalScale");
  };
  if (controller_object != nullptr) {
    for (const auto& ref : controller_object->references) {
      if (ref.field.find("_splitLines[") == 0) {
        ControllerLineMetadata line;
        line.sprite_renderer = ref;
        const auto* sprite = find_object(ref.target_bundle, ref.path_id);
        if (sprite == nullptr) continue;
        line.color = json_color(sprite->serialized_json, "m_Color");
        for (const auto& sprite_ref : sprite->references) {
          if (sprite_ref.field != "m_GameObject") continue;
          const auto* game_object = find_object(sprite_ref.target_bundle, sprite_ref.path_id);
          if (game_object == nullptr) continue;
          line.game_object_name = json_string(game_object->serialized_json, "m_Name");
          fill_transform(transform_for_game_object(game_object), line.transform);
        }
        loaded.controller_lines.push_back(std::move(line));
      } else if (ref.field.find("_splitEffectElements[") == 0) {
        ControllerElementMetadata element_metadata;
        element_metadata.component = ref;
        const auto* element = find_object(ref.target_bundle, ref.path_id);
        if (element == nullptr) continue;
        element_metadata.line_color = json_color(element->serialized_json, "_lineColor");
        for (const auto& element_ref : element->references) {
          if (element_ref.field == "m_GameObject") {
            const auto* game_object = find_object(element_ref.target_bundle, element_ref.path_id);
            if (game_object != nullptr) {
              element_metadata.game_object_name = json_string(game_object->serialized_json, "m_Name");
              fill_transform(transform_for_game_object(game_object), element_metadata.transform);
            }
          } else if (element_ref.field.find("_lineEffects[") == 0) {
            bool dup = false;
            for (const auto& existing : element_metadata.line_effects) {
              if (existing.path_id == element_ref.path_id) {
                dup = true;
                break;
              }
            }
            if (!dup) element_metadata.line_effects.push_back(element_ref);
          }
        }
        append_independent_element_particle_systems(*element, element_metadata.line_effects);
        loaded.controller_elements.push_back(std::move(element_metadata));
      }
    }
  }
  return loaded;
}

namespace {

void skip_ws(std::string_view s, std::size_t& i) {
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
}

bool consume_string(std::string_view s, std::size_t& i) {
  if (i >= s.size() || s[i] != '"') return false;
  ++i;
  while (i < s.size()) {
    if (s[i] == '\\') {
      i += (i + 1 < s.size()) ? 2 : 1;
      continue;
    }
    if (s[i] == '"') {
      ++i;
      return true;
    }
    ++i;
  }
  return false;
}

bool consume_value(std::string_view s, std::size_t& i, std::size_t& begin, std::size_t& end) {
  skip_ws(s, i);
  if (i >= s.size()) return false;
  begin = i;
  const char c = s[i];
  if (c == '"') {
    if (!consume_string(s, i)) return false;
    end = i;
    return true;
  }
  if (c == '{' || c == '[') {
    const char open = c;
    const char close = (c == '{') ? '}' : ']';
    int depth = 0;
    while (i < s.size()) {
      if (s[i] == '"') {
        if (!consume_string(s, i)) return false;
        continue;
      }
      if (s[i] == open) {
        ++depth;
        ++i;
        continue;
      }
      if (s[i] == close) {
        --depth;
        ++i;
        if (depth == 0) {
          end = i;
          return true;
        }
        continue;
      }
      ++i;
    }
    return false;
  }
  if (s.compare(i, 4, "true") == 0) {
    i += 4;
    end = i;
    return true;
  }
  if (s.compare(i, 5, "false") == 0) {
    i += 5;
    end = i;
    return true;
  }
  if (s.compare(i, 4, "null") == 0) {
    i += 4;
    end = i;
    return true;
  }
  if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) {
    ++i;
    while (i < s.size()) {
      const char ch = s[i];
      if ((ch >= '0' && ch <= '9') || ch == '.' || ch == 'e' || ch == 'E' ||
          ch == '+' || ch == '-') {
        ++i;
      } else {
        break;
      }
    }
    end = i;
    return true;
  }
  return false;
}

bool find_member(std::string_view object, std::string_view key, std::size_t& begin,
                 std::size_t& end) {
  std::size_t i = 0;
  skip_ws(object, i);
  if (i >= object.size() || object[i] != '{') return false;
  ++i;
  while (i < object.size()) {
    skip_ws(object, i);
    if (i >= object.size()) return false;
    if (object[i] == '}') return false;
    if (object[i] == ',') {
      ++i;
      continue;
    }
    if (object[i] != '"') return false;
    const std::size_t key_begin = i + 1;
    if (!consume_string(object, i)) return false;
    const std::string_view found(object.data() + key_begin, (i - 1) - key_begin);
    skip_ws(object, i);
    if (i >= object.size() || object[i] != ':') return false;
    ++i;
    std::size_t value_begin = 0;
    std::size_t value_end = 0;
    if (!consume_value(object, i, value_begin, value_end)) return false;
    if (found == key) {
      begin = value_begin;
      end = value_end;
      return true;
    }
  }
  return false;
}

std::string_view member_view(std::string_view object, std::string_view key) {
  std::size_t begin = 0;
  std::size_t end = 0;
  if (!find_member(object, key, begin, end)) return {};
  return object.substr(begin, end - begin);
}

std::string_view member_object(std::string_view object, std::string_view key) {
  const std::string_view value = member_view(object, key);
  return (!value.empty() && value.front() == '{') ? value : std::string_view{};
}

std::string_view member_array(std::string_view object, std::string_view key) {
  const std::string_view value = member_view(object, key);
  return (!value.empty() && value.front() == '[') ? value : std::string_view{};
}

bool parse_float_token(std::string_view token, float& out) {
  if (token.empty()) return false;
  const std::string copy(token);
  char* end = nullptr;
  const float value = std::strtof(copy.c_str(), &end);
  if (end == copy.c_str()) return false;
  out = value;
  return true;
}

bool parse_i64_token(std::string_view token, std::int64_t& out) {
  if (token.empty()) return false;
  const std::string copy(token);
  char* end = nullptr;
  const std::int64_t value = std::strtoll(copy.c_str(), &end, 10);
  if (end == copy.c_str()) return false;
  out = value;
  return true;
}

bool parse_i32_token(std::string_view token, int& out) {
  std::int64_t value = 0;
  if (!parse_i64_token(token, value)) return false;
  out = static_cast<int>(value);
  return true;
}

bool parse_u32_token(std::string_view token, std::uint32_t& out) {
  if (token.empty()) return false;
  const std::string copy(token);
  char* end = nullptr;
  const unsigned long value = std::strtoul(copy.c_str(), &end, 10);
  if (end == copy.c_str()) return false;
  out = static_cast<std::uint32_t>(value);
  return true;
}

bool try_member_bool(std::string_view object, std::string_view key, bool& out) {
  const std::string_view value = member_view(object, key);
  if (value == "true") {
    out = true;
    return true;
  }
  if (value == "false") {
    out = false;
    return true;
  }
  return false;
}

bool try_member_float(std::string_view object, std::string_view key, float& out) {
  return parse_float_token(member_view(object, key), out);
}

bool try_member_i32(std::string_view object, std::string_view key, int& out) {
  return parse_i32_token(member_view(object, key), out);
}

bool try_member_i64(std::string_view object, std::string_view key, std::int64_t& out) {
  return parse_i64_token(member_view(object, key), out);
}

bool try_member_u32(std::string_view object, std::string_view key, std::uint32_t& out) {
  return parse_u32_token(member_view(object, key), out);
}

template <typename Fn>
void for_each_array_element(std::string_view array, Fn&& fn) {
  std::size_t i = 0;
  skip_ws(array, i);
  if (i >= array.size() || array[i] != '[') return;
  ++i;
  while (i < array.size()) {
    skip_ws(array, i);
    if (i >= array.size() || array[i] == ']') return;
    if (array[i] == ',') {
      ++i;
      continue;
    }
    std::size_t begin = 0;
    std::size_t end = 0;
    if (!consume_value(array, i, begin, end)) return;
    fn(array.substr(begin, end - begin));
  }
}

bool parse_color(std::string_view object, unity::Color& color) {
  if (object.empty() || object.front() != '{') return false;
  bool any = false;
  any |= try_member_float(object, "r", color.r);
  any |= try_member_float(object, "g", color.g);
  any |= try_member_float(object, "b", color.b);
  any |= try_member_float(object, "a", color.a);
  return any;
}

bool parse_vec3(std::string_view object, unity::Vector3& value) {
  if (object.empty() || object.front() != '{') return false;
  bool any = false;
  any |= try_member_float(object, "x", value.x);
  any |= try_member_float(object, "y", value.y);
  any |= try_member_float(object, "z", value.z);
  return any;
}

unity::AnimationCurve parse_animation_curve(std::string_view object) {
  unity::AnimationCurve curve;
  if (object.empty() || object.front() != '{') return curve;
  for_each_array_element(member_array(object, "m_Curve"), [&](std::string_view key) {
    if (key.empty() || key.front() != '{') return;
    unity::Keyframe frame;
    try_member_float(key, "time", frame.time);
    try_member_float(key, "value", frame.value);
    try_member_float(key, "inSlope", frame.inTangent);
    try_member_float(key, "outSlope", frame.outTangent);
    curve.AddKey(frame);
  });
  return curve;
}

unity::MinMaxCurve parse_minmax_curve(std::string_view object) {
  unity::MinMaxCurve curve;
  if (object.empty() || object.front() != '{') return curve;
  int state = 0;
  try_member_i32(object, "minMaxState", state);
  curve.mode = static_cast<unity::ParticleSystemCurveMode>(state);
  float scalar = 0.0f;
  float min_scalar = 0.0f;
  try_member_float(object, "scalar", scalar);
  try_member_float(object, "minScalar", min_scalar);
  switch (curve.mode) {
    case unity::ParticleSystemCurveMode::Constant:
      curve.constant = scalar;
      break;
    case unity::ParticleSystemCurveMode::TwoConstants:
      curve.constantMin = min_scalar;
      curve.constant = scalar;
      break;
    case unity::ParticleSystemCurveMode::Curve:
    case unity::ParticleSystemCurveMode::TwoCurves:
      // Unity 2022.3 MinMaxCurve.curveMultiplier is typetree scalar;
      // minScalar stays on constantMin so bind scalar_pair matches generate.
      curve.curveMultiplier = scalar;
      curve.constantMin = min_scalar;
      curve.curve = parse_animation_curve(member_object(object, "maxCurve"));
      curve.curveMin = parse_animation_curve(member_object(object, "minCurve"));
      break;
  }
  return curve;
}

unity::Gradient parse_gradient(std::string_view object) {
  unity::Gradient gradient;
  if (object.empty() || object.front() != '{') return gradient;
  int color_count = 0;
  int alpha_count = 0;
  try_member_i32(object, "m_NumColorKeys", color_count);
  try_member_i32(object, "m_NumAlphaKeys", alpha_count);
  for (int i = 0; i < color_count; ++i) {
    const std::string key = "key" + std::to_string(i);
    const std::string ctime = "ctime" + std::to_string(i);
    unity::GradientColorKey color_key;
    parse_color(member_object(object, key), color_key.color);
    float serialized_time = 0.0f;
    if (try_member_float(object, ctime, serialized_time))
      color_key.time = serialized_time / 65535.0f;
    gradient.colorKeys.push_back(color_key);
  }
  for (int i = 0; i < alpha_count; ++i) {
    const std::string key = "key" + std::to_string(i);
    const std::string atime = "atime" + std::to_string(i);
    unity::Color color;
    parse_color(member_object(object, key), color);
    unity::GradientAlphaKey alpha_key;
    alpha_key.alpha = color.a;
    float serialized_time = 0.0f;
    if (try_member_float(object, atime, serialized_time))
      alpha_key.time = serialized_time / 65535.0f;
    gradient.alphaKeys.push_back(alpha_key);
  }
  return gradient;
}

unity::MinMaxGradient parse_minmax_gradient(std::string_view object) {
  unity::MinMaxGradient gradient;
  if (object.empty() || object.front() != '{') return gradient;
  int state = 0;
  try_member_i32(object, "minMaxState", state);
  gradient.mode = static_cast<unity::ParticleSystemGradientMode>(state);
  parse_color(member_object(object, "maxColor"), gradient.color);
  parse_color(member_object(object, "minColor"), gradient.colorMin);
  gradient.gradient = parse_gradient(member_object(object, "maxGradient"));
  gradient.gradientMin = parse_gradient(member_object(object, "minGradient"));
  return gradient;
}

void assign_curve(std::string_view parent, std::string_view key, unity::MinMaxCurve& dst) {
  const std::string_view object = member_object(parent, key);
  if (!object.empty()) dst = parse_minmax_curve(object);
}

void assign_curve_or_float(std::string_view parent, std::string_view key,
                           unity::MinMaxCurve& dst) {
  const std::string_view value = member_view(parent, key);
  if (value.empty()) return;
  if (value.front() == '{') {
    dst = parse_minmax_curve(value);
    return;
  }
  float scalar = 0.0f;
  if (!parse_float_token(value, scalar)) return;
  dst.mode = unity::ParticleSystemCurveMode::Constant;
  dst.constant = scalar;
}

void assign_range_xy(std::string_view parent, std::string_view key, unity::MinMaxCurve& dst) {
  const std::string_view value = member_view(parent, key);
  if (value.empty()) return;
  if (!value.empty() && value.front() == '{') {
    const bool has_state = !member_view(value, "minMaxState").empty();
    if (has_state) {
      dst = parse_minmax_curve(value);
      return;
    }
    float min_v = 0.0f;
    float max_v = 0.0f;
    try_member_float(value, "x", min_v);
    try_member_float(value, "y", max_v);
    dst.mode = unity::ParticleSystemCurveMode::TwoConstants;
    dst.constantMin = min_v;
    dst.constant = max_v;
    return;
  }
  assign_curve_or_float(parent, key, dst);
}

void assign_space(std::string_view module, unity::ParticleSystemSimulationSpace& space) {
  bool world = false;
  if (try_member_bool(module, "inWorldSpace", world)) {
    space = world ? unity::ParticleSystemSimulationSpace::World
                  : unity::ParticleSystemSimulationSpace::Local;
  }
}

struct ParsedPPtr {
  std::int32_t file_id = 0;
  std::int64_t path_id = 0;
  bool present = false;
};

ParsedPPtr parse_pptr(std::string_view object) {
  ParsedPPtr pptr;
  if (object.empty() || object.front() != '{') return pptr;
  pptr.present = true;
  try_member_i32(object, "m_FileID", pptr.file_id);
  try_member_i64(object, "m_PathID", pptr.path_id);
  return pptr;
}

const SerializedObjectRecord* resolve_pptr(const LoadedEffect& graph,
                                           const SerializedObjectRecord& source,
                                           const ParsedPPtr& pptr) {
  if (!pptr.present || pptr.path_id == 0) return nullptr;
  if (pptr.file_id == 0) {
    for (const auto& object : graph.objects) {
      if (object.bundle == source.bundle && object.path_id == pptr.path_id) return &object;
    }
    return nullptr;
  }
  std::string target_bundle;
  for (const auto& ref : source.references) {
    if (ref.file_id == pptr.file_id && ref.path_id == pptr.path_id) {
      target_bundle = ref.target_bundle;
      break;
    }
  }
  if (target_bundle.empty()) return nullptr;
  for (const auto& object : graph.objects) {
    if (object.bundle == target_bundle && object.path_id == pptr.path_id) return &object;
  }
  return nullptr;
}

std::string_view typetree_of(const SerializedObjectRecord& record) {
  const std::string_view json = record.serialized_json;
  const std::string_view tree = member_object(json, "typetree");
  return tree.empty() ? json : tree;
}

void read_enabled(std::string_view module, bool& enabled) {
  try_member_bool(module, "enabled", enabled);
}

void hydrate_main(unity::ParticleSystem& dst, std::string_view tree,
                  std::string_view initial, const SerializedObjectRecord& record) {
  auto& main = dst.main();
  read_enabled(initial, main.enabled);
  try_member_float(tree, "lengthInSec", main.duration);
  try_member_bool(tree, "looping", main.loop);
  try_member_bool(tree, "playOnAwake", main.playOnAwake);
  try_member_float(tree, "simulationSpeed", main.simulationSpeed);
  try_member_bool(tree, "useUnscaledTime", main.useUnscaledTime);
  int stop = 0;
  if (try_member_i32(tree, "stopAction", stop))
    main.stopAction = static_cast<unity::ParticleSystemStopAction>(stop);
  int scaling = 0;
  if (try_member_i32(tree, "scalingMode", scaling))
    main.scalingMode = static_cast<unity::ParticleSystemScalingMode>(scaling);
  int ring = 0;
  if (try_member_i32(tree, "ringBufferMode", ring))
    main.ringBufferMode = ring != 0;
  else
    try_member_bool(tree, "ringBufferMode", main.ringBufferMode);
  const std::string_view loop_range = member_object(tree, "ringBufferLoopRange");
  try_member_float(loop_range, "x", main.ringBufferLoopRangeMin);
  try_member_float(loop_range, "y", main.ringBufferLoopRangeMax);
  int space = 0;
  if (try_member_i32(tree, "simulationSpace", space) ||
      try_member_i32(tree, "moveWithTransform", space)) {
    main.simulationSpace = static_cast<unity::ParticleSystemSimulationSpace>(space);
  }
  int emitter_mode = 0;
  if (try_member_i32(tree, "emitterVelocityMode", emitter_mode)) {
    main.emitterVelocityMode =
        static_cast<unity::ParticleSystemEmitterVelocityMode>(emitter_mode);
  }
  int max_particles = 0;
  if (try_member_i32(initial, "maxNumParticles", max_particles))
    main.maxParticles = max_particles;
  std::string_view start_delay = member_object(initial, "startDelay");
  if (start_delay.empty()) start_delay = member_object(tree, "startDelay");
  if (!start_delay.empty()) main.startDelay = parse_minmax_curve(start_delay);
  assign_curve(initial, "startLifetime", main.startLifetime);
  assign_curve(initial, "startSpeed", main.startSpeed);
  assign_curve(initial, "startSize", main.startSize);
  main.startSizeX = main.startSize;
  assign_curve(initial, "startSizeY", main.startSizeY);
  assign_curve(initial, "startSizeZ", main.startSizeZ);
  try_member_bool(initial, "size3D", main.startSize3D);
  assign_curve(initial, "startRotation", main.startRotation);
  main.startRotationZ = main.startRotation;
  assign_curve(initial, "startRotationX", main.startRotationX);
  assign_curve(initial, "startRotationY", main.startRotationY);
  try_member_bool(initial, "rotation3D", main.startRotation3D);
  assign_curve(initial, "gravityModifier", main.gravityModifier);
  const std::string_view start_color = member_object(initial, "startColor");
  if (!start_color.empty()) main.startColor = parse_minmax_gradient(start_color);

  bool auto_seed = record.auto_random_seed;
  std::uint32_t seed = record.random_seed;
  try_member_bool(tree, "autoRandomSeed", auto_seed);
  try_member_u32(tree, "randomSeed", seed);
  dst.set_useAutoRandomSeed(auto_seed);
  dst.set_randomSeed(seed);
}

void hydrate_emission(unity::EmissionModule& dst, std::string_view module) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  assign_curve(module, "rateOverTime", dst.rateOverTime);
  assign_curve(module, "rateOverDistance", dst.rateOverDistance);
  std::vector<unity::Burst> bursts;
  for_each_array_element(member_array(module, "m_Bursts"), [&](std::string_view item) {
    if (item.empty() || item.front() != '{') return;
    unity::Burst burst;
    try_member_float(item, "time", burst.time);
    std::string_view count = member_object(item, "countCurve");
    if (count.empty()) count = member_object(item, "count");
    if (!count.empty()) burst.count = parse_minmax_curve(count);
    try_member_i32(item, "cycleCount", burst.cycleCount);
    try_member_float(item, "repeatInterval", burst.repeatInterval);
    try_member_float(item, "probability", burst.probability);
    bursts.push_back(burst);
  });
  dst.SetBursts(bursts.data(), static_cast<int>(bursts.size()));
}

void hydrate_shape(unity::ShapeModule& dst, std::string_view module,
                   const LoadedEffect& graph, const SerializedObjectRecord& source) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  int type = 0;
  if (try_member_i32(module, "type", type))
    dst.shapeType = static_cast<unity::ParticleSystemShapeType>(type);
  try_member_float(module, "angle", dst.angle);
  try_member_float(module, "radiusThickness", dst.radiusThickness);
  try_member_float(module, "length", dst.length);
  try_member_float(module, "randomDirectionAmount", dst.randomDirectionAmount);
  try_member_float(module, "sphericalDirectionAmount", dst.sphericalDirectionAmount);
  try_member_float(module, "randomPositionAmount", dst.randomPositionAmount);
  try_member_bool(module, "alignToDirection", dst.alignToDirection);
  const std::string_view radius = member_view(module, "radius");
  if (!radius.empty() && radius.front() == '{')
    try_member_float(radius, "value", dst.radius);
  else
    try_member_float(module, "radius", dst.radius);
  const std::string_view arc = member_view(module, "arc");
  if (!arc.empty() && arc.front() == '{') {
    try_member_float(arc, "value", dst.arc);
    int arc_mode = 0;
    if (try_member_i32(arc, "mode", arc_mode))
      dst.arcMode = static_cast<unity::ParticleSystemShapeMultiModeValue>(arc_mode);
    try_member_float(arc, "spread", dst.arcSpread);
    const std::string_view speed = member_view(arc, "speed");
    if (!speed.empty() && speed.front() == '{')
      try_member_float(speed, "scalar", dst.arcSpeed);
    else
      parse_float_token(speed, dst.arcSpeed);
  } else {
    try_member_float(module, "arc", dst.arc);
  }
  parse_vec3(member_object(module, "m_Position"), dst.position);
  parse_vec3(member_object(module, "m_Rotation"), dst.rotation);
  parse_vec3(member_object(module, "m_Scale"), dst.scale);
  const ParsedPPtr mesh = parse_pptr(member_object(module, "m_Mesh"));
  if (mesh.path_id != 0) {
    dst.mesh = new (std::nothrow) unity::Mesh();
    (void)resolve_pptr(graph, source, mesh);
  }
  const ParsedPPtr sprite = parse_pptr(member_object(module, "m_Sprite"));
  if (sprite.path_id != 0) {
    dst.sprite = new (std::nothrow) unity::Sprite();
    (void)resolve_pptr(graph, source, sprite);
  }
}

void hydrate_vol(unity::VelocityOverLifetimeModule& dst, std::string_view module) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  assign_space(module, dst.space);
  assign_curve(module, "x", dst.x);
  assign_curve(module, "y", dst.y);
  assign_curve(module, "z", dst.z);
  assign_curve(module, "orbitalX", dst.orbitalX);
  assign_curve(module, "orbitalY", dst.orbitalY);
  assign_curve(module, "orbitalZ", dst.orbitalZ);
  assign_curve(module, "orbitalOffsetX", dst.orbitalOffsetX);
  assign_curve(module, "orbitalOffsetY", dst.orbitalOffsetY);
  assign_curve(module, "orbitalOffsetZ", dst.orbitalOffsetZ);
  assign_curve(module, "radial", dst.radial);
  assign_curve(module, "speedModifier", dst.speedModifier);
}

void hydrate_force(unity::ForceOverLifetimeModule& dst, std::string_view module) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  assign_space(module, dst.space);
  assign_curve(module, "x", dst.x);
  assign_curve(module, "y", dst.y);
  assign_curve(module, "z", dst.z);
  assign_curve(module, "xMultiplier", dst.xMultiplier);
  assign_curve(module, "yMultiplier", dst.yMultiplier);
  assign_curve(module, "zMultiplier", dst.zMultiplier);
  try_member_bool(module, "randomizePerFrame", dst.randomized);
}

void hydrate_limit(unity::LimitVelocityOverLifetimeModule& dst, std::string_view module) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  assign_space(module, dst.space);
  assign_curve(module, "magnitude", dst.limit);
  assign_curve(module, "x", dst.limitX);
  assign_curve(module, "y", dst.limitY);
  assign_curve(module, "z", dst.limitZ);
  assign_curve_or_float(module, "dampen", dst.dampen);
  assign_curve(module, "drag", dst.drag);
  try_member_bool(module, "separateAxis", dst.separateAxes);
  try_member_bool(module, "separateAxes", dst.separateAxes);
  try_member_bool(module, "multiplyDragByParticleSize", dst.multiplyBySize);
  try_member_bool(module, "multiplyDragByParticleVelocity", dst.multiplyByVelocity);
}

void hydrate_noise(unity::NoiseModule& dst, std::string_view module) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  assign_curve(module, "strength", dst.strength);
  dst.strengthX = dst.strength;
  assign_curve(module, "strengthY", dst.strengthY);
  assign_curve(module, "strengthZ", dst.strengthZ);
  assign_curve_or_float(module, "frequency", dst.frequency);
  assign_curve(module, "scrollSpeed", dst.scrollSpeed);
  try_member_bool(module, "damping", dst.damping);
  try_member_bool(module, "separateAxes", dst.separateAxes);
  try_member_i32(module, "octaves", dst.octaveCount);
  try_member_float(module, "octaveScale", dst.octaveScale);
  try_member_float(module, "octaveMultiplier", dst.octaveMultiplier);
  int quality = 0;
  if (try_member_i32(module, "quality", quality))
    dst.quality = static_cast<unity::ParticleSystemNoiseQuality>(quality);
  try_member_bool(module, "remapEnabled", dst.remapEnabled);
  assign_curve(module, "remap", dst.remap);
  assign_curve(module, "remapY", dst.remapY);
  assign_curve(module, "remapZ", dst.remapZ);
  assign_curve(module, "positionAmount", dst.positionAmount);
  assign_curve(module, "rotationAmount", dst.rotationAmount);
  assign_curve(module, "sizeAmount", dst.sizeAmount);
}

void hydrate_col(unity::ColorOverLifetimeModule& dst, std::string_view module) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  const std::string_view gradient = member_object(module, "gradient");
  if (!gradient.empty()) dst.color = parse_minmax_gradient(gradient);
}

void hydrate_sol(unity::SizeOverLifetimeModule& dst, std::string_view module) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  try_member_bool(module, "separateAxes", dst.separateAxes);
  assign_curve(module, "curve", dst.size);
  dst.x = dst.size;
  assign_curve(module, "x", dst.x);
  assign_curve(module, "y", dst.y);
  assign_curve(module, "z", dst.z);
}

void hydrate_rol(unity::RotationOverLifetimeModule& dst, std::string_view module) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  try_member_bool(module, "separateAxes", dst.separateAxes);
  assign_curve(module, "curve", dst.z);
  assign_curve(module, "x", dst.x);
  assign_curve(module, "y", dst.y);
  assign_curve(module, "z", dst.z);
}

void hydrate_rbs(unity::RotationBySpeedModule& dst, std::string_view module) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  try_member_bool(module, "separateAxes", dst.separateAxes);
  assign_curve(module, "curve", dst.z);
  assign_curve(module, "x", dst.x);
  assign_curve(module, "y", dst.y);
  assign_curve(module, "z", dst.z);
  assign_range_xy(module, "range", dst.range);
}

void hydrate_inherit(unity::InheritVelocityModule& dst, std::string_view module) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  try_member_i32(module, "m_Mode", dst.mode);
  try_member_i32(module, "mode", dst.mode);
  assign_curve(module, "m_Curve", dst.curve);
  assign_curve(module, "curve", dst.curve);
}

void hydrate_custom_data(unity::CustomDataModule& dst, std::string_view module) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  const char* vector_keys[2][4] = {
      {"vector0_0", "vector0_1", "vector0_2", "vector0_3"},
      {"vector1_0", "vector1_1", "vector1_2", "vector1_3"},
  };
  const char* color_keys[2] = {"color0", "color1"};
  const unity::ParticleSystemCustomData streams[2] = {
      unity::ParticleSystemCustomData::Custom1,
      unity::ParticleSystemCustomData::Custom2,
  };
  for (int stream = 0; stream < 2; ++stream) {
    for (int component = 0; component < 4; ++component) {
      const std::string_view curve = member_object(module, vector_keys[stream][component]);
      if (!curve.empty())
        dst.SetVector(streams[stream], component, parse_minmax_curve(curve));
    }
    const std::string_view color = member_object(module, color_keys[stream]);
    if (!color.empty()) dst.SetColor(streams[stream], parse_minmax_gradient(color));
  }
}

void hydrate_subemitters(unity::SubEmittersModule& dst, std::string_view module,
                         const LoadedEffect& graph, const SerializedObjectRecord& source) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  for_each_array_element(member_array(module, "subEmitters"), [&](std::string_view item) {
    if (item.empty() || item.front() != '{') return;
    unity::SubEmitter emitter;
    int type = 0;
    int properties = 0;
    try_member_i32(item, "type", type);
    try_member_i32(item, "properties", properties);
    emitter.type = static_cast<unity::ParticleSystemSubEmitterType>(type);
    emitter.properties = static_cast<unity::ParticleSystemSubEmitterProperties>(properties);
    try_member_float(item, "emitProbability", emitter.probability);
    const ParsedPPtr pptr = parse_pptr(member_object(item, "emitter"));
    (void)resolve_pptr(graph, source, pptr);
    dst.AddSubEmitter(emitter);
  });
}

void hydrate_trails(unity::TrailModule& dst, std::string_view module) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  int mode = 0;
  if (try_member_i32(module, "mode", mode))
    dst.mode = static_cast<unity::ParticleSystemTrailMode>(mode);
  try_member_float(module, "ratio", dst.ratio);
  assign_curve(module, "lifetime", dst.lifetime);
  assign_curve(module, "widthOverTrail", dst.widthOverTrail);
  try_member_bool(module, "dieWithParticles", dst.dieWithParticles);
  try_member_bool(module, "sizeAffectsWidth", dst.sizeAffectsWidth);
  try_member_bool(module, "inheritParticleColor", dst.inheritParticleColor);
  int texture_mode = 0;
  if (try_member_i32(module, "textureMode", texture_mode))
    dst.textureMode = static_cast<unity::ParticleSystemTrailTextureMode>(texture_mode);
  try_member_float(module, "minVertexDistance", dst.minVertexDistance);
  const std::string_view color_life = member_object(module, "colorOverLifetime");
  if (!color_life.empty()) dst.colorOverLifetime = parse_minmax_gradient(color_life);
  const std::string_view color_trail = member_object(module, "colorOverTrail");
  if (!color_trail.empty()) dst.colorOverTrail = parse_minmax_gradient(color_trail);
}

void hydrate_tsa(unity::TextureSheetAnimationModule& dst, std::string_view module) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  int mode = 0;
  if (try_member_i32(module, "mode", mode))
    dst.mode = static_cast<unity::ParticleSystemAnimationMode>(mode);
  int animation = 0;
  if (try_member_i32(module, "animationType", animation))
    dst.animation = static_cast<unity::ParticleSystemAnimationType>(animation);
  int time_mode = 0;
  if (try_member_i32(module, "timeMode", time_mode))
    dst.timeMode = static_cast<unity::ParticleSystemAnimationTimeMode>(time_mode);
  try_member_i32(module, "tilesX", dst.numTilesX);
  try_member_i32(module, "tilesY", dst.numTilesY);
  assign_curve(module, "frameOverTime", dst.frameOverTime);
  assign_curve(module, "startFrame", dst.startFrame);
  float cycles = 0.0f;
  if (try_member_float(module, "cycles", cycles))
    dst.cycleCount = static_cast<int>(cycles);
  try_member_float(module, "fps", dst.fps);
  try_member_i32(module, "rowIndex", dst.rowIndex);
  int row_mode = 0;
  if (try_member_i32(module, "rowMode", row_mode))
    dst.rowMode = static_cast<decltype(dst.rowMode)>(row_mode);
  try_member_i32(module, "uvChannelMask", dst.uvChannelMask);
  try_member_bool(module, "randomRow", dst.useRandomRow);
  for_each_array_element(member_array(module, "sprites"), [&](std::string_view item) {
    if (item.empty()) return;
    dst.AddSprite(std::make_shared<unity::Sprite>());
    (void)item;
  });
}

void hydrate_color_by_speed(unity::ColorBySpeedModule& dst, std::string_view module) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  const std::string_view gradient = member_object(module, "gradient");
  if (!gradient.empty()) dst.color = parse_minmax_gradient(gradient);
  assign_range_xy(module, "range", dst.range);
}

void hydrate_size_by_speed(unity::SizeBySpeedModule& dst, std::string_view module) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  try_member_bool(module, "separateAxes", dst.separateAxes);
  assign_curve(module, "curve", dst.size);
  dst.x = dst.size;
  assign_curve(module, "x", dst.x);
  assign_curve(module, "y", dst.y);
  assign_curve(module, "z", dst.z);
  assign_range_xy(module, "range", dst.range);
}

void hydrate_lights(unity::LightsModule& dst, std::string_view module) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  try_member_float(module, "ratio", dst.ratioValue);
}

void hydrate_collision(unity::CollisionModule& dst, std::string_view module) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  assign_curve(module, "m_Dampen", dst.dampen);
  assign_curve(module, "m_Bounce", dst.bounce);
  assign_curve(module, "m_EnergyLossOnCollision", dst.lifetimeLoss);
}

void hydrate_trigger(unity::TriggerModule& dst, std::string_view module) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  try_member_i32(module, "inside", dst.inside);
  try_member_i32(module, "outside", dst.outside);
  try_member_i32(module, "enter", dst.enter);
  try_member_i32(module, "exit", dst.exit);
}

void hydrate_external(unity::ExternalForcesModule& dst, std::string_view module) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  assign_curve(module, "multiplierCurve", dst.multiplier);
  assign_curve(module, "multiplier", dst.multiplier);
  try_member_float(module, "influenceFilter", dst.influenceFilter);
}

void hydrate_lifetime_by_speed(unity::LifetimeByEmitterSpeedModule& dst,
                               std::string_view module) {
  if (module.empty()) return;
  read_enabled(module, dst.enabled);
  assign_curve(module, "m_Curve", dst.curve);
  assign_curve(module, "curve", dst.curve);
}

}  // namespace

bool hydrate_particle_system(unity::ParticleSystem& dst,
                             const SerializedObjectRecord& ps,
                             const LoadedEffect& graph) noexcept {
  if (ps.serialized_type != "ParticleSystem" || ps.serialized_json.empty()) return false;
  const std::string_view tree = typetree_of(ps);
  hydrate_main(dst, tree, member_object(tree, "InitialModule"), ps);
  hydrate_emission(dst.emission(), member_object(tree, "EmissionModule"));
  hydrate_shape(dst.shape(), member_object(tree, "ShapeModule"), graph, ps);
  hydrate_vol(dst.velocityOverLifetime(), member_object(tree, "VelocityModule"));
  hydrate_force(dst.forceOverLifetime(), member_object(tree, "ForceModule"));
  hydrate_limit(dst.limitVelocityOverLifetime(), member_object(tree, "ClampVelocityModule"));
  hydrate_noise(dst.noise(), member_object(tree, "NoiseModule"));
  hydrate_col(dst.colorOverLifetime(), member_object(tree, "ColorModule"));
  hydrate_sol(dst.sizeOverLifetime(), member_object(tree, "SizeModule"));
  hydrate_rol(dst.rotationOverLifetime(), member_object(tree, "RotationModule"));
  hydrate_rbs(dst.rotationBySpeed(), member_object(tree, "RotationBySpeedModule"));
  hydrate_inherit(dst.inheritVelocity(), member_object(tree, "InheritVelocityModule"));
  hydrate_custom_data(dst.customData(), member_object(tree, "CustomDataModule"));
  hydrate_subemitters(dst.subEmitters(), member_object(tree, "SubModule"), graph, ps);
  hydrate_trails(dst.trails(), member_object(tree, "TrailModule"));
  hydrate_tsa(dst.textureSheetAnimation(), member_object(tree, "UVModule"));
  hydrate_color_by_speed(dst.colorBySpeed(), member_object(tree, "ColorBySpeedModule"));
  hydrate_size_by_speed(dst.sizeBySpeed(), member_object(tree, "SizeBySpeedModule"));
  hydrate_lights(dst.lights(), member_object(tree, "LightsModule"));
  hydrate_collision(dst.collision(), member_object(tree, "CollisionModule"));
  hydrate_trigger(dst.trigger(), member_object(tree, "TriggerModule"));
  hydrate_external(dst.externalForces(), member_object(tree, "ExternalForcesModule"));
  hydrate_lifetime_by_speed(dst.lifetimeByEmitterSpeed(),
                            member_object(tree, "LifetimeByEmitterSpeedModule"));
  return true;
}

bool hydrate_particle_system_renderer(unity::ParticleSystemRenderer& dst,
                                      const SerializedObjectRecord& psr,
                                      const LoadedEffect& graph) noexcept {
  if (psr.serialized_type != "ParticleSystemRenderer" || psr.serialized_json.empty())
    return false;
  const std::string_view tree = typetree_of(psr);
  try_member_bool(tree, "m_Enabled", dst.enabled);
  int render_mode = 0;
  if (try_member_i32(tree, "m_RenderMode", render_mode))
    dst.renderMode = static_cast<unity::ParticleSystemRenderMode>(render_mode);
  int alignment = 0;
  if (try_member_i32(tree, "m_RenderAlignment", alignment))
    dst.alignment = static_cast<unity::ParticleSystemRenderSpace>(alignment);
  try_member_float(tree, "m_LengthScale", dst.lengthScale);
  try_member_float(tree, "m_VelocityScale", dst.velocityScale);
  try_member_float(tree, "m_CameraVelocityScale", dst.cameraVelocityScale);
  try_member_float(tree, "m_MaxParticleSize", dst.maxParticleSize);
  try_member_float(tree, "m_MinParticleSize", dst.minParticleSize);
  parse_vec3(member_object(tree, "m_Pivot"), dst.pivot);
  parse_vec3(member_object(tree, "m_Flip"), dst.flip);
  try_member_i32(tree, "m_SortingOrder", dst.sortingOrder);
  try_member_bool(tree, "m_FreeformStretching", dst.freeformStretching);
  try_member_bool(tree, "m_RotateWithStretchDirection", dst.rotateWithStretchDirection);
  for_each_array_element(member_array(tree, "m_Materials"), [&](std::string_view item) {
    auto* material = new (std::nothrow) unity::Material();
    if (material == nullptr) return;
    const ParsedPPtr pptr = parse_pptr(item);
    if (const auto* resolved = resolve_pptr(graph, psr, pptr)) {
      const std::string_view resolved_tree = typetree_of(*resolved);
      const std::string_view name = member_view(resolved_tree, "m_Name");
      if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
        material->name = std::string(name.substr(1, name.size() - 2));
    }
    dst.materials.push_back(material);
    if (dst.material == nullptr) dst.material = material;
  });
  const ParsedPPtr mesh = parse_pptr(member_object(tree, "m_Mesh"));
  if (mesh.path_id != 0) {
    dst.mesh = new (std::nothrow) unity::Mesh();
    (void)resolve_pptr(graph, psr, mesh);
  }
  std::vector<unity::ParticleSystemVertexStream> streams;
  for_each_array_element(member_array(tree, "m_VertexStreams"), [&](std::string_view item) {
    int value = 0;
    if (parse_i32_token(item, value))
      streams.push_back(static_cast<unity::ParticleSystemVertexStream>(value));
  });
  if (!streams.empty()) dst.SetActiveVertexStreams(streams);
  return true;
}

namespace {

int serialized_field_index(const std::string& field, const char* prefix) {
  const std::string needle = prefix;
  if (field.compare(0, needle.size(), needle) != 0) return -1;
  try {
    return std::stoi(field.substr(needle.size()));
  } catch (...) {
    return -1;
  }
}

const SerializedObjectRecord* find_object_record(const LoadedEffect& graph,
                                                 std::string_view bundle,
                                                 std::int64_t path_id) {
  if (path_id == 0) return nullptr;
  if (graph.object_index_ready) {
    const auto it = graph.object_index.find(SerializedObjectKey{bundle, path_id});
    return it == graph.object_index.end() ? nullptr : it->second;
  }
  for (const auto& object : graph.objects) {
    if (object.bundle == bundle && object.path_id == path_id) return &object;
  }
  return nullptr;
}

std::string_view reference_bundle(const SerializedObjectRecord& referrer,
                                  const SerializedReference& ref) {
  if (ref.file_id == 0) return referrer.bundle;
  return ref.target_bundle;
}

const SerializedObjectRecord* resolve_reference(const LoadedEffect& graph,
                                                const SerializedObjectRecord& referrer,
                                                const SerializedReference& ref) {
  if (ref.path_id == 0) return nullptr;
  return find_object_record(graph, reference_bundle(referrer, ref), ref.path_id);
}

const SerializedObjectRecord* transform_record_for_game_object(
    const LoadedEffect& graph, const SerializedObjectRecord& game_object) {
  std::vector<std::pair<int, const SerializedReference*>> components;
  for (const auto& ref : game_object.references) {
    const int index = serialized_field_index(ref.field, "m_Component[");
    if (index < 0) continue;
    components.emplace_back(index, &ref);
  }
  std::sort(components.begin(), components.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& entry : components) {
    const auto* component = resolve_reference(graph, game_object, *entry.second);
    if (component != nullptr && component->serialized_type == "Transform") return component;
  }
  return nullptr;
}

void apply_local_trs(unity::Transform& transform, const SerializedObjectRecord& xf) {
  const auto position = json_vector3(xf.serialized_json, "m_LocalPosition");
  if (position.present)
    transform.localPosition = {position.x, position.y, position.z};
  const auto rotation = json_quaternion(xf.serialized_json, "m_LocalRotation");
  if (rotation.present)
    transform.localRotation = {rotation.x, rotation.y, rotation.z, rotation.w};
  const auto scale = json_vector3(xf.serialized_json, "m_LocalScale");
  if (scale.present) transform.localScale = {scale.x, scale.y, scale.z};
}

std::shared_ptr<unity::GameObject> materialize_game_object_tree(
    const LoadedEffect& graph, const SerializedObjectRecord& game_object) {
  auto go = std::make_shared<unity::GameObject>(json_string(game_object.serialized_json, "m_Name"));
  go->openwds_path_id = game_object.path_id;
  go->activeSelf = json_bool(game_object.serialized_json, "m_IsActive", true);
  const auto* transform = transform_record_for_game_object(graph, game_object);
  if (transform != nullptr) apply_local_trs(go->transform, *transform);
  if (transform == nullptr) return go;

  std::vector<std::pair<int, const SerializedReference*>> children;
  for (const auto& ref : transform->references) {
    const int index = serialized_field_index(ref.field, "m_Children[");
    if (index < 0) continue;
    children.emplace_back(index, &ref);
  }
  std::sort(children.begin(), children.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& entry : children) {
    const auto* child_transform = resolve_reference(graph, *transform, *entry.second);
    if (child_transform == nullptr || child_transform->serialized_type != "Transform") continue;
    const SerializedObjectRecord* child_go_record = nullptr;
    for (const auto& child_ref : child_transform->references) {
      if (child_ref.field != "m_GameObject") continue;
      child_go_record = resolve_reference(graph, *child_transform, child_ref);
      break;
    }
    if (child_go_record == nullptr || child_go_record->serialized_type != "GameObject") continue;
    auto child = materialize_game_object_tree(graph, *child_go_record);
    child->transform.SetParent(&go->transform, false);
    go->children_owned.push_back(std::move(child));
  }
  return go;
}

}  // namespace

void ensure_serialized_object_index(const LoadedEffect& graph) noexcept {
  if (graph.object_index_ready) return;
  graph.object_index.clear();
  graph.object_index.reserve(graph.objects.size());
  for (const auto& object : graph.objects) {
    graph.object_index.emplace(SerializedObjectKey{object.bundle, object.path_id}, &object);
  }
  graph.object_index_ready = true;
}

const SerializedObjectRecord* find_serialized_object(const LoadedEffect& graph,
                                                     const std::string& bundle,
                                                     std::int64_t path_id) noexcept {
  return find_object_record(graph, bundle, path_id);
}

const SerializedObjectRecord* resolve_serialized_reference(
    const LoadedEffect& graph, const SerializedObjectRecord& referrer,
    const SerializedReference& ref) noexcept {
  return resolve_reference(graph, referrer, ref);
}

const SerializedObjectRecord* find_game_object_component(
    const LoadedEffect& graph, const SerializedObjectRecord& game_object,
    std::string_view serialized_type) noexcept {
  if (game_object.serialized_type != "GameObject") return nullptr;
  std::vector<std::pair<int, const SerializedReference*>> components;
  for (const auto& ref : game_object.references) {
    const int index = serialized_field_index(ref.field, "m_Component[");
    if (index < 0) continue;
    components.emplace_back(index, &ref);
  }
  std::sort(components.begin(), components.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& entry : components) {
    const auto* component = resolve_reference(graph, game_object, *entry.second);
    if (component != nullptr && component->serialized_type == serialized_type) return component;
  }
  return nullptr;
}

std::shared_ptr<unity::GameObject> materialize_element_prefab(
    const LoadedEffect& graph, const ControllerElementMetadata& element) {
  const SerializedObjectRecord* behaviour = nullptr;
  if (element.component.path_id != 0) {
    const std::string& bundle = element.component.file_id == 0
                                    ? (element.component.target_bundle.empty()
                                           ? std::string{}
                                           : element.component.target_bundle)
                                    : element.component.target_bundle;
    behaviour = find_object_record(graph, bundle, element.component.path_id);
    if (behaviour == nullptr && !element.component.target_bundle.empty())
      behaviour = find_object_record(graph, element.component.target_bundle,
                                     element.component.path_id);
  }
  if (behaviour == nullptr) return {};
  const SerializedObjectRecord* root_go = nullptr;
  for (const auto& ref : behaviour->references) {
    if (ref.field != "m_GameObject") continue;
    root_go = resolve_reference(graph, *behaviour, ref);
    break;
  }
  if (root_go == nullptr || root_go->serialized_type != "GameObject") return {};
  return materialize_game_object_tree(graph, *root_go);
}

}  // namespace wds::chart_render::openwds_split_effects
