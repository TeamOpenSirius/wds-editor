#include "wds/ui/regions/edit/chart_edit_panel.hpp"
#include "wds/ui/regions/edit/edit_gutters.hpp"
#include "wds/ui/regions/settings/curve_templates_dialog.hpp"
#include "wds/ui/regions/toolbar/editor_toolbar.hpp"
#include "wds/ui/ui_manager.hpp"

#include <wds/core/chart_editor_engine.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/note_edit_ops.hpp>
#include <wds/core/scratch_hold_curve.hpp>
#include <wds/interaction/editor_input.hpp>
#include <wds/interaction/events.hpp>
#include <wds/interaction/ui_painter.hpp>
#include <wds/interaction/widgets/button.hpp>
#include <wds/interaction/widgets/dropdown.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
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
using wds::interaction::CursorKind;
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
  mods.control = true;
  return mods;
}

Modifiers option_mods() {
  Modifiers mods;
  mods.alt = true;
  return mods;
}

Modifiers curve_mods() {
  Modifiers mods = primary_mods();
  mods.shift = true;
  return mods;
}

Modifiers shift_mods() {
  Modifiers mods;
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

  bool enter_hold(bool scratch, int32_t start_tick, int32_t end_tick, int32_t lane,
                  Modifiers down_mods = {}) {
    const auto origin = at_tick_lane(start_tick, lane);
    const auto tail = at_tick_lane(end_tick, lane);
    const PointerButton button = scratch ? PointerButton::Right : PointerButton::Left;
    panel.on_pointer_down(PointerDownEvent{origin, button, down_mods});
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
}

void test_regular_hold_shift_right_places_star() {
  Harness h;
  h.enter_hold(false, 0, 480, 3);
  Modifiers shift_only;
  shift_only.shift = true;
  h.panel.on_pointer_down(PointerDownEvent{h.at_tick_lane(240, 3), PointerButton::Right, shift_only});
  h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(480, 3), PointerButton::Left, {}});
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 1);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Sound), 1);
}

void test_regular_hold_curve_right_up_commits_without_star() {
  Harness h;
  h.enter_hold(false, 0, 480, 3);
  h.press_curve();
  CHECK(h.panel.curve_mode_active());
  const int ghost_bodies = count_type(h.panel.curve_ghost_notes(), NoteType::Hold);
  CHECK(ghost_bodies > 1);

  h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(480, 3), PointerButton::Right, curve_mods()});
  CHECK(!h.panel.curve_mode_active());
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), ghost_bodies);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldStart), 1);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Sound), 0);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHold), 0);
}

void test_regular_hold_curve_right_down_commits_without_star() {
  Harness h;
  h.enter_hold(false, 0, 480, 3);
  h.press_curve();
  CHECK(h.panel.curve_mode_active());
  const int ghost_bodies = count_type(h.panel.curve_ghost_notes(), NoteType::Hold);
  CHECK(ghost_bodies > 1);

  h.panel.on_pointer_down(
      PointerDownEvent{h.at_tick_lane(480, 3), PointerButton::Right, curve_mods()});
  CHECK(!h.panel.curve_mode_active());
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), ghost_bodies);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldStart), 1);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Sound), 0);
}

void test_regular_hold_curve_chord_without_curve_does_not_steal_draw() {
  Harness h;
  h.enter_hold(false, 0, 480, 3);
  CHECK(!h.panel.curve_mode_active());
  h.panel.on_pointer_down(
      PointerDownEvent{h.at_tick_lane(240, 3), PointerButton::Right, curve_mods()});
  h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(480, 3), PointerButton::Left, {}});
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 1);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldStart), 1);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Sound), 0);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Flick), 0);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Normal), 0);
}

void test_regular_hold_curve_left_up_commits_ghost_segments() {
  Harness h;
  h.enter_hold(false, 0, 480, 3);
  h.press_curve();
  CHECK(h.panel.curve_mode_active());
  const int ghost_bodies = count_type(h.panel.curve_ghost_notes(), NoteType::Hold);
  CHECK(ghost_bodies > 1);

  h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(480, 3), PointerButton::Left, curve_mods()});
  CHECK(!h.panel.curve_mode_active());
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), ghost_bodies);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldStart), 1);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHold), 0);
}

void test_regular_hold_curve_fill_generates_hold_bodies() {
  Harness h;
  h.enter_hold(false, 0, 480, 3);
  const auto before = h.engine.document().notes();
  CHECK(before.empty());
  h.press_curve();
  CHECK(h.panel.curve_mode_active());
  const auto ghosts = h.panel.curve_ghost_notes();
  CHECK(!ghosts.empty());
  CHECK_EQ(count_type(ghosts, NoteType::ScratchHold), 0);
  CHECK(count_type(ghosts, NoteType::Hold) >= 1);
  CHECK(notes_eq(h.engine.document().notes(), before));

  const int ghost_bodies = count_type(ghosts, NoteType::Hold);
  h.panel.on_pointer_down(PointerDownEvent{h.at_tick_lane(480, 3), PointerButton::Left, curve_mods()});
  CHECK(!h.panel.curve_mode_active());
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), ghost_bodies);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldStart), 1);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHold), 0);

  std::vector<NotationNote> holds;
  for (const auto& note : h.engine.document().notes()) {
    if (note.note_type == NoteType::Hold) holds.push_back(note);
  }
  std::sort(holds.begin(), holds.end(),
            [](const NotationNote& a, const NotationNote& b) { return a.start_tick < b.start_tick; });
  for (size_t i = 0; i + 1 < holds.size(); ++i) {
    CHECK_EQ(holds[i].end_tick, holds[i + 1].start_tick);
    const auto [lo, hi] = wds::chart_editor::get_scratch_end_lane_range(holds[i]);
    const int32_t union_l = std::min(holds[i].lane, holds[i + 1].lane);
    const int32_t union_r = std::max(holds[i].end_lane(), holds[i + 1].end_lane());
    CHECK_EQ(lo, union_l);
    CHECK_EQ(hi, union_r);
    if (lo == holds[i].lane && hi == holds[i].end_lane()) {
      CHECK(holds[i].gimmick_type == wds::chart_editor::GimmickType::OneDirection);
      CHECK_EQ(holds[i].scratch_length, 0);
    } else {
      CHECK(holds[i].gimmick_type == wds::chart_editor::GimmickType::JumpScratch);
    }
  }
}

