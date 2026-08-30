#include <wds/chart_render/split_line_skins.hpp>

#include <wds/chart_render/split_line_official_colors.hpp>
#include <wds/chart_render/split_soft_profile.hpp>
#include <wds/common/utf8_path.hpp>
#include <wds/renderer/texture.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace wds::renderer {

namespace {

namespace fs = std::filesystem;

using wds::common::is_directory_utf8;
using wds::common::path_from_utf8;
using wds::common::path_to_utf8;

#include "split_line_color_table.inc"

// '#' replaced with '_' so macOS codesign can seal the .app bundle.
constexpr const char* kBasePrefix = "Sirius Split Line _";
constexpr const char* kT1Prefix = "Sirius Split Line Transform 1 _";
constexpr const char* kT2Prefix = "Sirius Split Line Transform 2 _";

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

// Official ribbons are soft-edged white sprites tinted by LineColor. Expand thin /
// already-narrow sources with the official horizontal AA profile. PNG RGB is discarded.
bool queue_split_skin(TextureCache& cache, const std::string& path, std::string& out_key) {
  std::vector<unsigned char> px;
  int w = 0;
  int h = 0;
  if (!load_png_rgba8(path, px, w, h) || w <= 0 || h <= 0) {
    return false;
  }

  // Re-soft even 8px plates: stock 8-tap has an opaque core; preview wants a glow beam.
  constexpr int kSoftW = kSplitSoftPlateW;
  std::vector<unsigned char> soft(static_cast<size_t>(kSoftW) * static_cast<size_t>(h) * 4u);
  const int sx = w / 2;
  for (int y = 0; y < h; ++y) {
    const unsigned char* src =
        px.data() + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(sx)) * 4u;
    const float sa = static_cast<float>(src[3]) / 255.0f;
    for (int x = 0; x < kSoftW; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(kSoftW);
      const float edge = split_soft_edge_alpha(u);
      unsigned char* d =
          soft.data() +
          (static_cast<size_t>(y) * static_cast<size_t>(kSoftW) + static_cast<size_t>(x)) * 4u;
      write_white_soft_texel(d, sa * edge);
    }
  }
  const std::string key = path + "##soft48g";
  if (!cache.queue_rgba(key, std::move(soft), kSoftW, h)) {
    return false;
  }
  out_key = key;
  return true;
}

}  // namespace

void SplitLineSkinBank::queue_all(TextureCache& cache, const std::string& skins_directory) {
  path_base_.clear();
  path_t1_.clear();
  path_t2_.clear();
  suffix_exists_.clear();

  if (!is_directory_utf8(skins_directory)) {
    return;
  }

  auto collect_dir = [](const fs::path& dir) {
    std::vector<std::string> files;
#if defined(_WIN32)
    files = wds::common::list_regular_files_utf8(path_to_utf8(dir));
#else
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) {
      return files;
    }
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      if (ec || !entry.is_regular_file(ec) || ec) {
        continue;
      }
      files.push_back(path_to_utf8(entry.path()));
    }
#endif
    return files;
  };

  auto ingest = [&](const std::vector<std::string>& files, bool effects_tree) {
    for (const std::string& path : files) {
      const std::string name = path_to_utf8(path_from_utf8(path).filename());
      std::string suffix;
      std::string* dest = nullptr;
      if (effects_tree) {
        // skins/effects/split/lines/{base,transform1,transform2}/{suffix}.png
        const fs::path parent = path_from_utf8(path).parent_path();
        const std::string folder = path_to_utf8(parent.filename());
        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".png") != 0) {
          continue;
        }
        suffix = name.substr(0, name.size() - 4);
        if (suffix.empty()) continue;
        if (folder == "base") {
          dest = &path_base_[suffix];
        } else if (folder == "transform1") {
          dest = &path_t1_[suffix];
        } else if (folder == "transform2") {
          dest = &path_t2_[suffix];
        } else {
          continue;
        }
      } else if (parse_split_suffix(name, kBasePrefix, suffix)) {
        dest = &path_base_[suffix];
      } else if (parse_split_suffix(name, kT1Prefix, suffix)) {
        dest = &path_t1_[suffix];
      } else if (parse_split_suffix(name, kT2Prefix, suffix)) {
        dest = &path_t2_[suffix];
      } else {
        continue;
      }
      std::string key;
      if (queue_split_skin(cache, path, key)) {
        *dest = key;
        suffix_exists_[suffix] = true;
      }
    }
  };

  const fs::path root = path_from_utf8(skins_directory);
  // Root base plates first; effects/split/lines/base overrides when present.
  // Light mode does not draw Transform wipe sprites — skip those trees.
  ingest(collect_dir(root), false);
  const fs::path effects_lines = root / "effects" / "split" / "lines";
  ingest(collect_dir(effects_lines / "base"), true);
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
  return wds::chart_render::official_split_color_ids();
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

bool SplitLineSkinBank::color_for(int32_t color_id, int32_t line_slot, float& r, float& g,
                                  float& b) const {
  float a = 1.0f;
  return color_for(color_id, line_slot, r, g, b, a);
}

bool SplitLineSkinBank::color_for(int32_t color_id, int32_t line_slot, float& r, float& g, float& b,
                                  float& a) const {
  return wds::chart_render::official_split_line_color(color_id, line_slot, r, g, b, a);
}

std::vector<std::string> SplitLineSkinBank::suffixes_for(int32_t color_id) const {
  return resolve_suffixes(color_id);
}

}  // namespace wds::renderer
