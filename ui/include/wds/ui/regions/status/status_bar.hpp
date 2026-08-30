#pragma once

#include <wds/interaction/widget.hpp>

#include <string>

namespace wds::ui {

enum class StatusLevel {
  Info,
  Warning,
  Error,
};

// Full-width bottom strip: shows only the latest status line (no scroll history).
class StatusBar final : public wds::interaction::Widget {
 public:
  void set_message(std::string text, StatusLevel level = StatusLevel::Info);
  const std::string& message() const noexcept { return text_; }
  StatusLevel level() const noexcept { return level_; }

  // Tree paint is a no-op: the bar is drawn in the post-overlay pass so it
  // occludes edit-area note skins (depth write off; skins append after solid UI).
  void paint(wds::interaction::UiPainter& painter) const override;
  void paint_overlay(wds::interaction::UiPainter& painter) const;

 private:
  std::string text_;
  StatusLevel level_ = StatusLevel::Info;
};

}  // namespace wds::ui
