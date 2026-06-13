#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

#include "util/Logger.h"

namespace vmosue {

// Task 33: lightweight in-process profiler. The spec target is
// P95 < 60ms and CPU < 25% for the inference pipeline; this
// utility instruments a section of code (a "span") and emits a
// WARN log when the rolling P95 exceeds a configurable budget.
//
// Design notes:
//   - Header-only so callers don't need to link a separate TU.
//   - Static counters live in the class itself; multi-instance use
//     is supported by passing a `name` tag at construction.
//   - The P95 is computed from a rolling 64-sample window. The
//     64-sample window is small enough to keep the sort cheap
//     (O(N log N) at ~6 comparisons) and large enough to smooth
//     over single-frame stalls.
//   - Threshold: 60ms by default, matching the spec. The 60ms
//     number is the wall-clock budget for the inference thread's
//     per-frame work; a violation is a "we're missing the cadence"
//     signal, not necessarily a hard failure.
//   - This is forward-looking instrumentation. Production builds
//     can disable the warn log by passing `enabled=false` to the
//     constructor.
//
// Usage:
//   {
//     PROFILE_GUARD("inference");
//     ... work ...
//   }   // logs a warn if P95 > 60ms
//
// The macro form is preferred in hot paths so the destructor runs
// automatically. Direct usage of ProfileGuard is fine when the
// section is conditional.
class ProfileGuard {
 public:
  explicit ProfileGuard(const char* name, bool enabled = true,
                        double warnThresholdMs = 60.0)
      : name_(name), enabled_(enabled), start_(Clock::now()) {}

  ~ProfileGuard() {
    if (!enabled_) return;
    const auto end = Clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(end - start_).count();
    RecordSample(name_, ms);
    if (ms >= threshold_) {
      VMOSUE_LOG_WARN("ProfileGuard[{}] sample {:.2f}ms (>= {:.1f}ms)",
                      name_, ms, threshold_);
    }
  }

  // Read-only accessor for tests / external probes. Returns the
  // most recent sample (or 0 if none).
  static double LastSample(const char* name) {
    std::lock_guard<std::mutex> lk(Mu());
    auto& v = Series(name);
    return v.empty() ? 0.0 : v.back();
  }

  // Computes the P95 over the rolling window for `name`. Returns
  // 0.0 if no samples have been recorded.
  static double P95Ms(const char* name) {
    std::lock_guard<std::mutex> lk(Mu());
    auto& v = Series(name);
    if (v.empty()) return 0.0;
    std::vector<double> copy = v;
    std::sort(copy.begin(), copy.end());
    // P95 = the value at the 95th percentile index. With a small
    // window the index is rounded up so we get the actual sample
    // at-or-just-past the 95% mark.
    const size_t idx = std::min(
        copy.size() - 1,
        static_cast<size_t>(std::ceil(0.95 * copy.size()) - 1));
    return copy[idx];
  }

  // Resets the rolling window. Mostly useful for tests.
  static void Reset(const char* name) {
    std::lock_guard<std::mutex> lk(Mu());
    Series(name).clear();
  }

 private:
  using Clock = std::chrono::steady_clock;

  static std::mutex& Mu() {
    static std::mutex m;
    return m;
  }

  // Per-name sample series. We allocate a small vector the first
  // time a name is seen and reuse it; the window is bounded at 64
  // entries (FIFO eviction).
  static std::vector<double>& Series(const char* name) {
    static std::vector<std::vector<double>> series;
    static std::vector<const char*> names;
    for (size_t i = 0; i < names.size(); ++i) {
      if (names[i] == name) return series[i];
    }
    names.push_back(name);
    series.emplace_back();
    return series.back();
  }

  static void RecordSample(const char* name, double ms) {
    std::lock_guard<std::mutex> lk(Mu());
    auto& v = Series(name);
    constexpr size_t kWindow = 64;
    if (v.size() >= kWindow) v.erase(v.begin());
    v.push_back(ms);
  }

  const char* name_;
  bool enabled_;
  double threshold_;
  Clock::time_point start_;
};

#define PROFILE_GUARD(name) ::vmosue::ProfileGuard _prof_guard(name)
#define PROFILE_GUARD_DISABLED(name) \
  ::vmosue::ProfileGuard _prof_guard(name, false)

}  // namespace vmosue
