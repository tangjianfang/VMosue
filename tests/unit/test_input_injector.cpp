#include <gtest/gtest.h>
#include "input/InputInjector.h"

using vmosue::InputInjector;

// The singleton must hand out the same instance across calls. If this
// ever fails, callers in the gesture loop and the emergency-stop path
// would diverge in their internal state (leftDown_), which would be a
// serious bug.
TEST(InputInjector, SingletonReturnsSameInstance) {
  EXPECT_EQ(&InputInjector::Get(), &InputInjector::Get());
}

// Internal state must start clean (or be reset by SafeReleaseAll). The
// spec asks us to call SafeReleaseAll first so the assertion is robust
// against test ordering.
TEST(InputInjector, SafeReleaseAllClearsLeftDown) {
  auto& inj = InputInjector::Get();
  inj.SafeReleaseAll();
  EXPECT_FALSE(inj.IsLeftDown());
}

// LeftClick() is a discrete gesture: it must leave the button logically
// released regardless of prior state. This is the test that the spec's
// LeftClickTogglesState name alluded to but never actually exercised.
TEST(InputInjector, LeftClickLeavesButtonReleased) {
  auto& inj = InputInjector::Get();
  inj.SafeReleaseAll();
  EXPECT_FALSE(inj.IsLeftDown());

  inj.LeftClick();
  EXPECT_FALSE(inj.IsLeftDown());

  // A second LeftClick after an unrelated LeftDown must also leave the
  // button released. This catches a regression where LeftClick might
  // accidentally toggle but not "round-trip" the state.
  inj.LeftDown();
  EXPECT_TRUE(inj.IsLeftDown());
  inj.LeftClick();
  EXPECT_FALSE(inj.IsLeftDown());
}

// LeftDown followed by LeftUp must round-trip the IsLeftDown flag.
// Headless / no-interactive-session test runners will see SendInput
// silently return 0, but IsLeftDown reflects our internal flag and is
// observable regardless of whether the OS accepted the input.
TEST(InputInjector, LeftDownAndLeftUpToggle) {
  auto& inj = InputInjector::Get();
  inj.SafeReleaseAll();
  EXPECT_FALSE(inj.IsLeftDown());

  inj.LeftDown();
  EXPECT_TRUE(inj.IsLeftDown());

  inj.LeftUp();
  EXPECT_FALSE(inj.IsLeftDown());
}

// SafeReleaseAll() must clear a LeftDown state without requiring a
// matching LeftUp. This is the primary emergency-stop path.
TEST(InputInjector, SafeReleaseAllForcesLeftUp) {
  auto& inj = InputInjector::Get();
  inj.SafeReleaseAll();
  EXPECT_FALSE(inj.IsLeftDown());

  inj.LeftDown();
  EXPECT_TRUE(inj.IsLeftDown());

  inj.SafeReleaseAll();
  EXPECT_FALSE(inj.IsLeftDown());

  // Idempotent: a second call must be safe and must not flip the flag.
  inj.SafeReleaseAll();
  EXPECT_FALSE(inj.IsLeftDown());
}

// Defense-in-depth: MoveCursor must hard-clamp any runaway delta to
// kMaxMovePerCall before reaching SendInput. Without this clamp a
// noisy first frame or a future pipeline bug that skips the dead-
// zone could yank the OS cursor thousands of pixels in one frame
// and freeze the user's session. SendInput returns 0 in headless
// sessions so we test the pure ClampDelta helper, which is the
// single place the bound is enforced.
TEST(InputInjector, ClampDeltaCapsRunawayValues) {
  // In-bounds values are passed through untouched.
  {
    int dx = 10, dy = -20;
    InputInjector::ClampDelta(dx, dy);
    EXPECT_EQ(dx, 10);
    EXPECT_EQ(dy, -20);
  }
  // Out-of-bounds positive values clamp to +kMaxMovePerCall.
  {
    int dx = 99999, dy = 600;
    InputInjector::ClampDelta(dx, dy);
    EXPECT_EQ(dx,  InputInjector::kMaxMovePerCall);
    EXPECT_EQ(dy,  InputInjector::kMaxMovePerCall);
  }
  // Out-of-bounds negative values clamp to -kMaxMovePerCall.
  {
    int dx = -99999, dy = -600;
    InputInjector::ClampDelta(dx, dy);
    EXPECT_EQ(dx, -InputInjector::kMaxMovePerCall);
    EXPECT_EQ(dy, -InputInjector::kMaxMovePerCall);
  }
  // Mixed signs: each axis is clamped independently.
  {
    int dx = 99999, dy = -99999;
    InputInjector::ClampDelta(dx, dy);
    EXPECT_EQ(dx,  InputInjector::kMaxMovePerCall);
    EXPECT_EQ(dy, -InputInjector::kMaxMovePerCall);
  }
  // Exact-bound values must NOT be mutated (clamp is inclusive).
  {
    int dx =  InputInjector::kMaxMovePerCall;
    int dy = -InputInjector::kMaxMovePerCall;
    InputInjector::ClampDelta(dx, dy);
    EXPECT_EQ(dx,  InputInjector::kMaxMovePerCall);
    EXPECT_EQ(dy, -InputInjector::kMaxMovePerCall);
  }
}

// MoveCursor with (0, 0) must be a silent no-op: the early-return
// happens BEFORE the clamp, so a degenerate input still costs
// nothing. We can't observe the SendInput side-effect in headless
// mode, but we can at least verify the function returns cleanly.
TEST(InputInjector, MoveCursorZeroIsNoop) {
  auto& inj = InputInjector::Get();
  // No assertion needed beyond "does not crash"; this is a
  // regression check for any future change that might re-introduce
  // work in the zero-delta path (e.g. accidentally re-scaling).
  inj.MoveCursor(0, 0);
  SUCCEED();
}