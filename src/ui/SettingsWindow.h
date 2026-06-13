#pragma once
// Task 27: Settings dialog with camera, sensitivity, perf mode,
// calibration, and auto-start controls.
//
// v0.2 implementation: a modeless top-level Win32 window built at
// runtime with CreateWindowEx (no .rc resource file). Children are
// created in WM_CREATE: a combobox for cameras, a combobox for perf
// mode, two trackbar sliders for sensitivity and air-click
// sensitivity, a checkbox for auto-start, and a button to launch the
// calibration flow.
//
// Save semantics: on WM_CLOSE the current control values are read
// into vmosue::Config::Get().Mutable() and persisted with
// Config::Get().Save(). The air-click sensitivity is also written
// into Calibration::Save(activeProfile, params) so the gesture
// detector picks it up on the next frame.
//
// Threading: all public methods must be called from the main (UI)
// thread that owns the parent HWND; the same constraint that
// OverlayWindow and TrayIcon carry.
#include <windows.h>

namespace vmosue {

class SettingsWindow {
 public:
  SettingsWindow() = default;
  ~SettingsWindow();

  SettingsWindow(const SettingsWindow&) = delete;
  SettingsWindow& operator=(const SettingsWindow&) = delete;

  // Register the window class and create the modeless window as a
  // child of `hwndParent` (typically a hidden owner). Returns the
  // resulting HWND, or nullptr on failure. The window is created in
  // the hidden state; call Show() to display it.
  HWND Create(HWND hwndParent);

  // Show the window and bring it to the foreground. Populates the
  // camera dropdown from the current Config on entry. Idempotent.
  void Show();

  // Hide the window. Idempotent. Does NOT save: see WM_CLOSE.
  void Hide();

  // Read the current control values into Config and persist to disk.
  // Exposed (private-style as a free function in the .cpp) for tests.
  // Public here so App can trigger an explicit save before shutdown
  // if it ever needs to.
  void SaveToConfig();

 private:
  // Static WndProc that forwards to the instance via GWLP_USERDATA.
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

  // Build all child controls. Called once from WM_CREATE.
  void CreateControls();

  // Populate the camera dropdown. v0.2 falls back to a single
  // "Default Camera" entry; Task 28 will replace this with the real
  // CameraCapture::EnumerateDevices() output.
  void PopulateCameras();

  // Pull the current control values into the form, ready to display.
  void LoadFromConfig();

  HWND hwnd_ = nullptr;
  HWND hwndCameraCombo_ = nullptr;
  HWND hwndPerfCombo_   = nullptr;
  HWND hwndSensTrack_   = nullptr;
  HWND hwndAirSensTrack_= nullptr;
  HWND hwndAutoStartChk_= nullptr;
  HWND hwndCalibBtn_    = nullptr;
  HWND hwndSensLabel_   = nullptr;
  HWND hwndAirSensLabel_= nullptr;
};

}  // namespace vmosue
