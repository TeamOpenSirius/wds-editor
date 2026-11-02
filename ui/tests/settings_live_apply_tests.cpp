// Regression: the Qt 「设置」 panel keeps the EditorUiConfig snapshot taken when
// it was built. Toggling one of its fields used to hand that whole snapshot back
// to UiManager::apply_ui_config_from_qt(), which re-applied settings owned by
// other surfaces — notably the extended-mode lane range (轨道可见范围) dragged on
// the edit canvas. The freshly chosen range jumped back to the value captured at
// panel construction, and the stale value was then persisted to config.yml.
#include "wds/ui/qt/settings_dialog.hpp"

#include "wds/ui/editor_session.hpp"
#include "wds/ui/regions/edit/chart_edit_panel.hpp"
#include "wds/ui/ui_manager.hpp"

#include <QApplication>
#include <QByteArray>
#include <QString>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace wds::ui {
// SettingsPanel befriends this in settings_dialog.hpp.
struct SettingsPanelTestAccess {
  static void apply_live(SettingsPanel& panel) { panel.apply_live(); }
};
}  // namespace wds::ui

namespace {

int failures = 0;

void check(bool cond, const char* expr, int line) {
  if (!cond) {
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, line, expr);
    ++failures;
  }
}
#define CHECK(expr) check((expr), #expr, __LINE__)

std::string read_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A settings edit while the panel is open must not revive the lane range (or any
// other externally owned field) from the panel's construction-time snapshot.
void test_settings_edit_keeps_live_lane_range() {
  const auto path = std::filesystem::temp_directory_path() /
                    "wds_settings_live_apply_tests_config.yml";
  std::filesystem::remove(path);

  wds::ui::UiManager ui;
  ui.set_config_path(path.string());
  ui.load_ui_config();
  auto* edit = ui.edit_panel();
  CHECK(edit != nullptr);
  if (edit == nullptr) return;

  wds::ui::SettingsPanel panel(&ui, QString());
  // Extended mode on, then the user drags 「可见范围」 on the edit canvas.
  ui.session().set_extended_features(true);
  edit->set_extra_visible_lanes(7);
  CHECK(edit->extra_visible_lanes() == 7);

  // Now the user flips any setting in the panel.
  wds::ui::SettingsPanelTestAccess::apply_live(panel);

  // The lane range must survive the settings apply...
  CHECK(edit->extra_visible_lanes() == 7);

  // ...and reach config.yml instead of the stale snapshot value.
  const std::string saved = read_text(path);
  CHECK(saved.find("extra_visible_lanes: 7") != std::string::npos);

  // A config reload (what happens on the next launch / settings apply) keeps it.
  edit->set_extra_visible_lanes(0);
  ui.load_ui_config();
  CHECK(edit->extra_visible_lanes() == 7);

  std::filesystem::remove(path);
}

}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  test_settings_edit_keeps_live_lane_range();
  if (failures != 0) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("settings_live_apply_tests: ok\n");
  return 0;
}
