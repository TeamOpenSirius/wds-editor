#include "wds/interaction/editor_input.hpp"
#include "wds/interaction/editor_shortcuts.hpp"
#include "wds/interaction/font_atlas.hpp"
#include "wds/interaction/gesture.hpp"
#include "wds/interaction/platform.hpp"
#include "wds/interaction/shortcuts.hpp"
#include "wds/interaction/ui_painter.hpp"
#include "wds/interaction/widget_root.hpp"
#include "wds/interaction/widgets/button.hpp"
#include "wds/interaction/widgets/checkbox.hpp"
#include "wds/interaction/widgets/shortcut_field.hpp"
#include "wds/interaction/widgets/slider.hpp"
#include "wds/interaction/widgets/stepper.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace {

int failures = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++failures;
  }
}

}  // namespace

int main() {
  using namespace wds::interaction;

  ShortcutManager shortcuts;
  auto& preview = shortcuts.namespace_for("preview");
  int toggle_count = 0;
  expect(preview.bind({KeyCode::Space, {}}, [&] { ++toggle_count; }), "bind space");
  expect(!preview.bind({KeyCode::Space, {}}, [&] {}), "duplicate chord rejected");

  auto& edit = shortcuts.namespace_for("edit");
  int edit_count = 0;
  expect(edit.bind({KeyCode::Space, {}}, [&] { ++edit_count; }), "bind space in edit");

  shortcuts.set_active_namespace("preview");
  expect(shortcuts.dispatch(KeyDownEvent{KeyCode::Space, {}}), "preview dispatch");
  expect(toggle_count == 1, "preview action fired");
  expect(edit_count == 0, "edit inactive");

  shortcuts.set_active_namespace("edit");
  expect(shortcuts.dispatch(KeyDownEvent{KeyCode::Space, {}}), "edit dispatch");
  expect(edit_count == 1, "edit action fired");

  WidgetRoot root;
  root.set_bounds({0, 0, 200, 200});
  auto button = std::make_unique<Button>("Test");
  button->set_bounds({10, 10, 80, 36});
  int clicks = 0;
  button->on_click([&] { ++clicks; });
  root.add_child(std::move(button));

  root.process_frame(0.016f, {PointerDownEvent{{40, 28}, PointerButton::Left, {}},
                         PointerUpEvent{{40, 28}, PointerButton::Left, {}},
                         ClickEvent{{40, 28}, PointerButton::Left, {}, 1}});
  expect(clicks == 1, "button click callback");

  // Rapid second press: DoubleClick is queued before Click on the same release.
  // press_target_ must survive DoubleClick so the Click still reaches the button.
  root.process_frame(0.016f, {PointerDownEvent{{40, 28}, PointerButton::Left, {}},
                              PointerUpEvent{{40, 28}, PointerButton::Left, {}},
                              DoubleClickEvent{{40, 28}, PointerButton::Left, {}},
                              ClickEvent{{40, 28}, PointerButton::Left, {}, 2}});
  expect(clicks == 2, "button click survives preceding DoubleClick");

  Slider slider;
  slider.set_bounds({0, 0, 100, 20});
  slider.set_range(0.0f, 100.0f);
  slider.set_value(10.0f);
  float last = slider.value();
  slider.on_change([&](float v) { last = v; });
  slider.on_pointer_down(PointerDownEvent{{100, 10}, PointerButton::Left, {}});
  expect(last >= 99.0f, "slider drag to max");
  slider.on_pointer_down(PointerDownEvent{{-5, 10}, PointerButton::Left, {}});
  expect(last <= 1.0f, "slider drag clamped to min");

  expect(classify_swipe({0, 0}, {40, 0}) == SwipeDirection::Right, "right swipe");
  expect(classify_swipe({40, 0}, {0, 0}) == SwipeDirection::Left, "left swipe");
  expect(classify_swipe({0, 40}, {0, 0}) == SwipeDirection::Up, "up swipe");
  expect(classify_swipe({0, 0}, {0, 40}) == SwipeDirection::Down, "down swipe");
  expect(classify_swipe({0, 0}, {18, 0}) == SwipeDirection::Right, "short right swipe at 16px threshold");
  expect(classify_swipe({0, 0}, {10, 0}) == SwipeDirection::None, "below swipe threshold");

  PlaceSwipeTracker place_swipe;
  constexpr float kHalfLane = 50.0f;
  constexpr float kVert = 16.0f;
  expect(place_swipe.update(0, 0, {40, 0}, kHalfLane, kVert) == SwipeDirection::None,
         "dx below half-lane does not arm horizontal");
  expect(place_swipe.update(0, 0, {60, 0}, kHalfLane, kVert) == SwipeDirection::Right,
         "place swipe acquire right at half-lane dx");
  expect(place_swipe.axis_lock() == PlaceAxisLock::Horizontal, "place swipe locks horizontal");
  expect(place_swipe.update(0, 0, {60, -40}, kHalfLane, kVert) == SwipeDirection::Right,
         "horizontal lock ignores vertical angle");
  expect(place_swipe.update(0, 0, {-60, 20}, kHalfLane, kVert) == SwipeDirection::Left,
         "horizontal lock follows x only");
  // |dx| below half-lane: unlock + re-observe (Tap / Up).
  expect(place_swipe.update(0, 0, {5, 80}, kHalfLane, kVert) == SwipeDirection::Down,
         "sub half-lane dx unlocks and reclassifies");
  expect(place_swipe.axis_lock() == PlaceAxisLock::None, "horizontal lock cleared on sub-threshold dx");
  expect(place_swipe.update(0, 0, {60, 0}, kHalfLane, kVert) == SwipeDirection::Right,
         "re-arm horizontal");
  expect(place_swipe.update(0, 0, {10, -40}, kHalfLane, kVert) == SwipeDirection::Up,
         "sub half-lane dx unlocks into vertical hold");
  expect(place_swipe.axis_lock() == PlaceAxisLock::Vertical, "vertical lock after unlock");
  expect(place_swipe.update(0, 0, {60, -40}, kHalfLane, kVert) == SwipeDirection::Up,
         "vertical lock ignores horizontal angle");
  expect(place_swipe.update(0, 0, {60, -5}, kHalfLane, kVert) == SwipeDirection::Right,
         "sub vertical threshold unlocks into horizontal");
  expect(place_swipe.axis_lock() == PlaceAxisLock::Horizontal,
         "vertical lock cleared; horizontal re-armed");
  // Split origins: dx from press X, dy from note time Y (may differ from press).
  place_swipe.reset();
  expect(place_swipe.update(100, 200, {160, 200}, kHalfLane, kVert) == SwipeDirection::Right,
         "dx uses press origin_x");
  place_swipe.reset();
  expect(place_swipe.update(100, 200, {100, 160}, kHalfLane, kVert) == SwipeDirection::Up,
         "dy uses note-time origin_y");
  place_swipe.reset();
  // Press X offset from note center must not invent a horizontal swipe.
  expect(place_swipe.update(100, 200, {120, 160}, kHalfLane, kVert) == SwipeDirection::Up,
         "vertical when dx from press is small even if note-center dx is large");

  Modifiers primary;
#ifdef __APPLE__
  primary.super = true;
#else
  primary.control = true;
#endif
  expect(primary_modifier_down(primary), "primary modifier recognized");
  const Modifiers normalized = normalize_primary(primary);
  expect(normalized.control && !normalized.super, "primary normalized to control");

  FloatStepper speed;
  speed.set_bounds({0, 0, 120, 32});
  speed.layout({});
  speed.set_range(1.0, 10.0);
  speed.set_step(0.5);
  speed.set_value(1.0);
  speed.on_pointer_down(PointerDownEvent{{110, 16}, PointerButton::Left, {}});
  expect(speed.value() == 1.5, "float stepper uses configured step");

  UiPainter painter;
  expect(painter.measure_text("bitmap").x > 0.0f, "bitmap font measured");

  {
    auto& font = FontAtlas::instance();
    bool baked = false;
#ifdef WDS_REPO_ROOT
    baked = font.bake_font_file(std::string(WDS_REPO_ROOT) +
                                "/ui/assets/fonts/NotoSansSC-Regular.ttf",
                                64.0f);
#endif
    if (!baked) {
      baked = font.bake_font_file("ui/assets/fonts/NotoSansSC-Regular.ttf", 64.0f);
    }
    if (!baked) {
      baked = font.bake_system_font(64.0f);
    }
    if (baked) {
      expect(font.ensure_glyphs("百毫秒"), "CJK glyphs packed");
      const auto cjk = font.measure("百毫秒", 32.0f);
      const auto ascii = font.measure("ABC", 32.0f);
      expect(cjk.x > ascii.x * 0.8f, "CJK text has substantial width");
      const auto tofu = font.measure("???", 32.0f);
      expect(std::fabs(cjk.x - tofu.x) > 2.0f, "CJK not rendered as ASCII tofu");
      font.clear();
    }
  }

  Checkbox checkbox("Mute");
  checkbox.set_bounds({0, 0, 100, 24});
  checkbox.on_click(ClickEvent{{5, 5}, PointerButton::Left, {}, 1});
  expect(checkbox.checked(), "checkbox toggles");

  expect(resolve_place_intent(PointerButton::Left, SwipeDirection::None, true) == PlaceIntent::Tap,
         "place tap");
  expect(resolve_place_intent(PointerButton::Left, SwipeDirection::Left, false) == PlaceIntent::ExTap,
         "place extap");
  expect(
      resolve_place_intent(PointerButton::Left, SwipeDirection::Right, false) == PlaceIntent::HoldStart,
      "place hold start");
  expect(resolve_place_intent(PointerButton::Left, SwipeDirection::Up, false) == PlaceIntent::HoldBody,
         "place hold body up");
  expect(resolve_place_intent(PointerButton::Left, SwipeDirection::Down, false) == PlaceIntent::Tap,
         "down swipe rests as tap");
  expect(resolve_place_intent(PointerButton::Right, SwipeDirection::None, true) == PlaceIntent::Flick,
         "place flick");
  expect(
      resolve_place_intent(PointerButton::Right, SwipeDirection::Left, false) == PlaceIntent::FlickLeft,
      "place flick left");
  expect(resolve_place_intent(PointerButton::Right, SwipeDirection::Right, false) ==
             PlaceIntent::FlickRight,
         "place flick right");
  expect(resolve_place_intent(PointerButton::Right, SwipeDirection::Up, false) ==
             PlaceIntent::ScratchHoldBody,
         "place scratch hold up");
  expect(resolve_place_intent(PointerButton::Right, SwipeDirection::Down, false) == PlaceIntent::Flick,
         "down swipe rests as flick");

  expect(is_place_hold_star(PointerDownEvent{{0, 0}, PointerButton::Right, {true}}, false),
         "normal hold star is shift+right");
  expect(!is_chain_hold_body(PointerDownEvent{{0, 0}, PointerButton::Right, {}}, false),
         "normal hold does not chain");
  expect(is_finish_hold_body(PointerButton::Left, false), "normal hold finishes on left-up");
  expect(!is_finish_hold_body(PointerButton::Right, false),
         "normal hold ignores right-up for finish");
  expect(is_place_hold_star(PointerDownEvent{{0, 0}, PointerButton::Left, {true}}, true),
         "scratch hold star is shift+left");
  expect(is_chain_hold_body(PointerDownEvent{{0, 0}, PointerButton::Left, {}}, true),
         "scratch hold chain is left");
  expect(!is_chain_hold_body(PointerDownEvent{{0, 0}, PointerButton::Right, {}}, true),
         "scratch hold does not chain on right");
  expect(is_finish_hold_body(PointerButton::Right, true), "scratch hold finishes on right-up");
  expect(!is_finish_hold_body(PointerButton::Left, true),
         "scratch hold ignores left-up for finish");

  expect(is_delete_single_note(PointerDownEvent{{0, 0}, PointerButton::Middle, {}}),
         "middle deletes note");
  expect(is_cancel_placement(PointerDownEvent{{0, 0}, PointerButton::Middle, {}}),
         "middle cancels placement");
  expect(is_delete_selection_key(KeyCode::Delete), "delete key");
  expect(!is_delete_selection_key(KeyCode::Backspace), "backspace not delete-selection");
  expect(default_width_for_key(static_cast<KeyCode>('Q')) == 1, "width Q");
  expect(default_width_for_key(static_cast<KeyCode>('W')) == 2, "width W");
  expect(default_width_for_key(static_cast<KeyCode>('E')) == 3, "width E");
  expect(default_width_for_key(KeyCode::A) == 4, "width A");
  expect(default_width_for_key(static_cast<KeyCode>('S')) == 6, "width S");
  expect(default_width_for_key(static_cast<KeyCode>('D')) == 12, "width D");
  expect(resolve_edit_key(KeyDownEvent{KeyCode::Delete, {}}).action ==
             EditKeyAction::DeleteSelection,
         "resolve delete key");
  expect(resolve_edit_key(KeyDownEvent{static_cast<KeyCode>('Q'), {}}).action ==
             EditKeyAction::SetDefaultWidth,
         "resolve width Q");
  expect(is_toggle_select(PointerDownEvent{{0, 0}, PointerButton::Left, primary}),
         "toggle select primary+left");
  expect(is_vertical_swipe(SwipeDirection::Up) && is_vertical_swipe(SwipeDirection::Down),
         "vertical swipe helper");
  expect(chord_delete_selection().key == KeyCode::Delete, "delete chord");
  expect(chord_save().key == static_cast<KeyCode>('S'), "save chord key");
  expect(chord_width_slot(0).key == static_cast<KeyCode>('Q'), "width slot Q");
  expect(chord_width_slot(3).key == KeyCode::A, "width slot A");
  expect(chord_width_slot(5).key == static_cast<KeyCode>('D'), "width slot D");

  expect(is_forbidden_shortcut_key(KeyCode::Num1), "digit forbidden as shortcut");
  expect(is_forbidden_shortcut_key(static_cast<KeyCode>(46)), "period forbidden as shortcut");
  expect(!is_forbidden_shortcut_key(static_cast<KeyCode>('C')), "letter allowed as shortcut");
  expect(is_completing_shortcut_key(KeyCode::Space), "space completes shortcut");
  expect(is_completing_shortcut_key(KeyCode::F11), "F11 completes shortcut");
  expect(!is_completing_shortcut_key(KeyCode::Unknown), "unknown does not complete");
  expect(!is_completing_shortcut_key(KeyCode::Num5), "digit does not complete");
  {
    const auto parsed = parse_shortcut_chord("Shift+Ctrl+C");
    expect(parsed.has_value(), "parse Shift+Ctrl+C");
    if (parsed) {
      expect(parsed->key == static_cast<KeyCode>('C'), "parsed key C");
      expect(parsed->mods.shift && parsed->mods.control, "parsed Shift+Ctrl");
      const std::string shown = format_shortcut_chord(*parsed);
      expect(shown.find('C') != std::string::npos, "format contains C");
#ifdef __APPLE__
      expect(shown.find("Cmd") != std::string::npos, "Apple format uses Cmd");
#else
      expect(shown.find("Ctrl") != std::string::npos, "non-Apple format uses Ctrl");
#endif
    }
    expect(parse_shortcut_chord("Cmd+S").has_value(), "parse Cmd+S");
    expect(parse_shortcut_chord("").has_value() == false, "empty parse fails");
    expect(parse_shortcut_chord("Shift").has_value() == false, "mods-only parse fails");
  }
  // Round-trip every default binding through format/parse.
  for (std::size_t i = 0; i < kEditorShortcutCount; ++i) {
    const auto id = static_cast<EditorShortcut>(i);
    const ShortcutChord def = default_editor_shortcut(id);
    const auto again = parse_shortcut_chord(format_shortcut_chord(def));
    expect(again.has_value() && *again == def, editor_shortcut_id(id));
  }
  expect(!editor_shortcut_conflicts(EditorShortcut::Copy, chord_copy()),
         "self chord is not a conflict");
  expect(editor_shortcut_conflicts(EditorShortcut::Save, chord_copy()),
         "save setting to copy chord conflicts");
  expect(!editor_shortcut_conflicts(EditorShortcut::Save, chord_save()),
         "save keeping its own chord is fine");

  // Rebind width slot 0 and confirm resolve/default helpers follow the registry.
  {
    const ShortcutChord original = editor_shortcut(EditorShortcut::WidthSlot0);
    set_editor_shortcut(EditorShortcut::WidthSlot0, {static_cast<KeyCode>('Z'), {}});
    expect(default_width_for_key(static_cast<KeyCode>('Z')) == 1, "rebound width Z");
    expect(!default_width_for_key(static_cast<KeyCode>('Q')).has_value(),
           "old width Q unbound from helper");
    expect(resolve_edit_key(KeyDownEvent{static_cast<KeyCode>('Z'), {}}).action ==
               EditKeyAction::SetDefaultWidth,
           "resolve rebound width Z");
    expect(chord_width_slot(0).key == static_cast<KeyCode>('Z'), "chord_width_slot follows registry");
    set_editor_shortcut(EditorShortcut::WidthSlot0, original);
    reset_editor_shortcuts();
  }

  // ShortcutField: commit, conflict revert, forbidden reject, blur revert, captures_keys.
  {
    ShortcutField field;
    field.set_bounds({0, 0, 120, 28});
    field.set_chord(chord_primary(static_cast<KeyCode>('S')));
    int changes = 0;
    field.on_change([&](const ShortcutChord&) { ++changes; });
    const ShortcutChord blocked = chord_primary(static_cast<KeyCode>('C'));
    field.set_conflict_checker([&](const ShortcutChord& c) { return c == blocked; });

    field.on_pointer_down(PointerDownEvent{{10, 10}, PointerButton::Left, {}});
    expect(field.visual_state() == WidgetState::Focused, "shortcut field focuses");
    expect(field.captures_keys(), "shortcut field captures keys while focused");

    Modifiers shift;
    shift.shift = true;
    field.on_key_down(KeyDownEvent{KeyCode::Unknown, shift, false});
    expect(field.visual_state() == WidgetState::Focused, "modifier keeps focus");

    field.on_key_down(KeyDownEvent{static_cast<KeyCode>('X'), shift, false});
    expect(field.visual_state() == WidgetState::Normal, "completing key blurs");
    expect(field.chord().key == static_cast<KeyCode>('X') && field.chord().mods.shift,
           "committed Shift+X");
    expect(changes == 1, "on_change after commit");

    field.on_pointer_down(PointerDownEvent{{10, 10}, PointerButton::Left, {}});
    const int changes_before = changes;
    field.on_key_down(KeyDownEvent{blocked.key, blocked.mods, false});
    expect(field.chord().key == static_cast<KeyCode>('X'), "conflict keeps old chord");
    expect(changes == changes_before, "conflict does not fire on_change");
    expect(field.visual_state() == WidgetState::Focused, "conflict stays focused");

    field.on_key_down(KeyDownEvent{KeyCode::Num3, {}, false});
    expect(field.chord().key == static_cast<KeyCode>('X'), "digit rejected");

    field.on_key_down(KeyDownEvent{KeyCode::Escape, {}, false});
    expect(field.visual_state() == WidgetState::Normal, "escape cancels capture");

    field.on_pointer_down(PointerDownEvent{{10, 10}, PointerButton::Left, {}});
    field.on_key_down(KeyDownEvent{KeyCode::Unknown, shift, false});
    field.on_blur();
    expect(field.chord().key == static_cast<KeyCode>('X'), "blur reverts draft mods");
    expect(!field.captures_keys(), "blur clears captures_keys");
  }

  // Focused ShortcutField must receive keys before ShortcutManager.
  {
    WidgetRoot root2;
    root2.set_bounds({0, 0, 400, 400});
    auto sf = std::make_unique<ShortcutField>();
    auto* raw = sf.get();
    raw->set_bounds({10, 10, 100, 28});
    raw->set_chord({KeyCode::Space, {}});
    root2.add_child(std::move(sf));

    ShortcutManager mgr;
    auto& ns = mgr.namespace_for("editor");
    int fired = 0;
    ns.bind({static_cast<KeyCode>('K'), {}}, [&] { ++fired; });
    mgr.set_active_namespace("editor");

    root2.process_frame(0.016f, {PointerDownEvent{{20, 20}, PointerButton::Left, {}}});
    expect(root2.focused_widget() == raw, "root focuses shortcut field");
    root2.process_frame(0.016f, {KeyDownEvent{static_cast<KeyCode>('K'), {}, false}}, &mgr);
    expect(fired == 0, "global shortcut suppressed while capturing");
    expect(raw->chord().key == static_cast<KeyCode>('K'), "field consumed K");
  }

  {
    ShortcutNamespace ns;
    expect(ns.bind({KeyCode::Space, {}}, [] {}), "clear-test bind");
    ns.clear();
    expect(ns.size() == 0, "namespace clear empties bindings");
    expect(ns.bind({KeyCode::Space, {}}, [] {}), "bind after clear");
  }

  return failures == 0 ? 0 : 1;
}
