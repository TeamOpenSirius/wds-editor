// Tests for the index-backed tick lookups:
//  - ChartNoteIndex end-time tree (query_ends_in_range) stays in sync with every
//    ChartDocument mutator.
//  - ChartDocument::note_indices_starting_in / tail_note_indices_ending_in match a
//    brute-force scan exactly (indices, order).
//  - The rewritten note_edit_ops lookups return exactly what the previous linear-scan
//    implementations returned. The "old_*" functions below are verbatim copies of the
//    scan bodies from before the refactor and act as oracles.

#include "test_support.hpp"

#include <wds/core/chart_index.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/note_edit_ops.hpp>
#include <wds/core/notation.hpp>
#include <wds/core/scratch_hold_curve.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace wds::chart_editor;

// ---------------------------------------------------------------------------
// Generators
// ---------------------------------------------------------------------------

int rnd(std::mt19937& rng, int lo, int hi) {
  return std::uniform_int_distribution<int>(lo, hi)(rng);
}

bool chance(std::mt19937& rng, int percent) { return rnd(rng, 0, 99) < percent; }

NotationNote make_note(NoteType type, int32_t start, int32_t end, int32_t lane, int32_t width) {
  NotationNote n;
  n.note_type = type;
  n.start_tick = start;
  n.end_tick = end;
  n.lane = lane;
  n.width = width;
  return n;
}

// Single tempo, or several fractional-BPM changes (stresses tick->ms rounding).
MusicTiming make_timing(std::mt19937& rng, bool multi) {
  MusicTiming t;
  t.ticks_per_quarter = 480;
  t.offset_ms = rnd(rng, 0, 700);
  TimingPoint p0;
  p0.tick = 0;
  p0.bpm = 120.0;
  t.points.push_back(p0);
  if (multi) {
    TimingPoint p1;
    p1.tick = 1920;
    p1.bpm = 137.3;
    t.points.push_back(p1);
    TimingPoint p2;
    p2.tick = 5760;
    p2.bpm = 93.7;
    t.points.push_back(p2);
    if (chance(rng, 50)) {
      TimingPoint p3;
      p3.tick = 9600;
      p3.bpm = 171.9;
      t.points.push_back(p3);
    }
  }
  return t;
}

NotationNote random_note(std::mt19937& rng, int slots) {
  static constexpr NoteType kTypes[] = {
      NoteType::Normal,
      NoteType::Critical,
      NoteType::BlueTap,
      NoteType::Flick,
      NoteType::HoldStart,
      NoteType::CriticalHoldStart,
      NoteType::ScratchHoldStart,
      NoteType::ScratchCriticalHoldStart,
      NoteType::Hold,
      NoteType::CriticalHold,
      NoteType::ScratchHold,
      NoteType::ScratchCriticalHold,
      NoteType::NontailHold,
      NoteType::NontailCriticalHold,
      NoteType::NontailScratchHold,
      NoteType::NontailScratchCriticalHold,
      NoteType::Sound,
      NoteType::ScratchSound,
      NoteType::HoldEighth,
  };
  constexpr int kTypeCount = static_cast<int>(sizeof(kTypes) / sizeof(kTypes[0]));
  NotationNote n;
  n.note_type = kTypes[rnd(rng, 0, kTypeCount - 1)];
  n.width = rnd(rng, 1, 4);
  n.lane = rnd(rng, 0, 12 - n.width);
  n.start_tick = rnd(rng, 0, slots - 1) * 120;
  n.end_tick = n.start_tick;
  if (is_hold_with_tail(n.note_type) || is_nontail_hold_body(n.note_type)) {
    const int roll = rnd(rng, 0, 99);
    if (roll < 4) {
      n.end_tick = n.start_tick;  // degenerate: no span
    } else if (roll < 8) {
      n.end_tick = n.start_tick - 120;  // degenerate: reversed (may be negative)
    } else {
      n.end_tick = n.start_tick + rnd(rng, 1, 6) * 120;
    }
  }
  if (is_scratch_hold_body(n.note_type)) {
    const int s = rnd(rng, 0, 2);
    n.scratch_length = s == 0 ? 0 : (s == 1 ? n.width : -n.width);
  }
  return n;
}

// Dense chart: many notes per tick/lane, hold chains, paired / legacy heads, bound and
// unbound stars and eighths, tails ending on other notes' starts. Ids are 0..n-1 in
// push order so parent_hold_id references are known while generating.
std::vector<NotationNote> random_notes(std::mt19937& rng, int count, int slots) {
  std::vector<NotationNote> notes;
  const auto push = [&notes](NotationNote n) -> int32_t {
    n.id = static_cast<int32_t>(notes.size());
    notes.push_back(n);
    return n.id;
  };
  while (static_cast<int>(notes.size()) < count) {
    NotationNote n = random_note(rng, slots);
    if (n.note_type == NoteType::Sound || n.note_type == NoteType::ScratchSound) {
      n.parent_hold_id = (chance(rng, 40) && !notes.empty())
                             ? rnd(rng, 0, static_cast<int>(notes.size()) - 1)
                             : kNoBoundHoldId;
      push(n);
      continue;
    }
    const int32_t hold_id = push(n);
    if (!is_hold_with_tail(n.note_type) || n.end_tick <= n.start_tick) continue;

    const bool scratch = is_scratch_hold_body(n.note_type);
    const bool gold =
        n.note_type == NoteType::CriticalHold || n.note_type == NoteType::ScratchCriticalHold;
    if (chance(rng, 55)) {
      NotationNote head = make_note(NoteType::HoldStart, n.start_tick, n.start_tick, n.lane, n.width);
      if (scratch && chance(rng, 50)) {
        const int legacy = rnd(rng, 0, 2);
        head.note_type = legacy == 0 ? NoteType::Normal
                                     : (legacy == 1 ? NoteType::Critical : NoteType::BlueTap);
      } else if (scratch) {
        head.note_type = gold ? NoteType::ScratchCriticalHoldStart : NoteType::ScratchHoldStart;
      } else {
        head.note_type = gold ? NoteType::CriticalHoldStart : NoteType::HoldStart;
      }
      push(head);
    }
    const int span = (n.end_tick - n.start_tick) / 120;
    if (span >= 2 && chance(rng, 35)) {
      NotationNote star = make_note(scratch ? NoteType::ScratchSound : NoteType::Sound,
                                    n.start_tick + rnd(rng, 1, span - 1) * 120,
                                    0, n.lane, n.width);
      star.end_tick = star.start_tick;
      star.parent_hold_id = chance(rng, 75) ? hold_id : kNoBoundHoldId;
      if (chance(rng, 15) && n.lane + 1 + n.width <= 12) star.lane = n.lane + 1;
      push(star);
    }
    if (span >= 2 && chance(rng, 25)) {
      NotationNote eighth = make_note(NoteType::HoldEighth,
                                      n.start_tick + rnd(rng, 1, span - 1) * 120, 0, n.lane,
                                      n.width);
      eighth.end_tick = eighth.start_tick;
      eighth.parent_hold_id = chance(rng, 60) ? hold_id : kNoBoundHoldId;
      push(eighth);
    }
    if (chance(rng, 35)) {
      // Follower starting at this hold's end. Mostly the same lanes + a plain tail so
      // the pair is a connected chain.
      notes[static_cast<std::size_t>(hold_id)].scratch_length = chance(rng, 70) ? 0 : n.scratch_length;
      if (notes[static_cast<std::size_t>(hold_id)].scratch_length == 0) {
        notes[static_cast<std::size_t>(hold_id)].gimmick_type = GimmickType::None;
      }
      NotationNote next = n;
      next.start_tick = n.end_tick;
      next.end_tick = n.end_tick + rnd(rng, 1, 4) * 120;
      next.scratch_length = 0;
      next.gimmick_type = GimmickType::None;
      next.parent_hold_id = kNoBoundHoldId;
      if (chance(rng, 30)) {
        next.lane = rnd(rng, 0, 12 - next.width);
      }
      push(next);
    }
    if (chance(rng, 15)) {
      // A tap on the hold's end tick: occupies the next hold start's lanes.
      push(make_note(NoteType::Normal, n.end_tick, n.end_tick, rnd(rng, 0, 11), 1));
    }
  }
  return notes;
}

