#include "test_support.hpp"

#include <wds/core/chart_serializer.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/note_edit_ops.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/sus_chart.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

// SUS import/export scaling + equivalence tests.
//
// 1. make_auto_hold_head: the indexed overload must return exactly what the
//    ChartDocument overload returns (randomized note sets).
// 2. with_all_hold_eighths_recomputed (batched) must equal the sequential
//    per-hold generation it replaced (randomized note sets).
// 3. Measure-length / BPM / TIL tick resolution in SusChartFormat::parse is
//    checked against a verbatim copy of the old linear walk over #mmm02 lengths.
// 4. Hand-written SUS: expected notes, parse -> serialize -> parse stability.
// 5. Scaling: ~20k holds / ~100k notes / 2000 measures must parse + serialize fast.

namespace {

using namespace wds::chart_editor;

struct Rng {
  uint32_t state = 12345u;
  uint32_t next() {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
  }
  int range(int n) { return static_cast<int>(next() % static_cast<uint32_t>(n)); }
};

bool note_equal(const NotationNote& a, const NotationNote& b) {
  return a.id == b.id && a.start_tick == b.start_tick && a.end_tick == b.end_tick &&
         a.lane == b.lane && a.width == b.width && a.note_type == b.note_type &&
         a.gimmick_type == b.gimmick_type && a.scratch_length == b.scratch_length &&
         a.parent_hold_id == b.parent_hold_id;
}

bool optional_note_equal(const std::optional<NotationNote>& a,
                         const std::optional<NotationNote>& b) {
  if (a.has_value() != b.has_value()) return false;
  return !a.has_value() || note_equal(*a, *b);
}

bool notes_equal(const std::vector<NotationNote>& a, const std::vector<NotationNote>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (!note_equal(a[i], b[i])) return false;
  }
  return true;
}

NotationNote make_note(int32_t id, NoteType type, int32_t start, int32_t end, int32_t lane,
                       int32_t width, int32_t scratch_length = 0,
                       int32_t parent = kNoBoundHoldId) {
  NotationNote n;
  n.id = id;
  n.note_type = type;
  n.start_tick = start;
  n.end_tick = end;
  n.lane = lane;
  n.width = width;
  n.scratch_length = scratch_length;
  n.parent_hold_id = parent;
  return n;
}

// ---------------------------------------------------------------------------
// 1. make_auto_hold_head differential
// ---------------------------------------------------------------------------

std::vector<NotationNote> random_head_notes(Rng& rng, int count) {
  static const NoteType kTypes[] = {
      NoteType::Normal,         NoteType::Critical,
      NoteType::Flick,          NoteType::BlueTap,
      NoteType::HoldStart,      NoteType::CriticalHoldStart,
      NoteType::ScratchHoldStart, NoteType::ScratchCriticalHoldStart,
      NoteType::Hold,           NoteType::CriticalHold,
      NoteType::ScratchHold,    NoteType::ScratchCriticalHold,
      NoteType::Sound,          NoteType::ScratchSound,
      NoteType::HoldEighth};
  constexpr int kTypeCount = static_cast<int>(sizeof(kTypes) / sizeof(kTypes[0]));
  std::vector<NotationNote> notes;
  for (int i = 0; i < count; ++i) {
    const NoteType type = kTypes[rng.range(kTypeCount)];
    const int32_t start = rng.range(5) * 10;
    const int32_t lane = rng.range(10);
    const int32_t width = 1 + rng.range(3);  // lane + width <= 12
    int32_t end = start;
    int32_t scratch = 0;
    if (is_hold_with_tail(type)) {
      end = start + (1 + rng.range(4)) * 10;
      if (is_scratch_hold_body(type)) scratch = rng.range(7) - 3;
    }
    notes.push_back(make_note(i, type, start, end, lane, width, scratch));
  }
  return notes;
}

// Same key rule as the SUS importer: tailed holds count at their end tick, other
// notes at their start tick, bodies without tail / mid-stars never count.
std::unordered_map<int32_t, std::vector<size_t>> occupancy_buckets(
    const std::vector<NotationNote>& notes) {
  std::unordered_map<int32_t, std::vector<size_t>> buckets;
  for (size_t i = 0; i < notes.size(); ++i) {
    const NotationNote& n = notes[i];
    if (is_hold_with_tail(n.note_type)) {
      buckets[n.end_tick].push_back(i);
    } else if (!is_hold_body(n.note_type) && !is_hold_mid_star(n.note_type)) {
      buckets[n.start_tick].push_back(i);
    }
  }
  return buckets;
}

