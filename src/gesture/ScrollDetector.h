#pragma once
#include "inference/HandDetector.h"

namespace vmosue {

// Per-frame scroll deltas. The two-finger scroll gesture
// (index + middle fingertips together) drives both axes: moving
// the hand up/down gives a vertical wheel, moving it
// left/right gives a horizontal wheel. Many Windows apps treat
// the two axes as fully independent (Excel, browser tabs, etc.),
// so we emit them in the same call — the caller decides which
// to forward.
//
// Sign convention matches the Windows MOUSEEVENTF_WHEEL /
// MOUSEEVENTF_HWHEEL fields: positive = up / right.
struct ScrollDelta {
  int dy = 0;  // vertical wheel (positive = up)
  int dx = 0;  // horizontal wheel (positive = right)
};

class ScrollDetector {
 public:
  struct Config {
    float enterThresholdNorm = 0.05f;  // distance between index+middle to enter
    float exitThresholdNorm = 0.03f;
    int enterHoldMs = 100;
    float scaleFactor = 1500.0f;
  };
  void SetConfig(const Config&);
  ScrollDelta OnLandmarks(const HandLandmarks& left, int64_t ts);
  void Reset();
 private:
  enum class Phase { Idle, Active };
  Config cfg_;
  Phase phase_ = Phase::Idle;
  int64_t phaseStartMs_ = 0;
  // Midpoint of the two fingertips in normalized frame
  // coordinates. We track the midpoint rather than a single
  // fingertip so a slow separation between fingers (which moves
  // landmark 8 a few pixels toward landmark 12) does not show up
  // as spurious scroll motion.
  float lastMidX_ = 0.0f;
  float lastMidY_ = 0.0f;
};

}  // namespace vmosue
