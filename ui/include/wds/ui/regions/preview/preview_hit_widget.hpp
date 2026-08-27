#pragma once

#include <wds/interaction/widget.hpp>

#include <functional>
#include <utility>

namespace wds::ui {

class PreviewHitWidget final : public wds::interaction::Widget {
 public:
  using ScrollHandler = std::function<void(const wds::interaction::ScrollEvent&)>;

  void set_scroll_handler(ScrollHandler handler) { on_scroll_ = std::move(handler); }

  void paint(wds::interaction::UiPainter&) const override {}

  void on_scroll(const wds::interaction::ScrollEvent& event) override {
    if (on_scroll_) {
      on_scroll_(event);
    }
  }

 private:
  ScrollHandler on_scroll_;
};

}  // namespace wds::ui
