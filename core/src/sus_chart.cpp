#include <wds/core/sus_chart.hpp>

#include <wds/core/file_io.hpp>
#include <wds/core/gimmick.hpp>
#include <wds/core/note_edit_ops.hpp>
#include <wds/core/timing_map.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>
#include <sstream>
#include <utility>
#include <vector>

namespace wds::chart_editor {
namespace {

std::string trim_copy(std::string s) {
  auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

std::string to_lower_ascii(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

bool ends_with_ci(const std::string& value, const std::string& suffix) {
  if (value.size() < suffix.size()) return false;
  return to_lower_ascii(value.substr(value.size() - suffix.size())) == to_lower_ascii(suffix);
}

int base36_value(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'z') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'Z') return 10 + (ch - 'A');
  return -1;
}

char base36_digit(int value) {
  value = std::clamp(value, 0, 35);
  if (value < 10) return static_cast<char>('0' + value);
  return static_cast<char>('a' + (value - 10));
}

bool parse_quoted(const std::string& raw, std::string& out) {
  const std::string t = trim_copy(raw);
  if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
    out = t.substr(1, t.size() - 2);
    return true;
  }
  out = t;
  return !out.empty() || t == "\"\"";
}

bool parse_double(const std::string& text, double& out) {
  try {
    size_t idx = 0;
    out = std::stod(trim_copy(text), &idx);
    return idx > 0;
  } catch (...) {
    return false;
  }
}

bool parse_int(const std::string& text, int& out) {
  try {
    size_t idx = 0;
    out = std::stoi(trim_copy(text), &idx);
    return idx > 0;
  } catch (...) {
    return false;
  }
}

std::string read_file_text(const std::string& path, SerializeResult& status) {
  return read_text_file(path, status);
}

SerializeResult write_file_text(const std::string& path, const std::string& text) {
  return write_text_atomic(path, text);
}

struct RawEvent {
  int64_t tick = 0;
  int lane = 0;
  int width = 1;
  int type = 0;       // first digit of note pair
  int channel = 0;    // hold/slide channel
  int category = 0;   // 1 tap, 2 hold, 3/4 slide, 5 directional
};

struct HoldKey {
  int category = 0;
  int channel = 0;
  int lane = -1;  // only for category 2
  bool operator<(const HoldKey& o) const {
    if (category != o.category) return category < o.category;
    if (channel != o.channel) return channel < o.channel;
    return lane < o.lane;
  }
};

// SUS #mmm1x first digit (spec reserves 1..6). Ched: 1 tap, 2 ExTap, 3 flick, 4 damage.
constexpr int kSusTapNormal = 1;
constexpr int kSusTapCritical = 2;
constexpr int kSusTapFlick = 3;
constexpr int kSusTapDamage = 4;

NoteType tap_type_from_sus(int type) {
  switch (type) {
    case kSusTapCritical:
      return NoteType::Critical;
    case kSusTapFlick:
      return NoteType::Flick;
    default:
      return NoteType::Normal;
  }
}

bool same_tick_f(float a, float b) { return std::abs(a - b) < 0.5f; }

// Start lanes fully occupied by non-hold-body notes (or other hold tails ending
// here). Other hold bodies that merely start here are ignored — same occupancy
// model as make_auto_hold_head.
bool hold_start_fully_covered(const NotationNote& hold,
                              const std::vector<NotationNote>& notes) {
  if (hold.width < 1) return true;
  auto mark_range = [&](std::vector<char>& occupied, int32_t lo, int32_t hi) {
    lo = std::max(lo, hold.lane);
    hi = std::min(hi, hold.end_lane());
    for (int32_t lane = lo; lane <= hi; ++lane) {
      occupied[static_cast<size_t>(lane - hold.lane)] = 1;
    }
  };
  std::vector<char> occupied(static_cast<size_t>(hold.width), 0);
  for (const auto& note : notes) {
    if (note.id == hold.id) continue;
    if (is_hold_with_tail(note.note_type)) {
      if (!same_tick_f(note.end_tick, hold.start_tick)) continue;
      if (is_scratch_hold_body(note.note_type)) {
        const auto [tail_lo, tail_hi] = get_scratch_end_lane_range(note);
        mark_range(occupied, tail_lo, tail_hi);
      } else {
        mark_range(occupied, note.lane, note.end_lane());
      }
      continue;
    }
    if (!same_tick_f(note.start_tick, hold.start_tick)) continue;
    mark_range(occupied, note.lane, note.end_lane());
  }
  return std::all_of(occupied.begin(), occupied.end(), [](char c) { return c != 0; });
}

struct SusDamageMarker {
  float tick = 0.0f;
  int32_t lane = 0;
  int32_t width = 1;
};

bool matches_damage_marker(const SusDamageMarker& dmg, const NotationNote& hold) {
  return same_tick_f(dmg.tick, hold.start_tick) && dmg.lane == hold.lane &&
         dmg.width == hold.width;
}

// Critical tap that fully covers the hold start span (金头 marker on export).
bool hold_start_fully_covered_by_critical(const NotationNote& hold,
                                          const std::vector<NotationNote>& notes) {
  if (hold.width < 1) return false;
  std::vector<char> occupied(static_cast<size_t>(hold.width), 0);
  for (const auto& note : notes) {
    if (note.note_type != NoteType::Critical) continue;
    if (!same_tick_f(note.start_tick, hold.start_tick)) continue;
    const int32_t lo = std::max(note.lane, hold.lane);
    const int32_t hi = std::min(note.end_lane(), hold.end_lane());
    for (int32_t lane = lo; lane <= hi; ++lane) {
      occupied[static_cast<size_t>(lane - hold.lane)] = 1;
    }
  }
  return std::all_of(occupied.begin(), occupied.end(), [](char c) { return c != 0; });
}

int32_t flick_scratch_from_directional(int type) {
  // 1 up, 2 down, 3 left-up, 4 right-up, 5 left-down, 6 right-down
  switch (type) {
    case 3:
    case 5:
      return -1;
    case 4:
    case 6:
      return 1;
    default:
      return 0;
  }
}

}  // namespace

