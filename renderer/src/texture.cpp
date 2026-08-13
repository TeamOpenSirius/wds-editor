#include "wds/renderer/texture.hpp"

#include <wds/common/utf8_path.hpp>

#include <png.h>

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wds::renderer {
namespace {

// Read whole file into a mutable buffer (nsvgParse overwrites its input).
// Prefer this over nsvgParseFromFile so Windows Unicode/UTF-8 install paths work.
bool read_file_bytes(const std::string& path, std::vector<char>& out) {
  if (!wds::common::read_file_bytes(path, out) || out.empty()) {
    return false;
  }
  out.push_back('\0');
  return true;
}

// Icon SVGs are stroke art (~15–45% coverage). Reject empty or slab fills that
// would paint as solid white squares when tinted {1,1,1,1}.
bool svg_raster_looks_like_icon(const unsigned char* pixels, int width, int height) {
  if (pixels == nullptr || width <= 2 || height <= 2) {
    return false;
  }
  const int total = width * height;
  int opaque = 0;
  for (int i = 0; i < total; ++i) {
    if (pixels[static_cast<size_t>(i) * 4u + 3u] > 24) {
      ++opaque;
    }
  }
  const float coverage = static_cast<float>(opaque) / static_cast<float>(total);
  return coverage >= 0.02f && coverage <= 0.85f;
}

}  // namespace

bool load_png_rgba8(const std::string& path, std::vector<unsigned char>& out, int& width,
                    int& height) {
  FILE* fp = wds::common::fopen_utf8(path, "rb");
  if (fp == nullptr) {
    return false;
  }

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (png == nullptr) {
    // Typical CI failure mode: compiled against libpng 1.4 headers but linked /
    // bundled 1.6 — create_read_struct rejects the version string.
    std::fprintf(stderr,
                 "load_png_rgba8: png_create_read_struct failed for %s "
                 "(headers=" PNG_LIBPNG_VER_STRING ", runtime=%lu)\n",
                 path.c_str(),
                 static_cast<unsigned long>(png_access_version_number()));
    std::fclose(fp);
    return false;
  }
  png_infop info = png_create_info_struct(png);
  if (info == nullptr) {
    png_destroy_read_struct(&png, nullptr, nullptr);
    std::fclose(fp);
    return false;
  }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(fp);
    return false;
  }

  png_init_io(png, fp);
  png_read_info(png, info);

  width = static_cast<int>(png_get_image_width(png, info));
  height = static_cast<int>(png_get_image_height(png, info));
  const png_byte color_type = png_get_color_type(png, info);
  const png_byte bit_depth = png_get_bit_depth(png, info);

  if (bit_depth == 16) {
    png_set_strip_16(png);
  }
  if (color_type == PNG_COLOR_TYPE_PALETTE) {
    png_set_palette_to_rgb(png);
  }
  if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
    png_set_expand_gray_1_2_4_to_8(png);
  }
  if (png_get_valid(png, info, PNG_INFO_tRNS)) {
    png_set_tRNS_to_alpha(png);
  }
  if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY ||
      color_type == PNG_COLOR_TYPE_PALETTE) {
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
  }
  if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
    png_set_gray_to_rgb(png);
  }

  png_read_update_info(png, info);

  const size_t row_bytes = png_get_rowbytes(png, info);
  out.resize(row_bytes * static_cast<size_t>(height));
  std::vector<png_bytep> rows(static_cast<size_t>(height));
  for (int y = 0; y < height; ++y) {
    rows[static_cast<size_t>(height - 1 - y)] = out.data() + static_cast<size_t>(y) * row_bytes;
  }
  png_read_image(png, rows.data());
  png_destroy_read_struct(&png, &info, nullptr);
  std::fclose(fp);
  return true;
}

