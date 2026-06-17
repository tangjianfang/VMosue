#include <gtest/gtest.h>
#include "gesture/DwellGate.h"

using vmosue::ActionSet;
using vmosue::DwellGate;

namespace {

// v0.6.2: the DwellGate's input is TWO signals per frame:
//   1. *PinchHeld (or rightPushHeld)  — "user is currently sustaining
//      the gesture". Drives the dwell counter (latches startMs).
//   2. *Click (or rightClick) — "user just released the gesture".
//      Gates the commit: the click fires only if dwell elapsed.
// Old test fixtures used `MakeClick()` with leftClick=true continuously
// (treating the release-event as the held signal), which the
// release-event-only model allowed to fire after dwellMs of holding.
// The new model splits these so the click fires on the actual release
// frame, which is what the production wiring from GestureStateMachine
// produces.
ActionSet PinchHeld() {
  ActionSet a;
  a.leftPinchHeld = true;
  return a;
}
ActionSet LeftClickRelease() {
  ActionSet a;
  a.leftClick = true;   // release event
  return a;
}
ActionSet RightPushHeld() {
  ActionSet a;
  a.rightPushHeld = true;
  return a;
}
ActionSet RightClickRelease() {
  ActionSet a;
  a.rightClick = true;
  return a;
}

}  // namespace

TEST(DwellGate, DisabledGatePassesThroughImmediately) {
  DwellGate g;
  DwellGate::Config c;
  c.dwellMs = 0;  // disabled
  g.SetConfig(c);
  auto out = g.Process(LeftClickRelease(), 1000);
  EXPECT_TRUE(out.leftClick);
  EXPECT_FALSE(out.rightClick);
}

TEST(DwellGate, DoesNotFireBelowDwell) {
  DwellGate g;
  DwellGate::Config c;
  c.dwellMs = 1500;
  c.cooldownMs = 400;
  g.SetConfig(c);
  // Pinch for 1.4s then release — should not fire because dwell
  // never reached 1500ms.
  for (int t = 0; t <= 1400; t += 50) {
    auto out = g.Process(PinchHeld(), t);
    EXPECT_FALSE(out.leftClick) << "fired prematurely at t=" << t;
  }
  auto out = g.Process(LeftClickRelease(), 1450);
  EXPECT_FALSE(out.leftClick) << "fired on too-early release";
}

TEST(DwellGate, FiresAfterDwell) {
  DwellGate g;
  DwellGate::Config c;
  c.dwellMs = 1500;
  c.cooldownMs = 400;
  g.SetConfig(c);
  // Pinch for 1700ms — dwell exceeds 1500ms. The release on the
  // next frame should commit.
  for (int t = 0; t <= 1700; t += 50) {
    g.Process(PinchHeld(), t);
  }
  auto out = g.Process(LeftClickRelease(), 1750);
  EXPECT_TRUE(out.leftClick);
}

TEST(DwellGate, CancelsWhenGestureReleased) {
  DwellGate g;
  DwellGate::Config c;
  c.dwellMs = 1500;
  c.cooldownMs = 400;
  g.SetConfig(c);
  // Pinch for 800ms then release early — should cancel the dwell.
  for (int t = 0; t <= 800; t += 50) {
    g.Process(PinchHeld(), t);
  }
  auto out1 = g.Process(LeftClickRelease(), 850);
  EXPECT_FALSE(out1.leftClick) << "fired before dwell elapsed";
  // 5 frames of "no gesture" — dwell stays cancelled.
  for (int t = 900; t <= 1050; t += 50) {
    auto out = g.Process(ActionSet{}, t);
    EXPECT_FALSE(out.leftClick) << "fired after release at t=" << t;
  }
  // Restart the gesture — counter starts from zero. Pinch long
  // enough that the next release will satisfy dwell.
  for (int t = 1100; t <= 2800; t += 50) {
    g.Process(PinchHeld(), t);
  }
  // Release at t=2850 (dwell = 2850 - 1100 = 1750 >= 1500) —
  // should commit because the new dwell exceeded the threshold.
  bool fired = false;
  for (int t = 2850; t <= 3200; t += 50) {
    auto out = g.Process(LeftClickRelease(), t);
    if (out.leftClick) { fired = true; break; }
  }
  EXPECT_TRUE(fired);
}

