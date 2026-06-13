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
  // Start at non-zero t to ensure the regression where the very first
  // click was treated as a double-click (because lastClickMs_ defaulted
  // to 0) cannot return silently.
  int64_t t = 10000;
  d.OnLandmarks(makeHand(0.10f), t);
  d.OnLandmarks(makeHand(0.02f), t + 50);
  d.OnLandmarks(makeHand(0.10f), t + 100);  // first click
  d.OnLandmarks(makeHand(0.10f), t + 200);
  d.OnLandmarks(makeHand(0.02f), t + 250);
  EXPECT_EQ(d.OnLandmarks(makeHand(0.10f), t + 300), ClickEvent::LeftDoubleClick);
}

// Regression: with the old `int64_t lastClickMs_ = 0;` default, the very
// first click at t=0 had isDouble = (0 - 0) < 400 == true, so it emitted
// LeftDoubleClick instead of LeftClick. With std::optional, "never clicked"
// is explicit, and the first click is correctly a LeftClick.
TEST(ClickDetector, FirstClickIsNotDoubleClickEvenAtT0) {
  ClickDetector d;
  d.SetConfig({});
  int64_t t = 0;
  d.OnLandmarks(makeHand(0.10f), t);
  d.OnLandmarks(makeHand(0.02f), t + 50);
  EXPECT_EQ(d.OnLandmarks(makeHand(0.10f), t + 100), ClickEvent::LeftClick);
}

// Regression: Reset() must clear lastClickMs_ so a click that follows
// the reset within the doubleClickWindowMs is a fresh LeftClick, not
// paired with whatever was last stored before the reset.
TEST(ClickDetector, ResetClearsLastClick) {
  ClickDetector d;
  d.SetConfig({});
  int64_t t = 10000;
  d.OnLandmarks(makeHand(0.10f), t);
  d.OnLandmarks(makeHand(0.02f), t + 50);
  d.OnLandmarks(makeHand(0.10f), t + 100);  // first click
  d.Reset();
  // After reset, a new click within the doubleClick window should be
  // a fresh single click.
  d.OnLandmarks(makeHand(0.10f), t + 200);
  d.OnLandmarks(makeHand(0.02f), t + 250);
  EXPECT_EQ(d.OnLandmarks(makeHand(0.10f), t + 300), ClickEvent::LeftClick);
}

TEST(ClickDetector, EmitsDragStartOnSustainedPinch) {
  ClickDetector d;
  d.SetConfig({});
  int64_t t = 0;
  d.OnLandmarks(makeHand(0.10f), t);
  d.OnLandmarks(makeHand(0.02f), t + 50);
  EXPECT_EQ(d.OnLandmarks(makeHand(0.02f), t + 300), ClickEvent::LeftDragStart);
}

TEST(ClickDetector, EmitsDragStartOnLongPinch) {
  ClickDetector d;
  d.SetConfig({});
  int64_t t = 0;
  d.OnLandmarks(makeHand(0.10f), t);
  d.OnLandmarks(makeHand(0.02f), t + 50);
  EXPECT_EQ(d.OnLandmarks(makeHand(0.02f), t + 300), vmosue::ClickEvent::LeftDragStart);
  EXPECT_EQ(d.OnLandmarks(makeHand(0.10f), t + 400), vmosue::ClickEvent::LeftDragEnd);
}

TEST(ClickDetector, SuppressSingleInDoubleWindow) {
  ClickDetector d;
  vmosue::ClickDetector::Config c;
  c.suppressSingleClickInDoubleWindow = true;
  d.SetConfig(c);
  int64_t t = 0;
  d.OnLandmarks(makeHand(0.10f), t);
  d.OnLandmarks(makeHand(0.02f), t + 50);
  EXPECT_EQ(d.OnLandmarks(makeHand(0.10f), t + 100), vmosue::ClickEvent::None);  // suppressed
  d.OnLandmarks(makeHand(0.10f), t + 200);
  d.OnLandmarks(makeHand(0.02f), t + 250);
  EXPECT_EQ(d.OnLandmarks(makeHand(0.10f), t + 300), vmosue::ClickEvent::LeftDoubleClick);
}
