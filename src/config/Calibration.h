#pragma once
#include <filesystem>
#include <string>
#include "util/Result.h"

namespace vmosue {

// Tunable per-user parameters produced by the calibration flow and
// consumed by the gesture detectors. Defaults match the project's
// baseline so a fresh install still works without running calibration.
struct CalibrationParams {
  float pinchThreshold = 0.04f;
  float airClickZThreshold = 0.02f;
  float scaleX = 1.0f;
  float scaleY = 1.0f;
  float offsetX = 0.0f;
  float offsetY = 0.0f;
};

// Persists per-user calibration profiles to disk.
//
// Files live under <baseDir>/<profileName>.json. By default baseDir
// is %LOCALAPPDATA%\VMosue\profiles\, matching the convention used by
// main.cpp for logs. Tests call SetBaseDir() with a temp directory so
// the production path is never touched during a test run.
class Calibration {
 public:
  Calibration();

  // Override the directory used for Save/Load. The directory is
  // created on demand by Save(); Load() expects the file to already
  // exist. Setting an empty path resets to the default.
  void SetBaseDir(const std::filesystem::path& baseDir);

  // Returns the resolved base directory (default or override).
  std::filesystem::path BaseDir() const;

  // Full path of a profile's JSON file. Does not perform I/O.
  std::filesystem::path ProfilePath(const std::string& profileName) const;

  // Walks the user through an interactive 8-step calibration flow
  // (corner mapping, pinches, air clicks, drag, scroll) and writes
  // the resulting parameters. Not implemented in v0.2: returns Err so
  // callers can detect the missing UI and fall back to defaults.
  Result<void> RunInteractive();

  // Persist params to <baseDir>/<profileName>.json. Creates
  // baseDir if it does not already exist.
  Result<void> Save(const std::string& profileName, const CalibrationParams& params);

  // Load params from <baseDir>/<profileName>.json. Returns Err if
  // the file is missing or malformed.
  Result<CalibrationParams> Load(const std::string& profileName);

 private:
  std::filesystem::path baseDir_;
};

}  // namespace vmosue
