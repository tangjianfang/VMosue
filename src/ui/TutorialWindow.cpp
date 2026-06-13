#include "ui/TutorialWindow.h"

#include "util/Logger.h"

#include <windows.h>

#include <string>

namespace vmosue {

namespace {

// Window class name chosen to avoid colliding with the TrayIcon's
// "VMosueTrayMessageWindow", the OverlayWindow's "VMosueOverlay",
// the SettingsWindow's "VMosueSettingsWindow", or the DebugWindow's
// "VMosueDebugWindow". The App owns at most one TutorialWindow so
// a single global pointer is enough to route WndProc callbacks
// back to the instance.
static const wchar_t kClassName[] = L"VMosueTutorialWindow";
static TutorialWindow* g_tutorial = nullptr;

// Child-control IDs. Picked high enough to not collide with the
// ones in SettingsWindow. LOWORD(wParam) on WM_COMMAND carries
// these.
static constexpr UINT_PTR kIdBack = 0xB001;
static constexpr UINT_PTR kIdNext = 0xB002;
static constexpr UINT_PTR kIdSkip = 0xB003;

// Layout constants. The window is fixed-size (no WS_THICKFRAME /
// WS_MAXIMIZEBOX) so we can hard-code coordinates. Vertical stack:
// title, body, diagram, button row.
static constexpr int kWindowW  = 600;
static constexpr int kWindowH  = 500;
static constexpr int kPadX     = 20;
static constexpr int kTitleY   = 16;
static constexpr int kTitleH   = 28;
static constexpr int kBodyY    = 56;
static constexpr int kBodyH    = 130;
static constexpr int kDiagramY = 200;
static constexpr int kDiagramH = 200;
static constexpr int kBtnY     = 420;
static constexpr int kBtnH     = 32;
static constexpr int kBtnW     = 90;
static constexpr int kBtnGap   = 12;

// ---- Step content tables ----
//
// Each step has a title, a body paragraph, and an optional ASCII
// diagram. The body and diagram are stored as wide-string literals
// so the parse-check and production paths both work without
// runtime allocation.
//
// We use raw L"..." strings and \n for newlines. DrawTextW with
// DT_WORDBREAK (for the body) and DT_NOCLIP (for the diagram)
// handles rendering; the diagram is set to a fixed-pitch-friendly
// area, but we do not actually change the font (Windows picks a
// default). The diagram is read-only so any font works in
// practice.

// Step 0: welcome + system requirements.
static const wchar_t* const kStep0Title =
    L"Step 1/6: Welcome";
static const wchar_t* const kStep0Body =
    L"Welcome to VMosue. This tutorial will guide you through setting "
    L"up and using gesture control.\n\n"
    L"System requirements:\n"
    L"  - Windows 10 or later (x64)\n"
    L"  - A webcam (built-in or USB)\n"
    L"  - About 200 MB free disk space\n"
    L"  - A well-lit room (your hand should be clearly visible to "
    L"the camera)\n\n"
    L"Click Next to continue, or Skip to dismiss.";
static const wchar_t* const kStep0Diagram =
    L"+-------------------+\n"
    L"|   VMosue v1.0     |\n"
    L"|   Gesture Mouse   |\n"
    L"|                   |\n"
    L"|   (o) webcam      |\n"
    L"|     |             |\n"
    L"|     v             |\n"
    L"|   cursor          |\n"
    L"+-------------------+";

// Step 1: camera positioning.
static const wchar_t* const kStep1Title =
    L"Step 2/6: Camera positioning";
static const wchar_t* const kStep1Body =
    L"Position your camera so your dominant hand is clearly visible "
    L"at arm's length. Top of camera at eye level works well.\n\n"
    L"Tips:\n"
    L"  - Sit at your normal distance from the screen\n"
    L"  - Make sure the camera can see your full hand + wrist\n"
    L"  - Avoid back-lighting (a window behind you makes the "
    L"camera see a silhouette of your hand)\n"
    L"  - The Settings window lets you change the camera later.";
static const wchar_t* const kStep1Diagram =
    L"  screen\n"
    L"   +---+\n"
    L"   |   |\n"
    L"   +---+\n"
    L"     ^\n"
    L"     |  arm's length\n"
    L"     v\n"
    L"    ___        ___\n"
    L"   |   |      |cam| <-- top of camera at eye level\n"
    L"   |   |      |___|\n"
    L"   | hand|\n"
    L"   |_____|\n"
    L"    user";

// Step 2: hand visibility check.
static const wchar_t* const kStep2Title =
    L"Step 3/6: Hand visibility check";
static const wchar_t* const kStep2Body =
    L"Hold your hand in front of the camera, palm facing the camera. "
    L"You should see your hand highlighted in the cursor ring overlay.\n\n"
    L"If you don't see the highlight:\n"
    L"  - Move your hand closer to the camera\n"
    L"  - Improve the lighting (turn on a desk lamp)\n"
    L"  - Use the Settings window to verify the right camera is "
    L"selected\n"
    L"  - Open the Debug window to see what the camera sees";
static const wchar_t* const kStep2Diagram =
    L"  +-----------+\n"
    L"  |  camera   |\n"
    L"  |   view    |\n"
    L"  |           |\n"
    L"  |   /\\      |\n"
    L"  |  /  \\     |   <- your hand should appear\n"
    L"  |  \\  /     |      inside the ring\n"
    L"  |   \\/      |\n"
    L"  |   ()      |   <- cursor ring overlay\n"
    L"  +-----------+";

// Step 3: practice cursor movement.
static const wchar_t* const kStep3Title =
    L"Step 4/6: Practice cursor movement";
static const wchar_t* const kStep3Body =
    L"Move your hand to control the cursor. The index finger tip is "
    L"the cursor point. Keep your hand flat for smooth movement.\n\n"
    L"The cursor follows your index fingertip. Moving your hand to "
    L"the right moves the cursor to the right, and so on. If the "
    L"movement feels too slow or jumpy, adjust Cursor sensitivity in "
    L"the Settings window.";
static const wchar_t* const kStep3Diagram =
    L"   Hand   ---->   Cursor moves right\n"
    L"\n"
    L"       Thumb\n"
    L"         \\\n"
    L"          \\\n"
    L"  Index ->  *  <- cursor point\n"
    L"          /\n"
    L"         /\n"
    L"       Middle";

// Step 4: practice pinch click.
static const wchar_t* const kStep4Title =
    L"Step 5/6: Practice pinch click";
static const wchar_t* const kStep4Body =
    L"Touch your thumb to your index finger to click. Quick pinch + "
    L"release = single click. Hold pinch = drag.\n\n"
    L"The pinch detector looks at the distance between the thumb tip "
    L"and the index finger tip. When the distance drops below a small "
    L"threshold, the click fires. Releasing your fingers sends a "
    L"mouse-up. Holding the pinch keeps the mouse button held (drag).";
static const wchar_t* const kStep4Diagram =
    L"  Thumb --\\\n"
    L"            \\  -> pinch (click)\n"
    L"  Index  --/\n"
    L"\n"
    L"  Quick pinch + release  =  single click\n"
    L"\n"
    L"  Thumb -----  Index      (no click)\n"
    L"  Thumb --\\                \n"
    L"            \\              \n"
    L"  Index  --/   = single click\n"
    L"\n"
    L"  Hold the pinch = drag";

// Step 5: scroll, drag, pause.
static const wchar_t* const kStep5Title =
    L"Step 6/6: Scroll, drag, pause, emergency stop";
static const wchar_t* const kStep5Body =
    L"Use your left hand: two fingers close together + move up/down = "
    L"scroll. Open hand held 1 second = pause. Both hands open = "
    L"emergency stop.\n\n"
    L"Scroll: bring the index and middle fingers of your LEFT hand "
    L"close together (like a 'peace' sign without the spread), then "
    L"move the hand up or down. The system wheel events follow.\n\n"
    L"Pause: hold an open left hand still for 1 second. The overlay "
    L"indicator turns yellow and gesture input is ignored. Move your "
    L"hand again to resume.\n\n"
    L"Emergency stop: open BOTH hands at the same time. The system "
    L"releases all held keys/buttons and stops processing gestures. "
    L"You can also press Ctrl+Alt+G or hold Esc for 1 second.";
static const wchar_t* const kStep5Diagram =
    L"  Left hand:\n"
    L"    Index  \\\n"
    L"            \\   <- two fingers close + move up = scroll up\n"
    L"    Middle /\n"
    L"\n"
    L"  Left hand open, held 1s = pause\n"
    L"\n"
    L"  Both hands open  =  emergency stop\n"
    L"\n"
    L"  Ctrl+Alt+G  =  emergency stop (hotkey)\n"
    L"  Hold Esc for 1s  =  emergency stop (hotkey)";

// Lookup table indexed by step number.
struct StepContent {
  const wchar_t* title;
  const wchar_t* body;
  const wchar_t* diagram;
};

static const StepContent kSteps[] = {
    {kStep0Title, kStep0Body, kStep0Diagram},  // 0
    {kStep1Title, kStep1Body, kStep1Diagram},  // 1
    {kStep2Title, kStep2Body, kStep2Diagram},  // 2
    {kStep3Title, kStep3Body, kStep3Diagram},  // 3
    {kStep4Title, kStep4Body, kStep4Diagram},  // 4
    {kStep5Title, kStep5Body, kStep5Diagram},  // 5
};

// Compose "Back" / "Next" / "Skip" / "Finish" labels per the
// current step. The Next button reads "Finish" on the last step.
const wchar_t* NextLabelForStep(int step) {
  return (step >= TutorialWindow::kStepCount - 1) ? L"Finish" : L"Next";
}

}  // namespace

TutorialWindow::~TutorialWindow() { Shutdown(); }

bool TutorialWindow::Init(HWND parent) {
  if (hwnd_) return true;  // already created; treat as success

  HINSTANCE hinst = GetModuleHandle(nullptr);

  WNDCLASSEX wc{};
  wc.cbSize        = sizeof(wc);
  wc.style         = 0;
  wc.lpfnWndProc   = &TutorialWindow::WndProc;
  wc.hInstance     = hinst;
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.lpszClassName = kClassName;
  if (!RegisterClassEx(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    VMOSUE_LOG_ERROR("TutorialWindow: RegisterClassEx failed");
    return false;
  }

  g_tutorial = this;

  DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  // Pass `this` as the lpCreateParams so the WndProc can recover
  // the instance pointer from CREATESTRUCTW during WM_NCCREATE
  // (mirrors the SettingsWindow pattern).
  hwnd_ = CreateWindowEx(
      WS_EX_DLGMODALFRAME,
      kClassName,
      L"VMosue Tutorial",
      style,
      CW_USEDEFAULT, CW_USEDEFAULT,
      kWindowW, kWindowH,
      parent, nullptr, hinst, this);
  if (!hwnd_) {
    VMOSUE_LOG_ERROR("TutorialWindow: CreateWindowEx failed");
    g_tutorial = nullptr;
    return false;
  }
  return true;
}

void TutorialWindow::CreateControls() {
  if (!hwnd_) return;
  HINSTANCE hinst = GetModuleHandle(nullptr);

  // Title (single-line static).
  hwndTitle_ = CreateWindowEx(0, L"STATIC", L"",
                               WS_CHILD | WS_VISIBLE | SS_LEFT,
                               kPadX, kTitleY,
                               kWindowW - 2 * kPadX, kTitleH,
                               hwnd_, nullptr, hinst, nullptr);
  // Body (multi-line read-only; we use STATIC + DrawText semantics
  // by sending WM_SETTEXT and letting the static render the text
  // with \n line breaks).
  hwndBody_ = CreateWindowEx(0, L"STATIC", L"",
                             WS_CHILD | WS_VISIBLE | SS_LEFT,
                             kPadX, kBodyY,
                             kWindowW - 2 * kPadX, kBodyH,
                             hwnd_, nullptr, hinst, nullptr);
  // Diagram (monospace-friendly multi-line static). We don't
  // change the font; the default static font renders the ASCII
  // art legibly enough for v0.2.
  hwndDiagram_ = CreateWindowEx(0, L"STATIC", L"",
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                kPadX, kDiagramY,
                                kWindowW - 2 * kPadX, kDiagramH,
                                hwnd_, nullptr, hinst, nullptr);

  // Buttons: Back (left), Skip (right), Next (right-of-Skip). On
  // the first step Back is disabled; on the last step Next is
  // labeled "Finish" but the behavior is the same (hide).
  int backX = kPadX;
  int skipX = kWindowW - kPadX - kBtnW;
  int nextX = skipX - kBtnGap - kBtnW;

  hwndBack_ = CreateWindowEx(0, WC_BUTTON, L"Back",
                             WS_CHILD | WS_VISIBLE | WS_DISABLED
                                 | BS_PUSHBUTTON,
                             backX, kBtnY,
                             kBtnW, kBtnH,
                             hwnd_, reinterpret_cast<HMENU>(kIdBack),
                             hinst, nullptr);
  hwndSkip_ = CreateWindowEx(0, WC_BUTTON, L"Skip",
                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             skipX, kBtnY,
                             kBtnW, kBtnH,
                             hwnd_, reinterpret_cast<HMENU>(kIdSkip),
                             hinst, nullptr);
  hwndNext_ = CreateWindowEx(0, WC_BUTTON, L"Next",
                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             nextX, kBtnY,
                             kBtnW, kBtnH,
                             hwnd_, reinterpret_cast<HMENU>(kIdNext),
                             hinst, nullptr);
}

void TutorialWindow::RenderStep() {
  if (!hwnd_) return;
  // RenderStepStatic is the no-instance-pointer variant used by
  // the static WndProc when called from outside (we never do that
  // in v0.2, but the helper is there for symmetry with future
  // refactors).
  if (hwnd_ == nullptr) return;
  // Clamp step in case of programmatic misuse.
  if (currentStep_ < 0) currentStep_ = 0;
  if (currentStep_ >= kStepCount) currentStep_ = kStepCount - 1;

  const StepContent& s = kSteps[currentStep_];

  if (hwndTitle_) {
    SetWindowTextW(hwndTitle_, s.title);
  }
  if (hwndBody_) {
    // Replace "\n" with the real newline char so the static
    // control renders each line on its own row. DrawTextW
    // (used by the static control) splits on '\n' when the
    // control is tall enough; in practice the default
    // rendering wraps on the newline character. We keep the
    // source literal "\n" for readability and translate here
    // once.
    std::wstring body;
    body.reserve(std::wcslen(s.body) + 16);
    for (const wchar_t* p = s.body; *p; ++p) {
      if (p[0] == L'\\' && p[1] == L'n') {
        body.push_back(L'\n');
        ++p;
      } else {
        body.push_back(*p);
      }
    }
    SetWindowTextW(hwndBody_, body.c_str());
  }
  if (hwndDiagram_) {
    SetWindowTextW(hwndDiagram_, s.diagram);
  }
  // Back enabled only after the first step.
  if (hwndBack_) {
    EnableWindow(hwndBack_, currentStep_ > 0 ? TRUE : FALSE);
  }
  // Next button label: "Next" except on the last step where it
  // reads "Finish" (and the click closes the window either way).
  if (hwndNext_) {
    SetWindowTextW(hwndNext_, NextLabelForStep(currentStep_));
  }
}

void TutorialWindow::RenderStepStatic(HWND) {
  if (g_tutorial) g_tutorial->RenderStep();
}

bool TutorialWindow::SetStep(int newStep) {
  if (newStep < 0) newStep = 0;
  if (newStep >= kStepCount) newStep = kStepCount - 1;
  if (newStep == currentStep_) return false;
  currentStep_ = newStep;
  RenderStep();
  return true;
}

void TutorialWindow::OnCommand(UINT id, UINT /*code*/) {
  if (id == kIdBack) {
    if (currentStep_ > 0) {
      SetStep(currentStep_ - 1);
    }
  } else if (id == kIdNext) {
    if (currentStep_ < kStepCount - 1) {
      SetStep(currentStep_ + 1);
    } else {
      // Last step: hide the window. State is preserved so a
      // re-open returns to step 5 (Finish).
      Hide();
    }
  } else if (id == kIdSkip) {
    Hide();
  }
}

void TutorialWindow::Show() {
  if (!hwnd_) return;
  // Re-render the current step's text in case the user navigated
  // away (defensive — RenderStep is also called on step changes,
  // but it costs almost nothing to refresh here).
  RenderStep();
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);
}

void TutorialWindow::Hide() {
  if (!hwnd_) return;
  ShowWindow(hwnd_, SW_HIDE);
}

void TutorialWindow::Shutdown() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  // Null out the static pointer so a stale instance pointer
  // never ends up in a WndProc callback.
  if (g_tutorial == this) g_tutorial = nullptr;
  // Child control handles are owned by the window; the next
  // Init() will recreate them in CreateControls().
  hwndTitle_ = nullptr;
  hwndBody_  = nullptr;
  hwndDiagram_ = nullptr;
  hwndBack_  = nullptr;
  hwndNext_  = nullptr;
  hwndSkip_  = nullptr;
}