void test_make_auto_hold_head_differential() {
  Rng rng;
  rng.state = 777u;
  int with_head = 0;
  int without_head = 0;
  const std::vector<size_t> no_candidates;
  for (int trial = 0; trial < 400; ++trial) {
    ChartDocument doc;
    CHECK(doc.set_notes(random_head_notes(rng, 10 + rng.range(40))));
    const std::vector<NotationNote> notes = doc.notes();
    std::vector<size_t> all(notes.size());
    for (size_t i = 0; i < all.size(); ++i) all[i] = i;
    const auto buckets = occupancy_buckets(notes);

    std::vector<NotationNote> probes;
    for (const auto& n : notes) {
      if (is_bindable_hold_body(n.note_type)) probes.push_back(n);
    }
    for (int p = 0; p < 12; ++p) {
      static const NoteType kBodies[] = {NoteType::Hold, NoteType::CriticalHold,
                                         NoteType::ScratchHold, NoteType::ScratchCriticalHold};
      const int32_t start = rng.range(5) * 10;
      const int32_t lane = rng.range(10);
      probes.push_back(make_note(kAutoNoteId, kBodies[rng.range(4)], start,
                                 start + (1 + rng.range(4)) * 10, lane, 1 + rng.range(3),
                                 rng.range(5) - 2));
    }

    for (const auto& probe : probes) {
      const auto expected = make_auto_hold_head(doc, probe);
      const auto full = make_auto_hold_head(probe, notes, all);
      const auto it = buckets.find(probe.start_tick);
      const auto bucketed =
          make_auto_hold_head(probe, notes, it == buckets.end() ? no_candidates : it->second);
      CHECK(optional_note_equal(expected, full));
      CHECK(optional_note_equal(expected, bucketed));
      if (expected.has_value()) {
        ++with_head;
      } else {
        ++without_head;
      }
    }
  }
  std::printf("head differential: %d heads, %d without head\n", with_head, without_head);
  CHECK(with_head > 0);
  CHECK(without_head > 0);
}

// ---------------------------------------------------------------------------
// 2. batched hold eighth generation vs sequential per-hold generation
// ---------------------------------------------------------------------------

std::vector<NotationNote> oracle_all_eighths(std::vector<NotationNote> notes, int32_t tpq) {
  strip_hold_eighths(notes);
  std::vector<NotationNote> holds;
  for (const auto& n : notes) {
    if (is_bindable_hold_body(n.note_type)) holds.push_back(n);
  }
  for (const auto& hold : holds) {
    notes = with_recomputed_hold_eighths(std::move(notes), hold, tpq);
  }
  return notes;
}

std::vector<NotationNote> random_eighth_notes(Rng& rng, bool unique_hold_ids) {
  // Tailed and nontail bodies generate identical eighths.
  static const NoteType kBodies[] = {
      NoteType::Hold,         NoteType::CriticalHold,        NoteType::ScratchHold,
      NoteType::ScratchCriticalHold, NoteType::NontailHold,  NoteType::NontailCriticalHold,
      NoteType::NontailScratchHold,  NoteType::NontailScratchCriticalHold};
  std::vector<NotationNote> notes;
  const int hold_count = 1 + rng.range(6);
  int32_t next_id = 0;
  for (int h = 0; h < hold_count; ++h) {
    const int32_t start = rng.range(5) * 240 + (rng.range(4) == 0 ? 120 : 0);
    const int32_t length = rng.range(8) == 0 ? 100 : rng.range(7) * 240;
    int32_t id = next_id++;
    if (!unique_hold_ids) {
      const int roll = rng.range(4);
      if (roll == 0) id = -1;
      else if (roll == 1) id = 0;
    }
    notes.push_back(make_note(id, kBodies[rng.range(8)], start, start + length, rng.range(8),
                              1 + rng.range(4)));
  }
  const int other_count = rng.range(15);
  for (int i = 0; i < other_count; ++i) {
    static const NoteType kOthers[] = {NoteType::Sound, NoteType::ScratchSound,
                                       NoteType::HoldEighth, NoteType::HoldEighth,
                                       NoteType::Normal};
    const NoteType type = kOthers[rng.range(5)];
    // Stale / file-provided eighths of any shape (bound, unbound, dangling parent, odd
    // scratch_length) must all be dropped and recomputed.
    int32_t scratch = 0;
    if (type == NoteType::HoldEighth) scratch = rng.range(3) - 1;
    int32_t parent = kNoBoundHoldId;
    const int parent_roll = rng.range(4);
    if (parent_roll == 1) parent = rng.range(hold_count);
    if (parent_roll == 2) parent = 99;
    const int32_t tick = rng.range(12) * 240 + (rng.range(5) == 0 ? 120 : 0);
    notes.push_back(
        make_note(next_id++, type, tick, tick, rng.range(10), 1 + rng.range(3), scratch, parent));
  }
  return notes;
}

