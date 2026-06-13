#pragma once
#include "inference/HandDetector.h"

namespace vmosue {

class ScrollDetector {
 public:
  struct Config {
    float enterThresholdNorm = 0.05f;  // distance between index+middle to enter
    float exitThresholdNorm = 0.03f;
    int enterHoldMs = 100;
    float scaleFactor = 1500.0f;
  };
  void SetConfig(const Config&);
  int OnLandmarks(const HandLandmarks& left, int64_t ts);
  void Reset();
 private:
  enum class Phase { Idle, Active };
  Config cfg_;
  Phase phase_ = Phase::Idle;
  int64_t phaseStartMs_ = 0;
  float lastIndexY_ = 0.0f;
};

}  // namespace vmosue
