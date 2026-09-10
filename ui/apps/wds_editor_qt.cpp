#include "wds/ui/qt/editor_main_window.hpp"
#include "wds/ui/qt/realtime_vulkan_window.hpp"
#include "wds/ui/qt/chart_edit_widget.hpp"
#include "wds/ui/qt/wds_theme.hpp"
#include "wds/ui/qt/fluent_icons.hpp"
#include "wds/ui/ui_manager.hpp"
#include "wds/ui/editor_session.hpp"
#include "wds/ui/resource_paths.hpp"
#include "wds/ui/regions/preview/chart_preview_panel.hpp"
#include "wds/ui/regions/edit/chart_edit_panel.hpp"
#include "wds/renderer/preview_visual_config.hpp"
#include "wds/common/crash_handler.hpp"

#include <QApplication>
#include <QIcon>
#include <QSettings>
#include <QStyleFactory>
#include <QTimer>
#include <QMessageBox>
#include <QFontDatabase>
#include <QFont>

int main(int argc, char** argv) {
  wds::common::install_crash_handlers();
  QApplication app(argc, argv);
  QCoreApplication::setApplicationVersion(QStringLiteral(WDS_APP_VERSION));
  QApplication::setWindowIcon(QIcon(QStringLiteral(":/wds/app_icon.png")));
  // Fonts must register before the theme QSS (which names 'Noto Sans SC').
  const auto bundled_font = wds::ui::resolve_ui_font_path(argv[0]);
  const int font_id = QFontDatabase::addApplicationFont(QString::fromStdString(bundled_font));
  if (font_id >= 0) {
    const auto families = QFontDatabase::applicationFontFamilies(font_id);
    if (!families.isEmpty()) {
      QFont font(families.front());
      font.setPointSize(10);
      font.setHintingPreference(QFont::PreferFullHinting);
      QApplication::setFont(font);
    }
  }
  wds::ui::load_fluent_font(QString::fromStdString(wds::ui::resolve_fluent_font_path(argv[0])));
  const QString theme_dir = QString::fromStdString(wds::ui::resolve_theme_dir(argv[0]));
  const QString theme_id =
      QSettings("WDS", "WDS Editor").value("appearance/theme").toString();
  wds::ui::apply_wds_theme(app, theme_dir, theme_id);

  QVulkanInstance vk_instance;
  if (!vk_instance.create()) return 2;
  wds::ui::EditorMainWindow window;
  auto* preview_window = new wds::ui::RealtimeVulkanWindow(&vk_instance);
  window.set_viewport_windows(preview_window, nullptr);

  wds::ui::UiManager editor;
  editor.enable_qt_chrome();
  editor.set_config_path(wds::ui::resolve_editor_config_path(argv[0]));
  editor.load_ui_config();
  preview_window->set_idle_frame_rate(30);
  preview_window->set_idle_throttle_bypass([&editor] {
    return editor.chart_preview().ready() && editor.chart_preview().transport().playing();
  });
  window.set_skins_dir(wds::ui::resolve_skins_dir(argv[0]));
  window.set_theme_dir(theme_dir);
  window.bind_ui_manager(&editor);
  editor.set_request_close([&window] { window.close(); });
  auto* editor_widget = new wds::ui::ChartEditWidget(editor.edit_panel(), &window);
  editor_widget->set_skins_directory(QString::fromStdString(wds::ui::resolve_skins_dir(argv[0])));
  editor_widget->set_global_key_handler([&editor](const wds::interaction::KeyDownEvent& event) {
    return editor.dispatch_shortcut(event);
  });
  window.set_editor_widget(editor_widget);
  wds::renderer::PreviewVisualConfig visual;
  visual.skins_directory = wds::ui::resolve_skins_dir(argv[0]);
  visual.effects_directory = wds::ui::resolve_effects_dir(argv[0]);
  visual.bgm_path.clear();
  visual.lane_count = 12;
  visual.msaa_samples = 2;
  const std::string ui_font = wds::ui::resolve_ui_font_path(argv[0]);

  // load_ui_config already pushed the persisted display prefs into the preview
  // panel's config; fold them into the boot visual so initialization keeps them.
  const auto overlay_loaded_display = [&editor](wds::renderer::PreviewVisualConfig v) {
    const auto& loaded = editor.chart_preview().preview().config();
    v.note_speed = loaded.note_speed;
    v.note_start_offset = loaded.note_start_offset;
    v.note_height_level = loaded.note_height_level;
    v.split_line_opacity = loaded.split_line_opacity;
    v.lane_count = loaded.lane_count;
    return v;
  };


  preview_window->set_frame_callback([&editor, &window, preview_window, visual, ui_font,
                                      overlay_loaded_display](float delta, int logical_w,
                                                                     int logical_h, int fb_w,
                                                                     int fb_h, const std::vector<wds::interaction::InputEvent>& events) mutable {
      if (!editor.chart_preview().ready()) {
      auto host = preview_window->host_surface();
      if (host.external_instance == VK_NULL_HANDLE || host.external_surface == VK_NULL_HANDLE ||
          !editor.chart_preview().initialize_empty(host, overlay_loaded_display(visual), ui_font)) {
        return;
      }
      if (editor.session().chart_count() == 0) {
        // No project was chosen on the splash (or smoke/direct launch): seed
        // the normal blank document only after the preview backend is live.
        (void)editor.session().new_project();
      }
      // Startup project selection can happen before the Vulkan surface exists;
      // attach its BGM once the live transport has been initialized.
      if (!editor.session().music_path().empty()) {
        // Do not decode the project audio inside the first exposed frame. Let
        // Qt paint the shell and schedule the decode on the next event turn.
        const std::string music = editor.session().music_path();
        QTimer::singleShot(0, &window, [&editor, music] {
          (void)editor.chart_preview().load_music(music, false);
        });
      }
      editor.set_status("ui资源加载完毕", wds::ui::StatusLevel::Info);
    }
    editor.resize_preview_viewport(logical_w, logical_h, fb_w, fb_h);
    editor.chart_preview().tick(static_cast<int64_t>(delta * 1000000.0f));
    editor.chart_preview().render();
    editor.chart_preview().flush_retired_font_textures();
  });

  auto* edit_timer = new QTimer(&window);
  QObject::connect(edit_timer, &QTimer::timeout, &window, [&editor, editor_widget] {
    editor.resize_editor_viewport(editor_widget->width(), editor_widget->height(),
                                  editor_widget->width(), editor_widget->height());
    editor.update(1.0f / 60.0f, {});
    editor_widget->update();
  });
  edit_timer->start(16);

  window.setWindowIcon(QApplication::windowIcon());
  if (!app.arguments().contains("--smoke-test")) {
    if (!window.show_startup_splash()) return 0;
  }
  window.show();
  window.raise();
  window.activateWindow();
  if (app.arguments().contains("--smoke-test"))
    QTimer::singleShot(1200, &app, &QApplication::quit);
  return app.exec();
}