void test_batched_hold_eighths() {
  Rng rng;
  rng.state = 4242u;
  size_t generated_total = 0;
  for (int trial = 0; trial < 600; ++trial) {
    const bool unique_ids = (trial % 3) != 2;
    const std::vector<NotationNote> input = random_eighth_notes(rng, unique_ids);
    const auto expected = oracle_all_eighths(input, 480);
    const auto actual = with_all_hold_eighths_recomputed(input, 480);
    if (!notes_equal(expected, actual)) {
      std::fprintf(stderr, "eighth mismatch in trial %d (%zu vs %zu notes)\n", trial,
                   expected.size(), actual.size());
    }
    CHECK(notes_equal(expected, actual));
    generated_total += actual.size();
  }
  CHECK(generated_total > 0);
}

// ---------------------------------------------------------------------------
// 3. measure / BPM / TIL tick resolution vs the old linear walk
// ---------------------------------------------------------------------------

constexpr int kTpq = 480;

double oracle_beats(int m) {
  static const double kLens[] = {4.0, 3.0, 5.0, 2.5, 7.0, 6.0, 4.5};
  return kLens[(m * 3 + m / 7) % 7];  // consecutive measures always differ
}

// Verbatim shape of the old per-event walk over the #mmm02 map.
int64_t old_measure_len_ticks(const std::map<int, double>& lengths, int measure) {
  double beats = 4.0;
  for (const auto& [mm, len] : lengths) {
    if (mm > measure) break;
    beats = len;
  }
  return static_cast<int64_t>(std::llround(beats * static_cast<double>(kTpq)));
}

struct TilPair {
  int m1;
  int sub1;
  int m2;
  int sub2;
  int lines;
  int types;
};

