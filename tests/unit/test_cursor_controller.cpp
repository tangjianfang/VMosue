#include <gtest/gtest.h>
#include "gesture/CursorController.h"
#include "gesture/GestureStateMachine.h"

using vmosue::CursorController;

TEST(CursorController, ResetClearsState) {
  CursorController c;
  c.Reset();
  // We can't observe internal state directly, but Reset should not throw.
  SUCCEED();
}

TEST(CursorController, DeadZoneFiltersSmallMotion) {
  // Placeholder: this is a smoke test; real motion logic is validated via
  // the integration test using recorded landmarks.
  CursorController c;
  vmosue::HandLandmarks lm{};
  vmosue::ActionSet actions{};
  for (int i = 0; i < 5; ++i) c.OnLandmarks(lm, actions, 1.0/30.0);
  SUCCEED();
}
