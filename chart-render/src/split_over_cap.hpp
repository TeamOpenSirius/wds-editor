#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#ifndef WDS_DIAG_ENABLED
#define WDS_DIAG_ENABLED 0
#endif

#if WDS_DIAG_ENABLED
#include <cstdio>
#ifndef _WIN32
#include <unistd.h>
#endif
#endif

namespace wds::chart_render {

// Preview-pool hop, not Unity 2022.3 maxParticles. Between splits the caller
// still walks remaining budget first-come. Within one split: per-line share,
// then round-robin *counts* across (layer, sys) groups. Within a group:
// birth order (job_i / birth_index tie-break). Ribbon groups keep one
// contiguous run at the newest/head end so stitch has no holes and budget
// changes only move the oldest cut.
struct OverCapCandidate {
  int priority = 0;
  // Split instance that owns this station (EmitContext.line_start_ms). The
  // per-line budget share is really per split instance: two overlapping splits
  // of one effect on the same line are two ParticleSystems in OpenWDS.
  int64_t instance_start_ms = 0;
  int32_t line_index = 0;
  uint16_t layer = 0;
  uint32_t task_i = 0;
  uint32_t job_i = 0;
  int32_t birth = 0;
  uint32_t sys = 0;
  uint8_t primary = 0;
  uint8_t ribbon = 0;
};

inline bool over_cap_legacy_env() noexcept {
  const char* raw = std::getenv("WDS_SPLIT_CAP_LEGACY");
  return raw != nullptr && raw[0] == '1' && raw[1] == '\0';
}

inline bool over_cap_same_line(const OverCapCandidate& a, const OverCapCandidate& b) noexcept {
  return a.line_index == b.line_index && a.instance_start_ms == b.instance_start_ms;
}

inline bool over_cap_identity_lt(const OverCapCandidate& a, const OverCapCandidate& b) noexcept {
  if (a.line_index != b.line_index) {
    return a.line_index < b.line_index;
  }
  if (a.instance_start_ms != b.instance_start_ms) {
    return a.instance_start_ms < b.instance_start_ms;
  }
  if (a.layer != b.layer) {
    return a.layer < b.layer;
  }
  if (a.sys != b.sys) {
    return a.sys < b.sys;
  }
  if (a.birth != b.birth) {
    return a.birth < b.birth;
  }
  if (a.job_i != b.job_i) {
    return a.job_i < b.job_i;
  }
  return a.task_i < b.task_i;
}

inline bool over_cap_already_sorted(const std::vector<OverCapCandidate>& cs) noexcept {
  for (size_t i = 1; i < cs.size(); ++i) {
    if (over_cap_identity_lt(cs[i], cs[i - 1])) {
      return false;
    }
  }
  return true;
}

inline bool over_cap_same_candidate(const OverCapCandidate& a, const OverCapCandidate& b) noexcept {
  return a.priority == b.priority && a.instance_start_ms == b.instance_start_ms &&
         a.line_index == b.line_index && a.layer == b.layer && a.task_i == b.task_i &&
         a.job_i == b.job_i && a.birth == b.birth && a.sys == b.sys && a.primary == b.primary &&
         a.ribbon == b.ribbon;
}

inline bool over_cap_within_bucket_lt(const OverCapCandidate& a, const OverCapCandidate& b) noexcept {
  if (a.birth != b.birth) {
    return a.birth < b.birth;
  }
  if (a.job_i != b.job_i) {
    return a.job_i < b.job_i;
  }
  return a.task_i < b.task_i;
}

inline bool over_cap_within_bucket_eq(const OverCapCandidate& a, const OverCapCandidate& b) noexcept {
  return a.birth == b.birth && a.job_i == b.job_i && a.task_i == b.task_i;
}

inline bool over_cap_line_runs_identity_ready(const std::vector<OverCapCandidate>& cs) noexcept {
  const size_t n = cs.size();
  if (n <= 1) {
    return true;
  }
  bool have_prev = false;
  int32_t prev_line = 0;
  int64_t prev_inst = 0;
  size_t i = 0;
  while (i < n) {
    size_t j = i + 1;
    while (j < n && over_cap_same_line(cs[j], cs[i])) {
      ++j;
    }
    if (have_prev) {
      if (cs[i].line_index < prev_line) {
        return false;
      }
      if (cs[i].line_index == prev_line && cs[i].instance_start_ms < prev_inst) {
        return false;
      }
      if (cs[i].line_index == prev_line && cs[i].instance_start_ms == prev_inst) {
        return false;
      }
    }
    have_prev = true;
    prev_line = cs[i].line_index;
    prev_inst = cs[i].instance_start_ms;
    i = j;
  }
  return true;
}

struct OverCapBucketScratch {
  std::vector<OverCapCandidate> buf;
  std::vector<uint32_t> counts;
  std::vector<uint32_t> offsets;
  std::vector<uint16_t> key_layer;
  std::vector<uint32_t> key_sys;
  std::vector<uint32_t> order;
  std::vector<uint64_t> hash_key;
  std::vector<uint32_t> hash_id;
};

inline uint32_t over_cap_hash_probe(const OverCapBucketScratch& s, uint64_t packed) noexcept {
  const uint32_t mask = static_cast<uint32_t>(s.hash_id.size() - 1);
  uint32_t h = static_cast<uint32_t>(packed ^ (packed >> 32)) * 0x9e3779b9u;
  for (;;) {
    h &= mask;
    if (s.hash_id[h] == ~uint32_t{0} || s.hash_key[h] == packed) {
      return h;
    }
    ++h;
  }
}

inline void over_cap_hash_reset(OverCapBucketScratch& s, uint32_t nslots) {
  if (nslots < 8u) {
    nslots = 8u;
  }
  s.hash_key.resize(static_cast<size_t>(nslots));
  s.hash_id.assign(static_cast<size_t>(nslots), ~uint32_t{0});
  for (uint32_t i = 0; i < static_cast<uint32_t>(s.key_layer.size()); ++i) {
    const uint64_t packed =
        (static_cast<uint64_t>(s.key_layer[i]) << 32) | static_cast<uint64_t>(s.key_sys[i]);
    const uint32_t slot = over_cap_hash_probe(s, packed);
    s.hash_key[slot] = packed;
    s.hash_id[slot] = i;
  }
}

inline uint32_t over_cap_hash_insert(OverCapBucketScratch& s, uint16_t layer, uint32_t sys) {
  const uint64_t packed = (static_cast<uint64_t>(layer) << 32) | static_cast<uint64_t>(sys);
  if (s.hash_id.size() < 8u || (s.key_layer.size() + 1u) * 2u >= s.hash_id.size()) {
    uint32_t next = s.hash_id.size() < 8u ? 64u : static_cast<uint32_t>(s.hash_id.size() * 2u);
    over_cap_hash_reset(s, next);
  }
  const uint32_t slot = over_cap_hash_probe(s, packed);
  if (s.hash_id[slot] != ~uint32_t{0}) {
    return s.hash_id[slot];
  }
  const uint32_t id = static_cast<uint32_t>(s.key_layer.size());
  s.key_layer.push_back(layer);
  s.key_sys.push_back(sys);
  s.hash_key[slot] = packed;
  s.hash_id[slot] = id;
  return id;
}

inline bool over_cap_bucket_one_run(const OverCapCandidate* src, size_t n, OverCapCandidate* dst,
                                   OverCapBucketScratch& s) {
  if (n == 0) {
    return true;
  }
  if (n == 1) {
    dst[0] = src[0];
    return true;
  }
  s.key_layer.clear();
  s.key_sys.clear();
  if (s.hash_id.size() < 64u) {
    over_cap_hash_reset(s, 64u);
  } else {
    std::fill(s.hash_id.begin(), s.hash_id.end(), ~uint32_t{0});
  }
  s.counts.clear();
  for (size_t i = 0; i < n; ++i) {
    const uint32_t id = over_cap_hash_insert(s, src[i].layer, src[i].sys);
    if (id >= s.counts.size()) {
      s.counts.resize(static_cast<size_t>(id) + 1u, 0);
    }
    ++s.counts[id];
  }
  const uint32_t nk = static_cast<uint32_t>(s.key_layer.size());
  if (nk == 0 || nk > 65536u) {
    return false;
  }
  s.order.resize(static_cast<size_t>(nk));
  for (uint32_t i = 0; i < nk; ++i) {
    s.order[i] = i;
  }
  std::sort(s.order.begin(), s.order.end(), [&](uint32_t a, uint32_t b) {
    if (s.key_layer[a] != s.key_layer[b]) {
      return s.key_layer[a] < s.key_layer[b];
    }
    return s.key_sys[a] < s.key_sys[b];
  });
  s.offsets.resize(static_cast<size_t>(nk));
  uint32_t acc = 0;
  for (uint32_t i = 0; i < nk; ++i) {
    const uint32_t id = s.order[i];
    s.offsets[id] = acc;
    acc += s.counts[id];
  }
  if (acc != static_cast<uint32_t>(n)) {
    return false;
  }
  for (uint32_t i = 0; i < nk; ++i) {
    s.counts[i] = s.offsets[i];
  }
  for (size_t i = 0; i < n; ++i) {
    const uint64_t packed =
        (static_cast<uint64_t>(src[i].layer) << 32) | static_cast<uint64_t>(src[i].sys);
    const uint32_t slot = over_cap_hash_probe(s, packed);
    const uint32_t id = s.hash_id[slot];
    dst[s.counts[id]++] = src[i];
  }
  for (uint32_t i = 0; i < nk; ++i) {
    const uint32_t id = s.order[i];
    const uint32_t b = s.offsets[id];
    const uint32_t e = s.counts[id];
    bool sorted = true;
    for (uint32_t j = b + 1; j < e; ++j) {
      if (over_cap_within_bucket_eq(dst[j], dst[j - 1])) {
        return false;
      }
      if (over_cap_within_bucket_lt(dst[j], dst[j - 1])) {
        sorted = false;
      }
    }
    if (!sorted) {
      std::sort(dst + b, dst + e, over_cap_within_bucket_lt);
    }
  }
  return true;
}

inline bool over_cap_bucket_sort(std::vector<OverCapCandidate>& cs) {
  const size_t n = cs.size();
  if (n <= 1) {
    return true;
  }
  if (!over_cap_line_runs_identity_ready(cs)) {
    return false;
  }
  static thread_local OverCapBucketScratch scratch;
  scratch.buf.resize(n);
  size_t i = 0;
  while (i < n) {
    size_t j = i + 1;
    while (j < n && over_cap_same_line(cs[j], cs[i])) {
      ++j;
    }
    if (!over_cap_bucket_one_run(cs.data() + i, j - i, scratch.buf.data() + i, scratch)) {
      return false;
    }
    i = j;
  }
  cs.swap(scratch.buf);
  return true;
}

#if WDS_DIAG_ENABLED
inline bool over_cap_bucket_verify_env() noexcept {
  const char* raw = std::getenv("WDS_SPLIT_CAP_BUCKET_VERIFY");
  return raw != nullptr && raw[0] == '1' && raw[1] == '\0';
}

inline uint64_t& over_cap_verify_bucket_calls() noexcept {
  static uint64_t v = 0;
  return v;
}
inline uint64_t& over_cap_verify_already_sorted() noexcept {
  static uint64_t v = 0;
  return v;
}
inline uint64_t& over_cap_verify_fallback() noexcept {
  static uint64_t v = 0;
  return v;
}
inline uint64_t& over_cap_verify_mismatches() noexcept {
  static uint64_t v = 0;
  return v;
}
inline uint64_t& over_cap_verify_under_cap() noexcept {
  static uint64_t v = 0;
  return v;
}

inline void over_cap_bucket_verify_dump() noexcept {
  if (!over_cap_bucket_verify_env()) {
    return;
  }
  static bool dumped = false;
  if (dumped) {
    return;
  }
  dumped = true;
  long pid = 0;
#ifndef _WIN32
  pid = static_cast<long>(::getpid());
#endif
  std::fprintf(stderr,
               "CAP_BUCKET_VERIFY pid=%ld under_cap_calls=%llu bucket_calls=%llu "
               "already_sorted=%llu fallback_calls=%llu mismatches=%llu\n",
               pid, static_cast<unsigned long long>(over_cap_verify_under_cap()),
               static_cast<unsigned long long>(over_cap_verify_bucket_calls()),
               static_cast<unsigned long long>(over_cap_verify_already_sorted()),
               static_cast<unsigned long long>(over_cap_verify_fallback()),
               static_cast<unsigned long long>(over_cap_verify_mismatches()));
}

inline void over_cap_bucket_verify_register() noexcept {
  static bool once = false;
  if (!once) {
    once = true;
    std::atexit(over_cap_bucket_verify_dump);
  }
}

inline void over_cap_bucket_verify_check(const std::vector<OverCapCandidate>& got,
                                        const std::vector<OverCapCandidate>& expected) noexcept {
  if (got.size() != expected.size()) {
    ++over_cap_verify_mismatches();
    std::fprintf(stderr, "CAP_BUCKET_VERIFY mismatch size got=%zu expected=%zu\n", got.size(),
                 expected.size());
    over_cap_bucket_verify_dump();
    std::abort();
  }
  for (size_t i = 0; i < got.size(); ++i) {
    if (over_cap_same_candidate(got[i], expected[i])) {
      continue;
    }
    ++over_cap_verify_mismatches();
    const OverCapCandidate& g = got[i];
    const OverCapCandidate& e = expected[i];
    std::fprintf(stderr,
                 "CAP_BUCKET_VERIFY mismatch i=%zu got(line=%d inst=%lld layer=%u sys=%u "
                 "birth=%d job=%u task=%u pri=%d primary=%u ribbon=%u) "
                 "exp(line=%d inst=%lld layer=%u sys=%u birth=%d job=%u task=%u pri=%d "
                 "primary=%u ribbon=%u)\n",
                 i, g.line_index, static_cast<long long>(g.instance_start_ms), g.layer, g.sys,
                 g.birth, g.job_i, g.task_i, g.priority, g.primary, g.ribbon, e.line_index,
                 static_cast<long long>(e.instance_start_ms), e.layer, e.sys, e.birth, e.job_i,
                 e.task_i, e.priority, e.primary, e.ribbon);
    over_cap_bucket_verify_dump();
    std::abort();
  }
}
#endif  // WDS_DIAG_ENABLED

inline void over_cap_sort_under_cap(std::vector<OverCapCandidate>& candidates) {
#if WDS_DIAG_ENABLED
  const bool verify = over_cap_bucket_verify_env();
  std::vector<OverCapCandidate> expected;
  if (verify) {
    over_cap_bucket_verify_register();
    ++over_cap_verify_under_cap();
    expected = candidates;
    if (!over_cap_already_sorted(expected)) {
      std::sort(expected.begin(), expected.end(), over_cap_identity_lt);
    }
  }
#endif
  if (over_cap_already_sorted(candidates)) {
#if WDS_DIAG_ENABLED
    if (verify) {
      ++over_cap_verify_already_sorted();
      over_cap_bucket_verify_check(candidates, expected);
    }
#endif
    return;
  }
  if (!over_cap_bucket_sort(candidates)) {
    std::sort(candidates.begin(), candidates.end(), over_cap_identity_lt);
#if WDS_DIAG_ENABLED
    if (verify) {
      ++over_cap_verify_fallback();
    }
#endif
  } else {
#if WDS_DIAG_ENABLED
    if (verify) {
      ++over_cap_verify_bucket_calls();
    }
#endif
  }
#if WDS_DIAG_ENABLED
  if (verify) {
    over_cap_bucket_verify_check(candidates, expected);
  }
#endif
}

inline void apply_over_cap_select(std::vector<OverCapCandidate>& candidates, int cap,
                                  bool legacy = false) {
  if (cap <= 0) {
    candidates.clear();
    return;
  }
  if (static_cast<int>(candidates.size()) <= cap) {
    if (!legacy) {
      over_cap_sort_under_cap(candidates);
    }
    return;
  }

  if (legacy) {
    std::sort(candidates.begin(), candidates.end(),
              [](const OverCapCandidate& a, const OverCapCandidate& b) {
                if (a.line_index != b.line_index) {
                  return a.line_index < b.line_index;
                }
                if (a.instance_start_ms != b.instance_start_ms) {
                  return a.instance_start_ms < b.instance_start_ms;
                }
                if (a.layer != b.layer) {
                  return a.layer < b.layer;
                }
                return a.priority < b.priority;
              });
    int line_count = 1;
    for (size_t i = 1; i < candidates.size(); ++i) {
      if (!over_cap_same_line(candidates[i], candidates[i - 1])) {
        ++line_count;
      }
    }
    const int per_line = std::max(1, cap / std::max(1, line_count));
    std::vector<OverCapCandidate> kept;
    kept.reserve(static_cast<size_t>(cap));
    std::vector<size_t> head;
    std::vector<size_t> lim;
    size_t i = 0;
    while (i < candidates.size() && static_cast<int>(kept.size()) < cap) {
      const OverCapCandidate line_head = candidates[i];
      size_t line_end = i + 1;
      while (line_end < candidates.size() && over_cap_same_line(candidates[line_end], line_head)) {
        ++line_end;
      }
      head.clear();
      lim.clear();
      size_t run = i;
      while (run < line_end) {
        size_t run_end = run + 1;
        while (run_end < line_end && candidates[run_end].layer == candidates[run].layer) {
          ++run_end;
        }
        head.push_back(run);
        lim.push_back(run_end);
        run = run_end;
      }
      int taken = 0;
      bool progress = true;
      while (taken < per_line && progress && static_cast<int>(kept.size()) < cap) {
        progress = false;
        for (size_t layer = 0; layer < head.size(); ++layer) {
          if (head[layer] >= lim[layer] || taken >= per_line ||
              static_cast<int>(kept.size()) >= cap) {
            continue;
          }
          kept.push_back(candidates[head[layer]++]);
          ++taken;
          progress = true;
        }
      }
      i = line_end;
    }
    candidates.swap(kept);
    return;
  }

  if (!over_cap_already_sorted(candidates)) {
    std::sort(candidates.begin(), candidates.end(), over_cap_identity_lt);
  }

  int line_count = 1;
  for (size_t i = 1; i < candidates.size(); ++i) {
    if (!over_cap_same_line(candidates[i], candidates[i - 1])) {
      ++line_count;
    }
  }
  const int per_line = std::max(1, cap / std::max(1, line_count));
  std::vector<OverCapCandidate> kept;
  kept.reserve(static_cast<size_t>(cap));

  size_t i = 0;
  while (i < candidates.size() && static_cast<int>(kept.size()) < cap) {
    const OverCapCandidate line_head = candidates[i];
    size_t line_end = i + 1;
    while (line_end < candidates.size() && over_cap_same_line(candidates[line_end], line_head)) {
      ++line_end;
    }

    struct Group {
      size_t begin = 0;
      size_t end = 0;
      uint8_t ribbon = 0;
      int quota = 0;
    };
    std::vector<Group> groups;
    size_t run = i;
    while (run < line_end) {
      Group g;
      g.begin = run;
      g.ribbon = candidates[run].ribbon;
      size_t run_end = run + 1;
      while (run_end < line_end && candidates[run_end].layer == candidates[run].layer &&
             candidates[run_end].sys == candidates[run].sys) {
        if (candidates[run_end].ribbon != 0) {
          g.ribbon = 1;
        }
        ++run_end;
      }
      g.end = run_end;
      groups.push_back(g);
      run = run_end;
    }

    const int room = cap - static_cast<int>(kept.size());
    const int line_budget = std::min(per_line, room);
    int taken = 0;
    bool progress = true;
    while (taken < line_budget && progress) {
      progress = false;
      for (Group& g : groups) {
        const int avail = static_cast<int>(g.end - g.begin) - g.quota;
        if (avail <= 0 || taken >= line_budget) {
          continue;
        }
        ++g.quota;
        ++taken;
        progress = true;
      }
    }

    for (const Group& g : groups) {
      if (g.quota <= 0) {
        continue;
      }
      const int n = static_cast<int>(g.end - g.begin);
      const int k = std::min(g.quota, n);
      // Ribbon: newest/head contiguous run so stitch has no holes.
      // stitch t=0 is the oldest living of that run (align2-n6 hop).
      // Non-ribbon: oldest.
      const size_t start = g.ribbon != 0 ? g.end - static_cast<size_t>(k) : g.begin;
      for (int j = 0; j < k; ++j) {
        kept.push_back(candidates[start + static_cast<size_t>(j)]);
      }
    }
    i = line_end;
  }
  candidates.swap(kept);
}

}  // namespace wds::chart_render