void test_measure_helpers_against_oracle() {
  constexpr int kMeasures = 400;
  std::map<int, double> lengths;
  for (int m = 0; m < kMeasures; ++m) lengths[m] = oracle_beats(m);
  std::vector<int64_t> start(static_cast<size_t>(kMeasures) + 1, 0);
  for (int m = 0; m < kMeasures; ++m) {
    start[static_cast<size_t>(m) + 1] = start[static_cast<size_t>(m)] + old_measure_len_ticks(lengths, m);
  }

  const std::vector<TilPair> til_pairs = {
      {10, 480, 20, 0, 2, 1}, {30, 100, 45, 1919, 3, 2}, {150, 960, 151, 0, 4, 3}};

  std::string sus =
      "#REQUEST \"ticks_per_beat 480\"\n#BPM01: 120\n#BPM02: 150\n#BPM03: 90\n#BPM04: 200\n";
  {
    // Close entries listed before the matching opens: parse must order them by tick.
    std::string til = "#TIL01: \"";
    bool first = true;
    for (const auto& p : til_pairs) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "%d'%d:-%d.%d, %d'%d:%d.%d", p.m2, p.sub2, p.lines, p.types,
                    p.m1, p.sub1, p.lines, p.types);
      if (!first) til += ", ";
      til += buf;
      first = false;
    }
    til += "\"\n";
    sus += til;
  }

  std::vector<int64_t> expected_taps;
  std::vector<std::pair<int64_t, double>> expected_bpm;  // tick, bpm
  static const double kBpm[] = {120.0, 150.0, 90.0, 200.0};
  int bpm_index = 0;
  for (int m = 0; m < kMeasures; ++m) {
    char mm[8];
    std::snprintf(mm, sizeof(mm), "%03d", m);
    const int64_t mlen = old_measure_len_ticks(lengths, m);
    char len_line[64];
    std::snprintf(len_line, sizeof(len_line), "#%s02: %.1f\n", mm, oracle_beats(m));
    if (m % 2 == 0) sus += len_line;

    const int slot = (m * 5 + 3) % 16;
    std::string tap(32, '0');
    tap[static_cast<size_t>(slot) * 2] = '1';
    tap[static_cast<size_t>(slot) * 2 + 1] = '1';
    sus += std::string("#") + mm + "14: " + tap + "\n";
    expected_taps.push_back(start[static_cast<size_t>(m)] + mlen * slot / 16);

    if (m % 3 == 1) {
      std::string bpm = "00000000";
      bpm[5] = static_cast<char>('1' + bpm_index % 4);  // slot 2 of 4: "0X"
      sus += std::string("#") + mm + "08: " + bpm + "\n";
      expected_bpm.emplace_back(start[static_cast<size_t>(m)] + mlen * 2 / 4, kBpm[bpm_index % 4]);
      ++bpm_index;
    }
    if (m % 2 != 0) sus += len_line;
  }

  SusChartLoadResult loaded;
  const auto result = SusChartFormat::parse(sus, loaded);
  CHECK(result.error == SerializeError::Ok);
  if (result.error != SerializeError::Ok) return;

  std::vector<int64_t> got_taps;
  std::vector<std::pair<int64_t, int64_t>> got_splits;
  for (const auto& n : loaded.chart.notes) {
    if (n.note_type == NoteType::Normal) got_taps.push_back(n.start_tick);
    if (n.note_type == NoteType::None && is_split_lane_gimmick(n.gimmick_type)) {
      got_splits.emplace_back(n.start_tick, n.end_tick);
    }
  }
  std::sort(expected_taps.begin(), expected_taps.end());
  std::sort(got_taps.begin(), got_taps.end());
  CHECK(got_taps == expected_taps);

  std::vector<std::pair<int64_t, int64_t>> expected_splits;
  for (const auto& p : til_pairs) {
    const int64_t s = start[static_cast<size_t>(p.m1)] +
                      old_measure_len_ticks(lengths, p.m1) * p.sub1 / 1920;
    const int64_t e = start[static_cast<size_t>(p.m2)] +
                      old_measure_len_ticks(lengths, p.m2) * p.sub2 / 1920;
    expected_splits.emplace_back(s, e);
  }
  std::sort(expected_splits.begin(), expected_splits.end());
  std::sort(got_splits.begin(), got_splits.end());
  CHECK(got_splits == expected_splits);

  // BPM changes after the first one differ from their predecessor, so each must
  // survive timing normalisation as a point at the oracle tick.
  for (size_t j = 1; j < expected_bpm.size(); ++j) {
    bool found = false;
    for (const auto& point : loaded.chart.timing.points) {
      if (point.tick == expected_bpm[j].first && point.has_bpm &&
          std::fabs(point.bpm - expected_bpm[j].second) < 1e-9) {
        found = true;
        break;
      }
    }
    if (!found) {
      std::fprintf(stderr, "missing bpm point %zu at tick %lld\n", j,
                   static_cast<long long>(expected_bpm[j].first));
    }
    CHECK(found);
  }
  // Every measure whose length differs from the previous one carries a meter point.
  for (int m = 1; m < kMeasures; ++m) {
    bool found = false;
    for (const auto& point : loaded.chart.timing.points) {
      if (point.tick == start[static_cast<size_t>(m)] && point.has_meter) {
        found = true;
        break;
      }
    }
    if (!found) std::fprintf(stderr, "missing meter point at measure %d\n", m);
    CHECK(found);
  }
}

// ---------------------------------------------------------------------------
// 4. hand-written SUS: expected notes + round trip
// ---------------------------------------------------------------------------

std::string dump_chart(const NotationChart& chart) {
  std::string out;
  char buf[160];
  for (const auto& n : chart.notes) {
    std::snprintf(buf, sizeof(buf), "%d:%d:%d-%d:%d+%d:%d:%d:%d;", n.id,
                  static_cast<int>(n.note_type), n.start_tick, n.end_tick, n.lane, n.width,
                  n.scratch_length, static_cast<int>(n.gimmick_type), n.parent_hold_id);
    out += buf;
  }
  out += "T";
  for (const auto& p : chart.timing.points) {
    std::snprintf(buf, sizeof(buf), "%d:%.4f:%d/%d:%d%d;", p.tick, p.bpm, p.numerator,
                  p.denominator, p.has_bpm ? 1 : 0, p.has_meter ? 1 : 0);
    out += buf;
  }
  out += "S";
  for (const auto& s : chart.scroll_speed_points) {
    std::snprintf(buf, sizeof(buf), "%d:%.3f;", s.tick, s.multiplier);
    out += buf;
  }
  return out;
}

using NoteKey = std::tuple<int, int, int, int, int>;  // type, start, end, lane, width

struct HandCase {
  const char* name;
  std::string body;
  bool check_notes;              // compare against `expected`
  bool check_stable;             // dump(parse) == dump(parse(serialize(parse)))
  std::vector<NoteKey> expected;
};

