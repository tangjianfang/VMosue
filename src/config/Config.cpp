#include "config/Config.h"

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <system_error>

namespace vmosue {

namespace {

// Default config file: %LOCALAPPDATA%\VMosue\config.json. Mirrors the
// pattern in app/main.cpp and Calibration.cpp: std::getenv may return
// null in non-interactive sessions, so we fall back to ".".
std::filesystem::path DefaultConfigDir() {
  const char* base = std::getenv("LOCALAPPDATA");
  return std::filesystem::path(base ? base : ".") / "VMosue";
}

std::filesystem::path DefaultConfigPath() {
  return DefaultConfigDir() / "config.json";
}

nlohmann::json ConfigToJson(const AppConfig& c) {
  nlohmann::json j;
  j["activeProfile"] = c.activeProfile;
  j["cameraIndex"] = c.cameraIndex;
  j["performanceMode"] = c.performanceMode;
  j["autoStart"] = c.autoStart;
  j["showTutorialOnLaunch"] = c.showTutorialOnLaunch;
  j["sensitivity"] = c.sensitivity;
  j["logLevel"] = c.logLevel;
  return j;
}

// Field-by-field reader: uses .value() with defaults so missing or
// wrong-typed fields fall back to the struct's in-class default. This
// means an old config from a previous version (with fewer fields) can
// still load successfully.
AppConfig ConfigFromJson(const nlohmann::json& j) {
  AppConfig c;
  c.activeProfile = j.value("activeProfile", c.activeProfile);
  c.cameraIndex = j.value("cameraIndex", c.cameraIndex);
  c.performanceMode = j.value("performanceMode", c.performanceMode);
  c.autoStart = j.value("autoStart", c.autoStart);
  c.showTutorialOnLaunch =
      j.value("showTutorialOnLaunch", c.showTutorialOnLaunch);
  c.sensitivity = j.value("sensitivity", c.sensitivity);
  c.logLevel = j.value("logLevel", c.logLevel);
  return c;
}

}  // namespace

Config& Config::Get() {
  // Meyers singleton: function-local static is thread-safe in C++11+.
  static Config instance;
  return instance;
}

void Config::SetConfigPath(const std::filesystem::path& path) {
  std::lock_guard<std::mutex> lock(mu_);
  configPath_ = path.empty() ? DefaultConfigPath() : path;
}

std::filesystem::path Config::ConfigPath() const {
  std::lock_guard<std::mutex> lock(mu_);
  return configPath_;
}

Result<void> Config::Load() {
  std::lock_guard<std::mutex> lock(mu_);
  const auto path = configPath_;

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    // File missing on first run is not an error: caller can treat
    // `data_` (already at defaults) as the active config and call
    // Save() to materialize the file.
    return Result<void>::Err("Config not found: " + path.string());
  }

  std::stringstream buf;
  buf << in.rdbuf();
  // Reset to defaults BEFORE parsing so a corrupt file leaves the
  // singleton at defaults (rather than at stale values from a prior
  // successful load). Parsed fields overwrite the defaults below.
  data_ = AppConfig{};
  try {
    auto j = nlohmann::json::parse(buf.str());
    data_ = ConfigFromJson(j);
    return Result<void>::Ok({});
  } catch (const std::exception& e) {
    // Malformed JSON: surface as Err so callers can show a "config
    // was corrupt, restored to defaults" message. `data_` is left
    // at the fresh defaults assigned above.
    return Result<void>::Err(
        std::string("Failed to parse config: ") + e.what());
  }
}

Result<void> Config::Save() const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto path = configPath_;

  // Ensure the parent directory exists. create_directories with
  // error_code is non-throwing; failures should be surfaced, not
  // swallowed, so callers know the save did not happen.
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    return Result<void>::Err(
        "Failed to create config directory: " + ec.message());
  }

  // Atomic write: write to <path>.tmp first, then rename over <path>.
  // If the process crashes mid-write, the original config.json is
  // left intact and the partial file can be cleaned up on next launch.
  const auto tmpPath = path.string() + ".tmp";
  {
    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    if (!out) {
      return Result<void>::Err(
          "Failed to open temp config for write: " + tmpPath);
    }
    out << ConfigToJson(data_).dump(2);
    if (!out) {
      return Result<void>::Err(
          "Failed to write temp config: " + tmpPath);
    }
    out.flush();
    if (!out) {
      return Result<void>::Err(
          "Failed to flush temp config: " + tmpPath);
    }
  }

  // rename() is atomic on the same filesystem on Windows when the
  // destination already exists (since Windows Vista's MoveFileEx with
  // MOVEFILE_REPLACE_EXISTING). Use that overload for portability.
  std::error_code renameEc;
  std::filesystem::rename(tmpPath, path, renameEc);
  if (renameEc) {
    // Best-effort cleanup so a stale .tmp doesn't accumulate.
    std::filesystem::remove(tmpPath, ec);
    return Result<void>::Err(
        "Failed to rename temp config: " + renameEc.message());
  }
  return Result<void>::Ok({});
}

}  // namespace vmosue