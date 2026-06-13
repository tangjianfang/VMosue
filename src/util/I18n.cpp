#include "util/I18n.h"

#include "util/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

// Win32 API for system-locale detection. Defined in <windows.h>.
// On the parse-check stub we provide a thin inline shim that
// returns "en-US" so the translation unit compiles cleanly under
// cl /Zs without the full SDK. On a real build, the SDK header
// supplies the symbol.
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vmosue {

namespace {

// In-memory bundle, indexed by key. The map is built once at load
// time and never mutated again — T() is read-only after that.
using Bundle = std::map<std::string, std::string>;

// Parsed bundle cache. Held in a function-local static so the
// instance method can reach it without exposing it as a member.
// We use a plain struct (not unique_ptr) because the value type
// is small and we want one allocation per loaded language.
struct BundleCache {
  // We store two parallel maps so a fallback chain is a single
  // lookup pair (primary + fallback). If primary misses, fall back.
  // If fallback also misses, return the key.
  Bundle primary;     // lang_ (e.g. "zh")
  Bundle fallback;    // "en"
};

BundleCache& Cache() {
  static BundleCache c;
  return c;
}

// Convert a UTF-8 std::string to UTF-16 std::wstring via Win32.
// We need MultiByteToWideChar because nlohmann_json stores UTF-8
// and Win32 controls take wchar_t* (UTF-16 on Windows). When the
// input is empty we return an empty wstring without calling the
// API (MultiByteToWideChar on an empty source returns 0).
std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                   s.data(), static_cast<int>(s.size()),
                                   nullptr, 0);
  if (needed <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(needed), L'\0');
  int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    s.data(), static_cast<int>(s.size()),
                                    out.data(), needed);
  if (written <= 0) return std::wstring();
  return out;
}

// Trim a leading BOM if present. Real JSON files in the project
// should be UTF-8 without BOM, but VSCode sometimes writes one;
// removing it here is cheap and avoids the parse failing.
std::string StripBom(const std::string& s) {
  if (s.size() >= 3 &&
      static_cast<unsigned char>(s[0]) == 0xEF &&
      static_cast<unsigned char>(s[1]) == 0xBB &&
      static_cast<unsigned char>(s[2]) == 0xBF) {
    return s.substr(3);
  }
  return s;
}

}  // namespace

// ---- Singleton -------------------------------------------------------

I18n& I18n::Get() {
  // Meyers singleton. C++11 guarantees thread-safe init.
  static I18n instance;
  return instance;
}

I18n::I18n() {
  // Default base dir: <cwd>/resources/i18n. We resolve at first
  // load (not in the ctor) because the CWD may not be set yet when
  // the I18n singleton is first touched (e.g. static init order
  // across translation units). The fallback chain is "en".
  base_ = std::filesystem::current_path() / "resources" / "i18n";
}

// ---- Public lookup ---------------------------------------------------

std::string I18n::T(const std::string& key) const {
  EnsureLoaded();
  BundleCache& c = Cache();
  // Primary first, fallback second. We do two lookups because the
  // maps are tiny (~10 keys) and the cost is negligible compared
  // to a single disk read at load time.
  auto pIt = c.primary.find(key);
  if (pIt != c.primary.end() && !pIt->second.empty()) return pIt->second;
  auto fIt = c.fallback.find(key);
  if (fIt != c.fallback.end() && !fIt->second.empty()) return fIt->second;
  // Key missing in both bundles. Return the key itself so the UI
  // displays something visible instead of crashing.
  return key;
}

std::wstring I18n::TW(const std::string& key) const {
  return Utf8ToWide(T(key));
}

std::string I18n::Language() const {
  EnsureLoaded();
  return lang_;
}

void I18n::SetBaseDir(const std::filesystem::path& p) {
  base_ = p;
  loaded_ = false;        // force re-load from the new location
  BundleCache& c = Cache();
  c = BundleCache{};      // discard any cached bundle
}

void I18n::SetLanguage(const std::string& lang) {
  explicitLang_ = lang;
  loaded_ = false;
  BundleCache& c = Cache();
  c = BundleCache{};
}

void I18n::ResetForTests() {
  base_ = std::filesystem::current_path() / "resources" / "i18n";
  explicitLang_.clear();
  loaded_ = false;
  BundleCache& c = Cache();
  c = BundleCache{};
}

// ---- System locale detection -----------------------------------------

