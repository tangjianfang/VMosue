#pragma once
#include <functional>

namespace vmosue {

// Global hotkey watcher used to trigger the EmergencyStop from outside the
// camera/gesture pipeline. A single background watcher thread polls the
// Windows global key state at ~50 ms and fires the registered callback when
// the requested chord/long-press is observed.
//
// Two hotkeys are supported:
//   - Ctrl+Alt+G: classic hotkey. After firing, re-arms once G is released
//     so a user holding the chord for several seconds does not produce
//     dozens of callbacks.
//   - Esc long-press: fires once Esc has been continuously held for
//     `holdMs` milliseconds. Also re-arms on release.
//
// The watcher is a process-wide singleton (file-scope statics). The first
// successful Register* call starts the thread; Unregister* on both slots
// stops it. The thread uses a coarse 50 ms sleep, which is plenty for a
// user-driven hotkey and keeps the CPU near zero.
class Hotkey {
 public:
  // Register the Ctrl+Alt+G hotkey. Returns true on success.
  // Re-registering replaces the previous callback.
  static bool RegisterCtrlAltG(std::function<void()> onTrigger);

  // Clear the Ctrl+Alt+G registration. Safe to call when not registered.
  static void UnregisterCtrlAltG();

  // Register an Esc long-press hotkey. The callback fires once Esc has
  // been held continuously for `holdMs` milliseconds. Re-registering
  // replaces the previous callback and hold duration.
  static bool RegisterEsc(std::function<void()> onTrigger, int holdMs);

  // Clear the Esc registration. Safe to call when not registered.
  static void UnregisterEsc();

  // v0.6: F1 help hotkey. Fires on every F1 down-edge (latch pattern,
  // same as Ctrl+Alt+G). Used to toggle the ActionListWindow.
  static bool RegisterF1(std::function<void()> onTrigger);

  static void UnregisterF1();

  // Stop the watcher thread and clear all registrations. Used by tests
  // and the App shutdown path to avoid leaks across process restarts in
  // the same address space.
  static void Shutdown();
};

}  // namespace vmosue