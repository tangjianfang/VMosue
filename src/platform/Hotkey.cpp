#include "platform/Hotkey.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>

#include "util/Logger.h"

// Windows-only: we use GetAsyncKeyState to poll the global keyboard state
// regardless of which application is focused. The API is foreground-only
// (Service / Session 0 will not see these keys), which is acceptable for
// a v1.0 desktop product.
#ifdef _WIN32
#include <windows.h>
#endif

namespace vmosue {

namespace {

// ----- Singleton watcher state -----
// We use file-scope statics so the API can stay static-only (no instance
// to manage). All access is guarded by `mu_`. The watcher thread runs
// while `running_` is true; it self-terminates when both callbacks are
// cleared (so an Unregister on the last active hotkey stops the thread).

std::mutex mu_;
std::optional<std::function<void()>> g_ctrlAltG_;
std::optional<std::function<void()>> g_esc_;
int g_escHoldMs_ = 1000;
std::atomic<bool> running_{false};
std::thread watcher_;
// Per-hotkey re-arm latches. Each hotkey arms/disarms itself on its own
// key transition; sharing one latch across both would let, e.g., an
// Esc-hold silently disarm Ctrl+Alt+G (or vice versa) until the other
// key cycled, which is confusing for the user.
std::atomic<bool> gCtrlAltGArmed_{true};
std::atomic<bool> gEscArmed_{true};

bool any_registered_locked() {
  return g_ctrlAltG_.has_value() || g_esc_.has_value();
}

void start_watcher_locked() {
  if (running_.exchange(true)) return;
  gCtrlAltGArmed_.store(true);
  gEscArmed_.store(true);
  watcher_ = std::thread([]() {
    using namespace std::chrono;
    // Lambda-local edge-detection state. We do not use static-by-capture;
    // these are per-thread (per watcher instance) and reset every time
    // the watcher starts.
    bool gPrevDown = false;
    bool escPrevDown = false;
    auto escDownSince = steady_clock::time_point{};

    while (running_.load()) {
      // Snapshot the callbacks under the lock so a concurrent
      // Unregister+Register pair can't race with us reading them.
      std::optional<std::function<void()>> cbG;
      std::optional<std::function<void()>> cbEsc;
      int escHoldMs = 1000;
      {
        std::lock_guard<std::mutex> lk(mu_);
        cbG = g_ctrlAltG_;
        cbEsc = g_esc_;
        escHoldMs = g_escHoldMs_;
      }

      bool gArmed = gCtrlAltGArmed_.load();
      bool escArmed = gEscArmed_.load();

#ifdef _WIN32
      // Ctrl and Alt: GetAsyncKeyState returns the high bit set if the
      // key is currently down. VK_CONTROL covers both LCONTROL and
      // RCONTROL because the OS reports either as VK_CONTROL.
      bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
      bool altDown  = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
      bool gDown    = (GetAsyncKeyState('G')        & 0x8000) != 0;
      bool escDown  = (GetAsyncKeyState(VK_ESCAPE)  & 0x8000) != 0;

      // Ctrl+Alt+G: fire when all three are down AND we're armed.
      // After firing, latch `gArmed` low until G is released.
      if (cbG && ctrlDown && altDown && gDown) {
        if (gArmed) {
          gCtrlAltGArmed_.store(false);
          try {
            (*cbG)();
          } catch (...) {
            VMOSUE_LOG_ERROR("Ctrl+Alt+G callback threw");
          }
        }
      }
      // Re-arm when G goes from down -> up.
      if (gPrevDown && !gDown) {
        gCtrlAltGArmed_.store(true);
      }
      gPrevDown = gDown;

      // Esc long-press: track time the key has been continuously down.
      // Fire when (now - downStart) >= holdMs. Re-arm on release.
      if (cbEsc) {
        if (escDown && !escPrevDown) {
          escDownSince = steady_clock::now();
        } else if (escDown && escPrevDown) {
          auto held = duration_cast<milliseconds>(
                          steady_clock::now() - escDownSince).count();
          if (escArmed && held >= escHoldMs) {
            gEscArmed_.store(false);
            try {
              (*cbEsc)();
            } catch (...) {
              VMOSUE_LOG_ERROR("Esc callback threw");
            }
          }
        } else if (!escDown && escPrevDown) {
          // Released: re-arm for next press.
          gEscArmed_.store(true);
          escDownSince = {};
        }
        escPrevDown = escDown;
      }
#else
      // Non-Windows parse-check path: do nothing.
      (void)gPrevDown;
      (void)escPrevDown;
      (void)escDownSince;
      (void)ctrlDown;
      (void)altDown;
      (void)gDown;
      (void)escDown;
      (void)gArmed;
      (void)escArmed;
      (void)escHoldMs;
#endif

      // Sleep 50 ms. GetAsyncKeyState is cheap; this just keeps the
      // thread idle. We don't try to be precise about hold timing --
      // 50 ms slop on a 1000 ms hold is invisible to the user.
      std::this_thread::sleep_for(milliseconds(50));
    }
  });
}

void stop_watcher_locked() {
  if (!running_.exchange(false)) return;
  if (watcher_.joinable()) watcher_.join();
}

}  // namespace

bool Hotkey::RegisterCtrlAltG(std::function<void()> onTrigger) {
  if (!onTrigger) return false;
  std::lock_guard<std::mutex> lk(mu_);
  g_ctrlAltG_ = std::move(onTrigger);
  start_watcher_locked();
  return true;
}

void Hotkey::UnregisterCtrlAltG() {
  std::lock_guard<std::mutex> lk(mu_);
  g_ctrlAltG_.reset();
  // Re-arm latch is per-process; leave it as-is. If Esc is still
  // registered the watcher must keep running anyway.
  if (!any_registered_locked()) {
    stop_watcher_locked();
    gCtrlAltGArmed_.store(true);
    gEscArmed_.store(true);
  }
}

bool Hotkey::RegisterEsc(std::function<void()> onTrigger, int holdMs) {
  if (!onTrigger) return false;
  if (holdMs < 0) holdMs = 0;
  std::lock_guard<std::mutex> lk(mu_);
  g_esc_ = std::move(onTrigger);
  g_escHoldMs_ = holdMs;
  start_watcher_locked();
  return true;
}

void Hotkey::UnregisterEsc() {
  std::lock_guard<std::mutex> lk(mu_);
  g_esc_.reset();
  if (!any_registered_locked()) {
    stop_watcher_locked();
    gCtrlAltGArmed_.store(true);
    gEscArmed_.store(true);
  }
}

void Hotkey::Shutdown() {
  std::lock_guard<std::mutex> lk(mu_);
  g_ctrlAltG_.reset();
  g_esc_.reset();
  stop_watcher_locked();
  gCtrlAltGArmed_.store(true);
  gEscArmed_.store(true);
}

}  // namespace vmosue