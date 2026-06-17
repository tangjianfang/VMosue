// Per-action verification: one synthetic-landmark fixture per gesture,
// each asserting the action fires (positive) AND that no other action
// fires (anti-cross-talk). Fixtures live in tests/fixtures/actions/.
// This TU carries its own builder helpers (a superset of the e2e TU's,
// adding middle_pinch + push_z) so the two integration TUs are
// independent.
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <climits>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "gesture/GestureStateMachine.h"
#include "util/Adaptive.h"

#define VMOSUE_STRINGIFY_INNER(x) #x
#define VMOSUE_STRINGIFY(x) VMOSUE_STRINGIFY_INNER(x)

using vmosue::ActionSet;
using vmosue::GestureStateMachine;
using vmosue::GlobalState;
using vmosue::HandLandmarks;

namespace {

struct RightSpec {
  float x_base = 0.5f, y_mcp = 0.5f;
  bool open = false, pinch = false, middle_pinch = false;
  float push_z = 0.0f;  // world[8].z relative to wrist; negative = toward camera
};
struct LeftSpec {
  float x_base = 0.5f;
  bool open = false, scroll_active = false;
  float scroll_dy = 0.0f, scroll_dx = 0.0f;
};
struct FrameSpec {
  int64_t ts_ms = 0;
  bool has_right = false, has_left = false;
  RightSpec right{};
  LeftSpec  left{};
};

// Right hand: index MCP (5) is the cursor pivot.
// Optionally: open palm, thumb-index pinch, thumb-middle pinch,
// and a world-Z push on the index tip for the air-click gesture.
static HandLandmarks makeRightHand(const RightSpec& r) {
  HandLandmarks lm{};
  lm.handedness = 1;
  lm.points[5]  = {r.x_base, r.y_mcp, 0.0f};
  lm.points[9]  = {r.x_base, r.y_mcp, 0.0f};
  lm.points[13] = {r.x_base, r.y_mcp, 0.0f};
  lm.points[17] = {r.x_base, r.y_mcp, 0.0f};

  if (r.open) {
    lm.points[8]  = {r.x_base, r.y_mcp - 0.2f, 0.0f};
    lm.points[12] = {r.x_base, r.y_mcp - 0.2f, 0.0f};
    lm.points[16] = {r.x_base, r.y_mcp - 0.2f, 0.0f};
    lm.points[20] = {r.x_base, r.y_mcp - 0.2f, 0.0f};
  } else {
    lm.points[8]  = {r.x_base, r.y_mcp + 0.1f, 0.0f};
    lm.points[12] = {r.x_base, r.y_mcp + 0.1f, 0.0f};
    lm.points[16] = {r.x_base, r.y_mcp + 0.1f, 0.0f};
    lm.points[20] = {r.x_base, r.y_mcp + 0.1f, 0.0f};
  }
  // Thumb default: away from both index and middle so no pinch fires.
  lm.points[4] = {r.x_base - 0.10f, r.y_mcp, 0.0f};

  if (r.pinch) {
    // Thumb tip at index tip: dist(4,8) ~ 0.01 < pinch threshold.
    lm.points[4] = {r.x_base,        r.y_mcp, 0.0f};
    lm.points[8] = {r.x_base + 0.01f, r.y_mcp, 0.0f};
  } else if (r.middle_pinch) {
    // Thumb tip at middle tip: dist(4,12) ~ 0.01; keep index far.
    lm.points[4]  = {r.x_base,         r.y_mcp, 0.0f};
    lm.points[12] = {r.x_base + 0.01f,  r.y_mcp, 0.0f};
    lm.points[8]  = {r.x_base + 0.20f,  r.y_mcp, 0.0f};
  }

  // World landmarks for the air-click (right-click) gesture.
  // Wrist (0) at z=0; index tip (8) at push_z.
  // AirClickDetector reads world[], not points[].
  lm.world[0].z = 0.0f;
  lm.world[8].z = r.push_z;
  return lm;
}

// Left hand: open palm or two-finger scroll.
// scroll_dx/dy shift the index tip so the scroll detector sees motion.
static HandLandmarks makeLeftHand(const LeftSpec& l) {
  HandLandmarks lm{};
  lm.handedness = 0;
  lm.points[5]  = {l.x_base, 0.7f, 0.0f};
  lm.points[9]  = {l.x_base, 0.7f, 0.0f};
  lm.points[13] = {l.x_base, 0.7f, 0.0f};
  lm.points[17] = {l.x_base, 0.7f, 0.0f};

  if (l.open) {
    lm.points[8]  = {l.x_base, 0.5f, 0.0f};
    lm.points[12] = {l.x_base, 0.5f, 0.0f};
    lm.points[16] = {l.x_base, 0.5f, 0.0f};
    lm.points[20] = {l.x_base, 0.5f, 0.0f};
  } else if (l.scroll_active) {
    const float baseY = 0.6f;
    lm.points[8]  = {l.x_base + l.scroll_dx, baseY + l.scroll_dy, 0.0f};
    lm.points[12] = {l.x_base,               baseY,               0.0f};
    lm.points[16] = {l.x_base, 0.9f, 0.0f};
    lm.points[20] = {l.x_base, 0.9f, 0.0f};
  } else {
    lm.points[8]  = {l.x_base, 0.8f, 0.0f};
    lm.points[12] = {l.x_base, 0.8f, 0.0f};
    lm.points[16] = {l.x_base, 0.8f, 0.0f};
    lm.points[20] = {l.x_base, 0.8f, 0.0f};
  }
  // Thumb away so pinch doesn't fire accidentally.
  lm.points[4] = {l.x_base - 0.10f, 0.7f, 0.0f};
  return lm;
}

static std::vector<FrameSpec> loadFixture(const std::string& path) {
  std::ifstream f(path);
  EXPECT_TRUE(f.is_open()) << "Cannot open fixture: " << path;
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
      s.right.x_base       = r.value("x_base",       0.5f);
      s.right.y_mcp        = r.value("y_mcp",        0.5f);
      s.right.open         = r.value("open",         false);
      s.right.pinch        = r.value("pinch",        false);
      s.right.middle_pinch = r.value("middle_pinch", false);
      s.right.push_z       = r.value("push_z",       0.0f);
    }
    if (jf.contains("left")) {
      s.has_left = true;
      const auto& l = jf.at("left");
      s.left.x_base        = l.value("x_base",        0.5f);
      s.left.open          = l.value("open",          false);
      s.left.scroll_active = l.value("scroll_active", false);
      s.left.scroll_dy     = l.value("scroll_dy",     0.0f);
      s.left.scroll_dx     = l.value("scroll_dx",     0.0f);
    }
    out.push_back(s);
  }
  return out;
}

