#include "ui/SettingsWindow.h"

#include "capture/CameraCapture.h"
#include "config/Calibration.h"
#include "config/Config.h"
#include "platform/AutoStart.h"
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
static constexpr UINT_PTR kIdSensTrack      = 0xA003;
static constexpr UINT_PTR kIdAirSensTrack   = 0xA004;
static constexpr UINT_PTR kIdAutoStartChk   = 0xA005;
static constexpr UINT_PTR kIdCalibBtn       = 0xA006;

// Layout constants. The window is fixed-size (no WS_THICKFRAME /
// WS_MAXIMIZEBOX) so we can hard-code coordinates. Each row is
// label_left + spacer + control. Controls are vertically stacked
// from y=kRowStart with kRowGap between rows.
static constexpr int kWindowW     = 480;
static constexpr int kWindowH     = 420;
static constexpr int kRowStart    = 20;
static constexpr int kRowGap      = 56;
static constexpr int kLabelX      = 16;
static constexpr int kLabelW      = 180;
static constexpr int kControlX    = 200;
static constexpr int kControlW    = 260;
static constexpr int kControlH    = 24;

// Sensitivity slider range. 0..2500 maps to 0.5..3.0 with one
// decimal precision (step = 0.01 = 10 raw units, but we render
// only the integer tenth so 0.01 is sub-step). Default 1.0 is
// raw = (1.0 - 0.5) * 1000 = 500.
static constexpr int  kSensRawMin       = 0;
static constexpr int  kSensRawMax       = 2500;
static constexpr int  kSensDefaultRaw   = 500;
static constexpr float kSensRangeMin    = 0.5f;
static constexpr float kSensRangeMax    = 3.0f;

// Air-click sensitivity slider range. 0..190 maps to 0.005..0.10
// (step 0.005). Default 0.02 -> raw = (0.02 - 0.005) / 0.0005 = 30.
static constexpr int   kAirRawMin       = 0;
static constexpr int   kAirRawMax       = 190;
static constexpr int   kAirDefaultRaw   = 30;
static constexpr float kAirRangeMin     = 0.005f;
static constexpr float kAirRangeMax     = 0.10f;
static constexpr float kAirRangeStep    = 0.0005f;

// Performance-mode combo options. Order matches kPerfModeStrings
// below; selected index maps directly to AppConfig::performanceMode.
static const wchar_t* const kPerfModeStrings[] = {
    L"Battery",
    L"Balanced",
    L"Performance",
};
static constexpr int kPerfModeCount =
    sizeof(kPerfModeStrings) / sizeof(kPerfModeStrings[0]);

// Convert sensitivity slider position (raw integer) to a float in
// [kSensRangeMin, kSensRangeMax]. Linear mapping; no rounding
// tricks needed because we round to 0.01 at the call sites.
float SensRawToFloat(int raw) {
  float t = static_cast<float>(raw) /
            static_cast<float>(kSensRawMax - kSensRawMin);
  return kSensRangeMin + t * (kSensRangeMax - kSensRangeMin);
}
int SensFloatToRaw(float v) {
  float t = (v - kSensRangeMin) / (kSensRangeMax - kSensRangeMin);
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  return static_cast<int>(t * (kSensRawMax - kSensRawMin) + 0.5f);
}

// Air-click slider position <-> CalibrationParams::airClickZThreshold.
float AirRawToFloat(int raw) {
  return kAirRangeMin + static_cast<float>(raw) * kAirRangeStep;
}
int AirFloatToRaw(float v) {
  float t = (v - kAirRangeMin) / (kAirRangeMax - kAirRangeMin);
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  return static_cast<int>(t * (kAirRawMax - kAirRawMin) + 0.5f);
}

// Format "sensitivity: 1.20" / "air click: 0.020" style labels.
// Two decimals for the cursor sensitivity, three for the air
// threshold (which is naturally a small number).
std::wstring FormatSensLabel(float v) {
  wchar_t buf[64];
  swprintf_s(buf, L"Cursor sensitivity: %.2fx", v);
  return buf;
}
std::wstring FormatAirLabel(float v) {
  wchar_t buf[64];
  swprintf_s(buf, L"Air click sensitivity: %.3f", v);
  return buf;
}

// Copy at most dstsz-1 wide chars from src into dst, NUL-terminating.
void CopyWide(wchar_t* dst, size_t dstsz, const wchar_t* src) {
  if (!dst || dstsz == 0) return;
  if (!src) { dst[0] = 0; return; }
  size_t i = 0;
  for (; i + 1 < dstsz && src[i]; ++i) dst[i] = src[i];
  dst[i] = 0;
}

}  // namespace

