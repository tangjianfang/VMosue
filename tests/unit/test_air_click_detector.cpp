#include <gtest/gtest.h>
#include "gesture/AirClickDetector.h"

using vmosue::AirClickDetector;
using vmosue::AirClickEvent;

static vmosue::HandLandmarks makeHand(float indexZ, float wristZ) {
  vmosue::HandLandmarks lm{};
  for (auto& p : lm.world) { p.z = wristZ; }
  lm.world[8].z = indexZ;
  return lm;
}

TEST(AirClickDetector, EmitsRightClickOnApproachRetreat) {
  AirClickDetector d;
  d.SetConfig({});
  int64_t t = 1000;
  EXPECT_EQ(d.OnLandmarks(makeHand(0.0f, 0.0f), t), AirClickEvent::None);
  EXPECT_EQ(d.OnLandmarks(makeHand(-0.05f, 0.0f), t + 50), AirClickEvent::None);
  EXPECT_EQ(d.OnLandmarks(makeHand(0.0f, 0.0f), t + 150), AirClickEvent::RightClick);
}

TEST(AirClickDetector, RespectsCooldown) {
  AirClickDetector d;
  d.SetConfig({});
  int64_t t = 1000;
  d.OnLandmarks(makeHand(0.0f, 0.0f), t);
  d.OnLandmarks(makeHand(-0.05f, 0.0f), t + 50);
  d.OnLandmarks(makeHand(0.0f, 0.0f), t + 150);  // first click
  EXPECT_EQ(d.OnLandmarks(makeHand(0.0f, 0.0f), t + 200), AirClickEvent::None);
}

// Regression: a hand whose world landmarks are all-zero is a
// degenerate input (model produced no metric coords, or the hand was
// not really detected). The detector must not interpret the implied
// "index z == wrist z == 0" as a valid resting pose that a later
// frame could push out of — an all-zero frame should be ignored, not
// advance the state machine.
TEST(AirClickDetector, IgnoresAllZeroWorldLandmarks) {
  AirClickDetector d;
  d.SetConfig({});
  vmosue::HandLandmarks zero{};  // every world point is (0,0,0)
  int64_t t = 1000;
  EXPECT_EQ(d.OnLandmarks(zero, t), AirClickEvent::None);
  // A genuine push immediately after must still register a click, i.e.
  // the all-zero frame did not corrupt the phase.
  EXPECT_EQ(d.OnLandmarks(makeHand(0.0f, 0.0f), t + 10), AirClickEvent::None);
  EXPECT_EQ(d.OnLandmarks(makeHand(-0.05f, 0.0f), t + 60), AirClickEvent::None);
  EXPECT_EQ(d.OnLandmarks(makeHand(0.0f, 0.0f), t + 160),
            AirClickEvent::RightClick);
}