void init_doc(ChartDocument& doc, const std::vector<NotationNote>& notes) {
  CHECK(doc.set_compatibility_profile(ChartCompatibilityProfile::SiriusExtended));
  CHECK(doc.set_notes(notes));
}

bool same_note(const NotationNote& a, const NotationNote& b) {
  return a.id == b.id && a.start_tick == b.start_tick && a.end_tick == b.end_tick &&
         a.note_type == b.note_type && a.lane == b.lane && a.width == b.width &&
         a.gimmick_type == b.gimmick_type && a.scratch_length == b.scratch_length &&
         a.parent_hold_id == b.parent_hold_id;
}

bool same_opt(const std::optional<NotationNote>& a, const std::optional<NotationNote>& b) {
  if (a.has_value() != b.has_value()) return false;
  return !a.has_value() || same_note(*a, *b);
}

bool same_note_list(const std::vector<NotationNote>& a, const std::vector<NotationNote>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (!same_note(a[i], b[i])) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Index sync (end tree) and tick helpers vs brute force
// ---------------------------------------------------------------------------

std::vector<int32_t> brute_end_ids(const ChartDocument& doc, int64_t lo, int64_t hi) {
  std::vector<int32_t> ids;
  for (const auto& n : doc.notes()) {
    if (!is_hold_with_tail(n.note_type)) continue;
    const int64_t ms = tick_to_milliseconds(n.end_tick, doc.timing());
    if (ms >= lo && ms <= hi) ids.push_back(n.id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

// parent_hold_id -> children map vs brute force: every referenced parent (plus a few
// absent ones) returns exactly the ascending indices of notes bound to it.
void check_parent_map(const ChartDocument& doc) {
  const auto& notes = doc.notes();
  std::vector<int32_t> parents = {-1, -5, 0, 99999, std::numeric_limits<int32_t>::max()};
  for (const auto& n : notes) {
    parents.push_back(n.id);
    parents.push_back(n.parent_hold_id);
  }
  std::sort(parents.begin(), parents.end());
  parents.erase(std::unique(parents.begin(), parents.end()), parents.end());
  std::vector<std::size_t> got = {7777};  // must be cleared
  std::vector<std::size_t> want;
  std::size_t bound_total = 0;
  for (const int32_t p : parents) {
    want.clear();
    if (p >= 0) {
      for (std::size_t i = 0; i < notes.size(); ++i) {
        if (notes[i].parent_hold_id == p) want.push_back(i);
      }
    }
    doc.note_indices_with_parent(p, got);
    CHECK(got == want);
    bound_total += want.size();
  }
  std::size_t expected_total = 0;
  for (const auto& n : notes) expected_total += n.parent_hold_id >= 0 ? 1 : 0;
  CHECK_EQ(bound_total, expected_total);  // no parent missed
}

void check_end_tree(const ChartDocument& doc, std::mt19937& rng) {
  check_parent_map(doc);
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  std::vector<int32_t> got;

  doc.index().query_ends_in_range(kMin, kMax, got);
  std::sort(got.begin(), got.end());
  CHECK(std::adjacent_find(got.begin(), got.end()) == got.end());  // no duplicate ids
  CHECK(got == brute_end_ids(doc, kMin, kMax));

  // Start tree holds every note exactly once.
  std::vector<int32_t> starts;
  doc.index().query_candidates(0, kMax, kMax, starts);
  CHECK_EQ(starts.size(), doc.notes().size());

  doc.index().query_ends_in_range(10, 5, got);  // lower > upper
  CHECK(got.empty());

  const auto& notes = doc.notes();
  if (notes.empty()) return;
  for (int k = 0; k < 6; ++k) {
    const auto& n = notes[static_cast<std::size_t>(rnd(rng, 0, static_cast<int>(notes.size()) - 1))];
    const int64_t ms = tick_to_milliseconds(n.end_tick, doc.timing());
    const int64_t windows[4][2] = {{ms - 1, ms + 1}, {ms, ms}, {ms - 700, ms + 700}, {ms + 1, ms + 90}};
    for (const auto& w : windows) {
      doc.index().query_ends_in_range(w[0], w[1], got);
      std::sort(got.begin(), got.end());
      CHECK(got == brute_end_ids(doc, w[0], w[1]));
    }
    // Start tree: the note is found at its own start ms.
    const int64_t start_ms = tick_to_milliseconds(n.start_tick, doc.timing());
    doc.index().query_candidates(start_ms, 0, 0, starts);
    CHECK(std::find(starts.begin(), starts.end(), n.id) != starts.end());
  }
}

void check_lookups(const ChartDocument& doc, std::mt19937& rng, int rounds) {
  const auto& notes = doc.notes();
  std::vector<std::size_t> got;
  std::vector<std::size_t> want;
  for (int r = 0; r < rounds; ++r) {
    int32_t lo = 0;
    int32_t hi = 0;
    switch (rnd(rng, 0, 5)) {
      case 0:
        lo = hi = rnd(rng, -2, 45) * 120;
        break;
      case 1:
        lo = rnd(rng, -300, 5000);
        hi = lo + rnd(rng, 0, 2400);
        break;
      case 2:
        lo = hi = rnd(rng, 0, 45) * 120 + rnd(rng, -2, 2);
        break;
      case 3:
        lo = rnd(rng, 0, 3000);
        hi = lo - rnd(rng, 1, 50);  // reversed
        break;
      case 4:
        lo = std::numeric_limits<int32_t>::min();
        hi = std::numeric_limits<int32_t>::max();
        break;
      default:
        lo = rnd(rng, 0, 40) * 120 - 1;
        hi = lo + 2;
        break;
    }
    want.clear();
    for (std::size_t i = 0; i < notes.size(); ++i) {
      if (notes[i].start_tick >= lo && notes[i].start_tick <= hi) want.push_back(i);
    }
    doc.note_indices_starting_in(lo, hi, got);
    CHECK(got == want);
    if (lo == hi) {
      doc.note_indices_starting_at(lo, got);
      CHECK(got == want);
    }

    want.clear();
    for (std::size_t i = 0; i < notes.size(); ++i) {
      if (is_hold_with_tail(notes[i].note_type) && notes[i].end_tick >= lo &&
          notes[i].end_tick <= hi) {
        want.push_back(i);
      }
    }
    doc.tail_note_indices_ending_in(lo, hi, got);
    CHECK(got == want);
    if (lo == hi) {
      doc.tail_note_indices_ending_at(lo, got);
      CHECK(got == want);
    }
  }
}

void test_end_tree_scenarios() {
  std::mt19937 rng(11);
  ChartDocument doc(make_timing(rng, false));
  CHECK(doc.set_compatibility_profile(ChartCompatibilityProfile::SiriusExtended));
  check_end_tree(doc, rng);  // empty

  const int32_t a = doc.add_note(make_note(NoteType::Hold, 480, 960, 2, 2));
  const int32_t tap = doc.add_note(make_note(NoteType::Normal, 480, 480, 0, 1));
  const int32_t b = doc.add_note(make_note(NoteType::ScratchHold, 240, 960, 6, 3));
  CHECK(a >= 0 && tap >= 0 && b >= 0);
  check_end_tree(doc, rng);
  check_lookups(doc, rng, 40);

  // Two tails on the same end tick, then remove one.
  CHECK(doc.remove_note(a));
  check_end_tree(doc, rng);
  CHECK(doc.remove_notes({b, tap}));
  check_end_tree(doc, rng);

  std::vector<int32_t> ids;
  CHECK(doc.add_notes({make_note(NoteType::Hold, 0, 480, 0, 1),
                       make_note(NoteType::CriticalHold, 120, 480, 1, 1),
                       make_note(NoteType::NontailHold, 120, 720, 2, 1),
                       make_note(NoteType::Normal, 480, 480, 3, 1)},
                      &ids));
  CHECK_EQ(ids.size(), static_cast<std::size_t>(4));
  check_end_tree(doc, rng);
  check_lookups(doc, rng, 40);

  // Update start only (end fixed).
  NotationNote n = *doc.find_note(ids[0]);
  n.start_tick = 240;
  CHECK(doc.update_note(ids[0], n));
  check_end_tree(doc, rng);

  // Update end only.
  n = *doc.find_note(ids[0]);
  n.end_tick = 1200;
  CHECK(doc.update_note(ids[0], n));
  check_end_tree(doc, rng);
  {
    std::vector<int32_t> hit;
    doc.index().query_ends_in_range(tick_to_milliseconds(1200, doc.timing()),
                                    tick_to_milliseconds(1200, doc.timing()), hit);
    CHECK(std::find(hit.begin(), hit.end(), ids[0]) != hit.end());
    doc.index().query_ends_in_range(tick_to_milliseconds(480, doc.timing()),
                                    tick_to_milliseconds(480, doc.timing()), hit);
    CHECK(std::find(hit.begin(), hit.end(), ids[0]) == hit.end());
  }

  // Non-key edit (lane only).
  n = *doc.find_note(ids[0]);
  n.lane = 7;
  CHECK(doc.update_note(ids[0], n));
  check_end_tree(doc, rng);

  // Tail -> non-tail types, and back.
  n = *doc.find_note(ids[1]);
  n.note_type = NoteType::Normal;
  n.end_tick = n.start_tick;
  CHECK(doc.update_note(ids[1], n));
  check_end_tree(doc, rng);
  n = *doc.find_note(ids[1]);
  n.note_type = NoteType::Hold;
  n.end_tick = n.start_tick + 360;
  CHECK(doc.update_note(ids[1], n));
  check_end_tree(doc, rng);
  n = *doc.find_note(ids[1]);
  n.note_type = NoteType::NontailHold;  // same ticks, no tail
  CHECK(doc.update_note(ids[1], n));
  check_end_tree(doc, rng);
  n = *doc.find_note(ids[2]);
  n.note_type = NoteType::ScratchCriticalHold;  // nontail -> tail, same ticks
  CHECK(doc.update_note(ids[2], n));
  check_end_tree(doc, rng);

  // Batch update of two notes (one moves, one changes type).
  {
    NotationNote u0 = *doc.find_note(ids[0]);
    u0.start_tick = 0;
    u0.end_tick = 600;
    NotationNote u3 = *doc.find_note(ids[3]);
    u3.note_type = NoteType::Hold;
    u3.end_tick = u3.start_tick + 240;
    CHECK(doc.apply_note_updates({NoteUpdate{ids[0], u0}, NoteUpdate{ids[3], u3}}));
    check_end_tree(doc, rng);
  }
  check_lookups(doc, rng, 60);

  // Timing / offset changes re-key everything.
  {
    MusicTiming t = doc.timing();
    t.points[0].bpm = 150.0;
    CHECK(doc.set_timing(t));
    check_end_tree(doc, rng);
    check_lookups(doc, rng, 40);
    CHECK(doc.set_offset_ms(123));
    check_end_tree(doc, rng);
    CHECK(doc.set_timing(make_timing(rng, true)));
    check_end_tree(doc, rng);
    check_lookups(doc, rng, 40);
  }

  // set_notes / load_from_chart / normalize_for_save.
  CHECK(doc.set_notes(random_notes(rng, 40, 12)));
  check_end_tree(doc, rng);
  check_lookups(doc, rng, 60);
  {
    const NotationChart chart = doc.to_notation_chart();
    doc.load_from_chart(chart);
    check_end_tree(doc, rng);
    check_lookups(doc, rng, 40);
  }
  CHECK(doc.normalize_for_save());
  check_end_tree(doc, rng);
  check_lookups(doc, rng, 40);
}

void test_end_tree_and_lookups_randomized_sequence() {
  for (int seed = 1; seed <= 6; ++seed) {
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed * 104729));
    ChartDocument doc(make_timing(rng, seed % 2 == 0));
    CHECK(doc.set_compatibility_profile(ChartCompatibilityProfile::SiriusExtended));
    for (int step = 0; step < 250; ++step) {
      const int size = static_cast<int>(doc.notes().size());
      const int op = size > 70 ? 3 : rnd(rng, 0, 9);
      switch (op) {
        case 0:
        case 1:
          CHECK(doc.add_note(random_note(rng, 16)) >= 0);
          break;
        case 2:
          CHECK(doc.add_notes({random_note(rng, 16), random_note(rng, 16), random_note(rng, 16)}));
          break;
        case 3:
          if (size > 0) {
            CHECK(doc.remove_note(doc.notes()[static_cast<std::size_t>(rnd(rng, 0, size - 1))].id));
          }
          break;
        case 4:
          if (size > 1) {
            const int i0 = rnd(rng, 0, size - 1);
            int i1 = rnd(rng, 0, size - 1);
            if (i1 == i0) i1 = (i1 + 1) % size;
            CHECK(doc.remove_notes({doc.notes()[static_cast<std::size_t>(i0)].id,
                                    doc.notes()[static_cast<std::size_t>(i1)].id}));
          }
          break;
        case 5:
        case 6:
        case 7:
          if (size > 0) {
            const int32_t id = doc.notes()[static_cast<std::size_t>(rnd(rng, 0, size - 1))].id;
            NotationNote repl = random_note(rng, 16);
            if (chance(rng, 40)) {
              // End-only edit: keep the start, change the tail.
              repl = *doc.find_note(id);
              repl.end_tick = repl.start_tick + rnd(rng, 0, 5) * 120;
            }
            CHECK(doc.update_note(id, repl));
          }
          break;
        case 8:
          if (chance(rng, 50)) {
            CHECK(doc.set_timing(make_timing(rng, chance(rng, 50))));
          } else {
            CHECK(doc.set_offset_ms(rnd(rng, -200, 900)));
          }
          break;
        default:
          CHECK(doc.set_notes(random_notes(rng, rnd(rng, 0, 40), 12)));
          break;
      }
      check_end_tree(doc, rng);
      check_lookups(doc, rng, 4);
    }
  }
}

void test_lookups_vs_brute_force_dense_multi_tempo() {
  for (int seed = 1; seed <= 20; ++seed) {
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed * 7919));
    ChartDocument doc(make_timing(rng, seed % 3 != 0));
    init_doc(doc, random_notes(rng, 30 + seed * 6, 6 + seed));
    // Notes at the exact same tick must come back in (start_tick, id) order.
    check_lookups(doc, rng, 120);
  }
}

