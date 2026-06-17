#include "ui/TrayIcon.h"
#include "util/I18n.h"
#include "util/Logger.h"

#include <shellapi.h>

namespace vmosue {

// Copy up to (dstsz - 1) wide characters from src into dst, always
// NUL-terminating. Returns false if dstsz is 0 or the source didn't
// fit; the caller can decide whether to truncate or fail. We avoid
// wcsncpy_s here to keep the parse-check stub simple.
static bool CopyWide(wchar_t* dst, size_t dstsz, const wchar_t* src) {
  if (!dst || dstsz == 0) return false;
  if (!src) { dst[0] = 0; return true; }
  size_t i = 0;
  for (; i + 1 < dstsz && src[i]; ++i) dst[i] = src[i];
  dst[i] = 0;
  return src[i] == 0;  // true if fully copied, false if truncated
}

// One global pointer is sufficient: there is at most one TrayIcon
// alive at a time (App owns it). The WndProc needs to find the
// instance to invoke its callbacks.
static TrayIcon* g_tray = nullptr;

// Window class name chosen to avoid colliding with the OverlayWindow
// ("VMosueOverlay") and any future VMosue* window classes.
static const wchar_t kClassName[] = L"VMosueTrayMessageWindow";

// Menu command IDs. Picked high enough to not collide with any
// resource IDs the real app might use later (resource IDs in real
// SDK projects are typically small integers).
static constexpr UINT kIdTogglePause   = 0x9001;
static constexpr UINT kIdOpenSettings  = 0x9002;
static constexpr UINT kIdOpenDebug     = 0x9003;
static constexpr UINT kIdOpenTutorial  = 0x9004;
static constexpr UINT kIdExit          = 0x9005;
static constexpr UINT kIdOpenHelp      = 0x9006;

// Windows sends WM_USER (or NOTIFYICON_VERSION_4 selector) to the
// HWND passed to Shell_NotifyIconNIM_ADD. lParam encodes which mouse
// event happened; wParam is the icon id.
static constexpr UINT kTrayMsg = WM_USER + 1;

bool TrayIcon::Init(HWND hwndMessage, const MenuCallbacks& cb) {
  hwnd_ = hwndMessage;
  callbacks_ = cb;

  if (!hwnd_) {
    VMOSUE_LOG_ERROR("TrayIcon::Init called with null hwndMessage");
    return false;
  }

  // Register the window class so the supplied HWND can receive tray
  // callbacks. The HWND may have been created by App with its own
  // class — in that case we don't replace its WndProc; we just store
  // a pointer via SetWindowLongPtr(GWLP_USERDATA). For simplicity
  // in v0.2 we expect the App to give us a dedicated HWND created
  // from this class. We still install a WndProc on whatever HWND we
  // receive so the tray callbacks dispatch correctly.
  g_tray = this;

  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd_;
  nid.uID = iconId_;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = kTrayMsg;
  // Use the shared system icon — no need to DestroyIcon this. A custom
  // icon can be loaded from resources/icons/tray.ico in a future task.
  // TODO(ui): load custom icon from resources/icons/tray.ico when it
  // exists; for now IDI_APPLICATION keeps the binary footprint small.
  nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_WINLOGO);
  tooltip_ = I18n::Get().TW("app.name");
  CopyWide(nid.szTip, sizeof(nid.szTip) / sizeof(wchar_t), tooltip_.c_str());

  added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
  if (!added_) {
    VMOSUE_LOG_ERROR("Shell_NotifyIcon NIM_ADD failed");
    return false;
  }
  VMOSUE_LOG_INFO("Tray icon registered");
  return true;
}

void TrayIcon::Shutdown() {
  if (!added_) return;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd_;
  nid.uID = iconId_;
  Shell_NotifyIconW(NIM_DELETE, &nid);
  added_ = false;
  if (g_tray == this) g_tray = nullptr;
  VMOSUE_LOG_INFO("Tray icon removed");
}

