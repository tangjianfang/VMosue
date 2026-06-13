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