#include "wds/ui/regions/edit/chart_edit_panel.hpp"
#include "wds/ui/regions/edit/chart_edit_renderer.hpp"
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
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

#include <wds/chart_render/note_draw_order.hpp>

namespace wds::ui {
// Befriended by ChartEditPanel: exposes the private hit-test / placement helpers.
struct ChartEditPanelTestAccess {
  static std::optional<wds::chart_editor::NotationNote> hit_test_note(
      const ChartEditPanel& panel, wds::interaction::Vec2 point) {
    return panel.hit_test_note(point);
  }
  static void placement(const ChartEditPanel& panel, wds::interaction::Vec2 point,
                        int32_t& lane, int32_t& width) {
    panel.apply_placement_lane_width(point, lane, width);
  }
  // Runs a whole marquee gesture (anchor at `from`, release at `to`) without going
  // through pointer routing, so tests control the exact rectangle.
  static void marquee(ChartEditPanel& panel, wds::interaction::Vec2 from,
                      wds::interaction::Vec2 to) {
    panel.begin_marquee(from);
    panel.finish_marquee(to);
  }
  static wds::interaction::Rect marquee_rect(ChartEditPanel& panel, wds::interaction::Vec2 from,
                                             wds::interaction::Vec2 to) {
    panel.begin_marquee(from);
    const auto box = panel.marquee_screen_rect(to);
    panel.finish_marquee(to);
    return box;
  }
  static std::pair<float, float> marquee_track_x_span(const ChartEditPanel& panel, float x) {
    return panel.marquee_track_x_span(x);
  }
};
}  // namespace wds::ui

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

using wds::chart_editor::ChartCompatibilityProfile;
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

void test_option_horizontal_wheel_updates_visible_range() {
  Harness h;
  const auto pos = h.at_tick_lane(480, 2);
  const int32_t before = h.panel.viewport().grid().visible_hectoms;
  // Qt Windows puts Alt+wheel on delta_x; macOS Option does the same.
  h.panel.on_scroll(wds::interaction::ScrollEvent{pos, 1.0f, 0.0f, option_mods()});
  CHECK(h.panel.viewport().grid().visible_hectoms != before);
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
  CHECK(cursor == CursorKind::ResizeHorizontal);
  h.panel.on_pointer_move(PointerMoveEvent{tail_mid, {}});
  CHECK(cursor == CursorKind::ResizeVertical);
  h.panel.on_pointer_move(PointerMoveEvent{body_left, {}});
  CHECK(cursor == CursorKind::ResizeHorizontal);
  h.panel.on_pointer_move(PointerMoveEvent{body_mid, {}});
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

  // Count and type are remembered across openings exactly like the color id.
  CHECK(h.panel.add_split_effect(split.tick + 960, 4, 10620, wds::chart_editor::SplitLaneType::Light));
  h.panel.on_pointer_down(
      PointerDownEvent{{30.0f, h.panel.viewport().y_at(1920)}, PointerButton::Left, {}});
  ChartEditPanel::SplitModalDraft again;
  CHECK(h.panel.take_split_modal(again));
  CHECK(again.tick != split.tick);
  CHECK(again.count == 4);
  CHECK(again.color_id == 10620);
  CHECK(again.type == wds::chart_editor::SplitLaneType::Light);

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

void test_unselected_edge_edits_keep_selection() {
  {
    Harness h;
    NotationNote keep;
    keep.note_type = NoteType::Normal;
    keep.start_tick = 1920;
    keep.end_tick = 1920;
    keep.lane = 8;
    keep.width = 1;
    const int32_t keep_id = h.engine.add_note(keep);
    NotationNote tap;
    tap.note_type = NoteType::Normal;
    tap.start_tick = 480;
    tap.end_tick = 480;
    tap.lane = 2;
    tap.width = 2;
    const int32_t tap_id = h.engine.add_note(tap);
    CHECK(keep_id >= 0);
    CHECK(tap_id >= 0);
    h.panel.set_selected({keep_id});

    const auto* before = find_note_id(h.engine.document().notes(), tap_id);
    CHECK(before != nullptr);
    const auto grab = body_right_edge_on_note(h, *before);
    const auto wider = h.at_tick_lane(480, before->end_lane() + 2);
    h.panel.on_pointer_down(PointerDownEvent{grab, PointerButton::Left, {}});
    h.panel.on_pointer_move(PointerMoveEvent{wider, {}});
    h.panel.on_pointer_up(PointerUpEvent{wider, PointerButton::Left, {}});

    const auto* after = find_note_id(h.engine.document().notes(), tap_id);
    CHECK(after != nullptr);
    CHECK(after->width > 2);
    CHECK_EQ(h.panel.selected().size(), 1u);
    CHECK(h.panel.selected().count(keep_id));
    CHECK(!h.panel.selected().count(tap_id));
  }
  {
    Harness h;
    NotationNote keep;
    keep.note_type = NoteType::Normal;
    keep.start_tick = 1920;
    keep.end_tick = 1920;
    keep.lane = 8;
    keep.width = 1;
    const int32_t keep_id = h.engine.add_note(keep);
    const int32_t hold_id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 3);
    CHECK(keep_id >= 0);
    h.panel.set_selected({keep_id});

    const auto* before = find_note_id(h.engine.document().notes(), hold_id);
    CHECK(before != nullptr);
    CHECK_EQ(before->end_tick, 960);
    const auto grab = h.at_tick_lane(before->end_tick, before->lane + 1);
    const auto longer = h.at_tick_lane(1440, before->lane + 1);
    h.panel.on_pointer_down(PointerDownEvent{grab, PointerButton::Left, {}});
    h.panel.on_pointer_move(PointerMoveEvent{longer, {}});
    h.panel.on_pointer_up(PointerUpEvent{longer, PointerButton::Left, {}});

    const auto* after = find_note_id(h.engine.document().notes(), hold_id);
    CHECK(after != nullptr);
    CHECK(after->end_tick != 960);
    CHECK_EQ(after->start_tick, 0);
    CHECK_EQ(h.panel.selected().size(), 1u);
    CHECK(h.panel.selected().count(keep_id));
    CHECK(!h.panel.selected().count(hold_id));
  }
}

void test_multi_select_drag_from_hold_keeps_foreign_notes() {
  // An already-selected hold inside a foreign multi-selection keeps the group for
  // dragging instead of entering its hold layer.
  Harness h;
  NotationNote tap;
  tap.note_type = NoteType::Normal;
  tap.start_tick = 1440;
  tap.end_tick = 1440;
  tap.lane = 6;
  tap.width = 1;
  const int32_t tap_id = h.engine.add_note(tap);
  CHECK(tap_id >= 0);
  h.enter_hold(false, 0, 960, 3);
  h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(960, 3), PointerButton::Left, {}});
  int32_t hold_id = -1;
  int32_t head_id = -1;
  for (const auto& n : h.engine.document().notes()) {
    if (n.note_type == NoteType::Hold) hold_id = n.id;
    if (n.note_type == NoteType::HoldStart) head_id = n.id;
  }
  CHECK(hold_id >= 0);
  CHECK(head_id >= 0);

  h.panel.set_selected({hold_id, head_id, tap_id});
  CHECK_EQ(h.panel.selected().size(), 3u);

  // Grab mid-body (not time edge) and drag up one beat.
  const auto grab = h.at_tick_lane(480, 3);
  const auto dest = h.at_tick_lane(960, 3);
  h.panel.on_pointer_down(PointerDownEvent{grab, PointerButton::Left, {}});
  CHECK(h.panel.selected().count(hold_id));
  CHECK(h.panel.selected().count(tap_id));
  CHECK_EQ(h.panel.selected().size(), 3u);
  h.panel.on_pointer_move(PointerMoveEvent{dest, {}});
  h.panel.on_pointer_up(PointerUpEvent{dest, PointerButton::Left, {}});

  const auto* hold_after = find_note_id(h.engine.document().notes(), hold_id);
  const auto* tap_after = find_note_id(h.engine.document().notes(), tap_id);
  CHECK(hold_after != nullptr);
  CHECK(tap_after != nullptr);
  CHECK_EQ(hold_after->start_tick, 480);
  CHECK_EQ(tap_after->start_tick, 1920);

  // Grabbing the head in the same multi-select also keeps the tap.
  h.panel.set_selected({hold_id, head_id, tap_id});
  const auto head_grab = h.at_tick_lane(hold_after->start_tick, 3);
  const auto head_dest = h.at_tick_lane(hold_after->start_tick + 480, 3);
  h.panel.on_pointer_down(PointerDownEvent{head_grab, PointerButton::Left, {}});
  CHECK(h.panel.selected().count(tap_id));
  CHECK_EQ(h.panel.selected().size(), 3u);
  h.panel.on_pointer_move(PointerMoveEvent{head_dest, {}});
  h.panel.on_pointer_up(PointerUpEvent{head_dest, PointerButton::Left, {}});
  const auto* tap2 = find_note_id(h.engine.document().notes(), tap_id);
  CHECK(tap2 != nullptr);
  CHECK_EQ(tap2->start_tick, 2400);
}

void test_hold_layer_only_on_double_click() {
  Harness h;
  h.enter_hold(false, 0, 960, 3);
  h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(960, 3), PointerButton::Left, {}});
  int32_t hold_id = -1;
  int32_t head_id = -1;
  for (const auto& n : h.engine.document().notes()) {
    if (n.note_type == NoteType::Hold) hold_id = n.id;
    if (n.note_type == NoteType::HoldStart) head_id = n.id;
  }
  CHECK(hold_id >= 0);
  CHECK(head_id >= 0);
  const int32_t star_id = add_bound_star(h, hold_id, 720, 3, 1);

  // Clear placement selection, then single-click the body. A lone headed hold
  // has no Chain layer: its top layer is Whole (body + head).
  h.panel.set_selected({});
  const auto mid = h.at_tick_lane(480, 3);
  h.panel.on_pointer_down(PointerDownEvent{mid, PointerButton::Left, {}});
  h.panel.on_pointer_up(PointerUpEvent{mid, PointerButton::Left, {}});
  CHECK(h.panel.selected().count(hold_id));
  CHECK(h.panel.selected().count(head_id));
  // A second single click on the same layer keeps body+head.
  h.panel.on_pointer_down(PointerDownEvent{mid, PointerButton::Left, {}});
  h.panel.on_pointer_up(PointerUpEvent{mid, PointerButton::Left, {}});
  CHECK(h.panel.selected().count(hold_id));
  CHECK(h.panel.selected().count(head_id));

  // (dbl) Whole → Parts(body) → Stars → wrap Whole.
  h.panel.on_double_click(
      wds::interaction::DoubleClickEvent{mid, PointerButton::Left, {}});
  CHECK_EQ(h.panel.selected().size(), 1u);
  CHECK(h.panel.selected().count(hold_id));
  CHECK(!h.panel.selected().count(head_id));
  h.panel.on_double_click(
      wds::interaction::DoubleClickEvent{mid, PointerButton::Left, {}});
  CHECK_EQ(h.panel.selected().size(), 1u);
  CHECK(h.panel.selected().count(star_id));
  h.panel.on_double_click(
      wds::interaction::DoubleClickEvent{mid, PointerButton::Left, {}});
  CHECK(h.panel.selected().count(hold_id));
  CHECK(h.panel.selected().count(head_id));
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

// engine.add_note never derives eighths; run the core generator once for test setup.
void regenerate_eighths(Harness& h, int32_t hold_id) {
  const auto hold = h.engine.document().find_note(hold_id);
  CHECK(hold.has_value());
  if (!hold) return;
  CHECK(wds::chart_editor::recompute_hold_eighths(h.engine.document(), *hold));
  h.engine.rebuild_snapshot();
}

int32_t add_hold_head(Harness& h, int32_t tick, int32_t lane) {
  NotationNote head;
  head.note_type = NoteType::HoldStart;
  head.start_tick = tick;
  head.lane = lane;
  head.width = 1;
  const int32_t id = h.engine.add_note(head);
  CHECK(id >= 0);
  return id;
}

// Sorted start ticks of the HoldEighths bound to `parent_id`.
std::vector<int32_t> eighth_ticks_of(const std::vector<NotationNote>& notes, int32_t parent_id) {
  std::vector<int32_t> ticks;
  for (const auto& n : notes) {
    if (n.note_type == NoteType::HoldEighth && n.parent_hold_id == parent_id) {
      ticks.push_back(n.start_tick);
    }
  }
  std::sort(ticks.begin(), ticks.end());
  return ticks;
}

void test_hold_selection_layer_order() {
  Harness h;
  const int32_t body_id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 1);
  const int32_t head_id = add_hold_head(h, 0, 3);
  const int32_t star_a = add_bound_star(h, body_id, 240, 3, 1);
  const int32_t star_b = add_bound_star(h, body_id, 720, 3, 1);
  const auto body_pos = h.at_tick_lane(480, 3);
  const auto head_pos = h.at_tick_lane(0, 3);
  const auto star_a_pos = h.at_tick_lane(240, 3);
  const auto star_b_pos = h.at_tick_lane(720, 3);

  auto dbl = [&](Vec2 p) {
    h.panel.on_double_click(wds::interaction::DoubleClickEvent{p, PointerButton::Left, {}});
  };
  auto click = [&](Vec2 p) {
    h.panel.on_pointer_down(PointerDownEvent{p, PointerButton::Left, {}});
    h.panel.on_pointer_up(PointerUpEvent{p, PointerButton::Left, {}});
  };
  auto selected_is = [&](std::initializer_list<int32_t> ids) {
    return h.panel.selected() == std::unordered_set<int32_t>(ids);
  };

  // A lone headed hold has no Chain: the first click selects Whole.
  h.panel.set_selected({});
  click(body_pos);
  CHECK(selected_is({body_id, head_id}));
  click(head_pos);
  CHECK(selected_is({body_id, head_id}));

  // Double-click advances Whole → Parts(body).
  dbl(body_pos);
  CHECK(selected_is({body_id}));
  // Parts single clicks switch head/body.
  click(head_pos);
  CHECK(selected_is({head_id}));
  click(body_pos);
  CHECK(selected_is({body_id}));

  // Double-click advances Parts → Stars(all).
  dbl(body_pos);
  CHECK(selected_is({star_a, star_b}));
  // Stars single clicks switch individual stars.
  click(star_a_pos);
  CHECK(selected_is({star_a}));
  click(star_b_pos);
  CHECK(selected_is({star_b}));

  // Single-click body/head at Stars steps back to Parts.
  click(body_pos);
  CHECK(selected_is({body_id}));
  click(head_pos);
  CHECK(selected_is({head_id}));
  click(body_pos);
  CHECK(selected_is({body_id}));

  // Double-click from Parts advances to Stars again; from Stars it wraps Whole.
  dbl(body_pos);
  CHECK(selected_is({star_a, star_b}));
  dbl(body_pos);
  CHECK(selected_is({body_id, head_id}));
}

