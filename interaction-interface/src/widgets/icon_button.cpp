#include "wds/interaction/widgets/icon_button.hpp"

#include "wds/interaction/theme.hpp"

#include <algorithm>
#include <cmath>

namespace wds::interaction {
namespace {

void draw_icon(UiPainter& painter, Icon icon, Rect r, Color color) {
  r = r.inset(r.w * 0.18f, r.h * 0.18f);
  const float t = std::clamp(std::min(r.w, r.h) * 0.11f, 2.0f, 4.0f);
  const auto line = [&](float x, float y, float w, float h) {
    painter.fill_rect({x, y, w, h}, color, std::min(t * 0.35f, 1.5f));
  };
  // Stroke-only glyphs — never flood-fill the cell (that looked like white squares
  // when SVG sprites failed to load and Save/Open fell back here).
  switch (icon) {
    case Icon::None:
      break;
    case Icon::Open:
    case Icon::Import: {
      // Folder silhouette.
      line(r.x, r.y + r.h * 0.32f, r.w, t);
      line(r.x, r.y + r.h * 0.32f, t, r.h * 0.55f);
      line(r.right() - t, r.y + r.h * 0.32f, t, r.h * 0.55f);
      line(r.x, r.bottom() - t, r.w, t);
      line(r.x, r.y + r.h * 0.18f, r.w * 0.42f, t);
      line(r.x, r.y + r.h * 0.18f, t, r.h * 0.18f);
      break;
    }
    case Icon::Save: {
      // Floppy outline + label window.
      line(r.x, r.y, r.w, t);
      line(r.x, r.bottom() - t, r.w, t);
      line(r.x, r.y, t, r.h);
      line(r.right() - t, r.y, t, r.h);
      line(r.x + r.w * 0.22f, r.y, t, r.h * 0.28f);
      line(r.x + r.w * 0.78f - t, r.y, t, r.h * 0.28f);
      line(r.x + r.w * 0.22f, r.y + r.h * 0.28f - t, r.w * 0.56f, t);
      line(r.x + r.w * 0.2f, r.y + r.h * 0.52f, r.w * 0.6f, t);
      line(r.x + r.w * 0.2f, r.y + r.h * 0.72f, r.w * 0.4f, t);
      break;
    }
    case Icon::Undo:
    case Icon::Redo: {
      const bool reverse = icon == Icon::Undo;
      line(r.x + r.w * 0.18f, r.y + r.h * 0.48f, r.w * 0.64f, t);
      if (reverse) {
        line(r.x + r.w * 0.15f, r.y + r.h * 0.22f, t, r.h * 0.4f);
        line(r.x + r.w * 0.15f, r.y + r.h * 0.22f, r.w * 0.34f, t);
      } else {
        line(r.right() - t - r.w * 0.15f, r.y + r.h * 0.22f, t, r.h * 0.4f);
        line(r.right() - r.w * 0.49f, r.y + r.h * 0.22f, r.w * 0.34f, t);
      }
      break;
    }
    case Icon::Settings: {
      const float cx = r.x + r.w * 0.5f;
      const float cy = r.y + r.h * 0.5f;
      const float outer = r.w * 0.42f;
      const float ring = std::max(t, outer * 0.22f);
      painter.fill_rect({cx - outer, cy - ring * 0.5f, outer * 2, ring}, color, ring * 0.2f);
      painter.fill_rect({cx - ring * 0.5f, cy - outer, ring, outer * 2}, color, ring * 0.2f);
      const float hole = r.w * 0.16f;
      painter.fill_rect({cx - hole, cy - hole, hole * 2, hole * 2}, theme::kSurfaceVariant, hole);
      break;
    }
    case Icon::Export:
      line(r.x, r.y + r.h * 0.78f, r.w, t);
      line(r.x + r.w * 0.45f - t * 0.5f, r.y + r.h * 0.12f, t, r.h * 0.66f);
      line(r.x + r.w * 0.28f, r.y + r.h * 0.12f, r.w * 0.44f, t);
      line(r.x + r.w * 0.28f, r.y + r.h * 0.12f, t, r.h * 0.2f);
      line(r.x + r.w * 0.72f - t, r.y + r.h * 0.12f, t, r.h * 0.2f);
      break;
    case Icon::Music:
      line(r.x + r.w * 0.62f, r.y + r.h * 0.08f, t, r.h * 0.62f);
      line(r.x + r.w * 0.34f, r.y + r.h * 0.08f, r.w * 0.3f, t);
      painter.fill_rect({r.x + r.w * 0.1f, r.y + r.h * 0.58f, r.w * 0.32f, r.h * 0.28f}, color,
                        r.w * 0.14f);
      painter.fill_rect({r.x + r.w * 0.42f, r.y + r.h * 0.48f, r.w * 0.32f, r.h * 0.28f}, color,
                        r.w * 0.14f);
      break;
    case Icon::Convert:
      line(r.x, r.y + r.h * 0.25f, r.w * 0.75f, t);
      line(r.x + r.w * 0.25f, r.y + r.h * 0.7f, r.w * 0.75f, t);
      break;
  }
}

// Wide short flat note, centered — preserves skin aspect (no vertical stretch).
// Returns the drawn note rect so overlays (flick arrows) can track note scale.
Rect draw_note_flat(UiPainter& painter, const Rect& area, const wds::renderer::TextureInfo& top,
                    const Color& tint) {
  constexpr float kNoteAspect = 3.6f;  // width / height of a typical note bar
  float h = area.h * 0.72f;
  float w = h * kNoteAspect;
  if (w > area.w * 0.92f) {
    w = area.w * 0.92f;
    h = w / kNoteAspect;
  }
  const float x = area.x + (area.w - w) * 0.5f;
  const float y = area.y + (area.h - h) * 0.5f;
  if (top) {
    painter.sprite({x, y, w, h}, top, tint, 0.96f);
  }
  return {x, y, w, h};
}

// Arrow size tracks the note rect (not the button bounds).
void draw_flick_arrows(UiPainter& painter, const Rect& note_rect,
                       const wds::renderer::TextureInfo& arrow, FlickArrowMode mode,
                       const Color& tint) {
  if (mode == FlickArrowMode::None || !arrow || note_rect.w <= 0.0f || note_rect.h <= 0.0f) {
    return;
  }
  // Slightly taller than the note bar so the arrow tip reads clearly.
  const float ah = note_rect.h * 1.25f;
  float aw = ah * 0.85f;
  aw = std::clamp(aw, 8.0f, note_rect.w * 0.42f);
  const float y = note_rect.y + (note_rect.h - ah) * 0.5f;
  const float mid = note_rect.x + note_rect.w * 0.5f;

  if (mode == FlickArrowMode::Left) {
    painter.sprite({mid - aw * 0.5f, y, aw, ah}, arrow, tint, 0.97f, false);
  } else if (mode == FlickArrowMode::Right) {
    painter.sprite({mid - aw * 0.5f, y, aw, ah}, arrow, tint, 0.97f, true);
  } else {
    const float gap = aw * 0.12f;
    painter.sprite({mid - aw - gap * 0.5f, y, aw, ah}, arrow, tint, 0.97f, false);
    painter.sprite({mid + gap * 0.5f, y, aw, ah}, arrow, tint, 0.97f, true);
  }
}

// Hold / Scratch Hold ribbon: same width as a flat note, only a little taller.
Rect draw_hold_body(UiPainter& painter, const Rect& area,
                    const wds::renderer::TextureInfo& connection, const Color& tint) {
  constexpr float kNoteAspect = 3.6f;
  float note_h = area.h * 0.72f;
  float note_w = note_h * kNoteAspect;
  if (note_w > area.w * 0.92f) {
    note_w = area.w * 0.92f;
    note_h = note_w / kNoteAspect;
  }
  // Same width as flat notes; ~12% taller — “上下各多一点点”.
  const float h = note_h * 1.12f;
  const float w = note_w;
  const Rect body{area.x + (area.w - w) * 0.5f, area.y + (area.h - h) * 0.5f, w, h};
  if (connection) {
    painter.sprite(body, connection, tint, 0.95f);
  } else {
    painter.fill_rect(body, theme::kPrimary.lerp(theme::kSurface, 0.35f), 4.0f);
  }
  return body;
}

}  // namespace

IconButton::IconButton(Icon icon) : icon_(icon) {}

void IconButton::paint(UiPainter& painter) const {
  if (!visible_) return;
  const Rect abs = absolute_bounds();
  Color fill = enabled_ ? theme::kSurfaceVariant : theme::kSurface;
  if (enabled_ && visual_state_ == WidgetState::Hovered) fill = fill.lerp(theme::kPrimary, .18f);
  if (enabled_ && visual_state_ == WidgetState::Pressed) fill = fill.lerp(theme::kPrimary, .35f);
  painter.fill_rect(abs, fill, theme::kCornerRadiusMd);

  // Ignore 1×1 / tiny sprites (e.g. mistaken solid-white UI texture) — those paint as
  // opaque white squares when tinted. Fall back to procedural stroke glyphs instead.
  const bool sprite_usable =
      static_cast<bool>(sprite_) && sprite_.width > 2 && sprite_.height > 2;
  const bool has_sprite = sprite_usable && !note_preview_;
  const bool has_label = !label_.empty();
  const bool show_tip =
      enabled_ && !tooltip_.empty() &&
      (visual_state_ == WidgetState::Hovered || visual_state_ == WidgetState::Pressed);
  Rect icon_area = abs;
  if (has_label) {
    const float pad_x = theme::px(3.0f);
    const float pad_y = theme::px(2.0f);
    icon_area = {abs.x + pad_x, abs.y + pad_y, abs.w - pad_x * 2.0f, abs.h * 0.55f};
  } else {
    const float inset = theme::px(3.0f);
    icon_area = abs.inset(inset, inset);
  }

  Color tint = enabled_ ? Color{1, 1, 1, 1} : Color{1, 1, 1, 0.35f};
  Color icon_color = enabled_ ? theme::kOnSurface : theme::kOutline;
  if (show_tip) {
    // Fade the glyph so the centered tip reads clearly on top.
    tint.a *= 0.22f;
    icon_color.a *= 0.22f;
  }
  if (note_preview_ && (preview_style_ == NotePreviewStyle::HoldBody ||
                         preview_style_ == NotePreviewStyle::ScratchHoldBody)) {
    Color hold_tint = connection_tint_;
    hold_tint.a *= tint.a;
    draw_hold_body(painter, icon_area, connection_, hold_tint);
  } else if (note_preview_) {
    const Rect note = draw_note_flat(painter, icon_area, sprite_, tint);
    draw_flick_arrows(painter, note, arrow_, flick_mode_, tint);
  } else if (has_sprite) {
    // Always draw loaded sprites (SVG/PNG). A size gate used to drop them on
    // HiDPI / wide layouts (macOS Retina, Windows Per-Monitor DPI, Linux fractional
    // scale) when the raster lagged the button — icons then vanished or fell back.
    constexpr float kSpriteScale = 0.68f;
    const float side = std::min(icon_area.w, icon_area.h) * kSpriteScale;
  // Snap in logical pixels; physical pixel alignment happens after flush×scale.
  const float x = std::floor(icon_area.x + (icon_area.w - side) * 0.5f + 0.5f);
  const float y = std::floor(icon_area.y + (icon_area.h - side) * 0.5f + 0.5f);
  const float s = std::floor(side + 0.5f);
    painter.sprite({x, y, s, s}, sprite_, tint, 0.96f);
  } else if (icon_ != Icon::None) {
    draw_icon(painter, icon_, icon_area, icon_color);
  }

  if (show_tip) {
    painter.label(abs.inset(theme::px(2.0f), theme::px(2.0f)), tooltip_, theme::kOnSurface, 0.99f,
                  true);
  }

  if (has_label) {
    painter.label({abs.x + theme::px(1.0f), abs.y + abs.h * 0.58f, abs.w - theme::px(2.0f),
                   abs.h * 0.38f},
                  label_, enabled_ ? theme::kOnSurface : theme::kOnSurfaceMuted);
  }
}

void IconButton::on_pointer_down(const PointerDownEvent& event) {
  if (enabled_ && event.button == PointerButton::Left) {
    pressed_ = true;
    set_visual_state(WidgetState::Pressed);
  }
}

void IconButton::on_pointer_up(const PointerUpEvent& event) {
  if (event.button == PointerButton::Left) {
    pressed_ = false;
    set_visual_state(absolute_bounds().contains(event.position) ? WidgetState::Hovered
                                                                : WidgetState::Normal);
  }
}

void IconButton::on_click(const ClickEvent& event) {
  if (!enabled_ || event.button != PointerButton::Left || !on_click_) {
    return;
  }
  if (!absolute_bounds().contains(event.position)) {
    return;
  }
  on_click_();
}
}  // namespace wds::interaction
