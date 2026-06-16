// AutoStart - thin wrapper around the HKCU Run registry key. See
// AutoStart.h for the contract.

#include "platform/AutoStart.h"

#include "util/Logger.h"

#include <windows.h>
#include <string>

namespace vmosue {

namespace {

// Convert a wide string to UTF-8 for logging. The spdlog/fmt format
// strings in this project are narrow (char-based) and cannot accept
// wchar_t* or std::wstring arguments, so any wide path that needs
// to appear in a log line must go through here.
std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                         static_cast<int>(w.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return {};
  std::string out(static_cast<size_t>(needed), '\0');
  const int written = WideCharToMultiByte(
      CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
      out.data(), needed, nullptr, nullptr);
  if (written != needed) out.resize(static_cast<size_t>(written));
  return out;
}

std::string WideToUtf8(const wchar_t* w) {
  return w ? WideToUtf8(std::wstring(w)) : std::string();
}

// Subkey under HKCU that Windows reads at user logon to populate the
// startup items. Real name; matches the SDK header.
constexpr const wchar_t* kRunKeyPath =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// Value name. Matches the app's display name; the leading character
// is uppercase "VMosue" — Windows shows this verbatim in Task Manager
// -> Startup. Keep the spelling in sync with the binary's
// ProductName in the NSIS installer (Task 35) so users see a single
// entry that matches the executable.
constexpr const wchar_t* kValueName = L"VMosue";

// Flag appended to the exe path so the launched instance starts into
// the system tray rather than popping up the settings window. Read
// by main() before App construction; see App.cpp's Run() entry path.
constexpr const wchar_t* kLaunchFlag = L" --minimized";

// Buffer size for GetModuleFileNameW. MAX_PATH (260) is enough for
// any path that doesn't include a long-path prefix; modern Windows
// extends this to 32k via the \\?\ prefix, but a 32k buffer is
// overkill here — VMosue's installer puts the binary under
// C:\Program Files\VMosue which is well under 260 chars.
constexpr DWORD kExePathBuf = MAX_PATH;

// Open the HKCU Run key with read+write access for the Enable/
// Disable paths. Returns nullptr on failure (and logs the LSTATUS).
HKEY OpenRunKey(REGSAM access) {
  HKEY hkey = nullptr;
  LSTATUS s = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0,
                            access, &hkey);
  if (s != ERROR_SUCCESS) {
    VMOSUE_LOG_WARN("AutoStart: RegOpenKeyExW({}) failed: {}",
                    access == KEY_READ ? "KEY_READ" : "KEY_WRITE",
                    static_cast<long>(s));
    return nullptr;
  }
  return hkey;
}

// Format a human-readable Win32 error code. Win32 uses the same
// LSTATUS typedef (long) so this works for both.
std::string FormatWinError(LSTATUS s) {
  wchar_t buf[256] = {};
  DWORD n = FormatMessageW(
      FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(s), 0, buf, 256, nullptr);
  if (n == 0) {
    return "Win32 error " + std::to_string(static_cast<long>(s));
  }
  // Trim trailing newline / period for cleaner log lines.
  while (n > 0 && (buf[n - 1] == L'\n' || buf[n - 1] == L'\r' ||
                   buf[n - 1] == L'.')) {
    buf[--n] = 0;
  }
  std::string out;
  out.reserve(n);
  for (DWORD i = 0; i < n; ++i) {
    // ASCII-safe cast (system error strings are English-only in the
    // Windows code path we hit here).
    out.push_back(static_cast<char>(buf[i]));
  }
  return out;
}

}  // namespace

bool AutoStart::IsEnabled() {
  HKEY hkey = OpenRunKey(KEY_READ);
  if (!hkey) return false;

  // Probe for the value with a zero-length query. RegQueryValueExW
  // returns ERROR_SUCCESS if the value exists (and tells us its
  // actual size), ERROR_FILE_NOT_FOUND if it does not, or some
  // other code on a deeper failure. We don't care about the data —
  // just the existence check.
  LSTATUS s = RegQueryValueExW(hkey, kValueName, nullptr, nullptr,
                               nullptr, nullptr);
  RegCloseKey(hkey);
  if (s == ERROR_SUCCESS) return true;
  if (s == ERROR_FILE_NOT_FOUND) return false;
  // Anything else (access denied, etc.) — log and report "not
  // enabled". The Settings UI will then show the checkbox in the
  // unchecked state, which matches the OS's view.
  VMOSUE_LOG_WARN("AutoStart: RegQueryValueExW existence check "
                  "failed: {}", FormatWinError(s));
  return false;
}

bool AutoStart::Enable() {
  // Look up our own .exe path. GetModuleFileNameW(NULL, ...) returns
  // the full path of the executable image that started this process.
  wchar_t exePath[kExePathBuf] = {};
  DWORD n = GetModuleFileNameW(nullptr, exePath, kExePathBuf);
  if (n == 0 || n >= kExePathBuf) {
    DWORD err = GetLastError();
    VMOSUE_LOG_ERROR("AutoStart: GetModuleFileNameW failed: {}",
                     FormatWinError(static_cast<LSTATUS>(err)));
    return false;
  }

  // Compose "<exe> --minimized". Worst-case length is MAX_PATH + 13
  // for the flag + null; allocate a little slack so a future flag
  // addition doesn't silently overflow.
  std::wstring command;
  command.reserve(n + 32);
  // std::wstring::append has both (const wchar_t*, size_t) and
  // (size_t, wchar_t) overloads; cast the count to size_t so the
  // narrower overload doesn't win via implicit conversion of n.
  command.append(exePath, static_cast<size_t>(n));
  command.append(kLaunchFlag);

  HKEY hkey = OpenRunKey(KEY_WRITE);
  if (!hkey) return false;

  // REG_SZ (wide string). cbData includes the trailing NUL per the
  // SDK contract — RegSetValueExW will write that many bytes; the
  // registry stores it as a NUL-terminated string.
  DWORD cbData = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
  LSTATUS s = RegSetValueExW(hkey, kValueName, 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(command.c_str()),
                             cbData);
  RegCloseKey(hkey);
  if (s != ERROR_SUCCESS) {
    VMOSUE_LOG_ERROR("AutoStart: RegSetValueExW failed: {}",
                     FormatWinError(s));
    return false;
  }
  // VMOSUE_LOG_* uses a narrow-char format string (spdlog/fmt cannot
  // mix narrow format spec with wide-char arguments), so convert the
  // wide registry value name and command to UTF-8 for logging only.
  VMOSUE_LOG_INFO("AutoStart: registered HKCU\\...\\Run\\{} = {}",
                  vmosue::WideToUtf8(kValueName),
                  vmosue::WideToUtf8(command));
  return true;
}

bool AutoStart::Disable() {
  HKEY hkey = OpenRunKey(KEY_WRITE);
  if (!hkey) return false;

  LSTATUS s = RegDeleteValueW(hkey, kValueName);
  RegCloseKey(hkey);
  if (s == ERROR_SUCCESS) {
    VMOSUE_LOG_INFO("AutoStart: removed HKCU\\...\\Run\\{}",
                    WideToUtf8(kValueName));
    return true;
  }
  if (s == ERROR_FILE_NOT_FOUND) {
    // Already disabled — treat as success so Disable() is idempotent
    // and the Settings checkbox can be unchecked regardless of the
    // current registry state.
    return true;
  }
  VMOSUE_LOG_ERROR("AutoStart: RegDeleteValueW failed: {}",
                   FormatWinError(s));
  return false;
}

}  // namespace vmosue