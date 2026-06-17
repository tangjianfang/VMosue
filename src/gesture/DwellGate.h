#pragma once
// v0.6: per-action dwell-time calibration.
//
// User's complaint: "所有的动作执行之前都要有一个校准的过程，比如说
// 检测到手型 2 秒或 3 秒以上，开始执行动作". A click that fires the
// instant the threshold crosses means the user has no chance to abort
// a misfire — by the time they realize it was a wrong gesture, the
// click is already on the screen. The DwellGate is a thin layer that
// wraps the merged ActionSet coming out of GestureStateMachine's
// arbitration, and only forwards an action once it has been
// continuously asserted for `dwellMs`.
//
// Why a state-machine-level gate (not per-detector)?
//
// 1. The arbitration in GestureStateMachine already picks a single
//    "winning" action per frame (left-wins-right, etc.). The
//    DwellGate just gates the winner.
// 2. Detectors stay pure — their unit tests still expect threshold
//    crossings to fire immediately. To preserve that contract the
//    DwellGate has a `dwellMs = 0` configuration that bypasses the
//    gate entirely.
// 3. The dwell preview (the "About to: Left click 1.2s" overlay)
//    needs a *stable* view of "what is currently counting down",
//    which the gesture-state-machine level can provide without
//    teaching every detector about it.
//
// What passes through unchanged:
//
//   - cursorX / cursorY  — continuous, no dwell
//   - leftDown / leftUp  — drag's begin/end markers; these are
//                           "sustained" actions that already
//                           advertise their intent via the pinch
//                           hold
//   - wheel / hWheel     — continuous scroll
//   - safeRelease        — emergency signal, must never be delayed
//
// What is gated:
//
//   - leftClick          — thumb-index pinch
//   - leftDoubleClick    — two pinches within the OS double-click
//                           window
//   - rightClick         — push forward toward camera
//   - middleClick        — thumb-middle pinch
//
// `pause` (open-hand toggle) already has its own 1s hold inside
// PauseDetector and is NOT gated here. Adding a second dwell on top
// would push the user-facing pause latency to ~3s.

#include "gesture/ActionSet.h"
#include <array>
#include <cstdint>
#include <optional>

namespace vmosue {

class DwellGate {
 public:
  enum class Kind : int {
    None = 0,
    LeftClick = 1,
    RightClick = 2,
    MiddleClick = 3,
    DoubleClick = 4,
    kKindCount
  };

  struct Config {
    // How long an action must be continuously asserted before it
    // fires. 0 disables the gate entirely (every action fires on
    // the first frame it is true), which is the contract the
    // legacy test_action_map fixtures depend on.
    int dwellMs = 1500;
    // After firing, suppress re-firing of the same action for this
    // many ms. Prevents a sustained pinch (e.g. the user is
    // "trying to click" and holds for 3s) from emitting multiple
    // clicks. 400ms is just over the OS double-click window so
    // the user can't accidentally double-click by holding.
    int cooldownMs = 400;
  };

  // Process one frame's worth of merged actions. The returned
  // ActionSet is what GestureStateMachine should publish to
  // `pending_` — only the gated subset of the one-shot actions
  // (click / right-click / middle-click / double-click) is
  // included; everything else (cursor, wheel, sustained LMB)
  // passes through unchanged.
  ActionSet Process(const ActionSet& local, int64_t ts);

  // Reset all dwell counters. Called from GestureStateMachine::Reset
  // and on Pause/EmergencyStop so a half-completed dwell cannot
  // survive a state change.
  void Reset();

  // For the on-screen overlay preview. Returns the action that is
  // currently counting down (or None) and its progress 0..1.
  // `nowMs` should be the same monotonic clock as the `ts` arg to
  // Process(); using the wall clock from App.cpp is fine.
  struct Preview {
    Kind kind = Kind::None;
    // 0..1, fraction of dwellMs elapsed. Returns 0 when Kind::None,
    // 1 when the action has just committed (and Process would emit
    // it on the same frame).
    float progress = 0.0f;
    int remainingMs = 0;
    int totalMs = 0;
  };
  Preview CurrentPreview(int64_t nowMs) const;

  void SetConfig(const Config& c) { cfg_ = c; }
  const Config& GetConfig() const { return cfg_; }

  // Test-only: read a slot's start timestamp.
  int64_t StartMsForTest(Kind k) const {
    return slot_(k).startMs;
  }
  int64_t CommittedMsForTest(Kind k) const {
    return slot_(k).committedMs;
  }

 private:
  struct Slot {
    // active_ distinguishes "gesture has been continuously
    // asserted since startMs" from "gesture is not asserted right
    // now". 0 is a perfectly valid startMs (the first frame of a
    // session can be at t=0), so we cannot use startMs==0 as a
    // sentinel — that was the v0.6-rc1 bug, fixed by adding the
    // active_ flag.
    bool    active = false;
    int64_t startMs = 0;       // first frame the action was asserted
    int64_t committedMs = 0;   // most recent frame the action fired
  };
  static constexpr int kSlotCount =
      static_cast<int>(Kind::kKindCount);
  std::array<Slot, kSlotCount> slots_{};

  static int index_(Kind k) {
    int i = static_cast<int>(k);
    return (i >= 0 && i < kSlotCount) ? i : 0;
  }
  const Slot& slot_(Kind k) const { return slots_[index_(k)]; }
  Slot& slot_(Kind k) { return slots_[index_(k)]; }

  // Map a local ActionSet's one-shot booleans to a list of
  // candidate Kinds. Priority: LeftClick > RightClick > MiddleClick
  // > DoubleClick (matches the existing left-wins arbitration
  // inside GestureStateMachine, so by the time we see them here
  // usually only one is set; if multiple are set we still pick the
  // highest-priority one as the dwell candidate).
  static Kind PickKind(const ActionSet& local);

  Config cfg_{};
  Preview lastPreview_{};
};

}  // namespace vmosue
