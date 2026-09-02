#include <wds/core/gimmick.hpp>
#include <wds/core/notation.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using wds::chart_editor::ChartDocument;
using wds::chart_editor::collect_hold_body_judge_times;
using wds::chart_editor::collect_preview_combo_hits;
using wds::chart_editor::ConcurrentLineNote;
using wds::chart_editor::GimmickType;
using wds::chart_editor::is_hold_body;
using wds::chart_editor::is_hold_mid_star;
using wds::chart_editor::is_hold_with_tail;
using wds::chart_editor::is_split_lane_gimmick;
using wds::chart_editor::MusicTiming;
using wds::chart_editor::NotationNote;
using wds::chart_editor::NoteType;
using wds::chart_editor::NoteUpdate;
using wds::chart_editor::TimingPoint;

using Clock = std::chrono::steady_clock;

constexpr int32_t kNoteCount = 5000;
constexpr int32_t kUpdateCount = 50;
constexpr int kWarmupTrials = 3;
constexpr int kTimedTrials = 20;
constexpr int32_t kLaneCount = 6;
constexpr int32_t kTickStep = 120;
constexpr int32_t kUpdateTickDelta = 240;
constexpr int32_t kHoldLengthTicks = 1920;

int fail(const char* message) {
  std::fprintf(stderr, "wds_core_bench: %s\n", message);
  return 1;
}

bool note_fields_equal(const NotationNote& a, const NotationNote& b) noexcept {
  return a.id == b.id && a.start_tick == b.start_tick && a.end_tick == b.end_tick &&
         a.note_type == b.note_type && a.lane == b.lane && a.width == b.width &&
         a.gimmick_type == b.gimmick_type && a.scratch_length == b.scratch_length;
}

bool concurrent_fields_equal(const ConcurrentLineNote& a, const ConcurrentLineNote& b) noexcept {
  return a.milliseconds == b.milliseconds && a.start_lane == b.start_lane && a.width == b.width;
}

bool sorted_ids_equal(std::vector<int32_t> left, std::vector<int32_t> right) {
  std::sort(left.begin(), left.end());
  std::sort(right.begin(), right.end());
  return left == right;
}

