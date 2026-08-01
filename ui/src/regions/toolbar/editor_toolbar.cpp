#include "wds/ui/regions/toolbar/editor_toolbar.hpp"

#include "wds/ui/editor_session.hpp"
#include "wds/ui/layout/editor_layout.hpp"
#include "wds/ui/native_file_dialog.hpp"
#include "wds/ui/regions/edit/chart_edit_panel.hpp"

#include <wds/core/chart_editor_engine.hpp>
#include <wds/core/edit_grid.hpp>

#include <wds/interaction/theme.hpp>
#include <wds/interaction/ui_painter.hpp>
#include <wds/interaction/validators.hpp>
#include <wds/interaction/widgets/button.hpp>
#include <wds/interaction/widgets/checkbox.hpp>
#include <wds/interaction/widgets/combo_box.hpp>
#include <wds/interaction/widgets/dropdown.hpp>
#include <wds/interaction/widgets/icon_button.hpp>
#include <wds/interaction/widgets/text_field.hpp>
#include <wds/common/log.hpp>
#include <wds/renderer/texture.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace wds::ui {
namespace fs = std::filesystem;

namespace {

using wds::interaction::FlickArrowMode;
using wds::interaction::IconButton;
using wds::interaction::NotePreviewStyle;

wds::interaction::Button* add_step_button(wds::interaction::Widget& parent, const char* label,
                                          std::function<void()> on_click) {
  auto button = std::make_unique<wds::interaction::Button>(label);
  button->on_click(std::move(on_click));
  auto* raw = button.get();
  parent.add_child(std::move(button));
  return raw;
}

wds::interaction::ComboBox* add_combo(wds::interaction::Widget& parent, std::vector<std::string> items,
                                      std::function<bool(const std::string&)> validator,
                                      std::function<void(const std::string&)> on_commit) {
  auto combo = std::make_unique<wds::interaction::ComboBox>();
  combo->set_items(std::move(items));
  combo->set_validator(std::move(validator));
  combo->on_commit(std::move(on_commit));
  combo->set_opens_upward(true);
  auto* raw = combo.get();
  parent.add_child(std::move(combo));
  return raw;
}

}  // namespace