void test_hold_chain_layer_then_headless_skips_parts() {
  Harness h;
  const int32_t first_id = add_hold_body(h, NoteType::Hold, 0, 480, 3, 1);
  const int32_t first_head = add_hold_head(h, 0, 3);
  const int32_t first_star = add_bound_star(h, first_id, 120, 3, 1);
  const int32_t second_id = add_hold_body(h, NoteType::Hold, 480, 960, 3, 1);
  const int32_t second_star = add_bound_star(h, second_id, 840, 3, 1);
  const auto* first = find_note_id(h.engine.document().notes(), first_id);
  CHECK(first != nullptr);
  if (first != nullptr) {
    CHECK(wds::chart_editor::chained_next_scratch_hold(h.engine.document(), *first).has_value());
  }

  auto dbl = [&](Vec2 p) {
    h.panel.on_double_click(wds::interaction::DoubleClickEvent{p, PointerButton::Left, {}});
  };
  auto click = [&](Vec2 p) {
    h.panel.on_pointer_down(PointerDownEvent{p, PointerButton::Left, {}});
    h.panel.on_pointer_up(PointerUpEvent{p, PointerButton::Left, {}});
  };
  auto selected_is = [&](std::initializer_list<int32_t> ids) {
    return h.panel.selected() == std::unordered_set<int32_t>(ids);
  };
  const auto first_body_pos = h.at_tick_lane(400, 3);
  const auto first_head_pos = h.at_tick_lane(0, 3);
  const auto first_star_pos = h.at_tick_lane(120, 3);
  const auto second_body_pos = h.at_tick_lane(600, 3);
  const auto second_star_pos = h.at_tick_lane(840, 3);

  // A connected chain's top layer is Chain: the first single click selects
  // every connected hold (both bodies + the first segment's head).
  h.panel.set_selected({});
  click(second_body_pos);
  CHECK(selected_is({first_id, first_head, second_id}));

  // Double-click advances Chain → Whole(second).
  dbl(second_body_pos);
  CHECK(selected_is({second_id}));
  // The second segment is headless: the next layer skips Parts and goes Stars.
  dbl(second_body_pos);
  CHECK(selected_is({second_star}));

  // Same-layer single clicks on stars switch anywhere inside the chain scope.
  click(first_star_pos);
  CHECK(selected_is({first_star}));
  click(second_star_pos);
  CHECK(selected_is({second_star}));
  // Double-click advances/wraps instead of switching.
  dbl(second_body_pos);
  CHECK(selected_is({first_id, first_head, second_id}));

  // Drill to Parts on the first segment, then switch parts/segments by click.
  dbl(first_body_pos);  // Chain → Whole(first)
  CHECK(selected_is({first_id, first_head}));
  dbl(first_body_pos);  // Whole → Parts(body)
  CHECK(selected_is({first_id}));
  click(second_body_pos);  // Parts layer switches to the other chain segment
  CHECK(selected_is({second_id}));
  click(first_head_pos);  // ...and to another segment's head part
  CHECK(selected_is({first_head}));
  click(first_body_pos);
  CHECK(selected_is({first_id}));
  dbl(first_body_pos);  // Parts → Stars(first)
  CHECK(selected_is({first_star}));

  // A headless segment has no Parts layer: single-clicking its body at Stars
  // steps back to Whole (layer 2).
  click(second_body_pos);
  CHECK(selected_is({second_id}));
  dbl(second_body_pos);  // Whole(headless) → Stars(second)
  CHECK(selected_is({second_star}));
}
void test_double_click_star_keeps_direct_jump() {
  Harness h;
  const int32_t body_id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 1);
  const int32_t head_id = add_hold_head(h, 0, 3);
  const int32_t star_a = add_bound_star(h, body_id, 240, 3, 1);
  const int32_t star_b = add_bound_star(h, body_id, 720, 3, 1);
  const auto body_pos = h.at_tick_lane(480, 3);
  const auto star_a_pos = h.at_tick_lane(240, 3);
  const auto star_b_pos = h.at_tick_lane(720, 3);

  auto dbl = [&](Vec2 p) {
    h.panel.on_double_click(wds::interaction::DoubleClickEvent{p, PointerButton::Left, {}});
  };
  auto selected_is = [&](std::initializer_list<int32_t> ids) {
    return h.panel.selected() == std::unordered_set<int32_t>(ids);
  };

  // From an unfocused chart, double-clicking a star selects that star directly.
  h.panel.set_selected({});
  dbl(star_a_pos);
  CHECK(selected_is({star_a}));

  // The shortcut still wins after entering the chain layer.
  h.panel.set_selected({});
  dbl(body_pos);
  CHECK(selected_is({body_id, head_id}));
  dbl(star_b_pos);
  CHECK(selected_is({star_b}));
}

void test_single_headless_hold_top_is_parts() {
  Harness h;
  const int32_t body_id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 1);
  const int32_t star_id = add_bound_star(h, body_id, 720, 3, 1);
  const auto body_pos = h.at_tick_lane(480, 3);

  auto dbl = [&](Vec2 p) {
    h.panel.on_double_click(wds::interaction::DoubleClickEvent{p, PointerButton::Left, {}});
  };
  auto click = [&](Vec2 p) {
    h.panel.on_pointer_down(PointerDownEvent{p, PointerButton::Left, {}});
    h.panel.on_pointer_up(PointerUpEvent{p, PointerButton::Left, {}});
  };
  auto selected_is = [&](std::initializer_list<int32_t> ids) {
    return h.panel.selected() == std::unordered_set<int32_t>(ids);
  };

  // Lone headless hold: no Chain and no Whole; Parts(body) is the top layer.
  h.panel.set_selected({});
  click(body_pos);
  CHECK(selected_is({body_id}));
  // Double-click advances straight to Stars.
  dbl(body_pos);
  CHECK(selected_is({star_id}));
  // Stars body click steps back to Parts(body), not Whole.
  click(body_pos);
  CHECK(selected_is({body_id}));
  dbl(body_pos);
  CHECK(selected_is({star_id}));
}

void test_headless_hold_start_tap_wins_visual_order() {
  Harness h;
  const int32_t hold_id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 1);
  // A note placed on a headless hold's start tick/lane paints above the ribbon.
  NotationNote tap;
  tap.note_type = NoteType::Normal;
  tap.start_tick = 0;
  tap.end_tick = 0;
  tap.lane = 3;
  tap.width = 1;
  const int32_t tap_id = h.engine.add_note(tap);
  CHECK(tap_id >= 0);

  const auto start = h.at_tick_lane(0, 3);
  const auto hit = wds::ui::ChartEditPanelTestAccess::hit_test_note(h.panel, start);
  CHECK(hit.has_value());
  if (hit) CHECK_EQ(hit->id, tap_id);

  // Hover must not turn into the hold's length-drag cursor while over the tap.
  h.panel.on_pointer_move(PointerMoveEvent{start, {}});
  CHECK(h.panel.hover_cursor() == CursorKind::Default);

  h.panel.on_pointer_down(PointerDownEvent{start, PointerButton::Left, {}});
  h.panel.on_pointer_up(PointerUpEvent{start, PointerButton::Left, {}});
  CHECK(h.panel.selected().count(tap_id) != 0);
  CHECK(h.panel.selected().count(hold_id) == 0);
}

// When the pointer sits inside several notes' hit boxes, the selected note owns
// the interaction (move, width, duration) over unrelated unselected notes. Notes
// in the same selection state keep the nearest / visual-order rules.
void test_selected_note_owns_overlapping_pointer() {
  const auto add_tap = [](Harness& h, int32_t tick, int32_t lane, int32_t width) {
    NotationNote tap;
    tap.note_type = NoteType::Normal;
    tap.start_tick = tick;
    tap.end_tick = tick;
    tap.lane = lane;
    tap.width = width;
    const int32_t id = h.engine.add_note(tap);
    CHECK(id >= 0);
    return id;
  };

  {  // Move: the unselected tap is under the pointer, the selected one overlaps it.
    Harness h;
    const int32_t selected_id = add_tap(h, 1040, 4, 1);
    const int32_t other_id = add_tap(h, 1080, 4, 1);
    h.panel.set_selected({selected_id});

    const auto grab = h.at_tick_lane(1080, 4);
    const auto dest = h.at_tick_lane(1560, 4);
    h.panel.on_pointer_down(PointerDownEvent{grab, PointerButton::Left, {}});
    CHECK_EQ(h.panel.selected().size(), 1u);
    CHECK(h.panel.selected().count(selected_id));
    h.panel.on_pointer_move(PointerMoveEvent{dest, {}});
    h.panel.on_pointer_up(PointerUpEvent{dest, PointerButton::Left, {}});

    const auto selected_after = h.engine.document().find_note(selected_id);
    const auto other_after = h.engine.document().find_note(other_id);
    CHECK(selected_after.has_value());
    if (selected_after) CHECK_EQ(selected_after->start_tick, 1560);
    CHECK(other_after.has_value());
    if (other_after) CHECK_EQ(other_after->start_tick, 1080);
  }

  {  // Width: the pointer is on both left handles; the selected note resizes.
    Harness h;
    const int32_t selected_id = add_tap(h, 1040, 3, 2);
    const int32_t other_id = add_tap(h, 1080, 3, 2);
    h.panel.set_selected({selected_id});

    const float left_x = h.panel.viewport().x_at(3) + 1.0f;
    const float y = h.at_tick_lane(1080, 3).y;
    h.panel.on_pointer_down(PointerDownEvent{{left_x, y}, PointerButton::Left, {}});
    const float wider_x = h.panel.viewport().x_at(2) + 1.0f;
    h.panel.on_pointer_move(PointerMoveEvent{{wider_x, y}, {}});
    h.panel.on_pointer_up(PointerUpEvent{{wider_x, y}, PointerButton::Left, {}});

    const auto selected_after = h.engine.document().find_note(selected_id);
    const auto other_after = h.engine.document().find_note(other_id);
    CHECK(selected_after.has_value());
    if (selected_after) {
      CHECK_EQ(selected_after->lane, 2);
      CHECK_EQ(selected_after->width, 3);
    }
    CHECK(other_after.has_value());
    if (other_after) {
      CHECK_EQ(other_after->lane, 3);
      CHECK_EQ(other_after->width, 2);
    }
    CHECK(h.panel.selected().count(selected_id));
  }

  {  // Duration: the unselected tap is under the pointer on the selected hold's
     // end-cap edge (the tap is nearer, the selected hold still owns the drag).
    Harness h;
    const int32_t hold_id = add_hold_body(h, NoteType::Hold, 960, 1440, 3, 1);
    const int32_t tap_id = add_tap(h, 1460, 3, 1);
    h.panel.set_selected({hold_id});

    const auto tail = h.at_tick_lane(1460, 3);
    h.panel.on_pointer_move(PointerMoveEvent{tail, {}});
    CHECK(h.panel.hover_cursor() == CursorKind::ResizeVertical);
    const auto longer = h.at_tick_lane(1920, 3);
    h.panel.on_pointer_down(PointerDownEvent{tail, PointerButton::Left, {}});
    CHECK(h.panel.selected().count(hold_id));
    h.panel.on_pointer_move(PointerMoveEvent{longer, {}});
    h.panel.on_pointer_up(PointerUpEvent{longer, PointerButton::Left, {}});

    const auto hold_after = h.engine.document().find_note(hold_id);
    const auto tap_after = h.engine.document().find_note(tap_id);
    CHECK(hold_after.has_value());
    if (hold_after) CHECK_EQ(hold_after->end_tick, 1920);
    CHECK(tap_after.has_value());
    if (tap_after) CHECK_EQ(tap_after->start_tick, 1460);
    CHECK(h.panel.selected().count(hold_id));
  }

  {  // Neither selected: the old nearest-note rule still picks the closer tap.
    Harness h;
    const int32_t farther_id = add_tap(h, 1040, 4, 1);
    const int32_t nearer_id = add_tap(h, 1080, 4, 1);
    const auto spot = h.at_tick_lane(1080, 4);
    h.panel.on_pointer_down(PointerDownEvent{spot, PointerButton::Left, {}});
    h.panel.on_pointer_up(PointerUpEvent{spot, PointerButton::Left, {}});
    CHECK_EQ(h.panel.selected().size(), 1u);
    CHECK(h.panel.selected().count(nearer_id));
    CHECK(!h.panel.selected().count(farther_id));
  }
}

void test_headless_hold_start_caps_are_tightened() {
  Harness h;
  const int32_t hold_id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 1);
  const auto& v = h.panel.viewport();
  const float x = h.at_tick_lane(0, 3).x;
  const float y0 = v.y_at(0);
  const float note_h = v.note_height_px();

  // The ribbon endpoint itself is still hittable...
  const auto on_start = wds::ui::ChartEditPanelTestAccess::hit_test_note(h.panel, {x, y0});
  CHECK(on_start.has_value());
  if (on_start) CHECK_EQ(on_start->id, hold_id);

  // ...but a head-height band below it is not (there is no head sprite there).
  const auto below =
      wds::ui::ChartEditPanelTestAccess::hit_test_note(h.panel, {x, y0 + note_h * 0.5f});
  CHECK(!below.has_value() || below->id != hold_id);

  // Selection overlay: body-only box ends at the start tick + border pad.
  wds::ui::ChartEditRenderer renderer;
  wds::interaction::UiPainter painter;
  renderer.paint_overlays(painter, v, h.engine.document().notes(), {hold_id}, std::nullopt,
                          &h.engine.document());
  float lowest = -1e9f;
  for (const auto& r : painter.rects()) lowest = std::max(lowest, r.bounds.bottom());
  CHECK(lowest >= y0 - 1.0f);
  CHECK(lowest <= y0 + 6.0f);

  // Parts body-only selection uses the headless style even when a paired head
  // exists but is not selected.
  const int32_t head_id = add_hold_head(h, 0, 3);
  wds::interaction::UiPainter parts;
  renderer.paint_overlays(parts, v, h.engine.document().notes(), {hold_id}, std::nullopt,
                          &h.engine.document());
  float parts_lowest = -1e9f;
  for (const auto& r : parts.rects()) parts_lowest = std::max(parts_lowest, r.bounds.bottom());
  CHECK(parts_lowest >= y0 - 1.0f);
  CHECK(parts_lowest <= y0 + 6.0f);

  // A paired head keeps its full start cap when head+body are selected.
  wds::interaction::UiPainter headed;
  renderer.paint_overlays(headed, v, h.engine.document().notes(), {hold_id, head_id},
                          std::nullopt, &h.engine.document());
  float headed_lowest = -1e9f;
  for (const auto& r : headed.rects()) headed_lowest = std::max(headed_lowest, r.bounds.bottom());
  CHECK(headed_lowest > y0 + note_h * 0.4f);
}

void test_double_click_inside_hold_after_edge_grab_selects_segment() {
  Harness h;
  const int32_t body_id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 1);
  const int32_t head_id = add_hold_head(h, 0, 3);
  const int32_t star_id = add_bound_star(h, body_id, 720, 3, 1);
  const auto mid = h.at_tick_lane(480, 3);
  h.panel.set_selected({body_id, head_id});

  // No first click was recorded: a double-click enters the outer layer (Whole
  // for a lone headed hold).
  h.panel.on_double_click(wds::interaction::DoubleClickEvent{mid, PointerButton::Left, {}});
  CHECK(h.panel.selected().count(body_id) != 0);
  CHECK(h.panel.selected().count(head_id) != 0);

  // Edge grab + double-click advances Whole → Parts(body).
  const auto edge = h.at_tick_lane(960, 3);
  h.panel.on_pointer_down(PointerDownEvent{edge, PointerButton::Left, {}});
  h.panel.on_pointer_up(PointerUpEvent{edge, PointerButton::Left, {}});
  h.panel.on_double_click(wds::interaction::DoubleClickEvent{mid, PointerButton::Left, {}});
  CHECK_EQ(h.panel.selected().size(), 1u);
  CHECK(h.panel.selected().count(body_id) != 0);

  // Another edge double-click advances Parts → Stars.
  h.panel.on_pointer_down(PointerDownEvent{edge, PointerButton::Left, {}});
  h.panel.on_pointer_up(PointerUpEvent{edge, PointerButton::Left, {}});
  h.panel.on_double_click(wds::interaction::DoubleClickEvent{mid, PointerButton::Left, {}});
  CHECK(h.panel.selected().count(star_id) != 0);
}

void test_right_click_selected_hold_tail_continues_draw() {
  Harness h;
  const int32_t body_id = add_hold_body(h, NoteType::Hold, 0, 480, 3, 1);
  const int32_t head_id = add_hold_head(h, 0, 3);
  h.panel.set_selected({body_id, head_id});
  const auto* before = find_note_id(h.engine.document().notes(), body_id);
  CHECK(before != nullptr);
  if (before == nullptr) return;

  const auto tail = h.at_tick_lane(before->end_tick, before->lane);
  const auto pull = h.at_tick_lane(before->end_tick + 480, before->lane);
  h.panel.on_pointer_down(PointerDownEvent{tail, PointerButton::Right, {}});
  h.panel.on_pointer_move(PointerMoveEvent{pull, {}});
  h.panel.on_pointer_up(PointerUpEvent{pull, PointerButton::Right, {}});

  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 2);
  const auto* first = find_note_id(h.engine.document().notes(), body_id);
  CHECK(first != nullptr);
  if (first) {
    CHECK(wds::chart_editor::chained_next_scratch_hold(h.engine.document(), *first).has_value());
  }

  // The ScratchHold family uses the same right-click continuation.
  {
    Harness hs;
    const int32_t scratch_id = add_hold_body(hs, NoteType::ScratchHold, 0, 480, 3, 1);
    hs.panel.set_selected({scratch_id});
    const auto* scratch = find_note_id(hs.engine.document().notes(), scratch_id);
    CHECK(scratch != nullptr);
    if (scratch != nullptr) {
      const auto scratch_tail = hs.at_tick_lane(scratch->end_tick, scratch->lane);
      const auto scratch_pull = hs.at_tick_lane(scratch->end_tick + 480, scratch->lane);
      hs.panel.on_pointer_down(PointerDownEvent{scratch_tail, PointerButton::Right, {}});
      hs.panel.on_pointer_move(PointerMoveEvent{scratch_pull, {}});
      hs.panel.on_pointer_up(PointerUpEvent{scratch_pull, PointerButton::Right, {}});
      CHECK_EQ(count_type(hs.engine.document().notes(), NoteType::ScratchHold), 2);
    }
  }
}

