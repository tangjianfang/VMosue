// Direct unit test for CursorController's absolute 1:1 mapping. The
// integration tests (test_action_map, test_pipeline_e2e) cover the
// cursor field end-to-end via merged ActionSet, but they assert
// direction (first vs last) rather than absolute pixel values, so a
// regression in the mapping math itself would not be caught there.
// This TU is the focused regression: synthetic HandLandmarks in,
// known screen pixel out, no fixtures, no mediator.
#include <gtest/gtest.h>
#include <climits>
#include <limits>

#include "gesture/CursorController.h"
#include "gesture/GestureStateMachine.h"  // ActionSet
#include "util/Adaptive.h"

namespace {

using vmosue::CursorController;
using vmosue::ActionSet;
using vmosue::GetSignalObserver;
using vmosue::HandLandmarks;
using vmosue::Point2F;

// Build a HandLandmarks with the index MCP (point 5) at (x, y) in
// normalized [0, 1] coords. handedness = 1 so the GestureStateMachine
// picks this hand as the cursor-driving right hand. Other points are
// zeroed; CursorController only reads point 5.
HandLandmarks makeHand(float x, float y) {
  HandLandmarks lm{};
  lm.handedness = 1;
  lm.score      = 0.9f;
  lm.points[5]  = {x, y, 0.0f};
  return lm;
}

// Per-test fixture: pin a known virtual-desktop size so the mapping
// math is deterministic, and reset the observer so a previous test's
// cold-start state does not leak.
class CursorControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GetSignalObserver().RecordVirtualDesktop(1920, 1080);
  }
  void TearDown() override {
    // Restore the singleton to its cold-start size so a later test
    // that doesn't pin the desktop still gets the documented
    // 1920x1080 fallback.
    GetSignalObserver().RecordVirtualDesktop(1920, 1080);
  }
};

}  // namespace

// ---- Mapping math ----

TEST_F(CursorControllerTest, CenterHandMapsToScreenCenter) {
  CursorController c;
  ActionSet a{};
  c.OnLandmarks(makeHand(0.5f, 0.5f), a, 1.0/30.0);
  // Mapping is screenX = (1 - nx) * (W - 1) and screenY = ny * (H - 1).
  // The (W - 1) / (H - 1) form means the screen's geometric center
  // sits at (W-1)/2 and (H-1)/2 — integer-dividing an even (W-1)
  // by 2 drops us at the pixel index one shy of W/2 / H/2. That's
  // expected: pixels are addressed 0..W-1, so the center index of a
  // 1920-wide screen is 959, not 960.
  EXPECT_EQ(a.cursorX, (1920 - 1) / 2);   // 959
  EXPECT_EQ(a.cursorY, (1080 - 1) / 2);   // 539
}

TEST_F(CursorControllerTest, SelfieMirrorFlipsX) {
  // Webcam is unmirrored (raw camera frame): my right hand appears on
  // camera's left side -> MediaPipe landmark x is small. Cursor must
  // go to my right (screen's right) -> screenX = (1 - nx) * (W - 1).
  CursorController c;
  ActionSet a{};
  c.OnLandmarks(makeHand(0.0f, 0.5f), a, 1.0/30.0);
  EXPECT_EQ(a.cursorX, 1919);  // right edge (W-1)
  EXPECT_EQ(a.cursorY, 539);   // vertically centered = (H-1)/2

  ActionSet b{};
  c.OnLandmarks(makeHand(1.0f, 0.5f), b, 1.0/30.0);
  EXPECT_EQ(b.cursorX, 0);     // left edge
  EXPECT_EQ(b.cursorY, 539);
}

TEST_F(CursorControllerTest, TopAndBottomEdgesAreCorrect) {
  CursorController c;
  ActionSet top{};
  c.OnLandmarks(makeHand(0.5f, 0.0f), top, 1.0/30.0);
  EXPECT_EQ(top.cursorY, 0);      // top of screen

  ActionSet bottom{};
  c.OnLandmarks(makeHand(0.5f, 1.0f), bottom, 1.0/30.0);
  EXPECT_EQ(bottom.cursorY, 1079); // bottom of screen (H-1)
}

