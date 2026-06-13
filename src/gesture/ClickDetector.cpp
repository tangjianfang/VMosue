#include "gesture/ClickDetector.h"
#include <cmath>

namespace vmosue {

void ClickDetector::SetConfig(const Config& c) { cfg_ = c; }
void ClickDetector::Reset() {
  phase_ = Phase::Idle;
  pinchStartMs_ = 0;
  lastClickMs_.reset();
}

static float dist2D(const Point2F& a, const Point2F& b) {
  float dx = a.x - b.x, dy = a.y - b.y;
  return std::sqrt(dx*dx + dy*dy);
}

ClickEvent ClickDetector::OnLandmarks(const HandLandmarks& right, int64_t ts) {
  if (right.points.size() < 9) return ClickEvent::None;
  // Thumb tip = 4, index tip = 8
  float d = dist2D(right.points[4], right.points[8]);
  ClickEvent ev = ClickEvent::None;
  switch (phase_) {
    case Phase::Idle:
      if (d < cfg_.pinchThresholdNorm) {
        phase_ = Phase::Pinching;
        pinchStartMs_ = ts;
      }
      break;
    case Phase::Pinching:
      if (d > cfg_.releaseThresholdNorm) {
        // Released; emit click or down+up
        bool isDouble = lastClickMs_.has_value()
                     && (ts - *lastClickMs_) < cfg_.doubleClickWindowMs;
        ev = isDouble ? ClickEvent::LeftDoubleClick : ClickEvent::LeftClick;
        lastClickMs_ = ts;
        phase_ = Phase::Idle;
      } else if ((ts - pinchStartMs_) > cfg_.holdForDragMs) {
        ev = ClickEvent::LeftDragStart;
        phase_ = Phase::Held;
      }
      break;
    case Phase::Held:
      if (d > cfg_.releaseThresholdNorm) {
        ev = ClickEvent::LeftDragEnd;
        phase_ = Phase::Idle;
      }
      break;
  }
  return ev;
}

}  // namespace vmosue