void test_hold_eighths_are_never_selectable_or_deletable() {
  Harness h;
  const int32_t hold_id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 1);
  const int32_t head_id = add_hold_head(h, 0, 3);
  regenerate_eighths(h, hold_id);
  std::unordered_set<int32_t> eighth_ids;
  for (const auto& n : h.engine.document().notes()) {
    if (n.note_type == NoteType::HoldEighth) eighth_ids.insert(n.id);
  }
  CHECK(!eighth_ids.empty());
  const int eighths_before = static_cast<int>(eighth_ids.size());
  auto selects_eighth = [&]() {
    for (const int32_t id : h.panel.selected()) {
      if (eighth_ids.count(id) != 0) return true;
    }
    return false;
  };

  {  // set_selected filters eighth ids defensively.
    auto ids = eighth_ids;
    ids.insert(hold_id);
    ids.insert(head_id);
    h.panel.set_selected(ids);
    CHECK(h.panel.selected().count(hold_id) != 0);
    CHECK(h.panel.selected().count(head_id) != 0);
    CHECK_EQ(h.panel.selected().size(), 2u);
    CHECK(!selects_eighth());
  }

  {  // Click / ctrl-click on every eighth never hits or selects one.
    h.panel.clear_selection();
    const auto notes = h.engine.document().notes();
    for (const auto& n : notes) {
      if (n.note_type != NoteType::HoldEighth) continue;
      const auto p = h.at_tick_lane(n.start_tick, n.lane);
      const auto hit = wds::ui::ChartEditPanelTestAccess::hit_test_note(h.panel, p);
      CHECK(!hit.has_value() || hit->note_type != NoteType::HoldEighth);
      h.panel.on_pointer_down(PointerDownEvent{p, PointerButton::Left, {}});
      h.panel.on_pointer_up(PointerUpEvent{p, PointerButton::Left, {}});
      CHECK(!selects_eighth());
      h.panel.on_pointer_down(PointerDownEvent{p, PointerButton::Left, primary_mods()});
      h.panel.on_pointer_up(PointerUpEvent{p, PointerButton::Left, primary_mods()});
      CHECK(!selects_eighth());
    }
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldEighth), eighths_before);
  }

  {  // Marquee over the whole hold selects the hold and head, never its eighths.
    h.panel.clear_selection();
    wds::ui::ChartEditPanelTestAccess::marquee(h.panel, h.at_tick_lane(0, 1),
                                               h.at_tick_lane(1000, 8));
    CHECK(h.panel.selected().count(hold_id) != 0);
    CHECK(h.panel.selected().count(head_id) != 0);
    CHECK(!selects_eighth());
  }

  {  // Delete-selected has nothing to delete when only eighths were requested.
    h.panel.set_selected(eighth_ids);
    CHECK(h.panel.selected().empty());
    CHECK(!h.panel.delete_selected());
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldEighth), eighths_before);
  }

  {  // Middle-click on an eighth's tick hits the hold; the hold and its eighths go together.
    h.panel.clear_selection();
    h.panel.on_pointer_down(
        PointerDownEvent{h.at_tick_lane(240, 3), PointerButton::Middle, {}});
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 0);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldEighth), 0);
    CHECK(h.engine.undo());
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 1);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldEighth), eighths_before);
  }
}

// The document's eighths of `hold_id` must equal with_recomputed_hold_eighths applied
// to the document's non-eighth notes (eighths are purely derived).
void check_hold_eighths_derived(Harness& h, int32_t hold_id) {
  const auto hold = h.engine.document().find_note(hold_id);
  CHECK(hold.has_value());
  if (!hold) return;
  auto base = h.engine.document().notes();
  base.erase(std::remove_if(base.begin(), base.end(),
                            [](const NotationNote& n) {
                              return n.note_type == NoteType::HoldEighth;
                            }),
             base.end());
  const auto expected = wds::chart_editor::with_recomputed_hold_eighths(
      std::move(base), *hold, h.engine.document().timing().ticks_per_quarter);
  CHECK(eighth_ticks_of(h.engine.document().notes(), hold_id) ==
        eighth_ticks_of(expected, hold_id));
}

bool has_tick(const std::vector<int32_t>& ticks, int32_t tick) {
  return std::find(ticks.begin(), ticks.end(), tick) != ticks.end();
}

void test_delete_star_regenerates_eighth_in_one_undo_step() {
  Harness h;
  const int32_t hold_id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 1);
  add_hold_head(h, 0, 3);
  const int32_t star_id = add_bound_star(h, hold_id, 480, 3, 1);
  regenerate_eighths(h, hold_id);
  CHECK(!has_tick(eighth_ticks_of(h.engine.document().notes(), hold_id), 480));
  check_hold_eighths_derived(h, hold_id);

  const size_t undo_before = h.engine.history().undo_count();
  h.panel.set_selected({star_id});
  CHECK(h.panel.delete_selected());
  CHECK_EQ(h.engine.history().undo_count(), undo_before + 1);
  CHECK(!h.engine.document().find_note(star_id).has_value());
  CHECK(has_tick(eighth_ticks_of(h.engine.document().notes(), hold_id), 480));
  check_hold_eighths_derived(h, hold_id);

  CHECK(h.engine.undo());  // one step restores the star and drops the eighth
  CHECK(h.engine.document().find_note(star_id).has_value());
  CHECK(!has_tick(eighth_ticks_of(h.engine.document().notes(), hold_id), 480));
  check_hold_eighths_derived(h, hold_id);
}

void test_nudge_star_within_hold_updates_eighths() {
  Harness h;
  const int32_t hold_id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 1);
  add_hold_head(h, 0, 3);
  const int32_t star_id = add_bound_star(h, hold_id, 480, 3, 1);
  regenerate_eighths(h, hold_id);

  const size_t undo_before = h.engine.history().undo_count();
  h.panel.set_selected({star_id});
  CHECK(h.panel.nudge_selected(240, 0));
  CHECK_EQ(h.engine.history().undo_count(), undo_before + 1);
  const auto star = h.engine.document().find_note(star_id);
  CHECK(star.has_value() && star->start_tick == 720);
  const auto ticks = eighth_ticks_of(h.engine.document().notes(), hold_id);
  CHECK(has_tick(ticks, 480));   // old star tick is free again
  CHECK(!has_tick(ticks, 720));  // new star tick is occupied
  check_hold_eighths_derived(h, hold_id);

  CHECK(h.engine.undo());
  const auto restored = h.engine.document().find_note(star_id);
  CHECK(restored.has_value() && restored->start_tick == 480);
  CHECK(!has_tick(eighth_ticks_of(h.engine.document().notes(), hold_id), 480));
  check_hold_eighths_derived(h, hold_id);
}

void test_nudge_hold_moves_stars_and_regenerates_eighths() {
  Harness h;
  const int32_t hold_id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 1);
  const int32_t star_id = add_bound_star(h, hold_id, 480, 3, 1);
  regenerate_eighths(h, hold_id);

  const size_t undo_before = h.engine.history().undo_count();
  h.panel.set_selected({hold_id});
  CHECK(h.panel.nudge_selected(240, 0));
  CHECK_EQ(h.engine.history().undo_count(), undo_before + 1);
  const auto hold = h.engine.document().find_note(hold_id);
  const auto star = h.engine.document().find_note(star_id);
  CHECK(hold.has_value() && hold->start_tick == 240 && hold->end_tick == 1200);
  CHECK(star.has_value() && star->start_tick == 720 && star->parent_hold_id == hold_id);
  const auto ticks = eighth_ticks_of(h.engine.document().notes(), hold_id);
  CHECK(!ticks.empty());
  CHECK(!has_tick(ticks, 720));
  for (const int32_t t : ticks) CHECK(t > 240 && t < 1200);
  check_hold_eighths_derived(h, hold_id);

  CHECK(h.engine.undo());
  const auto back = h.engine.document().find_note(star_id);
  CHECK(back.has_value() && back->start_tick == 480);
  check_hold_eighths_derived(h, hold_id);
}

void test_nudge_star_out_of_hold_is_refused() {
  Harness h;
  const int32_t hold_id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 1);
  const int32_t star_id = add_bound_star(h, hold_id, 480, 3, 1);
  regenerate_eighths(h, hold_id);
  h.panel.set_selected({star_id});
  const auto before = h.engine.document().notes();
  const size_t undo_before = h.engine.history().undo_count();
  CHECK(!h.panel.nudge_selected(960, 0));   // would land on the hold end
  CHECK(!h.panel.nudge_selected(-480, 0));  // would land on the hold start
  CHECK(!h.panel.nudge_selected(0, 1));     // would leave the hold's lanes
  CHECK(notes_eq(h.engine.document().notes(), before));
  CHECK_EQ(h.engine.history().undo_count(), undo_before);
  check_hold_eighths_derived(h, hold_id);
}

void test_copy_lone_mid_star_is_a_noop() {
  Harness h;
  const int32_t hold_id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 1);
  const int32_t head_id = add_hold_head(h, 0, 3);
  const int32_t star_id = add_bound_star(h, hold_id, 480, 3, 1);
  regenerate_eighths(h, hold_id);

  // A previous valid copy must be replaced (cleared) by a lone-star copy.
  h.panel.set_selected({hold_id, head_id});
  CHECK(h.panel.copy_selected());

  h.panel.set_selected({star_id});
  CHECK_EQ(h.panel.selected().size(), 1u);
  CHECK(!h.panel.copy_selected());
  const auto before = h.engine.document().notes();
  h.panel.on_pointer_move(PointerMoveEvent{h.at_tick_lane(1920, 3), {}});
  CHECK(!h.panel.paste_at_pointer());
  CHECK(notes_eq(h.engine.document().notes(), before));

  {  // Unbound star (parent_hold_id < 0) is not copyable either.
    NotationNote loose;
    loose.note_type = NoteType::Sound;
    loose.start_tick = 1440;
    loose.end_tick = 1440;
    loose.lane = 8;
    loose.width = 1;
    loose.parent_hold_id = -1;
    const int32_t loose_id = h.engine.add_note(loose);
    if (loose_id >= 0) {
      h.panel.set_selected({loose_id});
      CHECK(!h.panel.copy_selected());
      const auto with_loose = h.engine.document().notes();
      CHECK(!h.panel.paste_at_pointer());
      CHECK(notes_eq(h.engine.document().notes(), with_loose));
    }
  }

  {  // Mixed selection: the star is skipped, the tap is still copied and pasted.
    NotationNote tap;
    tap.note_type = NoteType::Normal;
    tap.start_tick = 480;
    tap.lane = 10;
    tap.width = 1;
    const int32_t tap_id = h.engine.add_note(tap);
    CHECK(tap_id >= 0);
    const int sounds_before = count_type(h.engine.document().notes(), NoteType::Sound);
    h.panel.set_selected({star_id, tap_id});
    CHECK(h.panel.copy_selected());
    h.panel.on_pointer_move(PointerMoveEvent{h.at_tick_lane(2400, 3), {}});
    CHECK(h.panel.paste_at_pointer());
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Normal), 2);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Sound), sounds_before);
  }
}

void test_copy_hold_with_star_pastes_bound_star_and_regenerated_eighths() {
  Harness h;
  const int32_t hold_id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 1);
  const int32_t head_id = add_hold_head(h, 0, 3);
  const int32_t star_id = add_bound_star(h, hold_id, 480, 3, 1);
  regenerate_eighths(h, hold_id);
  const auto orig_ticks = eighth_ticks_of(h.engine.document().notes(), hold_id);
  CHECK(!orig_ticks.empty());
  for (const int32_t tick : orig_ticks) CHECK(tick != 480);  // star tick stays free

  h.panel.set_selected({hold_id, head_id});
  CHECK(h.panel.copy_selected());
  h.panel.on_pointer_move(PointerMoveEvent{h.at_tick_lane(1920, 3), {}});
  CHECK(h.panel.paste_at_pointer());
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 2);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Sound), 2);

  int32_t new_hold_id = -1;
  int32_t new_hold_start = 0;
  for (const auto& n : h.engine.document().notes()) {
    if (n.note_type == NoteType::Hold && n.id != hold_id) {
      new_hold_id = n.id;
      new_hold_start = n.start_tick;
    }
  }
  CHECK(new_hold_id >= 0);
  int bound_new_stars = 0;
  int32_t new_star_tick = -1;
  for (const auto& n : h.engine.document().notes()) {
    if (n.note_type != NoteType::Sound || n.id == star_id) continue;
    CHECK_EQ(n.parent_hold_id, new_hold_id);
    new_star_tick = n.start_tick;
    ++bound_new_stars;
  }
  CHECK_EQ(bound_new_stars, 1);

  // Eighths were regenerated for the pasted hold (same grid, shifted), none copied.
  const auto new_ticks = eighth_ticks_of(h.engine.document().notes(), new_hold_id);
  CHECK_EQ(new_ticks.size(), orig_ticks.size());
  if (new_ticks.size() == orig_ticks.size()) {
    for (size_t i = 0; i < new_ticks.size(); ++i) {
      CHECK_EQ(new_ticks[i] - orig_ticks[i], new_hold_start);
      CHECK(new_ticks[i] != new_star_tick);
    }
  }
  CHECK_EQ(eighth_ticks_of(h.engine.document().notes(), hold_id).size(), orig_ticks.size());
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldEighth),
           static_cast<int>(orig_ticks.size() * 2));
  for (const int32_t id : h.panel.selected()) {
    const auto note = h.engine.document().find_note(id);
    CHECK(note.has_value() && note->note_type != NoteType::HoldEighth);
  }
}

