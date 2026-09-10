#include "wds/ui/regions/edit/chart_edit_renderer.hpp"

#include <wds/chart_render/note_draw_order.hpp>
#include <wds/chart_render/note_strips.hpp>
#include <wds/chart_render/note_visual_policy.hpp>
#include <wds/core/edit_grid.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/note_edit_ops.hpp>
#include <wds/ui/note_skin_mapping.hpp>
#include "wds/ui/regions/edit/edit_gutters.hpp"

#include <wds/interaction/theme.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wds::ui {
namespace {

using wds::chart_editor::NoteType;
using wds::chart_render::NoteVisualPass;

using NoteSprites = wds::ui::NoteSprites;
using wds::ui::apply_hold_tail_sprites;
using wds::ui::sprites_for;

void draw_skinned_note(wds::renderer::DrawBatch& batch, const wds::renderer::SkinCatalog& skin,
                       const EditViewport& viewport, const wds::chart_editor::NotationNote& note,
                       float alpha, float z, float arrow_z, int fb_w, int fb_h,
                       wds::renderer::ScreenBounds screen, NoteVisualPass pass, float tint_r,
                       float tint_g, float tint_b) {
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

  const NoteSprites sprites = sprites_for(skin, note.note_type);
  const bool hold_body =
      note.end_tick > note.start_tick && wds::chart_editor::is_hold_with_tail(note.note_type);

  if (pass == NoteVisualPass::HoldBody) {
    if (!hold_body || !sprites.connection) return;
    const float hold_inset = viewport.hold_inset_px(note.width);
    const float hold_x = viewport.x_at(note.lane) + hold_inset;
    const float hold_w = std::max(4.0f, viewport.lane_width(note.width) - hold_inset * 2.0f);
    const float top = std::min(y0, y1);
    const float bottom = std::max(y0, y1);
    const auto body = wds::interaction::rect_to_quad(
        {hold_x, top, hold_w, std::max(1.0f, bottom - top)}, fb_w, fb_h, screen);
    wds::renderer::add_sliced_note(batch, sprites.connection, body, skin.hold_slice_border_l,
                                   skin.hold_slice_border_r, z, alpha, alpha, -1.0f,
                                   sprites.connection_r * tint_r, sprites.connection_g * tint_g,
                                   sprites.connection_b * tint_b,
                                   viewport.hold_visual_world_width(note.width));
    return;
  }

  if (pass == NoteVisualPass::MidStar) {
    if (!sprites.is_tick || !sprites.tick) return;
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
    batch.add_sprite(sprites.tick, q, z, alpha, tint_r, tint_g, tint_b);
    return;
  }

  if (sprites.is_tick) {
    return;
  }

  // ScratchHold / JumpScratch end span (shared with preview snapshot).
  int32_t end_lane = note.lane;
  int32_t end_width = note.width;
  if (hold_body && wds::chart_editor::is_hold_chain_body(note.note_type)) {
    const auto span = wds::chart_editor::resolve_end_lane_span(note);
    end_lane = span.first;
    end_width = span.second;
  }

  // Static arrow packing (ArrowStyle::Static); sides from scratch_arrow_sides.
  const auto draw_flick_arrows = [&](float y, float ax0, float aw) {
    if (!skin.scratch_arrow || aw <= 1.0f) return;
    const float arrow_h = note_h * 0.95f;
    float arrow_w = std::clamp(viewport.lane_width(1) * 0.55f, 8.0f, note_h * 1.2f);
    arrow_w = std::min(arrow_w, aw);
    if (arrow_w <= 1.0f) return;
    wds::chart_render::StaticArrowLayoutParams params;
    params.span_left = ax0;
    params.span_right = ax0 + aw;
    params.arrow_w = arrow_w;
    params.scratch_length = note.scratch_length;
    for (const auto& inst : wds::chart_render::layout_static_scratch_arrows(params)) {
      wds::renderer::Quad q = wds::interaction::rect_to_quad(
          {inst.x0, y - arrow_h * 0.5f, inst.x1 - inst.x0, arrow_h}, fb_w, fb_h, screen);
      if (inst.flip_x) {
        std::swap(q.lb, q.rb);
        std::swap(q.lt, q.rt);
      }
      batch.add_sprite(skin.scratch_arrow, q, arrow_z, alpha * inst.alpha, tint_r, tint_g, tint_b);
    }
  };

  const bool draw_bottom = pass == NoteVisualPass::FlatBottom;
  const bool draw_top = pass == NoteVisualPass::FlatTop;
  const bool draw_arrow = pass == NoteVisualPass::Arrow;
  if (!draw_bottom && !draw_top && !draw_arrow) return;

  const auto draw_flat = [&](const NoteSprites& spr, float y, int32_t draw_lane,
                             int32_t draw_width, bool with_arrow_art) {
    const float draw_inset = viewport.note_inset_px(draw_width);
    const float draw_x = viewport.x_at(draw_lane) + draw_inset;
    const float draw_w = std::max(4.0f, viewport.lane_width(draw_width) - draw_inset * 2.0f);
    const auto head = wds::interaction::rect_to_quad({draw_x, y - note_h * 0.5f, draw_w, note_h},
                                                     fb_w, fb_h, screen);
    const float tap_world = viewport.tap_visual_world_width(draw_width);
    if (draw_bottom) {
      if (!spr.bottom) return;
      wds::renderer::add_sliced_note(batch, spr.bottom, head, skin.note_slice_border_l,
                                     skin.note_slice_border_r, z, alpha, alpha, -1.0f, tint_r,
                                     tint_g, tint_b, tap_world);
      return;
    }
    if (draw_top) {
      if (!spr.top) return;
      wds::renderer::add_sliced_note(batch, spr.top, head, skin.note_slice_border_l,
                                     skin.note_slice_border_r, z, alpha, alpha, -1.0f, tint_r,
                                     tint_g, tint_b, tap_world);
      return;
    }
    if (draw_arrow && with_arrow_art) {
      draw_flick_arrows(y, draw_x, draw_w);
    }
  };

  NoteSprites head = sprites;
  NoteSprites tail = sprites;
  const bool scratch_hold = hold_body && wds::chart_editor::is_scratch_hold_body(note.note_type);
  if (hold_body) {
    // Hold body never draws a start head — paired head notes own that art.
    apply_hold_tail_sprites(tail, skin, scratch_hold);
    tail.connection = sprites.connection;
    tail.connection_r = sprites.connection_r;
    tail.connection_g = sprites.connection_g;
    tail.connection_b = sprites.connection_b;
  }

  if (hold_body) {
    if (note.end_tick > note.start_tick) {
      draw_flat(tail, y1, end_lane, end_width, tail.is_scratch_family);
    }
  } else {
    draw_flat(head, y0, note.lane, note.width, head.is_scratch_family);
    if (note.end_tick > note.start_tick) {
      draw_flat(tail, y1, end_lane, end_width, tail.is_scratch_family);
    }
  }
}

std::vector<size_t> make_edit_draw_order(
    const std::vector<wds::chart_editor::NotationNote>& notes,
    const wds::chart_editor::MusicTiming& timing) {
  std::vector<size_t> order;
  // Match preview: primary key is milliseconds (not tick) so BPM changes keep
  // the same overlap order as the stage view.
  wds::chart_render::build_draw_order_indices(
      notes.size(), order,
      [&](size_t i) {
        return wds::chart_editor::tick_to_milliseconds(notes[i].start_tick, timing);
      },
      [&](size_t i) { return static_cast<int32_t>(notes[i].note_type); });
  return order;
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
              bool show_beat_grid,
              int32_t highlighted_split_note_id,
              const std::vector<int32_t>& error_ticks,
              const std::unordered_set<int32_t>* violation_note_ids,
              float violation_strength,
              const wds::audio::WaveformOverview* waveform,
              const wds::renderer::TextureInfo* spectrogram,
              EditSpectrumMode spectrum_mode) const {
  const auto& b = viewport.bounds();
  painter.fill_rect(b, {0.0f, 0.0f, 0.0f, 1.0f}, 0.0f, 0.86f);
  const bool drew_spectrogram = spectrum_mode == EditSpectrumMode::Spectrogram &&
                                spectrogram != nullptr && static_cast<bool>(*spectrogram) &&
                                waveform != nullptr && waveform->duration_ms() > 0 && b.h > 0.5f &&
                                b.w > 1.0f;
  if (drew_spectrogram) {
    const float dur = static_cast<float>(waveform->duration_ms());
    const float view_lo = viewport.ms_at_y(b.bottom());
    const float view_hi = viewport.ms_at_y(b.y);
    const float a_lo = std::max(0.0f, view_lo);
    const float a_hi = std::min(dur, view_hi);
    if (a_hi > a_lo) {
      const float y_top = std::clamp(viewport.y_at_ms(a_hi), b.y, b.bottom());
      const float y_bot = std::clamp(viewport.y_at_ms(a_lo), b.y, b.bottom());
      if (y_bot > y_top + 0.5f) {
        wds::renderer::TextureInfo uv = *spectrogram;
        uv.u0 = 0.0f;
        uv.u1 = 1.0f;
        uv.v0 = a_lo / dur;
        uv.v1 = a_hi / dur;
        painter.sprite_behind({b.x, y_top, b.w, y_bot - y_top}, uv, {1.0f, 1.0f, 1.0f, 1.0f},
                              kEditWaveformZ);
      }
    }
  } else if (spectrum_mode == EditSpectrumMode::Envelope && waveform != nullptr &&
             !waveform->empty() && b.h > 0.5f && b.w > 1.0f) {
    const int y0 = static_cast<int>(std::floor(b.y));
    const int y1 = static_cast<int>(std::ceil(b.bottom()));
    const int rows = std::max(0, y1 - y0);
    painter.reserve_rects(static_cast<std::size_t>(rows) + 1);
    const float cx = b.x + b.w * 0.5f;
    const float max_w = b.w * 0.94f;
    float run_y = 0.0f;
    float run_h = 0.0f;
    float run_w = -1.0f;
    auto flush_run = [&]() {
      if (run_w <= 0.5f || run_h <= 0.0f) return;
      const float top = std::max(run_y, b.y);
      const float bot = std::min(run_y + run_h, b.bottom());
      const float h = bot - top;
      if (h <= 0.0f) return;
      painter.fill_rect({cx - run_w * 0.5f, top, run_w, h}, kEditWaveformColor, 0.0f,
                        kEditWaveformZ);
    };
    for (int y = y0; y < y1; ++y) {
      const float py = static_cast<float>(y);
      const float py1 = py + 1.0f;
      const float ms_a = viewport.ms_at_y(std::clamp(py, b.y, b.bottom()));
      const float ms_b = viewport.ms_at_y(std::clamp(py1, b.y, b.bottom()));
      const float peak = waveform->peak_in_range(static_cast<double>(ms_a),
                                                 static_cast<double>(ms_b));
      const float w = std::sqrt(std::clamp(peak, 0.0f, 1.0f)) * max_w;
      if (w <= 0.5f) {
        flush_run();
        run_w = -1.0f;
        continue;
      }
      if (run_w >= 0.0f && std::abs(w - run_w) < 1.0f) {
        run_h += 1.0f;
        run_w = std::max(run_w, w);
      } else {
        flush_run();
        run_y = py;
        run_h = 1.0f;
        run_w = w;
      }
    }
    flush_run();
  }
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
  // Fade in/out: coarse knots + vertical GPU alpha (not 1px slices).
  constexpr int kFadeKnots = 8;
  // Logical px (Retina ×2 at flush). Soft plate fills the rect — keep ≤2 so it reads thin.
  constexpr float kSplitLineW = 2.0f;
  constexpr float kSplitLineHighlightW = 4.0f;
  const auto paint_one_split = [&](const wds::chart_editor::NotationNote& note, float line_w,
                                   bool highlight) {
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
      return;
    }

    const float z = highlight ? 0.914f : 0.91f;
    const auto draw_split_edges = [&](float y_a, float y_b, float opacity_a, float opacity_b) {
      const float top = std::min(y_a, y_b);
      const float bottom = std::max(y_a, y_b);
      if (bottom < b.y || top > b.bottom()) return;
      const float clip_top = std::max(top, b.y);
      const float clip_bot = std::min(bottom, b.bottom());
      const float h = std::max(0.0f, clip_bot - clip_top);
      if (h <= 0.0f) return;
      const float a_top = (top == y_a) ? opacity_a : opacity_b;
      const float a_bot = (bottom == y_a) ? opacity_a : opacity_b;
      if (a_top <= 0.001f && a_bot <= 0.001f) return;
      const int32_t color_id = note.scratch_length;
      const int32_t split_count = wds::chart_editor::get_split_count(note.gimmick_type);
      std::vector<int32_t> mids;
      split_boundaries_12(split_count, mids);
      const auto draw_v = [&](int32_t edge_lane, int32_t slot) {
        const float x = std::floor(viewport.x_at(edge_lane) + 0.5f);
        const wds::interaction::Rect line{x - line_w * 0.5f, clip_top, line_w, h};
        auto c = split_slot_color(color_id, slot, split_count, skin);
        if (c.a < 0.02f) return;
        apply_official_split_rgb_opacity(c);
        if (highlight) {
          c = c.lerp({1.0f, 1.0f, 1.0f, c.a}, 0.28f);
        }
        if (violation_note_ids != nullptr && violation_strength > 0.0f &&
            violation_note_ids->count(note.id) != 0) {
          c = c.lerp(wds::interaction::theme::kError, violation_strength);
        }
        if (skin != nullptr && skin->soft_split_line) {
          painter.sprite_vfade(line, skin->soft_split_line, {c.r, c.g, c.b, 1.0f}, z, a_bot, a_top);
          return;
        }
        c.a *= 0.5f * (a_top + a_bot);
        painter.fill_rect(line, c, 0.0f, z);
      };
      draw_v(0, 0);
      int32_t slot = 1;
      for (int32_t mid : mids) draw_v(mid + 1, slot++);
      draw_v(grid.lane_count, std::max(1, split_count));
    };

    const auto fade_opacity = [&](float mid_ms) {
      return split_line_opacity_at_ms(note, timing, preview,
                                     static_cast<int64_t>(std::llround(mid_ms)));
    };

    const auto paint_fade_range = [&](int64_t range_lo, int64_t range_hi) {
      const float ms0 = std::max(static_cast<float>(range_lo), view_ms_lo);
      const float ms1 = std::min(static_cast<float>(range_hi), view_ms_hi);
      if (ms1 <= ms0) return;
      const float y_a = viewport.y_at_ms(ms0);
      const float y_b = viewport.y_at_ms(ms1);
      if (std::abs(y_a - y_b) < 0.5f) {
        const float o = fade_opacity((ms0 + ms1) * 0.5f);
        draw_split_edges(y_a, y_b, o, o);
        return;
      }
      for (int i = 0; i < kFadeKnots; ++i) {
        const float t0 = static_cast<float>(i) / static_cast<float>(kFadeKnots);
        const float t1 = static_cast<float>(i + 1) / static_cast<float>(kFadeKnots);
        const float msa = ms0 + (ms1 - ms0) * t0;
        const float msb = ms0 + (ms1 - ms0) * t1;
        draw_split_edges(viewport.y_at_ms(msa), viewport.y_at_ms(msb), fade_opacity(msa),
                         fade_opacity(msb));
      }
    };

    paint_fade_range(fade_start_ms, start_ms);
    {
      const float ms0 = std::max(static_cast<float>(start_ms), view_ms_lo);
      const float ms1 = std::min(static_cast<float>(end_ms), view_ms_hi);
      if (ms1 > ms0) {
        draw_split_edges(viewport.y_at_ms(ms0), viewport.y_at_ms(ms1), 1.0f, 1.0f);
      }
    }
    paint_fade_range(end_ms, fade_end_ms);
  };