EditorToolbar::EditorToolbar(EditorSession& session, ChartEditPanel& edit)
    : session_(session), edit_(edit) {
  using wds::interaction::Icon;

  const std::array actions = {
      std::pair{Icon::Open, Action::Open},         std::pair{Icon::Save, Action::Save},
      std::pair{Icon::Import, Action::Import},     std::pair{Icon::Export, Action::Export},
      std::pair{Icon::Settings, Action::Settings}, std::pair{Icon::Music, Action::Music},
      std::pair{Icon::Undo, Action::Undo},         std::pair{Icon::Redo, Action::Redo},
  };
  const std::array<const char*, 8> action_tips = {
      "打开工程", "保存工程", "导入谱面（只读）", "导出谱面",
      "编辑器设置", "导入音乐", "撤销", "重做"};
  for (std::size_t i = 0; i < actions.size(); ++i) {
    auto button = std::make_unique<IconButton>(actions[i].first);
    button->set_tooltip(action_tips[i]);
    button->on_click([this, action = actions[i].second] { run(action); });
    action_buttons_[i] = button.get();
    add_child(std::move(button));
  }

  {
    auto field = std::make_unique<wds::interaction::TextField>("0");
    field->set_text("0");
    field->set_validator([](const std::string& text) {
      return wds::interaction::parse_non_negative_int(text).has_value();
    });
    field->on_commit([this](const std::string& text) {
      if (auto v = wds::interaction::parse_non_negative_int(text)) {
        session_.set_offset_ms(std::min<int64_t>(*v, 60000));
      }
      sync_numeric_fields();
    });
    delay_field_ = field.get();
    add_child(std::move(field));
  }

  auto chart = std::make_unique<wds::interaction::Dropdown>();
  chart->set_opens_upward(true);
  chart->on_select([this](int index, const std::string&) {
    if (index >= 0 && static_cast<std::size_t>(index) < session_.chart_count())
      session_.switch_chart(static_cast<std::size_t>(index));
  });
  chart_dropdown_ = chart.get();
  add_child(std::move(chart));
  chart_add_button_ = add_step_button(*this, "+", [this] {
    if (session_.read_only()) return;
    if (on_chart_add_) {
      on_chart_add_();
    } else {
      session_.add_chart();
    }
  });

  const auto hectom_step = [](const EditorToolbar* /*self*/) { return 5; };
  tick_minus_ = add_step_button(*this, "-", [this, hectom_step] {
    auto grid = edit_.viewport().grid();
    grid.visible_hectoms = std::max(1, grid.visible_hectoms - hectom_step(this));
    edit_.set_grid(grid);
    sync_numeric_fields();
    notify_persist();
  });
  tick_combo_ = add_combo(
      *this, {"10", "15", "20", "25", "30", "35", "40", "80"},
      [](const std::string& text) {
        const auto v = wds::interaction::parse_positive_int(text);
        return v.has_value() && *v >= 1 && *v <= 1000;
      },
      [this](const std::string& text) {
        if (auto v = wds::interaction::parse_positive_int(text)) {
          auto grid = edit_.viewport().grid();
          grid.visible_hectoms = std::clamp(*v, 1, 1000);
          edit_.set_grid(grid);
        }
        sync_numeric_fields();
        notify_persist();
      });
  tick_plus_ = add_step_button(*this, "+", [this, hectom_step] {
    auto grid = edit_.viewport().grid();
    grid.visible_hectoms = std::min(1000, grid.visible_hectoms + hectom_step(this));
    edit_.set_grid(grid);
    sync_numeric_fields();
    notify_persist();
  });

  division_minus_ = add_step_button(*this, "-", [this] {
    auto grid = edit_.viewport().grid();
    grid.subdivisions_per_beat = std::max(1, grid.subdivisions_per_beat - 1);
    edit_.set_grid(grid);
    sync_numeric_fields();
  });
  division_combo_ =
      add_combo(*this, {"2", "3", "4", "6", "8", "12", "16"},
                [](const std::string& text) {
                  const auto v = wds::interaction::parse_positive_int(text);
                  return v.has_value() && *v >= 1 && *v <= 64;
                },
                [this](const std::string& text) {
                  if (auto v = wds::interaction::parse_positive_int(text)) {
                    auto grid = edit_.viewport().grid();
                    grid.subdivisions_per_beat = std::min(64, *v);
                    edit_.set_grid(grid);
                  }
                  sync_numeric_fields();
                });
  division_plus_ = add_step_button(*this, "+", [this] {
    auto grid = edit_.viewport().grid();
    grid.subdivisions_per_beat = std::min(64, grid.subdivisions_per_beat + 1);
    edit_.set_grid(grid);
    sync_numeric_fields();
  });

  auto pause_at_current = std::make_unique<wds::interaction::Checkbox>("停止播放后停在当前时间");
  pause_at_current->on_change([this](bool checked) {
    edit_.set_pause_at_current(checked);
    notify_persist();
  });
  pause_at_current_checkbox_ = pause_at_current.get();
  add_child(std::move(pause_at_current));

  auto split_follow = std::make_unique<wds::interaction::Checkbox>("音符默认对齐分割线轨道");
  split_follow->on_change([this](bool checked) {
    edit_.set_split_width_follow(checked);
    notify_persist();
  });
  split_width_checkbox_ = split_follow.get();
  add_child(std::move(split_follow));

  const std::array convert_actions = {
      Action::ConvertTap,        Action::ConvertCritical,   Action::ConvertHoldStart,
      Action::ConvertHold,       Action::ConvertFlickLeft,  Action::ConvertFlick,
      Action::ConvertFlickRight, Action::ConvertScratchHold};
  // Keep tip order matched to convert_actions / skin preview order.
  const std::array<const char*, 8> convert_tips = {
      "转换为Tap",        "转换为ExTap",       "转换为Hold Head", "转换为Hold",
      "转换为Left Flick", "转换为Flick",       "转换为Right Flick", "转换为Scratch Hold"};
  for (std::size_t i = 0; i < convert_actions.size(); ++i) {
    auto button = std::make_unique<IconButton>(Icon::None);
    button->set_tooltip(convert_tips[i]);
    button->on_click([this, action = convert_actions[i]] { run(action); });
    convert_buttons_[i] = button.get();
    add_child(std::move(button));
  }
}

EditorToolbar::~EditorToolbar() { release_gpu_resources(); }

