#pragma once
#include "inference/HandDetector.h"

namespace vmosue {

enum class ClickEvent { None, LeftClick, LeftDoubleClick, LeftDown, LeftUp };

class ClickDetector {
 public:
  struct Config {
    float pinchThresholdNorm = 0.04f;   // 4% of frame
    float releaseThresholdNorm = 0.07f;
    int holdForDragMs = 200;
    int doubleClickWindowMs = 400;
  };

  void SetConfig(const Config&);
  ClickEvent OnLandmarks(const HandLandmarks& right, int64_t timestampMs);
  void Reset();

 private:
  enum class Phase { Idle, Pinching, Held };
  Config cfg_;
  Phase phase_ = Phase::Idle;
  int64_t pinchStartMs_ = 0;
  int64_t lastClickMs_ = 0;
};

}  // namespace vmosue
