#include <gtest/gtest.h>
#include "gesture/ScrollDetector.h"

using vmosue::ScrollDetector;
using vmosue::HandLandmarks;
using vmosue::ScrollDelta;

static vmosue::HandLandmarks twoFingerHand(float indexX, float indexY,
                                           float middleX, float middleY) {
  vmosue::HandLandmarks lm{};
  lm.handedness = 0;
  lm.points[8]  = {indexX,  indexY,  0.0f};
  lm.points[12] = {middleX, middleY, 0.0f};
  return lm;
}

TEST(ScrollDetector, EmitsWheelOnUpwardMotion) {
  ScrollDetector d;
  d.SetConfig({});
  int64_t t = 0;
  d.OnLandmarks(twoFingerHand(0.50f, 0.50f, 0.50f, 0.50f), t);     // enter (low delta)
  d.OnLandmarks(twoFingerHand(0.50f, 0.50f, 0.50f, 0.50f), t+50);
  d.OnLandmarks(twoFingerHand(0.50f, 0.40f, 0.50f, 0.40f), t+100); // index moves up
  auto delta = d.OnLandmarks(twoFingerHand(0.50f, 0.30f, 0.50f, 0.30f), t+150);
  // Spec: at t+150, hold elapsed, dy = (0.40-0.30)*1500 = 150.
  EXPECT_GT(delta.dy, 100);
  EXPECT_EQ(delta.dx, 0);  // X is stationary
}

TEST(ScrollDetector, EmitsNegativeDeltaOnDownwardMotion) {
  ScrollDetector d;
  d.SetConfig({});
  int64_t t = 0;
  d.OnLandmarks(twoFingerHand(0.50f, 0.50f, 0.50f, 0.50f), t);     // enter
  d.OnLandmarks(twoFingerHand(0.50f, 0.50f, 0.50f, 0.50f), t+50);
  // hold elapsed (t+100 >= 100), finger moves DOWN: y increases
  d.OnLandmarks(twoFingerHand(0.50f, 0.60f, 0.50f, 0.60f), t+100);
  auto delta = d.OnLandmarks(twoFingerHand(0.50f, 0.70f, 0.50f, 0.70f), t+150);
  // (0.60-0.70)*1500 = -150
  EXPECT_LT(delta.dy, -100);
  EXPECT_EQ(delta.dx, 0);
}

TEST(ScrollDetector, ExitsToIdleOnLargeSeparation) {
  ScrollDetector d;
  d.SetConfig({});
  int64_t t = 0;
  d.OnLandmarks(twoFingerHand(0.50f, 0.50f, 0.50f, 0.50f), t);     // enter
  d.OnLandmarks(twoFingerHand(0.50f, 0.50f, 0.50f, 0.50f), t+50);
  // spread fingers far apart to exceed exitThresholdNorm=0.03
  auto out1 = d.OnLandmarks(twoFingerHand(0.50f, 0.50f, 0.50f, 0.80f), t+100);
  EXPECT_EQ(out1.dy, 0);  // exit transition emits nothing
  EXPECT_EQ(out1.dx, 0);
  // After exit, large-separation should NOT re-enter (0.50 vs 0.80 -> d=0.30 > enterThresholdNorm=0.05)
  auto out2 = d.OnLandmarks(twoFingerHand(0.50f, 0.50f, 0.50f, 0.80f), t+150);
  EXPECT_EQ(out2.dy, 0);
  EXPECT_EQ(out2.dx, 0);
  // Bring fingers close again: re-entry
  d.OnLandmarks(twoFingerHand(0.50f, 0.50f, 0.50f, 0.50f), t+200);
  // Need to re-enter and the next call should be a hold-gated emission
  auto out3 = d.OnLandmarks(twoFingerHand(0.50f, 0.50f, 0.50f, 0.50f), t+250);
  EXPECT_EQ(out3.dy, 0);  // within hold
  EXPECT_EQ(out3.dx, 0);
  d.OnLandmarks(twoFingerHand(0.50f, 0.40f, 0.50f, 0.40f), t+350);
  auto out4 = d.OnLandmarks(twoFingerHand(0.50f, 0.30f, 0.50f, 0.30f), t+400);
  EXPECT_GT(out4.dy, 0);
}

