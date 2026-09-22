#include "wds/ui/qt/realtime_vulkan_window.hpp"
#include "wds/ui/qt/qt_input_adapter.hpp"

#include <wds/common/log.hpp>

#include <QEvent>
#include <QGuiApplication>
#include <QResizeEvent>
#include <algorithm>
#include <chrono>

namespace wds::ui {

void RealtimeVulkanWindow::inject_key_tap(wds::interaction::KeyCode key,
                                          wds::interaction::Modifiers mods) {
  wds::interaction::KeyDownEvent down;
  down.key = key;
  down.mods = mods;
  input_queue_.push(down);
  wds::interaction::KeyUpEvent up;
  up.key = key;
  up.mods = mods;
  input_queue_.push(up);
  schedule_frame();
}

RealtimeVulkanWindow::RealtimeVulkanWindow(QVulkanInstance* instance, QWindow* parent)
    : QWindow(parent), instance_(instance), last_frame_(std::chrono::steady_clock::now()) {
  setSurfaceType(QSurface::VulkanSurface);
  setVulkanInstance(instance);
  input_adapter_ = std::make_unique<QtInputAdapter>(this, input_queue_);
  resize_settle_timer_.setSingleShot(true);
  resize_settle_timer_.setInterval(120);
  QObject::connect(&resize_settle_timer_, &QTimer::timeout, this, [this] {
    // A splitter/floating dock drag can pause between moves while the button
    // is still held. Do not recreate its swapchain midway through that gesture.
    if (QGuiApplication::mouseButtons().testFlag(Qt::LeftButton)) {
      resize_settle_timer_.start();
      return;
    }
    resizing_ = false;
    last_frame_ = std::chrono::steady_clock::now();
    pending_elapsed_us_ = 0;
    schedule_frame();
  });
}

RealtimeVulkanWindow::~RealtimeVulkanWindow() = default;

void RealtimeVulkanWindow::set_resize_suspended(bool suspended) noexcept {
  host_resize_suspended_ = suspended;
  if (suspended) {
    pending_elapsed_us_ = 0;
    return;
  }
  last_frame_ = std::chrono::steady_clock::now();
  pending_elapsed_us_ = 0;
  schedule_frame();
}

void RealtimeVulkanWindow::request_frame() {
  force_frame_ = true;
  if (frame_callback_) requestUpdate();
}

void RealtimeVulkanWindow::mark_presented() noexcept { presented_ = true; }

wds::interaction::Vec2 RealtimeVulkanWindow::pointer_logical() const noexcept {
  return input_adapter_ ? input_adapter_->pointer_logical() : wds::interaction::Vec2{};
}

wds::renderer::VulkanHostSurface RealtimeVulkanWindow::host_surface() const {
  wds::renderer::VulkanHostSurface host;
  host.external_instance = instance_ != nullptr ? instance_->vkInstance() : VK_NULL_HANDLE;
  host.external_surface = QVulkanInstance::surfaceForWindow(const_cast<RealtimeVulkanWindow*>(this));
  host.renderer_owns_instance = false;
  host.renderer_owns_surface = false;
  auto* self = const_cast<RealtimeVulkanWindow*>(this);
  host.acquire_surface = [self] {
    return QVulkanInstance::surfaceForWindow(self);
  };
  host.display_refresh_hz = [self] {
    return self->screen() != nullptr ? std::max(1, qRound(self->screen()->refreshRate())) : 60;
  };
  host.framebuffer_size = [self](int* width, int* height) {
    const qreal scale = self->devicePixelRatio();
    *width = std::max(1, qRound(self->width() * scale));
    *height = std::max(1, qRound(self->height() * scale));
  };
  return host;
}

bool RealtimeVulkanWindow::event(QEvent* event) {
  if (event->type() == QEvent::UpdateRequest) {
    if (!frame_callback_) {
      return true;
    }
    // A request already queued before the resize must not reach the renderer,
    // even if input is pending. Resume with one frame at the final dimensions.
    // The first-open frame is the exception: drop+no-reschedule leaves the
    // preview blank until dock layout stops restarting the settle timers.
    const bool startup = !presented_;
    if ((resizing_ || host_resize_suspended_) && !startup && !force_frame_) {
      note_frame_hold("resize-suspend");
      pending_elapsed_us_ = 0;
      return true;
    }
    force_frame_ = false;
    initialized_ = isExposed();
    if (!initialized_) {
      note_frame_hold("unexposed");
      return true;
    }
    if (!resizing_ && !host_resize_suspended_) {
      note_frame_resume();
    }
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - last_frame_).count();
    last_frame_ = now;
    pending_elapsed_us_ = std::min<int64_t>(pending_elapsed_us_ + elapsed, 80000);
    const bool has_input = !input_queue_.events().empty();
    const bool bypass_idle_cap =
        static_cast<bool>(idle_throttle_bypass_) && idle_throttle_bypass_();
    // 2 ms slack: at exactly one vsync per interval the accumulator lands a
    // hair under the cap and every other frame gets skipped (60→30 fps).
    if (idle_frame_interval_us_ > 0 && !has_input && !bypass_idle_cap && !startup &&
        pending_elapsed_us_ + 2000 < idle_frame_interval_us_) {
      schedule_frame();
      return true;
    }
    int fb_w = 1, fb_h = 1;
    const auto host = host_surface();
    host.framebuffer_size(&fb_w, &fb_h);
    if (startup) {
      WDS_LOG("preview first frame resize=%d suspend=%d exposed=%d logical=%dx%d fb=%dx%d\n",
              resizing_ ? 1 : 0, host_resize_suspended_ ? 1 : 0, isExposed() ? 1 : 0, width(),
              height(), fb_w, fb_h);
    }
    if (frame_callback_) {
      frame_callback_(std::clamp(static_cast<float>(pending_elapsed_us_) / 1000000.0f, 0.0f, 0.08f),
                                width(), height(), fb_w, fb_h, input_queue_.events());
    }
    pending_elapsed_us_ = 0;
    input_queue_.clear();
    schedule_frame();
    return true;
  }
  return QWindow::event(event);
}

