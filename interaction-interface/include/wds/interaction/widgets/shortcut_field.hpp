#pragma once

#include <wds/interaction/shortcuts.hpp>
#include <wds/interaction/widget.hpp>

#include <functional>
#include <string>

namespace wds::interaction {

// Click-to-capture chord field. Modifiers update the label without committing;
// a completing key (letter / F-key / arrow / …) commits and blurs. Digits and
// period are rejected (brief red flash). Duplicate chords are allowed temporarily
// and highlighted red by the parent via set_conflict_highlight.
class ShortcutField : public Widget {
 public:
  using ChangeHandler = std::function<void(const ShortcutChord&)>;

  ShortcutField();

  void set_chord(ShortcutChord chord);
  const ShortcutChord& chord() const noexcept { return committed_; }
  void clear_chord();

  void on_change(ChangeHandler handler) { on_change_ = std::move(handler); }
  // Persistent red outline/text while this binding collides with another.
  void set_conflict_highlight(bool on) noexcept { conflict_highlight_ = on; }
  bool conflict_highlight() const noexcept { return conflict_highlight_; }

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
  void flash_reject();
  void refresh_capture_label(const Modifiers& mods);
  std::string display_text() const;

  ShortcutChord committed_{};
  ShortcutChord draft_{};
  bool capturing_ = false;
  bool conflict_highlight_ = false;
  float reject_flash_t_ = 0.0f;
  std::string capture_label_;
  ChangeHandler on_change_;
};

}  // namespace wds::interaction