static std::vector<HandLandmarks> makeHands(const FrameSpec& s) {
  std::vector<HandLandmarks> hands;
  if (s.has_left)  hands.push_back(makeLeftHand(s.left));
  if (s.has_right) hands.push_back(makeRightHand(s.right));
  return hands;
}

// Drive frames, return merged ActionSet AND the final state.
struct RunResult {
  ActionSet   actions{};
  // First cursorX/Y we observed from the state machine (sentinel until
  // a hand is detected). Recorded alongside the merged final cursor
  // target so cursor-motion tests can verify "cursor moved" by
  // comparing first vs last rather than asserting on the absolute
  // value (which depends on the test's virtual-desktop size).
  int firstCursorX = INT_MIN;
  int firstCursorY = INT_MIN;
  GlobalState finalState = GlobalState::Active;
};

static RunResult runFrames(GestureStateMachine& sm,
                            const std::vector<FrameSpec>& frames) {
  RunResult rr;
  int64_t prevTs = -1;
  for (const auto& f : frames) {
    double dt = (prevTs < 0) ? (1.0 / 30.0)
                             : double(f.ts_ms - prevTs) / 1000.0;
    if (dt <= 0.0) dt = 1.0 / 30.0;
    sm.OnLandmarks(makeHands(f), f.ts_ms, dt);
    auto d = sm.ConsumeActions();
    // Cursor target is absolute; the latest frame's value wins. We
    // additionally capture the FIRST observed cursorX/Y so tests can
    // assert "cursor moved" without depending on the virtual-desktop
    // size baked into the test build.
    if (d.cursorX != INT_MIN && rr.firstCursorX == INT_MIN) {
      rr.firstCursorX = d.cursorX;
      rr.firstCursorY = d.cursorY;
    }
    rr.actions.cursorX        = d.cursorX;
    rr.actions.cursorY        = d.cursorY;
    rr.actions.wheel          += d.wheel;
    rr.actions.hWheel         += d.hWheel;
    rr.actions.leftClick       = rr.actions.leftClick       || d.leftClick;
    rr.actions.leftDoubleClick = rr.actions.leftDoubleClick || d.leftDoubleClick;
    rr.actions.leftDown        = rr.actions.leftDown        || d.leftDown;
    rr.actions.leftUp          = rr.actions.leftUp          || d.leftUp;
    rr.actions.rightClick      = rr.actions.rightClick      || d.rightClick;
    rr.actions.middleClick     = rr.actions.middleClick     || d.middleClick;
    rr.actions.safeRelease     = rr.actions.safeRelease     || d.safeRelease;
    prevTs = f.ts_ms;
  }
  rr.finalState = sm.State();
  return rr;
}

