#include "gesture/CursorController.h"
#include "gesture/GestureStateMachine.h"  // for ActionSet (complete type)
#include "util/Adaptive.h"
#include "util/Logger.h"

#include <algorithm>
#include <climits>
#include <cmath>

namespace vmosue {

void CursorController::SetConfig(const Config&) {}
void CursorController::Reset() {}

void CursorController::OnLandmarks(const HandLandmarks& right, ActionSet& actions, double dt) {
  (void)dt;
  if (right.points.empty()) return;
  // Index finger MCP (landmark 5). MCP rather than fingertip so a
  // pinch (which snaps the tip sharply) doesn't yank the cursor.
  const auto& p = right.points[5];
  // Drop the frame cleanly on non-finite input so the static_cast<int>
  // below is well-defined and we don't propagate NaN/Inf into the OS
  // cursor.
  if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
    return;
  }

  // Pull the virtual-desktop size from the adaptive observer (cached
  // once on OverlayWindow::Init). Floor at 1920x1080 and cap at 32767
  // in either axis so the multiplication below can never overflow an
  // int on exotic multi-monitor configurations. (DesktopPixels() in
  // Adaptive.h already does the floor; the cap is defense-in-depth.)
  // The origin (virtX, virtY) is also read from the observer so a
  // multi-monitor rig where the primary monitor is not at (0, 0)
  // still produces the correct absolute SetCursorPos target.
  auto [virtW, virtH] = GetAdaptive().DesktopPixels();
  auto [virtX, virtY] = GetSignalObserver().VirtualDesktopOrigin();
  constexpr int kMaxDim = 32767;
  if (virtW <= 0) virtW = 1920;
  if (virtH <= 0) virtH = 1080;
  if (virtW > kMaxDim) virtW = kMaxDim;
  if (virtH > kMaxDim) virtH = kMaxDim;

  // Webcam frames are mirrored (selfie convention). The MediaPipe
  // landmark x grows toward the camera's right, but the user perceives
  // their own hand moving toward their right when the landmark x
  // shrinks. Flip X so "my hand to my right" → "cursor to the right of
  // the screen". Y is left untouched because vertical is not mirrored.
  //
  // Clamp to [0, 1] first: MediaPipe occasionally reports landmarks
  // slightly outside [0, 1] due to model padding; without the clamp
  // the cast to int would jump the cursor off the edge of the screen.
  const float nx = std::clamp(p.x, 0.0f, 1.0f);
  const float ny = std::clamp(p.y, 0.0f, 1.0f);
  // screenX / screenY are absolute desktop pixels (NOT just within
  // the primary monitor). On a single-monitor rig virtX == virtY ==
  // 0 so the result is identical to a primary-only coordinate; on
  // a multi-monitor rig where the primary is the right monitor
  // (virtX < 0) this lets SetCursorPos reach across to the
  // secondary monitor.
  int screenX = virtX + static_cast<int>((1.0f - nx) * static_cast<float>(virtW - 1));
  int screenY = virtY + static_cast<int>(ny * static_cast<float>(virtH - 1));
  // Clamp to the virtual desktop rectangle. Negative values come
  // from a hand held to the camera's edge before the clamp; values
  // past (virtX + virtW - 1) come from a hand held past the other
  // edge. Either way, push the cursor onto the desktop.
  if (screenX < virtX) screenX = virtX;
  if (screenY < virtY) screenY = virtY;
  if (screenX > virtX + virtW - 1) screenX = virtX + virtW - 1;
  if (screenY > virtY + virtH - 1) screenY = virtY + virtH - 1;

  // Feed the absolute target into the action set. The consumer
  // (App.cpp's stateMachineLoop) checks cursorX != INT_MIN to decide
  // whether to call SetCursorPos; we set it unconditionally here so
  // a frame with a hand present always carries an absolute target.
  actions.cursorX = screenX;
  actions.cursorY = screenY;
}

}  // namespace vmosue
