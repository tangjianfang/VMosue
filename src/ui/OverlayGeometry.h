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
// The mapping is the obvious 1:1 affine: a landmark at (0, 0) in
// camera coords lands at (virtX, virtY); (1, 1) at (virtX+virtW,
// virtY+virtH). Aspect ratio is NOT preserved here; the spec says
// landmarks map 1:1 to pixels.
inline ScreenPoint LandmarkToScreen(const Point2F& lm,
                                    int virtX, int virtY,
                                    int virtW, int virtH) {
  return ScreenPoint{
      static_cast<float>(virtX) + lm.x * static_cast<float>(virtW),
      static_cast<float>(virtY) + lm.y * static_cast<float>(virtH),
  };
}

}  // namespace vmosue