// Resolve a per-action fixture path.
static std::string fixturePath(const char* name) {
#ifdef VMOSUE_TEST_FIXTURES_DIR
  std::string candidate =
      VMOSUE_STRINGIFY(VMOSUE_TEST_FIXTURES_DIR) "/actions/";
  candidate += name;
  {
    std::ifstream f(candidate);
    if (f.is_open()) return candidate;
  }
#endif
  // Fallback: relative paths tried in order, matching the CWD layout
  // used when running ctest from the build directory.
  const char* cands[] = {
      "tests/fixtures/actions/",
      "../tests/fixtures/actions/",
      "../../tests/fixtures/actions/",
      "../../../tests/fixtures/actions/",
  };
  for (const char* c : cands) {
    std::string p = std::string(c) + name;
    std::ifstream f(p);
    if (f.is_open()) return p;
  }
  return std::string("tests/fixtures/actions/") + name;
}

// Assert that NO action field is set EXCEPT those listed in `allow`.
// `allow` is a space-separated list of tokens; absent tokens mean the
// field must be zero/false. Keeps anti-cross-talk checks to one line.
// `cursorX/Y` use INT_MIN as the "no cursor target this frame"
// sentinel, so we compare against that instead of 0 — a valid
// absolute position can legitimately be 0 (hand at top-left of the
// video frame).
//
// NOTE: for absolute cursor mapping, even a stationary hand produces
// a non-sentinel cursorX every frame (it points at the hand's screen
// position). What anti-cross-talk wants to assert is "the cursor
// TARGET DID NOT MOVE" across this fixture, not "cursorX is the
// sentinel". Pass the first observed cursor position via `first` so
// we can compare against it.
static void expectOnly(const ActionSet& a,
                       const std::string& allow,
                       int firstCursorX = INT_MIN,
                       int firstCursorY = INT_MIN) {
  auto has = [&](const char* key) {
    return allow.find(key) != std::string::npos;
  };
  if (!has("cursor")) {
    // Cursor cross-talk check: target must be either "no frame had a
    // hand" (sentinel) or "the target stayed put" (same as the first
    // observed position). Anything else means another gesture
    // accidentally moved the cursor.
    if (firstCursorX == INT_MIN) {
      EXPECT_EQ(a.cursorX, INT_MIN);
      EXPECT_EQ(a.cursorY, INT_MIN);
    } else {
      EXPECT_EQ(a.cursorX, firstCursorX)
          << "cursor must not move during a non-cursor gesture";
      EXPECT_EQ(a.cursorY, firstCursorY);
    }
  }
  if (!has("leftClick")) EXPECT_FALSE(a.leftClick);
  if (!has("double"))    EXPECT_FALSE(a.leftDoubleClick);
  if (!has("leftDown"))  EXPECT_FALSE(a.leftDown);
  if (!has("leftUp"))    EXPECT_FALSE(a.leftUp);
  if (!has("right"))     EXPECT_FALSE(a.rightClick);
  if (!has("middle"))    EXPECT_FALSE(a.middleClick);
  if (!has("wheel"))     EXPECT_EQ(a.wheel, 0);
  if (!has("hwheel"))    EXPECT_EQ(a.hWheel, 0);
  if (!has("safe"))      EXPECT_FALSE(a.safeRelease);
}

// Per-test fixture: reset global observer so cold-start thresholds
// apply and tests are order-independent.
class ActionMap : public ::testing::Test {
 protected:
  void SetUp() override { vmosue::GetSignalObserver().Reset(); }
};

}  // namespace

// ---- Tests ----

TEST_F(ActionMap, CursorMove) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.click.holdForDragMs = 100000;  // never let a stray pinch become a drag
  sm.Init(cfg);
  auto r = runFrames(sm, loadFixture(fixturePath("cursor_move.json")));
  // Absolute mapping: the camera image is mirrored (selfie
  // convention), so CursorController flips X: a hand moving right in
  // the video (x_base increasing) drives the cursor LEFT on screen
  // (cursorX decreasing). The merged cursorX is the last frame's
  // absolute target, so we compare it to the first frame's target
  // rather than asserting on a raw value (which depends on the test
  // machine's virtual-desktop size).
  ASSERT_NE(r.actions.cursorX, INT_MIN)
      << "CursorController should have produced an absolute target";
  ASSERT_NE(r.firstCursorX, INT_MIN)
      << "First-frame cursorX must be recorded for the move-direction check";
  EXPECT_LT(r.actions.cursorX, r.firstCursorX)
      << "cursor must move left when the hand moves right in the video "
         "(mirrored camera convention)";
  expectOnly(r.actions, "cursor");
}