void test_regular_hold_right_click_chains_next_segment() {
  Harness h;
  h.enter_hold(false, 0, 240, 2);
  h.panel.on_pointer_down(PointerDownEvent{h.at_tick_lane(240, 2), PointerButton::Right, {}});
  h.panel.on_pointer_move(PointerMoveEvent{h.at_tick_lane(480, 5), {}});
  h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(480, 5), PointerButton::Left, {}});
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 2);
  std::vector<NotationNote> holds;
  for (const auto& note : h.engine.document().notes()) {
    if (note.note_type == NoteType::Hold) holds.push_back(note);
  }
  std::sort(holds.begin(), holds.end(),
            [](const NotationNote& a, const NotationNote& b) { return a.start_tick < b.start_tick; });
  CHECK_EQ(static_cast<int>(holds.size()), 2);
  CHECK_EQ(holds[0].end_tick, holds[1].start_tick);
  const auto next = wds::chart_editor::chained_next_scratch_hold(h.engine.document(), holds[0]);
  CHECK(next.has_value());
  if (next) CHECK_EQ(next->id, holds[1].id);
  CHECK(holds[0].gimmick_type == wds::chart_editor::GimmickType::JumpScratch ||
        holds[0].gimmick_type == wds::chart_editor::GimmickType::OneDirection);
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

void test_exact_option_wheel_updates_visible_range_and_playhead_grid() {
  Harness h;
  const auto pos = h.at_tick_lane(480, 2);
  const int32_t before = h.panel.viewport().grid().visible_hectoms;
  const float judgeline = h.panel.viewport().judgeline_y();
  h.panel.on_scroll(wds::interaction::ScrollEvent{pos, 0.0f, 1.0f, option_mods()});
  CHECK(h.panel.viewport().grid().visible_hectoms != before);
  CHECK(std::fabs(h.panel.viewport().y_at(0) - judgeline) < 1.0f);
}

void test_primary_wheel_does_not_change_visible_range() {
  Harness h;
  const auto pos = h.at_tick_lane(480, 2);
  const int32_t before = h.panel.viewport().grid().visible_hectoms;
  h.panel.on_scroll(wds::interaction::ScrollEvent{pos, 0.0f, 1.0f, primary_mods()});
  CHECK_EQ(h.panel.viewport().grid().visible_hectoms, before);
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

const NotationNote* find_note_id(const std::vector<NotationNote>& notes, int32_t id) {
  for (const auto& note : notes) {
    if (note.id == id) return &note;
  }
  return nullptr;
}

int32_t add_hold_body(Harness& h, NoteType type, int32_t start, int32_t end, int32_t lane,
                      int32_t width, int32_t cover_left = -1, int32_t cover_right = -1) {
  NotationNote body;
  body.note_type = type;
  body.start_tick = start;
  body.end_tick = end;
  body.lane = lane;
  body.width = width;
  if (type == NoteType::ScratchHold || cover_left >= 0 || cover_right >= 0) {
    body.gimmick_type = wds::chart_editor::GimmickType::JumpScratch;
    const int32_t left = cover_left >= 0 ? cover_left : lane;
    const int32_t right = cover_right >= 0 ? cover_right : lane + width - 1;
    wds::chart_editor::set_scratch_hold_end_lanes(body, left, right);
  }
  const int32_t id = h.engine.add_note(body);
  CHECK(id >= 0);
  return id;
}

Vec2 tail_x_at_end(const Harness& h, const NotationNote& note, float x) {
  return {x, h.panel.viewport().y_at(note.end_tick)};
}

float tail_span_right_x(const Harness& h, const NotationNote& note) {
  const auto range = wds::chart_editor::get_scratch_end_lane_range(note);
  const float x0 = h.panel.viewport().x_at(range.first);
  return x0 + h.panel.viewport().lane_width(range.second - range.first + 1);
}

Vec2 tail_outer_right(const Harness& h, const NotationNote& note) {
  return tail_x_at_end(h, note, tail_span_right_x(h, note) + 3.0f);
}

Vec2 tail_right_edge_on_note(const Harness& h, const NotationNote& note) {
  return tail_x_at_end(h, note, tail_span_right_x(h, note) - 1.0f);
}

Vec2 tail_center_on_note(const Harness& h, const NotationNote& note) {
  const auto range = wds::chart_editor::get_scratch_end_lane_range(note);
  const float x0 = h.panel.viewport().x_at(range.first);
  const float w = h.panel.viewport().lane_width(range.second - range.first + 1);
  return tail_x_at_end(h, note, x0 + w * 0.5f);
}

void test_shift_at_press_draws_gold_head_hold_for_both_families() {
  {
    Harness h;
    // Shift only on press; later moves have no Shift (release must not retint).
    h.enter_hold(false, 0, 480, 3, shift_mods());
    h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(480, 3), PointerButton::Left, {}});
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::CriticalHold), 1);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::CriticalHoldStart), 1);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 0);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldStart), 0);
  }
  {
    Harness h;
    h.enter_hold(true, 0, 480, 3, shift_mods());
    h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(480, 3), PointerButton::Right, {}});
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchCriticalHold), 1);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchCriticalHoldStart), 1);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHold), 0);
  }
}

