#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace vmosue {

// Minimal step indicator for the calibration flow and the on-boarding
// tutorial. v0.2 implementation: each ShowStep pops a modeless
// MessageBox so the user can read instructions at their own pace.
// A real Direct2D overlay with progress dots is tracked separately
// (Task 30: Tutorial window).
//
// Lifetime: Init/Shutdown are reference-counted no-ops in this stub,
// matching the Direct2D overlay's shape so call sites can be written
// once and switched later without churn.
class TutorialWindow {
 public:
  TutorialWindow() = default;
  ~TutorialWindow();

  TutorialWindow(const TutorialWindow&) = delete;
  TutorialWindow& operator=(const TutorialWindow&) = delete;

  // Show the indicator as a child of `parent` (or top-level if null).
  // Returns true on success; in this stub, always true.
  bool Init(HWND parent = nullptr);

  // Display a step instruction. `current` is 1-based, `total` is the
  // number of steps. Safe to call before Init() (no-op).
  void ShowStep(int current, int total, const std::wstring& message);

  // Tear down any resources. Safe to call multiple times.
  void Shutdown();

 private:
  HWND parent_ = nullptr;
  bool initialized_ = false;
};

}  // namespace vmosue
