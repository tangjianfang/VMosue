#include "config/Calibration.h"

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <system_error>  // std::error_code (atomic-write rename/remove)

namespace vmosue {

namespace {

// Default base directory: %LOCALAPPDATA%\VMosue\profiles\.
// Mirrors the pattern in app/main.cpp; std::getenv is allowed to
// return null in non-interactive sessions, so we fall back to ".".
std::filesystem::path DefaultBaseDir() {
  const char* base = std::getenv("LOCALAPPDATA");
  return std::filesystem::path(base ? base : ".") / "VMosue" / "profiles";
}

// Reject path-traversal attempts: a profile name must be a simple
// file name with no separators. Stops ".." or "a\b" from escaping
// the base directory.
bool IsSafeProfileName(const std::string& name) {
  if (name.empty()) return false;
  for (char c : name) {
    if (c == '/' || c == '\\' || c == ':' || c == '\0') return false;
  }
  return true;
}

nlohmann::json ParamsToJson(const CalibrationParams& p) {
  nlohmann::json j;
  j["scaleX"] = p.scaleX;
  j["scaleY"] = p.scaleY;
  j["offsetX"] = p.offsetX;
  j["offsetY"] = p.offsetY;
  return j;
}

CalibrationParams ParamsFromJson(const nlohmann::json& j) {
  CalibrationParams p;
  // Use value() with defaults so missing fields fall back to the
  // struct's in-class default rather than failing the whole load.
  p.scaleX = j.value("scaleX", p.scaleX);
  p.scaleY = j.value("scaleY", p.scaleY);
  p.offsetX = j.value("offsetX", p.offsetX);
  p.offsetY = j.value("offsetY", p.offsetY);
  // v0.5 (Wave 4): silently ignore the legacy pinchThreshold and
  // airClickZThreshold fields from older profiles. Both have moved
  // to the adaptive controller; reading them and assigning into
  // p.<field> would not compile because the fields no longer exist.
  (void)j.value("pinchThreshold", 0.04f);
  (void)j.value("airClickZThreshold", 0.02f);
  return p;
}

}  // namespace

Calibration::Calibration() : baseDir_(DefaultBaseDir()) {}

void Calibration::SetBaseDir(const std::filesystem::path& baseDir) {
  baseDir_ = baseDir.empty() ? DefaultBaseDir() : baseDir;
}

std::filesystem::path Calibration::BaseDir() const { return baseDir_; }

std::filesystem::path Calibration::ProfilePath(const std::string& profileName) const {
  return baseDir_ / (profileName + ".json");
}

Result<void> Calibration::RunInteractive() {
  // The full guided UI (8 steps: corner mapping, 5 pinches, 5 air
  // clicks, 1 drag, 1 scroll) requires a real window and a live hand
  // pipeline. That's a separate task; for v0.2 we deliberately fail
  // closed so callers know to fall back to defaults.
  return Result<void>::Err("Calibration::RunInteractive not yet implemented");
}

Result<void> Calibration::Save(const std::string& profileName,
                                const CalibrationParams& params) {
  if (!IsSafeProfileName(profileName)) {
    return Result<void>::Err("Invalid profile name: " + profileName);
  }
  // create_directories with error_code is non-throwing; a failed
  // mkdir should not abort the process (matches main.cpp's log
  // directory handling).
  std::error_code ec;
  std::filesystem::create_directories(baseDir_, ec);
  if (ec) {
    return Result<void>::Err("Failed to create profile directory: " + ec.message());
  }

  const auto path = ProfilePath(profileName);

  // Atomic write: write to <path>.tmp, flush, then rename over the
  // destination. Mirrors Config::Save so a crash mid-write leaves any
  // existing profile intact instead of truncating it to a partial
  // (and unparseable) file. The previous direct-write path could
  // corrupt a good profile if the process died between open and the
  // final byte.
  const auto tmpPath = path.string() + ".tmp";
  {
    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    if (!out) {
      return Result<void>::Err("Failed to open temp profile for write: " +
                               tmpPath);
    }
    out << ParamsToJson(params).dump(2);
    if (!out) {
      return Result<void>::Err("Failed to write temp profile: " + tmpPath);
    }
    out.flush();
    if (!out) {
      return Result<void>::Err("Failed to flush temp profile: " + tmpPath);
    }
  }

  // rename() over an existing destination is atomic on Windows (Vista+
  // MoveFileEx with MOVEFILE_REPLACE_EXISTING); the std::filesystem
  // overload uses it.
  std::error_code renameEc;
  std::filesystem::rename(tmpPath, path, renameEc);
  if (renameEc) {
    std::error_code rmEc;
    std::filesystem::remove(tmpPath, rmEc);  // best-effort cleanup
    return Result<void>::Err("Failed to rename temp profile: " +
                             renameEc.message());
  }
  return Result<void>::Ok({});
}

Result<CalibrationParams> Calibration::Load(const std::string& profileName) {
  if (!IsSafeProfileName(profileName)) {
    return Result<CalibrationParams>::Err("Invalid profile name: " + profileName);
  }
  const auto path = ProfilePath(profileName);
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return Result<CalibrationParams>::Err("Profile not found: " + path.string());
  }
  std::stringstream buf;
  buf << in.rdbuf();
  try {
    auto j = nlohmann::json::parse(buf.str());
    return Result<CalibrationParams>::Ok(ParamsFromJson(j));
  } catch (const std::exception& e) {
    // Malformed JSON: surface as Err so the caller can recover (e.g.
    // fall back to defaults) instead of receiving a default-constructed
    // struct that looks valid.
    return Result<CalibrationParams>::Err(
        std::string("Failed to parse profile: ") + e.what());
  }
}

}  // namespace vmosue