void test_shift_after_press_does_not_make_gold_head() {
  Harness h;
  h.enter_hold(false, 0, 480, 3);
  h.panel.on_key_down(KeyDownEvent{KeyCode::Unknown, shift_mods(), false});
  h.panel.on_pointer_move(PointerMoveEvent{h.at_tick_lane(480, 3), shift_mods()});
  h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(480, 3), PointerButton::Left, shift_mods()});
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 1);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldStart), 1);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::CriticalHold), 0);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::CriticalHoldStart), 0);
}

void test_shift_gold_head_still_places_star() {
  Harness h;
  h.enter_hold(false, 0, 480, 3, shift_mods());
  h.panel.on_pointer_down(
      PointerDownEvent{h.at_tick_lane(240, 3), PointerButton::Right, shift_mods()});
  h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(480, 3), PointerButton::Left, {}});
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::CriticalHold), 1);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::CriticalHoldStart), 1);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Sound), 1);
}

void test_shift_does_not_break_curve_fill_or_chain_second_segment() {
  {
    Harness h;
    h.enter_hold(false, 0, 480, 3, shift_mods());
    h.press_curve();
    CHECK(h.panel.curve_mode_active());
    h.release_curve();
    h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(480, 3), PointerButton::Left, {}});
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::CriticalHold), 1);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::CriticalHoldStart), 1);
  }
  {
    Harness h;
    h.enter_hold(false, 0, 240, 2, shift_mods());
    h.panel.on_pointer_down(PointerDownEvent{h.at_tick_lane(240, 2), PointerButton::Right, {}});
    h.panel.on_pointer_move(PointerMoveEvent{h.at_tick_lane(480, 5), {}});
    h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(480, 5), PointerButton::Left, {}});
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::CriticalHold), 1);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 1);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::CriticalHoldStart), 1);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldStart), 0);
  }
}

void test_selected_regular_empty_cap_left_click_does_not_arm() {
  Harness h;
  const int32_t id = add_hold_body(h, NoteType::Hold, 0, 480, 3, 1, 3, 7);
  h.panel.set_selected({id});
  const auto* note = find_note_id(h.engine.document().notes(), id);
  CHECK(note != nullptr);
  const auto edge = tail_outer_right(h, *note);
  h.panel.on_pointer_down(PointerDownEvent{edge, PointerButton::Left, {}});
  h.panel.on_pointer_up(PointerUpEvent{edge, PointerButton::Left, {}});
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 1);
  const auto* after = find_note_id(h.engine.document().notes(), id);
  CHECK(after != nullptr);
  CHECK(!wds::chart_editor::chained_next_scratch_hold(h.engine.document(), *after).has_value());
}

void test_selected_regular_terminal_left_drag_resizes_tail() {
  Harness h;
  const int32_t id = add_hold_body(h, NoteType::Hold, 0, 480, 3, 1, 3, 7);
  h.panel.set_selected({id});
  const auto* before = find_note_id(h.engine.document().notes(), id);
  CHECK(before != nullptr);
  const auto orig = wds::chart_editor::get_scratch_end_lane_range(*before);
  CHECK_EQ(orig.second, 7);
  const auto grab = tail_right_edge_on_note(h, *before);
  const auto narrower = h.at_tick_lane(480, 5);
  h.panel.on_pointer_down(PointerDownEvent{grab, PointerButton::Left, {}});
  h.panel.on_pointer_move(PointerMoveEvent{narrower, {}});
  h.panel.on_pointer_up(PointerUpEvent{narrower, PointerButton::Left, {}});
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 1);
  const auto* after = find_note_id(h.engine.document().notes(), id);
  CHECK(after != nullptr);
  const auto got = wds::chart_editor::get_scratch_end_lane_range(*after);
  CHECK(got.second < orig.second);
  CHECK(!wds::chart_editor::chained_next_scratch_hold(h.engine.document(), *after).has_value());
}

void test_selected_regular_terminal_tail_shows_resize_cursor() {
  Harness h;
  const int32_t id = add_hold_body(h, NoteType::Hold, 0, 480, 3, 1, 3, 7);
  const auto* note = find_note_id(h.engine.document().notes(), id);
  CHECK(note != nullptr);
  const auto tail_edge = tail_right_edge_on_note(h, *note);
  const auto tail_mid = tail_center_on_note(h, *note);
  const auto body_left = Vec2{h.panel.viewport().x_at(note->lane), h.panel.viewport().y_at(240)};
  const auto body_mid = h.at_tick_lane(240, 3);
  const auto empty = h.at_tick_lane(720, 10);

  CursorKind cursor = CursorKind::Default;
  h.panel.set_cursor_setter([&](CursorKind kind) { cursor = kind; });

  h.panel.on_pointer_move(PointerMoveEvent{empty, {}});
  h.panel.on_pointer_move(PointerMoveEvent{tail_edge, {}});
  CHECK(cursor == CursorKind::Default);
  h.panel.on_pointer_move(PointerMoveEvent{tail_mid, {}});
  CHECK(cursor == CursorKind::Default);

  h.panel.set_selected({id});
  h.panel.on_pointer_move(PointerMoveEvent{empty, {}});
  CHECK(cursor == CursorKind::Default);
  h.panel.on_pointer_move(PointerMoveEvent{tail_edge, {}});
  CHECK(cursor == CursorKind::ResizeHorizontal);
  h.panel.on_pointer_move(PointerMoveEvent{tail_mid, {}});
  CHECK(cursor == CursorKind::ResizeVertical);

  h.panel.on_pointer_move(PointerMoveEvent{body_left, {}});
  CHECK(cursor == CursorKind::ResizeHorizontal);
  h.panel.on_pointer_move(PointerMoveEvent{body_mid, {}});
  CHECK(cursor == CursorKind::Default);
}