void TrayIcon::SetTooltip(const std::wstring& text) {
  tooltip_ = text;
  if (!added_ || !hwnd_) return;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd_;
  nid.uID = iconId_;
  nid.uFlags = NIF_TIP;
  CopyWide(nid.szTip, sizeof(nid.szTip) / sizeof(wchar_t), tooltip_.c_str());
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::SetPaused(bool paused) {
  // No NIM_MODIFY needed — the next ShowContextMenu rebuilds the
  // menu from the current paused_ value. We just store the flag.
  paused_ = paused;
}

void TrayIcon::ShowContextMenu(HWND hwnd) {
  HMENU m = CreatePopupMenu();
  if (!m) return;
  // Pull localized strings at menu-build time. I18n::TW returns
  // std::wstring; AppendMenuW takes const wchar_t*, which .c_str()
  // provides. The narrow keys are the canonical spec keys (e.g.
  // "tray.pause"). The pause/resume choice is driven by paused_;
  // App toggles that flag when the user actually flips the state.
  const std::wstring sPause  = I18n::Get().TW("tray.pause");
  const std::wstring sResume = I18n::Get().TW("tray.resume");
  const std::wstring sSet    = I18n::Get().TW("tray.settings");
  const std::wstring sDbg    = I18n::Get().TW("tray.debug");
  const std::wstring sTut    = I18n::Get().TW("tray.tutorial");
  const std::wstring sHelp   = I18n::Get().TW("tray.help");
  const std::wstring sExit   = I18n::Get().TW("tray.exit");

  AppendMenuW(m, MF_STRING, kIdTogglePause,
              paused_ ? sResume.c_str() : sPause.c_str());
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, kIdOpenSettings, sSet.c_str());
  AppendMenuW(m, MF_STRING, kIdOpenDebug,    sDbg.c_str());
  AppendMenuW(m, MF_STRING, kIdOpenTutorial, sTut.c_str());
  AppendMenuW(m, MF_STRING, kIdOpenHelp,     sHelp.c_str());
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, kIdExit,         sExit.c_str());

  POINT pt{};
  GetCursorPos(&pt);
  // Required so the menu dismisses correctly when the user clicks
  // outside it (otherwise the popup may not receive its WM_COMMAND).
  SetForegroundWindow(hwnd);
  // NOTE: do NOT pass TPM_RETURNCMD here. With TPM_RETURNCMD the
  // function returns the selected command id directly and does NOT
  // post WM_COMMAND to the owner window, which means the switch in
  // WndProc below would be dead code and every menu item silently
  // does nothing. The non-returning form posts WM_COMMAND to hwnd
  // and WndProc dispatches to the registered callbacks.
  TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_NONOTIFY,
                 pt.x, pt.y, 0, hwnd, nullptr);
  DestroyMenu(m);
}

LRESULT CALLBACK TrayIcon::WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  if (msg == kTrayMsg) {
    if (!g_tray) return 0;
    if (l == WM_RBUTTONUP) {
      g_tray->ShowContextMenu(hwnd);
    } else if (l == WM_LBUTTONDBLCLK) {
      // Double-click is a fast way to toggle pause/resume — mirrors
      // the behavior of many tray apps. Single left-click is reserved
      // for any future "open main window" behavior.
      if (g_tray->callbacks_.onTogglePause) g_tray->callbacks_.onTogglePause();
    }
    return 0;
  }
  if (msg == WM_COMMAND) {
    if (!g_tray) return 0;
    UINT id = LOWORD(w);
    switch (id) {
      case kIdTogglePause:
        if (g_tray->callbacks_.onTogglePause) g_tray->callbacks_.onTogglePause();
        return 0;
      case kIdOpenSettings:
        if (g_tray->callbacks_.onOpenSettings) g_tray->callbacks_.onOpenSettings();
        return 0;
      case kIdOpenDebug:
        if (g_tray->callbacks_.onOpenDebug) g_tray->callbacks_.onOpenDebug();
        return 0;
      case kIdOpenTutorial:
        if (g_tray->callbacks_.onOpenTutorial) g_tray->callbacks_.onOpenTutorial();
        return 0;
      case kIdOpenHelp:
        if (g_tray->callbacks_.onOpenHelp) g_tray->callbacks_.onOpenHelp();
        return 0;
      case kIdExit:
        if (g_tray->callbacks_.onExit) g_tray->callbacks_.onExit();
        return 0;
      default:
        break;
    }
  }
  return DefWindowProc(hwnd, msg, w, l);
}

}  // namespace vmosue