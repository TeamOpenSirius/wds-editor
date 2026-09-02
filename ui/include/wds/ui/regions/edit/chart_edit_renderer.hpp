#pragma once

#include "wds/ui/editor_ui_config.hpp"
#include "wds/ui/regions/edit/edit_draw_depth.hpp"
#include "wds/ui/regions/edit/edit_viewport.hpp"

#include <wds/audio/waveform_overview.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/preview_config.hpp>

#include <wds/interaction/ui_painter.hpp>
#include <wds/renderer/draw_batch.hpp>
#include <wds/renderer/skin_catalog.hpp>

#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

namespace wds::ui {

inline constexpr wds::interaction::Color kChartErrorMarkerColor{1.0f, 0.92f, 0.18f, 0.95f};
inline constexpr float kChartErrorMarkerThickness = 1.5f;
inline constexpr wds::interaction::Color kEditWaveformColor{0.56f, 0.56f, 0.58f, 0.38f};
inline constexpr float kEditWaveformZ = 0.868f;

struct EditGhost {
  wds::chart_editor::NotationNote note;
  bool visible = true;
  float alpha = 0.45f;
};

class ChartEditRenderer {
 public:
  const EditDrawDepthConfig& draw_depth() const noexcept { return depth_; }
  EditDrawDepthConfig& draw_depth() noexcept { return depth_; }
  void set_draw_depth(EditDrawDepthConfig depth) noexcept { depth_ = depth; }

  void paint(wds::interaction::UiPainter& painter, const EditViewport& viewport,
             const wds::chart_editor::MusicTiming& timing,
             const std::vector<wds::chart_editor::NotationNote>& notes,
             const wds::chart_editor::PreviewConfig& preview,
             const std::unordered_set<int32_t>& selected,
             const std::optional<EditGhost>& ghost = std::nullopt,
             const std::vector<EditGhost>& extra_ghosts = {},
             const std::optional<wds::interaction::Rect>& marquee = std::nullopt,
             const wds::renderer::SkinCatalog* skin = nullptr,
             bool show_beat_grid = true,
             int32_t highlighted_split_note_id = -1,
             const std::vector<int32_t>& error_ticks = {},
             const std::unordered_set<int32_t>* violation_note_ids = nullptr,
             float violation_strength = 0.0f,
             const wds::audio::WaveformOverview* waveform = nullptr,
             const wds::renderer::TextureInfo* spectrogram = nullptr,
             EditSpectrumMode spectrum_mode = EditSpectrumMode::Envelope) const;

  // Drawn after skinned notes so the highlight sits on top of sprites.
  void paint_overlays(wds::interaction::UiPainter& painter, const EditViewport& viewport,
                      const std::vector<wds::chart_editor::NotationNote>& notes,
                      const std::unordered_set<int32_t>& selected,
                      const std::optional<wds::interaction::Rect>& marquee = std::nullopt) const;

  void append_skinned_backdrop(wds::renderer::DrawBatch& batch,
                               const wds::renderer::SkinCatalog& skin,
                               const EditViewport& viewport, int fb_w, int fb_h,
                               wds::renderer::ScreenBounds screen,
                               float stage_opacity = 0.8f) const;

  void append_skinned_notes(wds::renderer::DrawBatch& batch,
                            const wds::renderer::SkinCatalog& skin,
                            const EditViewport& viewport,
                            const std::vector<wds::chart_editor::NotationNote>& notes,
                            const std::unordered_set<int32_t>& selected, int fb_w, int fb_h,
                            wds::renderer::ScreenBounds screen,
                            const std::unordered_set<int32_t>* violation_note_ids = nullptr,
                            float violation_strength = 0.0f) const;

  void append_skinned_ghosts(wds::renderer::DrawBatch& batch,
                             const wds::renderer::SkinCatalog& skin,
                             const EditViewport& viewport,
                             const std::optional<EditGhost>& ghost,
                             const std::vector<EditGhost>& extra_ghosts, int fb_w, int fb_h,
                             wds::renderer::ScreenBounds screen) const;

 private:
  EditDrawDepthConfig depth_{};
};

}  // namespace wds::ui
