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