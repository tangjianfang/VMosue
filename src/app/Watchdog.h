#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace vmosue {

// Watchdog monitors a set of registered worker threads and fires a
// user-supplied callback if any of them goes too long without calling
// Heartbeat(). The typical use is:
//
//   auto& wd = Watchdog::Get();
//   wd.RegisterThread(std::this_thread::get_id(), "capture", 5s);
//   wd.Start([](const std::string& name) { ... });
//
//   while (running) {
//     work();
//     wd.Heartbeat(std::this_thread::get_id());   // throttled internally
//   }
//
// Design notes:
//
//  - Singleton: there is one process-wide Watchdog. Mirrors the
//    Config::Get() Meyers-singleton pattern. The constructor is
//    public-but-marked-internal: tests may instantiate directly to
//    keep their state isolated; production code should always go
//    through Get().
//
//  - Heartbeat throttling: Heartbeat() itself enforces a minimum
//    interval of one second per thread. Worker loops that run at
//    30+ Hz can call Heartbeat() on every iteration without
//    worrying about lock contention — only the first call per
//    second actually takes the map mutex.
//
//  - Timeout firing: when the check loop sees (now - lastBeat) >
//    timeout for some thread, it invokes the callback with the
//    thread's name and then resets lastBeat = now. This means the
//    callback fires AT MOST once per `timeout` period, even if the
//    offending thread never recovers. Without the reset the
//    callback would re-fire every check (up to once per second),
//    which is rarely what the caller wants.
//
//  - Start/Stop are idempotent: a second Start() is a no-op;
//    Stop() when not running is a no-op. This makes the lifecycle
//    easy to wire into App::Run/Shutdown without worrying about
//    double-start paths (e.g. signal handler + destructor).
class Watchdog {
 public:
  using Clock = std::chrono::steady_clock;
  using TimeoutCb = std::function<void(const std::string&)>;

  // Meyers singleton. Function-local static is thread-safe in
  // C++11+ (see [stmt.dcl]/4).
  static Watchdog& Get();

  // Register a thread with a human-readable name and a heartbeat
  // timeout. If `lastBeat` is unset (zero time_point) it is
  // initialized to "now" on entry so a thread that registers but
  // never heartbeats will only time out after `timeout` elapses
  // rather than firing immediately. Re-registering an existing id
  // overwrites the previous name and timeout.
  void RegisterThread(std::thread::id id, std::string name,
                      std::chrono::seconds timeout);

  // Remove a thread from the watchdog. No-op if the id is not
  // registered. Safe to call from the thread being unregistered
  // (e.g. as the last action before it exits its loop).
  void UnregisterThread(std::thread::id id);

  // Record a heartbeat for the given thread. Internally throttled
  // to at most one update per second per thread — see class
  // comment. Unknown ids are silently ignored so a thread that
  // races Start() can heartbeat without crashing.
  void Heartbeat(std::thread::id id);

  // Begin monitoring. `onTimeout` is invoked with the registered
  // thread name whenever a thread misses its heartbeat for longer
  // than its registered timeout. The callback runs on the watcher
  // thread; it must not call back into Watchdog (would deadlock on
  // mu_) and should not block for long (would stall other
  // timeouts). Idempotent: a second Start() is a no-op.
  void Start(TimeoutCb onTimeout);

  // Signal the watcher thread to stop and join it. Idempotent:
  // calling Stop() when not running is a no-op, and calling
  // Stop() twice is safe.
  void Stop();

 private:
  Watchdog() = default;

  // Watcher thread body: sleeps ~1s, then under mu_ collects the
  // list of timed-out threads and (still under mu_) updates their
  // lastBeat to suppress re-fires. The callback is fired AFTER
  // releasing mu_ to avoid deadlock if the callback ever calls
  // back into Watchdog.
  void run();

  struct Info {
    std::string name;
    Clock::time_point lastBeat;     // guarded by mu_
    Clock::time_point lastBeatSeen; // throttle stamp, guarded by mu_
    std::chrono::seconds timeout;
  };

  std::mutex mu_;
  std::unordered_map<std::thread::id, Info> threads_;

  std::atomic<bool> running_{false};
  std::thread t_;
  TimeoutCb onTimeout_;
};

}  // namespace vmosue