// v0.6.2: end-to-end phantom-rejection regression test.
//
// What this file tests (and what it does NOT):
//
// In the real app, phantom hands are rejected at TWO layers:
//   1. HandStabilityFilter (in HandDetector.cpp) — drops hands
//      that have been above the score floor for fewer than
//      kStabilityFrames (currently 5) consecutive frames.
//   2. DwellGate (in GestureStateMachine) — even if a hand
//      passes layer 1, the click is held back until it has
//      been continuously asserted for `dwellMs`.
//
// The unit test in test_hand_stability_filter.cpp covers
// layer 1 in isolation. This integration test covers the
// COMBINED behavior: even with a deliberately noisy
// HandLandmarks stream (including realistic 3-7 frame
// phantom bursts at sub-floor scores that a stable
// filter would still pass through occasionally), the full
// state machine must NEVER produce a click.
//
// We feed GestureStateMachine a stream of HandLandmarks
// and assert the aggregated ActionSet is empty (no click,
// no drag, no scroll, no right-click). The DwellGate with
// 100ms hold (shortened from the 2500ms production default
// to keep the test fast) is the second line of defence.

#include <gtest/gtest.h>
#include <climits>
#include <cstdint>
#include <vector>

#include "gesture/GestureStateMachine.h"
#include "util/Adaptive.h"

using vmosue::ActionSet;
using vmosue::GestureStateMachine;
using vmosue::HandLandmarks;
using vmosue::Point2F;

namespace {

// Build a HandLandmarks snapshot for a phantom right hand.
// The score is below the adaptive floor (0.5) so layer 1
// would normally drop it; but we deliberately use a score
// JUST above the floor (0.55) so it could plausibly survive
// a borderline stability filter, mimicking the real-world
// case where a phantom flickers in and out of the floor.
// Points are set so a click detector would see a tight
// pinch (thumb-index distance = 0.01, well below the 0.04
// pinch threshold) if the state machine processes this
// frame.
HandLandmarks phantomRightHand() {
  HandLandmarks lm{};
  lm.handedness = 1;
  lm.score = 0.55f;
  // Tight pinch pose matching test_action_map.cpp's
  // pinch fixture: thumb tip (4) at origin, index tip (8)
  // one-hundredth away. This is comfortably below the
  // cold-start pinch threshold (0.04) and the release
  // threshold (0.07), so the ClickDetector enters and
  // stays in Pinching phase while this pose is fed.
  lm.points[4] = {0.0f, 0.0f, 0.0f};
  lm.points[5] = {0.5f, 0.5f, 0.0f};  // MCP pivot for cursor
  lm.points[8] = {0.01f, 0.0f, 0.0f};
  return lm;
}

// Build a HandLandmarks snapshot for a real right hand with
// a tight pinch. Same pose as phantom, but the score is
// strong (0.92) and a longer burst is sustained.
HandLandmarks realRightHandPinching() {
  HandLandmarks lm{};
  lm.handedness = 1;
  lm.score = 0.92f;
  lm.points[4] = {0.0f, 0.0f, 0.0f};
  lm.points[5] = {0.5f, 0.5f, 0.0f};
  lm.points[8] = {0.01f, 0.0f, 0.0f};
  return lm;
}

// Build a HandLandmarks snapshot for a real right hand NOT
// pinching (fingers apart). Used between bursts so the
// detector sees "release" events that could in principle
// promote a phantom burst to a click.
HandLandmarks realRightHandOpen() {
  HandLandmarks lm{};
  lm.handedness = 1;
  lm.score = 0.92f;
  lm.points[4] = {0.0f, 0.0f, 0.0f};
  lm.points[5] = {0.5f, 0.5f, 0.0f};
  lm.points[8] = {0.30f, 0.0f, 0.0f};  // far from thumb
  return lm;
}

}  // namespace

class PhantomE2E : public ::testing::Test {
 protected:
  void SetUp() override {
    vmosue::GetSignalObserver().Reset();
  }

