#include "ui/SettingsWindow.h"

#include "capture/CameraCapture.h"
#include "config/Calibration.h"
#include "config/Config.h"
#include "platform/AutoStart.h"
#include "util/Adaptive.h"
#include "util/I18n.h"
#include "util/Logger.h"

#include <commctrl.h>
#include <string>
#include <vector>

namespace vmosue {

namespace {

// Window class name chosen to avoid colliding with TrayIcon's
// "VMosueTrayMessageWindow", the OverlayWindow's "VMosueOverlay", or
// the TutorialWindow's "VMosueTutorial". App owns at most one
// SettingsWindow so a single global pointer is enough to route
// WndProc callbacks back to the instance.
static const wchar_t kClassName[] = L"VMosueSettingsWindow";
static SettingsWindow* g_settings = nullptr;

// Child-control IDs. Picked high enough to not collide with any
// real-Win32 resource IDs (those are typically small integers in
// SDK projects). LOWORD(wParam) on WM_COMMAND carries these.
static constexpr UINT_PTR kIdCameraCombo    = 0xA001;
static constexpr UINT_PTR kIdPerfCombo      = 0xA002;
static constexpr UINT_PTR kIdAutoStartChk   = 0xA005;
static constexpr UINT_PTR kIdCalibBtn       = 0xA006;
static constexpr UINT_PTR kIdReadoutTimer   = 0xA010;
// v0.6: dwell-time trackbar + readout, anti-interference combo,
// action-preview checkbox. IDs continue the existing A0xx range.
static constexpr UINT_PTR kIdDwellTrack     = 0xA020;
static constexpr UINT_PTR kIdAiCombo        = 0xA021;
static constexpr UINT_PTR kIdPreviewChk     = 0xA022;

// v0.5 (Wave 4): the live-readout section replaces the v0.4
// sliders. WM_TIMER fires at 4 Hz — fast enough that the user
// sees a fresh value when they nudge their hand, slow enough that
// the SetWindowTextW churn doesn't compete with the model loop.
static constexpr UINT kReadoutTimerMs = 250;

// Layout constants. The window is fixed-size (no WS_THICKFRAME /
// WS_MAXIMIZEBOX) so we can hard-code coordinates. Each row is
// label_left + spacer + control. The "Adaptive (auto)" header
// introduces a compact section below the performance dropdown
// where the readouts are stacked at kReadoutRowH with no extra
// gap, so a single column of 8 readouts takes about 200 px.
static constexpr int kWindowW     = 480;
// Window is tall enough to hold: camera (0), perf mode (1), header
// (~150), 8 readouts at 22 px each, autostart, calibrate, and the
// v0.6 calibration section (dwell track, anti-interference combo,
// preview checkbox) at ~140 more px. Picked 660 to keep all rows
// visible on a 1080p screen without scrolling.
static constexpr int kWindowH     = 660;
static constexpr int kRowStart    = 20;
static constexpr int kRowGap      = 56;
static constexpr int kLabelX      = 16;
static constexpr int kLabelW      = 180;
static constexpr int kControlX    = 200;
static constexpr int kControlW    = 260;
static constexpr int kControlH    = 24;
static constexpr int kHeaderY     = kRowStart + 2 * kRowGap;       // 132
static constexpr int kReadoutX    = kLabelX;
static constexpr int kReadoutY    = kHeaderY + 24;                 // 156
static constexpr int kReadoutRowH = 22;
// AutoStart sits below the readout section.
static constexpr int kAutoStartY  = kReadoutY + kReadoutRowH * 8;  // 332
static constexpr int kCalibY      = kAutoStartY + kRowGap;         // 388
// v0.6: calibration section. The section header sits one row
// below the calibrate button; the dwell track, anti-interference
// combo, and preview checkbox follow.
static constexpr int kCalibHeaderY = kCalibY + kRowGap;            // 444
static constexpr int kDwellTrackY  = kCalibHeaderY + 24;            // 468
static constexpr int kAiComboY     = kDwellTrackY + kRowGap;         // 524
static constexpr int kPreviewChkY  = kAiComboY + kRowGap;           // 580

// Anti-interference combo options. Order matches
// AppConfig::antiInterference. The visible label is a
// human-friendly string; the saved value is the canonical "off"
// / "low" / "medium" / "high" string.
static const wchar_t* const kAiStrings[] = {
    L"Off",
    L"Low",
    L"Medium",
    L"High",
};
static const char* const kAiValues[] = {
    "off",
    "low",
    "medium",
    "high",
};
static constexpr int kAiCount =
    sizeof(kAiStrings) / sizeof(kAiStrings[0]);

// v0.6: dwell-time trackbar range. The user-visible range is
// [0, 3000] ms in 100 ms steps; the saved value matches the
// trackbar position so the slider position and the saved
// number are 1:1.
static constexpr int kDwellMinMs   = 0;
static constexpr int kDwellMaxMs   = 3000;
static constexpr int kDwellStepMs  = 100;

// Performance-mode combo options. Order matches kPerfModeStrings
// below; selected index maps directly to AppConfig::performanceMode.
static const wchar_t* const kPerfModeStrings[] = {
    L"Battery",
    L"Balanced",
    L"Performance",
};
static constexpr int kPerfModeCount =
    sizeof(kPerfModeStrings) / sizeof(kPerfModeStrings[0]);

// Format helpers for each readout. Each Format* fn is a thin
// adapter from a single adaptive value to the label text the user
// sees. Putting them in one namespace-private block makes the
// kReadout* index ↔ formatter mapping easy to audit at a glance.
std::wstring FormatPinch(float v) {
  wchar_t b[64];
  swprintf_s(b, L"Click pinch:      %.3f", v);
  return b;
}
std::wstring FormatRelease(float v) {
  wchar_t b[64];
  swprintf_s(b, L"Click release:    %.3f", v);
  return b;
}
std::wstring FormatScrollEnter(float v) {
  wchar_t b[64];
  swprintf_s(b, L"Scroll enter:     %.3f", v);
  return b;
}
std::wstring FormatScrollExit(float v) {
  wchar_t b[64];
  swprintf_s(b, L"Scroll exit:      %.3f", v);
  return b;
}
std::wstring FormatScrollScale(float v) {
  wchar_t b[64];
  swprintf_s(b, L"Scroll scale:     %.0f px/u", v);
  return b;
}
std::wstring FormatDeadZone(float v) {
  wchar_t b[64];
  // Dead zone is a normalized fraction; multiplying by 100 gives
  // a percent which is more intuitive ("1.8% of frame") than
  // "0.018".
  swprintf_s(b, L"Cursor deadzone:  %.2f%%", v * 100.0f);
  return b;
}
std::wstring FormatZApproach(float v) {
  wchar_t b[64];
  swprintf_s(b, L"Z approach:       %.3f m", v);
  return b;
}
std::wstring FormatLandmarkFilter(double mincutoff, double beta) {
  wchar_t b[64];
  swprintf_s(b, L"OneEuro (min/β):  %.2f / %.3f", mincutoff, beta);
  return b;
}

}  // namespace

SettingsWindow::~SettingsWindow() {
  // Best-effort tear-down: kill the readout timer (if any), hide
  // the window, then destroy it. The destructor does not call
  // SaveToConfig (the user may be discarding changes). Callers
  // that want to save on close should do so via WM_CLOSE.
  if (hwnd_) {
    KillTimer(hwnd_, kIdReadoutTimer);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  if (g_settings == this) g_settings = nullptr;
}

HWND SettingsWindow::Create(HWND hwndParent) {
  if (hwnd_) return hwnd_;  // already created

  HINSTANCE hinst = GetModuleHandle(nullptr);

  // Register the window class once. Done here (not in App.cpp) so
  // the class name + WndProc live next to each other. Idempotent:
  // a second call returns ERROR_CLASS_ALREADY_EXISTS, which we
  // treat as success.
  WNDCLASSEX wc{};
  wc.cbSize        = sizeof(wc);
  wc.style         = 0;
  wc.lpfnWndProc   = &SettingsWindow::WndProc;
  wc.hInstance     = hinst;
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.lpszClassName = kClassName;
  if (!RegisterClassEx(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    VMOSUE_LOG_ERROR("SettingsWindow: RegisterClassEx failed");
    return nullptr;
  }

  g_settings = this;

  DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  // Pass `this` as the lpCreateParams so the WndProc can recover
  // the instance pointer from CREATESTRUCTW during WM_NCCREATE
  // (the only reliable place to capture it — WM_CREATE arrives
  // after the HWND exists but does not forward lpCreateParams).
  hwnd_ = CreateWindowEx(
      WS_EX_DLGMODALFRAME,  // gives a thinner border than a full
                            // resizable frame, matches dialog look
      kClassName,
      I18n::Get().TW("settings.title").c_str(),
      style,
      CW_USEDEFAULT, CW_USEDEFAULT,
      kWindowW, kWindowH,
      hwndParent, nullptr, hinst, this);
  if (!hwnd_) {
    VMOSUE_LOG_ERROR("SettingsWindow: CreateWindowEx failed");
    g_settings = nullptr;
    return nullptr;
  }
  return hwnd_;
}

void SettingsWindow::CreateControls() {
  if (!hwnd_) return;
  HINSTANCE hinst = GetModuleHandle(nullptr);

  // Row 0: Camera dropdown
  CreateWindowEx(0, L"STATIC", L"Camera:",
                 WS_CHILD | WS_VISIBLE,
                 kLabelX, kRowStart + 0 * kRowGap,
                 kLabelW, kControlH,
                 hwnd_, nullptr, hinst, nullptr);
  hwndCameraCombo_ = CreateWindowEx(0, WC_COMBOBOX, nullptr,
                                    WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST
                                        | WS_VSCROLL,
                                    kControlX, kRowStart + 0 * kRowGap,
                                    kControlW, 200,  // height for dropdown
                                    hwnd_, reinterpret_cast<HMENU>(kIdCameraCombo),
                                    hinst, nullptr);

  // Row 1: Performance mode dropdown
  CreateWindowEx(0, L"STATIC", L"Performance mode:",
                 WS_CHILD | WS_VISIBLE,
                 kLabelX, kRowStart + 1 * kRowGap,
                 kLabelW, kControlH,
                 hwnd_, nullptr, hinst, nullptr);
  hwndPerfCombo_ = CreateWindowEx(0, WC_COMBOBOX, nullptr,
                                  WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST
                                      | WS_VSCROLL,
                                  kControlX, kRowStart + 1 * kRowGap,
                                  kControlW, 200,
                                  hwnd_, reinterpret_cast<HMENU>(kIdPerfCombo),
                                  hinst, nullptr);
  for (int i = 0; i < kPerfModeCount; ++i) {
    SendMessageW(hwndPerfCombo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(kPerfModeStrings[i]));
  }

  // v0.5 (Wave 4): the "Adaptive (auto)" header strip introduces
  // the live-readout section. The subtitle hints at the philosophy:
  // these values are derived from observation, not configuration.
  hwndReadoutHeader_ = CreateWindowEx(
      0, L"STATIC",
      L"Adaptive (auto) — values derive from your hand and environment",
      WS_CHILD | WS_VISIBLE | SS_LEFT,
      kReadoutX, kHeaderY, kWindowW - 2 * kReadoutX, kControlH,
      hwnd_, nullptr, hinst, nullptr);

  // 8 STATIC labels, one per adaptive value we want to surface.
  // The order is fixed; UpdateReadouts() iterates in the same
  // order to refresh. Each label gets a 22-px row, no extra gap.
  for (int i = 0; i < kReadoutCount; ++i) {
    hwndReadouts_[i] = CreateWindowEx(
        0, L"STATIC", L"—",  // em-dash placeholder until first tick
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        kReadoutX, kReadoutY + i * kReadoutRowH,
        kWindowW - 2 * kReadoutX, kReadoutRowH,
        hwnd_, nullptr, hinst, nullptr);
  }

  // AutoStart checkbox (sits below the readout section).
  hwndAutoStartChk_ = CreateWindowEx(0, WC_BUTTON, L"Start VMosue with Windows",
                                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                     kLabelX, kAutoStartY,
                                     kControlW + kLabelW, kControlH,
                                     hwnd_, reinterpret_cast<HMENU>(kIdAutoStartChk),
                                     hinst, nullptr);

  // Run calibration button. Calibration::RunInteractive() is a
  // stub (see Calibration::RunInteractive()); clicking the button
  // just shows a message so the user knows the wiring exists but
  // the flow is not implemented yet.
  hwndCalibBtn_ = CreateWindowEx(0, WC_BUTTON, L"Run calibration...",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 kLabelX, kCalibY,
                                 kControlW, kControlH + 6,
                                 hwnd_, reinterpret_cast<HMENU>(kIdCalibBtn),
                                 hinst, nullptr);

  // v0.6: calibration section header. Same header style as the
  // v0.5 adaptive section: bold yellow text, single line.
  CreateWindowEx(0, WC_STATIC,
                 L"Calibration",
                 WS_CHILD | WS_VISIBLE | SS_LEFT,
                 kLabelX, kCalibHeaderY,
                 kControlW + kLabelW, kControlH,
                 hwnd_, nullptr, hinst, nullptr);

  // v0.6: dwell-time trackbar. Range [0, 3000] ms, step 100.
  // TBM_SETPOS is sent from LoadFromConfig once the HWND exists.
  // We also build a static "ms" readout that refreshes on
  // WM_HSCROLL so the user gets precise feedback while dragging.
  hwndDwellTrack_ = CreateWindowEx(0, TRACKBAR_CLASSW, L"",
                                   WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                                   kLabelX, kDwellTrackY,
                                   kControlW + kLabelW, kControlH + 8,
                                   hwnd_, reinterpret_cast<HMENU>(kIdDwellTrack),
                                   hinst, nullptr);
  SendMessageW(hwndDwellTrack_, TBM_SETRANGE, TRUE,
               MAKELPARAM(kDwellMinMs, kDwellMaxMs));
  SendMessageW(hwndDwellTrack_, TBM_SETPAGESIZE, 0, kDwellStepMs);

  hwndDwellReadout_ = CreateWindowEx(0, WC_STATIC, L"1500 ms",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     kLabelX, kDwellTrackY + kControlH + 4,
                                     kControlW + kLabelW, kControlH,
                                     hwnd_, nullptr, hinst, nullptr);

  // v0.6: anti-interference combo. Order matches kAiStrings /
  // kAiValues above. The visible label is human-readable
  // (Off/Low/Medium/High); the saved value is the canonical
  // string ("off" / "low" / etc.).
  hwndAiCombo_ = CreateWindowEx(0, WC_COMBOBOX, L"",
                                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                kControlX, kAiComboY,
                                kControlW, kControlH + 60,
                                hwnd_, reinterpret_cast<HMENU>(kIdAiCombo),
                                hinst, nullptr);
  for (int i = 0; i < kAiCount; ++i) {
    SendMessageW(hwndAiCombo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(kAiStrings[i]));
  }

  // v0.6: show-action-preview checkbox. On by default. The
  // user can disable it if the on-screen countdown is
  // distracting in their workflow (e.g. screen recording).
  hwndPreviewChk_ = CreateWindowEx(0, WC_BUTTON,
                                   L"Show action preview on screen",
                                   WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                   kLabelX, kPreviewChkY,
                                   kControlW + kLabelW, kControlH,
                                   hwnd_, reinterpret_cast<HMENU>(kIdPreviewChk),
                                   hinst, nullptr);
}

void SettingsWindow::PopulateCameras() {
  if (!hwndCameraCombo_) return;
  // Clear any previous entries (Show() may be called multiple times).
  SendMessageW(hwndCameraCombo_, CB_RESETCONTENT, 0, 0);

  // Query Media Foundation for the actual list of video capture
  // devices. EnumerateDevices() is static and may safely be
  // called from the UI thread before the App has spun up its
  // capture worker. If it returns an empty list (e.g. running in a
  // session with no camera permissions, or the stub fallback path
  // on a CI runner), we add a single "No cameras detected" entry
  // so the user can see the dropdown failed rather than a blank
  // one. The save path below clamps to index 0 in that case.
  std::vector<std::wstring> names = CameraCapture::EnumerateDevices();
  if (names.empty()) {
    SendMessageW(hwndCameraCombo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"No cameras detected"));
  } else {
    for (const auto& n : names) {
      SendMessageW(hwndCameraCombo_, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(n.c_str()));
    }
  }

  // Select the current cameraIndex if it falls inside the
  // list range; otherwise fall back to 0. We allow empty
  // enumeration to still pick index 0 (the placeholder) so the
  // user sees something selected rather than no selection.
  const auto& cfg = Config::Get().Data();
  int sel = cfg.cameraIndex;
  if (sel < 0) sel = 0;
  const int count = static_cast<int>(SendMessageW(hwndCameraCombo_,
                                                   CB_GETCOUNT, 0, 0));
  if (sel >= count) sel = 0;
  SendMessageW(hwndCameraCombo_, CB_SETCURSEL, sel, 0);
}

void SettingsWindow::LoadFromConfig() {
  const auto& cfg = Config::Get().Data();

  // Performance mode: pick the combo index that matches the stored
  // string. Unknown values fall back to "Balanced" (index 1).
  int perfIdx = 1;  // default Balanced
  for (int i = 0; i < kPerfModeCount; ++i) {
    // Compare the std::string against each wchar_t* option. We
    // convert the wchar_t* to a narrow string for the comparison so
    // the code doesn't depend on a non-standard operator== overload.
    // Use a size-limited construction to avoid the C4244 warning on
    // the wide-to-narrow conversion (ASCII literals only, so size
    // is preserved).
    const wchar_t* w = kPerfModeStrings[i];
    size_t n = 0;
    while (w[n]) ++n;
    std::string narrow;
    narrow.reserve(n);
    for (size_t k = 0; k < n; ++k) narrow.push_back(static_cast<char>(w[k]));
    if (cfg.performanceMode == narrow) { perfIdx = i; break; }
  }
  if (hwndPerfCombo_) {
    SendMessageW(hwndPerfCombo_, CB_SETCURSEL, perfIdx, 0);
  }

  // v0.5: readouts start with a fresh adaptive snapshot so the
  // user sees real values, not em-dashes, the first time they
  // open the window (before the first WM_TIMER tick).
  UpdateReadouts();

  // AutoStart checkbox reflects the live registry state. The
  // registry is the source of truth for "is autostart on right
  // now?" because the user may have toggled it from outside the
  // app (Task Manager -> Startup, regedit, etc.). The config's
  // autoStart flag is updated by WM_COMMAND when the user clicks
  // the checkbox so it survives a config-save round trip.
  if (hwndAutoStartChk_) {
    SendMessageW(hwndAutoStartChk_, BM_SETCHECK,
                 AutoStart::IsEnabled() ? BST_CHECKED
                                        : BST_UNCHECKED, 0);
  }

  // v0.6: dwell-time trackbar. The saved value is in ms in
  // [0, 5000]; the trackbar's max is 3000, so values above the
  // visible range are clamped for the slider but the underlying
  // value is preserved (Config::Data() returns the full 0-5000).
  if (hwndDwellTrack_) {
    int pos = cfg.dwellTimeMs;
    if (pos < kDwellMinMs) pos = kDwellMinMs;
    if (pos > kDwellMaxMs) pos = kDwellMaxMs;
    SendMessageW(hwndDwellTrack_, TBM_SETPOS, TRUE, pos);
  }
  if (hwndDwellReadout_) {
    wchar_t buf[32];
    swprintf_s(buf, L"%d ms", cfg.dwellTimeMs);
    SetWindowTextW(hwndDwellReadout_, buf);
  }

  // v0.6: anti-interference combo. Map the saved string to the
  // combo index; unknown values fall back to "Medium" (index 2).
  if (hwndAiCombo_) {
    int aiIdx = 2;  // default "medium"
    for (int i = 0; i < kAiCount; ++i) {
      if (cfg.antiInterference == kAiValues[i]) { aiIdx = i; break; }
    }
    SendMessageW(hwndAiCombo_, CB_SETCURSEL, aiIdx, 0);
  }

  // v0.6: show-action-preview checkbox.
  if (hwndPreviewChk_) {
    SendMessageW(hwndPreviewChk_, BM_SETCHECK,
                 cfg.showActionPreview ? BST_CHECKED : BST_UNCHECKED, 0);
  }
}

void SettingsWindow::UpdateReadouts() {
  if (!hwnd_) return;
  const auto& a = GetAdaptive();
  // Read each value once, format, push. The order MUST match the
  // hwndReadouts_[i] creation order in CreateControls() — index i
  // in the array corresponds to index i in this assignment.
  // Mismatch here would scramble the labels.
  const auto click = std::make_pair(a.PinchThreshold(), a.ReleaseThreshold());
  const auto scroll = std::make_pair(a.ScrollEnterThreshold(),
                                     a.ScrollExitThreshold());
  const auto oneEuro = a.LandmarkFilterParams();

  auto set = [&](int i, const std::wstring& s) {
    if (hwndReadouts_[i]) {
      SetWindowTextW(hwndReadouts_[i], s.c_str());
    }
  };
  set(0, FormatPinch(click.first));
  set(1, FormatRelease(click.second));
  set(2, FormatScrollEnter(scroll.first));
  set(3, FormatScrollExit(scroll.second));
  set(4, FormatScrollScale(a.ScrollScaleFactor()));
  set(5, FormatDeadZone(a.CursorDeadZone()));
  set(6, FormatZApproach(a.ZApproachThreshold()));
  set(7, FormatLandmarkFilter(oneEuro.first, oneEuro.second));
}

void SettingsWindow::Show() {
  if (!hwnd_) return;
  // Repopulate the camera dropdown and reload values from Config
  // every time the user opens the window, so external edits (e.g.
  // the calibration flow writing new params) are picked up.
  PopulateCameras();
  LoadFromConfig();
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);
}

void SettingsWindow::Hide() {
  if (!hwnd_) return;
  ShowWindow(hwnd_, SW_HIDE);
}

void SettingsWindow::SaveToConfig() {
  if (!hwnd_) return;

  auto& cfg = Config::Get().Mutable();

  // Camera selection. v0.2 has a single entry; we still write the
  // index into the config so Task 28 can read it back.
  if (hwndCameraCombo_) {
    int sel = static_cast<int>(SendMessageW(hwndCameraCombo_,
                                             CB_GETCURSEL, 0, 0));
    if (sel < 0) sel = 0;
    cfg.cameraIndex = sel;
  }

  // Performance mode.
  if (hwndPerfCombo_) {
    int sel = static_cast<int>(SendMessageW(hwndPerfCombo_,
                                             CB_GETCURSEL, 0, 0));
    if (sel < 0) sel = 1;  // Balanced default
    if (sel >= kPerfModeCount) sel = 1;
    // Convert the wide literal to a narrow std::string (AppConfig
    // uses std::string for portability with the JSON serializer).
    // Manual char-by-char conversion to avoid the C4244 warning on
    // the wide-to-narrow element cast (ASCII literals only).
    const wchar_t* w = kPerfModeStrings[sel];
    cfg.performanceMode.clear();
    for (const wchar_t* p = w; *p; ++p) {
      cfg.performanceMode.push_back(static_cast<char>(*p));
    }
  }

  // v0.5: the cursor sensitivity and air-click threshold are
  // gone — neither is user-configurable anymore. Nothing to do
  // for them.

  // AutoStart checkbox. The WM_COMMAND handler already updated
  // cfg.autoStart and the registry on toggle; we still re-read the
  // checkbox here so a future save path that bypasses the toggle
  // (e.g. a "Restore defaults" button) writes a consistent value.
  if (hwndAutoStartChk_) {
    LRESULT state = SendMessageW(hwndAutoStartChk_, BM_GETCHECK, 0, 0);
    cfg.autoStart = (state == BST_CHECKED);
  }

  // v0.6: dwell-time trackbar. Read the slider position back
  // into the config. The trackbar's range is [0, 3000]; we
  // store the same ms value the user sees on the readout.
  if (hwndDwellTrack_) {
    int pos = static_cast<int>(SendMessageW(hwndDwellTrack_,
                                             TBM_GETPOS, 0, 0));
    if (pos < 0) pos = 0;
    if (pos > 5000) pos = 5000;  // allow the rare 3000-5000
                                // range to round-trip, even
    // though the slider can't reach it
    cfg.dwellTimeMs = pos;
  }

  // v0.6: anti-interference combo. Map the index back to the
  // canonical "off" / "low" / "medium" / "high" string.
  if (hwndAiCombo_) {
    int sel = static_cast<int>(SendMessageW(hwndAiCombo_,
                                             CB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= kAiCount) sel = 2;  // medium default
    cfg.antiInterference = kAiValues[sel];
  }

  // v0.6: show-action-preview checkbox.
  if (hwndPreviewChk_) {
    LRESULT state = SendMessageW(hwndPreviewChk_, BM_GETCHECK, 0, 0);
    cfg.showActionPreview = (state == BST_CHECKED);
  }

  auto res = Config::Get().Save();
  if (!res.isOk()) {
    VMOSUE_LOG_WARN("SettingsWindow: Config::Save failed: {}", res.error());
  }
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg,
                                         WPARAM w, LPARAM l) {
  // Locate the instance via the per-window user data. Set in
  // WM_NCCREATE so we don't depend on a global being current
  // (defensive: another SettingsWindow could theoretically be
  // created, though the App's unique_ptr makes that impossible
  // in practice).
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                     reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    // Same race as TutorialWindow: WM_CREATE fires during
    // CreateWindowEx, before Init()'s `hwnd_ = CreateWindowEx(...)`
    // assignment runs. CreateControls() bails on a null hwnd_,
    // leaving the window blank. Cache the HWND here so it's
    // visible to the WM_CREATE handler that follows.
    SettingsWindow* ncSelf = reinterpret_cast<SettingsWindow*>(
        cs->lpCreateParams);
    if (ncSelf) ncSelf->hwnd_ = hwnd;
    return 1;
  }
  SettingsWindow* self = reinterpret_cast<SettingsWindow*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self) return DefWindowProc(hwnd, msg, w, l);