TEST(ScrollDetector, DoesNotEmitDuringHold) {
  ScrollDetector d;
  d.SetConfig({});
  int64_t t = 0;
  d.OnLandmarks(twoFingerHand(0.50f, 0.50f, 0.50f, 0.50f), t);     // enter at t=0, phaseStartMs_=0
  d.OnLandmarks(twoFingerHand(0.50f, 0.50f, 0.50f, 0.50f), t+50);
  // hold gate: at t+100, (100-0) is NOT < 100, so emission enabled. Test t+80 to be strictly within hold.
  auto delta = d.OnLandmarks(twoFingerHand(0.50f, 0.40f, 0.50f, 0.40f), t+80);
  EXPECT_EQ(delta.dy, 0);
  EXPECT_EQ(delta.dx, 0);
}

TEST(ScrollDetector, EmitsHorizontalDeltaOnLateralMotion) {
  // The two-finger scroll gesture should drive both axes. Hand
  // moving right (x increases) must emit a positive hWheel; hand
  // moving left must emit a negative hWheel. The vertical wheel
  // stays zero because the Y midpoint is held constant.
  ScrollDetector d;
  d.SetConfig({});
  int64_t t = 0;
  d.OnLandmarks(twoFingerHand(0.50f, 0.50f, 0.50f, 0.50f), t);     // enter
  d.OnLandmarks(twoFingerHand(0.50f, 0.50f, 0.50f, 0.50f), t+50);
  d.OnLandmarks(twoFingerHand(0.60f, 0.50f, 0.60f, 0.50f), t+100); // midpoint moves right
  auto delta = d.OnLandmarks(twoFingerHand(0.70f, 0.50f, 0.70f, 0.50f), t+150);
  EXPECT_GT(delta.dx, 100);
  EXPECT_EQ(delta.dy, 0);
}

TEST(ScrollDetector, EmitsNegativeHorizontalOnLeftMotion) {
  ScrollDetector d;
  d.SetConfig({});
  int64_t t = 0;
  d.OnLandmarks(twoFingerHand(0.50f, 0.50f, 0.50f, 0.50f), t);
  d.OnLandmarks(twoFingerHand(0.50f, 0.50f, 0.50f, 0.50f), t+50);
  d.OnLandmarks(twoFingerHand(0.40f, 0.50f, 0.40f, 0.50f), t+100); // midpoint moves left
  auto delta = d.OnLandmarks(twoFingerHand(0.30f, 0.50f, 0.30f, 0.50f), t+150);
  EXPECT_LT(delta.dx, -100);
  EXPECT_EQ(delta.dy, 0);
}

TEST(ScrollDetector, MidpointCancelsFingerSeparation) {
  // A slow spread between the two fingers should NOT produce
  // scroll delta. The midpoint of (8) and (12) is what we track;
  // a 0.1 symmetric separation around the midpoint leaves the
  // midpoint stationary, so the wheel deltas stay zero.
  ScrollDetector d;
  d.SetConfig({});
  int64_t t = 0;
  d.OnLandmarks(twoFingerHand(0.50f, 0.50f, 0.50f, 0.50f), t);     // enter
  d.OnLandmarks(twoFingerHand(0.50f, 0.50f, 0.50f, 0.50f), t+50);
  // Spread fingers apart symmetrically: 8 moves up by 0.05, 12 moves down by 0.05
  d.OnLandmarks(twoFingerHand(0.50f, 0.45f, 0.50f, 0.55f), t+100);
  auto delta = d.OnLandmarks(twoFingerHand(0.50f, 0.40f, 0.50f, 0.60f), t+150);
  // The midpoint y is still 0.50 both frames -> no vertical wheel.
  EXPECT_EQ(delta.dy, 0);
  EXPECT_EQ(delta.dx, 0);
}
