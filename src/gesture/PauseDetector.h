#pragma once
#include "inference/HandDetector.h"

namespace vmosue {

class PauseDetector {
 public:
  struct Config { int holdMs = 1000; };
  void SetConfig(const Config&);
  enum class Event { None, PauseToggle };
  Event OnLandmarks(const HandLandmarks& left, int64_t ts);
  void Reset();
 private:
  Config cfg_;
  bool open_ = false;
  int64_t openStartMs_ = 0;
  bool toggled_ = false;
};

}  // namespace vmosue