void test_selected_scratch_terminal_tail_keeps_resize_cursor() {
  Harness h;
  const int32_t id = add_hold_body(h, NoteType::ScratchHold, 0, 480, 3, 1, 3, 7);
  h.panel.set_selected({id});
  const auto* note = find_note_id(h.engine.document().notes(), id);
  CHECK(note != nullptr);
  const auto tail = tail_right_edge_on_note(h, *note);
  const auto empty = h.at_tick_lane(720, 10);

  CursorKind cursor = CursorKind::Default;
  h.panel.set_cursor_setter([&](CursorKind kind) { cursor = kind; });
  h.panel.on_pointer_move(PointerMoveEvent{empty, {}});
  h.panel.on_pointer_move(PointerMoveEvent{tail, {}});
  CHECK(cursor == CursorKind::ResizeHorizontal);
}

void test_hold_tail_adjust_follows_playback_resync() {
  Harness h;
  const int32_t id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 3);
  h.panel.set_selected({id});
  const auto grab = h.at_tick_lane(960, 4);
  h.panel.on_pointer_down(PointerDownEvent{grab, PointerButton::Left, {}});
  const auto* before = find_note_id(h.engine.document().notes(), id);
  CHECK(before != nullptr);
  CHECK_EQ(before->start_tick, 0);
  CHECK_EQ(before->end_tick, 960);

  h.panel.sync_to_timeline_ms(800.0);
  h.panel.resync_pointer_overlays();

  const int32_t expected = h.panel.viewport().tick_at(grab.y);
  CHECK(expected != 960);
  const auto* after = find_note_id(h.engine.document().notes(), id);
  CHECK(after != nullptr);
  CHECK_EQ(after->start_tick, 0);
  CHECK_EQ(after->end_tick, expected);
}

void test_jumpscratch_end_adjust_follows_wheel_resync() {
  Harness h;
  const int32_t id = add_hold_body(h, NoteType::ScratchHold, 0, 960, 3, 3);
  h.panel.set_selected({id});
  const auto grab = h.at_tick_lane(960, 4);
  h.panel.on_pointer_down(PointerDownEvent{grab, PointerButton::Left, {}});
  const auto* before = find_note_id(h.engine.document().notes(), id);
  CHECK(before != nullptr);
  CHECK_EQ(before->end_tick, 960);

  h.panel.on_scroll(wds::interaction::ScrollEvent{grab, 0.0f, -4.0f, {}});

  const int32_t expected = h.panel.viewport().tick_at(grab.y);
  CHECK(expected != 960);
  const auto* after = find_note_id(h.engine.document().notes(), id);
  CHECK(after != nullptr);
  CHECK_EQ(after->start_tick, 0);
  CHECK_EQ(after->end_tick, expected);
}

void test_jumpscratch_joint_adjust_follows_playback_resync() {
  Harness h;
  const int32_t prev_id = add_hold_body(h, NoteType::ScratchHold, 0, 480, 2, 2, 2, 3);
  const int32_t next_id = add_hold_body(h, NoteType::ScratchHold, 480, 960, 2, 2, 2, 3);
  h.panel.set_selected({prev_id});
  const auto grab = h.at_tick_lane(480, 2);
  h.panel.on_pointer_down(PointerDownEvent{grab, PointerButton::Left, {}});

  h.panel.sync_to_timeline_ms(400.0);
  h.panel.resync_pointer_overlays();

  const int32_t joint = h.panel.viewport().tick_at(grab.y);
  CHECK(joint != 480);
  const auto* prev = find_note_id(h.engine.document().notes(), prev_id);
  const auto* next = find_note_id(h.engine.document().notes(), next_id);
  CHECK(prev != nullptr);
  CHECK(next != nullptr);
  CHECK_EQ(prev->start_tick, 0);
  CHECK_EQ(prev->end_tick, joint);
  CHECK_EQ(next->start_tick, joint);
  CHECK_EQ(next->end_tick, 960);
}

void test_plain_primary_does_not_clear_hold_draft_during_draw() {
  Harness h;
  h.enter_hold(true, 0, 480, 2);
  CHECK(!h.panel.curve_mode_active());
  Modifiers primary;
  primary.control = true;
  h.panel.on_key_down(KeyDownEvent{KeyCode::Unknown, primary, false});
  h.panel.on_pointer_move(PointerMoveEvent{h.at_tick_lane(720, 2), primary});
  CHECK(!h.panel.curve_mode_active());
  CHECK(h.engine.document().notes().empty());
  CHECK(!h.engine.history().can_undo());
  h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(720, 2), PointerButton::Right, {}});
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHold), 1);
}

