#include "wds/ui/ui_manager.hpp"

#include "wds/ui/editor_session.hpp"
#include "wds/ui/editor_ui_config.hpp"
#include "wds/ui/native_file_dialog.hpp"
#include "wds/ui/regions/edit/chart_edit_panel.hpp"
#include "wds/ui/regions/preview/chart_preview_panel.hpp"
#include "wds/ui/regions/preview/preview_hit_widget.hpp"
#include "wds/ui/regions/settings/chart_add_dialog.hpp"
#include "wds/ui/regions/settings/curve_templates_dialog.hpp"
#include "wds/ui/regions/settings/export_choice_dialog.hpp"
#include "wds/ui/regions/settings/preview_settings_panel.hpp"
#include "wds/ui/regions/settings/unsaved_changes_dialog.hpp"
#include "wds/ui/regions/settings/width_slots_dialog.hpp"
#include "wds/ui/regions/status/status_bar.hpp"
#include "wds/ui/regions/toolbar/editor_toolbar.hpp"

#include <wds/core/chart_validation.hpp>
#include <wds/core/edit_grid.hpp>
#include <wds/core/notation.hpp>

#include <wds/common/crash_input_journal.hpp>
#include <wds/common/time.hpp>

#include <wds/interaction/editor_input.hpp>
#include <wds/interaction/theme.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace wds::ui {
namespace {

void capture_display_from_preview(const ChartPreviewPanel& preview, EditorUiConfig& cfg) {
  const auto& visual = preview.preview().config();
  cfg.note_speed = visual.note_speed;
  cfg.note_start_offset = visual.note_start_offset;
  cfg.note_height_level = visual.note_height_level;
  cfg.split_line_opacity =
      static_cast<int>(std::lround(static_cast<double>(visual.split_line_opacity) * 100.0));
  cfg.msaa_samples = visual.msaa_samples;
}

void apply_display_to_preview(ChartPreviewPanel& preview, const EditorUiConfig& cfg) {
  preview.apply_display_settings(cfg.note_speed, cfg.note_start_offset, cfg.note_height_level,
                                 cfg.split_line_opacity);
  preview.preview().apply_msaa(cfg.msaa_samples);
}

}  // namespace

