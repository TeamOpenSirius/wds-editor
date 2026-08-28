#include "wds/ui/regions/edit/chart_edit_panel.hpp"
#include "wds/ui/regions/settings/curve_templates_dialog.hpp"
#include "wds/ui/regions/toolbar/editor_toolbar.hpp"
#include "wds/ui/ui_manager.hpp"

#include <wds/core/chart_editor_engine.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/scratch_hold_curve.hpp>
#include <wds/interaction/editor_input.hpp>
#include <wds/interaction/events.hpp>
#include <wds/interaction/widgets/button.hpp>
#include <wds/interaction/widgets/dropdown.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* expr, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    ++g_failures;
  }
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b) check((a) == (b), #a " == " #b, __FILE__, __LINE__)

using wds::chart_editor::ChartEditorEngine;
using wds::chart_editor::EasingAlgorithm;
using wds::chart_editor::EasingDirection;
using wds::chart_editor::EditGridConfig;
using wds::chart_editor::MusicTiming;
using wds::chart_editor::NotationNote;
using wds::chart_editor::NoteType;
using wds::chart_editor::TimingPoint;
using wds::interaction::KeyCode;
using wds::interaction::KeyDownEvent;
using wds::interaction::KeyUpEvent;
using wds::interaction::Modifiers;
using wds::interaction::PointerButton;
using wds::interaction::PointerDownEvent;
using wds::interaction::PointerMoveEvent;
using wds::interaction::PointerUpEvent;
using wds::interaction::Vec2;
using wds::ui::ChartEditPanel;
using wds::ui::CurveFillSelection;
using wds::ui::CurveTemplate;
using wds::ui::UiManager;

Modifiers primary_mods() {
  Modifiers mods;
#ifdef __APPLE__
  mods.super = true;
#else
  mods.control = true;
#endif
  return mods;
}

Modifiers curve_mods() {
  Modifiers mods = primary_mods();
  mods.shift = true;
  return mods;
}

int count_type(const std::vector<NotationNote>& notes, NoteType type) {
  int n = 0;
  for (const auto& note : notes) {
    if (note.note_type == type) ++n;
  }
  return n;
}

bool notes_eq(const std::vector<NotationNote>& a, const std::vector<NotationNote>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].id != b[i].id || a[i].start_tick != b[i].start_tick || a[i].end_tick != b[i].end_tick ||
        a[i].note_type != b[i].note_type || a[i].lane != b[i].lane || a[i].width != b[i].width ||
        a[i].gimmick_type != b[i].gimmick_type || a[i].scratch_length != b[i].scratch_length) {
      return false;
    }
  }
  return true;
}

struct Harness {
  ChartEditorEngine engine;
  ChartEditPanel panel;

  Harness() : panel(engine) {
    panel.set_bounds({0.0f, 0.0f, 800.0f, 1000.0f});
    EditGridConfig grid;
    grid.ticks_per_quarter = 480;
    grid.visible_hectoms = 40;
    grid.subdivisions_per_beat = 4;
    grid.lane_count = 12;
    panel.set_grid(grid);
    panel.set_default_width(1);
    MusicTiming timing;
    timing.bpm = 120.0;
    timing.ticks_per_quarter = 480;
    timing.points = {TimingPoint{0, 120.0, 4, 4, true, true}};
    engine.document().set_timing(timing);
    panel.sync_to_timeline_ms(0.0);
    panel.on_pointer_move(PointerMoveEvent{{400.0f, 800.0f}, {}});
  }

  Vec2 at_tick_lane(int32_t tick, int32_t lane) const {
    const float x = panel.viewport().x_at(lane) + panel.viewport().lane_width(1) * 0.5f;
    const float y = panel.viewport().y_at(tick);
    return {x, y};
  }

  int32_t snapped_tick(int32_t tick, int32_t lane) const {
    return panel.viewport().tick_at(at_tick_lane(tick, lane).y);
  }