std::string I18n::DetectSystemLanguage() {
  // GetUserDefaultLocaleName writes a BCP-47 tag into the buffer,
  // e.g. "en-US", "zh-CN", "zh-Hant-TW". On Windows Vista+ the
  // buffer should be LOCALE_NAME_MAX_LENGTH (85) wide chars. We
  // hard-code the size here to avoid pulling in <locale.h>; 85 is
  // part of the documented Win32 contract.
  wchar_t buf[85] = {};
  int rc = GetUserDefaultLocaleName(buf, sizeof(buf) / sizeof(buf[0]));
  if (rc <= 0) {
    // API failed; fall back to English. Returning rather than
    // throwing keeps T() exception-safe.
    return "en";
  }
  // Strip a trailing null (the API NUL-terminates already, but
  // we measure for the std::string conversion).
  size_t n = 0;
  while (n < sizeof(buf) / sizeof(buf[0]) && buf[n]) ++n;

  // Convert to narrow string for prefix matching. The tag is ASCII
  // for the language subtag (the script + region subtags may
  // include dashes and digits, all ASCII). We intentionally avoid
  // UTF-8 round-tripping here because BCP-47 tags are ASCII-only.
  std::string tag;
  tag.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    // ASCII wchar_t -> char; safe because BCP-47 is ASCII.
    tag.push_back(static_cast<char>(buf[i] & 0x7F));
  }

  // Lower-case the language subtag for prefix check. Per the spec
  // language subtags are case-insensitive.
  std::string lower = tag;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });

  // Decision: anything starting with "zh" maps to "zh". Everything
  // else maps to "en" (including variants of English like
  // en-GB, en-AU; we don't ship variants in v1.0).
  if (lower.compare(0, 2, "zh") == 0) {
    return "zh";
  }
  return "en";
}

// ---- Lazy-load --------------------------------------------------------

void I18n::EnsureLoaded() const {
  if (loaded_) return;
  loaded_ = true;  // mark before doing work so a recursive call
                   // from a logger doesn't infinite-loop.

  // Resolve the active language: explicit override wins, otherwise
  // we auto-detect. We do this even before reading any file so
  // lang_ is observable via Language() after the first T().
  if (!explicitLang_.empty()) {
    lang_ = explicitLang_;
  } else {
    lang_ = DetectSystemLanguage();
  }

  BundleCache& c = Cache();
  c.primary.clear();
  c.fallback.clear();

  // Load primary (active) language. If the file is missing we
  // leave primary empty and rely on fallback.
  std::filesystem::path primaryPath = base_ / (lang_ + ".json");
  if (!LoadFromFile(primaryPath, /*target=*/c.primary)) {
    VMOSUE_LOG_WARN("I18n: failed to load '{}'; falling back to '{}'",
                    primaryPath.string(),
                    (base_ / (fallbackLang_ + ".json")).string());
  }

  // Load fallback ("en") if it differs from the primary language.
  if (lang_ != fallbackLang_) {
    std::filesystem::path fallbackPath = base_ / (fallbackLang_ + ".json");
    if (!LoadFromFile(fallbackPath, /*target=*/c.fallback)) {
      VMOSUE_LOG_WARN("I18n: failed to load fallback '{}'",
                      fallbackPath.string());
    }
  } else {
    // Active language IS English; primary doubles as fallback so
    // a key miss still surfaces the English string.
    c.fallback = c.primary;
  }
}

bool I18n::LoadFromFile(const std::filesystem::path& path, Bundle& target) const {
  // Open + read the entire file. The bundles are tiny (a few KB
  // at most for v1.0) so streaming is not worth the complexity.
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string text = StripBom(ss.str());
  if (text.empty()) return false;

  // Parse with nlohmann_json. On parse failure we return false and
  // let the caller fall back to English. We do not throw because
  // T() is documented as exception-free.
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(text);
  } catch (const std::exception& e) {
    VMOSUE_LOG_WARN("I18n: parse error in '{}': {}", path.string(), e.what());
    return false;
  }
  if (!j.is_object()) {
    VMOSUE_LOG_WARN("I18n: '{}' is not a JSON object", path.string());
    return false;
  }

  // Convert the JSON object into a flat map<string, string>. The
  // bundles are flat (no nested sections) in v1.0; if a nested
  // object appears we skip it silently.
  target.clear();
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (!it.value().is_string()) continue;
    target[it.key()] = it.value().get<std::string>();
  }
  return true;
}

}  // namespace vmosue