void test_nontail_hold_eighths_follow_paste_and_resize() {
  Harness h;
  h.engine.document().set_compatibility_profile(ChartCompatibilityProfile::SiriusExtended);
  const int32_t hold_id = add_hold_body(h, NoteType::NontailHold, 0, 960, 3, 1);
  CHECK(hold_id >= 0);
  regenerate_eighths(h, hold_id);
  const auto base_ticks = eighth_ticks_of(h.engine.document().notes(), hold_id);
  CHECK(!base_ticks.empty());

  {  // Paste: the pasted nontail hold gets its own regenerated eighths.
    h.panel.set_selected({hold_id});
    CHECK(h.panel.copy_selected());
    h.panel.on_pointer_move(PointerMoveEvent{h.at_tick_lane(1920, 3), {}});
    CHECK(h.panel.paste_at_pointer());
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::NontailHold), 2);
    int32_t pasted_id = -1;
    int32_t pasted_start = 0;
    for (const auto& n : h.engine.document().notes()) {
      if (n.note_type == NoteType::NontailHold && n.id != hold_id) {
        pasted_id = n.id;
        pasted_start = n.start_tick;
      }
    }
    CHECK(pasted_id >= 0);
    const auto pasted_ticks = eighth_ticks_of(h.engine.document().notes(), pasted_id);
    CHECK_EQ(pasted_ticks.size(), base_ticks.size());
    if (pasted_ticks.size() == base_ticks.size()) {
      for (size_t i = 0; i < base_ticks.size(); ++i) {
        CHECK_EQ(pasted_ticks[i] - base_ticks[i], pasted_start);
      }
    }
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldEighth),
             static_cast<int>(base_ticks.size() * 2));
  }

  {  // Resize the original's end edge: its eighths follow the new span.
    h.panel.clear_selection();
    const auto end_point = h.at_tick_lane(960, 3);
    const auto new_end = h.at_tick_lane(1440, 3);
    h.panel.on_pointer_down(PointerDownEvent{end_point, PointerButton::Left, {}});
    h.panel.on_pointer_move(PointerMoveEvent{new_end, {}});
    h.panel.on_pointer_up(PointerUpEvent{new_end, PointerButton::Left, {}});
    const auto hold = h.engine.document().find_note(hold_id);
    CHECK(hold.has_value());
    if (hold) {
      CHECK(hold->end_tick > 960);
      const auto ticks = eighth_ticks_of(h.engine.document().notes(), hold_id);
      CHECK(ticks.size() > base_ticks.size());
      CHECK_EQ(ticks.size(), wds::chart_editor::hold_eighth_slot_count(*hold, 480));
      for (const int32_t tick : ticks) {
        CHECK(tick > hold->start_tick && tick < hold->end_tick);
      }
    }
  }
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
    const int32_t child_id = add_bound_star(h, hold_id, 240, 3, 1);
    const int32_t other_id = add_bound_star(h, hold_id, 480, 3, 1);
    h.panel.set_selected({hold_id, child_id});
    CHECK(h.panel.convert_selected(NoteType::Flick, 1));
    const auto body = h.engine.document().find_note(hold_id);
    CHECK(body.has_value());
    if (body) {
      CHECK(body->note_type == NoteType::Flick);
      CHECK_EQ(body->end_tick, body->start_tick);
      CHECK_EQ(body->scratch_length, 1);
    }
    // Hold → non-hold deletes every child, including the selected one. Stars are
    // never converted to non-star types.
    CHECK(!h.engine.document().find_note(child_id).has_value());
    CHECK(!h.engine.document().find_note(other_id).has_value());
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldStart), 0);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Sound), 0);
  }
}

const NotationNote* last_of_type(const std::vector<NotationNote>& notes, NoteType type);

void test_star_target_converts_hold_children_and_star_bodies() {
  Harness h;
  const int32_t hold_id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 2);
  NotationNote head;
  head.note_type = NoteType::HoldStart;
  head.start_tick = 0;
  head.lane = 3;
  head.width = 2;
  const int32_t head_id = h.engine.add_note(head);
  const int32_t first = add_bound_star(h, hold_id, 240, 3, 2);
  const int32_t second = add_bound_star(h, hold_id, 480, 3, 2);

  h.panel.set_selected({hold_id});
  CHECK(h.panel.convert_selected(NoteType::ScratchSound));
  {
    const auto body = h.engine.document().find_note(hold_id);
    const auto star_a = h.engine.document().find_note(first);
    const auto star_b = h.engine.document().find_note(second);
    const auto head_after = h.engine.document().find_note(head_id);
    CHECK(body.has_value() && star_a.has_value() && star_b.has_value() && head_after.has_value());
    if (body && star_a && star_b && head_after) {
      CHECK(body->note_type == NoteType::Hold);
      CHECK(star_a->note_type == NoteType::ScratchSound);
      CHECK(star_b->note_type == NoteType::ScratchSound);
      CHECK_EQ(star_a->parent_hold_id, hold_id);
      CHECK_EQ(star_b->parent_hold_id, hold_id);
      CHECK(head_after->note_type == NoteType::HoldStart);
    }
  }

  // Direct star selection changes only that star.
  h.panel.set_selected({first});
  CHECK(h.panel.convert_selected(NoteType::Sound));
  {
    const auto star_a = h.engine.document().find_note(first);
    const auto star_b = h.engine.document().find_note(second);
    CHECK(star_a.has_value() && star_b.has_value());
    if (star_a && star_b) {
      CHECK(star_a->note_type == NoteType::Sound);
      CHECK(star_b->note_type == NoteType::ScratchSound);
      CHECK_EQ(star_a->parent_hold_id, hold_id);
    }
  }

  // Ordinary convert buttons never turn a star body into another note type.
  h.panel.set_selected({first});
  CHECK(!h.panel.convert_selected(NoteType::Normal));
  {
    const auto star_a = h.engine.document().find_note(first);
    CHECK(star_a.has_value());
    if (star_a) CHECK(star_a->note_type == NoteType::Sound);
  }
}

void test_toolbox_star_placement_snaps_to_pointer_hold() {
  namespace in = wds::interaction;
  {
    Harness h;
    const int32_t hold_id = add_hold_body(h, NoteType::Hold, 0, 960, 3, 2);
    h.panel.set_toolbox_place_mode(true);
    h.panel.set_place_intent_override(in::PlaceIntent::SoundPurple);

    // Pointer sits on lane 4 (right half of the lane-3..4 hold). The ghost and
    // the placed star must both snap to the hold's whole span.
    const auto p = h.at_tick_lane(480, 4);
    h.panel.on_pointer_move(PointerMoveEvent{p, {}});
    bool saw_star = false;
    for (const auto& ghost : h.panel.skinned_ghosts()) {
      if (ghost.note.note_type != NoteType::ScratchSound) continue;
      saw_star = true;
      CHECK_EQ(ghost.note.lane, 3);
      CHECK_EQ(ghost.note.width, 2);
      CHECK_EQ(ghost.note.parent_hold_id, hold_id);
    }
    CHECK(saw_star);

    h.panel.on_pointer_down(PointerDownEvent{p, PointerButton::Left, {}});
    h.panel.on_pointer_up(PointerUpEvent{p, PointerButton::Left, {}});
    const NotationNote* star = last_of_type(h.engine.document().notes(), NoteType::ScratchSound);
    CHECK(star != nullptr);
    if (star) {
      CHECK_EQ(star->lane, 3);
      CHECK_EQ(star->width, 2);
      CHECK_EQ(star->parent_hold_id, hold_id);
    }

    // Same tick again: duplicate star is refused (no extra note / undo step).
    const int count_before = count_type(h.engine.document().notes(), NoteType::ScratchSound);
    h.panel.on_pointer_down(PointerDownEvent{p, PointerButton::Left, {}});
    h.panel.on_pointer_up(PointerUpEvent{p, PointerButton::Left, {}});
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchSound), count_before);

    // Outside the hold's lane span: no ghost and no placement.
    h.panel.set_selected({hold_id});
    const auto outside = h.at_tick_lane(480, 8);
    h.panel.on_pointer_move(PointerMoveEvent{outside, {}});
    CHECK(h.panel.skinned_ghosts().empty());
    h.panel.on_pointer_down(PointerDownEvent{outside, PointerButton::Left, {}});
    h.panel.on_pointer_up(PointerUpEvent{outside, PointerButton::Left, {}});
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchSound), count_before);
    CHECK_EQ(static_cast<int>(h.panel.selected().size()), 1);
    CHECK(h.panel.selected().count(hold_id));

    // Hold start/end ticks are not open span: no ghost / no placement.
    const auto boundary = h.at_tick_lane(0, 3);
    h.panel.on_pointer_move(PointerMoveEvent{boundary, {}});
    CHECK(h.panel.skinned_ghosts().empty());
    h.panel.on_pointer_down(PointerDownEvent{boundary, PointerButton::Left, {}});
    h.panel.on_pointer_up(PointerUpEvent{boundary, PointerButton::Left, {}});
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchSound), count_before);
  }

  // Opposite color is legal: Sound on a purple ScratchHold.
  {
    Harness h;
    const int32_t hold_id = add_hold_body(h, NoteType::ScratchHold, 0, 960, 5, 2);
    h.panel.set_toolbox_place_mode(true);
    h.panel.set_place_intent_override(in::PlaceIntent::Sound);
    const auto p = h.at_tick_lane(480, 5);
    h.panel.on_pointer_move(PointerMoveEvent{p, {}});
    h.panel.on_pointer_down(PointerDownEvent{p, PointerButton::Left, {}});
    h.panel.on_pointer_up(PointerUpEvent{p, PointerButton::Left, {}});
    const NotationNote* star = last_of_type(h.engine.document().notes(), NoteType::Sound);
    CHECK(star != nullptr);
    if (star) {
      CHECK_EQ(star->lane, 5);
      CHECK_EQ(star->width, 2);
      CHECK_EQ(star->parent_hold_id, hold_id);
    }
  }
}

Vec2 chip_center(const wds::interaction::Rect& bounds) {
  return {bounds.x + bounds.w * 0.5f, bounds.y + bounds.h * 0.5f};
}

const wds::ui::GutterLabelHit* split_hit_for(const std::vector<wds::ui::GutterLabelHit>& hits,
                                             int32_t note_id, bool is_start) {
  for (const auto& hit : hits) {
    if (hit.note_id == note_id && hit.is_start == is_start) return &hit;
  }
  return nullptr;
}

void test_marquee_and_convert_ignore_split_lines() {
  using wds::chart_editor::GimmickType;
  Harness h;
  NotationNote split = make_split_effect(0, 480, GimmickType::Split3);
  split.scratch_length = 10120;
  const int32_t split_id = h.engine.add_note(split);
  CHECK(split_id >= 0);

  NotationNote tap;
  tap.note_type = NoteType::Normal;
  tap.start_tick = 0;
  tap.lane = 5;
  tap.width = 2;
  const int32_t tap_id = h.engine.add_note(tap);
  CHECK(tap_id >= 0);

  h.panel.set_selected({split_id, tap_id});
  CHECK(h.panel.selected().count(split_id));
  CHECK(h.panel.selected().count(tap_id));

  h.panel.clear_selection();
  const auto start = h.at_tick_lane(0, 1);
  const auto end = h.at_tick_lane(240, 8);
  h.panel.on_pointer_down(PointerDownEvent{start, PointerButton::Left, primary_mods()});
  h.panel.on_pointer_move(PointerMoveEvent{end, primary_mods()});
  h.panel.on_pointer_up(PointerUpEvent{end, PointerButton::Left, primary_mods()});
  CHECK(!h.panel.selected().count(split_id));
  CHECK(h.panel.selected().count(tap_id));

  h.panel.set_selected({split_id, tap_id});
  CHECK(h.panel.convert_selected(NoteType::Flick, 1));
  const auto after_split = h.engine.document().find_note(split_id);
  const auto after_tap = h.engine.document().find_note(tap_id);
  CHECK(after_split.has_value() && after_tap.has_value());
  if (after_split && after_tap) {
    CHECK(after_split->note_type == NoteType::None);
    CHECK(after_split->gimmick_type == GimmickType::Split3);
    CHECK_EQ(after_split->scratch_length, 10120);
    CHECK_EQ(after_split->width, 12);
    CHECK(after_tap->note_type == NoteType::Flick);
  }
}

void test_gutter_label_select_copy_mirror_and_move() {
  using wds::chart_editor::GimmickType;
  {
    Harness h;
    NotationNote split = make_split_effect(0, 480, GimmickType::Split3);
    split.scratch_length = 10120;
    const int32_t split_id = h.engine.add_note(split);
    NotationNote tap;
    tap.note_type = NoteType::Normal;
    tap.start_tick = 0;
    tap.lane = 3;
    tap.width = 2;
    const int32_t tap_id = h.engine.add_note(tap);
    h.panel.sync_to_timeline_ms(0.0);

    const auto hits =
        wds::ui::build_split_label_hits(h.panel.viewport(), h.panel.left_gutter(),
                                        h.engine.document().notes());
    {
      // Index-windowed path (doc given) must equal the full-scan path.
      const auto indexed = wds::ui::build_split_label_hits(
          h.panel.viewport(), h.panel.left_gutter(), h.engine.document().notes(),
          &h.engine.document());
      CHECK_EQ(indexed.size(), hits.size());
      for (size_t i = 0; i < indexed.size() && i < hits.size(); ++i) {
        CHECK_EQ(indexed[i].note_id, hits[i].note_id);
        CHECK_EQ(indexed[i].is_start, hits[i].is_start);
        CHECK_EQ(indexed[i].anchor_tick, hits[i].anchor_tick);
        CHECK_EQ(indexed[i].column, hits[i].column);
      }
    }
    const auto* start = split_hit_for(hits, split_id, true);
    const auto* end = split_hit_for(hits, split_id, false);
    CHECK(start != nullptr && end != nullptr);
    if (!start || !end) return;

    const auto start_c = chip_center(start->bounds);
    h.panel.on_pointer_down(PointerDownEvent{start_c, PointerButton::Left, {}});
    h.panel.on_pointer_up(PointerUpEvent{start_c, PointerButton::Left, {}});
    CHECK(!h.panel.selected().count(split_id));
    ChartEditPanel::SplitModalDraft draft;
    CHECK(h.panel.take_split_modal(draft));
    CHECK_EQ(draft.edit_id, split_id);

    h.panel.on_pointer_down(PointerDownEvent{start_c, PointerButton::Left, primary_mods()});
    h.panel.on_pointer_up(PointerUpEvent{start_c, PointerButton::Left, primary_mods()});
    CHECK(h.panel.selected().count(split_id));
    h.panel.on_pointer_down(PointerDownEvent{chip_center(end->bounds), PointerButton::Left,
                                             primary_mods()});
    h.panel.on_pointer_up(PointerUpEvent{chip_center(end->bounds), PointerButton::Left,
                                         primary_mods()});
    CHECK(!h.panel.selected().count(split_id));

    const Vec2 box_start{h.panel.left_gutter().x + 1.0f, end->bounds.y - 12.0f};
    const Vec2 box_end{end->bounds.right() - 1.0f, end->bounds.bottom() - 1.0f};
    h.panel.on_pointer_down(PointerDownEvent{box_start, PointerButton::Left, primary_mods()});
    h.panel.on_pointer_move(PointerMoveEvent{box_end, primary_mods()});
    h.panel.on_pointer_up(PointerUpEvent{box_end, PointerButton::Left, primary_mods()});
    CHECK(h.panel.selected().count(split_id));
    CHECK(!h.panel.selected().count(tap_id));

    h.panel.set_selected({split_id, tap_id});
    CHECK(h.panel.mirror_selected(false));
    const auto mirrored_split = h.engine.document().find_note(split_id);
    const auto mirrored_tap = h.engine.document().find_note(tap_id);
    CHECK(mirrored_split.has_value() && mirrored_tap.has_value());
    if (mirrored_split && mirrored_tap) {
      CHECK_EQ(mirrored_split->lane, 0);
      CHECK_EQ(mirrored_split->width, 12);
      CHECK_EQ(mirrored_tap->lane, 7);
    }

    h.panel.set_selected({split_id});
    CHECK(h.panel.copy_selected());
    const auto paste_at = h.at_tick_lane(960, 5);
    h.panel.on_pointer_move(PointerMoveEvent{paste_at, {}});
    CHECK(h.panel.paste_at_pointer());
    int split_count = 0;
    for (const auto& n : h.engine.document().notes()) {
      if (wds::chart_editor::is_split_lane_gimmick(n.gimmick_type)) ++split_count;
    }
    CHECK_EQ(split_count, 2);
    CHECK_EQ(static_cast<int>(h.panel.selected().size()), 1);
    for (const int32_t id : h.panel.selected()) {
      const auto pasted = h.engine.document().find_note(id);
      CHECK(pasted.has_value());
      if (pasted) {
        CHECK(wds::chart_editor::is_split_lane_gimmick(pasted->gimmick_type));
        CHECK(pasted->start_tick > 0);
        CHECK_EQ(pasted->end_tick - pasted->start_tick, 480);
      }
    }
  }
  {
    Harness h;
    NotationNote split = make_split_effect(0, 480, GimmickType::Split2);
    const int32_t split_id = h.engine.add_note(split);
    NotationNote tap;
    tap.note_type = NoteType::Normal;
    tap.start_tick = 0;
    tap.lane = 4;
    tap.width = 2;
    const int32_t tap_id = h.engine.add_note(tap);
    h.panel.sync_to_timeline_ms(0.0);
    h.panel.set_selected({split_id, tap_id});
    const auto hits =
        wds::ui::build_split_label_hits(h.panel.viewport(), h.panel.left_gutter(),
                                        h.engine.document().notes());
    const auto* start = split_hit_for(hits, split_id, true);
    CHECK(start != nullptr);
    if (!start) return;
    const auto press = chip_center(start->bounds);
    const auto dest = Vec2{press.x, h.panel.viewport().y_at(240)};
    h.panel.on_pointer_down(PointerDownEvent{press, PointerButton::Left, {}});
    h.panel.on_pointer_move(PointerMoveEvent{dest, {}});
    h.panel.on_pointer_up(PointerUpEvent{dest, PointerButton::Left, {}});
    const auto moved_split = h.engine.document().find_note(split_id);
    const auto moved_tap = h.engine.document().find_note(tap_id);
    CHECK(moved_split.has_value() && moved_tap.has_value());
    if (moved_split && moved_tap) {
      CHECK(moved_split->start_tick > 0);
      CHECK_EQ(moved_split->end_tick - moved_split->start_tick, 480);
      CHECK_EQ(moved_split->lane, 0);
      CHECK_EQ(moved_tap->start_tick, moved_split->start_tick);
      CHECK_EQ(moved_tap->lane, 4);
    }
  }
  {
    Harness h;
    h.panel.set_extended_editing_active(true);
    CHECK(h.panel.apply_scroll_speed(480, 1.5));
    h.panel.sync_to_timeline_ms(0.0);
    const auto hits = wds::ui::build_speed_label_hits(h.panel.viewport(), h.panel.speed_gutter(),
                                                      h.engine.document().scroll_speed_points());
    CHECK(!hits.empty());
    if (hits.empty()) return;
    const auto c = chip_center(hits.front().bounds);
    h.panel.on_pointer_down(PointerDownEvent{c, PointerButton::Left, {}});
    h.panel.on_pointer_up(PointerUpEvent{c, PointerButton::Left, {}});
    CHECK(h.panel.selected_speed_ticks().empty());
    ChartEditPanel::SpeedModalDraft speed;
    CHECK(h.panel.take_speed_modal(speed));

    h.panel.on_pointer_down(PointerDownEvent{c, PointerButton::Left, primary_mods()});
    h.panel.on_pointer_up(PointerUpEvent{c, PointerButton::Left, primary_mods()});
    CHECK(h.panel.selected_speed_ticks().count(480));
    CHECK(h.panel.copy_selected());
    const auto paste_at = h.at_tick_lane(960, 3);
    h.panel.on_pointer_move(PointerMoveEvent{paste_at, {}});
    CHECK(h.panel.paste_at_pointer());
    CHECK_EQ(static_cast<int>(h.engine.document().scroll_speed_points().size()), 2);
    CHECK(!h.panel.mirror_selected(false));
    CHECK(!h.panel.convert_selected(NoteType::Flick, 1));
  }
}

