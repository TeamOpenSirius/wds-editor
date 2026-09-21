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
  // Empty clears the callback; event(UpdateRequest) and schedule_frame no-op.
  void set_frame_callback(FrameCallback callback) { frame_callback_ = std::move(callback); }
  // Limit idle rendering for secondary viewports; pending input still renders
  // immediately so editing remains responsive.
  void set_idle_frame_rate(int fps) noexcept { idle_frame_interval_us_ = fps > 0 ? 1000000 / fps : 0; }
  // While this returns true the idle frame cap is bypassed (e.g. transport playing),
  // so scrolling stays at full display rate instead of the every-other-vsync cadence.
  void set_idle_throttle_bypass(std::function<bool()> playing) {
    idle_throttle_bypass_ = std::move(playing);
  }
  // Host-level resize suspension covers the short interval where QMainWindow
  // is relayouting docks and the native child window has not settled yet.
  // Pending frames are dropped until both host and child resizing have settled.
  void set_resize_suspended(bool suspended) noexcept;
  // Queue a synthetic key tap into the realtime input path (global shortcuts
  // forwarded from Qt widgets, e.g. Space anywhere in the app).
  void inject_key_tap(wds::interaction::KeyCode key, wds::interaction::Modifiers mods);
  wds::renderer::VulkanHostSurface host_surface() const;
  wds::interaction::Vec2 pointer_logical() const noexcept;
  // Queue a frame even while resize-settle is holding idle presents. Used for
  // the first-open init path so the preview is not stuck waiting on dock layout.
  void request_frame();
  void mark_presented() noexcept;

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
  std::function<bool()> idle_throttle_bypass_;
  std::chrono::steady_clock::time_point last_frame_{};
  int64_t idle_frame_interval_us_ = 0;
  int64_t pending_elapsed_us_ = 0;
  QTimer resize_settle_timer_;
  bool resizing_ = false;
  bool host_resize_suspended_ = false;
  bool was_exposed_ = false;
  bool presented_ = false;
  bool force_frame_ = false;
};
}
