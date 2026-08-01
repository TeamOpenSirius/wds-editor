#include "wds/ui/resource_paths.hpp"
#include "wds/ui/startup_deps.hpp"
#include "wds/ui/editor_session.hpp"
#include "wds/ui/editor_ui_config.hpp"
#include "wds/ui/macos_menu.hpp"
#include "wds/ui/native_file_dialog.hpp"
#include "wds/ui/ui_manager.hpp"
#include "wds/ui/window.hpp"
#include "wds/ui/regions/edit/chart_edit_panel.hpp"
#include "wds/ui/regions/preview/chart_preview_panel.hpp"
#include "wds/ui/regions/toolbar/editor_toolbar.hpp"

#include "wds/common/crash_handler.hpp"
#include "wds/interaction/events.hpp"
#include "wds/interaction/glfw_input_adapter.hpp"
#include "wds/interaction/widget_root.hpp"
#include "wds/renderer/log.hpp"
#include "wds/renderer/preview_visual_config.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <objbase.h>
#include <windows.h>
namespace {
// Ensure Per-Monitor DPI awareness before GLFW init so content scale matches
// the physical framebuffer (avoids OS bitmap upscaling → blurry UI).
void enable_win_dpi_awareness() {
  if (HMODULE user32 = ::GetModuleHandleW(L"user32.dll")) {
    using SetCtxFn = BOOL(WINAPI*)(void*);
    if (auto set_ctx = reinterpret_cast<SetCtxFn>(
            ::GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
      // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == -4
      if (set_ctx(reinterpret_cast<void*>(static_cast<intptr_t>(-4)))) return;
    }
  }
  if (HMODULE shcore = ::LoadLibraryW(L"Shcore.dll")) {
    using SetAwareFn = long(WINAPI*)(int);
    if (auto set_aware =
            reinterpret_cast<SetAwareFn>(::GetProcAddress(shcore, "SetProcessDpiAwareness"))) {
      set_aware(2);  // PROCESS_PER_MONITOR_DPI_AWARE
    }
  }
}

// Hold the COM apartment for the whole process. File dialogs must not CoUninitialize
// while Vulkan/DXGI still own the window.
struct ProcessCom {
  ProcessCom() {
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ok_ = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE || hr == S_FALSE;
  }
  ~ProcessCom() {
    // Intentionally leak the apartment until process exit — matching ComScope policy.
  }
  bool ok_ = false;
};
}  // namespace
#endif

namespace {

void glfw_error_callback(int code, const char* description) {
  std::fprintf(stderr, "[wds] GLFW error %d: %s\n", code,
               description != nullptr ? description : "(null)");
}

#if WDS_ENABLE_LOGGING
// Same location as crash logs — avoid fprintf'ing frame diag to the Debug console
// every second (AllocConsole I/O can invent hitch noise while diagnosing pacing).
std::filesystem::path resolve_frame_diag_log_path() {
  namespace fs = std::filesystem;
#if defined(_WIN32)
  char base[1024] = {};
  const DWORD n = ::GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof(base));
  if (n > 0 && n < sizeof(base)) {
    return fs::path(base) / "WDS" / "logs" / "frame-diag.log";
  }
  const DWORD n2 = ::GetEnvironmentVariableA("USERPROFILE", base, sizeof(base));
  if (n2 > 0 && n2 < sizeof(base)) {
    return fs::path(base) / "AppData" / "Local" / "WDS" / "logs" / "frame-diag.log";
  }
  return fs::path("frame-diag.log");
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    return fs::path(home) / "Library" / "Application Support" / "WDS" / "logs" /
           "frame-diag.log";
  }
  return fs::path("frame-diag.log");
#else
  if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr && xdg[0] != '\0') {
    return fs::path(xdg) / "WDS" / "logs" / "frame-diag.log";
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    return fs::path(home) / ".local" / "share" / "WDS" / "logs" / "frame-diag.log";
  }
  return fs::path("frame-diag.log");
#endif
}

FILE* open_frame_diag_log() {
  const auto path = resolve_frame_diag_log_path();
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  FILE* fp = std::fopen(path.string().c_str(), "a");
  if (fp != nullptr) {
    WDS_LOG("frame diag log: %s\n", path.string().c_str());
  } else {
    WDS_LOG("frame diag log open failed path=%s\n", path.string().c_str());
  }
  return fp;
}
#endif

