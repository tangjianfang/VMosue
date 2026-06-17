#pragma once
// Task 26: System tray icon with a small context menu.
//
// Owns a single Shell_NotifyIcon registration. Callbacks for the menu
// items (Pause/Resume, Settings, Debug, Tutorial, Exit) are supplied by
// the App via MenuCallbacks. The icon is the shared IDI_APPLICATION
// system icon for v0.2; a custom icon can be loaded later from
// resources/icons/tray.ico.
//
// Threading: all public methods are called from the main (UI) thread
// that owns the message-only window passed to Init. The HWND passed
// to Init MUST outlive this object — Shutdown() does not destroy
// that window, it only tears down the tray icon. The App owns both.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <functional>
#include <string>

namespace vmosue {

class TrayIcon {
 public:
  struct MenuCallbacks {
    std::function<void()> onTogglePause;
    std::function<void()> onOpenSettings;
    std::function<void()> onOpenDebug;
    std::function<void()> onOpenTutorial;
    std::function<void()> onOpenHelp;
    std::function<void()> onExit;
  };

  // Register the tray icon and target it at `hwndMessage`.
  // The HWND must remain valid for the lifetime of this object.
  // Returns false on Shell_NotifyIcon(NIM_ADD) failure.
  bool Init(HWND hwndMessage, const MenuCallbacks&);

  // Remove the tray icon. Idempotent. Does not destroy the message
  // window — the caller owns that HWND.
  void Shutdown();

  // Update the tooltip shown on hover. Safe to call any time after
  // Init returns true.
  void SetTooltip(const std::wstring&);

  // Update the pause flag so the next ShowContextMenu picks the
  // right label (tray.pause vs. tray.resume). The actual pause
  // state lives in the App; this setter is the way App tells
  // the tray icon to flip its label.
  void SetPaused(bool paused);

  // Dispatch a window message into the tray-icon handler. Exposed
  // publicly so that an external WndProc (registered on the App's
  // message-only window) can forward tray-icon notifications into
  // this object. Safe to call from any thread — the implementation
  // synchronises with the UI thread internally.
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

 private:
  void ShowContextMenu(HWND);

  HWND hwnd_ = nullptr;                 // message-only window owned by App
  bool added_ = false;                  // NIM_ADD succeeded
  UINT iconId_ = 1;                     // stable ID for the tray icon
  std::wstring tooltip_;                // last SetTooltip value
  MenuCallbacks callbacks_{};           // stored copy of callbacks
  bool paused_ = false;                 // drives tray.pause / tray.resume
                                        // label; toggled via the menu
                                        // callback that App supplies.
};

}  // namespace vmosue