UiManager::UiManager() : chart_preview_(std::make_unique<ChartPreviewPanel>()) {
  session_ = std::make_unique<EditorSession>(*chart_preview_);
  session_->set_status_handler([this](std::string text, StatusLevel level) {
    set_status(std::move(text), level);
  });

  // Child order: toolbar → settings → preview_hit → edit → status → dialogs.
  // Later siblings win reverse hit-test, so edit full-window modals and dialogs
  // stay in front of the preview hit target.
  auto edit = std::make_unique<ChartEditPanel>(session_->engine());
  edit_panel_ = edit.get();
  edit->set_seek_ms([this](int64_t ms) { chart_preview_->transport().request_seek_ms(ms); });
  edit->set_visible_range_changed_handler([this] {
    if (auto* toolbar_panel = this->toolbar_panel()) {
      toolbar_panel->sync_visible_range_field();
    }
    request_save_ui_config(false);
  });
  auto preview_hit = std::make_unique<PreviewHitWidget>();
  preview_hit_ = preview_hit.get();
  preview_hit_->set_scroll_handler([this](const wds::interaction::ScrollEvent& event) {
    if (edit_panel_ != nullptr) {
      edit_panel_->handle_timeline_wheel(event);
    }
  });
  preview_hit_->set_trace_peer(edit_panel_);
  auto toolbar = std::make_unique<EditorToolbar>(*session_, *edit);
  auto settings = std::make_unique<PreviewSettingsPanel>(*chart_preview_);
  auto status = std::make_unique<StatusBar>();
  status_bar_ = status.get();
  auto width_dialog = std::make_unique<WidthSlotsDialog>();
  width_slots_dialog_ = width_dialog.get();
  auto curve_dialog = std::make_unique<CurveTemplatesDialog>();
  curve_templates_dialog_ = curve_dialog.get();
  auto export_dialog = std::make_unique<ExportChoiceDialog>();
  export_choice_dialog_ = export_dialog.get();
  auto chart_add = std::make_unique<ChartAddDialog>();
  chart_add_dialog_ = chart_add.get();
  auto unsaved = std::make_unique<UnsavedChangesDialog>();
  unsaved_changes_dialog_ = unsaved.get();

  toolbar->set_settings_handler([this] {
    if (width_slots_dialog_ == nullptr) return;
    EditorUiConfig cfg;
    capture_live_ui_config(cfg);
    width_slots_dialog_->set_config(cfg);
    width_slots_dialog_->open();
  });
  toolbar->set_export_handler([this] {
    if (export_choice_dialog_ != nullptr) export_choice_dialog_->open();
  });
  toolbar->set_open_handler([this] {
    with_save_if_dirty([this] {
      if (auto path = native_file_dialog::open_file("打开 WDS 工程", {"wdsproject"})) {
        (void)session_->open_wdsproject(*path);
      } else {
        set_status("打开已取消", StatusLevel::Info);
      }
    });
  });
  toolbar->set_import_handler([this] {
    with_save_if_dirty([this] {
      if (auto path = native_file_dialog::open_file("导入官方谱面", {"csv", "sus"})) {
        if (!session_->import_official(*path)) {
          set_status("导入失败：" + *path, StatusLevel::Error);
        }
      } else {
        set_status("导入已取消", StatusLevel::Info);
      }
    });
  });
  toolbar->set_chart_add_handler([this] {
    if (chart_add_dialog_ != nullptr) chart_add_dialog_->open();
  });
  toolbar->bind_curve_state(&curve_template_state_);
  toolbar->set_curve_templates_handler([this] { open_curve_templates_dialog(); });
  toolbar->set_check_handler([this] { check_chart_errors(); });
  toolbar->set_curve_fill_changed_handler([this](const CurveFillSelection&) {
    push_curve_fill_selection();
  });
  chart_add->on_create_new([this] {
    if (!session_->read_only()) {
      if (session_->add_chart()) set_status("已添加空白谱面", StatusLevel::Info);
    } else {
      set_status("只读预览无法添加谱面", StatusLevel::Error);
    }
  });
  chart_add->on_add_existing([this] {
    if (session_->read_only()) {
      set_status("只读预览无法添加谱面", StatusLevel::Error);
      return;
    }
    if (auto path = native_file_dialog::open_file("添加已有谱面", {"wdschart"})) {
      if (session_->add_chart_from_file(*path)) {
        set_status("已添加谱面：" + *path, StatusLevel::Info);
      } else {
        set_status("添加谱面失败：" + *path, StatusLevel::Error);
      }
    } else {
      set_status("添加谱面已取消", StatusLevel::Info);
    }
  });
  unsaved->on_save([this] { schedule_pending_after_save_prompt(/*save_first=*/true); });
  unsaved->on_discard([this] { schedule_pending_after_save_prompt(/*save_first=*/false); });
  unsaved->on_cancel([this] {
    pending_after_save_ = {};
    flush_pending_after_save_ = false;
    pending_save_before_continue_ = false;
  });
  export_dialog->on_export_project([this](ExportFormat format) {
    if (session_->read_only()) {
      set_status("只读预览无法导出", StatusLevel::Error);
      return;
    }
    if (format == ExportFormat::Sus) {
      if (auto dir = native_file_dialog::choose_directory("选择 SUS 导出目录")) {
        (void)session_->export_sus_project(*dir);
      } else {
        set_status("导出已取消", StatusLevel::Info);
      }
      return;
    }
    if (auto dir = native_file_dialog::choose_directory("选择导出目录")) {
      (void)session_->export_official_project(*dir);
    } else {
      set_status("导出已取消", StatusLevel::Info);
    }
  });
  export_dialog->on_export_chart([this](ExportFormat format) {
    if (session_->read_only()) {
      set_status("只读预览无法导出", StatusLevel::Error);
      return;
    }
    if (format == ExportFormat::Sus) {
      const std::string default_name = session_->sus_chart_filename(session_->active_chart_index());
      if (auto path = native_file_dialog::save_file("导出当前谱面 (SUS)", default_name, {"sus"})) {
        (void)session_->export_sus(*path);
      } else {
        set_status("导出已取消", StatusLevel::Info);
      }
      return;
    }
    const std::string default_name =
        session_->official_chart_filename(session_->active_chart_index());
    if (auto path = native_file_dialog::save_file("导出当前谱面", default_name, {"csv"})) {
      (void)session_->export_official(*path);
    } else {
      set_status("导出已取消", StatusLevel::Info);
    }
  });
  root_.add_child(std::move(toolbar));
  root_.add_child(std::move(settings));
  root_.add_child(std::move(preview_hit));
  root_.add_child(std::move(edit));
  root_.add_child(std::move(status));
  root_.add_child(std::move(width_dialog));
  root_.add_child(std::move(curve_dialog));
  root_.add_child(std::move(export_dialog));
  root_.add_child(std::move(chart_add));
  root_.add_child(std::move(unsaved));

  set_status("就绪", StatusLevel::Info);

  const auto persist = [this] { save_ui_config(); };
  if (width_slots_dialog_ != nullptr) {
    width_slots_dialog_->set_on_applied([this, persist] {
      EditorUiConfig cfg;
      width_slots_dialog_->capture_config(cfg);
      session_->set_sus_auto_convert(cfg.sus_auto_convert);
      chart_preview_->preview().set_mute_hold_body_sfx(cfg.mute_hold_body_sfx);
      chart_preview_->preview().set_show_judgment_text(cfg.show_judgment_text);
      wds::interaction::set_invert_scroll_wheel(cfg.invert_scroll_wheel);
      wds::interaction::set_invert_visible_range_scroll(cfg.invert_visible_range_scroll);
      wds::interaction::set_scroll_wheel_speed(cfg.scroll_wheel_speed);
      if (cfg.shortcuts_initialized) {
        wds::interaction::set_editor_shortcuts(cfg.shortcuts);
      }
      apply_display_to_preview(*chart_preview_, cfg);
      bind_editor_shortcuts();
      wds::common::journal_set_allow_sensitive(cfg.allow_crash_log_sensitive);
      persist();
    });
  }
  if (curve_templates_dialog_ != nullptr) {
    curve_templates_dialog_->set_on_confirmed([this](const CurveTemplateUiState& working) {
      const auto fill_id = curve_template_state_.selected_id;
      const auto direction = curve_template_state_.direction;
      curve_template_state_.templates = working.templates;
      curve_template_state_.selected_id = fill_id;
      curve_template_state_.direction = direction;
      if (curve_template_state_.selected_id != 0 &&
          find_curve_template_by_id(curve_template_state_.templates,
                                    curve_template_state_.selected_id) == nullptr) {
        curve_template_state_.selected_id = 0;
      }
      if (auto* toolbar_panel = this->toolbar_panel()) toolbar_panel->refresh_curve_controls();
      push_curve_fill_selection();
      request_save_ui_config(true);
    });
  }
  if (auto* settings_panel = this->settings_panel()) {
    settings_panel->set_persist_handler(persist);
  }
  if (auto* toolbar_panel = this->toolbar_panel()) {
    toolbar_panel->set_persist_handler(persist);
  }

  shortcuts_.set_active_namespace("editor");
  bind_editor_shortcuts();
  push_curve_fill_selection();
}