  bool enter_hold(bool scratch, int32_t start_tick, int32_t end_tick, int32_t lane) {
    const auto origin = at_tick_lane(start_tick, lane);
    const auto tail = at_tick_lane(end_tick, lane);
    const PointerButton button = scratch ? PointerButton::Right : PointerButton::Left;
    panel.on_pointer_down(PointerDownEvent{origin, button, {}});
    const float nudge = tail.y < origin.y ? -20.0f : 20.0f;
    panel.on_pointer_move(PointerMoveEvent{{origin.x, origin.y + nudge}, {}});
    panel.on_pointer_move(PointerMoveEvent{tail, {}});
    return true;
  }

  void press_curve() { panel.on_key_down(KeyDownEvent{KeyCode::Unknown, curve_mods(), false}); }
  void release_curve() { panel.on_key_up(KeyUpEvent{KeyCode::Unknown, {}}); }

  void commit_first_scratch_and_extend_to(int32_t mid_tick, int32_t end_tick, int32_t start_lane,
                                          int32_t end_lane) {
    enter_hold(true, 0, mid_tick, start_lane);
    panel.on_pointer_down(PointerDownEvent{at_tick_lane(mid_tick, start_lane), PointerButton::Left, {}});
    panel.on_pointer_move(PointerMoveEvent{at_tick_lane(end_tick, end_lane), {}});
  }
};

struct DocSnap {
  std::vector<NotationNote> notes;
  std::uint64_t generation = 0;
  bool dirty = false;
  bool can_undo = false;
  bool can_redo = false;
};

DocSnap snapshot_doc(const ChartEditorEngine& engine) {
  DocSnap snap;
  snap.notes = engine.document().notes();
  snap.generation = engine.document().content_generation();
  snap.dirty = engine.document().is_dirty();
  snap.can_undo = engine.history().can_undo();
  snap.can_redo = engine.history().can_redo();
  return snap;
}

bool snap_eq(const DocSnap& snap, const ChartEditorEngine& engine) {
  return notes_eq(engine.document().notes(), snap.notes) &&
         engine.document().content_generation() == snap.generation &&
         engine.document().is_dirty() == snap.dirty &&
         engine.history().can_undo() == snap.can_undo &&
         engine.history().can_redo() == snap.can_redo;
}

void click_button(wds::interaction::Button* button) {
  CHECK(button != nullptr);
  if (button == nullptr) return;
  const auto b = button->absolute_bounds();
  const float w = b.w > 1.0f ? b.w : 1.0f;
  const float h = b.h > 1.0f ? b.h : 1.0f;
  const Vec2 p{b.x + w * 0.5f, b.y + h * 0.5f};
  button->on_click(wds::interaction::ClickEvent{p, PointerButton::Left, {}, 1});
}

void test_mode_guard_ordinary_and_non_scratch() {
  {
    Harness h;
    const auto p = h.at_tick_lane(240, 3);
    h.panel.on_pointer_move(PointerMoveEvent{p, {}});
    h.press_curve();
    CHECK(!h.panel.curve_mode_active());
    CHECK(h.panel.curve_ghost_notes().empty());
  }
  {
    Harness h;
    h.enter_hold(false, 0, 480, 3);
    const auto before = h.engine.document().notes();
    h.press_curve();
    CHECK(!h.panel.curve_mode_active());
    CHECK(h.panel.curve_ghost_notes().empty());
    CHECK(notes_eq(h.engine.document().notes(), before));
  }
}

void test_hot_switch_ghosts_and_no_document_mutation() {
  Harness h;
  h.enter_hold(true, 0, 480, 2);
  const auto before = h.engine.document().notes();
  CHECK(!h.engine.history().can_undo());

  h.press_curve();
  CHECK(h.panel.curve_mode_active());
  const auto ghosts = h.panel.curve_ghost_notes();
  CHECK(!ghosts.empty());
  const int32_t mouse_end = h.snapped_tick(480, 2);
  int32_t last_end = -1;
  int bodies = 0;
  for (const auto& note : ghosts) {
    if (note.note_type != NoteType::ScratchHold) continue;
    ++bodies;
    last_end = std::max(last_end, note.end_tick);
    CHECK(note.end_tick <= mouse_end);
  }
  CHECK(bodies >= 1);
  CHECK_EQ(last_end, mouse_end);
  CHECK(notes_eq(h.engine.document().notes(), before));

  h.panel.on_key_down(KeyDownEvent{KeyCode::Unknown, curve_mods(), true});
  CHECK(h.panel.curve_mode_active());
  CHECK_EQ(static_cast<int>(h.panel.curve_ghost_notes().size()), static_cast<int>(ghosts.size()));
  CHECK(notes_eq(h.engine.document().notes(), before));

  h.release_curve();
  CHECK(!h.panel.curve_mode_active());
  CHECK(h.panel.curve_ghost_notes().empty());
  CHECK(notes_eq(h.engine.document().notes(), before));
  CHECK(!h.engine.history().can_undo());
}