int run_editor(int argc, char** argv) {
#if defined(_WIN32) && WDS_ENABLE_LOGGING
  // GUI subsystem has no console; without this Debug logs / startup failures are invisible
  // and look like an instant flash-quit when deps are missing.
  if (::GetConsoleWindow() == nullptr) {
    if (::AllocConsole()) {
      FILE* fp = nullptr;
#if defined(_MSC_VER)
      freopen_s(&fp, "CONOUT$", "w", stdout);
      freopen_s(&fp, "CONOUT$", "w", stderr);
      freopen_s(&fp, "CONIN$", "r", stdin);
#else
      fp = std::freopen("CONOUT$", "w", stdout);
      fp = std::freopen("CONOUT$", "w", stderr);
      fp = std::freopen("CONIN$", "r", stdin);
      (void)fp;
#endif
      ::SetConsoleTitleW(L"WDS Editor (Debug log)");
    }
  }
#endif

  WDS_LOG("wds_editor logging=%d\n", WDS_ENABLE_LOGGING);

#if defined(_WIN32)
  ProcessCom process_com;
  (void)process_com;
  enable_win_dpi_awareness();
#endif

  glfwSetErrorCallback(glfw_error_callback);

  // Must run before glfwInit / Vulkan loader init so bundled MoltenVK wins.
  wds::ui::prepare_macos_vulkan_environment(argv[0]);

  {
    wds::ui::StartupDependencyReport deps = wds::ui::check_startup_dependencies(argv[0]);
    if (!deps.ok()) {
      return wds::ui::fail_startup_dependencies(deps);
    }
  }

  if (!glfwInit()) {
    wds::ui::StartupDependencyReport deps;
    deps.missing.push_back("GLFW 初始化失败（显示服务 / 图形环境不可用）");
    return wds::ui::fail_startup_dependencies(deps);
  }
  if (!glfwVulkanSupported()) {
    wds::ui::StartupDependencyReport deps;
    wds::ui::add_vulkan_unavailable(deps);
    glfwTerminate();
    return wds::ui::fail_startup_dependencies(deps);
  }

  wds::ui::UiWindow window;
  if (!window.create(1280, 734, "WDS Editor")) {
    wds::ui::StartupDependencyReport deps;
    deps.missing.push_back("无法创建窗口");
    glfwTerminate();
    return wds::ui::fail_startup_dependencies(deps);
  }
  window.set_app_icon_png(wds::ui::resolve_app_icon_png(argv[0]));
#if defined(_WIN32)
  wds::ui::native_file_dialog::set_owner_window(glfwGetWin32Window(window.handle()));
#endif

  wds::renderer::PreviewVisualConfig visual;
  visual.skins_directory = wds::ui::resolve_skins_dir(argv[0]);
  visual.effects_directory = wds::ui::resolve_effects_dir(argv[0]);
  // No default BGM — user imports music; path is persisted in .wdsproject.
  visual.bgm_path.clear();
  visual.lane_count = 12;
  // Unified 2× MSAA across platforms (diagonal AA without 4× fill-rate cost).
  visual.msaa_samples = 2;
  const std::string ui_font = wds::ui::resolve_ui_font_path(argv[0]);

  wds::ui::UiManager ui;
  if (!ui.chart_preview().initialize_empty(window.handle(), visual, ui_font)) {
    wds::ui::StartupDependencyReport deps;
    deps.missing.push_back(
        "预览初始化失败（音频 / Vulkan / skins）。macOS 请确认 .app 内含 "
        "Contents/MacOS/lib/libMoltenVK.dylib 与 Resources/vulkan/icd.d/"
        "MoltenVK_icd.json；下载安装后可执行：xattr -cr \"/Applications/WDS Editor.app\"");
    window.destroy();
    glfwTerminate();
    return wds::ui::fail_startup_dependencies(deps);
  }
  ui.session().new_project();
  if (argc >= 2 && !ui.session().open_wdsproject(argv[1])) {
    std::fprintf(stderr, "failed to open project: %s\n", argv[1]);
  }
  if (auto* toolbar = ui.toolbar_panel()) {
    toolbar->load_action_icons(ui.chart_preview().preview().vulkan(),
                               wds::ui::resolve_icons_dir(argv[0]));
    toolbar->set_skin(&ui.chart_preview().preview().skin());
  }
  if (auto* edit = ui.edit_panel()) {
    edit->set_skin(&ui.chart_preview().preview().skin());
  }

  ui.set_config_path(wds::ui::resolve_editor_config_path(argv[0]));
  ui.load_ui_config();

  window.set_close_handler([&ui] { return ui.confirm_close(); });
  ui.set_request_close([&window] { window.request_close(); });
  const auto toggle_fullscreen = [&window, &ui] {
    auto& vulkan = ui.chart_preview().preview().vulkan();
    if (window.is_fullscreen()) {
      // Leave: release FSE while still exclusive, then restore windowed mode.
      vulkan.release_fullscreen_exclusive();
      window.set_fullscreen(false);
    } else {
      // Enter: take the monitor / Space first, then ask Vulkan for FSE (Win).
      window.set_fullscreen(true);
      vulkan.set_exclusive_fullscreen_desired(true);
    }
    const auto win = window.window_size();
    const auto fb = window.framebuffer_size();
    ui.resize(static_cast<int>(win.x), static_cast<int>(win.y), static_cast<int>(fb.x),
              static_cast<int>(fb.y));
    WDS_LOG("fullscreen toggle: window_fs=%d desired=%d extension=%d acquired=%d fb=%.0fx%.0f\n",
            window.is_fullscreen() ? 1 : 0, vulkan.exclusive_fullscreen_desired() ? 1 : 0,
            vulkan.exclusive_fullscreen_extension() ? 1 : 0,
            vulkan.exclusive_fullscreen_acquired() ? 1 : 0, fb.x, fb.y);
  };
  ui.set_fullscreen_toggler(toggle_fullscreen);
  // macOS Window menu: Cmd+Shift+F11 (same as chord_toggle_fullscreen).
  wds::ui::install_fullscreen_menu_shortcut(toggle_fullscreen);

  wds::interaction::GlfwInputAdapter input(window.handle());
  input.attach();
  if (auto* edit = ui.edit_panel()) {
    edit->set_cursor_setter(
        [&input](wds::interaction::CursorKind kind) { input.set_cursor(kind); });
  }

  auto last = std::chrono::steady_clock::now();
  WDS_LOG("wds_editor ready — Space=play/pause, Ctrl/Cmd+Shift+F11=fullscreen, "
          "Cmd/Ctrl+S=save\n");

  bool close_teardown_done = false;
  auto prepare_close_teardown = [&] {
    if (close_teardown_done) {
      return;
    }
    if (ui.chart_preview().ready()) {
      ui.chart_preview().preview().vulkan().release_fullscreen_exclusive();
    }
    window.prepare_for_teardown();
    close_teardown_done = true;
  };

  // Diagnostic frame timing (Debug / WDS_ENABLE_LOGGING): average every ~1s.
  // Written to frame-diag.log (not the Debug console) to avoid I/O hitch noise.
  struct FrameDiagAcc {
    int64_t poll_us = 0;
    int64_t resize_us = 0;
    int64_t update_us = 0;
    int64_t update_flush_us = 0;
    int64_t update_bounds_us = 0;
    int64_t update_layout_us = 0;
    int64_t update_sync_us = 0;
    int64_t update_process_us = 0;
    int64_t tick_us = 0;
    int64_t ui_batch_us = 0;
    int64_t render_us = 0;
    int64_t fence_us = 0;
    int64_t fence_max_us = 0;
    int64_t acquire_us = 0;
    int64_t present_us = 0;
    int64_t submit_us = 0;
    int64_t wall_us = 0;
    int64_t wall_max_us = 0;
    int frames = 0;
    int playing_frames = 0;
    int hitch_count = 0;  // raw wall delta > 25ms (above one 60Hz period)
    std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
  } frame_diag;
#if WDS_ENABLE_LOGGING
  FILE* frame_diag_fp = open_frame_diag_log();
  using clock = std::chrono::steady_clock;
  auto elapsed_us = [](clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t0).count();
  };
