#pragma once

#include "theme.hpp"
#include "types.hpp"
#include "ui_painter.hpp"
#include "widget.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wds::interaction::popup_menu {

inline constexpr float kMenuZ = 0.9990f;
inline constexpr float kMenuRowZ = 0.9992f;
inline constexpr float kMenuHoverZ = 0.99925f;
inline constexpr float kMenuTextZ = 0.9993f;
// Host field chrome re-drawn above menu contents so scrolled rows cannot cover it.
inline constexpr float kHostZ = 0.9996f;
inline constexpr float kHostTextZ = 0.9997f;
inline constexpr float kWindowMargin = 4.0f;
inline constexpr int kMaxVisibleItems = 5;

enum class Placement : uint8_t {
  Down,  // always open below the field
  Up,    // always open above the field
};

inline float window_top(const Widget* widget) noexcept {
  const Widget* root = widget;
  while (root != nullptr && root->parent() != nullptr) {
    root = root->parent();
  }
  return root != nullptr ? root->bounds().y : 0.0f;
}

inline float window_bottom(const Widget* widget) noexcept {
  const Widget* root = widget;
  while (root != nullptr && root->parent() != nullptr) {
    root = root->parent();
  }
  return root != nullptr ? root->bounds().bottom() : 0.0f;
}

struct Geometry {
  Rect rect{};
  float content_h = 0.0f;
  float scroll = 0.0f;
  float row_h = theme::kControlHeight;
  int first_index = 0;
  int visible_count = 0;
  bool opens_upward = false;
};

inline Geometry layout(const Widget* host, const Rect& field_abs, std::size_t item_count,
                       float scroll_px, Placement placement = Placement::Down) noexcept {
  Geometry g;
  g.row_h = theme::kControlHeight;
  if (item_count == 0) {
    return g;
  }
  g.content_h = g.row_h * static_cast<float>(item_count);
  g.opens_upward = placement == Placement::Up;
  const float top = window_top(host) + kWindowMargin;
  const float bottom = window_bottom(host) - kWindowMargin;
  const float space_below = std::max(0.0f, bottom - field_abs.bottom());
  const float space_above = std::max(0.0f, field_abs.y - top);
  const float space = g.opens_upward ? space_above : space_below;
  const float max_h = g.row_h * static_cast<float>(kMaxVisibleItems);
  const float menu_h = std::min({g.content_h, space, max_h});
  if (menu_h < 1.0f) {
    return g;
  }
  const float max_scroll = std::max(0.0f, g.content_h - menu_h);
  g.scroll = std::clamp(scroll_px, 0.0f, max_scroll);
  if (g.opens_upward) {
    g.rect = {field_abs.x, field_abs.y - menu_h, field_abs.w, menu_h};
  } else {
    g.rect = {field_abs.x, field_abs.bottom(), field_abs.w, menu_h};
  }
  g.first_index = static_cast<int>(g.scroll / g.row_h);
  const int last = static_cast<int>(std::ceil((g.scroll + menu_h) / g.row_h));
  g.visible_count = std::clamp(last - g.first_index, 0, static_cast<int>(item_count));
  return g;
}

inline float clamp_scroll(const Geometry& g, float scroll_px) noexcept {
  const float max_scroll = std::max(0.0f, g.content_h - g.rect.h);
  return std::clamp(scroll_px, 0.0f, max_scroll);
}

inline std::optional<double> parse_item_number(std::string_view text) noexcept {
  auto try_parse = [](std::string_view s) -> std::optional<double> {
    if (s.empty()) {
      return std::nullopt;
    }
    try {
      std::size_t parsed = 0;
      const double value = std::stod(std::string(s), &parsed);
      if (parsed == s.size() && std::isfinite(value)) {
        return value;
      }
    } catch (...) {
    }
    return std::nullopt;
  };
  if (auto value = try_parse(text)) {
    return value;
  }
  if (!text.empty()) {
    const char suffix = text.back();
    if (suffix == '%' || suffix == 'x' || suffix == 'X') {
      return try_parse(text.substr(0, text.size() - 1));
    }
  }
  return std::nullopt;
}

