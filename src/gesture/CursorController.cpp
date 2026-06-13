#include "gesture/CursorController.h"
#include "gesture/GestureStateMachine.h"  // for ActionSet (complete type)
#include "util/Logger.h"
#include <cmath>

namespace vmosue {

void CursorController::SetConfig(const Config& c) { cfg_ = c; }
void CursorController::Reset() { initialized_ = false; prevPivot_.reset(); }

void CursorController::OnLandmarks(const HandLandmarks& right, ActionSet& actions, double dt) {
  (void)dt;  // Currently no time-aware smoothing; the dead zone + sensitivity
             // suffice. Kept in the signature to allow future velocity-based
             // filtering (e.g. frame-rate independent dead zones).
  if (right.points.empty()) return;
  // Index finger MCP is landmark 5; we'll track that to avoid cursor jump on click.
  const auto& p = right.points[5];
  if (!initialized_) {
    prevPivot_ = p;
    initialized_ = true;
    return;
  }
  float dx = (p.x - prevPivot_->x) * cfg_.sensitivityX;
  float dy = (p.y - prevPivot_->y) * cfg_.sensitivityY;
  if (std::fabs(dx) < cfg_.deadZoneNorm) dx = 0.0f;
  if (std::fabs(dy) < cfg_.deadZoneNorm) dy = 0.0f;
  // Convert normalized motion to pixel motion (will scale by screen size at injection site).
  int pixelDx = static_cast<int>(dx * 1920.0f);
  int pixelDy = static_cast<int>(dy * 1080.0f);
  if (pixelDx != 0 || pixelDy != 0) {
    VMOSUE_LOG_DEBUG("Cursor delta: ({}, {})", pixelDx, pixelDy);
    // Task 13: propagate delta via the ActionSet so the consumer thread
    // can dispatch it to InputInjector. The state machine owns the mutex.
    actions.cursorDx = pixelDx;
    actions.cursorDy = pixelDy;
  }
  prevPivot_ = p;
}

}  // namespace vmosue
