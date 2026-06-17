#include <gtest/gtest.h>
#include "gesture/GestureStateMachine.h"

using vmosue::GestureStateMachine;
using vmosue::HandLandmarks;
using vmosue::ClickEvent;

static HandLandmarks rightHandWithPinch(float thumbIndexDist) {
  HandLandmarks lm{};
  lm.handedness = 1;
  // HandLandmarks::points is std::array<Point2F, 21> zero-initialized by the
  // default struct initializer; we only need landmarks 4 (thumb tip) and 8
  // (index tip) for the pinch detector, but the cursor controller also
  // reads landmark 5 (index MCP) so set that to a stable point.
  lm.points[4] = {0.0f, 0.0f, 0.0f};
  lm.points[5] = {0.5f, 0.5f, 0.0f};  // keep pivot stable across frames
  lm.points[8] = {thumbIndexDist, 0.0f, 0.0f};
  return lm;
}

TEST(StateMachine, EmergencyStopSafeReleases) {
  GestureStateMachine sm;
  sm.Init({});
  sm.EmergencyStop();
  EXPECT_EQ(sm.State(), vmosue::GlobalState::EmergencyStopped);
  auto actions = sm.ConsumeActions();
  EXPECT_TRUE(actions.safeRelease);
}

TEST(StateMachine, PauseIgnoresLandmarks) {
  GestureStateMachine sm;
  sm.Init({});
  sm.Pause();
  sm.OnLandmarks({rightHandWithPinch(0.10f)}, 0,   1.0/30.0);
  sm.OnLandmarks({rightHandWithPinch(0.02f)}, 50,  1.0/30.0);
  sm.OnLandmarks({rightHandWithPinch(0.10f)}, 100, 1.0/30.0);
  auto actions = sm.ConsumeActions();
  EXPECT_FALSE(actions.leftClick);
}

TEST(StateMachine, ActiveEmitsClickOnPinch) {
  GestureStateMachine sm;
  sm.Init({});
  sm.Resume();
  sm.OnLandmarks({rightHandWithPinch(0.10f)}, 0,   1.0/30.0);
  sm.OnLandmarks({rightHandWithPinch(0.02f)}, 50,  1.0/30.0);
  sm.OnLandmarks({rightHandWithPinch(0.10f)}, 100, 1.0/30.0);
  auto actions = sm.ConsumeActions();
  EXPECT_TRUE(actions.leftClick);
}

// A right hand that is NOT pinching (thumb/index/middle far apart) but
// whose world index-fingertip depth can be driven for the air-click
// push gesture. world[0] is the wrist; world[8] is the index tip.
static HandLandmarks rightHandPush(float indexZ, float wristZ) {
  HandLandmarks lm{};
  lm.handedness = 1;
  lm.points[4]  = {0.0f, 0.0f, 0.0f};  // thumb tip
  lm.points[5]  = {0.5f, 0.5f, 0.0f};  // index MCP (cursor pivot, stable)
  lm.points[8]  = {0.3f, 0.0f, 0.0f};  // index tip — far from thumb (no pinch)
  lm.points[12] = {0.4f, 0.0f, 0.0f};  // middle tip — far from thumb
  for (auto& w : lm.world) w.z = wristZ;
  lm.world[8].z = indexZ;
  return lm;
}

// Regression for the right-click arbitration: a clean push-toward-
// camera (no competing left/middle pinch) must still emit a right
// click. The arbitration only suppresses the right click when a
// left/middle button event fires in the SAME frame; the happy path
// must be untouched.
TEST(StateMachine, EmitsRightClickOnCleanPush) {
  GestureStateMachine sm;
  // v0.6.2: disable the "first hand seen" grace gate for this
  // test. The test runs the simulation starting at ts=1000ms,
  // which is well inside the 1500ms default grace window — the
  // push-gesture right-click would be suppressed. Disabling
  // firstHandGraceMs isolates the right-click detector from
  // the grace gate, which is what this test is about.
  GestureStateMachine::Config cfg;
  cfg.firstHandGraceMs = 0;
  sm.Init(cfg);
  sm.Resume();
  sm.OnLandmarks({rightHandPush(0.0f, 0.0f)}, 1000, 1.0/30.0);
  sm.OnLandmarks({rightHandPush(-0.05f, 0.0f)}, 1050, 1.0/30.0);
  sm.OnLandmarks({rightHandPush(0.0f, 0.0f)}, 1150, 1.0/30.0);
  auto actions = sm.ConsumeActions();
  EXPECT_TRUE(actions.rightClick);
  EXPECT_FALSE(actions.leftClick);
}

