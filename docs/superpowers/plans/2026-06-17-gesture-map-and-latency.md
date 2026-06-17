# Gesture→Action Map, Per-Action Verification, and Latency Work — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce an authoritative gesture→action map, back every row with an isolated synthetic-landmark test that proves the action fires and nothing else does, and instrument the per-frame latency chain so optimization is evidence-driven.

**Architecture:** Reuse the existing fixture mechanism from `test_pipeline_e2e.cpp` (high-level semantic JSON expanded into 21-point landmarks by a builder, driven through `GestureStateMachine`, asserting on the merged `ActionSet`). Extend the builder to express middle-pinch and world-Z push. Add a per-action test TU. Instrument hot-path segments with the existing header-only `ProfileGuard`.

**Tech Stack:** C++20, GoogleTest, nlohmann/json, CMake + MSVC (build via the Community VS instance — see `docs/build-notes.md`), `ProfileGuard` (`src/util/ProfileGuard.h`).

**Build/test commands (this environment):** configure once with
`cmake -B build -S . -G "Visual Studio 17 2022" -A x64 -DCMAKE_GENERATOR_INSTANCE="C:\Program Files\Microsoft Visual Studio\2022\Community" -DCMAKE_TOOLCHAIN_FILE="%USERPROFILE%\vcpkg\scripts\buildsystems\vcpkg.cmake"`,
then build `cmake --build build --config Release --target vmosue_tests` and
run `ctest --test-dir build -C Release --output-on-failure`. (vcpkg must be
checked out at baseline `58613d0`; see prior notes.)

---

## File Structure

- `docs/user/gesture-action-map.md` — **create**. Authoritative G1 table.
- `docs/user/gestures.md` — **modify**. Add a link to the map doc.
- `tests/fixtures/actions/*.json` — **create** (10 fixtures). One per action.
- `tests/fixtures/README.md` — **modify**. Document the per-action fixtures + the extended schema flags.
- `tests/integration/test_action_map.cpp` — **create**. One test per fixture: positive + anti-cross-talk assertions. Contains its own copy of the fixture-builder helpers (extended with `middle_pinch` + `push_z`) so it does not depend on the e2e TU.
- `tests/CMakeLists.txt` — **modify**. Add the new TU to `vmosue_tests`.
- `src/app/App.cpp` — **modify**. Wrap the four hot-path segments in `ProfileGuard` spans and add a periodic P50/P95 log line.
- `src/util/ProfileGuard.h` — **modify**. Add a `P50Ms` accessor (companion to the existing `P95Ms`).
- `tests/unit/test_profile_guard.cpp` — **modify**. Test `P50Ms`.

---

## Part G1 — Authoritative gesture→action map

### Task 1: Write the gesture→action map document

**Files:**
- Create: `docs/user/gesture-action-map.md`
- Modify: `docs/user/gestures.md` (add a link near the top)

- [ ] **Step 1: Create the map document**

Create `docs/user/gesture-action-map.md` with this content:

```markdown
# Gesture → Action Map (authoritative)

Developer / QA reference. The normative definition of every gesture:
what landmarks and thresholds trigger it, which `ActionSet` field it
sets, the system action injected, the cross-talk it must avoid, and the
test fixture that proves the row.

Sources: `src/gesture/*` detectors, `src/gesture/GestureStateMachine.cpp`,
`src/input/InputInjector.cpp`. Landmark indices follow MediaPipe Hands
(0=wrist, 4=thumb tip, 5=index MCP, 8=index tip, 12=middle tip).

| # | Gesture | Hand | Trigger (landmarks / semantics) | ActionSet field | Injection | Must NOT also trigger | Fixture |
|---|---------|------|----------------------------------|-----------------|-----------|------------------------|---------|
| 1 | Cursor move | Right | Index-MCP (5) translation × sensitivity, past adaptive dead-zone | `cursorDx`, `cursorDy` | mouse move (relative) | any click | `cursor_move.json` |
| 2 | Left click | Right | dist(4,8) < pinch threshold then > release threshold within the double-click window | `leftClick` | LMB down+up | drag, middle, right | `left_click.json` |
| 3 | Double click | Right | Two qualifying pinch-releases within the system double-click window (`GetDoubleClickTime`, 200–900 ms) | `leftDoubleClick` | LMB ×2 | single click held | `double_click.json` |
| 4 | Left drag | Right | dist(4,8) < pinch held > `holdForDragMs`, then move, then release | `leftDown` → `cursorDx/Dy` → `leftUp` | LMB down, move, up | single/double click | `left_drag.json` |
| 5 | Middle click | Right | dist(4,12) < pinch then > release; suppressed if a left event fired same frame | `middleClick` | MMB down+up | left click | `middle_click.json` |
| 6 | Right click | Right | `world[8].z` < `world[0].z − zThreshold` (approach) then retract within `[minWindowMs, windowMs]`; suppressed if any left/middle event fired same frame | `rightClick` | RMB down+up | left, middle | `right_click.json` |
| 7a | Scroll (vertical) | Left | dist(8,12) below scroll-enter threshold, vertical midpoint motion | `wheel` (+ = up) | wheel | pause | `scroll_vertical.json` |
| 7b | Scroll (horizontal) | Left | as 7a, horizontal midpoint motion | `hWheel` (+ = right) | hwheel (tilt) | pause | `scroll_horizontal.json` |
| 8 | Pause / Resume | Left | Open left hand (4 fingertips above MCPs) held ≥ `holdMs` | state → Paused / Active | (none) | scroll | `pause_toggle.json` |
| 9 | Emergency stop | Either | Both hands open ≥ `twoHandOpenHoldMs` | `safeRelease`, state → EmergencyStopped | release all buttons | pause | `emergency_stop.json` |

## Arbitration rules

- **Left wins.** Within one frame the click detector evaluates the
  thumb-index pinch before the thumb-middle pinch; a left event
  suppresses the middle event (`ClickDetector::OnLandmarks`).
- **Right-click yields to left/middle.** The state machine drops
  `rightClick` if any of `leftClick / leftDoubleClick / leftDown /
  leftUp / middleClick` is set in the same frame
  (`GestureStateMachine.cpp`), preventing a conflicting `leftUp +
  rightClick` injection during a pinch-drag that ends on a forward push.
- **Pause short-circuits.** While `Paused`, only the pause detector runs;
  all other detectors are skipped until resumed.
- **Emergency stop is terminal.** Once `EmergencyStopped`, gestures are
  ignored until the hotkey / restart clears it.

## Thresholds: where each value comes from

- **Adaptive (observed):** pinch / release, scroll enter / exit,
  cursor dead-zone, air-click Z — derived by `AdaptiveController` from
  rolling signal statistics, with cold-start fallback to the v0.4
  constants. See `docs/superpowers/specs/2026-06-17-adaptive-parameters.md`.
- **System:** double-click window = `GetDoubleClickTime()` clamped to
  [200, 900] ms (`DisplayInfo::SystemDoubleClickTimeMs`).
- **Config-fixed:** `holdForDragMs`, air-click `windowMs / minWindowMs /
  cooldownMs`, `twoHandOpenHoldMs`.

Every row above is backed by a test in
`tests/integration/test_action_map.cpp`.
```

- [ ] **Step 2: Link it from the user gestures doc**

In `docs/user/gestures.md`, immediately after the opening paragraph
(the line ending "...added in a future release."), add:

```markdown

> **Developers / QA:** for the precise landmark-and-threshold
> definition of each gesture and its mapped action, see
> [gesture-action-map.md](gesture-action-map.md).
```

- [ ] **Step 3: Commit**

```bash
git add docs/user/gesture-action-map.md docs/user/gestures.md
git commit -m "docs(gestures): authoritative gesture->action map"
```

---

## Part G2 — Per-action fixtures and tests

> The test TU carries its own builder helpers (a superset of the e2e
> TU's, adding `middle_pinch` and `push_z`). We intentionally duplicate
> rather than share a header so the two integration TUs stay
> independent and a change to one cannot silently break the other.

### Task 2: Scaffold the per-action test TU with the cursor fixture

**Files:**
- Create: `tests/fixtures/actions/cursor_move.json`
- Create: `tests/integration/test_action_map.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the cursor fixture**

Create `tests/fixtures/actions/cursor_move.json`:

```json
{
  "description": "Right-hand index-MCP walks rightward; expect cursorDx>0 and no clicks.",
  "frames": [
    {"ts_ms": 0,   "right": {"x_base": 0.30, "y_mcp": 0.50}},
    {"ts_ms": 50,  "right": {"x_base": 0.34, "y_mcp": 0.50}},
    {"ts_ms": 100, "right": {"x_base": 0.38, "y_mcp": 0.50}},
    {"ts_ms": 150, "right": {"x_base": 0.42, "y_mcp": 0.50}},
    {"ts_ms": 200, "right": {"x_base": 0.46, "y_mcp": 0.50}}
  ]
}
```

- [ ] **Step 2: Write the test TU (builder + loader + cursor test)**

Create `tests/integration/test_action_map.cpp`:

```cpp
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
  float push_z = 0.0f;   // world[8].z relative to wrist (0); negative = toward camera
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
  LeftSpec left{};
};

// Right hand: index MCP (5) is the cursor pivot. Optionally pinch
// (thumb-index), middle-pinch (thumb-middle), open palm, and a world-Z
// push on the index tip for the air-click gesture.
static HandLandmarks makeRightHand(const RightSpec& r) {
  HandLandmarks lm{};
  lm.handedness = 1;
  lm.points[5] = {r.x_base, r.y_mcp, 0.0f};
  lm.points[9] = {r.x_base, r.y_mcp, 0.0f};
  lm.points[13] = {r.x_base, r.y_mcp, 0.0f};
  lm.points[17] = {r.x_base, r.y_mcp, 0.0f};
  if (r.open) {
    lm.points[8] = {r.x_base, r.y_mcp - 0.2f, 0.0f};
    lm.points[12] = {r.x_base, r.y_mcp - 0.2f, 0.0f};
    lm.points[16] = {r.x_base, r.y_mcp - 0.2f, 0.0f};
    lm.points[20] = {r.x_base, r.y_mcp - 0.2f, 0.0f};
  } else {
    lm.points[8] = {r.x_base, r.y_mcp + 0.1f, 0.0f};
    lm.points[12] = {r.x_base, r.y_mcp + 0.1f, 0.0f};
    lm.points[16] = {r.x_base, r.y_mcp + 0.1f, 0.0f};
    lm.points[20] = {r.x_base, r.y_mcp + 0.1f, 0.0f};
  }
  // Thumb default away from both index and middle so no pinch fires.
  lm.points[4] = {r.x_base - 0.10f, r.y_mcp, 0.0f};
  if (r.pinch) {
    // Thumb tip at index tip: dist(4,8) ~ 0.01 < pinch threshold.
    lm.points[4] = {r.x_base, r.y_mcp, 0.0f};
    lm.points[8] = {r.x_base + 0.01f, r.y_mcp, 0.0f};
  } else if (r.middle_pinch) {
    // Thumb tip at middle tip: dist(4,12) ~ 0.01; keep index far.
    lm.points[4] = {r.x_base, r.y_mcp, 0.0f};
    lm.points[12] = {r.x_base + 0.01f, r.y_mcp, 0.0f};
    lm.points[8] = {r.x_base + 0.20f, r.y_mcp, 0.0f};
  }
  // World landmarks for the air-click (right-click) gesture. Wrist (0)
  // at z=0; index tip (8) at push_z. AirClickDetector reads world only.
  lm.world[0].z = 0.0f;
  lm.world[8].z = r.push_z;
  return lm;
}

// Left hand: open palm, or two-finger scroll. scroll_dx/dy shift the
// index tip so the scroll detector sees motion (negative dy = up =
// positive wheel; positive dx = right = positive hWheel).
static HandLandmarks makeLeftHand(const LeftSpec& l) {
  HandLandmarks lm{};
  lm.handedness = 0;
  lm.points[5] = {l.x_base, 0.7f, 0.0f};
  lm.points[9] = {l.x_base, 0.7f, 0.0f};
  lm.points[13] = {l.x_base, 0.7f, 0.0f};
  lm.points[17] = {l.x_base, 0.7f, 0.0f};
  if (l.open) {
    lm.points[8] = {l.x_base, 0.5f, 0.0f};
    lm.points[12] = {l.x_base, 0.5f, 0.0f};
    lm.points[16] = {l.x_base, 0.5f, 0.0f};
    lm.points[20] = {l.x_base, 0.5f, 0.0f};
  } else if (l.scroll_active) {
    const float baseY = 0.6f;
    lm.points[8] = {l.x_base + l.scroll_dx, baseY + l.scroll_dy, 0.0f};
    lm.points[12] = {l.x_base, baseY, 0.0f};
    lm.points[16] = {l.x_base, 0.9f, 0.0f};
    lm.points[20] = {l.x_base, 0.9f, 0.0f};
  } else {
    lm.points[8] = {l.x_base, 0.8f, 0.0f};
    lm.points[12] = {l.x_base, 0.8f, 0.0f};
    lm.points[16] = {l.x_base, 0.8f, 0.0f};
    lm.points[20] = {l.x_base, 0.8f, 0.0f};
  }
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
      s.right.x_base = r.value("x_base", 0.5f);
      s.right.y_mcp = r.value("y_mcp", 0.5f);
      s.right.open = r.value("open", false);
      s.right.pinch = r.value("pinch", false);
      s.right.middle_pinch = r.value("middle_pinch", false);
      s.right.push_z = r.value("push_z", 0.0f);
    }
    if (jf.contains("left")) {
      s.has_left = true;
      const auto& l = jf.at("left");
      s.left.x_base = l.value("x_base", 0.5f);
      s.left.open = l.value("open", false);
      s.left.scroll_active = l.value("scroll_active", false);
      s.left.scroll_dy = l.value("scroll_dy", 0.0f);
      s.left.scroll_dx = l.value("scroll_dx", 0.0f);
    }
    out.push_back(s);
  }
  return out;
}

static std::vector<HandLandmarks> makeHands(const FrameSpec& s) {
  std::vector<HandLandmarks> hands;
  if (s.has_left) hands.push_back(makeLeftHand(s.left));
  if (s.has_right) hands.push_back(makeRightHand(s.right));
  return hands;
}

// Drive frames, return merged ActionSet AND the final state.
struct RunResult {
  ActionSet actions{};
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
    rr.actions.cursorDx += d.cursorDx;
    rr.actions.cursorDy += d.cursorDy;
    rr.actions.wheel += d.wheel;
    rr.actions.hWheel += d.hWheel;
    rr.actions.leftClick = rr.actions.leftClick || d.leftClick;
    rr.actions.leftDoubleClick = rr.actions.leftDoubleClick || d.leftDoubleClick;
    rr.actions.leftDown = rr.actions.leftDown || d.leftDown;
    rr.actions.leftUp = rr.actions.leftUp || d.leftUp;
    rr.actions.rightClick = rr.actions.rightClick || d.rightClick;
    rr.actions.middleClick = rr.actions.middleClick || d.middleClick;
    rr.actions.safeRelease = rr.actions.safeRelease || d.safeRelease;
    prevTs = f.ts_ms;
  }
  rr.finalState = sm.State();
  return rr;
}

static std::string fixturePath(const char* name) {
#ifdef VMOSUE_TEST_FIXTURES_DIR
  std::string candidate =
      VMOSUE_STRINGIFY(VMOSUE_TEST_FIXTURES_DIR) "/actions/";
  candidate += name;
  std::ifstream f(candidate);
  if (f.is_open()) return candidate;
#endif
  std::string rel = std::string("tests/fixtures/actions/") + name;
  return rel;
}

// Assert NO action field is set except the ones named. Keeps each
// test's anti-cross-talk check to one line.
static void expectOnly(const ActionSet& a, const std::string& allow) {
  auto banned = [&](const char* key) { return allow.find(key) == std::string::npos; };
  if (banned("cursor")) { EXPECT_EQ(a.cursorDx, 0); EXPECT_EQ(a.cursorDy, 0); }
  if (banned("leftClick"))  EXPECT_FALSE(a.leftClick);
  if (banned("double"))     EXPECT_FALSE(a.leftDoubleClick);
  if (banned("leftDown"))   EXPECT_FALSE(a.leftDown);
  if (banned("leftUp"))     EXPECT_FALSE(a.leftUp);
  if (banned("right"))      EXPECT_FALSE(a.rightClick);
  if (banned("middle"))     EXPECT_FALSE(a.middleClick);
  if (banned("wheel"))      EXPECT_EQ(a.wheel, 0);
  if (banned("hwheel"))     EXPECT_EQ(a.hWheel, 0);
  if (banned("safe"))       EXPECT_FALSE(a.safeRelease);
}

class ActionMap : public ::testing::Test {
 protected:
  void SetUp() override { vmosue::GetSignalObserver().Reset(); }
};

}  // namespace

TEST_F(ActionMap, CursorMove) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.click.holdForDragMs = 100000;  // never let a stray pinch become a drag
  sm.Init(cfg);
  auto r = runFrames(sm, loadFixture(fixturePath("cursor_move.json")));
  EXPECT_GT(r.actions.cursorDx, 0) << "cursor must move along +x";
  expectOnly(r.actions, "cursor");
}
```

- [ ] **Step 3: Register the TU in CMake**

In `tests/CMakeLists.txt`, in the `add_executable(vmosue_tests ...)`
list, add the new TU right after the existing integration test line
`integration/test_pipeline_e2e.cpp`:

```cmake
  integration/test_pipeline_e2e.cpp
  integration/test_action_map.cpp
```

- [ ] **Step 4: Build and run, expect the cursor test to pass**

Run: `cmake --build build --config Release --target vmosue_tests` then
`ctest --test-dir build -C Release -R ActionMap --output-on-failure`
Expected: `ActionMap.CursorMove` PASS.

- [ ] **Step 5: Commit**

```bash
git add tests/integration/test_action_map.cpp tests/fixtures/actions/cursor_move.json tests/CMakeLists.txt
git commit -m "test(actions): per-action verification scaffold + cursor fixture"
```

### Task 3: Left click, double click, drag fixtures + tests

**Files:**
- Create: `tests/fixtures/actions/left_click.json`, `double_click.json`, `left_drag.json`
- Modify: `tests/integration/test_action_map.cpp`

- [ ] **Step 1: Write the three fixtures**

`left_click.json` (quick pinch then release; default config does NOT
suppress single clicks, so a lone pinch-release emits `leftClick`):

```json
{
  "description": "Quick thumb-index pinch then release -> leftClick.",
  "frames": [
    {"ts_ms": 0,   "right": {"x_base": 0.50, "y_mcp": 0.50, "pinch": false}},
    {"ts_ms": 50,  "right": {"x_base": 0.50, "y_mcp": 0.50, "pinch": true}},
    {"ts_ms": 100, "right": {"x_base": 0.50, "y_mcp": 0.50, "pinch": false}}
  ]
}
```

`double_click.json` (two pinch-releases ~120 ms apart, inside the
default 200–900 ms window so the second promotes to a double click):

```json
{
  "description": "Two quick pinch-releases within the double-click window -> leftDoubleClick.",
  "frames": [
    {"ts_ms": 0,   "right": {"x_base": 0.50, "y_mcp": 0.50, "pinch": false}},
    {"ts_ms": 40,  "right": {"x_base": 0.50, "y_mcp": 0.50, "pinch": true}},
    {"ts_ms": 80,  "right": {"x_base": 0.50, "y_mcp": 0.50, "pinch": false}},
    {"ts_ms": 120, "right": {"x_base": 0.50, "y_mcp": 0.50, "pinch": true}},
    {"ts_ms": 160, "right": {"x_base": 0.50, "y_mcp": 0.50, "pinch": false}}
  ]
}
```

`left_drag.json` (pinch held > `holdForDragMs`, move, release):

```json
{
  "description": "Pinch held past the drag threshold, then move, then release -> leftDown, cursor delta, leftUp.",
  "frames": [
    {"ts_ms": 0,   "right": {"x_base": 0.50, "y_mcp": 0.50, "pinch": true}},
    {"ts_ms": 100, "right": {"x_base": 0.50, "y_mcp": 0.50, "pinch": true}},
    {"ts_ms": 250, "right": {"x_base": 0.50, "y_mcp": 0.50, "pinch": true}},
    {"ts_ms": 300, "right": {"x_base": 0.55, "y_mcp": 0.50, "pinch": true}},
    {"ts_ms": 350, "right": {"x_base": 0.60, "y_mcp": 0.50, "pinch": true}},
    {"ts_ms": 400, "right": {"x_base": 0.60, "y_mcp": 0.50, "pinch": false}}
  ]
}
```

- [ ] **Step 2: Add the three tests**

Append to `tests/integration/test_action_map.cpp` (after `CursorMove`):

```cpp
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
  cfg.click.doubleClickWindowMs = 400;  // pin the window so the fixture is deterministic
  sm.Init(cfg);
  auto r = runFrames(sm, loadFixture(fixturePath("double_click.json")));
  EXPECT_TRUE(r.actions.leftDoubleClick);
  // The first click may be held/emitted depending on suppression; allow
  // left-family events, forbid the unrelated families.
  expectOnly(r.actions, "leftClick double leftDown leftUp");
}

TEST_F(ActionMap, LeftDrag) {
  GestureStateMachine sm;
  GestureStateMachine::Config cfg;
  cfg.click.holdForDragMs = 200;
  sm.Init(cfg);
  auto r = runFrames(sm, loadFixture(fixturePath("left_drag.json")));
  EXPECT_TRUE(r.actions.leftDown) << "drag should press LMB";
  EXPECT_TRUE(r.actions.leftUp) << "drag should release LMB";
  EXPECT_GT(r.actions.cursorDx, 0) << "drag should move the cursor";
  expectOnly(r.actions, "leftDown leftUp cursor");
}
```

- [ ] **Step 3: Build and run, expect all three to pass**

Run: `cmake --build build --config Release --target vmosue_tests` then
`ctest --test-dir build -C Release -R ActionMap --output-on-failure`
Expected: `ActionMap.LeftClick`, `ActionMap.DoubleClick`,
`ActionMap.LeftDrag` PASS. If `LeftClick`/`DoubleClick` interact via the
suppression flag, adjust the per-test `cfg.click` (not the fixture).

- [ ] **Step 4: Commit**

```bash
git add tests/integration/test_action_map.cpp tests/fixtures/actions/left_click.json tests/fixtures/actions/double_click.json tests/fixtures/actions/left_drag.json
git commit -m "test(actions): left click, double click, drag fixtures"
```

### Task 4: Middle click and right click fixtures + tests

**Files:**
- Create: `tests/fixtures/actions/middle_click.json`, `right_click.json`
- Modify: `tests/integration/test_action_map.cpp`

- [ ] **Step 1: Write the two fixtures**

`middle_click.json`:

```json
{
  "description": "Thumb-middle pinch then release -> middleClick (no left pinch).",
  "frames": [
    {"ts_ms": 0,   "right": {"x_base": 0.50, "y_mcp": 0.50, "middle_pinch": false}},
    {"ts_ms": 50,  "right": {"x_base": 0.50, "y_mcp": 0.50, "middle_pinch": true}},
    {"ts_ms": 100, "right": {"x_base": 0.50, "y_mcp": 0.50, "middle_pinch": false}}
  ]
}
```

`right_click.json` (index-tip world-Z pushes toward camera then
retracts within the air-click window; default `zApproachThreshold` =
0.02, window 80–200 ms):

```json
{
  "description": "Index-tip world-z push toward camera then retract within the window -> rightClick.",
  "frames": [
    {"ts_ms": 0,   "right": {"x_base": 0.50, "y_mcp": 0.50, "push_z": 0.00}},
    {"ts_ms": 50,  "right": {"x_base": 0.50, "y_mcp": 0.50, "push_z": -0.05}},
    {"ts_ms": 150, "right": {"x_base": 0.50, "y_mcp": 0.50, "push_z": 0.00}}
  ]
}
```

- [ ] **Step 2: Add the two tests**

Append to `tests/integration/test_action_map.cpp`:

```cpp
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
```

- [ ] **Step 3: Build and run, expect both to pass**

Run: `cmake --build build --config Release --target vmosue_tests` then
`ctest --test-dir build -C Release -R "ActionMap.MiddleClick|ActionMap.RightClick" --output-on-failure`
Expected: both PASS. (RightClick exercises the `world[]`-population fix
from commit `3de39a2`; if it fails, the builder is not setting
`lm.world[8].z` — recheck `makeRightHand`.)

- [ ] **Step 4: Commit**

```bash
git add tests/integration/test_action_map.cpp tests/fixtures/actions/middle_click.json tests/fixtures/actions/right_click.json
git commit -m "test(actions): middle click + right click fixtures"
```

### Task 5: Scroll (vertical + horizontal), pause, emergency-stop fixtures + tests

**Files:**
- Create: `tests/fixtures/actions/scroll_vertical.json`, `scroll_horizontal.json`, `pause_toggle.json`, `emergency_stop.json`
- Modify: `tests/integration/test_action_map.cpp`

- [ ] **Step 1: Write the four fixtures**

`scroll_vertical.json`:

```json
{
  "description": "Left two-finger scroll moving up -> wheel != 0.",
  "frames": [
    {"ts_ms": 0,   "left": {"x_base": 0.30, "scroll_active": true, "scroll_dy":  0.000}},
    {"ts_ms": 50,  "left": {"x_base": 0.30, "scroll_active": true, "scroll_dy": -0.003}},
    {"ts_ms": 100, "left": {"x_base": 0.30, "scroll_active": true, "scroll_dy": -0.006}},
    {"ts_ms": 150, "left": {"x_base": 0.30, "scroll_active": true, "scroll_dy": -0.009}},
    {"ts_ms": 200, "left": {"x_base": 0.30, "scroll_active": true, "scroll_dy": -0.012}}
  ]
}
```

`scroll_horizontal.json`:

```json
{
  "description": "Left two-finger scroll moving right -> hWheel != 0.",
  "frames": [
    {"ts_ms": 0,   "left": {"x_base": 0.30, "scroll_active": true, "scroll_dx": 0.000}},
    {"ts_ms": 50,  "left": {"x_base": 0.30, "scroll_active": true, "scroll_dx": 0.003}},
    {"ts_ms": 100, "left": {"x_base": 0.30, "scroll_active": true, "scroll_dx": 0.006}},
    {"ts_ms": 150, "left": {"x_base": 0.30, "scroll_active": true, "scroll_dx": 0.009}},
    {"ts_ms": 200, "left": {"x_base": 0.30, "scroll_active": true, "scroll_dx": 0.012}}
  ]
}
```

`pause_toggle.json` (open left hand held past the pause hold; default
`PauseDetector` holdMs is 1000, so span > 1000 ms):

```json
{
  "description": "Open left hand held > holdMs -> state toggles to Paused.",
  "frames": [
    {"ts_ms": 0,    "left": {"open": true}},
    {"ts_ms": 300,  "left": {"open": true}},
    {"ts_ms": 600,  "left": {"open": true}},
    {"ts_ms": 900,  "left": {"open": true}},
    {"ts_ms": 1200, "left": {"open": true}}
  ]
}
```

`emergency_stop.json` (both hands open past `twoHandOpenHoldMs`=500):

```json
{
  "description": "Both hands open > twoHandOpenHoldMs -> EmergencyStopped + safeRelease.",
  "frames": [
    {"ts_ms": 0,   "left": {"open": true}, "right": {"open": true}},
    {"ts_ms": 200, "left": {"open": true}, "right": {"open": true}},
    {"ts_ms": 400, "left": {"open": true}, "right": {"open": true}},
    {"ts_ms": 600, "left": {"open": true}, "right": {"open": true}}
  ]
}
```

- [ ] **Step 2: Add the four tests**

Append to `tests/integration/test_action_map.cpp`:

```cpp
TEST_F(ActionMap, ScrollVertical) {
  GestureStateMachine sm;
  sm.Init({});
  auto r = runFrames(sm, loadFixture(fixturePath("scroll_vertical.json")));
  EXPECT_NE(r.actions.wheel, 0) << "vertical scroll must emit a wheel delta";
  expectOnly(r.actions, "wheel hwheel");  // dx/dy of midpoint may bleed tiny hWheel; allow both wheel axes
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
```

- [ ] **Step 3: Build and run, expect all four to pass**

Run: `cmake --build build --config Release --target vmosue_tests` then
`ctest --test-dir build -C Release -R ActionMap --output-on-failure`
Expected: all 10 `ActionMap.*` tests PASS. If `ScrollVertical` emits a
small stray `hWheel` (midpoint x jitter), the `expectOnly` allow-list
already permits both wheel axes; if pause does not trip, widen the
fixture span (the detector's default holdMs is 1000 ms).

- [ ] **Step 4: Commit**

```bash
git add tests/integration/test_action_map.cpp tests/fixtures/actions/scroll_vertical.json tests/fixtures/actions/scroll_horizontal.json tests/fixtures/actions/pause_toggle.json tests/fixtures/actions/emergency_stop.json
git commit -m "test(actions): scroll, pause, emergency-stop fixtures"
```

### Task 6: Document the extended fixture schema

**Files:**
- Modify: `tests/fixtures/README.md`

- [ ] **Step 1: Append the per-action fixture documentation**

Add to the end of `tests/fixtures/README.md`:

```markdown

## Per-action fixtures (`actions/`)

`tests/fixtures/actions/*.json` are minimal, single-action sequences
consumed by `tests/integration/test_action_map.cpp`. Each is the
executable proof of one row in
`docs/user/gesture-action-map.md`. Tests assert the action fires AND
that no other action fires (anti-cross-talk).

Frame schema (a superset of `sample_landmarks.json`):

- `right`: `x_base`, `y_mcp`, `open`, `pinch` (thumb-index),
  `middle_pinch` (thumb-middle), `push_z` (index-tip `world.z` relative
  to the wrist; negative = toward camera, drives the right-click
  air gesture).
- `left`: `x_base`, `open`, `scroll_active`, `scroll_dy` (negative =
  up), `scroll_dx` (positive = right).

Missing keys default to "feature off". Per-test `GestureStateMachine::
Config` overrides pin any timing the fixture depends on so the tests are
deterministic regardless of adaptive warm-up.
```

- [ ] **Step 2: Commit**

```bash
git add tests/fixtures/README.md
git commit -m "docs(tests): document per-action fixture schema"
```

---

## Part G3 — Latency instrumentation (measure first)

### Task 7: Add P50Ms to ProfileGuard

**Files:**
- Modify: `src/util/ProfileGuard.h`
- Modify: `tests/unit/test_profile_guard.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/test_profile_guard.cpp`:

```cpp
TEST(ProfileGuard, P50IsMedianOfWindow) {
  using vmosue::ProfileGuard;
  ProfileGuard::Reset("p50test");
  // Record 1..9 ms; median is 5.
  for (int i = 1; i <= 9; ++i) {
    ProfileGuard g("p50test", /*enabled=*/false);
    // enabled=false skips the warn log but RecordSample still runs in
    // the dtor; sleep-free, we just push a known value via the helper.
  }
  // Direct push path: use the public Reset + the dtor-recorded samples
  // are timing-based, so instead assert P50 is within the observed
  // window bounds (>0 once samples exist).
  // Push deterministic samples through the test-only accessor:
  for (int i = 1; i <= 9; ++i) ProfileGuard::RecordSampleForTest("p50test", double(i));
  EXPECT_DOUBLE_EQ(ProfileGuard::P50Ms("p50test"), 5.0);
}
```

- [ ] **Step 2: Run, expect compile failure**

Run: `cmake --build build --config Release --target vmosue_tests`
Expected: FAIL — `P50Ms` and `RecordSampleForTest` are not declared.

- [ ] **Step 3: Implement P50Ms + a test-only sample injector**

In `src/util/ProfileGuard.h`, add a public `RecordSampleForTest` (thin
wrapper over the private `RecordSample`) and `P50Ms`, right after the
existing `P95Ms` method:

```cpp
  // Test-only: inject a known sample so percentile math can be checked
  // without timing-dependent dtor samples.
  static void RecordSampleForTest(const char* name, double ms) {
    RecordSample(name, ms);
  }

  // Median (P50) over the rolling window; 0.0 if empty. Same
  // nth_element approach as P95Ms — only the median element needs to
  // reach its sorted position.
  static double P50Ms(const char* name) {
    std::lock_guard<std::mutex> lk(Mu());
    auto& v = Series(name);
    if (v.empty()) return 0.0;
    std::vector<double> copy = v;
    const size_t idx = copy.size() / 2;
    std::nth_element(copy.begin(), copy.begin() + idx, copy.end());
    return copy[idx];
  }