void RealtimeVulkanWindow::exposeEvent(QExposeEvent*) {
  // Qt 6.6+ on macOS uses QMetalLayer; [QNSView displayLayer:] calls
  // handleExposeEvent on every presented frame. Resetting last_frame_ here
  // would truncate the UpdateRequest delta (wall ~16.7 ms → measured ~8 ms).
  if (isExposed() && !was_exposed_) {
    last_frame_ = std::chrono::steady_clock::now();
    pending_elapsed_us_ = 0;
    schedule_frame();
  }
  was_exposed_ = isExposed();
}

void RealtimeVulkanWindow::resizeEvent(QResizeEvent*) {
  resizing_ = true;
  resize_settle_timer_.start();
}

void RealtimeVulkanWindow::note_frame_hold(const char* why) {
#if WDS_ENABLE_LOGGING
  if (frame_hold_logged_) {
    return;
  }
  frame_hold_logged_ = true;
  WDS_LOG("preview frames held why=%s exposed=%d resize=%d suspend=%d presented=%d logical=%dx%d\n",
          why != nullptr ? why : "?", isExposed() ? 1 : 0, resizing_ ? 1 : 0,
          host_resize_suspended_ ? 1 : 0, presented_ ? 1 : 0, width(), height());
#else
  (void)why;
#endif
}

void RealtimeVulkanWindow::note_frame_resume() {
#if WDS_ENABLE_LOGGING
  if (!frame_hold_logged_) {
    return;
  }
  frame_hold_logged_ = false;
  WDS_LOG("preview frames resumed exposed=%d resize=%d suspend=%d logical=%dx%d\n",
          isExposed() ? 1 : 0, resizing_ ? 1 : 0, host_resize_suspended_ ? 1 : 0, width(),
          height());
#endif
}

void RealtimeVulkanWindow::schedule_frame() {
  if (!frame_callback_) return;
  if (!isExposed()) {
    note_frame_hold("unexposed");
    return;
  }
  if (presented_ && (resizing_ || host_resize_suspended_)) {
    note_frame_hold("resize-suspend");
    return;
  }
  requestUpdate();
}
}
