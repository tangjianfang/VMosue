#include "gesture/AirClickDetector.h"
#include "util/Adaptive.h"
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
  // v0.5: approach threshold adapts to observed z noise floor
  // (3-sigma rule). During cold start the controller returns the
  // v0.4 default (0.02 m), so behavior is unchanged for the first
  // ~1 s. The retreat half-threshold scales with the approach
  // threshold to preserve the same "must retract by 50%" semantic
  // the v0.4 hard-coded 0.5 factor expressed.
  const float thr = GetAdaptive().ZApproachThreshold();
  AirClickEvent ev = AirClickEvent::None;
  switch (phase_) {
    case Phase::Idle:
      // Index finger tip pushed forward (Z decreasing) relative to wrist?
      if (right.world[8].z < right.world[0].z - thr) {
        phase_ = Phase::Approach;
        phaseStartMs_ = ts;
      }
      break;
    case Phase::Approach:
      if (right.world[8].z > right.world[0].z - thr * 0.5f) {
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