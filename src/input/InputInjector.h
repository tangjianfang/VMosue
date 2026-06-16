#pragma once
// InputInjector - synthesizes OS-level mouse and keyboard input on Windows
// via the SendInput() Win32 API.
//
// Task 12 (replaces the Task 9 stub). The API surface here is the canonical
// one consumed by the gesture module (CursorController, GestureStateMachine,
// AirClickDetector, etc.) and the emergency-stop / hotkey layers.
//
// Design notes:
// - Process-wide singleton (Get()) so the gesture loop and any
//   emergency-stop / watchdog code share one state machine. The atomic
//   leftDown_ flag is the canonical "is the left mouse button logically
//   held" state; SafeReleaseAll() is the single place that can
//   unconditionally force-release it (used on shutdown, emergency stop,
//   and pause).
// - SendInput() requires the calling thread to be in a desktop session
//   that "knows" about the user input state. When run from a service or
//   non-interactive session SendInput returns 0. We log and continue.
// - This header deliberately does NOT include <windows.h>. The public
//   surface uses only standard types (int, bool) so callers do not pull
//   in the Windows SDK header. The .cpp file is the only place that
//   needs the Win32 API.

#include <atomic>

namespace vmosue {

class InputInjector {
 public:
  static InputInjector& Get();

  // Move the system cursor by (dx, dy) pixels (relative). No-op when both
  // are zero so we don't issue spurious SendInput calls.
  void MoveCursor(int dx, int dy);

  // Maximum per-call pixel delta applied by MoveCursor. Exposed as a
  // constant so tests can assert the bound without re-deriving it.
  // Defense-in-depth: a noisy first frame or a future bug in the
  // gesture pipeline that skips the dead-zone could otherwise yank
  // the OS cursor by thousands of pixels and freeze the user's
  // session. 500px is generous for a normal flick (~15k px/sec at
  // 30Hz) but small enough that a runaway value of e.g. 5000 gets
  // capped before reaching SendInput.
  static constexpr int kMaxMovePerCall = 500;

  // Pure helper: clamp (dx, dy) to the per-call bound. Exposed so
  // unit tests can verify the clamping logic without depending on
  // SendInput (which returns 0 in headless sessions).
  static void ClampDelta(int& dx, int& dy);

  // Atomic down/up of the primary (left) mouse button. Down is a no-op
  // when the button is already logically held; Up is a no-op when it is
  // not held.
  void LeftDown();
  void LeftUp();

  // Convenience: LeftDown + LeftUp. Always leaves the button released.
  void LeftClick();

  // Right-click is a discrete gesture (no state tracking) so we just emit
  // down + up unconditionally.
  void RightClick();

  // Vertical scroll. Positive delta scrolls away from the user (Windows
  // convention: WHEEL_DELTA = 120 is one notch).
  void Wheel(int delta);

  // Unconditionally release any logically-held buttons and any modifier
  // keys (shift, ctrl, alt) we may have left behind. Safe to call from
  // emergency stop, pause, and process shutdown paths. Idempotent.
  void SafeReleaseAll();

  bool IsLeftDown() const { return leftDown_.load(); }

 private:
  InputInjector();

  std::atomic<bool> leftDown_{false};
};

}  // namespace vmosue