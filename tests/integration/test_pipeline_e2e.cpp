// Task 34: end-to-end integration test for the gesture pipeline.
//
// The original spec wanted to replay a recorded MP4 through
// `HandDetector`. In this environment MediaPipe is not installed and
// `HandDetector::Detect()` is a stub, so we instead feed hand-crafted
// `HandLandmarks` sequences straight into `GestureStateMachine`. The
// downstream contract (state machine -> ActionSet -> injection) is
// the same one `App` calls from its inference callback, so this test
// still exercises the post-inference pipeline end-to-end:
//
//   * frames 0-5   -> cursor movement (right hand MCP walks right)
//   * frames 6-10  -> left click (quick pinch on right hand)
//   * frames 11-15 -> drag start/end (long pinch on right hand)
//   * frames 16-20 -> scroll (left hand index+middle tips close,
//                    index tip moves up)
//   * frames 21-25 -> pause (left hand open, held)
//   * frames 26-29 -> two-hand-open emergency stop
//
// See tests/fixtures/README.md for why this replaces sample_video.mp4.

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "gesture/GestureStateMachine.h"
#include "util/Adaptive.h"

// The integration tests assert absolute behavior of the
// adaptive controller (e.g. "the wheel delta is non-zero when
// the fixture has scroll motion"). Earlier test suites
// (test_adaptive, test_scroll_detector) populate the global
// SignalObserver's rolling buffers with their own observations,
// which leaks across test boundaries. Without a reset, by the
// time the integration tests run, the adaptive ScrollEnterThreshold
// has been pushed above the fixture's small (0.008) fingertip
// separation and the detector never enters the Active phase.
//
// Resetting in SetUp() restores the cold-start state for every
// integration test, matching the documented "first ~1 s of real
// use" semantics the fixture is designed to model. The fixture
// is also a safe place to do it because all tests in this TU
// share the same observer singleton -- doing it per TEST rather
// than once at file scope matches gtest's per-test isolation
// model.
class PipelineE2E : public ::testing::Test {
 protected:
  void SetUp() override { vmosue::GetSignalObserver().Reset(); }
};

// Two-level stringification so we can turn a CMake-provided macro
// like VMOSUE_TEST_FIXTURES_DIR (which expands to a bare path) into
// a C string literal at the call site. Standard idiom; kept local
// so we don't pollute the project-wide headers with a name we only
// use in this test.
#define VMOSUE_STRINGIFY_INNER(x) #x
#define VMOSUE_STRINGIFY(x) VMOSUE_STRINGIFY_INNER(x)