void test_tail_at_pointer_one_and_multiple_segments() {
  Harness h;
  h.enter_hold(true, 0, 90, 2);
  h.press_curve();
  CHECK(h.panel.curve_mode_active());
  const int32_t short_end = h.snapped_tick(90, 2);
  int bodies = 0;
  int32_t last_end = -1;
  for (const auto& note : h.panel.curve_ghost_notes()) {
    if (note.note_type != NoteType::ScratchHold) continue;
    ++bodies;
    last_end = std::max(last_end, note.end_tick);
    CHECK(note.end_tick <= short_end);
  }
  CHECK_EQ(bodies, 1);
  CHECK_EQ(last_end, short_end);

  const auto distant_pos = h.at_tick_lane(480, 8);
  h.panel.on_pointer_move(PointerMoveEvent{distant_pos, curve_mods()});
  const int32_t far_end = h.panel.viewport().tick_at(distant_pos.y);
  bodies = 0;
  last_end = -1;
  for (const auto& note : h.panel.curve_ghost_notes()) {
    if (note.note_type != NoteType::ScratchHold) continue;
    ++bodies;
    last_end = std::max(last_end, note.end_tick);
    CHECK(note.end_tick <= far_end);
  }
  CHECK(bodies > 1);
  CHECK_EQ(last_end, far_end);
  CHECK(!h.engine.history().can_undo());
}

void test_left_click_and_right_up_commit_modifier_release_ordinary() {
  {
    Harness h;
    h.enter_hold(true, 0, 480, 2);
    h.press_curve();
    CHECK(h.panel.curve_mode_active());
    const int ghost_bodies = count_type(h.panel.curve_ghost_notes(), NoteType::ScratchHold);
    CHECK(ghost_bodies > 1);
    h.panel.on_pointer_down(PointerDownEvent{h.at_tick_lane(480, 2), PointerButton::Left, curve_mods()});
    CHECK(!h.panel.curve_mode_active());
    CHECK(h.panel.curve_ghost_notes().empty());
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHold), ghost_bodies);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHoldStart), 1);
    CHECK(h.engine.history().can_undo());
    CHECK(!h.engine.history().can_redo());
    const auto after = h.engine.document().notes();
    CHECK(h.engine.undo());
    CHECK(h.engine.document().notes().empty());
    CHECK(h.engine.redo());
    CHECK(notes_eq(h.engine.document().notes(), after));
  }
  {
    Harness h;
    h.enter_hold(true, 0, 480, 2);
    h.press_curve();
    CHECK(h.panel.curve_mode_active());
    const int ghost_bodies = count_type(h.panel.curve_ghost_notes(), NoteType::ScratchHold);
    CHECK(ghost_bodies > 1);
    h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(480, 2), PointerButton::Right, curve_mods()});
    CHECK(!h.panel.curve_mode_active());
    CHECK(h.panel.curve_ghost_notes().empty());
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHold), ghost_bodies);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHoldStart), 1);
    CHECK(h.engine.history().can_undo());
    CHECK(!h.engine.history().can_redo());
    const auto after = h.engine.document().notes();
    CHECK_EQ(count_type(after, NoteType::ScratchSound), 0);
    h.panel.on_pointer_down(PointerDownEvent{h.at_tick_lane(480, 2), PointerButton::Left, curve_mods()});
    CHECK(!h.panel.curve_mode_active());
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchSound), 0);
    CHECK(notes_eq(h.engine.document().notes(), after));
    CHECK(h.engine.undo());
    CHECK(h.engine.document().notes().empty());
    CHECK(h.engine.redo());
    CHECK(notes_eq(h.engine.document().notes(), after));
  }
  {
    Harness h;
    h.enter_hold(true, 0, 480, 2);
    h.press_curve();
    CHECK(h.panel.curve_mode_active());
    h.release_curve();
    CHECK(!h.panel.curve_mode_active());
    h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(480, 2), PointerButton::Right, {}});
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHold), 1);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHoldStart), 1);
  }
}