  switch (msg) {
    case WM_CREATE:
      // Build all child controls. We cannot use lpCreateParams here
      // because CreateWindowEx's last arg (the lpCreateParams we
      // passed) was not forwarded as the lParam of WM_CREATE in
      // every SDK; the WM_NCCREATE branch above is the portable
      // way to capture it. Once we have `self`, create children.
      self->CreateControls();
      self->PopulateCameras();
      self->LoadFromConfig();
      // Start the 4 Hz refresh timer so the adaptive readouts
      // update as the user moves their hand. The timer keeps
      // running while the window is visible; we stop it on hide
      // (see WM_TIMER + WM_CLOSE below) so a hidden window
      // doesn't keep churning SetWindowTextW.
      SetTimer(hwnd, kIdReadoutTimer, kReadoutTimerMs, nullptr);
      return 0;

    case WM_TIMER:
      // Refresh readouts. We don't guard on visibility because
      // the timer is killed on WM_CLOSE; if a future change moves
      // the timer to a permanent owner, add a visibility check
      // here so a hidden window doesn't waste CPU.
      if (w == kIdReadoutTimer) {
        self->UpdateReadouts();
        return 0;
      }
      break;

    case WM_COMMAND: {
      UINT id = LOWORD(w);
      UINT code = HIWORD(w);
      if (id == kIdCalibBtn && code == BN_CLICKED) {
        // Calibration::RunInteractive() returns Err (stub). Show
        // a message so the user knows the wiring is in place but
        // the flow is not implemented yet. When the real
        // calibration lands, it will only need to set the four
        // scale/offset fields in CalibrationParams — the
        // pinch/release/scroll/z thresholds are now adaptive.
        Calibration calib;
        auto r = calib.RunInteractive();
        if (!r.isOk()) {
          MessageBoxW(hwnd,
                      L"Calibration flow not yet implemented (stub). "
                      L"v0.5 derives thresholds from observation; the "
                      L"calibration flow only sets cursor scale/offset.",
                      L"VMosue", MB_OK | MB_ICONINFORMATION);
        }
        return 0;
      }
      if (id == kIdAutoStartChk && code == BN_CLICKED) {
        // React to the toggle by writing to the registry. We read
        // the checkbox state rather than tracking a "previous"
        // value so the handler is reentrant if the message fires
        // twice for some reason (e.g. double-click bounce). On
        // Enable() failure we revert the checkbox and surface a
        // message; Disable() is idempotent so no error popup is
        // needed there.
        LRESULT state = SendMessageW(self->hwndAutoStartChk_,
                                     BM_GETCHECK, 0, 0);
        bool want = (state == BST_CHECKED);
        bool ok = want ? AutoStart::Enable() : AutoStart::Disable();
        if (!ok) {
          // Revert checkbox and Config so the UI agrees with the
          // OS (registry is the source of truth).
          SendMessageW(self->hwndAutoStartChk_, BM_SETCHECK,
                       want ? BST_UNCHECKED : BST_CHECKED, 0);
          Config::Get().Mutable().autoStart = !want;
          MessageBoxW(hwnd,
                      want ? L"Failed to enable auto-start. "
                            L"Check that you have permission to "
                            L"write to HKEY_CURRENT_USER."
                          : L"Failed to disable auto-start.",
                      L"VMosue", MB_OK | MB_ICONERROR);
          return 0;
        }
        Config::Get().Mutable().autoStart = want;
        return 0;
      }
      // Combo / checkbox edits are reflected in the live label and
      // persisted on close; nothing to do here for v0.5.
      return 0;
    }

    case WM_HSCROLL: {
      // v0.6: dwell-time trackbar. The slider is parented to
      // this window so we get the WM_HSCROLL here. Update the
      // "ms" readout so the user sees the precise value while
      // dragging. Persisted value is written on WM_CLOSE via
      // SaveToConfig; this is a live preview only.
      if (reinterpret_cast<HWND>(l) == self->hwndDwellTrack_ &&
          self->hwndDwellReadout_) {
        int pos = static_cast<int>(SendMessageW(self->hwndDwellTrack_,
                                                 TBM_GETPOS, 0, 0));
        wchar_t buf[32];
        swprintf_s(buf, L"%d ms", pos);
        SetWindowTextW(self->hwndDwellReadout_, buf);
      }
      return 0;
    }

    case WM_CLOSE:
      // Save then hide. We hide (not destroy) so reopening is
      // instant — CreateControls only runs once. Kill the readout
      // timer first so a hidden window doesn't keep churning.
      KillTimer(hwnd, kIdReadoutTimer);
      self->SaveToConfig();
      ShowWindow(hwnd, SW_HIDE);
      return 0;

    case WM_DESTROY:
      // The window is going away (e.g. WM_QUIT or App teardown).
      // We do not call Save here because WM_CLOSE already saved.
      // If WM_DESTROY arrives without WM_CLOSE (rare — e.g. task-
      // kill), the user's last edits are lost; that is acceptable
      // for v0.5 and matches the typical Windows contract that
      // WM_CLOSE is the save point.
      if (g_settings == self) g_settings = nullptr;
      self->hwnd_ = nullptr;
      return 0;

    default:
      return DefWindowProc(hwnd, msg, w, l);
  }
  return DefWindowProc(hwnd, msg, w, l);
}

}  // namespace vmosue