TEST(StateMachine, TracksBothHands) {
  GestureStateMachine sm;
  sm.Init({});
  std::vector<vmosue::HandLandmarks> both;
  vmosue::HandLandmarks l{}; l.handedness = 0;
  vmosue::HandLandmarks r{}; r.handedness = 1;
  both.push_back(l); both.push_back(r);
  sm.OnLandmarks(both, 0, 1.0/30.0);
  SUCCEED();
}

// ---- Task 21: two-hand-open emergency stop ----

static vmosue::HandLandmarks openHand(int handedness) {
  vmosue::HandLandmarks lm{};
  lm.handedness = handedness;
  // Fingertips above MCPs in y (image origin top-left, so smaller y = up).
  lm.points[5]  = {0.5f, 0.7f, 0.0f};  // index MCP
  lm.points[8]  = {0.5f, 0.5f, 0.0f};  // index tip
  lm.points[9]  = {0.5f, 0.7f, 0.0f};  // middle MCP
  lm.points[12] = {0.5f, 0.5f, 0.0f};  // middle tip
  lm.points[13] = {0.5f, 0.7f, 0.0f};  // ring MCP
  lm.points[16] = {0.5f, 0.5f, 0.0f};  // ring tip
  lm.points[17] = {0.5f, 0.7f, 0.0f};  // pinky MCP
  lm.points[20] = {0.5f, 0.5f, 0.0f};  // pinky tip
  return lm;
}

static vmosue::HandLandmarks closedHand(int handedness) {
  vmosue::HandLandmarks lm{};
  lm.handedness = handedness;
  // Fingertips below MCPs (fist).
  lm.points[5]  = {0.5f, 0.5f, 0.0f};
  lm.points[8]  = {0.5f, 0.7f, 0.0f};
  lm.points[9]  = {0.5f, 0.5f, 0.0f};
  lm.points[12] = {0.5f, 0.7f, 0.0f};
  lm.points[13] = {0.5f, 0.5f, 0.0f};
  lm.points[16] = {0.5f, 0.7f, 0.0f};
  lm.points[17] = {0.5f, 0.5f, 0.0f};
  lm.points[20] = {0.5f, 0.7f, 0.0f};
  return lm;
}

TEST(StateMachineTwoHandOpen, TripsEmergencyAfterHoldMs) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.twoHandOpenHoldMs = 500;
  sm.Init(cfg);
  std::vector<vmosue::HandLandmarks> both;
  both.push_back(openHand(0));  // left
  both.push_back(openHand(1));  // right
  // Both open, but not yet 500 ms held.
  sm.OnLandmarks(both, 0,   1.0/30.0);
  sm.OnLandmarks(both, 200, 1.0/30.0);
  EXPECT_NE(sm.State(), vmosue::GlobalState::EmergencyStopped);
  // Cross the threshold.
  sm.OnLandmarks(both, 600, 1.0/30.0);
  EXPECT_EQ(sm.State(), vmosue::GlobalState::EmergencyStopped);
  auto actions = sm.ConsumeActions();
  EXPECT_TRUE(actions.safeRelease);
}

TEST(StateMachineTwoHandOpen, ResetsWhenHandCloses) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.twoHandOpenHoldMs = 500;
  sm.Init(cfg);
  std::vector<vmosue::HandLandmarks> bothOpen;
  bothOpen.push_back(openHand(0));
  bothOpen.push_back(openHand(1));

  // Open for 300 ms (under threshold) then close the other hand.
  sm.OnLandmarks(bothOpen, 0,   1.0/30.0);
  sm.OnLandmarks(bothOpen, 300, 1.0/30.0);
  std::vector<vmosue::HandLandmarks> oneClosed;
  oneClosed.push_back(closedHand(0));
  oneClosed.push_back(openHand(1));
  sm.OnLandmarks(oneClosed, 350, 1.0/30.0);

  // Now re-open the other hand and check the timer restarts.
  sm.OnLandmarks(bothOpen, 400, 1.0/30.0);
  // 300 ms after re-opening -- still under threshold.
  sm.OnLandmarks(bothOpen, 700, 1.0/30.0);
  EXPECT_NE(sm.State(), vmosue::GlobalState::EmergencyStopped);
  // Cross the 500 ms hold from the re-open (400 ms timestamp).
  sm.OnLandmarks(bothOpen, 950, 1.0/30.0);
  EXPECT_EQ(sm.State(), vmosue::GlobalState::EmergencyStopped);
}

TEST(StateMachineTwoHandOpen, IgnoredWhenOnlyOneHand) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.twoHandOpenHoldMs = 500;
  sm.Init(cfg);
  std::vector<vmosue::HandLandmarks> onlyRight;
  onlyRight.push_back(openHand(1));
  for (int i = 0; i <= 15; ++i) {
    sm.OnLandmarks(onlyRight, i * 100, 1.0/30.0);
  }
  EXPECT_NE(sm.State(), vmosue::GlobalState::EmergencyStopped);
}

