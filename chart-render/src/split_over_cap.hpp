#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace wds::chart_render {

// Preview-pool hop, not Unity 2022.3 maxParticles. Between splits the caller
// still walks remaining budget first-come. Within one split: per-line share,
// then round-robin *counts* across (layer, sys) groups. Within a group:
// birth order (job_i / birth_index tie-break). Ribbon groups keep one
// contiguous run at the newest/head end so stitch has no holes and budget
// changes only move the oldest cut.
struct OverCapCandidate {
  int priority = 0;
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

inline bool over_cap_identity_lt(const OverCapCandidate& a, const OverCapCandidate& b) noexcept {
  if (a.line_index != b.line_index) {
    return a.line_index < b.line_index;
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

inline void apply_over_cap_select(std::vector<OverCapCandidate>& candidates, int cap,
                                  bool legacy = false) {
  if (cap <= 0) {
    candidates.clear();
    return;
  }
  if (static_cast<int>(candidates.size()) <= cap) {
    if (!legacy && !over_cap_already_sorted(candidates)) {
      std::sort(candidates.begin(), candidates.end(), over_cap_identity_lt);
    }
    return;
  }

  if (legacy) {
    std::sort(candidates.begin(), candidates.end(),
              [](const OverCapCandidate& a, const OverCapCandidate& b) {
                if (a.line_index != b.line_index) {
                  return a.line_index < b.line_index;
                }
                if (a.layer != b.layer) {
                  return a.layer < b.layer;
                }
                return a.priority < b.priority;
              });
    int line_count = 1;
    for (size_t i = 1; i < candidates.size(); ++i) {
      if (candidates[i].line_index != candidates[i - 1].line_index) {
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
      const int32_t line = candidates[i].line_index;
      size_t line_end = i + 1;
      while (line_end < candidates.size() && candidates[line_end].line_index == line) {
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
    if (candidates[i].line_index != candidates[i - 1].line_index) {
      ++line_count;
    }
  }
  const int per_line = std::max(1, cap / std::max(1, line_count));
  std::vector<OverCapCandidate> kept;
  kept.reserve(static_cast<size_t>(cap));

  size_t i = 0;
  while (i < candidates.size() && static_cast<int>(kept.size()) < cap) {
    const int32_t line = candidates[i].line_index;
    size_t line_end = i + 1;
    while (line_end < candidates.size() && candidates[line_end].line_index == line) {
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
