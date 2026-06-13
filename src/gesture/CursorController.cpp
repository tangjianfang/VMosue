#include "gesture/CursorController.h"
#include "util/Logger.h"
#include <cmath>

namespace vmosue {

void CursorController::SetConfig(const Config& c) { cfg_ = c; }
void CursorController::Reset() { initialized_ = false; prevPivot_.reset(); }

void CursorController::OnLandmarks(const HandLandmarks& right, double dt) {
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
    // InputInjector::Get().MoveCursor(pixelDx, pixelDy);  // wired in Task 12
  }
  prevPivot_ = p;
}

}  // namespace vmosue
