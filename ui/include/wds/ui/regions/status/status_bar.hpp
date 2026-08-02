#pragma once

#include <wds/interaction/widget.hpp>

#include <string>

namespace wds::ui {

enum class StatusLevel {
  Info,
  Error,
};

// Full-width bottom strip: shows only the latest status line (no scroll history).
class StatusBar final : public wds::interaction::Widget {
 public:
  void set_message(std::string text, StatusLevel level = StatusLevel::Info);
  const std::string& message() const noexcept { return text_; }
  StatusLevel level() const noexcept { return level_; }

  void paint(wds::interaction::UiPainter& painter) const override;

 private:
  std::string text_;
  StatusLevel level_ = StatusLevel::Info;
};

}  // namespace wds::ui