void test_edit_side_columns_paint_and_modals() {
  Harness h;
  wds::interaction::UiPainter painter;
  painter.set_defer_glyphs(true);
  h.panel.paint_side_columns(painter);

  bool left_bg = false;
  bool right_bg = false;
  bool measure_bg = false;
  bool bpm_chip = false;
  bool meter_chip = false;
  for (const auto& r : painter.rects()) {
    if (std::fabs(r.bounds.x) < 0.5f && std::fabs(r.bounds.w - 60.0f) < 0.5f && r.bounds.h > 100.0f) {
      left_bg = true;
    }
    if (std::fabs(r.bounds.x - 708.0f) < 0.5f && std::fabs(r.bounds.w - 52.0f) < 0.5f &&
        r.bounds.h > 100.0f) {
      right_bg = true;
    }
    if (std::fabs(r.bounds.x - 760.0f) < 0.5f && std::fabs(r.bounds.w - 40.0f) < 0.5f &&
        r.bounds.h > 100.0f) {
      measure_bg = true;
    }
    if (r.color.r > 0.40f && r.color.g > 0.25f && r.color.g < 0.36f && r.color.b < 0.20f &&
        r.bounds.h > 20.0f) {
      bpm_chip = true;
    }
    if (r.color.r < 0.20f && r.color.g > 0.30f && r.color.b > 0.20f && r.color.b < 0.32f &&
        r.bounds.h > 20.0f) {
      meter_chip = true;
    }
  }
  bool bpm_text = false;
  bool meter_text = false;
  bool measure_text = false;
  for (const auto& label : painter.labels()) {
    if (label.text == "120") bpm_text = true;
    if (label.text == "4/4") meter_text = true;
    if (label.text == "1") measure_text = true;
  }
  CHECK(left_bg);
  CHECK(right_bg);
  CHECK(measure_bg);
  CHECK(bpm_chip);
  CHECK(meter_chip);
  CHECK(bpm_text);
  CHECK(meter_text);
  CHECK(measure_text);

  const float split_y = h.panel.viewport().y_at(480);
  h.panel.on_pointer_down(PointerDownEvent{{30.0f, split_y}, PointerButton::Left, {}});
  CHECK(h.panel.has_modal_popup());
  wds::interaction::UiPainter picker;
  picker.set_defer_glyphs(true);
  h.panel.paint_side_columns(picker);
  bool painted_form = false;
  for (const auto& label : picker.labels()) {
    if (label.text == "分割轨道数" || label.text == "确认" || label.text == "取消") {
      painted_form = true;
    }
  }
  CHECK(!painted_form);
  ChartEditPanel::SplitModalDraft split;
  CHECK(h.panel.take_split_modal(split));
  CHECK(!h.panel.has_modal_popup());
  CHECK(h.panel.add_split_effect(split.tick, 2, 1));
  bool added_split = false;
  for (const auto& note : h.engine.document().notes()) {
    if (wds::chart_editor::is_split_lane_gimmick(note.gimmick_type) && note.start_tick == split.tick &&
        note.scratch_length == 1) {
      added_split = true;
    }
  }
  CHECK(added_split);

  const wds::interaction::Rect right_gutter{708.0f, 0.0f, 52.0f, 1000.0f};
  const auto hits =
      wds::ui::build_timing_label_hits(h.panel.viewport(), right_gutter, h.engine.document().timing());
  CHECK(!hits.empty());
  bool opened_bpm = false;
  for (const auto& hit : hits) {
    if (hit.kind != wds::ui::TimingLabelKind::Bpm) continue;
    const float cx = hit.bounds.x + hit.bounds.w * 0.5f;
    const float cy = hit.bounds.y + hit.bounds.h * 0.5f;
    h.panel.on_pointer_down(PointerDownEvent{{cx, cy}, PointerButton::Left, {}});
    opened_bpm = true;
    break;
  }
  CHECK(opened_bpm);
  CHECK(h.panel.has_modal_popup());
  wds::interaction::UiPainter bpm;
  bpm.set_defer_glyphs(true);
  h.panel.paint_side_columns(bpm);
  bool painted_bpm_form = false;
  for (const auto& label : bpm.labels()) {
    if (label.text == "BPM 编辑" || label.text == "确认" || label.text == "取消") {
      painted_bpm_form = true;
    }
  }
  CHECK(!painted_bpm_form);
  ChartEditPanel::TimingModalDraft timing;
  CHECK(h.panel.take_timing_modal(timing));
  CHECK(timing.bpm_mode);
  CHECK(!h.panel.has_modal_popup());
  CHECK(h.panel.apply_bpm(timing.tick, 140.0));
  CHECK(std::fabs(wds::chart_editor::timing_point_at(h.engine.document().timing(), timing.tick).bpm -
                  140.0) < 0.001);
}

void test_split_picker_search_filter() {
  using wds::ui::filter_split_picker_color_ids;
  using wds::ui::is_split_picker_search_text_valid;
  CHECK(is_split_picker_search_text_valid(""));
  CHECK(is_split_picker_search_text_valid("10"));
  CHECK(!is_split_picker_search_text_valid("1a"));
  CHECK(!is_split_picker_search_text_valid("123456789"));
  const auto all = filter_split_picker_color_ids("");
  CHECK(!all.empty());
  const auto ones = filter_split_picker_color_ids("1");
  CHECK(!ones.empty());
  CHECK(ones.size() < all.size());
  for (int32_t id : ones) {
    CHECK(std::to_string(id).find('1') != std::string::npos);
  }
  CHECK(filter_split_picker_color_ids("00000").empty());
}

void expect_track(const std::vector<int32_t>& mids, int32_t probe, int32_t want_lane,
                  int32_t want_width, const char* label) {
  int32_t lane = -1;
  int32_t width = -1;
  CHECK(wds::ui::split_track_between_lines(mids, 12, probe, lane, width));
  if (lane != want_lane || width != want_width) {
    std::fprintf(stderr, "FAIL %s: probe %d got lane=%d width=%d want lane=%d width=%d\n",
                 label, probe, lane, width, want_lane, want_width);
    ++g_failures;
  }
}

