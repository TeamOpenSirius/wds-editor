#pragma once

#include <QWindow>
#include <QVulkanInstance>
#include <QTimer>
#include <functional>
#include <memory>
#include <chrono>
#include <vector>

#include "wds/interaction/events.hpp"
#include "wds/renderer/vulkan_renderer.hpp"

namespace wds::ui {
class QtInputAdapter;

class RealtimeVulkanWindow final : public QWindow {
 public:
  using FrameCallback = std::function<void(float, int, int, int, int,
                                           const std::vector<wds::interaction::InputEvent>&)>;
  explicit RealtimeVulkanWindow(QVulkanInstance* instance, QWindow* parent = nullptr);
  ~RealtimeVulkanWindow() override;
  bool event(QEvent* event) override;
  void set_frame_callback(FrameCallback callback) { frame_callback_ = std::move(callback); }
  // Limit idle rendering for secondary viewports; pending input still renders
  // immediately so editing remains responsive.
  void set_idle_frame_rate(int fps) noexcept { idle_frame_interval_us_ = fps > 0 ? 1000000 / fps : 0; }
  // Host-level resize suspension covers the short interval where QMainWindow
  // is relayouting docks and the native child window has not settled yet.
  void set_resize_suspended(bool suspended) noexcept;
  wds::renderer::VulkanHostSurface host_surface() const;
  wds::interaction::Vec2 pointer_logical() const noexcept;

 protected:
  void exposeEvent(QExposeEvent*) override;
  void resizeEvent(QResizeEvent*) override;

 private:
  void schedule_frame();
  bool initialized_ = false;
  QVulkanInstance* instance_ = nullptr;
  wds::interaction::InputQueue input_queue_;
  std::unique_ptr<QtInputAdapter> input_adapter_;
  FrameCallback frame_callback_;
  std::chrono::steady_clock::time_point last_frame_{};
  int64_t idle_frame_interval_us_ = 0;
  int64_t pending_elapsed_us_ = 0;
  QTimer resize_settle_timer_;
  bool resizing_ = false;
};
}