void UiManager::bind_editor_shortcuts() {
  auto& editor = shortcuts_.namespace_for("editor");
  editor.clear();
  using wds::interaction::chord_copy;
  using wds::interaction::chord_delete_selection;
  using wds::interaction::chord_mirror;
  using wds::interaction::chord_mirror_about_center;
  using wds::interaction::chord_nudge_down;
  using wds::interaction::chord_nudge_left;
  using wds::interaction::chord_nudge_right;
  using wds::interaction::chord_nudge_up;
  using wds::interaction::chord_open;
  using wds::interaction::chord_paste;
  using wds::interaction::chord_redo;
  using wds::interaction::chord_save;
  using wds::interaction::chord_pause_playback;
  using wds::interaction::chord_toggle_fullscreen;
  using wds::interaction::chord_toggle_playback;
  using wds::interaction::chord_toggle_sfx_mute;
  using wds::interaction::chord_playback_rate_slot;
  using wds::interaction::chord_undo;
  using wds::interaction::chord_width_slot;
  using wds::interaction::playback_rate_for_slot;
  using wds::interaction::width_slot_values_const;

  const auto handle_playback = [this](bool shift) {
    auto& transport = chart_preview_->transport();
    auto* edit = edit_panel();
    const bool pause_at_current = edit != nullptr && edit->pause_at_current();
    if (!transport.playing()) {
      play_anchor_ms_ = transport.committed_ms();
      transport.request_play();
      return;
    }
    // Default: Space → return to play start + pause; Shift+Space → pause in place.
    // Checked: swap those two Space behaviors.
    const bool pause_here = pause_at_current ? !shift : shift;
    if (!pause_here) {
      transport.request_seek_ms(play_anchor_ms_);
    }
    transport.request_pause();
  };
  editor.bind(chord_toggle_playback(), [handle_playback] { handle_playback(false); });
  editor.bind(chord_pause_playback(), [handle_playback] { handle_playback(true); });
  editor.bind(chord_toggle_fullscreen(), [this] {
    if (fullscreen_toggler_) fullscreen_toggler_();
  });
  editor.bind(chord_save(), [this] { save_current_project(); });
  editor.bind(chord_open(), [this] {
    with_save_if_dirty([this] {
      if (auto path = native_file_dialog::open_file("打开 WDS 工程", {"wdsproject"})) {
        (void)session_->open_wdsproject(*path);
      }
    });
  });
  editor.bind(chord_undo(), [this] { session_->engine().undo(); });
  editor.bind(chord_redo(), [this] { session_->engine().redo(); });
  editor.bind(chord_copy(), [this] {
    if (auto* panel = edit_panel()) panel->copy_selected();
  });
  editor.bind(chord_paste(), [this] {
    if (auto* panel = edit_panel()) panel->paste_at_pointer();
  });
  editor.bind(chord_mirror(), [this] {
    if (auto* panel = edit_panel()) panel->mirror_selected(false);
  });
  editor.bind(chord_mirror_about_center(), [this] {
    if (auto* panel = edit_panel()) panel->mirror_selected(true);
  });
  editor.bind(chord_nudge_up(), [this] {
    if (auto* panel = edit_panel())
      panel->nudge_selected(-wds::chart_editor::subdivision_tick_step(panel->viewport().grid()), 0);
  });
  editor.bind(chord_nudge_down(), [this] {
    if (auto* panel = edit_panel())
      panel->nudge_selected(wds::chart_editor::subdivision_tick_step(panel->viewport().grid()), 0);
  });
  editor.bind(chord_nudge_left(), [this] {
    if (auto* panel = edit_panel()) panel->nudge_selected(0, -1);
  });
  editor.bind(chord_nudge_right(), [this] {
    if (auto* panel = edit_panel()) panel->nudge_selected(0, 1);
  });
  editor.bind(chord_delete_selection(), [this] {
    if (auto* panel = edit_panel()) panel->delete_selected();
  });
  // Width slots → default place width. Bound globally so focus on toolbar/settings
  // still updates the edit panel (widget key path only reaches the focused widget).
  for (int slot = 0; slot < 6; ++slot) {
    editor.bind(chord_width_slot(slot), [this, slot] {
      if (has_blocking_modal_dialog()) return;
      if (auto* panel = edit_panel()) {
        panel->set_default_width(width_slot_values_const()[static_cast<std::size_t>(slot)]);
      }
    });
  }
  // Playback rate presets (default F1–F4 → 0.25x / 0.5x / 0.75x / 1x).
  for (int slot = 0; slot < 4; ++slot) {
    editor.bind(chord_playback_rate_slot(slot), [this, slot] {
      if (has_blocking_modal_dialog()) return;
      const auto rate = playback_rate_for_slot(slot);
      if (!rate) return;
      if (auto* settings = settings_panel()) settings->set_playback_rate(*rate);
    });
  }
  editor.bind(chord_toggle_sfx_mute(), [this] {
    if (has_blocking_modal_dialog()) return;
    if (auto* settings = settings_panel()) settings->toggle_sfx_mute();
  });
}

