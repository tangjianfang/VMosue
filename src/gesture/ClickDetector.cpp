#include "gesture/ClickDetector.h"
#include "util/Adaptive.h"
#include <cmath>

namespace vmosue {

void ClickDetector::SetConfig(const Config& c) { cfg_ = c; }
void ClickDetector::Reset() {
  phase_ = Phase::Idle;
  pinchStartMs_ = 0;
  lastClickMs_.reset();
  pendingSingleClick_ = false;
  pendingSingleClickStartMs_ = 0;
}

static float dist2D(const Point2F& a, const Point2F& b) {
  float dx = a.x - b.x, dy = a.y - b.y;
  return std::sqrt(dx*dx + dy*dy);
}

ClickEvent ClickDetector::OnLandmarks(const HandLandmarks& right, int64_t ts) {
  if (right.points.size() < 9) return ClickEvent::None;
  // Thumb tip = 4, index tip = 8
  float d = dist2D(right.points[4], right.points[8]);

  // v0.5: feed the distance into the adaptive observer (it
  // tracks the rolling min/max). The thresholds below come from
  // GetAdaptive() which interpolates between the observed
  // min/max range and the v0.4 default. During cold start the
  // default thresholds (0.04 / 0.07) apply, so behavior is
  // identical to v0.4 for the first ~1 s.
  GetSignalObserver().RecordClickDistance(d);
  const float pinch = GetAdaptive().PinchThreshold();
  const float release = GetAdaptive().ReleaseThreshold();

  ClickEvent ev = ClickEvent::None;
  // Check if a pending (held) single click has expired; if so, emit it now.
  // We do this at the start of each frame so the caller sees the click on
  // the very next OnLandmarks call after the window elapses.
  if (pendingSingleClick_
      && (ts - pendingSingleClickStartMs_) >= cfg_.doubleClickWindowMs) {
    ev = ClickEvent::LeftClick;
    pendingSingleClick_ = false;
    return ev;
  }
  switch (phase_) {
    case Phase::Idle:
      if (d < pinch) {
        phase_ = Phase::Pinching;
        pinchStartMs_ = ts;
      }
      break;
    case Phase::Pinching:
      if (d > release) {
        // Released; emit click or down+up
        bool isDouble = lastClickMs_.has_value()
                     && (ts - *lastClickMs_) < cfg_.doubleClickWindowMs;
        if (isDouble) {
          ev = ClickEvent::LeftDoubleClick;
          lastClickMs_ = ts;
          phase_ = Phase::Idle;
          // A pending single click is consumed by the double-click; discard it.
          pendingSingleClick_ = false;
        } else if (cfg_.suppressSingleClickInDoubleWindow) {
          // Hold the click until the window expires; if a second click
          // arrives in time, it will be promoted to a LeftDoubleClick.
          pendingSingleClick_ = true;
          pendingSingleClickStartMs_ = ts;
          lastClickMs_ = ts;
          phase_ = Phase::Idle;
        } else {
          ev = ClickEvent::LeftClick;
          lastClickMs_ = ts;
          phase_ = Phase::Idle;
        }
      } else if ((ts - pinchStartMs_) > cfg_.holdForDragMs) {
        ev = ClickEvent::LeftDragStart;
        phase_ = Phase::Held;
      }
      break;
    case Phase::Held:
      if (d > release) {
        ev = ClickEvent::LeftDragEnd;
        phase_ = Phase::Idle;
      }
      break;
  }
  return ev;
}

}  // namespace vmosue
