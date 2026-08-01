#include <wds/core/auto_judge_simulator.hpp>

#include <cmath>
#include <cstdlib>

namespace wds::chart_editor {

AutoJudgeSimulator::AutoJudgeSimulator(PreviewConfig config) : config_(config) {}

void AutoJudgeSimulator::set_config(PreviewConfig config) { config_ = config; }

AutoJudgeResult AutoJudgeSimulator::evaluate(const NotationNote& note, int64_t preview_time_ms,
                                             const MusicTiming& timing) const {
  AutoJudgeResult result;
  result.note_id = note.id;

  const int64_t start_ms = note.start_ms(timing);
  const int64_t end_ms = note.end_ms(timing);
  const int64_t feedback_ms =
      config_.show_auto_hit_feedback ? config_.auto_hit_feedback_ms : 0;

  // HoldStart is instantaneous at start (official endTime=-1). Do NOT treat it as a
  // duration hold — that would use a bogus end_ms and hide the head / skip SFX.
  if (is_hold_start(note.note_type)) {
    if (preview_time_ms < start_ms) {
      result.visual_state = PreviewNoteVisualState::Approaching;
      return result;
    }
    result.consumed = true;
    if (feedback_ms > 0 && preview_time_ms < start_ms + feedback_ms) {
      result.visual_state = PreviewNoteVisualState::AutoHit;
    } else {
      result.visual_state = PreviewNoteVisualState::Hidden;
    }
    return result;
  }

  // Hold body / mid-star family: head despawns at start, body until end, then end VFX.
  if (is_hold_body(note.note_type)) {
    if (preview_time_ms < start_ms) {
      result.visual_state = PreviewNoteVisualState::Approaching;
      return result;
    }

    if (config_.simulate_hold_body && preview_time_ms <= end_ms) {
      result.visual_state = PreviewNoteVisualState::Holding;
      result.consumed = true;
      return result;
    }

    // Past hold end: hit-feedback window then gone (note body already despawned).
    if (feedback_ms > 0 && preview_time_ms <= end_ms + feedback_ms) {
      result.visual_state = PreviewNoteVisualState::AutoHit;
    } else {
      result.visual_state = PreviewNoteVisualState::Hidden;
    }
    result.consumed = true;
    return result;
  }

  // Flat / tick notes: visible until judgment time, then immediately gone + VFX window.
  if (preview_time_ms < start_ms) {
    result.visual_state = PreviewNoteVisualState::Approaching;
    return result;
  }

  result.consumed = true;
  if (feedback_ms > 0 && preview_time_ms < start_ms + feedback_ms) {
    result.visual_state = PreviewNoteVisualState::AutoHit;
  } else {
    result.visual_state = PreviewNoteVisualState::Hidden;
  }
  return result;
}

}  // namespace wds::chart_editor
