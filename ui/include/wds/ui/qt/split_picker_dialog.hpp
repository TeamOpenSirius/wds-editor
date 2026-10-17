#pragma once

#include <wds/core/gimmick.hpp>
#include <wds/core/official_playfield.hpp>

#include <QDialog>
#include <QString>
#include <cstdint>
#include <memory>
#include <vector>

class QResizeEvent;
class QShowEvent;

class QButtonGroup;
class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QWidget;

namespace wds::ui {

struct SplitPickerPreviewState;

// Qt stand-in for the old painted split picker: track count, color-id search, preview list.
class SplitPickerDialog final : public QDialog {
 public:
  SplitPickerDialog(int32_t count, int32_t color_id,
                    wds::chart_editor::SplitLaneType type = wds::chart_editor::SplitLaneType::Full,
                    QWidget* parent = nullptr, QString skins_dir = {},
                    int quality = wds::chart_editor::kOfficialSpritEffectNormal);

  int32_t count() const noexcept { return count_; }
  int32_t color_id() const noexcept { return color_id_; }
  wds::chart_editor::SplitLaneType type() const noexcept { return type_; }

 protected:
  void showEvent(QShowEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  // Accept and reject both end here: remembers the filter chips and search text for the next opening.
  void done(int result) override;

 private:
  void rebuild_colors();
  void update_filter_ui();
  void clear_filters();
  void save_filter_memory() const;
  void refresh_previews();
  void restart_prefetch();
  void run_prefetch(int gen);
  void schedule_locate_selected();
  void locate_selected_color();
  void sync_aligned_width();
  int content_width() const;
  int min_body_width() const;
  int preview_avail_width() const;
  int preview_box_width() const;

  int32_t count_ = 2;
  int32_t color_id_ = 1;
  wds::chart_editor::SplitLaneType type_ = wds::chart_editor::SplitLaneType::Full;
  QString skins_dir_;
  QButtonGroup* counts_ = nullptr;
  QComboBox* type_combo_ = nullptr;
  QLabel* count_label_ = nullptr;
  QLabel* id_label_ = nullptr;
  QWidget* top_panel_ = nullptr;   // fixed-width, centred block holding the 3 control rows
  QWidget* controls_ = nullptr;    // row 0: count buttons, type, 编号 search
  QWidget* layer_flow_ = nullptr;  // 图层 chip flow (natural single-line width feeds the panel width)
  QWidget* theme_flow_ = nullptr;  // 主题 chip flow
  QLabel* layer_label_ = nullptr;
  QLabel* theme_label_ = nullptr;
  QLineEdit* search_ = nullptr;
  QPushButton* clear_btn_ = nullptr;  // 清除筛选 (end of the 图层 row)
  QLabel* empty_label_ = nullptr;     // shown in the preview host when no id matches
  std::vector<QPushButton*> chips_;   // tri-state tag chips (layers first, then themes)
  QScrollArea* colors_ = nullptr;
  QWidget* colors_host_ = nullptr;
  QGridLayout* colors_grid_ = nullptr;
  QWidget* selected_thumb_ = nullptr;
  std::shared_ptr<SplitPickerPreviewState> state_;
  std::vector<QWidget*> thumbs_;  // pooled ColorThumb widgets; first active_thumbs_ are shown
  size_t active_thumbs_ = 0;
  int prefetch_gen_ = 0;  // coalesces the single deferred prefetch call
  int last_preview_box_w_ = 0;
  bool aligning_ = false;
  bool locate_pending_ = false;
  int locate_retries_ = 0;
};

}  // namespace wds::ui
