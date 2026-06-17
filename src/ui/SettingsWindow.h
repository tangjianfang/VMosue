#pragma once
// v0.5 (Wave 4): Settings dialog. The v0.4 sensitivity and
// air-click-threshold sliders are gone — every tunable is now a
// deterministic function of observable signals (see util/Adaptive.h),
// so there is no user preference to expose. In their place we render
// a live readout of the adaptive values the controller is currently
// using, refreshed 4x/sec via WM_TIMER. The user can see that the
// system is alive and adapting without being able to push it off
// course.
//
// Children: camera dropdown, performance-mode dropdown, a header
// strip ("Adaptive (auto) — values derive from your hand and
// environment"), 8 STATIC labels that show the current adaptive
// values, the auto-start checkbox, and the calibration button.
//
// Save semantics: on WM_CLOSE we persist what the user can change
// (camera index, performance mode, auto-start). The adaptive values
// are not persisted because they are recomputed every frame from
// observable signals.
//
// Threading: all public methods must be called from the main (UI)
// thread that owns the parent HWND.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <array>
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

  // Populate the camera dropdown from the current Config on entry.
  void PopulateCameras();

  // Pull the current control values into the form, ready to display.
  void LoadFromConfig();

  // Refresh the 8 live-readout STATIC labels from the adaptive
  // controller. Called on WM_TIMER (4 Hz) and on Show() so the
  // user always sees a fresh value when they open the window.
  void UpdateReadouts();

  // Number of live-readout labels. Indexed by kReadout* constants
  // in the .cpp.
  static constexpr int kReadoutCount = 8;

  HWND hwnd_ = nullptr;
  HWND hwndCameraCombo_ = nullptr;
  HWND hwndPerfCombo_   = nullptr;
  HWND hwndAutoStartChk_= nullptr;
  HWND hwndCalibBtn_    = nullptr;
  HWND hwndReadoutHeader_ = nullptr;
  // v0.6: dwell-time slider + anti-interference dropdown +
  // "show action preview" checkbox. Sits below the auto-start
  // checkbox in a new "Calibration" section. These controls
  // are saved to Config (and applied to the live state machine
  // on next Init) like every other persisted setting.
  HWND hwndDwellTrack_   = nullptr;
  HWND hwndDwellReadout_ = nullptr;
  HWND hwndAiCombo_      = nullptr;
  HWND hwndPreviewChk_   = nullptr;
  std::array<HWND, kReadoutCount> hwndReadouts_{};
};

}  // namespace vmosue
