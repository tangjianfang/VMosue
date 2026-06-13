#include <gtest/gtest.h>
#include "gesture/ScrollDetector.h"

using vmosue::ScrollDetector;

static vmosue::HandLandmarks twoFingerHand(float indexY, float middleY) {
  vmosue::HandLandmarks lm{};
  lm.handedness = 0;
  lm.points[8] = {0.5f, indexY, 0.0f};
  lm.points[12] = {0.5f, middleY, 0.0f};
  return lm;
}

TEST(ScrollDetector, EmitsWheelOnUpwardMotion) {
  ScrollDetector d;
  d.SetConfig({});
  int64_t t = 0;
  d.OnLandmarks(twoFingerHand(0.50f, 0.50f), t);     // enter (low delta)
  d.OnLandmarks(twoFingerHand(0.50f, 0.50f), t+50);
  d.OnLandmarks(twoFingerHand(0.40f, 0.40f), t+100); // index moves up
  auto delta = d.OnLandmarks(twoFingerHand(0.30f, 0.30f), t+150);
  EXPECT_GT(delta, 0);
}