void test_paste_mirror_is_one_history_step_and_skips_splits() {
  using wds::chart_editor::GimmickType;
  Harness h;
  NotationNote tap;
  tap.note_type = NoteType::Normal;
  tap.start_tick = 0;
  tap.lane = 3;
  tap.width = 2;
  const int32_t tap_id = h.engine.add_note(tap);
  NotationNote split = make_split_effect(0, 480, GimmickType::Split3);
  const int32_t split_id = h.engine.add_note(split);
  h.panel.set_selected({tap_id, split_id});
  CHECK(h.panel.copy_selected());
  h.panel.on_pointer_move(PointerMoveEvent{h.at_tick_lane(960, 3), {}});
  CHECK(!h.engine.history().can_undo());
  CHECK(h.panel.paste_at_pointer(true));
  CHECK(h.engine.history().can_undo());

  int tap_count = 0;
  int split_count = 0;
  int32_t pasted_tap_lane = -1;
  int32_t pasted_split_lane = -1;
  for (const auto& n : h.engine.document().notes()) {
    if (n.note_type == NoteType::Normal) {
      ++tap_count;
      if (n.id == tap_id) {
        CHECK_EQ(n.lane, 3);
      } else {
        pasted_tap_lane = n.lane;
      }
    }
    if (wds::chart_editor::is_split_lane_gimmick(n.gimmick_type)) {
      ++split_count;
      if (n.id == split_id) {
        CHECK_EQ(n.lane, 0);
        CHECK_EQ(n.width, 12);
      } else {
        pasted_split_lane = n.lane;
      }
    }
  }
  CHECK_EQ(tap_count, 2);
  CHECK_EQ(split_count, 2);
  CHECK_EQ(pasted_tap_lane, 7);
  CHECK_EQ(pasted_split_lane, 0);

  CHECK(h.engine.undo());
  CHECK(!h.engine.history().can_undo());
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Normal), 1);
  int splits_after_undo = 0;
  for (const auto& n : h.engine.document().notes()) {
    if (wds::chart_editor::is_split_lane_gimmick(n.gimmick_type)) ++splits_after_undo;
  }
  CHECK_EQ(splits_after_undo, 1);
  const auto original_tap = h.engine.document().find_note(tap_id);
  CHECK(original_tap.has_value());
  if (original_tap) CHECK_EQ(original_tap->lane, 3);
}

// Paste follows the pointer in both directions: the clipboard's earliest item lands
// under the pointer even when that is above the copied source, and only the parts
// that would leave the allowed range (music not started, negative tick) are pushed.
void test_paste_can_move_backwards() {
  {  // Hold + bound star keep their internal offsets when pasted above the source.
    Harness h;
    const int32_t hold_id = add_hold_body(h, NoteType::Hold, 960, 1440, 3, 1);
    const int32_t star_id = add_bound_star(h, hold_id, 1200, 3, 1);
    regenerate_eighths(h, hold_id);

    h.panel.set_selected({hold_id});
    CHECK(h.panel.copy_selected());
    h.panel.on_pointer_move(PointerMoveEvent{h.at_tick_lane(480, 3), {}});
    CHECK(h.panel.paste_at_pointer());
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 2);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Sound), 2);

    int32_t pasted_hold_id = -1;
    for (const auto& n : h.engine.document().notes()) {
      if (n.note_type == NoteType::Hold && n.id != hold_id) pasted_hold_id = n.id;
    }
    CHECK(pasted_hold_id >= 0);
    const auto pasted_hold = h.engine.document().find_note(pasted_hold_id);
    CHECK(pasted_hold.has_value());
    if (pasted_hold) {
      CHECK_EQ(pasted_hold->start_tick, 480);
      CHECK_EQ(pasted_hold->end_tick, 960);
    }
    const auto original_hold = h.engine.document().find_note(hold_id);
    CHECK(original_hold.has_value());
    if (original_hold) {
      CHECK_EQ(original_hold->start_tick, 960);
      CHECK_EQ(original_hold->end_tick, 1440);
    }
    int32_t pasted_star_tick = -1;
    for (const auto& n : h.engine.document().notes()) {
      if (n.note_type != NoteType::Sound || n.id == star_id) continue;
      CHECK_EQ(n.parent_hold_id, pasted_hold_id);
      pasted_star_tick = n.start_tick;
    }
    CHECK_EQ(pasted_star_tick, 720);
  }

  {  // Destination above the music start: shift to the first legal tick, never negative.
    Harness h;
    CHECK(h.engine.document().set_offset_ms(-2000));  // Music starts at tick 1920.
    NotationNote tap;
    tap.note_type = NoteType::Normal;
    tap.start_tick = 2880;
    tap.lane = 4;
    tap.width = 1;
    const int32_t tap_id = h.engine.add_note(tap);
    CHECK(tap_id >= 0);

    h.panel.set_selected({tap_id});
    CHECK(h.panel.copy_selected());
    h.panel.sync_to_timeline_ms(-1500.0);  // Show the leading blank above the music.
    h.panel.on_pointer_move(PointerMoveEvent{h.at_tick_lane(480, 4), {}});
    CHECK(h.panel.paste_at_pointer());
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Normal), 2);
    int32_t pasted_tick = -1;
    for (const auto& n : h.engine.document().notes()) {
      if (n.note_type != NoteType::Normal || n.id == tap_id) continue;
      pasted_tick = n.start_tick;
    }
    CHECK_EQ(pasted_tick, 1920);
  }

  {  // Mirror paste shares the same backward anchor.
    Harness h;
    NotationNote tap;
    tap.note_type = NoteType::Normal;
    tap.start_tick = 1440;
    tap.lane = 3;
    tap.width = 2;
    const int32_t tap_id = h.engine.add_note(tap);

    h.panel.set_selected({tap_id});
    CHECK(h.panel.copy_selected());
    h.panel.on_pointer_move(PointerMoveEvent{h.at_tick_lane(960, 3), {}});
    CHECK(h.panel.paste_at_pointer(true));
    int32_t pasted_tick = -1;
    int32_t pasted_lane = -1;
    for (const auto& n : h.engine.document().notes()) {
      if (n.note_type != NoteType::Normal || n.id == tap_id) continue;
      pasted_tick = n.start_tick;
      pasted_lane = n.lane;
    }
    CHECK_EQ(pasted_tick, 960);
    CHECK_EQ(pasted_lane, 7);
  }

  {  // Scroll speed points follow the same backward delta.
    Harness h;
    h.panel.set_extended_editing_active(true);
    CHECK(h.panel.apply_scroll_speed(960, 1.5));
    h.panel.sync_to_timeline_ms(0.0);
    const auto hits = wds::ui::build_speed_label_hits(h.panel.viewport(), h.panel.speed_gutter(),
                                                      h.engine.document().scroll_speed_points());
    CHECK(!hits.empty());
    if (hits.empty()) return;
    const auto c = chip_center(hits.front().bounds);
    h.panel.on_pointer_down(PointerDownEvent{c, PointerButton::Left, primary_mods()});
    h.panel.on_pointer_up(PointerUpEvent{c, PointerButton::Left, primary_mods()});
    CHECK(h.panel.selected_speed_ticks().count(960));
    CHECK(h.panel.copy_selected());
    h.panel.on_pointer_move(PointerMoveEvent{h.at_tick_lane(480, 3), {}});
    CHECK(h.panel.paste_at_pointer());
    std::vector<int32_t> ticks;
    for (const auto& p : h.engine.document().scroll_speed_points()) ticks.push_back(p.tick);
    std::sort(ticks.begin(), ticks.end());
    CHECK_EQ(ticks.size(), 2u);
    if (ticks.size() == 2) {
      CHECK_EQ(ticks[0], 480);
      CHECK_EQ(ticks[1], 960);
    }
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

using wds::interaction::PlaceIntent;

void toolbox_click(Harness& h, PlaceIntent intent, int32_t tick, int32_t lane,
                   Modifiers mods = {}) {
  h.panel.set_toolbox_place_mode(true);
  h.panel.set_place_intent_override(intent);
  const auto p = h.at_tick_lane(tick, lane);
  h.panel.on_pointer_move(PointerMoveEvent{p, mods});
  h.panel.on_pointer_down(PointerDownEvent{p, PointerButton::Left, mods});
  h.panel.on_pointer_up(PointerUpEvent{p, PointerButton::Left, mods});
}

const NotationNote* last_of_type(const std::vector<NotationNote>& notes, NoteType type) {
  const NotationNote* found = nullptr;
  for (const auto& n : notes) {
    if (n.note_type == type) found = &n;
  }
  return found;
}

void test_toolbox_left_click_places_selected_type_and_ignores_swipe() {
  {
    Harness h;
    toolbox_click(h, PlaceIntent::Tap, 240, 3);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Normal), 1);
  }
  {
    Harness h;
    toolbox_click(h, PlaceIntent::ExTap, 240, 3);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Critical), 1);
  }
  {
    Harness h;
    toolbox_click(h, PlaceIntent::Flick, 240, 3);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Flick), 1);
  }
  {
    Harness h;
    h.panel.set_toolbox_place_mode(true);
    h.panel.set_place_intent_override(PlaceIntent::Tap);
    const auto origin = h.at_tick_lane(240, 3);
    h.panel.on_pointer_down(PointerDownEvent{origin, PointerButton::Left, {}});
    h.panel.on_pointer_move(PointerMoveEvent{{origin.x - 80.0f, origin.y}, {}});
    h.panel.on_pointer_up(PointerUpEvent{{origin.x - 80.0f, origin.y}, PointerButton::Left, {}});
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Normal), 1);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Critical), 0);
  }
  {
    Harness h;
    h.panel.set_toolbox_place_mode(true);
    h.panel.set_place_intent_override(PlaceIntent::Tap);
    const auto before = snapshot_doc(h.engine);
    const auto p = h.at_tick_lane(240, 3);
    h.panel.on_pointer_down(PointerDownEvent{p, PointerButton::Right, {}});
    h.panel.on_pointer_up(PointerUpEvent{p, PointerButton::Right, {}});
    CHECK(snap_eq(before, h.engine));
  }
}

void test_toolbox_hold_click_min_length_equal_tail_and_gold() {
  {
    Harness h;
    toolbox_click(h, PlaceIntent::HoldBody, 0, 3);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 1);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldStart), 1);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Normal), 0);
    const auto* body = last_of_type(h.engine.document().notes(), NoteType::Hold);
    CHECK(body != nullptr);
    if (body != nullptr) {
      CHECK_EQ(body->end_tick - body->start_tick, 120);
      CHECK_EQ(body->scratch_length, 0);
      CHECK_EQ(body->width, 1);
    }
  }
  {
    Harness h;
    toolbox_click(h, PlaceIntent::ScratchHoldBody, 0, 3);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHold), 1);
    CHECK(count_type(h.engine.document().notes(), NoteType::ScratchHoldStart) >= 1);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Flick), 0);
    const auto* body = last_of_type(h.engine.document().notes(), NoteType::ScratchHold);
    CHECK(body != nullptr);
    if (body != nullptr) {
      CHECK_EQ(body->end_tick - body->start_tick, 120);
      CHECK_EQ(body->scratch_length, 0);
    }
  }
  {
    Harness h;
    toolbox_click(h, PlaceIntent::HoldBody, 0, 3, shift_mods());
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::CriticalHold), 1);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 0);
  }
}

void test_toolbox_hold_ghost_follows_shift() {
  Harness h;
  h.panel.set_toolbox_place_mode(true);
  h.panel.set_place_intent_override(PlaceIntent::HoldBody);
  const auto p = h.at_tick_lane(240, 3);
  h.panel.on_pointer_move(PointerMoveEvent{p, {}});
  bool saw_hold = false;
  for (const auto& ghost : h.panel.skinned_ghosts()) {
    if (ghost.note.note_type == NoteType::Hold) saw_hold = true;
  }
  CHECK(saw_hold);
  h.panel.on_pointer_move(PointerMoveEvent{p, shift_mods()});
  bool saw_gold = false;
  for (const auto& ghost : h.panel.skinned_ghosts()) {
    if (ghost.note.note_type == NoteType::CriticalHold) saw_gold = true;
  }
  CHECK(saw_gold);
}

