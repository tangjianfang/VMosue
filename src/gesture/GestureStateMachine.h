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
    bool handednessRight = true;  // user-configurable
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
  std::atomic<GlobalState> state_{GlobalState::Active};
  std::mutex actionsMu_;
  ActionSet pending_;
};

}  // namespace vmosue