UiManager::~UiManager() {
  if (ui_config_dirty_) {
    save_ui_config();
  }
}

ChartEditPanel* UiManager::edit_panel() noexcept { return edit_panel_; }

const ChartEditPanel* UiManager::edit_panel() const noexcept { return edit_panel_; }

EditorToolbar* UiManager::toolbar_panel() noexcept {
  const auto& children = root_.children();
  if (children.empty()) return nullptr;
  return static_cast<EditorToolbar*>(children[0].get());
}

PreviewSettingsPanel* UiManager::settings_panel() noexcept {
  const auto& children = root_.children();
  if (children.size() < 2) return nullptr;
  return static_cast<PreviewSettingsPanel*>(children[1].get());
}

StatusBar* UiManager::status_bar() noexcept { return status_bar_; }

void UiManager::set_status(std::string text, StatusLevel level) {
  if (status_bar_ != nullptr) status_bar_->set_message(std::move(text), level);
}

WidthSlotsDialog* UiManager::width_slots_dialog() noexcept { return width_slots_dialog_; }

CurveTemplatesDialog* UiManager::curve_templates_dialog() noexcept {
  return curve_templates_dialog_;
}

void UiManager::open_curve_templates_dialog() {
  if (curve_templates_dialog_ == nullptr) return;
  curve_templates_dialog_->open(curve_template_state_);
}

void UiManager::check_chart_errors() {
  auto& engine = session_->engine();
  const auto result = wds::chart_editor::find_note_overlaps(engine.document().notes());
  const uint64_t generation = engine.document().content_generation();
  if (edit_panel_ != nullptr) {
    edit_panel_->set_error_ticks(result.error_ticks, generation);
  }
  if (result.error_ticks.empty()) {
    set_status("未发现音符重叠", StatusLevel::Info);
  } else {
    const int32_t first_tick = result.error_ticks.front();
    const int64_t ms =
        wds::chart_editor::tick_to_milliseconds(first_tick, engine.document().timing());
    chart_preview_->transport().request_pause();
    chart_preview_->transport().request_seek_ms(ms);
    wds::common::TimelineSnapshot snap;
    snap.position = wds::common::ms_to_us(ms);
    snap.state = wds::common::PlaybackState::Paused;
    engine.apply_timeline(snap);
    set_status("发现 " + std::to_string(result.error_ticks.size()) + " 处音符重叠（" +
                   std::to_string(result.pairs.size()) + " 组），已跳转到 tick " +
                   std::to_string(first_tick),
               StatusLevel::Warning);
  }
  if (on_check_chart_) on_check_chart_();
}

CurveFillSelection UiManager::curve_fill_selection() const {
  return make_curve_fill_selection(curve_template_state_);
}

void UiManager::push_curve_fill_selection() {
  const CurveFillSelection selection = curve_fill_selection();
  if (edit_panel_ != nullptr) edit_panel_->set_curve_fill_selection(selection);
  if (on_curve_fill_changed_) on_curve_fill_changed_(selection);
}