namespace {

using vmosue::GestureStateMachine;
using vmosue::GlobalState;
using vmosue::HandLandmarks;
using vmosue::Point2F;

// Build a 21-point right-hand HandLandmarks that uses landmark 5
// (index MCP) at (x_base, y_mcp) so the cursor controller sees a
// moving pivot, and either:
//   * an open palm (all four fingertips above their MCPs), or
//   * a pinch where thumb tip (4) sits at the MCP and index tip (8)
//     is dragged to it.
static HandLandmarks makeRightHand(float x_base, float y_mcp,
                                   bool open, bool pinch) {
  HandLandmarks lm{};
  lm.handedness = 1;
  // Index MCP (pivot for the cursor controller). Slight x jitter keeps
  // the OneEuroFilter from collapsing, but the test only cares about
  // dx, so any consistent motion is fine.
  lm.points[5]  = {x_base,         y_mcp,   0.0f};
  lm.points[9]  = {x_base,         y_mcp,   0.0f};
  lm.points[13] = {x_base,         y_mcp,   0.0f};
  lm.points[17] = {x_base,         y_mcp,   0.0f};

  if (open) {
    // All four fingertips above their MCPs (smaller y == higher up).
    lm.points[8]  = {x_base,         y_mcp - 0.2f, 0.0f};
    lm.points[12] = {x_base,         y_mcp - 0.2f, 0.0f};
    lm.points[16] = {x_base,         y_mcp - 0.2f, 0.0f};
    lm.points[20] = {x_base,         y_mcp - 0.2f, 0.0f};
  } else {
    // Closed (fingertips at or below MCPs).
    lm.points[8]  = {x_base,         y_mcp + 0.1f, 0.0f};
    lm.points[12] = {x_base,         y_mcp + 0.1f, 0.0f};
    lm.points[16] = {x_base,         y_mcp + 0.1f, 0.0f};
    lm.points[20] = {x_base,         y_mcp + 0.1f, 0.0f};
  }

  if (pinch) {
    // Thumb tip (4) sits at the MCP, index tip (8) is dragged toward
    // it so the distance falls under the default 0.04 pinch threshold.
    // We only override point 8 when the hand is closed (open=false) --
    // when both open=true and pinch=true the spec wants a pinching
    // open palm, which the test fixtures don't actually exercise but
    // we keep the index tip "above" the MCP to stay consistent with
    // the open branch.
    if (!open) {
      lm.points[4] = {x_base,         y_mcp,   0.0f};
      lm.points[8] = {x_base + 0.01f, y_mcp,   0.0f};
    } else {
      lm.points[4] = {x_base,         y_mcp,   0.0f};
    }
  } else {
    // Spread: thumb away from index tip. Only move the thumb; do
    // NOT override the index tip because the open/closed branch
    // already placed it correctly. Earlier versions of this helper
    // unconditionally re-set lm.points[8] to y_mcp in the spread
    // branch, which collapsed the "open hand" fingertip onto the
    // MCP and made IsHandOpen() return false (the two-hand-open
    // emergency-stop test then failed to trip because the right
    // hand was not recognised as open).
    lm.points[4] = {x_base - 0.10f, y_mcp,   0.0f};
  }
  return lm;
}

// Build a 21-point left-hand HandLandmarks. Supports two modes:
//   * open palm: all four fingertips above their MCPs.
//   * scroll: index + middle tips at the same y so the scroll
//     detector's enter threshold (~0.05 normalized) trips. The
//     `scroll_dy` argument shifts the index tip's y between frames
//     so the scroll detector observes motion and emits wheel deltas.
static HandLandmarks makeLeftHand(float x_base, bool open,
                                  bool scroll_active,
                                  float scroll_dy) {
  HandLandmarks lm{};
  lm.handedness = 0;
  lm.points[5]  = {x_base, 0.7f, 0.0f};
  lm.points[9]  = {x_base, 0.7f, 0.0f};
  lm.points[13] = {x_base, 0.7f, 0.0f};
  lm.points[17] = {x_base, 0.7f, 0.0f};

  if (open) {
    lm.points[8]  = {x_base, 0.5f, 0.0f};
    lm.points[12] = {x_base, 0.5f, 0.0f};
    lm.points[16] = {x_base, 0.5f, 0.0f};
    lm.points[20] = {x_base, 0.5f, 0.0f};
  } else if (scroll_active) {
    // Index tip and middle tip at the same y (distance = 0 < 0.05).
    // Shift the index tip's y by the per-frame `scroll_dy` so the
    // scroll detector computes a non-zero (lastIndexY_ - nowY)
    // delta. Negative scroll_dy means "index tip moved up" which
    // maps to a positive wheel delta (scroll up).
    const float baseY = 0.6f;
    lm.points[8]  = {x_base, baseY + scroll_dy, 0.0f};
    lm.points[12] = {x_base, baseY,           0.0f};
    lm.points[16] = {x_base, 0.9f,            0.0f};
    lm.points[20] = {x_base, 0.9f,            0.0f};
  } else {
    // Closed fist.
    lm.points[8]  = {x_base, 0.8f, 0.0f};
    lm.points[12] = {x_base, 0.8f, 0.0f};
    lm.points[16] = {x_base, 0.8f, 0.0f};
    lm.points[20] = {x_base, 0.8f, 0.0f};
  }
  // Thumb away from index so pinch doesn't accidentally fire.
  lm.points[4] = {x_base - 0.10f, 0.7f, 0.0f};
  return lm;
}

// Tiny per-frame JSON description used by sample_landmarks.json. We
// use string keys ("right", "left") to make the JSON readable and to
// keep the fixture stable across schema tweaks.
struct RightSpec {
  float x_base = 0.5f;
  float y_mcp = 0.5f;
  bool open = false;
  bool pinch = false;
};
struct LeftSpec {
  float x_base = 0.5f;
  bool open = false;
  bool scroll_active = false;
  float scroll_dy = 0.0f;
};
struct FrameSpec {
  int64_t ts_ms = 0;
  bool has_right = false;
  bool has_left = false;
  RightSpec right{};
  LeftSpec  left{};
};

// Parse sample_landmarks.json into a flat FrameSpec list. We accept
// the JSON shape authored by tests/fixtures/sample_landmarks.json; if
// a key is missing the corresponding flag defaults to "no hand" so a
// partial fixture is still loadable.
static std::vector<FrameSpec> loadFixture(const std::string& path) {
  std::ifstream f(path);
  EXPECT_TRUE(f.is_open()) << "Cannot open fixture: " << path;
  // Slurp the file into a string and use json::parse so the parse
  // path is independent of the streaming operator (which differs
  // subtly between real nlohmann_json and any local stub used during
  // parse-only verification).
  std::string text((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
  nlohmann::json j = nlohmann::json::parse(text);
  std::vector<FrameSpec> out;
  for (const auto& jf : j.at("frames")) {
    FrameSpec s;
    s.ts_ms = jf.value("ts_ms", int64_t{0});
    if (jf.contains("right")) {
      s.has_right = true;
      const auto& r = jf.at("right");
      s.right.x_base = r.value("x_base", 0.5f);
      s.right.y_mcp  = r.value("y_mcp",  0.5f);
      s.right.open   = r.value("open",   false);
      s.right.pinch  = r.value("pinch",  false);
    }
    if (jf.contains("left")) {
      s.has_left = true;
      const auto& l = jf.at("left");
      s.left.x_base        = l.value("x_base", 0.5f);
      s.left.open          = l.value("open",   false);
      s.left.scroll_active = l.value("scroll_active", false);
      s.left.scroll_dy     = l.value("scroll_dy",     0.0f);
    }
    out.push_back(s);
  }
  return out;
}

// Translate a FrameSpec into the {left,right} hands vector consumed
// by GestureStateMachine::OnLandmarks().
static std::vector<HandLandmarks> makeHands(const FrameSpec& s) {
  std::vector<HandLandmarks> hands;
  if (s.has_left)  hands.push_back(makeLeftHand (s.left.x_base, s.left.open,
                                                 s.left.scroll_active,
                                                 s.left.scroll_dy));
  if (s.has_right) hands.push_back(makeRightHand(s.right.x_base, s.right.y_mcp,
                                                 s.right.open,
                                                 s.right.pinch));
  return hands;
}

// Drive the state machine with the supplied frames and return the
// merged ActionSet accumulated across the whole run. Frames are
// spaced at the fixture's nominal dt (~50ms).
static vmosue::ActionSet runFrames(GestureStateMachine& sm,
                                   const std::vector<FrameSpec>& frames) {
  vmosue::ActionSet merged{};
  int64_t prevTs = -1;
  for (const auto& f : frames) {
    double dt = (prevTs < 0) ? (1.0 / 30.0) : double(f.ts_ms - prevTs) / 1000.0;
    if (dt <= 0.0) dt = 1.0 / 30.0;
    sm.OnLandmarks(makeHands(f), f.ts_ms, dt);
    auto drained = sm.ConsumeActions();
    merged.cursorDx        += drained.cursorDx;
    merged.cursorDy        += drained.cursorDy;
    merged.wheel           += drained.wheel;
    merged.hWheel          += drained.hWheel;
    merged.leftClick        = merged.leftClick        || drained.leftClick;
    merged.leftDoubleClick  = merged.leftDoubleClick  || drained.leftDoubleClick;
    merged.leftDown         = merged.leftDown         || drained.leftDown;
    merged.leftUp           = merged.leftUp           || drained.leftUp;
    merged.rightClick       = merged.rightClick       || drained.rightClick;
    merged.middleClick      = merged.middleClick      || drained.middleClick;
    merged.safeRelease      = merged.safeRelease      || drained.safeRelease;
    prevTs = f.ts_ms;
  }
  return merged;
}

// Locate the fixtures directory relative to the test working dir.
// In the vmosue_tests build tree the JSON sits at
//   <build>/bin/<Config>/../../tests/fixtures/sample_landmarks.json
// and at install time at
//   <install>/tests/fixtures/sample_landmarks.json
// We try a few candidate paths so the test works in both layouts.
static std::string locateFixture() {
  // The CMakeLists sets VMOSUE_TEST_FIXTURES_DIR via
  // add_compile_definitions. The value CMake passes does NOT have
  // surrounding quotes (target_compile_definitions escapes them
  // already), so we wrap the expansion in literal quotes here to
  // form a valid string literal at the call site. Falling back to
  // a relative search keeps the test parseable when the define is
  // missing.
#ifdef VMOSUE_TEST_FIXTURES_DIR
  std::string candidate = VMOSUE_STRINGIFY(VMOSUE_TEST_FIXTURES_DIR) "/sample_landmarks.json";
  std::ifstream f(candidate);
  if (f.is_open()) return candidate;
#endif
  const char* cands[] = {
      "tests/fixtures/sample_landmarks.json",
      "../tests/fixtures/sample_landmarks.json",
      "../../tests/fixtures/sample_landmarks.json",
      "../../../tests/fixtures/sample_landmarks.json",
  };
  for (const char* c : cands) {
    std::ifstream f(c);
    if (f.is_open()) return std::string(c);
  }
  return "";  // signals "not found"; callers should EXPECT fail.
}

}  // namespace

// ---- The actual gtest cases ----

TEST_F(PipelineE2E, FixtureFileLoads) {
  auto frames = loadFixture(locateFixture());
  // 30 frames per spec; if the fixture is malformed we want a hard
  // failure here rather than 30 silent skips downstream.
  EXPECT_EQ(frames.size(), size_t{30});
}

TEST_F(PipelineE2E, CursorMovesDuringFrames0Through4) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  // The click hold is short for the cursor phase so a pinch never
  // accidentally fires.
  cfg.click.holdForDragMs = 10000;
  sm.Init(cfg);
  auto frames = loadFixture(locateFixture());
  ASSERT_GE(frames.size(), size_t{5});
  std::vector<FrameSpec> phase(frames.begin(), frames.begin() + 5);
  auto actions = runFrames(sm, phase);
  EXPECT_NE(actions.cursorDx, 0)
      << "CursorController should have produced non-zero dx while the "
         "right-hand MCP walked rightward.";
  EXPECT_FALSE(actions.leftClick)
      << "Cursor phase must not emit any clicks.";
}

TEST_F(PipelineE2E, ClickFiresDuringFrames5Through8) {
  GestureStateMachine sm;
  sm.Init({});
  auto frames = loadFixture(locateFixture());
  ASSERT_GE(frames.size(), size_t{9});
  std::vector<FrameSpec> phase(frames.begin() + 5, frames.begin() + 9);
  auto actions = runFrames(sm, phase);
  EXPECT_TRUE(actions.leftClick)
      << "Frames 5-8 contain a quick pinch and release; the click "
         "detector must emit LeftClick.";
  // No drag start should appear in this short pinch window.
  EXPECT_FALSE(actions.leftDown);
}

TEST_F(PipelineE2E, DragStartAndEndDuringFrames9Through15) {
  GestureStateMachine sm;
  sm.Init({});
  auto frames = loadFixture(locateFixture());
  ASSERT_GE(frames.size(), size_t{16});
  std::vector<FrameSpec> phase(frames.begin() + 9, frames.begin() + 16);
  auto actions = runFrames(sm, phase);
  // The state machine maps LeftDragStart -> leftDown and
  // LeftDragEnd -> leftUp. A held pinch over the drag threshold
  // (200ms by default) must yield both edges within this 7-frame
  // window.
  EXPECT_TRUE(actions.leftDown)
      << "Frames 9-15 hold the pinch past holdForDragMs; expected a "
         "LeftDragStart (leftDown) edge.";
  EXPECT_TRUE(actions.leftUp)
      << "Frames 9-15 end with a release; expected a LeftDragEnd "
         "(leftUp) edge.";
  // Drag should not double as a click.
  EXPECT_FALSE(actions.leftClick);
}

TEST_F(PipelineE2E, ScrollFiresDuringFrames16Through20) {
  GestureStateMachine sm;
  // PauseDetector hold is long enough that the open-palm pause phase
  // doesn't fire spuriously during scroll.
  GestureStateMachine::Config cfg;
  cfg.pause.holdMs = 10000;
  sm.Init(cfg);
  auto frames = loadFixture(locateFixture());
  ASSERT_GE(frames.size(), size_t{21});
  std::vector<FrameSpec> phase(frames.begin() + 16, frames.begin() + 21);
  auto actions = runFrames(sm, phase);
  EXPECT_NE(actions.wheel, 0)
      << "ScrollDetector should have observed left-hand index-tip "
         "motion while index+middle tips were pinched together.";
}

TEST_F(PipelineE2E, PauseTogglesDuringFrames21Through25) {
  GestureStateMachine sm;
  // Lower pause holdMs so the test stays fast; the fixture holds the
  // open palm for ~250ms in this phase.
  GestureStateMachine::Config cfg;
  cfg.pause.holdMs = 150;
  cfg.twoHandOpenHoldMs = 10000;  // don't trip emergency here
  sm.Init(cfg);
  auto frames = loadFixture(locateFixture());
  ASSERT_GE(frames.size(), size_t{26});
  std::vector<FrameSpec> phase(frames.begin() + 21, frames.begin() + 26);
  (void)runFrames(sm, phase);
  EXPECT_EQ(sm.State(), GlobalState::Paused)
      << "Holding the left hand open past pause.holdMs should toggle "
         "the global state from Active to Paused.";
}

TEST_F(PipelineE2E, EmergencyStopDuringFrames26Through29) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.twoHandOpenHoldMs = 150;  // 4 frames * 50ms = 200ms held
  cfg.pause.holdMs      = 10000;  // don't trip pause here
  sm.Init(cfg);
  auto frames = loadFixture(locateFixture());
  ASSERT_GE(frames.size(), size_t{30});
  std::vector<FrameSpec> phase(frames.begin() + 26, frames.begin() + 30);
  auto actions = runFrames(sm, phase);
  EXPECT_EQ(sm.State(), GlobalState::EmergencyStopped)
      << "Both hands visibly open past twoHandOpenHoldMs should trip "
         "the emergency-stop path.";
  EXPECT_TRUE(actions.safeRelease)
      << "EmergencyStop() must surface a safeRelease hint to any "
         "downstream consumer (overlay, input layer).";
}

