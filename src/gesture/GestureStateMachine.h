#pragma once
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>
#include "inference/HandDetector.h"
#include "gesture/ActionSet.h"
#include "gesture/CursorController.h"
#include "gesture/ClickDetector.h"
#include "gesture/AirClickDetector.h"
#include "gesture/ScrollDetector.h"
#include "gesture/PauseDetector.h"
#include "gesture/DwellGate.h"
#include "util/Result.h"

namespace vmosue {

enum class GlobalState { Active, Paused, EmergencyStopped };

class GestureStateMachine {
 public:
  struct Config {
    CursorController::Config cursor;
    ClickDetector::Config click;
    AirClickDetector::Config airClick;
    ScrollDetector::Config scroll;
    PauseDetector::Config pause;
    bool handednessRight = true;        // user-configurable
    // Task 21: when both the primary and "other" hand are visibly open
    // for at least this many ms (using the same 4-fingertip-above-MCP
    // heuristic as PauseDetector), the state machine trips an
    // EmergencyStop. Mirrors the hotkey path so the user has a
    // camera-only fallback if their keyboard is unreachable.
    int twoHandOpenHoldMs = 500;
    // v0.6: dwell-time calibration. Each one-shot action (click,
    // right-click, middle-click, double-click) must be continuously
    // asserted for this many ms before it is published to
    // `pending_`. 0 disables the gate (legacy behavior, used by
    // unit tests and the action-map integration fixtures). The
    // production App's `Init` call is responsible for setting this
    // to the user's configured value (default 2500ms) so the
    // test-suite's default-init path keeps the old "fire
    // immediately" contract. 2500ms is the production default —
    // the user explicitly asked for "2-3 seconds" of calibration
    // so they can comfortably watch the on-screen countdown and
    // decide whether to commit. Capped at 5000ms to keep the
    // preview UI from looking broken.
    int dwellMs = 0;
    int dwellCooldownMs = 400;
    // v0.6.2: "first hand seen" grace period. When a hand first
    // appears (or re-appears after a 1+ second absence) we suppress
    // all action publication for this many ms. The user reported
    // "现在我随便一动，它就瞎乱点" — the moment a hand enters
    // frame, micro-movements of the fingers during settling (e.g.
    // getting a comfortable pinch position) would fire a click.
    // 1500ms is long enough for the user to settle into a neutral
    // pose, short enough that the user doesn't feel the app is
    // "dead on startup". Disabled when 0.
    int firstHandGraceMs = 1500;
  };

  Result<void> Init(const Config&);
  void OnLandmarks(const std::vector<HandLandmarks>& hands, int64_t ts, double dt);
  ActionSet ConsumeActions();
  void Pause();
  void Resume();
  void EmergencyStop();
  GlobalState State() const { return state_.load(); }
  void Reset();

  // v0.6: peek the DwellGate's current preview. Consumed by the
  // overlay to render "About to: Left click 1.2s". Caller is
  // expected to be on the gesture-state-machine thread (the gate
  // state is mutated there). The returned struct is a value copy
  // so the caller can keep it past the next OnLandmarks call.
  DwellGate::Preview GetDwellPreview(int64_t nowMs) const {
    return dwell_.CurrentPreview(nowMs);
  }

  // v0.6.2: settle-in grace status. Caller (App.cpp) reads
  // this to populate the Feedback fields consumed by the
  // overlay, which renders "Calibrating... 1.2s" while
  // `active` is true. The state machine owns the canonical
  // truth (which hand is visible, when the grace timer
  // started) — the overlay just renders what it's told.
  struct SettlingStatus {
    bool active = false;
    int remainingMs = 0;
    int totalMs = 0;
  };
  SettlingStatus GetSettlingStatus(int64_t nowMs) const {
    SettlingStatus s;
    if (cfg_.firstHandGraceMs <= 0 || graceStartMs_ == 0) return s;
    int64_t elapsed = nowMs - graceStartMs_;
    if (elapsed < 0) elapsed = 0;
    if (elapsed >= cfg_.firstHandGraceMs) return s;
    s.active = true;
    s.remainingMs = static_cast<int>(cfg_.firstHandGraceMs - elapsed);
    s.totalMs = cfg_.firstHandGraceMs;
    return s;
  }

  // v0.6.2: detect the active->inactive transition so the
  // overlay can flash "✓ Ready" for ~800ms after grace ends.
  // Returns true on the frame the grace gate flipped off;
  // false afterwards. Caller stores the result and clears it
  // once the overlay has rendered the flash at least once.
  // We deliberately don't use a callback here — the overlay
  // is allowed to miss the transition (e.g. the user has the
  // app focused elsewhere) and the user just doesn't see the
  // flash. The grace itself is still enforced by
  // GetSettlingStatus / the OnLandmarks gate.
  bool ConsumeSettlingJustEnded() {
    bool wasActive = lastSettlingActive_;
    bool isActive = (cfg_.firstHandGraceMs > 0 && graceStartMs_ != 0 &&
                     (lastHandSeenMs_ - graceStartMs_) < cfg_.firstHandGraceMs);
    lastSettlingActive_ = isActive;
    return wasActive && !isActive;
  }

 private:
  Config cfg_;
  CursorController cursor_;
  ClickDetector click_;
  AirClickDetector airClick_;
  ScrollDetector scroll_;
  PauseDetector pause_;
  // v0.6: dwell-time calibration. Sits between the per-detector
  // arbitration (which produces `local`) and the `pending_` write
  // (which the consumer thread pulls). It re-publishes only the
  // subset of `local` that has been continuously asserted for
  // cfg.dwellMs.
  DwellGate dwell_{};
  std::atomic<GlobalState> state_{GlobalState::Active};
  std::mutex actionsMu_;
  ActionSet pending_;
  // Task 21: timestamp (ms, same clock as `ts`) at which both hands
  // became visibly open, and a separate boolean to track whether the
  // gesture is currently in progress. We can't use `startMs_ == 0` as
  // the "not started" sentinel because 0 is a valid timestamp -- the
  // first frame of a session can legitimately arrive at t=0 and we
  // would otherwise fail to ever trip the gesture.
  bool twoHandOpenActive_ = false;
  int64_t twoHandOpenStartMs_ = 0;
  // v0.6.2: track the most recent "hand visible" timestamp. When a
  // hand re-appears after > 1s of absence, the grace timer is
  // re-armed so the user gets the same "settle in" period every
  // time they bring their hand back into frame. A bool tracks
  // "was a hand visible LAST frame?" rather than comparing ts
  // values, because ts is wall-clock-derived and the gap between
  // frames is small (~33ms) but the gap between sessions can be
  // hours; a simple bool + "first frame after a long absence"
  // heuristic is more robust than timestamp arithmetic.
  int64_t lastHandSeenMs_ = 0;
  int64_t graceStartMs_ = 0;
  bool handSeenLastFrame_ = false;
  // v0.6.2: edge-detect the grace window so the overlay can
  // flash "✓ Ready" exactly once when it ends.
  bool lastSettlingActive_ = false;
};

}  // namespace vmosue