void test_extended_speed_gutter_slider_and_nontail() {
  CHECK(wds::ui::lane_guide_visible(0, 3.0f));
  CHECK(wds::ui::lane_guide_visible(12, 3.0f));
  CHECK(!wds::ui::lane_guide_visible(1, 3.0f));
  CHECK(wds::ui::lane_guide_thickness(0) > wds::ui::lane_guide_thickness(1));
  {
    Harness h;
    CHECK(std::fabs(h.panel.speed_gutter().w) < 0.5f);
    CHECK(std::fabs(h.panel.playfield().w - 648.0f) < 0.5f);
    h.panel.set_extended_editing_active(true);
    CHECK(std::fabs(h.panel.speed_gutter().w - 52.0f) < 0.5f);
    CHECK(std::fabs(h.panel.playfield().w - 596.0f) < 0.5f);
    CHECK(std::fabs(h.panel.speed_gutter().x + h.panel.speed_gutter().w - 708.0f) < 0.5f);
    h.panel.set_extended_editing_active(false);
    CHECK(std::fabs(h.panel.speed_gutter().w) < 0.5f);
    CHECK(std::fabs(h.panel.playfield().w - 648.0f) < 0.5f);
  }
  {
    Harness h;
    h.panel.set_extended_editing_active(true);
    const auto p = h.at_tick_lane(240, 3);
    h.panel.on_pointer_move(PointerMoveEvent{p, {}});
    CHECK(!h.panel.skinned_ghosts().empty());
    h.panel.set_lane_slider_hovered(true);
    h.panel.on_pointer_move(PointerMoveEvent{p, {}});
    CHECK(h.panel.skinned_ghosts().empty());
    h.panel.set_lane_slider_hovered(false);
    h.panel.on_pointer_move(PointerMoveEvent{p, {}});
    CHECK(!h.panel.skinned_ghosts().empty());
  }
  {
    Harness h;
    h.panel.set_extended_editing_active(true);
    CHECK(wds::ui::format_speed_label(1.5) == "1.5x");
    CHECK(wds::ui::format_speed_label(0.0) == "0x");
    const auto sg = h.panel.speed_gutter();
    const float y = h.panel.viewport().y_at(480);
    h.panel.on_pointer_move(PointerMoveEvent{{sg.x + sg.w * 0.5f, y}, {}});
    CHECK_EQ(static_cast<int>(h.panel.gutter_ghost_count()), 1);
    h.panel.on_pointer_down(PointerDownEvent{{sg.x + sg.w * 0.5f, y}, PointerButton::Left, {}});
    ChartEditPanel::SpeedModalDraft speed;
    CHECK(h.panel.take_speed_modal(speed));
    CHECK(h.panel.apply_scroll_speed(speed.tick, 0.0));
    CHECK_EQ(static_cast<int>(h.engine.document().scroll_speed_points().size()), 1);
    CHECK(std::fabs(h.engine.document().scroll_speed_points().front().multiplier) < 1e-9);
    CHECK(h.panel.apply_scroll_speed(speed.tick, 1.5));
    CHECK_EQ(static_cast<int>(h.engine.document().scroll_speed_points().size()), 1);
    CHECK(h.engine.document().compatibility_profile() == ChartCompatibilityProfile::SiriusExtended);
    CHECK(std::fabs(h.engine.document().scroll_speed_points().front().multiplier - 1.5) < 1e-9);
    const auto hits = wds::ui::build_speed_label_hits(h.panel.viewport(), h.panel.speed_gutter(),
                                                      h.engine.document().scroll_speed_points());
    CHECK(!hits.empty());
    const float cx = hits.front().bounds.x + hits.front().bounds.w * 0.5f;
    const float cy = hits.front().bounds.y + hits.front().bounds.h * 0.5f;
    h.panel.on_pointer_down(PointerDownEvent{{cx, cy}, PointerButton::Middle, {}});
    CHECK(h.engine.document().scroll_speed_points().empty());
  }
  {
    Harness h;
    h.enter_hold(false, 0, 480, 3);
    h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(480, 3), PointerButton::Left, {}});
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 1);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldStart), 1);
    h.panel.set_extended_editing_active(true);
    // Select the body first so post-convert selection still covers the ribbon.
    h.panel.on_pointer_down(
        PointerDownEvent{h.at_tick_lane(240, 3), PointerButton::Left, {}});
    h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(240, 3), PointerButton::Left, {}});
    CHECK(h.panel.selected().size() >= 1);
    // Nontail holds derive the same eighths as tailed ones: converting must keep the
    // identical eighth set (regenerated, not cleared) inside the single undo step.
    int32_t tailed_id = -1;
    for (const auto& n : h.engine.document().notes()) {
      if (n.note_type == NoteType::Hold) tailed_id = n.id;
    }
    const auto eighths_tailed = eighth_ticks_of(h.engine.document().notes(), tailed_id);
    CHECK(!eighths_tailed.empty());
    h.panel.on_pointer_down(
        PointerDownEvent{h.at_tick_lane(480, 3), PointerButton::Middle, {}});
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 0);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::NontailHold), 1);
    // Deleting the tail must not invent a second HoldStart — only convert the body.
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldStart), 1);
    CHECK(h.engine.document().compatibility_profile() == ChartCompatibilityProfile::SiriusExtended);
    // Body id stays selected so the selection box can span the nontail ribbon.
    int32_t nontail_id = -1;
    for (const auto& n : h.engine.document().notes()) {
      if (n.note_type == NoteType::NontailHold) nontail_id = n.id;
    }
    CHECK(nontail_id >= 0);
    CHECK(h.panel.selected().count(nontail_id) != 0);
    CHECK(eighth_ticks_of(h.engine.document().notes(), nontail_id) == eighths_tailed);
    CHECK(h.engine.undo());
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Hold), 1);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::NontailHold), 0);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldStart), 1);
    CHECK(eighth_ticks_of(h.engine.document().notes(), tailed_id) == eighths_tailed);
    CHECK_EQ(count_type(h.engine.document().notes(), NoteType::HoldEighth),
             static_cast<int>(eighths_tailed.size()));
  }
}

void test_toolbox_scratch_hold_uses_regular_hold_buttons() {
  Harness h;
  h.panel.set_toolbox_place_mode(true);
  h.panel.set_place_intent_override(PlaceIntent::ScratchHoldBody);
  const auto origin = h.at_tick_lane(0, 3);
  const auto mid = h.at_tick_lane(480, 3);
  const auto tail = h.at_tick_lane(960, 4);
  h.panel.on_pointer_down(PointerDownEvent{origin, PointerButton::Left, {}});
  h.panel.on_pointer_move(PointerMoveEvent{mid, {}});
  h.panel.on_pointer_down(PointerDownEvent{mid, PointerButton::Right, {}});
  h.panel.on_pointer_move(PointerMoveEvent{tail, {}});
  h.panel.on_pointer_up(PointerUpEvent{tail, PointerButton::Left, {}});
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::ScratchHold), 2);
  CHECK_EQ(count_type(h.engine.document().notes(), NoteType::Flick), 0);
}

// ---------------------------------------------------------------------------
// Hit-test windowing: oracle = full O(N) scan of hit_test_note's rules
// (selection first between unrelated notes, then distance / priority / draw order).
// ---------------------------------------------------------------------------

using wds::ui::ChartEditPanelTestAccess;

std::optional<NotationNote> oracle_hit_test(const ChartEditPanel& panel, Vec2 point) {
  namespace ce = wds::chart_editor;
  const auto& viewport = panel.viewport();
  const auto& doc = panel.engine().document();
  const auto& selected = panel.selected();
  auto is_star = [](NoteType t) { return t == NoteType::Sound || t == NoteType::ScratchSound; };
  // Mirrors ChartEditPanel::hold_body_head_covers_x: a body with no paired head
  // (or a narrower auto head that does not cover x) has no start sprite there.
  auto head_covers = [&](const NotationNote& body, float x) {
    if (!ce::is_bindable_hold_body(body.note_type) || body.end_tick <= body.start_tick) {
      return false;
    }
    const auto head = ce::paired_hold_head_for(doc, body);
    if (!head) return false;
    const float hx0 = viewport.x_at(head->lane);
    const float hx1 = hx0 + viewport.lane_width(std::max(1, head->width));
    return x >= hx0 && x <= hx1;
  };

  std::optional<NotationNote> best;
  bool best_selected = false;
  float best_dist = 1e9f;
  int best_priority = 0;
  int64_t best_start = 0;
  int32_t best_type = 0;
  // Mirrors ChartEditPanel::same_hold_object: body / paired head / bound star of
  // one hold, or connected ScratchHold chain segments.
  auto resolve_body = [&](const NotationNote& note) -> std::optional<NotationNote> {
    if (ce::is_bindable_hold_body(note.note_type)) return note;
    if (ce::is_hold_head_note(note)) return ce::paired_hold_body_for(doc, note);
    if (is_star(note.note_type)) return ce::parent_hold_for(doc, note);
    return std::nullopt;
  };
  auto same_hold_object = [&](const NotationNote& a, const NotationNote& b) {
    if (a.id == b.id) return true;
    const auto a_body = resolve_body(a);
    const auto b_body = resolve_body(b);
    if (!a_body || !b_body) return false;
    if (a_body->id == b_body->id) return true;
    if (!ce::is_hold_chain_body(a_body->note_type) ||
        !ce::is_hold_chain_body(b_body->note_type)) {
      return false;
    }
    NotationNote cur = *a_body;
    for (int guard = 0; guard < 64; ++guard) {
      auto prev = ce::chained_prev_scratch_hold(doc, cur);
      if (!prev) break;
      cur = *prev;
    }
    for (int guard = 0; guard < 64; ++guard) {
      if (cur.id == b_body->id) return true;
      auto next = ce::chained_next_scratch_hold(doc, cur);
      if (!next) break;
      cur = *next;
    }
    return false;
  };
  auto consider = [&](float dist, int priority, const NotationNote& note) {
    const bool is_selected = selected.count(note.id) != 0;
    const int64_t start = static_cast<int64_t>(note.start_tick);
    const int32_t type = static_cast<int32_t>(note.note_type);
    if (best.has_value() && is_selected != best_selected &&
        !same_hold_object(note, *best)) {
      if (!is_selected) return;
    } else {
      bool better = dist < best_dist - 0.5f;
      if (!better && std::abs(dist - best_dist) <= 0.5f) {
        if (priority > best_priority) {
          better = true;
        } else if (priority == best_priority && best.has_value()) {
          better = wds::chart_render::is_visually_above(start, type, best_start, best_type);
        }
      }
      if (!better) return;
    }
    best_dist = dist;
    best_priority = priority;
    best_start = start;
    best_type = type;
    best = note;
    best_selected = is_selected;
  };

  for (const auto& note : doc.notes()) {
    if (ce::is_split_lane_gimmick(note.gimmick_type)) continue;
    if (note.note_type == NoteType::HoldEighth) continue;
    if (!viewport.lane_span_visible(note.lane, note.width)) {
      if (!(ce::is_hold_chain_body(note.note_type) && note.end_tick > note.start_tick)) continue;
    }
    if (is_star(note.note_type)) {
      const auto star = viewport.mid_star_screen_rect(note);
      if (!star.contains(point)) continue;
      const float dist = std::hypot(point.x - (star.x + star.w * 0.5f),
                                    point.y - (star.y + star.h * 0.5f));
      consider(dist, 4, note);
      continue;
    }
    const float y0 = viewport.y_at(note.start_tick);
    const float y1 = note.end_tick > note.start_tick ? viewport.y_at(note.end_tick) : y0;
    const float pad = viewport.note_height_px() * 0.55f;

    int32_t hit_lane = note.lane;
    int32_t hit_width = note.width;
    bool in_scratch_end_zone = false;
    if (ce::is_hold_chain_body(note.note_type) && note.end_tick > note.start_tick) {
      const auto end = ce::get_scratch_end_lane_range(note);
      const float end_y = viewport.y_at(note.end_tick);
      if (std::abs(point.y - end_y) <= pad) {
        hit_lane = std::min(hit_lane, end.first);
        hit_width = std::max(note.end_lane(), end.second) - hit_lane + 1;
        in_scratch_end_zone = true;
      }
    }
    const float x0 = viewport.x_at(hit_lane);
    const float x1 = x0 + viewport.lane_width(hit_width);
    if (point.x < x0 || point.x > x1) continue;

    const bool body_here =
        ce::is_bindable_hold_body(note.note_type) && note.end_tick > note.start_tick;
    const bool start_side = std::abs(point.y - y0) <= std::abs(point.y - y1);
    const bool head_covers_point = body_here && start_side && head_covers(note, point.x);
    const float start_pad = std::max(2.0f, viewport.note_height_px() * 0.12f);
    float top = std::min(y0, y1) - pad;
    float bottom = std::max(y0, y1) + pad;
    if (body_here && start_side && !head_covers_point) {
      if (y0 >= y1) {
        bottom = std::max(y0, y1) + start_pad;
      } else {
        top = std::min(y0, y1) - start_pad;
      }
    }
    if (point.y < top || point.y > bottom) continue;

    float dist;
    int priority;
    if (in_scratch_end_zone) {
      dist = std::abs(point.y - viewport.y_at(note.end_tick));
      const bool next_segment_side =
          point.y < viewport.y_at(note.end_tick) && ce::chained_next_scratch_hold(doc, note).has_value();
      priority = next_segment_side ? -8 : 3;
    } else {
      dist = std::abs(point.y - y0);
      if (ce::is_hold_head_note(note)) {
        priority = 2;
        if (point.y > y0) {
          const auto body = ce::paired_hold_body_for(doc, note);
          if (body && ce::is_hold_chain_body(body->note_type) &&
              ce::chained_prev_scratch_hold(doc, *body)) {
            priority = -8;
          }
        }
      } else if (body_here) {
        priority = (start_side && !head_covers_point) ? -9 : 1;
      } else {
        priority = 0;
      }
    }
    consider(dist, priority, note);
  }
  return best;
}

struct HitCompare {
  int compared = 0;
  int hits = 0;
  int mismatches = 0;
};

void compare_hit(const ChartEditPanel& panel, Vec2 point, HitCompare& out) {
  const auto want = oracle_hit_test(panel, point);
  const auto got = ChartEditPanelTestAccess::hit_test_note(panel, point);
  ++out.compared;
  if (want.has_value()) ++out.hits;
  const bool same = want.has_value() == got.has_value() && (!want || want->id == got->id);
  if (!same) {
    ++out.mismatches;
    if (out.mismatches <= 5) {
      std::fprintf(stderr, "hit mismatch at (%.1f, %.1f): want id %d got id %d\n", point.x,
                   point.y, want ? want->id : -1, got ? got->id : -1);
    }
  }
}

// Pointer sweep: a coarse grid plus points around every on-screen note's head / tail.
void sweep_hit_points(Harness& h, HitCompare& out) {
  const auto& vp = h.panel.viewport();
  const auto& b = vp.bounds();
  for (float y = b.y + 1.0f; y < b.y + b.h; y += 9.0f) {
    for (int32_t lane = 0; lane < 12; ++lane) {
      const float cx = vp.x_at(lane) + vp.lane_width(1) * 0.5f;
      compare_hit(h.panel, {cx, y}, out);
      compare_hit(h.panel, {cx - vp.lane_width(1) * 0.45f, y + 2.5f}, out);
    }
  }
  for (const auto& note : h.engine.document().notes()) {
    for (const int32_t tick : {note.start_tick, note.end_tick}) {
      const float y = vp.y_at(tick);
      if (y < b.y - 40.0f || y > b.y + b.h + 40.0f) continue;
      const float x0 = vp.x_at(note.lane);
      const float x1 = x0 + vp.lane_width(std::max(1, note.width));
      for (const float dy : {-6.0f, -1.0f, 0.0f, 1.0f, 6.0f}) {
        compare_hit(h.panel, {(x0 + x1) * 0.5f, y + dy}, out);
        compare_hit(h.panel, {x0 + 1.0f, y + dy}, out);
        compare_hit(h.panel, {x1 - 1.0f, y + dy}, out);
      }
    }
  }
}