void expect_extend_curve_exit_after_move_is_pure(Harness& h, bool right_commit) {
  h.commit_first_scratch_and_extend_to(480, 960, 2, 6);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHold), 1);
  const auto before_press = snapshot_doc(h.engine);
  CHECK(before_press.can_undo);

  h.press_curve();
  CHECK(h.panel.curve_mode_active());
  CHECK(!h.panel.curve_ghost_notes().empty());
  CHECK(snap_eq(before_press, h.engine));

  const auto moved = h.at_tick_lane(960, 8);
  h.panel.on_pointer_move(PointerMoveEvent{moved, curve_mods()});
  CHECK(h.panel.curve_mode_active());
  CHECK(snap_eq(before_press, h.engine));

  if (right_commit) {
    const int ghost_bodies = count_type(h.panel.curve_ghost_notes(), NoteType::ScratchHold);
    CHECK(ghost_bodies > 1);
    h.panel.on_pointer_up(PointerUpEvent{moved, PointerButton::Right, curve_mods()});
    CHECK(!h.panel.curve_mode_active());
    CHECK(h.panel.curve_ghost_notes().empty());
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHold), ghost_bodies);
    CHECK(h.engine.history().can_undo());
    CHECK(!snap_eq(before_press, h.engine));
    const auto after = snapshot_doc(h.engine);
    CHECK(!after.can_redo);
    CHECK(h.engine.undo());
    CHECK(!notes_eq(h.engine.document().notes(), after.notes));
    CHECK(h.engine.history().can_redo());
    CHECK(h.engine.redo());
    CHECK(notes_eq(h.engine.document().notes(), after.notes));
    return;
  }

  h.release_curve();
  CHECK(!h.panel.curve_mode_active());
  CHECK(h.panel.curve_ghost_notes().empty());
  CHECK(notes_eq(h.engine.document().notes(), before_press.notes));
  CHECK_EQ(h.engine.document().content_generation(), before_press.generation);
  CHECK_EQ(h.engine.document().is_dirty(), before_press.dirty);
  CHECK_EQ(h.engine.history().can_undo(), before_press.can_undo);
  CHECK_EQ(h.engine.history().can_redo(), before_press.can_redo);
  CHECK(snap_eq(before_press, h.engine));

  h.panel.on_pointer_move(PointerMoveEvent{h.at_tick_lane(960, 4), {}});
  CHECK(!notes_eq(h.engine.document().notes(), before_press.notes));
  CHECK(h.engine.document().content_generation() != before_press.generation);
  CHECK_EQ(h.engine.history().can_undo(), before_press.can_undo);
  CHECK_EQ(h.engine.history().can_redo(), before_press.can_redo);
  CHECK(!snap_eq(before_press, h.engine));
}

void test_extend_hot_switch_is_pure_preview() {
  {
    Harness h;
    expect_extend_curve_exit_after_move_is_pure(h, false);
  }
  {
    Harness h;
    expect_extend_curve_exit_after_move_is_pure(h, true);
  }
  {
    Harness h;
    h.commit_first_scratch_and_extend_to(480, 960, 2, 6);
    const auto before_press = snapshot_doc(h.engine);
    h.press_curve();
    CHECK(snap_eq(before_press, h.engine));
    const int ghost_bodies = count_type(h.panel.curve_ghost_notes(), NoteType::ScratchHold);
    CHECK(ghost_bodies > 1);
    h.panel.on_pointer_up(
        PointerUpEvent{h.at_tick_lane(960, 6), PointerButton::Right, curve_mods()});
    CHECK(!h.panel.curve_mode_active());
    CHECK(h.panel.curve_ghost_notes().empty());
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHold), ghost_bodies);
    CHECK(h.engine.history().can_undo());
    CHECK(!snap_eq(before_press, h.engine));
  }
  {
    Harness h;
    h.commit_first_scratch_and_extend_to(480, 960, 2, 6);
    const auto before_press = snapshot_doc(h.engine);
    h.press_curve();
    CHECK(snap_eq(before_press, h.engine));
    h.release_curve();
    CHECK(!h.panel.curve_mode_active());
    CHECK(h.panel.curve_ghost_notes().empty());
    CHECK(snap_eq(before_press, h.engine));
  }
}

