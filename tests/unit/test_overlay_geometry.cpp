#include <gtest/gtest.h>
#include "inference/HandDetector.h"
#include "ui/OverlayGeometry.h"

namespace {

using vmosue::LandmarkToScreen;
using vmosue::Point2F;
using vmosue::ScreenPoint;

// A landmark at (0, 0) in normalized camera coords should land at
// the top-left of the virtual desktop.
TEST(OverlayGeometry, ZeroMapsToVirtualDesktopOrigin) {
  Point2F p{0.0f, 0.0f};
  auto sp = LandmarkToScreen(p, /*virtX=*/-1920, /*virtY=*/0,
                             /*virtW=*/3840,  /*virtH=*/1080);
  EXPECT_FLOAT_EQ(sp.x, -1920.0f);
  EXPECT_FLOAT_EQ(sp.y, 0.0f);
}

// A landmark at (1, 1) should land at the bottom-right corner of
// the virtual desktop.
TEST(OverlayGeometry, UnitMapsToBottomRight) {
  Point2F p{1.0f, 1.0f};
  auto sp = LandmarkToScreen(p, -1920, 0, 3840, 1080);
  EXPECT_FLOAT_EQ(sp.x, 1920.0f);
  EXPECT_FLOAT_EQ(sp.y, 1080.0f);
}

// A landmark at the middle of the camera should land at the center
// of the virtual desktop, even if the origin is negative (the primary
// monitor is on the right of the secondary in this layout).
TEST(OverlayGeometry, HalfIsCenterOfVirtualDesktop) {
  Point2F p{0.5f, 0.5f};
  auto sp = LandmarkToScreen(p, -1920, 0, 3840, 1080);
  EXPECT_FLOAT_EQ(sp.x, 0.0f);
  EXPECT_FLOAT_EQ(sp.y, 540.0f);
}

// A landmark at the left third of the camera image on a
// side-by-side dual-monitor rig should land in the secondary monitor.
TEST(OverlayGeometry, QuarterLandsInSecondaryMonitor) {
  // Secondary monitor is x in [-1920, 0]. A 25%-from-left landmark
  // should land at -1920 + 0.25*3840 = -960.
  Point2F p{0.25f, 0.5f};
  auto sp = LandmarkToScreen(p, -1920, 0, 3840, 1080);
  EXPECT_FLOAT_EQ(sp.x, -960.0f);
  EXPECT_FLOAT_EQ(sp.y, 540.0f);
}

}  // namespace