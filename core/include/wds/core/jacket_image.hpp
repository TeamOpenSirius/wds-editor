#pragma once

#include <wds/core/official_playfield.hpp>

#include <vector>

namespace wds::chart_editor {

struct JacketSquareCrop {
  int x = 0;
  int y = 0;
  int side = 0;
};

// Center-square crop: side = min(w,h), long-edge origin (long - side) / 2 (integer).
inline JacketSquareCrop center_square_crop(int width, int height) noexcept {
  JacketSquareCrop out;
  if (width <= 0 || height <= 0) {
    return out;
  }
  out.side = width < height ? width : height;
  out.x = (width - out.side) / 2;
  out.y = (height - out.side) / 2;
  return out;
}

// Crop packed RGBA8 to 1:1 then bilinear-scale to official 800×800.
// Empty / non-positive size fails and leaves `out` empty.
bool prepare_official_jacket_rgba(const unsigned char* src, int width, int height,
                                  std::vector<unsigned char>& out);

}  // namespace wds::chart_editor