#endif

  while (!window.should_close()) {
    const auto now = std::chrono::steady_clock::now();
    const auto raw_delta_us =
        std::chrono::duration_cast<std::chrono::microseconds>(now - last).count();
    last = now;
    // Cap wall delta so UI animations and Transport share the same hitch guard.
    constexpr int64_t kMaxWallDeltaUs = 80000;  // 80 ms
    const int64_t delta_us =
        std::clamp<int64_t>(raw_delta_us, 0, kMaxWallDeltaUs);
    const float delta_seconds = static_cast<float>(delta_us) * 1.0e-6f;

#if WDS_ENABLE_LOGGING
    if (raw_delta_us > 25000) {
      ++frame_diag.hitch_count;
    }
    const auto poll_t0 = clock::now();
#endif
    window.poll_events();
#if WDS_ENABLE_LOGGING
    const int64_t poll_us = elapsed_us(poll_t0);
#endif
    if (window.should_close()) {
      // Confirmed close (incl. fullscreen + 不保存): leave FS / FSE before any
      // further present or Vulkan destroy — avoids Win32 TDR / 图形输出错误.
      prepare_close_teardown();
      break;
    }

    const auto win = window.window_size();
    const auto fb = window.framebuffer_size();
    const int logical_w = static_cast<int>(win.x);
    const int logical_h = static_cast<int>(win.y);
    const int fb_w = static_cast<int>(fb.x);
    const int fb_h = static_cast<int>(fb.y);
#if WDS_ENABLE_LOGGING
    const auto resize_t0 = clock::now();
#endif
    ui.resize(logical_w, logical_h, fb_w, fb_h);
#if WDS_ENABLE_LOGGING
    const int64_t resize_us = elapsed_us(resize_t0);
#endif

    auto events = input.queue().events();
    input.queue().clear();
    if (auto* edit = ui.edit_panel()) {
      edit->sync_global_pointer(input.pointer_logical());
    }
#if WDS_ENABLE_LOGGING
    const auto update_t0 = clock::now();
#endif
    ui.update(delta_seconds, events);
#if WDS_ENABLE_LOGGING
    const int64_t update_us = elapsed_us(update_t0);
#endif

    // Discard/Save on the unsaved dialog may set should-close mid-update.
    // Do not present another Vulkan frame onto a closing Win32 surface.
    if (window.should_close()) {
      prepare_close_teardown();
      break;
    }

#if WDS_ENABLE_LOGGING
    const auto tick_t0 = clock::now();
#endif
    ui.chart_preview().tick(delta_us);
#if WDS_ENABLE_LOGGING
    const int64_t tick_us = elapsed_us(tick_t0);
#endif

    if (fb_w > 0 && fb_h > 0) {
      if (auto* edit = ui.edit_panel()) {
        edit->sync_global_pointer(input.pointer_logical());
      }
      const auto& preview = ui.chart_preview().preview();
      const auto solid = ui.chart_preview().solid_texture();
      const auto batch_t0 = std::chrono::steady_clock::now();
      const auto& ui_batch = ui.build_ui_batch(solid, fb_w, fb_h, preview.geometry().screen());
      // Dropdown / modal must be post-overlay: main UI batch draws note-skin sprites after
      // rect fills, so in-batch menus would stay under convert-note artwork.
      const auto& post_batch =
          ui.build_post_overlay_batch(solid, fb_w, fb_h, preview.geometry().screen());
      const auto& chrome_batch =
          ui.build_modal_chrome_batch(solid, fb_w, fb_h, preview.geometry().screen());
      const wds::renderer::DrawBatch* post =
          post_batch.vertex_count() > 0 ? &post_batch : nullptr;
      const wds::renderer::DrawBatch* chrome =
          chrome_batch.vertex_count() > 0 ? &chrome_batch : nullptr;
      const auto batch_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - batch_t0)
                                .count();

      const auto render_t0 = std::chrono::steady_clock::now();
      ui.chart_preview().render(&ui_batch, post, chrome);
      const auto render_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - render_t0)
                                 .count();

