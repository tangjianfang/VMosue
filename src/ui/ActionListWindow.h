#pragma once
// v0.6: ActionListWindow — a read-only reference of every gesture
// the user can perform and the action it triggers. The user
// complaint was "我随便一动，它就瞎乱点" ("I wave my hand and it
// randomly clicks") — half of the problem was the system firing
// on a one-frame gesture, and the other half was the user not
// knowing which gestures are even mapped. The ActionListWindow
// closes the second half by giving the user an always-available
// cheat sheet.
//
// The window is a fixed-size 480x520 popup with a title, a
// scrollable body (one row per ActionRef in kActionList), and a
// Close button. WM_NCCREATE is used to cache `this` in
// GWLP_USERDATA so the static WndProc can find the instance —
// same pattern as TutorialWindow / DebugWindow / SettingsWindow.
//
// Lifetime: App owns a unique_ptr<ActionListWindow> and calls
// Init(parent) once in Run(). Show()/Hide()/Toggle() are driven
// by the F1 hotkey and the "Action list" tray menu item.
// Shutdown() is called from App::Shutdown() before the parent
// message window is destroyed.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <string>

namespace vmosue {

class ActionListWindow {
 public:
  ActionListWindow() = default;
  ~ActionListWindow();

  ActionListWindow(const ActionListWindow&) = delete;
  ActionListWindow& operator=(const ActionListWindow&) = delete;

  // Register the window class and create the modeless top-level
  // window as a child of `parent` (typically the tray message
  // window). Returns true on success. The window is created in
  // the hidden state; call Show() to display it.
  bool Init(HWND parent = nullptr);

  // Show the window and bring it to the foreground. Idempotent.
  void Show();

  // Hide the window. Idempotent.
  void Hide();

  // Toggle visibility: Show() if hidden, Hide() if visible.
  void Toggle();

  // Accessor for the underlying HWND. nullptr until Init succeeds
  // (or after Shutdown).
  HWND GetHwnd() const { return hwnd_; }

  // Tear down any resources. Safe to call multiple times.
  void Shutdown();

 private:
  // Static WndProc that forwards to the instance via GWLP_USERDATA.
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

  // Build all child controls. Called once from WM_CREATE.
  void CreateControls();

  // Re-render the body text into the static control so it picks
  // up the current i18n bundle (called once after Init; cheap
  // enough to re-call on WM_SETTINGCHANGE if we want hot reload).
  void RenderBody();

  HWND hwnd_ = nullptr;
  HWND hwndBody_  = nullptr;
  HWND hwndClose_ = nullptr;
};

}  // namespace vmosue
