#include <gtest/gtest.h>
#include "gesture/ClickDetector.h"

using vmosue::ClickDetector;
using vmosue::ClickEvent;

static vmosue::HandLandmarks makeHand(float thumbIndexDist) {
  vmosue::HandLandmarks lm{};
  lm.points[4] = {0.0f, 0.0f, 0.0f};
  lm.points[8] = {thumbIndexDist, 0.0f, 0.0f};
  return lm;
}

TEST(ClickDetector, EmitsClickOnPinchRelease) {
  ClickDetector d;
  d.SetConfig({});
  int64_t t = 0;
  EXPECT_EQ(d.OnLandmarks(makeHand(0.10f), t), ClickEvent::None);
  EXPECT_EQ(d.OnLandmarks(makeHand(0.02f), t + 50), ClickEvent::None);
  EXPECT_EQ(d.OnLandmarks(makeHand(0.10f), t + 100), ClickEvent::LeftClick);
}

TEST(ClickDetector, EmitsDoubleClickOnTwoQuickPinches) {
  ClickDetector d;
  d.SetConfig({});
  int64_t t = 0;
  d.OnLandmarks(makeHand(0.10f), t);
  d.OnLandmarks(makeHand(0.02f), t + 50);
  d.OnLandmarks(makeHand(0.10f), t + 100);  // first click
  d.OnLandmarks(makeHand(0.10f), t + 200);
  d.OnLandmarks(makeHand(0.02f), t + 250);
  EXPECT_EQ(d.OnLandmarks(makeHand(0.10f), t + 300), ClickEvent::LeftDoubleClick);
}

TEST(ClickDetector, EmitsDownOnSustainedPinch) {
  ClickDetector d;
  d.SetConfig({});
  int64_t t = 0;
  d.OnLandmarks(makeHand(0.10f), t);
  d.OnLandmarks(makeHand(0.02f), t + 50);
  EXPECT_EQ(d.OnLandmarks(makeHand(0.02f), t + 300), ClickEvent::LeftDown);
}