bool SusChartFormat::looks_like_sus_path(const std::string& path) {
  return ends_with_ci(path, ".sus");
}

bool SusChartFormat::looks_like_sus_text(const std::string& text) {
  std::istringstream ss(text);
  std::string line;
  int scored = 0;
  while (std::getline(ss, line) && scored < 3) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::string t = trim_copy(line);
    if (t.empty() || t[0] != '#') continue;
    if (t.rfind("#TITLE", 0) == 0 || t.rfind("#WAVE", 0) == 0 || t.rfind("#REQUEST", 0) == 0 ||
        t.rfind("#BPM", 0) == 0) {
      ++scored;
      continue;
    }
    // Data header like #00012: or #00220a:
    if (t.size() >= 6 && t[0] == '#' && t.find(':') != std::string::npos) {
      bool digits = true;
      for (size_t i = 1; i <= 3 && i < t.size(); ++i) {
        if (t[i] < '0' || t[i] > '9') digits = false;
      }
      if (digits) ++scored;
    }
  }
  return scored >= 1;
}

SerializeResult SusChartFormat::parse(const std::string& text, SusChartLoadResult& out) {
  SusChartMetadata meta;
  meta.ticks_per_beat = 480;

  std::map<int, double> bpm_defs;  // ordered: fallback picks lowest id
  std::map<int, double> measure_lengths;  // measure -> beats (applies from that measure on)
  measure_lengths[0] = 4.0;

  struct BpmChange {
    int64_t tick;
    double bpm;
  };
  std::vector<BpmChange> bpm_changes;
  std::vector<RawEvent> events;

  int measure_base = 0;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::string trimmed = trim_copy(line);
    if (trimmed.empty() || trimmed[0] != '#') continue;

    std::string header;
    std::string data;
    // Data rows are `#mmm…:` with no space before ':'. Metadata is `#KEY value`
    // (and may contain ':' inside Windows paths like F:\...).
    const size_t colon = trimmed.find(':');
    const size_t first_space = trimmed.find_first_of(" \t", 1);
    const bool colon_is_data_sep =
        colon != std::string::npos && (first_space == std::string::npos || colon < first_space);
    if (colon_is_data_sep) {
      header = trim_copy(trimmed.substr(1, colon - 1));
      data = trim_copy(trimmed.substr(colon + 1));
    } else {
      const std::string body = trim_copy(trimmed.substr(1));
      const size_t sp = body.find_first_of(" \t");
      if (sp == std::string::npos) {
        header = body;
      } else {
        header = trim_copy(body.substr(0, sp));
        data = trim_copy(body.substr(sp + 1));
      }
    }
    const std::string header_l = to_lower_ascii(header);

    auto set_meta_str = [&](const char* key, std::string& field) {
      if (to_lower_ascii(header) == key) {
        parse_quoted(data, field);
        return true;
      }
      return false;
    };

    // WAVE must be matched exactly before other handlers; Windows paths contain ':'.
    if (header_l == "wave") {
      parse_quoted(data, meta.wave_path);
      continue;
    }

    if (set_meta_str("title", meta.title) || set_meta_str("subtitle", meta.subtitle) ||
        set_meta_str("artist", meta.artist) || set_meta_str("designer", meta.designer) ||
        set_meta_str("desinger", meta.designer) || set_meta_str("songid", meta.song_id) ||
        set_meta_str("jacket", meta.jacket_path)) {
      continue;
    }
    if (header_l == "difficulty") {
      parse_quoted(data, meta.difficulty);
      if (meta.difficulty.empty()) meta.difficulty = trim_copy(data);
      continue;
    }
    if (header_l == "playlevel") {
      meta.play_level = trim_copy(data);
      continue;
    }
    if (header_l == "waveoffset") {
      parse_double(data, meta.wave_offset_sec);
      continue;
    }
    if (header_l == "basebpm") {
      parse_double(data, meta.base_bpm);
      continue;
    }
    if (header_l == "request") {
      std::string req;
      parse_quoted(data, req);
      // ticks_per_beat N
      std::istringstream rs(req);
      std::string token;
      if (rs >> token && to_lower_ascii(token) == "ticks_per_beat") {
        int tpq = 480;
        if (rs >> tpq && tpq > 0) meta.ticks_per_beat = tpq;
      }
      continue;
    }
    if (header_l == "measurebs") {
      int base = 0;
      if (parse_int(data, base)) measure_base = base;
      continue;
    }
    if (header_l.rfind("bpm", 0) == 0 && header.size() >= 5) {
      const int id = base36_value(header[3]) * 36 + base36_value(header[4]);
      double bpm = 120.0;
      if (id >= 0 && parse_double(data, bpm) && bpm > 0.0) bpm_defs[id] = bpm;
      continue;
    }
    // Ignore TIL / HISPEED / ATTRIBUTE blocks for now.
    if (header_l.rfind("til", 0) == 0 || header_l == "hispeed" || header_l == "nospeed" ||
        header_l.rfind("atr", 0) == 0 || header_l == "attribute" || header_l == "noattribute" ||
        header_l == "noattirbute" || header_l == "measurehs" || header_l == "genre" ||
        header_l == "background" || header_l == "movie" || header_l == "movieoffset") {
      continue;
    }

    // Numeric measure data headers.
    if (header.size() < 5) continue;
    bool meas_ok = true;
    int measure = 0;
    for (int i = 0; i < 3; ++i) {
      if (header[i] < '0' || header[i] > '9') {
        meas_ok = false;
        break;
      }
      measure = measure * 10 + (header[i] - '0');
    }
    if (!meas_ok) continue;
    measure += measure_base;

    const std::string rest = header.substr(3);
    const int tpq = meta.ticks_per_beat;

    auto ticks_per_measure_at = [&](int meas) {
      double beats = 4.0;
      for (const auto& [m, len] : measure_lengths) {
        if (m > meas) break;
        beats = len;
      }
      return static_cast<int64_t>(std::llround(beats * tpq));
    };

    // Lazy absolute tick for measure start — computed after all measure lengths known;
    // store relative for now via deferred resolution. We'll resolve after the scan.
    // For streaming we need measure lengths first — two-pass.
    // Here just store raw with measure + index; resolve later.
    (void)ticks_per_measure_at;

    if (rest == "02") {
      double beats = 4.0;
      if (parse_double(data, beats) && beats > 0.0) measure_lengths[measure] = beats;
      continue;
    }
    if (rest == "08") {
      // BPM change: data is zz ids packed in pairs.
      const std::string cleaned = trim_copy(data);
      const int n = static_cast<int>(cleaned.size() / 2);
      if (n <= 0) continue;
      for (int i = 0; i < n; ++i) {
        const int a = base36_value(cleaned[static_cast<size_t>(i) * 2]);
        const int b = base36_value(cleaned[static_cast<size_t>(i) * 2 + 1]);
        if (a < 0 || b < 0) continue;
        const int id = a * 36 + b;
        const auto it = bpm_defs.find(id);
        if (it == bpm_defs.end()) continue;
        // tick filled in pass 2
        bpm_changes.push_back({(static_cast<int64_t>(measure) << 32) |
                                   static_cast<int64_t>((i << 16) | n),
                               it->second});
      }
      continue;
    }

    int category = 0;
    int lane = 0;
    int channel = 0;
    bool note_header = false;
    if (rest.size() == 2 && rest[0] == '1') {
      category = 1;
      lane = base36_value(rest[1]);
      note_header = lane >= 0;
    } else if (rest.size() == 2 && rest[0] == '5') {
      category = 5;
      lane = base36_value(rest[1]);
      note_header = lane >= 0;
    } else if (rest.size() == 3 && (rest[0] == '2' || rest[0] == '3' || rest[0] == '4')) {
      category = rest[0] - '0';
      lane = base36_value(rest[1]);
      channel = base36_value(rest[2]);
      note_header = lane >= 0 && channel >= 0;
    }
    if (!note_header) continue;

    const std::string cleaned = trim_copy(data);
    if (cleaned.size() < 2 || (cleaned.size() % 2) != 0) continue;
    const int slots = static_cast<int>(cleaned.size() / 2);
    for (int i = 0; i < slots; ++i) {
      const int type = base36_value(cleaned[static_cast<size_t>(i) * 2]);
      const int width = base36_value(cleaned[static_cast<size_t>(i) * 2 + 1]);
      if (type <= 0 || width <= 0) continue;
      RawEvent ev;
      // Encode measure/slot temporarily in tick; rewrite in pass 2.
      ev.tick = (static_cast<int64_t>(measure) << 32) | static_cast<int64_t>((i << 16) | slots);
      ev.lane = lane;
      ev.width = std::clamp(width, 1, 35);
      ev.type = type;
      ev.channel = channel;
      ev.category = category;
      events.push_back(ev);
    }
  }

  // Pass 2: resolve measure → absolute ticks.
  auto measure_start_tick = [&](int measure) -> int64_t {
    int64_t tick = 0;
    double beats = 4.0;
    auto it = measure_lengths.begin();
    for (int m = 0; m < measure; ++m) {
      while (it != measure_lengths.end() && it->first <= m) {
        beats = it->second;
        ++it;
      }
      // re-walk lengths properly
      beats = 4.0;
      for (const auto& [mm, len] : measure_lengths) {
        if (mm > m) break;
        beats = len;
      }
      tick += static_cast<int64_t>(std::llround(beats * meta.ticks_per_beat));
    }
    return tick;
  };

  auto decode_packed = [&](int64_t packed, int64_t& out_tick) {
    const int measure = static_cast<int>(packed >> 32);
    const int slots = static_cast<int>(packed & 0xffff);
    const int index = static_cast<int>((packed >> 16) & 0xffff);
    const int64_t mstart = measure_start_tick(measure);
    double beats = 4.0;
    for (const auto& [mm, len] : measure_lengths) {
      if (mm > measure) break;
      beats = len;
    }
    const int64_t mlen = static_cast<int64_t>(std::llround(beats * meta.ticks_per_beat));
    out_tick = mstart + (slots > 0 ? mlen * index / slots : mstart);
  };

  for (auto& ev : events) {
    int64_t tick = 0;
    decode_packed(ev.tick, tick);
    ev.tick = tick;
  }
  for (auto& ch : bpm_changes) {
    int64_t tick = 0;
    decode_packed(ch.tick, tick);
    ch.tick = tick;
  }

  NotationChart chart;
  chart.timing.ticks_per_quarter = meta.ticks_per_beat;
  chart.timing.offset_ms =
      static_cast<int64_t>(std::llround(meta.wave_offset_sec * 1000.0));

  // Timing points from BPM changes + #mmm02 measure lengths.
  std::sort(bpm_changes.begin(), bpm_changes.end(),
            [](const BpmChange& a, const BpmChange& b) { return a.tick < b.tick; });
  if (!bpm_changes.empty()) {
    chart.timing.bpm = bpm_changes.front().bpm;
  } else if (meta.base_bpm > 0.0) {
    chart.timing.bpm = meta.base_bpm;
  } else if (!bpm_defs.empty()) {
    chart.timing.bpm = bpm_defs.begin()->second;
  } else {
    chart.timing.bpm = 120.0;
  }
  chart.timing.points.clear();
  chart.timing.points.push_back(
      TimingPoint{0, chart.timing.bpm, /*num*/ 4, /*den*/ 4, true, true});
  for (const auto& ch : bpm_changes) {
    if (ch.tick <= 0) {
      chart.timing.points[0].bpm = ch.bpm;
      chart.timing.points[0].has_bpm = true;
      chart.timing.bpm = ch.bpm;
      continue;
    }
    TimingPoint point;
    point.tick = static_cast<int32_t>(ch.tick);
    point.bpm = ch.bpm;
    point.numerator = 4;
    point.denominator = 4;
    point.has_bpm = true;
    point.has_meter = false;
    chart.timing.points.push_back(point);
  }

  auto upsert_meter = [&](int32_t tick, int32_t numerator, int32_t denominator) {
    for (auto& p : chart.timing.points) {
      if (p.tick == tick) {
        p.numerator = numerator;
        p.denominator = denominator;
        p.has_meter = true;
        return;
      }
    }
    TimingPoint point;
    point.tick = tick;
    point.bpm = chart.timing.bpm;
    point.numerator = numerator;
    point.denominator = denominator;
    point.has_bpm = false;
    point.has_meter = true;
    chart.timing.points.push_back(point);
  };
  auto beats_to_meter = [](double beats) -> std::pair<int32_t, int32_t> {
    // Prefer exact M/2^n as N/D meter: measure ticks = num * (tpq*4/den) ≈ beats*tpq.
    for (const int32_t den : {4, 8, 16, 32}) {
      const double num_f = beats * static_cast<double>(den) / 4.0;
      const int32_t num = static_cast<int32_t>(std::llround(num_f));
      if (num < 1) continue;
      if (std::abs(num_f - static_cast<double>(num)) < 1e-6) return {num, den};
    }
    const int32_t num =
        std::max(1, static_cast<int32_t>(std::llround(beats * 32.0 / 4.0)));
    return {num, 32};
  };
  for (const auto& [measure, beats] : measure_lengths) {
    if (beats <= 0.0) continue;
    const int32_t tick = static_cast<int32_t>(measure_start_tick(measure));
    const auto [numerator, denominator] = beats_to_meter(beats);
    upsert_meter(tick, numerator, denominator);
  }
  normalize_timing_points(chart.timing);

  // Lane offset: WDS export with ched_lane_padding writes L → L+2 (Ched 12-key
  // window 2..d). Prefer offset 2 whenever data fits that window so right-side
  // notes (e.g. WDS lane 7 → SUS 9) do not import as lane 9 with offset 0.
  int min_lane = 99;
  int max_end = 0;
  for (const auto& ev : events) {
    min_lane = std::min(min_lane, ev.lane);
    max_end = std::max(max_end, ev.lane + ev.width);
  }
  int lane_offset = 0;
  if (min_lane < 99 && min_lane >= 2 && max_end <= 14) {
    lane_offset = 2;
  } else if (min_lane < 99 && max_end > 12 && min_lane >= 2) {
    lane_offset = min_lane;
  } else if (min_lane < 99 && max_end > 12) {
    lane_offset = std::max(0, max_end - 12);
  }

  auto map_lane = [&](int lane) {
    return std::clamp(lane - lane_offset, 0, 11);
  };
  auto map_width = [&](int lane, int width) {
    const int left = map_lane(lane);
    return std::clamp(width, 1, 12 - left);
  };

  std::vector<NotationNote> notes;
  int32_t next_id = 0;

  // Directionals first → flick map by tick+lane (keep width for standalone flicks).
  struct FlickKey {
    int64_t tick;
    int lane;
    bool operator<(const FlickKey& o) const {
      return tick < o.tick || (tick == o.tick && lane < o.lane);
    }
  };
  struct DirectionalInfo {
    int32_t scratch = 0;
    int width = 1;
  };
  std::map<FlickKey, DirectionalInfo> directional_scratch;
  for (const auto& ev : events) {
    if (ev.category != 5) continue;
    FlickKey key{ev.tick, map_lane(ev.lane)};
    directional_scratch[key] =
        DirectionalInfo{flick_scratch_from_directional(ev.type), map_width(ev.lane, ev.width)};
  }

  // Taps. Damage (#1 type 4) marks intentional headless hold starts — collected
  // for hold assembly, never kept as chart notes (WDS has no Damage type).
  std::vector<SusDamageMarker> damage_markers;
  for (const auto& ev : events) {
    if (ev.category != 1) continue;
    if (ev.type == kSusTapDamage) {
      SusDamageMarker dmg;
      dmg.tick = static_cast<float>(ev.tick);
      dmg.lane = map_lane(ev.lane);
      dmg.width = map_width(ev.lane, ev.width);
      damage_markers.push_back(dmg);
      continue;
    }
    NotationNote note;
    note.id = next_id++;
    note.start_tick = static_cast<float>(ev.tick);
    note.end_tick = note.start_tick;
    note.lane = map_lane(ev.lane);
    note.width = map_width(ev.lane, ev.width);
    note.note_type = tap_type_from_sus(ev.type);
    note.gimmick_type = GimmickType::None;
    note.scratch_length = 0;
    FlickKey key{ev.tick, note.lane};
    if (const auto it = directional_scratch.find(key); it != directional_scratch.end()) {
      note.note_type = NoteType::Flick;
      note.scratch_length = it->second.scratch;
      directional_scratch.erase(it);
    } else if (note.note_type == NoteType::Flick) {
      note.scratch_length = 0;
    }
    notes.push_back(note);
  }

  // Remaining directionals without tap → standalone flicks.
  for (const auto& [key, info] : directional_scratch) {
    NotationNote note;
    note.id = next_id++;
    note.start_tick = static_cast<float>(key.tick);
    note.end_tick = note.start_tick;
    note.lane = key.lane;
    note.width = info.width;
    note.note_type = NoteType::Flick;
    note.scratch_length = info.scratch;
    notes.push_back(note);
  }

  // Holds / slides.
  std::map<HoldKey, std::vector<RawEvent>> hold_groups;
  for (const auto& ev : events) {
    if (ev.category != 2 && ev.category != 3 && ev.category != 4) continue;
    HoldKey key;
    key.category = ev.category == 2 ? 2 : 3;  // treat 3/4 as slide family
    key.channel = ev.channel;
    key.lane = (ev.category == 2) ? ev.lane : -1;
    hold_groups[key].push_back(ev);
  }

  for (auto& [key, group] : hold_groups) {
    std::sort(group.begin(), group.end(),
              [](const RawEvent& a, const RawEvent& b) {
                if (a.tick != b.tick) return a.tick < b.tick;
                return a.type < b.type;
              });

    // SUS 2.7: same channel links points. After an end, a later start begins a new
    // hold — pair start/end sequentially instead of keeping only the last of each.
    const RawEvent* start = nullptr;
    std::vector<const RawEvent*> pending_mids;
    auto emit_hold = [&](const RawEvent& end) {
      if (start == nullptr || end.tick < start->tick) {
        start = nullptr;
        pending_mids.clear();
        return;
      }

      const int start_lane = map_lane(start->lane);
      const int start_width = map_width(start->lane, start->width);
      const int end_lane = map_lane(end.lane);
      const int end_right = std::min(11, end_lane + map_width(end.lane, end.width) - 1);

      // #2 = Hold family, #3/#4 = ScratchHold family (even when end span == start).
      // Critical tap fully covering the start → CriticalHold* body (金头), headless.
      // Damage marker → intentional headless. Else fully covered → headless.
      // Otherwise auto-generate a head via make_auto_hold_head.
      const bool scratch = key.category != 2;
      NotationNote body;
      body.id = next_id++;
      body.start_tick = static_cast<float>(start->tick);
      body.end_tick = static_cast<float>(end.tick);
      body.lane = start_lane;
      body.width = start_width;
      body.gimmick_type = GimmickType::None;
      body.scratch_length = 0;

      NotationNote cover_probe = body;
      cover_probe.id = -1;
      const bool critical_cover = hold_start_fully_covered_by_critical(cover_probe, notes);

      if (scratch) {
        body.note_type =
            critical_cover ? NoteType::ScratchCriticalHold : NoteType::ScratchHold;
        set_scratch_hold_end_lanes(body, end_lane, end_right);
      } else {
        body.note_type = critical_cover ? NoteType::CriticalHold : NoteType::Hold;
      }

      const bool damage_headless = std::any_of(
          damage_markers.begin(), damage_markers.end(),
          [&](const SusDamageMarker& d) { return matches_damage_marker(d, body); });
      notes.push_back(body);

      // Critical cover already provides the start judgment — stay headless.
      if (!critical_cover && !damage_headless && !hold_start_fully_covered(body, notes)) {
        ChartDocument doc;
        doc.set_notes(notes);
        if (auto head = make_auto_hold_head(doc, body)) {
          head->id = next_id++;
          notes.push_back(*head);
        }
      }

      for (const RawEvent* mid : pending_mids) {
        if (mid->tick <= start->tick || mid->tick >= end.tick) continue;
        NotationNote star;
        star.id = next_id++;
        star.start_tick = static_cast<float>(mid->tick);
        star.end_tick = star.start_tick;
        star.lane = map_lane(mid->lane);
        star.width = map_width(mid->lane, mid->width);
        star.note_type = NoteType::Sound;
        star.gimmick_type = GimmickType::None;
        star.scratch_length = 0;
        notes.push_back(star);
      }

      start = nullptr;
      pending_mids.clear();
    };

    for (const auto& ev : group) {
      if (ev.type == 1) {
        start = &ev;
        pending_mids.clear();
      } else if (ev.type == 2) {
        emit_hold(ev);
      } else if (ev.type == 3) {
        // Visible mid → WDS Sound star.
        if (start != nullptr) pending_mids.push_back(&ev);
      }
      // type 4 bezier control / type 5 invisible mid — no WDS note
    }
  }

  std::sort(notes.begin(), notes.end(), [](const NotationNote& a, const NotationNote& b) {
    if (a.start_tick != b.start_tick) return a.start_tick < b.start_tick;
    if (a.lane != b.lane) return a.lane < b.lane;
    return a.id < b.id;
  });
  for (size_t i = 0; i < notes.size(); ++i) notes[i].id = static_cast<int32_t>(i);

  chart.notes = std::move(notes);
  chart.concurrent_lines = build_concurrent_lines(chart.notes, chart.timing);

  out.chart = std::move(chart);
  out.meta = std::move(meta);
  return {SerializeError::Ok, {}};
}