// ---------------------------------------------------------------------------
// Oracles: verbatim copies of the pre-refactor linear scans
// ---------------------------------------------------------------------------

bool o_is_visible_star(NoteType type) {
  return type == NoteType::Sound || type == NoteType::ScratchSound;
}

bool o_is_legacy_scratch_hold_head(const NotationNote& note) {
  if (note.end_tick > note.start_tick) return false;
  switch (note.note_type) {
    case NoteType::Normal:
    case NoteType::Critical:
    case NoteType::BlueTap:
      return true;
    default:
      return false;
  }
}

NoteType o_migrate_legacy_head_type(const NotationNote& head, const NotationNote& body) {
  if (head.note_type == NoteType::Critical || body.note_type == NoteType::ScratchCriticalHold ||
      body.note_type == NoteType::NontailScratchCriticalHold) {
    return NoteType::ScratchCriticalHoldStart;
  }
  return NoteType::ScratchHoldStart;
}

std::optional<NotationNote> old_legacy_scratch_head_for(const std::vector<NotationNote>& notes,
                                                        const NotationNote& hold) {
  if (!is_scratch_hold_body(hold.note_type) || !is_hold_with_tail(hold.note_type)) {
    return std::nullopt;
  }
  for (const auto& note : notes) {
    if (note.id == hold.id || !o_is_legacy_scratch_hold_head(note)) continue;
    if (note.start_tick != hold.start_tick || note.lane != hold.lane ||
        note.width != hold.width) {
      continue;
    }
    return note;
  }
  return std::nullopt;
}