void EditorToolbar::release_gpu_resources() {
  destroy_owned_icons();
  icons_raster_px_ = 0;
  icons_content_scale_ = 0.0f;
  vulkan_ = nullptr;
}

void EditorToolbar::destroy_owned_icons() {
  for (auto* button : action_buttons_) {
    if (button != nullptr) {
      static_cast<IconButton*>(button)->set_sprite({});
    }
  }
  if (vulkan_ != nullptr) {
    for (const auto id : owned_icon_ids_) {
      if (id != wds::renderer::kInvalidTextureId) vulkan_->destroy_texture(id);
    }
  }
  owned_icon_ids_.clear();
  // Do not clear icons_raster_px_ / scale / vulkan_ here — load_action_icons()
  // destroys old textures mid-reload and must keep the new size recorded.
}

void EditorToolbar::load_action_icons(wds::renderer::VulkanRenderer& vulkan,
                                      const std::string& icons_directory, int target_svg_px) {
  icons_directory_ = icons_directory;
  wds::renderer::VulkanRenderer* vk = &vulkan;
  // Build replacements first so a failed reload does not leave blank/white buttons.
  std::array<wds::renderer::TextureInfo, 8> next_sprites{};
  std::vector<wds::renderer::TextureId> next_ids;
  next_ids.reserve(8);

  namespace th = wds::interaction::theme;
  const float scale = std::max(th::ui_content_scale(), 0.01f);
  int svg_px = target_svg_px;
  if (svg_px <= 0) {
    // Bootstrap only: prefer real left-column width once bounds exist; else stage estimate.
    float icon_px = std::max(th::kMinIconPx, th::kToolbarIconSize);
    if (bounds_.w > 1.0f) {
      icon_px = std::max(icon_px, estimate_toolbar_icon_px(bounds_.w));
    } else if (vk->framebuffer_width() > 0) {
      const float logical_w = static_cast<float>(vk->framebuffer_width()) / scale;
      const float left_w = logical_w * (1.0f - EditorLayouter::kEditFrac);
      icon_px = std::max(icon_px, estimate_toolbar_icon_px(left_w));
    }
    // Physical texture: logical icon × DPI (create_texture_from_svg also SSAA×2).
    svg_px = std::max(128, static_cast<int>(std::lround(icon_px * scale)));
  } else {
    svg_px = std::max(128, svg_px);
  }

  if (!icons_directory_.empty()) {
    const fs::path dir(icons_directory_);
    // Matches icons/*.svg (import-audio replaces the old music.png).
    const std::array<const char*, 8> names = {"open",     "save",         "import", "export",
                                              "settings", "import-audio", "undo",   "redo"};
    for (std::size_t i = 0; i < names.size(); ++i) {
      const fs::path svg = dir / (std::string(names[i]) + ".svg");
      const fs::path png = dir / (std::string(names[i]) + ".png");
      wds::renderer::TextureInfo tex{};
      if (fs::is_regular_file(svg)) {
        tex = wds::renderer::create_texture_from_svg(*vk, svg.string(), svg_px);
      }
      if (!tex && fs::is_regular_file(png)) {
        tex = wds::renderer::create_texture_from_png(*vk, png.string());
      }
      // Tiny textures (e.g. 1×1 white) become solid squares when stretched — skip.
      if (tex && (tex.width <= 2 || tex.height <= 2)) {
        vk->destroy_texture(tex.id);
        tex = {};
      }
      if (!tex) {
        continue;
      }
      next_ids.push_back(tex.id);
      next_sprites[i] = tex;
    }
  }

  destroy_owned_icons();
  vulkan_ = vk;
  owned_icon_ids_ = std::move(next_ids);
  // Record after destroy — old destroy_owned_icons wiped these and forced every-frame reload.
  icons_raster_px_ = svg_px;
  icons_content_scale_ = scale;
  WDS_LOG("toolbar icons raster svg_px=%d scale=%.3f count=%zu\n", svg_px, scale,
          owned_icon_ids_.size());
  for (std::size_t i = 0; i < next_sprites.size(); ++i) {
    if (next_sprites[i]) {
      static_cast<IconButton*>(action_buttons_[i])->set_sprite(next_sprites[i]);
    }
  }
}