NoteKey key_of(NoteType type, int start, int end, int lane, int width) {
  return NoteKey{static_cast<int>(type), start, end, lane, width};
}

// Import always appends the derived HoldEighth rows of a hold (one per tpq/2 step
// strictly inside the span, except ticks taken by a visible mid star).
std::vector<NoteKey> with_eighths(std::vector<NoteKey> keys, int start, int end, int lane,
                                  int width, int occupied_tick = -1) {
  constexpr int kStep = kTpq / 2;
  for (int t = start + kStep; t < end; t += kStep) {
    if (t != occupied_tick) keys.push_back(key_of(NoteType::HoldEighth, t, t, lane, width));
  }
  return keys;
}

std::vector<HandCase> hand_cases() {
  const std::string hold = "#000340: 12000022\n";  // SUS lane 4, width 2, 0 -> 1440
  std::vector<HandCase> cases;
  // SUS lane offset 2 is detected, so SUS lane 4 becomes lane 2.
  // Hold 0 -> 1440 at tpq 480 gets eighths at 240, 480, 720, 960, 1200.
  cases.push_back({"uncovered_head", hold, true, true,
                   with_eighths({key_of(NoteType::Hold, 0, 1440, 2, 2),
                                 key_of(NoteType::HoldStart, 0, 0, 2, 2)},
                                0, 1440, 2, 2)});
  cases.push_back({"partly_covered", hold + "#00014: 11000000\n", true, true,
                   with_eighths({key_of(NoteType::Normal, 0, 0, 2, 1),
                                 key_of(NoteType::Hold, 0, 1440, 2, 2),
                                 key_of(NoteType::HoldStart, 0, 0, 3, 1)},
                                0, 1440, 2, 2)});
  cases.push_back({"fully_covered", hold + "#00014: 12000000\n", true, true,
                   with_eighths({key_of(NoteType::Normal, 0, 0, 2, 2),
                                 key_of(NoteType::Hold, 0, 1440, 2, 2)},
                                0, 1440, 2, 2)});
  cases.push_back({"critical_cover", hold + "#00014: 22000000\n", true, true,
                   with_eighths({key_of(NoteType::Critical, 0, 0, 2, 2),
                                 key_of(NoteType::CriticalHold, 0, 1440, 2, 2)},
                                0, 1440, 2, 2)});
  cases.push_back({"damage_start", hold + "#00014: 42000000\n", true, true,
                   with_eighths({key_of(NoteType::Hold, 0, 1440, 2, 2)}, 0, 1440, 2, 2)});
  cases.push_back({"damage_both", hold + "#00014: 42000042\n", true, true,
                   with_eighths({key_of(NoteType::NontailHold, 0, 1440, 2, 2)}, 0, 1440, 2, 2)});
  // The mid star at 480 takes that eighth slot.
  cases.push_back({"mid_star", hold + "#000350: 00310000\n", true, true,
                   with_eighths({key_of(NoteType::Hold, 0, 1440, 2, 2),
                                 key_of(NoteType::HoldStart, 0, 0, 2, 2),
                                 key_of(NoteType::Sound, 480, 480, 3, 1)},
                                0, 1440, 2, 2, 480)});
  cases.push_back({"legacy_hold", "#000240: 12000022\n", true, true,
                   with_eighths({key_of(NoteType::Hold, 0, 1440, 2, 2),
                                 key_of(NoteType::HoldStart, 0, 0, 2, 2)},
                                0, 1440, 2, 2)});
  // Remaining cases only have to import, export and re-import without errors.
  cases.push_back({"end_air_up", hold + "#00054: 00000012\n", false, false, {}});
  cases.push_back({"end_air_left", hold + "#00052: 00000042\n", false, false, {}});
  cases.push_back({"end_air_right", hold + "#00054: 00000032\n", false, false, {}});
  cases.push_back({"start_air_only", hold + "#00054: 12000000\n", false, false, {}});
  cases.push_back(
      {"mid_air_two_segments", hold + "#00054: 00120000\n#00054: 00120012\n", false, false, {}});
  cases.push_back({"two_channels_shared_start",
                   hold + "#000351: 12000022\n#00014: 11000000\n", false, false, {}});
  cases.push_back({"tail_blocks_head",
                   "#000340: 12220000\n#000560: 12000022\n#000351: 00001200\n", false, false, {}});
  cases.push_back(
      {"flick_tap", "#00014: 31000000\n#00054: 10000000\n#00016: 11000011\n", false, false, {}});
  cases.push_back({"meters_bpm_til",
                   "#BPM02: 150\n#BPM03: 90\n#00102: 3\n#00202: 5.5\n#00408: 02000300\n"
                   "#000340: 12000022\n#001340: 12000022\n#002340: 12000022\n"
                   "#00214: 11000000\n#005350: 12000000\n#006350: 22000000\n"
                   "#TIL00: \"0'0:1.0, 1'480:2.0, 3'0:0.5\"\n"
                   "#TIL01: \"0'0:2.1, 1'960:-2.1, 2'0:3.0, 2'1000:-3.0\"\n",
                   false, false, {}});
  return cases;
}