void test_split_track_between_overlapping_lines() {
  // Split3 lines after lanes 3 and 7; Split2 after lane 5.
  // Union: tracks [0,4) [4,6) [6,8) [8,12).
  const std::vector<int32_t> union_mids{3, 7, 5};
  expect_track(union_mids, 0, 0, 4, "leftmost");
  expect_track(union_mids, 3, 0, 4, "left track right edge");
  expect_track(union_mids, 4, 4, 2, "first inner");
  expect_track(union_mids, 5, 4, 2, "first inner right");
  expect_track(union_mids, 6, 6, 2, "second inner");
  expect_track(union_mids, 7, 6, 2, "second inner right");
  expect_track(union_mids, 8, 8, 4, "rightmost");
  expect_track(union_mids, 11, 8, 4, "rightmost edge");

  // Single Split3 still partitions into thirds.
  int32_t lane = -1;
  int32_t width = -1;
  CHECK(wds::ui::split_track_for_lane(3, 12, 4, lane, width));
  CHECK_EQ(lane, 4);
  CHECK_EQ(width, 4);

  // Duplicate mids from two identical Split3 effects do not shrink tracks.
  expect_track({3, 7, 3, 7}, 4, 4, 4, "duplicate split3");

  // No interior lines (Split1): whole playfield.
  expect_track({}, 5, 0, 12, "no lines");
}

NotationNote make_split_effect(int32_t start_tick, int32_t end_tick,
                               wds::chart_editor::GimmickType type) {
  NotationNote note;
  note.start_tick = start_tick;
  note.end_tick = end_tick;
  note.lane = 0;
  note.width = 12;
  note.note_type = NoteType::None;
  note.gimmick_type = type;
  return note;
}

void place_tap_at(Harness& h, int32_t tick, int32_t lane) {
  const auto p = h.at_tick_lane(tick, lane);
  h.panel.on_pointer_move(PointerMoveEvent{p, {}});
  h.panel.on_pointer_down(PointerDownEvent{p, PointerButton::Left, {}});
  h.panel.on_pointer_up(PointerUpEvent{p, PointerButton::Left, {}});
}

const NotationNote* last_tap(const std::vector<NotationNote>& notes) {
  const NotationNote* found = nullptr;
  for (const auto& n : notes) {
    if (n.note_type == NoteType::Normal) found = &n;
  }
  return found;
}

void test_split_width_follow_unions_overlapping_effects() {
  using wds::chart_editor::GimmickType;
  Harness h;
  h.panel.set_split_width_follow(true);
  h.panel.set_default_width(1);
  h.engine.add_note(make_split_effect(0, 1920, GimmickType::Split3));
  h.engine.add_note(make_split_effect(0, 1920, GimmickType::Split2));
  place_tap_at(h, 240, 4);
  const NotationNote* tap = last_tap(h.engine.document().notes());
  CHECK(tap != nullptr);
  if (tap != nullptr) {
    CHECK_EQ(tap->lane, 4);
    CHECK_EQ(tap->width, 2);
  }
}

int32_t add_bound_star(Harness& h, int32_t hold_id, int32_t tick, int32_t lane, int32_t width) {
  NotationNote star;
  star.note_type = NoteType::Sound;
  star.start_tick = tick;
  star.end_tick = tick;
  star.lane = lane;
  star.width = width;
  star.parent_hold_id = hold_id;
  const int32_t id = h.engine.add_note(star);
  CHECK(id >= 0);
  return id;
}

Vec2 body_right_edge_on_note(const Harness& h, const NotationNote& note) {
  const float x =
      h.panel.viewport().x_at(note.lane) + h.panel.viewport().lane_width(note.width) - 1.0f;
  // Prefer the note body center so hold time-edge hit tests do not steal the grab.
  const float y = note.end_tick > note.start_tick
                      ? (h.panel.viewport().y_at(note.start_tick) +
                         h.panel.viewport().y_at(note.end_tick)) *
                            0.5f
                      : h.panel.viewport().y_at(note.start_tick);
  return {x, y};
}

void test_selected_width_resize_affects_only_grabbed_note() {
  Harness h;
  NotationNote a;
  a.note_type = NoteType::Normal;
  a.start_tick = 480;
  a.end_tick = 480;
  a.lane = 2;
  a.width = 2;
  NotationNote b;
  b.note_type = NoteType::Normal;
  b.start_tick = 960;
  b.end_tick = 960;
  b.lane = 5;
  b.width = 3;
  const int32_t a_id = h.engine.add_note(a);
  const int32_t b_id = h.engine.add_note(b);
  CHECK(a_id >= 0);
  CHECK(b_id >= 0);

  h.panel.set_selected({a_id, b_id});
  const auto* before_a = find_note_id(h.engine.document().notes(), a_id);
  const auto* before_b = find_note_id(h.engine.document().notes(), b_id);
  CHECK(before_a != nullptr);
  CHECK(before_b != nullptr);
  const int32_t b_width0 = before_b->width;
  const int32_t b_lane0 = before_b->lane;

  const auto grab = body_right_edge_on_note(h, *before_a);
  const auto wider = h.at_tick_lane(480, before_a->end_lane() + 2);
  h.panel.on_pointer_down(PointerDownEvent{grab, PointerButton::Left, {}});
  h.panel.on_pointer_move(PointerMoveEvent{wider, {}});
  h.panel.on_pointer_up(PointerUpEvent{wider, PointerButton::Left, {}});

  const auto* after_a = find_note_id(h.engine.document().notes(), a_id);
  const auto* after_b = find_note_id(h.engine.document().notes(), b_id);
  CHECK(after_a != nullptr);
  CHECK(after_b != nullptr);
  CHECK(after_a->width > 2);
  CHECK_EQ(after_b->width, b_width0);
  CHECK_EQ(after_b->lane, b_lane0);
  CHECK(h.panel.selected().count(a_id));
  CHECK(h.panel.selected().count(b_id));
}

