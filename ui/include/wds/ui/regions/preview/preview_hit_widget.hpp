#pragma once

#include <wds/interaction/widget.hpp>

#include <functional>
#include <utility>

namespace wds::ui {

class PreviewHitWidget final : public wds::interaction::Widget {
 public:
  using ScrollHandler = std::function<void(const wds::interaction::ScrollEvent&)>;

  void set_scroll_handler(ScrollHandler handler) { on_scroll_ = std::move(handler); }
  void set_trace_peer(wds::interaction::Widget* peer) noexcept { trace_peer_ = peer; }

  void paint(wds::interaction::UiPainter&) const override {}

  void on_scroll(const wds::interaction::ScrollEvent& event) override {
    if (on_scroll_) {
      on_scroll_(event);
    }
  }

  const char* trace_name() const override { return "PreviewHitWidget"; }
  void trace_snapshot(wds::common::CrashTraceSnap& snap) const override {
    if (trace_peer_ != nullptr) {
      trace_peer_->trace_snapshot(snap);
    }
  }

 private:
  ScrollHandler on_scroll_;
  wds::interaction::Widget* trace_peer_ = nullptr;
};

}  // namespace wds::ui