void test_hand_written_cases() {
  const std::string header = "#REQUEST \"ticks_per_beat 480\"\n#BPM01: 120\n";
  for (const auto& c : hand_cases()) {
    SusChartLoadResult first;
    const auto parsed = SusChartFormat::parse(header + c.body, first);
    if (parsed.error != SerializeError::Ok) std::fprintf(stderr, "case %s: parse failed\n", c.name);
    CHECK(parsed.error == SerializeError::Ok);
    if (parsed.error != SerializeError::Ok) continue;

    if (c.check_notes) {
      std::vector<NoteKey> got;
      for (const auto& n : first.chart.notes) {
        got.push_back(key_of(n.note_type, n.start_tick, n.end_tick, n.lane, n.width));
      }
      std::vector<NoteKey> want = c.expected;
      std::sort(got.begin(), got.end());
      std::sort(want.begin(), want.end());
      if (got != want) {
        std::fprintf(stderr, "case %s: unexpected notes (%zu got, %zu expected)\n", c.name,
                     got.size(), want.size());
      }
      CHECK(got == want);
    }

    std::string text;
    SusChartSaveOptions options;
    const auto saved = SusChartFormat::serialize(first.chart, options, text);
    if (saved.error != SerializeError::Ok) std::fprintf(stderr, "case %s: serialize failed\n", c.name);
    CHECK(saved.error == SerializeError::Ok);
    if (saved.error != SerializeError::Ok) continue;

    SusChartLoadResult second;
    const auto reparsed = SusChartFormat::parse(text, second);
    if (reparsed.error != SerializeError::Ok) std::fprintf(stderr, "case %s: reparse failed\n", c.name);
    CHECK(reparsed.error == SerializeError::Ok);
    if (reparsed.error != SerializeError::Ok) continue;

    if (c.check_stable) {
      const std::string a = dump_chart(first.chart);
      const std::string b = dump_chart(second.chart);
      if (a != b) {
        std::fprintf(stderr, "case %s round trip differs\n  first:  %s\n  second: %s\n", c.name,
                     a.c_str(), b.c_str());
      }
      CHECK(a == b);
    }
  }
}

// Regression: a Sirius Extended note anywhere in the file must not change the head
// generated for a hold (the old parser saw no neighbours once such a note was pushed).
std::vector<NotationNote> parse_sus_notes(const std::string& body, bool sirius_header) {
  std::string text = "#REQUEST \"ticks_per_beat 480\"\n#BPM01: 120\n";
  if (sirius_header) text += "#WDS_PROFILE SiriusExtended\n#WDS_LANE_OFFSET 2\n";
  text += body;
  SusChartLoadResult result;
  const auto parsed = SusChartFormat::parse(text, result);
  CHECK(parsed.error == SerializeError::Ok);
  return result.chart.notes;
}

std::vector<NotationNote> hold_heads_of(const std::vector<NotationNote>& notes) {
  std::vector<NotationNote> heads;
  for (const auto& n : notes) {
    if (n.note_type == NoteType::HoldStart) heads.push_back(n);
  }
  return heads;
}

bool any_requires_extended(const std::vector<NotationNote>& notes) {
  return std::any_of(notes.begin(), notes.end(), [](const NotationNote& n) {
    return note_requires_sirius_extended(n);
  });
}

bool head_overlaps_tap(const std::vector<NotationNote>& notes, const NotationNote& head) {
  for (const auto& n : notes) {
    if (n.note_type != NoteType::Normal || n.start_tick != head.start_tick) continue;
    if (n.lane <= head.end_lane() && head.lane <= n.end_lane()) return true;
  }
  return false;
}