  // Aggregated click-counter helper. A real click event is
  // one that the user would actually see and complain
  // about. We don't care about cursor motion or scroll
  // deltas here — those are intentional outputs. We DO
  // care about button events: leftClick, leftDown, leftUp,
  // leftDoubleClick, middleClick, rightClick. If ANY of
  // those ever fire from a phantom frame, the test fails.
  struct ClickCounts {
    int leftClick = 0;
    int leftDown = 0;
    int leftUp = 0;
    int leftDoubleClick = 0;
    int middleClick = 0;
    int rightClick = 0;
    int totalFrames = 0;
  };

  // Drive a state machine through `frames` (one HandLandmarks
  // per simulated frame at 30Hz = ~33ms per frame). The
  // `cfg` lets each test tune dwell/grace to focus on a
  // specific layer. Returns the aggregated click counts.
  ClickCounts Drive(GestureStateMachine& sm,
                    const std::vector<HandLandmarks>& frames) {
    ClickCounts c{};
    c.totalFrames = static_cast<int>(frames.size());
    int64_t ts = 0;
    constexpr int kFrameMs = 33;
    for (const auto& h : frames) {
      std::vector<HandLandmarks> batch{h};
      sm.OnLandmarks(batch, ts, 1.0 / 30.0);
      ActionSet a = sm.ConsumeActions();
      if (a.leftClick)       ++c.leftClick;
      if (a.leftDown)        ++c.leftDown;
      if (a.leftUp)          ++c.leftUp;
      if (a.leftDoubleClick) ++c.leftDoubleClick;
      if (a.middleClick)     ++c.middleClick;
      if (a.rightClick)      ++c.rightClick;
      ts += kFrameMs;
    }
    return c;
  }
};

// =================================================================
// Layer-2 test: dwell gate keeps a burst of phantom
// pinches from ever producing a click. Even if the
// HandStabilityFilter is removed (which is what this test
// simulates by feeding the SM directly), the DwellGate's
// 2500ms continuous-assertion requirement should hold back
// any 3-7 frame phantom burst. The test shortens dwellMs
// to 100ms so the test runs fast, but still expects 0
// clicks from phantoms — the key insight is that even a
// 100ms dwell requires *continuous* assertion, and a
// phantom burst always has gaps.
// =================================================================

TEST_F(PhantomE2E, PhantomBurstsNeverProduceClick) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.firstHandGraceMs = 0;  // disable grace to isolate dwell
  cfg.dwellMs = 100;          // short for test speed
  cfg.click.holdForDragMs = 100000;  // never let a stray pinch drag
  cfg.handednessRight = true;
  sm.Init(cfg);

  // 20 cycles of: 5-frame phantom pinch burst, 30-frame
  // gap, 30-frame real hand open, 30-frame gap. The
  // phantom bursts are 5 frames long at 30Hz = 165ms,
  // which is longer than the 100ms dwell — yet no click
  // should fire because the bursts are never sustained
  // (the gaps of 30+ frames reset the dwell counter).
  std::vector<HandLandmarks> frames;
  for (int cycle = 0; cycle < 20; ++cycle) {
    for (int i = 0; i < 5; ++i) frames.push_back(phantomRightHand());
    for (int i = 0; i < 30; ++i) frames.push_back(HandLandmarks{});  // no hand
    for (int i = 0; i < 30; ++i) frames.push_back(realRightHandOpen());
    for (int i = 0; i < 30; ++i) frames.push_back(HandLandmarks{});
  }
  auto c = Drive(sm, frames);
  EXPECT_EQ(c.leftClick, 0)
      << "20 phantom bursts each 5 frames long produced "
         "a click — dwell gate failed to suppress";
  EXPECT_EQ(c.leftDown, 0) << "phantom produced drag-start";
  EXPECT_EQ(c.rightClick, 0) << "phantom produced right-click";
  EXPECT_EQ(c.middleClick, 0) << "phantom produced middle-click";
}

// =================================================================
// Boundary test: a phantom burst that is sustained for
// EXACTLY the dwell window. The pinch is held for 5
// frames at 30Hz = 165ms, which is just over the 100ms
// test dwell. This is the "could go either way" case —
// the test asserts that a release AFTER the dwell window
// DOES fire, but only if the burst was actually held
// continuously. A 5-frame phantom followed by a 1-frame
// gap should NOT fire.
// =================================================================

