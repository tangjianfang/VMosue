#include <gtest/gtest.h>
#include "gesture/DwellGate.h"

using vmosue::ActionSet;
using vmosue::DwellGate;

namespace {

ActionSet MakeClick() {
  ActionSet a;
  a.leftClick = true;
  return a;
}
ActionSet MakeRightClick() {
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
  auto out = g.Process(MakeClick(), 1000);
  EXPECT_TRUE(out.leftClick);
  EXPECT_FALSE(out.rightClick);
}

TEST(DwellGate, DoesNotFireBelowDwell) {
  DwellGate g;
  DwellGate::Config c;
  c.dwellMs = 1500;
  c.cooldownMs = 400;
  g.SetConfig(c);
  // Pinch for 1.4s total — should not fire.
  for (int t = 0; t <= 1400; t += 50) {
    auto out = g.Process(MakeClick(), t);
    EXPECT_FALSE(out.leftClick) << "fired prematurely at t=" << t;
  }
}

TEST(DwellGate, FiresAfterDwell) {
  DwellGate g;
  DwellGate::Config c;
  c.dwellMs = 1500;
  c.cooldownMs = 400;
  g.SetConfig(c);
  bool fired = false;
  for (int t = 0; t < 1700; t += 50) {
    auto out = g.Process(MakeClick(), t);
    if (out.leftClick) fired = true;
  }
  EXPECT_TRUE(fired);
}

TEST(DwellGate, CancelsWhenGestureReleased) {
  DwellGate g;
  DwellGate::Config c;
  c.dwellMs = 1500;
  c.cooldownMs = 400;
  g.SetConfig(c);
  // Pinch for 800ms then release.
  for (int t = 0; t <= 800; t += 50) {
    g.Process(MakeClick(), t);
  }
  // 5 frames of "no click" — the dwell should be cancelled.
  for (int t = 850; t <= 1050; t += 50) {
    auto out = g.Process(ActionSet{}, t);
    EXPECT_FALSE(out.leftClick) << "fired after release at t=" << t;
  }
  // Restart the gesture and try again — counter should start from
  // zero, not from 800.
  for (int t = 1100; t <= 2500; t += 50) {
    auto out = g.Process(MakeClick(), t);
    if (t < 2600) {
      // Not enough dwell yet.
      if (out.leftClick) {
        FAIL() << "fired too early after restart at t=" << t;
      }
    }
  }
}

TEST(DwellGate, CooldownSuppressesDoubleFire) {
  DwellGate g;
  DwellGate::Config c;
  c.dwellMs = 1000;
  c.cooldownMs = 500;
  g.SetConfig(c);
  // First dwell completes at t=1000 → fires.
  for (int t = 0; t <= 1050; t += 50) {
    auto out = g.Process(MakeClick(), t);
    if (t >= 1000 && t < 1050) {
      EXPECT_TRUE(out.leftClick) << "missed first fire at t=" << t;
    }
  }
  // Pinch again immediately: counter starts at 0 again (startMs
  // was reset on commit). After 1000ms of new pinch the second
  // fire is allowed.
  for (int t = 1100; t <= 1600; t += 50) {
    auto out = g.Process(MakeClick(), t);
    if (t - 1100 < 1000) {
      EXPECT_FALSE(out.leftClick)
          << "fired during cooldown at t=" << t;
    }
  }
  // After 1500ms total of second pinch (well past dwell), the
  // second commit is allowed because committedMs is 1000 and
  // (t - 1000) >= 500 (cooldown).
  bool seen = false;
  for (int t = 2100; t <= 2500; t += 50) {
    auto out = g.Process(MakeClick(), t);
    if (out.leftClick) { seen = true; break; }
  }
  EXPECT_TRUE(seen);
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
    g.Process(MakeClick(), t);
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
  g.Process(MakeClick(), 0);
  g.Process(MakeClick(), 100);
  g.Process(MakeClick(), 200);
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
  bool fired = false;
  for (int t = 0; t <= 1100; t += 50) {
    auto out = g.Process(MakeRightClick(), t);
    if (out.rightClick) fired = true;
  }
  EXPECT_TRUE(fired) << "right click never fired";
  // Left-click slot is still 0 (never seen) — its startMs is
  // still the default value because we never asserted it.
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
