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

// Variant that sets the thumb-middle distance independently of
// thumb-index, so middle-click tests can isolate the new
// detector. When `thumbMiddleDist` is 1.0 the middle fingertip is
// far away (no middle pinch) so left-click tests can ignore it.
static vmosue::HandLandmarks makeHandBoth(float thumbIndexDist,
                                          float thumbMiddleDist) {
  vmosue::HandLandmarks lm{};
  lm.points[4]  = {0.0f, 0.0f, 0.0f};
  lm.points[8]  = {thumbIndexDist,  0.0f, 0.0f};
  lm.points[12] = {thumbMiddleDist, 0.0f, 0.0f};
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

// Middle-click: thumb-middle pinch then release. The state
// machine runs in parallel with the left-click machine; thumb
// (4) and middle (12) fingertips approaching the thumb tip
// should emit MiddleClick on release.
TEST(ClickDetector, EmitsMiddleClickOnPinchRelease) {
  ClickDetector d;
  d.SetConfig({});
  int64_t t = 0;
  // Thumb-index far apart, thumb-middle far apart: nothing.
  EXPECT_EQ(d.OnLandmarks(makeHandBoth(0.10f, 0.10f), t),
            vmosue::ClickEvent::None);
  // Thumb-middle pinches (middle fingertip moves to thumb tip).
  EXPECT_EQ(d.OnLandmarks(makeHandBoth(0.10f, 0.02f), t + 50),
            vmosue::ClickEvent::None);
  // Release: middle fingertip moves away again.
  EXPECT_EQ(d.OnLandmarks(makeHandBoth(0.10f, 0.10f), t + 100),
            vmosue::ClickEvent::MiddleClick);
}

// Left-click and middle-click state machines must run in
// parallel: a thumb-index pinch on one frame and a thumb-middle
// pinch on the next both emit their respective clicks. The user
// can interleave the two gestures without one cancelling the
// other.
TEST(ClickDetector, LeftAndMiddleClicksCanInterleave) {
  ClickDetector d;
  d.SetConfig({});
  int64_t t = 10000;
  // Left-click sequence.
  d.OnLandmarks(makeHandBoth(0.10f, 0.10f), t);
  d.OnLandmarks(makeHandBoth(0.02f, 0.10f), t + 50);
  EXPECT_EQ(d.OnLandmarks(makeHandBoth(0.10f, 0.10f), t + 100),
            vmosue::ClickEvent::LeftClick);
  // Middle-click sequence, starting from idle.
  d.OnLandmarks(makeHandBoth(0.10f, 0.10f), t + 200);
  d.OnLandmarks(makeHandBoth(0.10f, 0.02f), t + 250);
  EXPECT_EQ(d.OnLandmarks(makeHandBoth(0.10f, 0.10f), t + 300),
            vmosue::ClickEvent::MiddleClick);
}

// Priority: if both state machines fire on the same frame
// (a thumb-index and a thumb-middle release in the same tick),
// the left-click wins. The middle state machine is silenced
// for that frame but resumes on the next.
TEST(ClickDetector, LeftClickWinsPriorityWhenBothReleaseSameFrame) {
  ClickDetector d;
  d.SetConfig({});
  int64_t t = 0;
  // Start both pinches.
  d.OnLandmarks(makeHandBoth(0.02f, 0.02f), t);
  // Release both: left wins.
  EXPECT_EQ(d.OnLandmarks(makeHandBoth(0.10f, 0.10f), t + 50),
            vmosue::ClickEvent::LeftClick);
  // Middle state machine is still in Idle (it never reached
  // Pinching because left-click took the slot), so the next
  // middle pinch should still produce a MiddleClick normally.
  d.OnLandmarks(makeHandBoth(0.10f, 0.02f), t + 100);
  EXPECT_EQ(d.OnLandmarks(makeHandBoth(0.10f, 0.10f), t + 150),
            vmosue::ClickEvent::MiddleClick);
}

// Reset() must clear the middle state machine so a middle
// click does not re-fire on the next gesture after a reset.
TEST(ClickDetector, ResetClearsMiddleState) {
  ClickDetector d;
  d.SetConfig({});
  int64_t t = 0;
  d.OnLandmarks(makeHandBoth(0.10f, 0.02f), t);     // middle pinches
  d.OnLandmarks(makeHandBoth(0.10f, 0.10f), t + 50); // would emit MiddleClick
  d.Reset();
  // After reset, the middle state machine is in Idle. A frame
  // where the thumb-middle is already far apart is not a
  // "release" because we never started pinching.
  EXPECT_EQ(d.OnLandmarks(makeHandBoth(0.10f, 0.10f), t + 100),
            vmosue::ClickEvent::None);
}