SettingsWindow::~SettingsWindow() {
  // Best-effort tear-down: hide the window, then destroy it. The
  // destructor does not call SaveToConfig (the user may be
  // discarding changes). Callers that want to save on close should
  // do so via WM_CLOSE.
  if (hwnd_) {
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

  // Row 2: Sensitivity slider + live label
  hwndSensLabel_ = CreateWindowEx(0, L"STATIC", L"Cursor sensitivity: 1.00x",
                                  WS_CHILD | WS_VISIBLE | SS_LEFT,
                                  kLabelX, kRowStart + 2 * kRowGap,
                                  kLabelW, kControlH,
                                  hwnd_, nullptr, hinst, nullptr);
  hwndSensTrack_ = CreateWindowEx(0, TRACKBAR_CLASS, nullptr,
                                  WS_CHILD | WS_VISIBLE | TBS_HORZ
                                      | TBS_AUTOTICKS,
                                  kControlX, kRowStart + 2 * kRowGap - 4,
                                  kControlW, kControlH + 8,
                                  hwnd_, reinterpret_cast<HMENU>(kIdSensTrack),
                                  hinst, nullptr);
  SendMessageW(hwndSensTrack_, TBM_SETRANGE, TRUE,
               MAKELONG(kSensRawMin, kSensRawMax));
  SendMessageW(hwndSensTrack_, TBM_SETPOS, TRUE, kSensDefaultRaw);

  // Row 3: Air click sensitivity slider + live label
  hwndAirSensLabel_ = CreateWindowEx(0, L"STATIC",
                                     L"Air click sensitivity: 0.020",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     kLabelX, kRowStart + 3 * kRowGap,
                                     kLabelW, kControlH,
                                     hwnd_, nullptr, hinst, nullptr);
  hwndAirSensTrack_ = CreateWindowEx(0, TRACKBAR_CLASS, nullptr,
                                     WS_CHILD | WS_VISIBLE | TBS_HORZ
                                         | TBS_AUTOTICKS,
                                     kControlX, kRowStart + 3 * kRowGap - 4,
                                     kControlW, kControlH + 8,
                                     hwnd_, reinterpret_cast<HMENU>(kIdAirSensTrack),
                                     hinst, nullptr);
  SendMessageW(hwndAirSensTrack_, TBM_SETRANGE, TRUE,
               MAKELONG(kAirRawMin, kAirRawMax));
  SendMessageW(hwndAirSensTrack_, TBM_SETPOS, TRUE, kAirDefaultRaw);

  // Row 4: AutoStart checkbox (Task 32 wires the real Enable/Disable;
  // for v0.2 it is a UI-only checkbox whose state is read into
  // config.autoStart on close).
  hwndAutoStartChk_ = CreateWindowEx(0, WC_BUTTON, L"Start VMosue with Windows",
                                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                     kLabelX, kRowStart + 4 * kRowGap,
                                     kControlW + kLabelW, kControlH,
                                     hwnd_, reinterpret_cast<HMENU>(kIdAutoStartChk),
                                     hinst, nullptr);

  // Row 5: Run calibration button. Calibration::RunInteractive() is
  // a stub in v0.2 (see Task 22); clicking the button just shows a
  // message so the user knows the wiring exists but the flow is not
  // implemented yet.
  hwndCalibBtn_ = CreateWindowEx(0, WC_BUTTON, L"Run calibration...",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 kLabelX, kRowStart + 5 * kRowGap,
                                 kControlW, kControlH + 6,
                                 hwnd_, reinterpret_cast<HMENU>(kIdCalibBtn),
                                 hinst, nullptr);
}

void SettingsWindow::PopulateCameras() {
  if (!hwndCameraCombo_) return;
  // Clear any previous entries (Show() may be called multiple times).
  SendMessageW(hwndCameraCombo_, CB_RESETCONTENT, 0, 0);

  // Task 28: query Media Foundation for the actual list of video
  // capture devices. EnumerateDevices() is static and may safely be
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
  // enumeration to still pick index 0 (the placeholder) so
  // the user sees something selected rather than no selection.
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

  // Sliders: convert float to raw int.
  if (hwndSensTrack_) {
    SendMessageW(hwndSensTrack_, TBM_SETPOS, TRUE,
                 SensFloatToRaw(cfg.sensitivity));
    if (hwndSensLabel_) {
      SetWindowTextW(hwndSensLabel_, FormatSensLabel(cfg.sensitivity).c_str());
    }
  }
  if (hwndAirSensTrack_) {
    // We do not store airClickZThreshold in AppConfig; for v0.2 we
    // load from Calibration::Load(activeProfile) and fall back to
    // the default if the profile is missing or malformed.
    Calibration calib;
    CalibrationParams params;
    auto loaded = calib.Load(cfg.activeProfile);
    float airVal = loaded.isOk() ? loaded.value().airClickZThreshold
                                 : params.airClickZThreshold;
    SendMessageW(hwndAirSensTrack_, TBM_SETPOS, TRUE, AirFloatToRaw(airVal));
    if (hwndAirSensLabel_) {
      SetWindowTextW(hwndAirSensLabel_, FormatAirLabel(airVal).c_str());
    }
  }

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

  // Sliders.
  if (hwndSensTrack_) {
    int raw = static_cast<int>(SendMessageW(hwndSensTrack_,
                                             TBM_GETPOS, 0, 0));
    cfg.sensitivity = SensRawToFloat(raw);
  }

  // Air click sensitivity: write into a CalibrationParams, also
  // starting from whatever the active profile currently holds (so
  // we do not clobber pinchThreshold / scaleX / scaleY / etc).
  if (hwndAirSensTrack_) {
    int raw = static_cast<int>(SendMessageW(hwndAirSensTrack_,
                                             TBM_GETPOS, 0, 0));
    float airVal = AirRawToFloat(raw);
    Calibration calib;
    CalibrationParams params;
    auto loaded = calib.Load(cfg.activeProfile);
    if (loaded.isOk()) params = loaded.value();
    params.airClickZThreshold = airVal;
    auto saved = calib.Save(cfg.activeProfile, params);
    if (!saved.isOk()) {
      VMOSUE_LOG_WARN("SettingsWindow: Calibration::Save failed: {}",
                      saved.error());
    }
  }

  // AutoStart checkbox. The WM_COMMAND handler already updated
  // cfg.autoStart and the registry on toggle; we still re-read the
  // checkbox here so a future save path that bypasses the toggle
  // (e.g. a "Restore defaults" button) writes a consistent value.
  if (hwndAutoStartChk_) {
    LRESULT state = SendMessageW(hwndAutoStartChk_, BM_GETCHECK, 0, 0);
    cfg.autoStart = (state == BST_CHECKED);
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
      return 0;

    case WM_HSCROLL:
      // Slider thumb moved. Update the live label so the user
      // sees the new value as they drag. We do not save on every
      // tick — save happens on WM_CLOSE so the user can cancel by
      // closing without confirming.
      if (self->hwndSensLabel_ && self->hwndSensTrack_) {
        int raw = static_cast<int>(SendMessageW(self->hwndSensTrack_,
                                                 TBM_GETPOS, 0, 0));
        SetWindowTextW(self->hwndSensLabel_,
                       FormatSensLabel(SensRawToFloat(raw)).c_str());
      }
      if (self->hwndAirSensLabel_ && self->hwndAirSensTrack_) {
        int raw = static_cast<int>(SendMessageW(self->hwndAirSensTrack_,
                                                 TBM_GETPOS, 0, 0));
        SetWindowTextW(self->hwndAirSensLabel_,
                       FormatAirLabel(AirRawToFloat(raw)).c_str());
      }
      return 0;

    case WM_COMMAND: {
      UINT id = LOWORD(w);
      UINT code = HIWORD(w);
      if (id == kIdCalibBtn && code == BN_CLICKED) {
        // v0.2: Calibration::RunInteractive() returns Err (Task 22
        // stub). Show a message so the user knows the wiring is in
        // place but the flow is not implemented yet. When Task 22
        // lands, replace this with a real launch.
        Calibration calib;
        auto r = calib.RunInteractive();
        if (!r.isOk()) {
          MessageBoxW(hwnd,
                      L"Calibration flow not yet implemented (see Task 22 stub).",
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
      // persisted on close; nothing to do here for v0.2.
      return 0;
    }

    case WM_CLOSE:
      // Save then hide. We hide (not destroy) so reopening is
      // instant — CreateControls only runs once.
      self->SaveToConfig();
      ShowWindow(hwnd, SW_HIDE);
      return 0;

    case WM_DESTROY:
      // The window is going away (e.g. WM_QUIT or App teardown).
      // We do not call Save here because WM_CLOSE already saved.
      // If WM_DESTROY arrives without WM_CLOSE (rare — e.g. task-
      // kill), the user's last edits are lost; that is acceptable
      // for v0.2 and matches the typical Windows contract that
      // WM_CLOSE is the save point.
      if (g_settings == self) g_settings = nullptr;
      self->hwnd_ = nullptr;
      return 0;

    default:
      return DefWindowProc(hwnd, msg, w, l);
  }
}

}  // namespace vmosue
