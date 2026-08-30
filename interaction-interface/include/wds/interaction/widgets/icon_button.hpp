#pragma once

#include <wds/interaction/widget.hpp>
#include <wds/renderer/draw_batch.hpp>

#include <functional>

namespace wds::interaction {

enum class Icon { Open, Save, Undo, Redo, Settings, Import, Export, Music, Convert, None };

// How convert-note buttons present their skin preview.
enum class NotePreviewStyle {
  Flat,            // note strip only (Tap / ExTap / HoldStart / Flick)
  HoldBody,        // Hold connection ribbon only (no head / tail)
  ScratchHoldBody  // Scratch connection ribbon (no arrows)
};

enum class FlickArrowMode {
  None,
  Left,
  Right,
  Both,
};

class IconButton : public Widget {
 public:
  using ClickHandler = std::function<void()>;

  explicit IconButton(Icon icon = Icon::None);

  void set_icon(Icon icon) noexcept { icon_ = icon; }
  Icon icon() const noexcept { return icon_; }
  void set_sprite(wds::renderer::TextureInfo sprite) noexcept {
    sprite_ = sprite;
    connection_ = {};
    arrow_ = {};
    preview_style_ = NotePreviewStyle::Flat;
    flick_mode_ = FlickArrowMode::None;
    connection_tint_ = {1.0f, 1.0f, 1.0f, 1.0f};
    note_preview_ = false;
  }
  void clear_sprite() noexcept {
    sprite_ = {};
    note_preview_ = false;
  }
  // Official flat note: single Top sprite (+ optional arrow). Hold: connection + RGB tint.
  void set_note_preview(NotePreviewStyle style, wds::renderer::TextureInfo top,
                        wds::renderer::TextureInfo connection = {},
                        wds::renderer::TextureInfo arrow = {},
                        FlickArrowMode flick = FlickArrowMode::None,
                        Color connection_tint = {1.0f, 1.0f, 1.0f, 1.0f}) noexcept {
    note_preview_ = true;
    preview_style_ = style;
    sprite_ = top;
    connection_ = connection;
    arrow_ = arrow;
    flick_mode_ = flick;
    connection_tint_ = connection_tint;
  }
  void set_label(std::string label) { label_ = std::move(label); }
  void on_click(ClickHandler handler) { on_click_ = std::move(handler); }

  void paint(UiPainter& painter) const override;
  bool paints_inline_tooltip() const override { return !tooltip().empty(); }
  void on_pointer_down(const PointerDownEvent& event) override;
  void on_pointer_up(const PointerUpEvent& event) override;
  void on_click(const ClickEvent& event) override;

 private:
  Icon icon_;
  wds::renderer::TextureInfo sprite_{};
  wds::renderer::TextureInfo connection_{};
  wds::renderer::TextureInfo arrow_{};
  Color connection_tint_{1.0f, 1.0f, 1.0f, 1.0f};
  NotePreviewStyle preview_style_ = NotePreviewStyle::Flat;
  FlickArrowMode flick_mode_ = FlickArrowMode::None;
  bool note_preview_ = false;
  std::string label_;
  ClickHandler on_click_;
  bool pressed_ = false;
};

}  // namespace wds::interaction