void EditorToolbar::ensure_action_icons_resolution(float left_w) {
  if (vulkan_ == nullptr || icons_directory_.empty() || left_w <= 1.0f) {
    return;
  }
  namespace th = wds::interaction::theme;
  const float scale = th::ui_content_scale();
  const float icon_px = estimate_toolbar_icon_px(left_w);
  const int want =
      std::max(128, static_cast<int>(std::lround(icon_px * scale)));
  const bool scale_changed = std::abs(scale - icons_content_scale_) > 0.04f;
  const bool needs_larger = want > icons_raster_px_ + 16;
  if (!scale_changed && !needs_larger) {
    return;
  }
  // Pass the exact want so load records icons_raster_px_ == want (no estimate mismatch).
  load_action_icons(*vulkan_, icons_directory_, want);
}

void EditorToolbar::set_skin(const wds::renderer::SkinCatalog* skin) {
  skin_ = skin;
  edit_.set_skin(skin);
  apply_convert_skins();
}

void EditorToolbar::apply_convert_skins() {
  if (skin_ == nullptr) return;
  struct Entry {
    NotePreviewStyle style;
    wds::renderer::TextureInfo left, middle, right, connection, arrow;
    FlickArrowMode flick;
  };
  const Entry entries[] = {
      {NotePreviewStyle::Flat, skin_->note_red_left, skin_->note_red_middle, skin_->note_red_right,
       {}, {}, FlickArrowMode::None},
      {NotePreviewStyle::Flat, skin_->note_yellow_left, skin_->note_yellow_middle,
       skin_->note_yellow_right, {}, {}, FlickArrowMode::None},
      {NotePreviewStyle::Flat, skin_->note_blue_left, skin_->note_blue_middle,
       skin_->note_blue_right, {}, {}, FlickArrowMode::None},
      {NotePreviewStyle::HoldBody, {}, {}, {}, skin_->hold_connection_blue, {},
       FlickArrowMode::None},
      {NotePreviewStyle::Flat, skin_->note_purple_left, skin_->note_purple_middle,
       skin_->note_purple_right, {}, skin_->scratch_arrow, FlickArrowMode::Left},
      {NotePreviewStyle::Flat, skin_->note_purple_left, skin_->note_purple_middle,
       skin_->note_purple_right, {}, skin_->scratch_arrow, FlickArrowMode::Both},
      {NotePreviewStyle::Flat, skin_->note_purple_left, skin_->note_purple_middle,
       skin_->note_purple_right, {}, skin_->scratch_arrow, FlickArrowMode::Right},
      {NotePreviewStyle::ScratchHoldBody, {}, {}, {}, skin_->hold_connection_purple, {},
       FlickArrowMode::None},
  };
  for (std::size_t i = 0; i < convert_buttons_.size(); ++i) {
    auto* button = static_cast<IconButton*>(convert_buttons_[i]);
    button->set_label({});
    button->set_note_preview(entries[i].style, entries[i].left, entries[i].middle,
                             entries[i].right, entries[i].connection, entries[i].arrow,
                             entries[i].flick);
  }
}

void EditorToolbar::run(Action action) {
  using wds::chart_editor::NoteType;
  auto& engine = session_.engine();
  switch (action) {
    case Action::Open:
      if (on_open_) {
        on_open_();
      } else if (auto path = native_file_dialog::open_file("打开 WDS 工程", {"wdsproject"})) {
        (void)session_.open_wdsproject(*path);
      }
      break;
    case Action::Save:
      if (session_.read_only()) break;
      if (session_.project_path().empty()) {
        if (auto path = native_file_dialog::save_file("保存 WDS 工程", "untitled.wdsproject",
                                                      {"wdsproject"}))
          session_.save_as(*path);
      } else {
        session_.save();
      }
      break;
    case Action::Import:
      if (on_import_) {
        on_import_();
      } else if (auto path = native_file_dialog::open_file("导入官方谱面", {"csv", "sus"})) {
        session_.import_official(*path);
      }
      break;
    case Action::Export:
      if (session_.read_only()) break;
      if (on_export_) {
        on_export_();
      } else if (auto path = native_file_dialog::save_file(
                     "导出官方谱面", session_.official_chart_filename(session_.active_chart_index()),
                     {"csv"})) {
        session_.export_official(*path);
      }
      break;
    case Action::Settings:
      if (on_settings_) on_settings_();
      break;
    case Action::Music:
      if (auto path = native_file_dialog::open_file("导入音乐", {"ogg", "wav"}))
        session_.import_music(*path);
      break;
    case Action::Undo:
      engine.undo();
      break;
    case Action::Redo:
      engine.redo();
      break;
    case Action::ConvertTap:
      edit_.convert_selected(NoteType::Normal);
      break;
    case Action::ConvertCritical:
      edit_.convert_selected(NoteType::Critical);
      break;
    case Action::ConvertHoldStart:
      edit_.convert_selected(NoteType::HoldStart);
      break;
    case Action::ConvertHold:
      edit_.convert_selected(NoteType::Hold);
      break;
    case Action::ConvertFlick:
      edit_.convert_selected(NoteType::Flick, 0);
      break;
    case Action::ConvertFlickLeft:
      edit_.convert_selected(NoteType::Flick, -1);
      break;
    case Action::ConvertFlickRight:
      edit_.convert_selected(NoteType::Flick, 1);
      break;
    case Action::ConvertScratchHold:
      edit_.convert_selected(NoteType::ScratchHold);
      break;
  }
}

