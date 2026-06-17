#pragma once
#include "inference/HandDetector.h"  // for Point2F

namespace vmosue {

// A pixel position in virtual-desktop coordinates. Returned by
// LandmarkToScreen so the renderer can convert to D2D1_POINT_2F as
// needed. We avoid D2D1_POINT_2F here to keep this header free of
// the d2d1.h include chain (the unit tests should not need Direct2D).
struct ScreenPoint {
  float x;
  float y;
};

// Map a normalized hand landmark to a pixel position on the virtual
// desktop. The four metrics come from GetSystemMetrics:
//   virtX = SM_XVIRTUALSCREEN  (can be negative)
//   virtY = SM_YVIRTUALSCREEN  (can be negative)
//   virtW = SM_CXVIRTUALSCREEN
//   virtH = SM_CYVIRTUALSCREEN
//
// The X axis is flipped for the selfie-mirror convention: a
// landmark at the camera's left (small lm.x) maps to the screen's
// right. The Y axis is unmirrored (vertical is not affected by the
// selfie convention). The (W - 1) / (H - 1) form is intentional —
// it matches CursorController's absolute 1:1 mapping so the
// skeleton overlay drawn by this helper and the OS cursor moved by
// CursorController agree on the hand's screen position. (An earlier
// version used `lm.x * virtW` and diverged from CursorController;
// the user saw the skeleton on one side of the screen and the
// cursor on the other.)
inline ScreenPoint LandmarkToScreen(const Point2F& lm,
                                    int virtX, int virtY,
                                    int virtW, int virtH) {
  // Defensive clamps: the W-1 / H-1 form means a single bad landmark
  // could otherwise send the overlay outside the desktop bounds.
  const int w = (virtW > 0) ? virtW : 1;
  const int h = (virtH > 0) ? virtH : 1;
  return ScreenPoint{
      static_cast<float>(virtX) + (1.0f - lm.x) * static_cast<float>(w - 1),
      static_cast<float>(virtY) + lm.y * static_cast<float>(h - 1),
  };
}

}  // namespace vmosue