std::optional<NotationNote> old_paired_hold_head_for(const std::vector<NotationNote>& notes,
                                                     const NotationNote& hold) {
  if (!is_hold_with_tail(hold.note_type) && !is_nontail_hold_body(hold.note_type)) {
    return std::nullopt;
  }
  for (const auto& note : notes) {
    if (note.id == hold.id) continue;
    if (hold_head_pairs_with_body(note, hold)) return note;
  }
  return std::nullopt;
}

std::optional<NotationNote> old_paired_hold_body_for(const std::vector<NotationNote>& notes,
                                                     const NotationNote& head) {
  if (!is_hold_head_note(head)) return std::nullopt;
  for (const auto& note : notes) {
    if (hold_head_pairs_with_body(head, note)) return note;
  }
  return std::nullopt;
}

std::optional<NotationNote> old_chained_next(const std::vector<NotationNote>& notes,
                                             const NotationNote& body) {
  if (!is_hold_chain_body(body.note_type) || body.end_tick <= body.start_tick) {
    return std::nullopt;
  }
  for (const auto& note : notes) {
    if (note.id == body.id || !is_hold_chain_body(note.note_type)) continue;
    if (!same_hold_chain_family(body.note_type, note.note_type)) continue;
    if (note.start_tick != body.end_tick) continue;
    if (old_paired_hold_head_for(notes, note)) continue;
    if (!hold_chain_lanes_connected(body, note)) continue;
    return note;
  }
  return std::nullopt;
}

std::optional<NotationNote> old_chained_prev(const std::vector<NotationNote>& notes,
                                             const NotationNote& body) {
  if (!is_hold_chain_body(body.note_type) || body.end_tick <= body.start_tick) {
    return std::nullopt;
  }
  if (old_paired_hold_head_for(notes, body)) return std::nullopt;
  for (const auto& note : notes) {
    if (note.id == body.id || !is_hold_chain_body(note.note_type)) continue;
    if (!same_hold_chain_family(body.note_type, note.note_type)) continue;
    if (note.end_tick != body.start_tick) continue;
    if (!hold_chain_lanes_connected(note, body)) continue;
    return note;
  }
  return std::nullopt;
}