void EditorToolbar::sync_numeric_fields() const {
  auto set_combo = [](wds::interaction::Widget* widget, const std::string& text) {
    auto* combo = static_cast<wds::interaction::ComboBox*>(widget);
    if (combo->visual_state() != wds::interaction::WidgetState::Focused) {
      combo->set_text(text);
    }
  };
  auto* delay = static_cast<wds::interaction::TextField*>(delay_field_);
  if (delay->visual_state() != wds::interaction::WidgetState::Focused) {
    delay->set_text(
        std::to_string(static_cast<int>(std::min<int64_t>(session_.offset_ms(), 60000))));
  }
  const auto grid = edit_.viewport().grid();
  set_combo(tick_combo_, std::to_string(grid.visible_hectoms));
  set_combo(division_combo_, std::to_string(grid.subdivisions_per_beat));
}

void EditorToolbar::sync_checkboxes() const {
  if (pause_at_current_checkbox_ != nullptr) {
    static_cast<wds::interaction::Checkbox*>(pause_at_current_checkbox_)
        ->set_checked(edit_.pause_at_current());
  }
  if (split_width_checkbox_ != nullptr) {
    static_cast<wds::interaction::Checkbox*>(split_width_checkbox_)
        ->set_checked(edit_.split_width_follow());
  }
}

void EditorToolbar::notify_persist() const {
  if (on_persist_) on_persist_();
}

void EditorToolbar::apply_config(const EditorUiConfig& cfg) {
  auto grid = edit_.viewport().grid();
  grid.visible_hectoms = std::clamp(cfg.visible_hectoms, 1, 1000);
  edit_.set_grid(grid);
  edit_.set_pause_at_current(cfg.pause_at_current);
  edit_.set_split_width_follow(cfg.split_width_follow);
  sync_numeric_fields();
  sync_checkboxes();
}

void EditorToolbar::capture_config(EditorUiConfig& cfg) const {
  cfg.visible_hectoms = edit_.viewport().grid().visible_hectoms;
  cfg.pause_at_current = edit_.pause_at_current();
  cfg.split_width_follow = edit_.split_width_follow();
}

