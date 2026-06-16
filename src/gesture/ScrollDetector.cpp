#include "gesture/ScrollDetector.h"
#include "util/Adaptive.h"
#include <cmath>

namespace vmosue {

void ScrollDetector::SetConfig(const Config& c) { cfg_ = c; }
void ScrollDetector::Reset() { phase_ = Phase::Idle; phaseStartMs_ = 0; }

int ScrollDetector::OnLandmarks(const HandLandmarks& left, int64_t ts) {
  if (left.points.size() < 13) return 0;
  // Index tip = 8, middle tip = 12
  float d = std::fabs(left.points[8].y - left.points[12].y);

  // v0.5: record the distance (min/max histogram for the
  // threshold) and pull the enter/exit thresholds + scale factor
  // from the adaptive controller. The scaleFactor compensates
  // for screen height (1080p gets the full 1500, 4K gets ~750)
  // so the user gets the same physical scroll per cm of finger
  // motion regardless of monitor resolution.
  GetSignalObserver().RecordScrollDistance(d);
  const float enter = GetAdaptive().ScrollEnterThreshold();
  const float exit  = GetAdaptive().ScrollExitThreshold();
  const float scale = GetAdaptive().ScrollScaleFactor();

  int delta = 0;
  switch (phase_) {
    case Phase::Idle:
      if (d < enter) {
        phase_ = Phase::Active;
        phaseStartMs_ = ts;
        lastIndexY_ = left.points[8].y;
      }
      break;
    case Phase::Active:
      if (d > exit) {
        phase_ = Phase::Idle;
        break;
      }
      if ((ts - phaseStartMs_) < cfg_.enterHoldMs) {
        // Hold not yet complete: don't emit, but keep tracking so we don't
        // treat the post-hold frame as a sudden jump of (holdStartY -> nowY).
        lastIndexY_ = left.points[8].y;
        break;
      }
      delta = static_cast<int>((lastIndexY_ - left.points[8].y) * scale);
      lastIndexY_ = left.points[8].y;
      break;
  }
  return delta;
}

}  // namespace vmosue