ExportChoiceDialog* UiManager::export_choice_dialog() noexcept { return export_choice_dialog_; }

ChartAddDialog* UiManager::chart_add_dialog() noexcept { return chart_add_dialog_; }

UnsavedChangesDialog* UiManager::unsaved_changes_dialog() noexcept {
  return unsaved_changes_dialog_;
}

bool UiManager::save_current_project() {
  if (session_->read_only()) {
    set_status("只读预览无法保存", StatusLevel::Error);
    return false;
  }
  if (session_->project_path().empty()) {
    if (auto path = native_file_dialog::save_file("保存 WDS 工程", "untitled.wdsproject",
                                                  {"wdsproject"})) {
      return session_->save_as(*path);
    }
    set_status("保存已取消", StatusLevel::Info);
    return false;
  }
  return session_->save();
}

void UiManager::with_save_if_dirty(std::function<void()> continue_fn) {
  if (!session_->dirty()) {
    if (continue_fn) continue_fn();
    return;
  }
  if (unsaved_changes_dialog_ == nullptr) {
    if (continue_fn) continue_fn();
    return;
  }
  pending_after_save_ = std::move(continue_fn);
  flush_pending_after_save_ = false;
  pending_save_before_continue_ = false;
  unsaved_changes_dialog_->open();
}

void UiManager::schedule_pending_after_save_prompt(bool save_first) {
  pending_save_before_continue_ = save_first;
  flush_pending_after_save_ = true;
}

void UiManager::flush_pending_after_save_prompt() {
  if (!flush_pending_after_save_) return;
  flush_pending_after_save_ = false;
  const bool save_first = pending_save_before_continue_;
  pending_save_before_continue_ = false;
  if (save_first) {
    if (!save_current_project()) {
      pending_after_save_ = {};
      return;
    }
  }
  auto fn = std::move(pending_after_save_);
  pending_after_save_ = {};
  if (fn) fn();
}

bool UiManager::confirm_close() {
  if (allow_close_once_) {
    allow_close_once_ = false;
    return true;
  }
  if (!session_->dirty()) return true;
  with_save_if_dirty([this] {
    allow_close_once_ = true;
    if (request_close_) request_close_();
  });
  return false;
}

void UiManager::load_ui_config() {
  if (config_path_.empty()) return;
  EditorUiConfig cfg;
  if (!load_editor_ui_config(config_path_, cfg)) return;
  wds::interaction::set_width_slot_values(cfg.width_slots);
  session_->set_sus_auto_convert(cfg.sus_auto_convert);
  chart_preview_->preview().set_mute_hold_body_sfx(cfg.mute_hold_body_sfx);
  chart_preview_->preview().set_show_judgment_text(cfg.show_judgment_text);
  wds::interaction::set_invert_scroll_wheel(cfg.invert_scroll_wheel);
  wds::interaction::set_invert_visible_range_scroll(cfg.invert_visible_range_scroll);
  wds::interaction::set_scroll_wheel_speed(cfg.scroll_wheel_speed);
  if (cfg.shortcuts_initialized) {
    wds::interaction::set_editor_shortcuts(cfg.shortcuts);
    bind_editor_shortcuts();
  }
  apply_display_to_preview(*chart_preview_, cfg);
  capture_curve_template_state(cfg, curve_template_state_);
  if (width_slots_dialog_ != nullptr) width_slots_dialog_->set_config(cfg);
  wds::common::journal_set_allow_sensitive(cfg.allow_crash_log_sensitive);
  if (auto* settings = settings_panel()) settings->apply_config(cfg);
  if (auto* toolbar = toolbar_panel()) {
    toolbar->apply_config(cfg);
    toolbar->refresh_curve_controls();
  }
  push_curve_fill_selection();
}

void UiManager::capture_live_ui_config(EditorUiConfig& cfg) {
  if (auto* settings = settings_panel()) settings->capture_config(cfg);
  if (auto* toolbar = toolbar_panel()) toolbar->capture_config(cfg);
  cfg.width_slots = wds::interaction::width_slot_values_const();
  cfg.mute_hold_body_sfx = chart_preview_->preview().mute_hold_body_sfx();
  cfg.show_judgment_text = chart_preview_->preview().show_judgment_text();
  cfg.sus_auto_convert = session_->sus_auto_convert();
  cfg.invert_scroll_wheel = wds::interaction::invert_scroll_wheel();
  cfg.invert_visible_range_scroll = wds::interaction::invert_visible_range_scroll();
  cfg.scroll_wheel_speed = wds::interaction::scroll_wheel_speed();
  cfg.allow_crash_log_sensitive = wds::common::journal_allow_sensitive();
  cfg.shortcuts = wds::interaction::editor_shortcuts_snapshot();
  cfg.shortcuts_initialized = true;
  capture_display_from_preview(*chart_preview_, cfg);
  apply_curve_template_state(cfg, curve_template_state_);
}

