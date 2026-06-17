#pragma once
#include <filesystem>
#include <mutex>
#include <string>

#include "util/Result.h"

namespace vmosue {

// Application-wide user preferences. Loaded from and persisted to a
// JSON file under %LOCALAPPDATA%\VMosue\config.json by default. Every
// field has an in-class default so a fresh install (or a corrupted
// config file) still produces a usable configuration.
//
// v0.5 (Wave 4): the user-facing `sensitivity` field is removed. The
// v0.4 slider let the user bias cursor speed; in v0.5 every tunable
// is a deterministic function of observable signals, so there is no
// user preference to persist. The serializer remains tolerant of old
// configs that still contain the field (it just gets ignored) so a
// v0.4 → v0.5 upgrade does not require a one-shot migration step.
struct AppConfig {
  std::string activeProfile = "default";
  int cameraIndex = 0;
  std::string performanceMode = "balanced";
  bool autoStart = false;
  bool showTutorialOnLaunch = true;
  std::string logLevel = "info";
  // v0.6: dwell-time calibration. Each one-shot action
  // (left/right/middle/double click) must be continuously
  // asserted for this many ms before it fires. 0 disables the
  // gate (every action fires on the first frame it is true) —
  // the legacy behavior the test_action_map fixtures depend
  // on. 500ms is the production default — long enough to
  // reject 1-2 frame phantom hands, short enough that a
  // deliberate click is not painful. The SettingsWindow lets
  // the user pick anything in [0, 3000] ms.
  int  dwellTimeMs = 500;
  // v0.6: anti-interference strength. Drives the per-handedness
  // stability threshold (HandStabilityFilter) and the Python
  // min-hand-confidence argument. Valid values: "off", "low",
  // "medium", "high". Anything else falls back to "medium".
  std::string antiInterference = "medium";
  // v0.6: render the "About to: <action>" label and progress
  // bar in the overlay while DwellGate is counting down. On by
  // default — the user explicitly asked for the affordance —
  // but easy to disable for users who find the overlay
  // distracting.
  bool showActionPreview = true;
  // v0.6.1: auto-show the 3-action list (F1 / "Action list" tray
  // entry) on first launch, ~5s into the run. The user reported
  // not knowing which gestures are supported — silently launching
  // a gesture-mouse with no on-screen reference is what made the
  // previous 7-action build feel unusable. Returning users can
  // flip this off in %LOCALAPPDATA%\VMosue\config.json.
  bool showActionListOnLaunch = true;
};

// Process-wide singleton holding the active AppConfig.
//
// Default path: %LOCALAPPDATA%\VMosue\config.json (matches the
// convention in Calibration.cpp and app/main.cpp). Tests call
// SetConfigPath() with a temp file before Load/Save so the production
// path is never touched during a test run; per-test isolation also
// keeps a corrupt file from one test poisoning the next.
//
// Load() and Save() are thread-safe: a std::mutex guards `data_`. On
// corruption Load() returns Err, leaving `data_` set to defaults so
// the app can show a "config was corrupt, restored to defaults"
// message and continue running with safe defaults.
class Config {
 public:
  // Meyers singleton: thread-safe initialization in C++11+.
  static Config& Get();

  // Override the path used by Load/Save. Empty path resets to the
  // default (%LOCALAPPDATA%\VMosue\config.json).
  void SetConfigPath(const std::filesystem::path& path);

  // Resolved config file path.
  std::filesystem::path ConfigPath() const;

  // Load AppConfig from disk. Returns Err if the file is missing or
  // malformed; in either case `data_` is left at its current value
  // (caller should reset via a fresh instance or accept defaults).
  Result<void> Load();

  // Atomically write AppConfig to disk: writes to <path>.tmp, then
  // renames over <path>. Returns Err if the directory cannot be
  // created or the file cannot be written.
  Result<void> Save() const;

  // Mutable access to the in-memory config. Not thread-safe with
  // Load/Save; callers should serialize externally if needed.
  AppConfig& Mutable() { return data_; }
  // Read-only access. Named Data() rather than Get() to avoid a
  // name clash with the static singleton accessor Config::Get().
  const AppConfig& Data() const { return data_; }

 private:
  Config() = default;

  mutable std::mutex mu_;
  std::filesystem::path configPath_;
  AppConfig data_;
};

}  // namespace vmosue