TEST_F(CursorControllerTest, VideoRangeMaps1to1ToScreenRange) {
  // The user's headline ask: "视频的范围完全映射到屏幕的范围". Walk
  // both axes from 0 to 1 in 0.1 steps; every (x, y) in the video
  // must produce a (screenX, screenY) that linearly covers the
  // screen's full range.
  CursorController c;
  for (int i = 0; i <= 10; ++i) {
    const float nx = static_cast<float>(i) / 10.0f;
    ActionSet a{};
    c.OnLandmarks(makeHand(nx, nx), a, 1.0/30.0);
    // Selfie-mirror X: nx=0 -> screenX=W-1, nx=1 -> screenX=0.
    // Y axis is unmirrored: ny=0 -> screenY=0, ny=1 -> screenY=H-1.
    const int expectedX = static_cast<int>((1.0f - nx) * 1919.0f);
    const int expectedY = static_cast<int>(nx * 1079.0f);
    EXPECT_EQ(a.cursorX, expectedX)
        << "x=" << nx << " expected screenX=" << expectedX
        << " got " << a.cursorX;
    EXPECT_EQ(a.cursorY, expectedY)
        << "y=" << nx << " expected screenY=" << expectedY
        << " got " << a.cursorY;
  }
}

TEST_F(CursorControllerTest, OutOfRangeCoordinatesAreClamped) {
  // MediaPipe occasionally returns landmarks slightly outside [0, 1]
  // due to model padding; without clamping the cursor would jump off
  // the edge of the screen.
  CursorController c;
  ActionSet a{};
  c.OnLandmarks(makeHand(-0.5f, -0.5f), a, 1.0/30.0);
  EXPECT_GE(a.cursorX, 0);
  EXPECT_GE(a.cursorY, 0);
  EXPECT_LT(a.cursorX, 1920);
  EXPECT_LT(a.cursorY, 1080);

  ActionSet b{};
  c.OnLandmarks(makeHand(1.5f, 1.5f), b, 1.0/30.0);
  EXPECT_GE(b.cursorX, 0);
  EXPECT_GE(b.cursorY, 0);
  EXPECT_LT(b.cursorX, 1920);
  EXPECT_LT(b.cursorY, 1080);
}

TEST_F(CursorControllerTest, NonFinitePivotLeavesSentinel) {
  CursorController c;
  ActionSet a{};
  HandLandmarks lm = makeHand(0.5f, 0.5f);
  lm.points[5].x = std::numeric_limits<float>::quiet_NaN();
  c.OnLandmarks(lm, a, 1.0/30.0);
  // A bad pivot must NOT propagate NaN into the OS cursor; the
  // sentinel INT_MIN lets the consumer (state machine) skip this
  // frame entirely.
  EXPECT_EQ(a.cursorX, INT_MIN);
  EXPECT_EQ(a.cursorY, INT_MIN);
}

TEST_F(CursorControllerTest, MultiMonitorDesktopResizesMapping) {
  // Drive the same hand coordinates through three different
  // virtual-desktop sizes; the cursor must scale linearly with the
  // desktop so a 4K monitor maps the whole video range to 3840
  // pixels, not 1920.
  CursorController c;
  struct Size { int w, h; };
  const Size sizes[] = {{1920, 1080}, {2560, 1440}, {3840, 2160}};
  for (const auto& s : sizes) {
    GetSignalObserver().RecordVirtualDesktop(s.w, s.h);
    ActionSet a{};
    c.OnLandmarks(makeHand(0.5f, 0.5f), a, 1.0/30.0);
    EXPECT_EQ(a.cursorX, (s.w - 1) / 2) << "w=" << s.w;
    EXPECT_EQ(a.cursorY, (s.h - 1) / 2) << "h=" << s.h;

    ActionSet edge{};
    c.OnLandmarks(makeHand(0.0f, 0.0f), edge, 1.0/30.0);
    EXPECT_EQ(edge.cursorX, s.w - 1) << "w=" << s.w;
    EXPECT_EQ(edge.cursorY, 0);
  }
}

TEST_F(CursorControllerTest, NoOffsetForHandAtCenter) {
  // The user's "摄像头的位置偏移" complaint: with the old relative
  // cursor scheme, a hand held at the visual center of the video
  // did not produce a cursor at the screen center -- it drifted
  // according to the accumulated delta. With absolute mapping, a
  // stationary hand at the video center MUST produce a cursor at
  // the screen center, regardless of how many frames we drive.
  CursorController c;
  for (int i = 0; i < 100; ++i) {
    ActionSet a{};
    c.OnLandmarks(makeHand(0.5f, 0.5f), a, 1.0/30.0);
    EXPECT_EQ(a.cursorX, 959)
        << "frame " << i << ": center-of-video hand must stay at screen center";
    EXPECT_EQ(a.cursorY, 539);
  }
}