TEST(StateMachineTwoHandOpen, DoesNotRefireWhileEmergencyStopped) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.twoHandOpenHoldMs = 500;
  sm.Init(cfg);
  std::vector<vmosue::HandLandmarks> both;
  both.push_back(openHand(0));
  both.push_back(openHand(1));
  // Trip it once.
  sm.OnLandmarks(both, 0,   1.0/30.0);
  sm.OnLandmarks(both, 600, 1.0/30.0);
  EXPECT_EQ(sm.State(), vmosue::GlobalState::EmergencyStopped);
  // Subsequent frames must not change state nor attempt to re-fire.
  sm.OnLandmarks(both, 1200, 1.0/30.0);
  sm.OnLandmarks(both, 2000, 1.0/30.0);
  EXPECT_EQ(sm.State(), vmosue::GlobalState::EmergencyStopped);
}

// v0.6.2: "first hand seen" grace period. The user reported
// "现在我随便一动，它就瞎乱点" — micro-movements of the fingers
// while the user settles into a comfortable pinch pose on
// first appearance used to fire a click within ~50ms. The
// grace gate suppresses every action publication for
// cfg.firstHandGraceMs after a hand first appears (or
// re-appears after >= 1s of absence), while still letting the
// cursor move so the user has visual feedback that the system
// sees their hand.
//
// To make the test fast, we set firstHandGraceMs = 200 and
// dwellMs = 0 (immediate-fire legacy). The test is therefore
// fully deterministic on the ms timestamp alone.

TEST(StateMachine, FirstHandGraceSuppressesClickDuringSettleIn) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.firstHandGraceMs = 200;  // short window for the test
  cfg.dwellMs = 0;             // no dwell delay, isolate the grace gate
  cfg.handednessRight = true;
  sm.Init(cfg);

  // Frame 0: hand appears, immediately tries to pinch (the
  // "settling" pose the user described). Click MUST NOT fire.
  std::vector<vmosue::HandLandmarks> right{rightHandWithPinch(0.02f)};
  sm.OnLandmarks(right, /*ts=*/0, 1.0/30.0);
  auto a = sm.ConsumeActions();
  EXPECT_FALSE(a.leftClick)
      << "click during grace window — this is the user's bug";

  // Frames 1-5 (0-150ms in 33ms steps): still inside the
  // 200ms grace window. Clicks must continue to be
  // suppressed even though the pinch is held.
  for (int ts = 33; ts <= 165; ts += 33) {
    sm.OnLandmarks(right, ts, 1.0/30.0);
  }
  a = sm.ConsumeActions();
  EXPECT_FALSE(a.leftClick)
      << "click within grace window at ts=165";

  // Frame at 200ms: grace just ended. Pinch held for
  // ~200ms — release it now to see if a click fires.
  // Use the detector's "release" branch: lift the pinch.
  std::vector<vmosue::HandLandmarks> released{rightHandWithPinch(0.10f)};
  sm.OnLandmarks(released, /*ts=*/200, 1.0/30.0);
  a = sm.ConsumeActions();
  // The grace gate only suppresses during the window. After
  // 200ms, the gesture is allowed to fire normally. So a
  // release after the grace window SHOULD fire a click.
  EXPECT_TRUE(a.leftClick)
      << "post-grace release must fire — the grace gate is a "
         "settle-in window, not a permanent block";
}

TEST(StateMachine, FirstHandGraceReArmsOnLongAbsence) {
  // If the user takes their hand out for >1s and brings it
  // back, the grace gate must re-arm so a fresh settle-in
  // window applies. Without this the second session would
  // feel jumpy in exactly the way the first session did.
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.firstHandGraceMs = 200;
  cfg.dwellMs = 0;
  sm.Init(cfg);

  std::vector<vmosue::HandLandmarks> right{rightHandWithPinch(0.02f)};
  std::vector<vmosue::HandLandmarks> released{rightHandWithPinch(0.10f)};

  // First session: settle + release normally.
  sm.OnLandmarks(right, 0, 1.0/30.0);
  sm.OnLandmarks(released, 200, 1.0/30.0);
  auto a = sm.ConsumeActions();
  EXPECT_TRUE(a.leftClick) << "first session click";

  // 2s gap (well over the 1s re-arm threshold).
  sm.OnLandmarks({}, 2200, 1.0/30.0);

  // Second session: hand re-appears at 2233ms with a pinch.
  // Grace must re-arm, so this click MUST NOT fire.
  sm.OnLandmarks(right, 2233, 1.0/30.0);
  a = sm.ConsumeActions();
  EXPECT_FALSE(a.leftClick)
      << "grace did not re-arm after 2s absence — second session "
         "feels as jumpy as the first";
}