TEST_F(ActionMap, LeftClick) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.click.suppressSingleClickInDoubleWindow = false;
  sm.Init(cfg);
  auto r = runFrames(sm, loadFixture(fixturePath("left_click.json")));
  EXPECT_TRUE(r.actions.leftClick);
  // The right hand is stationary in this fixture; cursor target
  // must equal the first observed position.
  expectOnly(r.actions, "leftClick",
             r.firstCursorX, r.firstCursorY);
}

TEST_F(ActionMap, DoubleClick) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.click.doubleClickWindowMs = 400;  // pin the window for determinism
  sm.Init(cfg);
  auto r = runFrames(sm, loadFixture(fixturePath("double_click.json")));
  EXPECT_TRUE(r.actions.leftDoubleClick);
  // Allow left-family events; forbid unrelated families.
  expectOnly(r.actions, "leftClick double leftDown leftUp",
             r.firstCursorX, r.firstCursorY);
}

TEST_F(ActionMap, LeftDrag) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.click.holdForDragMs = 200;
  sm.Init(cfg);
  auto r = runFrames(sm, loadFixture(fixturePath("left_drag.json")));
  EXPECT_TRUE(r.actions.leftDown) << "drag should press LMB";
  EXPECT_TRUE(r.actions.leftUp)   << "drag should release LMB";
  // Same mirrored-camera convention as CursorMove: the dragged hand
  // walks right in the video (x_base 0.50 -> 0.60), so the cursor
  // target walks left on screen.
  EXPECT_NE(r.actions.cursorX, INT_MIN)
      << "drag should produce an absolute cursor target";
  EXPECT_LT(r.actions.cursorX, r.firstCursorX)
      << "drag should move the cursor (mirrored)";
  expectOnly(r.actions, "leftDown leftUp cursor");
}

TEST_F(ActionMap, MiddleClick) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.click.holdForDragMs = 100000;
  sm.Init(cfg);
  auto r = runFrames(sm, loadFixture(fixturePath("middle_click.json")));
  EXPECT_TRUE(r.actions.middleClick);
  // Stationary right hand; cursor target must equal the first
  // observed position.
  expectOnly(r.actions, "middle",
             r.firstCursorX, r.firstCursorY);
}

TEST_F(ActionMap, RightClick) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.click.holdForDragMs = 100000;
  sm.Init(cfg);
  auto r = runFrames(sm, loadFixture(fixturePath("right_click.json")));
  EXPECT_TRUE(r.actions.rightClick);
  // Stationary right hand; cursor target must equal the first
  // observed position.
  expectOnly(r.actions, "right",
             r.firstCursorX, r.firstCursorY);
}

TEST_F(ActionMap, ScrollVertical) {
  GestureStateMachine sm;
  sm.Init({});
  auto r = runFrames(sm, loadFixture(fixturePath("scroll_vertical.json")));
  EXPECT_NE(r.actions.wheel, 0) << "vertical scroll must emit a wheel delta";
  // Small x-axis midpoint jitter can produce a tiny hWheel; allow both axes.
  expectOnly(r.actions, "wheel hwheel");
}

TEST_F(ActionMap, ScrollHorizontal) {
  GestureStateMachine sm;
  sm.Init({});
  auto r = runFrames(sm, loadFixture(fixturePath("scroll_horizontal.json")));
  EXPECT_NE(r.actions.hWheel, 0) << "horizontal scroll must emit an hWheel delta";
  expectOnly(r.actions, "wheel hwheel");
}

TEST_F(ActionMap, PauseToggle) {
  GestureStateMachine sm;
  sm.Init({});
  auto r = runFrames(sm, loadFixture(fixturePath("pause_toggle.json")));
  EXPECT_EQ(r.finalState, GlobalState::Paused)
      << "open left hand held past holdMs must toggle Paused";
}

TEST_F(ActionMap, EmergencyStop) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.twoHandOpenHoldMs = 500;
  sm.Init(cfg);
  auto r = runFrames(sm, loadFixture(fixturePath("emergency_stop.json")));
  EXPECT_EQ(r.finalState, GlobalState::EmergencyStopped);
  EXPECT_TRUE(r.actions.safeRelease);
}