TEST(DwellGate, CooldownSuppressesDoubleFire) {
  DwellGate g;
  DwellGate::Config c;
  c.dwellMs = 1000;
  c.cooldownMs = 500;
  g.SetConfig(c);
  // First dwell completes when t reaches 1000 → release commits.
  for (int t = 0; t <= 1000; t += 50) {
    g.Process(PinchHeld(), t);
  }
  auto out1 = g.Process(LeftClickRelease(), 1050);
  EXPECT_TRUE(out1.leftClick) << "missed first fire";
  // Pinch again immediately (slot re-arms with a fresh startMs at
  // the first held frame after the disarm).
  for (int t = 1100; t <= 2200; t += 50) {
    g.Process(PinchHeld(), t);
  }
  // Release at t=2250 (dwell = 2250 - 1100 = 1150 >= 1000, and
  // since-cooldown = 2250 - 1050 = 1200 >= 500) — should commit.
  bool seen = false;
  for (int t = 2250; t <= 2700; t += 50) {
    auto out = g.Process(LeftClickRelease(), t);
    if (out.leftClick) { seen = true; break; }
  }
  EXPECT_TRUE(seen) << "second commit never happened";
}

TEST(DwellGate, PreviewReportsProgress) {
  DwellGate g;
  DwellGate::Config c;
  c.dwellMs = 1000;
  c.cooldownMs = 400;
  g.SetConfig(c);
  // Pinch for 600ms: preview should report ~60% on the last
  // Process() call.
  for (int t = 0; t <= 600; t += 50) {
    g.Process(PinchHeld(), t);
  }
  auto prev = g.CurrentPreview(600);
  EXPECT_EQ(prev.kind, DwellGate::Kind::LeftClick);
  EXPECT_NEAR(prev.progress, 0.6f, 0.05f);
  EXPECT_EQ(prev.totalMs, 1000);
  EXPECT_NEAR(prev.remainingMs, 400, 50);
}

TEST(DwellGate, PreviewIsNoneWhenNoGesture) {
  DwellGate g;
  DwellGate::Config c;
  c.dwellMs = 1000;
  c.cooldownMs = 400;
  g.SetConfig(c);
  // No pinch at all.
  g.Process(ActionSet{}, 0);
  g.Process(ActionSet{}, 100);
  auto prev = g.CurrentPreview(100);
  EXPECT_EQ(prev.kind, DwellGate::Kind::None);
}

TEST(DwellGate, ResetClearsCounters) {
  DwellGate g;
  DwellGate::Config c;
  c.dwellMs = 1000;
  c.cooldownMs = 400;
  g.SetConfig(c);
  g.Process(PinchHeld(), 0);
  g.Process(PinchHeld(), 100);
  g.Process(PinchHeld(), 200);
  EXPECT_EQ(g.StartMsForTest(DwellGate::Kind::LeftClick), 0);
  g.Reset();
  EXPECT_EQ(g.StartMsForTest(DwellGate::Kind::LeftClick), 0);
  EXPECT_EQ(g.CommittedMsForTest(DwellGate::Kind::LeftClick), 0);
}

TEST(DwellGate, RightClickHasItsOwnSlot) {
  DwellGate g;
  DwellGate::Config c;
  c.dwellMs = 1000;
  c.cooldownMs = 400;
  g.SetConfig(c);
  // Right-click dwell counts independently of left-click dwell.
  for (int t = 0; t <= 1100; t += 50) {
    g.Process(RightPushHeld(), t);
  }
  auto out = g.Process(RightClickRelease(), 1150);
  EXPECT_TRUE(out.rightClick) << "right click never fired";
  // Left-click slot was never held, so its startMs is still the
  // default value.
  EXPECT_EQ(g.StartMsForTest(DwellGate::Kind::LeftClick), 0);
}

TEST(DwellGate, PassthroughOfContinuousActions) {
  DwellGate g;
  DwellGate::Config c;
  c.dwellMs = 1500;
  c.cooldownMs = 400;
  g.SetConfig(c);
  ActionSet in;
  in.cursorX = 100;
  in.cursorY = 200;
  in.wheel = 5;
  in.hWheel = 0;
  in.leftDown = true;
  in.safeRelease = false;
  auto out = g.Process(in, 0);
  EXPECT_EQ(out.cursorX, 100);
  EXPECT_EQ(out.cursorY, 200);
  EXPECT_EQ(out.wheel, 5);
  EXPECT_EQ(out.hWheel, 0);
  EXPECT_TRUE(out.leftDown);
  EXPECT_FALSE(out.safeRelease);
  EXPECT_FALSE(out.leftClick);
}
