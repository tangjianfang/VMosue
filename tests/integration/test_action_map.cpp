// Per-action verification: one synthetic-landmark fixture per gesture,
// each asserting the action fires (positive) AND that no other action
// fires (anti-cross-talk). Fixtures live in tests/fixtures/actions/.
// This TU carries its own builder helpers (a superset of the e2e TU's,
// adding middle_pinch + push_z) so the two integration TUs are
// independent.
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

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
    rr.actions.cursorDx       += d.cursorDx;
    rr.actions.cursorDy       += d.cursorDy;
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
static void expectOnly(const ActionSet& a, const std::string& allow) {
  auto has = [&](const char* key) {
    return allow.find(key) != std::string::npos;
  };
  if (!has("cursor"))    { EXPECT_EQ(a.cursorDx, 0); EXPECT_EQ(a.cursorDy, 0); }
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
  // The camera image is mirrored (selfie convention) so CursorController
  // negates dx: a hand moving right (+x_base) moves the cursor LEFT
  // (cursorDx < 0). This is the intended behaviour per the comment in
  // CursorController.cpp ("Negate dx so cursor follows the hand as the
  // user sees it").
  EXPECT_LT(r.actions.cursorDx, 0) << "cursor must move left when hand moves right (mirrored camera)";
  expectOnly(r.actions, "cursor");
}

TEST_F(ActionMap, LeftClick) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.click.suppressSingleClickInDoubleWindow = false;
  sm.Init(cfg);
  auto r = runFrames(sm, loadFixture(fixturePath("left_click.json")));
  EXPECT_TRUE(r.actions.leftClick);
  expectOnly(r.actions, "leftClick");
}

TEST_F(ActionMap, DoubleClick) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.click.doubleClickWindowMs = 400;  // pin the window for determinism
  sm.Init(cfg);
  auto r = runFrames(sm, loadFixture(fixturePath("double_click.json")));
  EXPECT_TRUE(r.actions.leftDoubleClick);
  // Allow left-family events; forbid unrelated families.
  expectOnly(r.actions, "leftClick double leftDown leftUp");
}

TEST_F(ActionMap, LeftDrag) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.click.holdForDragMs = 200;
  sm.Init(cfg);
  auto r = runFrames(sm, loadFixture(fixturePath("left_drag.json")));
  EXPECT_TRUE(r.actions.leftDown) << "drag should press LMB";
  EXPECT_TRUE(r.actions.leftUp)   << "drag should release LMB";
  // Same mirrored-camera convention as CursorMove: hand moves right (+x),
  // cursor moves left (cursorDx < 0).
  EXPECT_LT(r.actions.cursorDx, 0) << "drag should move the cursor (mirrored)";
  expectOnly(r.actions, "leftDown leftUp cursor");
}

TEST_F(ActionMap, MiddleClick) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.click.holdForDragMs = 100000;
  sm.Init(cfg);
  auto r = runFrames(sm, loadFixture(fixturePath("middle_click.json")));
  EXPECT_TRUE(r.actions.middleClick);
  expectOnly(r.actions, "middle");
}

TEST_F(ActionMap, RightClick) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.click.holdForDragMs = 100000;
  sm.Init(cfg);
  auto r = runFrames(sm, loadFixture(fixturePath("right_click.json")));
  EXPECT_TRUE(r.actions.rightClick);
  expectOnly(r.actions, "right");
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