```

- [ ] **Step 4: Simplify the test to use the deterministic path only**

Replace the test body written in Step 1 with the deterministic version
(the dtor-loop preamble was illustrative; drop it):

```cpp
TEST(ProfileGuard, P50IsMedianOfWindow) {
  using vmosue::ProfileGuard;
  ProfileGuard::Reset("p50test");
  for (int i = 1; i <= 9; ++i)
    ProfileGuard::RecordSampleForTest("p50test", double(i));
  EXPECT_DOUBLE_EQ(ProfileGuard::P50Ms("p50test"), 5.0);
  EXPECT_DOUBLE_EQ(ProfileGuard::P95Ms("p50test"), 9.0);
}
```

- [ ] **Step 5: Run, expect pass**

Run: `cmake --build build --config Release --target vmosue_tests` then
`ctest --test-dir build -C Release -R ProfileGuard --output-on-failure`
Expected: `ProfileGuard.P50IsMedianOfWindow` PASS.

- [ ] **Step 6: Commit**

```bash
git add src/util/ProfileGuard.h tests/unit/test_profile_guard.cpp
git commit -m "feat(profile): add P50Ms + test-only sample injector"
```

### Task 8: Instrument the four hot-path segments + periodic P50/P95 log

**Files:**
- Modify: `src/app/App.cpp`

- [ ] **Step 1: Wrap the capture acquisition span**

In `App::captureLoop` (`src/app/App.cpp`), wrap the
`cam_.TryGetLatestFrame(f)` block so the acquisition cost is timed.
Immediately before the `if (cam_.TryGetLatestFrame(f)) {` line, open a
scope with a guard, and close it after the `}`:

```cpp
      {
        PROFILE_GUARD_DISABLED("lat_capture");  // Release: record-only, no warn spam
        if (cam_.TryGetLatestFrame(f)) {
          frameQ_.push(f);
          if (debug_) debug_->PushFrame(f);
        }
      }