LRESULT CALLBACK TutorialWindow::WndProc(HWND hwnd, UINT msg,
                                         WPARAM w, LPARAM l) {
  // Locate the instance via the per-window user data. Set in
  // WM_NCCREATE so we don't depend on a global being current
  // (defensive: matches SettingsWindow's pattern).
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                     reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return 1;
  }
  TutorialWindow* self = reinterpret_cast<TutorialWindow*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self) return DefWindowProc(hwnd, msg, w, l);

  switch (msg) {
    case WM_CREATE:
      // Build all child controls. After CreateControls the
      // window is fully populated; we render the initial step
      // here (step 0) so a Show() right after Init() displays
      // the right content even if the caller never calls
      // RenderStep() explicitly.
      self->CreateControls();
      self->RenderStep();
      return 0;

    case WM_COMMAND: {
      UINT id = LOWORD(w);
      UINT code = HIWORD(w);
      // We only act on BN_CLICKED (0) — the WndProc also gets
      // accelerator messages with code != 0 which we ignore.
      if (code == BN_CLICKED) self->OnCommand(id, code);
      return 0;
    }

    case WM_CLOSE:
      // Hide, don't destroy. State (currentStep_) is preserved
      // so re-opening shows the same step.
      ShowWindow(hwnd, SW_HIDE);
      return 0;

    case WM_DESTROY:
      // The window is going away (e.g. WM_QUIT or App
      // teardown). Null out the static pointer so a future
      // WndProc can't dispatch into a freed instance.
      if (g_tutorial == self) g_tutorial = nullptr;
      self->hwnd_ = nullptr;
      return 0;

    default:
      return DefWindowProc(hwnd, msg, w, l);
  }
}

}  // namespace vmosue
