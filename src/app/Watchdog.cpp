#include "app/Watchdog.h"

#include <vector>

namespace vmosue {

Watchdog& Watchdog::Get() {
  // Meyers singleton: function-local static initialization is
  // thread-safe in C++11+ ([stmt.dcl]/4). Construction is
  // deliberately trivial so we never pay for I/O here.
  static Watchdog instance;
  return instance;
}

void Watchdog::RegisterThread(std::thread::id id, std::string name,
                              std::chrono::seconds timeout) {
  std::lock_guard<std::mutex> lock(mu_);
  auto now = Clock::now();
  Info info;
  info.name = std::move(name);
  // lastBeat starts at "now" so a thread that registers but never
  // heartbeats only fires after `timeout` elapses, not immediately.
  info.lastBeat = now;
  // lastBeatSeen starts at the epoch (zero) so the very first
  // Heartbeat() is always accepted as "no previous beat", rather
  // than being throttled out by the registration time.
  info.lastBeatSeen = Clock::time_point{};
  info.timeout = timeout;
  threads_[id] = std::move(info);
}

void Watchdog::UnregisterThread(std::thread::id id) {
  std::lock_guard<std::mutex> lock(mu_);
  threads_.erase(id);
}

void Watchdog::Heartbeat(std::thread::id id) {
  // Throttle: take the mutex, decide if this call is a no-op based
  // on the per-thread lastBeatSeen timestamp, update lastBeat if it
  // is not. Workers that loop at 30+ Hz call Heartbeat on every
  // iteration; only the first call per second actually mutates
  // lastBeat. The throttle stamp lives in the same struct as
  // lastBeat so we can update both under the same lock.
  std::lock_guard<std::mutex> lock(mu_);
  auto it = threads_.find(id);
  if (it == threads_.end()) return;
  auto now = Clock::now();
  // Throttle window: 1 second. If less than 1s has elapsed since
  // the last accepted heartbeat, treat this one as a no-op.
  if (now - it->second.lastBeatSeen < std::chrono::seconds(1)) {
    return;
  }
  it->second.lastBeatSeen = now;
  it->second.lastBeat = now;
}

void Watchdog::Start(TimeoutCb onTimeout) {
  // Idempotent. Only the first Start() spawns the watcher thread
  // and installs the callback. Subsequent Start() calls (e.g. if
  // App::Run is somehow entered twice) are no-ops.
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) return;

  {
    std::lock_guard<std::mutex> lock(mu_);
    onTimeout_ = std::move(onTimeout);
  }
  t_ = std::thread([this]() { this->run(); });
}

void Watchdog::Stop() {
  // Idempotent. The CAS ensures only one caller flips running_ and
  // joins the watcher thread; concurrent callers (e.g. signal
  // handler + destructor) will see false and early-out.
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false)) return;

  // The watcher loop checks running_ once per second, so Stop()
  // returns within ~1s in the worst case. join() is what actually
  // serializes the teardown here.
  if (t_.joinable()) t_.join();

  // Drop the callback so any captured state is released. Done
  // after join() so the watcher can never be in the middle of
  // invoking the callback when we destroy it.
  std::lock_guard<std::mutex> lock(mu_);
  onTimeout_ = nullptr;
}

void Watchdog::run() {
  // 1Hz check loop. We sleep in 200ms slices so Stop() takes at
  // most ~200ms to take effect — the full 1s budget is unnecessary
  // because Stop() is itself idempotent and only one caller is
  // allowed to flip running_ to false.
  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (!running_.load()) break;

    // Phase 1: under mu_, collect the names of timed-out threads
    // and reset their lastBeat so we don't re-fire on the next
    // iteration. Holding the mutex here is fine because the work
    // is O(num_threads) and the map is small.
    std::vector<std::string> timedOut;
    TimeoutCb cb;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!onTimeout_) continue;  // Start() not called yet
      cb = onTimeout_;
      auto now = Clock::now();
      for (auto& kv : threads_) {
        auto& info = kv.second;
        if (now - info.lastBeat > info.timeout) {
          timedOut.push_back(info.name);
          // Reset lastBeat so the callback fires at most once per
          // timeout period. Without this reset the callback would
          // re-fire every check tick (every ~200ms in this loop)
          // for as long as the thread stays unhealthy — typically
          // not what callers want and a log-flood risk.
          info.lastBeat = now;
        }
      }
    }

    // Phase 2: fire the callback OUTSIDE the mutex. If the
    // callback ever calls back into Watchdog (e.g. logs and
    // touches some shared state) it must not deadlock waiting for
    // mu_. The std::function copy was made under the lock so we
    // don't depend on onTimeout_ still being installed here.
    for (const auto& name : timedOut) {
      cb(name);
    }
  }
}

}  // namespace vmosue