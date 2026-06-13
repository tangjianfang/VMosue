#include "gesture/AirClickDetector.h"
#include <cmath>

namespace vmosue {

void AirClickDetector::SetConfig(const Config& c) { cfg_ = c; }
void AirClickDetector::Reset() {
  phase_ = Phase::Idle;
  phaseStartMs_ = 0;
  lastClickMs_.reset();
}

AirClickEvent AirClickDetector::OnLandmarks(const HandLandmarks& right, int64_t ts) {
  if (right.world.empty()) return AirClickEvent::None;
  if (lastClickMs_.has_value() && (ts - *lastClickMs_) < cfg_.cooldownMs) {
    return AirClickEvent::None;
  }
  AirClickEvent ev = AirClickEvent::None;
  switch (phase_) {
    case Phase::Idle:
      // Index finger tip pushed forward (Z decreasing) relative to wrist?
      if (right.world[8].z < right.world[0].z - cfg_.zApproachThreshold) {
        phase_ = Phase::Approach;
        phaseStartMs_ = ts;
      }
      break;
    case Phase::Approach:
      if (right.world[8].z > right.world[0].z - cfg_.zApproachThreshold * 0.5f) {
        // Retreated
        int64_t dur = ts - phaseStartMs_;
        if (dur >= cfg_.minWindowMs && dur <= cfg_.windowMs) {
          ev = AirClickEvent::RightClick;
          lastClickMs_ = ts;
        }
        phase_ = Phase::Idle;
      } else if (ts - phaseStartMs_ > cfg_.windowMs) {
        phase_ = Phase::Idle;
      }
      break;
    case Phase::Retreat:
      phase_ = Phase::Idle;
      break;
  }
  return ev;
}

}  // namespace vmosue