void test_extended_note_does_not_change_head() {
  const std::string hold = "#000340: 12000022\n";  // SUS lane 4 width 2 -> WDS lane 2..3
  // Extended tap: SUS lane 14 (e) -> WDS lane 12, so lane + width > 12.
  const std::string extended = "#0001e: 00110000\n";

  // Partly covered start: tap on WDS lane 2 -> head must be lane 3, width 1.
  const std::string partly = hold + "#00014: 11000000\n";
  const auto a = parse_sus_notes(partly, true);
  const auto b = parse_sus_notes(extended + partly, true);
  CHECK(!any_requires_extended(a));
  CHECK(any_requires_extended(b));
  const auto heads_a = hold_heads_of(a);
  const auto heads_b = hold_heads_of(b);
  CHECK_EQ(static_cast<int>(heads_a.size()), 1);
  CHECK_EQ(static_cast<int>(heads_b.size()), 1);
  if (heads_a.size() == 1 && heads_b.size() == 1) {
    for (const auto* h : {&heads_a.front(), &heads_b.front()}) {
      CHECK_EQ(h->lane, 3);
      CHECK_EQ(h->width, 1);
      CHECK_EQ(h->start_tick, 0);
      CHECK_EQ(h->end_tick, 0);
    }
    CHECK(!head_overlaps_tap(a, heads_a.front()));
    CHECK(!head_overlaps_tap(b, heads_b.front()));
  }

  // Start lanes split into two free runs -> no head, with or without the extended note.
  const std::string split = "#000340: 13000023\n#00015: 11000000\n";
  const auto c = parse_sus_notes(split, true);
  const auto d = parse_sus_notes(extended + split, true);
  CHECK(!any_requires_extended(c));
  CHECK(any_requires_extended(d));
  CHECK(hold_heads_of(c).empty());
  CHECK(hold_heads_of(d).empty());
}

// ---------------------------------------------------------------------------
// 5. scaling
// ---------------------------------------------------------------------------

// Deterministic synthetic chart. Per measure: `holds_per_measure` slide holds on
// distinct channels (some with tap covers, damage markers, end airs, mid stars), a
// number of taps, occasional #mmm02 / #mmm08 lines. Measures >= 1000 use #MEASUREBS.
std::string generate_large_sus(int measures, int holds_per_measure, int taps_per_measure) {
  std::string s;
  s.reserve(static_cast<size_t>(measures) * 1100);
  s += "#REQUEST \"ticks_per_beat 480\"\n#BPM01: 120\n#BPM02: 150\n#BPM03: 90\n#BPM04: 200\n";
  Rng rng;
  auto next = [&]() { return rng.next(); };
  static const char* kB36 = "0123456789abcdefghijklmnopqrstuvwxyz";
  static const double kLens[] = {4.0, 3.0, 5.0, 6.0, 2.5, 7.0};
  for (int m = 0; m < measures; ++m) {
    if (m > 0 && m % 1000 == 0) s += "#MEASUREBS " + std::to_string(m) + "\n";
    char mm[8];
    std::snprintf(mm, sizeof(mm), "%03d", m % 1000);
    if (m % 5 == 1) {
      char line[64];
      std::snprintf(line, sizeof(line), "#%s02: %.1f\n", mm, kLens[next() % 6]);
      s += line;
    }
    if (m % 7 == 3) {
      std::string d(32, '0');
      d[1] = kB36[1 + next() % 4];
      d[17] = kB36[1 + next() % 4];
      s += std::string("#") + mm + "08: " + d + "\n";
    }
    std::vector<std::string> tap_lines(14, std::string(32, '0'));
    std::vector<std::string> air_lines(14, std::string(32, '0'));
    for (int k = 0; k < holds_per_measure; ++k) {
      const int lane = 2 + (k % 8);
      const int width = 2;
      const int a = 1 + static_cast<int>(next() % 5);
      const int b = 8 + static_cast<int>(next() % 6);
      std::string d(32, '0');
      d[static_cast<size_t>(a) * 2] = '1';
      d[static_cast<size_t>(a) * 2 + 1] = kB36[width];
      d[static_cast<size_t>(b) * 2] = '2';
      d[static_cast<size_t>(b) * 2 + 1] = kB36[width];
      if (k % 3 == 0) {
        const int mid = (a + b) / 2;
        if (mid > a && mid < b) {
          d[static_cast<size_t>(mid) * 2] = '3';
          d[static_cast<size_t>(mid) * 2 + 1] = kB36[width];
        }
      }
      s += std::string("#") + mm + "3" + kB36[lane] + kB36[k % 36] + ": " + d + "\n";
      auto& tl = tap_lines[static_cast<size_t>(lane)];
      if (k % 4 == 1) {  // partial cover
        tl[static_cast<size_t>(a) * 2] = '1';
        tl[static_cast<size_t>(a) * 2 + 1] = '1';
      } else if (k % 4 == 2) {  // full / critical cover
        tl[static_cast<size_t>(a) * 2] = (k % 8 == 2) ? '2' : '1';
        tl[static_cast<size_t>(a) * 2 + 1] = kB36[width];
      } else if (k % 7 == 3) {  // damage marker at the hold start
        tl[static_cast<size_t>(a) * 2] = '4';
        tl[static_cast<size_t>(a) * 2 + 1] = kB36[width];
      }
      if (k % 5 == 0) {  // end air
        auto& al = air_lines[static_cast<size_t>(lane)];
        al[static_cast<size_t>(b) * 2] = "134"[k % 3];
        al[static_cast<size_t>(b) * 2 + 1] = kB36[width];
      }
    }
    for (int t = 0; t < taps_per_measure; ++t) {
      const int lane = 2 + static_cast<int>(next() % 12);
      const int slot = static_cast<int>(next() % 16);
      auto& tl = tap_lines[static_cast<size_t>(lane)];
      if (tl[static_cast<size_t>(slot) * 2] == '0') {
        tl[static_cast<size_t>(slot) * 2] = (t % 4 == 0) ? '2' : '1';
        tl[static_cast<size_t>(slot) * 2 + 1] = '1';
      }
    }
    for (int lane = 0; lane < 14; ++lane) {
      if (tap_lines[static_cast<size_t>(lane)].find_first_not_of('0') != std::string::npos) {
        s += std::string("#") + mm + "1" + kB36[lane] + ": " +
             tap_lines[static_cast<size_t>(lane)] + "\n";
      }
      if (air_lines[static_cast<size_t>(lane)].find_first_not_of('0') != std::string::npos) {
        s += std::string("#") + mm + "5" + kB36[lane] + ": " +
             air_lines[static_cast<size_t>(lane)] + "\n";
      }
    }
  }
  return s;
}

