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