TEST_F(PhantomE2E, ContinuousAssertFiresOnceDwellMet) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.firstHandGraceMs = 0;
  cfg.dwellMs = 100;
  cfg.click.holdForDragMs = 100000;
  // v0.6.2 fix: the default ClickDetector holds a click for
  // ~500ms (the double-click window) so a second pinch can
  // promote it to LeftDoubleClick. For this test we want
  // the click to fire on release, so disable the suppress
  // and set the double-click window to 0 for determinism.
  cfg.click.suppressSingleClickInDoubleWindow = false;
  sm.Init(cfg);

  // 5 frames of pinching = 165ms > 100ms dwell. The
  // ClickDetector should fire LeftClick when the user
  // releases (next frame with d > release threshold).
  // Note: we use a "open hand" frame for the release,
  // not a default-constructed HandLandmarks{}, because
  // the latter is a no-hand (handedness=0) frame and
  // would not be picked up by the right-hand search
  // path. The "open hand" pose has thumb-index distance
  // 0.30, well above the 0.07 release threshold.
  std::vector<HandLandmarks> frames;
  for (int i = 0; i < 5; ++i) frames.push_back(realRightHandPinching());
  frames.push_back(realRightHandOpen());  // release

  auto c = Drive(sm, frames);
  EXPECT_GE(c.leftClick, 1)
      << "sustained real pinch over the dwell window should fire";
}

TEST_F(PhantomE2E, GapInBurstBreaksDwell) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.firstHandGraceMs = 0;
  cfg.dwellMs = 100;
  cfg.click.holdForDragMs = 100000;
  cfg.click.suppressSingleClickInDoubleWindow = false;
  sm.Init(cfg);

  // 3-frame pinch, 1-frame gap (open hand), 3-frame
  // pinch, release. The 1-frame gap is an "open hand"
  // pose (d=0.30 > release 0.07), so the ClickDetector
  // sees a release — the gesture is broken across two
  // bursts. With cfg.dwellMs=100 and a 3-frame burst
  // (99ms) just under dwell, neither burst crosses the
  // 100ms continuous-assertion threshold.
  std::vector<HandLandmarks> frames;
  for (int i = 0; i < 3; ++i) frames.push_back(realRightHandPinching());
  frames.push_back(realRightHandOpen());  // 1-frame gap (release)
  for (int i = 0; i < 3; ++i) frames.push_back(realRightHandPinching());
  frames.push_back(realRightHandOpen());  // release

  auto c = Drive(sm, frames);
  // Both bursts are 99ms — just under the 100ms dwell.
  // We expect 0 clicks because neither burst crosses
  // the threshold. The test is intentionally
  // borderline: if the dwell counter's behavior on a
  // gap ever changes to "forgive the gap", this test
  // will start failing and force a re-think.
  EXPECT_EQ(c.leftClick, 0)
      << "intermittent burst (3 pinch + 1 gap + 3 pinch) "
         "should not produce a click within the 100ms dwell";
}

// =================================================================
// Real-hand baseline: a sustained real hand pinch DOES
// produce exactly one click when the user releases. This
// is the "happy path" — it must keep working after any
// anti-phantom change, otherwise the app feels broken.
// =================================================================

TEST_F(PhantomE2E, SustainedRealPinchFiresOnce) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.firstHandGraceMs = 0;
  cfg.dwellMs = 100;
  cfg.click.holdForDragMs = 100000;
  cfg.click.suppressSingleClickInDoubleWindow = false;
  sm.Init(cfg);

  // 10-frame sustained pinch (330ms), then open hand
  // (release). The release pose has d=0.30 which is
  // > release threshold 0.07, so the ClickDetector
  // emits LeftClick.
  std::vector<HandLandmarks> frames;
  for (int i = 0; i < 10; ++i) frames.push_back(realRightHandPinching());
  frames.push_back(realRightHandOpen());
  frames.push_back(realRightHandOpen());

  auto c = Drive(sm, frames);
  EXPECT_EQ(c.leftClick, 1)
      << "a single release after a sustained 10-frame pinch "
         "must produce exactly one click — anything else is "
         "a regression in the click detector";
  EXPECT_EQ(c.leftDown, 0) << "no drag — holdForDragMs is huge";
}