namespace {

int next_pow2(int v) {
  int p = 64;
  while (p < v) {
    p *= 2;
  }
  return p;
}

struct PackRect {
  const std::string* path = nullptr;
  int width = 0;
  int height = 0;
  int x = 0;
  int y = 0;
};

bool shelf_pack(std::vector<PackRect>& rects, int atlas_w, int atlas_h, int padding) {
  std::sort(rects.begin(), rects.end(), [](const PackRect& a, const PackRect& b) {
    if (a.height != b.height) {
      return a.height > b.height;
    }
    return a.width > b.width;
  });

  int shelf_y = padding;
  int shelf_h = 0;
  int x = padding;

  for (auto& r : rects) {
    if (r.width + 2 * padding > atlas_w || r.height + 2 * padding > atlas_h) {
      return false;
    }
    if (x + r.width + padding > atlas_w) {
      shelf_y += shelf_h + padding;
      x = padding;
      shelf_h = 0;
    }
    if (shelf_y + r.height + padding > atlas_h) {
      return false;
    }
    r.x = x;
    r.y = shelf_y;
    x += r.width + padding;
    shelf_h = std::max(shelf_h, r.height);
  }
  return true;
}

}  // namespace

TextureInfo create_texture_from_png(VulkanRenderer& renderer, const std::string& path) {
  std::vector<unsigned char> pixels;
  int width = 0;
  int height = 0;
  if (!load_png_rgba8(path, pixels, width, height) || width <= 0 || height <= 0) {
    return {};
  }
  return renderer.create_texture_rgba(pixels.data(), width, height);
}

TextureInfo create_texture_from_svg(VulkanRenderer& renderer, const std::string& path,
                                    int target_size) {
  if (target_size <= 0) {
    return {};
  }
  std::vector<char> file_bytes;
  if (!read_file_bytes(path, file_bytes)) {
    return {};
  }
  NSVGimage* image = nsvgParse(file_bytes.data(), "px", 96.0f);
  if (image == nullptr || image->width <= 0.0f || image->height <= 0.0f) {
    if (image != nullptr) {
      nsvgDelete(image);
    }
    return {};
  }

  const float src_w = image->width;
  const float src_h = image->height;
  const float out_scale = static_cast<float>(target_size) / std::max(src_w, src_h);
  const int width = std::max(1, static_cast<int>(std::ceil(src_w * out_scale)));
  const int height = std::max(1, static_cast<int>(std::ceil(src_h * out_scale)));

  // 2× supersample into an exact 2× buffer, then box-filter down.
  constexpr int kSSAA = 2;
  const int hi_w = width * kSSAA;
  const int hi_h = height * kSSAA;
  const float hi_scale = out_scale * static_cast<float>(kSSAA);

  std::vector<unsigned char> hi(static_cast<size_t>(hi_w) * static_cast<size_t>(hi_h) * 4u, 0);
  NSVGrasterizer* rast = nsvgCreateRasterizer();
  if (rast == nullptr) {
    nsvgDelete(image);
    return {};
  }
  nsvgRasterize(rast, image, 0.0f, 0.0f, hi_scale, hi.data(), hi_w, hi_h, hi_w * 4);
  nsvgDeleteRasterizer(rast);
  nsvgDelete(image);

  std::vector<unsigned char> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u,
                                    0);

  // Premultiplied box filter (SSAA downsample).
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int x0 = x * kSSAA;
      const int y0 = y * kSSAA;
      unsigned sum[4] = {0, 0, 0, 0};
      for (int oy = 0; oy < kSSAA; ++oy) {
        for (int ox = 0; ox < kSSAA; ++ox) {
          const unsigned char* p =
              hi.data() + (static_cast<size_t>(y0 + oy) * hi_w + (x0 + ox)) * 4u;
          const unsigned a = p[3];
          sum[0] += static_cast<unsigned>(p[0]) * a;
          sum[1] += static_cast<unsigned>(p[1]) * a;
          sum[2] += static_cast<unsigned>(p[2]) * a;
          sum[3] += a;
        }
      }
      unsigned char* dst = pixels.data() + (static_cast<size_t>(y) * width + x) * 4u;
      constexpr unsigned kCount = static_cast<unsigned>(kSSAA * kSSAA);
      if (sum[3] == 0) {
        dst[0] = dst[1] = dst[2] = dst[3] = 0;
        continue;
      }
      dst[3] = static_cast<unsigned char>(sum[3] / kCount);
      if (dst[3] == 0) {
        dst[0] = dst[1] = dst[2] = 0;
      } else {
        dst[0] = static_cast<unsigned char>(sum[0] / sum[3]);
        dst[1] = static_cast<unsigned char>(sum[1] / sum[3]);
        dst[2] = static_cast<unsigned char>(sum[2] / sum[3]);
      }
    }
  }

  if (!svg_raster_looks_like_icon(pixels.data(), width, height)) {
    return {};
  }

  // Match PNG loader's bottom-up flip for the Vulkan upload convention.
  std::vector<unsigned char> flipped(pixels.size());
  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  for (int y = 0; y < height; ++y) {
    const unsigned char* src = pixels.data() + static_cast<size_t>(y) * row_bytes;
    unsigned char* dst = flipped.data() + static_cast<size_t>(height - 1 - y) * row_bytes;
    std::memcpy(dst, src, row_bytes);
  }

  return renderer.create_texture_rgba(flipped.data(), width, height);
}

