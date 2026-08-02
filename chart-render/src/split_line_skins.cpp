#include <wds/chart_render/split_line_skins.hpp>

#include <algorithm>
#include <filesystem>
#include <string>

namespace wds::renderer {

namespace {

namespace fs = std::filesystem;

#include "split_line_color_table.inc"

// '#' replaced with '_' so macOS codesign can seal the .app bundle.
constexpr const char* kBasePrefix = "Sirius Split Line _";
constexpr const char* kT1Prefix = "Sirius Split Line Transform 1 _";
constexpr const char* kT2Prefix = "Sirius Split Line Transform 2 _";

bool is_pure_numeric_suffix(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

bool parse_split_suffix(const std::string& filename, const char* prefix, std::string& out_suffix) {
  const std::string pref(prefix);
  if (filename.size() <= pref.size() + 4) {
    return false;
  }
  if (filename.compare(0, pref.size(), pref) != 0) {
    return false;
  }
  if (filename.size() < 4 || filename.compare(filename.size() - 4, 4, ".png") != 0) {
    return false;
  }
  out_suffix = filename.substr(pref.size(), filename.size() - pref.size() - 4);
  return !out_suffix.empty();
}

}  // namespace

void SplitLineSkinBank::queue_all(TextureCache& cache, const std::string& skins_directory) {
  path_base_.clear();
  path_t1_.clear();
  path_t2_.clear();
  suffix_exists_.clear();

  std::error_code ec;
  const fs::path dir(skins_directory);
  if (!fs::is_directory(dir, ec) || ec) {
    return;
  }

  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (ec || !entry.is_regular_file(ec) || ec) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    std::string suffix;
    std::string* dest = nullptr;
    if (parse_split_suffix(name, kBasePrefix, suffix)) {
      dest = &path_base_[suffix];
    } else if (parse_split_suffix(name, kT1Prefix, suffix)) {
      dest = &path_t1_[suffix];
    } else if (parse_split_suffix(name, kT2Prefix, suffix)) {
      dest = &path_t2_[suffix];
    } else {
      continue;
    }
    const std::string path = entry.path().string();
    if (cache.queue_png(path)) {
      *dest = path;
      suffix_exists_[suffix] = true;
    }
  }
}

void SplitLineSkinBank::bind_after_bake(TextureCache& cache, const std::string& /*skins_directory*/) {
  by_suffix_.clear();
  for (const auto& [suffix, exists] : suffix_exists_) {
    if (!exists) {
      continue;
    }
    SplitLineVariant v;
    if (const auto it = path_base_.find(suffix); it != path_base_.end()) {
      v.base = cache.get(it->second);
    }
    if (const auto it = path_t1_.find(suffix); it != path_t1_.end()) {
      v.transform1 = cache.get(it->second);
    }
    if (const auto it = path_t2_.find(suffix); it != path_t2_.end()) {
      v.transform2 = cache.get(it->second);
    }
    if (v.base || v.transform1 || v.transform2) {
      by_suffix_.emplace(suffix, v);
    }
  }
}

std::vector<std::string> SplitLineSkinBank::resolve_suffixes(int32_t color_id) const {
  for (const auto& entry : kSplitColorOverrides) {
    if (entry.color_id == color_id) {
      std::vector<std::string> out;
      out.reserve(entry.count);
      for (size_t i = 0; i < entry.count; ++i) {
        out.emplace_back(entry.suffixes[i]);
      }
      return out;
    }
  }

  const std::string id = std::to_string(color_id);
  if (suffix_exists_.count(id) != 0) {
    return {id};
  }

  // #1060a / #1060b style (suffix after underscore separator in filename)
  std::vector<std::string> lettered;
  for (char c = 'a'; c <= 'z'; ++c) {
    const std::string s = id + c;
    if (suffix_exists_.count(s) == 0) {
      break;
    }
    lettered.push_back(s);
  }
  if (!lettered.empty()) {
    return lettered;
  }

  // #10321_1 style
  std::vector<std::string> numbered;
  for (int i = 1; i <= 16; ++i) {
    const std::string s = id + '_' + std::to_string(i);
    if (suffix_exists_.count(s) == 0) {
      break;
    }
    numbered.push_back(s);
  }
  if (!numbered.empty()) {
    return numbered;
  }

  // Official default red (#1) only as last resort.
  if (suffix_exists_.count("1") != 0) {
    return {"1"};
  }
  return {};
}

const SplitLineVariant* SplitLineSkinBank::variant_for_suffix(
    const std::string& suffix) const noexcept {
  const auto it = by_suffix_.find(suffix);
  if (it == by_suffix_.end()) {
    return nullptr;
  }
  return &it->second;
}

std::vector<int32_t> SplitLineSkinBank::catalog_color_ids() const {
  std::vector<int32_t> out;
  out.reserve(std::size(kSplitColorOverrides) + suffix_exists_.size());
  for (const auto& entry : kSplitColorOverrides) {
    out.push_back(entry.color_id);
  }
  for (const auto& [suffix, exists] : suffix_exists_) {
    if (!exists || !is_pure_numeric_suffix(suffix)) continue;
    const int32_t id = std::stoi(suffix);
    if (std::find(out.begin(), out.end(), id) == out.end()) {
      out.push_back(id);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

const TextureInfo* SplitLineSkinBank::texture_for(int32_t color_id, int32_t line_slot,
                                                  int32_t anim_phase) const {
  const std::vector<std::string> suffixes = resolve_suffixes(color_id);
  if (suffixes.empty()) {
    return nullptr;
  }
  const size_t idx =
      static_cast<size_t>(line_slot < 0 ? 0 : line_slot) % suffixes.size();
  const SplitLineVariant* variant = variant_for_suffix(suffixes[idx]);
  if (variant == nullptr) {
    return nullptr;
  }
  return variant->for_phase(anim_phase);
}

std::vector<std::string> SplitLineSkinBank::suffixes_for(int32_t color_id) const {
  return resolve_suffixes(color_id);
}

}  // namespace wds::renderer