  for (const auto& note : notes) {
    if (!wds::chart_editor::is_split_lane_gimmick(note.gimmick_type)) continue;
    if (note.id == highlighted_split_note_id) continue;
    paint_one_split(note, kSplitLineW, false);
  }
  for (const auto& note : notes) {
    if (!wds::chart_editor::is_split_lane_gimmick(note.gimmick_type)) continue;
    if (note.id != highlighted_split_note_id) continue;
    paint_one_split(note, kSplitLineHighlightW, true);
  }

  // Over the beat/lane grid, under skinned notes (separate later batch).
  if (!error_ticks.empty()) {
    painter.reserve_rects(error_ticks.size());
    for (int32_t tick : error_ticks) {
      const float y = viewport.y_at(tick);
      if (y < b.y || y > b.bottom()) continue;
      painter.fill_rect({b.x, y - kChartErrorMarkerThickness * 0.5f, b.w, kChartErrorMarkerThickness},
                        kChartErrorMarkerColor, 0.0f, 0.907f);
    }
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

  auto paired_head = [&](const NotationNote& hold) -> const NotationNote* {
    for (const auto& n : notes) {
      if (n.id != hold.id && wds::chart_editor::hold_head_pairs_with_body(n, hold)) return &n;
    }
    return nullptr;
  };
  auto paired_body = [&](const NotationNote& head) -> const NotationNote* {
    for (const auto& n : notes) {
      if (wds::chart_editor::hold_head_pairs_with_body(head, n)) return &n;
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
    // HoldEighth is derived and never selectable — skip any leaked selection ids.
    if (note.note_type == NoteType::HoldEighth) continue;
    if (wds::chart_editor::is_split_lane_gimmick(note.gimmick_type)) continue;

    // Mid-stars: selection box matches the tight square hit / draw size.
    if (note.note_type == NoteType::Sound || note.note_type == NoteType::ScratchSound) {
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

    const float y0 = viewport.y_at(note.start_tick);
    const float y1 =
        note.end_tick > note.start_tick ? viewport.y_at(note.end_tick) : y0;
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
    if (note.end_tick > note.start_tick &&
        wds::chart_editor::is_hold_chain_body(note.note_type)) {
      const auto span = wds::chart_editor::resolve_end_lane_span(note);
      end_lane = span.first;
      end_width = span.second;
    }
    const float x0 = std::min(viewport.x_at(note.lane), viewport.x_at(end_lane));
    const float x1 = std::max(viewport.x_at(note.lane) + viewport.lane_width(note.width),
                              viewport.x_at(end_lane) + viewport.lane_width(end_width));
    const float inset =
        (note.end_tick > note.start_tick && wds::chart_editor::is_hold_with_tail(note.note_type))
            ? viewport.hold_inset_px(std::max(note.width, end_width))
            : viewport.note_inset_px(std::max(note.width, end_width));
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
    wds::renderer::ScreenBounds screen, const std::unordered_set<int32_t>* violation_note_ids,
    float violation_strength) const {
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

  // Draw order keyed by ms (parity with preview); z bias stays tick-scaled.
  const std::vector<size_t> order = make_edit_draw_order(notes, viewport.timing());

  // Edit is a flat 2D view: skip FlatBottom (preview keeps the pseudo-3D sandwich).
  // See docs/edit-preview-render-parity.md.
  const struct {
    NoteVisualPass pass;
    float base_z;
  } passes[] = {
      {NoteVisualPass::HoldBody, depth_.hold_body},
      {NoteVisualPass::FlatTop, depth_.note},
      {NoteVisualPass::MidStar, depth_.mid_star},
      {NoteVisualPass::Arrow, depth_.flick_arrow},
  };
  for (const auto& layer : passes) {
    for (size_t idx : order) {
      const auto& note = notes[idx];
      if (!in_window(note)) continue;
      const float alpha = selected.count(note.id) != 0 ? 1.0f : 0.92f;
      const float z = layer_z(layer.base_z, static_cast<float>(note.start_tick));
      wds::interaction::Color tint{1.0f, 1.0f, 1.0f, 1.0f};
      if (violation_note_ids != nullptr && violation_strength > 0.0f &&
          violation_note_ids->count(note.id) != 0) {
        tint = tint.lerp(wds::interaction::theme::kError, violation_strength);
      }
      draw_skinned_note(batch, skin, viewport, note, alpha, z,
                        layer_z(depth_.flick_arrow, static_cast<float>(note.start_tick)), fb_w,
                        fb_h, screen, layer.pass, tint.r, tint.g, tint.b);
    }
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
    const struct {
      NoteVisualPass pass;
      float base_z;
    } passes[] = {
        {NoteVisualPass::HoldBody, depth_.ghost_hold_body},
        {NoteVisualPass::FlatTop, depth_.ghost_note},
        {NoteVisualPass::MidStar, depth_.ghost_mid_star},
        {NoteVisualPass::Arrow, depth_.ghost_flick_arrow},
    };
    for (const auto& layer : passes) {
      draw_skinned_note(batch, skin, viewport, n, g.alpha,
                        layer_z(layer.base_z, static_cast<float>(n.start_tick)),
                        layer_z(depth_.ghost_flick_arrow, static_cast<float>(n.start_tick)), fb_w,
                        fb_h, screen, layer.pass, 1.0f, 1.0f, 1.0f);
    }
  };
  if (ghost) draw_ghost(*ghost);
  for (const auto& g : extra_ghosts) draw_ghost(g);
}

}  // namespace wds::ui