#if WDS_ENABLE_LOGGING
      auto& vulkan = ui.chart_preview().preview().vulkan();
      const int64_t wall = std::max<int64_t>(0, raw_delta_us);
      frame_diag.wall_us += wall;
      frame_diag.wall_max_us = std::max(frame_diag.wall_max_us, wall);
      frame_diag.poll_us += poll_us;
      frame_diag.resize_us += resize_us;
      frame_diag.update_us += update_us;
      frame_diag.update_flush_us += ui.last_update_flush_us();
      frame_diag.update_bounds_us += ui.last_update_bounds_us();
      frame_diag.update_layout_us += ui.last_update_layout_us();
      frame_diag.update_sync_us += ui.last_update_sync_us();
      frame_diag.update_process_us += ui.last_update_process_us();
      frame_diag.tick_us += tick_us;
      frame_diag.ui_batch_us += batch_us;
      frame_diag.render_us += render_us;
      frame_diag.fence_us += vulkan.last_fence_wait_us();
      frame_diag.fence_max_us =
          std::max(frame_diag.fence_max_us, vulkan.last_fence_wait_us());
      frame_diag.acquire_us += vulkan.last_acquire_wait_us();
      frame_diag.present_us += vulkan.last_present_us();
      frame_diag.submit_us += vulkan.last_gpu_submit_us();
      ++frame_diag.frames;
      if (ui.chart_preview().transport().playing()) {
        ++frame_diag.playing_frames;
      }
      const auto diag_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - frame_diag.window_start)
                                    .count();
      if (diag_elapsed >= 1000 && frame_diag.frames > 0) {
        const double n = static_cast<double>(frame_diag.frames);
        const double elapsed_s = std::max(0.001, static_cast<double>(diag_elapsed) / 1000.0);
        const double fps = n / elapsed_s;
        const double accounted_ms =
            (frame_diag.poll_us + frame_diag.resize_us + frame_diag.update_us +
             frame_diag.tick_us + frame_diag.ui_batch_us + frame_diag.render_us) /
            n / 1000.0;
        char line[896];
        const int len = std::snprintf(
            line, sizeof(line),
            "frame diag: n=%d fps=%.1f playing=%d/%d fs=%d fse=%d "
            "wall_avg=%.2fms wall_max=%.2fms hitch=%d accounted=%.2fms "
            "poll=%.2fms resize=%.2fms update=%.2fms "
            "[flush=%.2f bounds=%.2f layout=%.2f sync=%.2f process=%.2f] "
            "tick=%.2fms ui_batch=%.2fms render=%.2fms fence=%.2fms fence_max=%.2fms "
            "acquire=%.2fms submit=%.2fms present=%.2fms fb=%dx%d\n",
            frame_diag.frames, fps, frame_diag.playing_frames, frame_diag.frames,
            window.is_fullscreen() ? 1 : 0, vulkan.exclusive_fullscreen_acquired() ? 1 : 0,
            frame_diag.wall_us / n / 1000.0, frame_diag.wall_max_us / 1000.0,
            frame_diag.hitch_count, accounted_ms, frame_diag.poll_us / n / 1000.0,
            frame_diag.resize_us / n / 1000.0, frame_diag.update_us / n / 1000.0,
            frame_diag.update_flush_us / n / 1000.0, frame_diag.update_bounds_us / n / 1000.0,
            frame_diag.update_layout_us / n / 1000.0, frame_diag.update_sync_us / n / 1000.0,
            frame_diag.update_process_us / n / 1000.0, frame_diag.tick_us / n / 1000.0,
            frame_diag.ui_batch_us / n / 1000.0, frame_diag.render_us / n / 1000.0,
            frame_diag.fence_us / n / 1000.0, frame_diag.fence_max_us / 1000.0,
            frame_diag.acquire_us / n / 1000.0, frame_diag.submit_us / n / 1000.0,
            frame_diag.present_us / n / 1000.0, fb_w, fb_h);
        if (len > 0 && frame_diag_fp != nullptr) {
          std::fwrite(line, 1, static_cast<std::size_t>(len), frame_diag_fp);
          std::fflush(frame_diag_fp);
        }
        frame_diag = {};
        frame_diag.window_start = std::chrono::steady_clock::now();
      }
