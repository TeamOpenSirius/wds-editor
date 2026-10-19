#include <wds/core/jacket_image.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace wds::chart_editor {
namespace {

unsigned char lerp_u8(unsigned char a, unsigned char b, float t) noexcept {
  return static_cast<unsigned char>(
      std::lround(static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t));
}

}  // namespace

bool prepare_official_jacket_rgba(const unsigned char* src, int width, int height,
                                  std::vector<unsigned char>& out) {
  out.clear();
  if (src == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  const JacketSquareCrop crop = center_square_crop(width, height);
  if (crop.side <= 0) {
    return false;
  }
  constexpr int kDst = kOfficialJacketPixelSize;
  const std::size_t src_stride = static_cast<std::size_t>(width) * 4u;
  out.assign(static_cast<std::size_t>(kDst) * static_cast<std::size_t>(kDst) * 4u, 0);
  const float scale = static_cast<float>(crop.side) / static_cast<float>(kDst);
  const int side_max = crop.side - 1;
  for (int y = 0; y < kDst; ++y) {
    const float sy = (static_cast<float>(y) + 0.5f) * scale - 0.5f;
    const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, side_max);
    const int y1 = std::min(y0 + 1, side_max);
    const float fy = std::clamp(sy - static_cast<float>(y0), 0.0f, 1.0f);
    for (int x = 0; x < kDst; ++x) {
      const float sx = (static_cast<float>(x) + 0.5f) * scale - 0.5f;
      const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, side_max);
      const int x1 = std::min(x0 + 1, side_max);
      const float fx = std::clamp(sx - static_cast<float>(x0), 0.0f, 1.0f);
      const std::size_t row0 =
          (static_cast<std::size_t>(crop.y + y0) * src_stride) +
          static_cast<std::size_t>(crop.x + x0) * 4u;
      const std::size_t row0x1 =
          (static_cast<std::size_t>(crop.y + y0) * src_stride) +
          static_cast<std::size_t>(crop.x + x1) * 4u;
      const std::size_t row1 =
          (static_cast<std::size_t>(crop.y + y1) * src_stride) +
          static_cast<std::size_t>(crop.x + x0) * 4u;
      const std::size_t row1x1 =
          (static_cast<std::size_t>(crop.y + y1) * src_stride) +
          static_cast<std::size_t>(crop.x + x1) * 4u;
      unsigned char* dst =
          out.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(kDst) +
                        static_cast<std::size_t>(x)) *
                           4u;
      for (int c = 0; c < 4; ++c) {
        const unsigned char v0 = lerp_u8(src[row0 + static_cast<std::size_t>(c)],
                                         src[row0x1 + static_cast<std::size_t>(c)], fx);
        const unsigned char v1 = lerp_u8(src[row1 + static_cast<std::size_t>(c)],
                                         src[row1x1 + static_cast<std::size_t>(c)], fx);
        dst[c] = lerp_u8(v0, v1, fy);
      }
    }
  }
  return true;
}

}  // namespace wds::chart_editor