void test_paste_hold_does_not_select_eighths() {
  Harness h;
  h.enter_hold(false, 0, 960, 3);
  h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(960, 3), PointerButton::Left, {}});
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 1);
  const int eighths_before = count_type(h.engine.document().notes(), NoteType::HoldEighth);
  CHECK(eighths_before > 0);

  std::unordered_set<int32_t> copy_ids;
  for (const auto& n : h.engine.document().notes()) {
    if (n.note_type == NoteType::Hold || n.note_type == NoteType::HoldStart) {
      copy_ids.insert(n.id);
    }
  }
  h.panel.set_selected(copy_ids);
  CHECK(h.panel.copy_selected());
  h.panel.on_pointer_move(PointerMoveEvent{h.at_tick_lane(1920, 3), {}});
  CHECK(h.panel.paste_at_pointer());
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 2);
  // Eighths are regenerated for the pasted hold, not copied from the clipboard.
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldEighth), eighths_before * 2);

  int32_t pasted_hold_id = -1;
  for (const auto& n : h.engine.document().notes()) {
    if (n.note_type != NoteType::Hold) continue;
    if (n.start_tick >= 1920) {
      pasted_hold_id = n.id;
      break;
    }
  }
  CHECK(pasted_hold_id >= 0);
  int bound_eighths = 0;
  for (const auto& n : h.engine.document().notes()) {
    if (n.note_type != NoteType::HoldEighth) continue;
    if (n.parent_hold_id != pasted_hold_id) continue;
    CHECK_EQ(n.lane, 3);
    CHECK_EQ(n.width, 1);
    ++bound_eighths;
  }
  CHECK_EQ(bound_eighths, eighths_before);

  for (const int32_t id : h.panel.selected()) {
    const auto note = h.engine.document().find_note(id);
    CHECK(note.has_value());
    if (!note) continue;
    CHECK(note->note_type != NoteType::HoldEighth);
  }
  CHECK(!h.panel.selected().empty());
}

void test_mirror_and_copy_hold_includes_mid_stars() {
  // Whole-hold selection is body + head only; stars must still flip / copy.
  {
    Harness h;
    const int32_t hold_id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 1);
    NotationNote head;
    head.note_type = NoteType::HoldStart;
    head.start_tick = 0;
    head.lane = 3;
    head.width = 1;
    const int32_t head_id = h.engine.add_note(head);
    CHECK(head_id >= 0);
    const int32_t star_id = add_bound_star(h, hold_id, 480, 3, 1);

    h.panel.set_selected({hold_id, head_id});
    CHECK(h.panel.mirror_selected(false));
    CHECK(!h.panel.selected().count(star_id));

    const auto hold = h.engine.document().find_note(hold_id);
    const auto star = h.engine.document().find_note(star_id);
    CHECK(hold.has_value() && star.has_value());
    if (hold && star) {
      CHECK_EQ(hold->lane, 8);
      CHECK_EQ(star->lane, 8);
      CHECK_EQ(star->parent_hold_id, hold_id);
    }
  }

  {
    Harness h;
    const int32_t hold_id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 1);
    NotationNote head;
    head.note_type = NoteType::HoldStart;
    head.start_tick = 0;
    head.lane = 3;
    head.width = 1;
    const int32_t head_id = h.engine.add_note(head);
    CHECK(head_id >= 0);
    add_bound_star(h, hold_id, 480, 3, 1);

    h.panel.set_selected({hold_id, head_id});
    CHECK(h.panel.copy_selected());
    h.panel.on_pointer_move(PointerMoveEvent{h.at_tick_lane(1920, 3), {}});
    CHECK(h.panel.paste_at_pointer());
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 2);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Sound), 2);

    int bound_stars = 0;
    for (const auto& n : h.engine.document().notes()) {
      if (n.note_type != NoteType::Sound) continue;
      const auto parent = h.engine.document().find_note(n.parent_hold_id);
      CHECK(parent.has_value());
      if (!parent) continue;
      CHECK_EQ(static_cast<int>(parent->note_type), static_cast<int>(NoteType::Hold));
      CHECK_EQ(n.lane, parent->lane);
      ++bound_stars;
    }
    CHECK_EQ(bound_stars, 2);
  }
}

