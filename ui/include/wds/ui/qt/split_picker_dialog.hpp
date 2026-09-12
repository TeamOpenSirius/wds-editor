#pragma once

#include <QDialog>
#include <cstdint>

class QButtonGroup;
class QLineEdit;
class QListWidget;

namespace wds::ui {

// Qt stand-in for the old painted split picker: track count, color-id search, preview list.
class SplitPickerDialog final : public QDialog {
 public:
  SplitPickerDialog(int32_t count, int32_t color_id, QWidget* parent = nullptr);

  int32_t count() const noexcept { return count_; }
  int32_t color_id() const noexcept { return color_id_; }

 private:
  void rebuild_colors();

  int32_t count_ = 2;
  int32_t color_id_ = 1;
  QButtonGroup* counts_ = nullptr;
  QLineEdit* search_ = nullptr;
  QListWidget* colors_ = nullptr;
};

}  // namespace wds::ui