int old_repair_legacy_hold_heads(ChartDocument& doc) {
  int repaired = 0;
  std::vector<NotationNote> bodies;
  for (const auto& note : doc.notes()) {
    if (is_scratch_hold_body(note.note_type) && is_hold_with_tail(note.note_type)) {
      bodies.push_back(note);
    }
  }
  for (const auto& body : bodies) {
    auto head = old_legacy_scratch_head_for(doc.notes(), body);
    if (!head) continue;
    NotationNote fixed = *head;
    fixed.note_type = o_migrate_legacy_head_type(*head, body);
    if (doc.update_note(fixed.id, fixed)) ++repaired;
  }
  return repaired;
}

bool o_star_matches_hold_for_legacy_bind(const NotationNote& star, const NotationNote& hold) {
  // Relaxed binding: geometry only, Sound / ScratchSound may attach to either
  // hold family. Keep this oracle in sync with infer_legacy_star_hold_binds.
  if (!is_bindable_hold_body(hold.note_type)) return false;
  if (hold.end_tick <= hold.start_tick) return false;
  if (star.start_tick <= hold.start_tick || star.start_tick >= hold.end_tick) return false;
  return star.lane == hold.lane && star.width == hold.width;
}

void old_infer_legacy_star_hold_binds(std::vector<NotationNote>& notes) {
  std::unordered_map<int32_t, const NotationNote*> by_id;
  by_id.reserve(notes.size());
  for (const auto& note : notes) {
    if (note.id >= 0) by_id[note.id] = &note;
  }
  for (auto& star : notes) {
    if (!o_is_visible_star(star.note_type)) continue;
    if (star.parent_hold_id >= 0) {
      const auto it = by_id.find(star.parent_hold_id);
      if (it == by_id.end() || !is_bindable_hold_body(it->second->note_type)) {
        star.parent_hold_id = kNoBoundHoldId;
      }
    }
    if (star.parent_hold_id >= 0) continue;
    for (const auto& hold : notes) {
      if (hold.id < 0 || !o_star_matches_hold_for_legacy_bind(star, hold)) continue;
      star.parent_hold_id = hold.id;
      break;
    }
  }
}

bool old_prune_predicate(const NotationNote& note, const NotationNote& hold) {
  if (!o_is_visible_star(note.note_type)) return false;
  if (note.parent_hold_id != hold.id) return false;
  const bool same_lanes = note.lane == hold.lane && note.width == hold.width;
  if (!same_lanes) return true;
  return note.start_tick <= hold.start_tick || note.start_tick >= hold.end_tick;
}

// ---------------------------------------------------------------------------
// Differential tests
// ---------------------------------------------------------------------------

void test_lookup_ops_match_old_scans() {
  int paired_head_hits = 0;
  int paired_body_hits = 0;
  int chained_next_hits = 0;
  int chained_prev_hits = 0;
  int auto_head_hits = 0;
  int auto_head_nullopt = 0;
  int star_hits = 0;
  for (int seed = 1; seed <= 40; ++seed) {
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed * 31337));
    ChartDocument doc(make_timing(rng, seed % 3 != 0));
    init_doc(doc, random_notes(rng, 25 + seed * 4, 4 + seed / 2));
    const std::vector<NotationNote>& all = doc.notes();

    for (const auto& n : all) {
      const auto head = paired_hold_head_for(doc, n);
      CHECK(same_opt(head, old_paired_hold_head_for(all, n)));
      if (head) ++paired_head_hits;

      const auto body = paired_hold_body_for(doc, n);
      CHECK(same_opt(body, old_paired_hold_body_for(all, n)));
      if (body) ++paired_body_hits;

      const auto next = chained_next_scratch_hold(doc, n);
      CHECK(same_opt(next, old_chained_next(all, n)));
      if (next) ++chained_next_hits;

      const auto prev = chained_prev_scratch_hold(doc, n);
      CHECK(same_opt(prev, old_chained_prev(all, n)));
      if (prev) ++chained_prev_hits;

      // make_auto_hold_head: the vector overload is the unchanged full scan.
      if (is_bindable_hold_body(n.note_type)) {
        const auto auto_head = make_auto_hold_head(doc, n);
        CHECK(same_opt(auto_head, make_auto_hold_head(n, all)));
        if (auto_head) {
          ++auto_head_hits;
        } else {
          ++auto_head_nullopt;
        }
      }
    }

    // Drafts: unassigned id, arbitrary start / lanes (may sit on empty ticks).
    for (int d = 0; d < 25; ++d) {
      NotationNote draft = random_note(rng, 4 + seed / 2 + 2);
      draft.note_type = chance(rng, 50) ? NoteType::Hold : NoteType::ScratchCriticalHold;
      draft.end_tick = draft.start_tick + 240;
      draft.id = kAutoNoteId;
      CHECK(same_opt(make_auto_hold_head(doc, draft), make_auto_hold_head(draft, all)));
      CHECK(same_opt(paired_hold_head_for(doc, draft), old_paired_hold_head_for(all, draft)));
      CHECK(same_opt(chained_next_scratch_hold(doc, draft), old_chained_next(all, draft)));
      CHECK(same_opt(chained_prev_scratch_hold(doc, draft), old_chained_prev(all, draft)));
    }

    // hold_has_visible_star_at(doc) vs the (unchanged) vector overload.
    for (int q = 0; q < 60; ++q) {
      const int32_t hold_id = rnd(rng, -1, static_cast<int>(all.size()) - 1);
      const int32_t tick = rnd(rng, 0, 4 + seed / 2) * 120;
      const int32_t except = chance(rng, 30) ? rnd(rng, 0, static_cast<int>(all.size()) - 1) : -2;
      const bool want = hold_has_visible_star_at(all, hold_id, tick, except);
      CHECK_EQ(hold_has_visible_star_at(doc, hold_id, tick, except), want);
      if (want) ++star_hits;
    }
  }
  // The generator must actually exercise the interesting branches.
  CHECK(paired_head_hits > 0);
  CHECK(paired_body_hits > 0);
  CHECK(chained_next_hits > 0);
  CHECK(chained_prev_hits > 0);
  CHECK(auto_head_hits > 0);
  CHECK(auto_head_nullopt > 0);
  CHECK(star_hits > 0);
}

