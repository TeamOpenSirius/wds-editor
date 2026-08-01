#include "wds/ui/ui_manager.hpp"

#include "wds/ui/editor_session.hpp"
#include "wds/ui/editor_ui_config.hpp"
#include "wds/ui/native_file_dialog.hpp"
#include "wds/ui/regions/edit/chart_edit_panel.hpp"
#include "wds/ui/regions/preview/chart_preview_panel.hpp"
#include "wds/ui/regions/settings/chart_add_dialog.hpp"
#include "wds/ui/regions/settings/export_choice_dialog.hpp"
#include "wds/ui/regions/settings/preview_settings_panel.hpp"
#include "wds/ui/regions/settings/unsaved_changes_dialog.hpp"
#include "wds/ui/regions/settings/width_slots_dialog.hpp"
#include "wds/ui/regions/toolbar/editor_toolbar.hpp"

#include <wds/core/edit_grid.hpp>

#include <wds/interaction/editor_input.hpp>
#include <wds/interaction/theme.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

namespace wds::ui {

UiManager::UiManager() : chart_preview_(std::make_unique<ChartPreviewPanel>()) {
  session_ = std::make_unique<EditorSession>(*chart_preview_);

  // Child order: toolbar → settings → edit → dialogs (topmost).
  auto edit = std::make_unique<ChartEditPanel>(session_->engine());
  edit->set_seek_ms([this](int64_t ms) { chart_preview_->transport().request_seek_ms(ms); });
  auto toolbar = std::make_unique<EditorToolbar>(*session_, *edit);
  auto settings = std::make_unique<PreviewSettingsPanel>(*chart_preview_);
  auto width_dialog = std::make_unique<WidthSlotsDialog>();
  width_slots_dialog_ = width_dialog.get();
  auto export_dialog = std::make_unique<ExportChoiceDialog>();
  export_choice_dialog_ = export_dialog.get();
  auto chart_add = std::make_unique<ChartAddDialog>();
  chart_add_dialog_ = chart_add.get();
  auto unsaved = std::make_unique<UnsavedChangesDialog>();
  unsaved_changes_dialog_ = unsaved.get();

  toolbar->set_settings_handler([this] {
    if (width_slots_dialog_ == nullptr) return;
    EditorUiConfig cfg;
    if (auto* settings_panel = this->settings_panel()) settings_panel->capture_config(cfg);
    if (auto* toolbar_panel = this->toolbar_panel()) toolbar_panel->capture_config(cfg);
    cfg.width_slots = wds::interaction::width_slot_values_const();
    cfg.mute_hold_body_sfx = chart_preview_->preview().mute_hold_body_sfx();
    cfg.sus_auto_convert = session_->sus_auto_convert();
    cfg.invert_scroll_wheel = wds::interaction::invert_scroll_wheel();
    cfg.scroll_wheel_speed = wds::interaction::scroll_wheel_speed();
    cfg.shortcuts = wds::interaction::editor_shortcuts_snapshot();
    cfg.shortcuts_initialized = true;
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
      }
    });
  });
  toolbar->set_import_handler([this] {
    with_save_if_dirty([this] {
      if (auto path = native_file_dialog::open_file("导入官方谱面", {"csv", "sus"})) {
        session_->import_official(*path);
      }
    });
  });
  toolbar->set_chart_add_handler([this] {
    if (chart_add_dialog_ != nullptr) chart_add_dialog_->open();
  });
  chart_add->on_create_new([this] {
    if (!session_->read_only()) session_->add_chart();
  });
  chart_add->on_add_existing([this] {
    if (session_->read_only()) return;
    if (auto path = native_file_dialog::open_file("添加已有谱面", {"wdschart"})) {
      session_->add_chart_from_file(*path);
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
    if (session_->read_only()) return;
    if (format == ExportFormat::Sus) {
      if (auto dir = native_file_dialog::choose_directory("选择 SUS 导出目录")) {
        session_->export_sus_project(*dir);
      }
      return;
    }
    if (auto dir = native_file_dialog::choose_directory("选择导出目录")) {
      session_->export_official_project(*dir);
    }
  });
  export_dialog->on_export_chart([this](ExportFormat format) {
    if (session_->read_only()) return;
    if (format == ExportFormat::Sus) {
      const std::string default_name = session_->sus_chart_filename(session_->active_chart_index());
      if (auto path = native_file_dialog::save_file("导出当前谱面 (SUS)", default_name, {"sus"})) {
        session_->export_sus(*path);
      }
      return;
    }
    const std::string default_name =
        session_->official_chart_filename(session_->active_chart_index());
    if (auto path = native_file_dialog::save_file("导出当前谱面", default_name, {"csv"})) {
      session_->export_official(*path);
    }
  });
  root_.add_child(std::move(toolbar));
  root_.add_child(std::move(settings));
  root_.add_child(std::move(edit));
  root_.add_child(std::move(width_dialog));
  root_.add_child(std::move(export_dialog));
  root_.add_child(std::move(chart_add));
  root_.add_child(std::move(unsaved));

  const auto persist = [this] { save_ui_config(); };
  if (width_slots_dialog_ != nullptr) {
    width_slots_dialog_->set_on_applied([this, persist] {
      EditorUiConfig cfg;
      width_slots_dialog_->capture_config(cfg);
      session_->set_sus_auto_convert(cfg.sus_auto_convert);
      chart_preview_->preview().set_mute_hold_body_sfx(cfg.mute_hold_body_sfx);
      wds::interaction::set_invert_scroll_wheel(cfg.invert_scroll_wheel);
      wds::interaction::set_scroll_wheel_speed(cfg.scroll_wheel_speed);
      if (cfg.shortcuts_initialized) {
        wds::interaction::set_editor_shortcuts(cfg.shortcuts);
      }
      bind_editor_shortcuts();
      persist();
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
      if (width_slots_dialog_ != nullptr && width_slots_dialog_->is_open()) return;
      if (export_choice_dialog_ != nullptr && export_choice_dialog_->is_open()) return;
      if (chart_add_dialog_ != nullptr && chart_add_dialog_->is_open()) return;
      if (unsaved_changes_dialog_ != nullptr && unsaved_changes_dialog_->is_open()) return;
      if (auto* panel = edit_panel()) {
        panel->set_default_width(width_slot_values_const()[static_cast<std::size_t>(slot)]);
      }
    });
  }
  // Playback rate presets (default F1–F4 → 0.25x / 0.5x / 0.75x / 1x).
  for (int slot = 0; slot < 4; ++slot) {
    editor.bind(chord_playback_rate_slot(slot), [this, slot] {
      if (width_slots_dialog_ != nullptr && width_slots_dialog_->is_open()) return;
      if (export_choice_dialog_ != nullptr && export_choice_dialog_->is_open()) return;
      if (chart_add_dialog_ != nullptr && chart_add_dialog_->is_open()) return;
      if (unsaved_changes_dialog_ != nullptr && unsaved_changes_dialog_->is_open()) return;
      const auto rate = playback_rate_for_slot(slot);
      if (!rate) return;
      if (auto* settings = settings_panel()) settings->set_playback_rate(*rate);
    });
  }
}