void test_extend_curve_confirm_undo_redo_and_eighths() {
  Harness h;
  auto grid = h.panel.viewport().grid();
  grid.subdivisions_per_beat = 1;
  h.panel.set_grid(grid);
  h.enter_hold(true, 0, 480, 2);
  h.panel.on_pointer_down(PointerDownEvent{h.at_tick_lane(480, 2), PointerButton::Left, {}});
  const auto after_first_commit = snapshot_doc(h.engine);
  CHECK(count_type(after_first_commit.notes, NoteType::HoldEighth) > 0);
  h.panel.on_pointer_move(PointerMoveEvent{h.at_tick_lane(960, 6), {}});
  const auto before_press = snapshot_doc(h.engine);

  h.press_curve();
  CHECK(h.panel.curve_mode_active());
  CHECK(snap_eq(before_press, h.engine));
  const int ghost_bodies = count_type(h.panel.curve_ghost_notes(), NoteType::ScratchHold);
  CHECK(ghost_bodies > 1);

  h.panel.on_pointer_down(
      PointerDownEvent{h.at_tick_lane(960, 6), PointerButton::Left, curve_mods()});
  CHECK(!h.panel.curve_mode_active());
  CHECK(h.panel.curve_ghost_notes().empty());
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHoldStart), 1);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHold), ghost_bodies);
  CHECK(count_type(h.engine.document().notes(), NoteType::HoldEighth) >
        count_type(after_first_commit.notes, NoteType::HoldEighth));
  const auto after = snapshot_doc(h.engine);
  CHECK(after.can_undo);
  CHECK(!after.can_redo);

  CHECK(h.engine.undo());
  CHECK(notes_eq(h.engine.document().notes(), after_first_commit.notes));
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldEighth),
           count_type(after_first_commit.notes, NoteType::HoldEighth));
  CHECK(h.engine.history().can_redo());

  CHECK(h.engine.redo());
  CHECK(notes_eq(h.engine.document().notes(), after.notes));
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldEighth),
           count_type(after.notes, NoteType::HoldEighth));
}

void test_new_chain_head_vs_extend_and_prev_cover() {
  Harness h;
  h.enter_hold(true, 0, 480, 2);
  h.panel.on_pointer_down(PointerDownEvent{h.at_tick_lane(480, 2), PointerButton::Left, {}});
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHold), 1);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHoldStart), 1);
  const auto distant_pos = h.at_tick_lane(960, 6);
  h.panel.on_pointer_move(PointerMoveEvent{distant_pos, {}});
  h.press_curve();
  CHECK(h.panel.curve_mode_active());
  const auto before_curve = h.engine.document().notes();
  int32_t prev_id = -1;
  for (const auto& note : before_curve) {
    if (note.note_type == NoteType::ScratchHold) prev_id = note.id;
  }
  CHECK(prev_id >= 0);
  const int ghost_bodies = count_type(h.panel.curve_ghost_notes(), NoteType::ScratchHold);
  h.panel.on_pointer_down(PointerDownEvent{distant_pos, PointerButton::Left, curve_mods()});
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHoldStart), 1);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHold), ghost_bodies);
  const auto prev = h.engine.document().find_note(prev_id);
  CHECK(prev.has_value());
}

