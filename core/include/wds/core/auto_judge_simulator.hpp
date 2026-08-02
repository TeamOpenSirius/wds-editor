#pragma once

#include <wds/core/notation.hpp>
#include <wds/core/preview_config.hpp>
#include <wds/core/types.hpp>

#include <cstdint>
#include <vector>

namespace wds::chart_editor {

struct AutoJudgeResult {
  int32_t note_id = 0;
  PreviewNoteVisualState visual_state = PreviewNoteVisualState::Hidden;
  bool consumed = false;
};

// Stateless auto-mode judge helper (port concept from AutoTouch + TimingDecider).
// Re-evaluated from scratch at each preview time -> supports rollback.
class AutoJudgeSimulator {
 public:
  explicit AutoJudgeSimulator(PreviewConfig config = {});

  void set_config(PreviewConfig config);

  AutoJudgeResult evaluate(const NotationNote& note,
                           int64_t preview_time_ms,
                           const MusicTiming& timing) const;

 private:
  PreviewConfig config_;
};

}  // namespace wds::chart_editor
