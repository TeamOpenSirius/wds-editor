#pragma once

#include <wds/interaction/widget.hpp>

#include <functional>

namespace wds::interaction {

class Modal : public Widget {
 public:
  using CloseHandler = std::function<void()>;

  void set_content_bounds(Rect bounds) noexcept { content_bounds_ = bounds; }
  const Rect& content_bounds() const noexcept { return content_bounds_; }
  void set_close_on_backdrop(bool close) noexcept { close_on_backdrop_ = close; }
  bool close_on_backdrop() const noexcept { return close_on_backdrop_; }
  void on_close(CloseHandler handler) { on_close_ = std::move(handler); }
  void close();

  void layout(const Rect& parent_bounds) override;
  void paint(UiPainter& painter) const override;
  Widget* hit_test(Vec2 point) override;
  void on_click(const ClickEvent& event) override;

 private:
  Rect content_bounds_{};
  bool close_on_backdrop_ = true;
  CloseHandler on_close_;
};

}  // namespace wds::interaction
