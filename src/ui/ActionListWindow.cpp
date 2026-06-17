#include "ui/ActionListWindow.h"
#include "ui/ActionReference.h"
#include "util/I18n.h"
#include "util/Logger.h"

#include <string>

namespace vmosue {

namespace {

// Window class name chosen to avoid colliding with the
// TutorialWindow's "VMosueTutorialWindow", TrayIcon's
// "VMosueTrayMessageWindow", etc.
static const wchar_t kClassName[] = L"VMosueActionListWindow";
static ActionListWindow* g_self = nullptr;

// Child control IDs.
static constexpr UINT_PTR kIdClose = 0xC001;

// Layout constants. The window is fixed-size (no WS_THICKFRAME
// or WS_MAXIMIZEBOX) so we can hard-code coordinates.
static constexpr int kWindowW = 480;
static constexpr int kWindowH = 520;
static constexpr int kPadX    = 16;
static constexpr int kTitleY  = 14;
static constexpr int kTitleH  = 24;
static constexpr int kBodyY   = 48;
static constexpr int kBodyH   = 400;
static constexpr int kBtnY    = 460;
static constexpr int kBtnH    = 32;
static constexpr int kBtnW    = 90;

}  // namespace

LRESULT CALLBACK ActionListWindow::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCT*>(l);
    SetWindowLongPtrW(h, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return DefWindowProc(h, m, w, l);
  }
  if (m == WM_GETMINMAXINFO) {
    // Pin the window to kWindowW x kWindowH. Without this, the
    // window manager would still respect WS_THICKFRAME-less size
    // for resizing corners on some themes, which would let the
    // body text overflow the visible area.
    auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
    mmi->ptMinTrackSize = POINT{kWindowW, kWindowH};
    mmi->ptMaxTrackSize = POINT{kWindowW, kWindowH};
    return 0;
  }
  if (m == WM_COMMAND) {
    if (LOWORD(w) == kIdClose) {
      ActionListWindow* self = reinterpret_cast<ActionListWindow*>(
          GetWindowLongPtrW(h, GWLP_USERDATA));
      if (self) self->Hide();
      return 0;
    }
  }
  if (m == WM_CLOSE) {
    ActionListWindow* self = reinterpret_cast<ActionListWindow*>(
        GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) self->Hide();
    return 0;
  }
  return DefWindowProc(h, m, w, l);
}

ActionListWindow::~ActionListWindow() {
  Shutdown();
}

bool ActionListWindow::Init(HWND parent) {
  if (hwnd_) return true;
  WNDCLASSEX wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kClassName;
  if (!RegisterClassEx(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    VMOSUE_LOG_ERROR("ActionListWindow: RegisterClassEx failed");
    return false;
  }

  std::wstring title = I18n::Get().TW("help.title");
  hwnd_ = CreateWindowExW(
      0, kClassName, title.c_str(),
      WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
      CW_USEDEFAULT, CW_USEDEFAULT,
      kWindowW, kWindowH,
      parent, nullptr,
      GetModuleHandle(nullptr), this);
  if (!hwnd_) {
    VMOSUE_LOG_ERROR("ActionListWindow: CreateWindowExW failed");
    return false;
  }
  g_self = this;
  CreateControls();
  RenderBody();
  return true;
}

void ActionListWindow::CreateControls() {
  if (!hwnd_) return;

  // Body: a multi-line read-only STATIC that holds the formatted
  // action reference. We use SS_LEFT with DT_WORDBREAK so long
  // descriptions wrap to the body width.
  hwndBody_ = CreateWindowExW(
      0, WC_STATICW, L"",
      WS_CHILD | WS_VISIBLE | SS_LEFT,
      kPadX, kBodyY, kWindowW - 2 * kPadX, kBodyH,
      hwnd_, nullptr,
      GetModuleHandle(nullptr), nullptr);

  // Close button.
  std::wstring sClose = I18n::Get().TW("tray.exit");
  // Use a generic "Close" — the i18n "tray.exit" reads as
  // "Exit" which is the wrong label for a modeless dismiss
  // button. Keep tray.exit untouched and just hardcode "Close"
  // in English/Chinese here. (Falls back to the key if missing
  // — same defensive behavior as the rest of the UI.)
  sClose = I18n::Get().TW("actionList.close");
  if (sClose == L"actionList.close") sClose = L"Close / 关闭";
  hwndClose_ = CreateWindowExW(
      0, WC_BUTTONW, sClose.c_str(),
      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
      (kWindowW - kBtnW) / 2, kBtnY, kBtnW, kBtnH,
      hwnd_, reinterpret_cast<HMENU>(kIdClose),
      GetModuleHandle(nullptr), nullptr);
}

void ActionListWindow::RenderBody() {
  if (!hwndBody_) return;
  // Build "<gesture>  ->  <action>\n  <description>" rows,
  // one per ActionRef, separated by a blank line.
  std::wstring body;
  for (const auto& ref : kActionList) {
    std::wstring g = I18n::Get().TW(ref.gestureKey);
    std::wstring a = I18n::Get().TW(ref.actionKey);
    std::wstring d = I18n::Get().TW(ref.descriptionKey);
    if (g.empty()) {
      // Missing translation: keep the key as visible feedback so
      // the user can see something is wrong with the i18n files.
      std::wstring key(ref.gestureKey,
                       ref.gestureKey + std::char_traits<char>::length(ref.gestureKey));
      g = L"[" + key + L"]";
    }
    body += g;
    body += L"  ->  ";
    body += a;
    body += L"\n  ";
    body += d;
    body += L"\n\n";
  }
  SetWindowTextW(hwndBody_, body.c_str());
}

void ActionListWindow::Show() {
  if (!hwnd_) return;
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);
  BringWindowToTop(hwnd_);
}

void ActionListWindow::Hide() {
  if (!hwnd_) return;
  ShowWindow(hwnd_, SW_HIDE);
}

void ActionListWindow::Toggle() {
  if (!hwnd_) return;
  if (IsWindowVisible(hwnd_)) {
    Hide();
  } else {
    Show();
  }
}

void ActionListWindow::Shutdown() {
  if (!hwnd_) return;
  // Detach the global first so any in-flight WM_NCCREATE-style
  // message that lands after Shutdown doesn't see a half-dead
  // pointer.
  if (g_self == this) g_self = nullptr;
  hwndBody_ = nullptr;
  hwndClose_ = nullptr;
  DestroyWindow(hwnd_);
  hwnd_ = nullptr;
}

}  // namespace vmosue
