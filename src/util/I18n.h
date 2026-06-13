#pragma once

// I18n (Task 31): tiny message-bundle lookup for the VMosue UI.
//
// Design notes
// ------------
//  - One Meyers-singleton: `I18n::Get()`. All public methods are
//    thin wrappers around the instance, so the API reads as if it
//    were free functions (`I18n::TW("tray.pause")`).
//
//  - Lazy-load: the first call to T() / TW() loads the JSON file
//    matching the active language. Subsequent lookups are O(log n)
//    against an in-memory std::map. There is no unload; the
//    bundle lives for the process lifetime, which is acceptable
//    for a desktop app that runs for hours.
//
//  - Language resolution order:
//      1. Explicit override via SetLanguage().
//      2. System locale via GetUserDefaultLocaleName (Win32).
//         "zh*" -> "zh"; everything else -> "en".
//      3. Hard-coded fallback "en".
//
//  - File lookup:
//      base_dir + "/" + lang + ".json". base_dir defaults to the
//      process working directory + "resources/i18n". Tests use
//      SetBaseDir() to point at a temp directory. If the requested
//      file is missing, we transparently try "en.json" before
//      giving up.
//
//  - Key fallback:
//      requested lang -> "en" -> the key itself. So even if the
//      JSON files are missing entirely, T() returns a string the
//      caller can display and the UI never crashes on a missing
//      translation.
//
//  - Wide-string: the JSON values are UTF-8 in nlohmann_json's
//    std::string. Win32 wants std::wstring (UTF-16 on Windows).
//    We use MultiByteToWideChar(CP_UTF8, ...) so the round-trip
//    is faithful for CJK characters. The narrow T() is also
//    exported for log messages.

#include <filesystem>
#include <map>
#include <string>

namespace vmosue {

class I18n {
 public:
  // Singleton accessor (Meyers style: thread-safe init, no exit
  // ordering surprises).
  static I18n& Get();

  // ---- Lookup ---------------------------------------------------------

  // Narrow (UTF-8) translation lookup. Returns `default` if both
  // the requested language and English are missing the key. When
  // `default` is empty, the key itself is returned (so the caller
  // gets a visible reminder in the UI that the key is untranslated).
  std::string T(const std::string& key) const;

  // Same as T() but returns std::wstring (UTF-16). This is what
  // the Win32 UI uses for menu items, window titles, and static
  // controls.
  std::wstring TW(const std::string& key) const;

  // ---- Configuration --------------------------------------------------

  // Override the lookup directory. Default is the process CWD +
  // "resources/i18n". Used by tests; may also be called by App
  // once the install layout is known (next to the .exe).
  void SetBaseDir(const std::filesystem::path& p);

  // Override the active language. Bypasses the system locale
  // detection. Empty string restores auto-detect.
  void SetLanguage(const std::string& lang);

  // Returns the active language code ("en" or "zh" for v1.0).
  std::string Language() const;

  // Detects the system language. Public so tests can compare
  // without mutating the singleton's state.
  static std::string DetectSystemLanguage();

  // ---- Test / debug helpers ------------------------------------------

  // Reset internal state: clears the loaded bundle, restores
  // auto-detect, and clears any explicit base-dir override. Used
  // by unit tests so each test starts from a known baseline.
  void ResetForTests();

 private:
  I18n();  // private ctor: singleton
  I18n(const I18n&) = delete;
  I18n& operator=(const I18n&) = delete;

  void EnsureLoaded() const;  // mutable because the cache is the
                             // only state and we want T() const.
  bool LoadFromFile(const std::filesystem::path& path,
                    std::map<std::string, std::string>& target) const;

  mutable bool loaded_ = false;
  mutable std::string lang_;            // active language code
  mutable std::filesystem::path base_;  // base dir for *.json
  mutable std::string explicitLang_;    // SetLanguage() override;
                                        // empty means "auto"
  mutable std::string fallbackLang_ = "en";  // secondary chain
};

}  // namespace vmosue