void test_hit_test_windowed_scan_matches_full_scan_on_dense_chart() {
  using wds::chart_editor::GimmickType;
  Harness h;
  // Two tempo points so tick <-> y is not linear.
  MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  timing.points = {TimingPoint{0, 120.0, 4, 4, true, true},
                   TimingPoint{9600, 200.0, 4, 4, true, true},
                   TimingPoint{30720, 90.0, 4, 4, true, true}};
  h.engine.document().set_timing(timing);

  std::uint32_t rng = 12345u;
  auto next = [&rng](std::uint32_t mod) {
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) % mod;
  };
  // Random lanes/widths can be rejected by the engine (e.g. past the last
  // lane); this sweep only needs whatever the engine accepted, so unlike
  // add_hold_body it must not CHECK the id.
  auto try_add_body = [&h](NoteType type, int32_t start, int32_t end, int32_t lane,
                           int32_t width) {
    NotationNote body;
    body.note_type = type;
    body.start_tick = start;
    body.end_tick = end;
    body.lane = lane;
    body.width = width;
    if (type == NoteType::ScratchHold) {
      body.gimmick_type = GimmickType::JumpScratch;
      wds::chart_editor::set_scratch_hold_end_lanes(body, lane, lane + width - 1);
    }
    return h.engine.add_note(body);
  };
  std::vector<int32_t> hold_ids;
  for (int i = 0; i < 700; ++i) {
    const int32_t tick = static_cast<int32_t>(next(60000u));
    const int32_t lane = static_cast<int32_t>(next(11u));
    NotationNote n;
    n.start_tick = tick;
    n.end_tick = tick;
    n.lane = lane;
    n.width = 1 + static_cast<int32_t>(next(3u));
    switch (next(5u)) {
      case 0:
      case 1: {  // plain taps (heavily overlapping lanes / ticks)
        n.note_type = next(2u) == 0 ? NoteType::Normal : NoteType::Critical;
        h.engine.add_note(n);
        break;
      }
      case 2: {  // regular hold body with head; some very long
        const int32_t len = next(6u) == 0 ? 20000 + static_cast<int32_t>(next(30000u))
                                          : 240 + static_cast<int32_t>(next(2400u));
        const int32_t id = try_add_body(NoteType::Hold, tick, tick + len, lane, n.width);
        if (id >= 0) hold_ids.push_back(id);
        NotationNote head = n;
        head.note_type = NoteType::HoldStart;
        h.engine.add_note(head);
        break;
      }
      case 3: {  // scratch hold chain: body A, then body B starting at A's end
        const int32_t len = 480 + static_cast<int32_t>(next(1900u));
        const int32_t a = try_add_body(NoteType::ScratchHold, tick, tick + len, lane, n.width);
        const int32_t lane_b = std::clamp(lane + static_cast<int32_t>(next(3u)) - 1, 0, 10);
        try_add_body(NoteType::ScratchHold, tick + len, tick + len + 960, lane_b, n.width);
        if (a >= 0) hold_ids.push_back(a);
        break;
      }
      default: {  // mid-star bound to an earlier hold, plus a flick
        if (!hold_ids.empty()) {
          const auto parent = h.engine.document().find_note(
              hold_ids[next(static_cast<std::uint32_t>(hold_ids.size()))]);
          if (parent && parent->end_tick > parent->start_tick) {
            add_bound_star(h, parent->id, (parent->start_tick + parent->end_tick) / 2,
                           parent->lane, parent->width);
          }
        }
        n.note_type = NoteType::Flick;
        h.engine.add_note(n);
        break;
      }
    }
  }
  // Long split effects (skipped by the hit test, but must not disturb it).
  h.engine.add_note(make_split_effect(0, 55000, GimmickType::Split3));
  h.engine.add_note(make_split_effect(12000, 40000, GimmickType::Split2));
  CHECK(h.engine.document().notes().size() > 400u);

  HitCompare total;
  const double scrolls[] = {0.0, 1500.0, 7000.0, 13000.0, 21000.0, 30000.0, 44000.0, 58000.0};
  for (const bool with_selection : {false, true}) {
    if (with_selection) {
      std::unordered_set<int32_t> ids;
      for (const auto& note : h.engine.document().notes()) {
        if (next(4u) == 0) ids.insert(note.id);
      }
      h.panel.set_selected(ids);
    }
    for (const double ms : scrolls) {
      h.panel.sync_to_timeline_ms(ms);
      sweep_hit_points(h, total);
    }
  }
  std::fprintf(stderr, "hit_test sweep: %d points, %d hits, %d mismatches\n", total.compared,
               total.hits, total.mismatches);
  CHECK(total.compared > 5000);
  CHECK(total.hits > 300);
  CHECK_EQ(total.mismatches, 0);

  // Cache invalidation: lengthen a short hold into a chart-spanning one, then
  // shorten it again; the windowed scan must follow the content generation.
  h.panel.clear_selection();
  const auto victim = [&]() -> std::optional<NotationNote> {
    for (const auto& note : h.engine.document().notes()) {
      if (note.note_type == NoteType::Hold && note.end_tick - note.start_tick < 3000 &&
          note.start_tick < 30000) {
        return note;
      }
    }
    return std::nullopt;
  }();
  if (victim) {
    HitCompare cache;
    const double probe_ms =
        static_cast<double>(wds::chart_editor::tick_to_milliseconds(45000, timing));
    h.panel.sync_to_timeline_ms(probe_ms);
    sweep_hit_points(h, cache);  // warms the span cache
    NotationNote longer = *victim;
    longer.end_tick = 58000;
    CHECK(h.engine.document().update_note(victim->id, longer));
    h.panel.sync_to_timeline_ms(probe_ms);
    sweep_hit_points(h, cache);
    const Vec2 probe = {h.panel.viewport().x_at(longer.lane) + 2.0f,
                        h.panel.viewport().y_at(45000)};
    const auto hit = ChartEditPanelTestAccess::hit_test_note(h.panel, probe);
    CHECK(hit.has_value());  // the lengthened hold (or something on top of it) is found
    compare_hit(h.panel, probe, cache);
    longer.end_tick = victim->end_tick;
    CHECK(h.engine.document().update_note(victim->id, longer));
    sweep_hit_points(h, cache);
    CHECK_EQ(cache.mismatches, 0);
  } else {
    CHECK(false);
  }
}

void test_split_placement_window_matches_full_scan() {
  using wds::chart_editor::GimmickType;
  Harness h;
  h.panel.set_split_width_follow(true);
  h.panel.set_default_width(1);
  const GimmickType kinds[] = {GimmickType::Split2, GimmickType::Split3, GimmickType::Split2,
                               GimmickType::Split3};
  std::uint32_t rng = 777u;
  auto next = [&rng](std::uint32_t mod) {
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) % mod;
  };
  for (int i = 0; i < 60; ++i) {
    const int32_t start = static_cast<int32_t>(next(40000u));
    const int32_t len = next(5u) == 0 ? 15000 : 240 + static_cast<int32_t>(next(3000u));
    h.engine.add_note(make_split_effect(start, start + len, kinds[next(4u)]));
  }
  for (int i = 0; i < 100; ++i) {  // non-split noise sorted between the splits
    NotationNote n;
    n.note_type = NoteType::Normal;
    n.start_tick = static_cast<int32_t>(next(40000u));
    n.end_tick = n.start_tick;
    n.lane = static_cast<int32_t>(next(11u));
    n.width = 1;
    h.engine.add_note(n);
  }

  int compared = 0;
  int mismatches = 0;
  int narrowed = 0;
  for (const double ms : {0.0, 4000.0, 9000.0, 17000.0, 26000.0, 34000.0}) {
    h.panel.sync_to_timeline_ms(ms);
    const auto& vp = h.panel.viewport();
    const auto& b = vp.bounds();
    for (float y = b.y + 1.0f; y < b.y + b.h; y += 11.0f) {
      for (int32_t lane = 0; lane < 12; ++lane) {
        const Vec2 point{vp.x_at(lane) + vp.lane_width(1) * 0.5f, y};
        // Oracle: original full scan over every split effect.
        const int32_t tick = vp.tick_at(y);
        std::vector<int32_t> mids;
        std::vector<int32_t> one;
        bool any = false;
        for (const auto& note : h.engine.document().notes()) {
          if (!wds::chart_editor::is_split_lane_gimmick(note.gimmick_type)) continue;
          const int32_t start = note.start_tick;
          const int32_t end = std::max(start, note.end_tick);
          if (tick >= start && tick <= end) {
            any = true;
            wds::ui::split_boundaries_12(wds::chart_editor::get_split_count(note.gimmick_type), one);
            mids.insert(mids.end(), one.begin(), one.end());
          }
        }
        int32_t want_lane = vp.lane_at(point.x, 1);
        int32_t want_width = 1;
        bool followed = false;
        if (any) {
          followed = wds::ui::split_track_between_lines(mids, vp.grid().lane_count,
                                                       vp.lane_at(point.x, 1), want_lane,
                                                       want_width);
        }
        if (!followed) {
          want_width = h.panel.default_width();
          want_lane = vp.lane_at(point.x, want_width);
        }
        int32_t got_lane = -1;
        int32_t got_width = -1;
        ChartEditPanelTestAccess::placement(h.panel, point, got_lane, got_width);
        ++compared;
        if (followed) ++narrowed;
        if (got_lane != want_lane || got_width != want_width) {
          ++mismatches;
          if (mismatches <= 5) {
            std::fprintf(stderr, "placement mismatch at (%.1f, %.1f): want %d/%d got %d/%d\n",
                         point.x, point.y, want_lane, want_width, got_lane, got_width);
          }
        }
      }
    }
  }
  CHECK(compared > 500);
  CHECK(narrowed > 50);
  CHECK_EQ(mismatches, 0);
}

// ---------------------------------------------------------------------------
// Index-backed marquee selection / "newly placed notes" selection.
// Oracles = the original whole-note-list scans.
// ---------------------------------------------------------------------------

// Dense mixed chart (taps, holds with heads, scratch chains, bound stars, eighths,
// flicks) plus split effects of very different lengths. Returns how many split
// effects the engine accepted.
int build_dense_chart_for_index_tests(Harness& h, MusicTiming& timing_out) {
  using wds::chart_editor::GimmickType;
  MusicTiming timing;
  timing.bpm = 120.0;
  timing.ticks_per_quarter = 480;
  timing.points = {TimingPoint{0, 120.0, 4, 4, true, true},
                   TimingPoint{9600, 200.0, 4, 4, true, true},
                   TimingPoint{30720, 90.0, 4, 4, true, true}};
  h.engine.document().set_timing(timing);
  timing_out = timing;

  std::uint32_t rng = 987654321u;
  auto next = [&rng](std::uint32_t mod) {
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) % mod;
  };
  auto add_body = [&h](NoteType type, int32_t start, int32_t end, int32_t lane, int32_t width) {
    NotationNote body;
    body.note_type = type;
    body.start_tick = start;
    body.end_tick = end;
    body.lane = lane;
    body.width = width;
    if (type == NoteType::ScratchHold) {
      body.gimmick_type = GimmickType::JumpScratch;
      wds::chart_editor::set_scratch_hold_end_lanes(body, lane, lane + width - 1);
    }
    return h.engine.add_note(body);
  };
  std::vector<int32_t> hold_ids;
  for (int i = 0; i < 380; ++i) {
    // Many notes share ticks (multiples of 120) so same-tick lookups return several.
    const int32_t tick = static_cast<int32_t>(next(500u)) * 120;
    const int32_t lane = static_cast<int32_t>(next(11u));
    NotationNote n;
    n.start_tick = tick;
    n.end_tick = tick;
    n.lane = lane;
    n.width = 1 + static_cast<int32_t>(next(3u));
    switch (next(5u)) {
      case 0:
      case 1:
        n.note_type = next(2u) == 0 ? NoteType::Normal : NoteType::Critical;
        h.engine.add_note(n);
        break;
      case 2: {
        const int32_t len = next(6u) == 0 ? 15000 + static_cast<int32_t>(next(20000u))
                                          : 240 + static_cast<int32_t>(next(2400u));
        const int32_t id = add_body(NoteType::Hold, tick, tick + len, lane, n.width);
        if (id >= 0) hold_ids.push_back(id);
        NotationNote head = n;
        head.note_type = NoteType::HoldStart;
        h.engine.add_note(head);
        break;
      }
      case 3: {
        const int32_t len = 480 + static_cast<int32_t>(next(1900u));
        const int32_t a = add_body(NoteType::ScratchHold, tick, tick + len, lane, n.width);
        const int32_t lane_b = std::clamp(lane + static_cast<int32_t>(next(3u)) - 1, 0, 10);
        add_body(NoteType::ScratchHold, tick + len, tick + len + 960, lane_b, n.width);
        if (a >= 0) hold_ids.push_back(a);
        break;
      }
      default: {
        if (!hold_ids.empty()) {
          const auto parent = h.engine.document().find_note(
              hold_ids[next(static_cast<std::uint32_t>(hold_ids.size()))]);
          if (parent && parent->end_tick > parent->start_tick) {
            NotationNote star;
            star.note_type = NoteType::Sound;
            star.start_tick = (parent->start_tick + parent->end_tick) / 2;
            star.end_tick = star.start_tick;
            star.lane = parent->lane;
            star.width = parent->width;
            star.parent_hold_id = parent->id;
            h.engine.add_note(star);  // may be rejected (stacked stars); irrelevant
            NotationNote eighth = star;
            eighth.note_type = NoteType::HoldEighth;
            eighth.start_tick = star.start_tick + 60;
            eighth.end_tick = eighth.start_tick;
            h.engine.add_note(eighth);
          }
        }
        n.note_type = NoteType::Flick;
        h.engine.add_note(n);
        break;
      }
    }
  }
  // Split effects: long ones that start long before and end inside later windows,
  // short ones, zero-length (end == start) and a legacy-style end_tick 0.
  const std::pair<int32_t, int32_t> splits[] = {
      {0, 55000},     {12000, 40000}, {5000, 5000},   {7000, 7001},  {30000, 30480},
      {20000, 0},     {45000, 52000}, {16000, 16960}, {33333, 33900}, {58000, 59000}};
  int accepted = 0;
  const GimmickType kinds[] = {GimmickType::Split3, GimmickType::Split2, GimmickType::Split4};
  int k = 0;
  for (const auto& [start, end] : splits) {
    if (h.engine.add_note(make_split_effect(start, end, kinds[k++ % 3])) >= 0) ++accepted;
  }
  return accepted;
}

bool rects_overlap_inclusive(const wds::interaction::Rect& a, const wds::interaction::Rect& b) {
  return !(a.right() < b.x || a.x > b.right() || a.bottom() < b.y || a.y > b.bottom());
}

struct MarqueeOracle {
  std::unordered_set<int32_t> must;  // exact tick / lane based selection
  std::unordered_set<int32_t> may;   // must + notes whose gutter label chips overlap the box
};

// finish_marquee note selection oracle (full scans). X is expanded to whole tracks
// the same way as ChartEditPanel::marquee_screen_rect.
MarqueeOracle oracle_marquee(const ChartEditPanel& panel, Vec2 a, Vec2 b) {
  namespace ce = wds::chart_editor;
  const auto& vp = panel.viewport();
  const auto& pf = vp.bounds();
  const auto& gutter = panel.left_gutter();
  const auto& notes = panel.engine().document().notes();
  const int32_t t0 = vp.tick_at(a.y);
  const int32_t t1 = vp.tick_at(b.y);
  const int32_t lo = std::min(t0, t1);
  const int32_t hi = std::max(t0, t1);
  const auto ax = ChartEditPanelTestAccess::marquee_track_x_span(panel, a.x);
  const auto bx = ChartEditPanelTestAccess::marquee_track_x_span(panel, b.x);
  const float bx0 = std::min(ax.first, bx.first);
  const float bx1 = std::max(ax.second, bx.second);

  MarqueeOracle out;
  const float play_x0 = std::max(bx0, pf.x);
  const float play_x1 = std::min(bx1, pf.right());
  if (play_x1 > play_x0 + 0.5f) {
    const int32_t l0 = vp.lane_at(play_x0, 1);
    const int32_t l1 = vp.lane_at(std::max(play_x0, play_x1 - 0.01f), 1);
    for (const auto& note : notes) {
      if (!ce::is_playfield_selectable(note)) continue;
      if (note.start_tick >= lo && note.start_tick <= hi && note.lane <= std::max(l0, l1) &&
          note.end_lane() >= std::min(l0, l1)) {
        out.must.insert(note.id);
      }
    }
  }
  if (bx0 < gutter.right() && bx1 > gutter.x) {
    for (const auto& note : notes) {
      if (!ce::is_split_lane_gimmick(note.gimmick_type)) continue;
      const int32_t end_tick = std::max(note.start_tick, note.end_tick);
      if ((note.start_tick >= lo && note.start_tick <= hi) || (end_tick >= lo && end_tick <= hi)) {
        out.must.insert(note.id);
      }
    }
  }
  out.may = out.must;
  const wds::interaction::Rect box{bx0, vp.y_at(hi), bx1 - bx0, vp.y_at(lo) - vp.y_at(hi)};
  for (const auto& hit : wds::ui::build_split_label_hits(vp, gutter, notes)) {
    if (rects_overlap_inclusive(hit.bounds, box)) out.may.insert(hit.note_id);
  }
  return out;
}

