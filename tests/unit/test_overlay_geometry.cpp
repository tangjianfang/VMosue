#include <gtest/gtest.h>
#include "inference/HandDetector.h"
#include "ui/OverlayGeometry.h"

namespace {

using vmosue::LandmarkToScreen;
using vmosue::Point2F;
using vmosue::ScreenPoint;

// The webcam delivers an unmirrored raw frame, but both
// CursorController (OS cursor) and LandmarkToScreen (skeleton
// overlay) flip X for the selfie-mirror convention. That means a
// landmark at the camera's left (small lm.x) lands at the screen's
// right and vice versa. These tests pin that mapping.

TEST(OverlayGeometry, ZeroLandmarkLandsAtRightEdge) {
  // lm.x = 0 means the hand is at the camera's LEFT edge. After the
  // selfie-mirror flip, that lands at the desktop's RIGHT edge.
  Point2F p{0.0f, 0.0f};
  auto sp = LandmarkToScreen(p, /*virtX=*/-1920, /*virtY=*/0,
                             /*virtW=*/3840,  /*virtH=*/1080);
  EXPECT_FLOAT_EQ(sp.x, 1919.0f) << "X should be at the rightmost pixel";
  EXPECT_FLOAT_EQ(sp.y, 0.0f);
}

TEST(OverlayGeometry, UnitLandmarkLandsAtLeftEdge) {
  // lm.x = 1 means the hand is at the camera's RIGHT edge. After
  // the selfie-mirror flip, that lands at the desktop's LEFT edge.
  Point2F p{1.0f, 1.0f};
  auto sp = LandmarkToScreen(p, -1920, 0, 3840, 1080);
  EXPECT_FLOAT_EQ(sp.x, -1920.0f) << "X should be at the leftmost pixel";
  EXPECT_FLOAT_EQ(sp.y, 1079.0f) << "Y should be at the bottommost pixel";
}

TEST(OverlayGeometry, HalfIsCenterOfVirtualDesktop) {
  // Center of the camera image lands at the geometric center of the
  // virtual desktop, even when the primary monitor is at negative X.
  // For W=3840: (1 - 0.5) * (3840 - 1) = 1919.5, plus virtX=-1920
  // gives -0.5 (one half-pixel shy of the seam; correct given the
  // (W-1) form).
  Point2F p{0.5f, 0.5f};
  auto sp = LandmarkToScreen(p, -1920, 0, 3840, 1080);
  EXPECT_FLOAT_EQ(sp.x, -0.5f);
  EXPECT_FLOAT_EQ(sp.y, 539.5f);
}

TEST(OverlayGeometry, QuarterOnCameraFlipsToRightOnDesktop) {
  // The hand at the camera's 25% mark (camera-LEFT) flips to the
  // desktop's right 75% mark. With virtX=-1920, W=3840, that is:
  //   -1920 + (1 - 0.25) * 3839 = -1920 + 2879.25 = 959.25
  Point2F p{0.25f, 0.5f};
  auto sp = LandmarkToScreen(p, -1920, 0, 3840, 1080);
  EXPECT_FLOAT_EQ(sp.x, 959.25f);
  EXPECT_FLOAT_EQ(sp.y, 539.5f);
}

// Cross-component consistency: OverlayGeometry::LandmarkToScreen and
// CursorController must produce the SAME screen position for the
// same input landmark. The user's headline complaint ("现在完全不
// 匹配") was caused by these two going out of sync — the skeleton
// was drawn at one place and the OS cursor went to the mirrored
// position. This test is the regression that would have caught it.
//
// Note on tolerance: the helper preserves float subpixel precision
// (D2D's DrawLine / DrawEllipse round per-pixel at draw time);
// CursorController does a static_cast<int> which truncates toward
// zero. The two therefore can disagree by up to 1 pixel at sample
// points that fall on a fractional coordinate, which is invisible
// to the user. EXPECT_NEAR with tolerance 1.0 expresses the real
// semantic: "the overlay skeleton and the OS cursor draw on the
// same pixel, give or take the sub-pixel rounding."
TEST(OverlayGeometry, MatchesCursorControllerSingleMonitor) {
  // Single-monitor rig: virtX = virtY = 0.
  const int virtX = 0, virtY = 0, virtW = 1920, virtH = 1080;
  // Sweep a grid of landmarks; for each one, the helper's float
  // output should be within 1 pixel of CursorController's int
  // output (which is what SetCursorPos receives).
  for (int ix = 0; ix <= 10; ++ix) {
    for (int iy = 0; iy <= 10; ++iy) {
      const float nx = ix / 10.0f;
      const float ny = iy / 10.0f;
      auto sp = LandmarkToScreen({nx, ny, 0.0f},
                                 virtX, virtY, virtW, virtH);
      const int cursorX = virtX +
          static_cast<int>((1.0f - nx) * static_cast<float>(virtW - 1));
      const int cursorY = virtY +
          static_cast<int>(ny * static_cast<float>(virtH - 1));
      EXPECT_NEAR(sp.x, static_cast<float>(cursorX), 1.0f)
          << "mismatch at nx=" << nx;
      EXPECT_NEAR(sp.y, static_cast<float>(cursorY), 1.0f)
          << "mismatch at ny=" << ny;
    }
  }
}

TEST(OverlayGeometry, MatchesCursorControllerMultiMonitor) {
  // Multi-monitor rig: primary is the right monitor, secondary on
  // the left. virtX is negative. Both helpers must produce the
  // same absolute desktop pixel.
  const int virtX = -1920, virtY = 0, virtW = 3840, virtH = 1080;
  const float samples[][2] = {
      {0.0f, 0.0f}, {0.25f, 0.5f}, {0.5f, 0.5f},
      {0.75f, 0.5f}, {1.0f, 1.0f},
  };
  for (const auto& s : samples) {
    auto sp = LandmarkToScreen({s[0], s[1], 0.0f},
                               virtX, virtY, virtW, virtH);
    const int cursorX = virtX +
        static_cast<int>((1.0f - s[0]) * static_cast<float>(virtW - 1));
    const int cursorY = virtY +
        static_cast<int>(s[1] * static_cast<float>(virtH - 1));
    EXPECT_NEAR(sp.x, static_cast<float>(cursorX), 1.0f)
        << "mismatch at lm.x=" << s[0];
    EXPECT_NEAR(sp.y, static_cast<float>(cursorY), 1.0f)
        << "mismatch at lm.y=" << s[1];
  }
}

}  // namespace