bool contains_id(const std::vector<int32_t>& ids, int32_t id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

const NotationNote* find_note_by_id(const std::vector<NotationNote>& notes, int32_t id) {
  for (const auto& note : notes) {
    if (note.id == id) {
      return &note;
    }
  }
  return nullptr;
}

std::vector<int32_t> candidates_at(const ChartDocument& doc, int64_t start_ms) {
  std::vector<int32_t> ids;
  doc.index().query_candidates(start_ms, 0, 0, ids);
  return ids;
}

std::vector<int32_t> splits_up_to(const ChartDocument& doc, int64_t time_ms) {
  std::vector<int32_t> ids;
  doc.index().query_split_lanes_up_to(time_ms, ids);
  return ids;
}

bool index_query_equal(const ChartDocument& left, const ChartDocument& right, int64_t time_ms,
                       int64_t lead_ms, int64_t tail_ms) {
  std::vector<int32_t> left_ids;
  std::vector<int32_t> right_ids;
  left.index().query_candidates(time_ms, lead_ms, tail_ms, left_ids);
  right.index().query_candidates(time_ms, lead_ms, tail_ms, right_ids);
  if (!sorted_ids_equal(left_ids, right_ids)) {
    return false;
  }

  left.index().query_split_lanes_up_to(time_ms, left_ids);
  right.index().query_split_lanes_up_to(time_ms, right_ids);
  return sorted_ids_equal(left_ids, right_ids);
}

bool update_index_keys_match(const ChartDocument& left, const ChartDocument& right,
                             const std::vector<NotationNote>& original_notes,
                             const std::vector<NoteUpdate>& updates) {
  const auto& timing = left.timing();
  for (const auto& update : updates) {
    const NotationNote* old_note = find_note_by_id(original_notes, update.id);
    if (old_note == nullptr) {
      std::fprintf(stderr, "wds_core_bench: missing original note id=%d\n", update.id);
      return false;
    }
    NotationNote new_note = update.note;
    new_note.id = update.id;
    const int64_t old_ms = old_note->start_ms(timing);
    const int64_t new_ms = new_note.start_ms(timing);

    const auto left_new = candidates_at(left, new_ms);
    const auto right_new = candidates_at(right, new_ms);
    if (!sorted_ids_equal(left_new, right_new)) {
      std::fprintf(stderr, "wds_core_bench: query_candidates new key mismatch id=%d ms=%lld\n",
                   update.id, static_cast<long long>(new_ms));
      return false;
    }
    if (!contains_id(left_new, update.id) || !contains_id(right_new, update.id)) {
      std::fprintf(stderr, "wds_core_bench: id=%d missing at new key ms=%lld\n", update.id,
                   static_cast<long long>(new_ms));
      return false;
    }

    const auto left_old = candidates_at(left, old_ms);
    const auto right_old = candidates_at(right, old_ms);
    if (!sorted_ids_equal(left_old, right_old)) {
      std::fprintf(stderr, "wds_core_bench: query_candidates old key mismatch id=%d ms=%lld\n",
                   update.id, static_cast<long long>(old_ms));
      return false;
    }
    if (old_ms != new_ms) {
      if (contains_id(left_old, update.id) || contains_id(right_old, update.id)) {
        std::fprintf(stderr, "wds_core_bench: id=%d still at old key ms=%lld\n", update.id,
                     static_cast<long long>(old_ms));
        return false;
      }
    }

    if (!is_split_lane_gimmick(old_note->gimmick_type) &&
        !is_split_lane_gimmick(new_note.gimmick_type)) {
      continue;
    }

    const auto left_split_new = splits_up_to(left, new_ms);
    const auto right_split_new = splits_up_to(right, new_ms);
    if (!sorted_ids_equal(left_split_new, right_split_new)) {
      std::fprintf(stderr, "wds_core_bench: split query mismatch at new_ms=%lld id=%d\n",
                   static_cast<long long>(new_ms), update.id);
      return false;
    }
    if (!contains_id(left_split_new, update.id) || !contains_id(right_split_new, update.id)) {
      std::fprintf(stderr, "wds_core_bench: split id=%d missing at new_ms=%lld\n", update.id,
                   static_cast<long long>(new_ms));
      return false;
    }
    if (old_ms == new_ms) {
      continue;
    }
    const int64_t exclude_ms = old_ms < new_ms ? old_ms : (new_ms - 1);
    const auto left_split_ex = splits_up_to(left, exclude_ms);
    const auto right_split_ex = splits_up_to(right, exclude_ms);
    if (!sorted_ids_equal(left_split_ex, right_split_ex)) {
      std::fprintf(stderr, "wds_core_bench: split query mismatch at exclude_ms=%lld id=%d\n",
                   static_cast<long long>(exclude_ms), update.id);
      return false;
    }
    if (contains_id(left_split_ex, update.id) || contains_id(right_split_ex, update.id)) {
      std::fprintf(stderr, "wds_core_bench: split id=%d still visible at exclude_ms=%lld\n",
                   update.id, static_cast<long long>(exclude_ms));
      return false;
    }
  }
  return true;
}

bool documents_semantically_equal(const ChartDocument& left, const ChartDocument& right,
                                  const std::vector<NotationNote>& original_notes,
                                  const std::vector<NoteUpdate>& updates) {
  const auto& left_notes = left.notes();
  const auto& right_notes = right.notes();
  if (left_notes.size() != right_notes.size()) {
    return false;
  }
  for (size_t i = 0; i < left_notes.size(); ++i) {
    if (!note_fields_equal(left_notes[i], right_notes[i])) {
      return false;
    }
  }

  const auto& left_lines = left.concurrent_lines();
  const auto& right_lines = right.concurrent_lines();
  if (left_lines.size() != right_lines.size()) {
    return false;
  }
  for (size_t i = 0; i < left_lines.size(); ++i) {
    if (!concurrent_fields_equal(left_lines[i], right_lines[i])) {
      return false;
    }
  }

  if (left.index().max_hold_span_ms() != right.index().max_hold_span_ms() ||
      left.index().hold_span_stale() != right.index().hold_span_stale()) {
    return false;
  }

  constexpr int64_t kProbeTimes[] = {0, 1000, 10000, 50000, 100000, 1000000000};
  constexpr int64_t kWindows[][2] = {{300, 300}, {2000, 2000}, {10000, 10000}};
  for (const int64_t time_ms : kProbeTimes) {
    for (const auto& window : kWindows) {
      if (!index_query_equal(left, right, time_ms, window[0], window[1])) {
        return false;
      }
    }
  }
  return update_index_keys_match(left, right, original_notes, updates);
}

NotationNote make_legal_note(int32_t index) {
  NotationNote note;
  note.id = index;
  note.start_tick = (index / kLaneCount) * kTickStep;
  note.lane = index % kLaneCount;
  note.width = 1;
  note.note_type = NoteType::Normal;

  if (index % 40 == 0) {
    note.note_type = NoteType::Hold;
    note.end_tick = note.start_tick + kHoldLengthTicks;
  }
  if (index % 80 == 0) {
    note.gimmick_type = GimmickType::Split3;
    note.lane = 0;
    note.width = kLaneCount;
    note.end_tick = note.start_tick + kHoldLengthTicks;
  }
  return note;
}

std::vector<NotationNote> make_notes() {
  std::vector<NotationNote> notes;
  notes.reserve(static_cast<size_t>(kNoteCount));
  for (int32_t i = 0; i < kNoteCount; ++i) {
    notes.push_back(make_legal_note(i));
  }
  return notes;
}

std::vector<NoteUpdate> make_updates(const std::vector<NotationNote>& notes) {
  std::vector<NoteUpdate> updates;
  updates.reserve(static_cast<size_t>(kUpdateCount));
  const int32_t stride = kNoteCount / kUpdateCount;
  for (int32_t i = 0; i < kUpdateCount; ++i) {
    NotationNote note = notes[static_cast<size_t>(i * stride)];
    note.start_tick += kUpdateTickDelta;
    if (note.end_tick > 0) {
      note.end_tick += kUpdateTickDelta;
    }
    updates.push_back(NoteUpdate{note.id, note});
  }
  return updates;
}

bool fill_equivalent_document(ChartDocument& doc, const std::vector<NotationNote>& notes) {
  return doc.set_notes(notes) && static_cast<int32_t>(doc.notes().size()) == kNoteCount;
}

int64_t elapsed_ns(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
}

int64_t median_ns(std::vector<int64_t> samples) {
  const size_t n = samples.size();
  std::sort(samples.begin(), samples.end());
  if (n % 2 == 0) {
    return (samples[n / 2 - 1] + samples[n / 2]) / 2;
  }
  return samples[n / 2];
}

struct TrialTimes {
  int64_t single_ns = 0;
  int64_t batch_ns = 0;
};

bool run_trial(const std::vector<NotationNote>& notes, const std::vector<NoteUpdate>& updates,
               TrialTimes* out_times) {
  ChartDocument single_doc;
  ChartDocument batch_doc;
  if (!fill_equivalent_document(single_doc, notes) ||
      !fill_equivalent_document(batch_doc, notes)) {
    std::fprintf(stderr, "wds_core_bench: failed to construct equivalent documents\n");
    return false;
  }

  const auto single_begin = Clock::now();
  for (const auto& update : updates) {
    if (!single_doc.update_note(update.id, update.note)) {
      std::fprintf(stderr, "wds_core_bench: update_note failed for id=%d\n", update.id);
      return false;
    }
  }
  const auto single_end = Clock::now();

  const auto batch_begin = Clock::now();
  if (!batch_doc.apply_note_updates(updates)) {
    std::fprintf(stderr, "wds_core_bench: apply_note_updates failed\n");
    return false;
  }
  const auto batch_end = Clock::now();

  if (!documents_semantically_equal(single_doc, batch_doc, notes, updates)) {
    std::fprintf(stderr, "wds_core_bench: notes/concurrent/index semantics differ\n");
    return false;
  }

  if (out_times != nullptr) {
    out_times->single_ns = elapsed_ns(single_begin, single_end);
    out_times->batch_ns = elapsed_ns(batch_begin, batch_end);
  }
  return true;
}

bool reference_lanes_overlap(const NotationNote& a, const NotationNote& b) noexcept {
  return a.lane <= b.end_lane() && b.lane <= a.end_lane();
}

bool reference_is_combo_head_note(const NotationNote& note) noexcept {
  if (is_split_lane_gimmick(note.gimmick_type)) {
    return false;
  }
  if (is_hold_mid_star(note.note_type) || is_hold_body(note.note_type)) {
    return false;
  }
  switch (note.note_type) {
    case NoteType::Normal:
    case NoteType::Critical:
    case NoteType::Flick:
    case NoteType::BlueTap:
    case NoteType::HoldStart:
    case NoteType::CriticalHoldStart:
    case NoteType::ScratchHoldStart:
    case NoteType::ScratchCriticalHoldStart:
      return true;
    default:
      return false;
  }
}

bool reference_mid_star_on_hold(const NotationNote& star, const NotationNote& hold) noexcept {
  if (!is_hold_mid_star(star.note_type)) return false;
  if (star.parent_hold_id >= 0) return star.parent_hold_id == hold.id;
  return reference_lanes_overlap(hold, star);
}

void reference_collect_hold_body_judge_times(const NotationNote& hold,
                                             const std::vector<NotationNote>& notes,
                                             const MusicTiming& timing,
                                             std::vector<int64_t>& out_sorted_unique) {
  out_sorted_unique.clear();
  if (!is_hold_body(hold.note_type) || is_hold_mid_star(hold.note_type)) {
    return;
  }
  const int64_t start = hold.start_ms(timing);
  const int64_t end = hold.end_ms(timing);
  if (end <= start) {
    return;
  }
  std::vector<int64_t> times;
  for (const auto& note : notes) {
    if (!is_hold_mid_star(note.note_type)) {
      continue;
    }
    const int64_t ms = note.start_ms(timing);
    if (ms <= start || ms >= end) {
      continue;
    }
    if (!reference_mid_star_on_hold(note, hold)) {
      continue;
    }
    times.push_back(ms);
  }
  std::sort(times.begin(), times.end());
  times.erase(std::unique(times.begin(), times.end()), times.end());
  out_sorted_unique = std::move(times);
}

void reference_collect_preview_combo_hits(const std::vector<NotationNote>& notes,
                                          const MusicTiming& timing,
                                          std::vector<int64_t>& out_sorted_hits) {
  out_sorted_hits.clear();
  std::vector<int64_t> hold_times;
  std::vector<char> star_consumed(notes.size(), 0);
  for (size_t i = 0; i < notes.size(); ++i) {
    const auto& note = notes[i];
    if (is_hold_body(note.note_type) && !is_hold_mid_star(note.note_type)) {
      reference_collect_hold_body_judge_times(note, notes, timing, hold_times);
      out_sorted_hits.insert(out_sorted_hits.end(), hold_times.begin(), hold_times.end());
      const int64_t start = note.start_ms(timing);
      const int64_t end = note.end_ms(timing);
      for (size_t j = 0; j < notes.size(); ++j) {
        if (!is_hold_mid_star(notes[j].note_type)) {
          continue;
        }
        const int64_t ms = notes[j].start_ms(timing);
        if (ms > start && ms < end && reference_mid_star_on_hold(notes[j], note)) {
          star_consumed[j] = 1;
        }
      }
      if (is_hold_with_tail(note.note_type) && end > start) {
        out_sorted_hits.push_back(end);
      }
      continue;
    }
    if (reference_is_combo_head_note(note)) {
      out_sorted_hits.push_back(note.start_ms(timing));
    }
  }
  for (size_t i = 0; i < notes.size(); ++i) {
    if (star_consumed[i] || !is_hold_mid_star(notes[i].note_type)) {
      continue;
    }
    if (is_split_lane_gimmick(notes[i].gimmick_type)) {
      continue;
    }
    out_sorted_hits.push_back(notes[i].start_ms(timing));
  }
  std::sort(out_sorted_hits.begin(), out_sorted_hits.end());
}

MusicTiming combo_bench_timing() {
  MusicTiming timing;
  timing.bpm = 160.0;
  timing.ticks_per_quarter = 480;
  timing.points = {TimingPoint{0, 160.0, 4, 4, true, true}};
  return timing;
}

// Hold+mid-star pairs plus taps. groups=5000 → 10000 notes, matching the audit fixture.
std::vector<NotationNote> make_combo_structure(int32_t groups) {
  std::vector<NotationNote> notes;
  notes.reserve(static_cast<size_t>(groups) * 2);
  constexpr int32_t kLanes = 12;
  constexpr int32_t kHoldTicks = 1920;
  for (int32_t i = 0; i < groups; ++i) {
    NotationNote hold;
    hold.id = i * 2;
    hold.start_tick = i * 240;
    hold.end_tick = hold.start_tick + kHoldTicks;
    hold.lane = i % kLanes;
    hold.width = (i % 11 == 0) ? 6 : ((i % 5 == 0) ? 3 : 1);
    hold.note_type = (i % 9 == 0) ? NoteType::NontailHold : NoteType::Hold;
    notes.push_back(hold);

    NotationNote star;
    star.id = i * 2 + 1;
    star.start_tick = hold.start_tick + 480 + (i % 3) * 240;
    star.end_tick = star.start_tick;
    star.lane = hold.lane;
    star.width = (i % 7 == 0) ? 3 : 1;
    star.note_type = (i % 4 == 0) ? NoteType::Sound : NoteType::HoldEighth;
    if (i % 17 == 0) {
      star.gimmick_type = GimmickType::Split3;
    }
    if (i % 13 == 0) {
      star.lane = (star.lane + 6) % kLanes;
    }
    notes.push_back(star);
  }
  return notes;
}

bool combo_hits_match_reference(const std::vector<NotationNote>& notes, const MusicTiming& timing) {
  std::vector<int64_t> got;
  std::vector<int64_t> ref;
  collect_preview_combo_hits(notes, timing, got);
  reference_collect_preview_combo_hits(notes, timing, ref);
  if (got != ref) {
    std::fprintf(stderr, "wds_core_bench: combo hits differ (got=%zu ref=%zu)\n", got.size(),
                 ref.size());
    return false;
  }
  int hold_index = 0;
  int checked_holds = 0;
  for (const auto& note : notes) {
    if (!is_hold_body(note.note_type) || is_hold_mid_star(note.note_type)) {
      continue;
    }
    const bool sample = hold_index == 0 || (hold_index % 32) == 0;
    ++hold_index;
    if (!sample) {
      continue;
    }
    collect_hold_body_judge_times(note, notes, timing, got);
    reference_collect_hold_body_judge_times(note, notes, timing, ref);
    if (got != ref) {
      std::fprintf(stderr, "wds_core_bench: hold judge times differ\n");
      return false;
    }
    ++checked_holds;
  }
  if (hold_index > 0 && checked_holds <= 0) {
    std::fprintf(stderr, "wds_core_bench: no hold samples checked\n");
    return false;
  }
  return true;
}

int run_combo_size(int32_t groups, const MusicTiming& timing, int64_t* out_ref_ns,
                   int64_t* out_prod_ns) {
  const std::vector<NotationNote> notes = make_combo_structure(groups);
  if (!combo_hits_match_reference(notes, timing)) {
    return 1;
  }

  for (int i = 0; i < kWarmupTrials; ++i) {
    std::vector<int64_t> hits;
    reference_collect_preview_combo_hits(notes, timing, hits);
    collect_preview_combo_hits(notes, timing, hits);
  }

  std::vector<int64_t> ref_samples;
  std::vector<int64_t> prod_samples;
  ref_samples.reserve(static_cast<size_t>(kTimedTrials));
  prod_samples.reserve(static_cast<size_t>(kTimedTrials));
  for (int i = 0; i < kTimedTrials; ++i) {
    std::vector<int64_t> hits;
    const auto ref_begin = Clock::now();
    reference_collect_preview_combo_hits(notes, timing, hits);
    const auto ref_end = Clock::now();
    const size_t ref_n = hits.size();

    const auto prod_begin = Clock::now();
    collect_preview_combo_hits(notes, timing, hits);
    const auto prod_end = Clock::now();
    if (hits.size() != ref_n) {
      std::fprintf(stderr, "wds_core_bench: combo size changed during timing N=%d\n", groups);
      return 1;
    }
    ref_samples.push_back(elapsed_ns(ref_begin, ref_end));
    prod_samples.push_back(elapsed_ns(prod_begin, prod_end));
  }

  *out_ref_ns = median_ns(ref_samples);
  *out_prod_ns = median_ns(prod_samples);
  const double speed =
      *out_prod_ns <= 0 ? 0.0
                        : static_cast<double>(*out_ref_ns) / static_cast<double>(*out_prod_ns);
  std::printf("wds_core_bench combo groups=%d notes=%d warmup=%d trials=%d\n", groups, groups * 2,
              kWarmupTrials, kTimedTrials);
  std::printf("median_reference_ns=%lld\n", static_cast<long long>(*out_ref_ns));
  std::printf("median_collect_preview_combo_hits_ns=%lld\n", static_cast<long long>(*out_prod_ns));
  std::printf("combo_speed_ratio=%.3f\n", speed);
  return 0;
}

int run_combo_benches() {
  const MusicTiming timing = combo_bench_timing();
  constexpr int32_t kSizes[] = {2000, 5000, 10000};
  int64_t ref_ns[3] = {0, 0, 0};
  int64_t prod_ns[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    if (run_combo_size(kSizes[i], timing, &ref_ns[i], &prod_ns[i]) != 0) {
      return 1;
    }
  }
  const auto growth = [](int64_t a, int64_t b) {
    return a <= 0 ? 0.0 : static_cast<double>(b) / static_cast<double>(a);
  };
  std::printf("combo_growth_ref_2k_to_5k=%.3f\n", growth(ref_ns[0], ref_ns[1]));
  std::printf("combo_growth_ref_5k_to_10k=%.3f\n", growth(ref_ns[1], ref_ns[2]));
  std::printf("combo_growth_prod_2k_to_5k=%.3f\n", growth(prod_ns[0], prod_ns[1]));
  std::printf("combo_growth_prod_5k_to_10k=%.3f\n", growth(prod_ns[1], prod_ns[2]));
  return 0;
}

}  // namespace

