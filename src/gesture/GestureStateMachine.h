#pragma once
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>
#include "inference/HandDetector.h"
#include "gesture/CursorController.h"
#include "gesture/ClickDetector.h"
#include "gesture/AirClickDetector.h"
#include "gesture/ScrollDetector.h"
#include "gesture/PauseDetector.h"
#include "util/Result.h"

namespace vmosue {

enum class GlobalState { Active, Paused, EmergencyStopped };

struct ActionSet {
  int cursorDx = 0, cursorDy = 0;
  bool leftClick = false, leftDoubleClick = false;
  bool leftDown = false, leftUp = false;
  bool rightClick = false;
  int wheel = 0;
  bool safeRelease = false;
};

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
  };

  Result<void> Init(const Config&);
  void OnLandmarks(const std::vector<HandLandmarks>& hands, int64_t ts, double dt);
  ActionSet ConsumeActions();
  void Pause();
  void Resume();
  void EmergencyStop();
  GlobalState State() const { return state_.load(); }
  void Reset();

 private:
  Config cfg_;
  CursorController cursor_;
  ClickDetector click_;
  AirClickDetector airClick_;
  ScrollDetector scroll_;
  PauseDetector pause_;
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
};

}  // namespace vmosue