```

(Replace the existing `if (cam_.TryGetLatestFrame(f)) { ... }` block
with the guarded version above; keep the inner body identical to what
is there now.)

- [ ] **Step 2: Wrap the inference IPC span**

In `App::inferenceLoop`, find the `detector_.Detect(...)` call and wrap
it:

```cpp
      std::vector<HandLandmarks> hands;
      {
        PROFILE_GUARD_DISABLED("lat_ipc_rtt");
        hands = detector_.Detect(downscaled);  // use the existing arg name
      }
```

(Use whatever the existing variable names are — wrap the existing
single `Detect` call, do not change its arguments.)

- [ ] **Step 3: Wrap the gesture span**

In `App::stateMachineLoop` (or wherever `sm_.OnLandmarks(...)` is
called from the consumer thread), wrap that call:

```cpp
      {
        PROFILE_GUARD_DISABLED("lat_gesture");
        sm_.OnLandmarks(hands, ts, dt);  // existing args
      }
```

- [ ] **Step 4: Add a periodic P50/P95 dump**

In `App::inferenceLoop`, where the FPS / idle bookkeeping already runs
once per second (search for `currentFps_`), add a throttled dump. Near
the top of `App.cpp` add a small static helper, then call it from the
once-per-second branch:

```cpp
// Emit one log line with P50/P95 for each instrumented latency span.
// Called at ~1 Hz from the inference loop. Cheap: four locked reads.
static void LogLatencyP50P95() {
  VMOSUE_LOG_INFO(
      "latency ms (P50/P95): capture {:.1f}/{:.1f}  ipc {:.1f}/{:.1f}  "
      "gesture {:.1f}/{:.1f}",
      ::vmosue::ProfileGuard::P50Ms("lat_capture"),
      ::vmosue::ProfileGuard::P95Ms("lat_capture"),
      ::vmosue::ProfileGuard::P50Ms("lat_ipc_rtt"),
      ::vmosue::ProfileGuard::P95Ms("lat_ipc_rtt"),
      ::vmosue::ProfileGuard::P50Ms("lat_gesture"),
      ::vmosue::ProfileGuard::P95Ms("lat_gesture"));
}
```

Then, inside the existing once-per-second block in `inferenceLoop`
(the same branch that updates `currentFps_`), add:

```cpp
        LogLatencyP50P95();