void EditorToolbar::layout(const wds::interaction::Rect& parent_bounds) {
  const auto b = bounds_;
  namespace th = wds::interaction::theme;
  const float gap = th::kUiGap;
  const float pad = th::kUiPad;
  const float ctrl_h = th::kControlHeight;

  // Keep SVG rasters matched to live DPI + logical column width.
  ensure_action_icons_resolution(b.w);

  const float half_w = std::max(1.0f, (b.w - pad * 2.0f - gap) * 0.5f);
  const float icon = estimate_toolbar_icon_px(b.w);
  const float pitch = icon + gap;
  const float grid_h = icon * 2.0f + gap;
  const float icon_block_h = grid_h + gap;
  const float ctrl_y0 = icon_block_h;
  const float ctrl_h_avail = std::max(1.0f, b.h - ctrl_y0);

  // 3 rows: free height split evenly across top / gap / gap / bottom (4 slots).
  constexpr float kRows = 3.0f;
  constexpr float kSlots = kRows + 1.0f;
  const float free = std::max(0.0f, ctrl_h_avail - ctrl_h * kRows);
  const float slot = free / kSlots;
  const float y_a = ctrl_y0 + slot;
  const float y_b = y_a + ctrl_h + slot;
  const float y_c = y_b + ctrl_h + slot;

  const auto place_icon_grid = [&](float x0, std::array<wds::interaction::Widget*, 8>& buttons) {
    const float grid_w = icon * 4.0f + gap * 3.0f;
    const float ox = x0 + (half_w - grid_w) * 0.5f;
    const float oy = (icon_block_h - grid_h) * 0.5f;
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      const int col_i = static_cast<int>(i % 4);
      const int row = static_cast<int>(i / 4);
      buttons[i]->set_bounds({ox + static_cast<float>(col_i) * pitch,
                              oy + static_cast<float>(row) * pitch, icon, icon});
    }
  };
  place_icon_grid(pad, action_buttons_);
  place_icon_grid(pad + half_w + gap, convert_buttons_);

  // Two equal horizontal halves; each group's label+cluster (and checkbox) centers in its half.
  const float label_w = th::kLabelW4;
  const float step_w = th::kStepButtonW;
  float cluster_w = th::kControlClusterW;
  const float full_w = std::max(1.0f, b.w - pad * 2.0f);
  const float col_w = std::max(1.0f, (full_w - gap) * 0.5f);
  while (label_w + cluster_w > col_w && cluster_w > th::px(60.0f)) {
    cluster_w -= th::px(2.0f);
  }
  const float group_w = label_w + cluster_w;
  const float col0 = pad + std::max(0.0f, (col_w - group_w) * 0.5f);
  const float col1 = pad + col_w + gap + std::max(0.0f, (col_w - group_w) * 0.5f);
  const float field_in_step = std::max(th::px(32.0f), cluster_w - step_w * 2.0f - gap * 2.0f);
  const float field_in_chart = std::max(th::px(32.0f), cluster_w - step_w - gap);

  delay_field_->set_bounds({col0 + label_w, y_a, cluster_w, ctrl_h});
  chart_dropdown_->set_bounds({col1 + label_w, y_a, field_in_chart, ctrl_h});
  chart_add_button_->set_bounds({col1 + label_w + field_in_chart + gap, y_a, step_w, ctrl_h});

  const auto place_step = [&](float col, wds::interaction::Widget* minus,
                              wds::interaction::Widget* combo, wds::interaction::Widget* plus) {
    float px = col + label_w;
    minus->set_bounds({px, y_b, step_w, ctrl_h});
    px += step_w + gap;
    combo->set_bounds({px, y_b, field_in_step, ctrl_h});
    px += field_in_step + gap;
    plus->set_bounds({px, y_b, step_w, ctrl_h});
  };
  place_step(col0, tick_minus_, tick_combo_, tick_plus_);
  place_step(col1, division_minus_, division_combo_, division_plus_);

  // Each checkbox+caption centers in the same half as its input column.
  wds::interaction::UiPainter measure_painter;
  auto unit_w = [&](wds::interaction::Widget* w) {
    auto* cb = static_cast<wds::interaction::Checkbox*>(w);
    const float side = std::clamp(ctrl_h * 0.72f, th::px(9.0f), th::px(14.0f));
    return side + th::px(5.0f) + measure_painter.measure_text(cb->label(), th::kFontSizeMd).x +
           th::px(4.0f);
  };
  const float u0 = pause_at_current_checkbox_ ? unit_w(pause_at_current_checkbox_) : 0.0f;
  const float u1 = split_width_checkbox_ ? unit_w(split_width_checkbox_) : 0.0f;
  const float half0_x = pad;
  const float half1_x = pad + col_w + gap;
  if (pause_at_current_checkbox_ != nullptr) {
    pause_at_current_checkbox_->set_bounds(
        {half0_x + std::max(0.0f, (col_w - u0) * 0.5f), y_c, u0, ctrl_h});
  }
  if (split_width_checkbox_ != nullptr) {
    split_width_checkbox_->set_bounds(
        {half1_x + std::max(0.0f, (col_w - u1) * 0.5f), y_c, u1, ctrl_h});
  }

  Widget::layout(parent_bounds);
}

