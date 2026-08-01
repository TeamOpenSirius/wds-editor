#pragma once

#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/preview_config.hpp>
#include <wds/core/preview_snapshot.hpp>

#include <cstdint>
#include <vector>

namespace wds::chart_editor {

// Stateless port of Sirius.Game.SplitLaneScheduler + SplitLaneEntry.
// Evaluates active split-lane effects at an arbitrary preview time (rollback safe).
class SplitLaneSimulator {
 public:
  explicit SplitLaneSimulator(PreviewConfig config = {});

  void set_config(PreviewConfig config);

  bool is_split_active(const NotationNote& note, const MusicTiming& timing,
                       int64_t preview_time_ms) const;

  void fill_instance(PreviewSplitLaneInstance& out, const NotationNote& note,
                     const MusicTiming& timing, int64_t preview_time_ms) const;

  void build_active_splits(const std::vector<NotationNote>& notes,
                           const std::vector<int32_t>& split_candidate_ids,
                           const MusicTiming& timing, int64_t preview_time_ms,
                           std::vector<PreviewSplitLaneInstance>& out) const;

 private:
  PreviewConfig config_;
};

}  // namespace wds::chart_editor