void UiManager::save_ui_config() {
  ui_config_dirty_ = false;
  ui_config_dirty_us_ = 0;
  if (config_path_.empty()) return;
  EditorUiConfig cfg;
  capture_live_ui_config(cfg);
  save_editor_ui_config(config_path_, cfg);
}

void UiManager::request_save_ui_config(bool immediate) {
  if (immediate) {
    save_ui_config();
    return;
  }
  ui_config_dirty_ = true;
  ui_config_dirty_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
}

void UiManager::flush_pending_ui_config() {
  if (!ui_config_dirty_) {
    return;
  }
  const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  if (now_us - ui_config_dirty_us_ >= 500000) {
    save_ui_config();
  }
}

void UiManager::resize(int logical_width, int logical_height, int framebuffer_width,
                       int framebuffer_height) {
  const int w = std::max(1, logical_width);
  const int h = std::max(1, logical_height);
  const int fb_w = std::max(1, framebuffer_width);
  const int fb_h = std::max(1, framebuffer_height);
  if (w == width_ && h == height_ && fb_w == fb_width_ && fb_h == fb_height_) {
    return;
  }
  width_ = w;
  height_ = h;
  fb_width_ = fb_w;
  fb_height_ = fb_h;

  const EditorLayoutResult computed = layouter_.compute(width_, height_);
  layout_ = computed.regions;
  root_.set_bounds({0, 0, static_cast<float>(width_), static_cast<float>(height_)});
  apply_region_bounds();

  chart_preview_->resize_framebuffer(fb_width_, fb_height_);
  // Preview stage is placed in framebuffer pixels.
  const float s = wds::interaction::theme::ui_content_scale();
  const auto round_fb = [s](float logical) {
    return std::max(1, static_cast<int>(std::lround(logical * s)));
  };
  chart_preview_->set_content_bounds(
      static_cast<int>(std::lround(static_cast<float>(computed.preview_content.x) * s)),
      static_cast<int>(std::lround(static_cast<float>(computed.preview_content.y) * s)),
      round_fb(static_cast<float>(computed.preview_content.width)),
      round_fb(static_cast<float>(computed.preview_content.height)));
  // Full preview column — background cover fills this without spilling into edit.
  chart_preview_->set_panel_bounds(
      static_cast<int>(std::lround(layout_.preview.x * s)),
      static_cast<int>(std::lround(layout_.preview.y * s)),
      round_fb(layout_.preview.w), round_fb(layout_.preview.h));
}

void UiManager::apply_region_bounds() {
  if (auto* toolbar = toolbar_panel()) {
    toolbar->set_bounds(layout_.toolbar);
  }
  if (auto* settings = settings_panel()) {
    settings->set_bounds(layout_.settings);
  }
  if (preview_hit_ != nullptr) {
    preview_hit_->set_bounds(layout_.preview);
  }
  if (edit_panel_ != nullptr) {
    edit_panel_->set_bounds(layout_.edit);
  }
  if (status_bar_ != nullptr) {
    status_bar_->set_bounds(layout_.status);
  }
  // Modal dialogs cover the whole window (later siblings win reverse hit-test).
  if (width_slots_dialog_ != nullptr) {
    width_slots_dialog_->set_bounds(root_.bounds());
  }
  if (curve_templates_dialog_ != nullptr) {
    curve_templates_dialog_->set_bounds(root_.bounds());
  }
  if (export_choice_dialog_ != nullptr) {
    export_choice_dialog_->set_bounds(root_.bounds());
  }
  if (chart_add_dialog_ != nullptr) {
    chart_add_dialog_->set_bounds(root_.bounds());
  }
  if (unsaved_changes_dialog_ != nullptr) {
    unsaved_changes_dialog_->set_bounds(root_.bounds());
  }
}

void UiManager::update(float delta_seconds, const std::vector<wds::interaction::InputEvent>& events) {
  using clock = std::chrono::steady_clock;
  const auto phase_us = [](clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t0).count();
  };

  // Native file panels must not open inside a button click handler — defer from the
  // unsaved-changes dialog to the next frame (after glfwPollEvents has fully settled).
  auto t0 = clock::now();
  flush_pending_after_save_prompt();
  last_update_flush_us_ = phase_us(t0);

  t0 = clock::now();
  apply_region_bounds();
  last_update_bounds_us_ = phase_us(t0);

  t0 = clock::now();
  root_.layout(root_.bounds());
  last_update_layout_us_ = phase_us(t0);

  t0 = clock::now();
  if (auto* edit = edit_panel()) {
    // Edit visible window for preview lead-in time mapping (not CHART_DELAY_MS).
    session_->engine().set_preview_lead_in_visible_ms(edit->viewport().visible_ms());
    double sync_ms = session_->engine().timeline_us() / 1000.0;
    if (chart_preview_ != nullptr &&
        session_->engine().playback_state() == wds::common::PlaybackState::Playing) {
      // Match preview present clock (committed + one display-frame lead).
      sync_ms += static_cast<double>(chart_preview_->display_frame_lead_us()) / 1000.0;
    }
    edit->sync_to_timeline_ms(sync_ms);
  }
  last_update_sync_us_ = phase_us(t0);

  t0 = clock::now();
  root_.process_frame(delta_seconds, events, &shortcuts_);
  last_update_process_us_ = phase_us(t0);
  flush_pending_ui_config();
}

