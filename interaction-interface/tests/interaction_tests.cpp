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
#include "wds/interaction/widgets/combo_box.hpp"
#include "wds/interaction/widgets/dropdown.hpp"
#include "wds/interaction/widgets/shortcut_field.hpp"
#include "wds/interaction/widgets/slider.hpp"
#include "wds/interaction/widgets/stepper.hpp"
#include "wds/interaction/widgets/text_field.hpp"

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

  // ShortcutField: commit, allow duplicate temporarily, forbidden reject, clear, blur.
  {
    ShortcutField field;
    field.set_bounds({0, 0, 120, 28});
    field.set_chord(chord_primary(static_cast<KeyCode>('S')));
    int changes = 0;
    field.on_change([&](const ShortcutChord&) { ++changes; });
    const ShortcutChord blocked = chord_primary(static_cast<KeyCode>('C'));

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
    expect(field.chord().key == blocked.key, "duplicate chord is allowed temporarily");
    expect(changes == changes_before + 1, "duplicate still fires on_change");
    field.set_conflict_highlight(true);
    expect(field.conflict_highlight(), "parent can mark conflict highlight");

    field.on_pointer_down(PointerDownEvent{{10, 10}, PointerButton::Left, {}});
    field.on_key_down(KeyDownEvent{KeyCode::Num3, {}, false});
    expect(field.chord().key == blocked.key, "digit rejected");

    field.on_key_down(KeyDownEvent{KeyCode::Escape, {}, false});
    expect(field.visual_state() == WidgetState::Normal, "escape cancels capture");

    field.clear_chord();
    expect(field.chord().key == KeyCode::Unknown, "clear empties chord");
    expect(changes == changes_before + 2, "clear fires on_change");

    field.on_pointer_down(PointerDownEvent{{10, 10}, PointerButton::Left, {}});
    field.on_key_down(KeyDownEvent{KeyCode::Unknown, shift, false});
    field.on_blur();
    expect(field.chord().key == KeyCode::Unknown, "blur reverts draft mods");
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

  // Focused TextField must suppress global chords (e.g. Space play/pause) while typing.
  {
    WidgetRoot root3;
    root3.set_bounds({0, 0, 400, 400});
    auto tf = std::make_unique<TextField>();
    auto* raw = tf.get();
    raw->set_bounds({10, 10, 120, 28});
    root3.add_child(std::move(tf));

    ShortcutManager mgr;
    auto& ns = mgr.namespace_for("editor");
    int fired = 0;
    ns.bind({KeyCode::Space, {}}, [&] { ++fired; });
    mgr.set_active_namespace("editor");

    root3.process_frame(0.016f, {PointerDownEvent{{20, 20}, PointerButton::Left, {}}});
    expect(root3.focused_widget() == raw, "root focuses text field");
    expect(raw->captures_keys(), "text field captures keys while focused");
    root3.process_frame(0.016f, {KeyDownEvent{KeyCode::Space, {}, false}}, &mgr);
    expect(fired == 0, "Space shortcut suppressed while typing");
    root3.process_frame(0.016f, {TextInputEvent{" "}}, &mgr);
    expect(raw->text() == " ", "space still inserts into text field");
  }

  // Editable ComboBox captures; dropdown-only only while menu open.
  {
    ComboBox editable;
    expect(editable.captures_keys(), "editable combo captures keys");
    ComboBox menu_only;
    menu_only.set_dropdown_only(true);
    expect(!menu_only.captures_keys(), "closed dropdown-only does not capture");
  }

  // Opening one popup closes any other open Dropdown / ComboBox.
  {
    WidgetRoot root;
    root.set_bounds({0, 0, 400, 400});

    auto a = std::make_unique<Dropdown>();
    auto* drop_a = a.get();
    drop_a->set_bounds({10, 10, 80, 28});
    drop_a->set_items({"A1", "A2", "A3"});
    drop_a->set_selected_index(0);
    root.add_child(std::move(a));

    auto b = std::make_unique<ComboBox>();
    auto* combo_b = b.get();
    combo_b->set_bounds({120, 10, 80, 28});
    combo_b->set_dropdown_only(true);
    combo_b->set_items({"B1", "B2", "B3"});
    combo_b->set_text("B1");
    root.add_child(std::move(b));

    root.process_frame(0.016f, {PointerDownEvent{{40, 24}, PointerButton::Left, {}}});
    expect(drop_a->is_open(), "first dropdown opens");
    expect(!combo_b->is_open(), "combo still closed");
    expect(root.exclusive_popup() == drop_a, "exclusive tracks first open");

    root.process_frame(0.016f, {PointerDownEvent{{160, 24}, PointerButton::Left, {}}});
    expect(!drop_a->is_open(), "opening another popup closes the first");
    expect(combo_b->is_open(), "second popup opens");
    expect(root.exclusive_popup() == combo_b, "exclusive tracks second open");

    // Toggle-close the open menu must not mass-close unrelated popup hosts.
    struct SpyPopup : Widget {
      int close_count = 0;
      void close_own_popup() override { ++close_count; }
    };
    auto spy = std::make_unique<SpyPopup>();
    auto* spy_raw = spy.get();
    spy_raw->set_bounds({300, 10, 40, 28});
    root.add_child(std::move(spy));
    const int closes_before = spy_raw->close_count;
    root.process_frame(0.016f, {PointerDownEvent{{160, 24}, PointerButton::Left, {}}});
    expect(!combo_b->is_open(), "second popup toggles closed");
    expect(root.exclusive_popup() == nullptr, "exclusive cleared on close");
    expect(spy_raw->close_count == closes_before,
           "closing one menu must not close unrelated popups");
  }

  // Side-by-side editable ComboBoxes (toolbar-style): open via chevron only.
  {
    WidgetRoot root;
    root.set_bounds({0, 0, 400, 400});

    auto a = std::make_unique<ComboBox>();
    auto* combo_a = a.get();
    combo_a->set_bounds({10, 10, 80, 28});
    combo_a->set_dropdown_only(false);
    combo_a->set_opens_upward(true);
    combo_a->set_items({"10", "20", "30"});
    combo_a->set_text("10");
    root.add_child(std::move(a));

    auto b = std::make_unique<ComboBox>();
    auto* combo_b = b.get();
    combo_b->set_bounds({120, 10, 80, 28});
    combo_b->set_dropdown_only(false);
    combo_b->set_opens_upward(true);
    combo_b->set_items({"2", "4", "8"});
    combo_b->set_text("4");
    root.add_child(std::move(b));

    // Chevron hit is the rightmost ≥32px (wider than the painted triangle).
    root.process_frame(0.016f, {PointerDownEvent{{85, 24}, PointerButton::Left, {}}});
    expect(combo_a->is_open(), "editable combo opens from chevron");
    expect(!combo_b->is_open(), "peer combo stays closed");

    root.process_frame(0.016f, {PointerDownEvent{{195, 24}, PointerButton::Left, {}}});
    expect(!combo_a->is_open(), "opening peer closes the first (non-overlapping)");
    expect(combo_b->is_open(), "peer combo opens from its chevron");

    root.process_frame(0.016f, {PointerDownEvent{{195, 24}, PointerButton::Left, {}}});
    expect(!combo_b->is_open(), "chevron toggles peer closed");
    expect(!combo_a->is_open(), "first remains closed after peer close");
  }

  // Chevron hit extends inward past the painted triangle (avoid focusing as text).
  {
    WidgetRoot root;
    root.set_bounds({0, 0, 400, 400});
    auto box = std::make_unique<ComboBox>();
    auto* combo = box.get();
    combo->set_bounds({10, 10, 80, 28});
    combo->set_dropdown_only(false);
    combo->set_items({"10", "20", "30"});
    combo->set_text("10");
    root.add_child(std::move(box));

    // Field right=90. Old 18px slot started at x=72; 70 used to miss into text input.
    root.process_frame(0.016f, {PointerDownEvent{{70, 24}, PointerButton::Left, {}}});
    expect(combo->is_open(), "click inward of triangle still opens the menu");
    expect(root.focused_widget() == combo, "opening via chevron still focuses");

    combo->close_own_popup();
    root.process_frame(0.016f, {PointerDownEvent{{40, 24}, PointerButton::Left, {}}});
    expect(!combo->is_open(), "text-area click does not open the menu");
    expect(root.focused_widget() == combo, "text-area click focuses for typing");
  }

  // Selecting a popup item commits and immediately blurs the combo.
  {
    WidgetRoot root;
    root.set_bounds({0, 0, 400, 400});
    auto box = std::make_unique<ComboBox>();
    auto* combo = box.get();
    combo->set_bounds({10, 10, 80, 28});
    combo->set_dropdown_only(false);
    combo->set_items({"10", "20", "30"});
    combo->set_text("10");
    int commits = 0;
    std::string last;
    combo->on_commit([&](const std::string& text) {
      ++commits;
      last = text;
    });
    root.add_child(std::move(box));

    root.process_frame(0.016f, {PointerDownEvent{{85, 24}, PointerButton::Left, {}}});
    expect(combo->is_open(), "menu opens before item pick");
    expect(root.focused_widget() == combo, "combo focused while menu open");

    ShortcutManager mgr;
    auto& ns = mgr.namespace_for("editor");
    int fired = 0;
    ns.bind({KeyCode::Space, {}}, [&] { ++fired; });
    mgr.set_active_namespace("editor");

    // Menu opens downward; row 1 ("20") is at y≈67–96 (kControlHeight=29).
    root.process_frame(0.016f, {PointerDownEvent{{40, 80}, PointerButton::Left, {}}});
    expect(combo->text() == "20", "selecting an item updates the value");
    expect(commits == 1, "selecting an item commits once");
    expect(last == "20", "commit receives the selected label");
    expect(!combo->is_open(), "menu closes after selecting an item");
    expect(root.focused_widget() == nullptr, "combo blurs after picking a menu item");
    expect(combo->visual_state() == WidgetState::Normal, "picked combo is not focused");

    root.process_frame(0.016f, {KeyDownEvent{KeyCode::Space, {}, false}}, &mgr);
    expect(fired == 1, "global shortcut works after menu pick blur");
  }

  // Open menu items win over a sibling field they cover (no click-through).
  {
    WidgetRoot root;
    root.set_bounds({0, 0, 400, 400});

    auto a = std::make_unique<Dropdown>();
    auto* drop_a = a.get();
    drop_a->set_bounds({10, 10, 80, 28});
    drop_a->set_items({"A1", "A2", "A3", "A4", "A5"});
    drop_a->set_selected_index(0);
    int selected = -1;
    drop_a->on_select([&](int index, const std::string&) { selected = index; });
    root.add_child(std::move(a));

    auto b = std::make_unique<ComboBox>();
    auto* combo_b = b.get();
    combo_b->set_bounds({10, 50, 80, 28});
    combo_b->set_dropdown_only(true);
    combo_b->set_items({"B1", "B2", "B3"});
    combo_b->set_text("B1");
    root.add_child(std::move(b));

    root.process_frame(0.016f, {PointerDownEvent{{40, 24}, PointerButton::Left, {}}});
    expect(drop_a->is_open(), "covering dropdown opens");

    // Menu row 1 (A2) is at y≈67–96 and covers combo_b's host (y=50–78).
    root.process_frame(0.016f, {PointerDownEvent{{40, 72}, PointerButton::Left, {}}});
    expect(selected == 1, "overlapping menu item is selected");
    expect(drop_a->selected_index() == 1, "dropdown value updates");
    expect(!drop_a->is_open(), "menu closes after selecting an item");
    expect(!combo_b->is_open(), "covered field must not steal the click");
    expect(combo_b->text() == "B1", "covered combo value unchanged");
  }

  {
    ShortcutNamespace ns;
    expect(ns.bind({KeyCode::Space, {}}, [] {}), "clear-test bind");
    ns.clear();
    expect(ns.size() == 0, "namespace clear empties bindings");
    expect(ns.bind({KeyCode::Space, {}}, [] {}), "bind after clear");
  }

  // M14: OS key-repeat must not fire bound toggle/command actions.
  {
    ShortcutNamespace ns;
    int fired = 0;
    expect(ns.bind({KeyCode::Space, {}}, [&] { ++fired; }), "bind space for repeat test");
    expect(!ns.dispatch(KeyDownEvent{KeyCode::Space, {}, true}), "repeat dispatch ignored");
    expect(fired == 0, "repeat does not fire action");
    expect(ns.dispatch(KeyDownEvent{KeyCode::Space, {}, false}), "non-repeat dispatch ok");
    expect(fired == 1, "non-repeat fires once");
  }

  // M15: Backspace deletes a full UTF-8 codepoint (CJK).
  {
    TextField field;
    field.set_text("");
    field.set_visual_state(WidgetState::Focused);
    field.on_text_input(TextInputEvent{"测"});
    expect(field.text() == "测", "text field accepts CJK");
    field.on_key_down(KeyDownEvent{KeyCode::Backspace, {}, false});
    expect(field.text().empty(), "backspace removes full CJK codepoint");
  }
  {
    ComboBox box;
    box.set_dropdown_only(false);
    box.set_text("测");
    box.set_visual_state(WidgetState::Focused);
    expect(box.text() == "测", "combo set_text keeps CJK");
    box.on_key_down(KeyDownEvent{KeyCode::Backspace, {}, false});
    expect(box.text().empty(), "combo backspace removes full CJK codepoint");
  }

  // m22: hidden widgets must not keep an active tooltip.
  {
    WidgetRoot root;
    root.set_bounds({0, 0, 400, 400});
    auto btn = std::make_unique<Button>();
    auto* raw = btn.get();
    raw->set_bounds({10, 10, 80, 28});
    raw->set_tooltip("tip");
    root.add_child(std::move(btn));
    root.process_frame(0.016f, {PointerMoveEvent{{20, 20}, {}}});
    expect(root.active_tooltip() == "tip", "hover shows tooltip");
    raw->set_visible(false);
    root.process_frame(0.016f, {});
    expect(root.active_tooltip().empty(), "hidden widget clears tooltip");
  }

  // m23: hiding a focused TextField restores global shortcuts.
  {
    WidgetRoot root;
    root.set_bounds({0, 0, 400, 400});
    auto tf = std::make_unique<TextField>();
    auto* raw = tf.get();
    raw->set_bounds({10, 10, 120, 28});
    root.add_child(std::move(tf));

    ShortcutManager mgr;
    auto& ns = mgr.namespace_for("editor");
    int fired = 0;
    ns.bind({KeyCode::Space, {}}, [&] { ++fired; });
    mgr.set_active_namespace("editor");

    root.process_frame(0.016f, {PointerDownEvent{{20, 20}, PointerButton::Left, {}}});
    expect(root.focused_widget() == raw, "text field focused");
    raw->set_visible(false);
    expect(root.focused_widget() == nullptr, "hide clears focus");
    root.process_frame(0.016f, {KeyDownEvent{KeyCode::Space, {}, false}}, &mgr);
    expect(fired == 1, "Space shortcut works after hide");
  }

  // Modal must block dropdown hit-testing behind it (click-through to menus).
  {
    struct ModalPanel : Widget {
      bool open = false;
      bool is_interaction_modal() const override { return open; }
      Widget* hit_test(Vec2 point) override {
        if (!open || !visible() || !enabled()) return nullptr;
        if (!absolute_bounds().contains(point)) return nullptr;
        for (auto it = children().rbegin(); it != children().rend(); ++it) {
          if (Widget* hit = (*it)->hit_test(point)) return hit;
        }
        return this;
      }
    };

    WidgetRoot root;
    root.set_bounds({0, 0, 400, 400});

    auto drop = std::make_unique<Dropdown>();
    auto* menu = drop.get();
    menu->set_bounds({40, 40, 80, 28});
    menu->set_items({"A1", "A2", "A3", "A4", "A5"});
    menu->set_selected_index(0);
    root.add_child(std::move(drop));

    auto modal = std::make_unique<ModalPanel>();
    auto* dlg = modal.get();
    dlg->set_bounds({0, 0, 400, 400});
    auto btn = std::make_unique<Button>("OK");
    auto* ok = btn.get();
    ok->set_bounds({150, 180, 100, 32});
    int clicks = 0;
    ok->on_click([&] { ++clicks; });
    dlg->add_child(std::move(btn));
    root.add_child(std::move(modal));

    root.process_frame(0.016f, {PointerDownEvent{{80, 54}, PointerButton::Left, {}}});
    expect(menu->is_open(), "dropdown opens before modal");

    dlg->open = true;
    dlg->set_visible(true);
    // Stale open menu behind an open modal: click must hit the dialog button.
    root.process_frame(0.016f,
                       {PointerDownEvent{{200, 196}, PointerButton::Left, {}},
                        PointerUpEvent{{200, 196}, PointerButton::Left, {}},
                        ClickEvent{{200, 196}, PointerButton::Left, {}, 1}});
    expect(!menu->is_open(), "modal frame closes background exclusive popup");
    expect(clicks == 1, "modal button receives click, not background menu");

    root.process_frame(0.016f, {PointerDownEvent{{80, 54}, PointerButton::Left, {}}});
    expect(!menu->is_open(), "modal blocks opening dropdown behind scrim");
  }

  // DrawBatch / UiPainter::flush_to append — frame rebuilds must clear first.
  // (Regression: post-overlay popup_batch cleared only when empty → ghost menus.)
  {
    wds::renderer::DrawBatch batch;
    const wds::renderer::ScreenBounds screen{};
    UiPainter painter;
    painter.fill_rect({0, 0, 10, 10}, {1, 1, 1, 1});
    painter.flush_to(batch, /*solid*/ 1, 100, 100, screen);
    const auto first = batch.vertex_count();
    expect(first > 0, "flush_to emits verts");
    painter.flush_to(batch, 1, 100, 100, screen);
    expect(batch.vertex_count() == first * 2, "flush_to appends without clear");
    batch.clear();
    painter.flush_to(batch, 1, 100, 100, screen);
    expect(batch.vertex_count() == first, "clear then flush replaces frame content");

    wds::renderer::DrawBatch scratch;
    painter.flush_to(scratch, 1, 100, 100, screen);
    batch.clear();
    batch.append_from(scratch);
    const auto merged = batch.vertex_count();
    batch.append_from(scratch);
    expect(batch.vertex_count() == merged * 2, "append_from also accumulates");
  }

  return failures == 0 ? 0 : 1;
}
