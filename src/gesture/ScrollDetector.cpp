#include "gesture/ScrollDetector.h"
#include <cmath>

namespace vmosue {

void ScrollDetector::SetConfig(const Config& c) { cfg_ = c; }
void ScrollDetector::Reset() { phase_ = Phase::Idle; phaseStartMs_ = 0; }

int ScrollDetector::OnLandmarks(const HandLandmarks& left, int64_t ts) {
  if (left.points.size() < 13) return 0;
  // Index tip = 8, middle tip = 12
  float d = std::fabs(left.points[8].y - left.points[12].y);
  int delta = 0;
  switch (phase_) {
    case Phase::Idle:
      if (d < cfg_.enterThresholdNorm) {
        phase_ = Phase::Active;
        phaseStartMs_ = ts;
        lastIndexY_ = left.points[8].y;
      }
      break;
    case Phase::Active:
      if (d > cfg_.exitThresholdNorm) {
        phase_ = Phase::Idle;
        break;
      }
      if ((ts - phaseStartMs_) < cfg_.enterHoldMs) {
        // hold not yet complete; don't emit
        lastIndexY_ = left.points[8].y;
        break;
      }
      delta = static_cast<int>((lastIndexY_ - left.points[8].y) * cfg_.scaleFactor);
      lastIndexY_ = left.points[8].y;
      break;
  }
  return delta;
}

}  // namespace vmosue