void UiManager::paint(wds::interaction::UiPainter& painter) const { root_.paint(painter); }

void UiManager::prepare_painter(wds::interaction::UiPainter& painter) const {
  if (chart_preview_ != nullptr) {
    painter.set_soft_disk(chart_preview_->preview().skin().soft_disk);
  }
}

const wds::renderer::DrawBatch& UiManager::build_ui_batch(wds::renderer::TextureId solid_texture,
                                                          int fb_w, int fb_h,
                                                          const wds::renderer::ScreenBounds& screen) {
  ui_batch_.clear();
  if (auto* edit = edit_panel()) {
    // Viewport already synced in update() after transport tick; only re-snap
    // placement ghosts to the stationary pointer under the current viewport.
    edit->resync_pointer_overlays();
  }
  wds::interaction::UiPainter painter;
  prepare_painter(painter);
  paint(painter);
  wds::interaction::UiPainter overlay;
  if (const auto* edit = edit_panel()) {
    // Selection highlight after skinned notes (depthWrite off → draw order).
    prepare_painter(overlay);
    edit->paint_overlays(overlay);
  }
  // Upload after paints packed on-demand glyphs; flush resolves font TextureId live.
  chart_preview_->sync_ui_font_texture();
  painter.flush_to(ui_batch_, solid_texture, fb_w, fb_h, screen);
  if (const auto* edit = edit_panel()) {
    edit->append_skin_batch(ui_batch_, chart_preview_->preview().skin(), fb_w, fb_h, screen,
                            chart_preview_->preview().config().stage_opacity);
    overlay.flush_to(ui_batch_, solid_texture, fb_w, fb_h, screen);
  }
  return ui_batch_;
}

const wds::renderer::DrawBatch& UiManager::build_popup_batch(
    wds::renderer::TextureId solid_texture, int fb_w, int fb_h,
    const wds::renderer::ScreenBounds& screen) {
  popup_batch_.clear();
  wds::interaction::UiPainter painter;
  prepare_painter(painter);
  root_.paint_popup_layers(painter);
  if (painter.rects().empty() && painter.front_rects().empty() && painter.sprites().empty()) {
    return popup_batch_;
  }
  painter.flush_to(popup_batch_, solid_texture, fb_w, fb_h, screen);
  return popup_batch_;
}

const wds::renderer::DrawBatch& UiManager::build_modal_batch(
    wds::renderer::TextureId solid_texture, int fb_w, int fb_h,
    const wds::renderer::ScreenBounds& screen) {
  modal_batch_.clear();
  if (!has_modal_popup()) {
    return modal_batch_;
  }
  wds::interaction::UiPainter modal;
  prepare_painter(modal);
  const auto* edit = edit_panel();
  if (edit != nullptr && edit->has_modal_popup()) {
    edit->paint_popups(modal);
  }
  if (width_slots_dialog_ != nullptr && width_slots_dialog_->is_open()) {
    width_slots_dialog_->paint_modal(modal);
  }
  if (curve_templates_dialog_ != nullptr && curve_templates_dialog_->is_open()) {
    curve_templates_dialog_->paint_modal(modal);
  }
  if (export_choice_dialog_ != nullptr && export_choice_dialog_->is_open()) {
    export_choice_dialog_->paint_modal(modal);
  }
  if (chart_add_dialog_ != nullptr && chart_add_dialog_->is_open()) {
    chart_add_dialog_->paint_modal(modal);
  }
  if (unsaved_changes_dialog_ != nullptr && unsaved_changes_dialog_->is_open()) {
    unsaved_changes_dialog_->paint_modal(modal);
  }
  if (!modal.rects().empty() || !modal.front_rects().empty() || !modal.sprites().empty()) {
    modal.flush_to(modal_batch_, solid_texture, fb_w, fb_h, screen);
  }
  return modal_batch_;
}

