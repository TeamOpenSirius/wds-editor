#pragma once

#include <wds/interaction/shortcuts.hpp>
#include <wds/interaction/widget.hpp>

#include <functional>
#include <string>

namespace wds::interaction {

// Click-to-capture chord field. Modifiers update the label without committing;
// a completing key (letter / F-key / arrow / …) commits and blurs. Digits and
// period are rejected. Conflict → red flash + revert. Click-away → revert.
class ShortcutField : public Widget {
 public:
  using ChangeHandler = std::function<void(const ShortcutChord&)>;
  using ConflictChecker = std::function<bool(const ShortcutChord&)>;

  ShortcutField();

  void set_chord(ShortcutChord chord);
  const ShortcutChord& chord() const noexcept { return committed_; }

  void on_change(ChangeHandler handler) { on_change_ = std::move(handler); }
  void set_conflict_checker(ConflictChecker checker) { conflict_checker_ = std::move(checker); }

  bool wants_focus() const override { return true; }
  bool is_focusable() const override { return true; }
  bool captures_keys() const override { return capturing_; }

  void paint(UiPainter& painter) const override;
  void paint_at(UiPainter& painter, float z) const;
  void update(float delta_seconds) override;
  void on_pointer_down(const PointerDownEvent& event) override;
  void on_key_down(const KeyDownEvent& event) override;
  void on_key_up(const KeyUpEvent& event) override;
  void on_focus() override;
  void on_blur() override;

 private:
  void begin_capture();
  void cancel_capture();
  void try_commit(ShortcutChord chord);
  void flash_conflict();
  void refresh_capture_label(const Modifiers& mods);
  std::string display_text() const;

  ShortcutChord committed_{};
  ShortcutChord draft_{};
  bool capturing_ = false;
  float conflict_flash_t_ = 0.0f;
  std::string capture_label_;
  ChangeHandler on_change_;
  ConflictChecker conflict_checker_;
};

}  // namespace wds::interaction
