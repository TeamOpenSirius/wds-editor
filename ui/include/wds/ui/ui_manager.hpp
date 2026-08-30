#pragma once

#include "wds/ui/curve_template.hpp"
#include "wds/ui/layout/editor_layout.hpp"
#include "wds/ui/toolbar_curve_selection.hpp"

#include "wds/ui/regions/status/status_bar.hpp"

#include "wds/interaction/shortcuts.hpp"
#include "wds/interaction/types.hpp"
#include "wds/interaction/ui_painter.hpp"
#include "wds/interaction/widget_root.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wds::ui {

class ChartPreviewPanel;
class EditorSession;
class ChartEditPanel;
class PreviewHitWidget;
class PreviewSettingsPanel;
class EditorToolbar;
class StatusBar;
class WidthSlotsDialog;
class CurveTemplatesDialog;
class ExportChoiceDialog;
class ChartAddDialog;
class UnsavedChangesDialog;

// Shell orchestrator: owns the four regions and applies EditorLayouter results.
class UiManager {
 public:
  UiManager();
  ~UiManager();

  ChartPreviewPanel& chart_preview() noexcept { return *chart_preview_; }
  const ChartPreviewPanel& chart_preview() const noexcept { return *chart_preview_; }

  wds::interaction::WidgetRoot& root() noexcept { return root_; }
  wds::interaction::ShortcutManager& shortcuts() noexcept { return shortcuts_; }
  EditorSession& session() noexcept { return *session_; }

  ChartEditPanel* edit_panel() noexcept;
  const ChartEditPanel* edit_panel() const noexcept;
  EditorToolbar* toolbar_panel() noexcept;
  PreviewSettingsPanel* settings_panel() noexcept;
  StatusBar* status_bar() noexcept;
  WidthSlotsDialog* width_slots_dialog() noexcept;
  CurveTemplatesDialog* curve_templates_dialog() noexcept;
  ExportChoiceDialog* export_choice_dialog() noexcept;
  ChartAddDialog* chart_add_dialog() noexcept;
  UnsavedChangesDialog* unsaved_changes_dialog() noexcept;

  // Copies live curve-template state into the editor modal. Confirm writes back
  // and persists; cancel/Escape/backdrop discard the working copy.
  void open_curve_templates_dialog();

  // Toolbar Check: snapshot notes, validate overlaps, never mutate document/history.
  void check_chart_errors();
  void set_on_check_chart(std::function<void()> handler) { on_check_chart_ = std::move(handler); }

  // Selected curve-fill template + resolved easing for ChartEditPanel (Task 5).
  CurveFillSelection curve_fill_selection() const;
  void set_on_curve_fill_changed(std::function<void(const CurveFillSelection&)> handler) {
    on_curve_fill_changed_ = std::move(handler);
  }

  // Latest-only status line shown in the bottom bar.
  void set_status(std::string text, StatusLevel level = StatusLevel::Info);

  // Load/save editor UI prefs (OS data dir; see resolve_editor_config_path).
  void set_config_path(std::string path) { config_path_ = std::move(path); }
  const std::string& config_path() const noexcept { return config_path_; }
  void load_ui_config();
  void save_ui_config();
  // Live curve-fill templates. Dialog/toolbar should read/write this; load/save
  // copy it so ordinary settings persists do not wipe config.yml templates.
  CurveTemplateUiState& curve_template_state() noexcept { return curve_template_state_; }
  const CurveTemplateUiState& curve_template_state() const noexcept {
    return curve_template_state_;
  }
  // Wheel/visible-range: coalesce disk writes (~500ms). Settings/toolbar persist stays immediate.
  void request_save_ui_config(bool immediate = true);

  // Used by the window-close path to actually quit after the in-app prompt.
  void set_request_close(std::function<void()> handler) { request_close_ = std::move(handler); }
  // Fullscreen toggle (Ctrl/Cmd+Shift+F11). Bound once the host sets this
  // (needs UiWindow + VulkanRenderer).
  void set_fullscreen_toggler(std::function<void()> handler) {
    fullscreen_toggler_ = std::move(handler);
  }
  // If dirty, shows the in-app unsaved dialog and returns false (abort this close).
  // After Save/Discard, requests close again with a one-shot allow flag.
  bool confirm_close();

  bool save_current_project();

  void resize(int logical_width, int logical_height, int framebuffer_width, int framebuffer_height);
  const EditorLayoutRects& layout() const noexcept { return layout_; }

  void update(float delta_seconds, const std::vector<wds::interaction::InputEvent>& events);
  // Last update() phase costs (µs). Used by frame-diag; always updated.
  int64_t last_update_flush_us() const noexcept { return last_update_flush_us_; }
  int64_t last_update_bounds_us() const noexcept { return last_update_bounds_us_; }
  int64_t last_update_layout_us() const noexcept { return last_update_layout_us_; }
  int64_t last_update_sync_us() const noexcept { return last_update_sync_us_; }
  int64_t last_update_process_us() const noexcept { return last_update_process_us_; }
  void paint(wds::interaction::UiPainter& painter) const;