void test_repair_legacy_hold_heads_matches_old() {
  int total_repaired = 0;
  for (int seed = 1; seed <= 40; ++seed) {
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed * 2953));
    const MusicTiming timing = make_timing(rng, seed % 2 == 0);
    const std::vector<NotationNote> notes = random_notes(rng, 30 + seed * 3, 5 + seed / 3);
    ChartDocument a(timing);
    ChartDocument b(timing);
    init_doc(a, notes);
    init_doc(b, notes);
    const int got = repair_legacy_hold_heads(a);
    const int want = old_repair_legacy_hold_heads(b);
    CHECK_EQ(got, want);
    CHECK(same_note_list(a.notes(), b.notes()));
    total_repaired += got;
    // The index must still be consistent after the repairs.
    check_end_tree(a, rng);
    check_lookups(a, rng, 20);
  }
  CHECK(total_repaired > 0);
}

void test_infer_legacy_star_binds_matches_old() {
  int newly_bound = 0;
  for (int seed = 1; seed <= 60; ++seed) {
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed * 6151));
    std::vector<NotationNote> notes = random_notes(rng, 20 + seed * 5, 4 + seed / 4);
    // Garbage / dangling parents on some stars, unassigned ids on some notes.
    for (auto& n : notes) {
      if (!o_is_visible_star(n.note_type)) continue;
      const int roll = rnd(rng, 0, 9);
      if (roll == 0) n.parent_hold_id = 99999;
      if (roll == 1) n.parent_hold_id = kNoBoundHoldId;
      if (roll == 2) n.parent_hold_id = 0;  // may be a non-hold
    }
    // Shuffle so vector order differs from (start_tick, id) order.
    std::shuffle(notes.begin(), notes.end(), rng);
    for (auto& n : notes) {
      if (chance(rng, 4)) n.id = kAutoNoteId;
    }

    std::vector<NotationNote> got = notes;
    std::vector<NotationNote> want = notes;
    infer_legacy_star_hold_binds(got);
    old_infer_legacy_star_hold_binds(want);
    CHECK(same_note_list(got, want));
    for (std::size_t i = 0; i < notes.size(); ++i) {
      if (notes[i].parent_hold_id < 0 && got[i].parent_hold_id >= 0) ++newly_bound;
    }
  }
  CHECK(newly_bound > 0);
}

void test_prune_hold_mid_stars_matches_old() {
  int changed_runs = 0;
  for (int seed = 1; seed <= 30; ++seed) {
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed * 4001));
    const MusicTiming timing = make_timing(rng, false);
    const std::vector<NotationNote> notes = random_notes(rng, 40 + seed * 3, 6 + seed / 3);
    ChartDocument a(timing);
    ChartDocument b(timing);
    init_doc(a, notes);
    init_doc(b, notes);
    for (int round = 0; round < 8; ++round) {
      const auto& cur = a.notes();
      const NotationNote src = cur[static_cast<std::size_t>(rnd(rng, 0, static_cast<int>(cur.size()) - 1))];
      if (!is_bindable_hold_body(src.note_type)) continue;
      NotationNote hold = src;
      if (chance(rng, 50)) hold.end_tick = hold.start_tick + 120;  // shrink: prunes stars
      if (chance(rng, 20)) hold.lane = (hold.lane + 1) % (12 - hold.width + 1);

      std::vector<NotationNote> expected = b.notes();
      const auto end = std::remove_if(expected.begin(), expected.end(),
                                      [&](const NotationNote& note) {
                                        return old_prune_predicate(note, hold);
                                      });
      const bool expect_changed = end != expected.end();
      if (expect_changed) {
        expected.erase(end, expected.end());
        CHECK(b.set_notes(std::move(expected)));
        ++changed_runs;
      }
      CHECK_EQ(prune_hold_mid_stars(a, hold), expect_changed);
      CHECK(same_note_list(a.notes(), b.notes()));
    }
  }
  CHECK(changed_runs > 0);
}

// Verbatim copies of the pre-refactor scans.
bool o_overlaps(const NotationNote& a, const NotationNote& b) {
  return a.lane <= b.end_lane() && b.lane <= a.end_lane();
}

std::vector<NotationNote> old_hold_eighths_for(const ChartDocument& doc,
                                               const NotationNote& hold) {
  std::vector<NotationNote> out;
  if (!is_bindable_hold_body(hold.note_type) || hold.end_tick <= hold.start_tick) return out;
  for (const auto& note : doc.notes()) {
    if (note.note_type != NoteType::HoldEighth) continue;
    if (hold.id >= 0 && note.parent_hold_id == hold.id) {
      out.push_back(note);
      continue;
    }
    if (note.parent_hold_id >= 0) continue;
    if (note.start_tick > hold.start_tick && note.start_tick < hold.end_tick &&
        o_overlaps(note, hold)) {
      out.push_back(note);
    }
  }
  return out;
}

std::vector<NotationNote> old_hold_attached_notes_for(const ChartDocument& doc,
                                                      const NotationNote& hold) {
  std::vector<NotationNote> out;
  if (!is_bindable_hold_body(hold.note_type) || hold.end_tick <= hold.start_tick) return out;
  for (const auto& note : doc.notes()) {
    if (hold.id < 0 || note.parent_hold_id != hold.id) continue;
    if (note.note_type == NoteType::HoldEighth || o_is_visible_star(note.note_type)) {
      out.push_back(note);
    }
  }
  return out;
}

// Moves/shortens holds and re-parents/deletes things so bound notes end up outside
// their hold's range, unbound eighths appear, and some parents are deleted (orphans).
void scramble_parents(ChartDocument& doc, std::mt19937& rng) {
  for (int k = 0; k < 6; ++k) {
    const auto& cur = doc.notes();
    if (cur.empty()) return;
    NotationNote n = cur[static_cast<std::size_t>(rnd(rng, 0, static_cast<int>(cur.size()) - 1))];
    const int32_t id = n.id;
    switch (rnd(rng, 0, 3)) {
      case 0:  // move a hold away from its children
        if (!is_hold_with_tail(n.note_type)) continue;
        n.start_tick += 1200;
        n.end_tick += 1200;
        break;
      case 1:  // shorten
        if (!is_hold_with_tail(n.note_type)) continue;
        n.end_tick = n.start_tick + 120;
        break;
      case 2:  // re-parent / unbind a bound-capable note
        if (!o_is_visible_star(n.note_type) && n.note_type != NoteType::HoldEighth) continue;
        n.parent_hold_id = chance(rng, 40) ? kNoBoundHoldId
                                           : cur[static_cast<std::size_t>(rnd(rng, 0, static_cast<int>(cur.size()) - 1))].id;
        break;
      default:  // delete (may orphan children)
        CHECK(doc.remove_note(id));
        check_parent_map(doc);
        continue;
    }
    CHECK(doc.update_note(id, n));
    check_parent_map(doc);
  }
}