```

- [ ] **Step 5: Build the full app, expect success**

Run: `cmake --build build --config Release --target vmosue` (use the
Community-instance configure from the header).
Expected: BUILD succeeds; `vmosue.exe` links. No test asserts the log
line — it is operator-facing telemetry, verified by inspection at
runtime.

- [ ] **Step 6: Run the full test suite, expect no regressions**

Run: `cmake --build build --config Release --target vmosue_tests` then
`ctest --test-dir build -C Release --output-on-failure`
Expected: ALL tests pass (the prior 133 + the new `ActionMap.*` + the
new `ProfileGuard.P50IsMedianOfWindow`).

- [ ] **Step 7: Commit**

```bash
git add src/app/App.cpp
git commit -m "feat(perf): instrument capture/ipc/gesture latency, log P50/P95 at 1Hz"
```

### Task 9: Record the measurement procedure

**Files:**
- Modify: `docs/build-notes.md`

- [ ] **Step 1: Document how to read the latency telemetry**

Append to `docs/build-notes.md`:

```markdown
## Measuring per-frame latency

A Release build logs a latency line once per second to the VMosue log
(`%LOCALAPPDATA%\VMosue\logs\`):

```
latency ms (P50/P95): capture 0.4/0.9  ipc 35.2/61.0  gesture 0.1/0.2
```

- `capture` — frame acquisition + NV12→BGRA in the capture loop.
- `ipc` — the Python detector round-trip (`HandDetector::Detect`): pipe
  write of the BGRA frame + MediaPipe inference + response read. This is
  the suspected dominant cost; the numbers here decide whether the
  shared-memory IPC rewrite (ROADMAP D2/D3) is worth it.
- `gesture` — state machine + input injection.

To rank the bottleneck, run the app, perform gestures for ~30 s, and
read the steady-state P95 values from the log. Optimize the largest
first; re-run the `ActionMap.*` fixtures after any change to confirm no
recognition regression.
```

- [ ] **Step 2: Commit**

```bash
git add docs/build-notes.md
git commit -m "docs: how to read the per-frame latency telemetry"
```

---

## Self-Review

**Spec coverage:**
- G1 (authoritative map) → Task 1. ✓
- G2 (per-action fixtures, positive + anti-cross-talk, CI-able) → Tasks
  2–6 (10 fixtures + 10 tests + schema doc). ✓ All nine actions
  (cursor, left, double, drag, middle, right, scroll v/h, pause, e-stop)
  covered.
- G3 (measure latency chain first, P50/P95, evidence before rewrite) →
  Tasks 7–9 (P50 accessor, 3 instrumented spans + 1 Hz dump, procedure
  doc). The IPC-write-size and shared-memory items remain explicitly
  deferred to ROADMAP D2/D3 per the spec's non-goals. ✓
- Spec's "timing-stress fixture" idea: folded into the existing
  `ts_ms`-aware fixtures (each already carries real timestamps);
  a dedicated high-cadence stress fixture is a follow-up once D1 data
  shows whether it's needed — noted here so it isn't lost.

**Placeholder scan:** No TBD/TODO. Every code step shows complete code.
The one "use the existing arg name" notes in Task 8 are deliberate —
they wrap existing calls whose local names must not be guessed; the
instruction is to wrap, not rewrite. ✓

**Type consistency:** `RunResult{ActionSet actions; GlobalState finalState}`,
`expectOnly(const ActionSet&, std::string)`, `fixturePath(const char*)`,
`makeRightHand(const RightSpec&)`, `makeLeftHand(const LeftSpec&)`,
`RecordSampleForTest`/`P50Ms`/`P95Ms` are used consistently across
tasks. The fixture flag names (`pinch`, `middle_pinch`, `push_z`,
`scroll_dy`, `scroll_dx`, `open`, `scroll_active`) match between the
JSON, the loader, and the README. ✓

**Scope:** Single coherent plan — map → fixtures → measurement. The
heavy performance rewrites are out of scope by design. ✓
