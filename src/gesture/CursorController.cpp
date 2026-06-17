#include "gesture/CursorController.h"
#include "gesture/GestureStateMachine.h"  // for ActionSet (complete type)
#include "util/Adaptive.h"
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

  // v0.5: dead-zone and desktop-pixel resolution come from the
  // adaptive controller, not the user config. The dead zone is
  // computed from observed cursor stillness noise (3-sigma rule);
  // the desktop pixel size is read from the observer cache that
  // OverlayWindow::Init populated. The Config struct still
  // carries the v0.4 defaults for tests, but production paths
  // ignore cfg_.deadZoneNorm and the hard-coded 1920x1080.
  const float dz = GetAdaptive().CursorDeadZone();
  auto [virtW, virtH] = GetAdaptive().DesktopPixels();
  // Defensive clamp. DesktopPixels() already floors at (1920,1080),
  // but an unusual multi-monitor virtual desktop could report a very
  // large extent; cap it so `dx * virtW` below can't overflow a
  // 32-bit int when sensitivity is high. 32767 covers any real screen
  // arrangement (the Win32 virtual desktop itself is bounded near
  // there) while keeping the float->int product comfortably in range.
  constexpr int kMaxDim = 32767;
  if (virtW <= 0 || virtW > kMaxDim) virtW = (virtW <= 0) ? 1920 : kMaxDim;
  if (virtH <= 0 || virtH > kMaxDim) virtH = (virtH <= 0) ? 1080 : kMaxDim;

  // WebCam frames are natively mirrored (selfie convention), so the
  // MediaPipe landmark x grows toward the user's actual right hand
  // but the user perceives motion in the opposite direction — moving
  // their hand to screen-left produces a decreasing landmark x, yet
  // the cursor would drift screen-right if we just multiplied by +1.
  // Negate dx so the cursor follows the hand as the user sees it.
  // Y is left untouched because webcam vertical is not mirrored.
  float dx = -(p.x - prevPivot_->x) * cfg_.sensitivityX;
  float dy = (p.y - prevPivot_->y) * cfg_.sensitivityY;
  // Guard against NaN/Inf landmarks (a malformed detector frame). A
  // non-finite delta would make the static_cast<int> below undefined
  // behavior, so drop the frame cleanly and re-baseline next frame.
  if (!std::isfinite(dx) || !std::isfinite(dy)) {
    prevPivot_ = p;
    return;
  }
  if (std::fabs(dx) < dz) dx = 0.0f;
  if (std::fabs(dy) < dz) dy = 0.0f;
  // Convert normalized motion to pixel motion. virtW/virtH are the
  // real virtual-desktop dimensions (cached once on OverlayWindow
  // ::Init), so this works correctly on 4K and multi-monitor
  // setups — not just the old 1920x1080 assumption.
  int pixelDx = static_cast<int>(dx * static_cast<float>(virtW));
  int pixelDy = static_cast<int>(dy * static_cast<float>(virtH));

  // v0.5: feed the post-deadzone pixel delta into the adaptive
  // observer so CursorDeadZone() can adapt from the real
  // stillness noise. We record the *pixel* delta (not normalized)
  // because that's what the 3-sigma threshold is calibrated in.
  if (pixelDx != 0 || pixelDy != 0) {
    GetSignalObserver().RecordCursorMotion(
        static_cast<double>(pixelDx),
        static_cast<double>(pixelDy));
  }

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