void test_parent_map_mutators() {
  std::mt19937 rng(5);
  ChartDocument doc(make_timing(rng, false));
  CHECK(doc.set_compatibility_profile(ChartCompatibilityProfile::SiriusExtended));
  check_parent_map(doc);
  const int32_t hold = doc.add_note(make_note(NoteType::Hold, 480, 1920, 2, 2));
  check_parent_map(doc);
  NotationNote star = make_note(NoteType::Sound, 960, 960, 2, 2);
  star.parent_hold_id = hold;
  const int32_t s1 = doc.add_note(star);
  check_parent_map(doc);
  std::vector<std::size_t> idx;
  doc.note_indices_with_parent(hold, idx);
  CHECK_EQ(idx.size(), static_cast<std::size_t>(1));
  // add_notes with children added before the parent in sort order.
  NotationNote eighth = make_note(NoteType::HoldEighth, 120, 120, 2, 2);
  eighth.parent_hold_id = hold;
  NotationNote star2 = star;
  star2.start_tick = 1200;
  CHECK(doc.add_notes({eighth, star2}));
  check_parent_map(doc);
  doc.note_indices_with_parent(hold, idx);
  CHECK_EQ(idx.size(), static_cast<std::size_t>(3));
  // parent change with unchanged id
  NotationNote moved = *doc.find_note(s1);
  moved.parent_hold_id = kNoBoundHoldId;
  CHECK(doc.update_note(s1, moved));
  check_parent_map(doc);
  doc.note_indices_with_parent(hold, idx);
  CHECK_EQ(idx.size(), static_cast<std::size_t>(2));
  // remove child / remove parent (orphans keep their stale parent id)
  CHECK(doc.remove_note(s1));
  check_parent_map(doc);
  CHECK(doc.remove_note(hold));
  check_parent_map(doc);
  doc.note_indices_with_parent(hold, idx);
  CHECK_EQ(idx.size(), static_cast<std::size_t>(2));
  CHECK(doc.remove_notes({doc.notes()[0].id}));
  check_parent_map(doc);
  // set_notes / load_from_chart / normalize (renumbering)
  CHECK(doc.set_notes(random_notes(rng, 60, 10)));
  check_parent_map(doc);
  for (int i = 0; i < 4; ++i) {
    scramble_parents(doc, rng);
    check_parent_map(doc);
  }
  {
    NotationChart chart = doc.to_notation_chart();
    for (auto& n : chart.notes) n.id = chance(rng, 50) ? kAutoNoteId : n.id;  // forces renumber path
    doc.load_from_chart(chart);
    check_parent_map(doc);
  }
  CHECK(doc.normalize_for_save());
  check_parent_map(doc);
  // read-only doc rejects mutations and leaves the map intact
  doc.set_edit_mode(ChartEditMode::OfficialPreviewOnly);
  CHECK(!doc.remove_note(doc.notes()[0].id));
  check_parent_map(doc);
}

void test_parent_map_randomized_sequence() {
  for (int seed = 1; seed <= 6; ++seed) {
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed * 15485863));
    ChartDocument doc(make_timing(rng, seed % 2 == 0));
    init_doc(doc, random_notes(rng, 40, 10));
    for (int step = 0; step < 200; ++step) {
      const int size = static_cast<int>(doc.notes().size());
      const int op = size > 90 ? 3 : rnd(rng, 0, 7);
      switch (op) {
        case 0: {
          NotationNote n = random_note(rng, 14);
          if (size > 0 && chance(rng, 60)) {
            n.parent_hold_id = doc.notes()[static_cast<std::size_t>(rnd(rng, 0, size - 1))].id;
          }
          CHECK(doc.add_note(n) >= 0);
          break;
        }
        case 1: {
          std::vector<NotationNote> batch;
          for (int k = 0; k < 3; ++k) {
            NotationNote n = random_note(rng, 14);
            if (size > 0 && chance(rng, 60)) {
              n.parent_hold_id = doc.notes()[static_cast<std::size_t>(rnd(rng, 0, size - 1))].id;
            }
            batch.push_back(n);
          }
          CHECK(doc.add_notes(batch));
          break;
        }
        case 2:
        case 3:
          if (size > 0) {
            CHECK(doc.remove_note(doc.notes()[static_cast<std::size_t>(rnd(rng, 0, size - 1))].id));
          }
          break;
        case 4:
        case 5:
          if (size > 1) {
            // batch update including parent changes (and start moves that reorder indices)
            const int i0 = rnd(rng, 0, size - 1);
            int i1 = rnd(rng, 0, size - 1);
            if (i1 == i0) i1 = (i1 + 1) % size;
            NotationNote a = doc.notes()[static_cast<std::size_t>(i0)];
            NotationNote b = doc.notes()[static_cast<std::size_t>(i1)];
            a.parent_hold_id = chance(rng, 50) ? kNoBoundHoldId
                                               : doc.notes()[static_cast<std::size_t>(rnd(rng, 0, size - 1))].id;
            b.start_tick = rnd(rng, 0, 13) * 120;
            if (b.end_tick < b.start_tick) b.end_tick = b.start_tick;
            b.parent_hold_id = chance(rng, 50) ? kNoBoundHoldId : a.id;
            const int32_t ida = a.id;
            const int32_t idb = b.id;
            CHECK(doc.apply_note_updates({NoteUpdate{ida, a}, NoteUpdate{idb, b}}));
          }
          break;
        case 6:
          CHECK(doc.set_notes(random_notes(rng, rnd(rng, 0, 40), 10)));
          break;
        default:
          if (chance(rng, 50)) {
            doc.load_from_chart(doc.to_notation_chart());
          } else {
            CHECK(doc.normalize_for_save());
          }
          break;
      }
      check_parent_map(doc);
    }
  }
}

