#pragma once

#include <QWidget>
#include <array>
#include <functional>

#include <wds/core/easing.hpp>

class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace wds::ui {

class UiManager;

class CurveTemplatesPanel final : public QWidget {
 public:
  explicit CurveTemplatesPanel(UiManager* manager, QWidget* parent = nullptr);
  void set_on_changed(std::function<void()> handler) { on_changed_ = std::move(handler); }
  void reload_from_manager();

 private:
  void build_ui();
  void sync_fields_from_state();
  void apply_live();
  void update_enabled();

  UiManager* manager_ = nullptr;
  bool syncing_ = false;
  std::function<void()> on_changed_;

  QListWidget* list_ = nullptr;
  QPushButton* add_ = nullptr;
  QPushButton* remove_ = nullptr;
  QLineEdit* name_ = nullptr;
  QComboBox* algorithm_ = nullptr;
  QDoubleSpinBox* parameter_ = nullptr;
  std::array<QWidget*, 4> previews_{};
};

}  // namespace wds::ui