UiManager::~UiManager() = default;

ChartEditPanel* UiManager::edit_panel() noexcept {
  const auto& children = root_.children();
  if (children.size() < 3) return nullptr;
  return static_cast<ChartEditPanel*>(children[2].get());
}

const ChartEditPanel* UiManager::edit_panel() const noexcept {
  const auto& children = root_.children();
  if (children.size() < 3) return nullptr;
  return static_cast<const ChartEditPanel*>(children[2].get());
}

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

WidthSlotsDialog* UiManager::width_slots_dialog() noexcept { return width_slots_dialog_; }

ExportChoiceDialog* UiManager::export_choice_dialog() noexcept { return export_choice_dialog_; }

ChartAddDialog* UiManager::chart_add_dialog() noexcept { return chart_add_dialog_; }

UnsavedChangesDialog* UiManager::unsaved_changes_dialog() noexcept {
  return unsaved_changes_dialog_;
}

bool UiManager::save_current_project() {
  if (session_->read_only()) return false;
  if (session_->project_path().empty()) {
    if (auto path = native_file_dialog::save_file("保存 WDS 工程", "untitled.wdsproject",
                                                  {"wdsproject"})) {
      return session_->save_as(*path);
    }
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
    if (session_->read_only() || !save_current_project()) {
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
  wds::interaction::set_invert_scroll_wheel(cfg.invert_scroll_wheel);
  wds::interaction::set_scroll_wheel_speed(cfg.scroll_wheel_speed);
  if (cfg.shortcuts_initialized) {
    wds::interaction::set_editor_shortcuts(cfg.shortcuts);
    bind_editor_shortcuts();
  }
  if (width_slots_dialog_ != nullptr) width_slots_dialog_->set_config(cfg);
  if (auto* settings = settings_panel()) settings->apply_config(cfg);
  if (auto* toolbar = toolbar_panel()) toolbar->apply_config(cfg);
}

void UiManager::save_ui_config() {
  if (config_path_.empty()) return;
  EditorUiConfig cfg;
  if (auto* settings = settings_panel()) settings->capture_config(cfg);
  if (auto* toolbar = toolbar_panel()) toolbar->capture_config(cfg);
  cfg.width_slots = wds::interaction::width_slot_values_const();
  cfg.mute_hold_body_sfx = chart_preview_->preview().mute_hold_body_sfx();
  cfg.sus_auto_convert = session_->sus_auto_convert();
  cfg.invert_scroll_wheel = wds::interaction::invert_scroll_wheel();
  cfg.scroll_wheel_speed = wds::interaction::scroll_wheel_speed();
  cfg.shortcuts = wds::interaction::editor_shortcuts_snapshot();
  cfg.shortcuts_initialized = true;
  save_editor_ui_config(config_path_, cfg);
}

void UiManager::resize(int logical_width, int logical_height, int framebuffer_width,
                       int framebuffer_height) {
  width_ = std::max(1, logical_width);
  height_ = std::max(1, logical_height);
  fb_width_ = std::max(1, framebuffer_width);
  fb_height_ = std::max(1, framebuffer_height);

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
}

void UiManager::apply_region_bounds() {
  const auto& children = root_.children();
  if (children.size() >= 3) {
    children[0]->set_bounds(layout_.toolbar);
    children[1]->set_bounds(layout_.settings);
    children[2]->set_bounds(layout_.edit);
  }
  // Modal dialogs cover the whole window.
  for (std::size_t i = 3; i < children.size(); ++i) {
    children[i]->set_bounds(root_.bounds());
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
    edit->sync_to_timeline_ms(session_->engine().timeline_us() / 1000.0);
  }
  last_update_sync_us_ = phase_us(t0);

  t0 = clock::now();
  root_.process_frame(delta_seconds, events, &shortcuts_);
  last_update_process_us_ = phase_us(t0);
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
    // Final scroll sample for this frame (post-transport tick), then re-snap
    // placement ghosts to the stationary pointer under the new viewport.
    edit->sync_to_timeline_ms(session_->engine().timeline_us() / 1000.0);
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
  // One upload after all paints may have packed on-demand glyphs.
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
  chart_preview_->sync_ui_font_texture();
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
    chart_preview_->sync_ui_font_texture();
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
  post_overlay_batch_.clear();
  // Modal scrim first, then dropdown popups — otherwise menus open inside a modal
  // (export format combo) are drawn under the 55% dim and look transparent.
  if (has_modal_popup()) {
    post_overlay_batch_.append_from(build_modal_batch(solid_texture, fb_w, fb_h, screen));
  }
  post_overlay_batch_.append_from(build_popup_batch(solid_texture, fb_w, fb_h, screen));
  return post_overlay_batch_;
}

bool UiManager::has_modal_popup() const noexcept {
  const auto* edit = edit_panel();
  const bool edit_modal = edit != nullptr && edit->has_modal_popup();
  const bool width_modal = width_slots_dialog_ != nullptr && width_slots_dialog_->is_open();
  const bool export_modal = export_choice_dialog_ != nullptr && export_choice_dialog_->is_open();
  const bool chart_add_modal = chart_add_dialog_ != nullptr && chart_add_dialog_->is_open();
  const bool unsaved_modal =
      unsaved_changes_dialog_ != nullptr && unsaved_changes_dialog_->is_open();
  return edit_modal || width_modal || export_modal || chart_add_modal || unsaved_modal;
}

}  // namespace wds::ui
