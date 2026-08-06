#include "wds/ui/regions/status/status_bar.hpp"

#include <wds/interaction/theme.hpp>

namespace wds::ui {
namespace {

namespace th = wds::interaction::theme;

}  // namespace

void StatusBar::set_message(std::string text, StatusLevel level) {
  text_ = std::move(text);
  level_ = level;
}

void StatusBar::paint(wds::interaction::UiPainter& painter) const {
  const auto b = absolute_bounds();
  painter.fill_rect(b, th::kSurface);
  // Top hairline separates the bar from edit/preview.
  painter.fill_rect({b.x, b.y, b.w, 1.0f}, th::kOutline);

  if (text_.empty()) return;

  const float pad = th::kUiPad;
  const auto color = (level_ == StatusLevel::Error)     ? th::kError
                     : (level_ == StatusLevel::Warning) ? th::kWarning
                                                        : th::kOnSurfaceMuted;
  painter.label({b.x + pad, b.y, b.w - pad * 2.0f, b.h}, text_, color, 0.92f, false, 0.0f,
                /*left_align=*/true);
}

}  // namespace wds::ui