void test_template_direction_change_and_missing_linear() {
  Harness h;
  h.enter_hold(true, 0, 480, 2);
  h.panel.on_pointer_move(PointerMoveEvent{h.at_tick_lane(480, 8), {}});
  CurveFillSelection linear;
  linear.easing.algorithm = EasingAlgorithm::Linear;
  linear.easing.direction = EasingDirection::In;
  h.panel.set_curve_fill_selection(linear);
  h.press_curve();
  CHECK(h.panel.curve_mode_active());
  const auto linear_ghosts = h.panel.curve_ghost_notes();
  CHECK(!linear_ghosts.empty());

  CurveFillSelection poly;
  poly.template_id = 3;
  poly.easing.algorithm = EasingAlgorithm::Poly;
  poly.easing.direction = EasingDirection::In;
  poly.easing.parameter = 1.0;
  h.panel.set_curve_fill_selection(poly);
  const auto poly_ghosts = h.panel.curve_ghost_notes();
  CHECK(!poly_ghosts.empty());
  bool lanes_differ = linear_ghosts.size() != poly_ghosts.size();
  const size_t n = std::min(linear_ghosts.size(), poly_ghosts.size());
  for (size_t i = 0; i < n; ++i) {
    if (linear_ghosts[i].lane != poly_ghosts[i].lane) lanes_differ = true;
  }
  CHECK(lanes_differ);

  CurveFillSelection missing;
  missing.template_id = 0;
  missing.easing.algorithm = EasingAlgorithm::Linear;
  missing.easing.direction = EasingDirection::Out;
  h.panel.set_curve_fill_selection(missing);
  CHECK(h.panel.curve_fill_selection().easing.algorithm == EasingAlgorithm::Linear);
  const auto restored = h.panel.curve_ghost_notes();
  CHECK(!restored.empty());
}

void test_ui_manager_pushes_selection() {
  UiManager ui;
  ui.resize(1600, 900, 1600, 900);
  auto* edit = ui.edit_panel();
  auto* toolbar = ui.toolbar_panel();
  CHECK(edit != nullptr);
  CHECK(toolbar != nullptr);
  CHECK(edit->curve_fill_selection().easing.algorithm == EasingAlgorithm::Linear);
  toolbar->layout(toolbar->bounds());

  CurveTemplate tmpl;
  tmpl.id = 11;
  tmpl.name = "poly";
  tmpl.algorithm = EasingAlgorithm::Poly;
  tmpl.parameter = 2.0;
  ui.curve_template_state().templates = {tmpl};
  ui.curve_template_state().selected_id = 11;
  ui.curve_template_state().direction = EasingDirection::In;
  toolbar->refresh_curve_controls();

  wds::interaction::Button* out_btn = nullptr;
  for (const auto& child : toolbar->children()) {
    auto* button = dynamic_cast<wds::interaction::Button*>(child.get());
    if (button != nullptr && button->label() == "O") out_btn = button;
  }
  CHECK(out_btn != nullptr);
  click_button(out_btn);
  CHECK(edit->curve_fill_selection().easing.direction == EasingDirection::Out);

  ui.curve_template_state().selected_id = 11;
  ui.curve_template_state().direction = EasingDirection::InOut;
  ui.open_curve_templates_dialog();
  auto* dialog = ui.curve_templates_dialog();
  CHECK(dialog != nullptr);
  wds::interaction::Button* confirm = nullptr;
  if (dialog != nullptr) {
    dialog->layout(dialog->bounds());
    for (const auto& child : dialog->children()) {
      auto* button = dynamic_cast<wds::interaction::Button*>(child.get());
      if (button != nullptr && button->label() == "确认") confirm = button;
    }
  }
  CHECK(confirm != nullptr);
  click_button(confirm);
  CHECK(edit->curve_fill_selection().easing.algorithm == EasingAlgorithm::Poly);
  CHECK(edit->curve_fill_selection().easing.direction == EasingDirection::InOut);

  const auto path = std::filesystem::temp_directory_path() / "wds_task5_curve_config.yml";
  {
    std::ofstream out(path);
    out << "curve_template_count: 1\n"
        << "curve_template_0_id: 7\n"
        << "curve_template_0_name_hex: 61\n"
        << "curve_template_0_algorithm: sine\n"
        << "curve_template_0_parameter: 0\n"
        << "curve_selected_template_id: 7\n"
        << "curve_selected_direction: outin\n";
  }
  ui.set_config_path(path.string());
  ui.load_ui_config();
  CHECK(edit->curve_fill_selection().easing.algorithm == EasingAlgorithm::Sine);
  CHECK(edit->curve_fill_selection().easing.direction == EasingDirection::OutIn);

  {
    std::ofstream out(path);
    out << "curve_template_count: 1\n"
        << "curve_template_0_id: 7\n"
        << "curve_template_0_name_hex: 61\n"
        << "curve_template_0_algorithm: poly\n"
        << "curve_template_0_parameter: 1\n"
        << "curve_selected_template_id: 99\n"
        << "curve_selected_direction: out\n";
  }
  ui.load_ui_config();
  CHECK_EQ(edit->curve_fill_selection().template_id, static_cast<std::uint64_t>(0));
  CHECK(edit->curve_fill_selection().easing.algorithm == EasingAlgorithm::Linear);
  CHECK(edit->curve_fill_selection().easing.direction == EasingDirection::Out);
  std::filesystem::remove(path);
}