void test_marquee_snaps_x_to_tracks_including_gutters() {
  Harness h;
  h.panel.set_extended_editing_active(true);
  h.panel.sync_to_timeline_ms(0.0);
  const auto& vp = h.panel.viewport();
  const auto& pf = vp.bounds();
  const auto& split = h.panel.left_gutter();
  const auto& speed = h.panel.speed_gutter();
  CHECK(speed.w > 0.5f);

  const float y0 = pf.y + pf.h * 0.4f;
  const float y1 = pf.y + pf.h * 0.6f;

  // Mid-lane drag expands to that lane's full width.
  const float lane3_left = vp.x_at(3);
  const float lane3_right = lane3_left + vp.lane_width(1);
  const float lane5_right = vp.x_at(5) + vp.lane_width(1);
  {
    const auto box = ChartEditPanelTestAccess::marquee_rect(
        h.panel, {lane3_left + vp.lane_width(1) * 0.3f, y0},
        {lane3_left + vp.lane_width(1) * 0.7f, y1});
    CHECK(std::fabs(box.x - lane3_left) < 0.5f);
    CHECK(std::fabs(box.right() - lane3_right) < 0.5f);
  }
  // Crossing lanes 3→5 covers every boundary between them.
  {
    const auto box = ChartEditPanelTestAccess::marquee_rect(
        h.panel, {lane3_left + 2.0f, y0}, {vp.x_at(5) + 2.0f, y1});
    CHECK(std::fabs(box.x - lane3_left) < 0.5f);
    CHECK(std::fabs(box.right() - lane5_right) < 0.5f);
  }
  // Split gutter alone snaps to the full split column.
  {
    const auto box = ChartEditPanelTestAccess::marquee_rect(
        h.panel, {split.x + 4.0f, y0}, {split.x + split.w * 0.6f, y1});
    CHECK(std::fabs(box.x - split.x) < 0.5f);
    CHECK(std::fabs(box.right() - split.right()) < 0.5f);
  }
  // Split → lane 1 covers the gutter and lanes 0..1.
  {
    const auto box = ChartEditPanelTestAccess::marquee_rect(
        h.panel, {split.x + 4.0f, y0}, {vp.x_at(1) + 2.0f, y1});
    CHECK(std::fabs(box.x - split.x) < 0.5f);
    CHECK(std::fabs(box.right() - (vp.x_at(1) + vp.lane_width(1))) < 0.5f);
  }
  // Speed gutter alone snaps to the full speed column.
  {
    const auto box = ChartEditPanelTestAccess::marquee_rect(
        h.panel, {speed.x + 4.0f, y0}, {speed.x + speed.w * 0.6f, y1});
    CHECK(std::fabs(box.x - speed.x) < 0.5f);
    CHECK(std::fabs(box.right() - speed.right()) < 0.5f);
  }
  // Last playfield lane → speed covers both tracks.
  {
    const int32_t last = vp.first_visible_lane() + vp.visible_lane_count() - 1;
    const auto box = ChartEditPanelTestAccess::marquee_rect(
        h.panel, {vp.x_at(last) + 2.0f, y0}, {speed.x + 4.0f, y1});
    CHECK(std::fabs(box.x - vp.x_at(last)) < 0.5f);
    CHECK(std::fabs(box.right() - speed.right()) < 0.5f);
  }
}

void test_marquee_index_matches_full_scan_on_dense_chart() {
  Harness h;
  MusicTiming timing;
  const int accepted_splits = build_dense_chart_for_index_tests(h, timing);
  CHECK(accepted_splits >= 3);
  CHECK(h.engine.document().notes().size() > 300u);

  std::uint32_t rng = 424242u;
  auto frac = [&rng]() {
    rng = rng * 1664525u + 1013904223u;
    return static_cast<float>((rng >> 8) & 0xFFFFu) / 65535.0f;
  };

  const auto& vp = h.panel.viewport();
  const auto& pf = vp.bounds();
  const auto gutter = h.panel.left_gutter();
  CHECK(gutter.right() <= pf.x + 0.01f);

  // Scroll positions chosen so the window sits on, just before and after split ends.
  const int32_t focus_ticks[] = {0, 5000, 12000, 20000, 33900, 40000, 52000, 55000, 58500};
  int compared = 0;
  int mismatches = 0;
  int end_only_splits = 0;  // splits picked although their START is outside the box
  int playfield_picks = 0;
  for (const int32_t focus : focus_ticks) {
    h.panel.sync_to_timeline_ms(
        static_cast<double>(wds::chart_editor::tick_to_milliseconds(focus, timing)));
    for (int i = 0; i < 70; ++i) {
      // x range: 0 = playfield only, 1 = left gutter only, 2 = spans both.
      const int kind = i % 3;
      float xa, xb;
      auto in_pf = [&]() { return pf.x + 1.0f + frac() * (pf.w - 2.0f); };
      auto in_gutter = [&]() { return gutter.x + 1.0f + frac() * (gutter.w - 2.0f); };
      if (kind == 0) {
        xa = in_pf();
        xb = in_pf();
      } else if (kind == 1) {
        xa = in_gutter();
        xb = in_gutter();
      } else {
        xa = in_gutter();
        xb = in_pf();
      }
      // Thin, medium and tall boxes (anchor y and release y anywhere in the panel).
      const float ya = pf.y + 1.0f + frac() * (pf.h - 2.0f);
      const float span = (i % 4 == 0) ? 6.0f : (i % 4 == 1 ? 120.0f : pf.h);
      const float yb = std::clamp(ya + (frac() - 0.5f) * span, pf.y + 1.0f, pf.y + pf.h - 1.0f);
      const Vec2 from{xa, ya};
      const Vec2 to{xb, yb};

      const auto want = oracle_marquee(h.panel, from, to);
      ChartEditPanelTestAccess::marquee(h.panel, from, to);
      std::unordered_set<int32_t> got;
      for (const int32_t id : h.panel.selected()) got.insert(id);

      ++compared;
      bool ok = true;
      for (const int32_t id : want.must) ok = ok && got.count(id) != 0;
      for (const int32_t id : got) ok = ok && want.may.count(id) != 0;
      if (!ok) {
        ++mismatches;
        if (mismatches <= 5) {
          std::fprintf(stderr,
                       "marquee mismatch focus=%d box=(%.1f,%.1f)-(%.1f,%.1f): must=%zu may=%zu "
                       "got=%zu\n",
                       focus, xa, ya, xb, yb, want.must.size(), want.may.size(), got.size());
        }
      }
      const int32_t lo = std::min(vp.tick_at(ya), vp.tick_at(yb));
      const int32_t hi = std::max(vp.tick_at(ya), vp.tick_at(yb));
      for (const int32_t id : got) {
        const auto note = h.engine.document().find_note(id);
        if (!note) continue;
        if (wds::chart_editor::is_split_lane_gimmick(note->gimmick_type)) {
          if (note->start_tick < lo || note->start_tick > hi) ++end_only_splits;
        } else {
          ++playfield_picks;
        }
      }
    }
  }
  std::fprintf(stderr, "marquee sweep: %d boxes, %d mismatches, %d end-only splits, %d picks\n",
               compared, mismatches, end_only_splits, playfield_picks);
  CHECK(compared > 500);
  CHECK_EQ(mismatches, 0);
  CHECK(end_only_splits > 0);  // the start-before-box / end-inside case really occurred
  CHECK(playfield_picks > 200);

  // Mutating a split (shorter / longer) must be followed by the cached span window.
  const auto long_split = [&]() -> std::optional<NotationNote> {
    for (const auto& n : h.engine.document().notes()) {
      if (wds::chart_editor::is_split_lane_gimmick(n.gimmick_type) && n.start_tick == 0 &&
          n.end_tick == 55000) {
        return n;
      }
    }
    return std::nullopt;
  }();
  if (long_split) {
    for (const int32_t new_end : {200, 55000}) {
      NotationNote edited = *long_split;
      edited.end_tick = new_end;
      CHECK(h.engine.document().update_note(long_split->id, edited));
      h.panel.sync_to_timeline_ms(
          static_cast<double>(wds::chart_editor::tick_to_milliseconds(52000, timing)));
      for (int i = 0; i < 20; ++i) {
        const Vec2 from{gutter.x + 2.0f, pf.y + 1.0f + frac() * pf.h * 0.5f};
        const Vec2 to{gutter.right() - 2.0f, pf.y + pf.h * 0.5f + frac() * pf.h * 0.5f};
        const auto want = oracle_marquee(h.panel, from, to);
        ChartEditPanelTestAccess::marquee(h.panel, from, to);
        std::unordered_set<int32_t> got;
        for (const int32_t id : h.panel.selected()) got.insert(id);
        bool ok = true;
        for (const int32_t id : want.must) ok = ok && got.count(id) != 0;
        for (const int32_t id : got) ok = ok && want.may.count(id) != 0;
        CHECK(ok);
      }
    }
  } else {
    CHECK(false);
  }
}

// Notes selected after a placement must be exactly the new head / body notes (ids
// the edit introduced), found without diffing the whole note list.
void test_place_selection_picks_only_new_notes_in_dense_chart() {
  Harness h;
  MusicTiming timing;
  build_dense_chart_for_index_tests(h, timing);
  h.panel.sync_to_timeline_ms(
      static_cast<double>(wds::chart_editor::tick_to_milliseconds(100000, timing)));

  auto ids_of = [&h]() {
    std::unordered_set<int32_t> ids;
    for (const auto& n : h.engine.document().notes()) ids.insert(n.id);
    return ids;
  };

  {  // Single tap: the one new note is selected, nothing else.
    const auto before = ids_of();
    place_tap_at(h, 100240, 3);
    std::unordered_set<int32_t> want;
    for (const auto& n : h.engine.document().notes()) {
      if (!before.count(n.id)) want.insert(n.id);
    }
    CHECK_EQ(want.size(), 1u);
    std::unordered_set<int32_t> got;
    for (const int32_t id : h.panel.selected()) got.insert(id);
    CHECK(got == want);
  }
  {  // Hold: selection == new head + new body; eighths / stars stay unselected.
    const auto before = ids_of();
    h.enter_hold(false, 100480, 101200, 6);
    h.panel.on_pointer_up(PointerUpEvent{h.at_tick_lane(101200, 6), PointerButton::Left, {}});
    std::unordered_set<int32_t> want;
    int new_eighths = 0;
    for (const auto& n : h.engine.document().notes()) {
      if (before.count(n.id)) continue;
      if (n.note_type == NoteType::HoldEighth) ++new_eighths;
      if (wds::chart_editor::is_hold_head_note(n) ||
          wds::chart_editor::is_hold_with_tail(n.note_type)) {
        want.insert(n.id);
      }
    }
    CHECK(new_eighths > 0);
    CHECK(!want.empty());
    std::unordered_set<int32_t> got;
    for (const int32_t id : h.panel.selected()) got.insert(id);
    CHECK(got == want);
  }
}

void test_visual_revision_changes_on_every_selection_change() {
  Harness h;
  std::vector<int32_t> ids;
  for (int i = 0; i < 4; ++i) {
    NotationNote n;
    n.note_type = NoteType::Normal;
    n.start_tick = i * 480;
    n.end_tick = n.start_tick;
    n.lane = 1 + i * 2;
    n.width = 1;
    const int32_t id = h.engine.add_note(n);
    CHECK(id >= 0);
    ids.push_back(id);
  }
  h.panel.sync_to_timeline_ms(0.0);
  uint64_t rev = h.panel.visual_revision();
  auto expect_changed = [&](const char* what) {
    const uint64_t now = h.panel.visual_revision();
    if (now == rev) {
      std::fprintf(stderr, "FAIL visual_revision unchanged after %s\n", what);
      ++g_failures;
    }
    rev = now;
  };
  // Pure reads must keep it stable (no per-call state).
  CHECK_EQ(h.panel.visual_revision(), rev);

  h.panel.set_selected({ids[0]});
  expect_changed("set_selected(a)");
  h.panel.set_selected({ids[0], ids[1]});
  expect_changed("set_selected(a,b)");
  h.panel.set_selected({ids[2]});
  expect_changed("set_selected(c) replaces a,b");
  h.panel.set_selected({ids[3]});
  expect_changed("set_selected(d) same size, different member");
  h.panel.clear_selection();
  expect_changed("clear_selection");

  // Marquee (primary-drag) selects through the pointer path.
  const auto start = h.at_tick_lane(0, 0);
  const auto end = h.at_tick_lane(1000, 11);
  h.panel.on_pointer_down(PointerDownEvent{start, PointerButton::Left, primary_mods()});
  h.panel.on_pointer_move(PointerMoveEvent{end, primary_mods()});
  h.panel.on_pointer_up(PointerUpEvent{end, PointerButton::Left, primary_mods()});
  CHECK(!h.panel.selected().empty());
  expect_changed("marquee select");

  // Marquee over empty space clears the selection.
  const auto empty_a = h.at_tick_lane(0, 10);
  const auto empty_b = h.at_tick_lane(240, 11);
  h.panel.on_pointer_down(PointerDownEvent{empty_a, PointerButton::Left, primary_mods()});
  h.panel.on_pointer_move(PointerMoveEvent{empty_b, primary_mods()});
  h.panel.on_pointer_up(PointerUpEvent{empty_b, PointerButton::Left, primary_mods()});
  CHECK(h.panel.selected().empty());
  expect_changed("marquee over empty space");

  // Speed-point selection (extended editing only).
  h.panel.set_extended_editing_active(true);
  rev = h.panel.visual_revision();
  wds::chart_editor::ScrollSpeedPoint point;
  point.tick = 480;
  point.multiplier = 2.0;
  if (h.engine.document().set_scroll_speed_points({point})) {
    rev = h.panel.visual_revision();
    h.panel.set_selected_speed_ticks({480});
    CHECK_EQ(h.panel.selected_speed_ticks().size(), 1u);
    expect_changed("set_selected_speed_ticks");
    h.panel.clear_selection();
    CHECK(h.panel.selected_speed_ticks().empty());
    expect_changed("clear_selection with speed tick");
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
  test_headless_hold_start_tap_wins_visual_order();
  test_selected_note_owns_overlapping_pointer();
  test_headless_hold_start_caps_are_tightened();
  test_double_click_inside_hold_after_edge_grab_selects_segment();
  test_right_click_selected_hold_tail_continues_draw();
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
  test_option_horizontal_wheel_updates_visible_range();
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
  test_unselected_edge_edits_keep_selection();
  test_multi_select_drag_from_hold_keeps_foreign_notes();
  test_hold_layer_only_on_double_click();
  test_hold_selection_layer_order();
  test_hold_chain_layer_then_headless_skips_parts();
  test_double_click_star_keeps_direct_jump();
  test_single_headless_hold_top_is_parts();
  test_paste_hold_does_not_select_eighths();
  test_hold_eighths_are_never_selectable_or_deletable();
  test_delete_star_regenerates_eighth_in_one_undo_step();
  test_nudge_star_within_hold_updates_eighths();
  test_nudge_hold_moves_stars_and_regenerates_eighths();
  test_nudge_star_out_of_hold_is_refused();
  test_copy_lone_mid_star_is_a_noop();
  test_copy_hold_with_star_pastes_bound_star_and_regenerated_eighths();
  test_nontail_hold_eighths_follow_paste_and_resize();
  test_mirror_and_copy_hold_includes_mid_stars();
  test_convert_selected_hold_stars_and_defaults();
  test_star_target_converts_hold_children_and_star_bodies();
  test_toolbox_star_placement_snaps_to_pointer_hold();
  test_marquee_and_convert_ignore_split_lines();
  test_gutter_label_select_copy_mirror_and_move();
  test_paste_mirror_is_one_history_step_and_skips_splits();
  test_paste_can_move_backwards();
  test_split_width_follow_closed_interval_includes_endpoints();
  test_toolbox_left_click_places_selected_type_and_ignores_swipe();
  test_toolbox_hold_click_min_length_equal_tail_and_gold();
  test_toolbox_hold_ghost_follows_shift();
  test_extended_speed_gutter_slider_and_nontail();
  test_toolbox_scratch_hold_uses_regular_hold_buttons();
  test_hit_test_windowed_scan_matches_full_scan_on_dense_chart();
  test_split_placement_window_matches_full_scan();
  test_marquee_index_matches_full_scan_on_dense_chart();
  test_marquee_snaps_x_to_tracks_including_gutters();
  test_place_selection_picks_only_new_notes_in_dense_chart();
  test_visual_revision_changes_on_every_selection_change();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  return 0;
}