#else
      (void)batch_us;
      (void)render_us;
#endif
    }
  }

#if WDS_ENABLE_LOGGING
  if (frame_diag_fp != nullptr) {
    std::fclose(frame_diag_fp);
    frame_diag_fp = nullptr;
  }
#endif

  if (auto* edit = ui.edit_panel()) {
    edit->set_cursor_setter({});
  }
  // Safe Vulkan teardown order for Win32 (windowed and fullscreen):
  // 1) release FSE + leave fullscreen / hide HWND (idempotent)
  // 2) GPU idle
  // 3) free app-owned textures while the device is still alive
  // 4) destroy device/surface
  // 5) destroy the GLFW window
  prepare_close_teardown();
  if (ui.chart_preview().ready()) {
    ui.chart_preview().preview().vulkan().device_wait_idle();
  }
  if (auto* toolbar = ui.toolbar_panel()) {
    toolbar->release_gpu_resources();
  }
  ui.chart_preview().shutdown();
  input.detach();
#if defined(_WIN32)
  wds::ui::native_file_dialog::set_owner_window(nullptr);
#endif
  window.destroy();
  glfwTerminate();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  wds::common::install_crash_handlers();
  try {
    return run_editor(argc, argv);
  } catch (const std::exception& ex) {
    wds::common::report_fatal("未捕获的 C++ 异常", ex.what());
    return 1;
  } catch (...) {
    wds::common::report_fatal("未捕获的 C++ 异常", "unknown non-std exception");
    return 1;
  }
}
