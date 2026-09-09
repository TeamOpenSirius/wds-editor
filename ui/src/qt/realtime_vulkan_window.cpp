#include "wds/ui/qt/realtime_vulkan_window.hpp"
#include "wds/ui/qt/qt_input_adapter.hpp"

#include <QEvent>
#include <QResizeEvent>
#include <algorithm>
#include <chrono>

namespace wds::ui {
RealtimeVulkanWindow::RealtimeVulkanWindow(QVulkanInstance* instance, QWindow* parent)
    : QWindow(parent), instance_(instance), last_frame_(std::chrono::steady_clock::now()) {
  setSurfaceType(QSurface::VulkanSurface);
  setVulkanInstance(instance);
  input_adapter_ = std::make_unique<QtInputAdapter>(this, input_queue_);
  resize_settle_timer_.setSingleShot(true);
  resize_settle_timer_.setInterval(120);
  QObject::connect(&resize_settle_timer_, &QTimer::timeout, this, [this] {
    resizing_ = false;
    last_frame_ = std::chrono::steady_clock::now();
    pending_elapsed_us_ = 0;
    schedule_frame();
  });
}

RealtimeVulkanWindow::~RealtimeVulkanWindow() = default;

void RealtimeVulkanWindow::set_resize_suspended(bool suspended) noexcept {
  resizing_ = suspended;
  if (suspended) {
    resize_settle_timer_.stop();
    pending_elapsed_us_ = 0;
    return;
  }
  last_frame_ = std::chrono::steady_clock::now();
  pending_elapsed_us_ = 0;
  schedule_frame();
}

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
    if (resizing_) return true;
    initialized_ = isExposed();
    if (initialized_) {
      const auto now = std::chrono::steady_clock::now();
      const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - last_frame_).count();
      last_frame_ = now;
      pending_elapsed_us_ = std::min<int64_t>(pending_elapsed_us_ + elapsed, 80000);
      const bool has_input = !input_queue_.events().empty();
      if (idle_frame_interval_us_ > 0 && !has_input && pending_elapsed_us_ < idle_frame_interval_us_) {
        schedule_frame();
        return true;
      }
      int fb_w = 1, fb_h = 1;
      const auto host = host_surface();
      host.framebuffer_size(&fb_w, &fb_h);
      if (frame_callback_) {
        frame_callback_(std::clamp(static_cast<float>(pending_elapsed_us_) / 1000000.0f, 0.0f, 0.08f),
                                  width(), height(), fb_w, fb_h, input_queue_.events());
      }
      pending_elapsed_us_ = 0;
      input_queue_.clear();
      schedule_frame();
    }
    return true;
  }
  return QWindow::event(event);
}

void RealtimeVulkanWindow::exposeEvent(QExposeEvent*) {
  if (isExposed()) {
    resizing_ = false;
    last_frame_ = std::chrono::steady_clock::now();
    schedule_frame();
  }
}

void RealtimeVulkanWindow::resizeEvent(QResizeEvent*) {
  resizing_ = true;
  resize_settle_timer_.start();
}

void RealtimeVulkanWindow::schedule_frame() {
  if (!resizing_ && isExposed()) requestUpdate();
}
}