wds::interaction::Button* find_labeled_button(wds::interaction::Widget& host, const char* label) {
  for (const auto& child : host.children()) {
    auto* button = dynamic_cast<wds::interaction::Button*>(child.get());
    if (button != nullptr && button->label() == label) return button;
  }
  return nullptr;
}

wds::interaction::Dropdown* find_toolbar_curve_dropdown(wds::ui::EditorToolbar& toolbar) {
  wds::interaction::Dropdown* second = nullptr;
  int seen = 0;
  for (const auto& child : toolbar.children()) {
    auto* dropdown = dynamic_cast<wds::interaction::Dropdown*>(child.get());
    if (dropdown == nullptr) continue;
    ++seen;
    if (seen == 2) second = dropdown;
  }
  return second;
}

void test_dialog_confirm_keeps_toolbar_fill_and_delete_falls_back() {
  using wds::ui::kEmptyCurveTemplateLabel;

  UiManager ui;
  ui.resize(1600, 900, 1600, 900);
  auto* edit = ui.edit_panel();
  auto* toolbar = ui.toolbar_panel();
  auto* dialog = ui.curve_templates_dialog();
  CHECK(edit != nullptr);
  CHECK(toolbar != nullptr);
  CHECK(dialog != nullptr);

  CurveTemplate a;
  a.id = 1;
  a.name = "fill";
  a.algorithm = EasingAlgorithm::Poly;
  a.parameter = 2.0;
  CurveTemplate b;
  b.id = 2;
  b.name = "other";
  b.algorithm = EasingAlgorithm::Exp;
  b.parameter = 3.0;
  ui.curve_template_state().templates = {a, b};
  ui.curve_template_state().selected_id = 1;
  ui.curve_template_state().direction = EasingDirection::Out;
  toolbar->refresh_curve_controls();
  toolbar->layout(toolbar->bounds());

  ui.open_curve_templates_dialog();
  dialog->layout(dialog->bounds());
  CHECK(dialog->is_open());
  CHECK_EQ(dialog->session().selected_id(), static_cast<std::uint64_t>(1));

  auto* del1 = dynamic_cast<wds::interaction::Button*>(dialog->children()[1].get());
  CHECK(del1 != nullptr);
  CHECK(del1->visible());
  const auto row = del1->bounds();
  dialog->on_click(wds::interaction::ClickEvent{
      {row.x - 24.0f, row.y + row.h * 0.5f}, PointerButton::Left, {}, 1});
  CHECK_EQ(dialog->session().selected_id(), static_cast<std::uint64_t>(2));
  CHECK_EQ(ui.curve_template_state().selected_id, static_cast<std::uint64_t>(1));

  click_button(find_labeled_button(*dialog, "确认"));
  CHECK(!dialog->is_open());
  CHECK_EQ(ui.curve_template_state().selected_id, static_cast<std::uint64_t>(1));
  CHECK(ui.curve_template_state().direction == EasingDirection::Out);
  CHECK(edit->curve_fill_selection().template_id == 1);
  CHECK(edit->curve_fill_selection().easing.algorithm == EasingAlgorithm::Poly);
  auto* dropdown = find_toolbar_curve_dropdown(*toolbar);
  CHECK(dropdown != nullptr);
  CHECK_EQ(dropdown->selected_index(), 0);

  ui.open_curve_templates_dialog();
  dialog->layout(dialog->bounds());
  auto* del0 = dynamic_cast<wds::interaction::Button*>(dialog->children()[0].get());
  CHECK(del0 != nullptr);
  click_button(del0);
  CHECK_EQ(dialog->session().templates().size(), static_cast<std::size_t>(1));
  click_button(find_labeled_button(*dialog, "确认"));
  CHECK_EQ(ui.curve_template_state().selected_id, static_cast<std::uint64_t>(0));
  CHECK(ui.curve_template_state().direction == EasingDirection::Out);
  CHECK_EQ(edit->curve_fill_selection().template_id, static_cast<std::uint64_t>(0));
  CHECK(edit->curve_fill_selection().easing.algorithm == EasingAlgorithm::Linear);
  CHECK(edit->curve_fill_selection().easing.direction == EasingDirection::Out);
  dropdown = find_toolbar_curve_dropdown(*toolbar);
  CHECK(dropdown != nullptr);
  CHECK_EQ(dropdown->selected_index(), -1);
  CHECK_EQ(dropdown->selected_label(), std::string(kEmptyCurveTemplateLabel));
}

