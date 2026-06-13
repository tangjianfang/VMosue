#pragma once
// Task 30: Interactive 6-step tutorial window for first-time users.
//
// v0.2 (Task 22) shipped a minimal stub that used MessageBoxW. Task 30
// replaces that with a real Win32 modeless window so the user can step
// through the tutorial at their own pace (Back / Next / Skip buttons
// plus the system Close button) without dismissing a stack of modal
// dialogs.
//
// The window is 600x500, fixed size, no resize. Each step renders:
//   - a title row ("Step N/6: <name>")
//   - a body paragraph
//   - an ASCII-art text diagram (no SVG/PNG assets in v0.2)
//   - a row of three buttons: Back | Skip | Next
//
// State: the current step (0..5) is preserved across show/hide. Re-
// opening the window returns to the same step the user was on.
//
// Lifetime: App owns a unique_ptr<TutorialWindow> and calls
// Init(parent) once during Run(). Show() and Hide() are driven by:
//   - the tray menu's "Tutorial" item (on-demand)
//   - the launch-time check on AppConfig::showTutorialOnLaunch
//     (auto-show after a 3s delay so the main UI has time to appear)
// Shutdown() is called from App::Shutdown() before the parent message
// window is destroyed.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace vmosue {

class TutorialWindow {
 public:
  TutorialWindow() = default;
  ~TutorialWindow();

  TutorialWindow(const TutorialWindow&) = delete;
  TutorialWindow& operator=(const TutorialWindow&) = delete;

  // Register the window class and create the modeless top-level
  // window as a child of `parent` (typically the tray message
  // window). Returns true on success. Idempotent: a second call
  // returns the existing HWND's success. The window is created in
  // the hidden state; call Show() to display it.
  bool Init(HWND parent = nullptr);

  // Show the window and bring it to the foreground. Idempotent.
  // The current step is preserved across hide/show cycles; the
  // window displays whatever step the user was on when they
  // closed it.
  void Show();

  // Hide the window. Idempotent. State (currentStep_) is preserved
  // so re-opening shows the same step.
  void Hide();

  // Tear down any resources. Safe to call multiple times.
  void Shutdown();

  // Number of steps in the tutorial. Exposed for tests / callers
  // that want to drive the window externally. Internal step state
  // is private; use Show()/Hide() for the normal flow.
  static constexpr int kStepCount = 6;

 private:
  // Static WndProc that forwards to the instance via GWLP_USERDATA.
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

  // Build all child controls. Called once from WM_CREATE.
  void CreateControls();

  // Re-render title + body + diagram into the static controls to
  // reflect the current step_. Called after every step change.
  void RenderStep();

  // Forward to RenderStep() so the static WndProc can call it
  // without an instance pointer in scope.
  static void RenderStepStatic(HWND hwnd);

  // Set currentStep_ and update the controls. Clamps to [0, 5].
  // Returns true if the step actually changed.
  bool SetStep(int newStep);

  // WM_COMMAND handler. Routes Back / Next / Skip button clicks.
  void OnCommand(UINT id, UINT code);

  HWND hwnd_ = nullptr;
  HWND hwndTitle_ = nullptr;
  HWND hwndBody_  = nullptr;
  HWND hwndDiagram_ = nullptr;
  HWND hwndBack_  = nullptr;
  HWND hwndNext_  = nullptr;
  HWND hwndSkip_  = nullptr;

  // 0..5. Survives Hide()/Show() cycles. After "Skip" or "Finish"
  // we leave the value where the user stopped, so re-opening shows
  // the same step (or they can navigate with Back/Next).
  int currentStep_ = 0;
};

}  // namespace vmosue
