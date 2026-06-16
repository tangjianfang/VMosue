#include "gesture/ScrollDetector.h"
#include "util/Adaptive.h"
#include <cmath>

namespace vmosue {

void ScrollDetector::SetConfig(const Config& c) { cfg_ = c; }
void ScrollDetector::Reset() {
  phase_ = Phase::Idle;
  phaseStartMs_ = 0;
  lastMidX_ = 0.0f;
  lastMidY_ = 0.0f;
}

ScrollDelta ScrollDetector::OnLandmarks(const HandLandmarks& left, int64_t ts) {
  if (left.points.size() < 13) return {};
  // Index tip = 8, middle tip = 12. We track the midpoint so
  // slow separation between the two fingers (which moves each
  // landmark a few pixels toward the other) cancels out: the
  // midpoint is stable as long as the hand as a whole is still.
  const float midX = 0.5f * (left.points[8].x + left.points[12].x);
  const float midY = 0.5f * (left.points[8].y + left.points[12].y);
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

  ScrollDelta delta;
  switch (phase_) {
    case Phase::Idle:
      if (d < enter) {
        phase_ = Phase::Active;
        phaseStartMs_ = ts;
        lastMidX_ = midX;
        lastMidY_ = midY;
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
        lastMidX_ = midX;
        lastMidY_ = midY;
        break;
      }
      // Sign convention matches MOUSEEVENTF_WHEEL /
      // MOUSEEVENTF_HWHEEL: positive dy = up (hand moved up,
      // landmark y decreased), positive dx = right (hand moved
      // right, landmark x increased). So dy = (last - now)
      // while dx = (now - last) — the two signs flip.
      delta.dy = static_cast<int>((lastMidY_ - midY) * scale);
      delta.dx = static_cast<int>((midX - lastMidX_) * scale);
      lastMidX_ = midX;
      lastMidY_ = midY;
      break;
  }
  return delta;
}

}  // namespace vmosue