void test_convert_selected_hold_stars_and_defaults() {
  {
    Harness h;
    NotationNote tap;
    tap.note_type = NoteType::Normal;
    tap.start_tick = 480;
    tap.lane = 2;
    tap.width = 2;
    const int32_t tap_id = h.engine.add_note(tap);
    h.panel.set_selected({tap_id});
    CHECK(h.panel.convert_selected(NoteType::Hold));
    const auto hold = h.engine.document().find_note(tap_id);
    CHECK(hold.has_value());
    if (hold) {
      CHECK(hold->note_type == NoteType::Hold);
      CHECK_EQ(hold->end_tick, 960);
    }
  }

  {
    Harness h;
    const int32_t hold_id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 1);
    NotationNote head;
    head.note_type = NoteType::HoldStart;
    head.start_tick = 0;
    head.lane = 3;
    head.width = 1;
    const int32_t head_id = h.engine.add_note(head);
    const int32_t star_id = add_bound_star(h, hold_id, 480, 3, 1);
    h.panel.set_selected({hold_id});
    CHECK(h.panel.convert_selected(NoteType::ScratchHold));
    const auto body = h.engine.document().find_note(hold_id);
    const auto star = h.engine.document().find_note(star_id);
    const auto new_head = h.engine.document().find_note(head_id);
    CHECK(body.has_value() && star.has_value() && new_head.has_value());
    if (body && star && new_head) {
      CHECK(body->note_type == NoteType::ScratchHold);
      CHECK_EQ(body->end_tick, 960);
      CHECK(star->note_type == NoteType::ScratchSound);
      CHECK_EQ(star->parent_hold_id, hold_id);
      CHECK(new_head->note_type == NoteType::ScratchHoldStart);
    }
  }

  {
    Harness h;
    const int32_t hold_id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 1);
    NotationNote head;
    head.note_type = NoteType::HoldStart;
    head.start_tick = 0;
    head.lane = 3;
    head.width = 1;
    h.engine.add_note(head);
    const int32_t keep_id = add_bound_star(h, hold_id, 240, 3, 1);
    const int32_t drop_id = add_bound_star(h, hold_id, 480, 3, 1);
    h.panel.set_selected({hold_id, keep_id});
    CHECK(h.panel.convert_selected(NoteType::Flick, 1));
    const auto body = h.engine.document().find_note(hold_id);
    const auto kept = h.engine.document().find_note(keep_id);
    CHECK(body.has_value() && kept.has_value());
    if (body && kept) {
      CHECK(body->note_type == NoteType::Flick);
      CHECK_EQ(body->end_tick, body->start_tick);
      CHECK_EQ(body->scratch_length, 1);
      CHECK(kept->note_type == NoteType::Flick);
      CHECK_EQ(kept->scratch_length, 1);
    }
    CHECK(!h.engine.document().find_note(drop_id).has_value());
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldStart), 0);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Sound), 0);
  }
}

void test_split_width_follow_closed_interval_includes_endpoints() {
  using wds::chart_editor::GimmickType;
  // Split3 covers [0, 480], Split2 covers [480, 960]. Tick 480 is in both.
  Harness overlap;
  overlap.panel.set_split_width_follow(true);
  overlap.panel.set_default_width(1);
  overlap.engine.add_note(make_split_effect(0, 480, GimmickType::Split3));
  overlap.engine.add_note(make_split_effect(480, 960, GimmickType::Split2));
  place_tap_at(overlap, 480, 4);
  const NotationNote* at_joint = last_tap(overlap.engine.document().notes());
  CHECK(at_joint != nullptr);
  if (at_joint != nullptr) {
    CHECK_EQ(at_joint->lane, 4);
    CHECK_EQ(at_joint->width, 2);
  }

  Harness before;
  before.panel.set_split_width_follow(true);
  before.panel.set_default_width(1);
  before.engine.add_note(make_split_effect(0, 480, GimmickType::Split3));
  before.engine.add_note(make_split_effect(480, 960, GimmickType::Split2));
  place_tap_at(before, 240, 4);
  const NotationNote* only_split3 = last_tap(before.engine.document().notes());
  CHECK(only_split3 != nullptr);
  if (only_split3 != nullptr) {
    CHECK_EQ(only_split3->lane, 4);
    CHECK_EQ(only_split3->width, 4);
  }
}

}  // namespace

int main() {
  test_mode_guard_ordinary_and_non_scratch();
  test_regular_hold_shift_right_places_star();
  test_regular_hold_curve_right_up_commits_without_star();
  test_regular_hold_curve_right_down_commits_without_star();
  test_regular_hold_curve_chord_without_curve_does_not_steal_draw();
  test_regular_hold_curve_left_up_commits_ghost_segments();
  test_regular_hold_curve_fill_generates_hold_bodies();
  test_regular_hold_right_click_chains_next_segment();
  test_shift_at_press_draws_gold_head_hold_for_both_families();
  test_shift_after_press_does_not_make_gold_head();
  test_shift_gold_head_still_places_star();
  test_shift_does_not_break_curve_fill_or_chain_second_segment();
  test_selected_regular_empty_cap_left_click_does_not_arm();
  test_selected_regular_terminal_left_drag_resizes_tail();
  test_selected_regular_terminal_tail_shows_resize_cursor();
  test_selected_scratch_terminal_tail_keeps_resize_cursor();
  test_hot_switch_ghosts_and_no_document_mutation();
  test_tail_at_pointer_one_and_multiple_segments();
  test_left_click_and_right_up_commit_modifier_release_ordinary();
  test_extend_hot_switch_is_pure_preview();
  test_extend_curve_confirm_undo_redo_and_eighths();
  test_new_chain_head_vs_extend_and_prev_cover();
  test_template_direction_change_and_missing_linear();
  test_ui_manager_pushes_selection();
  test_dialog_confirm_keeps_toolbar_fill_and_delete_falls_back();
  test_exact_option_wheel_updates_visible_range_and_playhead_grid();
  test_primary_wheel_does_not_change_visible_range();
  test_curve_fill_wheel_does_not_change_visible_range();
  test_hold_tail_adjust_follows_playback_resync();
  test_jumpscratch_end_adjust_follows_wheel_resync();
  test_jumpscratch_joint_adjust_follows_playback_resync();
  test_plain_primary_does_not_clear_hold_draft_during_draw();
  test_edit_side_columns_paint_and_modals();
  test_split_picker_search_filter();
  test_split_track_between_overlapping_lines();
  test_split_width_follow_unions_overlapping_effects();
  test_selected_width_resize_affects_only_grabbed_note();
  test_paste_hold_does_not_select_eighths();
  test_mirror_and_copy_hold_includes_mid_stars();
  test_convert_selected_hold_stars_and_defaults();
  test_split_width_follow_closed_interval_includes_endpoints();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  return 0;
}