inline int nearest_item_index(const std::vector<std::string>& items,
                              std::string_view text) noexcept {
  if (items.empty()) {
    return -1;
  }
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (items[i] == text) {
      return static_cast<int>(i);
    }
  }
  const auto target = parse_item_number(text);
  if (!target) {
    return -1;
  }
  int best = -1;
  double best_delta = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < items.size(); ++i) {
    const auto value = parse_item_number(items[i]);
    if (!value) {
      continue;
    }
    const double delta = std::fabs(*value - *target);
    if (delta < best_delta) {
      best_delta = delta;
      best = static_cast<int>(i);
    }
  }
  return best;
}

inline float scroll_to_show_index(const Widget* host, const Rect& field_abs,
                                  std::size_t item_count, int index,
                                  Placement placement = Placement::Down) noexcept {
  if (item_count == 0 || index < 0) {
    return 0.0f;
  }
  const Geometry probe = layout(host, field_abs, item_count, 0.0f, placement);
  if (probe.rect.h < 1.0f || probe.row_h <= 0.0f) {
    return 0.0f;
  }
  return clamp_scroll(probe, static_cast<float>(index) * probe.row_h);
}

inline int index_at_point(const Geometry& g, Vec2 point, std::size_t item_count) noexcept {
  if (!g.rect.contains(point) || item_count == 0 || g.row_h <= 0.0f) {
    return -1;
  }
  const float y_in_content = point.y - g.rect.y + g.scroll;
  const int index = static_cast<int>(std::floor(y_in_content / g.row_h));
  if (index < 0 || index >= static_cast<int>(item_count)) {
    return -1;
  }
  return index;
}

inline void paint_opaque_panel(UiPainter& painter, const Rect& rect) {
  // Fully opaque panel (alpha=1) at high Z so nothing behind shows through.
  painter.fill_rect(rect, theme::kSurface, theme::kCornerRadiusSm, kMenuZ);
  painter.fill_rect_outline(rect, theme::kSurface, theme::kOutline, theme::kCornerRadiusSm,
                            kMenuZ + 0.0001f);
}

inline void paint_items(UiPainter& painter, const Geometry& g,
                        const std::vector<std::string>& items, int selected_index,
                        int hover_index = -1) {
  if (items.empty() || g.rect.h <= 0.0f) {
    return;
  }
  paint_opaque_panel(painter, g.rect);
  const int begin = std::max(0, g.first_index);
  const int end = std::min(static_cast<int>(items.size()), begin + g.visible_count + 1);
  for (int i = begin; i < end; ++i) {
    const float row_y = g.rect.y + static_cast<float>(i) * g.row_h - g.scroll;
    Rect row{g.rect.x, row_y, g.rect.w, g.row_h};
    // Clip highlight to panel.
    const float top = std::max(row.y, g.rect.y);
    const float bottom = std::min(row.bottom(), g.rect.bottom());
    if (bottom - top < 1.0f) {
      continue;
    }
    row.y = top;
    row.h = bottom - top;
    if (i == selected_index) {
      painter.fill_rect(row, theme::kPrimary.lerp(theme::kSurface, 0.65f), 0.0f, kMenuRowZ);
    } else if (i == hover_index) {
      // Semi-transparent hover veil on unselected rows.
      constexpr Color kMenuHover{1.0f, 1.0f, 1.0f, 0.14f};
      painter.fill_rect(row, kMenuHover, 0.0f, kMenuHoverZ);
    }
    // Labels only when the row is fully inside the menu — UiPainter cannot scissor,
    // and post-overlay text would otherwise paint over the host field above.
    if (row_y >= g.rect.y - 0.5f && row_y + g.row_h <= g.rect.bottom() + 0.5f) {
      painter.label({g.rect.x, row_y, g.rect.w, g.row_h}, items[static_cast<std::size_t>(i)],
                    theme::kOnSurface, kMenuTextZ);
    }
  }
  // Top/bottom edge fade indicators when scrollable.
  if (g.scroll > 1.0f) {
    painter.fill_rect({g.rect.x, g.rect.y, g.rect.w, 3.0f}, theme::kOutline, 0.0f, kMenuTextZ);
  }
  if (g.scroll + g.rect.h < g.content_h - 1.0f) {
    painter.fill_rect({g.rect.x, g.rect.bottom() - 3.0f, g.rect.w, 3.0f}, theme::kOutline, 0.0f,
                      kMenuTextZ);
  }
}

}  // namespace wds::interaction::popup_menu