void test_hold_eighth_and_attached_lookups_match_old() {
  int bound_hits = 0;
  int unbound_hits = 0;
  int out_of_range_bound = 0;
  for (int seed = 1; seed <= 40; ++seed) {
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed * 86243));
    ChartDocument doc(make_timing(rng, seed % 3 != 0));
    init_doc(doc, random_notes(rng, 40 + seed * 4, 6 + seed / 2));
    for (int phase = 0; phase < 3; ++phase) {
      if (phase > 0) scramble_parents(doc, rng);
      const std::vector<NotationNote> snapshot = doc.notes();
      for (const auto& n : snapshot) {
        const auto got_e = hold_eighths_for(doc, n);
        CHECK(same_note_list(got_e, old_hold_eighths_for(doc, n)));
        const auto got_a = hold_attached_notes_for(doc, n);
        CHECK(same_note_list(got_a, old_hold_attached_notes_for(doc, n)));
        for (const auto& e : got_e) {
          if (e.parent_hold_id >= 0) {
            ++bound_hits;
            if (e.start_tick <= n.start_tick || e.start_tick >= n.end_tick) ++out_of_range_bound;
          } else {
            ++unbound_hits;
          }
        }
      }
      // Drafts: unassigned id (matches only unbound eighths), arbitrary range.
      for (int d = 0; d < 10; ++d) {
        NotationNote draft = random_note(rng, 8);
        draft.note_type = NoteType::Hold;
        draft.end_tick = draft.start_tick + rnd(rng, 0, 8) * 120;
        draft.id = chance(rng, 50) ? kAutoNoteId : -7;
        CHECK(same_note_list(hold_eighths_for(doc, draft), old_hold_eighths_for(doc, draft)));
        CHECK(same_note_list(hold_attached_notes_for(doc, draft),
                             old_hold_attached_notes_for(doc, draft)));
      }
    }
  }
  CHECK(bound_hits > 0);
  CHECK(unbound_hits > 0);
  CHECK(out_of_range_bound > 0);
}

void test_prune_hold_mid_stars_after_scramble_matches_old() {
  int changed_runs = 0;
  for (int seed = 1; seed <= 30; ++seed) {
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed * 92821));
    const MusicTiming timing = make_timing(rng, false);
    const std::vector<NotationNote> notes = random_notes(rng, 50 + seed * 2, 6 + seed / 3);
    ChartDocument a(timing);
    ChartDocument b(timing);
    init_doc(a, notes);
    init_doc(b, notes);
    for (int round = 0; round < 8; ++round) {
      scramble_parents(a, rng);
      CHECK(b.set_notes(a.notes()));
      const auto& cur = a.notes();
      if (cur.empty()) break;
      NotationNote hold = cur[static_cast<std::size_t>(rnd(rng, 0, static_cast<int>(cur.size()) - 1))];
      if (!is_bindable_hold_body(hold.note_type)) continue;
      if (chance(rng, 15)) hold.id = kAutoNoteId;  // negative id: unbound stars match
      std::vector<NotationNote> expected = b.notes();
      const auto end = std::remove_if(expected.begin(), expected.end(),
                                      [&](const NotationNote& note) {
                                        return old_prune_predicate(note, hold);
                                      });
      const bool expect_changed = end != expected.end();
      expected.erase(end, expected.end());
      CHECK_EQ(prune_hold_mid_stars(a, hold), expect_changed);
      if (expect_changed) {
        ++changed_runs;
        CHECK(b.set_notes(std::move(expected)));
      }
      CHECK(same_note_list(a.notes(), b.notes()));
      check_parent_map(a);
    }
  }
  CHECK(changed_runs > 0);
}

void test_scratch_curve_commit_document_matches_scan() {
  int with_head = 0;
  int with_prev = 0;
  for (int seed = 1; seed <= 30; ++seed) {
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed * 9973));
    ChartDocument doc(make_timing(rng, seed % 2 == 0));
    init_doc(doc, random_notes(rng, 30 + seed * 3, 5 + seed / 3));
    const std::vector<NotationNote>& all = doc.notes();

    for (int trial = 0; trial < 12; ++trial) {
      ScratchHoldCurveCommitInput input;
      input.notes = all;

      std::optional<NotationNote> prev;
      if (chance(rng, 60)) {
        std::vector<std::size_t> chain;
        for (std::size_t i = 0; i < all.size(); ++i) {
          if (is_hold_chain_body(all[i].note_type) && all[i].end_tick > all[i].start_tick) {
            chain.push_back(i);
          }
        }
        if (!chain.empty()) {
          prev = all[chain[static_cast<std::size_t>(rnd(rng, 0, static_cast<int>(chain.size()) - 1))]];
        }
      }
      const int32_t start = prev ? prev->end_tick : rnd(rng, 0, 4 + seed / 3) * 120;
      const int32_t width = rnd(rng, 1, 4);
      NotationNote body1 = make_note(chance(rng, 50) ? NoteType::ScratchHold
                                                     : NoteType::ScratchCriticalHold,
                                     start, start + rnd(rng, 1, 3) * 120, rnd(rng, 0, 12 - width),
                                     width);
      input.generated_bodies.push_back(body1);
      if (chance(rng, 50)) {
        NotationNote body2 = body1;
        body2.start_tick = body1.end_tick;
        body2.end_tick = body1.end_tick + 120;
        body2.lane = rnd(rng, 0, 12 - width);
        input.generated_bodies.push_back(body2);
      }
      if (prev) {
        input.previous_body_id = prev->id;
        input.previous_body_original = *prev;
        ++with_prev;
      }
      input.include_head = chance(rng, 85);
      input.ticks_per_quarter = doc.timing().ticks_per_quarter;
      input.next_id = doc.next_note_id();

      const auto scan = build_scratch_hold_curve_commit(input);
      input.document = &doc;
      const auto indexed = build_scratch_hold_curve_commit(input);
      CHECK_EQ(scan.has_value(), indexed.has_value());
      if (scan && indexed) {
        CHECK(same_note_list(scan->before, indexed->before));
        CHECK(same_note_list(scan->after, indexed->after));
        // A newly created head has an id >= next_id and sits at the first body's start.
        for (const auto& note : scan->after) {
          if (note.id >= input.next_id && is_hold_head_note(note)) {
            ++with_head;
            break;
          }
        }
      }
    }
  }
  CHECK(with_head > 0);
  CHECK(with_prev > 0);
}

}  // namespace

int main() {
  test_end_tree_scenarios();
  test_end_tree_and_lookups_randomized_sequence();
  test_lookups_vs_brute_force_dense_multi_tempo();
  test_lookup_ops_match_old_scans();
  test_repair_legacy_hold_heads_matches_old();
  test_infer_legacy_star_binds_matches_old();
  test_prune_hold_mid_stars_matches_old();
  test_scratch_curve_commit_document_matches_scan();
  test_parent_map_mutators();
  test_parent_map_randomized_sequence();
  test_hold_eighth_and_attached_lookups_match_old();
  test_prune_hold_mid_stars_after_scramble_matches_old();

  const int failures = wds::chart_editor::test::failure_count();
  if (failures == 0) {
    std::printf("All note_index_lookup tests passed.\n");
    return 0;
  }
  std::printf("%d note_index_lookup test(s) failed.\n", failures);
  return 1;
}
