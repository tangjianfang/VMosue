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
  sm.Init({});
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
