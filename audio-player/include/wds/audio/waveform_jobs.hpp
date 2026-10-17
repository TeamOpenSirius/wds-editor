#pragma once

#include "waveform_overview.hpp"

#include <wds/common/crash_handler.hpp>
#include <wds/common/log.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace wds::audio {

// Background WaveformOverview decodes with generation-based staleness.
//
// Control (GUI) thread only. The one rule it exists to enforce: the control
// thread never waits on a decode. A song switch calls cancel_all()/restart(),
// which only flips atomics; the cancelled job keeps running until it notices
// (the flag is checked before and after the stream open and per decode chunk),
// stays in `jobs_` as the "retired" list, and is reaped by poll() once its
// future is ready. Only shutdown() joins, and callers must run it before
// anything that ends in BASS_Free(): a decode owns a BASS stream handle that
// must not outlive the device.
//
// Results from a superseded generation are dropped in poll(), never returned.
class WaveformJobs {
 public:
  WaveformJobs() = default;
  ~WaveformJobs() { shutdown(); }

  WaveformJobs(const WaveformJobs&) = delete;
  WaveformJobs& operator=(const WaveformJobs&) = delete;

  // Cancel everything in flight (non-blocking) and, for a non-empty path, start
  // a new decode. Returns the generation of the new job (or the bumped
  // generation when `path` is empty).
  std::uint64_t restart(const std::string& path) {
    cancel_all();
    if (path.empty()) {
      return generation_;
    }
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    jobs_.push_back(Job{generation_, cancel,
                        std::async(std::launch::async, [path, cancel] {
                          wds::common::install_thread_crash_stack();
                          WaveformOverview decoded;
                          if (!decoded.load(path, cancel.get())) {
                            if (!cancel->load(std::memory_order_relaxed)) {
                              WDS_LOG("WaveformJobs: decode failed %s\n", path.c_str());
                            }
                            decoded.clear();
                          }
                          return decoded;
                        })});
    return generation_;
  }

  // Invalidate every job: results are discarded, workers wind down on their own.
  // Never blocks.
  void cancel_all() noexcept {
    ++generation_;
    for (Job& job : jobs_) {
      job.cancel->store(true, std::memory_order_relaxed);
    }
  }

  // Reap finished jobs without blocking. True when `out` received the result of
  // a still-current, non-empty decode (the newest one if several finished).
  bool poll(WaveformOverview& out) {
    using namespace std::chrono_literals;
    bool got = false;
    for (auto it = jobs_.begin(); it != jobs_.end();) {
      if (!it->result.valid() || it->result.wait_for(0ms) != std::future_status::ready) {
        ++it;
        continue;
      }
      WaveformOverview decoded;
      try {
        decoded = it->result.get();
      } catch (...) {
        WDS_LOG("WaveformJobs: worker failed\n");
        decoded.clear();
      }
      const bool cancelled = it->cancel->load(std::memory_order_relaxed);
      const bool current = it->generation == generation_;
      it = jobs_.erase(it);
      if (cancelled || !current || decoded.empty()) {
        continue;
      }
      out = std::move(decoded);
      got = true;
    }
    return got;
  }

  // Cancel and join everything. Blocking: bounded by one uninterruptible stream
  // open plus one decode chunk per job, so call it only where the device is about
  // to be torn down (BASS_Free) or the owner is shutting down.
  void shutdown() {
    cancel_all();
    for (Job& job : jobs_) {
      if (job.result.valid()) {
        try {
          (void)job.result.get();
        } catch (...) {
          WDS_LOG("WaveformJobs: worker failed during shutdown\n");
        }
      }
    }
    jobs_.clear();
  }

  // Jobs not yet reaped (running, retired-but-still-running, or finished unpolled).
  std::size_t in_flight() const noexcept { return jobs_.size(); }
  std::uint64_t generation() const noexcept { return generation_; }

 private:
  struct Job {
    std::uint64_t generation = 0;
    std::shared_ptr<std::atomic<bool>> cancel;
    std::future<WaveformOverview> result;
  };

  std::vector<Job> jobs_;
  std::uint64_t generation_ = 0;
};

}  // namespace wds::audio
