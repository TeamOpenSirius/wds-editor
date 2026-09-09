#include "wds/ui/qt/editor_main_window.hpp"
#include "wds/ui/qt/realtime_vulkan_window.hpp"
#include "wds/ui/ui_manager.hpp"
#include "wds/ui/resource_paths.hpp"
#include "wds/ui/regions/preview/chart_preview_panel.hpp"
#include "wds/ui/regions/edit/chart_edit_panel.hpp"
#include "wds/renderer/preview_visual_config.hpp"
#include "wds/common/crash_handler.hpp"

#include <QApplication>
#include <QIcon>
#include <QStyleFactory>
#include <QTimer>
#include <QMessageBox>
#include <QFontDatabase>
#include <QFont>

int main(int argc, char** argv) {
  wds::common::install_crash_handlers();
  QApplication app(argc, argv);
  QApplication::setStyle(QStyleFactory::create("Fusion"));
  const auto bundled_font = wds::ui::resolve_ui_font_path(argv[0]);
  const int font_id = QFontDatabase::addApplicationFont(QString::fromStdString(bundled_font));
  if (font_id >= 0) {
    const auto families = QFontDatabase::applicationFontFamilies(font_id);
    if (!families.isEmpty()) QApplication::setFont(QFont(families.front()));
  }

  QVulkanInstance vk_instance;
  if (!vk_instance.create()) return 2;
  wds::ui::EditorMainWindow window;
  auto* preview_window = new wds::ui::RealtimeVulkanWindow(&vk_instance);
  auto* editor_window = new wds::ui::RealtimeVulkanWindow(&vk_instance);
  editor_window->set_idle_frame_rate(60);
  window.set_viewport_windows(preview_window, editor_window);

  wds::ui::UiManager editor;
  editor.enable_qt_chrome();
  editor.set_config_path(wds::ui::resolve_editor_config_path(argv[0]));
  editor.load_ui_config();
  window.set_skins_dir(wds::ui::resolve_skins_dir(argv[0]));
  window.bind_ui_manager(&editor);
  editor.set_request_close([&window] { window.close(); });
  wds::renderer::PreviewVisualConfig visual;
  visual.skins_directory = wds::ui::resolve_skins_dir(argv[0]);
  visual.effects_directory = wds::ui::resolve_effects_dir(argv[0]);
  visual.bgm_path.clear();
  visual.lane_count = 12;
  visual.msaa_samples = 2;
  const std::string ui_font = wds::ui::resolve_ui_font_path(argv[0]);

  wds::ui::ChartPreviewPanel editor_view;
  wds::renderer::DrawBatch editor_batch;
  auto editor_visual = visual;
  editor_visual.stage_opacity = 0.0f;

  preview_window->set_frame_callback([&editor, preview_window, visual, ui_font](float delta, int logical_w,
                                                                     int logical_h, int fb_w,
                                                                     int fb_h, const std::vector<wds::interaction::InputEvent>& events) mutable {
    if (!editor.chart_preview().ready()) {
      auto host = preview_window->host_surface();
      if (host.external_instance == VK_NULL_HANDLE || host.external_surface == VK_NULL_HANDLE ||
          !editor.chart_preview().initialize_empty(host, visual, ui_font)) {
        return;
      }
      editor.set_status("ui资源加载完毕", wds::ui::StatusLevel::Info);
    }
    editor.resize_preview_viewport(logical_w, logical_h, fb_w, fb_h);
    editor.chart_preview().tick(static_cast<int64_t>(delta * 1000000.0f));
    editor.chart_preview().render();
    editor.chart_preview().flush_retired_font_textures();
  });

  editor_window->set_frame_callback([&editor, editor_window, &editor_view, &editor_batch, editor_visual, ui_font](
                                         float delta, int logical_w, int logical_h, int fb_w,
                                         int fb_h, const std::vector<wds::interaction::InputEvent>& events) mutable {
    if (!editor_view.ready()) {
      auto host = editor_window->host_surface();
      if (host.external_instance == VK_NULL_HANDLE || host.external_surface == VK_NULL_HANDLE ||
          !editor_view.initialize_empty(host, editor_visual, ui_font, false)) {
        return;
      }
    }
    editor.resize_editor_viewport(logical_w, logical_h, fb_w, fb_h);
    if (auto* edit = editor.edit_panel()) edit->sync_global_pointer(editor_window->pointer_logical());
    editor.update(delta, events);
    editor_view.preview().resize(fb_w, fb_h);
    editor_view.sync_ui_font_texture();
    const auto& screen = editor_view.preview().geometry().screen();
    editor.build_editor_batch(editor_batch, editor_view.solid_texture(), fb_w, fb_h, screen,
                              editor_view.preview().skin());
    editor_view.preview().render_editor_only(editor_batch);
    editor_view.flush_retired_font_textures();
  });

  window.setWindowIcon(QIcon(QStringLiteral(WDS_REPO_ROOT "/ui/assets/app_icon/wds.png")));
  window.show();
  if (app.arguments().contains("--smoke-test"))
    QTimer::singleShot(1200, &app, &QApplication::quit);
  return app.exec();
}