int main() {
  const std::vector<NotationNote> notes = make_notes();
  const std::vector<NoteUpdate> updates = make_updates(notes);
  if (static_cast<int32_t>(updates.size()) != kUpdateCount) {
    return fail("failed to construct updates");
  }

  for (int i = 0; i < kWarmupTrials; ++i) {
    if (!run_trial(notes, updates, nullptr)) {
      return 1;
    }
  }

  std::vector<int64_t> single_samples;
  std::vector<int64_t> batch_samples;
  single_samples.reserve(static_cast<size_t>(kTimedTrials));
  batch_samples.reserve(static_cast<size_t>(kTimedTrials));

  for (int i = 0; i < kTimedTrials; ++i) {
    TrialTimes times;
    if (!run_trial(notes, updates, &times)) {
      return 1;
    }
    single_samples.push_back(times.single_ns);
    batch_samples.push_back(times.batch_ns);
  }

  const int64_t single_median = median_ns(single_samples);
  const int64_t batch_median = median_ns(batch_samples);
  const double speed_ratio =
      batch_median <= 0 ? 0.0
                        : static_cast<double>(single_median) / static_cast<double>(batch_median);

  std::printf("wds_core_bench N=%d K=%d warmup=%d trials=%d\n", kNoteCount, kUpdateCount,
              kWarmupTrials, kTimedTrials);
  std::printf("median_update_note_ns=%lld\n", static_cast<long long>(single_median));
  std::printf("median_apply_note_updates_ns=%lld\n", static_cast<long long>(batch_median));
  std::printf("speed_ratio=%.3f\n", speed_ratio);
  return run_combo_benches();
}