  // Main UI (panels / edit skins). Status bar, dropdown menus, and modal dialogs
  // are built separately so the preview compositor can draw them above skinned note
  // sprites (depth write is off; UiPainter rects would lose to later sprites in the
  // same batch). Batches are reused across frames (sticky bucket capacity); each
  // build_* clears its target at entry before appending. Returned refs are valid
  // until the next build_* call of the same kind.
  const wds::renderer::DrawBatch& build_ui_batch(wds::renderer::TextureId solid_texture, int fb_w,
                                                 int fb_h,
                                                 const wds::renderer::ScreenBounds& screen);
  // Open ComboBox/Dropdown menus — drawn in the post-overlay pass above UI sprites.
  const wds::renderer::DrawBatch& build_popup_batch(wds::renderer::TextureId solid_texture, int fb_w,
                                                    int fb_h,
                                                    const wds::renderer::ScreenBounds& screen);
  const wds::renderer::DrawBatch& build_modal_batch(wds::renderer::TextureId solid_texture, int fb_w,
                                                    int fb_h,
                                                    const wds::renderer::ScreenBounds& screen);
  const wds::renderer::DrawBatch& build_modal_chrome_batch(wds::renderer::TextureId solid_texture,
                                                           int fb_w, int fb_h,
                                                           const wds::renderer::ScreenBounds& screen);
  // Popup + modal merged into one post-overlay batch (reused).
  const wds::renderer::DrawBatch& build_post_overlay_batch(wds::renderer::TextureId solid_texture,
                                                           int fb_w, int fb_h,
                                                           const wds::renderer::ScreenBounds& screen);
  bool has_modal_popup() const noexcept;

 private:
  void apply_region_bounds();
  void flush_pending_ui_config();
  void prepare_painter(wds::interaction::UiPainter& painter) const;
  // Clear + rebind the active "editor" shortcut namespace from current chords.
  void bind_editor_shortcuts();
  void push_curve_fill_selection();
  // If dirty, open the unsaved dialog and run `continue_fn` after Save/Discard.
  void with_save_if_dirty(std::function<void()> continue_fn);
  // Schedule continue (and optional save) for the start of the next update().
  void schedule_pending_after_save_prompt(bool save_first);
  void flush_pending_after_save_prompt();
  bool has_blocking_modal_dialog() const noexcept;

  std::unique_ptr<ChartPreviewPanel> chart_preview_;
  std::unique_ptr<EditorSession> session_;
  wds::interaction::WidgetRoot root_;
  wds::interaction::ShortcutManager shortcuts_;
  EditorLayouter layouter_{};
  EditorLayoutRects layout_{};
  int width_ = 1;   // logical (window) px
  int height_ = 1;  // logical (window) px
  int fb_width_ = 1;
  int fb_height_ = 1;
  // Timeline position (ms) when the current play segment started; Space return target.
  int64_t play_anchor_ms_ = 0;
  ChartEditPanel* edit_panel_ = nullptr;
  PreviewHitWidget* preview_hit_ = nullptr;
  WidthSlotsDialog* width_slots_dialog_ = nullptr;
  CurveTemplatesDialog* curve_templates_dialog_ = nullptr;
  ExportChoiceDialog* export_choice_dialog_ = nullptr;
  ChartAddDialog* chart_add_dialog_ = nullptr;
  UnsavedChangesDialog* unsaved_changes_dialog_ = nullptr;
  StatusBar* status_bar_ = nullptr;
  std::string config_path_;
  CurveTemplateUiState curve_template_state_{};
  std::function<void()> pending_after_save_;
  std::function<void()> request_close_;
  std::function<void()> fullscreen_toggler_;
  std::function<void()> on_check_chart_;
  std::function<void(const CurveFillSelection&)> on_curve_fill_changed_;
  bool allow_close_once_ = false;
  // Run pending open/import/close after the click frame finishes (native panels need this).
  bool flush_pending_after_save_ = false;
  bool pending_save_before_continue_ = false;

  wds::renderer::DrawBatch ui_batch_;
  wds::renderer::DrawBatch popup_batch_;
  wds::renderer::DrawBatch modal_batch_;
  wds::renderer::DrawBatch chrome_batch_;
  wds::renderer::DrawBatch post_overlay_batch_;

  int64_t last_update_flush_us_ = 0;
  int64_t last_update_bounds_us_ = 0;
  int64_t last_update_layout_us_ = 0;
  int64_t last_update_sync_us_ = 0;
  int64_t last_update_process_us_ = 0;
  bool ui_config_dirty_ = false;
  int64_t ui_config_dirty_us_ = 0;
};

}  // namespace wds::ui