double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void test_mid_size_round_trip() {
  const std::string sus = generate_large_sus(120, 8, 20);
  SusChartLoadResult first;
  CHECK(SusChartFormat::parse(sus, first).error == SerializeError::Ok);
  std::string text;
  SusChartSaveOptions options;
  CHECK(SusChartFormat::serialize(first.chart, options, text).error == SerializeError::Ok);
  SusChartLoadResult second;
  CHECK(SusChartFormat::parse(text, second).error == SerializeError::Ok);
  CHECK(!first.chart.notes.empty());
  CHECK(!second.chart.notes.empty());
}

void test_scaling() {
  const std::string sus = generate_large_sus(2000, 10, 25);
  const auto t0 = std::chrono::steady_clock::now();
  SusChartLoadResult loaded;
  const auto pr = SusChartFormat::parse(sus, loaded);
  const double parse_s = seconds_since(t0);
  CHECK(pr.error == SerializeError::Ok);
  if (pr.error != SerializeError::Ok) return;

  const auto t1 = std::chrono::steady_clock::now();
  std::string text;
  SusChartSaveOptions options;
  const auto sr = SusChartFormat::serialize(loaded.chart, options, text);
  const double ser_s = seconds_since(t1);
  CHECK(sr.error == SerializeError::Ok);

  size_t holds = 0;
  for (const auto& n : loaded.chart.notes) {
    if (is_hold_with_tail(n.note_type)) ++holds;
  }
  std::printf("sus perf: %zu bytes, %zu notes (%zu holds), parse %.3fs, serialize %.3fs\n",
              sus.size(), loaded.chart.notes.size(), holds, parse_s, ser_s);
  CHECK(holds >= 19000);
  CHECK(loaded.chart.notes.size() > 40000);
#ifdef NDEBUG
  constexpr double kBound = 5.0;
#else
  constexpr double kBound = 60.0;  // unoptimised / sanitizer builds
#endif
  CHECK(parse_s + ser_s < kBound);
}

}  // namespace

int main() {
  test_make_auto_hold_head_differential();
  test_batched_hold_eighths();
  test_measure_helpers_against_oracle();
  test_hand_written_cases();
  test_extended_note_does_not_change_head();
  test_mid_size_round_trip();
  test_scaling();

  if (wds::chart_editor::test::failure_count() != 0) {
    std::fprintf(stderr, "%d failure(s)\n", wds::chart_editor::test::failure_count());
    return 1;
  }
  std::printf("sus_import_perf_tests: all passed\n");
  return 0;
}