TEST_F(PipelineE2E, FullSequenceProducesAllExpectedEvents) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  // Use tighter thresholds to match the fixture timings (50ms/frame).
  cfg.pause.holdMs         = 150;
  cfg.twoHandOpenHoldMs    = 150;
  cfg.click.holdForDragMs  = 200;
  cfg.scroll.enterHoldMs   = 100;
  sm.Init(cfg);
  auto frames = loadFixture(locateFixture());
  ASSERT_EQ(frames.size(), 30u);
  auto actions = runFrames(sm, frames);
  // Sanity-check the merged ActionSet across the full sequence.
  EXPECT_NE(actions.cursorDx, 0)
      << "Cursor phase should have produced non-zero dx.";
  EXPECT_TRUE(actions.leftClick)   << "Click phase must emit LeftClick.";
  EXPECT_TRUE(actions.leftDown)    << "Drag phase must emit LeftDown.";
  EXPECT_TRUE(actions.leftUp)      << "Drag phase must emit LeftUp.";
  EXPECT_NE(actions.wheel, 0)      << "Scroll phase must emit wheel deltas.";
  EXPECT_TRUE(actions.safeRelease)
      << "Emergency-stop phase must surface a safeRelease hint.";
  EXPECT_EQ(sm.State(), GlobalState::EmergencyStopped)
      << "Final state must be EmergencyStopped after the two-hand-open "
         "gesture.";
}