#include "wds/ui/qt/editor_main_window.hpp"
#include "wds/ui/qt/wds_theme.hpp"
#include "wds/ui/ui_manager.hpp"

#include <QApplication>
#include <QDockWidget>
#include <QEventLoop>
#include <QFontDatabase>
#include <QFileInfo>
#include <QPalette>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool value, const char* message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
void settle() {
  QEventLoop loop;
  QTimer::singleShot(200, &loop, &QEventLoop::quit);
  loop.exec();
}
}

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir prefs;
  require(prefs.isValid(), "Temporary settings directory unavailable");
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, prefs.path());
  QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, prefs.path());
  const int font = QFontDatabase::addApplicationFont(
      QStringLiteral(WDS_REPO_ROOT "/ui/assets/fonts/NotoSansSC-Regular.ttf"));
  require(font >= 0, "Bundled Noto font failed to load");
  require(QFontDatabase::applicationFontFamilies(font).contains("Noto Sans SC"),
          "Theme font family does not match bundled font");
  const QString themes = QStringLiteral(WDS_REPO_ROOT "/ui/assets/theme");
  const auto available = wds::ui::available_themes(themes);
  require(available.size() == 7, "Missing bundled OBS themes");
  for (const auto& theme : available) {
    wds::ui::apply_wds_theme(app, themes, theme.id);
    const auto qss = app.styleSheet();
    require(qss.contains("'Noto Sans SC'"), "Theme overrides Noto");
    require(!qss.contains("var(--") && !qss.contains("@OBSTheme"), "Unresolved theme syntax");
    const QRegularExpression urls(R"(url\(\s*([^\)]+)\))");
    auto assets = urls.globalMatch(qss);
    while (assets.hasNext()) {
      const QString path = assets.next().captured(1).trimmed();
      if (!QFileInfo::exists(path)) {
        std::cerr << theme.id.toStdString() << ": missing " << path.toStdString() << '\n';
        return 1;
      }
    }
    if (!theme.dark)
      require(app.palette().color(QPalette::Window).lightness() > 180, "Light palette not applied");
  }
  wds::ui::apply_wds_theme(app, themes);

  QVulkanInstance instance;
  require(instance.create(), "Vulkan instance unavailable for viewport regression test");
  wds::ui::UiManager manager;
  manager.enable_qt_chrome();
  wds::ui::EditorMainWindow window;
  auto* preview = new wds::ui::RealtimeVulkanWindow(&instance);
  auto* editor = new wds::ui::RealtimeVulkanWindow(&instance);
  window.set_viewport_windows(preview, editor);
  window.bind_ui_manager(&manager);
  window.showNormal();
  window.resize(1400, 1000);
  settle();
  auto* playback = window.findChild<QDockWidget*>("playbackAudioDock");
  auto* audio = window.findChild<QDockWidget*>("audioMixDock");
  auto* toolbox = window.findChild<QDockWidget*>("editorToolboxDock");
  auto* viewport = window.findChild<QDockWidget*>("editorViewportDock");
  require(playback && audio && toolbox && viewport, "Missing docks");
  const int bottom = playback->height();
  require(playback->minimumHeight() == 200 && playback->maximumHeight() == 200,
          "Playback dock is not pinned to the control-row height");
  require(audio->minimumHeight() == 200 && audio->maximumHeight() == 200,
          "Audio dock is not pinned to the control-row height");
  require(toolbox->minimumHeight() == 200 && toolbox->maximumHeight() == 200,
          "Toolbox dock is not pinned to the control-row height");
  const int top = viewport->height();
  window.resize(1400, 850);
  settle();
  std::cout << "bottom " << bottom << " -> " << playback->height()
            << ", top " << top << " -> " << viewport->height() << std::endl;
  require(std::abs(playback->height() - bottom) <= 2, "Window resize changed bottom row height");
  require(viewport->height() < top - 100, "Viewport did not absorb window resize");
  window.resizeDocks({playback}, {bottom + 50}, Qt::Vertical);
  settle();
  require(std::abs(playback->height() - bottom) <= 2,
          "Bottom control row accepted a vertical resize");
  window.resize(1400, 1000);
  settle();
  require(std::abs(playback->height() - bottom) <= 2, "Bottom row height was lost");
  window.resize(1000, 500);
  settle();
  window.resize(1400, 1000);
  settle();
  require(std::abs(playback->height() - bottom) <= 2,
          "Temporary minimum-size constraints changed the control row");

  int frames = 0;
  editor->set_frame_callback([&](float, int, int, int, int, const auto&) { ++frames; });
  settle();
  require(frames > 0, "Viewport never started rendering");
  editor->set_resize_suspended(true);
  const int before = frames;
  // Simulate a child resize during host suspension, followed by its debounce
  // timeout and an already-queued update carrying input.
  QResizeEvent resized(editor->size(), editor->size());
  QApplication::sendEvent(editor, &resized);
  editor->inject_key_tap(wds::interaction::KeyCode::Space, {});
  settle();
  QEvent update(QEvent::UpdateRequest);
  QApplication::sendEvent(editor, &update);
  require(frames == before, "Suspended viewport rendered a queued frame");
  editor->set_resize_suspended(false);
  settle();
  require(frames > before, "Viewport failed to resume after resizing");
  std::cout << "Qt themes, font, dock sizing and render suspension passed\n";
}