const wds::renderer::DrawBatch& UiManager::build_modal_chrome_batch(
    wds::renderer::TextureId solid_texture, int fb_w, int fb_h,
    const wds::renderer::ScreenBounds& screen) {
  chrome_batch_.clear();
  const auto* edit = edit_panel();
  const bool edit_chrome = edit != nullptr && edit->has_modal_popup();
  const bool export_chrome =
      export_choice_dialog_ != nullptr && export_choice_dialog_->is_open();
  const bool settings_chrome =
      width_slots_dialog_ != nullptr && width_slots_dialog_->is_open();
  if (!edit_chrome && !export_chrome && !settings_chrome) {
    return chrome_batch_;
  }
  wds::interaction::UiPainter chrome;
  prepare_painter(chrome);
  if (edit_chrome) {
    edit->paint_popup_chrome(chrome);
  }
  // Separate pass after modal glyphs: UiPainter emits all solids then all fonts
  // per batch, so in-batch menu panels cannot cover later button text sprites.
  if (export_chrome) {
    export_choice_dialog_->paint_dropdown(chrome);
  }
  if (settings_chrome) {
    width_slots_dialog_->paint_dropdown(chrome);
  }
  if (!chrome.rects().empty() || !chrome.front_rects().empty() || !chrome.sprites().empty()) {
    chart_preview_->sync_ui_font_texture();
    chrome.flush_to(chrome_batch_, solid_texture, fb_w, fb_h, screen);
  }
  return chrome_batch_;
}

const wds::renderer::DrawBatch& UiManager::build_post_overlay_batch(
    wds::renderer::TextureId solid_texture, int fb_w, int fb_h,
    const wds::renderer::ScreenBounds& screen) {
  // Clear every reused batch at the start of the frame. UiPainter::flush_to and
  // DrawBatch::append_from only append; clearing only when a layer is empty left
  // prior-frame popup/modal verts in place for as long as any menu stayed open.
  post_overlay_batch_.clear();
  modal_batch_.clear();
  popup_batch_.clear();

  // Paint all post-overlay layers first, then one atlas upload before any flush
  // so mid-frame destroy cannot invalidate an earlier DrawBatch TextureId.
  wds::interaction::UiPainter status;
  bool have_status = false;
  if (status_bar_ != nullptr) {
    prepare_painter(status);
    status_bar_->paint_overlay(status);
    have_status =
        !status.rects().empty() || !status.front_rects().empty() || !status.sprites().empty();
  }

  wds::interaction::UiPainter modal;
  bool have_modal = false;
  if (has_modal_popup()) {
    prepare_painter(modal);
    const auto* edit = edit_panel();
    if (edit != nullptr && edit->has_modal_popup()) {
      edit->paint_popups(modal);
    }
    if (width_slots_dialog_ != nullptr && width_slots_dialog_->is_open()) {
      width_slots_dialog_->paint_modal(modal);
    }
    if (curve_templates_dialog_ != nullptr && curve_templates_dialog_->is_open()) {
      curve_templates_dialog_->paint_modal(modal);
    }
    if (export_choice_dialog_ != nullptr && export_choice_dialog_->is_open()) {
      export_choice_dialog_->paint_modal(modal);
    }
    if (chart_add_dialog_ != nullptr && chart_add_dialog_->is_open()) {
      chart_add_dialog_->paint_modal(modal);
    }
    if (unsaved_changes_dialog_ != nullptr && unsaved_changes_dialog_->is_open()) {
      unsaved_changes_dialog_->paint_modal(modal);
    }
    have_modal =
        !modal.rects().empty() || !modal.front_rects().empty() || !modal.sprites().empty();
  }

  wds::interaction::UiPainter popup;
  prepare_painter(popup);
  root_.paint_popup_layers(popup);
  const bool have_popup =
      !popup.rects().empty() || !popup.front_rects().empty() || !popup.sprites().empty();

  if (have_status || have_modal || have_popup) {
    chart_preview_->sync_ui_font_texture();
  }
  // Flush straight into the cleared post batch (status → modal → popup). No
  // intermediate scratch+append_from: that path previously cleared scratch only
  // when a layer was absent, so open menus accumulated every frame.
  if (have_status) {
    status.flush_to(post_overlay_batch_, solid_texture, fb_w, fb_h, screen);
  }
  if (have_modal) {
    modal.flush_to(post_overlay_batch_, solid_texture, fb_w, fb_h, screen);
  }
  if (have_popup) {
    popup.flush_to(post_overlay_batch_, solid_texture, fb_w, fb_h, screen);
  }
  return post_overlay_batch_;
}

bool UiManager::has_blocking_modal_dialog() const noexcept {
  return (width_slots_dialog_ != nullptr && width_slots_dialog_->is_open()) ||
         (curve_templates_dialog_ != nullptr && curve_templates_dialog_->is_open()) ||
         (export_choice_dialog_ != nullptr && export_choice_dialog_->is_open()) ||
         (chart_add_dialog_ != nullptr && chart_add_dialog_->is_open()) ||
         (unsaved_changes_dialog_ != nullptr && unsaved_changes_dialog_->is_open());
}

bool UiManager::has_modal_popup() const noexcept {
  const auto* edit = edit_panel();
  const bool edit_modal = edit != nullptr && edit->has_modal_popup();
  return edit_modal || has_blocking_modal_dialog();
}

}  // namespace wds::ui
