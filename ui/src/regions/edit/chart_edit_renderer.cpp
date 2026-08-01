#include "wds/ui/regions/edit/chart_edit_renderer.hpp"

#include <wds/chart_render/note_strips.hpp>
#include <wds/core/edit_grid.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/note_edit_ops.hpp>
#include <wds/ui/note_skin_mapping.hpp>
#include "wds/ui/regions/edit/edit_gutters.hpp"

#include <algorithm>
#include <cmath>

namespace wds::ui {
namespace {

using wds::chart_editor::NoteType;

using NoteSprites = wds::ui::NoteSprites;
using wds::ui::sprites_for;

enum class NoteDrawPass {
  HoldRibbons,  // connection strips only — always under every other note layer
  NoteCaps,     // heads / tails / taps / stars (hold bodies: tail only, never start head)
};

void draw_skinned_note(wds::renderer::DrawBatch& batch, const wds::renderer::SkinCatalog& skin,
                       const EditViewport& viewport, const wds::chart_editor::NotationNote& note,
                       float alpha, float z, float arrow_z, int fb_w, int fb_h,
                       wds::renderer::ScreenBounds screen, NoteDrawPass pass) {
  // Split-lane gimmicks are authoring markers — never draw as flat/hold note art.
  if (wds::chart_editor::is_split_lane_gimmick(note.gimmick_type)) return;
  if (note.note_type == NoteType::HoldEighth || alpha <= 0.0f) return;

  const auto& bounds = viewport.bounds();
  const float note_h = viewport.note_height_px();
  const float y0 = viewport.y_at(note.start_tick);
  const float y1 = note.end_tick > note.start_tick ? viewport.y_at(note.end_tick) : y0;
  if (std::max(y0, y1) < bounds.y - note_h || std::min(y0, y1) > bounds.bottom() + note_h) {
    return;
  }

  const float inset = viewport.note_inset_px(note.width);
  const float x = viewport.x_at(note.lane) + inset;
  const float width = std::max(4.0f, viewport.lane_width(note.width) - inset * 2.0f);
  const float border_percent = EditViewport::kNoteBorderPercent;

  const NoteSprites sprites = sprites_for(skin, note.note_type);
  const bool hold_body =
      note.end_tick > note.start_tick && wds::chart_editor::is_hold_with_tail(note.note_type);

  if (pass == NoteDrawPass::HoldRibbons) {
    if (!hold_body || !sprites.connection) return;
    const float top = std::min(y0, y1);
    const float bottom = std::max(y0, y1);
    const auto body = wds::interaction::rect_to_quad(
        {x, top, width, std::max(1.0f, bottom - top)}, fb_w, fb_h, screen);
    batch.add_sprite(sprites.connection, body, z, alpha);
    return;
  }

  if (sprites.is_tick) {
    if (!sprites.tick) return;
    // Tick art is square (112×112); size by note height and keep native aspect so
    // a 1-lane-wide quad does not flatten the star.
    const float aspect =
        sprites.tick.height > 0
            ? static_cast<float>(sprites.tick.width) / static_cast<float>(sprites.tick.height)
            : 1.0f;
    float th = note_h;
    float tw = th * aspect;
    if (tw > width && width > 1.0f) {
      tw = width;
      th = tw / aspect;
    }
    const float cx = x + width * 0.5f;
    const auto q = wds::interaction::rect_to_quad({cx - tw * 0.5f, y0 - th * 0.5f, tw, th}, fb_w,
                                                  fb_h, screen);
    batch.add_sprite(sprites.tick, q, z, alpha);
    return;
  }

  // ScratchHold end uses Sirius scratchLength span; body ribbon keeps note.lane/width.
  int32_t end_lane = note.lane;
  int32_t end_width = note.width;
  if (hold_body && wds::chart_editor::is_scratch_hold_body(note.note_type)) {
    const auto range = wds::chart_editor::get_scratch_end_lane_range(note);
    end_lane = range.first;
    end_width = std::max(1, range.second - range.first + 1);
  } else if (hold_body && wds::chart_editor::is_jump_scratch(note.gimmick_type)) {
    const auto range = wds::chart_editor::get_jump_scratch_lane_range(note);
    end_lane = range.first;
    end_width = std::max(1, range.second - range.first + 1);
  }

  // scratch_length: - left, + right, 0 both (match preview / Sirius).
  // Bidirectional: pack from both edges and stop at the midline on wide notes;
  // on 1-lane notes still force one arrow per direction (otherwise aw/2 < arrow).
  const auto draw_flick_arrows = [&](float y, float ax0, float aw) {
    if (!skin.scratch_arrow || aw <= 1.0f) return;
    const float arrow_h = note_h * 0.95f;
    float arrow_w = std::clamp(viewport.lane_width(1) * 0.55f, 8.0f, note_h * 1.2f);
    arrow_w = std::min(arrow_w, aw);
    if (arrow_w <= 1.0f) return;
    const int32_t sl = note.scratch_length;
    const float step = (sl == 0) ? arrow_w * 0.9f : arrow_w * 0.55f;
    const int max_n = std::max(1, static_cast<int>(aw / std::max(step, 1.0f)) + 1);
    const float mid = ax0 + aw * 0.5f;
    auto add_arrow = [&](float ax, bool flip_x) {
      wds::renderer::Quad q = wds::interaction::rect_to_quad(
          {ax, y - arrow_h * 0.5f, arrow_w, arrow_h}, fb_w, fb_h, screen);
      if (flip_x) {
        std::swap(q.lb, q.rb);
        std::swap(q.lt, q.rt);
      }
      batch.add_sprite(skin.scratch_arrow, q, arrow_z, alpha);
    };
    if (sl <= 0) {
      int drawn = 0;
      for (int i = 0; i < max_n; ++i) {
        const float ax = ax0 + static_cast<float>(i) * step;
        if (ax + arrow_w > ax0 + aw + 0.5f) break;
        if (sl == 0 && ax + arrow_w > mid + 0.5f) break;
        add_arrow(ax, false);
        ++drawn;
      }
      // 1-lane bidirectional: midline cull would skip everything — keep one left arrow.
      if (drawn == 0) add_arrow(ax0, false);
    }
    if (sl >= 0) {
      int drawn = 0;
      for (int i = 0; i < max_n; ++i) {
        const float ax = ax0 + aw - arrow_w - static_cast<float>(i) * step;
        if (ax < ax0 - 0.5f) break;
        if (sl == 0 && ax < mid - 0.5f) break;
        add_arrow(ax, true);
        ++drawn;
      }
      if (drawn == 0) add_arrow(ax0 + aw - arrow_w, true);
    }
  };

  const auto draw_flat = [&](const NoteSprites& spr, float y, float note_z, bool with_arrow,
                             int32_t draw_lane, int32_t draw_width) {
    if (!spr.middle) return;
    const float draw_inset = viewport.note_inset_px(draw_width);
    const float draw_x = viewport.x_at(draw_lane) + draw_inset;
    const float draw_w = std::max(4.0f, viewport.lane_width(draw_width) - draw_inset * 2.0f);
    const auto head = wds::interaction::rect_to_quad({draw_x, y - note_h * 0.5f, draw_w, note_h},
                                                     fb_w, fb_h, screen);
    if (spr.left || spr.right) {
      wds::renderer::add_note_strips(batch, spr.left, spr.middle, spr.right, head, border_percent,
                                     std::max(1, draw_width), note_z, alpha);
    } else {
      batch.add_sprite(spr.middle, head, note_z, alpha);
    }
    if (with_arrow) draw_flick_arrows(y, draw_x, draw_w);
  };

  NoteSprites head = sprites;
  NoteSprites tail = sprites;
  const bool scratch_hold = hold_body && wds::chart_editor::is_scratch_hold_body(note.note_type);
  if (hold_body) {
    // Hold body never draws a start head — paired head notes own that art; missing head
    // means an empty start (authoring-correct). Caps pass only draws the tail / flick end.
    if (scratch_hold) {
      tail = {skin.note_purple_left, skin.note_purple_middle, skin.note_purple_right,
              sprites.connection, {}, true};
    } else {
      tail = {skin.note_blue_left, skin.note_blue_middle, skin.note_blue_right, sprites.connection,
              {}, false};
    }
  }

  if (hold_body) {
    if (note.end_tick > note.start_tick) {
      draw_flat(tail, y1, z, tail.is_scratch_family, end_lane, end_width);
    }
  } else {
    draw_flat(head, y0, z, head.is_scratch_family, note.lane, note.width);
    if (note.end_tick > note.start_tick) {
      draw_flat(tail, y1, z, tail.is_scratch_family, end_lane, end_width);
    }
  }
}

}  // namespace

void ChartEditRenderer::paint(wds::interaction::UiPainter& painter, const EditViewport& viewport,
                              const wds::chart_editor::MusicTiming& timing,
                              const std::vector<wds::chart_editor::NotationNote>& notes,
                              const wds::chart_editor::PreviewConfig& preview,
                              const std::unordered_set<int32_t>& selected,
                              const std::optional<EditGhost>& ghost,
                              const std::vector<EditGhost>& extra_ghosts,
                              const std::optional<wds::interaction::Rect>& marquee,
                              const wds::renderer::SkinCatalog* skin,
                              bool show_beat_grid) const {
  const auto& b = viewport.bounds();
  painter.fill_rect(b, {0.0f, 0.0f, 0.0f, 1.0f}, 0.0f, 0.86f);
  const auto& grid = viewport.grid();
  // Horizontal beat/subdiv grid (LOD + no per-frame tick vector alloc).
  if (show_beat_grid) {
    paint_horizontal_grid(painter, viewport, b);
  }

  const auto draw_lane_guides = [&](float y_top, float y_bot) {
    const float top = std::min(y_top, y_bot);
    const float bottom = std::max(y_top, y_bot);
    if (bottom < b.y || top > b.bottom()) return;
    const float clip_top = std::max(top, b.y);
    const float clip_bot = std::min(bottom, b.bottom());
    const float h = std::max(0.0f, clip_bot - clip_top);
    if (h <= 0.0f) return;
    painter.reserve_rects(static_cast<std::size_t>(grid.lane_count) + 1);
    for (int lane = 0; lane <= grid.lane_count; ++lane) {
      const bool edge = lane == 0 || lane == grid.lane_count;
      const float thickness = edge ? 1.5f : 1.0f;
      painter.fill_rect({viewport.x_at(lane) - thickness * 0.5f, clip_top, thickness, h},
                        edge ? wds::interaction::Color{0.42f, 0.44f, 0.48f, 0.95f}
                             : wds::interaction::Color{0.22f, 0.24f, 0.28f, 0.9f},
                        0.0f, 0.90f);
    }
  };

  // Default lane guides only outside split coverage (incl. fade). Coverage is
  // wall-clock ms for both editable projects and official preview — never
  // sampled on 拍内分割 / BPM→tick bands. (U1: keep ms-precision hide; do not
  // coalesce across split windows.)
  {
    const float view_ms_lo = viewport.scroll_ms();
    const float view_ms_hi = view_ms_lo + static_cast<float>(viewport.visible_ms());
    const auto merged =
        merged_split_hide_ranges_ms(notes, timing, preview, view_ms_lo, view_ms_hi);
    float cursor = view_ms_lo;
    for (const auto& seg : merged) {
      if (seg.first > cursor) {
        draw_lane_guides(viewport.y_at_ms(cursor), viewport.y_at_ms(seg.first));
      }
      cursor = std::max(cursor, seg.second);
    }
    if (view_ms_hi > cursor) {
      draw_lane_guides(viewport.y_at_ms(cursor), viewport.y_at_ms(view_ms_hi));
    }
  }

  // Split lines: continuous along Y. Soft horizontal AA via soft_split_line (no MSAA).
  // Fade in/out: 1px slices + smoothstep opacity — keep ms-level sampling (U1 skipped
  // coarsening here; split count is small).
  constexpr float kFadeSlicePx = 1.0f;
  constexpr float kSplitLineW = 7.0f;
  const auto draw_split_edges = [&](const wds::chart_editor::NotationNote& split_note, float y_a,
                                    float y_b, float opacity, int32_t /*anim_phase*/) {
    const float top = std::min(y_a, y_b);
    const float bottom = std::max(y_a, y_b);
    if (bottom < b.y || top > b.bottom()) return;
    const float clip_top = std::max(top, b.y);
    const float clip_bot = std::min(bottom, b.bottom());
    const float h = std::max(0.0f, clip_bot - clip_top);
    if (h <= 0.0f || opacity <= 0.001f) return;
    const int32_t color_id = split_note.scratch_length;
    const int32_t split_count = wds::chart_editor::get_split_count(split_note.gimmick_type);
    std::vector<int32_t> mids;
    split_boundaries_12(split_count, mids);
    const auto draw_v = [&](int32_t edge_lane, int32_t slot) {
      // Pixel-snap center so soft edges don't shimmer while scrolling.
      const float x = std::floor(viewport.x_at(edge_lane) + 0.5f);
      const wds::interaction::Rect line{x - kSplitLineW * 0.5f, clip_top, kSplitLineW, h};
      auto c = split_slot_color(color_id, slot, skin);
      // Soft strip maps a smoothstep alpha across width — covers thin-line AA without MSAA.
      if (skin != nullptr && skin->soft_split_line) {
        painter.sprite(line, skin->soft_split_line, {c.r, c.g, c.b, opacity}, 0.91f);
        return;
      }
      c.a *= opacity;
      painter.fill_rect(line, c, 0.0f, 0.91f);
    };
    draw_v(0, 0);
    int32_t slot = 1;
    for (int32_t mid : mids) draw_v(mid + 1, slot++);
    draw_v(grid.lane_count, std::max(1, split_count));
  };

  for (const auto& note : notes) {
    if (!wds::chart_editor::is_split_lane_gimmick(note.gimmick_type)) continue;
    const int64_t start_ms = note.start_ms(timing);
    const int64_t end_ms = std::max(start_ms, note.end_ms(timing));
    const int64_t appear_ms = std::max<int64_t>(
        1, static_cast<int64_t>(
               std::llround(static_cast<double>(preview.split_line_animation_start_sec) * 1000.0)));
    const int64_t disappear_ms = std::max<int64_t>(
        1, static_cast<int64_t>(
               std::llround(static_cast<double>(preview.split_line_animation_end_sec) * 1000.0)));
    const int64_t fade_start_ms = start_ms - appear_ms;
    const int64_t fade_end_ms = end_ms + disappear_ms;

    const float view_ms_lo = viewport.scroll_ms();
    const float view_ms_hi = view_ms_lo + static_cast<float>(viewport.visible_ms());
    if (static_cast<float>(fade_end_ms) < view_ms_lo ||
        static_cast<float>(fade_start_ms) > view_ms_hi) {
      continue;
    }

    const auto fade_opacity = [&](float mid_ms) {
      float o = split_line_opacity_at_ms(note, timing, preview,
                                        static_cast<int64_t>(std::llround(mid_ms)));
      // Soften fade ends so translucent bands don't read as hard steps.
      return o * o * (3.0f - 2.0f * o);
    };

    const auto paint_fade_range = [&](int64_t range_lo, int64_t range_hi, int32_t phase) {
      const float ms0 = std::max(static_cast<float>(range_lo), view_ms_lo);
      const float ms1 = std::min(static_cast<float>(range_hi), view_ms_hi);
      if (ms1 <= ms0) return;
      const float y_a = viewport.y_at_ms(ms0);
      const float y_b = viewport.y_at_ms(ms1);
      if (std::abs(y_a - y_b) < 0.5f) {
        draw_split_edges(note, y_a, y_b, fade_opacity((ms0 + ms1) * 0.5f), phase);
        return;
      }
      // Higher ms → smaller screen Y; walk from larger Y toward smaller Y.
      float y = std::max(y_a, y_b);
      const float y_stop = std::min(y_a, y_b);
      while (y > y_stop + 0.25f) {
        const float y_next = std::max(y_stop, y - kFadeSlicePx);
        const float y_mid = (y + y_next) * 0.5f;
        draw_split_edges(note, y, y_next, fade_opacity(viewport.ms_at_y(y_mid)), phase);
        y = y_next;
      }
    };

    paint_fade_range(fade_start_ms, start_ms, /*appear*/ 0);
    {
      const float ms0 = std::max(static_cast<float>(start_ms), view_ms_lo);
      const float ms1 = std::min(static_cast<float>(end_ms), view_ms_hi);
      if (ms1 > ms0) {
        draw_split_edges(note, viewport.y_at_ms(ms0), viewport.y_at_ms(ms1), 1.0f, /*steady*/ 1);
      }
    }
    paint_fade_range(end_ms, fade_end_ms, /*disappear*/ 2);
  }

  (void)selected;
  (void)ghost;
  (void)extra_ghosts;
  (void)marquee;
}

void ChartEditRenderer::paint_overlays(
    wds::interaction::UiPainter& painter, const EditViewport& viewport,
    const std::vector<wds::chart_editor::NotationNote>& notes,
    const std::unordered_set<int32_t>& selected,
    const std::optional<wds::interaction::Rect>& marquee) const {
  using wds::chart_editor::NotationNote;
  const auto& b = viewport.bounds();
  const float note_h = viewport.note_height_px();
  // Soft outer glow + thick bright border so selection reads clearly over skins.
  constexpr float kPad = 3.5f;
  constexpr float kBorder = 2.5f;
  constexpr float kZ = 0.97f;
  // Unified amber selection for all notes. Head+body together vs alone is told by
  // box shape (full chain vs clipped body / head-only) and junction ticks — not color.
  const wds::interaction::Color solo_glow{1.0f, 0.78f, 0.12f, 0.32f};
  const wds::interaction::Color solo_outline{1.0f, 0.95f, 0.35f, 1.0f};
  const wds::interaction::Color solo_inner{1.0f, 1.0f, 0.85f, 0.95f};

  auto lane_overlap = [](const NotationNote& a, const NotationNote& b) {
    return a.lane <= b.end_lane() && b.lane <= a.end_lane();
  };
  auto paired_head = [&](const NotationNote& hold) -> const NotationNote* {
    if (!wds::chart_editor::is_hold_with_tail(hold.note_type)) return nullptr;
    for (const auto& n : notes) {
      if (n.id == hold.id || n.start_tick != hold.start_tick || !lane_overlap(n, hold)) continue;
      if (wds::chart_editor::is_hold_head_note(n)) return &n;
    }
    return nullptr;
  };
  auto paired_body = [&](const NotationNote& head) -> const NotationNote* {
    if (!wds::chart_editor::is_hold_head_note(head)) return nullptr;
    for (const auto& n : notes) {
      if (!wds::chart_editor::is_hold_with_tail(n.note_type) || n.start_tick != head.start_tick ||
          !lane_overlap(n, head)) {
        continue;
      }
      return &n;
    }
    return nullptr;
  };

  auto stroke_rect = [&](const wds::interaction::Rect& r, float thickness,
                         const wds::interaction::Color& color) {
    const float t = thickness;
    painter.fill_rect({r.x, r.y, r.w, t}, color, 0.0f, kZ + 0.001f);
    painter.fill_rect({r.x, r.bottom() - t, r.w, t}, color, 0.0f, kZ + 0.001f);
    painter.fill_rect({r.x, r.y, t, r.h}, color, 0.0f, kZ + 0.001f);
    painter.fill_rect({r.right() - t, r.y, t, r.h}, color, 0.0f, kZ + 0.001f);
  };
  auto paint_box = [&](const wds::interaction::Rect& box, bool paired) {
    if (box.w <= 1.0f || box.h <= 1.0f) return;
    painter.fill_rect(box, solo_glow, 0.0f, kZ);
    stroke_rect(box, paired ? kBorder + 0.5f : kBorder, solo_outline);
    stroke_rect(box.inset(kBorder, kBorder), 1.0f, solo_inner);
  };

  for (const auto& note : notes) {
    if (!selected.count(note.id)) continue;
    if (wds::chart_editor::is_split_lane_gimmick(note.gimmick_type)) continue;

    // Mid-stars: selection box matches the tight square hit / draw size.
    if (note.note_type == NoteType::Sound || note.note_type == NoteType::SoundPurple) {
      auto star = viewport.mid_star_screen_rect(note);
      star = {star.x - kPad, star.y - kPad, star.w + kPad * 2.0f, star.h + kPad * 2.0f};
      if (star.bottom() < b.y || star.y > b.bottom()) continue;
      paint_box(star, false);
      continue;
    }

    const bool hold_body =
        note.end_tick > note.start_tick && wds::chart_editor::is_hold_with_tail(note.note_type);
    bool paired = false;
    bool clear_head_zone = false;
    if (hold_body) {
      if (const auto* head = paired_head(note)) {
        paired = selected.count(head->id) != 0;
        clear_head_zone = !paired;  // body alone: leave head sprite unhighlighted
      }
    } else if (wds::chart_editor::is_hold_head_note(note)) {
      if (const auto* body = paired_body(note)) {
        paired = selected.count(body->id) != 0;
        // When both are selected the body draws the full chain box; skip the
        // head-only box so outlines don't stack into one muddy frame.
        if (paired) continue;
      }
    }

    const float y0 = viewport.y_at(static_cast<float>(note.start_tick));
    const float y1 =
        note.end_tick > note.start_tick ? viewport.y_at(static_cast<float>(note.end_tick)) : y0;
    const float cap = note_h * 0.55f + kPad;
    float top = std::min(y0, y1) - cap;
    float bottom = std::max(y0, y1) + cap;
    if (clear_head_zone) {
      // Head sprite sits at y0; clip body selection out of that band so solo
      // body vs solo head / paired chain stay visually distinct.
      const float head_lo = y0 - note_h * 0.5f;
      const float head_hi = y0 + note_h * 0.5f;
      if (y1 < y0) {
        bottom = std::min(bottom, head_lo - kPad);
      } else if (y1 > y0) {
        top = std::max(top, head_hi + kPad);
      }
    }
    if (bottom < b.y || top > b.bottom() || bottom - top < 2.0f) continue;

    // Cover body span and (possibly wider) hold-tail lanes.
    int32_t end_lane = note.lane;
    int32_t end_width = note.width;
    if (note.end_tick > note.start_tick) {
      if (wds::chart_editor::is_scratch_hold_body(note.note_type)) {
        const auto range = wds::chart_editor::get_scratch_end_lane_range(note);
        end_lane = range.first;
        end_width = std::max(1, range.second - range.first + 1);
      } else if (wds::chart_editor::is_jump_scratch(note.gimmick_type)) {
        const auto range = wds::chart_editor::get_jump_scratch_lane_range(note);
        end_lane = range.first;
        end_width = std::max(1, range.second - range.first + 1);
      }
    }
    const float x0 = std::min(viewport.x_at(note.lane), viewport.x_at(end_lane));
    const float x1 = std::max(viewport.x_at(note.lane) + viewport.lane_width(note.width),
                              viewport.x_at(end_lane) + viewport.lane_width(end_width));
    const float inset = viewport.note_inset_px(std::max(note.width, end_width));
    const wds::interaction::Rect box{x0 + inset - kPad, top, (x1 - x0) - inset * 2.0f + kPad * 2.0f,
                                     bottom - top};
    paint_box(box, paired);

    // Paired chain: short ticks at the head junction (start of the body).
    if (paired && hold_body) {
      const float tick = std::max(6.0f, note_h * 0.35f);
      const float jy = y0;
      painter.fill_rect({box.x - 2.0f, jy - 1.5f, tick, 3.0f}, solo_outline, 0.0f, kZ + 0.002f);
      painter.fill_rect({box.right() - tick + 2.0f, jy - 1.5f, tick, 3.0f}, solo_outline, 0.0f,
                        kZ + 0.002f);
    }
  }
  if (marquee) {
    painter.fill_rect(*marquee, {0.25f, 0.55f, 1.0f, 0.18f}, 0.0f, kZ);
    stroke_rect(*marquee, 2.0f, {0.45f, 0.8f, 1.0f, 1.0f});
  }
}

void ChartEditRenderer::append_skinned_backdrop(wds::renderer::DrawBatch& batch,
                                                const wds::renderer::SkinCatalog& skin,
                                                const EditViewport& viewport, int fb_w, int fb_h,
                                                wds::renderer::ScreenBounds screen,
                                                float /*stage_opacity*/) const {
  const auto& b = viewport.bounds();
  if (b.w <= 0.0f || b.h <= 0.0f) return;
  // Judgeline must span the full edit width so its 12 cells line up with lane guides.
  if (skin.judgeline) {
    const float jh = viewport.judgeline_height_px();
    const float jy = viewport.judgeline_y() - jh * 0.5f;
    const auto jq = wds::interaction::rect_to_quad({b.x, jy, b.w, jh}, fb_w, fb_h, screen);
    batch.add_sprite(skin.judgeline, jq, depth_.judgeline, 1.0f);
  }
}

void ChartEditRenderer::append_skinned_notes(
    wds::renderer::DrawBatch& batch, const wds::renderer::SkinCatalog& skin,
    const EditViewport& viewport, const std::vector<wds::chart_editor::NotationNote>& notes,
    const std::unordered_set<int32_t>& selected, int fb_w, int fb_h,
    wds::renderer::ScreenBounds screen) const {
  const auto layer_z = [&](float base, float start_tick) {
    return base - start_tick * depth_.tick_bias;
  };
  const auto range = viewport.visible_tick_range();
  // Pad so long holds that start above the window still contribute a ribbon.
  const float tick_pad = 480.0f;
  const float tick_lo = static_cast<float>(range.first) - tick_pad;
  const float tick_hi = static_cast<float>(range.second) + tick_pad;
  auto in_window = [&](const wds::chart_editor::NotationNote& note) {
    const float end =
        note.end_tick > note.start_tick ? note.end_tick : note.start_tick;
    return end >= tick_lo && note.start_tick <= tick_hi;
  };

  auto draw_ribbons = [&] {
    for (const auto& note : notes) {
      if (!in_window(note)) continue;
      const float alpha = selected.count(note.id) != 0 ? 1.0f : 0.92f;
      draw_skinned_note(batch, skin, viewport, note, alpha, layer_z(depth_.hold_body, note.start_tick),
                        depth_.flick_arrow, fb_w, fb_h, screen, NoteDrawPass::HoldRibbons);
    }
  };
  auto draw_caps = [&] {
    for (const auto& note : notes) {
      if (!in_window(note)) continue;
      const float alpha = selected.count(note.id) != 0 ? 1.0f : 0.92f;
      const bool tick = note.note_type == NoteType::Sound || note.note_type == NoteType::SoundPurple;
      const float base = tick ? depth_.mid_star : depth_.note;
      draw_skinned_note(batch, skin, viewport, note, alpha, layer_z(base, note.start_tick),
                        layer_z(depth_.flick_arrow, note.start_tick), fb_w, fb_h, screen,
                        NoteDrawPass::NoteCaps);
    }
  };

  // depthWrite is off → draw order is stacking. Sort passes by configured depth.
  if (depth_.hold_body <= depth_.note) {
    draw_ribbons();
    draw_caps();
  } else {
    draw_caps();
    draw_ribbons();
  }
}

void ChartEditRenderer::append_skinned_ghosts(
    wds::renderer::DrawBatch& batch, const wds::renderer::SkinCatalog& skin,
    const EditViewport& viewport, const std::optional<EditGhost>& ghost,
    const std::vector<EditGhost>& extra_ghosts, int fb_w, int fb_h,
    wds::renderer::ScreenBounds screen) const {
  const auto layer_z = [&](float base, float start_tick) {
    return base - start_tick * depth_.tick_bias;
  };
  auto draw_ghost = [&](const EditGhost& g) {
    if (!g.visible) return;
    const auto& n = g.note;
    auto ribbons = [&] {
      draw_skinned_note(batch, skin, viewport, n, g.alpha,
                        layer_z(depth_.ghost_hold_body, n.start_tick),
                        layer_z(depth_.ghost_flick_arrow, n.start_tick), fb_w, fb_h, screen,
                        NoteDrawPass::HoldRibbons);
    };
    auto caps = [&] {
      const bool tick = n.note_type == NoteType::Sound || n.note_type == NoteType::SoundPurple;
      const float base = tick ? depth_.ghost_mid_star : depth_.ghost_note;
      draw_skinned_note(batch, skin, viewport, n, g.alpha, layer_z(base, n.start_tick),
                        layer_z(depth_.ghost_flick_arrow, n.start_tick), fb_w, fb_h, screen,
                        NoteDrawPass::NoteCaps);
    };
    if (depth_.ghost_hold_body <= depth_.ghost_note) {
      ribbons();
      caps();
    } else {
      caps();
      ribbons();
    }
  };
  if (ghost) draw_ghost(*ghost);
  for (const auto& g : extra_ghosts) draw_ghost(g);
}

}  // namespace wds::ui