SerializeResult SusChartFormat::load_file(const std::string& path, SusChartLoadResult& out) {
  SerializeResult status;
  const std::string text = read_file_text(path, status);
  if (status.error != SerializeError::Ok) return status;
  return parse(text, out);
}

SerializeResult SusChartFormat::serialize(const NotationChart& chart,
                                          const SusChartSaveOptions& options,
                                          std::string& out_text) {
  MusicTiming timing = chart.timing;
  normalize_timing_points(timing);
  const int tpq = std::max(1, timing.ticks_per_quarter);
  const int lane_pad = options.ched_lane_padding ? 2 : 0;

  auto sus_lane = [&](int32_t lane) { return std::clamp(lane + lane_pad, 0, 35); };

  // Build measure starts from the real timing map (meters / bar lengths).
  int64_t max_tick = 0;
  for (const auto& p : timing.points) {
    max_tick = std::max(max_tick, static_cast<int64_t>(p.tick));
  }
  for (const auto& n : chart.notes) {
    max_tick = std::max(max_tick, static_cast<int64_t>(std::llround(n.start_tick)));
    max_tick = std::max(max_tick, static_cast<int64_t>(std::llround(n.end_tick)));
  }
  std::vector<int32_t> measure_starts = measure_ticks_in_range(0, static_cast<int32_t>(max_tick), timing);
  if (measure_starts.empty()) measure_starts.push_back(0);
  // Extend one bar past the last content tick so locate() always has a length.
  while (static_cast<int64_t>(measure_starts.back()) <= max_tick) {
    const TimingPoint& meter = timing_meter_at(timing, measure_starts.back());
    const int32_t step = measure_length_ticks(meter, tpq);
    const int32_t candidate = measure_starts.back() + step;
    if (candidate <= measure_starts.back()) break;
    measure_starts.push_back(candidate);
  }

  struct MeasureLoc {
    int measure = 0;
    int64_t local = 0;
    int64_t measure_len = 1;
  };
  auto locate_tick = [&](int64_t tick) -> MeasureLoc {
    MeasureLoc loc;
    if (measure_starts.empty()) return loc;
    tick = std::max<int64_t>(0, tick);
    auto it = std::upper_bound(measure_starts.begin(), measure_starts.end(),
                               static_cast<int32_t>(tick));
    size_t idx = 0;
    if (it != measure_starts.begin()) idx = static_cast<size_t>((it - measure_starts.begin()) - 1);
    loc.measure = static_cast<int>(idx);
    loc.local = tick - measure_starts[idx];
    const int64_t start = measure_starts[idx];
    const int64_t end = (idx + 1 < measure_starts.size())
                            ? static_cast<int64_t>(measure_starts[idx + 1])
                            : start + measure_length_ticks(timing_meter_at(timing, static_cast<int32_t>(start)),
                                                          tpq);
    loc.measure_len = std::max<int64_t>(1, end - start);
    if (loc.local >= loc.measure_len) loc.local = loc.measure_len - 1;
    return loc;
  };

  auto measure_of = [&](int64_t tick) -> int { return locate_tick(tick).measure; };
  auto slot_of = [&](int64_t tick, int slots) -> int {
    const auto loc = locate_tick(tick);
    if (slots <= 0) return 0;
    return static_cast<int>((loc.local * slots + loc.measure_len / 2) / loc.measure_len);
  };

  // Choose subdivision so every note tick lands on a slot (gcd-friendly).
  // Per-measure we pick the LCM of needed divisions capped at 192.
  auto needed_div = [&](int64_t tick) {
    const auto loc = locate_tick(tick);
    if (loc.local == 0) return 1;
    int div = static_cast<int>(loc.measure_len / std::gcd(loc.local, loc.measure_len));
    return std::clamp(div, 1, 192);
  };

  std::ostringstream ss;
  ss << "This file was generated by WDS Chart Editor.\n";
  const auto& meta = options.meta;
  auto emit_str = [&](const char* key, const std::string& value) {
    ss << '#' << key << " \"" << value << "\"\n";
  };
  emit_str("TITLE", meta.title);
  emit_str("ARTIST", meta.artist);
  emit_str("DESIGNER", meta.designer);
  if (!meta.difficulty.empty()) ss << "#DIFFICULTY " << meta.difficulty << '\n';
  else ss << "#DIFFICULTY 0\n";
  if (!meta.play_level.empty()) ss << "#PLAYLEVEL " << meta.play_level << '\n';
  emit_str("SONGID", meta.song_id);
  emit_str("WAVE", meta.wave_path);
  ss.setf(std::ios::fixed);
  ss.precision(3);
  ss << "#WAVEOFFSET " << (meta.wave_offset_sec != 0.0
                               ? meta.wave_offset_sec
                               : static_cast<double>(timing.offset_ms) / 1000.0)
     << '\n';
  emit_str("JACKET", meta.jacket_path);
  ss << "\n#REQUEST \"ticks_per_beat " << tpq << "\"\n\n";

  // Deferred SUS data lines so #MEASUREBS can be emitted in measure order.
  struct SusDataLine {
    int measure = 0;
    std::string suffix;  // e.g. "02", "08", "12", "20a"
    std::string data;
  };
  std::vector<SusDataLine> data_lines;
  auto push_data_line = [&](int meas, std::string suffix, std::string data) {
    data_lines.push_back(SusDataLine{meas, std::move(suffix), std::move(data)});
  };

  // #mmm02 measure lengths from the timing map (SUS beats = measure_ticks / tpq).
  {
    double prev_beats = -1.0;
    for (size_t i = 0; i + 1 < measure_starts.size(); ++i) {
      const double beats =
          static_cast<double>(measure_starts[i + 1] - measure_starts[i]) / static_cast<double>(tpq);
      if (i > 0 && std::abs(beats - prev_beats) < 1e-9) continue;
      std::ostringstream beats_ss;
      beats_ss.setf(std::ios::fixed);
      beats_ss.precision(4);
      beats_ss << beats;
      push_data_line(static_cast<int>(i), "02", beats_ss.str());
      prev_beats = beats;
    }
    if (!measure_starts.empty() && prev_beats < 0.0) {
      // Single trailing start with no length sample — emit active meter at tick 0.
      const TimingPoint& meter = timing_meter_at(timing, 0);
      const double beats =
          static_cast<double>(measure_length_ticks(meter, tpq)) / static_cast<double>(tpq);
      std::ostringstream beats_ss;
      beats_ss.setf(std::ios::fixed);
      beats_ss.precision(4);
      beats_ss << beats;
      push_data_line(0, "02", beats_ss.str());
    }
  }

  // BPM definitions from authored BPM points.
  std::map<double, int> bpm_to_id;
  int next_bpm_id = 1;
  auto bpm_id_for = [&](double bpm) {
    const auto it = bpm_to_id.find(bpm);
    if (it != bpm_to_id.end()) return it->second;
    const int id = next_bpm_id++;
    bpm_to_id[bpm] = id;
    return id;
  };
  for (const auto& p : timing.points) {
    if (p.has_bpm) bpm_id_for(p.bpm);
  }
  if (bpm_to_id.empty()) bpm_id_for(timing.bpm);
  for (const auto& [bpm, id] : bpm_to_id) {
    ss.precision(4);
    ss << "#BPM" << base36_digit(id / 36) << base36_digit(id % 36) << ": " << bpm << '\n';
  }
  ss << '\n';

  // Emit #mmm08 BPM changes at real in-measure subdivisions (not just bar heads).
  struct MeasBpmEvent {
    int64_t tick = 0;
    int id = 1;
  };
  std::map<int, std::vector<MeasBpmEvent>> bpm_by_measure;
  for (const auto& p : timing.points) {
    if (!p.has_bpm) continue;
    const auto loc = locate_tick(p.tick);
    bpm_by_measure[loc.measure].push_back({p.tick, bpm_id_for(p.bpm)});
  }
  for (auto& [meas, events] : bpm_by_measure) {
    std::sort(events.begin(), events.end(),
              [](const MeasBpmEvent& a, const MeasBpmEvent& b) { return a.tick < b.tick; });
    int div = 1;
    for (const auto& ev : events) {
      const int need = needed_div(ev.tick);
      div = std::min(192, div / static_cast<int>(std::gcd(div, need)) * need);
    }
    std::string data(static_cast<size_t>(div) * 2, '0');
    for (const auto& ev : events) {
      const int slot = std::clamp(slot_of(ev.tick, div), 0, div - 1);
      data[static_cast<size_t>(slot) * 2] = base36_digit(ev.id / 36);
      data[static_cast<size_t>(slot) * 2 + 1] = base36_digit(ev.id % 36);
    }
    push_data_line(meas, "08", std::move(data));
  }

  // Channel allocator for holds.
  int next_channel = 0;
  auto alloc_channel = [&]() {
    const int ch = next_channel++ % 36;
    return ch;
  };

  // Group notes into emit buckets: header -> measure -> (slot data map).
  // key: header without measure (e.g. "12", "20a", "52")
  struct Cell {
    int type = 0;
    int width = 0;
  };
  // header_suffix -> measure -> slot_count -> slot_index -> cell
  std::map<std::string, std::map<int, std::map<int, std::map<int, Cell>>>> buckets;

  auto place = [&](const std::string& suffix, int64_t tick, int type, int width) {
    const int meas = measure_of(tick);
    int div = needed_div(tick);
    // Raise existing division if needed.
    auto& by_div = buckets[suffix][meas];
    if (!by_div.empty()) {
      const int existing = by_div.begin()->first;
      const int lcm = existing / static_cast<int>(std::gcd(existing, div)) * div;
      div = std::min(192, lcm);
      if (div != existing) {
        // Remap old slots into new division.
        std::map<int, Cell> remapped;
        for (const auto& [idx, cell] : by_div[existing]) {
          remapped[idx * div / existing] = cell;
        }
        by_div.clear();
        by_div[div] = std::move(remapped);
      }
    }
    const int slot = slot_of(tick, div);
    by_div[div][slot] = Cell{type, width};
  };

  // Index hold bodies by start for pairing with heads.
  auto notes_overlap = [](const NotationNote& a, const NotationNote& b) {
    return a.lane <= b.end_lane() && b.lane <= a.end_lane();
  };
  auto same_tick = [](float a, float b) { return std::abs(a - b) < 0.5f; };

  struct HoldEmit {
    const NotationNote* head = nullptr;
    const NotationNote* body = nullptr;
    int channel = 0;
  };
  std::vector<HoldEmit> holds;
  std::vector<const NotationNote*> mids;
  std::vector<const NotationNote*> taps;

  // Collect heads first so bodies that appear earlier in `notes` still pair.
  for (const auto& n : chart.notes) {
    if (n.note_type == NoteType::HoldStart || n.note_type == NoteType::CriticalHoldStart ||
        n.note_type == NoteType::ScratchHoldStart ||
        n.note_type == NoteType::ScratchCriticalHoldStart) {
      HoldEmit h;
      h.head = &n;
      holds.push_back(h);
    } else if (n.note_type == NoteType::Sound || n.note_type == NoteType::SoundPurple) {
      mids.push_back(&n);
    } else if (n.note_type == NoteType::HoldEighth) {
      // Synthesized grid — never export (would become Sound stars on re-import).
    } else if (n.gimmick_type == GimmickType::None || !is_split_lane_gimmick(n.gimmick_type)) {
      // Nontail* bodies are hold ribbons (exported below), not taps — is_hold_with_tail
      // intentionally excludes them for combo/sync, so exclude explicitly here.
      if (n.note_type != NoteType::None && n.note_type != NoteType::HiSpeed &&
          !is_hold_with_tail(n.note_type) && !is_nontail_hold_body(n.note_type)) {
        taps.push_back(&n);
      }
    }
  }
  for (const auto& n : chart.notes) {
    if (!(n.note_type == NoteType::Hold || n.note_type == NoteType::CriticalHold ||
          n.note_type == NoteType::ScratchHold || n.note_type == NoteType::ScratchCriticalHold ||
          n.note_type == NoteType::NontailHold || n.note_type == NoteType::NontailCriticalHold ||
          n.note_type == NoteType::NontailScratchHold ||
          n.note_type == NoteType::NontailScratchCriticalHold)) {
      continue;
    }
    bool attached = false;
    for (auto& h : holds) {
      if (h.body != nullptr || h.head == nullptr) continue;
      // Partial-width heads share start tick and overlap lanes (not necessarily lane==).
      if (same_tick(h.head->start_tick, n.start_tick) && notes_overlap(*h.head, n)) {
        h.body = &n;
        attached = true;
        break;
      }
    }
    if (!attached) {
      // Headless body still exports as a hold/slide using the body span.
      HoldEmit h;
      h.body = &n;
      holds.push_back(h);
    }
  }

  for (auto& h : holds) {
    const NotationNote* body = h.body;
    const NotationNote* head = h.head;
    if (body == nullptr) continue;  // orphan head with no body — skip
    const bool authored_headless = (head == nullptr);
    if (head == nullptr) head = body;

    h.channel = alloc_channel();
    const int64_t t0 = static_cast<int64_t>(std::llround(body->start_tick));
    const int64_t t1 = static_cast<int64_t>(std::llround(body->end_tick));
    if (t1 <= t0) continue;

    // Hold/slide channel follows body ribbon geometry. Authored heads (including
    // partial-width) only drive Critical judgment taps — using head width here
    // would shrink the body on roundtrip (w=3 body + w=2 head → w=2 body).
    const int channel_lane = body->lane;
    const int channel_width = std::clamp(body->width, 1, 35);
    const int sl = sus_lane(channel_lane);

    int end_l = channel_lane;
    int end_w = channel_width;
    const bool scratch_body =
        body->note_type == NoteType::ScratchHold ||
        body->note_type == NoteType::ScratchCriticalHold ||
        body->note_type == NoteType::NontailScratchHold ||
        body->note_type == NoteType::NontailScratchCriticalHold;
    if (scratch_body) {
      const auto range = get_scratch_end_lane_range(*body);
      end_l = range.first;
      end_w = std::max(1, range.second - range.first + 1);
    }

    // Hold family → #2; ScratchHold family → #3 even when end span equals start
    // (otherwise purple/blue collapse on roundtrip).
    const bool as_slide = scratch_body;
    std::string suffix = as_slide ? "3" : "2";
    suffix.push_back(base36_digit(sl));
    suffix.push_back(base36_digit(h.channel));
    place(suffix, t0, 1, channel_width);

    std::string end_suffix = as_slide ? "3" : "2";
    end_suffix.push_back(base36_digit(sus_lane(end_l)));
    end_suffix.push_back(base36_digit(h.channel));
    place(end_suffix, t1, 2, std::clamp(end_w, 1, 35));

    // Critical Hold* → Critical tap + hold/slide channel (no separate HoldStart*).
    const bool critical_hold =
        (!authored_headless &&
         (head->note_type == NoteType::CriticalHoldStart ||
          head->note_type == NoteType::ScratchCriticalHoldStart)) ||
        body->note_type == NoteType::CriticalHold ||
        body->note_type == NoteType::ScratchCriticalHold ||
        body->note_type == NoteType::NontailCriticalHold ||
        body->note_type == NoteType::NontailScratchCriticalHold;
    if (critical_hold) {
      const int crit_lane = !authored_headless ? head->lane : body->lane;
      const int crit_w =
          std::clamp(!authored_headless ? head->width : body->width, 1, 35);
      std::string crit_suffix = "1";
      crit_suffix.push_back(base36_digit(sus_lane(crit_lane)));
      place(crit_suffix, t0, kSusTapCritical, crit_w);
    }

    // Truly headless (authored without head, start not fully covered) → Damage
    // marker of equal body lane/width. Fully covered / CriticalHold need no marker.
    if (authored_headless && !critical_hold &&
        !hold_start_fully_covered(*body, chart.notes)) {
      std::string dmg_suffix = "1";
      dmg_suffix.push_back(base36_digit(sus_lane(body->lane)));
      place(dmg_suffix, t0, kSusTapDamage, std::clamp(body->width, 1, 35));
    }

    for (const NotationNote* mid : mids) {
      // HoldEighth is synthesized in-editor — never write as SUS mid (type 3 → Sound).
      if (mid->note_type == NoteType::HoldEighth) continue;
      if (mid->start_tick <= body->start_tick || mid->start_tick >= body->end_tick) continue;
      if (mid->lane + mid->width <= body->lane || mid->lane >= body->lane + body->width) {
        continue;
      }
      std::string mid_suffix = as_slide ? "3" : "2";
      mid_suffix.push_back(base36_digit(sus_lane(mid->lane)));
      mid_suffix.push_back(base36_digit(h.channel));
      place(mid_suffix, static_cast<int64_t>(std::llround(mid->start_tick)), 3,
            std::clamp(mid->width, 1, 35));
    }
  }

  for (const NotationNote* n : taps) {
    const int64_t tick = static_cast<int64_t>(std::llround(n->start_tick));
    const int sl = sus_lane(n->lane);
    const int w = std::clamp(n->width, 1, 35);
    if (n->note_type == NoteType::Flick) {
      std::string tap_suffix = "1";
      tap_suffix.push_back(base36_digit(sl));
      place(tap_suffix, tick, kSusTapFlick, w);
      int dir = 1;  // up
      if (n->scratch_length < 0) dir = 3;
      else if (n->scratch_length > 0) dir = 4;
      std::string dir_suffix = "5";
      dir_suffix.push_back(base36_digit(sl));
      place(dir_suffix, tick, dir, w);
    } else {
      const int type = (n->note_type == NoteType::Critical) ? kSusTapCritical : kSusTapNormal;
      std::string suffix = "1";
      suffix.push_back(base36_digit(sl));
      place(suffix, tick, type, w);
    }
  }

  // Flush buckets + deferred timing lines in measure order with #MEASUREBS.
  for (const auto& [suffix, by_meas] : buckets) {
    for (const auto& [meas, by_div] : by_meas) {
      if (by_div.empty()) continue;
      const int div = by_div.begin()->first;
      const auto& cells = by_div.begin()->second;
      std::string data(static_cast<size_t>(div) * 2, '0');
      for (const auto& [slot, cell] : cells) {
        if (slot < 0 || slot >= div) continue;
        data[static_cast<size_t>(slot) * 2] = base36_digit(cell.type);
        data[static_cast<size_t>(slot) * 2 + 1] = base36_digit(cell.width);
      }
      push_data_line(meas, suffix, std::move(data));
    }
  }

  std::sort(data_lines.begin(), data_lines.end(),
            [](const SusDataLine& a, const SusDataLine& b) {
              if (a.measure != b.measure) return a.measure < b.measure;
              return a.suffix < b.suffix;
            });
  int measure_base = 0;
  for (const auto& line : data_lines) {
    const int base = (std::max(0, line.measure) / 1000) * 1000;
    if (base != measure_base) {
      ss << "#MEASUREBS " << base << '\n';
      measure_base = base;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%03d", line.measure - measure_base);
    ss << '#' << buf << line.suffix << ": " << line.data << '\n';
  }

  out_text = ss.str();
  return {SerializeError::Ok, {}};
}

SerializeResult SusChartFormat::save_file(const NotationChart& chart, const std::string& path,
                                          const SusChartSaveOptions& options) {
  std::string text;
  const auto result = serialize(chart, options, text);
  if (result.error != SerializeError::Ok) return result;
  return write_file_text(path, text);
}

}  // namespace wds::chart_editor
