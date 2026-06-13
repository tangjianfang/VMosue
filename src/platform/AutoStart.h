#pragma once
// AutoStart - thin wrapper around the HKCU Run registry key used by
// the OS to launch a binary at user logon.
//
// Task 32 (Auto-Start). The Settings dialog exposes a "Start VMosue
// with Windows" checkbox; this module is the single point of truth
// for reading/writing the corresponding registry value so the
// settings UI stays decoupled from advapi32 details.
//
// Registry layout:
//   HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run
//     Value name : L"VMosue"
//     Value data : "<full path to vmosue.exe> --minimized"
//
// HKCU (per-user) is preferred over HKLM (machine-wide) because
// HKLM requires admin rights and is rejected by UAC for a
// non-elevated app. HKCU is the standard hook used by Dropbox,
// Slack, Steam, and other consumer apps to register autostart.
//
// The `--minimized` flag tells the executable to launch into the
// tray without showing any windows. The flag is consumed by main()
// (Task 14) before the App instance is constructed.
//
// All three methods are static. There is no per-instance state; the
// class exists only as a namespace.

namespace vmosue {

class AutoStart {
 public:
  // Return true if the "VMosue" value exists under the HKCU Run key.
  // Returns false on any registry error (key missing, value missing,
  // access denied, etc.) — callers should treat this as "not enabled".
  static bool IsEnabled();

  // Set the "VMosue" value to "<exe path> --minimized" so that Windows
  // launches the app at next user logon. Returns true on success,
  // false on any registry error. The caller is responsible for
  // surfacing the error to the user (the Settings window shows a
  // MessageBox on failure).
  static bool Enable();

  // Delete the "VMosue" value from the HKCU Run key. Idempotent:
  // returns true if the value is gone (whether because we deleted it
  // or because it was never there in the first place). Returns false
  // only on a registry error other than "value not found".
  static bool Disable();
};

}  // namespace vmosue