TextureCache::~TextureCache() { clear(); }

bool TextureCache::queue_png(const std::string& path) {
  if (pending_.count(path) || cache_.count(path)) {
    return true;
  }
  CpuImage image;
  if (!load_png_rgba8(path, image.pixels, image.width, image.height)) {
    return false;
  }
  pending_.emplace(path, std::move(image));
  baked_ = false;
  return true;
}

bool TextureCache::queue_rgba(const std::string& key, std::vector<unsigned char> pixels, int width,
                              int height) {
  if (key.empty() || width <= 0 || height <= 0) {
    return false;
  }
  const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
  if (pixels.size() != expected) {
    return false;
  }
  CpuImage image;
  image.pixels = std::move(pixels);
  image.width = width;
  image.height = height;
  pending_[key] = std::move(image);
  baked_ = false;
  return true;
}

bool TextureCache::bake_atlas() {
  if (renderer_ == nullptr || !renderer_->ready()) {
    return false;
  }
  if (pending_.empty() && baked_ && atlas_id_ != kInvalidTextureId) {
    return true;
  }
  if (pending_.empty()) {
    return false;
  }

  constexpr int kPadding = 2;
  std::vector<PackRect> rects;
  rects.reserve(pending_.size());
  for (auto& kv : pending_) {
    PackRect r;
    r.path = &kv.first;
    r.width = kv.second.width;
    r.height = kv.second.height;
    rects.push_back(r);
  }

  int atlas_w = 512;
  int atlas_h = 512;
  bool packed = false;
  // Soft-expanded split lines (8×256 × ~1k) need up to 4096×4096; keep headroom for
  // 8192×4096 before giving up (MoltenVK / desktop limits are typically ≥8192).
  for (int attempt = 0; attempt < 10; ++attempt) {
    auto try_rects = rects;
    if (shelf_pack(try_rects, atlas_w, atlas_h, kPadding)) {
      rects = std::move(try_rects);
      packed = true;
      break;
    }
    if (atlas_w <= atlas_h) {
      atlas_w = next_pow2(atlas_w + 1);
    } else {
      atlas_h = next_pow2(atlas_h + 1);
    }
  }
  if (!packed) {
    std::fprintf(stderr,
                 "TextureCache::bake_atlas: shelf pack failed after growing to %dx%d (%zu sprites)\n",
                 atlas_w, atlas_h, rects.size());
    return false;
  }

  std::vector<unsigned char> atlas(static_cast<size_t>(atlas_w) * atlas_h * 4, 0);
  for (const auto& r : rects) {
    const CpuImage& src = pending_[*r.path];
    for (int y = 0; y < r.height; ++y) {
      const unsigned char* src_row =
          src.pixels.data() + static_cast<size_t>(y) * src.width * 4;
      unsigned char* dst_row =
          atlas.data() + (static_cast<size_t>(r.y + y) * atlas_w + r.x) * 4;
      std::memcpy(dst_row, src_row, static_cast<size_t>(r.width) * 4);
    }
    // Bleed edge pixels 1px into padding so bilinear filtering doesn't sample
    // black atlas zeros (which darkens translucent sprites).
    auto px = [&](int x, int y) -> unsigned char* {
      x = std::clamp(x, 0, atlas_w - 1);
      y = std::clamp(y, 0, atlas_h - 1);
      return atlas.data() + (static_cast<size_t>(y) * atlas_w + x) * 4;
    };
    auto copy4 = [](unsigned char* d, const unsigned char* s) {
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
      d[3] = s[3];
    };
    for (int y = 0; y < r.height; ++y) {
      copy4(px(r.x - 1, r.y + y), px(r.x, r.y + y));
      copy4(px(r.x + r.width, r.y + y), px(r.x + r.width - 1, r.y + y));
    }
    for (int x = -1; x <= r.width; ++x) {
      copy4(px(r.x + x, r.y - 1), px(r.x + std::clamp(x, 0, r.width - 1), r.y));
      copy4(px(r.x + x, r.y + r.height),
            px(r.x + std::clamp(x, 0, r.width - 1), r.y + r.height - 1));
    }
  }

  if (atlas_id_ != kInvalidTextureId) {
    renderer_->destroy_texture(atlas_id_);
    atlas_id_ = kInvalidTextureId;
  }

  TextureInfo atlas_tex = renderer_->create_texture_rgba(atlas.data(), atlas_w, atlas_h);
  if (!atlas_tex) {
    return false;
  }
  atlas_id_ = atlas_tex.id;

  cache_.clear();
  const float inv_w = 1.0f / static_cast<float>(atlas_w);
  const float inv_h = 1.0f / static_cast<float>(atlas_h);
  for (const auto& r : rects) {
    TextureInfo info;
    info.id = atlas_id_;
    info.width = r.width;
    info.height = r.height;
    // Half-texel inset to reduce linear filtering bleed.
    info.u0 = (static_cast<float>(r.x) + 0.5f) * inv_w;
    info.v0 = (static_cast<float>(r.y) + 0.5f) * inv_h;
    info.u1 = (static_cast<float>(r.x + r.width) - 0.5f) * inv_w;
    info.v1 = (static_cast<float>(r.y + r.height) - 0.5f) * inv_h;
    cache_.emplace(*r.path, info);
  }

  pending_.clear();
  baked_ = true;
  return true;
}

TextureInfo TextureCache::load_standalone_png(const std::string& path) {
  if (renderer_ == nullptr || !renderer_->ready() || path.empty()) {
    return {};
  }
  const auto it = cache_.find(path);
  if (it != cache_.end()) {
    return it->second;
  }
  TextureInfo info = create_texture_from_png(*renderer_, path);
  if (!info) {
    return {};
  }
  standalone_ids_.push_back(info.id);
  cache_.emplace(path, info);
  return info;
}

TextureInfo TextureCache::get(const std::string& path) const {
  const auto it = cache_.find(path);
  if (it == cache_.end()) {
    return {};
  }
  return it->second;
}

void TextureCache::clear() {
  if (renderer_ != nullptr) {
    if (atlas_id_ != kInvalidTextureId) {
      renderer_->destroy_texture(atlas_id_);
    }
    for (TextureId id : standalone_ids_) {
      if (id != kInvalidTextureId && id != atlas_id_) {
        renderer_->destroy_texture(id);
      }
    }
  }
  atlas_id_ = kInvalidTextureId;
  standalone_ids_.clear();
  pending_.clear();
  cache_.clear();
  baked_ = false;
}

}  // namespace wds::renderer