void test_exact_primary_wheel_updates_visible_range_and_playhead_grid() {
  Harness h;
  const auto pos = h.at_tick_lane(480, 2);
  const int32_t before = h.panel.viewport().grid().visible_hectoms;
  const float judgeline = h.panel.viewport().judgeline_y();
  h.panel.on_scroll(wds::interaction::ScrollEvent{pos, 0.0f, 1.0f, primary_mods()});
  CHECK(h.panel.viewport().grid().visible_hectoms != before);
  CHECK(std::fabs(h.panel.viewport().y_at(0) - judgeline) < 1.0f);
}

void test_curve_fill_wheel_does_not_change_visible_range() {
  Harness h;
  h.enter_hold(true, 0, 480, 2);
  h.press_curve();
  CHECK(h.panel.curve_mode_active());
  const auto pos = h.at_tick_lane(480, 2);
  const int32_t before = h.panel.viewport().grid().visible_hectoms;
  h.panel.on_scroll(wds::interaction::ScrollEvent{pos, 0.0f, 1.0f, curve_mods()});
  CHECK_EQ(h.panel.viewport().grid().visible_hectoms, before);
  CHECK(h.panel.curve_mode_active());
  const auto ghosts = h.panel.curve_ghost_notes();
  CHECK(!ghosts.empty());
}

void test_plain_primary_does_not_clear_hold_draft_during_draw() {
  Harness h;
  h.enter_hold(true, 0, 480, 2);
  CHECK(!h.panel.curve_mode_active());
  Modifiers primary;
#ifdef __APPLE__
  primary.super = true;
#else
  primary.control = true;
#endif
  h.panel.on_key_down(KeyDownEvent{KeyCode::Unknown, primary, false});
  h.panel.on_pointer_move(PointerMoveEvent{h.at_tick_lane(720, 2), primary});
  CHECK(!h.panel.curve_mode_active());
  CHECK(h.engine.document().notes().empty());
  CHECK(!h.engine.history().can_undo());
  h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(720, 2), PointerButton::Right, {}});
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHold), 1);
}

}  // namespace

int main() {
  test_mode_guard_ordinary_and_non_scratch();
  test_hot_switch_ghosts_and_no_document_mutation();
  test_tail_at_pointer_one_and_multiple_segments();
  test_left_click_and_right_up_commit_modifier_release_ordinary();
  test_extend_hot_switch_is_pure_preview();
  test_extend_curve_confirm_undo_redo_and_eighths();
  test_new_chain_head_vs_extend_and_prev_cover();
  test_template_direction_change_and_missing_linear();
  test_ui_manager_pushes_selection();
  test_dialog_confirm_keeps_toolbar_fill_and_delete_falls_back();
  test_exact_primary_wheel_updates_visible_range_and_playhead_grid();
  test_curve_fill_wheel_does_not_change_visible_range();
  test_plain_primary_does_not_clear_hold_draft_during_draw();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  return 0;
}