void EditorToolbar::paint(wds::interaction::UiPainter& painter) const {
  const auto b = absolute_bounds();
  namespace th = wds::interaction::theme;
  painter.fill_rect(b, th::kSurface);

  const float gap = th::kUiGap;
  const float pad = th::kUiPad;
  const float ctrl_h = th::kControlHeight;
  const float split_x = b.x + b.w * 0.5f;

  const float icon = estimate_toolbar_icon_px(b.w);
  const float grid_h = icon * 2.0f + gap;
  const float icon_block_h = grid_h + gap;
  const float ctrl_y0 = b.y + icon_block_h;
  const float ctrl_h_avail = std::max(1.0f, b.h - icon_block_h);
  constexpr float kRows = 3.0f;
  constexpr float kSlots = kRows + 1.0f;
  const float free = std::max(0.0f, ctrl_h_avail - ctrl_h * kRows);
  const float slot = free / kSlots;
  const float y_a = ctrl_y0 + slot;
  const float y_b = y_a + ctrl_h + slot;

  painter.fill_rect({split_x - 0.5f, b.y, 1.0f, icon_block_h}, th::kOutline);
  painter.fill_rect({b.x + pad, b.y + icon_block_h - 0.5f, std::max(1.0f, b.w - pad * 2.0f), 1.0f},
                    th::kOutline);

  const float label_w = th::kLabelW4;
  float cluster_w = th::kControlClusterW;
  const float full_w = std::max(1.0f, b.w - pad * 2.0f);
  const float col_w = std::max(1.0f, (full_w - gap) * 0.5f);
  while (label_w + cluster_w > col_w && cluster_w > th::px(60.0f)) {
    cluster_w -= th::px(2.0f);
  }
  const float group_w = label_w + cluster_w;
  const float col0 = b.x + pad + std::max(0.0f, (col_w - group_w) * 0.5f);
  const float col1 = b.x + pad + col_w + gap + std::max(0.0f, (col_w - group_w) * 0.5f);

  // Left-align tips in the label column so stacked labels share one left edge.
  painter.label({col0, y_a, label_w, ctrl_h}, "谱面延迟", th::kOnSurfaceMuted, 0.9f, false, 0.0f,
                true);
  painter.label({col1, y_a, label_w, ctrl_h}, "谱面选择", th::kOnSurfaceMuted, 0.9f, false, 0.0f,
                true);
  painter.label({col0, y_b, label_w, ctrl_h}, "可见范围", th::kOnSurfaceMuted, 0.9f, false, 0.0f,
                true);
  painter.label({col1, y_b, label_w, ctrl_h}, "拍内分割", th::kOnSurfaceMuted, 0.9f, false, 0.0f,
                true);

  const bool editable = !session_.read_only();
  action_buttons_[1]->set_enabled(editable);
  action_buttons_[3]->set_enabled(editable);
  delay_field_->set_enabled(session_.delay_editable());
  // Read-only pack imports may still switch among multiple charts.
  chart_dropdown_->set_enabled(editable || session_.chart_count() > 1);
  chart_add_button_->set_enabled(editable);
  for (auto* button : convert_buttons_) button->set_enabled(editable);

  sync_numeric_fields();
  auto* chart = static_cast<wds::interaction::Dropdown*>(chart_dropdown_);
  std::vector<std::string> charts;
  for (std::size_t i = 0; i < session_.chart_count(); ++i)
    charts.push_back("谱面 " + std::to_string(i + 1));
  if (charts.empty()) charts.push_back("谱面 1");
  chart->set_items(std::move(charts));
  chart->set_selected_index(static_cast<int>(session_.active_chart_index()));

  for (auto* button : action_buttons_) button->paint(painter);
  for (auto* button : convert_buttons_) button->paint(painter);
  for (auto* w : {tick_minus_, tick_plus_, division_minus_, division_plus_, chart_add_button_}) {
    w->paint(painter);
  }
  delay_field_->paint(painter);
  tick_combo_->paint(painter);
  division_combo_->paint(painter);
  chart_dropdown_->paint(painter);
  if (pause_at_current_checkbox_ != nullptr) pause_at_current_checkbox_->paint(painter);
  if (split_width_checkbox_ != nullptr) split_width_checkbox_->paint(painter);
}

}  // namespace wds::ui
